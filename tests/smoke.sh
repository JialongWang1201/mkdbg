#!/usr/bin/env bash
# tests/smoke.sh — mkdbg host tool integration smoke test
#
# Builds mkdbg-native, wire-host, and seam-analyze from source via CMake,
# then runs a minimal set of checks to confirm the binaries are functional.
#
# Usage:
#   bash tests/smoke.sh              # build + test
#   SMOKE_BUILD_DIR=/tmp/b bash tests/smoke.sh   # custom build dir
#
# Exit codes:
#   0  all checks passed
#   1  a check failed
#   2  build or setup error
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${SMOKE_BUILD_DIR:-$(mktemp -d)}"
KEEP_BUILD="${SMOKE_KEEP_BUILD:-0}"

cleanup() {
  if [[ "${KEEP_BUILD}" == "0" && "${BUILD_DIR}" == /tmp/* ]]; then
    rm -rf "${BUILD_DIR}"
  fi
}
trap cleanup EXIT

# ── build ─────────────────────────────────────────────────────────────────────

echo "smoke: configuring in ${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" \
  --target mkdbg-native wire-host seam-analyze arch_test \
  --parallel >/dev/null

# ── binary checks ─────────────────────────────────────────────────────────────

MKDBG="${BUILD_DIR}/mkdbg-native"
WIRE="${BUILD_DIR}/wire-host"
SEAM="${BUILD_DIR}/seam-analyze"
ARCH_TEST="${BUILD_DIR}/arch_test"

check() {
  local label="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    echo "  PASS  ${label}"
  else
    echo "  FAIL  ${label}"
    exit 1
  fi
}

echo "smoke: running checks"

check "mkdbg-native is executable"    test -x "${MKDBG}"
check "wire-host is executable"       test -x "${WIRE}"
check "seam-analyze is executable"    test -x "${SEAM}"

check "mkdbg-native --version"        "${MKDBG}" --version
check "wire-host --version"           "${WIRE}" --version
check "seam-analyze --help"           "${SEAM}" --help

# seam-analyze with a known-good fixture
FIXTURE="${ROOT_DIR}/deps/seam/test/fixtures/normal_kdi_cascade.cfl"
if [[ -f "${FIXTURE}" ]]; then
  check "seam-analyze normal_kdi_cascade.cfl" \
    bash -c "${SEAM} ${FIXTURE} | grep -q VERDICT:"
else
  echo "  SKIP  seam-analyze fixture (submodule not initialised)"
fi

# arch plugin registry
check "arch_test 9/9"  bash -c "${ARCH_TEST} | grep -q '9/9 tests passed'"

echo ""
echo "smoke: OK"
