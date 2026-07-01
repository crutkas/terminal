# Docs & samples review

You are reviewing a diff for this Windows Terminal / OpenConsole fork and asking:
**is the change adequately documented and demonstrated, and is existing
documentation kept truthful?** Apply the shared output contract in
`_shared-contract.md`. Set `Domain: docs-and-samples` on every finding.

This is a low-severity dimension by nature. Stay focused; do not inflate.

## What to look for

- **Spec-link comments.** New protocol handling (an APC/DCS/OSC handler, a new
  kitty/sixel key, an error code, a geometry rule) should carry a short comment
  citing the governing spec section/URL — this repo cites `vt100.net`, xterm
  ctlseqs, and the kitty graphics-protocol URL next to non-obvious logic. Missing
  citation on new non-trivial protocol code is a finding (also raised by
  spec-conformance; report it once, under whichever fits).
- **Stale comments/docs.** A comment or doc that the diff makes untrue — e.g. a
  header comment still describing the old algorithm after a redesign, a
  "limitation: X not supported" note for something now supported, a doc listing
  the wrong default. Stale docs are worse than missing docs; prioritize these.
- **Visual-feature screenshots.** Visual features in this repo land with a
  committed screenshot under `doc/kitty-graphics/` or `doc/sixel/` **and** that
  image embedded in the PR body. Flag a visual change with no screenshot
  artifact. (Whether it's embedded in the PR body is checked by the orchestrator,
  not you — you check the committed file.)
- **Vendored-code docs.** A new `oss/<lib>/` drop should include the pieces this
  repo expects: `cgmanifest.json` (pinned name + version + commit), `LICENSE`,
  and a short `MAINTAINER_README.md` explaining what it is, why it's vendored,
  the exact upstream commit, and any local modifications. Flag missing pieces.
- **User-facing surface.** If the change adds a setting, an option, or observable
  behavior a user/integrator would need to know about, is there a note where such
  things live? (Don't demand a full doc for an internal helper.)
- **Samples/repro.** For a protocol feature, is there enough in the PR (a repro
  string, a demo, a test that doubles as an example) for a maintainer to try it?

## What to drop

- Requests to document self-explanatory internal code.
- Doc-comment nitpicks with no truth/accuracy impact.
- Style of prose.

## Severity guide for this dimension

- A comment/doc the diff makes actively false (stale/misleading) → medium.
- Missing spec-link on new non-trivial protocol logic → low.
- Visual feature with no committed screenshot artifact → low/medium.
- New `oss/` drop missing `cgmanifest.json`/LICENSE/MAINTAINER_README → medium
  (this one has compliance weight — coordinate with build-and-perf).
- Missing note for a new user-facing option → low.
