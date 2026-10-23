param(
    [switch]$NoSign,
    [string]$CertThumbprint = "C37B5196437E1423DBAC38DCC9967EE3537D606B"
)

# Builds the MAF virtual audio driver (SysVAD TabletAudioSample with the MAF loopback mixer)
# and refreshes maf_sysvad_package: TabletAudioSample.sys, a re-stamped INF and a signed catalog.
#
# The kernel-mode platform toolset is not installed, so this compiles every source file with
# one set of driver flags against the WDK/SDK NuGet packages in .\packages and links the .sys
# directly. Needs Visual Studio 2022 (Build Tools is enough) with the C++ x64 tools.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sysvad = Join-Path $root "Windows-driver-samples\audio\sysvad"
$kitVersion = "10.0.28000.0"
$wdk = Join-Path $root "packages\Microsoft.Windows.WDK.x64.10.0.28000.1839\c"
$sdk = Join-Path $root "packages\Microsoft.Windows.SDK.CPP.10.0.28000.1721\c"
$out = Join-Path $root "build\driver"
$package = Join-Path $root "maf_sysvad_package"

function Import-VcEnvironment {
    $vcvars = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat", "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $vcvars) {
        throw "Visual Studio 2022 C++ x64 tools not found (vcvars64.bat)."
    }
    # Only this process's environment changes.
    cmd /c "`"$($vcvars.FullName)`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$([IO.Path]::GetFileName($Exe)) failed with exit code $LASTEXITCODE."
    }
}

foreach ($path in $sysvad, $wdk, $sdk, $package) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing: $path"
    }
}

Import-VcEnvironment
New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem -LiteralPath $out -Include *.obj, *.res, *.sys, *.pdb -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force

$defines = @(
    "_NO_CRT_STDIO_INLINE", "_AMD64_", "AMD64", "_WIN64", "_WIN32", "UNICODE", "_UNICODE",
    "PC_IMPLEMENTATION", "DEBUG_LEVEL=DEBUGLVL_TERSE", "_USE_WAVERT_", "SYSVAD_BTH_BYPASS",
    "SYSVAD_USB_SIDEBAND", "_USE_IPortClsRuntimePower", "_NEW_DELETE_OPERATORS_",
    "KMDF_VERSION_MAJOR=1", "KMDF_VERSION_MINOR=35"
) | ForEach-Object { "/D$_" }

$kitIncludes = @(
    "$wdk\Include\wdf\kmdf\1.35",
    "$wdk\Include\$kitVersion\km",
    "$wdk\Include\$kitVersion\shared",
    "$sdk\Include\$kitVersion\shared",
    "$sdk\Include\$kitVersion\um"
) | ForEach-Object { "/I$_" }

$compileFlags = @(
    "/c", "/nologo", "/kernel", "/Zi", "/W3", "/O2", "/Gm-", "/GS-", "/GR-", "/Gd", "/TP",
    "/fp:precise", "/Zc:wchar_t", "/Zc:forScope", "/Zc:inline", "/Zc:threadSafeInit-",
    "/std:c++17", "/wd4324", "/wd4595", "/FC", "/Fo$out\", "/Fd$out\vc143.pdb"
) + $defines + $kitIncludes

# Same sources and include order as the TabletAudioSample and EndpointsCommon projects.
$driverSources = @(
    "A2dpHpDevice.cpp", "adapter.cpp", "basetopo.cpp", "BthhfpDevice.cpp", "common.cpp", "hw.cpp",
    "kshelper.cpp", "savedata.cpp", "tonegenerator.cpp", "UsbHsDevice.cpp"
) | ForEach-Object { Join-Path $sysvad $_ }
$driverSources += @("hdmitopo.cpp", "micintopo.cpp", "spdiftopo.cpp") | ForEach-Object { Join-Path $sysvad "TabletAudioSample\$_" }
$commonSources = @(
    "a2dphpminwavert.cpp", "a2dphpspeakertopo.cpp", "a2dphptopo.cpp", "AudioModuleHelper.cpp",
    "bthhfpmictopo.cpp", "bthhfpminwavert.cpp", "bthhfpspeakertopo.cpp", "bthhfptopo.cpp",
    "micarraytopo.cpp", "MiniportAudioEngineNode.cpp", "MiniportStreamAudioEngineNode.cpp",
    "mintopo.cpp", "minwavert.cpp", "minwavertstream.cpp", "NewDelete.cpp", "speakerhptopo.cpp",
    "speakertopo.cpp", "usbhsminwavert.cpp", "usbhsmictopo.cpp", "usbhsspeakertopo.cpp", "usbhstopo.cpp"
) | ForEach-Object { Join-Path $sysvad "EndpointsCommon\$_" }

Write-Host "Compiling driver sources..."
Invoke-Tool "cl.exe" ($compileFlags + @("/I$sysvad", "/I$sysvad\EndpointsCommon", "/I$sysvad\TabletAudioSample") + $driverSources)
Write-Host "Compiling endpoint sources..."
Invoke-Tool "cl.exe" ($compileFlags + @("/I$sysvad", "/I$sysvad\EndpointsCommon") + $commonSources)

Write-Host "Compiling resources..."
$rc = Join-Path $sdk "bin\$kitVersion\x64\rc.exe"
Invoke-Tool $rc (@("/nologo", "/l0x0409", "/fo$out\TabletAudioSample.res") + $defines + $kitIncludes +
    @("/I$sysvad", "/I$sysvad\EndpointsCommon", (Join-Path $sysvad "TabletAudioSample\TabletAudioSample.rc")))

Write-Host "Linking TabletAudioSample.sys..."
$kmLib = "$wdk\Lib\$kitVersion\km\x64"
$kmdfLib = "$wdk\Lib\wdf\kmdf\x64\1.35"
$libs = @(
    "$kmLib\ntoskrnl.lib", "$kmLib\hal.lib", "$kmLib\wmilib.lib", "$kmLib\portcls.lib", "$kmLib\stdunk.lib",
    "$kmLib\libcntpr.lib", "$kmLib\bufferoverflowfastfailk.lib", "$kmLib\ntstrsafe.lib",
    "$kmdfLib\WdfLdr.lib", "$kmdfLib\WdfDriverEntry.lib"
)
$objects = Get-ChildItem -LiteralPath $out -Filter *.obj | ForEach-Object { $_.FullName }
Invoke-Tool "link.exe" (@(
    "/NOLOGO", "/OUT:$out\TabletAudioSample.sys", "/PDB:$out\TabletAudioSample.pdb", "/DEBUG:FULL",
    "/NODEFAULTLIB", "/MANIFEST:NO", "/Driver", "/ENTRY:FxDriverEntry", "/SUBSYSTEM:NATIVE",
    "/MACHINE:X64", "/DYNAMICBASE", "/NXCOMPAT", "/RELEASE", "/OPT:REF", "/OPT:ICF",
    "$out\TabletAudioSample.res"
) + $objects + $libs)

Write-Host "Updating maf_sysvad_package..."
$inf = Join-Path $package "ComponentizedAudioSample.inf"
$now = Get-Date
$driverVer = "{0},15.{1}.{2}.{3}" -f $now.ToString("MM/dd/yyyy", [Globalization.CultureInfo]::InvariantCulture), $now.Year, [int]$now.ToString("MMdd"), [int]$now.ToString("HHmm")
$infText = [IO.File]::ReadAllText($inf)
if ($infText -notmatch '(?m)^DriverVer\s*=') {
    throw "DriverVer line not found in $inf"
}
$infText = [regex]::Replace($infText, '(?m)^DriverVer\s*=.*$', "DriverVer = $driverVer")
[IO.File]::WriteAllText($inf, $infText, [Text.Encoding]::Unicode)
Copy-Item -LiteralPath "$out\TabletAudioSample.sys" -Destination $package -Force
Remove-Item -LiteralPath (Join-Path $package "sysvad.cat") -ErrorAction SilentlyContinue

$signArgs = $null
if (-not $NoSign) {
    $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My | Where-Object { $_.Thumbprint -eq $CertThumbprint -and $_.HasPrivateKey } | Select-Object -First 1
    if (-not $cert) {
        throw "Signing certificate $CertThumbprint with its private key was not found. Use -NoSign to skip signing."
    }
    $signtool = Join-Path $sdk "bin\$kitVersion\x64\signtool.exe"
    $signArgs = @("sign", "/fd", "sha256", "/sha1", $CertThumbprint, "/s", "My")
    if ($cert.PSParentPath -match "LocalMachine") {
        $signArgs += "/sm"
    }
    # The .sys is signed before the catalog is made, because the catalog hashes the signed file.
    Invoke-Tool $signtool ($signArgs + @("$package\TabletAudioSample.sys"))
}

$inf2cat = Join-Path $wdk "bin\$kitVersion\x86\Inf2Cat.exe"
Invoke-Tool $inf2cat @("/driver:$package", "/os:10_X64")

if ($signArgs) {
    Invoke-Tool $signtool ($signArgs + @("$package\sysvad.cat"))
}

Write-Host ""
Write-Host "Driver package ready: $package (DriverVer $driverVer)"
Write-Host "Install it with install_maf_driver.ps1 (as Administrator) or from MAF > Driver."
