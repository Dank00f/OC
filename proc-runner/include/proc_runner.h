#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace proc {
using millis = int64_t;

struct Process;

enum class Result { Ok=0, Timeout, Running, NotStarted, InvalidArg, SysError };

struct Status { Result code=Result::Ok; int sys_errno=0; std::string message; };

struct SpawnOptions { std::string workdir; bool create_no_window=true; };

Status spawn(const std::string& program,
             const std::vector<std::string>& args,
             const SpawnOptions& opts,
             Process*& out_proc);

bool   is_running(const Process* p);

Status wait(Process* p, millis timeout_ms);

Status exit_code(const Process* p, int& out_code);

void   close(Process*& p);

Status run_and_wait(const std::string& program,
                    const std::vector<std::string>& args,
                    const SpawnOptions& opts,
                    int& out_code);
} // namespace proc
