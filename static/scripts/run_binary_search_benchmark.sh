#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="${SCRIPT_DIR}/binary_search_benchmark.cpp"
OUTPUT="/tmp/binary_search_benchmark"

echo "=========================================================="
echo "Compiling Binary Search Benchmark for Apple Silicon..."
echo "=========================================================="

# Check for Apple Silicon / ARM64
ARCH="$(uname -m)"
FLAGS="-O3 -std=c++20"

if [[ "$ARCH" == "arm64" ]]; then
    FLAGS="${FLAGS} -mcpu=apple-m2"
fi

clang++ ${FLAGS} "${SOURCE}" -o "${OUTPUT}"

echo "Running benchmark..."
echo ""
"${OUTPUT}"
