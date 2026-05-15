# vmbuild.ps1 - run a build inside the Win7 VM over SSH and stream output.
# usage: vmbuild.ps1 <plugins|apps|tools> <name> [Build|Rebuild|Clean] [Release|Debug]
#
# VM connection defaults can be overridden via env vars:
#   PS3VM_USER, PS3VM_HOST, PS3VM_PORT, PS3VM_KEY
param(
   [Parameter(Mandatory)][ValidateSet('plugins','apps','tools')] [string]$Kind,
   [Parameter(Mandatory)] [string]$Name,
   [ValidateSet('Build','Rebuild','Clean')] [string]$Target = 'Build',
   [ValidateSet('Release','Debug')] [string]$Config = 'Release',
   [string]$VmUser = $(if ($env:PS3VM_USER) { $env:PS3VM_USER } else { 'Mohammed' }),
   [string]$VmHost = $(if ($env:PS3VM_HOST) { $env:PS3VM_HOST } else { '127.0.0.1' }),
   [int]   $VmPort = $(if ($env:PS3VM_PORT) { [int]$env:PS3VM_PORT } else { 2222 }),
   [string]$VmKey  = $(if ($env:PS3VM_KEY)  { $env:PS3VM_KEY  } else { "$env:USERPROFILE\.ssh\ps3vm_ed25519" })
)

# stream stdout+stderr line by line so we see progress; exit code is preserved.
& ssh -o IdentitiesOnly=yes -o LogLevel=ERROR -i $VmKey -p $VmPort "$VmUser@$VmHost" `
   "X:\dev\build.bat $Kind $Name $Target $Config" 2>&1 |
   ForEach-Object { Write-Host $_ }

$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "[vmbuild] build failed (exit $code)" -ForegroundColor Red }
exit $code
