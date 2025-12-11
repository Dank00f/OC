#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

using clk = std::chrono::system_clock;
using namespace std::chrono_literals;

static std::string iso_utc_from_epoch(long long sec_floor){
    std::time_t tt = (std::time_t)sec_floor;
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  gm.tm_year+1900, gm.tm_mon+1, gm.tm_mday,
                  gm.tm_hour, gm.tm_min, gm.tm_sec);
    return std::string(buf);
}

int main() {
    std::mt19937_64 rng{1234567};
    std::normal_distribution<double> base(23.5, 0.9);
    while (true) {
        auto tnow = clk::now();
        long long ts = std::chrono::duration_cast<std::chrono::seconds>(tnow.time_since_epoch()).count();
        double temp = std::round(base(rng)*1000.0)/1000.0;
        std::cout << iso_utc_from_epoch(ts) << "," << std::fixed << std::setprecision(3) << temp << std::endl;
        std::this_thread::sleep_for(1s);
    }
}
