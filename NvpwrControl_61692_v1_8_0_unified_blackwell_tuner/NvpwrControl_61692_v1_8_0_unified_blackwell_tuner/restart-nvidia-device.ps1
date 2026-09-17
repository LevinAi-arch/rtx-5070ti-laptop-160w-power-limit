param([switch]$Yes)
$ErrorActionPreference = 'Stop'

# Supported Windows PnP reset path. This does NOT bypass driver signing and it
# does NOT reload Nvpwr.sys. The display may go black while the NVIDIA device is
# disabled/enabled. Runtime NVIDIA tuning state is expected to reset.

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell.'
}

$ctl = Join-Path $PSScriptRoot 'dist\NvpwrCtl.exe'
if(Test-Path $ctl) {
    Write-Host 'Restoring Nvpwr OEM state first...' -ForegroundColor Cyan
    & $ctl restore
    if($LASTEXITCODE -ne 0) { throw 'Nvpwr OEM restore failed. NVIDIA PnP restart refused.' }
}

$devices = @(Get-PnpDevice -Class Display -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_10DE*' -and $_.Status -ne 'Unknown'
})
if($devices.Count -ne 1) {
    throw "Expected exactly one present NVIDIA display device, found $($devices.Count)."
}
$gpu = $devices[0]
Write-Host "NVIDIA device: $($gpu.FriendlyName)" -ForegroundColor Cyan
Write-Host "Instance ID:   $($gpu.InstanceId)"
Write-Host 'The screen can go black for a few seconds.' -ForegroundColor Yellow

if(-not $Yes) {
    $answer = Read-Host 'Type RESTART to continue'
    if($answer -cne 'RESTART') { Write-Host 'Cancelled.'; exit 1 }
}

Disable-PnpDevice -InstanceId $gpu.InstanceId -Confirm:$false
Start-Sleep -Seconds 2
Enable-PnpDevice -InstanceId $gpu.InstanceId -Confirm:$false
Start-Sleep -Seconds 3

$after = Get-PnpDevice -InstanceId $gpu.InstanceId
Write-Host "NVIDIA device state after restart: $($after.Status)" -ForegroundColor Green
