param([string]$OutDir = (Join-Path $PSScriptRoot ("debug_bundle_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))))
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# WHAT/WHY: this collector does not change GPU policy. It only gathers the GUI
# log plus read-only service/driver/power status so a failed transition can be
# reviewed without asking for many separate screenshots.
$guiLog = Join-Path $env:ProgramData 'NvpwrControl\nvpwr-control.log'
if (Test-Path $guiLog) { Copy-Item $guiLog (Join-Path $OutDir 'nvpwr-control.log') -Force }

cmd /c 'sc query Nvpwr' 2>&1 | Out-File (Join-Path $OutDir 'service-nvpwr.txt') -Encoding utf8
cmd /c 'sc query XMGPowerPatch' 2>&1 | Out-File (Join-Path $OutDir 'service-xmgpowerpatch.txt') -Encoding utf8
cmd /c 'bcdedit /enum {current}' 2>&1 | Out-File (Join-Path $OutDir 'bcdedit-current.txt') -Encoding utf8

$sys = Join-Path $PSScriptRoot 'dist\Nvpwr.sys'
if (Test-Path $sys) {
    Get-AuthenticodeSignature $sys | Format-List * |
        Out-File (Join-Path $OutDir 'nvpwr-signature.txt') -Encoding utf8
}


$xmgSys = Join-Path $env:ProgramData 'XMGPowerPatch\Runtime\Driver\XMGPowerPatch.sys'
if (Test-Path $xmgSys) {
    Get-AuthenticodeSignature $xmgSys | Format-List * |
        Out-File (Join-Path $OutDir 'xmgpowerpatch-signature.txt') -Encoding utf8
    Get-FileHash -Algorithm SHA256 $xmgSys | Format-List * |
        Out-File (Join-Path $OutDir 'xmgpowerpatch-sha256.txt') -Encoding utf8
}

$ctl = Join-Path $PSScriptRoot 'dist\NvpwrCtl.exe'
if (Test-Path $ctl) {
    & $ctl status 2>&1 | Out-File (Join-Path $OutDir 'nvpwr-status.txt') -Encoding utf8
}

$nvsmi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if ($nvsmi) {
    & $nvsmi.Source -q -d POWER,CLOCK 2>&1 | Out-File (Join-Path $OutDir 'nvidia-smi-power-clock.txt') -Encoding utf8
}

try { Get-PnpDevice -Class Display | Format-List * | Out-File (Join-Path $OutDir 'display-devices.txt') -Encoding utf8 } catch { $_ | Out-File (Join-Path $OutDir 'display-devices.txt') -Encoding utf8 }

$zip = "$OutDir.zip"
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zip -Force
Write-Host "Debug bundle: $zip" -ForegroundColor Green
