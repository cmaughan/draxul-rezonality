$ErrorActionPreference = 'SilentlyContinue'
$esc = [char]27
[Console]::CursorVisible = $false
$frame = 0
[Console]::Write("$esc[2J$esc[H")
$feed = [System.Collections.Generic.Queue[string]]::new()
$events = @(
    'NAV ephemeris synced',
    'SIG carrier locked',
    'OPS drones nominal',
    'ENG bottle corrected',
    'SCI anomaly classified',
    'COM packet authenticated',
    'TAC silhouette cleared',
    'SYS checksum accepted',
    'LSS atmosphere balanced',
    'EXT wake resolved'
)

while ($true) {
    $event = $events[$frame % $events.Count]
    $sector = 40 + (($frame * 17) % 59)
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $feed.Enqueue("$esc[38;2;80;130;190m$stamp$esc[0m $esc[38;2;45;235;255mS-$sector$esc[0m $event")
    while ($feed.Count -gt 13) { [void]$feed.Dequeue() }
    $carrier = 0.5 + 0.5 * [Math]::Sin($frame * 0.19)
    $spark = ('▁','▂','▃','▄','▅','▆','▇','█')[[int]($carrier * 7)]
    $header = (" $spark ORPHEUS BURST / RX 99.7%").PadRight(34)
    $renderLines = @($feed)
    while ($renderLines.Count -lt 13) { $renderLines += '' }
    $lines = $renderLines -join "`n"

    $screen = @"
$esc[38;2;255;55;180m┌──────────────────────────────────┐
│$header│
└──────────────────────────────────┘$esc[0m
$lines

$esc[38;2;120;255;120m>> CARRIER STABLE$esc[0m  $esc[38;2;45;235;255mPACKETS $($frame.ToString('000000'))$esc[0m
$esc[38;2;75;90;130m   listening beyond the heliopause...$esc[0m
$esc[38;2;255;185;45m NYX::COMMS STREAM ACTIVE$esc[0m
"@
    $clean = (($screen -split "`r?`n") | ForEach-Object { "$esc[2K$_" }) -join "`r`n"
    [Console]::Write("$esc[H$clean")
    $frame++
    Start-Sleep -Milliseconds 1000
}
