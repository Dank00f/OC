#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

static std::string iso_utc_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
#if defined(_WIN32)
    std::tm t{};
    gmtime_s(&t, &tt);
#else
    std::tm t{};
    gmtime_r(&tt, &t);
#endif
    std::ostringstream os;
    os << std::put_time(&t, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

int main() {
    std::mt19937_64 rng{1234567};
    std::normal_distribution<double> base(23.5, 0.9);
    std::uniform_real_distribution<double> noise(-0.8, 0.8);

    while (true) {
        std::string ts = iso_utc_now();
        double temp = std::round((base(rng) + noise(rng)) * 1000.0) / 1000.0;
        std::cout << ts << "," << std::fixed << std::setprecision(3) << temp << "\n";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
