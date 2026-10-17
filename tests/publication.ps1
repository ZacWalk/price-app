param([Parameter(Mandatory)][string]$Probe, [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Force $root
$script:checks = 0
function Check([bool]$condition, [string]$message) {
    $script:checks++
    if (-not $condition) { throw $message }
}
function Start-Probe([string]$Destination, [string]$Payload, [string]$Barrier, [string]$Phase) {
    $info = [Diagnostics.ProcessStartInfo]::new($Probe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @($Destination, $Payload, $Barrier, $Phase)) { $info.ArgumentList.Add($argument) }
    return [Diagnostics.Process]::Start($info)
}
function Wait-Ready($Process, [string]$Barrier) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path "$Barrier/ready")) {
        if ($Process.HasExited) { throw "Writer exited before barrier: $($Process.StandardError.ReadToEnd())" }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Writer did not reach barrier' }
        Start-Sleep -Milliseconds 10
    }
}
function Finish($Process) {
    if (-not $Process.WaitForExit(10000)) { $Process.Kill($true); throw 'Writer did not exit' }
    return @{ code = $Process.ExitCode; error = $Process.StandardError.ReadToEnd(); output = $Process.StandardOutput.ReadToEnd() }
}
function Release-And-Dispose($Process, [string]$Barrier) {
    [IO.File]::WriteAllText("$Barrier/release", 'release')
    if (-not $Process.WaitForExit(10000)) { $Process.Kill($true); $Process.WaitForExit() }
    $Process.Dispose()
}

foreach ($phase in @('reserved', 'staged', 'backed_up')) {
    $barrier = Join-Path $root $phase
    $null = New-Item -ItemType Directory $barrier
    $destination = Join-Path $barrier 'archive.csv'
    [IO.File]::WriteAllText($destination, 'original')
    $first = Start-Probe $destination 'winner' $barrier $phase
    try {
        Wait-Ready $first $barrier
        $second = Start-Probe $destination 'loser' $barrier 'none'
        try { $loser = Finish $second } finally { $second.Dispose() }
        Check ($loser.code -eq 2 -and $loser.error -match 'reservation unavailable') "${phase}: second writer must lose without publishing"
        Check (-not $first.HasExited) "${phase}: owner must still hold the barrier"
        if ($phase -ne 'backed_up') {
            Check ([IO.File]::ReadAllText($destination) -ceq 'original') "${phase}: concurrent loser changed original"
        } else {
            Check ([IO.File]::ReadAllText("$destination.previous") -ceq 'original') 'backed-up original changed'
        }
        [IO.File]::WriteAllText("$barrier/release", 'release')
        $winner = Finish $first
        Check ($winner.code -eq 0) "${phase}: reservation owner failed: $($winner.error)"
        Check ([IO.File]::ReadAllText($destination) -ceq 'winner') "${phase}: winner payload corrupted"
        Check ([IO.File]::ReadAllText("$destination.previous") -ceq 'original') "${phase}: original backup not retained"
        Check (-not (Test-Path "$destination.part")) "${phase}: successful publication left a stage"
    } finally { Release-And-Dispose $first $barrier }
}

foreach ($scenario in @(
    @{ name='late-stage'; phase='reserved'; suffix='.part'; originalExpected='original' },
    @{ name='late-backup'; phase='staged'; suffix='.previous'; originalExpected='original' },
    @{ name='late-destination'; phase='backed_up'; suffix=''; originalExpected='intruder' }
)) {
    $barrier = Join-Path $root $scenario.name
    $null = New-Item -ItemType Directory $barrier
    $destination = Join-Path $barrier 'archive.csv'
    [IO.File]::WriteAllText($destination, 'original')
    $first = Start-Probe $destination 'winner' $barrier $scenario.phase
    try {
        Wait-Ready $first $barrier
        [IO.File]::WriteAllText("$destination$($scenario.suffix)", 'intruder')
        [IO.File]::WriteAllText("$barrier/release", 'release')
        $result = Finish $first
        Check ($result.code -eq 2) "$($scenario.name): racing entry must not be overwritten"
        Check ([IO.File]::ReadAllText("$destination$($scenario.suffix)") -ceq 'intruder') "$($scenario.name): racing entry changed"
        Check ([IO.File]::ReadAllText($destination) -ceq $scenario.originalExpected) "$($scenario.name): destination changed"
        if ($scenario.name -eq 'late-destination') {
            Check ([IO.File]::ReadAllText("$destination.previous") -ceq 'original') 'failed publication lost original backup'
            Check ([IO.File]::ReadAllText("$destination.part") -ceq 'winner') 'failed publication lost staged data'
        }
    } finally { Release-And-Dispose $first $barrier }
}
Write-Output "$checks deterministic multi-process publication checks passed"
