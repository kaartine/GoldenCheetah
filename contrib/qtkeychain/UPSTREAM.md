# QtKeychain upstream

- Project: https://github.com/frankosterfeld/qtkeychain
- Version: 0.17.0
- Source: https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.17.0.tar.gz
- Source SHA-256: `3b85c3929034b0a99da777130c34d99f006fcd3a9d56564159399a33fee0e504`
- License: Modified BSD (`COPYING`)

The upstream archive is vendored with two compatibility patches and one
whitespace-only repository cleanup:

- `qtkeychain/libsecret.cpp` temporarily hides Qt's `signals` macro while
  parsing GLib headers so projects using a Qt precompiled header can build.
- `qtkeychain/libsecret.cpp` accepts GoldenCheetah's validated bundled
  libsecret path so AppImages do not fall back to a host-only runtime.
- Trailing whitespace and the extra final blank line in `ChangeLog` are
