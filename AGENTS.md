# AGENTS.md

Instructions for agents working in price-app.

## Contracts and evidence

Read [docs/design.md](docs/design.md) before changing code. It describes the
implemented position predictor, data formats, causality and evaluation rules.
When code and design disagree, fix the code or explicitly agree a contract
change; do not silently diverge.

Read [docs/experiments.md](docs/experiments.md) before proposing an improvement.
It records datasets, controls, cost sensitivity, rejected hypotheses and
engineering measurements. Add measured outcomes, including negative ones.
Keep it an experiment record, not an implementation or dependency change log.
Other Markdown describes the current system, not the steps used to build it.

## Data and prediction

- The output is a target position: short, flat or long. Do not describe it as
  a price forecast, calibrated probability or live trading system.
- Generic input is an explicitly declared OHLC/OHLCV CSV, stored as SPQB v2.
  Preserve source, currency, calendar, price basis, optional-volume availability
  and per-bar session IDs. Never invent flow, trade counts or bars in gaps.
- A calendar label is not a built-in exchange calendar. Supplied session IDs
  carry the producer's session/DST/holiday decisions. Reject malformed data;
  do not silently reinterpret timestamp units, skip bad rows or fill prices.
- Generic data uses the versioned eleven-column `ohlc-v1` feature profile.
  SPQB v1 retains the twenty-column Binance reproduction profile. Bind agents
  by feature name, not magic column offsets.
- Never automatically rewrite old bars, runs or checkpoints. Schema and
  numerical changes require explicit contracts and verification checks.

## Causality and experiments

1. Features and decisions use only information available at the decision close.
   The realised return/reward is observed after the next close, never supplied
   as an input. Streaming indicators own state, not an indexable future series.
2. No centred windows or whole-file normalisation. Keep preprocessing causal.
   Run `cli audit` on both profiles after touching feature computation.
3. Seed from configuration using `rng` in `src/common.h`. Never use
   `std::random_device` or standard distributions for reproducible experiments.
4. Report costs and sensitivity at 1, 2, 5 and 10 bps, not gross returns alone.
   Generic rewards include terminal liquidation costs. Funding, borrow and
   other unsupported market economics must remain explicit limitations.
5. Keep constants, rules, shuffled returns and the rotated timing control.
   For generic data, shuffling matches elapsed durations; the timing control
   counter-rotates returns against unchanged positions/costs. Do not manufacture
   returns across pooled fold boundaries or claim unmodelled carrying costs
   are matched.
6. Select parameters on chronological validation windows, never the test table.
   Match calendar spans when comparing intervals or instruments. Record the
   feature, data, timing and cost contracts with every result.
7. Statistical significance is not an economic edge. Pattern claims need the
   persistence/cost screen (`PAYS`), not just a t-statistic. A degenerate null is
   `UNTESTED`; a study without the required data is unavailable.
8. Earn complexity: constants, rules, tabular Q, then DQN. The recorded DQN has
   not beaten the rule rung after costs. Negative results are expected.

## Architecture and dependencies

`cli.exe` imports, trains, evaluates and writes. `app.exe` only reads and draws.
Never put training or file writing in the viewer, or drawing/platform-h in the
CLI. In `src/app.cpp`, use `pf::`, not Win32 types or headers.

Four immutable full revisions are declared in
[cmake/dd-dependencies.json](cmake/dd-dependencies.json): libcmlp owns neural
math; libcfindata owns neutral transport/JSON/CSV/date utilities; spike-db is a storage benchmark
dependency; platform-h is viewer-only. The storage benchmark is SPQB v1-only:
never discard v2 metadata to force parity. Do not copy shared-library math,
HTTP, parsing or storage implementations into the application.

Binance acquisition is application-owned C11 in `src/binance`, built once as
`binance_acquisition` and used by `cli download`. Keep its `fg_` contracts and
`tests/binance_tests.c` regression suite, and use only public libcfindata
headers. OpenSSL SHA256 verification precedes libarchive extraction and all
callbacks; retain archive/row limits, exact timestamps/decimals and spot/UM/CM
semantics. There is no checksum bypass or silent fallback.

The upstream dd 0.2.2 runtime is immutable. Do not edit `dd.ps1` or `.dd/`;
their hashes are in [docs/dd-upstream.json](docs/dd-upstream.json). Extend
[dd.psd1](dd.psd1) and [cmake/project-command.ps1](cmake/project-command.ps1).
Do not add parallel build drivers or move dependency pins without coordination.

Normal dependency acquisition uses recorded commits, not sibling checkouts.
Explicit `CRYPTO_SOURCE_*` overrides must point at clean matching revisions;
local override evidence is not proof of remote acquisition. Do not change
another user's snapshots to make a build pass.

## Build and validation

C++20, CMake/Ninja, PowerShell 7.4+. Windows uses MSVC; Linux builds the CLI
without configuring platform-h. Read [README.md](README.md) for vcpkg setup and
the dependency list, which the adoption test keeps in step with the pins.
Windows uses static acquisition archives with `/MD` or `/MDd`; retain
`/fp:precise` and Release `/arch:AVX2`.

```powershell
.\dd.ps1 build release
.\dd.ps1 test --label crypto --non-interactive
.\dd.ps1 data --dry-run
.\dd.ps1 app  # offscreen
```

The internal CMake targets are `crypto_cli` and `crypto_app`, with shared
`crypto_core` and `crypto_network` libraries. Keep those identifiers and the
`crypto` test label aligned across CMake, dd and CI.

**Do not launch a desktop viewer to validate a change.** Use the label-filtered
command above, which checks both configurations and paints offscreen. Bare
upstream `dd test` also launches a desktop GUI smoke and is not safe unattended.
`--show true` is an explicit user-facing action only.

Tests use project-local build/test artifacts or `tmp/`, never persistent
`data/` or `runs/`. No market download is needed. The repository and every
dependency are public under MIT: CI uses no secrets, and pull requests from
forks run the same checks. Never add secrets or `pull_request_target` to CI.

A check must be seen to fail: inject a controlled fault, rebuild, observe the
intended failure, restore it and rerun. Keep exact legacy agent regression
gates and generic determinism/cost/schema checks. After extraction, inspect
call sites and confirm replaced logic is gone; a green build alone is not
proof that the new path is used.

## Style and scope

- Follow C++20, `snake_case`, tabs and existing formatting.
- Use RAII and flat `std::vector` storage. No new owning raw pointers.
- Features, rewards, replay and metrics stay binary64. The DQN alone uses
  checked binary32 I/O through libcmlp and records its numerical contract.
- Keep comments short and limited to non-obvious reasoning.
- Return a code and stderr message at the CLI boundary; do not hide failures
  behind default values or successful-looking output.
- Paths come from arguments and are relative to the working directory, not
  hard-coded machine paths.
- Never commit `build/`, `exe/`, `intermediate/`, `.vs/`, `vcpkg_installed/`,
  `data/`, `runs/` or `tmp/`.
