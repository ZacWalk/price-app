<#
.SYNOPSIS
    spq - download data, build, run.

.DESCRIPTION
    Single entry point for the project. See docs/design.md for the design
    contract and AGENTS.md for the rules this script enforces.

    Zero dependencies: uses only built-in PowerShell cmdlets and the Visual
    Studio C++ toolchain. Nothing is installed.

.EXAMPLE
    .\dd.ps1 all
    .\dd.ps1 data -From 2024-07-23 -To 2024-07-25
    .\dd.ps1 build -Config Release
    .\dd.ps1 run -- --config configs/dqn.txt --seed 7
    .\dd.ps1 app
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('help', 'data', 'build', 'bars', 'run', 'app', 'clean', 'all')]
    [string] $Command = 'help',

    # --- data ---
    [string] $Symbol = 'ETHUSDT',
    [ValidateSet('um', 'cm', 'spot')]
    [string] $Market = 'um',
    [ValidateSet('trades', 'aggTrades', 'klines')]
    [string] $Kind = 'trades',
    [string] $From = '2024-07-22',
    [string] $To = '2024-07-26',
    [switch] $Force,
    [switch] $SkipChecksum,

    # --- bars ---
    [string] $Interval = '10s',

    # --- build ---
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',
    [ValidateSet('x64', 'Win32')]
    [string] $Platform = 'x64',

    # --- passthrough to cli.exe / app.exe ---
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = $PSScriptRoot
$SrcDir = Join-Path $Root 'src'
$ExeDir = Join-Path $Root 'exe'
$DataDir = Join-Path $Root 'data'
$RawDir = Join-Path $DataDir 'raw'
$BarsDir = Join-Path $DataDir 'bars'
$ZipDir = Join-Path $DataDir 'zip'
$RunsDir = Join-Path $Root 'runs'
$Solution = Join-Path $Root 'spq.sln'

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Info($msg) { Write-Host "    $msg" -ForegroundColor DarkGray }
function Write-Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }
function Write-Good($msg) { Write-Host "    $msg" -ForegroundColor Green }

function Show-Help {
    @"
spq - download data, build, run.

USAGE
    .\dd.ps1 <command> [options]

COMMANDS
    data     Download and unzip market data into data/raw
    build    Build both projects into exe/
    bars     Aggregate data/raw into a bar file
    run      Run an experiment with cli.exe
    app      Open the viewer on the newest run
    clean    Remove exe/ and intermediate/
    all      data, build, bars, run, then app
    help     This message

DATA OPTIONS
    -Symbol <s>      Trading pair.               Default ETHUSDT
    -Market <m>      um | cm | spot.             Default um
    -Kind <k>        trades | aggTrades | klines. Default trades
    -From <date>     First day, yyyy-MM-dd.      Default 2024-07-22
    -To <date>       Last day, inclusive.        Default 2024-07-26
    -Force           Re-download files already present
    -SkipChecksum    Do not verify the published SHA256

BAR OPTIONS
    -Interval <d>    Bar width, e.g. 1s 10s 1m.  Default 10s

BUILD OPTIONS
    -Config <c>      Debug | Release.            Default Release
    -Platform <p>    x64 | Win32.                Default x64

PASSTHROUGH
    Anything after -- is forwarded to the binary:
        .\dd.ps1 run -- --agent qtable --seeds 5

SEE ALSO
    docs\design.md   design contract
    AGENTS.md        rules for AI agents
"@ | Write-Host
}

# ---------------------------------------------------------------- data ------

function Get-DataUrlPrefix {
    if ($Market -eq 'spot') { return "data/spot/daily/$Kind/$Symbol" }
    return "data/futures/$Market/daily/$Kind/$Symbol"
}

function Get-DayList {
    $start = [datetime]::ParseExact($From, 'yyyy-MM-dd', $null)
    $endText = if ([string]::IsNullOrWhiteSpace($To)) { $From } else { $To }
    $end = [datetime]::ParseExact($endText, 'yyyy-MM-dd', $null)

    if ($end -lt $start) { throw "-To ($endText) is before -From ($From)." }

    $days = @()
    for ($d = $start; $d -le $end; $d = $d.AddDays(1)) {
        $days += $d.ToString('yyyy-MM-dd')
    }
    return $days
}

function Test-Checksum($zipPath, $checksumUrl) {
    $expected = $null
    try {
        $content = (Invoke-WebRequest -Uri $checksumUrl -UseBasicParsing -TimeoutSec 60).Content
        # .CHECKSUM is served as binary, so Content may be a byte array.
        $text = if ($content -is [byte[]]) { [System.Text.Encoding]::ASCII.GetString($content) } else { [string] $content }
        $expected = ($text.Trim() -split '\s+')[0]
    }
    catch {
        Write-Warn "no checksum published, skipping verification"
        return $true
    }

    $actual = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
    if ($actual -ieq $expected) { return $true }

    Write-Warn "checksum mismatch: expected $expected, got $actual"
    return $false
}

function Invoke-Data {
    Write-Step "Downloading $Symbol $Kind ($Market)"

    New-Item -ItemType Directory -Force -Path $RawDir, $ZipDir, $BarsDir | Out-Null

    $prefix = Get-DataUrlPrefix
    $days = Get-DayList
    $ok = 0
    $skipped = 0
    $missing = 0

    # Invoke-WebRequest is an order of magnitude faster without the progress UI.
    $previousProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'

    try {
        foreach ($day in $days) {
            $name = "$Symbol-$Kind-$day"
            $zipPath = Join-Path $ZipDir "$name.zip"
            $csvPath = Join-Path $RawDir "$name.csv"

            if ((Test-Path $csvPath) -and -not $Force) {
                Write-Info "$name.csv already present"
                $skipped += 1
                continue
            }

            $url = "https://data.binance.vision/$prefix/$name.zip"

            try {
                Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing -TimeoutSec 600
            }
            catch {
                # 404 is normal: no file is published for days the pair did not trade.
                Write-Warn "$name not available"
                if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
                $missing += 1
                continue
            }

            if (-not $SkipChecksum) {
                if (-not (Test-Checksum $zipPath "$url.CHECKSUM")) {
                    Remove-Item $zipPath -Force
                    $missing += 1
                    continue
                }
            }

            Expand-Archive -Path $zipPath -DestinationPath $RawDir -Force
            Remove-Item $zipPath -Force

            if (Test-Path $csvPath) {
                $mb = [math]::Round((Get-Item $csvPath).Length / 1MB, 1)
                Write-Good "$name.csv  ${mb} MB"
                $ok += 1
            }
            else {
                Write-Warn "$name unzipped but no matching csv found"
                $missing += 1
            }
        }
    }
    finally {
        $ProgressPreference = $previousProgress
    }

    Write-Info "downloaded $ok, already had $skipped, unavailable $missing"
    if ($ok -eq 0 -and $skipped -eq 0) {
        throw "No data retrieved. Check -Symbol, -Market, -Kind and the date range."
    }
}

# --------------------------------------------------------------- build ------

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -prerelease `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($path -and (Test-Path $path)) { return $path }
    }

    $onPath = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "MSBuild not found. Install Visual Studio with the 'Desktop development with C++' workload."
}

function Invoke-Build {
    Write-Step "Building $Config|$Platform"

    $msbuild = Find-MSBuild
    Write-Info ([System.IO.Path]::GetFileName($msbuild))

    & $msbuild $Solution `
        /nologo `
        /verbosity:minimal `
        /maxcpucount `
        "/p:Configuration=$Config" `
        "/p:Platform=$Platform"

    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

    Get-ChildItem $ExeDir -Filter *.exe -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Good $_.Name }
}

# ----------------------------------------------------------- run / app ------

function Get-Binary($debugName, $releaseName) {
    $name = if ($Config -eq 'Debug') { $debugName } else { $releaseName }
    $path = Join-Path $ExeDir $name

    if (-not (Test-Path $path)) {
        Write-Info "$name not built yet"
        Invoke-Build
    }
    if (-not (Test-Path $path)) { throw "$name still missing after build." }

    return $path
}

function Get-PassthroughArgs {
    if ($null -eq $Rest) { return @() }
    # Strip a leading -- separator so `run -- --seed 7` works.
    return @($Rest | Where-Object { $_ -ne '--' })
}

function Get-BarsPath {
    return Join-Path $BarsDir "$Symbol-$Interval.bin"
}

function Invoke-Bars {
    Write-Step "Building $Interval bars"

    $exe = Get-Binary 'cli-d.exe' 'cli.exe'
    $bars = Get-BarsPath

    & $exe bars --in $RawDir --out $bars --interval $Interval --symbol $Symbol
    if ($LASTEXITCODE -ne 0) { throw "bar build failed with exit code $LASTEXITCODE." }
}

function Invoke-Run {
    Write-Step 'Running experiment'

    $exe = Get-Binary 'cli-d.exe' 'cli.exe'
    $bars = Get-BarsPath

    if (-not (Test-Path $bars)) { Invoke-Bars }
    New-Item -ItemType Directory -Force -Path $RunsDir | Out-Null

    $cliArgs = Get-PassthroughArgs
    if ($cliArgs -notcontains '--bars') { $cliArgs = @('--bars', $bars) + $cliArgs }
    if ($cliArgs -notcontains '--agent') { $cliArgs = $cliArgs + @('--agent', 'dqn') }

    Write-Info "cli walk $($cliArgs -join ' ')"

    & $exe walk @cliArgs
    if ($LASTEXITCODE -ne 0) { Write-Warn "cli exited with code $LASTEXITCODE" }
}

function Invoke-App {
    Write-Step 'Opening viewer'

    $exe = Get-Binary 'app-d.exe' 'app.exe'
    $appArgs = Get-PassthroughArgs

    if ($appArgs.Count -eq 0) {
        $bars = Get-BarsPath
        if (Test-Path $bars) { $appArgs = @('--bars', $bars) }

        $newest = Get-ChildItem $RunsDir -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'equity.csv') } |
            Sort-Object LastWriteTime | Select-Object -Last 1

        if ($newest) {
            $appArgs += @('--run', "runs/$($newest.Name)")
            Write-Info "run $($newest.Name)"
        }
    }

    Start-Process -FilePath $exe -ArgumentList $appArgs -WorkingDirectory $Root
}

# --------------------------------------------------------------- clean ------

function Invoke-Clean {
    Write-Step 'Cleaning build output'

    foreach ($dir in @($ExeDir, (Join-Path $Root 'intermediate'))) {
        if (Test-Path $dir) {
            Remove-Item $dir -Recurse -Force
            Write-Info "removed $([System.IO.Path]::GetFileName($dir))"
        }
    }
    Write-Info "data/ and runs/ left alone - delete them by hand if you mean it"
}

# ------------------------------------------------------------ dispatch ------

switch ($Command) {
    'help' { Show-Help }
    'data' { Invoke-Data }
    'build' { Invoke-Build }
    'bars' { Invoke-Bars }
    'run' { Invoke-Run }
    'app' { Invoke-App }
    'clean' { Invoke-Clean }
    'all' {
        Invoke-Data
        Invoke-Build
        Invoke-Bars
        Invoke-Run
        Invoke-App
    }
}
