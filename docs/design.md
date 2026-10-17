# price-app - design

price-app is an offline, single-instrument **price-action position predictor**.
At each decision it chooses short, flat or long from historical prices and its
current position. It evaluates those decisions chronologically against costs,
simple agents and controls. It does not output a future price or calibrated
direction probability.

This document describes the implemented contract. [experiments.md](experiments.md)
contains measured results, not implementation history. Generic data support
does not establish a predictive edge: the market evidence recorded so far is
from ETHUSDT/BTCUSDT perpetuals.

## 1. Architecture

| Component | Responsibility |
|---|---|
| `cli.exe` | Import/aggregate data, audit features, train and evaluate agents, write runs, report and verify. |
| `app.exe` | Read-only viewer for bars, predicted positions and equity. |
| [bars](../src/bars.h) | Canonical OHLCV import, Binance aggregation, versioned bar files, metadata and hashing. |
| [features](../src/features.h) | Streaming causal indicators and versioned feature matrices. |
| [environment](../src/env.h) | Position state, next-observed-bar return and costs. |
| [agents](../src/agents.h) | Constants, rules, tabular Q, DQN and seed voting. |
| [experiments](../src/experiment.h) | Walk-forward windows, baselines, controls and artifacts. |
| [patterns](../src/patterns.h) | Bucket studies with permutation and persistence/cost screens. |

libcmlp owns neural math; libcfindata owns acquisition; spike-db is used only by
the storage comparison; platform-h belongs only to the viewer. Full revisions
are recorded in [the dependency manifest](../cmake/dd-dependencies.json).
The CLI never links platform-h, and Linux does not configure it.

The viewer never trains, evaluates or writes. Opening a result cannot change
it. All market acquisition is explicit through `cli download`; experiments,
verification and tests need no network.

## 2. Generic data contract

### 2.1 Canonical CSV

```text
cli bars --format ohlcv --in <csv> --out <file> --symbol <instrument>
         --interval <duration> --calendar <utc|supplied-name> --currency <unit>
         [--source-name local-csv] [--price-basis raw|adjusted]
```

Input is a single already-aggregated, headered CSV. Required columns are
`time,open,high,low,close`. Optional columns are `volume` and `session`.
Columns may be reordered; unknown or duplicate columns are errors. Plain
numeric cells are required, not quoted/provider-specific CSV expressions.
A UTF-8 BOM and whitespace around cells are accepted.

| Field | Meaning |
|---|---|
| `time` | Bar-open time, nonnegative integer UTC epoch milliseconds. |
| `open,high,low,close` | Finite positive prices, with `low <= open/close <= high`. |
| `volume` | Optional finite nonnegative provider quantity. Not an inferred trade count or aggressor flow. |
| `session` | Required for a supplied calendar: nonnegative, nondecreasing integer IDs, repeated within a session. |

`--symbol`, `--interval`, `--calendar` and `--currency` must be explicit. An
interval is a positive whole duration (`ms`, `s`, `m`, `h`, `d`) up to `365d`.
Times must increase by at least that interval; duplicates, out-of-order rows,
overlapping bars, invalid prices and missing numeric cells are rejected before
publication. An existing generic output file is rejected.

With `--calendar utc`, there is no session column: the session ID is the UTC
date derived from each timestamp. Any other calendar label requires the input
session column. That label identifies the producer's convention; there is no
built-in exchange calendar, timezone conversion, holiday or DST database.
Supplied IDs can express sessions spanning UTC midnight and changes in UTC
opening times without splitting a session incorrectly.

The importer preserves observed timestamps and reports gaps. It does not
resample, fill closures or infer whether a gap is a closure, halt or missing
data. Coverage and calendar correctness are the producer's responsibility.
The research horizon is **one observed bar**, not necessarily one nominal
interval of elapsed time.

Volume availability is explicit. An omitted column is unavailable, not an
observation of zero liquidity. Neither optional volume nor fabricated flow
enters the generic feature profile.

### 2.2 Persistent formats

Generic data uses **SPQB v2**. It records the symbol and nominal interval,
source, currency, calendar label, price basis (`raw` or `adjusted`), volume
availability, OHLC records and one session ID per bar. The underlying
72-byte `bar` layout remains unchanged; v2 has an extended header and a parallel
session vector. Absent quote volume, aggressor volume and trade counts are
marked unavailable by the format, not presented as observations.

Readers validate v2 metadata, record sizes, timestamps, prices and session
consistency. The data hash covers metadata and session IDs as well as prices,
symbol and interval. Changing only the calendar or currency changes identity.

**SPQB v1** is the Binance reproduction format. Its bytes, hashing,
twenty-feature profile and recorded experiment semantics are retained.
No file is automatically migrated or overwritten. The spike-db comparison is
v1-only and rejects v2 rather than discarding its metadata.

### 2.3 Binance data

The dedicated importer detects Binance trades or klines; aggregate-trade
archives are refused rather than read as trades. Trades aggregate into
clock slots; empty slots carry the previous close. Klines can only aggregate
to whole multiples of their source interval; they cannot be split. Native
defaults are 1s for trades and the source interval for klines. Archives must
not overlap: a repeated or earlier kline, or a trade file starting inside data
already read, is refused rather than merged. Daily and monthly archives for the
same dates share a default directory, so this is a routine hazard; once it
folded a month into one bar and filled the rest with synthetic flat bars.

The reproduction path assumes USD-M millisecond timestamps and base/quote
volume semantics. Acquisition of spot or coin-margined files is not a claim
that they satisfy that modelling contract.

libcfindata downloads ZIPs and mandatory SHA256 sidecars, validates archive
members/limits/CSV schemas, then publishes under per-destination OS locks.
Explicit fixtures never fall back to the network. Missing checksums fail even
with `--skip-missing`. Forced downloads retain `.previous`; failures preserve
staging/backup evidence, and live `.lock` anchors must not be removed.
Typed rows default to a 10,000,000 limit; byte limits are independent.

## 3. Features and causality

### 3.1 Generic `ohlc-v1` profile

Eleven price-only columns, followed by three position columns:

| Feature | Definition |
|---|---|
| `ret_1,ret_5,ret_15,ret_60,ret_300` | Lagged log return, divided by trailing 60-bar realised volatility times the square root of the lag. |
| `trend_fast` | EMA(12) minus EMA(48), divided by the trailing range scale. |
| `trend_slow` | EMA(48) minus EMA(192), divided by that scale. |
| `vol_ratio` | Log ratio of 60-bar to 1,440-bar realised volatility. |
| `range_pos` | Close within its trailing 300-bar close range, centred on zero. |
| `pd_range_pos` | Close within the previous completed session's high/low range, centred on zero. |
| `pd_high_dist` | Distance above the previous completed session high, divided by its range. |

The range scale uses the 48-bar mean relative high-low range, rescaled by the
current close. It is not an exchange calendar or gap-aware true-range model.
Generic log returns are differences of logarithms, avoiding an overflowing
intermediate price ratio.

The first eight columns use causal expanding Welford statistics and a +/-5
clamp. Range position is bounded; previous-session distances use a +/-2 clamp.
Warmup is at least **1,740 observed bars** and requires a completed previous
session. Lookbacks count observations, not calendar minutes. Changing the bar
interval changes the model's time horizons.

The environment appends current position, unrealised P&L in trailing bar-range
units, and transformed bars held. Generic observations therefore have fourteen
columns. Agents bind named features, not fixed offsets.

### 3.2 Binance `binance-v1` profile

The v1 profile has twenty base features: the price features above plus
aggressor flow, trade-count activity, hourly activity surprise, two harmonics
of UTC time of day and day-of-week sine/cosine. Position state makes
twenty-three observation columns. Its UTC-session and continuous-bar
normalisation/warmup semantics are preserved for reproduction.

### 3.3 Causality guarantee

Indicators consume one completed bar at a time and own only past state.
Previous-session levels become available at rollover, never before that
session is complete. Features at decision `t` do not read the return earned
after `t`. Expanding statistics are causal; no complete test window is fitted
as a preprocessing dataset.

`cli audit` recomputes features on prefixes at eight probes and compares them
with the full series. It carries the same format, metadata and truncated
session IDs into each prefix. Any future read must fail. Both profiles have
been tested with deliberately injected one-bar look-ahead; see the experiment
record.

## 4. Position prediction and learning

Actions are target positions `{short=-1, flat=0, long=1}`. A decision uses the
completed bar's features and current position. Its payoff is the signed log
return from that close to the next observed close. It is not available at the
bar-open timestamp stored in the data.

For the generic path:

```text
gross = position * (log(next_close) - log(close))
one_way_cost = round_trip_bps / 2 / 10000 + slippage_bps / 10000
reward = gross - one_way_cost * abs(position_change)
         - holding_penalty * abs(position)
```

Episodes start flat and finish flat. Generic training rewards and reported
costs both include terminal liquidation. The holding penalty regularises
learning; it is not a substitute for funding or an actual carrying-cost ledger.
v1 retains its reproduction training convention; reported metrics include
liquidation costs in both formats.

Fills are an explicit close-plus-slippage assumption. No execution price is
chosen from the same bar's high/low. The generic path rejects
`--cost-activity`, which requires unavailable trade counts. The v1 path
supports causal trade-count-scaled costs.

| Agent | Policy |
|---|---|
| `flat`, `long`, `short` | Constant position baselines. |
| `random` | Seeded random target changes with probability 0.02 per bar. |
| `crossover` | Long above `trend_fast > 0.5`, short below -0.5, otherwise hold. |
| `breakout` | Long above the previous completed session high, otherwise flat. |
| `qtable` | Tabular Q-learning over 225 trend/return/volatility/position states. |
| `dqn` | Double DQN with replay, a target network, Huber derivatives and Adam. |
| ensemble | Vote of the configured seeds, holding the current position on a tie. |

The native default is `dqn`, not an assertion that it is the best predictor.
Constants and rules must be beaten out of sample before additional complexity
is justified. Seeds are trained side by side; seed 0 is the fixed saved-policy
choice, not a seed selected on test performance.

Features, rewards, replay and metrics are binary64. The DQN uses a move-only
libcmlp adapter with finite/range/underflow-checked binary32 conversions.
Its contract is `cmlp-f32-scalar-he-splitmix64-v1`: scalar kernels, He/Xavier
initialisation, deterministic nonzero seed mapping and Adam with global norm
clip 10. CMLP checkpoints retain optimiser state. SPQN checkpoints and
incompatible library revisions are explicitly rejected.

## 5. Evaluation

Walk-forward folds contain train, optional validation and test windows, with
configurable gaps. All lengths count observed bars. Use a nonzero `--valid`
window for parameter selection; the test table is scored and reported, not
searched. Compare actual calendar spans when changing interval or market.
Choose the gap to match the experiment's dependency/purge requirements rather
than assuming any positive gap eliminates overlapping lookbacks.

Every run includes:

- Constant/rule baselines, individual seeds and their ensemble.
- Costs at 1, 2, 5 and 10 bps, plus the requested cost if different.
- Shuffled-return and rotated-timing headline controls.
- Per-fold and pooled metrics; validation metrics when requested.

For generic data, the shuffled control only exchanges returns having the same
elapsed duration, so an overnight return is not substituted for an intraday
one. The timing control counter-rotates the actual scored returns against the
unchanged position/cost sequence. This preserves bar exposure, turnover and
recorded costs, includes every scored transition and never constructs a
return across a gap between pooled folds. Eight offsets provide the reported
median-by-Sharpe rotation.

These are diagnostic nulls, not simulated executable alternative strategies.
They do not establish exchangeability across every regime or model costs that
the environment omits. The v1 controls retain their recorded convention.
If the shuffled control beats the real headline outright, `walk` exits 1.

Metrics include cumulative net/gross log return, cost paid, annualised log
return, Sharpe, Sortino, log drawdown, turnover, hit rate and exposure by side.
When gross is not positive, the console's cost/gross share is undefined and
prints `-`, not a misleading zero.

The console also prints each variant's **break-even round-trip cost**: costs
are linear in the cost-scaled position units charged (entries, switches and
liquidations), so the rate consuming the whole gross is exact for flat,
slipped and activity-scaled models alike. It prints `-` when no nonnegative
cost pays. The headline is then split by UTC calendar year, or by month when
the scored span lies within one year, showing the underlying market return,
net, break-even, and a rotated timing control confined to that period. The
periods partition the pooled headline exactly. A result carried by one period,
or decaying across them, is visible without post-processing; the split is a
diagnostic, never a basis for selecting a period. These console figures are
derived from in-memory logs and do not change any artifact.

For generic files, annualisation uses the effective scored observation rate:
number of scored transitions divided by their summed elapsed durations,
scaled to a 365.25-day year. Overnight time counts; unscored train/purge gaps
do not. Sharpe is descriptive sample scaling, not a forecast or a
heteroskedasticity correction. Exposure fractions count observed bars, not
wall-clock time. v1 retains continuous-interval annualisation.

## 6. Pattern studies

`cli patterns` measures forward drift and previous-direction momentum by
bucket, against a block-permutation maximum-statistic null. It reports both
sample halves. `PAYS` requires significance and a position that persists long
enough to cover the selected round-trip cost; a high t-statistic alone is not
an edge.

Generic data supports previous-session high/low/close distances and range
position. Flow, activity, VWAP, impact and UTC-seasonality groupings are
explicitly unavailable. An explicit `--block N` in observed bars is required;
there is no inferred 24-hour session. Permutations that cannot move the tested
grouping, or lack two complete blocks, are `UNTESTED`.

The Binance profile also supports the UTC/participant groupings, with a
default block of one day of bars. Whole-day blocks cannot test hour-of-day
effects on a regular series; use a justified alternative block for that study.

## 7. Run artifacts and verification

Each run owns a new directory under the requested output root:

| Artifact | Contents |
|---|---|
| `manifest.txt` | Config, seeds, data hash, format, feature names/profile, normalisation and prediction/timing contracts. |
| `metrics.csv` | Variant, seed, cost, fold and performance metrics. |
| `equity.csv` | Bar time, price, out-of-sample ensemble position, gross, cost and cumulative net log return. |
| `learning.csv` | Learner episode reward, exploration and loss, when training occurs. |
| `policy.csv` / `weights.bin` | First-seed learned policy, where applicable. |

Generic manifests also record source, currency, calendar, price basis,
availability flags, execution/carry limitations and control conventions.
Generic price output preserves binary64 significant digits rather than
rounding very small prices to zero.

`verify` re-hashes the bars and metadata, validates feature meanings as well
as dimensions, and checks the prediction and market contracts before rerunning
the configuration under a separate `verify` root. Older v1 runs without the
new feature metadata use the reproduction path; DQN numerical/library checks
still apply. Completing is not reproducing: `verify` then compares the rerun's
`metrics.csv`, `equity.csv`, `learning.csv`, `policy.csv` and `weights.bin`
byte for byte with each recorded one present, and exits 1 if any differs.
A recorded run without `metrics.csv` has nothing to verify and is an error.

`report` prints the manifest and pooled metric rows. `list` discovers runs.
Unknown options, malformed values, incompatible contracts and unavailable
required fields are errors. Exit codes: 0 completed, 1 failed causality,
control or reproduction, 2 invalid input or runtime error, including a failed
artifact write. No exception crosses the CLI boundary.

There is no standalone saved-model inference command: position predictions
are generated within the offline walk-forward evaluation. The viewer does not
recompute them.

## 8. Viewer

The platform-h viewer draws price, position bands, equity and an overview strip.
Drag to pan, wheel to zoom, shift-wheel to scroll, and use the mini-map or
Home/End/arrow keys to navigate. It uses per-monitor DPI and a cached bitmap
overview, with no direct Win32 calls.

Equity rows join to bars by **exact timestamps**, not arithmetic slot indices.
The generic readout includes currency, calendar and price basis and preserves
small-price visibility. Missing bars, missing runs and invalid options produce
visible diagnostics. `--offscreen` paints without opening a desktop window.

## 9. Build and validation

Use the immutable dd driver and [README quickstart](../README.md#build-and-test).
Project build/command declarations live outside the upstream runtime. Release
keeps `/fp:precise` and `/arch:AVX2`; there is no nondeterministic RNG path.

`.\dd.ps1 test --label crypto --non-interactive` builds and tests both
configurations. Windows has ten suites: network, market contract, generic
predictor, publication, acquisition, legacy agents/causality, dd adoption,
storage and two offscreen viewer suites. Linux has the eight non-viewer suites.
Fixtures and outputs stay under isolated project-local test artifacts.

Checks cover malformed/gapped inputs, optional volume, session rollover,
schema/metadata tampering, terminal costs, deterministic agents, cost curves,
exact legacy baselines, prefix audits and exact viewer alignment. Controlled
faults must fail before their corresponding checks are accepted as evidence.

## 10. Scope limits

- Positive-price OHLC data, one instrument per run; no close-only or
  zero/negative-price return contract.
- No live feeds, brokerage, order routing, portfolio construction or
  multi-instrument training.
- No automatic exchange calendar, timezone conversion, data-quality repair or
  generic resampling.
- No funding, stock borrow, financing, dividends, contract rolls, margin,
  liquidity/capacity model or short-sale restrictions. Source adjustments and
  corporate actions must be understood before interpreting returns.
- No supervised price/return forecasting, calibrated probabilities or
  uncertainty intervals.
- No claim that crypto findings transfer to equities, FX or commodities.

A generic input interface is useful precisely because it allows those
hypotheses to be tested honestly. It is not a substitute for the tests.
