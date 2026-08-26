# Nyabula Plugin SDK

`nyabula_plugin.py` builds and packages TypeScript/JavaScript plugins for the
Nyabula Core runtime.

## Project layout

```text
plugin/
├── manifest.json
├── src/main.ts
├── assets/
└── node_modules/
```

The source module must default-export an object created with `definePlugin()`.
The build command generates the runtime lifecycle exports and leaves
`@nyabula/*` capability imports external for the device loader.

## Commands

```sh
python nyabula_plugin.py build PLUGIN --esbuild /path/to/esbuild
python nyabula_plugin.py test PLUGIN
python nyabula_plugin.py pack PLUGIN --output plugin.nya
python nyabula_plugin.py install plugin.nya --root SIM_PLUGIN_ROOT
```

`pack` creates a deterministic ZIP-compatible `.nya` archive with a complete
SHA-256 `hashes.json`. `install` verifies the archive and atomically extracts a
new plugin ID under the selected root. Signed upgrades and last-known-good
activation belong to the M3 package manager.
