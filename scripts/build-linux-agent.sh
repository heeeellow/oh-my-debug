#!/usr/bin/env bash
set -euo pipefail

configuration="${1:-Release}"
output_root="${2:-}"

case "$configuration" in
  Debug|Release) ;;
  *) echo "configuration must be Debug or Release" >&2; exit 2 ;;
esac

machine="$(uname -m)"
case "$machine" in
  aarch64|arm64) architecture="aarch64" ;;
  *) echo "unsupported Linux architecture: $machine (ARM only)" >&2; exit 2 ;;
esac

script_directory="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repository_root="$(dirname -- "$script_directory")"
source_directory="$repository_root/src/agent"
build_directory="$repository_root/build/agent/linux-$architecture-$configuration"
if [[ -z "$output_root" ]]; then
  output_root="$repository_root/build/agents"
fi
artifact_directory="$output_root/linux/$architecture"

cmake_arguments=(
  -S "$source_directory"
  -B "$build_directory"
  "-DCMAKE_BUILD_TYPE=$configuration"
  -DTESTTOOLS_BUILD_TESTS=ON
)
if command -v ninja >/dev/null 2>&1; then
  cmake_arguments+=( -G Ninja )
fi

cmake "${cmake_arguments[@]}"
cmake --build "$build_directory" --parallel
ctest --test-dir "$build_directory" --output-on-failure
cmake -E make_directory "$artifact_directory"
cmake -E copy_if_different "$build_directory/testtools-agent" \
  "$artifact_directory/testtools-agent"

echo "Linux Agent ready: $artifact_directory/testtools-agent"
