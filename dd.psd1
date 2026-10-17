@{
    schema = 1
    project = @{ name = 'price-app'; type = 'cli'; 'default-target' = 'cli' }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{ debug = 'windows-debug'; release = 'windows-release' }
        'x64-linux' = @{ debug = 'linux-debug'; release = 'linux-release' }
    }
    targets = @(
        @{ id = 'cli'; kind = 'cli'; 'cmake-target' = 'crypto_cli'; 'test-label' = 'crypto'
           'debug-path' = 'exe/cli-d{exe}'; 'release-path' = 'exe/cli{exe}' },
        @{ id = 'app'; kind = 'gui'; 'cmake-target' = 'crypto_app'; 'test-label' = 'crypto'
           'debug-path' = 'exe/app-d{exe}'; 'release-path' = 'exe/app{exe}'; platforms = @('x64-windows') }
    )
    commands = @{
        data = @{
            description = 'Explicit Binance archive acquisition via the linked C client. Network unless --fixtures.'
            script = 'cmake/project-command.ps1'; effects = 'write'; 'supports-dry-run' = $true; 'timeout-secs' = 7200
            parameters = @{
                symbol = @{ type = 'string'; default = 'ETHUSDT' }
                market = @{ type = 'string'; default = 'um'; choices = @('um', 'cm', 'spot') }
                kind = @{ type = 'string'; default = 'trades'; choices = @('trades', 'aggTrades', 'klines', 'fundingRate', 'metrics') }
                period = @{ type = 'string'; default = 'auto'; choices = @('auto', 'daily', 'monthly') }
                source = @{ type = 'string'; default = '1m' }
                'max-rows' = @{ type = 'integer'; description = 'Positive typed rows per archive; CLI default 10000000.' }
                from = @{ type = 'string'; default = '2024-07-22' }
                to = @{ type = 'string'; default = '2024-07-26' }
                out = @{ type = 'string' }; fixtures = @{ type = 'string' }
                force = @{ type = 'boolean' }; 'skip-missing' = @{ type = 'boolean' }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
        bars = @{
            description = 'Import generic OHLCV (SPQB v2) or aggregate Binance trade/kline CSVs (SPQB v1). Offline.'
            script = 'cmake/project-command.ps1'; effects = 'write'; 'supports-dry-run' = $true; 'timeout-secs' = 7200
            parameters = @{
                in = @{ type = 'string'; default = 'data/raw' }; out = @{ type = 'string' }
                symbol = @{ type = 'string' }; interval = @{ type = 'string' }
                format = @{ type = 'string'; choices = @('binance', 'ohlcv') }
                calendar = @{ type = 'string' }; currency = @{ type = 'string' }
                'source-name' = @{ type = 'string' }
                'price-basis' = @{ type = 'string'; choices = @('raw', 'adjusted') }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
        experiment = @{
            description = 'Legacy run: walk-forward experiment, default bars selected by symbol/interval. Offline; control failure is exit 1.'
            script = 'cmake/project-command.ps1'; effects = 'write'; 'supports-dry-run' = $true; 'timeout-secs' = 9999
            parameters = @{
                args = @{ type = 'string'; default = '[]'; description = 'JSON array of CLI tokens, e.g. ["--agent","qtable"].' }
                symbol = @{ type = 'string'; default = 'ETHUSDT' }; interval = @{ type = 'string'; default = '10s' }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
        app = @{
            description = 'Read-only viewer on the newest run. Offscreen by default; --show true explicitly opens a window.'
            script = 'cmake/project-command.ps1'; effects = 'read'; 'supports-dry-run' = $true; 'timeout-secs' = 120
            platforms = @('x64-windows')
            parameters = @{
                args = @{ type = 'string'; default = '[]' }; show = @{ type = 'boolean'; default = $false }
                symbol = @{ type = 'string'; default = 'ETHUSDT' }; interval = @{ type = 'string'; default = '10s' }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
        all = @{
            description = 'Legacy pipeline: build, explicit download, bars, experiment, then offscreen viewer. Network command.'
            script = 'cmake/project-command.ps1'; effects = 'write'; 'supports-dry-run' = $true; 'timeout-secs' = 9999
            parameters = @{
                args = @{ type = 'string'; default = '[]' }
                symbol = @{ type = 'string'; default = 'ETHUSDT' }; interval = @{ type = 'string'; default = '10s' }
                market = @{ type = 'string'; default = 'um'; choices = @('um', 'cm', 'spot') }
                kind = @{ type = 'string'; default = 'trades'; choices = @('trades', 'aggTrades', 'klines', 'fundingRate', 'metrics') }
                period = @{ type = 'string'; default = 'auto'; choices = @('auto', 'daily', 'monthly') }
                source = @{ type = 'string'; default = '1m' }
                'max-rows' = @{ type = 'integer'; description = 'Positive typed rows per archive; CLI default 10000000.' }
                from = @{ type = 'string'; default = '2024-07-22' }; to = @{ type = 'string'; default = '2024-07-26' }
                fixtures = @{ type = 'string' }
                force = @{ type = 'boolean' }; 'skip-missing' = @{ type = 'boolean' }
                config = @{ type = 'string'; default = 'release'; choices = @('debug', 'release') }
            }
        }
    }
}
