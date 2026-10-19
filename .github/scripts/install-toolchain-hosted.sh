#!/usr/bin/env bash

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export TZ=Etc/UTC

sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get update
sudo -E apt-get install -y build-essential clang clang-14 lld lld-14 llvm llvm-14 libssl-dev wget git zip
sudo -E apt-get build-dep -y linux

clang --version
ld.lld --version
