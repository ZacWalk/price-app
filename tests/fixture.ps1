param([string]$Output = 'tmp/p5/bars.bin', [int]$Count = 16000)
$ErrorActionPreference = 'Stop'
$path = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path (Split-Path $PSScriptRoot) $Output }
$null = New-Item -ItemType Directory -Force (Split-Path $path)
$file = [IO.BinaryWriter]::new([IO.File]::Create($path))
try {
    $file.Write([Text.Encoding]::ASCII.GetBytes('SPQB'))
    $file.Write([uint32]1)
    $file.Write([long]900000)
    $file.Write([long]$Count)
    $file.Write([uint32]7)
    $file.Write([Text.Encoding]::ASCII.GetBytes('ETHUSDT'))
    $last = 1000.0
    for ($i = 0; $i -lt $Count; $i++) {
        $close = 1000.0 + $i * 0.01 + 20 * [Math]::Sin($i * 0.017) + 5 * [Math]::Sin($i * 0.31)
        $file.Write([long](1577836800000L + $i * 900000L))
        foreach ($value in @($last, ([Math]::Max($last, $close) + 0.5), ([Math]::Min($last, $close) - 0.5), $close, (100 + $i % 37), ($close * (100 + $i % 37)), (40 + $i % 31))) {
            $file.Write([double]$value)
        }
        $file.Write([int](20 + $i % 17))
        $file.Write([int]0)
        $last = $close
    }
} finally { $file.Dispose() }
