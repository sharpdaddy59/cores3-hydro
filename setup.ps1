# setup.ps1 — one-time arduino-cli configuration for cores3-hydro.
#
# Idempotent — safe to re-run. Installs:
#   - arduino-cli itself (via winget) if missing
#   - The M5Stack arduino-esp32 board package (m5stack:esp32 core)
#   - Required libraries (M5CoreS3, M5Unified, ArduinoJson, WiFiManager,
#     DallasTemperature, OneWire)
#
# Run once after cloning the project, or whenever you start fresh on a
# new machine. After this completes, .\build.ps1 is the everyday entry point.

$ErrorActionPreference = 'Stop'

$Sketchbook = 'C:\Users\gregl\Documents\Arduino'
$BoardUrl   = 'https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json'

function Have-Cmd($name) {
    $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# arduino-cli itself
# ---------------------------------------------------------------------------
if (-not (Have-Cmd 'arduino-cli')) {
    Write-Host '[setup] arduino-cli not found; installing via winget...'
    winget install --id ArduinoSA.CLI -e --accept-source-agreements --accept-package-agreements
    if (-not (Have-Cmd 'arduino-cli')) {
        Write-Host '[setup] winget installed arduino-cli, but it is not on PATH for this shell.'
        Write-Host '[setup] Open a NEW PowerShell window and re-run setup.ps1.'
        exit 1
    }
} else {
    Write-Host ('[setup] arduino-cli found: ' + ((arduino-cli version) -join ' '))
}

# ---------------------------------------------------------------------------
# Config (sketchbook + M5Stack board manager URL)
# ---------------------------------------------------------------------------
Write-Host '[setup] Initializing arduino-cli config (no-op if it already exists)...'
arduino-cli config init 2>$null | Out-Null

Write-Host "[setup] Pointing sketchbook at $Sketchbook"
arduino-cli config set directories.user "$Sketchbook"

Write-Host '[setup] Adding M5Stack board manager URL...'
arduino-cli config add board_manager.additional_urls $BoardUrl

# ---------------------------------------------------------------------------
# Cores
# ---------------------------------------------------------------------------
Write-Host '[setup] Updating package index...'
arduino-cli core update-index

Write-Host '[setup] Installing m5stack:esp32 core (M5Stack''s arduino-esp32 fork)...'
arduino-cli core install m5stack:esp32

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
$libs = @(
    'M5CoreS3'               # Pulls in M5Unified + M5GFX as dependencies
    'M5Unified'              # Pinned explicitly in case the M5CoreS3 dep fails
    'ArduinoJson'
    'WiFiManager'
    'DallasTemperature'      # Pulls in OneWire as a dependency
    'OneWire'
)
foreach ($lib in $libs) {
    Write-Host "[setup] Installing library: $lib"
    arduino-cli lib install "$lib"
}

# ---------------------------------------------------------------------------
# Sanity check
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[setup] Verifying CoreS3 board is recognized...'
arduino-cli board listall m5stack | Select-String -Pattern 'cores3' -CaseSensitive:$false

Write-Host ''
Write-Host '[setup] Done. Try a build with:    .\build.ps1'
Write-Host '[setup] Plug in your CoreS3 then:  .\build.ps1 -Upload -Monitor'
