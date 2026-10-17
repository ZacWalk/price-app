param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Force $root
$culture = [Globalization.CultureInfo]::InvariantCulture
$script:checks = 0
function Check([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { throw $message }
}
function Invoke-Cli([string[]]$Arguments, [int[]]$Codes = @(0)) {
    $output = & $Cli @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -notin $Codes) { throw "cli $($Arguments -join ' '): exit $LASTEXITCODE`n$output" }
    return $output
}
$csv = Join-Path $root 'prices.csv'
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('time,open,high,low,close,session')
$previous = 100.0
$start = [DateTimeOffset]::Parse('2020-01-02T14:30:00Z', $culture)
for ($i = 0; $i -lt 7000; $i++) {
    $day = [int][Math]::Floor($i / 26)
    $time = $start.AddDays($day + 2 * [Math]::Floor($day / 5)).AddMinutes(15 * ($i % 26)).ToUnixTimeMilliseconds()
    $close = 100.0 + 0.001 * $i + 2 * [Math]::Sin($i * 0.017) + 0.5 * [Math]::Sin($i * 0.31)
    $values = @($time.ToString($culture), $previous.ToString('R', $culture),
        ([Math]::Max($previous, $close) + 0.05).ToString('R', $culture),
        ([Math]::Min($previous, $close) - 0.05).ToString('R', $culture),
        $close.ToString('R', $culture), ($day + 1).ToString($culture))
    $lines.Add($values -join ',')
    $previous = $close
}
[IO.File]::WriteAllLines($csv, $lines)
$bars = Join-Path $root 'prices.bin'
$import = @('bars', '--format', 'ohlcv', '--in', $csv, '--out', $bars, '--symbol', 'TEST',
    '--interval', '15m', '--calendar', 'fixture-sessions', '--currency', 'USD', '--source-name', 'synthetic')
$output = Invoke-Cli $import
Check ($output -match '7000 bars' -and $output -match 'SPQB-v2') "generic import: $output"
$bytes = [IO.File]::ReadAllBytes($bars)
Check ([BitConverter]::ToUInt32($bytes, 4) -eq 2) 'generic bars are not explicitly versioned'
$hash = (Get-FileHash $bars).Hash
$output = Invoke-Cli $import @(2)
Check ($output -match 'refusing to overwrite' -and (Get-FileHash $bars).Hash -ceq $hash) 'generic import replaced an existing bar file'
$utc = Join-Path $root 'utc.csv'
$utcBars = Join-Path $root 'utc.bin'
[IO.File]::WriteAllLines($utc, @('close,low,high,open,time,volume',
    '100,99,101,100,1577975400000,0', '101,100,102,100,1577976300000,12.5'))
$output = Invoke-Cli @('bars', '--format', 'ohlcv', '--in', $utc, '--out', $utcBars,
    '--symbol', 'OTHER', '--interval', '15m', '--calendar', 'utc', '--currency', 'EUR')
Check ($output -match '2 bars' -and $output -match 'volume available') 'UTC OHLCV with reordered columns was not imported'
$output = Invoke-Cli @('audit', '--bars', $bars)
Check ($output -match '11 features' -and $output -match 'shift audit passed') "price-only audit: $output"
$output = Invoke-Cli @('patterns', '--bars', $bars, '--shuffles', '20', '--block', '26')
Check ($output -match 'UNAVAILABLE' -and $output -match 'previous session') "generic pattern capabilities: $output"
$output = Invoke-Cli @('patterns', '--bars', $bars) @(2)
Check ($output -match '--block') 'generic patterns guessed a 24-hour block'

$bad = Join-Path $root 'bad.csv'
$badOut = Join-Path $root 'bad.bin'
$badImport = @('bars', '--format', 'ohlcv', '--in', $bad, '--out', $badOut, '--symbol', 'TEST',
    '--interval', '15m', '--calendar', 'fixture-sessions', '--currency', 'USD')
foreach ($rows in @(
    @('time,open,high,low,close,session', '1577975400000,100,101,99,NaN,1'),
    @('time,open,high,low,close,session', '1577975400000,100,101,99,0,1'),
    @('time,open,high,low,close,session', '1577975400000,100,99,101,100,1'),
    @('time,open,high,low,close,session', '1577975400000x,100,101,99,100,1'),
    @('time,open,high,low,close,session', '1577975400000,100,101,99,100,1', '1577975400000,100,101,99,100,1'),
    @('time,open,high,low,close,session', '1577975400000,100,101,99,100,2', '1577976300000,100,101,99,100,1'),
    @('time,open,high,low,close,volume,session', '1577975400000,100,101,99,100,,1'),
    @('time,open,high,low,close', '1577975400000,100,101,99,100'),
    @('time,open,high,low,close,session', '1577975400000,100,101,99,100,1', '1577975400900,100,101,99,100,1')
)) {
    [IO.File]::WriteAllLines($bad, $rows)
    $output = Invoke-Cli $badImport @(2)
    Check (-not (Test-Path $badOut)) "rejected CSV published a bar file: $output"
}

$walk = @('walk', '--bars', $bars, '--seeds', '2', '--seed', '7', '--train', '1500',
    '--valid', '200', '--gap', '300', '--test', '600', '--episode', '500', '--epochs', '1',
    '--hidden', '8,8', '--batch', '8', '--target-sync', '50')
foreach ($name in @('flat', 'long', 'short', 'random', 'crossover', 'breakout', 'qtable', 'dqn')) {
    $first = Join-Path $root "$name-first"
    $second = Join-Path $root "$name-second"
    $walked = Invoke-Cli ($walk + @('--agent', $name, '--out', $first)) @(0, 1)
    $null = Invoke-Cli ($walk + @('--agent', $name, '--out', $second)) @(0, 1)
    $a = Get-ChildItem $first -Directory | Select-Object -First 1
    $b = Get-ChildItem $second -Directory | Select-Object -First 1
    foreach ($file in @('metrics.csv', 'equity.csv')) {
        Check ((Get-FileHash (Join-Path $a.FullName $file)).Hash -ceq (Get-FileHash (Join-Path $b.FullName $file)).Hash) "$name $file does not reproduce"
    }
    $manifest = Get-Content (Join-Path $a.FullName 'manifest.txt') -Raw
    Check ($manifest -match 'feature_profile=ohlc-v1' -and $manifest -match 'calendar=fixture-sessions' -and
        $manifest -match 'annualisation=scored-elapsed-time' -and $manifest -match 'prediction=short-flat-long') "$name metadata incomplete"
    $metrics = Import-Csv (Join-Path $a.FullName 'metrics.csv')
    $headline = $metrics | Where-Object { $_.variant -eq 'ensemble' -and $_.fold -eq '-1' -and $_.cost_bps -eq '5.00' }
    $rotated = $metrics | Where-Object { $_.variant -eq 'control:ensemble-rotated' -and $_.fold -eq '-1' }
    Check ($headline.bars -eq $rotated.bars -and $headline.cost_paid -eq $rotated.cost_paid -and
        $headline.turnover -eq $rotated.turnover -and $headline.time_long -eq $rotated.time_long -and
        $headline.time_short -eq $rotated.time_short) "$name rotation changed count, costs, turnover or exposure"
    Check (@($metrics | Where-Object { $_.variant -eq 'ensemble' -and $_.fold -eq '-1' } |
        Select-Object -ExpandProperty cost_bps -Unique).Count -eq 4) "$name cost curve missing"
    $lines = $walked -split '\r?\n'
    $at = [array]::IndexOf($lines, ($lines | Where-Object { $_ -like 'By calendar *' } | Select-Object -First 1))
    $periods = @()
    for ($i = $at + 2; $at -ge 0 -and $i -lt $lines.Count -and $lines[$i] -match '^\s+\d{4}(-\d{2})?\s'; $i++) {
        $periods += , ($lines[$i].Trim() -split '\s+')
    }
    Check ($periods.Count -ge 2 -and ($periods | ForEach-Object { [long]$_[1] } | Measure-Object -Sum).Sum -eq [long]$headline.bars -and
        [Math]::Abs(($periods | ForEach-Object { [double]$_[3] } | Measure-Object -Sum).Sum - [double]$headline.net_return) -le 0.001) "$name calendar periods do not partition the headline:`n$walked"
    if ($name -in @('breakout', 'dqn')) {
        $output = Invoke-Cli @('verify', '--run', $a.Name, '--root', $first) @(0, 1)
        Check ($output -match 'reproduced: \d+ recorded artifacts byte-identical' -and $output -notmatch 'DIFFERS' -and
            ($name -ne 'dqn' -or ($output -match 'weights\.bin\s+identical' -and $output -match 'learning\.csv\s+identical'))) "$name verify did not reproduce:`n$output"
    }
    if ($name -eq 'breakout') {
        # verify once reran and exited 0 without comparing anything; a same-size edit must be caught.
        $metricsPath = Join-Path $a.FullName 'metrics.csv'
        $bytes = [IO.File]::ReadAllBytes($metricsPath)
        $digit = [Array]::FindLastIndex($bytes, [Predicate[byte]] { param($value) $value -ge 48 -and $value -le 57 })
        $altered = $bytes.Clone()
        $altered[$digit] = if ($bytes[$digit] -eq 48) { 49 } else { 48 }
        [IO.File]::WriteAllBytes($metricsPath, $altered)
        $output = Invoke-Cli @('verify', '--run', $a.Name, '--root', $first) @(1)
        Check ($output -match 'metrics\.csv\s+DIFFERS' -and $output -match 'equity\.csv\s+identical' -and
            $output -match 'NOT REPRODUCED') "verify accepted altered metrics:`n$output"
        [IO.File]::WriteAllBytes($metricsPath, $bytes)
        $manifestPath = Join-Path $a.FullName 'manifest.txt'
        foreach ($key in @('feature_profile', 'feature_schema', 'normalisation', 'calendar', 'currency', 'execution')) {
            [IO.File]::WriteAllText($manifestPath, ($manifest -replace "(?m)^$key=.*$", "$key=unknown"))
            $output = Invoke-Cli @('verify', '--run', $a.Name, '--root', $first) @(2)
            Check ($output -match 'mismatch') "verify accepted a different $key contract"
        }
        [IO.File]::WriteAllText($manifestPath, $manifest)
    }
}
$output = Invoke-Cli ($walk + @('--agent', 'flat', '--out', (Join-Path $root 'invalid-cost'), '--cost-activity', '1')) @(2)
Check ($output -match 'trade count' -and -not (Test-Path (Join-Path $root 'invalid-cost'))) 'generic costs silently invented trade counts'
Write-Output "$script:checks generic predictor checks passed; synthetic inputs only"
