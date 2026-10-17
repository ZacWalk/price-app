# price-app - experiments

This is a record of experiments, controls, measurements and conclusions.
[design.md](design.md) describes the current system. Implementation chronology,
dependency publication history and routine build changes do not belong here.

**Market conclusion:** a thin trend-following effect beats its timing controls
on ETHUSDT and BTCUSDT. Re-measured with pre-registered windows on data through
2026-10-15 (section 5.1), the breakout rule breaks even at 8.6 bps round trip
on ETH, 6-7 bps in every year since 2022, and at 2.6 bps on BTC, where it loses
at 5 bps; it covers neither asset's retail execution cost. The older crossover
rule breaks even at 19.4 and 11.8 bps, but its gains rest on a few years and
it lost in others on both assets. Funding, now measured, takes about 8% of the
ETH breakout net. The DQN, in its recorded SPQN form and its current libcmlp
form, loses to turnover. No non-crypto market edge has been established.

The historical DQN market measurements in sections 6.1-6.3 use the original
binary64/SPQN network; section 6.4 measures the current libcmlp binary32
network. Rule/baseline reproduction is checked separately. Synthetic
engineering experiments establish software properties, not an investment
result.

## 1. Recording protocol

Every new market experiment should record:

- Hypothesis, instrument/source, date range, format, data hash, calendar,
  price basis, feature profile and numerical contract.
- Bar interval and real calendar horizons; chronological train/validation/test
  windows, purge, seeds and parameters fixed before test scoring.
- Constants/rules, shuffled and rotated controls, cost sensitivity at
  1/2/5/10 bps, and any additional execution/carry assumptions.
- Per-fold and recent-period behaviour, attempts made, selection criteria and
  a conclusion, including when the answer is no.

Never tune on the test table. A long-only rule needs a timing control, not just
shuffled returns. Statistical significance must pass the persistence/cost
screen; quote `PAYS`, not a t-statistic in isolation.

### Four false positives

| Claim | Evidence that rejected it |
|---|---|
| 5m DQN ensemble net +0.355 | Maximum of three base seeds; replications gave +0.12 and -0.86. |
| Coarser bars fixed the DQN | 1m/5m runs covered different calendar windows; `long` was -0.225 in one and +0.174 in the other. |
| Switching margin 10 produced +1.53 | Maximum of a five-point sweep; replications averaged +0.37 and validation selected the least-trading setting. |
| Monotone momentum gradient across yesterday's range | Entirely a first-half effect; that half included the 5 August 2024 crash. |

These are experimental failures, not evidence that the present test windows
can be searched again.

## 2. Datasets and source equivalence

The market sample is **2020-01-01 to 2026-07-31: 2,404 days and 3,461,760
one-minute bars per symbol**, ETHUSDT and BTCUSDT, with no empty bars.
Binance USD-M monthly klines supply trade count and taker-buy volume.

| Data volume | Raw trades | 1m klines |
|---|---|---|
| 92 days of ETH | 15.5 GB | approximately 5 MB |
| Full ETH + BTC sample | not attempted | approximately 740 MB CSV |

Source equivalence was checked on all 132,480 bars of a common quarter:

| Fields | Result |
|---|---|
| Time, OHLC, filled flag | Bit-identical |
| Base/quote/buy volume | Median relative difference 3e-15 |
| Material volume differences | 4, 38 and 4 bars respectively |
| Trade counts | 12 differing bars |

The small floating differences reflect summation order; the material exceptions
are trades exactly on minute boundaries. This verifies the specific Binance
paths, not arbitrary providers.

## 3. Pattern evidence

### 3.1 Short sample

On 92 days, no tested grouping beat its shuffled 95% maximum:

| Grouping | Largest absolute t | Shuffled 95% | Verdict |
|---|---:|---:|---|
| Hour of day | 1.70 | 2.84 | Chance |
| Day of week | 2.04 | 2.55 | Chance |
| Previous-day high distance | 1.44 | 2.73 | Chance |
| Previous-day low distance | 1.55 | 2.50 | Chance |
| Previous-day close distance | 2.56 | 2.71 | Chance |
| Previous-day range position | 1.50 | 2.65 | Chance |

The effective sample for an hourly effect was about 92 days, not the count of
intraday bars. The detectable effect was approximately 0.25% per hour, far above
typical intraday seasonality.

### 3.2 Seasonality and the economic screen

Volatility and activity were strongly seasonal, not direction:

| Measure | Peak | Trough | Ratio |
|---|---|---|---|
| Hourly absolute return | 20.3 bps at 14:00 UTC | 9.5 bps at 11:00 | 2.1x |
| Hourly trades | 21,751 at 14:00 UTC | 8,627 at 23:00 | 2.5x |
| Weekday absolute return | 17.4 Monday | 8.6 Saturday | 2.0x |

On the full sample, 16 of 20 tests initially cleared their permutation null.
That was not an economic discovery. Close versus VWAP reached t=6.13 on just
0.092 bps with 688,396 samples. Measuring the implied position's persistence
removed 21 of 23 flagged rows, including Saturday reversion at t=-6.46.

`hold` is how long the position persists; `need` is round-trip cost divided by
absolute mean return. `PAYS` requires `hold > need` and significance. A bucket
can persist while its momentum position flips every bar.

Hour/day direction, activity surprise, trade size, VWAP and price impact failed
the economic screen. The surviving family was drift above the previous
session's high.

### 3.3 Permutation validity

With whole-day blocks on regular data, each hour bucket retains its returns.
The observed/shuffled maxima were 2.48/2.48 on a synthetic 15m fixture,
2.17/2.17 on ETHUSDT 1m and 1.87/1.87 on 5m in July 2024. Rounding could decide
the apparent verdict. With fewer than two blocks, a zero threshold incorrectly
made all twenty tests survive.

Those nulls are `UNTESTED`. Part-day blocks moved the hour null (3.16 and 2.91
on the same data; nothing survived). The historical hour-of-day result below,
2.24 against 3.34, came from a moving null, not whole-day blocks.

## 4. Bar width and cross-asset replication

Breakout-state drift, measured against its persistence:

| Width | Mean bps | t | Hold, bars | Edge/episode, bps | Net after 5 bps |
|---|---:|---:|---:|---:|---:|
| 1m | 0.132 | 4.13 | 53.4 | 7.0 | +2.0 |
| 5m | 0.683 | 4.51 | 22.7 | 15.5 | +10.5 |
| 7m | 0.902 | 4.33 | 19.1 | 17.2 | +12.2 |
| 11m | 1.523 | 4.77 | 15.0 | 22.8 | +17.8 |
| 13m | 1.732 | 4.69 | 13.4 | 23.2 | +18.2 |
| 14m | 1.994 | 5.06 | 13.2 | 26.3 | +21.3 |
| 15m | 2.322 | 5.37 | 12.5 | 29.0 | +24.0 |
| 16m | 2.365 | 5.20 | 12.0 | 28.4 | +23.4 |
| 17m | 2.467 | 5.09 | 11.6 | 28.6 | +23.6 |
| 19m | 2.643 | 4.99 | 11.0 | 29.1 | +24.1 |
| 23m | 3.211 | 5.05 | 10.0 | 32.1 | +27.1 |
| 1h | 7.966 | 4.83 | 5.4 | 43.0 | +38.0 |

The nonstandard widths showed no isolated 15m spike. At 1m, threshold flicker
split a move into several round trips. The practical conclusion for this
dataset was to store at 1m and study roughly 13-17m bars, **not** to declare
that band optimal for other markets.

BTCUSDT 15m, previous-day range position >=1: mean **1.140 bps**, t=3.11,
hold 11.3 bars, need 4 bars. This was cross-asset replication, not another seed.
The threshold and widths were explored on results and remain subject to
selection bias; no independently tuned generic-market claim follows.

## 5. Breakout rule and costs

Rule: long above the previous completed session high, otherwise flat.

At 15m with a validation window, test windows occurred later and buy-and-hold
lost. At 5 bps round trip:

| Variant | Net log return | Sharpe | Max log DD | Turnover |
|---|---:|---:|---:|---:|
| Long | -0.18 | -0.05 | 1.69 | 0.000 |
| Breakout | +0.64 | 0.47 | 0.35 | 0.020 |
| Shuffled control | +0.19 | 0.12 | 0.60 | 0.020 |
| Rotated control | -0.87 | -0.64 | 1.31 | 0.020 |

Recorded cost sensitivity, each with its rotated control:

| Cost model | Breakout | Rotated | Gap |
|---|---:|---:|---:|
| Flat 5 bps | +0.635 | -0.869 | 1.50 |
| 5 bps + 1 bp one-way slippage | +0.282 | -1.222 | 1.50 |
| Activity-scaled 5 bps | +0.641 | -0.864 | 1.51 |
| 10 bps | -0.247 | -1.751 | 1.50 |

Break-even was roughly 7-8 bps round trip. The contemporaneous retail all-in
estimate of 11-12 bps put it underwater. One basis point of slippage halved
the apparent benefit; activity scaling barely changed it.

| Summary | Full sample | Recent half | With +1 bp slippage |
|---|---:|---:|---:|
| Annual return | 13.8% | approximately 6.9% | 5.7% |
| Sharpe | 0.47 | approximately 0.25 | 0.21 |
| Max drawdown | 35% | 35% | 37% |

The effect decayed by about half in both assets. Funding was not modelled.
`crossover` earned more net (+2.48 versus +0.64) with approximately twice the
drawdown, so the supported finding is a thin trend-following family, not a
uniquely special threshold. These figures do not constitute a deployable
strategy.

### 5.1 Pre-registered re-measurement - 2026-10-16

**Hypothesis:** the breakout rule's recorded thin edge holds under windows
fixed before scoring, on both assets, and survives funding.

Binance USD-M 1m klines, 2020-01-01 to 2026-10-15, were downloaded with SHA256
verification: 3,571,200 rows per symbol with no duplicate, missing or
inconsistent OHLC row. 15m SPQB v1 bars have data hashes `fbae11e44c3be301`
(ETHUSDT) and `5dd063aa58caf408` (BTCUSDT); both audits passed eight probes.
Windows were fixed before any test score was seen: train 35,040 bars (365
days), validation 8,640, purges 960, test 8,640 (90 days), seed 1, 5 bps, no
slippage. That gives 22 folds testing 2021-05-08 to 2026-10-09. These windows
overlap the sample in which breakout was found, so this is a re-measurement,
not independent evidence; only 2026-08-01 onward was unseen.

| 5 bps | ETH net | Sharpe | Max DD | Break-even | BTC net | Sharpe | Max DD | Break-even |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Long | -0.37 | -0.09 | 1.69 | - | +0.33 | 0.11 | 1.48 | 155.0 |
| Breakout | **+0.70** | 0.46 | 0.35 | **8.6** | **-0.50** | -0.41 | 0.77 | **2.6** |
| Breakout shuffled | -1.52 | -0.89 | 1.70 | - | -0.66 | -0.52 | 1.10 | 1.9 |
| Breakout rotated | -0.79 | -0.49 | 1.17 | 0.9 | -0.90 | -0.75 | 1.04 | 0.7 |
| Crossover | +2.94 | 0.74 | 0.74 | 19.4 | +1.41 | 0.47 | 0.72 | 11.8 |
| Crossover rotated | -1.02 | -0.26 | 2.06 | - | -0.86 | -0.28 | 2.22 | 0.9 |

The ETH result reproduces the recorded +0.64, Sharpe 0.47 and drawdown 0.35.
The rule fails on BTC: its timing beats both controls, but its gross covers
only 2.6 bps. The recorded BTC replication was a pattern-study drift
statistic, not this rule's walk-forward net.

| Net by round trip | 1 bps | 2 bps | 5 bps | 10 bps |
|---|---:|---:|---:|---:|
| ETH breakout | +1.471 | +1.277 | +0.696 | -0.273 |
| BTC breakout | +0.342 | +0.132 | -0.498 | -1.548 |
| ETH crossover | +3.761 | +3.556 | +2.943 | +1.920 |
| BTC crossover | +2.243 | +2.035 | +1.412 | +0.373 |

Net is linear in cost, so the break-even column summarises each curve.

Breakout by year, with the market's log return and a rotated control inside
each year:

| Year | ETH market | ETH net | Rotated | Break-even | BTC market | BTC net | Rotated | Break-even |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2021 | +0.04 | **+0.385** | -0.326 | 20.6 | -0.23 | -0.047 | -0.135 | 3.3 |
| 2022 | -1.13 | +0.065 | -0.381 | 7.1 | -1.03 | -0.429 | -0.430 | - |
| 2023 | +0.65 | +0.048 | +0.010 | 6.5 | +0.95 | +0.217 | +0.020 | 11.7 |
| 2024 | +0.38 | +0.094 | -0.259 | 7.2 | +0.79 | +0.118 | -0.069 | 8.2 |
| 2025 | -0.12 | +0.047 | -0.040 | 6.3 | -0.07 | -0.218 | -0.264 | - |
| 2026 | -0.18 | +0.058 | -0.073 | 7.2 | -0.07 | -0.139 | -0.155 | 0.9 |

The partial year 2021 carried 55% of the ETH net. Since 2022 ETH breakout has
earned a small positive net and beaten its timing control in every year, but
with only 6-7 bps of cost headroom. Crossover is lumpy: it lost in 2023 and
2025 on ETH and in 2024 and 2025 on BTC. Its 2019 parameters were not tuned on
this data, but these windows overlap the sample in which it was first observed.

Funding was measured by charging each recorded test position at every 8h
settlement with Binance's archived USD-M rate. Attributing a settlement to the
position held before or after it changed totals by at most 0.0007.

| Rule | Long / short time | Funding paid | Net at 5 bps | Net with funding |
|---|---|---:|---:|---:|
| ETH breakout | 14.9% / 0% | 0.057 | +0.696 | +0.639 |
| BTC breakout | 14.4% / 0% | 0.046 | -0.498 | -0.544 |
| ETH crossover | 46.3% / 53.5% | 0.035 | +2.943 | +2.907 |
| BTC crossover | 46.7% / 53.2% | 0.005 | +1.412 | +1.407 |

Funding is concentrated in bull years: ETH breakout paid 0.029 in 2024, about
a third of that year's net. It is a real cost for a long-biased rule, but
smaller than one basis point of slippage and it overturns no verdict here. It
remains unmodelled in the environment.

On the 69 unseen days from 2026-08-01, ETH/BTC breakout netted +0.088/+0.118
and crossover +0.179/+0.140 while buy-and-hold rose 0.289/0.266. With no
in-period control that short, this is consistent with, not evidence of, an
edge.

**Conclusion:** the ETH breakout result reproduces and is thin; the BTC rule
does not pay. Nothing here justifies tuning the rule, its threshold or the
windows on these test tables.

## 6. Reinforcement learner

### 6.1 Signal versus turnover

Historical DQN gross return separated from its shuffled control:

| Seed | DQN gross | Control gross |
|---|---:|---:|
| 1 | +4.47 | -0.67 |
| 7920 | +5.65 | -2.87 |
| 15839 | +6.27 | -0.63 |
| 23758 | +4.01 | +0.52 |
| 31677 | +6.51 | -1.64 |

But at the recorded cost:

| Agent | Turnover | Trades | Gross | Cost | Net |
|---|---:|---:|---:|---:|---:|
| Breakout | 0.012 | 3,338 | +2.07 | 1.67 | +0.40 |
| DQN | 0.095 | 26,601 | +5.65 | 13.30 | -7.66 |

The learner found about 2.6 times the gross signal and paid eight times the
cost. Five-minute mean reversion was approximately -0.240 bps, t=-7.58:
statistically detectable and roughly twenty times too small for a 2.5 bps
one-way charge.

### 6.2 Switching margin and discount

With parameters selected on validation:

| Margin | Validation | Test | Turnover |
|---|---:|---:|---:|
| 0 | -6.05 | -7.59 | 0.133 |
| 2.5 | -1.88 | -3.87 | 0.072 |
| 5 | -1.46 | -3.33 | 0.036 |
| 10 | -0.62 | -2.61 | 0.004 |
| 20 | -0.33 | -0.87 | 0.000 |

Validation selected the setting that traded least, and it still lost. Between
margins 2.5 and 5, gross fell 72% while cost fell 63%; reducing turnover also
destroyed the signal.

| Discount | Margin | Validation | Test | Turnover |
|---|---:|---:|---:|---:|
| 0.99 | 0 | -6.05 | -7.59 | 0.133 |
| 0.999 | 0 | -5.82 | -6.85 | 0.127 |
| 0.99 | 5 | -1.46 | -3.33 | 0.036 |
| 0.999 | 5 | -1.19 | -2.74 | 0.033 |

A tenfold longer discount horizon changed turnover by only about 4%.
Previous-session features were already present; merely adding that information
or extending the horizon did not make the DQN find the rule's behaviour.

### 6.3 Five-day reference

ETHUSDT USD-M trades, 22-26 July 2024: 19,405,116 trades, 43,200 ten-second
bars, seven folds, train 12,000, gap 500, test 4,000, five seeds, 5 bps round
trip. These are historical pooled out-of-sample log returns:

| Variant | Net | Sharpe | Max DD | Turnover |
|---|---:|---:|---:|---:|
| Flat | 0.0000 | 0.00 | 0.0000 | 0.000 |
| Long | -0.0570 | -9.70 | 0.1266 | 0.000 |
| Random | -0.1117 | -25.88 | 0.1172 | 0.018 |
| Crossover | -0.1584 | -26.72 | 0.1834 | 0.024 |
| DQN median | -0.0668 | -12.07 | 0.1045 | 0.009 |
| DQN ensemble | -0.0895 | -16.01 | 0.1146 | 0.005 |
| Shuffled median | -0.0615 | -11.75 | 0.0793 | 0.008 |
| Shuffled ensemble | -0.0708 | -14.03 | 0.0886 | 0.005 |

No edge. Voting reduced turnover but did not separate the learner from noise.
The sample was too short for annualised statistics to be forecasts. At one
second, crossover gross +10.6% paid roughly 200% in costs; tabular Q eventually
chose flat in 183 of 210 visited states.

### 6.4 Current libcmlp DQN on real data - 2026-10-16

First market measurement of the `cmlp-f32-scalar-he-splitmix64-v1` network,
on the section 5.1 ETHUSDT 15m data and windows, with defaults fixed before
scoring: hidden 64,64, two epochs, episode 3,600, three seeds, 5 bps.

| Variant | Net | Sharpe | Max DD | Turnover |
|---|---:|---:|---:|---:|
| DQN seeds | -5.54 / -7.72 / -7.86 | -1.48 to -2.14 | | 0.138 median |
| DQN ensemble | **-7.12** | -1.90 | 7.51 | 0.136 |
| Shuffled ensemble | -7.39 | -1.86 | 7.85 | 0.136 |
| Rotated | -6.85 | -1.87 | 8.04 | 0.136 |
| Tabular Q ensemble | -2.34 | -1.69 | 2.77 | 0.058 |
| Breakout | +0.70 | 0.46 | 0.35 | 0.020 |

The ensemble's gross was -0.65, between its controls. Seed grosses were
+1.46, -1.14 and -0.81 against shuffled-control grosses of -1.65, +3.18 and
+2.91. Unlike the historical network (section 6.1), this run shows no gross
signal to lose. Validation gave the same verdict (-6.37), so no parameter
choice is indicated. The run took 531 s single-threaded; `verify` reproduced
its metrics, equity, learning curve and weights byte for byte.

## 7. Rejected hypotheses

| Hypothesis | Recorded result | Conclusion |
|---|---|---|
| DQN should be the headline method | Net -7.66 versus a rule's +0.40; current network -7.12, gross -0.65 | It has not earned the complexity. |
| Longer discount fixes churn | Turnover about -4% | Horizon was not the constraint. |
| 1m experiments are preferable | Economic margin 1.4x versus 6.3x at 15m | Threshold flicker increases costs. |
| Hour-of-day direction | Largest absolute t 2.24 versus null 3.34 | No supported directional seasonality. |
| Day-of-week direction | Largest absolute t 1.75 versus 2.75 | Same conclusion. |
| Close versus VWAP | t=6.13, mean 0.092 bps | Too small to trade. |
| Trade size and price impact | Significant, never pays | Activity/volatility information, not a cost-covering direction signal. |
| General momentum/reversion | Pooled t=-7.58 | Real but about twenty times too small. |
| Bar-level permutation is enough | 16/20 initially significant | Null/criterion did not establish economic value. |
| Save the best test seed | Apparent best policy | Model selection on the test set. |
| The breakout rule pays on BTC too | Walk-forward -0.50 at 5 bps; break-even 2.6 bps | The BTC drift replicated; a tradeable rule did not. |
| Funding overturns the ETH result | 0.057 of +0.696 net | A real cost, not the deciding one. |

## 8. Engineering measurements

### 8.1 Numerical and storage experiments

The original flat-buffer/batched network rewrite reproduced all five seeds'
net, Sharpe and trade counts exactly. Enabling AVX2 with `/fp:precise` also
preserved recorded figures, reducing one runtime from 146.9 to 131.8 seconds.
These measurements are not a comparison with the current binary32 libcmlp
numerical contract.

A synthetic 3,461,760-record SPQB v1/spike-db comparison used five interleaved
Windows Release trials with warm OS caches and byte-identical scan hashes:

| Format / access | Cache | Median ms | Peak working set MiB |
|---|---|---:|---:|
| SPQB / materialise | none | 469.773 | 242.05 |
| spike-db / materialise | 4 MiB | 389.426 | 246.28 |
| spike-db / materialise | 64 MiB | 404.962 | 306.34 |
| SPQB / stream | none | 431.647 | 4.32 |
| spike-db / stream | 4 MiB | 360.296 | 8.59 |
| spike-db / stream | 64 MiB | 379.614 | 68.63 |

SPQB occupied 249,246,755 bytes; spike-db occupied 293,208,064, 17.6% more.
Creation took 206.022 versus 1304.42 ms, but durability guarantees differed.
No cold-cache or disk-latency conclusion follows. Approximately 80 ms saved on
a subsecond warm scan did not justify replacing batch storage. The scan hash
was `0a3027f8cb70ccac`.

Repeat with `storage_bench create ROOT COUNT` or a SPQB v1 source file, then
`storage_bench scan ROOT spqb|spike CACHE_PAGES materialize|stream`, using a new
isolated root. This benchmark does not support the generic v2 metadata format.

### 8.2 Generic position-predictor validation - 2026-10-16

**Synthetic functional evidence only.** No new market data was downloaded and
no non-crypto return claim was made.

[tests/generic.ps1](../tests/generic.ps1) constructs 7,000 deterministic
15-minute OHLC observations: 26 bars per supplied session, overnight gaps and
two-day gaps every five sessions, with no trade counts or aggressor flow.
The price path has deliberately smooth periodic structure. It is not a
realistic null market or evidence of profitable forecasting.

Each of the eight agents runs twice with seeds 7/7926, train 1,500, validation
200, gaps 300, test 600, episodes 500, one epoch, and a small 8,8 DQN.
The requested cost is 5 bps alongside the 1/2/5/10 bps curve and both controls.
These settings are fixed test inputs, not parameters chosen from the test
results.

Measured outcomes:

- All eight agents reproduce metric and equity bytes.
- Q-table/DQN policies and learning artifacts also reproduce. The generic DQN
  records nonzero training loss, and a full DQN `verify` rerun reproduces its
  metric and equity bytes.
- The generic audit passes all eight probes for eleven price features.
- Rotation preserves scored-row count, recorded costs, turnover and side
  exposure. Shuffling keeps overnight/intraday duration classes separate.
- Generic verification rejects altered feature schemas, normalisation, market
  metadata and execution conventions.
- Generic terminal reward agrees with reported liquidation costs.
- Exact Windows metric gates for all seven non-DQN v1 agents remain unchanged.
- Tiny positive prices survive generic CSV output; session-gapped equity rows
  align in the offscreen viewer.

Checks were observed failing, not merely running green:

| Fault | Observed failure |
|---|---|
| Generic import against the non-generic CLI | Unknown generic option, exit 2. |
| One-bar future read in feature construction | Both profiles failed all eight audit probes, exit 1. |
| Arithmetic timestamp-to-index lookup across gaps | Native lookup and offscreen viewer tests both failed. |
| Missing terminal liquidation charge in generic reward | Native reward-versus-reported-cost check failed. |

All controlled faults were removed before final validation.
`.\dd.ps1 test --label crypto --non-interactive` passed **10/10 Windows Release
and 10/10 Windows Debug**:

| Suite | Checks |
|---|---:|
| Network adapter | 94 |
| Market/data/feature contract | 31 |
| Generic predictor | 63 |
| Publication concurrency | 32 |
| Acquisition and negative cases | 122 |
| Legacy agents/causality | 83 |
| dd adoption, including generic argument forwarding | 136 |
| Storage parity | 5 |
| Viewer input checks, all offscreen | 10 |

There is also a separate offscreen viewer smoke. The 38 upstream dd hashes are
unchanged. Linux was not rerun for this experiment. The outcome establishes
the generic pipeline and its compatibility checks, not a general price-action
edge.

### 8.3 Real-data review - 2026-10-16

The full pipeline was exercised on the section 5.1 downloads: 192 kline and 162
funding-rate archives, plus two daily klines and one aggregate-trade archive
fetched for importer tests, all SHA256-verified.

Two importer defects were found on real archives and fixed:

| Input | Before | After |
|---|---|---|
| 2026-09-29/30 daily klines beside the September monthly archive | Exit 0. 2,688 synthetic flat bars for September 1-28; the last 15m bar spanned 2,355.12-2,806.76 with volume 105,762,425, against a true 2,683.25-2,688.34 and 10,997. | Exit 2 naming the monthly file and first repeated kline; nothing written. |
| An ETHUSDT 2026-10-01 aggregate-trades archive | Exit 0. 1,065,814 rows read as trades gave 69 bars dated 1970-04-12. | Exit 2, unrecognised layout; nothing written. |

Repeated klines are refused row by row. A trades file starting before data
already read is refused, while timestamp artifacts within a file still fold
into the open bar. Clean full-history imports stayed byte-identical: 15m and
1m bars for both symbols kept the hashes recorded in section 5.1, plus
`02d7e4649d813ad0` (ETHUSDT 1m) and `9751a7b95bc4e0c5` (BTCUSDT 1m).

Reporting and verification changes were checked against the pre-change binary
on the same data:

- `walk` now prints break-even round-trip cost and a calendar-period table.
  Both matched an independent calculation from `equity.csv`. Breakout,
  crossover and Q-table `metrics.csv`, `equity.csv`, manifests, policy and
  learning files stayed byte-identical.
- `verify` now compares artifacts. Running the new binary, it reproduced the
  pre-change binary's runs byte for byte: breakout (2 artifacts), Q-table (4),
  and the 531 s DQN run (4, including `weights.bin`, in 622 s).
- A failed artifact append is now sticky and turns the run into exit 2.

Real-data timings, single-threaded Release: importing 3,571,200 klines took
5.2 s to 15m and 5.7 s to 1m; a 15m audit 0.5 s; breakout and crossover walks
0.9 s; three-seed Q-table 1.8 s; three-seed default DQN 531 s. The offscreen
viewer loaded and painted 3,571,200 bars with a run in 0.73 s.

Each new check was observed failing under an injected fault:

| Fault | Observed failure |
|---|---|
| Overlapping or repeated archives merged (pre-change binary) | Acquisition suite: overlap accepted, exit 0. |
| Aggregate trades read as trades (pre-change binary) | Acquisition suite: 1970 bar file published. |
| Liquidation units omitted from break-even | Market contract failed; e2e reported break-even 219.8 against 109.9. |
| `verify` comparison always equal | Generic suite: a same-size metrics edit was reported reproduced, exit 0. |
| Write failure not remembered | Market contract failed. |
| Last calendar period dropped | Generic and e2e partition checks failed. |

`.\dd.ps1 test --label crypto --non-interactive` then passed **10/10 Windows
Release and 10/10 Windows Debug**. Network 94, market contract 40, generic
predictor 74, publication 32, acquisition 134, legacy agents/causality 89,
dd adoption 136, storage 5 and viewer 10 checks; 38 upstream dd hashes
unchanged. Linux was not rerun.
