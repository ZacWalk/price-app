$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path $PSScriptRoot
Set-Location $root
$scratch = Join-Path $root 'tmp/dd-tests'
$null = New-Item -ItemType Directory -Force $scratch
$env:TEMP = $scratch; $env:TMP = $scratch; $env:TMPDIR = $scratch
$upstream = Get-Content docs/dd-upstream.json -Raw | ConvertFrom-Json -AsHashtable
$checks = 0
if ($upstream.version -ne '0.2.2' -or $upstream.revision -ne '114890d721907dee64651371947f49dd33dad27c') {
    throw 'Unexpected dd revision'
}
foreach ($name in $upstream.files.Keys) {
    $checks++
    if ((Get-FileHash $name -Algorithm SHA256).Hash -cne $upstream.files[$name]) { throw "Immutable dd changed: $name" }
}
foreach ($arguments in @(
    @('commands'), @('targets'), @('dep','list'),
    @('data','--dry-run'), @('bars','--dry-run'), @('experiment','--dry-run'), @('all','--dry-run')
)) {
    $json = & pwsh -NoProfile -File ./dd.ps1 @arguments --json --non-interactive | ConvertFrom-Json -AsHashtable
    $checks++
    if ($LASTEXITCODE -ne 0 -or -not $json.ok) { throw "dd failed: $arguments" }
    if ($arguments[0] -eq 'dep') {
        $checks++
        if (($json.data | ConvertTo-Json -Depth 20) -notmatch 'libcmlp') { throw 'dd dependency declaration hidden' }
    }
}
if ($IsWindows) {
    $json = & pwsh -NoProfile -File ./dd.ps1 app --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
    $checks++
    if (-not $json.ok -or $json.data.result.plan[0].arguments -notcontains '--offscreen' -or
        $json.data.result.plan[0].mode -ne 'run') { throw 'Viewer must be bounded/offscreen by default' }
    $json = & pwsh -NoProfile -File ./dd.ps1 app --show true --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
    $checks++
    if (-not $json.ok -or $json.data.result.plan[0].mode -ne 'launch' -or
        $json.data.result.plan[0].arguments -contains '--offscreen') { throw 'Interactive viewer must use nonblocking launch' }
    $json = & pwsh -NoProfile -File ./dd.ps1 app --show true --args '["--offscreen"]' --dry-run --json --non-interactive |
        ConvertFrom-Json -AsHashtable
    $checks++
    if (-not $json.ok -or $json.data.result.plan[0].mode -ne 'run') { throw 'Explicit offscreen must stay bounded' }
}
$declarations = Import-PowerShellDataFile ./dd.psd1
foreach ($command in @('data', 'all')) {
    $checks++
    if (($declarations.commands[$command].parameters.kind.choices -join '|') -cne
        'trades|aggTrades|klines|fundingRate|metrics') { throw "$command lost a legacy archive kind" }
    $json = & pwsh -NoProfile -File ./dd.ps1 $command --symbol BTCUSDT --market cm --kind fundingRate `
        --period daily --source 5m --from 2024-08-01 --to 2024-08-02 --force true --config debug `
        --max-rows 20000000 `
        --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
    $checks++
    if ($LASTEXITCODE -ne 0 -or -not $json.ok) { throw "$command rejected legacy download options" }
    $checks++
    if (($json.data.result.plan[0].arguments -join '|') -cne
        'download|--symbol|BTCUSDT|--market|cm|--kind|fundingRate|--period|daily|--source|5m|--from|2024-08-01|--to|2024-08-02|--max-rows|20000000|--force') {
        throw "$command did not forward the legacy download options"
    }
    $json = & pwsh -NoProfile -File ./dd.ps1 $command --fixtures= --dry-run --json --non-interactive |
        ConvertFrom-Json -AsHashtable
    $checks++
    if ($LASTEXITCODE -eq 0 -or $json.ok -or ($json.errors -join ' ') -notmatch '--fixtures requires a non-empty directory') {
        throw "$command silently accepted or dropped an empty fixture argument"
    }
    foreach ($limit in @('0', '-1', '9223372036854775808', '1.5')) {
        $json = & pwsh -NoProfile -File ./dd.ps1 $command --max-rows $limit --dry-run --json --non-interactive |
            ConvertFrom-Json -AsHashtable
        $checks++
        if ($LASTEXITCODE -eq 0 -or $json.ok -or ($json.errors -join ' ') -notmatch 'max-rows') {
            throw "$command accepted an invalid row limit: $limit"
        }
    }
}
$json = & pwsh -NoProfile -File ./dd.ps1 data --skip-checksum --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
$checks++
if ($LASTEXITCODE -eq 0 -or $json.ok) { throw 'dd accepted checksum bypass' }
$checks++
if ((Get-Content src/agents.h -Raw) -notmatch 'dqn_network online_' -or (Test-Path src/nn.cpp) -or (Test-Path src/nn.h)) {
    throw 'Local neural implementation was not replaced'
}
$checks++
if ((Get-Content src/download.cpp -Raw) -notmatch 'fg_binance_download' -or
    (Get-Content cmake/project-command.ps1 -Raw) -match 'Invoke-WebRequest|Expand-Archive|Test-Checksum') {
    throw 'Old PowerShell acquisition was not replaced'
}
$cmake = Get-Content CMakeLists.txt -Raw
foreach ($contract in @('add_library(binance_acquisition STATIC src/binance/fg_binance.c)',
    'target_link_libraries(binance_acquisition PUBLIC cfindata PRIVATE LibArchive::LibArchive OpenSSL::Crypto)',
    'target_link_libraries(crypto_cli PRIVATE crypto_core binance_acquisition)',
    'target_link_libraries(download_offline_probe PRIVATE crypto_core binance_acquisition)',
    'NAME binance COMMAND binance_tests')) {
    $checks++
    if (-not $cmake.Contains($contract)) { throw "App-owned Binance contract missing: $contract" }
}
$checks++
if ((Get-Content src/binance/fg_binance.c -Raw) -match 'fg_sources_internal|fg_sec_internal' -or
    (Get-Content src/download.cpp -Raw) -notmatch '#include "binance/fg_binance.h"' -or
    (Get-Content .gitattributes -Raw) -notmatch '/tests/fixtures/binance/\*\* -text') {
    throw 'Binance must use public neutral APIs and byte-stable app-owned fixtures'
}
# Controlled negative: a byte change in a private copy must break the same hash contract.
$copy = Join-Path $scratch ('dd-' + [Guid]::NewGuid().ToString('N') + '.ps1')
Copy-Item dd.ps1 $copy
[IO.File]::AppendAllText($copy, "`n# deliberate corruption`n")
$checks++
if ((Get-FileHash $copy).Hash -ceq $upstream.files['dd.ps1']) { throw 'Hash fault did not go red' }

$pins = Get-Content cmake/dd-dependencies.json -Raw | ConvertFrom-Json -AsHashtable
foreach ($dependency in $pins.dependencies.Keys) {
    $checks++
    if ($pins.dependencies[$dependency].commit -notmatch '^[a-f0-9]{40}$') { throw "Expected full pin: $dependency" }
}
$checks++
$acquisition = Get-Content cmake/dependencies.cmake -Raw
foreach ($filter in @('filter.lfs.process=', 'filter.lfs.smudge=', 'filter.lfs.clean=', 'filter.lfs.required=false')) {
    if (-not $acquisition.Contains($filter)) { throw 'Library acquisition must not fetch example git-lfs data or install hooks' }
}
$presets = Get-Content CMakePresets.json -Raw | ConvertFrom-Json -AsHashtable
$windows = $presets.configurePresets | Where-Object name -eq windows
$vcpkg = Get-Content vcpkg.json -Raw | ConvertFrom-Json -AsHashtable
$checks++
if ($vcpkg.'builtin-baseline' -notmatch '^[a-f0-9]{40}$' -or
    $windows.toolchainFile -ne '${sourceDir}/cmake/vcpkg-toolchain.cmake' -or
    $windows.cacheVariables.VCPKG_TARGET_TRIPLET -ne 'x64-windows-static-md' -or
    $windows.cacheVariables.VCPKG_INSTALLED_DIR -ne '${sourceDir}/vcpkg_installed') {
    throw 'Windows presets must restore pinned static acquisition dependencies with the dynamic CRT'
}
$toolchainScratch = Join-Path $root ('tmp/vcpkg-root-tests/' + [guid]::NewGuid().ToString('N'))
& cmake "-DWRAPPER=$(Join-Path $root 'cmake/vcpkg-toolchain.cmake')" "-DTEST_ROOT=$toolchainScratch" `
    -P (Join-Path $PSScriptRoot 'vcpkg-toolchain.cmake')
$checks++
if ($LASTEXITCODE -ne 0) { throw 'Stable vcpkg toolchain root checks failed' }
$workflow = Get-Content .github/workflows/ci.yml -Raw
foreach ($required in @('  push:', '  pull_request:', '  workflow_dispatch:', 'contents: read',
    'actions/checkout@v5', 'persist-credentials: false', 'windows-latest',
    '"VCPKG_ROOT=$env:VCPKG_INSTALLATION_ROOT", "CI_VCPKG_ROOT=$env:VCPKG_INSTALLATION_ROOT"',
    'dd.ps1 dep install --non-interactive', 'dd.ps1 doctor --non-interactive',
    'dd.ps1 test --label crypto --non-interactive', 'if: failure()',
    'tmp/**/dd-ctest-*.xml', 'build/**/Testing/Temporary/*.log')) {
    $checks++
    if (-not $workflow.Contains($required)) { throw "CI contract missing: $required" }
}
# Every dependency is public, so CI needs no secrets and pull requests from forks run the same checks.
$mainCheckout = [regex]::Match($workflow, '(?ms)^      - uses: actions/checkout@v5\r?\n.*?(?=^      - |\z)').Value
$checks++
if (-not $mainCheckout.Contains('persist-credentials: false') -or
    [regex]::Matches($workflow, '(?m)^\s+(?:- )?uses: actions/checkout@v5\s*$').Count -ne 1 -or
    $workflow -match '\bsecrets\b|ssh-key|CRYPTO_SOURCE_|ZacWalk/libcfindata|pull_request_target|continue-on-error|(?m)^\s+token:') {
    throw 'CI must check out only this repository, without secrets or persisted credentials'
}
$artifactPaths = [regex]::Match($workflow, '(?m)^          path: \|\r?\n((?:            [^\r\n]+\r?\n)+)').Groups[1].Value.Trim() -split '\r?\n' |
    ForEach-Object { $_.Trim() }
$allowedArtifacts = @('tmp/**/dd-logs/*.log', 'tmp/**/dd-ctest-*.xml', 'build/**/Testing/Temporary/*.log',
    'build/**/CMakeFiles/CMakeConfigureLog.yaml', 'build/**/vcpkg-manifest-install.log')
$checks++
if (Compare-Object ($artifactPaths | Sort-Object) ($allowedArtifacts | Sort-Object)) {
    throw 'CI artifacts must contain only approved diagnostics'
}
$checks++
if ($acquisition -match '\$ENV\{CRYPTO_SOURCE_') {
    throw 'Dependency source overrides must be explicit CMake values, never seeded from the environment'
}
$restore = [regex]::Match($workflow, '(?ms)^      - name: Restore pinned Windows acquisition dependencies\r?\n.*?(?=^      - |\z)').Value
foreach ($required in @('timeout-minutes: 30', '& (Join-Path $env:VCPKG_ROOT ''vcpkg.exe'') install',
    ('--triplet=' + $windows.cacheVariables.VCPKG_TARGET_TRIPLET),
    '"--x-manifest-root=$PWD"', '"--x-install-root=$(Join-Path $PWD ''vcpkg_installed'')"',
    '$PSNativeCommandUseErrorActionPreference = $false', 'Tee-Object', 'vcpkg-restore.log',
    '(Get-Content -LiteralPath ''vcpkg.json'' -Raw | ConvertFrom-Json).''builtin-baseline''',
    '$baseline -isnot [string] -or $baseline -cnotmatch ''^[0-9a-f]{40}$''',
    '& git -C $env:VCPKG_ROOT fetch --no-tags https://github.com/microsoft/vcpkg.git $baseline',
    'if ($LASTEXITCODE -ne 0) { throw "Pinned vcpkg baseline fetch failed',
    '& git -C $env:VCPKG_ROOT cat-file -e "${baseline}:versions/baseline.json"',
    'if ($LASTEXITCODE -ne 0) { throw "Pinned vcpkg baseline object is unavailable',
    'vcpkg-baseline.log',
    'if ($LASTEXITCODE -ne 0) { throw "Pinned vcpkg restore failed')) {
    $checks++
    if (-not $restore.Contains($required)) { throw "Bounded vcpkg restore contract missing: $required" }
}
$checks++
if ([regex]::Match($restore, '--triplet=([a-z0-9-]+)').Groups[1].Value -cne $windows.cacheVariables.VCPKG_TARGET_TRIPLET -or
    $workflow.IndexOf('Restore pinned Windows acquisition dependencies') -gt $workflow.IndexOf('dd.ps1 test --label crypto --non-interactive') -or
    $restore.IndexOf('fetch --no-tags') -gt $restore.IndexOf('cat-file -e') -or
    $restore.IndexOf('cat-file -e') -gt $restore.IndexOf("'vcpkg.exe') install") -or
    $restore -match '--keep-going|cat-file -p|(?m)& git[^\r\n]*\s(pull|checkout|reset)\b') {
    throw 'The pinned vcpkg restore must complete before dd builds either configuration'
}
$checks++
if ($workflow -match 'ubuntu-|macos-|dd\.ps1 test(?! --label crypto --non-interactive)|--show') {
    throw 'App CI must remain Windows-only, offscreen and diagnostics-only'
}
$badge = 'https://github.com/ZacWalk/price-app/actions/workflows/ci.yml'
$readme = Get-Content README.md -Raw
$checks++
if (-not $readme.Contains("($badge/badge.svg)]($badge)")) { throw 'Incorrect CI README badge' }
# The README lists every pinned revision, so a repin that leaves it behind fails here.
foreach ($revision in @($pins.dependencies.Values | ForEach-Object { $_.commit }) + @($vcpkg.'builtin-baseline', $upstream.revision)) {
    $checks++
    if (-not $readme.Contains('`' + $revision.Substring(0, 7) + '`')) {
        throw "README dependencies do not name pinned revision $($revision.Substring(0, 7))"
    }
}
$drivers = @(Get-ChildItem -LiteralPath $root -File -Filter '*.ps1')
$checks++
if ($drivers.Count -ne 1 -or $drivers[0].Name -ne 'dd.ps1' -or (Test-Path dd.sh)) {
    throw 'Only immutable dd may remain as a root driver'
}
foreach ($path in @('dd.ps1', 'tests/dd-adoption.ps1') + @($declarations.commands.Values.script | Sort-Object -Unique)) {
    $tokens = $null; $parseErrors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root $path), [ref]$tokens, [ref]$parseErrors)
    $checks++
    if ($parseErrors.Count) { throw "PowerShell parse failed: $path : $parseErrors" }
}
$rejected = & pwsh -NoProfile -File ./dd.ps1 build invalid --json --non-interactive | ConvertFrom-Json
$checks++
if ($LASTEXITCODE -ne 2 -or $rejected.ok -or $rejected.exitCode -ne 2 -or
    ($rejected.errors -join ' ') -notmatch 'Expected debug, release or both') {
    throw 'An invalid configuration must fail before configure/build'
}
$json = & pwsh -NoProfile -File ./dd.ps1 bars --format ohlcv --in prices.csv --out prices.bin `
    --symbol TEST --interval 15m --calendar XNYS --currency USD --source-name local-fixture --price-basis adjusted `
    --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
$checks++
if ($LASTEXITCODE -ne 0 -or -not $json.ok -or ($json.data.result.plan[0].arguments -join '|') -cne
    'bars|--in|prices.csv|--out|prices.bin|--interval|15m|--symbol|TEST|--format|ohlcv|--calendar|XNYS|--currency|USD|--source-name|local-fixture|--price-basis|adjusted') {
    throw 'Generic bar metadata did not reach the native CLI'
}
foreach ($missing in @('symbol', 'interval', 'calendar', 'currency')) {
    $arguments = @('bars', '--format', 'ohlcv', '--in', 'prices.csv', '--out', 'prices.bin')
    foreach ($pair in @(@('symbol', 'TEST'), @('interval', '15m'), @('calendar', 'XNYS'), @('currency', 'USD'))) {
        if ($pair[0] -ne $missing) { $arguments += @("--$($pair[0])", $pair[1]) }
    }
    $json = & pwsh -NoProfile -File ./dd.ps1 @arguments --dry-run --json --non-interactive | ConvertFrom-Json -AsHashtable
    $checks++
    if ($LASTEXITCODE -eq 0 -or $json.ok -or ($json.errors -join ' ') -notmatch "--$missing") {
        throw "Generic bars silently defaulted --$missing"
    }
}
Write-Output "$checks dd adoption checks passed; $($upstream.files.Count) immutable upstream hashes"
