<#
    backup-doors.ps1 - snapshot the NVS-backed state of one or more RFID doors.

    Captures the state that lives only on the device and cannot be rebuilt from
    this source tree, so it survives a flash erase, a partition-table change, or
    a swapped board:

        /api/list      enrolled fobs (uid + name)   <- the one that matters
        /api/schedule  unlock window (enabled, start/end minutes, days mask)
        /status.txt    device id, door/site labels, board, reader counters

    Writes one timestamped folder per run, one file per door per endpoint.

    SECURITY: the output contains fob credential numbers. Anyone holding them can
    clone a working card. Keep backups off public storage and out of this repo --
    .gitignore excludes "door-backup-*/" for exactly that reason. Prefer an
    -OutDir outside the working tree.

    WiFi credentials are deliberately NOT captured: no endpoint exposes them (by
    design). A full erase therefore still costs a captive-portal re-provision.

    Usage (from anywhere):
        powershell -ExecutionPolicy Bypass -File tools\backup-doors.ps1 jtc-main.local
        .\tools\backup-doors.ps1 jtc-main.local rfid-a1b2c3.local -OutDir D:\backups
        .\tools\backup-doors.ps1 192.168.1.50            # IP works too

    If a ".local" name will not resolve (some Windows and Android clients cannot
    do mDNS), use the IP shown on the device's OLED.
#>
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$DoorHosts,
    [string]$OutDir = "."
)

$stamp = Get-Date -Format "yyyy-MM-dd_HHmmss"
$dest  = Join-Path $OutDir "door-backup-$stamp"
New-Item -ItemType Directory -Force $dest | Out-Null
Write-Host "Writing to $dest" -ForegroundColor Cyan

$failures = 0

foreach ($h in $DoorHosts) {
    $safe = $h -replace '[^A-Za-z0-9\.\-]', '_'
    Write-Host "`n=== $h ===" -ForegroundColor Cyan

    # Enrolled fobs. The only state here that cannot be recreated from source.
    try {
        $list = Invoke-RestMethod -Uri "http://$h/api/list" -TimeoutSec 10
        $n = @($list.entries).Count
        $list | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $dest "$safe-fobs.json")
        Write-Host ("  fobs      : {0} enrolled" -f $n) -ForegroundColor Green
        foreach ($e in $list.entries) { Write-Host ("              {0}  {1}" -f $e.uid, $e.name) }
        if ($n -eq 0) {
            Write-Host "  WARNING: zero fobs returned - verify before trusting this backup" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  fobs      : FAILED - $($_.Exception.Message)" -ForegroundColor Red
        $failures++
    }

    # Unlock schedule: also NVS-backed, also lost on erase, and easy to forget
    # because it is invisible until the window opens.
    try {
        $sch = Invoke-RestMethod -Uri "http://$h/api/schedule" -TimeoutSec 10
        $sch | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $dest "$safe-schedule.json")
        Write-Host ("  schedule  : enabled={0} {1:d2}:{2:d2}-{3:d2}:{4:d2} days=0x{5:X2}" -f `
            $sch.enabled, [int]($sch.start / 60), [int]($sch.start % 60), `
            [int]($sch.end / 60), [int]($sch.end % 60), $sch.days) -ForegroundColor Green
    } catch {
        Write-Host "  schedule  : FAILED - $($_.Exception.Message)" -ForegroundColor Red
        $failures++
    }

    # Human-readable reference: identity, door/site labels, board, counters.
    try {
        $st = Invoke-WebRequest -Uri "http://$h/status.txt" -TimeoutSec 10
        $st.Content | Set-Content -Encoding UTF8 (Join-Path $dest "$safe-status.txt")
        Write-Host "  status    : saved" -ForegroundColor Green
    } catch {
        Write-Host "  status    : FAILED - $($_.Exception.Message)" -ForegroundColor Red
        $failures++
    }
}

Write-Host "`nDone. Files in $dest" -ForegroundColor Cyan
Get-ChildItem $dest | Select-Object Name, Length | Format-Table -AutoSize

if ($failures -gt 0) {
    Write-Host "$failures request(s) failed - backup is INCOMPLETE." -ForegroundColor Red
    exit 1
}
