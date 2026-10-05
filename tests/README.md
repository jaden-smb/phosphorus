# `tests/` — the architecture's safety net

Phosphorus has no GoogleTest and no test framework dependency. Tests use the ~90-line in-house
harness in [`phx_test.h`](phx_test.h), in the same spirit as the engine: no allocation surprises,
no magic, readable in one sitting.

Everything here runs **headlessly** on the `null` platform (virtual clock, software framebuffer,
scripted input — including the desktop extension's scripted key/mouse/text queue that the
`editors` suite drives Phosphorus Studio's widget kit with), so the whole suite is deterministic and
needs no display.

## Layout

| Folder | What lives there | How it builds |
|---|---|---|
| **`unit/`** | `test_*.cpp` — pure module tests (fixed-point, allocators, ECS, codecs, …), plus `main.cpp`, the runner. | **All linked into one binary**, `build/phx_tests`. Cases self-register at static-init; the runner executes every `PHX_TEST` in the binary. |
| **`suites/`** | `*_test.cpp` — headless *integration* binaries that boot the real engine and drive a real scenario (drop a body on a tile floor, play the platformer, bake a bundle and mount it back). | **One binary per file, each with its own `main()`** — one per `make` suite target. |
| **`verify/`** | `*_verify.cpp` — real-device verification: open an SDL window / read back from a GPU / open an audio device, and diff against the software golden. | Needs SDL2 (+ libGL) and a display; not part of `make check`. |
| **`fixtures/`** | Shared test data (`png_fixtures.h` — real PNG byte blobs in source form). | Header-only; `-Itests` puts it on the include path everywhere. |

The two naming conventions are load-bearing, not historical accident:
**`test_foo.cpp` = a unit file with no `main()`** (it joins the shared runner);
**`foo_test.cpp` = a standalone suite with its own `main()`** (a suite may still use `PHX_TEST`
cases with its own runner, as `suites/editors_test.cpp` does). Put a file in the wrong folder and
you either get a duplicate-`main` link error or a test that never runs.

## Running

```bash
make check                # THE gate: unit + every suite + the asset pipeline + the dependency law
make test                 # just the unit binary (build/phx_tests)
make test TIER=gba_sim    # the same unit binary on the GBA fixed-point tier (scalar = fixed16)
make physics              # any single suite: its make target IS its name
```

There is **no per-test-case filter** — the runner executes every registered case in the binary. To
narrow scope, run one suite target.

Expected: `PASS <N> checks across <M> cases`, a `<SUITE> PASS` line per suite, `depcheck: OK`.

Release gates beyond `make check`:

```bash
make determinism   # the suites + a rendered frame, byte-identical across BOTH scalar tiers
make sanitize      # the full check suite under ASan + UBSan
make size-gate     # the GBA ROM / IWRAM / EWRAM budgets
```

`ctest` mirrors `make check` from the CMake tree (`cmake -S . -B build/linux -DPHX_TARGET=linux`),
including the GBA-PPU and PSP-GU host-model checks, which are compiled there as standalone
executables because swapping the render backend is incompatible with CMake's
one-backend-per-build configure.

## The harness

```cpp
#include "phx_test.h"

PHX_TEST(fixed_mul_is_exact) {      // self-registers; the name is what prints on failure
    CHECK(x > 0);                   // a bare condition
    CHECK_EQ(a, b);                 // prints both integer values on failure
    CHECK_NEAR(a, b, 1e-4);         // tolerance compare — for the float tier only
}
```

A failure prints `FAIL <file>:<line>` with the offending expression and the values, then keeps
going (tests do not abort on first failure); the process exits non-zero if any check failed.

## Adding a test

**A unit case** — drop `unit/test_<mod>.cpp`, write `PHX_TEST`s, then add the path to `TEST_SRC` in
the [`Makefile`](../Makefile) and to the `phx_tests` source list in [`CMakeLists.txt`](CMakeLists.txt).
No registration boilerplate beyond the macro.

**An integration suite** — drop `suites/<name>_test.cpp` with its own `main()` that boots an `App`,
runs a bounded number of frames, asserts on the framebuffer / ECS / mixer output, prints
`<NAME> PASS`, and returns non-zero on failure. Then: add a `<name>` target in the `Makefile`
(copy the shape of an existing one), append it to the `check:` target's dependency list, and add
`<name>:<name>_test` to `_itests` in `CMakeLists.txt`.

## Conventions that matter

- **Both scalar tiers must pass.** Anything numeric has to hold with `scalar = float` *and*
  `scalar = fixed16` (`make test TIER=gba_sim`), and `make determinism` proves the rendered frame
  is byte-identical between them. Use the Q16 integer idiom for tier-exact math, not floats.
- **Bound-check test fixtures that carve from static pools** (`std::abort()` on overflow). ASan
  caught real silent overflows from unchecked ones.
- **The null platform's virtual clock** advances one sim step per `pump_events()` (i.e. per frame),
  plus 1 µs per `clock_ns()` read — so frame pacing does not depend on how often the loop reads the
  clock. Tests that assert exact fixed-step counts rely on this.
- **Zero warnings** under `-Wall -Wextra -Wpedantic`, including in test code.
