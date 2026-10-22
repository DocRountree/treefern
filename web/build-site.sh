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
# List the samples for the web app's menu, so a sample committed to samples/
# appears without touching the code. Each is titled by its optional "title",
# else its root clade's name; files that do not parse are left out.
python3 - "$out/samples" <<'PY'
import json, os, sys
folder, found = sys.argv[1], []
for f in sorted(os.listdir(folder)):
    if not f.endswith('.json') or f == 'index.json':
        continue
    try:
        with open(os.path.join(folder, f), encoding='utf-8') as fh:
            top = json.load(fh)
        title = top.get('title') if isinstance(top.get('title'), str) else ''
        if not title:
            root = next((n for n in top.get('nodes', []) if isinstance(n, dict) and n.get('id') == top.get('root')), top)
            title = root.get('name') or f[:-5]
    except Exception as e:
        print(f'build-site: leaving out samples/{f}: {e}', file=sys.stderr)
        continue
    found.append({'file': f, 'title': title})
found.sort(key=lambda s: (s['file'] != 'life.json', s['title'].lower()))  # the Tree of life first: it opens by default
with open(os.path.join(folder, 'index.json'), 'w', encoding='utf-8') as fh:
    json.dump(found, fh, ensure_ascii=False, indent=1)
PY
cp "$src/packaging/treefern.svg" "$out/"
[ -f "$src/docs/social-preview.png" ] && cp "$src/docs/social-preview.png" "$out/"
if [ "$channel" = dev ]; then
  sed -i 's|<head>|<head>\n<meta name="robots" content="noindex">\n<meta name="treefern-channel" content="dev">|' "$out/index.html"
  sed -i 's|<title>TreeFern|<title>TreeFern (development)|' "$out/index.html"
  sed -i 's|"name": "TreeFern"|"name": "TreeFern (development)"|; s|"short_name": "TreeFern"|"short_name": "TreeFern dev"|' "$out/manifest.webmanifest"
fi
