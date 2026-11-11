#!/usr/bin/env bash
set -euo pipefail

# The toolchain is required even when the optional compiler cache is unavailable.
test -f "$ANDROID_NDK_HOME/source.properties"
test -x "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
echo "ndk=$(sha256sum "$ANDROID_NDK_HOME/source.properties" | cut -d ' ' -f 1)" >> "$GITHUB_OUTPUT"

if ! command -v ccache >/dev/null 2>&1; then
  # Bound package-manager retries so a runner network failure cannot stall the build.
  apt_options=(-o Acquire::Retries=2 -o Acquire::http::Timeout=10 -o Acquire::https::Timeout=10)
  if ! timeout 60s sudo apt-get "${apt_options[@]}" update -qq ||
     ! timeout 60s sudo apt-get "${apt_options[@]}" install -y --no-install-recommends ccache; then
    echo "::warning::Could not install ccache; continuing without a compiler cache."
  fi
fi

launcher=$(command -v ccache || true)
echo "launcher=$launcher" >> "$GITHUB_OUTPUT"
if [[ -z "$launcher" ]]; then
  echo "available=false" >> "$GITHUB_OUTPUT"
  exit 0
fi

echo "available=true" >> "$GITHUB_OUTPUT"
{
  echo "CCACHE_DIR=$RUNNER_TEMP/ccache"
  echo "CCACHE_BASEDIR=$GITHUB_WORKSPACE"
  echo "CCACHE_COMPILERCHECK=content"
  echo "CCACHE_MAXSIZE=1G"
} >> "$GITHUB_ENV"
