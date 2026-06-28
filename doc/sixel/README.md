# Sixel graphics — scenario screenshots

These screenshots were captured from a real Windows Terminal build, with the
sequences fed through a real shell (`cmd /k type <file>`) — i.e. through the full
shell → conhost ConPTY → Windows Terminal path, exactly as a real application
would drive them.

| File | Scenario |
|------|----------|
| `01-colors.png` | Color registers — six RGB swatches (`#c;2;r;g;b`) and the same hues via HLS (`#c;1;h;l;s`), proving RGB ≡ HLS (DEC hue: 0°=blue, 120°=red) |
| `02-gradient.png` | A smooth gradient using many color registers |
| `03-transparency.png` | Background select — `P2=1` (transparent, only the disc draws) vs `P2=0` (opaque, the whole raster fills) |
| `04-picture.png` | A multi-band picture (bands, `$` color overlays, concentric rings) |
| `05-aspect.png` | Raster aspect ratio — `"1;1` (square) vs `"2;1` (pixels twice as tall) |
