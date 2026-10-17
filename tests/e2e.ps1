param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
$bars = Join-Path $root 'bars.bin'
& "$PSScriptRoot/fixture.ps1" -Output $bars
& "$PSScriptRoot/agents.ps1" -Cli $Cli -Root "$root/first" -Bars $bars
& "$PSScriptRoot/agents.ps1" -Cli $Cli -Root "$root/second" -Bars $bars
$first = Get-Content "$root/first/summary.json" -Raw | ConvertFrom-Json
$second = Get-Content "$root/second/summary.json" -Raw | ConvertFrom-Json
$checks = 0
foreach ($a in $first) {
    $b = $second | Where-Object agent -eq $a.agent
    $checks++
    if ($a.metrics -cne $b.metrics -or $a.equity -cne $b.equity -or $a.exit -ne $b.exit) {
        throw "$($a.agent) does not reproduce"
    }
}
if ($IsWindows) {
    $expected = @{
        flat='02FDECCABD093635A33F5E75CB323C66E9657039D2375E3267B17313944B0F0C'
        long='AE286BF8C2E516C9C0B277AC13918019FF967FCBC505F54CE3FC1E3A6A66B6A8'
        short='432B0EB385866A16B40B2222C214B56F3761A912F2822567572EECE720B99173'
        random='D65BBA541001391C7BFC11D5C2221D0AFF116C24786DF2C35DD77B9B7C9B94DC'
        crossover='831732FC86ECF10A470506A9F53B3B7A50F33B82FC5E4A8B7DB4AFF790C52E45'
        breakout='440AB4A6BCA39F35D83303A68FB6FA1D0071C6F685051D867F33A59669849EC0'
        qtable='B87FF42CD6A4C846DCC138EBE7D21BBF1747C5ECF3A7D79A4C959106AB114BEE'
    }
    foreach ($a in $first | Where-Object agent -ne 'dqn') {
        $checks++
        if ($a.metrics -cne $expected[$a.agent]) { throw "$($a.agent) differs from untouched 0a1bae3 baseline" }
    }
}
$a = $first | Where-Object agent -eq 'dqn'
$b = $second | Where-Object agent -eq 'dqn'
foreach ($filename in @('weights.bin', 'learning.csv')) {
    $checks++
    if ((Get-FileHash "$root/first/dqn/$($a.run)/$filename").Hash -cne (Get-FileHash "$root/second/dqn/$($b.run)/$filename").Hash) {
        throw "DQN $filename does not reproduce"
    }
}
$manifest = "$root/first/dqn/$($a.run)/manifest.txt"
$learning = Import-Csv "$root/first/dqn/$($a.run)/learning.csv"
$checks++
if (-not @($learning | Where-Object { [double]$_.loss -gt 0 }).Count) { throw 'Real DQN never trained a batch' }
$text = Get-Content $manifest -Raw
$checks++
if ($text -notmatch 'dqn_numerics=cmlp-f32-scalar-he-splitmix64-v1') { throw 'DQN numerics metadata missing' }
# Seeds 7 and 7926 once printed as identical rows, and long keys and variants pushed their rows out of line.
$report = & $Cli report --run $a.run --root "$root/first/dqn" 2>&1 | Out-String
$reported = $LASTEXITCODE
$lines = $report -split '\r?\n'
$pooled = [array]::IndexOf($lines, 'pooled results (fold -1)')
$table = @($lines[($pooled + 1)..($lines.Count - 1)] | Where-Object { $_ })
$checks++
if ($reported -ne 0 -or $pooled -lt 4 -or $table[0] -notmatch '^\s+variant\s+seed\s+cost\s' -or
    $report -notmatch '(?m)^\s+dqn\s+7\s+5\.00\s' -or $report -notmatch '(?m)^\s+dqn\s+7926\s+5\.00\s') {
    throw "report does not say which seed each row came from:`n$report"
}
$checks++
if (@($table | ForEach-Object Length | Sort-Object -Unique).Count -ne 1 -or
    @($lines[2..($pooled - 2)] | ForEach-Object { ([regex]::Match($_, '^  \S+ +')).Length } | Sort-Object -Unique).Count -ne 1) {
    throw "report columns are out of line:`n$report"
}
# A share of a gross that is not positive once printed as 0%, which reads as trading for free.
$log = Get-Content "$root/first/crossover.log"
$crossover = ($first | Where-Object agent -eq 'crossover').run
foreach ($row in Import-Csv "$root/first/crossover/$crossover/metrics.csv" | Where-Object {
        $_.fold -eq '-1' -and $_.seed -eq '0' -and $_.cost_bps -eq '5.00' -and $_.variant -in @('flat', 'long', 'random', 'crossover', 'breakout') }) {
    $line = $log | Where-Object { $_ -match "^\s+$($row.variant)\s+-?\d" } | Select-Object -First 1
    $expected = if ([double]$row.gross_return -gt 0) { '\d%$' } else { '\s-$' }
    $checks++
    if ($line -notmatch $expected) { throw "cost% of $($row.variant) with gross $($row.gross_return): '$line'" }
    # Costs are linear in the units charged, so at 5 bps and no slippage break-even is 5 * gross / cost.
    $cell = ($line.Trim() -split '\s+')[-2]
    $checks++
    if ([double]$row.gross_return -gt 0 -and [double]$row.cost_paid -gt 0) {
        $breakeven = 5 * [double]$row.gross_return / [double]$row.cost_paid
        if ([Math]::Abs([double]$cell - $breakeven) -gt 0.051) { throw "break-even of $($row.variant) is $cell, expected $breakeven" }
    } elseif ($cell -ne '-') { throw "break-even of $($row.variant) with gross $($row.gross_return) is '$cell', not '-'" }
}
# The calendar breakdown has to partition the pooled headline exactly: every scored bar once, nets adding up.
$headline = Import-Csv "$root/first/crossover/$crossover/metrics.csv" | Where-Object {
    $_.variant -eq 'ensemble' -and $_.fold -eq '-1' -and $_.cost_bps -eq '5.00' }
$at = [array]::IndexOf($log, ($log | Where-Object { $_ -like 'By calendar month*' } | Select-Object -First 1))
$periods = @()
for ($i = $at + 2; $at -ge 0 -and $i -lt $log.Count -and $log[$i] -match '^\s+\d{4}-\d{2}\s'; $i++) { $periods += , ($log[$i].Trim() -split '\s+') }
$checks++
if ($periods.Count -lt 2 -or ($periods | ForEach-Object { [long]$_[1] } | Measure-Object -Sum).Sum -ne [long]$headline.bars -or
    [Math]::Abs(($periods | ForEach-Object { [double]$_[3] } | Measure-Object -Sum).Sum - [double]$headline.net_return) -gt 0.001) {
    throw "calendar periods do not partition the headline:`n$($log -join "`n")"
}
$legacy = "$root/legacy/legacy-run"
$null = New-Item -ItemType Directory -Force $legacy
[IO.File]::WriteAllText("$legacy/manifest.txt", ($text -replace '(?m)^dqn_numerics=.*\r?\n',''))
$output = & $Cli verify --run legacy-run --root "$root/legacy" 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 2 -or $output -notmatch 'legacy runs require') { throw 'Legacy DQN verify was not explicitly rejected' }
[IO.File]::WriteAllText("$legacy/manifest.txt", ($text -replace '(?m)^libcmlp_revision=.*$', ('libcmlp_revision=' + ('0' * 40))))
$output = & $Cli verify --run legacy-run --root "$root/legacy" 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 2 -or $output -notmatch 'libcmlp revision mismatch') { throw 'Mismatched library revision was not explicitly rejected' }
foreach ($args in @(@('--batch','0'), @('--target-sync','0'), @('--lr','NaN'), @('--lr-decay','0'),
    @('--seeds','0'), @('--train','-1'), @('--agent','bogus'), @('--agnet','breakout'), @('--hidden','abc'),
    @('--hidden','64,0'), @('--epsilon-decay','-5'), @('stray'))) {
    # Windows that fit the fixture, so the configuration itself is what gets rejected.
    $null = & $Cli walk --bars $bars --agent dqn --seeds 1 --train 5000 --gap 300 --test 2000 --episode 1000 `
        --epochs 1 --hidden 8,8 --out "$root/invalid" @args 2>&1
    $checks++
    if ($LASTEXITCODE -ne 2) { throw "Invalid config was not rejected: $args" }
}
$qtable = @('walk', '--bars', $bars, '--agent', 'qtable', '--seeds', '1', '--train', '5000', '--gap', '300',
    '--test', '2000', '--episode', '1000', '--epochs', '1')
foreach ($invalid in @(@('--q-lr','-1'), @('--q-lr','0'), @('--q-lr','1.5'), @('--gamma','1.5'), @('--gamma','-0.1'),
    @('--epsilon-decay','-5'))) {
    $output = & $Cli @qtable --out "$root/invalid" @invalid 2>&1 | Out-String
    $checks++
    if ($LASTEXITCODE -ne 2 -or $output -notmatch 'invalid Q-table configuration') { throw "Invalid qtable config was not rejected: $invalid" }
}
# The edges of each range still train; one step past them does not.
$null = & $Cli @qtable --out "$root/edges" --q-lr 1 --gamma 1 --epsilon-decay 0 2>&1
$edges = $LASTEXITCODE
$null = & $Cli @qtable --out "$root/invalid" --q-lr 1 --gamma 1 --epsilon-decay -1 2>&1
$checks++
if ($edges -notin @(0, 1) -or $LASTEXITCODE -ne 2) { throw "qtable range edges: $edges for the edges, $LASTEXITCODE one past them" }
$checks++
if (@(Get-ChildItem "$root/invalid" -Directory -ErrorAction SilentlyContinue).Count) {
    throw 'A rejected configuration left a run directory behind'
}
# Each refusal names the flag. These once trained anyway: --q-lr 0.5x as 0.5, --seeds 4294967297 as one
# seed, a bare --lr as 1, --cost-bps -5 as a rebate, and every DQN fault shared one vague message.
$dqn = @('walk', '--bars', $bars, '--agent', 'dqn', '--seeds', '1', '--train', '5000', '--gap', '300', '--test', '2000',
    '--episode', '1000', '--epochs', '1', '--hidden', '8,8', '--out', "$root/invalid")
foreach ($case in @(@('--batch', '0', '--batch must be at least 1'), @('--target-sync', '0', '--target-sync must be at least 1'),
    @('--lr', '0', '--lr must be positive'), @('--lr-decay', '0', '--lr-decay must be positive'),
    @('--gamma', '2', '--gamma must be in \[0, 1\]'), @('--switch-margin', '-1', '--switch-margin must be at least 0'),
    @('--batch', '4294967297', '--batch 4294967297 is out of range'), @('--seeds', '4294967297', '--seeds 4294967297 is out of range'),
    @('--q-lr', '0.5x', "bad finite number --q-lr '0\.5x'"), @('--gamma', '0.9.9', "bad finite number --gamma '0\.9\.9'"),
    @('--cost-bps', '-5', '--cost-bps must be at least 0'), @('--slippage-bps', '-1', '--slippage-bps must be at least 0'),
    @('--cost-activity', '-1', '--cost-activity must be at least 0'), @('--hold-penalty', '-1', '--hold-penalty must be at least 0'),
    @('--seed', '-1', '--seed must be at least 0'), @('--lr', $null, '--lr needs a value'))) {
    $flag = @($case[0]) + @($case[1] | Where-Object { $null -ne $_ })
    $output = & $Cli @dqn @flag 2>&1 | Out-String
    $checks++
    if ($LASTEXITCODE -ne 2 -or $output -notmatch $case[2]) { throw "walk $($flag -join ' ') was not refused by name:`n$output" }
}
$checks++
if (@(Get-ChildItem "$root/invalid" -Directory -ErrorAction SilentlyContinue).Count) {
    throw 'A configuration refused by name left a run directory behind'
}
[IO.File]::WriteAllText("$root/occupied", 'a file, not a runs directory')
$output = & $Cli @dqn --out "$root/occupied" 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 2 -or $output -notmatch 'cannot create .*occupied') { throw "walk --out <file> was not refused plainly:`n$output" }
foreach ($case in @(@('--width', '0.5x', "bad finite number --width '0\.5x'"), @('--shuffles', '4294967297', 'out of range'),
    @('--block', '-5', 'must be at least 0'), @('--cost-bps', '-5', 'must be at least 0'),
    # Against one shuffled draw, noise survives half the time; 20 is the fewest that can place a 95% threshold.
    @('--shuffles', '19', '--shuffles must be at least 20'))) {
    $output = & $Cli patterns --bars $bars --shuffles 20 $case[0] $case[1] 2>&1 | Out-String
    $checks++
    if ($LASTEXITCODE -ne 2 -or $output -notmatch $case[2]) { throw "patterns $($case[0]) $($case[1]) was not refused:`n$output" }
}
# Whole-day blocks keep every bar's hour, so the hour-of-day null never moved and rounding decided its
# verdict (SURVIVES on real 5m bars); fewer than two blocks gave a threshold of 0, and all 20 tests survived.
$text = & $Cli patterns --bars $bars --shuffles 20 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 0 -or [regex]::Matches($text, 'UNTESTED').Count -ne 2 -or
    [regex]::Matches($text, 'against a shuffled 95% of').Count -ne 18) { throw "a null that cannot move was given a verdict:`n$text" }
$text = & $Cli patterns --bars $bars --shuffles 20 --block 50 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 0 -or $text -match 'UNTESTED' -or [regex]::Matches($text, 'against a shuffled 95% of').Count -ne 20) {
    throw "part-day blocks must test every grouping:`n$text"
}
$text = & $Cli patterns --bars $bars --shuffles 20 --block 100000 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 0 -or [regex]::Matches($text, 'UNTESTED').Count -ne 20 -or
    $text -match 'SURVIVES the permutation test' -or $text -match '(?m)  PAYS\r?$') {
    throw "fewer than two blocks must not survive anything:`n$text"
}
# A run without results once reported its manifest alone and exited 0.
$null = New-Item -ItemType Directory -Force "$root/broken/no-metrics"
Copy-Item $manifest "$root/broken/no-metrics"
$output = & $Cli report --run no-metrics --root "$root/broken" 2>&1 | Out-String
$checks++
if ($LASTEXITCODE -ne 2 -or $output -notmatch 'no metrics\.csv') { throw "report of a run without metrics.csv did not fail:`n$output" }
foreach ($case in @(@{ arguments = @(); code = 2 }, @{ arguments = @('help'); code = 0 },
    @{ arguments = @('--help'); code = 0 }, @{ arguments = @('bogus'); code = 2 },
    @{ arguments = @('report'); code = 2 }, @{ arguments = @('verify', '--root', "$root/legacy"); code = 2 },
    @{ arguments = @('list', '--rot', "$root/legacy"); code = 2 },
    @{ arguments = @('audit', '--bars', $bars, 'extra'); code = 2 })) {
    $null = & $Cli @($case.arguments) 2>&1
    $checks++
    if ($LASTEXITCODE -ne $case.code) { throw "cli $($case.arguments -join ' ') exited $LASTEXITCODE, expected $($case.code)" }
}
Write-Output "$checks end-to-end checks passed; 17 complete agent runs, both audits passed"
