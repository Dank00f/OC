#pragma once
#include <string>
#include <vector>

namespace proc {
    // Запускает внешнюю программу.
    // path  — путь к исполняемому файлу (например, /bin/echo)
    // args  — аргументы (без самого path)
    // wait  — ждать завершения или вернуть сразу
    // exit_code — код выхода дочернего процесса (если wait=true)
    // Возвращает 0 при успехе, -1 при ошибке запуска.
    int run_program(const std::string& path,
                    const std::vector<std::string>& args,
                    bool wait,
                    int& exit_code);
}
