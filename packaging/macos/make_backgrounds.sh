#!/bin/bash
# Regenerates the macOS Installer background images in packaging/macos/resources/ from
# packaging/assets/trino-logo.svg. Requires ImageMagick 7 (magick) with SVG support (librsvg).
#
# Installer.app draws the product background behind the whole window, but the right-hand content
# pane is opaque, so only the left sidebar (below the list of steps) shows it. The logo is therefore
# placed in the bottom-left corner of a transparent 620x418 canvas (the Installer window size;
# @2x = 1240x836 for Retina) instead of the centred packaging/assets/generated/pkg-background.png.
#   background.png / @2x         light appearance: the logo on a dark rounded plate (the logo is the
#                                "knockout" variant, drawn for dark backgrounds; its white parts
#                                would vanish on the light sidebar)
#   background-dark.png / @2x    dark appearance (darkAqua): the logo alone on transparency
#   logo.png                     256x256 plate with the logo, embedded (at 72 pt) in the welcome and
#                                conclusion panes
# build_pkg.sh combines each background pair into one multi-resolution TIFF with tiffutil.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
svg="$here/../assets/trino-logo.svg"
out="$here/resources"
mkdir -p "$out"

mk() {  # scale outfile plate(0|1)
  s="$1"; w=$((620 * s)); h=$((418 * s)); plate=$((132 * s)); margin=$((22 * s)); logo=$((104 * s))
  r=$((22 * s))
  if [ "$3" = 1 ]; then
    magick -size "${w}x${h}" xc:none \
      \( -size "${plate}x${plate}" xc:none -fill '#1d2233' -draw "roundrectangle 0,0 $((plate - 1)),$((plate - 1)) $r,$r" \
         \( -density $((96 * s * 4)) -background none "$svg" -resize "x${logo}" \) -gravity center -composite \) \
      -gravity southwest -geometry "+${margin}+${margin}" -composite "PNG32:$2"
  else
    magick -size "${w}x${h}" xc:none \
      \( -size "${plate}x${plate}" xc:none \
         \( -density $((96 * s * 4)) -background none "$svg" -resize "x${logo}" \) -gravity center -composite \) \
      -gravity southwest -geometry "+${margin}+${margin}" -composite "PNG32:$2"
  fi
}
mk 1 "$out/background.png" 1
mk 2 "$out/background@2x.png" 1
mk 1 "$out/background-dark.png" 0
mk 2 "$out/background-dark@2x.png" 0
magick -size 256x256 xc:none -fill '#1d2233' -draw "roundrectangle 0,0 255,255 44,44" \
  \( -density 768 -background none "$svg" -resize x204 \) -gravity center -composite "PNG32:$out/logo.png"
echo "backgrounds written to $out"
