#!/bin/sh
# Assemble the static web app from a checkout into an output folder.
#   web/build-site.sh <checkout> <out> [dev]
# "dev" marks the copy as a development build: a badge in the header, kept out
# of search engines, and its own browser storage so it cannot disturb the main site's.
set -eu
src=$1 out=$2 channel=${3:-}
mkdir -p "$out"
cp -r "$src/web/." "$out/"
rm -f "$out/build-site.sh"
cp -r "$src/samples" "$out/"
cp "$src/packaging/treefern.svg" "$out/"
[ -f "$src/docs/social-preview.png" ] && cp "$src/docs/social-preview.png" "$out/"
if [ "$channel" = dev ]; then
  sed -i 's|<head>|<head>\n<meta name="robots" content="noindex">\n<meta name="treefern-channel" content="dev">|' "$out/index.html"
  sed -i 's|<title>TreeFern|<title>TreeFern (development)|' "$out/index.html"
fi
