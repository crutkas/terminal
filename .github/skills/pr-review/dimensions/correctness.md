# Correctness & edge-case review

You are reviewing a diff for this Windows Terminal / OpenConsole fork for
**functional correctness and missed edge cases**. Apply the shared output
contract in `_shared-contract.md`. Set `Domain: correctness` on every finding.

This is modern C++ (C++20/23, `til::` utilities, WIL). Focus on logic, state,
and edge cases — not memory-safety exploitability (that's the security
dimension) and not style (the compiler/formatter cover it).

## What to look for

- **VT / escape-sequence state.** Parser state that isn't reset on abort (CAN,
  a new command mid-transfer, RIS/hard reset). Chunked/continued sequences
  (`m=1`) reassembled in the wrong order, or a control field taken from the
  wrong chunk. A key parsed but never validated.
- **Geometry & rounding.** Pixel→cell and cell→pixel conversions; `c=/r=`
  scaling and aspect preservation; crop (`x/y/w/h`) composed with scale; clamp
  to the page vs wrap; inclusive/exclusive edges. Off-by-one at the 1-row /
  1-column boundary. Cursor advancement after a placement (and the "don't move"
  cases).
- **Buffer / ImageSlice logic.** Column ownership (by image id vs placement),
  slice replacement when the cell size differs, erase-by-owner vs erase-by-rect,
  co-resident content (Sixel id 0) surviving a targeted delete, reflow/scroll
  moving or invalidating a slice.
- **Registry / lifetime / cascade.** Delete and re-transmit semantics; group
  lifetime (deleting a parent deletes relative children; an image with no
  placements is deleted); cycle/depth/missing-parent handling; LRU eviction
  leaving dangling references; re-put replacing (moving) vs appending.
- **Numeric parsing.** Signed vs unsigned key parsing (`H/V` are signed);
  clamping at `INT32_MIN`/`INT32_MAX`/`UINT32_MAX`; empty/garbage → default;
  overflow at the `0x80000000` negation boundary.
- **Concurrency.** Per-pane/per-connection threads touching shared state; a
  `DispatchQueue`/render-thread interaction; a static built lazily.
- **Contract mismatches.** A function whose return/`out` param the new caller
  ignores or misuses; a default parameter that changes behavior for existing
  callers; an assumption that a container is non-empty.
- **Error paths.** Does every failure leave state consistent (output cleared,
  registry unchanged, no half-drawn image)? Does an early `return` skip needed
  cleanup? Is the reported error **code** the right one for the condition?

## Method

Trace the new/changed code paths with a couple of concrete inputs, including a
boundary one (0, 1, max, empty, off-screen, exactly-at-cap). If you flag an edge
case, state the specific input that triggers it — that doubles as a test the
test-coverage dimension can cite.

## What to drop

- "Consider handling X" where X can't occur given the surrounding validation.
- Restating the happy path.
- Anything a passing unit test in the same diff already proves.

## Severity guide for this dimension

- Wrong result / crash on a reachable ordinary input → high.
- Wrong result only on a rare boundary input → medium.
- Inconsistent state after an error path (recoverable) → medium.
- A latent contract mismatch not yet reachable → low.
