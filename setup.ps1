#Requires -RunAsAdministrator
# One-liner: irm https://raw.githubusercontent.com/kasaiarashi/poweroffd/main/setup.ps1 | iex

param(
    [string]$Version = "latest"
)

$ErrorActionPreference = "Stop"

$Repo       = "kasaiarashi/poweroffd"
$ServiceName = "poweroffd"
$InstallDir  = "$env:ProgramFiles\poweroffd"
$ConfigDir   = "$env:ProgramData\poweroffd"
$ConfigFile  = "$ConfigDir\poweroffd.conf"

Write-Host "==> poweroffd Windows installer" -ForegroundColor Cyan

# Resolve version
if ($Version -eq "latest") {
    Write-Host "==> Fetching latest release..."
    $release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest"
    $Version = $release.tag_name -replace '^v', ''
}

if (-not $Version) {
    Write-Error "Could not determine latest version. Usage: setup.ps1 [-Version 1.0.0]"
}

Write-Host "==> Version: $Version"

# Download zip
$ZipName = "poweroffd_${Version}_win-x64.zip"
$Url     = "https://github.com/$Repo/releases/download/v${Version}/$ZipName"
$TmpDir  = Join-Path $env:TEMP "poweroffd-setup"
$ZipPath = Join-Path $TmpDir $ZipName

try {
    # Prep temp dir
    if (Test-Path $TmpDir) { Remove-Item -Recurse -Force $TmpDir }
    New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

    Write-Host "==> Downloading $ZipName..."
    try {
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
    } catch {
        Write-Host ""
        Write-Host "Error: Failed to download $Url" -ForegroundColor Red
        Write-Host ""
        Write-Host "No prebuilt binary for your platform?" -ForegroundColor Yellow
        Write-Host "Build from source instead:"
        Write-Host "  git clone https://github.com/$Repo.git"
        Write-Host "  cd poweroffd\windows"
        Write-Host "  build.bat"
        Write-Host "  powershell -ExecutionPolicy Bypass -File install.ps1"
        exit 1
    }

    # Extract
    Write-Host "==> Extracting..."
    Expand-Archive -Path $ZipPath -DestinationPath $TmpDir -Force

    # Stop existing service if running
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq "Running") {
        Write-Host "==> Stopping existing service..."
        Stop-Service -Name $ServiceName -Force
        Start-Sleep -Seconds 2
    }

    # Create directories
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    New-Item -ItemType Directory -Force -Path $ConfigDir  | Out-Null

    # Copy binaries
    Copy-Item "$TmpDir\poweroffd.exe"     "$InstallDir\poweroffd.exe"     -Force
    Copy-Item "$TmpDir\poweroff-send.exe" "$InstallDir\poweroff-send.exe" -Force -ErrorAction SilentlyContinue

    # Config (don't overwrite existing)
    if (-not (Test-Path $ConfigFile)) {
        if (Test-Path "$TmpDir\poweroffd.conf") {
            Copy-Item "$TmpDir\poweroffd.conf" $ConfigFile
        } else {
            @"
# poweroffd configuration (Windows)
port = 9
bind = 0.0.0.0
# mac = AA:BB:CC:DD:EE:FF
# secret = change-me-to-something-random
delay = 5
"@ | Set-Content $ConfigFile
        }
        Write-Host "==> Config written to $ConfigFile"
    } else {
        Write-Host "==> Config already exists — not overwriting"
    }

    # Install/update service
    $binPath = "`"$InstallDir\poweroffd.exe`" -c `"$ConfigFile`""

    if (-not $svc) {
        & sc.exe create $ServiceName binPath= $binPath start= auto DisplayName= "Power-Off Daemon (WoL-style remote shutdown)"
        & sc.exe description $ServiceName "WoL-style remote shutdown daemon. Listens for magic packets and shuts down the system."
        & sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/30000
    } else {
        & sc.exe config $ServiceName binPath= $binPath
    }

    # Add to PATH
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($machinePath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$machinePath;$InstallDir", "Machine")
        Write-Host "==> Added $InstallDir to system PATH"
    }

    # Firewall rule
    $ruleName = "poweroffd (UDP-in)"
    if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $ruleName `
            -Direction Inbound -Protocol UDP -LocalPort 9 `
            -Action Allow -Profile Any `
            -Description "Allow incoming WoL magic packets for poweroffd" | Out-Null
        Write-Host "==> Firewall rule created (UDP port 9)"
    }

    Write-Host ""
    Write-Host "==> Done!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:"
    Write-Host "  1. Edit $ConfigFile"
    Write-Host "     Set your MAC address and HMAC secret"
    Write-Host "  2. net start $ServiceName"
    Write-Host ""

} finally {
    # Cleanup
    if (Test-Path $TmpDir) { Remove-Item -Recurse -Force $TmpDir }
}
