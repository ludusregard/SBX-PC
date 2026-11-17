#!/bin/bash
# Compiles files in src/game with warnings on (syntax and layout checks).
W=/home/claude/wp; D=$W/sbx_pc/tools/native_gen/difftest
cd $W/sbx_pc
for f in "$@"; do clang++ -std=c++23 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -mssse3 -msse4.1 -fsyntax-only -I$D/shim -I$W/sbx_pc/src -I$W/sbx_pc -I$W/sbx_pc/generated/default -I$W/rexglue-sdk/include -I$W/rexglue-sdk/thirdparty/fmt/include -I$W/rexglue-sdk/thirdparty/spdlog/include -I$W/rexglue-sdk/thirdparty/simde -I$W/rexglue-sdk/thirdparty -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -DSPDLOG_HEADER_ONLY src/game/$f.cpp 2>&1 | grep -E "(error|warning)" | grep -v "^In file\|SPDLOG_HEADER_ONLY\|warning generated" | sort -u | head -8; done; echo compiled "$@"
