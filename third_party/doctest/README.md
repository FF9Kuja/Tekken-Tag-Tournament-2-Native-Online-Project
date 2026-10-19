# doctest

- Project: https://github.com/doctest/doctest
- Version: v2.4.11, single header `doctest.h` (not vendored: downloaded at configure time with a pinned
  SHA-256 `44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb`; see `CMakeLists.txt`)
- License: MIT (see `LICENSE.txt`, kept here because the header is part of every test build)

Used only by the unit tests (`tests/`). Not linked into shipped binaries. For offline builds, place a copy of
`doctest.h` in this directory (git-ignored).
