# trace-decode.ps1 - decode a TRAC v3 capture into a human-readable listing.
#
# usage:
#   .\trace-decode.ps1 -InputBin <path-to-.bin> [-OutputTxt <path>]
#
# input format (big-endian PPU dump):
#   header (16 bytes): magic[4]='TRAC', version u32, manifestOffset u32, reserved u32
#   events  [16 .. manifestOffset): packed 16-byte records { slotAddr u32, r3 u32, r4 u32, r5 u32 }
#   manifest tail (ASCII):
#     \n==MANIFEST==\n
#     slot 0x<addr>\t<callerModule>\t0x<nid>\n   (one per armed slot)
#     ==SUMMARY==\n
#     slots\trequested=N armed=N dropped=N\n
#     events\twritten=N ring_dropped=N\n
#     ==END==\n
#
# nid -> name resolution uses nid-dump/nid_names.json (global table).
# nid -> providing library resolution uses nid-dump/nid-xref.json (providers[].module).

[CmdletBinding()]
param(
   [Parameter(Mandatory)] [string]$InputBin,
   [string]$OutputTxt
)

$ErrorActionPreference = 'Stop'

if (-not $OutputTxt) {
   $base = [System.IO.Path]::GetFileNameWithoutExtension($InputBin)
   $dir  = [System.IO.Path]::GetDirectoryName((Resolve-Path $InputBin))
   $OutputTxt = Join-Path $dir "$base.decoded.txt"
}

$nidRoot = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'nid-dump'
$namesPath = Join-Path $nidRoot 'nid_names.json'
$xrefPath  = Join-Path $nidRoot 'nid-xref.json'

function be32 { param($buf, [int]$off)
   return ([uint32]$buf[$off]     -shl 24) -bor `
          ([uint32]$buf[$off + 1] -shl 16) -bor `
          ([uint32]$buf[$off + 2] -shl  8) -bor `
          ([uint32]$buf[$off + 3])
}

function fmtNid { param([uint32]$n) return ('0x{0:X8}' -f $n) }

Write-Host "[trace-decode] reading $InputBin"
$buf = [System.IO.File]::ReadAllBytes((Resolve-Path $InputBin))
if ($buf.Length -lt 16) { throw "file too small" }

$magic = [System.Text.Encoding]::ASCII.GetString($buf, 0, 4)
if ($magic -ne 'TRAC') { throw "bad magic: $magic" }
$version = be32 $buf 4
$manifestOff = [int](be32 $buf 8)
if ($version -ne 3) { throw "unsupported version: $version" }
if ($manifestOff -lt 16 -or $manifestOff -gt $buf.Length) { throw "bad manifestOffset: $manifestOff" }

$eventsLen = $manifestOff - 16
if ($eventsLen % 16 -ne 0) { Write-Warning "events region not multiple of 16: $eventsLen" }
$eventCount = [int]($eventsLen / 16)
Write-Host "[trace-decode] header ok: events=$eventCount manifestOffset=$manifestOff totalBytes=$($buf.Length)"

# parse manifest tail.
$tail = [System.Text.Encoding]::ASCII.GetString($buf, $manifestOff, $buf.Length - $manifestOff)
$slotMap = @{}   # slotAddr (uint32) -> @{ caller=...; nid=uint32 }
foreach ($line in ($tail -split "`n")) {
   if ($line -match '^slot\s+0x([0-9a-fA-F]+)\s+(\S+)\s+0x([0-9a-fA-F]+)\s*$') {
      $addr = [Convert]::ToUInt32($matches[1], 16)
      $nid  = [Convert]::ToUInt32($matches[3], 16)
      $slotMap[$addr] = @{ caller = $matches[2]; nid = $nid }
   }
}
Write-Host "[trace-decode] manifest slots: $($slotMap.Count)"

# parse summary (best effort).
$summary = @()
foreach ($line in ($tail -split "`n")) {
   if ($line -match '^(slots|events)\s+\S') { $summary += $line }
}

Write-Host "[trace-decode] loading nid_names.json..."
$names = Get-Content -Raw -LiteralPath $namesPath | ConvertFrom-Json
Write-Host "[trace-decode] loading nid-xref.json..."
$xref  = Get-Content -Raw -LiteralPath $xrefPath  | ConvertFrom-Json

function lookupName { param([uint32]$nid)
   $key = fmtNid $nid
   $v = $names.$key
   if ($v) { return $v }
   # also try lowercase form, some tables differ
   $kl = '0x' + ($key.Substring(2).ToLower())
   $v = $names.$kl
   if ($v) { return $v }
   return ''
}

function lookupProvider { param([uint32]$nid)
   $key = fmtNid $nid
   $x = $xref.$key
   if (-not $x) { return '' }
   if (-not $x.providers) { return '' }
   $mods = @($x.providers | ForEach-Object { $_.module }) | Sort-Object -Unique
   return ($mods -join ',')
}

# decode events.
$out = New-Object System.Text.StringBuilder
[void]$out.AppendLine("# trace-decode  input=$InputBin")
[void]$out.AppendLine("# events=$eventCount  manifestSlots=$($slotMap.Count)")
foreach ($s in $summary) { [void]$out.AppendLine("# $s") }
[void]$out.AppendLine("# columns: idx  slotAddr  providerLib  nid        name                          r3         r4         r5")

for ($i = 0; $i -lt $eventCount; $i++) {
   $o = 16 + $i * 16
   $slot = be32 $buf $o
   $r3   = be32 $buf ($o + 4)
   $r4   = be32 $buf ($o + 8)
   $r5   = be32 $buf ($o + 12)

   $entry = $slotMap[$slot]
   if ($entry) {
      $nid  = [uint32]$entry.nid
      $name = lookupName $nid
      $prov = lookupProvider $nid
   } else {
      $nid  = [uint32]0
      $name = '<slot-not-in-manifest>'
      $prov = ''
   }

   $line = '{0,6}  0x{1:X8}  {2,-22}  {3}  {4,-32}  0x{5:X8}  0x{6:X8}  0x{7:X8}' -f `
      $i, $slot, $prov, (fmtNid $nid), $name, $r3, $r4, $r5
   [void]$out.AppendLine($line)
}

[System.IO.File]::WriteAllText($OutputTxt, $out.ToString())
Write-Host "[trace-decode] wrote $OutputTxt"
