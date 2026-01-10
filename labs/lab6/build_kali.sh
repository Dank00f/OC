#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

sudo apt-get update
sudo apt-get install -y cmake g++ qt6-base-dev qt6-base-dev-tools qt6-charts-dev

rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo "OK: ./build/temp_gui"
