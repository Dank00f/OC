# Lab 5


# ЛР4: Температурный логгер (C/C++, кроссплатформенно)

## Сборка (Linux)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Запуск (симуляция устройства)
```bash
mkdir -p logs
./build/temp_logger --simulate --log-dir ./logs
```

## Проверка почасовой сводки (libfaketime)
```bash
LIB=/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1
env LD_PRELOAD="$LIB" FAKETIME="2025-12-10 10:59:50" TZ=UTC timeout -k 1 25s ./build/temp_logger --simulate --log-dir ./logs
cat logs/hourly_avg.log
```

## Сборка (Windows, MSVC)
```bat
cd labs\lab4
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\temp_logger.exe --simulate --log-dir .\logs
```
