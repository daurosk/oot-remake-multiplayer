#!/bin/sh
# Build, run the unit tests and the content check, as CI does. Run from anywhere.
# Usage: tools/dev-check.sh [build folder, default build]
set -e
cd "$(dirname "$0")/.."
dir="${1:-build}"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
cmake -S . -B "$dir" -DCOOP_BUILD_TESTS=ON
cmake --build "$dir" --config Release --parallel
ctest --test-dir "$dir" -C Release --output-on-failure
"$py" tools/check_content.py
echo "dev-check: all passed"
