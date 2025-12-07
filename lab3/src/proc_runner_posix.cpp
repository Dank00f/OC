#ifndef _WIN32
#include "proc_runner.hpp"
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace proc {
int run_program(const std::string& path,
                const std::vector<std::string>& args,
                bool wait,
                int& exit_code)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(path.c_str()));
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    } else if (pid == 0) {
        execvp(path.c_str(), argv.data());
        _exit(127);
    } else {
        if (!wait) return 0;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return -1;
        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
        else exit_code = -1;
        return 0;
    }
}
}
#endif
