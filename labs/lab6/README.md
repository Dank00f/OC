# Lab6 GUI client

GUI-клиент к серверу (lab5). Требует на сервере:
- GET /api/current  -> {"ts":"...Z","temp":N}
- GET /api/stats?from=...Z&to=...Z -> {"from":"...Z","to":"...Z","count":N,"avg":X,"min":Y,"max":Z}

## Build (Kali)
./setup_lab6_all.sh
./build/temp_gui
