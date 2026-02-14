#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using socket_t = SOCKET;
  static void sock_close(socket_t s){ closesocket(s); }
  static bool sock_init(){
      WSADATA w{};
      return WSAStartup(MAKEWORD(2,2), &w) == 0;
  }
  static void sock_cleanup(){ WSACleanup(); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static void sock_close(socket_t s){ close(s); }
  static bool sock_init(){ return true; }
  static void sock_cleanup(){}
#endif

static std::atomic<bool> g_stop{false};
static void on_signal(int){ g_stop = true; }

static time_t timegm_portable(std::tm* t){
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}

static bool gmtime_r_portable(const time_t* tt, std::tm* out){
#ifdef _WIN32
    return gmtime_s(out, tt) == 0;
#else
    return gmtime_r(tt, out) != nullptr;
#endif
}

static time_t parse_iso_utc(const std::string& iso){
  
    if (iso.size() < 20) return (time_t)-1;
    if (!(iso[4]=='-' && iso[7]=='-' && iso[10]=='T' && iso[13]==':' && iso[16]==':' && iso.back()=='Z')) return (time_t)-1;

    std::tm t{};
    try{
        t.tm_year = std::stoi(iso.substr(0,4)) - 1900;
        t.tm_mon  = std::stoi(iso.substr(5,2)) - 1;
        t.tm_mday = std::stoi(iso.substr(8,2));
        t.tm_hour = std::stoi(iso.substr(11,2));
        t.tm_min  = std::stoi(iso.substr(14,2));
        t.tm_sec  = std::stoi(iso.substr(17,2));
        t.tm_isdst = 0;
    } catch(...) { return (time_t)-1; }

    return timegm_portable(&t);
}

static std::string iso_utc_from(time_t tt){
    std::tm t{};
    if(!gmtime_r_portable(&tt, &t)) return "1970-01-01T00:00:00Z";
    std::ostringstream os;
    os << std::put_time(&t, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}


static int hexval(char c){
    if (c>='0' && c<='9') return c - '0';
    if (c>='a' && c<='f') return 10 + (c - 'a');
    if (c>='A' && c<='F') return 10 + (c - 'A');
    return -1;
}

static std::string url_decode(const std::string& s){
    std::string out;
    out.reserve(s.size());
    for (size_t i=0;i<s.size();i++){
        if (s[i]=='%' && i+2<s.size()){
            int a=hexval(s[i+1]), b=hexval(s[i+2]);
            if (a>=0 && b>=0){
                out.push_back(char((a<<4)|b));
                i+=2;
                continue;
            }
        }
        if (s[i]=='+'){ out.push_back(' '); continue; }
        out.push_back(s[i]);
    }
    return out;
}

static std::unordered_map<std::string,std::string> parse_query(const std::string& q){
    std::unordered_map<std::string,std::string> m;
    size_t i=0;
    while(i<q.size()){
        size_t amp = q.find('&', i);
        if (amp==std::string::npos) amp=q.size();
        size_t eq = q.find('=', i);
        if (eq==std::string::npos || eq>amp){
            std::string k = url_decode(q.substr(i, amp-i));
            if(!k.empty()) m[k] = "";
        } else {
            std::string k = url_decode(q.substr(i, eq-i));
            std::string v = url_decode(q.substr(eq+1, amp-(eq+1)));
            if(!k.empty()) m[k] = v;
        }
        i = amp + 1;
    }
    return m;
}

static std::string json_escape(const std::string& s){
    std::ostringstream os;
    for(char c: s){
        switch(c){
            case '\\': os<<"\\\\"; break;
            case '"':  os<<"\\\""; break;
            case '\n': os<<"\\n"; break;
            case '\r': os<<"\\r"; break;
            case '\t': os<<"\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    os<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<(int)(unsigned char)c<<std::dec;
                } else os<<c;
        }
    }
    return os.str();
}

struct Sample { time_t tt{}; double temp{}; };

static bool parse_csv_line(const std::string& line, Sample& s){
    auto p = line.find(',');
    if (p==std::string::npos) return false;
    std::string ts = line.substr(0,p);
    std::string vs = line.substr(p+1);
    time_t tt = parse_iso_utc(ts);
    if (tt==(time_t)-1) return false;
    char* end=nullptr;
    double v = std::strtod(vs.c_str(), &end);
    if (end==vs.c_str()) return false;
    s.tt = tt;
    s.temp = v;
    return true;
}

static bool read_last_sample(const std::filesystem::path& file, Sample& out){
    std::ifstream f(file);
    if(!f) return false;
    std::string line, last;
    while(std::getline(f,line)){
        if(!line.empty()) last=line;
    }
    if(last.empty()) return false;
    return parse_csv_line(last, out);
}

struct Stats {
    size_t count=0;
    double sum=0.0;
    double minv= std::numeric_limits<double>::infinity();
    double maxv=-std::numeric_limits<double>::infinity();
    void add(double x){
        count++;
        sum += x;
        minv = std::min(minv, x);
        maxv = std::max(maxv, x);
    }
    double avg() const { return count? sum/double(count) : std::numeric_limits<double>::quiet_NaN(); }
};

static std::string http_response(int code, const std::string& content_type, const std::string& body){
    std::ostringstream os;
    if (code==200) os<<"HTTP/1.1 200 OK\r\n";
    else if (code==404) os<<"HTTP/1.1 404 Not Found\r\n";
    else os<<"HTTP/1.1 500 Internal Server Error\r\n";

    os<<"Content-Type: "<<content_type<<"\r\n";
    os<<"Content-Length: "<<body.size()<<"\r\n";
    os<<"Connection: close\r\n";
    os<<"Access-Control-Allow-Origin: *\r\n";
    os<<"\r\n";
    os<<body;
    return os.str();
}

static bool send_all(socket_t s, const std::string& data){
    const char* p = data.data();
    size_t left = data.size();
    while(left){
#ifdef _WIN32
        int n = ::send(s, p, (int)left, 0);
#else
        ssize_t n = ::send(s, p, left, 0);
#endif
        if (n<=0) return false;
        p += n;
        left -= (size_t)n;
    }
    return true;
}

static std::string recv_request(socket_t s){
    std::string buf;
    buf.reserve(8192);
    char tmp[2048];
    while(buf.find("\r\n\r\n")==std::string::npos && buf.size()<65536){
#ifdef _WIN32
        int n = ::recv(s, tmp, (int)sizeof(tmp), 0);
#else
        ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
        if (n<=0) break;
        buf.append(tmp, tmp+n);
    }
    return buf;
}

static void handle_client(socket_t c,
                          const std::filesystem::path& data_dir,
                          std::mutex& mtx,
                          Sample& latest)
{
    std::string req = recv_request(c);
    
    std::istringstream is(req);
    std::string method, target, ver;
    is >> method >> target >> ver;
    if (method != "GET" || target.empty()){
        send_all(c, http_response(404, "text/plain", ""));
        return;
    }

    std::string path = target;
    std::string query;
    auto qpos = target.find('?');
    if (qpos!=std::string::npos){
        path = target.substr(0,qpos);
        query = target.substr(qpos+1);
    }

    if (path == "/api/current"){
        Sample cur{};
        {
            std::lock_guard<std::mutex> lk(mtx);
            cur = latest;
        }
        std::ostringstream body;
        body<<"{\"ts\":\""<<json_escape(iso_utc_from(cur.tt))<<"\",\"temp\":"<<std::fixed<<std::setprecision(3)<<cur.temp<<"}";
        send_all(c, http_response(200, "application/json", body.str()));
        return;
    }

    if (path == "/api/stats"){
        auto q = parse_query(query);
        auto itf = q.find("from");
        auto itt = q.find("to");
        if (itf==q.end() || itt==q.end()){
            send_all(c, http_response(500, "application/json", "{\"error\":\"from/to required\"}"));
            return;
        }
        time_t from = parse_iso_utc(itf->second);
        time_t to   = parse_iso_utc(itt->second);
        if (from==(time_t)-1 || to==(time_t)-1 || to<=from){
            send_all(c, http_response(500, "application/json", "{\"error\":\"bad from/to\"}"));
            return;
        }

        std::filesystem::path file = data_dir / "measurements.csv";
        std::ifstream f(file);
        Stats st;
        std::vector<Sample> samples;
        samples.reserve(2048);

        if (f){
            std::string line;
            while(std::getline(f,line)){
                if(line.empty()) continue;
                Sample s{};
                if(!parse_csv_line(line,s)) continue;
                if (s.tt >= from && s.tt <= to){
                    st.add(s.temp);
                    samples.push_back(s);
                }
            }
        }

       
        const size_t MAXP = 300;
        if (samples.size() > MAXP){
            size_t step = (samples.size() + MAXP - 1) / MAXP;
            std::vector<Sample> ds;
            ds.reserve((samples.size()+step-1)/step);
            for(size_t i=0;i<samples.size();i+=step) ds.push_back(samples[i]);
            samples.swap(ds);
        }

        std::ostringstream body;
        body<<"{";
        body<<"\"from\":\""<<json_escape(itf->second)<<"\",";
        body<<"\"to\":\""<<json_escape(itt->second)<<"\",";
        body<<"\"count\":"<<st.count<<",";
        if (st.count){
            body<<"\"avg\":"<<std::fixed<<std::setprecision(3)<<st.avg()<<",";
            body<<"\"min\":"<<std::fixed<<std::setprecision(3)<<st.minv<<",";
            body<<"\"max\":"<<std::fixed<<std::setprecision(3)<<st.maxv<<",";
        } else {
            body<<"\"avg\":null,\"min\":null,\"max\":null,";
        }
        body<<"\"samples\":[";
        for(size_t i=0;i<samples.size();i++){
            if(i) body<<",";
            body<<"{\"ts\":\""<<json_escape(iso_utc_from(samples[i].tt))<<"\",\"temp\":"<<std::fixed<<std::setprecision(3)<<samples[i].temp<<"}";
        }
        body<<"]}";
        send_all(c, http_response(200, "application/json", body.str()));
        return;
    }

  
    if (path == "/" || path == "/index.html"){
        const char* html =
            "<!doctype html><html><head><meta charset='utf-8'><title>Temp Server</title></head>"
            "<body><h3>Temp Server</h3><p>Endpoints:</p><ul>"
            "<li>/api/current</li><li>/api/stats?from=...&to=...</li></ul></body></html>";
        send_all(c, http_response(200, "text/html; charset=utf-8", html));
        return;
    }

    send_all(c, http_response(404, "text/plain", ""));
}

int main(int argc, char** argv){
    std::string data_dir = "data";
    int port = 8080;
    bool simulate = false;

    for(int i=1;i<argc;i++){
        std::string a = argv[i];
        if (a=="--data-dir" && i+1<argc) data_dir = argv[++i];
        else if (a=="--port" && i+1<argc) port = std::atoi(argv[++i]);
        else if (a=="--simulate") simulate = true;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if(!sock_init()){
        std::cerr<<"socket init failed\n";
        return 1;
    }

    std::filesystem::path dd(data_dir);
    std::error_code ec;
    std::filesystem::create_directories(dd, ec);
    std::filesystem::path file = dd / "measurements.csv";

    std::mutex mtx;
    Sample latest{};
    latest.tt = std::time(nullptr);
    latest.temp = 23.5;

    Sample last{};
    if (read_last_sample(file, last)){
        latest = last;
    }

    std::atomic<bool> sim_stop{false};
    std::thread sim_thr;
    if (simulate){
        sim_thr = std::thread([&](){
            std::mt19937_64 rng{1234567};
            std::normal_distribution<double> base(23.5, 0.9);
            std::uniform_real_distribution<double> noise(-0.8,0.8);

            while(!g_stop && !sim_stop){
                time_t now = std::time(nullptr);
                double temp = std::round((base(rng)+noise(rng))*1000.0)/1000.0;

                {
                    std::lock_guard<std::mutex> lk(mtx);
                    latest.tt = now;
                    latest.temp = temp;
                }

                std::ofstream out(file, std::ios::app);
                if (out){
                    out<<iso_utc_from(now)<<","<<std::fixed<<std::setprecision(3)<<temp<<"\n";
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }

    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == (socket_t)-1){
        std::cerr<<"socket() failed\n";
        g_stop = true;
    }

#ifdef _WIN32
    BOOL yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#else
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (!g_stop && ::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0){
        std::cerr<<"bind failed (port "<<port<<")\n";
        g_stop = true;
    }

    if (!g_stop && ::listen(s, 16) != 0){
        std::cerr<<"listen failed\n";
        g_stop = true;
    }

    std::cout<<"temp_server listening on http://127.0.0.1:"<<port<<"\n";
    std::cout<<"data: "<<std::filesystem::absolute(dd).string()<<"\n";
    std::cout<<"simulate: "<<(simulate?"ON":"OFF")<<"\n";

    while(!g_stop){
        sockaddr_in caddr{};
#ifdef _WIN32
        int clen = sizeof(caddr);
#else
        socklen_t clen = sizeof(caddr);
#endif
        socket_t c = ::accept(s, (sockaddr*)&caddr, &clen);
#ifdef _WIN32
        if (c == INVALID_SOCKET){
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
#else
        if (c < 0){
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
#endif
        // one-thread-per-conn is fine for lab
        std::thread([&, c](){
            handle_client(c, dd, mtx, latest);
            sock_close(c);
        }).detach();
    }

    sim_stop = true;
    if (sim_thr.joinable()) sim_thr.join();
    sock_close(s);
    sock_cleanup();
    return 0;
}
