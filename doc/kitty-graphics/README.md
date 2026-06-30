# Kitty graphics protocol — MVP screenshots

These screenshots were captured from a real Windows Terminal build, with the
sequences fed through a real shell (`cmd /k type <file>`) — i.e. through the full
shell → conhost ConPTY → Windows Terminal path, exactly as a real application
would drive them.

| File | Scenario |
|------|----------|
| `01-formats.png` | All four transmission formats: Sixel (baseline), Kitty RGB `f=24`, RGBA `f=32`, PNG `f=100` |
| `02-transparency.png` | RGBA alpha compositing — alpha ramp 255→0 and checkerboard transparency |
| `03-placement.png` | Store/place lifecycle — `a=t` store-only (hidden) → `a=p` place by id → reuse → place by number |
| `04-chunked.png` | A 260×150 image reassembled from 39 base64 chunks (`m=1…m=0`) |
| `05-gallery.png` | Inline text + image and multiple images with spec-correct cursor advance |
| `relative-placement.png` | Relative placements — a blue child placed `P=1,Q=1,H=10,V=4` relative to a red parent, measured at exactly 10 cols / 4 rows offset |
