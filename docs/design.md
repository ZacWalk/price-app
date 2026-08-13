# spq — design

spq answers one question: can an agent learn, from ETH/USD trade data alone,
when to be long and when to be short, and keep the edge after costs?

This document describes the system as it is. Where the code and this document
disagree, this document is the contract and the code is wrong.
[Appendix A](#appendix-a--gaps-in-the-original-prototype) records what the
original prototype got wrong, because that is why several of these rules exist.

The measured answer, on the data tested so far, is **no edge**. See
[section 10](#10-results).

## 1. Scope

Two binaries, one job each.

- `cli.exe` **runs experiments.** It is the only thing that builds bars, trains,
  evaluates or writes results. Headless, deterministic, scriptable.
- `app.exe` **views data and results.** It reads bars and a run directory and
  draws them. It never trains and never writes.

That split is what makes results trustworthy: you cannot produce a number by
clicking around, and you cannot invalidate one by looking at it.

### 1.1 Non-goals

- Live trading, order routing, exchange connectivity.
- Being a general ML framework. The learner serves this one problem.
- Portfolio construction across many assets.
- GPU training. Everything runs on one CPU in minutes.

## 2. Principles

**Zero dependencies.** C++20, the standard library, and Win32. Nothing else
links in. No package manager, no vendored library, no JSON parser, no charting
library, no HTTP client. CSV parsing, the optimiser, the metrics and the chart
renderer are all in `src/`.

**Causality is enforced, not assumed.** No feature, label or reward may read a
bar later than the one it is computed for. Section 5 describes how that is
structurally guaranteed and how it is tested.

**Reproducible by construction.** All randomness comes from a seeded generator
written out in `common.h`, never `std::random_device` and never a standard
distribution — those are not specified bit-exactly. Release builds use
`/fp:precise`. Every run records its config, seed and a hash of the bars it
consumed.

**Text-first artifacts.** Results are CSV and `key=value` text: diffable,
greppable, readable in a decade. Only network weights are binary, and they carry
a versioned header.

## 3. Repository layout

```
docs/design.md          this document — the contract
README.md               quickstart, delegates here
AGENTS.md               rules for AI agents, delegates here
dd.ps1                  download data, build, run

src/                    all source, no external includes
  common.h/.cpp         types, seeded rng, hashing, string and time helpers
  bars.h/.cpp           trade CSV -> bars, binary bar file, data hash
  features.h/.cpp       streaming causal indicators, feature matrix
  env.h/.cpp            the MDP: state, actions, reward, cost model
  nn.h/.cpp             the MLP: forward, backward, Adam, save/load
  agents.h/.cpp         baselines, rule agent, tabular Q, DQN
  metrics.h/.cpp        per-fold performance measurement
  run.h/.cpp            run directory artifacts
  experiment.h/.cpp     walk-forward loop, baselines, controls
  cli.cpp               experiment runner (console)
  app.cpp               viewer (Win32)
  app.manifest          per-monitor v2 DPI awareness
  cli.vcxproj           -> exe/cli.exe, exe/cli-d.exe
  app.vcxproj           -> exe/app.exe, exe/app-d.exe

data/                   downloaded and derived data, gitignored
  raw/ETHUSDT-trades-YYYY-MM-DD.csv
  bars/ETHUSDT-10s.bin

runs/                   one directory per experiment, gitignored
  <run-id>/
    manifest.txt        config, seed, data hash, fold layout
    metrics.csv         one row per variant, seed, cost level and fold
    equity.csv          time, price, position, gross, cost, equity
    learning.csv        episode, mean reward, epsilon, loss
    weights.bin         policy parameters (policy.csv for the tabular agent)

exe/                    build output, gitignored
intermediate/           build intermediates, gitignored
```

Only source and documents are committed.

## 4. Data

### 4.1 Source

Binance public data, USD-M futures daily trade files from `data.binance.vision`.
Each row is one executed trade:

```
id,price,qty,quote_qty,time,is_buyer_maker
4225178877,3534.3,0.48,1696.464,1721606402850,false
```

`is_buyer_maker=false` means the buyer was the aggressor, which gives take-side
for free and is a real microstructure signal.

`dd.ps1 data` downloads, verifies the published SHA256 and unzips into
`data/raw/`. Downloading is the only thing that touches the network, and it is a
separate explicit step — no binary ever fetches anything.

### 4.2 Trades are not the modelling unit

Raw trades arrive irregularly, and the arrival rate is itself information.
Indexing by trade *count* silently changes the time scale between quiet and busy
periods, so "a 733-trade moving average" means something different at 03:00 than
at 14:30. The pipeline is therefore:

```
raw trades  ->  bars  ->  features  ->  agent
```

`cli bars` streams the CSVs once and aggregates into fixed-interval bars: open,
high, low, close, base volume, quote volume, aggressor buy volume and trade
count. Intervals with no trades are emitted as filled bars carrying the previous
close forward, so the time axis stays regular. Bars are written to a compact
binary file with a header; no later stage re-parses CSV.

Volume and dollar bars would normalise information arrival better than clock
time. Not built.

### 4.3 Features

Thirteen features per bar, all causal and all scale-free — the agent must not be
able to tell $3,700 ETH from $1,800 ETH, or it will memorise the regime instead
of learning the behaviour.

| # | Name | Form |
|---|---|---|
| 0–4 | `ret_1` … `ret_300` | `log(c_t / c_{t-k})` for k in {1,5,15,60,300}, divided by realised vol × √k |
| 5 | `trend_fast` | `(ema_12 − ema_48) / atr` |
| 6 | `trend_slow` | `(ema_48 − ema_192) / atr` |
| 7 | `vol_ratio` | `log(rv_60 / rv_1440)` |
| 8 | `range_pos` | position of close in its 300-bar range, centred on zero |
| 9 | `flow` | `(2 × aggressor buys − volume) / volume` over 60 bars |
| 10 | `activity` | trade count over 60 bars against its long-run rate |
| 11–12 | `tod_sin`, `tod_cos` | time of day |

Features 0–7, 9 and 10 are then standardised with **expanding** statistics
(Welford, past only) and clamped to ±5. Bounded and periodic columns are left
alone. Warmup is 1,740 bars; earlier rows are not usable.

The environment appends three more columns at decision time: current position,
unrealised pnl in ATR units, and bars held. An agent that cannot see its own
position cannot learn to hold one, and will churn.

## 5. Causality

Getting this wrong once invalidates every number the project has produced, and
it is invisible in the output. It is defended three ways.

**Streaming-only computation.** Every indicator in `features.h` is an object
with `push(x) -> value`. It owns its state and never holds the series, so it is
structurally incapable of indexing forwards. A centred window
(`sum -= x[i-r]; sum += x[i+r]`) cannot be expressed in that interface. That
centred window was the original bug.

**Expanding normalisation.** Whole-file mean and variance would leak the future.
`expanding_stats` only ever sees the past.

**A shift audit.** `cli audit` recomputes every feature over a series truncated
at `t` and requires the value at `t` to be identical to the full-series value.
It probes eight points and exits non-zero on any disagreement.

The audit has been verified to fail: injecting a deliberate one-bar shift made
all eight probes report `LOOK-AHEAD`, with a largest disagreement of 2.62 in
`ret_1`. A test that has never been seen to fail is not evidence.

## 6. The reinforcement learning problem

### 6.1 Formulation

- **State** — the 13 features at bar `t` plus the three position columns.
- **Action** — `{short, flat, long}`, the target position for the next bar.
  Three actions rather than two, because "no opinion" is a real answer and
  forcing a side manufactures turnover.
- **Reward**

  ```
  r_t = pos_t · log(c_{t+1} / c_t) − cost · |pos_t − pos_{t-1}| − λ · |pos_t|
  ```

  where `cost` is half the round-trip basis points, so flat→long→flat pays the
  full round trip and long→short also pays one round trip. `λ` is an optional
  holding penalty, default zero.

- **Episode** — a contiguous slice of bars, default 3,600. Episodes start flat
  and are forced flat at the end, so no unrealised position is ever banked.
- **Discount** — 0.99 by default.

The decision at bar `t` is made from information up to bar `t`'s close and earns
the return from `t` to `t+1`. That ordering is the causality guarantee at the
environment level.

Rewarding log return rather than raw pnl keeps rewards additive and
scale-invariant. Rewards are scaled by 10,000 inside the learners so Q-values
are O(1) rather than O(1e-5); reported metrics stay in true units.

### 6.2 Why RL rather than supervised classification

A supervised "will the next bar be up?" classifier optimises a proxy. It has no
notion of transaction cost, cannot express "hold through this noise", and treats
a 0.01% move and a 2% move as the same label. Risk-adjusted return after costs,
given that you are already in a position, is exactly a sequential decision
problem. The reward *is* the objective.

The honest counterpoint: RL is sample-hungry and unstable, and the signal is
faint. That is why the baselines stay in the suite permanently.

### 6.3 Agents

| Agent | What it is |
|---|---|
| `flat`, `long`, `short` | Constant position. The bars most strategies quietly fail to clear. |
| `random` | Flips to a random action with probability 0.02 per bar. The cost floor. |
| `crossover` | Long above `trend_fast > 0.5`, short below `−0.5`, hold inside the dead band. |
| `qtable` | Tabular Q-learning over 225 discretised states (trend × 60-bar return × vol × position). Small enough that `policy.csv` prints the whole learned policy. |
| `dqn` | Double DQN: experience replay (200k), target network synced every 2,000 steps, ε-greedy 1.0→0.05, Huber loss, gradient clipping, Adam. |

The rule is to earn complexity: each rung must beat the one below it
out-of-sample before the next is worth writing. The linear-Q rung between
`qtable` and `dqn` was skipped, and is worth revisiting only if the DQN ever
beats the tabular agent.

### 6.4 The network

`mlp` in `nn.h` is a fully connected network with ReLU hidden layers, **linear
outputs** (Q-values are unbounded; a sigmoid would cap them at 1), real bias
parameters, He initialisation, Adam, global-norm gradient clipping and versioned
`save`/`load`. Tensors are flat `std::vector<xfloat>`; there are no owning raw
pointers.

## 7. Evaluation

**Walk-forward, purged.** A train window and a test window roll forward through
time with a gap between them, so no training bar shares a feature window with a
test bar. Every fold is reported separately as well as pooled.

**Every variant is evaluated inside the fold loop.** The cost curve and the
controls see the policy at exactly the stage of learning the headline saw.
Evaluating the cost curve once at the end with the final policy is wrong; it
produced visibly inconsistent trade counts before it was fixed.

**Metrics per fold**, all after costs: net and gross log return, cost paid, cost
as a share of gross, annualised return, Sharpe, Sortino, max drawdown, turnover,
hit rate, average win and loss, and time in market by side.

Cost-paid-as-a-share-of-gross is the number that kills most high-frequency
ideas, so it is on the summary line rather than buried.

**Cost sensitivity.** Every run reports 1, 2, 5 and 10 bps round trip. A result
that only survives at zero cost is a data artifact.

**Seeds.** The learner runs over several seeds and the summary reports the
median. Single-seed results on financial data are close to noise.

**Controls.** A shuffled-return control runs alongside every experiment: the
same policy, the same costs, the returns permuted. If it scores as well as the
real thing, there is no edge. `cli walk` exits non-zero if the control beats the
real run outright.

Not built: the reversed-signal control, a turnover-matched random control, sweep
logging with attempt counts, and multiple-testing accounting.

## 8. `cli.exe`

```
cli bars   --in <dir|file> --out <file> [--interval 10s] [--symbol ETHUSDT]
cli audit  --bars <file>
cli walk   --bars <file> [--agent dqn] [--seeds 3] [--train 40000] [--test 10000]
           [--gap 2000] [--episode 3600] [--epochs 2] [--cost-bps 5]
           [--hold-penalty 0] [--lr 3e-4] [--hidden 64,64] [--gamma 0.99]
           [--epsilon-decay 200000] [--batch 32] [--target-sync 2000]
           [--q-lr 0.1] [--seed 1] [--out runs]
cli report --run <id> [--root runs]
cli list   [--root runs]
cli verify --run <id> [--root runs]
```

`walk` is the whole experiment: it trains fold by fold, evaluates each held-out
window, and runs the baselines, the cost curve and the shuffled control
alongside. `verify` re-hashes the bar file against the manifest and re-runs the
recorded config.

Exit codes: `0` success, `1` a control or the audit failed, `2` bad input.

Not built: `cli sweep`, and a config-file surface — everything is flags today.
`verify` re-runs and reports rather than diffing equity curves.

## 9. `app.exe`

A read-only Win32 viewer. It renders with GDI into a DIB section — no GPU
libraries, no UI toolkit, no charting dependency — and ships a per-monitor-v2
DPI manifest so text and chart are drawn at real device pixels rather than
bitmap-stretched.

```
app.exe --bars data/bars/ETHUSDT-10s.bin --run runs/<id>
```

Layout, top to bottom:

- **Header** — symbol, bar file, visible bar range, and a readout of the bar
  under the cursor: time, price, position, equity.
- **Price** — one column per pixel, aggregating high and low over the bars that
  fall in it, so it stays responsive across a million bars.
- **Position band** — green long, red short, grey flat.
- **Equity** — the run's cumulative net log return, with the area to zero shaded.
- **Mini-map** — the whole series in one strip, coloured by position, with the
  current viewport drawn as a lit window over a dimmed whole.

Navigation: drag or middle-drag to pan, wheel to zoom about the cursor,
shift-wheel or horizontal wheel to scroll, click or drag the mini-map to jump,
Home/End and arrow keys. All layout is DPI-scaled, and the mini-map is cached
per pixel column so dragging stays smooth on a large series.

The viewer runs without arguments and shows a message rather than crashing when
there are no bars.

Not built: feature panes, seed-spread display, side-by-side run comparison.

## 10. Results

Measured on 5 days of ETHUSDT USD-M futures trades (2024-07-22 to 07-26),
19,405,116 trades aggregated to 43,200 ten-second bars. Walk-forward, 7 folds,
12,000 train bars, 500 gap, 4,000 test, 5 bps round trip, DQN over 5 seeds.
Figures are out-of-sample cumulative log return over the pooled test windows.

| Variant | Net | Sharpe | Max DD | Turnover |
|---|---|---|---|---|
| flat | 0.0000 | 0.00 | 0.0000 | 0.000 |
| long | −0.0570 | −9.70 | 0.1266 | 0.000 |
| random | −0.1117 | −25.88 | 0.1172 | 0.018 |
| crossover | −0.1584 | −26.72 | 0.1834 | 0.024 |
| **dqn (median of 5 seeds)** | **−0.0668** | **−12.07** | 0.1045 | 0.009 |
| control: shuffled returns | −0.0615 | −11.75 | 0.0793 | 0.008 |

**No edge.** Three things make that hard to argue with:

1. **The DQN is indistinguishable from its own control.** Median net −6.7%
   against −6.2% for the same policy trading a shuffled return stream. All five
   seeds are negative, in a tight band from −5.7% to −10.0%.
2. **Positive gross return proves nothing here.** The DQN's gross was +0.5% to
   +3.4% across seeds — but the *random* agent's gross was +1.4%, and the
   shuffled control's ranged from −7.9% to +5.0%. The window is a downtrend, so
   anything with a short bias shows positive gross. The learner's gross sits
   inside the control's noise band.
3. **Costs dominate.** The crossover rule earned +1.1% gross and paid 16.9% in
   costs. At one-second bars it earned +10.6% gross and paid 200%.

A related observation from the one-second run: with enough training the tabular
Q agent converges to **flat in 183 of its 210 visited states**. Given the cost
model, the optimal learned policy is to not trade. That is the correct answer,
and it is reassuring that a learner finds it unprompted.

Caveats: five days is a very short sample, one regime, one pair. The annualised
and Sharpe figures are scaled up from ten-second bars and should be read as
ordering information, not forecasts.

## 11. What would be worth trying next

In rough order of expected value:

1. **More data.** Five days cannot separate a weak edge from noise. Thirty days
   at one-minute bars is a better shape than five days at ten seconds.
2. **Coarser bars.** Cost as a share of gross falls roughly linearly with bar
   width. One-minute and five-minute bars are the obvious sweep.
3. **A holding penalty**, to push the agent toward fewer, larger positions
   rather than trimming the ones it has.
4. **The missing controls** — reversed signal, and random at matched turnover.
5. **Feature panes in the viewer**, so a policy can be read against what it saw.

The expected outcome remains that a liquid pair at short horizons has no
tradeable edge after realistic costs. A bench that establishes that quickly and
convincingly is doing its job.

## Appendix A — gaps in the original prototype

All of these were found in the review that preceded the rewrite. Every one is
now closed; they are kept because they are why the rules above exist.

| # | Gap | Resolution |
|---|---|---|
| 1 | CSV reader called `getline` twice per iteration, dropping every second row | rewritten in `bars.cpp` |
| 2 | `sscanf_s` format used `;` separators on comma-separated data, leaving `quantity`, `quote` and `time` uninitialised | field-by-field `strtod` parse |
| 3 | Moving averages were centred (`i-r` to `i+r`) — look-ahead in every feature and in the label | streaming indicators only, enforced by `cli audit` |
| 4 | All real features were zeroed and the label fed in as an input, so the model learned a tautology | 13 real causal features in `features.cpp` |
| 5 | Hard-coded absolute path to a file outside the repo; empty-result path was UB | all paths from the command line |
| 6 | No reinforcement learning: no reward, policy, value function, episodes or exploration | `env.cpp`, `agents.cpp` |
| 7 | `nn` owned raw `xfloat***`, was implicitly copyable, had uninitialised members | flat `std::vector`, no owning pointers |
| 8 | Sigmoid output and hidden layers, MSE loss, fixed LR, bias faked by a pinned neuron | ReLU hidden, linear output, real biases, Huber, Adam |
| 9 | `sigmoid` used `powf(EXP, -x)` with a truncated constant | activation helpers rewritten |
| 10 | Renderer indexed `trades[x]` per pixel column unchecked; GDI objects leaked | bounds-checked aggregation, cleanup on `WM_DESTROY` |
| 11 | Cost model was a flat `0.01` in price units | basis points on position change, with a sensitivity curve |
| 12 | No weight serialisation, no seeding, no splits, no metrics | `mlp::save`, seeded `rng`, walk-forward, `metrics.cpp` |
| 13 | Build output committed, including OpenSSL DLLs and an orphan `bot.exe` | deleted; `exe/` gitignored |
| 14 | MNIST constants left in `common.h` | removed |
