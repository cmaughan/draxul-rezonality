param(
    [string]$DraxulPath = '',
    [string]$SpaceId = '',
    [string]$Session = '',
    [string]$ServerRuntimeDir = '',
    [string]$TabName = 'NYX // FLIGHT DECK',
    [switch]$Crt
)

$ErrorActionPreference = 'Stop'

$demoRoot = $PSScriptRoot
$rezonalityRoot = (Resolve-Path (Join-Path $demoRoot '../..')).Path
$repoRoot = (Resolve-Path (Join-Path $rezonalityRoot '../..')).Path
$shaderProject = (Resolve-Path (Join-Path $demoRoot 'shaders')).Path.Replace('\', '/')
$robotAssetPath = Join-Path $rezonalityRoot 'examples/robot2/models/robot/scene.gltf'
if (-not (Test-Path -LiteralPath $robotAssetPath)) {
    throw 'The Rezonality robot2 example is missing. Run: git submodule update --init plugins/rezonality'
}
$robotProject = (Resolve-Path (Join-Path $demoRoot 'robot-crt')).Path.Replace('\', '/')

function Resolve-DraxulExecutable {
    if ($DraxulPath) {
        return (Resolve-Path -LiteralPath $DraxulPath).Path
    }

    $candidates = @(
        (Join-Path $repoRoot 'build-ninja-release/draxul.exe'),
        (Join-Path $repoRoot 'build-ninja-debug/draxul.exe'),
        (Join-Path $repoRoot 'build/Release/draxul.exe'),
        (Join-Path $repoRoot 'build/Debug/draxul.exe'),
        (Join-Path $repoRoot 'build/draxul.app/Contents/MacOS/draxul')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    $onPath = Get-Command draxul -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'Could not find draxul. Build Draxul or pass -DraxulPath.'
}

$draxul = Resolve-DraxulExecutable
$routeArguments = @()
if ($Session) { $routeArguments += @('--session', $Session) }
if ($ServerRuntimeDir) { $routeArguments += @('--server-runtime-dir', $ServerRuntimeDir) }

function Invoke-DraxulJson {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $allArguments = @($Arguments) + @($script:routeArguments)
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $script:draxul
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $allArguments) { $start.ArgumentList.Add([string]$argument) }
    $process = [System.Diagnostics.Process]::Start($start)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Draxul command timed out: draxul $($Arguments -join ' ')"
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        throw "Draxul command failed ($($process.ExitCode)): $stderr"
    }
    try {
        return ($stdout | ConvertFrom-Json)
    }
    catch {
        throw "Draxul returned invalid JSON for: draxul $($Arguments -join ' ')"
    }
}

if (-not $SpaceId) { $SpaceId = $env:DRAXUL_SPACE_ID }
if (-not $SpaceId) {
    $spaceResult = Invoke-DraxulJson @('space', 'list', '--json')
    $spaces = @($spaceResult)
    if ($spaces.Count -ne 1) {
        throw 'Pass -SpaceId when the target session does not contain exactly one Space.'
    }
    $SpaceId = $spaces[0].space_id
}

$space = Invoke-DraxulJson @('space', 'get', $SpaceId, '--json')
$resolvedSpaceId = if ($space.space_id) { $space.space_id } else { $space.id }
if ($resolvedSpaceId -ne $SpaceId) {
    throw "Space '$SpaceId' was not found."
}

$runTag = 'nyx-' + (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') `
    + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6)

function New-RezonalityConfig {
    param(
        [Parameter(Mandatory = $true)][string]$Project,
        [Parameter(Mandatory = $true)][string]$Scenegraph,
        [Parameter(Mandatory = $true)][int]$Index
    )

    if ($script:Crt) {
        if ($Scenegraph -eq 'default.scenegraph') {
            $Scenegraph = 'crt.scenegraph'
        }
        else {
            $Scenegraph = $Scenegraph -replace '\.scenegraph$', '-crt.scenegraph'
        }
    }

    return ([ordered]@{
        auto_reload = $true
        compile_debounce_ms = 120
        diagnostics_id = "$script:runTag-$Index"
        paused = $false
        project_path = $Project
        scenegraph = $Scenegraph
    } | ConvertTo-Json -Compress)
}

function Rename-Pane {
    param([string]$PaneId, [string]$Name)
    Invoke-DraxulJson @('pane', 'rename', $PaneId, '--name', $Name, '--json') | Out-Null
}

function Add-PluginPane {
    param(
        [string]$FromPane,
        [string]$Direction,
        [double]$Ratio,
        [string]$Name,
        [string]$Project,
        [string]$Scenegraph,
        [int]$Index
    )

    $config = New-RezonalityConfig -Project $Project -Scenegraph $Scenegraph -Index $Index
    $result = Invoke-DraxulJson @(
        'pane', 'split', $FromPane,
        '--direction', $Direction,
        '--ratio', $Ratio.ToString([Globalization.CultureInfo]::InvariantCulture),
        '--plugin', 'dev.draxul.rezonality',
        '--plugin-config', $config,
        '--json'
    )
    Rename-Pane -PaneId $result.created_id -Name $Name
    return $result.created_id
}

function Add-TerminalPane {
    param(
        [string]$FromPane,
        [string]$Direction,
        [double]$Ratio,
        [string]$Name
    )

    $result = Invoke-DraxulJson @(
        'pane', 'split', $FromPane,
        '--direction', $Direction,
        '--ratio', $Ratio.ToString([Globalization.CultureInfo]::InvariantCulture),
        '--cwd', $demoRoot,
        '--json'
    )
    Rename-Pane -PaneId $result.created_id -Name $Name
    return $result.created_id
}

$tabId = ''
try {
    $reactorConfig = New-RezonalityConfig -Project $shaderProject -Scenegraph 'panel-0.scenegraph' -Index 0
    $createdTab = Invoke-DraxulJson @(
        'tab', 'create', '--space', $SpaceId, '--name', $TabName,
        '--plugin', 'dev.draxul.rezonality', '--plugin-config', $reactorConfig, '--json'
    )
    $tabId = $createdTab.created_id
    $tab = Invoke-DraxulJson @('tab', 'get', $tabId, '--json')
    $reactor = @($tab.panes)[0].pane_id
    Rename-Pane -PaneId $reactor -Name 'REACTOR CROWN'

    # Build five equal-width top cells by repeatedly splitting the remaining
    # right-hand region: 1/5, then 1/4, 1/3, and 1/2.
    $telemetry = Add-TerminalPane -FromPane $reactor -Direction right -Ratio 0.2 -Name 'FLIGHT TELEMETRY'
    $quantum = Add-PluginPane -FromPane $telemetry -Direction right -Ratio 0.25 -Name 'QUANTUM POLYHEDRON' -Project $shaderProject -Scenegraph 'panel-2.scenegraph' -Index 2
    $gyro = Add-PluginPane -FromPane $quantum -Direction right -Ratio (1.0 / 3.0) -Name 'CELESTIAL GYROSCOPE' -Project $shaderProject -Scenegraph 'panel-3.scenegraph' -Index 3
    $comms = Add-TerminalPane -FromPane $gyro -Direction right -Ratio 0.5 -Name 'ORPHEUS COMMS'

    # Split each column into equal top and bottom cells.
    $sentinel = Add-PluginPane -FromPane $reactor -Direction down -Ratio 0.5 -Name 'NEON SENTINEL // GLTF' -Project $robotProject -Scenegraph 'default.scenegraph' -Index 1
    $shield = Add-PluginPane -FromPane $telemetry -Direction down -Ratio 0.5 -Name 'SHIELD HARMONICS' -Project $shaderProject -Scenegraph 'panel-6.scenegraph' -Index 6
    $target = Add-PluginPane -FromPane $quantum -Direction down -Ratio 0.5 -Name 'TARGET LOCK' -Project $shaderProject -Scenegraph 'panel-7.scenegraph' -Index 7
    $plasma = Add-PluginPane -FromPane $gyro -Direction down -Ratio 0.5 -Name 'PLASMA WAKE' -Project $shaderProject -Scenegraph 'panel-8.scenegraph' -Index 8
    $event = Add-PluginPane -FromPane $comms -Direction down -Ratio 0.5 -Name 'EVENT GATE' -Project $shaderProject -Scenegraph 'panel-9.scenegraph' -Index 9

    $flightScript = (Resolve-Path (Join-Path $demoRoot 'terminals/flight-telemetry.ps1')).Path.Replace("'", "''")
    $commsScript = (Resolve-Path (Join-Path $demoRoot 'terminals/comms-stream.ps1')).Path.Replace("'", "''")
    $flightCommand = "pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File '$flightScript'"
    $commsCommand = "pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File '$commsScript'"
    Invoke-DraxulJson @('pane', 'run', $telemetry, '--command', $flightCommand, '--json') | Out-Null
    Invoke-DraxulJson @('pane', 'run', $comms, '--command', $commsCommand, '--json') | Out-Null
    Invoke-DraxulJson @('pane', 'wait-output', $telemetry, '--text', 'NYX::TELEMETRY ONLINE', '--timeout', '10s', '--json') | Out-Null
    Invoke-DraxulJson @('pane', 'wait-output', $comms, '--text', 'NYX::COMMS STREAM ACTIVE', '--timeout', '10s', '--json') | Out-Null

    $final = Invoke-DraxulJson @('tab', 'get', $tabId, '--json')
    [pscustomobject]@{
        ok = $true
        space_id = $SpaceId
        tab_id = $tabId
        tab_name = $TabName
        pane_count = @($final.panes).Count
        telemetry_pane = $telemetry
        comms_pane = $comms
        gltf_pane = $sentinel
    } | ConvertTo-Json
}
catch {
    if ($tabId) {
        Write-Warning "Launch stopped after creating tab $tabId; the partial tab was retained for inspection."
    }
    throw
}
