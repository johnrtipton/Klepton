#!/bin/bash
# Fail on the first failed build or gate; use Makefile source lists and flags.
set -euo pipefail
cd "$(dirname "$0")/.."
make check-host-O2
