#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

if [[ -n "${VM32_MEM_SIZE:-}" ]]; then
  cmake --fresh -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/arm-none-eabi.cmake" \
    -DVM32_MEM_SIZE="${VM32_MEM_SIZE}" \
    ${BOARD_UART_PORT:+-DBOARD_UART_PORT="${BOARD_UART_PORT}"}
else
  cmake --fresh -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/arm-none-eabi.cmake" \
    ${BOARD_UART_PORT:+-DBOARD_UART_PORT="${BOARD_UART_PORT}"}
fi
cmake --build "${BUILD_DIR}" --parallel
