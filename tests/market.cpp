#include "agents.h"
#include "bars.h"
#include "features.h"
#include "metrics.h"
#include "run.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace
{
	int checks = 0;

	void require(const bool ok, const char* label)
	{
		++checks;
		if (!ok) throw std::runtime_error(label);
	}

	bar candle(const int64_t time, const double close)
	{
		bar b;
		b.time = time;
		b.open = b.close = close;
		b.high = close + 1.0;
		b.low = close - 1.0;
		return b;
	}
}

int main(const int argc, char** argv)
{
	try
	{
		const auto root = fs::path(argc > 1 ? argv[1] : "tmp/market-tests") /
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		fs::create_directories(root);
		bar_series series;
		series.generic = true;
		series.symbol = "TEST";
		series.interval_ms = 900000;
		series.market = { "synthetic", "USD", "supplied-sessions", "raw", false };
		series.bars = { candle(1709933400000, 100), candle(1709934300000, 102),
			candle(1710181800000, 104), candle(1710182700000, 103) };
		series.sessions = { 1, 1, 2, 2 };
		std::string error;
		const auto path = (root / "bars.bin").string();
		require(write_bars(path, series, &error), "write generic bars");
		bar_series restored;
		require(read_bars(path, restored, &error), "read generic bars");
		require(restored.size() == 4 && restored.generic && restored.sessions == series.sessions,
			"session gaps survive without fabricated bars");
		require(hash_bars(series) == hash_bars(restored), "versioned bar round trip");
		restored.market.currency = "EUR";
		require(hash_bars(series) != hash_bars(restored), "currency participates in the data hash");
		restored = series;
		restored.sessions[1] = 2;
		require(hash_bars(series) != hash_bars(restored), "session boundaries participate in the data hash");
		require(bar_index_at_time(series, series.bars[2].time) == 2, "gapped timestamp lookup");
		require(bar_index_at_time(series, series.bars[0].time + 1) == series.size(), "timestamp lookup is exact");
		require(bar_index_at_time(series, series.bars[1].time + series.interval_ms) == series.size(), "closed interval has no bar");
		const std::vector<xfloat> returns{ 0.1, 2.0, 0.3 };
		const auto shuffled = shuffled_returns(returns, series, 7);
		require(shuffled[1] == 2.0 && std::abs(shuffled[0] + shuffled[2] - 0.4) < 1e-12,
			"shuffled control does not exchange overnight and intraday horizons");
		require(shuffled == shuffled_returns(returns, series, 7), "duration-matched shuffle reproduces");
		restored = series;
		restored.sessions.pop_back();
		require(!write_bars((root / "invalid.bin").string(), restored, &error), "missing sessions rejected before writing");
		require(!fs::exists(root / "invalid.bin"), "invalid bars leave no output");
		const auto corrupt_path = root / "trailing.bin";
		fs::copy_file(path, corrupt_path);
		{ std::ofstream trailing(corrupt_path, std::ios::binary | std::ios::app); trailing.put('x'); }
		require(!read_bars(corrupt_path.string(), restored, &error), "SPQB v2 rejects inconsistent record sizes");

		daily_levels levels;
		const int64_t midnight = 1704153600000;
		levels.push(candle(midnight - 900000, 100), 10);
		levels.push(candle(midnight, 110), 10);
		require(!levels.ready(), "UTC midnight does not split an explicitly supplied session");
		levels.push(candle(midnight + 900000, 120), 11);
		require(levels.ready() && levels.previous_high() == 111, "only a completed session publishes its high");
		require(levels.previous_low() == 99, "previous session retains its full range");

		const auto features = build_features(series);
		require(features.columns == 11 && features.names.back() == "pd_high_dist", "price-only schema");
		require(features.column("pd_high_dist") == 10, "named price-only binding");
		std::vector<xfloat> state(14, 0.0);
		state[10] = 0.5;
		auto breakout = make_agent("breakout", features, {}, {}, 7);
		require(breakout->act(state, true) == position_to_action(1), "breakout reads the price-only high distance");
		state[10] = -0.5;
		require(breakout->act(state, true) == position_to_action(0), "breakout returns flat below the level");
		auto crossover = make_agent("crossover", features, {}, {}, 7);
		state[features.column("trend_fast")] = -2.0;
		require(crossover->act(state, true) == position_to_action(-1), "crossover named binding");
		auto usable = features;
		usable.warmup = 0;
		env terminal(series, usable, {});
		terminal.reset(0, 1);
		bool done = false;
		const xfloat reward = terminal.step(position_to_action(1), done);
		require(done && std::abs(reward - (terminal.last_gross() - terminal.last_cost())) < 1e-14,
			"generic terminal reward includes the reported liquidation cost");
		require(terminal.last_units() == 2.0 && terminal.last_cost() == terminal.cost_per_unit() * 2.0,
			"entry and liquidation are both counted as charged units");

		trade_log costed;
		costed.add(0, 100, 1, 0.003, 0.0005, 0, 0.003, 1.0);
		costed.add(900000, 100, 0, 0.0, 0.0005, 0, 0.001, 1.0);
		const auto paid = compute_metrics(costed, 900000);
		require(paid.cost_units == 2.0 && std::abs(paid.market_return - 0.004) < 1e-15, "units and market return are pooled");
		require(std::abs(breakeven_cost_bps(paid, 0.0) - 30.0) < 1e-9, "break-even is linear in the units charged");
		require(std::abs(breakeven_cost_bps(paid, 1.0) - 28.0) < 1e-9, "slippage takes its share of the break-even");
		require(std::isnan(breakeven_cost_bps(paid, 16.0)), "no nonnegative cost pays once slippage consumes the gross");
		costed.add(1800000, 100, 1, -0.01, 0.0, 0, -0.01, 0.0);
		require(std::isnan(breakeven_cost_bps(compute_metrics(costed, 900000), 0.0)), "a losing gross has no break-even");

		run_writer failing;
		require(failing.open(root.string(), "write-failure", &error) && failing.write_metrics_header() && failing.ok(),
			"metrics header written");
		const fs::path metrics_path = failing.path("metrics.csv");
		fs::permissions(metrics_path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
			fs::perm_options::remove);
		// A privileged account can still write a read-only file; there is no fault to inject then.
		if (!std::ofstream(metrics_path, std::ios::app))
		{
			require(!failing.append_metrics("ensemble", 0, 5.0, -1, paid), "a refused append reports failure");
			require(failing.append_learning(1, 0.0, 0.0, 0.0) && !failing.ok(), "a failed write is remembered by the run");
		}
		fs::permissions(metrics_path, fs::perms::owner_write, fs::perm_options::add);

		trade_log regular, gapped;
		for (int i = 0; i < 4; ++i)
		{
			const double gross = i % 2 ? 0.001 : 0.003;
			regular.add(i * 900000, 100, 1, gross, 0.0001);
			gapped.add(i * 900000, 100, 1, gross, 0.0001, 1800000, gross);
		}
		const auto a = compute_metrics(regular, 900000);
		const auto b = compute_metrics(gapped, 900000);
		require(a.net_return == b.net_return, "elapsed time does not change realised returns");
		require(std::abs(b.annual_return * 2.0 - a.annual_return) < 1e-12, "annual return uses scored elapsed time");
		require(std::abs(b.sharpe * std::sqrt(2.0) - a.sharpe) < 1e-12, "Sharpe uses scored sampling rate");
		trade_log tiny;
		tiny.add(1709933400000, 1e-12, 0, 0.0, 0.0, 900000, 0.0);
		run_writer writer;
		require(writer.open(root.string(), "tiny-price", &error) && writer.write_equity(tiny), "write tiny price prediction");
		std::ifstream equity(writer.path("equity.csv"));
		std::string line;
		std::getline(equity, line);
		std::getline(equity, line);
		xfloat price = 0.0;
		require(parse_real(split(line, ',')[1], price) && price == 1e-12, "generic price output does not round a positive price to zero");

		auto legacy = series;
		legacy.generic = false;
		legacy.sessions.clear();
		const auto legacy_path = (root / "legacy.bin").string();
		require(write_bars(legacy_path, legacy, &error), "write SPQB v1");
		require(read_bars(legacy_path, restored, &error) && !restored.generic, "read SPQB v1");
		require(build_features(restored).columns == 20, "legacy feature schema retained");
		std::cout << checks << " market contract checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
