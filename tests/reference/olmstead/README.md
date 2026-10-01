# Olmstead's reference interpreter

`malbolge.c` is Ben Olmstead's original 1998 Malbolge interpreter, vendored **byte-identical**.
Never edit it: the differential test builds an instrumented copy at build time from this pristine
file (see `../instrument.cmake`).

- Upstream URL: https://github.com/graue/esofiles/blob/master/malbolge/impl/malbolge.c
  (raw: https://raw.githubusercontent.com/graue/esofiles/master/malbolge/impl/malbolge.c; last
  commit touching the file in that repo: `03eb1ca764e28e9e1f8f2fdce2f96523728f8675`).
- Original author's mirror: https://www.lscheffer.com/malbolge_interp.html. The program text is
  the same (both copies include `<malloc.h>`).
- Size: 4739 bytes.
- SHA-256: `ca3b4f321bc3273195eb29eee7ee2002031b057c2bf0c8d7a4f7b6e5b3f648c0`
- Licence: public domain. The file header says: "This interpreter isn't even Copylefted; I
  hereby place it in the public domain."

Check: `sha256sum tests/reference/olmstead/malbolge.c`. `instrument.cmake` also aborts the build if
the file no longer has the expected hash.
