#!/bin/sh
# Build the Node-API addons. Zig's C++ toolchain links libc++ statically and targets
# glibc 2.17, so the .node files load on any x86-64 or aarch64 Node 22 image
# (no libstdc++ version match, no runtime dependencies beyond libc).
set -e
cd "$(dirname "$0")"
zig c++ -O3 -std=c++20 -fPIC -shared -target x86_64-linux-gnu.2.17 \
  -I"${NODE_INCLUDE:-/usr/include/node}" engine.cpp -o engine.node
strip engine.node
zig c++ -O3 -std=c++20 -fPIC -shared -s -target aarch64-linux-gnu.2.17 \
  -I"${NODE_INCLUDE:-/usr/include/node}" engine.cpp -o engine.arm64.node
