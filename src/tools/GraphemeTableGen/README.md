# GraphemeTableGen

`Program.cs` is the **sole authoritative generator** for the multi-stage
trie that backs `src/types/CodepointWidthDetector.cpp` (the per-codepoint
grapheme/width/emoji table) and for the grapheme join-rule tables.

## Why this is the only generator

Historically, the repo carried two parallel sources of truth for width
data:

1. `tools/Generate-CodepointWidthsFromUCD.ps1` — a PowerShell pipeline
   that ingested `EastAsianWidth.txt` and emitted a flat `IsWide` table.
2. `src/types/unicode_width_overrides.xml` — a hand-edited override list
   (Box Drawing → Narrow, Yijing Hexagrams → Narrow, Combining Half
   Marks → Narrow, ...).

Both are deleted as of the WIDE/CJK PR (D6 / R13). All width decisions
— EastAsianWidth, EastAsianWidth=Ambiguous policy, the Yijing /
Combining-Half-Marks / Box-Drawing overrides, and the
`Emoji_Presentation=Yes` widening (W17) — now flow exclusively through
`Program.cs`. Editing either of the deleted artifacts will have no
effect because they no longer exist.

## Inputs

The generator consumes the standard XML form of the UCD database, plus
the emoji property data file. Both must come from the **same Unicode
release** to avoid mismatches between East-Asian-Width and emoji
properties.

| File                          | Source                                                   |
|-------------------------------|----------------------------------------------------------|
| `ucd.nounihan.grouped.xml`    | https://www.unicode.org/Public/16.0.0/ucdxml/            |
| `emoji-data.txt` (via UCD XML)| Embedded under `/repertoire/group/char/@Emoji_Presentation` |

The currently shipped trie is generated from **Unicode 16.0.0** (UCD
release 2024-09-10; emoji-data.txt revision 2024-05-01). The version
string is also stamped into `CodepointWidthDetector.cpp` near `s_stage0`.

## Running it

```powershell
dotnet run --project src/tools/GraphemeTableGen `
    -- path\to\ucd.nounihan.grouped.xml > src/types/CodepointWidthDetector.cpp.gen
```

Diff the generated file against the committed `CodepointWidthDetector.cpp`
trie block to see exactly which codepoints moved width before committing.

## Cross-references

- Test corpus regenerated from `emoji-data.txt`:
  `src/types/ut_types/EPresCorpus.inc` (1212 codepoints, Unicode 16.0.0),
  produced by `src/types/ut_types/gen_epres_corpus.ps1`.
- Runtime trie consumer: `ucdLookup` in
  `src/types/CodepointWidthDetector.cpp`.
