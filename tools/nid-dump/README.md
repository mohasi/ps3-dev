# nid-dump

Frozen symbol-ID database for PS3 firmware (4.93 Evilnat dump), plus a small
PowerShell script to query it. Every PS3 library function is identified by a
4-byte "NID" (a hash of its name); this database maps those NIDs back to human
names, tells you which module provides or calls each one, and whether a VSH
plugin can reach it. The generation pipeline and intermediate caches were
removed after the data was verified — what remains is the finished output and
the query helpers.

The `debug-bridge-client` tool reads `nid_names.json` and `nid_protos.json` to
label symbols in its Modules and Trace tabs.

## files

- `nid.json`             per-module exports/imports with NIDs and (when known) names
- `nid-xref.json`        inverted index keyed by NID (providers, callers, reach)
- `nid_names.json`       flat NID -> name dictionary
- `nid_names_meta.json`  provenance sidecar: `source` + `confidence` per name
- `nid_names_local.json` hand-added local overrides (e.g. `vshNotify`, `isXmbReady`)
- `nid_protos.json`      function prototype per NID (`ret` + `args`), for readable call traces
- `nid.ps1`              query functions (see below)

## querying

```powershell
. .\nid.ps1                      # dot-source to import the functions
Get-Nid 0xB257BC44               # name + provenance for one NID
Get-NidByName cellFsOpen         # NID for a name (case-insensitive)
Get-NidModule cellFs             # exports + imports for a module
Get-NidProviders 0xB257BC44      # which modules export this NID
Get-NidCallers 0xB257BC44        # which modules import this NID
Test-NidVsh 0xB257BC44           # is it reachable from a VSH plugin?
Find-NidName fsOpen              # substring search across the name table
```

The JSONs load lazily on first use and are cached for the session.

## origin tags

Where each providing/calling module lives on a real PS3:

- `external` -> /dev_flash/sys/external (shared libs, app + vsh)
- `internal` -> /dev_flash/sys/internal (kernel-side / lv2-private)
- `vsh`      -> /dev_flash/vsh/module   (XMB / VSH plugins)

`available_from` rolls up across providers and callers. To check whether a NID
is reachable from a VSH plugin: `"vsh" in entry["available_from"]`.

## provenance

`source` is one of:
- `public` - SDK stubs/headers, RPCS3, CHM docs, psdevwiki, PSP libdoc cross-hash
- `binary` - identifiers recovered from ELF rodata
- `brute`  - structured brute-force candidates that survived auditing

## coverage (at freeze)

- modules parsed:     384
- distinct NIDs:      10,446
- named NIDs:         5,063 (48.5%)
- reachable from VSH: 6,490

## notes

- big-endian PPU only.
- NID = first 4 bytes (LE) of SHA1(name + sony-suffix). Same hash as PSP/Vita.
