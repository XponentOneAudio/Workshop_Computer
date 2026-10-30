#!/usr/bin/env bash
# Build and run the offline Dual Borg renderer. Needs only g++ (C++17).
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++17 -Wall render.cpp -o render
./render .
echo
echo "WAVs written to: $(pwd)"
