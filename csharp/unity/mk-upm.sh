#!/usr/bin/env bash
# Assemble the UPM package tree CI pushes to the upm branch. Unity's immutable package
# cache synthesizes no .meta files, so every entry gets one here. Usage: mk-upm.sh <outdir>
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/metas.sh"                                    # guid() + the .meta bodies
out="${1:?usage: mk-upm.sh <outdir>}"
pkg="com.rant.dart"                                   # GUID namespace = install path root

rm -rf "$out" && mkdir -p "$out/Runtime"
cp "$here/package.json" "$here/README.md" "$out/"
cp -r "$here/Runtime/." "$out/Runtime/"

write_meta() {                                        # write_meta <package-relative path>
  local rel="$1" full="$out/$1" body
  if [ -d "$full" ]; then body="$folder_meta"
  else case "$rel" in
    package.json)                body="$package_meta";;
    *.md|*.txt)                  body="$text_meta";;
    *.cs)                        body="$cs_meta";;
    *.asmdef)                    body="$asmdef_meta";;
    *.dll|*.so|*.dylib|*.bundle) body="$plugin_meta";;
    *)                           body="$text_meta";;
  esac; fi
  { echo "fileFormatVersion: 2"; echo "guid: $(guid "$pkg/$rel")"; printf '%s\n' "$body"; } > "$full.meta"
}

# One .meta per file and folder (skip any .meta already present). The package root folder
# itself needs none: it is defined by package.json.
find "$out" -mindepth 1 \( -type d -o -type f \) ! -name '*.meta' -print | while IFS= read -r p; do
  write_meta "${p#"$out"/}"
done
echo "assembled upm package with .meta files at $out"
