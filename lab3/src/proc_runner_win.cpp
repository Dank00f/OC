#include "proc_runner.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace proc {

static std::string quote_arg(const std::string& s) {
    if (s.empty()) return "\"\"";
    bool need = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '"') { need = true; break; }
    }
    if (!need) return s;

    std::string out;
    out.push_back('"');
    int bs = 0;
    for (char c : s) {
        if (c == '\\') {
            bs++;
        } else if (c == '"') {
            out.append(bs * 2 + 1, '\\');
            out.push_back('"');
            bs = 0;
        } else {
            out.append(bs, '\\');
            bs = 0;
            out.push_back(c);
        }
    }
    out.append(bs * 2, '\\');
    out.push_back('"');
    return out;
}

int run_program(const std::string& path,
                const std::vector<std::string>& args,
                bool wait,
                int& exit_code)
{
    exit_code = 0;

    std::string cmd = quote_arg(path);
    for (const auto& a : args) {
        cmd.push_back(' ');
        cmd += quote_arg(a);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmdline.data(),
        nullptr, nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!ok) return -1;

    CloseHandle(pi.hThread);

    if (!wait) {
        CloseHandle(pi.hProcess);
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        exit_code = (int)code;
    } else {
        exit_code = -1;
    }

    CloseHandle(pi.hProcess);
    return 0;
}

} // namespace proc

#endif
