#!/bin/sh
# Clones the ReXGlue SDK at the pinned commit (third_party/rexglue-sdk/PIN) into
# third_party/rexglue-sdk/src with its submodules (shallow), and applies the
# project's patches from third_party/rexglue-patches/*.patch if any exist.
#   scripts/fetch-rexglue-sdk.sh [destination]
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
pin="$root/third_party/rexglue-sdk/PIN"
url="$(sed -n 's/^url=//p' "$pin")"
commit="$(sed -n 's/^commit=//p' "$pin")"
dest="${1:-$root/third_party/rexglue-sdk/src}"
if [ -d "$dest/.git" ]; then
    current="$(git -C "$dest" rev-parse HEAD)"
    if [ "$current" = "$commit" ]; then
        echo "rexglue-sdk already at $commit"
    else
        echo "rexglue-sdk is at $current, expected $commit; refusing to touch it (remove $dest to refetch)"
        exit 1
    fi
else
    git clone --no-checkout "$url" "$dest"
    git -C "$dest" checkout --quiet "$commit"
fi
git -C "$dest" submodule update --init --recursive --depth 1 --jobs 4
patches="$root/third_party/rexglue-patches"
if [ -d "$patches" ]; then
    for p in "$patches"/*.patch; do
        [ -e "$p" ] || continue
        if git -C "$dest" apply --check "$p" 2>/dev/null; then
            git -C "$dest" apply "$p"
            echo "applied $(basename "$p")"
        elif git -C "$dest" apply --reverse --check "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            echo "patch does not apply: $p"
            exit 1
        fi
    done
fi
echo "rexglue-sdk ready at $dest ($commit)"
