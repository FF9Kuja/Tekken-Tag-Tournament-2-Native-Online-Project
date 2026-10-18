# Contributing

1. Read `docs/LEGAL.md` first: no game content, generated game code or secrets may be committed. Install the
   pre-commit hook: `scripts/install-git-hooks.sh`.
2. Build and test locally (`docs/DEVELOPMENT.md`); keep Clang, GCC and MSVC warning-clean.
3. Work on a branch; open a pull request against `main`. `main` is protected: CI must pass.
4. Add or update tests with every change to a parser, hash or format tool. Synthetic inputs only.
5. Record decisions that change the architecture in `docs/decisions/` and keep `docs/STATUS.md` honest about what
   is verified.
6. New dependencies need an entry in `docs/DEPENDENCIES.md` with a license check.
