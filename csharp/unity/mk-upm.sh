#!/usr/bin/env bash
# Assemble the UPM package tree (package.json + README.md + Runtime/) that CI force-pushes to
# the `upm` branch for git-URL install. A git-URL package lands in Unity's IMMUTABLE package
# cache, where Unity will NOT synthesize .meta files, so every file and folder must already
# carry one or the asset is ignored. This walks the assembled tree and writes a .meta per
# entry, importer chosen by extension, GUID = md5(package-relative path) so upgrades keep
# their references. Run pack.sh first (stages Dart.cs + native libs into Runtime/).
# Usage: mk-upm.sh <outdir>
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
