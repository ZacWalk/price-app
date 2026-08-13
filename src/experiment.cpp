#include "experiment.h"

#include <algorithm>
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

	void print_row(const std::string& label, const metrics& m)
	{
		char buf[256];
		snprintf(buf, sizeof buf,
			"  %-22s %9.4f %8.2f %9.4f %8.3f %8.4f %7.0f%%",
			label.c_str(), m.net_return, m.sharpe, m.max_drawdown, m.turnover,
			m.annual_return, m.cost_fraction * 100.0);
		std::cout << buf << '\n';
	}

	void print_header()
	{
		std::cout << "  " << std::string(22, ' ')
			<< "      net   sharpe    maxdd turnover   annual    cost%\n";
	}
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
		f.test_start = f.train_end + config.gap_bars;
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
			environment.last_gross(), environment.last_cost());
	}
}

int run_experiment(const experiment_config& config, std::string* error)
{
	bar_series series;
	if (!read_bars(config.bars_path, series, error)) return 2;

	if (series.bars.size() < 5000)
	{
		if (error) *error = "not enough bars; build a longer range first";
		return 2;
	}

	std::cout << "bars      " << series.bars.size() << " x " << series.interval_ms << "ms  "
		<< format_time_ms(series.bars.front().time) << " .. "
		<< format_time_ms(series.bars.back().time) << '\n';

	const feature_matrix features = build_features(series);
	const int state_size = features.columns + 3;

	env_config base_env;
	base_env.cost_bps = config.cost_bps;
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
		<< "  gap " << config.gap_bars << "  test " << config.test_bars << "\n\n";

	const std::string id = make_run_id(config.agent, config.base_seed);
	run_writer writer;
	if (!writer.open(config.runs_root, id, error)) return 2;
	writer.write_metrics_header();

	const std::vector<std::string> baselines = { "flat", "long", "random", "crossover" };
	std::map<std::string, variant_result> results;

	std::vector<xfloat> raw_returns(series.bars.size() - 1);
	for (size_t i = 0; i + 1 < series.bars.size(); i += 1)
	{
		raw_returns[i] = std::log(series.bars[i + 1].close / series.bars[i].close);
	}

	// The learner, once per seed, plus its controls.
	trade_log headline;
	xfloat headline_sharpe = -1e18;
	int headline_seed = 0;

	for (int seed_index = 0; seed_index < config.seeds; seed_index += 1)
	{
		const uint64_t seed = config.base_seed + static_cast<uint64_t>(seed_index) * 7919ull;

		std::unique_ptr<agent> learner = make_agent(config.agent, state_size, features.columns,
			config.dqn, config.tabular, seed);
		if (!learner)
		{
			if (error) *error = "unknown agent: " + config.agent;
			return 2;
		}

		env environment(series, features, base_env);
		trade_log log;
		int episode_counter = 0;

		// The cost curve and the control must see the policy at the same stage of
		// learning as the headline, so every variant is evaluated inside the fold
		// loop rather than once at the end.
		std::vector<xfloat> alt_costs;
		for (const xfloat bps : config.cost_curve) if (bps != config.cost_bps) alt_costs.push_back(bps);

		std::vector<env> alt_envs;
		std::vector<trade_log> alt_logs(alt_costs.size());
		alt_envs.reserve(alt_costs.size());

		for (const xfloat bps : alt_costs)
		{
			env_config alt = base_env;
			alt.cost_bps = bps;
			alt_envs.emplace_back(series, features, alt);
		}

		const std::vector<xfloat> shuffled = shuffled_returns(raw_returns, seed);
		env control_env(series, features, base_env);
		control_env.set_return_override(&shuffled);
		trade_log control_log;

		for (size_t i = 0; i < folds.size(); i += 1)
		{
			const fold& f = folds[i];
			train_on_fold(*learner, environment, f, config, seed_index == 0 ? &writer : nullptr, episode_counter);

			trade_log fold_log;
			evaluate(*learner, environment, series, f.test_start, f.test_end, fold_log);
			writer.append_metrics(config.agent, static_cast<int>(seed), config.cost_bps,
				static_cast<int>(i), compute_metrics(fold_log, series.interval_ms));

			for (size_t j = 0; j < fold_log.size(); j += 1)
			{
				log.add(fold_log.time[j], fold_log.price[j], fold_log.position[j],
					fold_log.gross[j], fold_log.cost[j]);
			}

			for (size_t c = 0; c < alt_envs.size(); c += 1)
			{
				evaluate(*learner, alt_envs[c], series, f.test_start, f.test_end, alt_logs[c]);
			}

			evaluate(*learner, control_env, series, f.test_start, f.test_end, control_log);
		}

		const metrics m = compute_metrics(log, series.interval_ms);
		results[config.agent].name = config.agent;
		results[config.agent].per_seed.push_back(m);
		writer.append_metrics(config.agent, static_cast<int>(seed), config.cost_bps, -1, m);

		std::cout << "seed " << seed << "  net " << m.net_return
			<< "  sharpe " << m.sharpe << "  trades " << m.trades << '\n';

		if (m.sharpe > headline_sharpe)
		{
			headline_sharpe = m.sharpe;
			headline_seed = static_cast<int>(seed);
			headline = log;
			learner->save(writer.path(config.agent == "qtable" ? "policy.csv" : "weights.bin"));
		}

		for (size_t c = 0; c < alt_costs.size(); c += 1)
		{
			writer.append_metrics(config.agent, static_cast<int>(seed), alt_costs[c], -1,
				compute_metrics(alt_logs[c], series.interval_ms));
		}

		const metrics cm = compute_metrics(control_log, series.interval_ms);
		results["control:shuffled"].name = "control:shuffled";
		results["control:shuffled"].per_seed.push_back(cm);
		writer.append_metrics("control:shuffled", static_cast<int>(seed), config.cost_bps, -1, cm);
	}

	// Baselines and the reversed-signal control share one pass; they do not learn.
	for (const std::string& name : baselines)
	{
		std::unique_ptr<agent> baseline = make_agent(name, state_size, features.columns,
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

	writer.write_equity(headline);

	writer.set("agent", config.agent);
	writer.set("bars", config.bars_path);
	writer.set("symbol", series.symbol);
	writer.set("interval_ms", series.interval_ms);
	writer.set("bar_count", static_cast<int64_t>(series.bars.size()));
	writer.set("data_hash", hex64(hash_bars(series)));
	writer.set("base_seed", static_cast<int64_t>(config.base_seed));
	writer.set("headline_seed", static_cast<int64_t>(headline_seed));
	writer.set("seeds", config.seeds);
	writer.set("train_bars", config.train_bars);
	writer.set("test_bars", config.test_bars);
	writer.set("gap_bars", config.gap_bars);
	writer.set("episode_bars", config.episode_bars);
	writer.set("epochs", config.epochs);
	writer.set("folds", static_cast<int64_t>(folds.size()));
	writer.set("cost_bps", config.cost_bps);
	writer.set("hold_penalty", config.hold_penalty);
	writer.set("feature_columns", features.columns);
	writer.set("warmup_bars", features.warmup);
	writer.write_manifest();

	std::cout << "\nWalk-forward, out of sample, " << config.cost_bps << " bps round trip\n";
	print_header();

	for (const std::string& name : baselines) print_row(name, median_by_sharpe(results[name].per_seed));
	print_row(config.agent + " (median)", median_by_sharpe(results[config.agent].per_seed));
	print_row("control:shuffled", median_by_sharpe(results["control:shuffled"].per_seed));

	const metrics learned = median_by_sharpe(results[config.agent].per_seed);
	const metrics control = median_by_sharpe(results["control:shuffled"].per_seed);

	std::cout << "\nrun       " << writer.directory() << '\n';

	if (control.sharpe > learned.sharpe && control.sharpe > 0.5)
	{
		std::cout << "\nFAILED CONTROL: the shuffled-return run scores higher than the real one.\n";
		return 1;
	}

	return 0;
}
