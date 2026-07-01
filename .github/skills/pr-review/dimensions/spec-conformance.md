# Spec & protocol conformance review

You are reviewing a diff for this Windows Terminal / OpenConsole fork for
**alignment with the documented terminal protocols**. This replaces a generic
"UX" dimension: for a terminal emulator, "does it match the spec" is the UX.
Apply the shared output contract in `_shared-contract.md`. Set
`Domain: spec-conformance` on every finding.

## The specs that govern this code

- **DEC / VT100 / VT500** control functions — `https://vt100.net/`.
- **xterm** control sequences — `https://invisible-island.net/xterm/ctlseqs/ctlseqs.html`.
- **Sixel** graphics — the DEC sixel spec / xterm's implementation.
- **Kitty graphics protocol** — `https://sw.kovidgoyal.net/kitty/graphics-protocol/`
  (transmission, placement, Unicode placeholders, relative placements, deletion,
  compression `o=z`, error/ACK codes like `OK`/`ENOENT`/`EINVAL`/`EBADF`/
  `ENOPARENT`/`ECYCLE`/`ETOODEEP`).

kitty is **GPLv3**: behavior must be derived from the **public spec**, not from
kitty's source or tests. Flag anything that looks copied/translated.

## What to look for

- **Behavior matches the spec.** Does a parsed key do what the spec says? Are
  defaults correct (a key omitted → the spec's default, e.g. `p=0` semantics,
  opaque vs transparent background, aspect handling)? Is the coordinate origin,
  sign convention, and unit (cells vs pixels) right?
- **ACK / error codes.** The exact code string the spec mandates for each
  condition (e.g. missing parent → `ENOPARENT`, cycle → `ECYCLE`, over-depth →
  `ETOODEEP`, unreadable file → `EBADF`). A generic `EINVAL` where the spec
  names a specific code is a finding. The ack framing (`ESC _G … ESC \`, C1
  vs 7-bit) and whether `i=`/`I=`/`p=` are echoed per spec.
- **Spec citations in code.** New non-obvious protocol handling should carry a
  spec-link comment (this repo cites `vt100.net` / `xterm` / the kitty protocol
  URL next to the logic). A new APC/DCS handler or error code with no spec
  reference is a (low/medium) finding — it's the repo's convention.
- **Deferred vs wrong.** It's fine to reject an unimplemented feature with the
  spec's error code (`EINVAL:unsupported …`) and defer it. It is **not** fine to
  silently accept-and-misbehave, or to invent a code the spec doesn't define.
- **Interoperability.** Would a real client (kitty's own `icat`, a sixel
  encoder, tmux passthrough) get a response it can parse? Round-trip
  assumptions (what the client expects back).
- **Clean-room.** Magic tables/constants that must match the spec for interop
  (e.g. the rowcolumn-diacritics list) are spec *data* and OK to reproduce, but
  logic/comments lifted from kitty source are not. Flag suspected copying.

## What to drop

- Cosmetic wording of an error *detail* string (the `:detail` after the code) —
  only the code itself is contractual.
- Requests to implement more of the spec than the PR scopes (that's a backlog
  issue, not a review blocker) — unless the PR *claims* to implement it.

## Severity guide for this dimension

- Behavior that violates the spec in a way a client observes (wrong geometry,
  wrong code, wrong default) → high.
- Wrong/missing ACK code where the spec names a specific one → high.
- Silent accept-and-misbehave instead of the spec's rejection → high.
- Missing spec-link comment on new protocol logic → low (medium if the logic
  is subtle and easy to get wrong later).
- Suspected copying from GPLv3 kitty source → high (license risk) — flag for a
  human to verify.
