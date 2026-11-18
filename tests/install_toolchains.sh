#!/usr/bin/env bash
# Idempotent toolchain installer for JELLL tests / development.
# Heavy toolchains live under ~/.cache (snapshot-excluded) so they never bloat
# the workspace; rerun this script after a fresh session. The engine finds them
# via JELLL_TOOLCHAIN_DIR.
set -euo pipefail

TC="${JELLL_TOOLCHAIN_DIR:-$HOME/.cache/jelll-tools}"
PROJ=/home/user/jelll-linux
mkdir -p "$TC/bin" "$TC/downloads"

export JELLL_TOOLCHAIN_DIR="$TC"
export PLAYWRIGHT_BROWSERS_PATH="$TC/pw-browsers"

# ---- esbuild: npm install then copy the standalone native binary out ----
if [ ! -x "$TC/bin/esbuild" ]; then
  echo "[esbuild] installing"
  npm i --prefix "$TC/npm-scratch" esbuild@0.28.2 >/dev/null 2>&1
  cp "$TC/npm-scratch/node_modules/@esbuild/linux-x64/bin/esbuild" "$TC/bin/esbuild"
fi
"$TC/bin/esbuild" --version

# ---- Emscripten ----
if [ ! -x "$TC/emsdk/upstream/emscripten/emcc" ]; then
  echo "[emsdk] installing 6.0.13"
  if [ ! -d "$TC/emsdk" ]; then
    git clone --depth 1 --branch 6.0.13 https://github.com/emscripten-core/emsdk.git "$TC/emsdk"
  fi
  "$TC/emsdk/emsdk" install 6.0.13 >/dev/null
  "$TC/emsdk/emsdk" activate 6.0.13 >/dev/null
fi
"$TC/emsdk/upstream/emscripten/emcc" --version | head -1

# ---- Rust (rustup, wasm32 target) ----
if [ ! -x "$TC/cargo/bin/rustc" ]; then
  echo "[rust] installing rustup"
  ARCH=x86_64; [ "$(uname -m)" = aarch64 ] && ARCH=aarch64
  RURL="https://static.rust-lang.org/rustup/dist/${ARCH}-unknown-linux-gnu/rustup-init"
  curl --proto '=https' --tlsv1.2 -fsSL "$RURL" -o "$TC/downloads/rustup-init"
  curl --proto '=https' --tlsv1.2 -fsSL "$RURL.sha256" -o "$TC/downloads/rustup-init.sha256"
  (cd "$TC/downloads" && sha256sum -c rustup-init.sha256)
  chmod +x "$TC/downloads/rustup-init"
  RUSTUP_HOME="$TC/rustup" CARGO_HOME="$TC/cargo" \
    "$TC/downloads/rustup-init" -y --profile minimal --default-toolchain stable \
    --target wasm32-unknown-unknown --no-modify-path >/dev/null
fi
RUSTUP_HOME="$TC/rustup" CARGO_HOME="$TC/cargo" "$TC/cargo/bin/rustc" --version

# ---- Zig ----
if [ ! -x "$TC/zig/zig" ]; then
  echo "[zig] installing"
  curl -fsSL https://ziglang.org/download/index.json -o "$TC/downloads/zig-index.json"
  read -r ZURL ZSUM < <(node -e '
    const d=require(process.argv[1]);
    const v=Object.keys(d).find(k=>/^\d+\.\d+\.\d+$/.test(k));
    const e=d[v][process.argv[2]];
    console.log(e.tarball, e.shasum);' \
    "$TC/downloads/zig-index.json" "$(uname -m)-linux")
  curl -fsSL "$ZURL" -o "$TC/downloads/zig.tar.xz"
  echo "$ZSUM  $TC/downloads/zig.tar.xz" | sha256sum -c -
  mkdir -p "$TC/zig"
  tar -xJf "$TC/downloads/zig.tar.xz" --strip-components=1 -C "$TC/zig"
fi
"$TC/zig/zig" version

# ---- Playwright browsers (JS lib is installed by the caller in tests/e2e) ----
if [ ! -d "$TC/pw-browsers" ] || [ -z "$(ls -A "$TC/pw-browsers" 2>/dev/null)" ]; then
  echo "[playwright] installing chromium headless shell"
  (cd "$PROJ/tests/e2e" && npm i playwright@1.64.1 >/dev/null 2>&1)
  (cd "$PROJ/tests/e2e" && npx playwright install --with-deps --only-shell chromium >/dev/null 2>&1)
fi
ls "$TC/pw-browsers" | head -3

# ---- playwright JS library (node_modules does not persist) ----
(cd "$PROJ/tests/e2e" && npm i playwright@1.64.1 >/dev/null 2>&1)

echo "toolchains ready under $TC"
