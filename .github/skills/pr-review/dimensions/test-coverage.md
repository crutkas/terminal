# Test coverage review

You are reviewing a diff for this Windows Terminal / OpenConsole fork and asking:
**are the changes adequately and durably covered by tests?** Apply the shared
output contract in `_shared-contract.md`. Set `Domain: test-coverage` on every
finding.

## Test surfaces in this repo

- **Adapter unit tests:** `src/terminal/adapter/ut_adapter/adapterTest.cpp`
  (TAEF: `TEST_METHOD`, `VERIFY_*`). Most VT / kitty / sixel behavior is tested
  here, driving `_stateMachine->ProcessString(L"…")` and asserting on the
  `TextBuffer` / `ImageSlice` / the mock `TestGetSet` responses. `AdapterTest`
  is a `friend` of `AdaptDispatch` (under `UNIT_TESTING`), so private helpers and
  state can be asserted directly.
- **Parser tests:** `src/terminal/parser/ut_parser/`.
- **Other `ut_*` / `*.UnitTests*` projects** across buffer, renderer, types.
- Tests build with the same `/W4` + warnings-as-errors as product code.

## What to look for

- **New behavior, no test.** A new key, action, error code, geometry rule, or
  helper with no `TEST_METHOD` exercising it — at least one happy path and one
  rejection/edge. Static/pure helpers are trivially testable; flag if untested.
- **Edge cases from other dimensions untested.** If correctness/security flagged
  a boundary (0/1/max, empty, off-screen, at-cap, overflow, cycle, missing
  parent), is there a test that would catch a regression? If not, that's a
  coverage finding with the exact input to use.
- **Visual / rendering change with no pixel assertion.** A change that alters
  what is drawn should assert on actual cells/pixels (`SliceContainsColor`,
  `SlicePixelAt`, `ColumnOwner`, `CountImageRows`, `FindFirstImageSlice`), not
  just that a slice exists. For features where "looks right" matters, the repo's
  bar is an objective check — e.g. a direct-vs-alternate **pixel-equivalence**
  test and/or a committed real-app **screenshot** under `doc/`. Flag a visual
  feature that ships with neither.
- **The "why this test exists" convention.** This repo's tests document the
  contract/regression/edge each guards (e.g. `// Regression (#17951): …`, `// the
  old default 1x1 grid drew the whole image`). A new `TEST_METHOD` with **no
  comment**, or a bare mechanical "what it does" restatement, is a finding —
  recommend a one-line rationale.
- **Flip-the-negative convention.** When a feature lands, a prior "deferred →
  EINVAL/rejected" negative test for it should be **flipped** to a positive
  test. Flag a newly-supported feature whose old rejection test still asserts
  rejection.
- **Brittleness.** Tests that hardcode coordinates that depend on the mock cell
  size instead of reading it, rely on machine state, or depend on ordering that
  isn't guaranteed. Prefer self-calibrating assertions (derive the cell size,
  read the actual cursor) over magic numbers.
- **Assertion strength.** A test named for a behavior that only checks a weaker
  proxy (e.g. "cascade deletes" that only checks map size, not that the image /
  pixels are actually gone).

## What to drop

- "Increase coverage to 100%" without a specific uncovered scenario.
- Tests for trivial getters or auto-generated code.
- Asking for a screenshot on a non-visual change.

## Severity guide for this dimension

- New user-visible behavior/error code with zero tests → high.
- A flagged security/correctness edge with no regression test → high.
- Visual/rendering feature with no pixel assertion or screenshot → medium.
- New `TEST_METHOD` with no "why" rationale → low.
- Under-asserting test (passes without proving the claim) → medium.
- Un-flipped stale negative test for a now-supported feature → medium.
