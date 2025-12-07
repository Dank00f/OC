# ЛР 3: Процессы и запуск программ (POSIX)

## Структура

lab3/
include/proc_runner.hpp
src/proc_runner_posix.cpp
tests/proc_test.cpp
CMakeLists.txt


## Сборка (Linux/Kali)
```
cmake -S . -B build_posix
cmake --build build_posix -j
```
Примеры запуска
```
./build_posix/proc_test /bin/echo hello
./build_posix/proc_test /bin/sleep 1
./build_posix/proc_test /bin/ls -la
```

Проверка зависимостей
```
ldd ./build_posix/proc_test
```
Отладка (по желанию)
```
strace -f -o trace.txt ./build_posix/proc_test /bin/echo hi || true
tail -n 120 trace.txt
```


