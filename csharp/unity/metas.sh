# The shared Unity .meta bodies and the deterministic GUID scheme, sourced by both package
# builders. A GUID is the md5 of the asset's import path, so upgrades keep references.
guid() { printf '%s' "$1" | md5sum | cut -c1-32; }

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

# upm-only: package.json is a package manifest, README.md a text asset.
package_meta='PackageManifestImporter:
  externalObjects: {}
  userData:'

text_meta='TextScriptImporter:
  externalObjects: {}
  userData:'

# Native plugins marked "Any platform" so DllImport("dart") loads dart.dll / libdart.so by
# name per OS (editor + desktop standalone).
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
