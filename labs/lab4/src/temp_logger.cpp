#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

#ifdef _WIN32
static time_t timegm_compat(tm* t) { return _mkgmtime(t); }
static void gmtime_compat(const time_t* tt, tm* out) { gmtime_s(out, tt); }
#else
static time_t timegm_compat(tm* t) { return timegm(t); }
static void gmtime_compat(const time_t* tt, tm* out) { gmtime_r(tt, out); }
#endif

static time_t parse_iso_utc(const string& iso) {
    tm t{};
    if (iso.size() < 20 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' ||
        iso[13] != ':' || iso[16] != ':' || iso.back() != 'Z')
        return (time_t)-1;

    t.tm_year = stoi(iso.substr(0, 4)) - 1900;
    t.tm_mon  = stoi(iso.substr(5, 2)) - 1;
    t.tm_mday = stoi(iso.substr(8, 2));
    t.tm_hour = stoi(iso.substr(11, 2));
    t.tm_min  = stoi(iso.substr(14, 2));
    t.tm_sec  = stoi(iso.substr(17, 2));
    t.tm_isdst = 0;

    return timegm_compat(&t);
}

static string iso_utc_from(time_t tt) {
    tm t{};
    gmtime_compat(&tt, &t);
    ostringstream os;
    os << put_time(&t, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

static time_t floor_hour(time_t tt) { return (tt / 3600) * 3600; }
static time_t floor_day (time_t tt) { return (tt / 86400) * 86400; }

struct Acc {
    double sum = 0.0;
    size_t n = 0;
    void add(double x) { sum += x; ++n; }
    double avg() const { return n ? sum / n : numeric_limits<double>::quiet_NaN(); }
    void reset() { sum = 0.0; n = 0; }
};

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    string logDir = "./logs";
    bool simulate = false;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--log-dir" && i + 1 < argc) logDir = argv[++i];
        else if (a == "--simulate") simulate = true;
    }

    fs::create_directories(fs::path(logDir));

    const string f_meas = (fs::path(logDir) / "measurements.log").string();
    const string f_hour = (fs::path(logDir) / "hourly_avg.log").string();
    const string f_day  = (fs::path(logDir) / "daily_avg.log").string();

    ofstream meas(f_meas, ios::app);
    ofstream hourly(f_hour, ios::app);
    ofstream daily(f_day, ios::app);

    if (!meas)   { cerr << "open fail " << f_meas << "\n"; return 1; }
    if (!hourly) { cerr << "open fail " << f_hour << "\n"; return 1; }
    if (!daily)  { cerr << "open fail " << f_day  << "\n"; return 1; }

    Acc acc_h, acc_d;
    bool have_h = false, have_d = false;
    time_t cur_h = -1, cur_d = -1;

    auto roll_hour = [&](time_t hstart) {
        if (acc_h.n == 0) return;
        hourly << iso_utc_from(hstart) << "," << fixed << setprecision(3) << acc_h.avg() << "\n";
        hourly.flush();
        acc_h.reset();
    };

    auto roll_day = [&](time_t dstart) {
        if (acc_d.n == 0) return;
        daily << iso_utc_from(dstart) << "," << fixed << setprecision(3) << acc_d.avg() << "\n";
        daily.flush();
        acc_d.reset();
    };

    auto feed_sample = [&](const string& ts_iso, double temp) {
        meas << ts_iso << "," << fixed << setprecision(3) << temp << "\n";
        meas.flush();

        time_t tt = parse_iso_utc(ts_iso);
        if (tt == (time_t)-1) return;

        time_t h = floor_hour(tt);
        time_t d = floor_day(tt);

        if (!have_h) { cur_h = h; have_h = true; }
        if (!have_d) { cur_d = d; have_d = true; }

        if (d != cur_d) {
            if (h != cur_h) { roll_hour(cur_h); cur_h = h; }
            roll_day(cur_d);
            cur_d = d;
        }
        if (h != cur_h) { roll_hour(cur_h); cur_h = h; }

        acc_h.add(temp);
        acc_d.add(temp);
    };

    if (simulate) {
        mt19937_64 rng{1234567};
        normal_distribution<double> base(23.5, 0.9);
        uniform_real_distribution<double> noise(-0.8, 0.8);

        while (!g_stop) {
            auto now = chrono::system_clock::now();
            time_t tt = chrono::system_clock::to_time_t(now);
            string ts = iso_utc_from(tt);
            double temp = round((base(rng) + noise(rng)) * 1000.0) / 1000.0;
            feed_sample(ts, temp);
            this_thread::sleep_for(chrono::milliseconds(200));
        }
    } else {
        string line;
        while (!g_stop && getline(cin, line)) {
            if (line.empty()) continue;
            auto p = line.find(',');
            if (p == string::npos) continue;
            string ts = line.substr(0, p);
            double v = atof(line.c_str() + p + 1);
            feed_sample(ts, v);
        }
    }

    if (have_h) roll_hour(cur_h);
    if (have_d) roll_day(cur_d);

    return 0;
}
