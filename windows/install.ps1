#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install poweroffd as a Windows service.
.DESCRIPTION
    Copies binaries to Program Files, creates config in ProgramData, and
    registers the Windows service. Run from the build output directory.
.PARAMETER Uninstall
    Remove the service and binaries.
#>

param(
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$ServiceName = "poweroffd"
$InstallDir  = "$env:ProgramFiles\poweroffd"
$ConfigDir   = "$env:ProgramData\poweroffd"
$ConfigFile  = "$ConfigDir\poweroffd.conf"

function Install-Poweroffd {
    # Check binaries exist
    $daemon = ".\poweroffd.exe"
    $client = ".\poweroff-send.exe"

    if (-not (Test-Path $daemon)) {
        Write-Error "poweroffd.exe not found in current directory. Build first with CMake."
        return
    }

    # Check service doesn't already exist
    $existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Error "Service '$ServiceName' already exists. Run with -Uninstall first."
        return
    }

    # Create directories
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    New-Item -ItemType Directory -Force -Path $ConfigDir  | Out-Null

    # Copy binaries
    Copy-Item $daemon "$InstallDir\poweroffd.exe" -Force
    if (Test-Path $client) {
        Copy-Item $client "$InstallDir\poweroff-send.exe" -Force
    }

    # Copy config (don't overwrite existing)
    if (-not (Test-Path $ConfigFile)) {
        $confSrc = ".\poweroffd.conf"
        if (-not (Test-Path $confSrc)) {
            $confSrc = "$PSScriptRoot\poweroffd.conf"
        }
        if (Test-Path $confSrc) {
            Copy-Item $confSrc $ConfigFile
        } else {
            # Write default config
            @"
# poweroffd configuration (Windows)
port = 9
bind = 0.0.0.0
# mac = AA:BB:CC:DD:EE:FF
# secret = change-me-to-something-random
delay = 5
"@ | Set-Content $ConfigFile
        }
        Write-Host "Config written to $ConfigFile"
    } else {
        Write-Host "Config already exists at $ConfigFile — not overwriting"
    }

    # Install service
    $binPath = "`"$InstallDir\poweroffd.exe`" -c `"$ConfigFile`""

    New-Service -Name $ServiceName `
        -BinaryPathName $binPath `
        -DisplayName "Power-Off Daemon (WoL-style remote shutdown)" `
        -StartupType Automatic `
        -Description "WoL-style remote shutdown daemon. Listens for magic packets and shuts down the system."

    # Recovery: restart on failure
    & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/30000

    # Add install dir to PATH for poweroff-send
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($machinePath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$machinePath;$InstallDir", "Machine")
        Write-Host "Added $InstallDir to system PATH"
    }

    # Configure Windows Firewall
    $ruleName = "poweroffd (UDP-in)"
    $existing = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    if (-not $existing) {
        New-NetFirewallRule -DisplayName $ruleName `
            -Direction Inbound -Protocol UDP -LocalPort 9 `
            -Action Allow -Profile Any `
            -Description "Allow incoming WoL magic packets for poweroffd" | Out-Null
        Write-Host "Firewall rule '$ruleName' created (UDP port 9)"
    }

    Write-Host ""
    Write-Host "poweroffd installed successfully!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:"
    Write-Host "  1. Edit $ConfigFile"
    Write-Host "     Set your MAC address and HMAC secret"
    Write-Host "  2. net start $ServiceName"
    Write-Host ""
    Write-Host "To send a shutdown packet from another machine:"
    Write-Host "  poweroff-send <target-ip> <mac> [secret] [port]"
}

function Uninstall-Poweroffd {
    # Stop service if running
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc) {
        if ($svc.Status -eq "Running") {
            Write-Host "Stopping service..."
            Stop-Service -Name $ServiceName -Force
            Start-Sleep -Seconds 2
        }
        & sc.exe delete $ServiceName
        Write-Host "Service '$ServiceName' removed."
    } else {
        Write-Host "Service '$ServiceName' not found."
    }

    # Remove firewall rule
    $ruleName = "poweroffd (UDP-in)"
    Remove-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue

    # Remove binaries (keep config)
    if (Test-Path $InstallDir) {
        Remove-Item -Recurse -Force $InstallDir
        Write-Host "Removed $InstallDir"
    }

    # Remove from PATH
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($machinePath -like "*$InstallDir*") {
        $newPath = ($machinePath -split ";" | Where-Object { $_ -ne $InstallDir }) -join ";"
        [Environment]::SetEnvironmentVariable("Path", $newPath, "Machine")
        Write-Host "Removed $InstallDir from system PATH"
    }

    Write-Host ""
    Write-Host "poweroffd uninstalled." -ForegroundColor Yellow
    Write-Host "Config preserved at $ConfigFile (delete manually if not needed)"
}

# Main
if ($Uninstall) {
    Uninstall-Poweroffd
} else {
    Install-Poweroffd
}
