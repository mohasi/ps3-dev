# deploy.ps1 - build a plugin in the VM and install it on the live PS3 via
# the running debug-bridge-client. no FTP, no manual restart by default.
#
# usage:
#   deploy.ps1 <name> [-RestartXmb] [-Config Release|Debug]
#
# examples:
#   deploy.ps1 simple-disc-mount
#   deploy.ps1 simple-debug-bridge -RestartXmb   # self-replace: needs restart to load new code
#   deploy.ps1 simple-ftp -RestartXmb
#
# preconditions:
#   - debug-bridge-client.exe running on host (provides http://localhost:8786/)
#   - PS3 connected (bridge status = connected)
param(
   [Parameter(Mandatory)] [string]$Name,
   [switch]$RestartXmb,
   [ValidateSet('Release','Debug')] [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$bridge = 'http://localhost:8786'

function Invoke-Bridge($path, $method = 'GET', $body = $null) {
   $url = "$bridge/$path"
   if ($body) {
      return (Invoke-WebRequest -Uri $url -Method $method -Body $body -ContentType 'application/octet-stream' -UseBasicParsing).Content
   }
   return (Invoke-WebRequest -Uri $url -Method $method -UseBasicParsing).Content
}

# 1. precondition checks
Write-Host "[deploy] checking bridge..." -ForegroundColor Cyan
try   { $status = Invoke-Bridge 'status' }
catch { Write-Host "[deploy] http bridge unreachable - is debug-bridge-client.exe running?" -ForegroundColor Red; exit 1 }
if ($status -ne 'connected') { Write-Host "[deploy] bridge says: $status" -ForegroundColor Red; exit 1 }

# 2. build in VM
Write-Host "[deploy] building $Name ($Config)..." -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File (Join-Path $root 'vmbuild.ps1') plugins $Name Build $Config
if ($LASTEXITCODE -ne 0) { Write-Host "[deploy] build failed" -ForegroundColor Red; exit $LASTEXITCODE }

$sprx = Join-Path $root "out\$Name.sprx"
if (-not (Test-Path $sprx)) { Write-Host "[deploy] missing $sprx" -ForegroundColor Red; exit 1 }
$bytes = [IO.File]::ReadAllBytes($sprx)
Write-Host "[deploy] built $Name.sprx ($($bytes.Length) bytes)" -ForegroundColor Green

# 3. install via bridge (idempotent: collapses any stale manifest entries)
Write-Host "[deploy] installing on PS3..." -ForegroundColor Cyan
$resp = Invoke-Bridge "vsh-plugin-install?name=$Name&size=$($bytes.Length)" 'POST' $bytes
Write-Host "[deploy] ps3 -> $resp"
if ($resp -notmatch '^OK ') { Write-Host "[deploy] install failed" -ForegroundColor Red; exit 1 }

# 4. optional restart so cobra reloads the sprx (required for self-replace)
if ($RestartXmb) {
   Write-Host "[deploy] restart-xmb..." -ForegroundColor Cyan
   $resp = Invoke-Bridge 'restart-xmb'
   Write-Host "[deploy] ps3 -> $resp"
   Write-Host "[deploy] waiting for bridge..." -ForegroundColor Cyan
   $deadline = (Get-Date).AddSeconds(30)
   do {
      Start-Sleep -Seconds 2
      try { $s = Invoke-Bridge 'status' } catch { $s = 'down' }
   } while ($s -ne 'connected' -and (Get-Date) -lt $deadline)
   if ($s -eq 'connected') { Write-Host "[deploy] bridge back up" -ForegroundColor Green }
   else                    { Write-Host "[deploy] bridge did not reconnect in 30s" -ForegroundColor Yellow }
}
else {
   Write-Host "[deploy] done. run with -RestartXmb to load the new sprx now." -ForegroundColor Green
}
