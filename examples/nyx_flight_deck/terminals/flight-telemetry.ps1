$ErrorActionPreference = 'SilentlyContinue'
$esc = [char]27
[Console]::CursorVisible = $false
$frame = 0
[Console]::Write("$esc[2J$esc[H")

function Bar([double]$value, [int]$width = 22) {
    $filled = [Math]::Clamp([int]($value * $width), 0, $width)
    return ('█' * $filled) + ('░' * ($width - $filled))
}

while ($true) {
    $t = $frame / 9.0
    $reactor = 0.73 + 0.19 * [Math]::Sin($t * 0.71)
    $shield = 0.82 + 0.11 * [Math]::Sin($t * 0.43 + 1.8)
    $flux = 0.56 + 0.31 * [Math]::Sin($t * 1.13 + 0.4)
    $velocity = 18240 + [int](2460 * [Math]::Sin($t * 0.29))
    $drift = 0.003 + 0.002 * [Math]::Abs([Math]::Sin($t * 0.61))
    $phase = 17.0 + 6.0 * [Math]::Sin($t * 0.37)
    $pulse = @('/','-','\','|')[$frame % 4]
    $header = (" NYX FLIGHT / LINK:LIVE $pulse").PadRight(36)

    $screen = @"
$esc[38;2;45;235;255m╔════════════════════════════════════╗
║$header║
╠════════════════════════════════════╣$esc[0m
$esc[38;2;255;55;180m REACTOR$esc[0m  $(Bar $reactor) $([int]($reactor*100))%
$esc[38;2;45;220;255m SHIELDS$esc[0m  $(Bar $shield) $([int]($shield*100))%
$esc[38;2;120;255;120m Q-FLUX $esc[0m  $(Bar $flux) $([int]($flux*100))%

$esc[38;2;110;155;255m VELOCITY$esc[0m   $velocity km/s
$esc[38;2;110;155;255m PHASE LOCK$esc[0m $($phase.ToString('00.000'))°
$esc[38;2;110;155;255m VECTOR D $esc[0m  $($drift.ToString('0.00000')) AU
$esc[38;2;110;155;255m GRAVITY   $esc[0m  0.9987 g

$esc[38;2;255;185;45m NAV SOLUTION / KESTREL-9$esc[0m
 AXIS A  $(([Math]::Sin($t)*14.0).ToString('+00.000;-00.000'))
 AXIS B  $(([Math]::Cos($t*.7)*9.0).ToString('+00.000;-00.000'))
 AXIS C  $(([Math]::Sin($t*.3+2)*3.0).ToString('+00.000;-00.000'))

$esc[38;2;45;235;255m NYX::TELEMETRY ONLINE // FRAME $($frame.ToString('000000'))$esc[0m
"@
    $clean = (($screen -split "`r?`n") | ForEach-Object { "$esc[2K$_" }) -join "`r`n"
    [Console]::Write("$esc[H$clean")
    $frame++
    Start-Sleep -Milliseconds 500
}
