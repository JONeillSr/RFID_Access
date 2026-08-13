<#
    export_gerbers.ps1 - regenerate fab outputs from the SAVED board.

    Reads RFID_Door_Controller.kicad_pcb as-is (respects manual edits),
    runs DRC, then writes Gerbers + Excellon drill into fab/ and rebuilds
    the upload zip. It NEVER runs gen_board.py, so hand edits are safe.

    Usage (from anywhere):
        powershell -ExecutionPolicy Bypass -File export_gerbers.ps1
    or, if your policy already allows scripts:
        .\export_gerbers.ps1
    Add -SkipDrc to export even if DRC finds violations (not recommended).
#>
param([switch]$SkipDrc)

$ErrorActionPreference = "Stop"
$cli  = Join-Path $env:ProgramFiles "KiCad\10.0\bin\kicad-cli.exe"
$proj = Join-Path (Split-Path $PSScriptRoot -Parent) "RFID_Door_Controller"
$pcb  = Join-Path $proj "RFID_Door_Controller.kicad_pcb"
$fab  = Join-Path $proj "fab"
$zip  = Join-Path $proj "RFID_Door_Controller_gerbers.zip"

if (-not (Test-Path $cli)) { throw "kicad-cli not found at $cli" }
if (-not (Test-Path $pcb)) { throw "board not found at $pcb" }

Write-Host "DRC..." -ForegroundColor Cyan
& $cli pcb drc --severity-error --exit-code-violations `
    -o (Join-Path $proj "drc.rpt") $pcb | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "DRC found violations - see drc.rpt" -ForegroundColor Red
    if (-not $SkipDrc) { throw "Fix DRC first, or re-run with -SkipDrc to override." }
    Write-Host "Continuing anyway (-SkipDrc)." -ForegroundColor Yellow
} else {
    Write-Host "DRC clean." -ForegroundColor Green
}

New-Item -ItemType Directory -Force $fab | Out-Null
Get-ChildItem $fab -File | Remove-Item -Force

Write-Host "Exporting Gerbers + drill..." -ForegroundColor Cyan
& $cli pcb export gerbers -o "$fab\" `
    --layers "F.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,Edge.Cuts,F.Paste,B.Paste" $pcb | Out-Null
& $cli pcb export drill -o "$fab\" --format excellon `
    --generate-map --map-format gerberx2 $pcb | Out-Null

Compress-Archive -Path (Join-Path $fab '*') -DestinationPath $zip -Force
Write-Host "Done. Wrote $zip" -ForegroundColor Green
