#!/usr/bin/env bash
# Build a .unitypackage from the assembled Runtime/ (Dart.cs + native plugins) WITHOUT a
# Unity install: it fabricates the deterministic GUID + .meta entries and the tar layout
# a .unitypackage uses. Run pack.sh first (copies Dart.cs + built libs into Runtime/).
# Imports into Assets/Dart/. Native plugins are marked "Any platform" so DllImport("dart")
# loads dart.dll / libdart.so by name per OS. Usage: mk-unitypackage.sh <out.unitypackage>
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/Runtime"
out="${1:-$here/dart.unitypackage}"
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

guid() { printf '%s' "$1" | md5sum | cut -c1-32; }   # stable GUID from the asset path

# add_entry <Assets/ path> <source file, or "" for a folder> <meta body>
add_entry() {
  local apath="$1" src="$2" meta="$3" g
  g="$(guid "$apath")"
  mkdir -p "$work/$g"
  printf '%s' "$apath" > "$work/$g/pathname"
  [ -n "$src" ] && cp "$src" "$work/$g/asset"
  { echo "fileFormatVersion: 2"; echo "guid: $g"; printf '%s\n' "$meta"; } > "$work/$g/asset.meta"
}

folder_meta='folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData:'
cs_meta='MonoImporter:
  externalObjects: {}
  serializedVersion: 2
  defaultReferences: []
  executionOrder: 0
  icon: {instanceID: 0}
  userData:'
asmdef_meta='AssemblyDefinitionImporter:
  externalObjects: {}
  userData:'
plugin_meta='PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 1
      settings: {}
  userData:'

add_entry "Assets/Dart" "" "$folder_meta"
add_entry "Assets/Dart/Dart.cs" "$rt/Dart.cs" "$cs_meta"
[ -f "$rt/Dart.asmdef" ] && add_entry "Assets/Dart/Dart.asmdef" "$rt/Dart.asmdef" "$asmdef_meta"
add_entry "Assets/Dart/Plugins" "" "$folder_meta"
for lib in "$rt"/Plugins/x86_64/*; do
  [ -f "$lib" ] || continue
  add_entry "Assets/Dart/Plugins/$(basename "$lib")" "$lib" "$plugin_meta"
done

( cd "$work" && tar czf "$out" -- * )
echo "wrote $out"
