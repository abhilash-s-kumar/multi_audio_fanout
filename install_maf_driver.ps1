param(
    [switch]$Clean,
    [switch]$SkipDefault,
    [switch]$InstallPythonDeps
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this script from an elevated PowerShell window."
    }
}

function Get-TestSigningEnabled {
    $output = & bcdedit /enum 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query bcdedit. Output: $output"
    }

    return ($output -match "(?im)^\s*testsigning\s+Yes\s*$")
}

function Enable-TestSigning {
    $output = & bcdedit /set testsigning on 2>&1
    if ($LASTEXITCODE -ne 0) {
        $joined = ($output | Out-String).Trim()
        throw @"
Could not enable test-signing.

If this says the value is protected by Secure Boot policy, disable Secure Boot in BIOS/UEFI, boot Windows, then run this installer again.

bcdedit output:
$joined
"@
    }
}

function Import-MafCertificate {
    param([string]$CertificatePath)

    if (-not (Test-Path -LiteralPath $CertificatePath)) {
        throw "Missing certificate: $CertificatePath"
    }

    Import-Certificate -FilePath $CertificatePath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $CertificatePath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

function Get-DevconPath {
    param([string]$Root)

    $local = Join-Path $Root "devcon.exe"
    if (Test-Path -LiteralPath $local) {
        return $local
    }

    $wdkDevcon = Get-ChildItem -LiteralPath (Join-Path $Root "packages") -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\devcon\.exe$" } |
        Select-Object -First 1

    if ($wdkDevcon) {
        return $wdkDevcon.FullName
    }

    $pathDevcon = Get-Command devcon.exe -ErrorAction SilentlyContinue
    if ($pathDevcon) {
        return $pathDevcon.Source
    }

    throw "Could not find devcon.exe. Put devcon.exe beside this script or keep the local WDK package folder."
}

function Remove-ExistingMaf {
    param([string]$Devcon)

    & $Devcon remove "Root\sysvad_ComponentizedAudioSample" | ForEach-Object { Write-Host $_ }

    $drivers = pnputil /enum-drivers /class MEDIA
    $published = $null
    $original = $null
    $provider = $null
    $toDelete = New-Object System.Collections.Generic.List[string]

    foreach ($line in $drivers) {
        if ($line -match "^\s*Published Name:\s+(.+?)\s*$") {
            $published = $matches[1]
            $original = $null
            $provider = $null
            continue
        }
        if ($line -match "^\s*Original Name:\s+(.+?)\s*$") {
            $original = $matches[1]
            continue
        }
        if ($line -match "^\s*Provider Name:\s+(.+?)\s*$") {
            $provider = $matches[1]
        }

        if ($published -and $original -eq "componentizedaudiosample.inf" -and ($provider -eq "MAF" -or $provider -eq "TODO-Set-Provider")) {
            $toDelete.Add($published)
            $published = $null
        }
    }

    foreach ($driver in $toDelete | Select-Object -Unique) {
        pnputil /delete-driver $driver /uninstall /force | ForEach-Object { Write-Host $_ }
    }
}

function Install-MafDriver {
    param(
        [string]$InfPath,
        [string]$Devcon
    )

    if (-not (Test-Path -LiteralPath $InfPath)) {
        throw "Missing driver INF: $InfPath"
    }

    & $Devcon install $InfPath "Root\sysvad_ComponentizedAudioSample" | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "devcon failed to install the MAF driver."
    }
}

function Set-MafDefault {
    param([string]$Root)

    $helperExe = Join-Path $Root "set_maf_default.exe"
    if (Test-Path -LiteralPath $helperExe) {
        & $helperExe
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Driver installed, but set_maf_default.exe failed to set MAF as the default output."
        }
        return
    }

    $helper = Join-Path $Root "set_maf_default.py"
    if (-not (Test-Path -LiteralPath $helper)) {
        Write-Warning "Skipping default device setup because no set_maf_default helper was found."
        return
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        Write-Warning "Skipping default device setup because python.exe was not found."
        return
    }

    & $python.Source $helper
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Driver installed, but setting MAF as the default output failed. Run set_maf_default.py after installing Python dependencies."
    }
}

function Install-PythonRequirements {
    param([string]$Root)

    $requirements = Join-Path $Root "requirements.txt"
    if (-not (Test-Path -LiteralPath $requirements)) {
        Write-Warning "Skipping Python dependency setup because requirements.txt is missing."
        return
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        Write-Warning "Skipping Python dependency setup because python.exe was not found. MAF.exe can still run without Python."
        return
    }

    & $python.Source -m pip install --user -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Python dependency installation failed."
    }
}

Assert-Admin

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Join-Path $root "maf_sysvad_package"
$inf = Join-Path $package "ComponentizedAudioSample.inf"
$cert = Join-Path $root "maf_signing.cer"
$devcon = Get-DevconPath -Root $root

Write-Host "MAF driver installer"
Write-Host "Root: $root"
Write-Host "Driver INF: $inf"
Write-Host "DevCon: $devcon"

Import-MafCertificate -CertificatePath $cert
Write-Host "MAF test certificate imported into LocalMachine Root and TrustedPublisher."

if (-not (Get-TestSigningEnabled)) {
    Write-Host "Windows test-signing is not enabled. Enabling it now..."
    Enable-TestSigning
    Write-Host ""
    Write-Host "Test-signing was enabled. Reboot Windows, then run this script again to install the driver."
    exit 3010
}

Write-Host "Windows test-signing is enabled."

if ($InstallPythonDeps) {
    Write-Host "Installing Python dependencies for source-script usage..."
    Install-PythonRequirements -Root $root
}

if ($Clean) {
    Write-Host "Cleaning existing MAF driver/device entries..."
    Remove-ExistingMaf -Devcon $devcon
}

Install-MafDriver -InfPath $inf -Devcon $devcon

Start-Sleep -Seconds 2
$mafDevices = Get-PnpDevice | Where-Object { $_.FriendlyName -match "MAF Virtual Audio Output" }
if (-not $mafDevices) {
    throw "Driver install completed, but no MAF PnP device or endpoint was found."
}

$mafDevices | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize

if (-not $SkipDefault) {
    Set-MafDefault -Root $root
}

Write-Host "MAF driver install complete."
