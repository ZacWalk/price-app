param([Parameter(Mandatory)][string]$Bench, [Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Join-Path $Directory ([Guid]::NewGuid().ToString('N'))
& $Bench create $root 10000
if ($LASTEXITCODE -ne 0) { throw 'Storage fixture creation failed' }
$hash = $null
$checks = 0
foreach ($format in @('spqb', 'spike')) {
    foreach ($mode in @('stream','materialize')) {
        $output = & $Bench scan $root $format 64 $mode
        if ($LASTEXITCODE -ne 0 -or $output -notmatch 'records=10000,hash=([a-f0-9]+),') { throw "Invalid scan: $output" }
        if ($hash -and $hash -cne $Matches[1]) { throw 'Storage byte parity failed' }
        $hash = $Matches[1]; $checks++
        Write-Output $output
    }
}
$null = & $Bench create $root 10000 2>&1
if ($LASTEXITCODE -ne 2) { throw 'Storage benchmark overwrote existing files' }
Write-Output "$($checks + 1) storage checks passed"
