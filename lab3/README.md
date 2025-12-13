# Лаб3 — proc_runner (cross-platform)

Задача: библиотека `proc_runner` + тестовая утилита `proc_test`, которые запускают внешний процесс с аргументами, с опцией ожидания завершения и получения `exit_code`.

Поддерживаемые ОС:
- Linux / POSIX (fork/exec + waitpid)
- Windows (CreateProcess + WaitForSingleObject)

## Структура
- `include/proc_runner.hpp` — API
- `src/proc_runner_posix.cpp` — реализация для POSIX
- `src/proc_runner_win.cpp` — реализация для Windows
- `tests/proc_test.cpp` — тест/пример запуска

## Сборка (Windows, MSVC + NMake)
Открой **Developer Command Prompt** (VsDevCmd):

```bat
cd /d %USERPROFILE%\OC\lab3
rmdir /s /q build 2>nul
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\proc_test.exe
```

## Пример запуска внешней программы:

```bat
build\proc_test.exe C:\Windows\System32\ping.exe ya.ru -n 2
```

## Сборка (Linux / Kali)

```bat
cd ~/OC/lab3
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/proc_test
```

>Пример:

```bat
./build/proc_test /bin/echo hello world
```

