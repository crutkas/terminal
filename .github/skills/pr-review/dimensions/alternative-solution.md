# Alternative-solution check

You are reviewing a diff for this Windows Terminal / OpenConsole fork and asking:
**is this the right way to solve the problem, or is there a simpler / more
idiomatic one the repo already supports?** Apply the shared output contract in
`_shared-contract.md`. Set `Domain: alternative-solution` on every finding.

You are not looking for bugs (other dimensions do that). You are looking for
solutions that reinvent something, fight the grain of the codebase, or add
complexity a existing primitive would remove.

## What to look for

- **Reinventing an existing primitive.** The repo has rich utilities — `til::`
  (point/size/rect/rle/small_vector/enumset/…), WIL (`wil::`), `ImageSlice`
  helpers (`EraseBlock`, `EraseByOwner`, `ClearForeignColumns`, column owners),
  the VT parser/`StateMachine`, existing `AdaptDispatch` helpers, `Viewport`,
  `TextBuffer` APIs. Hand-rolled geometry, manual bounds loops, a bespoke
  container, or a manual buffer scan where a helper exists is a finding.
- **Vendoring vs building.** A new `oss/` dependency where a smaller vendored
  file or an existing in-tree capability would do (or vice-versa: hand-rolling
  something a tiny permissive lib does more safely). Was the smallest, most
  auditable option chosen for an untrusted-input path?
- **Duplication.** New code that duplicates logic already present (a second
  cascade loop, a second base64/parse routine, a second clamp). Recommend
  extracting/reusing.
- **Over-engineering for the scope.** A general framework where the PR needs one
  case; premature abstraction. Conversely, a one-off that clearly should reuse a
  shared path.
- **Wrong layer.** Logic placed in the adapter that belongs in the buffer or
  renderer (or vice versa); a concern solved per-call that the surrounding
  object already tracks.
- **A materially simpler design.** If there is a concretely simpler approach
  (fewer moving parts, fewer states, one pass instead of two) that meets the
  same requirements, name it — with the specific trade-off, not a vague "could
  be cleaner."

## Method

For each new function/type, ask: does something in `til::`, `wil::`,
`ImageSlice`, `TextBuffer`, or the existing kitty/sixel helpers already do this?
Grep the surrounding files for a similar routine before claiming novelty. Only
emit a finding if you can name the concrete alternative.

## What to drop

- Vague "this could be cleaner" without a named alternative.
- Style/structure preferences with no functional or maintenance payoff.
- Micro-optimizations (that's build-and-perf, and only if measurable).

## Severity guide for this dimension

- Reinvents a primitive in a way that also risks bugs (e.g. manual bounds vs a
  checked helper) → medium.
- Meaningful duplication that will drift out of sync → medium.
- A concretely simpler equivalent design → low/medium (medium if it also
  reduces risk on an untrusted path).
- Pure "nicer structure" → drop unless it removes real complexity.
