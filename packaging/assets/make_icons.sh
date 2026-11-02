#!/bin/bash
# Regenerates every icon/bitmap used by the installers from packaging/assets/trino-logo.svg.
# Requires: rsvg-convert (librsvg), magick (ImageMagick 7), iconutil (macOS, for .icns).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
svg="$here/trino-logo.svg"
out="$here/generated"
mkdir -p "$out/png" "$out/TrinoODBC.iconset"

# Square, transparent PNGs (the logo is taller than wide; it is centred on a square canvas).
for s in 16 32 48 64 128 256 512 1024; do
  rsvg-convert -h "$s" "$svg" -o "$out/png/logo-$s-raw.png"
  magick "$out/png/logo-$s-raw.png" -background none -gravity center -extent "${s}x${s}" "$out/png/logo-$s.png"
  rm -f "$out/png/logo-$s-raw.png"
done

# Windows .ico (multi-resolution).
magick "$out/png/logo-16.png" "$out/png/logo-32.png" "$out/png/logo-48.png" "$out/png/logo-64.png" \
       "$out/png/logo-128.png" "$out/png/logo-256.png" "$out/trinoodbc.ico"

# macOS .icns
for s in 16 32 128 256 512; do
  cp "$out/png/logo-$s.png" "$out/TrinoODBC.iconset/icon_${s}x${s}.png"
  cp "$out/png/logo-$((s*2)).png" "$out/TrinoODBC.iconset/icon_${s}x${s}@2x.png"
done
if command -v iconutil >/dev/null 2>&1; then
  iconutil -c icns "$out/TrinoODBC.iconset" -o "$out/TrinoODBC.icns"
fi

# Installer bitmaps (24-bit BMP3, white background): NSIS/Inno welcome 164x314, NSIS header 150x57,
# Inno small image 55x58.
mk_bmp() {  # width height logo-height outfile
  magick -size "$1x$2" xc:white \( "$svg" -background none -resize "x$3" \) -gravity center -composite \
         -type TrueColor -define bmp:format=bmp3 -compress none "$4"
}
mk_bmp 164 314 200 "$out/installer-welcome.bmp"
mk_bmp 150 57 44 "$out/installer-header.bmp"
mk_bmp 55 58 48 "$out/installer-small.bmp"

# macOS installer background (Retina 1240x836 and 620x418) and DMG background.
magick -size 620x418 xc:white \( "$svg" -background none -resize x220 \) -gravity center -composite "$out/pkg-background.png"
magick -size 1240x836 xc:white \( "$svg" -background none -resize x440 \) -gravity center -composite "$out/pkg-background@2x.png"
magick -size 600x400 xc:white \( "$svg" -background none -resize x200 \) -gravity northwest -geometry +60+100 -composite "$out/dmg-background.png"
echo "assets written to $out"
ls -la "$out"
