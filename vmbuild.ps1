# vmbuild.ps1 - run a build inside the Win7 VM over SSH and stream output.
# usage: vmbuild.ps1 <plugins|apps|tools> <name> [Build|Rebuild|Clean] [Release|Debug]
#
# auto-starts the VM headless via VBoxManage if it isn't already running, then
# waits for SSH to come up before invoking the build.
#
# VM connection defaults can be overridden via env vars:
#   PS3VM_USER, PS3VM_HOST, PS3VM_PORT, PS3VM_KEY, PS3VM_NAME
param(
   [Parameter(Mandatory)][ValidateSet('plugins','apps','tools')] [string]$Kind,
   [Parameter(Mandatory)] [string]$Name,
   [ValidateSet('Build','Rebuild','Clean')] [string]$Target = 'Build',
   [ValidateSet('Release','Debug')] [string]$Config = 'Release',
   [string]$VmUser = $(if ($env:PS3VM_USER) { $env:PS3VM_USER } else { 'Mohammed' }),
   [string]$VmHost = $(if ($env:PS3VM_HOST) { $env:PS3VM_HOST } else { '127.0.0.1' }),
   [int]   $VmPort = $(if ($env:PS3VM_PORT) { [int]$env:PS3VM_PORT } else { 2222 }),
   [string]$VmKey  = $(if ($env:PS3VM_KEY)  { $env:PS3VM_KEY  } else { "$env:USERPROFILE\.ssh\ps3vm_ed25519" }),
   [string]$VmName = $(if ($env:PS3VM_NAME) { $env:PS3VM_NAME } else { 'Windows7' })
)

$vbox = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'

# bring the VM up if not already running, then poll SSH until ready.
function Ensure-VmUp {
   if (-not (Test-Path $vbox)) { return }   # no VBox installed; assume VM is managed externally
   $running = & $vbox list runningvms
   if ($running -match [regex]::Escape($VmName)) { return }

   Write-Host "[vmbuild] starting VM '$VmName' headless..." -ForegroundColor Cyan
   & $vbox startvm $VmName --type headless | Out-Null
   if ($LASTEXITCODE -ne 0) { Write-Host "[vmbuild] VBoxManage startvm failed" -ForegroundColor Red; exit 1 }

   $deadline = (Get-Date).AddSeconds(120)
   $prevProgress = $ProgressPreference
   $ProgressPreference = 'SilentlyContinue'
   try {
      do {
         Start-Sleep -Seconds 3
         $tcp = Test-NetConnection -ComputerName $VmHost -Port $VmPort -InformationLevel Quiet -WarningAction SilentlyContinue
         if ($tcp) { Write-Host "[vmbuild] VM ssh up" -ForegroundColor Green; return }
         Write-Host "[vmbuild] waiting for ssh..." -ForegroundColor DarkGray
      } while ((Get-Date) -lt $deadline)
   } finally {
      $ProgressPreference = $prevProgress
   }

   Write-Host "[vmbuild] VM did not expose ssh within 120s" -ForegroundColor Red
   exit 1
}

Ensure-VmUp

# stream stdout+stderr line by line so we see progress; exit code is preserved.
& ssh -o IdentitiesOnly=yes -o LogLevel=ERROR -i $VmKey -p $VmPort "$VmUser@$VmHost" `
   "X:\dev\build.bat $Kind $Name $Target $Config" 2>&1 |
   ForEach-Object { Write-Host $_ }

$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "[vmbuild] build failed (exit $code)" -ForegroundColor Red }
exit $code
