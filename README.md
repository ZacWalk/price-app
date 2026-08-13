# spq

A reinforcement learning bench for one question: can an agent learn, from
ETH/USD trade data alone, when to be long and when to be short, and keep the
edge after costs?

Self-contained C++20 and Win32. No package manager, no third-party libraries, no
network access at runtime.

**The measured answer so far is no edge.** On 5 days of ETHUSDT futures at
ten-second bars, the DQN is statistically indistinguishable from the same policy
trading a shuffled return stream, and every agent that trades loses to sitting
flat once 5 bps of cost is applied. Full numbers in
[docs/design.md §10](docs/design.md#10-results).

**[docs/design.md](docs/design.md) is the design contract** — data pipeline, RL
formulation, causality guarantees, evaluation methodology, and what is and is
not built. Read it before changing anything.

## Two binaries

| Binary | Job |
|---|---|
| `exe/cli.exe` | **Runs experiments.** Builds bars, trains, evaluates, writes `runs/`. Headless and deterministic. |
| `exe/app.exe` | **Views data and results.** Read-only Win32 viewer for bars, positions and equity curves. Never trains. |

Opening the viewer never changes a number. Producing a number never requires
opening the viewer.

## Quickstart

```powershell
.\dd.ps1 all
```

That downloads 5 days of ETHUSDT trades, builds both projects, aggregates
ten-second bars, runs a DQN experiment, and opens the viewer on the result.

The steps are also available individually:

```powershell
.\dd.ps1 data  -Symbol ETHUSDT -From 2024-07-22 -To 2024-07-26
.\dd.ps1 build -Config Release
.\dd.ps1 bars  -Interval 10s
.\dd.ps1 run   -- --agent qtable --seeds 5
.\dd.ps1 app
.\dd.ps1 clean
```

Run `.\dd.ps1 help` for the full parameter list.

## Running experiments directly

```powershell
# aggregate raw trades into bars
.\exe\cli.exe bars --in data\raw --out data\bars\ETHUSDT-10s.bin --interval 10s --symbol ETHUSDT

# prove no feature reads the future
.\exe\cli.exe audit --bars data\bars\ETHUSDT-10s.bin

# walk-forward train and evaluate, with baselines, cost curve and controls
.\exe\cli.exe walk --bars data\bars\ETHUSDT-10s.bin --agent dqn --seeds 5 `
    --train 12000 --test 4000 --gap 500 --episode 1440 --epochs 6 --cost-bps 5

.\exe\cli.exe list
.\exe\cli.exe report --run <id>
```

Agents: `flat`, `long`, `short`, `random`, `crossover`, `qtable`, `dqn`.

Every run writes `runs/<id>/` containing `manifest.txt`, `metrics.csv`,
`equity.csv`, `learning.csv` and the policy — all plain text except the weights.

## Viewing results

```powershell
.\exe\app.exe --bars data\bars\ETHUSDT-10s.bin --run runs\<id>
```

Price with the agent's position as a coloured band underneath, the equity curve
below that, and a mini-map of the whole series at the bottom. Drag or
middle-drag to pan, wheel to zoom, shift-wheel to scroll, click the mini-map to
jump. Per-monitor DPI aware, so it is sharp on scaled displays.

## Requirements

- Windows, PowerShell 5.1 or later
- Visual Studio 2022+ with the C++ desktop workload

Nothing else. `dd.ps1` locates MSBuild via `vswhere`, and downloading uses
built-in PowerShell cmdlets.

## Layout

```
docs/design.md   the contract
AGENTS.md        rules for AI agents
dd.ps1           download data, build, run
src/             all source — no external includes
data/            downloaded and derived data (gitignored)
runs/            experiment output, one directory per run (gitignored)
exe/             build output (gitignored)
```

Only source and documents are committed.
