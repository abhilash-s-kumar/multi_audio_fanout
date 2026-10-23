param(
    [switch]$Clean,
    [switch]$SkipDefault,
    [switch]$InstallPythonDeps,
    [switch]$Status,
    [string]$LogPath
)

# Installs or updates the MAF virtual audio driver from maf_sysvad_package.
#
#   .\install_maf_driver.ps1           install, or update the existing MAF device in place
#   .\install_maf_driver.ps1 -Clean    remove every MAF device and driver first, then install
#   .\install_maf_driver.ps1 -Status   print the driver state as JSON (no admin needed)
#
# Exit codes: 0 done, 3010 restart Windows and run again, 4 blocked by Secure Boot, 1 failed.

$ErrorActionPreference = "Stop"

$HardwareId = "Root\sysvad_ComponentizedAudioSample"
$CertThumbprint = "C37B5196437E1423DBAC38DCC9967EE3537D606B"
$EndpointName = "MAF Virtual Audio Output"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Join-Path $root "maf_sysvad_package"
$inf = Join-Path $package "ComponentizedAudioSample.inf"
$cert = Join-Path $root "maf_signing.cer"

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-TestSigningEnabled {
    # Options Windows booted with. Readable without admin, unlike bcdedit.
    $options = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control" -ErrorAction SilentlyContinue).SystemStartOptions
    return [bool]($options -match "(?i)\bTESTSIGNING\b")
}

function Get-TestSigningPending {
    # Test-signing set for the next boot (needs admin).
    $output = & bcdedit /enum "{current}" 2>&1
    return [bool]($LASTEXITCODE -eq 0 -and ($output -match "(?im)^\s*testsigning\s+Yes\s*$"))
}

function Get-SecureBootEnabled {
    $state = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State" -ErrorAction SilentlyContinue
    return [bool]($state -and $state.UEFISecureBootEnabled -eq 1)
}

function Get-CertificateTrusted {
    return ((Test-Path "Cert:\LocalMachine\Root\$CertThumbprint") -and (Test-Path "Cert:\LocalMachine\TrustedPublisher\$CertThumbprint"))
}

function Get-PackageDriverVersion {
    if (-not (Test-Path -LiteralPath $inf)) {
        return $null
    }
    $line = Get-Content -LiteralPath $inf | Where-Object { $_ -match '^\s*DriverVer\s*=' } | Select-Object -First 1
    if ($line -match '=\s*([^,]+)\s*,\s*([\d.]+)') {
        return [pscustomobject]@{ Date = $matches[1].Trim(); Version = $matches[2].Trim() }
    }
    return $null
}

function Get-MafDevices {
    @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $HardwareId })
}

function Get-MafEndpoints {
    @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match [regex]::Escape($EndpointName) })
}

function Get-DeviceDriverVersion {
    param([string]$InstanceId)
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue
    if ($property) {
        return [string]$property.Data
    }
    return $null
}

function Get-MafStatus {
    $devices = @(Get-MafDevices)
    $endpoints = @(Get-MafEndpoints)
    $packageVersion = Get-PackageDriverVersion
    $installed = $null
    $deviceStatus = $null
    $rebootRequired = $false
    if ($devices.Count -gt 0) {
        $installed = Get-DeviceDriverVersion -InstanceId $devices[0].InstanceId
        $deviceStatus = [string]$devices[0].Status
        $reboot = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName DEVPKEY_Device_IsRebootRequired -ErrorAction SilentlyContinue
        $rebootRequired = [bool]($reboot -and $reboot.Data)
    }
    $update = $false
    if ($packageVersion -and $installed) {
        try {
            $update = ([version]$packageVersion.Version) -gt ([version]$installed)
        } catch {
            $update = $packageVersion.Version -ne $installed
        }
    }
    [pscustomobject]@{
        admin             = (Test-Admin)
        secure_boot       = (Get-SecureBootEnabled)
        test_signing      = (Get-TestSigningEnabled)
        cert_trusted      = (Get-CertificateTrusted)
        package_present   = (Test-Path -LiteralPath (Join-Path $package "TabletAudioSample.sys"))
        package_version   = if ($packageVersion) { $packageVersion.Version } else { $null }
        package_date      = if ($packageVersion) { $packageVersion.Date } else { $null }
        device_count      = $devices.Count
        device_status     = $deviceStatus
        installed_version = $installed
        reboot_required   = $rebootRequired
        endpoint_present  = ($endpoints.Count -gt 0)
        endpoint_status   = if ($endpoints.Count -gt 0) { [string]$endpoints[0].Status } else { $null }
        update_available  = $update
    }
}

function Get-DevconPath {
    $local = Join-Path $root "devcon.exe"
    if (Test-Path -LiteralPath $local) {
        return $local
    }
    $wdkDevcon = Get-ChildItem -LiteralPath (Join-Path $root "packages") -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
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

function Import-MafCertificate {
    if (Get-CertificateTrusted) {
        Write-Host "MAF test certificate is already trusted."
        return
    }
    if (-not (Test-Path -LiteralPath $cert)) {
        throw "Missing certificate: $cert"
    }
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Host "MAF test certificate imported into LocalMachine Root and TrustedPublisher."
}

function Remove-ExistingMaf {
    param([string]$Devcon)

    & $Devcon remove $HardwareId | ForEach-Object { Write-Host $_ }

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

function Set-MafDefault {
    $helperExe = Join-Path $root "set_maf_default.exe"
    if (Test-Path -LiteralPath $helperExe) {
        & $helperExe | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Driver installed, but set_maf_default.exe failed to set MAF as the default output."
        }
        return
    }

    $helper = Join-Path $root "set_maf_default.py"
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $helper) -or -not $python) {
        Write-Warning "Skipping default device setup: no set_maf_default helper or python.exe found."
        return
    }
    & $python.Source $helper | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Driver installed, but setting MAF as the default output failed."
    }
}

function Install-PythonRequirements {
    $requirements = Join-Path $root "requirements.txt"
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $requirements) -or -not $python) {
        Write-Warning "Skipping Python dependency setup (requirements.txt or python.exe missing). MAF.exe does not need Python."
        return
    }
    & $python.Source -m pip install --user -r $requirements | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Python dependency installation failed."
    }
}

function Invoke-Install {
    if (-not (Test-Admin)) {
        throw "Run this script from an elevated PowerShell window (Run as administrator)."
    }
    foreach ($file in $inf, (Join-Path $package "TabletAudioSample.sys"), (Join-Path $package "sysvad.cat")) {
        if (-not (Test-Path -LiteralPath $file)) {
            throw "Missing driver package file: $file"
        }
    }

    $devcon = Get-DevconPath
    $packageVersion = Get-PackageDriverVersion
    Write-Host "MAF driver installer"
    Write-Host "Package: $package (version $($packageVersion.Version), $($packageVersion.Date))"

    Import-MafCertificate

    if (-not (Get-TestSigningEnabled)) {
        if (Get-TestSigningPending) {
            Write-Host "Test-signing is set, but Windows has not restarted since. Restart Windows, then run this again."
            return 3010
        }
        if (Get-SecureBootEnabled) {
            Write-Host "Secure Boot is on, so Windows will not allow test-signing."
            Write-Host "Turn off Secure Boot in the BIOS/UEFI setup, start Windows, then run this again."
            return 4
        }
        Write-Host "Enabling Windows test-signing..."
        $output = & bcdedit /set testsigning on 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "Could not enable test-signing: $(($output | Out-String).Trim())"
        }
        Write-Host "Test-signing enabled. Restart Windows, then run this again to install the driver."
        return 3010
    }
    Write-Host "Windows test-signing is on."

    if ($InstallPythonDeps) {
        Install-PythonRequirements
    }

    if ($Clean) {
        Write-Host "Removing existing MAF devices and drivers..."
        Remove-ExistingMaf -Devcon $devcon
    }

    $result = 0
    $devices = @(Get-MafDevices)
    $fresh = $devices.Count -eq 0
    if ($devices.Count -gt 1) {
        Write-Warning "$($devices.Count) MAF devices found. Run with -Clean to leave only one."
    }

    if ($fresh) {
        Write-Host "Installing the MAF device..."
        & $devcon install $inf $HardwareId | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "devcon failed to install the MAF driver (exit code $LASTEXITCODE)."
        }
    } else {
        # Moves the existing device onto the new package; never adds a second device.
        Write-Host "Updating the MAF device (driver $(Get-DeviceDriverVersion -InstanceId $devices[0].InstanceId) -> $($packageVersion.Version))..."
        pnputil /add-driver $inf /install | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -eq 3010) {
            Write-Host "Windows asks for a restart to finish the update."
            $result = 3010
        } elseif ($LASTEXITCODE -ne 0) {
            throw "pnputil failed to update the MAF driver (exit code $LASTEXITCODE)."
        }
    }

    # The playback device appears a moment after the driver starts.
    $endpoints = @()
    for ($i = 0; $i -lt 20 -and $endpoints.Count -eq 0; $i++) {
        Start-Sleep -Milliseconds 500
        $endpoints = @(Get-MafEndpoints)
    }

    $devices = @(Get-MafDevices)
    if ($devices.Count -eq 0) {
        throw "The driver installed, but no MAF device is present."
    }
    $installed = Get-DeviceDriverVersion -InstanceId $devices[0].InstanceId
    Write-Host "MAF device: $($devices[0].Status), driver $installed"
    if ($installed -ne $packageVersion.Version -and $result -eq 0) {
        throw "The MAF device still uses driver $installed instead of $($packageVersion.Version). Try again with -Clean."
    }
    if ($endpoints.Count -eq 0) {
        Write-Warning "The MAF device is installed, but the '$EndpointName' playback device has not appeared yet. Restart Windows if it does not show up."
    } else {
        Write-Host "Playback device: $($endpoints[0].FriendlyName) ($($endpoints[0].Status))"
    }

    if ($fresh -and -not $SkipDefault) {
        Set-MafDefault
    }

    Write-Host "MAF driver install complete."
    return $result
}

if ($Status) {
    Get-MafStatus | ConvertTo-Json -Compress
    exit 0
}

if ($LogPath) {
    Start-Transcript -Path $LogPath -Force | Out-Null
}
$exitCode = 1
try {
    # The last value Invoke-Install returns is the exit code.
    $exitCode = [int]@(Invoke-Install)[-1]
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    $exitCode = 1
} finally {
    if ($LogPath) {
        Stop-Transcript | Out-Null
    }
}
exit $exitCode
