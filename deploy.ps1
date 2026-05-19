# deploy.ps1 - build in the VM and install on the live PS3 via the
# running debug-bridge-client. no FTP, no manual restart by default.
#
# usage:
#   deploy.ps1 <name> [-RestartXmb] [-Config Release|Debug] [-NoClean]
#
# target is auto-detected:
#   dev/plugins/<name>  -> vsh plugin (.sprx); installs via vsh-plugin-install
#   dev/apps/<name>     -> npdrm app   (.pkg);  installs via pkg-install
#                          (XMB restart is implied so the tile appears)
#
# examples:
#   deploy.ps1 simple-disc-mount
#   deploy.ps1 simple-debug-bridge -RestartXmb
#   deploy.ps1 file-manager                 # app: restart-xmb is automatic
#   deploy.ps1 app-sample -NoClean          # keep existing /game/<TID> tree
#
# preconditions:
#   - debug-bridge-client.exe running on host (provides http://localhost:8786/)
#   - PS3 connected (bridge status = connected)
param(
   [Parameter(Mandatory)] [string]$Name,
   [switch]$RestartXmb,
   [switch]$NoClean,
   [ValidateSet('Release','Debug')] [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$bridge = 'http://localhost:8786'

function Invoke-Bridge($path, $method = 'GET', $body = $null, $timeoutSec = 100) {
   $url = "$bridge/$path"
   if ($body) {
      return (Invoke-WebRequest -Uri $url -Method $method -Body $body -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec $timeoutSec).Content
   }
   return (Invoke-WebRequest -Uri $url -Method $method -UseBasicParsing -TimeoutSec $timeoutSec).Content
}

function Wait-Bridge($seconds) {
   Write-Host "[deploy] waiting for bridge..." -ForegroundColor Cyan
   $deadline = (Get-Date).AddSeconds($seconds)
   do {
      Start-Sleep -Seconds 1
      try { $s = Invoke-Bridge 'status' } catch { $s = 'down' }
   } while ($s -ne 'connected' -and (Get-Date) -lt $deadline)
   if ($s -eq 'connected') { Write-Host "[deploy] bridge back up" -ForegroundColor Green }
   else                    { Write-Host "[deploy] bridge did not reconnect in ${seconds}s" -ForegroundColor Yellow }
}

# 0. resolve target kind from folder layout
if     (Test-Path (Join-Path $root "plugins\$Name")) { $kind = 'plugins' }
elseif (Test-Path (Join-Path $root "apps\$Name"))    { $kind = 'apps' }
else { Write-Host "[deploy] no plugin or app named '$Name' under dev/" -ForegroundColor Red; exit 1 }

# 1. precondition checks
Write-Host "[deploy] checking bridge..." -ForegroundColor Cyan
try   { $status = Invoke-Bridge 'status' }
catch { Write-Host "[deploy] http bridge unreachable - is debug-bridge-client.exe running?" -ForegroundColor Red; exit 1 }
if ($status -ne 'connected') { Write-Host "[deploy] bridge says: $status" -ForegroundColor Red; exit 1 }

# 2. build in VM
Write-Host "[deploy] building $kind/$Name ($Config)..." -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File (Join-Path $root 'vmbuild.ps1') $kind $Name Build $Config
if ($LASTEXITCODE -ne 0) { Write-Host "[deploy] build failed" -ForegroundColor Red; exit $LASTEXITCODE }

# 3. install via bridge
if ($kind -eq 'plugins') {
   $sprx = Join-Path $root "out\$Name.sprx"
   if (-not (Test-Path $sprx)) { Write-Host "[deploy] missing $sprx" -ForegroundColor Red; exit 1 }
   $bytes = [IO.File]::ReadAllBytes($sprx)
   Write-Host "[deploy] built $Name.sprx ($($bytes.Length) bytes)" -ForegroundColor Green

   Write-Host "[deploy] installing on PS3..." -ForegroundColor Cyan
   $resp = Invoke-Bridge "vsh-plugin-install?name=$Name&size=$($bytes.Length)" 'POST' $bytes
   Write-Host "[deploy] ps3 -> $resp"
   if ($resp -notmatch '^OK ') { Write-Host "[deploy] install failed" -ForegroundColor Red; exit 1 }

   # 4. optional restart so cobra reloads the sprx (required for self-replace)
   if ($RestartXmb) {
      Write-Host "[deploy] restart-xmb..." -ForegroundColor Cyan
      $resp = Invoke-Bridge 'restart-xmb'
      Write-Host "[deploy] ps3 -> $resp"
      Wait-Bridge 30
   }
   else {
      Write-Host "[deploy] done. run with -RestartXmb to load the new sprx now." -ForegroundColor Green
   }
}
else {
   # app: upload the pkg, let the bridge stage + extract into /dev_hdd0/game/<TID>/.
   $pkg = Join-Path $root "out\$Name.pkg"
   if (-not (Test-Path $pkg)) { Write-Host "[deploy] missing $pkg" -ForegroundColor Red; exit 1 }
   $bytes = [IO.File]::ReadAllBytes($pkg)
   Write-Host "[deploy] built $Name.pkg ($($bytes.Length) bytes)" -ForegroundColor Green

   $clean = if ($NoClean) { 0 } else { 1 }
   Write-Host "[deploy] installing on PS3 (clean=$clean)..." -ForegroundColor Cyan
   $resp = Invoke-Bridge "pkg-install?name=$Name&clean=$clean" 'POST' $bytes 300
   Write-Host "[deploy] ps3 -> $resp"
   if ($resp -notmatch '^OK ') { Write-Host "[deploy] install failed" -ForegroundColor Red; exit 1 }

   # XMB caches the Games column at boot; without a restart the new tile
   # won't appear. always do it for app installs (cheap compared to the
   # ~30s an SDK installer would take).
   Write-Host "[deploy] restart-xmb..." -ForegroundColor Cyan
   $resp = Invoke-Bridge 'restart-xmb'
   Write-Host "[deploy] ps3 -> $resp"
   Wait-Bridge 30
}

