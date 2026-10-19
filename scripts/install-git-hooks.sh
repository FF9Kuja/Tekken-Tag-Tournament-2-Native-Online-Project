#!/bin/sh
# Installs the repository's git hooks into .git/hooks (symlink-free copy).
set -eu
root="$(git rev-parse --show-toplevel)"
hooks="$(git rev-parse --git-path hooks)"
mkdir -p "$hooks"
cp "$root/scripts/git-hooks/pre-commit" "$hooks/pre-commit"
chmod +x "$hooks/pre-commit"
echo "installed pre-commit hook (repository hygiene check)"
