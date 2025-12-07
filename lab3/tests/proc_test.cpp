#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include "proc_runner.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <program> [args...]\n";
        return 2;
    }

    std::string path = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

    int exit_code = -1;
    int rc = proc::run_program(path, args, true, exit_code);

    if (rc != 0) {
        std::cerr << "spawn failed\n";
        return 1;
    }

    std::cout << "spawned, running=yes\n";
    std::cerr << "status: " << (rc == 0 ? "Ok" : "Fail") << "\n";
    std::cout << "exit code: " << exit_code << "\n";
    return 0;
}
