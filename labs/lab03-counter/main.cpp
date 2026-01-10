// lab03-counter : кроссплатформенный счётчик с лидером и копиями
// без сторонних библиотек, C++17

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <io.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

// ----------- короткие алиасы времени -----------
using clock_steady = std::chrono::steady_clock;
using clock_system = std::chrono::system_clock;
using ms           = std::chrono::milliseconds;

static inline uint64_t now_ms() {
  return std::chrono::duration_cast<ms>(clock_steady::now().time_since_epoch()).count();
}
static inline uint64_t now_wall_ms() {
  return std::chrono::duration_cast<ms>(clock_system::now().time_since_epoch()).count();
}
static inline std::string fmt_time(uint64_t wall_ms) {
  std::time_t t  = static_cast<std::time_t>(wall_ms / 1000);
  int         ms3 = static_cast<int>(wall_ms % 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
     << std::setw(3) << std::setfill('0') << ms3;
  return os.str();
}

static inline uint64_t pid_self() {
#ifdef _WIN32
  return static_cast<uint64_t>(GetCurrentProcessId());
#else
  return static_cast<uint64_t>(getpid());
#endif
}

// ----------- файлы состояния и лог -----------
static constexpr const char* FILE_STATE = "shared.bin";
static constexpr const char* FILE_LOCK  = "shared.lock";
static constexpr const char* FILE_LOG   = "program.log";

static constexpr uint32_t STATE_MAGIC = 0xC0DECAFE;
struct SharedState {
  uint32_t magic{STATE_MAGIC};
  uint32_t version{1};
  int64_t  counter{0};
  uint64_t leader_pid{0};
  uint64_t leader_heartbeat_ms{0};
  uint64_t child1_pid{0};
  uint64_t child2_pid{0};
  uint64_t child1_start_ms{0};
  uint64_t child2_start_ms{0};
};

// ----------- примитив блокировки файла -----------
struct FileLock {
#ifdef _WIN32
  HANDLE h{INVALID_HANDLE_VALUE};
#else
  int fd{-1};
#endif
  bool ok{false};
};

static FileLock lock_acquire(const char* path) {
  FileLock L;
#ifdef _WIN32
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return L;
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) {
    CloseHandle(h);
    return L;
  }
  L.h = h; L.ok = true;
#else
  int fd = ::open(path, O_CREAT | O_RDWR, 0666);
  if (fd < 0) return L;
  struct flock fl{};
  fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
  if (fcntl(fd, F_SETLKW, &fl) == -1) { ::close(fd); return L; }
  L.fd = fd; L.ok = true;
#endif
  return L;
}

static void lock_release(FileLock& L) {
#ifdef _WIN32
  if (L.h != INVALID_HANDLE_VALUE) {
    OVERLAPPED ov{};
    UnlockFileEx(L.h, 0, MAXDWORD, MAXDWORD, &ov);
    CloseHandle(L.h);
    L.h = INVALID_HANDLE_VALUE;
  }
#else
  if (L.fd >= 0) {
    struct flock fl{};
    fl.l_type = F_UNLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
    fcntl(L.fd, F_SETLK, &fl);
    ::close(L.fd);
    L.fd = -1;
  }
#endif
  L.ok = false;
}

// ----------- чтение и запись состояния -----------
static void state_ensure_size(std::fstream& f) {
  f.seekg(0, std::ios::end);
  if (f.tellg() < static_cast<std::streamoff>(sizeof(SharedState))) {
    SharedState s;
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&s), sizeof(s));
    f.flush();
  }
}

static bool state_read(SharedState& out) {
  std::fstream f(FILE_STATE, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) {
    std::fstream nf(FILE_STATE, std::ios::out | std::ios::binary);
    nf.close();
    f.open(FILE_STATE, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;
  }
  state_ensure_size(f);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(&out), sizeof(out));
  return f.good() && out.magic == STATE_MAGIC;
}

static bool state_write(const SharedState& s) {
  std::fstream f(FILE_STATE, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) return false;
  f.seekp(0);
  f.write(reinterpret_cast<const char*>(&s), sizeof(s));
  f.flush();
  return true;
}

// ----------- логирование -----------
static void log_locked(const std::string& line) {
  std::ofstream out(FILE_LOG, std::ios::app);
  out << line << '\n';
  out.flush();
}
static void log_line(const std::string& line) {
  auto L = lock_acquire(FILE_LOCK);
  if (!L.ok) return;
  log_locked(line);
  lock_release(L);
}

// ----------- проверка процесса -----------
static bool process_alive(uint64_t pid) {
  if (pid == 0) return false;
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  BOOL ok = GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return ok && code == STILL_ACTIVE;
#else
  int r = kill(static_cast<pid_t>(pid), 0);
  if (r == 0) return true;
  return errno == EPERM;
#endif
}

// ----------- путь к себе и запуск копии -----------
static std::string self_path() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return std::string(buf, buf + n);
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0) return "./lab03";
  buf[n] = 0;
  return std::string(buf);
#endif
}

static bool spawn_self(const std::vector<std::string>& args, uint64_t& out_pid) {
#ifdef _WIN32
  auto q = [](const std::string& s) -> std::string {
    if (s.empty()) return "\"\"";
    if (s.find('"') != std::string::npos) return s;
    for (char c : s) if (c == ' ' || c == '\t') return "\"" + s + "\"";
    return s;
  };
  std::string cmd = q(self_path());
  for (const auto& a : args) { cmd.push_back(' '); cmd += q(a); }

  STARTUPINFOA si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<char> buf(cmd.begin(), cmd.end()); buf.push_back('\0');

  BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
  if (!ok) return false;
  out_pid = static_cast<uint64_t>(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    std::string me = self_path();
    std::vector<char*> av;
    av.reserve(args.size() + 2);
    av.push_back(const_cast<char*>(me.c_str()));
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv(me.c_str(), av.data());
    _exit(127);
  }
  out_pid = static_cast<uint64_t>(pid);
  return true;
#endif
}

// ----------- роли дочерних копий -----------
static int child1() {
  auto pid = pid_self();
  auto t0  = now_wall_ms();

  auto L = lock_acquire(FILE_LOCK);
  if (L.ok) {
    SharedState s{};
    if (state_read(s)) {
      s.counter += 10;
      state_write(s);
      std::ostringstream os;
      os << '[' << fmt_time(t0) << "] child1 start pid=" << pid
         << " counter+=10 -> " << s.counter;
      log_locked(os.str());
    }
    lock_release(L);
  }

  auto t1 = now_wall_ms();
  auto L2 = lock_acquire(FILE_LOCK);
  if (L2.ok) {
    std::ostringstream os;
    os << '[' << fmt_time(t1) << "] child1 exit pid=" << pid;
    log_locked(os.str());
    lock_release(L2);
  }
  return 0;
}

static int child2() {
  auto pid = pid_self();
  auto t0  = now_wall_ms();

  auto L = lock_acquire(FILE_LOCK);
  if (L.ok) {
    SharedState s{};
    if (state_read(s)) {
      s.counter *= 2;
      state_write(s);
      std::ostringstream os;
      os << '[' << fmt_time(t0) << "] child2 start pid=" << pid
         << " counter*=2 -> " << s.counter;
      log_locked(os.str());
    }
    lock_release(L);
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto Lm = lock_acquire(FILE_LOCK);
  if (Lm.ok) {
    SharedState s{};
    if (state_read(s)) {
      s.counter /= 2;
      state_write(s);
      std::ostringstream os;
      os << '[' << fmt_time(now_wall_ms()) << "] child2 mid pid=" << pid
         << " counter/=2 -> " << s.counter;
      log_locked(os.str());
    }
    lock_release(Lm);
  }

  auto Le = lock_acquire(FILE_LOCK);
  if (Le.ok) {
    std::ostringstream os;
    os << '[' << fmt_time(now_wall_ms()) << "] child2 exit pid=" << pid;
    log_locked(os.str());
    lock_release(Le);
  }
  return 0;
}

// ----------- выбор лидера -----------
static bool ensure_leader(SharedState& s, uint64_t me, uint64_t now) {
  static constexpr uint64_t lease_ms = 6000;
  bool dead_or_expired = (s.leader_pid == 0) ||
                         !process_alive(s.leader_pid) ||
                         (s.leader_heartbeat_ms + lease_ms < now);
  if (dead_or_expired) {
    s.leader_pid = me;
    s.leader_heartbeat_ms = now;
    return true;
  }
  return s.leader_pid == me;
}

// ----------- main -----------
int main(int argc, char** argv) {
  // режим дочерней роли
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0) {
      if (std::strcmp(argv[i + 1], "child1") == 0) return child1();
      if (std::strcmp(argv[i + 1], "child2") == 0) return child2();
    }
  }

  auto mypid = pid_self();

  // запись о старте
  {
    auto L = lock_acquire(FILE_LOCK);
    if (L.ok) {
      SharedState s{};
      state_read(s);
      std::ostringstream os;
      os << '[' << fmt_time(now_wall_ms()) << "] start pid=" << mypid;
      log_locked(os.str());
      lock_release(L);
    }
  }

#ifndef _WIN32
  // тихие сигналы
  struct sigaction sa{};
  sa.sa_handler = [](int){};
  sigaction(SIGPIPE, &sa, nullptr);
#endif

  std::atomic<bool> stop{false};

  // тикающий поток: +1 каждые 300 мс
  std::thread t_tick([&]{
    while (!stop.load()) {
      std::this_thread::sleep_for(ms(300));
      auto L = lock_acquire(FILE_LOCK);
      if (!L.ok) continue;
      SharedState s{};
      if (state_read(s)) { s.counter += 1; state_write(s); }
      lock_release(L);
    }
  });

  // лидер: раз в 1 с пишет лог, раз в 3 с спаунит копии
  std::thread t_leader([&]{
    uint64_t last_log = 0, last_spawn = 0;
    while (!stop.load()) {
      std::this_thread::sleep_for(ms(100));
      uint64_t t = now_ms();

      auto L = lock_acquire(FILE_LOCK);
      if (!L.ok) continue;

      SharedState s{};
      if (!state_read(s)) { lock_release(L); continue; }

      bool iam = ensure_leader(s, mypid, t);
      if (iam) {
        s.leader_heartbeat_ms = t;
        state_write(s);

        if (t - last_log >= 1000) {
          SharedState ss{};
          state_read(ss);
          std::ostringstream os;
          os << '[' << fmt_time(now_wall_ms()) << "] leader pid=" << mypid
             << " counter=" << ss.counter;
          log_locked(os.str());
          last_log = t;
        }

        if (t - last_spawn >= 3000) {
          bool c1 = process_alive(s.child1_pid);
          bool c2 = process_alive(s.child2_pid);
          if (c1 || c2) {
            std::ostringstream os;
            os << '[' << fmt_time(now_wall_ms()) << "] leader pid=" << mypid
               << " skip spawn (child running)";
            log_locked(os.str());
          } else {
            uint64_t p1 = 0, p2 = 0;
            bool a = spawn_self({"--role", "child1"}, p1);
            bool b = spawn_self({"--role", "child2"}, p2);
            SharedState ss{};
            state_read(ss);
            if (a) { ss.child1_pid = p1; ss.child1_start_ms = now_wall_ms(); }
            if (b) { ss.child2_pid = p2; ss.child2_start_ms = now_wall_ms(); }
            state_write(ss);

            std::ostringstream os;
            os << '[' << fmt_time(now_wall_ms()) << "] leader pid=" << mypid
               << " spawned child1=" << (a ? std::to_string(p1) : "err")
               << " child2="       << (b ? std::to_string(p2) : "err");
            log_locked(os.str());
          }
          last_spawn = t;
        }
      }
      lock_release(L);
    }
  });

  // ввод пользователя: set <val> или q
  std::thread t_input([&]{
    std::string line;
    while (!stop.load() && std::getline(std::cin, line)) {
      if (line == "q" || line == "quit" || line == "exit") {
        stop.store(true);
        break;
      }
      if (line.rfind("set ", 0) == 0) {
        long long val = 0;
        try { val = std::stoll(line.substr(4)); } catch (...) { continue; }
        auto L = lock_acquire(FILE_LOCK);
        if (!L.ok) continue;
        SharedState s{};
        if (state_read(s)) {
          s.counter = val;
          state_write(s);
          std::ostringstream os;
          os << '[' << fmt_time(now_wall_ms()) << "] pid=" << mypid
             << " set counter=" << s.counter;
          log_locked(os.str());
        }
        lock_release(L);
      }
    }
    stop.store(true);
  });

  t_input.join();
  stop.store(true);
  t_tick.join();
  t_leader.join();

  // запись о завершении
  {
    auto L = lock_acquire(FILE_LOCK);
    if (L.ok) {
      std::ostringstream os;
      os << '[' << fmt_time(now_wall_ms()) << "] stop pid=" << mypid;
      log_locked(os.str());
      lock_release(L);
    }
  }
  return 0;
}
