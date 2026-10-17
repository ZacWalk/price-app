param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$OfflineCli,
    [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Force $root
$script:checks = 0
function Check([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { throw $message }
}
function Invoke-Cli([string[]]$Arguments, [int]$Expected = 0) {
    $output = & $Cli @Arguments 2>&1 | Out-String
    Check ($LASTEXITCODE -eq $Expected) "Expected $Expected from $($Arguments -join ' '), got ${LASTEXITCODE}: $output"
    return $output
}
function Invoke-OfflineCli([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($OfflineCli)
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw 'Offline CLI probe timed out'
        }
        return @{ code = $process.ExitCode; output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    } finally { $process.Dispose() }
}
function Fixture([string]$Name, [string]$Stem, [string]$Csv, [string]$Member = '') {
    $directory = Join-Path $root $Name
    $null = New-Item -ItemType Directory -Force $directory
    $zip = Join-Path $directory "$Stem.zip"
    $stream = [IO.File]::Create($zip)
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        if (-not $Member) { $Member = "$Stem.csv" }
        $entry = $archive.CreateEntry($Member)
        $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $writer.Write($Csv) } finally { $writer.Dispose() }
    } finally { $archive.Dispose(); $stream.Dispose() }
    $digest = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$zip.CHECKSUM", "$digest  $Stem.zip`n")
    return $directory
}
$trades = "id,price,qty,quote_qty,time,is_buyer_maker`n1,100,2,200,1721606400000,false`n2,101,3,303,1721606401000,true`n"
$klines = "1721606400000,100,101,99,100,5,1721606459999,500,2,2,200,0`n"
$stem = 'ETHUSDT-trades-2024-07-22'
$fixture = Fixture 'valid' $stem $trades
$out = Join-Path $root 'output'
$base = @('download', '--symbol', 'ETHUSDT', '--from', '2024-07-22', '--to', '2024-07-22', '--out', $out)
$emptyOutput = Join-Path $root 'empty-fixtures-output'
$probeArgs = @('download', '--from', '2024-07-22', '--to', '2024-07-22', '--out', $emptyOutput)
foreach ($cached in @($false, $true)) {
    if ($cached) {
        $null = New-Item -ItemType Directory $emptyOutput
        [IO.File]::WriteAllText("$emptyOutput/$stem.csv", 'cached sentinel')
    }
    $result = Invoke-OfflineCli ($probeArgs + @('--fixtures', ''))
    Check ($result.code -eq 2 -and $result.output -match '--fixtures requires a non-empty directory') 'explicit empty fixture argument must fail validation'
    Check ($result.output -notmatch 'DEFAULT_NETWORK_TRANSPORT_BLOCKED') 'empty fixtures reached native-client construction'
    if ($cached) {
        Check ((Get-ChildItem $emptyOutput -File).Count -eq 1 -and
            [IO.File]::ReadAllText("$emptyOutput/$stem.csv") -ceq 'cached sentinel') 'empty fixtures changed cached files'
    } else { Check (-not (Test-Path $emptyOutput)) 'empty fixtures created output files' }
}
$result = Invoke-OfflineCli $probeArgs
Check ($result.code -eq 2 -and $result.output -match 'DEFAULT_NETWORK_TRANSPORT_BLOCKED') 'omitted fixtures must retain normal network-mode selection'
Check ((Get-ChildItem $emptyOutput -File).Count -eq 1) 'network-selection probe changed outputs'
$null = Invoke-Cli ($base + @('--fixtures', $fixture))
Check ([IO.File]::ReadAllText((Join-Path $out "$stem.csv")) -ceq $trades) 'CSV lexemes preserved exactly'
$hash = (Get-FileHash (Join-Path $out "$stem.csv")).Hash
$null = Invoke-Cli ($base + @('--fixtures', "$root/missing"))
Check ((Get-FileHash (Join-Path $out "$stem.csv")).Hash -eq $hash) 'existing data skips acquisition'
$null = Invoke-Cli @('bars', '--in', $out, '--out', "$root/trades.bin", '--interval', '1m')
Check ((Get-Item "$root/trades.bin").Length -eq (35 + 72)) 'trade fixture aggregates one unchanged 72-byte bar'
$monthly = Fixture 'monthly' 'ETHUSDT-1m-2024-07' $klines
$null = Invoke-Cli @('download', '--kind', 'klines', '--from', '2024-07-22', '--to', '2024-07-23',
    '--fixtures', $monthly, '--out', "$root/klines")
$null = Invoke-Cli @('bars', '--in', "$root/klines", '--out', "$root/klines.bin", '--interval', '1m')
Check ((Get-Item "$root/klines.bin").Length -eq 107) 'monthly klines aggregate'
$monthlyTrades = Fixture 'monthly-trades' 'ETHUSDT-trades-2024-07' $trades
foreach ($limit in @(1, 2, 3, 20000000)) {
    $rowOutput = "$root/row-limit-$limit"
    $arguments = @('download', '--period', 'monthly', '--from', '2024-07-01', '--to', '2024-07-31',
        '--fixtures', $monthlyTrades, '--out', $rowOutput, '--max-rows', "$limit")
    $result = Invoke-Cli $arguments $(if ($limit -eq 1) { 2 } else { 0 })
    if ($limit -eq 1) {
        Check ($result -match 'limit' -and -not (Test-Path $rowOutput)) 'row overflow must fail before publication'
    } else {
        Check ([IO.File]::ReadAllText("$rowOutput/ETHUSDT-trades-2024-07.csv") -ceq $trades) 'monthly trade rows within the limit must be preserved'
    }
}
$null = Invoke-Cli @('download', '--kind', 'klines', '--from', '2024-07-01', '--to', '2024-07-31',
    '--fixtures', $monthly, '--out', "$root/kline-row-limit", '--max-rows', '1')
foreach ($limit in @('0', '-1', '9223372036854775808', '18446744073709551616', '1.5', 'not-a-number', '')) {
    $arguments = @('download', '--from', '2024-07-22', '--to', '2024-07-22',
        '--out', "$root/invalid-row-limit", '--max-rows', $limit)
    $result = Invoke-OfflineCli $arguments
    Check ($result.code -eq 2 -and $result.output -match 'max-rows' -and
        $result.output -notmatch 'DEFAULT_NETWORK_TRANSPORT_BLOCKED') 'invalid row limit reached client creation'
}
Check (-not (Test-Path "$root/invalid-row-limit")) 'invalid row limits created output'
foreach ($kind in @('aggTrades', 'fundingRate', 'metrics')) {
    $stamp = if ($kind -eq 'fundingRate') { '2024-07' } else { '2024-07-22' }
    $rawStem = "ETHUSDT-$kind-$stamp"
    $raw = "provider_column,opaque_value`n$kind,001.2500`n"
    $rawFixture = Fixture "raw-$kind" $rawStem $raw
    Push-Location $root
    try {
        $arguments = @('download', '--kind', $kind, '--from', '2024-07-22', '--to', '2024-07-22',
            '--fixtures', $rawFixture)
        $null = Invoke-Cli $arguments
        $destination = "$root/data/$kind/ETHUSDT/$rawStem.csv"
        Check ([IO.File]::ReadAllText($destination) -ceq $raw) "$kind raw CSV/schema/default directory preserved"
        [IO.File]::WriteAllText("$rawFixture/$rawStem.zip.CHECKSUM", (('0' * 64) + "  $rawStem.zip`n"))
        $null = Invoke-Cli ($arguments + @('--force')) 2
        Check ([IO.File]::ReadAllText($destination) -ceq $raw) "$kind checksum failure preserved old CSV"
    } finally { Pop-Location }
}
$badDigest = Fixture 'checksum' $stem $trades
[IO.File]::WriteAllText("$badDigest/$stem.zip.CHECKSUM", (('0' * 64) + "  $stem.zip`n"))
$null = Invoke-Cli ($base + @('--fixtures', $badDigest, '--force')) 2
Check ((Get-FileHash (Join-Path $out "$stem.csv")).Hash -eq $hash) 'checksum failure cannot replace existing CSV'
$missingDigest = Join-Path $root 'missing-checksum'
$null = New-Item -ItemType Directory $missingDigest
Copy-Item "$fixture/$stem.zip" $missingDigest
$null = Invoke-Cli ($base + @('--fixtures', $missingDigest, '--force')) 2
$mixedFixtures = Join-Path $root 'mixed-fixtures'
$mixedOutput = Join-Path $root 'mixed-output'
$null = New-Item -ItemType Directory $mixedFixtures, $mixedOutput
$dayB = 'ETHUSDT-trades-2024-07-23'
$dayBFixture = Fixture 'day-b' $dayB ($trades.Replace('172160640', '172169280'))
Copy-Item "$dayBFixture/$dayB.zip" $mixedFixtures
Copy-Item "$out/$stem.csv" $mixedOutput
[IO.File]::WriteAllText("$mixedOutput/untouched.note", 'unchanged')
$snapshot = @(Get-ChildItem $mixedOutput -File | Sort-Object Name |
    ForEach-Object { $_.Name + ':' + (Get-FileHash $_.FullName).Hash }) -join '|'
$mixedResult = Invoke-Cli @('download', '--from', '2024-07-22', '--to', '2024-07-23',
    '--fixtures', $mixedFixtures, '--out', $mixedOutput, '--skip-missing') 2
Check ($mixedResult -match 'checksum') 'missing sidecar must be a checksum failure, not skippable missing ZIP'
$after = @(Get-ChildItem $mixedOutput -File | Sort-Object Name |
    ForEach-Object { $_.Name + ':' + (Get-FileHash $_.FullName).Hash }) -join '|'
Check ($after -ceq $snapshot) 'mixed-range checksum failure changed output files'
foreach ($spec in @(
    @{name='traversal'; csv=$trades; member='../escape.csv'},
    @{name='wrong-member'; csv=$trades; member='other.csv'},
    @{name='bad-csv'; csv="id,price`n1,broken`n"; member=''},
    @{name='wrong-period'; csv=$trades.Replace('1721606400000','1704067200000'); member=''}
)) {
    $bad = Fixture $spec.name $stem $spec.csv $spec.member
    $null = Invoke-Cli ($base + @('--fixtures', $bad, '--force')) 2
}
$truncated = Fixture 'truncated' $stem $trades
[IO.File]::WriteAllBytes("$truncated/$stem.zip", [byte[]]@(0x50,0x4b,3,4))
[IO.File]::WriteAllText("$truncated/$stem.zip.CHECKSUM", ((Get-FileHash "$truncated/$stem.zip").Hash + "  $stem.zip`n"))
$null = Invoke-Cli ($base + @('--fixtures', $truncated, '--force')) 2
foreach ($extra in @(
    @('--max-archive-bytes', '10'), @('--max-csv-bytes', '10'),
    @('--max-csv-bytes', '-1'), @('--max-archive-bytes', 'nonsense'),
    @('--skip-checksum'), @('--kind', 'unknown'), @('--symbol', '../ETH'),
    @('--market', 'unknown'), @('--period', 'yearly'), @('--from', '2024-02-30'),
    @('--from', '2024-07-23'), @('--to', '2024-7-22'), @('--from', '1970-01-01'),
    # A switch takes no value; `--force false` once forced the replacement.
    @('--force', 'false'), @('--skip-missing', 'no')
)) {
    $null = Invoke-Cli ($base + @('--fixtures', $fixture, '--force') + $extra) 2
}
$null = Invoke-Cli ($base + @('--fixtures', "$root/absent", '--force')) 2
$null = Invoke-Cli @('download', '--fixtures', "$root/absent", '--from', '2024-07-22', '--to', '2024-07-22',
    '--out', "$root/missing-output", '--skip-missing') 2
Check (-not (Test-Path "$root/missing-output")) 'missing archives create no output'
Check (-not (Test-Path "$root/escape.csv")) 'ZIP traversal never reaches disk'
Check ((Get-FileHash (Join-Path $out "$stem.csv")).Hash -eq $hash) 'all negative cases preserve old file'
$null = Invoke-Cli ($base + @('--fixtures', $fixture, '--force'))
Check ((Get-FileHash "$out/$stem.csv.previous").Hash -eq $hash) '--force keeps backup'
$null = Invoke-Cli ($base + @('--fixtures', $fixture, '--force')) 2
Check ((Get-FileHash "$out/$stem.csv.previous").Hash -eq $hash) 'existing backup is never overwritten'

$bytes = [IO.File]::ReadAllBytes("$root/trades.bin")
foreach ($fault in @('magic', 'version', 'negative-count', 'huge-count', 'zero-interval', 'truncated', 'trailing')) {
    $bad = [byte[]]$bytes.Clone()
    switch ($fault) {
        magic { $bad[0] = 0 }
        version { $bad[4] = 9 }
        negative-count { [Array]::Copy([BitConverter]::GetBytes([long]-1), 0, $bad, 16, 8) }
        huge-count { [Array]::Copy([BitConverter]::GetBytes([long]::MaxValue), 0, $bad, 16, 8) }
        zero-interval { [Array]::Clear($bad, 8, 8) }
        truncated { $bad = $bad[0..50] }
        trailing { $bad += 0 }
    }
    [IO.File]::WriteAllBytes("$root/$fault.bin", $bad)
    $null = Invoke-Cli @('audit', '--bars', "$root/$fault.bin") 2
}
$null = Invoke-Cli @('audit', '--bars', "$root/trades.bin") 2
$result = Invoke-Cli @('bars', '--in', "$root/absent-input", '--out', "$root/absent.bin") 2
Check ($result -match 'cannot open' -and -not (Test-Path "$root/absent.bin")) 'missing bars input must say it cannot be opened'
$nonfinite = Join-Path $root 'nonfinite'
$null = New-Item -ItemType Directory $nonfinite
[IO.File]::WriteAllText("$nonfinite/ETHUSDT-trades-nonfinite.csv", "id,price,qty,quote_qty,time,is_buyer_maker`n" +
    "1,100,2,200,1721606400000,false`n2,nan,3,303,1721606401000,true`n3,inf,1,102,1721606402000,false`n" +
    "4,1e400,1,99,1721606403000,true`n5,101,nan,99,1721606404000,true`n")
$result = Invoke-Cli @('bars', '--in', $nonfinite, '--out', "$root/nonfinite.bin", '--interval', '1s')
Check ($result -match '(?m)^\s+ETHUSDT-trades-nonfinite\.csv\s+1 trades' -and (Get-Item "$root/nonfinite.bin").Length -eq 107) 'non-finite trade rows must be skipped'
$nonfiniteKlines = Join-Path $root 'nonfinite-klines'
$null = New-Item -ItemType Directory $nonfiniteKlines
[IO.File]::WriteAllText("$nonfiniteKlines/ETHUSDT-1m-nonfinite.csv", $klines +
    "1721606460000,100,101,99,nan,5,1721606519999,500,2,2,200,0`n1721606520000,100,inf,99,100,5,1721606579999,500,2,2,200,0`n")
$result = Invoke-Cli @('bars', '--in', $nonfiniteKlines, '--out', "$root/nonfinite-klines.bin", '--interval', '1m')
Check ($result -match '1 klines' -and (Get-Item "$root/nonfinite-klines.bin").Length -eq 107) 'non-finite kline rows must be skipped'
# At the old 1s default each 1m kline became one bar and 59 synthetic copies of its close.
$minutes = Join-Path $root 'kline-minutes'
$null = New-Item -ItemType Directory $minutes
[IO.File]::WriteAllText("$minutes/ETHUSDT-1m-2024-07.csv",
    "open_time,open,high,low,close,volume,close_time,quote_volume,count,taker_buy_volume,taker_buy_quote_volume,ignore`n" +
    $klines + "1721606460000,100,102,99,101,6,1721606519999,600,3,3,300,0`n1721606520000,101,103,100,102,7,1721606579999,700,4,4,400,0`n")
$result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/minutes.bin")
Check ($result -match '(?m)^3 klines -> ') 'kline input must be reported as klines, not trades'
Check ($result -match ' -> 3 bars x 60000ms \(0 empty\)' -and (Get-Item "$root/minutes.bin").Length -eq (35 + 3 * 72)) 'klines must default to their own interval'
foreach ($interval in @('1s', '90s')) {
    $result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/minutes-$interval.bin", '--interval', $interval) 2
    Check ($result -match 'whole multiple' -and -not (Test-Path "$root/minutes-$interval.bin")) "--interval $interval must not split or straddle 1m klines"
}
# dd passes its trades-only 10s default unasked, so the refusal has to say what to pass instead.
$result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/minutes-10s.bin", '--interval', '10s') 2
Check ($result -match '--interval 10s is not a whole multiple of the 1m klines' -and $result -match 'Use --interval 1m, or any other whole multiple of 1m') "the 10s refusal must name the interval to use:`n$result"
$result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/minutes-90s.bin", '--interval', '90s') 2
Check ($result -match 'Use --interval 1m or 2m,') "the 90s refusal must name the nearest whole multiples:`n$result"
# 106751991167d once wrapped to a positive interval and wrote one bar dated 1970.
foreach ($interval in @('1w', '1.5m', '106751991167d', '9223372036854775807', '366d')) {
    $result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/bad-interval.bin", '--interval', $interval) 2
    Check ($result -match "bad --interval '$([regex]::Escape($interval))'; expected a whole number and a unit" -and
        -not (Test-Path "$root/bad-interval.bin")) "--interval $interval must be refused with the accepted form"
}
$result = Invoke-Cli @('bars', '--in', $minutes, '--out', '') 2
Check ($result -match '--out needs a value') 'an empty --out must be refused by name'
$result = Invoke-Cli @('bars', '--in', $minutes, '--out', "$root/minutes-3m.bin", '--interval', '3m')
Check ($result -match '(?m)^3 klines -> 1 bars x 180000ms' -and (Get-Item "$root/minutes-3m.bin").Length -eq 107) 'klines must aggregate to a whole multiple of their interval'
# A daily archive fetched before its monthly one was published sorts first; the month was then folded into
# the last bar (its whole high/low range and volume) and the days before became synthetic flat bars, exit 0.
$overlap = Join-Path $root 'kline-overlap'
$null = New-Item -ItemType Directory $overlap
Copy-Item "$minutes/ETHUSDT-1m-2024-07.csv" $overlap
[IO.File]::WriteAllText("$overlap/ETHUSDT-1m-2024-07-22.csv", "1721606520000,101,103,100,102,7,1721606579999,700,4,4,400,0`n")
$result = Invoke-Cli @('bars', '--in', $overlap, '--out', "$root/overlap.bin") 2
Check ($result -match 'overlapping' -and $result -match 'ETHUSDT-1m-2024-07\.csv' -and -not (Test-Path "$root/overlap.bin")) "overlapping kline archives must be refused:`n$result"
$duplicate = Join-Path $root 'kline-duplicate'
$null = New-Item -ItemType Directory $duplicate
[IO.File]::WriteAllText("$duplicate/ETHUSDT-1m-2024-07-22.csv", $klines + $klines)
$result = Invoke-Cli @('bars', '--in', $duplicate, '--out', "$root/duplicate.bin", '--interval', '1m') 2
Check ($result -match 'overlapping' -and -not (Test-Path "$root/duplicate.bin")) "a repeated kline must be refused:`n$result"
$adjacent = Join-Path $root 'kline-adjacent'
$null = New-Item -ItemType Directory $adjacent
[IO.File]::WriteAllText("$adjacent/ETHUSDT-1m-2024-07-22.csv", $klines)
[IO.File]::WriteAllText("$adjacent/ETHUSDT-1m-2024-07-23.csv", "1721692800000,100,101,99,100,5,1721692859999,500,2,2,200,0`n")
$result = Invoke-Cli @('bars', '--in', $adjacent, '--out', "$root/adjacent.bin", '--interval', '1m')
Check ($result -match '(?m)^2 klines -> 1441 bars x 60000ms \(1439 empty\)') "consecutive kline archives must still join:`n$result"
$tradeOverlap = Join-Path $root 'trade-overlap'
$null = New-Item -ItemType Directory $tradeOverlap
[IO.File]::WriteAllText("$tradeOverlap/ETHUSDT-trades-2024-07-22.csv", $trades)
[IO.File]::WriteAllText("$tradeOverlap/ETHUSDT-trades-2024-07.csv", $trades)
$result = Invoke-Cli @('bars', '--in', $tradeOverlap, '--out', "$root/trade-overlap.bin", '--interval', '1m') 2
Check ($result -match 'overlapping' -and -not (Test-Path "$root/trade-overlap.bin")) "overlapping trade archives must be refused:`n$result"
$tradeDays = Join-Path $root 'trade-days'
$null = New-Item -ItemType Directory $tradeDays
[IO.File]::WriteAllText("$tradeDays/ETHUSDT-trades-2024-07-22.csv", $trades)
[IO.File]::WriteAllText("$tradeDays/ETHUSDT-trades-2024-07-23.csv", "id,price,qty,quote_qty,time,is_buyer_maker`n3,102,1,102,1721692800000,false`n")
$result = Invoke-Cli @('bars', '--in', $tradeDays, '--out', "$root/trade-days.bin", '--interval', '1m')
Check ($result -match '(?m)^3 trades -> 1441 bars x 60000ms \(1439 empty\)') "consecutive trade archives must still join:`n$result"
# Aggregate trades fit the trades layout by width; their trade ids were read as times, giving bars dated 1970.
$aggregate = Join-Path $root 'agg-trades'
$null = New-Item -ItemType Directory $aggregate
[IO.File]::WriteAllText("$aggregate/ETHUSDT-aggTrades-2024-07-22.csv",
    "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker`n3105523214,2684.68,0.032,8806707936,8806707936,1721606400000,false`n")
$result = Invoke-Cli @('bars', '--in', $aggregate, '--out', "$root/agg-trades.bin", '--interval', '1m') 2
Check ($result -match 'unrecognised csv layout' -and -not (Test-Path "$root/agg-trades.bin")) "aggregate trades must not import as trades:`n$result"
if ($IsWindows) {
    # Outside every ANSI code page, so only a UTF-8 argv/file API can open it.
    $unicode = Join-Path $root "漢字-données"
    $null = New-Item -ItemType Directory $unicode
    Copy-Item "$out/$stem.csv" $unicode
    $null = Invoke-Cli @('bars', '--in', $unicode, '--out', "$unicode/漢字.bin", '--interval', '1m')
    Check ((Get-Item -LiteralPath "$unicode/漢字.bin").Length -eq 107) 'non-ASCII paths must reach the CLI intact'
}
Write-Output "$checks acquisition/negative checks passed"
