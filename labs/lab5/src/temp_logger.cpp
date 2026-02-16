#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sqlite3.h> // SQLite API (таблица measurements)

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
  static void closesock(SOCKET s){ closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SOCKET = int;
  static void closesock(SOCKET s){ close(s); }
  static const int INVALID_SOCKET = -1;
  static const int SOCKET_ERROR = -1;
#endif

using namespace std;

// Флаг остановки для корректного завершения (Ctrl+C и т.д.)
static atomic<bool> g_stop{false};
static void on_signal(int){ g_stop = true; }

// Лог в stderr
static void log_line(const string& s){
  cerr << s << "\n";
  cerr.flush();
}

// Проверка формата времени "YYYY-MM-DDTHH:MM:SSZ"
static bool is_isoz(const string& iso){
  return iso.size() == 20 &&
         iso[4]=='-' && iso[7]=='-' && iso[10]=='T' &&
         iso[13]==':' && iso[16]==':' && iso[19]=='Z';
}

// ISO UTC ("...Z") -> epoch seconds (int64)
static optional<int64_t> parse_iso_utc_to_epoch(const string& iso){
  if(!is_isoz(iso)) return nullopt;

  tm t{};
  t.tm_year = stoi(iso.substr(0,4)) - 1900;
  t.tm_mon  = stoi(iso.substr(5,2)) - 1;
  t.tm_mday = stoi(iso.substr(8,2));
  t.tm_hour = stoi(iso.substr(11,2));
  t.tm_min  = stoi(iso.substr(14,2));
  t.tm_sec  = stoi(iso.substr(17,2));
  t.tm_isdst = 0;

#ifdef _WIN32
  time_t tt = _mkgmtime(&t);   // Windows: UTC tm -> time_t
#else
  time_t tt = timegm(&t);      // Linux: UTC tm -> time_t
#endif

  if(tt < 0) return nullopt;
  return (int64_t)tt;
}

// epoch seconds -> ISO UTC ("...Z")
static string iso_utc_from_epoch(int64_t epoch){
  time_t tt = (time_t)epoch;
  tm t{};
#ifdef _WIN32
  gmtime_s(&t, &tt);
#else
  gmtime_r(&tt, &t);
#endif
  ostringstream os;
  os << put_time(&t, "%Y-%m-%dT%H:%M:%SZ");
  return os.str();
}

// Обертка над SQLite: потокобезопасно (mutex), потому что симулятор и HTTP сервер в одном процессе
struct Db {
  sqlite3* db=nullptr;
  mutex m;

  // Открыть базу и создать таблицу
  bool open(const string& path){
    if(sqlite3_open(path.c_str(), &db) != SQLITE_OK){
      log_line(string("DB open failed: ") + (db?sqlite3_errmsg(db):"unknown"));
      return false;
    }

    // WAL лучше для записи/чтения одновременно
    // measurements(ts PRIMARY KEY, temp REAL)
    const char* sql =
      "PRAGMA journal_mode=WAL;"
      "CREATE TABLE IF NOT EXISTS measurements("
      " ts INTEGER PRIMARY KEY,"
      " temp REAL NOT NULL"
      ");";

    char* err=nullptr;
    if(sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK){
      log_line(string("DB init failed: ") + (err?err:"(null)"));
      sqlite3_free(err);
      return false;
    }
    return true;
  }

  void close(){
    lock_guard<mutex> lk(m);
    if(db){ sqlite3_close(db); db=nullptr; }
  }

  // Вставка одного измерения (ts в секундах epoch)
  // INSERT OR REPLACE, чтобы если ts совпал, строка обновилась
  bool insert(int64_t ts, double temp){
    lock_guard<mutex> lk(m);
    static const char* sql = "INSERT OR REPLACE INTO measurements(ts,temp) VALUES(?,?);";
    sqlite3_stmt* st=nullptr;
    if(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_double(st, 2, temp);
    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
  }

  // Последнее измерение по времени
  optional<pair<int64_t,double>> latest(){
    lock_guard<mutex> lk(m);
    static const char* sql = "SELECT ts,temp FROM measurements ORDER BY ts DESC LIMIT 1;";
    sqlite3_stmt* st=nullptr;
    if(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return nullopt;
    optional<pair<int64_t,double>> res;
    if(sqlite3_step(st) == SQLITE_ROW){
      int64_t ts = sqlite3_column_int64(st, 0);
      double temp = sqlite3_column_double(st, 1);
      res = make_pair(ts,temp);
    }
    sqlite3_finalize(st);
    return res;
  }

  // Статистика за период + серия точек для графика
  struct Stats {
    int64_t from=0, to=0;
    int count=0;
    double avg=numeric_limits<double>::quiet_NaN();
    double mn=numeric_limits<double>::quiet_NaN();
    double mx=numeric_limits<double>::quiet_NaN();
    vector<pair<int64_t,double>> series; // (ts,temp)
  };

  // from/to - epoch seconds, max_points - ограничение точек на графике что бы не было каши
  optional<Stats> stats(int64_t from, int64_t to, int max_points=300){
    if(to <= from) return nullopt;

    Stats s; s.from=from; s.to=to;
    lock_guard<mutex> lk(m);

    // 1) агрегаты (count, avg, min, max)
    {
      static const char* sql =
        "SELECT COUNT(*), AVG(temp), MIN(temp), MAX(temp) "
        "FROM measurements WHERE ts>=? AND ts<=?;";

      sqlite3_stmt* st=nullptr;
      if(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return nullopt;
      sqlite3_bind_int64(st, 1, from);
      sqlite3_bind_int64(st, 2, to);

      if(sqlite3_step(st) == SQLITE_ROW){
        s.count = sqlite3_column_int(st, 0);
        s.avg = sqlite3_column_double(st, 1);
        s.mn  = sqlite3_column_double(st, 2);
        s.mx  = sqlite3_column_double(st, 3);
      }
      sqlite3_finalize(st);
    }

    // 2) серия: берем не все подряд,а по step-ам, чтобы не отправлять трилиард точек
    int64_t span = to - from;
    int64_t step = (span / max_points);
    if(step < 1) step = 1;

    // Берем точки, где (ts-from) % step == 0
    static const char* sql2 =
      "SELECT ts,temp FROM measurements "
      "WHERE ts>=? AND ts<=? AND ((ts-?) % ? = 0) "
      "ORDER BY ts ASC;";

    sqlite3_stmt* st=nullptr;
    if(sqlite3_prepare_v2(db, sql2, -1, &st, nullptr) != SQLITE_OK) return nullopt;
    sqlite3_bind_int64(st, 1, from);
    sqlite3_bind_int64(st, 2, to);
    sqlite3_bind_int64(st, 3, from);
    sqlite3_bind_int64(st, 4, step);

    while(sqlite3_step(st) == SQLITE_ROW){
      int64_t ts = sqlite3_column_int64(st, 0);
      double temp = sqlite3_column_double(st, 1);
      s.series.push_back({ts,temp});
    }
    sqlite3_finalize(st);

    return s;
  }
};

// Сборка HTTP ответа строкой (минимальный HTTP/1.1)
static string http_response(int code, const string& ct, const string& body){
  const char* msg = (code==200) ? "OK" : (code==404 ? "Not Found" : "Error");
  ostringstream os;
  os << "HTTP/1.1 " << code << " " << msg << "\r\n";
  os << "Content-Type: " << ct << "\r\n";
  os << "Content-Length: " << body.size() << "\r\n";
  os << "Connection: close\r\n";
  os << "Access-Control-Allow-Origin: *\r\n"; // чтобы browser/Qt GUI могли дергать API без CORS проблем
  os << "\r\n";
  os << body;
  return os.str();
}

// Отправка данных до конца (send может отправить кусок, поэтому loop)
static bool send_all(SOCKET c, const string& data){
  const char* p=data.c_str();
  size_t left=data.size();
  while(left){
#ifdef _WIN32
    int n = ::send(c, p, (int)left, 0);
#else
    ssize_t n = ::send(c, p, left, 0);
#endif
    if(n <= 0) return false;
    p += n;
    left -= (size_t)n;
  }
  return true;
}

// Читаем HTTP request до "\r\n\r\n" (заголовки), тело не нужно (GET)
static optional<string> recv_request(SOCKET c){
  string buf;
  buf.reserve(4096);
  char tmp[1024];
  while(true){
#ifdef _WIN32
    int n = ::recv(c, tmp, (int)sizeof(tmp), 0);
#else
    ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
#endif
    if(n <= 0) break;
    buf.append(tmp, tmp+n);
    if(buf.find("\r\n\r\n") != string::npos) break;
    if(buf.size() > 65536) break;
  }
  if(buf.empty()) return nullopt;
  return buf;
}

// URL decode: %xx и '+'
static string url_decode(const string& s){
  string out; out.reserve(s.size());
  for(size_t i=0;i<s.size();i++){
    if(s[i]=='%' && i+2<s.size()){
      int v = 0;
      for(int k=1;k<=2;k++){
        char c=s[i+k];
        v <<= 4;
        if(c>='0'&&c<='9') v += (c-'0');
        else if(c>='a'&&c<='f') v += (c-'a'+10);
        else if(c>='A'&&c<='F') v += (c-'A'+10);
      }
      out.push_back((char)v);
      i+=2;
    } else if(s[i]=='+') out.push_back(' ');
    else out.push_back(s[i]);
  }
  return out;
}

// query string "a=1&b=2" -> map
static unordered_map<string,string> parse_query(const string& q){
  unordered_map<string,string> m;
  size_t i=0;
  while(i<q.size()){
    size_t amp = q.find('&', i);
    string part = q.substr(i, amp==string::npos? string::npos : amp-i);
    size_t eq = part.find('=');
    if(eq!=string::npos){
      string k = url_decode(part.substr(0,eq));
      string v = url_decode(part.substr(eq+1));
      m[k]=v;
    }
    if(amp==string::npos) break;
    i = amp+1;
  }
  return m;
}

// Прочитать файл (для статики web/)
static string read_file_bin(const filesystem::path& p){
  ifstream f(p, ios::binary);
  if(!f) return "";
  ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Content-Type по расширению (чтобы браузер не ругался)
static string content_type_for(const string& path){
  string lower = path;
  for(char& c: lower) c = (char)tolower((unsigned char)c);
  if(lower.size()>=5 && lower.substr(lower.size()-5)==".html") return "text/html; charset=utf-8";
  if(lower.size()>=3 && lower.substr(lower.size()-3)==".js")   return "application/javascript; charset=utf-8";
  if(lower.size()>=4 && lower.substr(lower.size()-4)==".css")  return "text/css; charset=utf-8";
  if(lower.size()>=5 && lower.substr(lower.size()-5)==".json") return "application/json; charset=utf-8";
  return "application/octet-stream";
}

int main(int argc, char** argv){
  setvbuf(stderr, nullptr, _IONBF, 0);

  // обработка Ctrl+C (нормально обрабатывается выход были траблы )
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  // параметры по умолчанию
  string db_path="temp.db";
  bool serve=false;
  bool simulate=false;
  string bind_ip="127.0.0.1";
  int port=8080;
  string web_dir="./web";

  auto fatal = [&](const string& msg)->int{
    log_line("FATAL: " + msg);
    return 1;
  };

  // разбор аргументов командной строки
  try{
    for(int i=1;i<argc;i++){
      string a=argv[i];
      auto need = [&](const char* name)->string{
        if(i+1>=argc) throw runtime_error(string("missing value for ")+name);
        return string(argv[++i]);
      };
      if(a=="--db") db_path = need("--db");
      else if(a=="--serve") serve=true;
      else if(a=="--simulate") simulate=true;
      else if(a=="--bind") bind_ip = need("--bind");
      else if(a=="--port") port = stoi(need("--port"));
      else if(a=="--web-dir") web_dir = need("--web-dir");
      else if(a=="--help"){
        cout <<
          "Usage:\n"
          "  temp_logger --db temp.db --serve --bind 127.0.0.1 --port 8080 --simulate --web-dir ./web\n"
          "Endpoints:\n"
          "  /api/current\n"
          "  /api/stats?from=ISOZ&to=ISOZ\n";
        return 0;
      } else {
        throw runtime_error(string("unknown arg: ")+a);
      }
    }
  } catch(const exception& e){
    return fatal(e.what());
  }

#ifdef _WIN32
  // Windows: инициализация Winsock обязательна перед socket()
  WSADATA wsa{};
  if(WSAStartup(MAKEWORD(2,2), &wsa)!=0){
    return fatal("WSAStartup failed");
  }
#endif

  // открыть/инициализировать БД
  Db db;
  if(!db.open(db_path)){
    db.close();
#ifdef _WIN32
    WSACleanup();
#endif
    return fatal("DB open/init failed");
  }

  // статика web/ не критична, но предупредим
  if(!filesystem::exists(web_dir)){
    log_line("WARN: web dir not found: " + web_dir + " (static UI will 404)");
  }

  // поток симуляции: каждые 250мс в temp в SQLite
  thread sim_thr;
  if(simulate){
    sim_thr = thread([&](){
      mt19937_64 rng{1234567};
      normal_distribution<double> base(23.5, 0.9);
      uniform_real_distribution<double> noise(-0.8,0.8);
      while(!g_stop){
        int64_t ts = (int64_t)time(nullptr); // epoch seconds
        double temp = round((base(rng)+noise(rng))*1000.0)/1000.0;
        if(!db.insert(ts, temp)) log_line("WARN: DB insert failed");
        this_thread::sleep_for(chrono::milliseconds(250));
      }
    });
  }

  // если не попросили --serve, то делать нечего
  if(!serve){
    log_line("Nothing to do: use --serve (and optionally --simulate). Try --help");
    g_stop = true;
    if(sim_thr.joinable()) sim_thr.join();
    db.close();
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  // создать TCP сокет
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
  if(s == (SOCKET)INVALID_SOCKET){
    g_stop = true;
    if(sim_thr.joinable()) sim_thr.join();
    db.close();
#ifdef _WIN32
    WSACleanup();
#endif
    return fatal("socket() failed");
  }

  // reuseaddr чтобы быстрее перезапускать сервер
  int one=1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

  // bind на ip:port
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);

#ifdef _WIN32
  if(inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1){
    closesock(s);
    return fatal("bad --bind ip");
  }
#else
  if(inet_aton(bind_ip.c_str(), &addr.sin_addr) == 0){
    closesock(s);
    return fatal("bad --bind ip");
  }
#endif

  if(::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR){
    string msg = "bind() failed on " + bind_ip + ":" + to_string(port) + " (порт занят?)";
    closesock(s);
    g_stop = true;
    if(sim_thr.joinable()) sim_thr.join();
    db.close();
#ifdef _WIN32
    WSACleanup();
#endif
    return fatal(msg);
  }

  if(::listen(s, 64) == SOCKET_ERROR){
    closesock(s);
    g_stop = true;
    if(sim_thr.joinable()) sim_thr.join();
    db.close();
#ifdef _WIN32
    WSACleanup();
#endif
    return fatal("listen() failed");
  }

  log_line("OK: listening on http://" + bind_ip + ":" + to_string(port));
  log_line("DB: " + db_path);
  log_line("Web dir: " + web_dir);

  // основной цикл: принятие соединения, чиитаем request, роутим по path
  while(!g_stop){
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200*1000;

    // select чтобы не висеть в accept навечно, а периодически проверять g_stop
    int rc = select((int)(s+1), &rfds, nullptr, nullptr, &tv);
    if(rc <= 0) continue;

    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    SOCKET c = ::accept(s, (sockaddr*)&caddr, &clen);
    if(c == (SOCKET)INVALID_SOCKET) continue;

    // читаем запрос
    auto reqOpt = recv_request(c);
    if(!reqOpt){ closesock(c); continue; }

    string req = *reqOpt;
    string first = req.substr(0, req.find("\r\n"));

    // парс первой строкм: "GET /path?query HTTP/1.1"
    istringstream iss(first);
    string method, target, ver;
    iss >> method >> target >> ver;

    string path = target;
    string query;
    auto qpos = target.find('?');
    if(qpos != string::npos){
      path = target.substr(0,qpos);
      query = target.substr(qpos+1);
    }

    // поддерживаем только GET
    if(method != "GET"){
      send_all(c, http_response(404, "text/plain; charset=utf-8", "Not Found"));
      closesock(c);
      continue;
    }

    // API: current
    if(path == "/api/current"){
      auto cur = db.latest();
      string body;
      if(cur){
        body = string("{\"ts\":\"") + iso_utc_from_epoch(cur->first) + "\",\"temp\":" + to_string(cur->second) + "}";
      } else {
        body = "{\"ts\":null,\"temp\":null}";
      }
      send_all(c, http_response(200, "application/json; charset=utf-8", body));
      closesock(c);
      continue;
    }

    // API: stats
    if(path == "/api/stats"){
      auto m = parse_query(query);
      if(!m.count("from") || !m.count("to")){
        send_all(c, http_response(404, "text/plain; charset=utf-8", "missing from/to"));
        closesock(c);
        continue;
      }

      // сервер строго требует ISOZ (с 'Z' на конце)
      auto fromE = parse_iso_utc_to_epoch(m["from"]);
      auto toE   = parse_iso_utc_to_epoch(m["to"]);
      if(!fromE || !toE){
        send_all(c, http_response(404, "text/plain; charset=utf-8", "bad ISOZ"));
        closesock(c);
        continue;
      }

      auto st = db.stats(*fromE, *toE);
      if(!st){
        send_all(c, http_response(404, "text/plain; charset=utf-8", "bad range"));
        closesock(c);
        continue;
      }

      // JSON: агрегаты + series
      // series формат: [ ["ISOZ", temp], ["ISOZ", temp], ... ]
      ostringstream body;
      body << "{";
      body << "\"from\":\"" << iso_utc_from_epoch(st->from) << "\",";
      body << "\"to\":\""   << iso_utc_from_epoch(st->to)   << "\",";
      body << "\"count\":" << st->count << ",";
      body << "\"avg\":" << (isnan(st->avg)? string("null") : to_string(st->avg)) << ",";
      body << "\"min\":" << (isnan(st->mn)?  string("null") : to_string(st->mn))  << ",";
      body << "\"max\":" << (isnan(st->mx)?  string("null") : to_string(st->mx))  << ",";
      body << "\"series\":[";
      for(size_t i=0;i<st->series.size();i++){
        if(i) body << ",";
        body << "[\"" << iso_utc_from_epoch(st->series[i].first) << "\"," << st->series[i].second << "]";
      }
      body << "]}";

      send_all(c, http_response(200, "application/json; charset=utf-8", body.str()));
      closesock(c);
      continue;
    }

    // статика: "/" -> "/index.html"
    if(path == "/") path = "/index.html";

    // защита от выхода из web_dir через ../
    filesystem::path web_root = filesystem::path(web_dir).lexically_normal();
    filesystem::path f = (filesystem::path(web_dir) / path.substr(1)).lexically_normal();

    auto fstr = f.string();
    auto rstr = web_root.string();
    if(fstr.size() < rstr.size() || fstr.compare(0, rstr.size(), rstr) != 0 || !filesystem::exists(f)){
      send_all(c, http_response(404, "text/plain; charset=utf-8", "Not Found"));
      closesock(c);
      continue;
    }

    // читаем файл и отправляем
    string data = read_file_bin(f);
    send_all(c, http_response(200, content_type_for(f.string()), data));
    closesock(c);
  }

  // graceful shutdown
  log_line("Stopping...");
  closesock(s);
  g_stop = true;
  if(sim_thr.joinable()) sim_thr.join();
  db.close();

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
