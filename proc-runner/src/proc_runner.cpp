#include "proc_runner.h"
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <errno.h>
  #include <signal.h>
#endif

namespace proc {

struct Process {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
#else
    pid_t pid = -1;
    int   status_cached = 0;
    bool  has_status = false;
#endif
    bool started = false;
};

static Status ok(){ return {}; }

static Status sys_error(const char* where, int syscode){
    Status s; s.code = Result::SysError;
#ifdef _WIN32
    s.sys_errno = syscode ? syscode : (int)GetLastError();
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, (DWORD)s.sys_errno, 0, buf, sizeof(buf), nullptr);
    s.message = std::string(where) + ": " + buf;
#else
    s.sys_errno = syscode ? syscode : errno;
    s.message = std::string(where) + ": " + std::strerror(s.sys_errno);
#endif
    return s;
}

Status spawn(const std::string& program,
             const std::vector<std::string>& args,
             const SpawnOptions& opts,
             Process*& out_proc)
{
    out_proc = nullptr;
    if (program.empty()) return {Result::InvalidArg, 0, "empty program path"};
    auto p = new Process();

#ifdef _WIN32
    std::string cmd = "\"" + program + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    DWORD flags = CREATE_NEW_PROCESS_GROUP;
    if (opts.create_no_window) flags |= CREATE_NO_WINDOW;

    std::vector<char> cmdline(cmd.begin(), cmd.end()); cmdline.push_back('\0');

    BOOL ok_create = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, FALSE, flags,
        nullptr, opts.workdir.empty() ? nullptr : opts.workdir.c_str(),
        &si, &p->pi
    );
    if (!ok_create) { auto s = sys_error("CreateProcess", GetLastError()); delete p; return s; }
    p->started = true;

#else
    pid_t pid = fork();
    if (pid < 0) { auto s = sys_error("fork", errno); delete p; return s; }
    if (pid == 0) {
        if (!opts.workdir.empty()) { if (chdir(opts.workdir.c_str()) != 0) _exit(127); }
        std::vector<char*> argv; argv.reserve(args.size()+2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a: args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127); // если не удалось execvp
    } else {
        p->pid = pid; p->started = true;
    }
#endif
    out_proc = p; return ok();
}

bool is_running(const Process* p){
    if (!p || !p->started) return false;
#ifdef _WIN32
    DWORD code=0; if(!GetExitCodeProcess(p->pi.hProcess,&code)) return false; return code==STILL_ACTIVE;
#else
    if (p->has_status) return false;
    int status=0; pid_t r=waitpid(p->pid,&status,WNOHANG);
    if (r==0) return true;
    if (r==p->pid){ const_cast<Process*>(p)->status_cached=status; const_cast<Process*>(p)->has_status=true; return false; }
    return false;
#endif
}

Status wait(Process* p, millis timeout_ms){
    if (!p || !p->started) return {Result::NotStarted,0,"process not started"};
#ifdef _WIN32
    DWORD timeout = (timeout_ms<0)? INFINITE : (DWORD)timeout_ms;
    DWORD wr = WaitForSingleObject(p->pi.hProcess, timeout);
    if (wr==WAIT_TIMEOUT) return {Result::Timeout,0,"timeout"};
    if (wr!=WAIT_OBJECT_0) return sys_error("WaitForSingleObject", GetLastError());
    return ok();
#else
    if (timeout_ms < 0) {
        int status=0; pid_t r=waitpid(p->pid,&status,0);
        if (r<0) return sys_error("waitpid", errno);
        p->status_cached=status; p->has_status=true; return ok();
    } else {
        auto start=std::chrono::steady_clock::now();
        while (true) {
            int status=0; pid_t r=waitpid(p->pid,&status,WNOHANG);
            if (r==p->pid){ p->status_cached=status; p->has_status=true; return ok(); }
            if (r<0) return sys_error("waitpid", errno);
            auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-start).count();
            if (elapsed >= timeout_ms) return {Result::Timeout,0,"timeout"};
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
#endif
}

Status exit_code(const Process* p, int& out_code){
    if (!p || !p->started) return {Result::NotStarted,0,"process not started"};
#ifdef _WIN32
    DWORD code=0; if(!GetExitCodeProcess(p->pi.hProcess,&code)) return sys_error("GetExitCodeProcess", GetLastError());
    if (code==STILL_ACTIVE) return {Result::Running,0,"still running"};
    out_code=(int)code; return ok();
#else
    int status=0;
    if (!p->has_status) {
        pid_t r=waitpid(p->pid,&status,WNOHANG);
        if (r==0) return {Result::Running,0,"still running"};
        if (r<0) return sys_error("waitpid", errno);
        const_cast<Process*>(p)->status_cached=status; const_cast<Process*>(p)->has_status=true;
    } else status=p->status_cached;
    if (WIFEXITED(status)) { out_code=WEXITSTATUS(status); return ok(); }
    else if (WIFSIGNALED(status)) { out_code=128+WTERMSIG(status); return ok(); }
    return {Result::SysError,0,"unknown status"};
#endif
}

void close(Process*& p){
    if (!p) return;
#ifdef _WIN32
    if (p->pi.hThread)  CloseHandle(p->pi.hThread);
    if (p->pi.hProcess) CloseHandle(p->pi.hProcess);
#endif
    delete p; p=nullptr;
}

Status run_and_wait(const std::string& program,
                    const std::vector<std::string>& args,
                    const SpawnOptions& opts,
                    int& out_code){
    Process* pr=nullptr; auto s=spawn(program,args,opts,pr);
    if (s.code!=Result::Ok) return s;
    s=wait(pr,-1); if (s.code!=Result::Ok){ close(pr); return s; }
    s=exit_code(pr,out_code); close(pr); return s;
}
} // namespace proc
