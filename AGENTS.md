# AGENTS.md

Instructions for AI agents working in this repository.

## Read the design first

**[docs/design.md](docs/design.md) is the contract.** It defines the data
pipeline, the RL formulation, the causality guarantees, the evaluation
methodology, and explicitly lists what is and is not built. When the code and
the design disagree, the design is right — fix the code, or propose a design
change explicitly. Never silently diverge.

## Hard rules

**1. No dependencies.** C++20, the standard library, and Win32. Nothing else.
Do not add a package manager, a submodule, a vendored library, a JSON parser, a
charting library, or an HTTP client. If a task seems to need one, write the
minimum yourself or say the task is out of scope. `exe/` once shipped
`libssl-3-x64.dll` and `libcrypto-3-x64.dll`; that is the failure mode this rule
exists to prevent.

**2. `cli.exe` runs experiments. `app.exe` views them.** Never add training,
evaluation or file writing to the viewer. Never add a window, dialog or GDI call
to the CLI.

**3. Causality is non-negotiable.** No feature, label or reward may read a bar
later than the one it is computed for.

- Centred windows (`sum -= x[i-r]; sum += x[i+r]`) are banned. They were the
  original bug.
- Normalisation uses expanding or trailing statistics, never whole-file
  statistics.
- Indicators are streaming objects with `push(x) -> value`. They own their state
  and never hold the series, so they *cannot* index forwards. Keep it that way.
- Run `cli audit` after touching `features.cpp`. It must pass.

**4. Reproducibility.** Seed from the run config. Never `std::random_device`,
and never a `std::` distribution — those are not specified bit-exactly. Use the
`rng` in `common.h`. Keep release builds on `/fp:precise`.

**5. Costs are always applied.** Never report a return without a cost model, and
never a single cost level alone — the run reports 1, 2, 5 and 10 bps.

**6. Never present a result without its control.** The shuffled-return control
runs alongside every experiment. If it scores as well as the real run, the
result is noise and must be reported as such. A negative result is a valid and
expected outcome.

**7. Never commit build output or data.** `exe/`, `intermediate/`, `.vs/`,
`data/` and `runs/` are gitignored.

**8. No absolute paths in source.** Paths come from the command line, resolved
relative to the working directory.

## Building

**Always build `spq.sln`, never `src\*.vcxproj` directly.** The projects set
`OutDir` to `$(SolutionDir)exe\`; building a project file makes `SolutionDir`
the *project* directory, so output silently lands in `src\exe\` and you then
test a stale `exe\cli.exe`. This has already caused a false "test passed".

```powershell
.\dd.ps1 build          # correct
.\dd.ps1 data           # download trades
.\dd.ps1 bars           # aggregate to bars
.\dd.ps1 run -- --agent qtable
.\dd.ps1 app
```

Do not add parallel build scripts. Extend `dd.ps1` if a step is missing.

New source files must be added to the relevant `.vcxproj` **and** its
`.filters` file. `cli.vcxproj` builds `cli.exe`; `app.vcxproj` builds `app.exe`.

## Verifying

A test that has never been seen to fail is not evidence. When you add or rely on
a check, prove it goes red: inject the fault, rebuild **through the solution**,
confirm the failure, then revert. That is how `cli audit` was validated.

Likewise, a green build does not prove a refactor landed. Grep for call sites of
anything you extracted, and for the distinctive strings of whatever you replaced.

## Working style

- Earn complexity. Baselines, then rule agents, then tabular Q, then DQN. Do not
  skip rungs; each must beat the one below it out-of-sample.
- Prefer deleting dead code to working around it.
- Report negative results plainly. "No edge after costs" is the expected
  outcome and the bench exists to establish it quickly.
- Keep comments to what the code cannot show. One short line, not a paragraph.

## Style

- C++20, `snake_case`, tabs, matching the existing source.
- RAII everywhere. Owning raw pointers are banned in new code — use
  `std::vector`. Respect the rule of five or delete the copy operations.
- Prefer a flat `std::vector<xfloat>` with computed offsets over
  pointer-to-pointer-to-pointer tensors.
- No exceptions across the CLI boundary; return codes and a message on stderr.
