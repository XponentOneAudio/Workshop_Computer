#!/usr/bin/env bash
# Regenerate ../src/toaster_tables.h (needs only g++).
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++17 -Wall gen_tables.cpp -o gen_tables
./gen_tables
rm -f gen_tables
