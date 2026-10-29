#!/usr/bin/env bash
# Build the host tests and the offline renderer (needs only g++), run the VCF
# check, and render the demo WAVs into sim/out/.
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++17 -Wall vcf_test.cpp -o vcf_test
./vcf_test
g++ -O2 -std=c++17 -Wall render.cpp -o render
mkdir -p out
./render out
