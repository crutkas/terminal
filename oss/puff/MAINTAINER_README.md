### Notes for Future Maintainers

`puff.c` / `puff.h` are Mark Adler's reference inflate (RFC 1951 DEFLATE), vendored
verbatim from zlib's `contrib/puff/` at tag `v1.3.1`
(commit `925af44f3cde53c6b076611c297850091b5dc7bb`, see `cgmanifest.json`).

They back the Kitty graphics `o=z` (zlib) decode path in
`src/terminal/adapter/adaptDispatch.cpp` (`_inflateKittyZlib`): that helper validates
and strips the RFC 1950 zlib header / Adler-32 trailer, then calls `puff()` to inflate
the DEFLATE body. puff's NULL-destination "scanning mode" is used first to compute the
exact output size, so a decompression bomb is rejected against `MaxKittyPayload`
*before* any buffer is allocated.

Only `puff.c` and `puff.h` are used; do not pull in the rest of zlib. Keep the source
pristine (no local edits) so the `cgmanifest.json` provenance stays accurate -- the
zlib license requires altered source versions to be plainly marked as such.
