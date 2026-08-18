# Box2D 3.1.1

This directory contains the production C sources and public headers from the
Box2D `v3.1.1` release:

- Upstream: <https://github.com/erincatto/box2d>
- Commit: `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3`
- License: MIT, reproduced in `LICENSE`
- License SHA-256:
  `68a3e676d7e94093b102d5cba0d4e04af812040d6f230c3db67a6664574e43d2`

Samples, benchmarks, upstream unit tests, CMake files and Git metadata are not
vendored. In particular, upstream test configuration uses CMake FetchContent;
GoldenCheetah production and unit-test builds must remain offline and use only
the pinned sources in this directory.
