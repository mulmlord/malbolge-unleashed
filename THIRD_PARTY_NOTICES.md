# Third-party notices

malbolge-unleashed itself is licensed under the MIT License (see `LICENSE`). It uses or ships the following
third-party material.

## Linked into every `malm` binary and every packed executable

| Component | Version | Licence | Notes |
|---|---|---|---|
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause (dual-licensed BSD / GPLv2; used under BSD) | `.malm` compression, statically linked. Full text: [`third_party/LICENSE-zstd.txt`](third_party/LICENSE-zstd.txt) |

**If you distribute `malm` binaries or executables built with `malm pack`**, the BSD licence requires you to
reproduce Zstandard's copyright notice, conditions and disclaimer in the documentation or other materials provided
with the distribution, e.g. by shipping `third_party/LICENSE-zstd.txt` alongside them.

## Used only for building and testing (not part of any binary we ship)

| Component | Licence | Location |
|---|---|---|
| [doctest](https://github.com/doctest/doctest) 2.5.3 | MIT | fetched at configure time (`cmake/MalmTests.cmake`) |
| [riscv-tests](https://github.com/riscv-software-src/riscv-tests) (commit 7b68f7b) | BSD-3-Clause | fetched at configure time (`tests/rv/CMakeLists.txt`) |
| Ben Olmstead's reference interpreter `malbolge.c` | public domain (stated in its header) | `tests/reference/olmstead/` |
| Malbolge20 sample `hello20.mb` (Nagoya Univ.) | MIT | `tests/corpus/`, licence text in `tests/corpus/LICENSE-malbolge20.txt` |
| Malbolge programs from the esolangs.org wiki | CC0 (Esolang:Copyrights) | `tests/programs/`, `tests/corpus/` |
| Output of Matthias Ernst's `stringout.c` | CC0 (the tool's dedication) | `tests/corpus/` |

Per-file provenance is recorded in `tests/programs/SOURCES.txt`, `tests/corpus/SOURCES.md` and
`tests/reference/olmstead/README.md`.
