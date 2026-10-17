param(
    [string]$Cli = 'exe/cli.exe',
    [string]$Root = 'tmp/p5/agents',
    [string]$Bars = 'tmp/p5/bars.bin'
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-Location (Split-Path $PSScriptRoot)
$null = New-Item -ItemType Directory -Force $Root
& $Cli audit --bars $Bars *> "$Root/audit.log"
if ($LASTEXITCODE -ne 0) { throw 'Causality audit failed.' }
$summary = @()
foreach ($agent in @('flat', 'long', 'short', 'random', 'crossover', 'breakout', 'qtable', 'dqn')) {
    $out = "$Root/$agent"
    & $Cli walk --bars $Bars --agent $agent --seeds 2 --train 5000 --valid 500 --gap 300 --test 2000 --episode 1000 --epochs 1 --hidden 8,8 --batch 8 --target-sync 50 --lr-decay 0.95 --seed 7 --out $out *> "$Root/$agent.log"
    $code = $LASTEXITCODE
    if ($code -notin @(0, 1)) { throw "$agent failed: $code ($(Get-Content "$Root/$agent.log" -Tail 5))" }
    $run = Get-ChildItem $out -Directory | Sort-Object Name -Descending | Select-Object -First 1
    foreach ($file in @('manifest.txt', 'metrics.csv', 'equity.csv')) {
        if (-not (Test-Path (Join-Path $run.FullName $file))) { throw "$agent missing $file" }
    }
    $summary += [pscustomobject]@{
        agent = $agent; exit = $code; run = $run.Name
        metrics = (Get-FileHash (Join-Path $run.FullName 'metrics.csv')).Hash
        equity = (Get-FileHash (Join-Path $run.FullName 'equity.csv')).Hash
    }
}
$summary | ConvertTo-Json | Set-Content "$Root/summary.json"
$summary | Format-Table agent, exit, metrics
