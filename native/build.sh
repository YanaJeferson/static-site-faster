#!/bin/sh
# Build the Node-API addon. Zig's C++ toolchain links libc++ statically and targets
# glibc 2.17, so the .node loads on any x86-64 Node 22 image (no libstdc++ version match).
set -e
cd "$(dirname "$0")"
zig c++ -O3 -std=c++20 -fPIC -shared -target x86_64-linux-gnu.2.17 \
  -I"${NODE_INCLUDE:-/usr/include/node}" engine.cpp -o engine.node
strip engine.node
