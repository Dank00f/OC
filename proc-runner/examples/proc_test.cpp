#include "proc_runner.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>

static void print_status(const proc::Status& s) {
    std::cerr << "status: ";
    switch (s.code) {
        case proc::Result::Ok:         std::cerr << "Ok"; break;
        case proc::Result::Timeout:    std::cerr << "Timeout"; break;
        case proc::Result::Running:    std::cerr << "Running"; break;
        case proc::Result::NotStarted: std::cerr << "NotStarted"; break;
        case proc::Result::InvalidArg: std::cerr << "InvalidArg"; break;
        case proc::Result::SysError:   std::cerr << "SysError"; break;
    }
    if (!s.message.empty()) std::cerr << " (" << s.message << ")";
    if (s.sys_errno)        std::cerr << " [sys=" << s.sys_errno << "]";
    std::cerr << "\n";
}

int main(int argc, char** argv) {
    using namespace proc;

    if (argc < 2) {
#ifdef _WIN32
        std::cout
            << "examples:\n"
            << "  proc_test cmd /c echo hello\n"
            << "  proc_test cmd /c timeout /t 3 --timeout-ms 1000\n";
#else
        std::cout
            << "examples:\n"
            << "  proc_test /bin/echo hello\n"
            << "  proc_test /bin/sleep 3 --timeout-ms 1000\n";
#endif
        return 0;
    }

    SpawnOptions opts;
#ifdef _WIN32
    opts.create_no_window = false;
#endif
    int64_t timeout = -1;
    std::vector<std::string> argsv;
    std::string program = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--timeout-ms" && i + 1 < argc) {
            timeout = std::stoll(argv[++i]);
        } else if (a == "--workdir" && i + 1 < argc) {
            opts.workdir = argv[++i];
        } else {
            argsv.push_back(a);
        }
    }

    proc::Process* p = nullptr;
    auto s = spawn(program, argsv, opts, p);
    if (s.code != Result::Ok) { print_status(s); return 1; }

    std::cout << "spawned, running=" << (is_running(p) ? "yes" : "no") << "\n";

    s = wait(p, timeout);
    print_status(s);
    if (s.code == Result::Timeout) {
        std::cout << "still running, waiting...\n";
        s = wait(p, -1);
        print_status(s);
    }

    int code = -1;
    s = exit_code(p, code);
    if (s.code == Result::Ok) std::cout << "exit code: " << code << "\n";
    else                      print_status(s);

    close(p);
    return 0;
}
