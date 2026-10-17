#include "experiment.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>

#include "run.h"

namespace
{
	struct variant_result
	{
		std::string name;
		std::vector<metrics> per_seed;
	};

	metrics median_by_sharpe(std::vector<metrics> values)
	{
		if (values.empty()) return {};
		std::sort(values.begin(), values.end(),
			[](const metrics& a, const metrics& b) { return a.sharpe < b.sharpe; });
		return values[values.size() / 2];
	}

	std::string bps_cell(const xfloat bps)
	{
		char cell[16];
		if (std::isnan(bps)) snprintf(cell, sizeof cell, "%8s", "-");
		else snprintf(cell, sizeof cell, "%8.1f", bps);
		return cell;
	}

	void print_row(const std::string& label, const metrics& m, const xfloat slippage_bps)
	{
		// A share of a gross that is not positive is undefined; printed as 0% it
		// read as "no cost" on a crossover that had paid 0.19 in costs.
		char share[16];
		if (m.gross_return > 0.0) snprintf(share, sizeof share, "%7.0f%%", m.cost_fraction * 100.0);
		else snprintf(share, sizeof share, "%8s", "-");

		// One cost level hides how close a result sits to zero; b/e is the round
		// trip at which it would be.
		char buf[256];
		snprintf(buf, sizeof buf,
			"  %-22s %9.4f %8.2f %9.4f %8.3f %8.4f %s %s",
			label.c_str(), m.net_return, m.sharpe, m.max_drawdown, m.turnover,
			m.annual_return, bps_cell(breakeven_cost_bps(m, slippage_bps)).c_str(), share);
		std::cout << buf << '\n';
	}

	void print_header()
	{
		char buf[256];
		snprintf(buf, sizeof buf, "  %-22s %9s %8s %9s %8s %8s %8s %8s",
			"", "net", "sharpe", "maxdd", "turnover", "annual", "b/e bps", "cost%");
		std::cout << buf << '\n';
	}

	void append_log(trade_log& into, const trade_log& from)
	{
		for (size_t i = 0; i < from.size(); i += 1)
		{
			into.add(from.time[i], from.price[i], from.position[i], from.gross[i], from.cost[i],
				from.elapsed_ms[i], from.market_return[i], from.units[i]);
		}
	}

	// Turnover-matched control: the same position sequence, rotated in time. It
	// keeps exposure, holding periods and cost exactly, and destroys only the
	// alignment with returns. The shuffled control asks whether the returns
	// matter; this one asks whether being long 15% of a rising market is enough
	// to explain the result on its own.
	trade_log rotate_positions(const trade_log& log, const size_t offset)
	{
		trade_log out;
		const size_t n = log.size();
		if (n < 3) return out;

		if (!log.elapsed_ms.empty() && log.elapsed_ms.front() > 0)
		{
			// Counter-rotate returns, keeping position transitions and costs intact.
			for (size_t i = 0; i < n; ++i)
			{
				const size_t j = (i + n - offset % n) % n;
				out.add(log.time[i], log.price[i], log.position[i], log.position[i] * log.market_return[j],
					log.cost[i], log.elapsed_ms[i], log.market_return[j], log.units[i]);
			}
			return out;
		}

		for (size_t i = 0; i + 1 < n; i += 1)
		{
			const size_t j = (i + offset) % (n - 1);
			const xfloat a = log.price[i];
			const xfloat b = log.price[i + 1];
			const xfloat r = a > 0.0 && b > 0.0 ? std::log(b / a) : 0.0;
			out.add(log.time[i], a, log.position[j], log.position[j] * r, log.cost[j], 0, r, log.units[j]);
		}

		return out;
	}

	// Calendar years of the pooled out-of-sample log, or months when it sits in
	// one year. A result carried by one period, or fading across them, is the
	// commonest false positive here. Rotating inside each period asks whether
	// timing earned it, rather than being exposed to that period's drift.
	void print_periods(const trade_log& log, const int64_t interval_ms, const xfloat cost_bps,
		const xfloat slippage_bps)
	{
		using namespace std::chrono;
		std::vector<int> years(log.size()), months(log.size());
		for (size_t i = 0; i < log.size(); i += 1)
		{
			const year_month_day day{ floor<days>(sys_time<milliseconds>(milliseconds(log.time[i]))) };
			years[i] = static_cast<int>(day.year());
			months[i] = years[i] * 12 + static_cast<int>(static_cast<unsigned>(day.month())) - 1;
		}
		const auto runs = [](const std::vector<int>& keys) {
			size_t count = keys.empty() ? 0 : 1;
			for (size_t i = 1; i < keys.size(); i += 1) if (keys[i] != keys[i - 1]) count += 1;
			return count;
		};
		const bool monthly = runs(years) < 2;
		const std::vector<int>& key = monthly ? months : years;
		if (runs(key) < 2) return;

		std::cout << "\nBy calendar " << (monthly ? "month" : "year") << " (UTC), ensemble at " << cost_bps
			<< " bps; rotated is a timing control inside each period\n";
		char buf[160];
		snprintf(buf, sizeof buf, "  %-10s %8s %9s %9s %9s %8s", "period", "bars", "market", "net", "rotated", "b/e bps");
		std::cout << buf << '\n';

		for (size_t from = 0; from < log.size();)
		{
			size_t to = from;
			while (to < log.size() && key[to] == key[from]) to += 1;

			trade_log part;
			for (size_t i = from; i < to; i += 1)
			{
				part.add(log.time[i], log.price[i], log.position[i], log.gross[i], log.cost[i],
					log.elapsed_ms[i], log.market_return[i], log.units[i]);
			}

			std::vector<metrics> rotations;
			for (int k = 1; k <= 8; k += 1)
			{
				const size_t offset = part.size() * static_cast<size_t>(k) / 9;
				rotations.push_back(compute_metrics(rotate_positions(part, offset), interval_ms));
			}
			const metrics m = compute_metrics(part, interval_ms);

			char label[16];
			if (monthly) snprintf(label, sizeof label, "%04d-%02d", key[from] / 12, key[from] % 12 + 1);
			else snprintf(label, sizeof label, "%04d", key[from]);
			snprintf(buf, sizeof buf, "  %-10s %8lld %9.4f %9.4f %9.4f %s", label, static_cast<long long>(m.bars),
				m.market_return, m.net_return, median_by_sharpe(rotations).net_return,
				bps_cell(breakeven_cost_bps(m, slippage_bps)).c_str());
			std::cout << buf << '\n';
			from = to;
		}
	}

	// Everything one seed needs to be trained and scored independently. Held by
	// pointer because control_env points into shuffled, which must not move.
	struct seed_state
	{
		uint64_t seed = 0;
		std::unique_ptr<agent> learner;
		std::unique_ptr<env> environment;
		std::unique_ptr<env> control_env;
		std::vector<std::unique_ptr<env>> alt_envs;
		std::vector<xfloat> shuffled;
		std::vector<trade_log> alt_logs;
		trade_log log;
		trade_log valid_log;
		trade_log control_log;
		int episode_counter = 0;
	};
}

std::vector<fold> make_folds(const experiment_config& config, const int first_bar, const int last_bar)
{
	std::vector<fold> folds;

	int start = first_bar;
	while (true)
	{
		fold f;
		f.train_start = start;
		f.train_end = start + config.train_bars;

		// Validation sits inside its own purge on both sides, so no feature window
		// is shared with either the training or the test data.
		f.valid_start = f.train_end + config.gap_bars;
		f.valid_end = f.valid_start + config.valid_bars;
		f.test_start = f.valid_end + (config.valid_bars > 0 ? config.gap_bars : 0);
		f.test_end = f.test_start + config.test_bars;

		if (f.test_end > last_bar) break;

		folds.push_back(f);
		start += config.test_bars;
	}

	return folds;
}

void train_on_fold(agent& learner, env& environment, const fold& f, const experiment_config& config,
	run_writer* writer, int& episode_counter)
{
	if (!learner.learns()) return;

	const int span = f.train_end - f.train_start;
	const int episode = config.episode_bars > 0 ? config.episode_bars : span;
	if (span <= 1) return;

	std::vector<xfloat> state;
	std::vector<xfloat> next;

	for (int epoch = 0; epoch < config.epochs; epoch += 1)
	{
		for (int offset = 0; offset + 2 < span; offset += episode)
		{
			const int start = f.train_start + offset;
			const int stop = std::min(start + episode, f.train_end);
			if (stop - start < 2) continue;

			environment.reset(start, stop);
			learner.begin_episode();

			xfloat total = 0.0;
			int steps = 0;
			bool done = false;

			while (!done)
			{
				state = environment.state();
				const int action = learner.act(state, false);
				const xfloat reward = environment.step(action, done);
				next = environment.state();

				learner.observe(state, action, reward, next, done);
				total += reward;
				steps += 1;
			}

			episode_counter += 1;

			if (writer && steps > 0)
			{
				writer->append_learning(episode_counter, total / steps,
					learner.exploration(), learner.recent_loss());
			}
		}

		learner.end_epoch();
	}
}

void evaluate(agent& learner, env& environment, const bar_series& series,
	const int start, const int end, trade_log& log)
{
	if (end - start < 2) return;

	environment.reset(start, end);
	learner.begin_episode();

	bool done = false;
	while (!done)
	{
		const int index = environment.index();
		const int action = learner.act(environment.state(), true);
		environment.step(action, done);

		log.add(series.bars[index].time, series.bars[index].close,
			static_cast<int32_t>(action_to_position(action)),
			environment.last_gross(), environment.last_cost(),
			series.generic ? series.bars[index + 1].time - series.bars[index].time : 0,
			environment.step_return(index), environment.last_units());
	}
}

int run_experiment(const experiment_config& config, std::string* error, std::string* directory)
{
	if (config.train_bars <= 0 || config.test_bars <= 1 || config.valid_bars < 0 ||
		config.gap_bars < 0 || config.episode_bars <= 1 || config.epochs <= 0 ||
		config.seeds <= 0 || config.seeds > 1000)
	{
		if (error) *error = "invalid walk-forward windows, epochs or seed count";
		return 2;
	}
	// A negative cost is a rebate and a negative penalty a bonus; neither is the
	// cost model every reported return must carry.
	const std::pair<const char*, xfloat> costs[] = { { "--cost-bps", config.cost_bps },
		{ "--slippage-bps", config.slippage_bps }, { "--cost-activity", config.cost_activity },
		{ "--hold-penalty", config.hold_penalty } };
	for (const auto& [flag, value] : costs)
	{
		if (!(value >= 0.0))
		{
			if (error) *error = std::string(flag) + " must be at least 0";
			return 2;
		}
	}
	if (config.agent == "dqn")
		std::cout << "DQN numerics " << DQN_NUMERICS << " (not legacy SPQN/binary64-compatible)\n";
	bar_series series;
	if (!read_bars(config.bars_path, series, error)) return 2;

	if (series.bars.size() < (series.generic ? 2u : 5000u))
	{
		if (error) *error = "not enough bars; build a longer range first";
		return 2;
	}

	std::cout << "bars      " << series.bars.size() << " x " << series.interval_ms << "ms  "
		<< format_time_ms(series.bars.front().time) << " .. "
		<< format_time_ms(series.bars.back().time) << '\n';
	if (series.generic)
		std::cout << "market    " << series.symbol << " " << series.market.currency << ", "
			<< series.market.calendar << ", " << series.market.price_basis << ", " << series.market.source << '\n'
			<< "limits    no funding, borrow, dividends, contract rolls or short-sale constraints modelled\n";

	const feature_matrix features = build_features(series);

	env_config base_env;
	base_env.cost_bps = config.cost_bps;
	base_env.slippage_bps = config.slippage_bps;
	base_env.cost_activity = config.cost_activity;
	base_env.hold_penalty = config.hold_penalty;
	base_env.episode_bars = config.episode_bars;

	env probe(series, features, base_env);
	const std::vector<fold> folds = make_folds(config, probe.first_bar(), probe.last_bar());

	if (folds.empty())
	{
		if (error) *error = "no walk-forward folds fit; reduce --train or --test";
		return 2;
	}

	std::cout << "features  " << features.columns << " base + 3 position, warmup "
		<< features.warmup << " bars\n";
	std::cout << "folds     " << folds.size() << "  train " << config.train_bars
		<< "  gap " << config.gap_bars;
	if (config.valid_bars > 0) std::cout << "  valid " << config.valid_bars;
	std::cout << "  test " << config.test_bars << "\n\n";

	const std::vector<std::string> baselines = { "flat", "long", "random", "crossover", "breakout" };
	std::map<std::string, variant_result> results;
	std::map<std::string, variant_result> valid_results;

	std::vector<xfloat> raw_returns(series.bars.size() - 1);
	for (size_t i = 0; i + 1 < series.bars.size(); i += 1)
	{
		raw_returns[i] = series.generic
			? std::log(series.bars[i + 1].close) - std::log(series.bars[i].close)
			: std::log(series.bars[i + 1].close / series.bars[i].close);
	}

	// The learner, once per seed, plus its controls. Seeds are trained side by
	// side rather than one after another so the fold's ensemble can be scored on
	// the same held-out window the individual seeds saw.
	std::vector<xfloat> alt_costs;
	for (const xfloat bps : config.cost_curve) if (bps != config.cost_bps) alt_costs.push_back(bps);

	const auto make_env = [&](const xfloat bps) {
		env_config cfg = base_env;
		cfg.cost_bps = bps;
		return std::make_unique<env>(series, features, cfg);
	};

	std::vector<std::unique_ptr<seed_state>> seeds;

	for (int seed_index = 0; seed_index < config.seeds; seed_index += 1)
	{
		auto state = std::make_unique<seed_state>();
		state->seed = config.base_seed + static_cast<uint64_t>(seed_index) * 7919ull;
		state->learner = make_agent(config.agent, features,
			config.dqn, config.tabular, state->seed);

		if (!state->learner)
		{
			if (error) *error = "unknown agent: " + config.agent;
			return 2;
		}

		state->environment = make_env(config.cost_bps);
		state->shuffled = shuffled_returns(raw_returns, series, state->seed);
		state->control_env = make_env(config.cost_bps);
		state->control_env->set_return_override(&state->shuffled);

		for (const xfloat bps : alt_costs) state->alt_envs.push_back(make_env(bps));
		state->alt_logs.resize(alt_costs.size());

		seeds.push_back(std::move(state));
	}

	// Opened only once every agent was built, so an unknown agent or a rejected
	// network configuration leaves no half-written run directory behind.
	const std::string id = make_run_id(config.agent, config.base_seed);
	run_writer writer;
	if (!writer.open(config.runs_root, id, error)) return 2;
	if (directory) *directory = writer.directory();
	writer.write_metrics_header();

	std::vector<agent*> members;
	for (const auto& state : seeds) members.push_back(state->learner.get());
	ensemble_agent ensemble(members);

	auto ensemble_env = make_env(config.cost_bps);
	auto ensemble_valid_env = make_env(config.cost_bps);
	auto ensemble_control_env = make_env(config.cost_bps);
	const std::vector<xfloat> ensemble_shuffled = shuffled_returns(raw_returns, series, config.base_seed);
	ensemble_control_env->set_return_override(&ensemble_shuffled);

	std::vector<std::unique_ptr<env>> ensemble_alt_envs;
	for (const xfloat bps : alt_costs) ensemble_alt_envs.push_back(make_env(bps));
	std::vector<trade_log> ensemble_alt_logs(alt_costs.size());

	trade_log ensemble_log;
	trade_log ensemble_valid_log;
	trade_log ensemble_control_log;

	for (size_t i = 0; i < folds.size(); i += 1)
	{
		const fold& f = folds[i];

		for (size_t s = 0; s < seeds.size(); s += 1)
		{
			seed_state& state = *seeds[s];
			train_on_fold(*state.learner, *state.environment, f, config,
				s == 0 ? &writer : nullptr, state.episode_counter);

			// The cost curve and the control must see the policy at the same
			// stage of learning as the headline, so every variant is evaluated
			// inside the fold loop rather than once at the end.
			trade_log fold_log;
			evaluate(*state.learner, *state.environment, series, f.test_start, f.test_end, fold_log);
			writer.append_metrics(config.agent, static_cast<int>(state.seed), config.cost_bps,
				static_cast<int>(i), compute_metrics(fold_log, series.interval_ms));
			append_log(state.log, fold_log);

			// Parameters are chosen here; the test window is scored once and never
			// selected on.
			if (config.valid_bars > 0)
			{
				trade_log valid_fold;
				evaluate(*state.learner, *state.environment, series, f.valid_start, f.valid_end, valid_fold);
				append_log(state.valid_log, valid_fold);
			}

			for (size_t c = 0; c < state.alt_envs.size(); c += 1)
			{
				evaluate(*state.learner, *state.alt_envs[c], series, f.test_start, f.test_end, state.alt_logs[c]);
			}

			evaluate(*state.learner, *state.control_env, series, f.test_start, f.test_end, state.control_log);
		}

		trade_log ensemble_fold;
		evaluate(ensemble, *ensemble_env, series, f.test_start, f.test_end, ensemble_fold);
		writer.append_metrics("ensemble", 0, config.cost_bps, static_cast<int>(i),
			compute_metrics(ensemble_fold, series.interval_ms));
		append_log(ensemble_log, ensemble_fold);

		if (config.valid_bars > 0)
		{
			trade_log ensemble_valid_fold;
			evaluate(ensemble, *ensemble_valid_env, series, f.valid_start, f.valid_end, ensemble_valid_fold);
			append_log(ensemble_valid_log, ensemble_valid_fold);
		}

		for (size_t c = 0; c < ensemble_alt_envs.size(); c += 1)
		{
			evaluate(ensemble, *ensemble_alt_envs[c], series, f.test_start, f.test_end, ensemble_alt_logs[c]);
		}

		evaluate(ensemble, *ensemble_control_env, series, f.test_start, f.test_end, ensemble_control_log);
	}

	xfloat net_low = 0.0;
	xfloat net_high = 0.0;

	for (size_t s = 0; s < seeds.size(); s += 1)
	{
		seed_state& state = *seeds[s];

		const metrics m = compute_metrics(state.log, series.interval_ms);
		results[config.agent].name = config.agent;
		results[config.agent].per_seed.push_back(m);
		writer.append_metrics(config.agent, static_cast<int>(state.seed), config.cost_bps, -1, m);

		std::cout << "seed " << state.seed << "  net " << m.net_return
			<< "  sharpe " << m.sharpe << "  trades " << m.trades << '\n';

		net_low = s == 0 ? m.net_return : std::min(net_low, m.net_return);
		net_high = s == 0 ? m.net_return : std::max(net_high, m.net_return);

		for (size_t c = 0; c < alt_costs.size(); c += 1)
		{
			writer.append_metrics(config.agent, static_cast<int>(state.seed), alt_costs[c], -1,
				compute_metrics(state.alt_logs[c], series.interval_ms));
		}

		const metrics cm = compute_metrics(state.control_log, series.interval_ms);
		results["control:shuffled"].name = "control:shuffled";
		results["control:shuffled"].per_seed.push_back(cm);
		writer.append_metrics("control:shuffled", static_cast<int>(state.seed), config.cost_bps, -1, cm);

		if (config.valid_bars > 0)
		{
			const metrics vm = compute_metrics(state.valid_log, series.interval_ms);
			valid_results[config.agent].name = config.agent;
			valid_results[config.agent].per_seed.push_back(vm);
			writer.append_metrics(config.agent + ":valid", static_cast<int>(state.seed), config.cost_bps, -1, vm);
		}
	}

	const metrics ensemble_metrics = compute_metrics(ensemble_log, series.interval_ms);
	const metrics ensemble_control = compute_metrics(ensemble_control_log, series.interval_ms);
	writer.append_metrics("ensemble", 0, config.cost_bps, -1, ensemble_metrics);
	writer.append_metrics("control:ensemble-shuffled", 0, config.cost_bps, -1, ensemble_control);

	// Median over several rotations, so one unlucky offset cannot decide it.
	std::vector<metrics> rotations;
	for (int k = 1; k <= 8; k += 1)
	{
		const size_t offset = ensemble_log.size() * static_cast<size_t>(k) / 9;
		rotations.push_back(compute_metrics(rotate_positions(ensemble_log, offset), series.interval_ms));
	}
	const metrics ensemble_rotated = median_by_sharpe(rotations);
	writer.append_metrics("control:ensemble-rotated", 0, config.cost_bps, -1, ensemble_rotated);

	for (size_t c = 0; c < alt_costs.size(); c += 1)
	{
		writer.append_metrics("ensemble", 0, alt_costs[c], -1,
			compute_metrics(ensemble_alt_logs[c], series.interval_ms));
	}

	// The saved policy is the first seed, chosen before any test window was
	// scored. Picking the best seed here would be selection on the test set.
	if (!seeds.empty())
	{
		if (!seeds.front()->learner->save(writer.path(config.agent == "qtable" ? "policy.csv" : "weights.bin")))
		{
			if (error) *error = "cannot save policy";
			return 2;
		}
	}

	// Baselines do not learn, so one pass over the test windows is enough.
	for (const std::string& name : baselines)
	{
		std::unique_ptr<agent> baseline = make_agent(name, features,
			config.dqn, config.tabular, config.base_seed);
		if (!baseline) continue;

		env environment(series, features, base_env);
		trade_log log;
		for (const fold& f : folds) evaluate(*baseline, environment, series, f.test_start, f.test_end, log);

		const metrics m = compute_metrics(log, series.interval_ms);
		results[name].name = name;
		results[name].per_seed.push_back(m);
		writer.append_metrics(name, 0, config.cost_bps, -1, m);
	}

	if (!writer.write_equity(ensemble_log))
	{
		if (error) *error = "cannot write equity.csv";
		return 2;
	}

	std::string hidden;
	for (const int width : config.dqn.hidden)
	{
		if (!hidden.empty()) hidden += ',';
		hidden += std::to_string(width);
	}

	writer.set("agent", config.agent);
	writer.set("experiment_scalar", "binary64");
	writer.set("bar_format", bar_format_name(series));
	writer.set("prediction", "short-flat-long");
	writer.set("feature_profile", features.profile);
	writer.set("feature_schema", features.schema());
	writer.set("normalisation", "causal-expanding-clamped-v1");
	writer.set("decision_time", "bar-close");
	writer.set("horizon", "next-observed-bar");
	writer.set("annualisation", series.generic ? "scored-elapsed-time" : "continuous-interval");
	if (series.generic)
	{
		writer.set("source", series.market.source);
		writer.set("currency", series.market.currency);
		writer.set("calendar", series.market.calendar);
		writer.set("price_basis", series.market.price_basis);
		writer.set("volume_available", series.market.volume_available ? 1 : 0);
		writer.set("flow_available", 0);
		writer.set("exposure_basis", "observed-bars");
		writer.set("execution", "decision-close-plus-slippage");
		writer.set("carry_costs", "not-modelled");
		writer.set("reward_costs", "entry-switch-and-terminal");
		writer.set("shuffled_control", "equal-elapsed-duration");
		writer.set("rotated_control", "bar-exposure-and-recorded-cost-matched");
	}
	writer.set("dependency_manifest_sha256", CRYPTO_DEPENDENCY_MANIFEST_SHA256);
	if (config.agent == "dqn")
	{
		writer.set("dqn_numerics", DQN_NUMERICS);
		writer.set("libcmlp_revision", CRYPTO_CMLP_REVISION);
		writer.set("dqn_model_format", "CMLP-v1-binary32");
		writer.set("dqn_initialization", "libcmlp-He-hidden-Xavier-linear");
		writer.set("dqn_seed_mapping", "mix64-xor-halves-zero-to-one-v1");
		writer.set("dqn_network_seed", static_cast<int64_t>(dqn_network::network_seed(config.base_seed)));
		writer.set("dqn_optimizer", "Adam-beta1=0.9-beta2=0.999-epsilon=1e-8-clip=10");
	}
	writer.set("headline", "ensemble");
	writer.set("bars", config.bars_path);
	writer.set("symbol", series.symbol);
	writer.set("interval_ms", series.interval_ms);
	writer.set("bar_count", static_cast<int64_t>(series.bars.size()));
	writer.set("data_hash", hex64(hash_bars(series)));
	writer.set("base_seed", static_cast<int64_t>(config.base_seed));
	writer.set("policy_seed", static_cast<int64_t>(seeds.empty() ? 0 : seeds.front()->seed));
	writer.set("seeds", config.seeds);
	writer.set("train_bars", config.train_bars);
	writer.set("valid_bars", config.valid_bars);
	writer.set("test_bars", config.test_bars);
	writer.set("gap_bars", config.gap_bars);
	writer.set("episode_bars", config.episode_bars);
	writer.set("epochs", config.epochs);
	writer.set("folds", static_cast<int64_t>(folds.size()));
	writer.set("cost_bps", config.cost_bps);
	writer.set("slippage_bps", config.slippage_bps);
	writer.set("cost_activity", config.cost_activity);
	writer.set("hold_penalty", config.hold_penalty);
	writer.set("hidden", hidden);
	writer.set("lr", config.dqn.learning_rate);
	writer.set("lr_decay", config.dqn.lr_decay);
	writer.set("gamma", config.dqn.discount);
	writer.set("epsilon_decay", config.dqn.epsilon_decay_steps);
	writer.set("batch", config.dqn.batch_size);
	writer.set("target_sync", config.dqn.target_sync);
	writer.set("switch_margin", config.dqn.switch_margin);
	writer.set("q_lr", config.tabular.learning_rate);
	writer.set("feature_columns", features.columns);
	writer.set("warmup_bars", features.warmup);
	if (!writer.write_manifest())
	{
		if (error) *error = "cannot write manifest.txt";
		return 2;
	}
	if (!writer.ok())
	{
		if (error) *error = "failed writing metrics.csv or learning.csv in " + writer.directory();
		return 2;
	}

	std::cout << "\nWalk-forward, out of sample, " << config.cost_bps << " bps round trip\n";
	print_header();

	const xfloat slip = config.slippage_bps;
	for (const std::string& name : baselines) print_row(name, median_by_sharpe(results[name].per_seed), slip);
	print_row(config.agent + " (median)", median_by_sharpe(results[config.agent].per_seed), slip);
	print_row(config.agent + " (ensemble)", ensemble_metrics, slip);
	print_row("control:shuffled", median_by_sharpe(results["control:shuffled"].per_seed), slip);
	print_row("control:ensemble", ensemble_control, slip);
	print_row("control:rotated", ensemble_rotated, slip);
	print_periods(ensemble_log, series.interval_ms, config.cost_bps, slip);

	if (config.valid_bars > 0)
	{
		// Select on this table. The one above is scored once and reported, never
		// searched over, or the number it prints means nothing.
		std::cout << "\nValidation window, for choosing parameters\n";
		print_header();
		print_row(config.agent + " (median)", median_by_sharpe(valid_results[config.agent].per_seed), slip);
		print_row(config.agent + " (ensemble)", compute_metrics(ensemble_valid_log, series.interval_ms), slip);
	}

	// The seed band is the noise floor. A difference smaller than it is not a
	// result, whichever direction it points.
	std::cout << "\nseed band  net " << net_low << " .. " << net_high
		<< "  width " << (net_high - net_low) << '\n';

	std::cout << "run        " << writer.directory() << '\n';

	if (ensemble_control.sharpe > ensemble_metrics.sharpe && ensemble_control.sharpe > 0.5)
	{
		std::cout << "\nFAILED CONTROL: the shuffled-return run scores higher than the real one.\n";
		return 1;
	}

	return 0;
}
