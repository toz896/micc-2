#!/bin/sh
# Linux / WSL:  sh tests/run_tests.sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra test_dsp.cpp ../src/plugin.cpp ../src/gui_stub.cpp -o /tmp/veq_test
/tmp/veq_test
