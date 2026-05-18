# replace-in-file.ps1 - safe literal substring replacement in a file.
#
# Exists because the IDE's replace_string_in_file tool has silently
# truncated long single lines (MSBuild attributes, ;-separated lists)
# on this workspace, leaving malformed XML that only surfaces at build
# time. This script does a plain literal replace via [regex]::Escape,
# with payloads sourced from files so command-line escaping isn't a
# factor.
#
# usage:
#   replace-in-file.ps1 -File <path> -OldFromFile <path> -NewFromFile <path>
#   replace-in-file.ps1 -File <path> -Old "<str>" -New "<str>"
#
# exit codes: 0 = replaced, 1 = old string not found, 2 = bad args.
param(
   [Parameter(Mandatory)] [string]$File,
   [string]$Old,
   [string]$New,
   [string]$OldFromFile,
   [string]$NewFromFile,
   [switch]$AllowMultiple
)

# resolve payloads
if ($OldFromFile) { $Old = [IO.File]::ReadAllText((Resolve-Path $OldFromFile)) }
if ($NewFromFile) { $New = [IO.File]::ReadAllText((Resolve-Path $NewFromFile)) }
if ($null -eq $Old) { Write-Error "missing -Old / -OldFromFile"; exit 2 }
if ($null -eq $New) { $New = '' }

if (-not (Test-Path $File)) { Write-Error "no such file: $File"; exit 2 }

# read, count, replace, write
$content = [IO.File]::ReadAllText((Resolve-Path $File))
$pattern = [regex]::Escape($Old)
$matches = [regex]::Matches($content, $pattern)
if ($matches.Count -eq 0) {
   Write-Error "old string not found in $File"
   exit 1
}
if ($matches.Count -gt 1 -and -not $AllowMultiple) {
   Write-Error "old string matches $($matches.Count) times in $File (pass -AllowMultiple to replace all)"
   exit 1
}

$updated = [regex]::Replace($content, $pattern, { param($m) $New })

# preserve original byte-level newline style by writing without adding one
[IO.File]::WriteAllText((Resolve-Path $File), $updated)
Write-Host "[replace-in-file] $File : replaced $($matches.Count) occurrence(s)"
exit 0
