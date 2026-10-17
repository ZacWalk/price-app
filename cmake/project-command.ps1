#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$request = [Console]::In.ReadLine() | ConvertFrom-Json -AsHashtable
$root = Split-Path $PSScriptRoot
if ($request.schema -ne 1 -or $request.projectRoot -ne $root -or $request.parameters -isnot [hashtable] -or
    $request.dryRun -isnot [bool] -or $request.command -notin @('data', 'bars', 'experiment', 'app', 'all')) {
    throw 'Invalid project-command request.'
}
Set-Location $root
$p = $request.parameters
$config = if ($p.config) { $p.config } else { 'release' }
$suffix = if ($config -eq 'debug') { '-d' } else { '' }
$ext = if ($IsWindows) { '.exe' } else { '' }
$cli = Join-Path $root "exe/cli$suffix$ext"
$app = Join-Path $root "exe/app$suffix$ext"
$symbol = if ($p.symbol) { $p.symbol } else { 'ETHUSDT' }
$interval = if ($p.interval) { $p.interval } else { '10s' }
$bars = "data/bars/$symbol-$interval.bin"
$extra = @()
if ($p.args) {
    $extra = ConvertFrom-Json -InputObject $p.args -NoEnumerate
    if ($extra -isnot [array] -or @($extra | Where-Object { $_ -isnot [string] }).Count) {
        throw '--args must be a JSON string array; shell expressions are never evaluated.'
    }
}
$plan = [Collections.Generic.List[object]]::new()
function Add-Step([string]$binary, [string[]]$arguments, [bool]$experiment = $false, [bool]$launch = $false) {
    $plan.Add(@{ binary = $binary; arguments = $arguments; experiment = $experiment
        mode = $(if ($launch) { 'launch' } else { 'run' }) })
}
function Data-Step {
    if ($p.ContainsKey('fixtures') -and [string]::IsNullOrEmpty([string]$p.fixtures)) {
        throw '--fixtures requires a non-empty directory'
    }
    if ($p.ContainsKey('max-rows') -and
        (($p['max-rows'] -isnot [int] -and $p['max-rows'] -isnot [long]) -or $p['max-rows'] -le 0)) {
        throw '--max-rows must be a positive integer'
    }
    $arguments = @('download', '--symbol', $symbol)
    foreach ($key in @('market', 'kind', 'period', 'source', 'from', 'to', 'out', 'fixtures', 'max-rows')) {
        if ($p.ContainsKey($key)) { $arguments += @("--$key", [string]$p[$key]) }
    }
    foreach ($key in @('force', 'skip-missing')) { if ($p[$key]) { $arguments += "--$key" } }
    Add-Step $cli $arguments
}
function Bars-Step {
    if ($p.format -eq 'ohlcv') {
        foreach ($key in @('symbol', 'interval', 'calendar', 'currency')) {
            if (-not $p.ContainsKey($key) -or [string]::IsNullOrWhiteSpace([string]$p[$key])) {
                throw "--format ohlcv requires --$key"
            }
        }
    }
    $input = if ($p.in) { $p.in } else { 'data/raw' }
    $output = if ($p.out -and $request.command -eq 'bars') { $p.out } else { $bars }
    $arguments = @('bars', '--in', $input, '--out', $output, '--interval', $interval, '--symbol', $symbol)
    foreach ($key in @('format', 'calendar', 'currency', 'source-name', 'price-basis')) {
        if ($p.ContainsKey($key)) { $arguments += @("--$key", [string]$p[$key]) }
    }
    Add-Step $cli $arguments
}
function Experiment-Step {
    $arguments = @($extra)
    if ($arguments -notcontains '--bars') {
        if (-not (Test-Path $bars) -and $request.command -ne 'all') { Bars-Step }
        $arguments = @('--bars', $bars) + $arguments
    }
    Add-Step $cli (@('walk') + $arguments) $true
}
function App-Step {
    if (-not $IsWindows) { return }
    $arguments = if ($request.command -eq 'app') { @($extra) } else { @() }
    if (-not $arguments.Count) {
        if (Test-Path $bars) { $arguments += @('--bars', $bars) }
        $newest = Get-ChildItem runs -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'equity.csv') } |
            Sort-Object LastWriteTime | Select-Object -Last 1
        if ($newest) { $arguments += @('--run', $newest.FullName) }
    }
    if (-not $p.show -and $arguments -notcontains '--offscreen') { $arguments += '--offscreen' }
    Add-Step $app $arguments $false ($p.show -and $arguments -notcontains '--offscreen')
}
switch ($request.command) {
    data { Data-Step }
    bars { Bars-Step }
    experiment { Experiment-Step }
    app { App-Step }
    all { Data-Step; Bars-Step; Experiment-Step; App-Step }
}
$results = @()
$controlFailed = $false
if (-not $request.dryRun) {
    if ($request.command -eq 'all') {
        $buildOutput = & pwsh -NoProfile -File (Join-Path $root 'dd.ps1') build $config --non-interactive 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOutput" }
    }
    foreach ($step in $plan) {
        if (-not (Test-Path $step.binary)) {
            $buildOutput = & pwsh -NoProfile -File (Join-Path $root 'dd.ps1') build $config --non-interactive 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOutput" }
        }
        $arguments = $step.arguments
        if ($request.command -eq 'all' -and $step.binary -eq $app) {
            $arguments = @('--offscreen', '--bars', $bars)
            $newest = Get-ChildItem runs -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName 'equity.csv') } |
                Sort-Object LastWriteTime | Select-Object -Last 1
            if ($newest) { $arguments += @('--run', $newest.FullName) }
        }
        if ($step.mode -eq 'launch') {
            . (Join-Path $root '.dd/core.ps1')
            . (Join-Path $root '.dd/execution.ps1')
            $results += @{ exitCode = 0; launch = (Start-DDApplication $step.binary $arguments $root) }
            continue
        }
        $output = & $step.binary @arguments 2>&1 | Out-String
        $code = $LASTEXITCODE
        $results += @{ exitCode = $code; output = $output }
        if ($step.experiment -and $code -eq 1) { $controlFailed = $true; continue }
        if ($code -ne 0) {
            [Console]::Error.WriteLine($output)
            exit $code
        }
    }
}
@{ schema = 1; data = @{ plan = @($plan); results = $results }; files = @() } |
    ConvertTo-Json -Depth 15 -Compress | Write-Output
if ($controlFailed) {
    [Console]::Error.WriteLine('Shuffled control beat the real experiment; inspect the run artifacts. This is not a successful edge.')
    exit 1
}
