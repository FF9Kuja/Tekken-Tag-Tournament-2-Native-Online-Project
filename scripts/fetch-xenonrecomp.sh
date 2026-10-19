#!/bin/sh
# Clones XenonRecomp at the pinned commit (third_party/xenonrecomp/PIN) with only the submodules
# the ttt2-xexpatch tool needs.
#   scripts/fetch-xenonrecomp.sh [destination]
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
pin="$root/third_party/xenonrecomp/PIN"
url="$(sed -n 's/^url=//p' "$pin")"
commit="$(sed -n 's/^commit=//p' "$pin")"
submodules="$(sed -n 's/^submodules=//p' "$pin")"
dest="${1:-$root/third_party/xenonrecomp/src}"
if [ -d "$dest/.git" ]; then
    current="$(git -C "$dest" rev-parse HEAD)"
    [ "$current" = "$commit" ] || { echo "$dest is at $current, expected $commit"; exit 1; }
    echo "xenonrecomp already at $commit"
else
    git clone --no-checkout "$url" "$dest"
    git -C "$dest" checkout --quiet "$commit"
fi
for sm in $submodules; do
    git -C "$dest" submodule update --init --depth 1 "$sm"
done
echo "xenonrecomp ready at $dest ($commit)"
