# nid.ps1 - query the frozen PS3 NID database from the command line.
# loads the JSONs lazily on first use and caches them for the session.
#
# usage:
#   . .\nid.ps1                                # dot-source to import functions
#   Get-Nid 0xB257BC44                         # name + provenance for one NID
#   Get-NidByName cellFsOpen                   # NID for a name (case-insensitive partial match)
#   Get-NidModule cellFs                       # exports + imports for a module name
#   Get-NidProviders 0xB257BC44                # which modules export this NID
#   Get-NidCallers 0xB257BC44                  # which modules import this NID
#   Test-NidVsh 0xB257BC44                     # bool: reachable from a VSH plugin
#   Find-NidName fsOpen                        # substring search across name table
#
# jsons live next to this script (frozen output, no regeneration needed).

$script:NidRoot       = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:NidCache      = $null
$script:NidXrefCache  = $null
$script:NidNamesCache = $null
$script:NidMetaCache  = $null

function script:Load-Json($file) {
   $path = Join-Path $script:NidRoot $file
   if (-not (Test-Path $path)) { throw "missing $path" }
   Write-Host "[nid] loading $file..." -ForegroundColor DarkGray
   return Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
}

function script:Get-NidJson()      { if (-not $script:NidCache)      { $script:NidCache      = Load-Json 'nid.json' };            $script:NidCache }
function script:Get-NidXrefJson()  { if (-not $script:NidXrefCache)  { $script:NidXrefCache  = Load-Json 'nid-xref.json' };       $script:NidXrefCache }
function script:Get-NidNamesJson() { if (-not $script:NidNamesCache) { $script:NidNamesCache = Load-Json 'nid_names.json' };      $script:NidNamesCache }
function script:Get-NidMetaJson()  { if (-not $script:NidMetaCache)  { $script:NidMetaCache  = Load-Json 'nid_names_meta.json' }; $script:NidMetaCache }

# normalize to "0xXXXXXXXX" (uppercase hex, 8 digits). accepts int (possibly
# negative due to 0x80000000+ literals parsing as int32), uint32, long, or string.
function script:Format-Nid($nid) {
   if ($nid -is [string]) {
     $s = $nid.Trim()
     if ($s -match '^0[xX]') { $s = $s.Substring(2) }
     $v = [Convert]::ToUInt32($s, 16)
     return ('0x{0:X8}' -f $v)
   }
   $u = [uint32]([int64]$nid -band 0xFFFFFFFFL)
   return ('0x{0:X8}' -f $u)
}

function Get-Nid {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] $Nid)
   $key   = Format-Nid $Nid
   $meta  = (Get-NidMetaJson).$key
   $xref  = (Get-NidXrefJson).$key
   [pscustomobject]@{
     Nid           = $key
     Name          = if ($meta) { $meta.name }       else { $null }
     Source        = if ($meta) { $meta.source }     else { $null }
     Confidence    = if ($meta) { $meta.confidence } else { $null }
     AvailableFrom = if ($xref) { $xref.available_from } else { @() }
     ProviderCount = if ($xref -and $xref.providers) { @($xref.providers).Count } else { 0 }
     CallerCount   = if ($xref -and $xref.callers)   { @($xref.callers).Count }   else { 0 }
   }
}

function Get-NidByName {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] [string]$Name)
   $names = Get-NidNamesJson
   $hits = $names.PSObject.Properties | Where-Object { $_.Value -eq $Name }
   if (-not $hits) {
     # fall back to case-insensitive contains
     $hits = $names.PSObject.Properties | Where-Object { $_.Value -like "*$Name*" }
   }
   $hits | ForEach-Object { [pscustomobject]@{ Nid = $_.Name; Name = $_.Value } }
}

function Get-NidModule {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] [string]$Module)
   $nid  = Get-NidJson
   $hits = $nid.PSObject.Properties | Where-Object { $_.Name -like "*$Module*" }
   $hits | ForEach-Object {
     $m = $_.Value
     [pscustomobject]@{
       Module      = $_.Name
       File        = $m.file
       ExportCount = if ($m.exports) { @($m.exports.PSObject.Properties).Count } else { 0 }
       ImportLibs  = if ($m.imports) { @($m.imports.PSObject.Properties.Name) }  else { @() }
     }
   }
}

function Get-NidProviders {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] $Nid)
   $key = Format-Nid $Nid
   $x = (Get-NidXrefJson).$key
   if (-not $x -or -not $x.providers) { return @() }
   $x.providers
}

function Get-NidCallers {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] $Nid)
   $key = Format-Nid $Nid
   $x = (Get-NidXrefJson).$key
   if (-not $x -or -not $x.callers) { return @() }
   $x.callers
}

function Test-NidVsh {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] $Nid)
   $key = Format-Nid $Nid
   $x = (Get-NidXrefJson).$key
   if (-not $x) { return $false }
   return @($x.available_from) -contains 'vsh'
}

function Find-NidName {
   [CmdletBinding()] param([Parameter(Mandatory, Position=0)] [string]$Substring)
   $names = Get-NidNamesJson
   $names.PSObject.Properties |
     Where-Object { $_.Value -like "*$Substring*" } |
     ForEach-Object { [pscustomobject]@{ Nid = $_.Name; Name = $_.Value } }
}
