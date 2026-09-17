param([ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'

# WHERE/WHY: the original project used only one vswhere query. On some Visual
# Studio 18/Preview installations that query returns nothing even though MSBuild
# is installed. Keep vswhere as the first choice, then fall back to the known
# VS layout and finally a constrained recursive search.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}
if (-not $vsRoot) {
    $knownRoots = @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\18\BuildTools",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools"
    )
    $vsRoot = $knownRoots | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vsRoot) { throw 'Visual Studio C++ build tools not found.' }

$cl = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Filter "cl.exe" -Recurse | Where-Object { $_.FullName -match "bin\\Hostx64\\x64\\cl\.exe$" } | Select-Object -First 1 -ExpandProperty FullName
$link = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Filter "link.exe" -Recurse | Where-Object { $_.FullName -match "bin\\Hostx64\\x64\\link\.exe$" } | Select-Object -First 1 -ExpandProperty FullName
$msvcToolsDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $cl)))
$msvcLib = Join-Path $msvcToolsDir "lib\x64"
$msvcInc = Join-Path $msvcToolsDir "include"

$pkg = "$env:USERPROFILE\.nuget\packages"
$wdkKmInc = "$pkg\microsoft.windows.wdk.x64\10.0.28000.2526\c\Include\10.0.28000.0\km"
$wdkKmCrt = "$pkg\microsoft.windows.wdk.x64\10.0.28000.2526\c\Include\10.0.28000.0\km\crt"
$wdkKmLib = "$pkg\microsoft.windows.wdk.x64\10.0.28000.2526\c\Lib\10.0.28000.0\km\x64"
$sdkUmInc = "$pkg\microsoft.windows.sdk.cpp\10.0.28000.1721\c\Include\10.0.28000.0\um"
$sdkSharedInc = "$pkg\microsoft.windows.sdk.cpp\10.0.28000.1721\c\Include\10.0.28000.0\shared"
$sdkUcrtInc = "$pkg\microsoft.windows.sdk.cpp\10.0.28000.1721\c\Include\10.0.28000.0\ucrt"
$sdkUmLib = "$pkg\microsoft.windows.sdk.cpp.x64\10.0.28000.1721\c\um\x64"
$sdkUcrtLib = "$pkg\microsoft.windows.sdk.cpp.x64\10.0.28000.1721\c\ucrt\x64"

$signtool = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter "signtool.exe" -Recurse | Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } | Select-Object -First 1 -ExpandProperty FullName

Write-Host "Compiler: $cl" -ForegroundColor Cyan

# 1. Driver Build
Write-Host 'Building Nvpwr driver...' -ForegroundColor Yellow
$driverOut = "$PSScriptRoot\driver\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $driverOut | Out-Null
& $cl /c /nologo /W4 /Ox /D _WIN64 /D _AMD64_ /D AMD64 /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000012 /kernel `
  /I "$wdkKmInc" /I "$wdkKmCrt" /I "$sdkSharedInc" /I "$sdkUcrtInc" /I "$PSScriptRoot\shared" `
  /Fo"$driverOut\driver.obj" "$PSScriptRoot\driver\driver.c"
if ($LASTEXITCODE -ne 0) { throw 'Driver compilation failed.' }

& $link /nologo /WX /SECTION:"INIT,d" `
  "$wdkKmLib\Aux_Klib.lib" `
  "$wdkKmLib\BufferOverflowFastFailK.lib" `
  "$wdkKmLib\ntoskrnl.lib" `
  "$wdkKmLib\hal.lib" `
  "$wdkKmLib\wmilib.lib" `
  /NODEFAULTLIB /MANIFEST:NO /DEBUG /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" `
  /RELEASE /MERGE:"_TEXT=.text;_PAGE=PAGE" /MACHINE:X64 /kernel `
  /OUT:"$driverOut\Nvpwr.sys" "$driverOut\driver.obj"
if ($LASTEXITCODE -ne 0) { throw 'Driver linking failed.' }

# Sign driver with local test certificate
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -match "Nvpwr Local Test Cert" } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Nvpwr Local Test Cert" -CertStoreLocation "Cert:\CurrentUser\My"
}
Export-Certificate -Cert $cert -FilePath "$PSScriptRoot\driver\Nvpwr.cer" -Force | Out-Null
if ($signtool) {
    & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /v "$driverOut\Nvpwr.sys"
}

# 2. GUI Build
Write-Host 'Building GUI...' -ForegroundColor Yellow
$appOut = "$PSScriptRoot\app\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $appOut | Out-Null
$appInc = @(
  "/I", "$sdkUmInc",
  "/I", "$sdkSharedInc",
  "/I", "$sdkUcrtInc",
  "/I", "$msvcInc",
  "/I", "$PSScriptRoot\shared"
)
& $cl /c /nologo /W4 /O2 /std:c++17 /utf-8 /EHsc $appInc /Fo"$appOut\" `
  "$PSScriptRoot\app\main.cpp" "$PSScriptRoot\app\xmg_probe.cpp" "$PSScriptRoot\app\nvapi_probe.cpp" "$PSScriptRoot\app\nvapi_tuner.cpp"
if ($LASTEXITCODE -ne 0) { throw 'GUI compilation failed.' }

& $link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Comctl32.lib Advapi32.lib User32.lib Gdi32.lib Kernel32.lib `
  /OUT:"$appOut\NvpwrControl.exe" "$appOut\main.obj" "$appOut\xmg_probe.obj" "$appOut\nvapi_probe.obj" "$appOut\nvapi_tuner.obj"
if ($LASTEXITCODE -ne 0) { throw 'GUI linking failed.' }

# 3. CLI Build
Write-Host 'Building CLI fallback...' -ForegroundColor Yellow
$cliOut = "$PSScriptRoot\cli\x64\$Configuration"
New-Item -ItemType Directory -Force -Path $cliOut | Out-Null
& $cl /c /nologo /W4 /O2 /std:c++17 /utf-8 /EHsc $appInc /Fo"$cliOut\nvpwrctl.obj" "$PSScriptRoot\cli\nvpwrctl.cpp"
if ($LASTEXITCODE -ne 0) { throw 'CLI compilation failed.' }

& $link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /RELEASE /OPT:REF /OPT:ICF `
  /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" `
  /LIBPATH:"$msvcLib" /LIBPATH:"$sdkUmLib" /LIBPATH:"$sdkUcrtLib" `
  Advapi32.lib Kernel32.lib `
  /OUT:"$cliOut\NvpwrCtl.exe" "$cliOut\nvpwrctl.obj"
if ($LASTEXITCODE -ne 0) { throw 'CLI linking failed.' }

# Distribute
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item "$driverOut\Nvpwr.sys" $dist -Force
Copy-Item "$PSScriptRoot\driver\Nvpwr.cer" $dist -Force
Copy-Item "$appOut\NvpwrControl.exe" $dist -Force
Copy-Item "$cliOut\NvpwrCtl.exe" $dist -Force
Copy-Item "$PSScriptRoot\restart-nvidia-device.ps1" $dist -Force
Copy-Item "$PSScriptRoot\collect-debug.ps1" $dist -Force
Copy-Item "$driverOut\Nvpwr.sys" $appOut -Force

Write-Host ''
Write-Host 'Build complete. Run dist\NvpwrControl.exe as administrator.' -ForegroundColor Green
Write-Host 'GUI log: C:\ProgramData\NvpwrControl\nvpwr-control.log' -ForegroundColor Cyan
Get-ChildItem $dist | Select-Object Name,Length,FullName
