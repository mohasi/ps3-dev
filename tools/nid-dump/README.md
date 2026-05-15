# nid-dump

Frozen NID database for PS3 firmware (4.93 Evilnat dump). Pipeline scripts and
intermediate caches were removed; what remains is the verified output.

## files

- `nid.json`            per-module exports/imports with NIDs and (when known) names
- `nid-xref.json`       inverted index keyed by NID
- `nid_names.json`      flat NID -> name dictionary (16,497 entries)
- `nid_names_meta.json` provenance sidecar: `source` + `confidence` per name

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

## coverage

- modules parsed:     384
- distinct NIDs:      10,446
- named NIDs:         5,063 (48.5%)
- reachable from VSH: 6,490

## notes

- big-endian PPU only.
- NID = first 4 bytes (LE) of SHA1(name + sony-suffix). Same hash as PSP/Vita.
