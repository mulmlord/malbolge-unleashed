# Classic conformance corpus

Known Malbolge programs and negative cases, run in classic mode (`--classic`, spec section 8)
through `ClassicVm` by `corpus_test` (label `fast`) and `corpus_slow_test` (label `slow`).
Provenance and licences: `SOURCES.md`.

## Format

`manifest.txt` (fast) and `manifest_slow.txt` (slow, beer99 with 13.8 million steps) hold one
program per line:

    name program stdin step_cap outcome steps out_len out_sha256

`program` and `stdin` are paths relative to this directory (`-`: no stdin). `step_cap` is the total
step budget (`-`: none). `outcome` is one of:

- `halt`: halts (exit 0)
- `limit`: still running when the step cap is reached (non-halting programs; the output up to
  that step is deterministic, so its length and SHA-256 are checked)
- `E_LOAD@<offset>`: rejected by the loader at that byte offset (0-based, whitespace counts)
- `E_EXEC@<step>` / `E_ENCRYPT@<step>`: runtime fault at that step

`steps` is the exact step count of a `halt` entry (`-` otherwise); the test runs it with a cap of
`steps + 1`, so a regression cannot hang CI. `out_len` and `out_sha256` describe the bytes written (up to the halt, the fault or the cap). The
SHA-256 test vectors of the test's own implementation are checked as well.

## How the expected values were made

Each value was produced by Olmstead's reference interpreter and cross-checked against an
executable model of docs/spec.md section 8 (both agree on output, outcome and step count):

- halting programs: the unmodified `malbolge.c` (public domain) with empty or the given stdin;
- step-capped programs: the instrumented reference of `../reference/` (it can stop after N steps);
- `E_EXEC` / `E_ENCRYPT`: the reference hangs or reads out of bounds there, so the expected
  outcome is spec 8's defined error. The instrumented reference reports the same event at the same
  step (`../reference/` tests this on random programs);
- `E_LOAD`: the model and `load_text`; the reference loader rejects `cat_typo.mal` and
  `malbolge20_hello20.mb` too ("invalid character", "input file too long"); it accepts the
  byte of `loader_bug.mal` (and then hangs) (the loader bug spec 8 does not reproduce).

## Programs

| Name | Program | Outcome |
|---|---|---|
| `hello_esolangs` | esolangs Hello world | halts after 45 steps, `Hello, world.` |
| `hello_stringout` | stringout.c output | halts, `Hello World!\n` |
| `text_stringout` | stringout.c output | halts, `Malbolge: 0123456789 [ok]\n` |
| `nop` | Olmstead's `nop.mal` | halts after 2 steps, no output |
| `cat_input` | non-halting cat, stdin `cat.in` | echoes the input, then byte 0xA8 (EOF = 59048, written mod 256) until the cap |
| `cat_empty` | same, empty stdin | 0xA8 until the cap |
| `truth_0` / `truth_1` | truth-machine | `0` halts; `1` prints `1` forever (cap 20000) |
| `beer99` (slow) | Iizawa's 99 bottles | halts after 13802606 steps, 11459 bytes (same output file as `../programs/beer99.expected`) |
| `encrypt_fault` | own work | one byte, then `E_ENCRYPT` at step 3 (an `answer.mal` style program: no result) |
| `exec_fault` | own work | one byte, then `E_EXEC` at step 4 |
| `cat_typo` | cat with the MalbolgeLISP README's typo | `E_LOAD` at offset 65 |
| `loader_bug` | hello with byte 10 replaced by 0xFF | `E_LOAD` at offset 10 |
| `malbolge20_hello20` | Malbolge20's `hello20.mb` | `E_LOAD` at offset 59049 (more than 59049 cells) |

## Not included

The licence is unclear or restrictive, so the files are neither committed nor downloaded by the
tests (no network in tests).

| Program | Source | Reason |
|---|---|---|
| `answer.mal` (mtve) | graue/esofiles `malbolge/src/answer.mal` | No licence. The esofiles README says that many programs "have no associated copyright/license information" and that only the collection is CC0, "not all the individual programs". `encrypt_fault.mal` covers the same case (`E_ENCRYPT`, no `42`). |
| `copy.mal` (Lou Scheffer) | graue/esofiles `malbolge/src/copy.mal` | No licence, same reason. `loader_bug.mal` covers the same case (loader bug, `E_LOAD`). |
| `beer.mal` (straight-line, uuencoded gzip of the lyrics) | graue/esofiles `malbolge/src/beer.mal` | No author or licence given. |
| Cooke's `hello-world.mb` | Andrew Cooke, http://www.acooke.org/malbolge.html (archive.org) | No licence on the page. |
| Lutter's `cat.mal`, `quine.mal`, `hello-world.mu`, `cat.mu`, `brainfuck.mu` | https://malbolge.org/, https://lutter.cc/unshackled/ | No licence or copyright statement on any of the pages (checked the cat, quine, Unshackled hello-world, cat and brainfuck pages and the site index). The `.mu` files are Unshackled programs. |
| Ernst's sample on his page | http://matthias-ernst.eu/malbolge.html (archive.org) | The page has no licence. The CC0 generator `stringout.c` is used to generate equivalent programs instead (`stringout_*.mal`). |
| MalbolgeLISP README hello and cat | https://github.com/iczelia/malbolge-lisp | Mixed licence signals: the repository `LICENSE` is CC0, but the README says the program is governed by the GNU GPLv3 (only the core is public domain). Not committed; the cat typo it demonstrates is reproduced by `cat_typo.mal`, derived from the CC0 esolangs cat. |
| Normalized cat, Unshackled-only programs, MalbolgeLISP, blc-mb | research section 6 | Not classic programs, or tens to hundreds of MB. |
