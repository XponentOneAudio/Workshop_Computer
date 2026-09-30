#!/bin/sh
# Desktop tests: seeding, DSP and engines (C++), and the webapp's seeding (JS).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -fsanitize=undefined,address \
	-fno-sanitize-recover=undefined test/host_test.cpp -o build/host_test
build/host_test
node web/seed.test.mjs

# Browser test, if Playwright is installed (locally or globally)
if NODE_PATH="$(npm root -g 2>/dev/null)" node -e "require('playwright')" 2>/dev/null; then
	NODE_PATH="$(npm root -g)" node test/web_test.mjs "$@"
else
	echo "skipping browser test (Playwright not installed)"
fi
