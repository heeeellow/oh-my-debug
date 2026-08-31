#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: build-linux-agent-aarch64.sh <source> <output> <build> <toolchain-root> <toolchain-file>" >&2
  exit 2
fi

source_directory="$1"
output_file="$2"
build_directory="$3"
toolchain_root="$4"
toolchain_file="$5"

if [[ ! -d "$source_directory" ]]; then
  echo "Agent source directory not found: $source_directory" >&2
  exit 2
fi
if [[ ! -f "$toolchain_file" ]]; then
  echo "Linux aarch64 toolchain file not found: $toolchain_file" >&2
  exit 2
fi
if [[ ! -x "$toolchain_root/usr/bin/aarch64-linux-gnu-gcc-10" ||
      ! -x "$toolchain_root/usr/bin/aarch64-linux-gnu-g++-10" ]]; then
  echo "Debian Bullseye aarch64 compiler was not found below: $toolchain_root" >&2
  exit 2
fi
if [[ ! -d "$toolchain_root/usr/aarch64-linux-gnu/include" ]]; then
  echo "glibc sysroot was not found below: $toolchain_root" >&2
  exit 2
fi

cmake_program="${CMAKE_PROGRAM:-cmake}"
if ! command -v "$cmake_program" >/dev/null 2>&1; then
  echo "CMake is required inside WSL" >&2
  exit 2
fi

export LD_LIBRARY_PATH="$toolchain_root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
compiler="$toolchain_root/usr/bin/aarch64-linux-gnu-g++-10"
compiler_version="$("$compiler" --version | head -1)"
if [[ "$compiler_version" != *"10.2.1"* ]]; then
  echo "The selected compiler is not Debian GCC 10.2.1: $compiler_version" >&2
  exit 1
fi

generator_args=()
if command -v ninja >/dev/null 2>&1; then
  generator_args=(-G Ninja)
fi

"$cmake_program" -S "$source_directory" -B "$build_directory" "${generator_args[@]}" \
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain_file" \
  "-DTESTTOOLS_LINUX_AARCH64_TOOLCHAIN_ROOT=$toolchain_root" \
  -DCMAKE_BUILD_TYPE=Release \
  -DTESTTOOLS_BUILD_TESTS=OFF \
  -DTESTTOOLS_BUILD_FAKE_AGENT=OFF
"$cmake_program" --build "$build_directory" --parallel

mkdir -p "$(dirname "$output_file")"
"$cmake_program" -E copy "$build_directory/testtools-agent" "$output_file"

readelf_program="$toolchain_root/usr/bin/aarch64-linux-gnu-readelf"
if [[ ! -x "$readelf_program" ]]; then
  echo "Cross readelf was not found below: $toolchain_root" >&2
  exit 2
fi

sysroot_libc="$toolchain_root/usr/aarch64-linux-gnu/lib/libc.so.6"
if [[ ! -f "$sysroot_libc" ]]; then
  echo "glibc 2.31 libc.so.6 was not found in the sysroot" >&2
  exit 1
fi
sysroot_libc_real="$(readlink -f "$sysroot_libc")"
if [[ "$(basename "$sysroot_libc_real")" != "libc-2.31.so" ]]; then
  echo "The selected sysroot is not glibc 2.31: $sysroot_libc_real" >&2
  exit 1
fi

file_output="$(file "$output_file")"
if [[ "$file_output" != *"ELF 64-bit"* || "$file_output" != *"ARM aarch64"* ]]; then
  echo "Linux Agent is not an ARM64 ELF: $file_output" >&2
  exit 1
fi

dynamic_output="$("$readelf_program" -d "$output_file")"
if [[ "$dynamic_output" != *"NEEDED"* || "$dynamic_output" != *"libc.so.6"* ]]; then
  echo "Linux Agent does not have the expected glibc dynamic dependency" >&2
  exit 1
fi

glibc_versions="$("$readelf_program" --version-info "$output_file" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu || true)"
if [[ -z "$glibc_versions" ]]; then
  echo "Linux Agent has no inspectable GLIBC symbol versions" >&2
  exit 1
fi
highest_glibc="$(printf '%s\n' "$glibc_versions" | tail -1 | sed 's/^GLIBC_//')"
if [[ "$(printf '%s\n' 2.31 "$highest_glibc" | sort -V | tail -1)" != "2.31" ]]; then
  echo "Linux Agent requires GLIBC_$highest_glibc, above the 2.31 baseline" >&2
  exit 1
fi

echo "Linux Agent ready: $output_file"
echo "ELF: $file_output"
echo "GLIBC symbols: $(printf '%s' "$glibc_versions" | tr '\n' ' ')"
