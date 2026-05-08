# xml-to-sfo

Tiny .NET console app that turns a PSF (PARAM.SFO) source XML into the binary
`PARAM.SFO` consumed by the PS3 (and `make_package_npdrm`).

Drop an XML onto `xml-to-sfo.exe` (or pass its path as argv). On success the
generator writes `PARAM.SFO` next to the input and exits silently. On any
validation or parse error it prints a message with a line number and pauses
so you can read it.

```
xml-to-sfo <paramsfo.xml> [outputPath]
```

`outputPath` is optional. It may be a full file path or a directory (in which
case `PARAM.SFO` is appended). When omitted it defaults to the input's
directory. Missing parent directories are created.

Targets .NET Framework 3.5 — only because it works out of the box on a
Windows 7 VM where the PS3 SDK is installed, no other reason.

## XML schema

Root must be `<paramsfo>`. Each child is a `<param>` whose key, type, and
optional `maxlength` describe one PSF index entry. The element's text content
is the value.

```xml
<paramsfo>
  <param key="APP_VER">01.00</param>
  <param key="BOOTABLE">1</param>
  <param key="TITLE">My App</param>
  <param key="TITLE_ID">MYAPP0001</param>
</paramsfo>
```

### Attributes

* `key` (required) — the PSF key, uppercase letters / digits / underscores.
  Sony's standard keys are `APP_VER`, `ATTRIBUTE`, `BOOTABLE`, `CATEGORY`,
  `LICENSE`, `PARENTAL_LEVEL`, `PS3_SYSTEM_VER`, `RESOLUTION`, `SOUND_FORMAT`,
  `TITLE`, `TITLE_ID`, `VERSION`.
* `type` (optional) — one of:
  * `utf8` — null-terminated UTF-8 string. PSF format `0x0204`.
  * `utf8-special` — UTF-8 without trailing null. PSF format `0x0004`.
    Rare; prefer `utf8`.
  * `int4` — 32-bit little-endian integer. PSF format `0x0404`.

  For well-known keys the type is inferred automatically. If omitted on an
  unknown key it defaults to `utf8`.
* `maxlength` (optional) — total slot size in bytes. For well-known keys this
  is inferred from the SFO spec (e.g. `TITLE` = 128, `TITLE_ID` = 16). Only
  required for custom / unknown keys with `utf8` or `utf8-special` type.
  Ignored for `int4` (always 4).

### Value parsing

* For `utf8` / `utf8-special`, the element's text content is taken verbatim
  (with leading/trailing whitespace trimmed) and encoded as UTF-8.
* For `int4`, the value may be decimal (`63`) or hex (`0x3F`).

### Output layout

Entries are sorted alphabetically by key (so the output matches what Sony's
`make_package_npdrm` and friends produce). The file structure is:

* header (0x14 bytes) — magic `\0PSF`, version `1.1`, key/data table offsets,
  entry count
* index — `0x10` bytes per entry
* key table — null-terminated keys, padded to 4-byte alignment
* data table — each slot is `maxlength` bytes, zero-padded after the value

## Field reference

| Key              | Type | Notes                                                    |
|------------------|------|----------------------------------------------------------|
| `APP_VER`        | utf8 | Master version, `MM.mm`                                  |
| `ATTRIBUTE`      | int4 | Hardware/feature flag bitfield (`0` = none)              |
| `BOOTABLE`       | int4 | `1` for bootable EBOOT.BIN, `0` for data-only            |
| `CATEGORY`       | utf8 | `HG` (homebrew/disc game), `HM`, `AP`, `GD`              |
| `LICENSE`        | utf8 | Long-form license text (typical max 512 bytes)           |
| `PARENTAL_LEVEL` | int4 | `0`–`11` (`0` = no restriction)                          |
| `PS3_SYSTEM_VER` | utf8 | Required firmware, `MM.mmmm` (e.g. `04.7500`)            |
| `RESOLUTION`     | int4 | Bitfield: `1`=480, `2`=576, `4`=720, `8`=1080, `16`=480w, `32`=576w, `0x3F`=all |
| `SOUND_FORMAT`   | int4 | Bitfield: `1`=LPCM2.0, `4`=LPCM5.1, `16`=LPCM7.1, `0x100`=DD5.1, `0x200`=DTS5.1 |
| `TITLE`          | utf8 | Display name (typical max 128 bytes)                     |
| `TITLE_ID`       | utf8 | 9 chars + null, e.g. `MYAPP0001`                         |
| `VERSION`        | utf8 | Disc version, `MM.mm`                                    |

## Examples

* `sample.xml` — homebrew HDD game SFO with comments documenting each field.
