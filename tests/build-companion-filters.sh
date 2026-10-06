#!/bin/sh
set -eu

target_dir=$1
fixture_dir=$2
mkdir -p "$target_dir/companions" "$fixture_dir"

# Full revisions and each companion's Cargo.lock keep local and CI builds aligned.
while read -r name revision; do
    source_dir="$target_dir/companions/$name-$revision"
    if ! test -f "$source_dir/.extracted"; then
        archive="$target_dir/companions/$name-$revision.tar.gz"
        curl -fsSL --retry 3 -o "$archive" \
            "https://codeload.github.com/RamazanKara/$name/tar.gz/$revision"
        tar -xzf "$archive" -C "$target_dir/companions"
        touch "$source_dir/.extracted"
        rm -f "$archive"
    fi

    CARGO_TARGET_DIR="$target_dir" "${CARGO:-cargo}" build \
        --manifest-path "$source_dir/Cargo.toml" --locked \
        --release --target wasm32-unknown-unknown -j 2
    module=$(printf '%s' "$name" | tr '-' '_')
    cp "$target_dir/wasm32-unknown-unknown/release/$module.wasm" "$fixture_dir/"
done <<'EOF'
proxy-wasm-jwt-validator c292011e0145182f8516cefb0818d05905261cd0
proxy-wasm-signature-verifier 7dc9aedc99d7032ba510b78e4713c8d799126d6f
proxy-wasm-cache-key-normalizer f7d67a865f0fe3b750d2788685015a9beae7564e
EOF
