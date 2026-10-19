# Third-party notices

## Project code and vendored dd

The original price-app code is distributed under the [MIT license](LICENSE),
copyright 2019-2026 Zac Walker.

The C11 Binance implementation in [src/binance](src/binance) and its synthetic
regression fixtures were developed in libcfindata by the same copyright holder
and remain MIT licensed as application-owned code.

The vendored `dd.ps1` and `.dd/` files come from
[ZacWalk/dd v0.2.2](https://github.com/ZacWalk/dd/tree/114890d721907dee64651371947f49dd33dad27c).
Their exact revision and SHA-256 fingerprints are recorded in
[docs/dd-upstream.json](docs/dd-upstream.json).

That upstream revision does not include a license file. Zac Walker, as the
copyright holder, expressly grants the MIT license in this repository to these
vendored dd files as well. This grant covers the included copies; it does not
claim to change the licensing of other upstream revisions. The vendored bytes
remain unchanged.

## Pinned source libraries

CMake fetches these at the commits recorded in
[cmake/dd-dependencies.json](cmake/dd-dependencies.json) when the project is
configured. Their sources are not vendored in this repository.

| Library | Licensing |
|---|---|
| [libcmlp](https://github.com/ZacWalk/libcmlp) | MIT |
| [libcfindata](https://github.com/ZacWalk/libcfindata) | MIT; its [notices](https://github.com/ZacWalk/libcfindata/blob/main/THIRD_PARTY_NOTICES.md) cover its own dependencies |
| [spike-db](https://github.com/ZacWalk/spike-db) | MIT; storage benchmark only |
| [platform-h](https://github.com/ZacWalk/platform-h) | MIT; Windows viewer only |

## External dependencies

External packages are not relicensed under price-app's MIT license. CMake
finds them through vcpkg on Windows or system packages on Linux; their sources
and installed package trees are not vendored in this repository.

| Dependency | Licensing |
|---|---|
| [libarchive](https://github.com/libarchive/libarchive/blob/master/COPYING) | BSD-style terms, including two-clause and three-clause notices and file-specific terms |
| [OpenSSL](https://www.openssl.org/source/license.html) | OpenSSL 3.x uses Apache-2.0; retain the notices for the actual version supplied by the build |
| [libcurl](https://curl.se/docs/copyright.html) | curl's permissive copyright and permission notice; used by the Linux transport |
| [zlib](https://zlib.net/zlib_license.html) | zlib license; a dependency of the pinned Windows libarchive build |

Dependencies can have additional transitive dependencies depending on the
platform and selected package features. When distributing binaries, include
the license texts and notices required by every bundled dependency, not just
this summary. This is especially relevant to the Windows CLI's statically
linked dependencies. vcpkg supplies package notices under
`vcpkg_installed/<triplet>/share/<package>/copyright`; system packages provide
their own copyright files. Windows system libraries remain subject to the
applicable Microsoft terms.

## Market data

The software license does not grant rights to market data, provider accounts,
provider trademarks or redistribution of downloaded records. Users must comply
with each provider's terms, including Binance's for archives fetched by
`cli download`. The tracked test fixtures and the README screenshot use
synthetic prices, not a licensed market dataset.
