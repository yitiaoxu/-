#!/bin/bash

rm -rf ./build

cmake -S . -B build

# 限制为 2 个线程（根据 RK3588 的 CPU 和内存调整）
cmake --build build --parallel 2