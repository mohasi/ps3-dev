<#
.SYNOPSIS
   Obtains a Google Drive refresh token for the PS3 file-manager (full "drive" scope).

.DESCRIPTION
   The console can't run a browser, and Google's device flow refuses the full Drive scope, so the
   one-time consent happens here on your PC through the usual sign-in page. The script prints the
   three lines to paste into the PS3's settings.txt. Needs a Google OAuth client of type
   "Desktop app" - see the file-manager README for how to create one.

.EXAMPLE
   .\get-gdrive-token.ps1 -ClientId XXX -ClientSecret YYY
#>
param(
   [string]$ClientId,
   [string]$ClientSecret
)

$ErrorActionPreference = 'Stop'

$scope       = 'https://www.googleapis.com/auth/drive'
$authUrl     = 'https://accounts.google.com/o/oauth2/v2/auth'
$tokenUrl    = 'https://oauth2.googleapis.com/token'
$port        = 8765
$redirectUri = "http://localhost:$port/"

if (-not $ClientId)     { $ClientId     = (Read-Host 'Google OAuth client_id').Trim() }
if (-not $ClientSecret) { $ClientSecret = (Read-Host 'Google OAuth client_secret').Trim() }
if (-not $ClientId -or -not $ClientSecret) { throw 'client_id and client_secret are required' }

# build the consent link. prompt=consent forces a refresh_token even if this account already approved once.
$state = [Guid]::NewGuid().ToString('N')
$query = @{
   client_id     = $ClientId
   redirect_uri  = $redirectUri
   response_type = 'code'
   scope         = $scope
   access_type   = 'offline'
   prompt        = 'consent'
   state         = $state
}
$pairs = $query.Keys | ForEach-Object { "$_=" + [Uri]::EscapeDataString($query[$_]) }
$authLink = $authUrl + '?' + ($pairs -join '&')

# serve exactly the one redirect Google sends back to localhost
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add($redirectUri)
$listener.Start()

Write-Host "`nOpening your browser to approve Google Drive access..."
Write-Host "If it doesn't open, visit this URL:`n$authLink`n"
Start-Process $authLink

$context  = $listener.GetContext()
$returned = @{}
foreach ($pair in $context.Request.Url.Query.TrimStart('?').Split('&')) {
   $parts = $pair -split '=', 2
   if ($parts.Count -eq 2) { $returned[$parts[0]] = [Uri]::UnescapeDataString($parts[1]) }
}
$code          = $returned['code']
$errorCode     = $returned['error']
$returnedState = $returned['state']

$message = if ($code) { 'Sign-in complete. Close this tab and return to the terminal.' } else { "Sign-in failed: $errorCode" }
$page    = [Text.Encoding]::UTF8.GetBytes("<html><body style='font-family:sans-serif'><h2>$message</h2></body></html>")
$context.Response.ContentType = 'text/html'
$context.Response.OutputStream.Write($page, 0, $page.Length)
$context.Response.Close()
$listener.Stop()

if (-not $code)                  { throw "Authorization failed: $errorCode" }
if ($returnedState -ne $state)   { throw 'State mismatch; aborting for safety.' }

# exchange the one-time code for the long-lived refresh token
$tokens = Invoke-RestMethod -Uri $tokenUrl -Method Post -ContentType 'application/x-www-form-urlencoded' -Body @{
   code          = $code
   client_id     = $ClientId
   client_secret = $ClientSecret
   redirect_uri  = $redirectUri
   grant_type    = 'authorization_code'
}

if (-not $tokens.refresh_token) { throw 'No refresh_token returned. Revoke this app at myaccount.google.com and re-run.' }

Write-Host "`n=== Paste these three lines into settings.txt on the PS3 (over FTP) ===`n"
Write-Host "google_client_id=$ClientId"
Write-Host "google_client_secret=$ClientSecret"
Write-Host "google_refresh_token=$($tokens.refresh_token)"
Write-Host "`nThen relaunch the file manager and open the Google Drive folder."
