# build.ps1 — arduino-cli build/upload/monitor wrapper for cores3-hydro.
#
# This project uses M5Stack's arduino-esp32 fork via arduino-cli rather than
# PlatformIO's mainstream arduino-esp32 because the M5Stack fork has the
# correct PSRAM init, AW9523 GPIO expander handling, and other CoreS3
# board-specific tweaks. PIO never paid off for this device.
#
# Examples:
#   .\build.ps1                                 # compile prod env (default)
#   .\build.ps1 -Env no-display                 # compile without display task
#   .\build.ps1 -Strict                         # warnings=all
#   .\build.ps1 -Upload                         # compile + auto-detect + upload
#   .\build.ps1 -Upload -Port COM8              # compile + upload to specific port
#   .\build.ps1 -Upload -Monitor                # upload then attach serial monitor
#
# To put a sensor in simulation mode at runtime (no rebuild needed):
#   Invoke-WebRequest -Method POST "http://cores3-hydro.local/sim?air=on"
#
# Pre-reqs (run setup.ps1 once):
#   - arduino-cli on PATH
#   - m5stack:esp32 core installed
#   - M5CoreS3 + dependent libraries installed

[CmdletBinding()]
param(
    [ValidateSet("prod", "no-display")]
    [string]$Env = "prod",
    [string]$Port = "",
    [switch]$Upload,
    [switch]$Monitor,
    [switch]$Strict
)

$Fqbn      = "m5stack:esp32:m5stack_cores3"
$SketchDir = $PSScriptRoot

# Per-env compile-time defines. Sensor simulation is now a runtime concern
# (per-sensor flags toggled via POST /sim, persisted in NVS) — there's no
# "sim build" anymore. Only "no-display" remains as a true compile-time env
# because removing the display task is a memory-footprint decision.
$EnvFlags = @{
    "prod"       = ""
    "no-display" = "-DDISABLE_DISPLAY=1"
}
$ExtraFlags = $EnvFlags[$Env]

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
$warn = if ($Strict) { "all" } else { "default" }
Write-Host "[build] Compiling for env=$Env (FQBN: $Fqbn, warnings=$warn)"

$compileArgs = @(
    "compile"
    "--fqbn"; $Fqbn
    "--warnings"; $warn
)
if ($ExtraFlags) {
    # Use compiler.cpp.extra_flags (empty by default, appends to the
    # build) rather than build.extra_flags (which would REPLACE the board
    # manifest's defaults like ARDUINO_M5STACK_CORES3, ARDUINO_USB_CDC_ON_BOOT,
    # BOARD_HAS_PSRAM and break the build).
    $compileArgs += "--build-property"
    $compileArgs += "compiler.cpp.extra_flags=$ExtraFlags"
}
$compileArgs += $SketchDir

& arduino-cli @compileArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $Upload) {
    Write-Host "[build] Compile OK."
    return
}

# ---------------------------------------------------------------------------
# Upload (with auto-port-detect)
# ---------------------------------------------------------------------------
if (-not $Port) {
    Write-Host "[build] Auto-detecting CoreS3 port..."
    $raw = arduino-cli board list --format json | ConvertFrom-Json
    $boards = if ($raw.detected_ports) { $raw.detected_ports } else { $raw }

    $candidates = @()
    foreach ($b in $boards) {
        if ($b.port.protocol -ne "serial") { continue }
        $fqbns = @()
        if ($b.matching_boards) { $fqbns = @($b.matching_boards.fqbn) }

        $score = 0
        if ($fqbns -contains $Fqbn)                              { $score = 3 }
        elseif ($fqbns | Where-Object { $_ -like "m5stack:*" })  { $score = 2 }
        elseif ($fqbns | Where-Object { $_ -like "esp32:*"   })  { $score = 1 }
        if ($score -gt 0) {
            $candidates += [pscustomobject]@{
                Port  = $b.port.address
                Score = $score
                Fqbns = $fqbns
            }
        }
    }
    if (-not $candidates) {
        Write-Host "[build] No ESP32-family serial port detected."
        Write-Host "[build] Plug in the CoreS3 or pass -Port COMx explicitly."
        exit 1
    }
    $best = $candidates | Sort-Object -Property Score -Descending | Select-Object -First 1
    $Port = $best.Port
    if ($best.Score -lt 3) {
        Write-Host "[build] Found ESP32-family device on $Port (FQBN: $($best.Fqbns -join ', '))."
        Write-Host "[build] Not an exact CoreS3 match, but proceeding."
    } else {
        Write-Host "[build] Found CoreS3 on $Port"
    }
}

Write-Host "[build] Uploading to $Port..."
arduino-cli upload -p $Port --fqbn $Fqbn $SketchDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Monitor) {
    Write-Host "[build] Opening serial monitor on $Port at 115200. Ctrl+C to exit."
    arduino-cli monitor -p $Port -c baudrate=115200
}
