#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
ELF_FILE="${BUILD_DIR}/MicroKernel_MPU.elf"
OPENOCD_CFG="${ROOT_DIR}/tools/openocd.cfg"

if [[ ! -f "${OPENOCD_CFG}" ]]; then
  echo "OpenOCD config not found: ${OPENOCD_CFG}" >&2
  exit 1
fi

if [[ ! -f "${ELF_FILE}" ]]; then
  echo "ELF not found: ${ELF_FILE}" >&2
  echo "Run: ${ROOT_DIR}/tools/build.sh" >&2
  exit 1
fi

openocd -f "${OPENOCD_CFG}" -c "program ${ELF_FILE} verify reset exit"
