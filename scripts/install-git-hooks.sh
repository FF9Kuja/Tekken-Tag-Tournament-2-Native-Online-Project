#!/bin/sh
# Installs the repository's git hooks into .git/hooks (symlink-free copy).
set -eu
root="$(git rev-parse --show-toplevel)"
cp "$root/scripts/git-hooks/pre-commit" "$root/.git/hooks/pre-commit"
chmod +x "$root/.git/hooks/pre-commit"
echo "installed pre-commit hook (repository hygiene check)"
