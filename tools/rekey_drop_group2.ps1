param(
  [string]$ReadyFile,
  [string]$ResultFile
)

$ErrorActionPreference = "Stop"
try {
  Disable-NetAdapter -Name "WLAN" -Confirm:$false
  Set-Content -LiteralPath $ReadyFile -Value "disabled" -Encoding ascii
  Start-Sleep -Seconds 8
  Enable-NetAdapter -Name "WLAN" -Confirm:$false
  Set-Content -LiteralPath $ResultFile -Value "ok" -Encoding ascii
}
catch {
  Set-Content -LiteralPath $ResultFile -Value $_.Exception.Message -Encoding utf8
  try { Enable-NetAdapter -Name "WLAN" -Confirm:$false } catch {}
  exit 1
}
