#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT="${BUILD_DIR}/mkdbg-native"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra -Werror -O2 \
  -o "${OUT}" \
  "${ROOT_DIR}/tools/mkdbg_native.c"

echo "built ${OUT}"
