#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 1 ]]; then
  echo "usage: install-linux-aarch64-toolchain.sh [toolchain-root]" >&2
  exit 2
fi

user_home="$(getent passwd "$(id -un)" | cut -d: -f6)"
if [[ -z "$user_home" || ! -d "$user_home" ]]; then
  echo "Unable to determine the WSL user home directory" >&2
  exit 2
fi

toolchain_root="${1:-$user_home/.local/share/testtools/toolchains/debian-bullseye-aarch64-glibc-2.31}"
download_root="$user_home/.cache/testtools/debian-bullseye-aarch64-glibc-2.31"
staging_root="${toolchain_root}.staging.$$"
mkdir -p "$download_root" "$staging_root"
trap 'rm -rf -- "$staging_root"' EXIT

packages=$(cat <<'EOF'
pool/main/b/binutils/binutils-common_2.35.2-2_amd64.deb 47cd7cb9bf37f657993a6ef2f24d4943ded5ef616d866f2d43bdf5e5b3abb6ea
pool/main/b/binutils/binutils-aarch64-linux-gnu_2.35.2-2_amd64.deb b5d852fa469ac600304a7737e4f05a1f32465eb79d362faedeb36856a5b69c45
pool/main/g/gcc-10/gcc-10-base_10.2.1-6_amd64.deb be65535e94f95fbf04b104e8ab36790476f063374430f7dfc6c516cbe2d2cd1e
pool/main/g/gcc-10-cross/gcc-10-cross-base_10.2.1-6cross1_all.deb 9555613216433c2db637fe15341d61e07dd20a81abcef9d72ec755dacc88957c
pool/main/g/gcc-10-cross/gcc-10-aarch64-linux-gnu-base_10.2.1-6cross1_amd64.deb ca7a5cb17094e5cb3adab9a3597ea3f7eb203c5f3c669980d191a43a8337e3d7
pool/main/g/gcc-10-cross/cpp-10-aarch64-linux-gnu_10.2.1-6cross1_amd64.deb d6767f29be8d81fe832e6b8c6d224ed0a95f079bd1d015899db88f592f49b9ba
pool/main/g/gcc-10-cross/gcc-10-aarch64-linux-gnu_10.2.1-6cross1_amd64.deb 05b3b40ef2f54e1852409cd9a59a98d962845ec3f2d1abc7ed40ccd949a7c1cd
pool/main/g/gcc-10-cross/g++-10-aarch64-linux-gnu_10.2.1-6cross1_amd64.deb 31360f3bea56e7032107762dbecca228adec19427e58150eebc74a86591bc5f4
pool/main/g/gcc-10-cross/libgcc-10-dev-arm64-cross_10.2.1-6cross1_all.deb eca208337cfdecc8d625cddb6dbc0bffa00b48cfec2d1cc347bfbac3abcea4f5
pool/main/g/gcc-10-cross/libgcc-s1-arm64-cross_10.2.1-6cross1_all.deb 2966823a38c100f77786a49f734d240c892fc86c473dbf1c862c23424900fd83
pool/main/g/gcc-10-cross/libgomp1-arm64-cross_10.2.1-6cross1_all.deb 02fe4d1b883d5f5fe7e5b547ac9543e3a937271fce29b0c26eed6268483a2b95
pool/main/g/gcc-10-cross/libitm1-arm64-cross_10.2.1-6cross1_all.deb e5acba714cfea8c8d02f1d890ff2cc8e5afb8928bf4f3b57461fc810e4945e54
pool/main/g/gcc-10-cross/libatomic1-arm64-cross_10.2.1-6cross1_all.deb a0a6a99ff833bc589554d2864ecf74be21dd1d2e8ae5cfbc22a2d7f87f329ce0
pool/main/g/gcc-10-cross/libasan6-arm64-cross_10.2.1-6cross1_all.deb 5d45a4be5239527f5075b4409f9c56ef21343090a16524ec7e5e208fba17cd88
pool/main/g/gcc-10-cross/liblsan0-arm64-cross_10.2.1-6cross1_all.deb 6bb9ff76e83f205e0c4d64efe5f47a7d7154954532a002e0ce88d6435e7ffd40
pool/main/g/gcc-10-cross/libtsan0-arm64-cross_10.2.1-6cross1_all.deb 9a73a346575de632a567e9caafa83c0170a8b13592f2076daf34fe04e79a4109
pool/main/g/gcc-10-cross/libubsan1-arm64-cross_10.2.1-6cross1_all.deb 7aa11373aa2a6b9d46cd3856786caf535b7e88f5e01f663ddc576dfa6b205f08
pool/main/g/gcc-10-cross/libstdc++-10-dev-arm64-cross_10.2.1-6cross1_all.deb ce6e193cfa963ebd894c48ca4bfd21dd7a95a5e74293fea68789abdbf3904b23
pool/main/g/gcc-10-cross/libstdc++6-arm64-cross_10.2.1-6cross1_all.deb fe90e9cdf5250a3ea77961223a3953f1fd54bd3c579053b762ba990aa4f61c4f
pool/main/c/cross-toolchain-base/libc6-arm64-cross_2.31-9cross4_all.deb ccd237814281910e72422a6e86a6ef7570105fc69119ff8fc82f6a4827a8a015
pool/main/c/cross-toolchain-base/libc6-dev-arm64-cross_2.31-9cross4_all.deb 4b88456f6913474587be547a865c6b42756b2031ca11ddf6285ea6ff4275a48d
pool/main/c/cross-toolchain-base/linux-libc-dev-arm64-cross_5.10.13-1cross4_all.deb 3a5c7920a299c88f69b7d64eb60fb6e65631cd57721a30ed60a2f6b7adf931d3
EOF
)

while read -r package_path package_hash; do
  [[ -z "$package_path" ]] && continue
  package_file="${package_path##*/}"
  package_file_path="$download_root/$package_file"
  if [[ ! -f "$package_file_path" ]] || ! printf '%s  %s\n' "$package_hash" "$package_file_path" | sha256sum -c - >/dev/null 2>&1; then
    curl --fail --location --connect-timeout 15 --max-time 180 \
      "https://deb.debian.org/debian/$package_path" -o "$package_file_path.tmp"
    printf '%s  %s\n' "$package_hash" "$package_file_path.tmp" | sha256sum -c -
    mv "$package_file_path.tmp" "$package_file_path"
  fi
  dpkg-deb -x "$package_file_path" "$staging_root"
done <<< "$packages"

mkdir -p "$(dirname "$toolchain_root")"
if [[ -e "$toolchain_root" ]]; then
  rm -rf -- "$toolchain_root"
fi
mv "$staging_root" "$toolchain_root"
trap - EXIT

export LD_LIBRARY_PATH="$toolchain_root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
compiler="$toolchain_root/usr/bin/aarch64-linux-gnu-g++-10"
if [[ ! -x "$compiler" || ! -d "$toolchain_root/usr/aarch64-linux-gnu/include" ]]; then
  echo "The extracted compiler or glibc sysroot is incomplete" >&2
  exit 1
fi
if ldd "$compiler" | grep -q 'not found'; then
  echo "The extracted compiler has missing WSL host dependencies" >&2
  exit 1
fi
sysroot_libc="$toolchain_root/usr/aarch64-linux-gnu/lib/libc.so.6"
sysroot_libc_real="$(readlink -f "$sysroot_libc")"
if [[ "$(basename "$sysroot_libc_real")" != "libc-2.31.so" ]]; then
  echo "The extracted sysroot is not glibc 2.31: $sysroot_libc_real" >&2
  exit 1
fi

printf 'Installed Debian Bullseye aarch64 toolchain at %s\n' "$toolchain_root"
compiler_version="$("$compiler" --version | head -1)"
if [[ "$compiler_version" != *"10.2.1"* ]]; then
  echo "The installed compiler is not Debian GCC 10.2.1: $compiler_version" >&2
  exit 1
fi
printf '%s\n' "$compiler_version"
printf 'glibc baseline: 2.31\n'
