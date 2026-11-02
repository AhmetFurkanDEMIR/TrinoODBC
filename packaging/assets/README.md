# Installer artwork: the Trino logo

This folder holds the logo that the installers, icons and disk images of the Trino ODBC driver
show, and everything generated from it.

> **Trademark.** The Trino logo is a trademark of the Trino Software Foundation. This driver is an
> independent open-source project, not affiliated with or endorsed by the Trino Software
> Foundation. The logo is used only to indicate compatibility with Trino; no endorsement is
> implied, and the logo is not covered by this project's Apache License 2.0. Every place that shows
> the logo (the installer pages, the READMEs, [NOTICE](../../NOTICE) and
> [THIRD-PARTY-NOTICES.md](../../THIRD-PARTY-NOTICES.md#trademarks)) carries this note.

## Files

| File | What it is | Used by |
|---|---|---|
| `trino-logo.svg` | The official Trino logo ("knockout" variant, drawn for dark backgrounds), unmodified | the source of everything below |
| `make_icons.sh` | Regenerates `generated/` from the SVG | run by hand after changing the SVG |
| `generated/png/logo-<size>.png` | Square, transparent PNGs, 16 to 1024 px (the logo centred on the square) | the other icons; the macOS installer panes fall back to `logo-256.png` |
| `generated/trinoodbc.ico` | Windows icon, 16/32/48/64/128/256 px | NSIS and Inno Setup installer / uninstaller icon, Add/Remove Programs icon |
| `generated/installer-welcome.bmp` | 164x314, 24-bit BMP, white background | NSIS welcome and finish pages, Inno Setup wizard image |
| `generated/installer-header.bmp` | 150x57, 24-bit BMP | NSIS header of the inner pages |
| `generated/installer-small.bmp` | 55x58, 24-bit BMP | Inno Setup small wizard image |
| `generated/TrinoODBC.icns`, `generated/TrinoODBC.iconset/` | macOS icon (16 to 512 pt, @1x and @2x) | volume icon of the macOS disk image (`.VolumeIcon.icns`) |
| `generated/pkg-background.png`, `pkg-background@2x.png` | 620x418 / 1240x836, logo centred on white | fallback macOS Installer background when `packaging/macos/resources/background*.png` are missing |
| `generated/dmg-background.png` | 600x400, logo on white | not used yet: a DMG background needs a Finder-written `.DS_Store`, which `build_pkg.sh` does not create |

The macOS Installer uses its own images in [`packaging/macos/resources/`](../macos/resources/),
made from the same SVG by [`packaging/macos/make_backgrounds.sh`](../macos/make_backgrounds.sh).
Installer.app only shows the background in the sidebar (the content pane is opaque), so those put
the logo in the bottom-left corner of a transparent canvas:

| File | What it is |
|---|---|
| `background.png`, `background@2x.png` | light appearance: the logo on a dark rounded plate (its white parts would vanish on the light sidebar) |
| `background-dark.png`, `background-dark@2x.png` | dark appearance (`background-darkAqua`): the logo alone |
| `logo.png` | 256x256 plate with the logo, embedded (as a `data:` URI) in the welcome and conclusion pages |

`build_pkg.sh` combines each background pair into one multi-resolution TIFF with `tiffutil`, and
inlines `style.css` and the logo into the rendered pages, because `productbuild` archives only the
resources that the Distribution file names.

## Regenerating

```sh
brew install librsvg imagemagick      # rsvg-convert, magick; iconutil comes with macOS
packaging/assets/make_icons.sh        # -> packaging/assets/generated/
packaging/macos/make_backgrounds.sh   # -> packaging/macos/resources/*.png
```

`make_icons.sh` builds the `.icns` only on macOS (`iconutil`). The generated files are committed,
so building the installers needs neither librsvg nor ImageMagick. Replace `trino-logo.svg` only
with an official Trino logo, and keep the trademark note above wherever the logo is shown.
