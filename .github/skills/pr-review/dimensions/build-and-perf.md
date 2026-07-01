# Build & performance review

You are reviewing a diff for this Windows Terminal / OpenConsole fork for
**build integrity and performance**. Apply the shared output contract in
`_shared-contract.md`. Set `Domain: build-and-perf` on every finding. (This
dimension replaces a generic "packaging" one — the equivalent here is the MSBuild
project graph and the render hot path.)

You do **not** run a build. Reason about the project files and the code.

## Build integrity — the big one for this repo

- **`.vcxproj` ↔ `sources.inc` lockstep.** New/renamed/removed `.cpp`/`.h` files
  must be reflected in BOTH the MSBuild `.vcxproj` (for the VS/msbuild build) and
  the corresponding `sources.inc` (for the OpenConsole/`nmake`-style build). A
  file added to one but not the other is a **broken build** on the other track —
  this is the single most common breakage here. Flag any new source file that
  isn't wired into both.
- **New `oss/` vendored code:** must appear in the consuming project(s) and have
  a `cgmanifest.json` entry (component governance). A vendored source compiled
  but not registered is a compliance/build finding.
- **Warnings-as-errors.** Product code builds `/W4` with specific warnings
  promoted to errors (`/we…`); some third-party/vendored code carries scoped
  `#pragma warning(push/disable/pop)` or per-file `AdditionalOptions`. New code
  that would trip `/W4` (unused param/var, signed/unsigned compare, truncation,
  `4267`/`4244`, uninitialized) will **fail CI**, not warn. Flag likely `/W4`
  violations in new code, and flag a blanket warning-disable that hides real
  issues.
- **Headers / includes / precompiled.** Missing include that only works
  transitively; a new public header not added where consumers expect it; PCH
  assumptions.
- **Config/platform.** Debug-only or x64-only code paths; something that won't
  compile for ARM64 or in Release; a test-only symbol (`UNIT_TESTING`,
  `friend AdapterTest`) leaking into product config.

## Performance — the render/parse hot path

- **Per-cell / per-pixel / per-frame cost.** Code on the draw path
  (`ImageSlice`, `BackendD3D`/`BackendD2D`/GDI `_drawBitmap`, reflow, the
  `AdaptDispatch` output loop) runs at very high frequency. An allocation, a
  `std::map` lookup, a `std::function`, a copy, or an O(cells) scan added *per
  cell or per frame* is a real regression. Prefer the amortized/precomputed path.
- **Decode/inflate cost.** Two-pass decode is fine (and safer) if the sizing pass
  is O(input); flag an O(output) or repeated-work pass. Flag re-decoding an image
  that could be cached in the registry.
- **Unbounded growth = perf too.** Registries/vectors that grow with attacker
  input degrade steady-state performance even before they exhaust memory
  (security owns the exhaustion angle; you own the "this gets slow" angle).
- **Redundant redraws / recompose.** Recomposing the whole buffer/row set when a
  bounded region changed; invalidating more than necessary.

## What to drop

- Micro-optimizations off the hot path with no measurable impact.
- "Use `reserve()`" on a small, cold, bounded vector.
- Speculative perf worries with no frequency argument.

## Severity guide for this dimension

- New source file missing from `.vcxproj` or `sources.inc` (breaks a build
  track) → high.
- Likely `/W4`-as-error violation in new product code → high.
- Vendored code missing `cgmanifest.json` registration → medium.
- Per-cell/per-frame allocation or O(cells) work added to the draw path → medium
  (high if clearly per-cell on large images).
- Re-decode/recompose that a cache/bounded-region update would avoid → low/medium.
