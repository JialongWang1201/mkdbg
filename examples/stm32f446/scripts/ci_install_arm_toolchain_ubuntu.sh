#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "error: apt-get is required" >&2
  exit 2
fi

CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME:-}")"
if [[ -z "${CODENAME}" ]]; then
  echo "error: unable to resolve Ubuntu codename" >&2
  exit 2
fi

cat > /tmp/ubuntu-ci.list <<EOF
deb http://archive.ubuntu.com/ubuntu ${CODENAME} main universe restricted multiverse
deb http://archive.ubuntu.com/ubuntu ${CODENAME}-updates main universe restricted multiverse
deb http://archive.ubuntu.com/ubuntu ${CODENAME}-backports main universe restricted multiverse
deb http://security.ubuntu.com/ubuntu ${CODENAME}-security main universe restricted multiverse
EOF

sudo apt-get -o Acquire::Retries=3 \
  -o Dir::Etc::sourcelist=/tmp/ubuntu-ci.list \
  -o Dir::Etc::sourceparts=- \
  update
sudo apt-get -o Acquire::Retries=3 \
  -o Dir::Etc::sourcelist=/tmp/ubuntu-ci.list \
  -o Dir::Etc::sourceparts=- \
  install -y --no-install-recommends \
  cmake \
  gcc-arm-none-eabi \
  libnewlib-arm-none-eabi \
  libstdc++-arm-none-eabi-newlib \
  binutils-arm-none-eabi
