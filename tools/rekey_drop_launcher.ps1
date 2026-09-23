$toolDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ready = Join-Path $toolDir "rekey-drop.ready"
$result = Join-Path $toolDir "rekey-drop.result"
$script = Join-Path $toolDir "rekey_drop_group2.ps1"

Remove-Item -LiteralPath $ready, $result -Force -ErrorAction SilentlyContinue
$arguments = @(
  "-NoProfile",
  "-ExecutionPolicy", "Bypass",
  "-File", $script,
  "-ReadyFile", $ready,
  "-ResultFile", $result
)
Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments

$deadline = (Get-Date).AddSeconds(30)
while (!(Test-Path -LiteralPath $ready) -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 100
}

if (!(Test-Path -LiteralPath $ready)) {
  throw "elevated script did not become ready"
}

adb shell k7flash aptest status
$deadline = (Get-Date).AddSeconds(15)
while (!(Test-Path -LiteralPath $result) -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 200
}

if (Test-Path -LiteralPath $result) {
  Get-Content -LiteralPath $result
}

Start-Sleep -Seconds 3
adb devices
adb shell k7flash aptest status
netsh wlan show interfaces
