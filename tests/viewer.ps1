param([Parameter(Mandatory)][string]$App, [Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Force $root
$script:checks = 0
function Check([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { throw $message }
}
# Offscreen only: the viewer paints off the desktop, prints one line and exits.
function Invoke-Viewer([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($App)
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.WorkingDirectory = $root
    foreach ($argument in @('--offscreen') + $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        if (-not $process.WaitForExit(20000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "viewer did not finish within 20 s: $($Arguments -join ' ')"
        }
        return @{ code = $process.ExitCode; output = $stdout.GetAwaiter().GetResult() }
    } finally { $process.Dispose() }
}
function Write-Run([string]$Name, [int]$Rows) {
    $run = Join-Path $root $Name
    $null = New-Item -ItemType Directory -Force $run
    $lines = @('time,price,position,gross,cost,equity')
    for ($i = 0; $i -lt $Rows; $i++) {
        $lines += '{0},1000.0,{1},0.0001,0.0,{2}' -f (1577836800000L + (2000 + $i) * 900000L), (($i % 3) - 1), (-0.05 - $i * 0.0001)
    }
    [IO.File]::WriteAllLines((Join-Path $run 'equity.csv'), $lines)
    return $run
}

$result = Invoke-Viewer @()
Check ($result.code -eq 0 -and $result.output -match 'cannot open data/bars/bars.bin') "viewer without bars: $($result.code) $($result.output)"

$bars = Join-Path $root 'bars.bin'
& "$PSScriptRoot/fixture.ps1" -Output $bars
$run = Write-Run 'run' 4000
$result = Invoke-Viewer @('--bars', $bars, '--run', $run)
Check ($result.code -eq 0 -and $result.output -match 'ETHUSDT  16000 bars') "viewer with bars and run: $($result.code) $($result.output)"

$result = Invoke-Viewer @('--bars', $bars, '--run', (Join-Path $root 'absent'))
Check ($result.code -eq 0 -and $result.output -match 'no equity.csv in') "viewer with a missing run: $($result.code) $($result.output)"

# A typo once made its value the bar file, and a bare --run swallowed the next option as its directory.
$result = Invoke-Viewer @('--bars', $bars, '--rn', $run)
Check ($result.code -eq 0 -and $result.output -match 'unknown option --rn  -  the viewer takes') "viewer with an unknown option: $($result.code) $($result.output)"
$result = Invoke-Viewer @('--bars', $bars, '--run')
Check ($result.code -eq 0 -and $result.output -match '--run needs a value') "viewer with a bare --run: $($result.code) $($result.output)"

# A valid header with no records, opened with a run, once read bars.front() of an empty series.
$empty = Join-Path $root 'empty.bin'
& "$PSScriptRoot/fixture.ps1" -Output $empty -Count 0
$result = Invoke-Viewer @('--bars', $empty, '--run', $run)
Check ($result.code -eq 0 -and $result.output -match 'ETHUSDT  0 bars') "viewer with empty bars and a run: $($result.code) $($result.output)"

# Non-finite prices once mapped to y = INT_MIN and walked ~2^31 pixels per column.
$bytes = [IO.File]::ReadAllBytes($bars)
$nan = [BitConverter]::GetBytes([double]::NaN)
$infinity = [BitConverter]::GetBytes([double]::PositiveInfinity)
foreach ($start in @(4000, 8000, 12000)) {
    for ($i = $start; $i -lt $start + 40; $i++) {
        foreach ($field in 1..4) {
            $value = if ($i % 2) { $nan } else { $infinity }
            [Array]::Copy($value, 0, $bytes, 35 + $i * 72 + $field * 8, 8)
        }
    }
}
$nonfinite = Join-Path $root 'nonfinite.bin'
[IO.File]::WriteAllBytes($nonfinite, $bytes)
$watch = [Diagnostics.Stopwatch]::StartNew()
$result = Invoke-Viewer @('--bars', $nonfinite, '--run', $run)
Check ($result.code -eq 0 -and $result.output -match 'ETHUSDT  16000 bars') "viewer with non-finite prices: $($result.code) $($result.output)"
Check ($watch.ElapsedMilliseconds -lt 10000) "viewer with non-finite prices took $($watch.ElapsedMilliseconds) ms"

$genericCsv = Join-Path $root 'generic.csv'
$genericBars = Join-Path $root 'generic.bin'
[IO.File]::WriteAllLines($genericCsv, @('time,open,high,low,close,session',
    '1709933400000,0.00000001,0.00000002,0.000000005,0.000000015,1',
    '1710181800000,0.000000015,0.000000025,0.00000001,0.00000002,2'))
$output = & $Cli bars --format ohlcv --in $genericCsv --out $genericBars --symbol TEST --interval 15m `
    --calendar fixture-sessions --currency USD 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0) "generic viewer input import: $output"
$genericRun = Join-Path $root 'generic-run'
$null = New-Item -ItemType Directory $genericRun
[IO.File]::WriteAllLines((Join-Path $genericRun 'equity.csv'), @('time,price,position,gross,cost,equity',
    '1710181800000,0.00000002,1,0.001,0.0005,0.0005'))
$result = Invoke-Viewer @('--bars', $genericBars, '--run', $genericRun)
Check ($result.code -eq 0 -and $result.output -match 'TEST  2 bars' -and
    $result.output -notmatch 'did not line up') "viewer session-gap timestamp alignment: $($result.output)"
Write-Output "$checks viewer offscreen checks passed"
