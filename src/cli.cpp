// cli.exe - runs experiments. Headless, deterministic, writes to runs/.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "bars.h"
#include "common.h"
#include "experiment.h"
#include "features.h"
#include "run.h"

namespace fs = std::filesystem;

namespace
{
	class options
	{
	public:
		options(const int argc, char** argv)
		{
			for (int i = 2; i < argc; i += 1)
			{
				std::string token = argv[i];
				if (token.rfind("--", 0) != 0) continue;

				token = token.substr(2);
				if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
				{
					values_[token] = argv[i + 1];
					i += 1;
				}
				else
				{
					values_[token] = "1";
				}
			}
		}

		bool has(const std::string& key) const { return values_.count(key) > 0; }

		std::string text(const std::string& key, const std::string& fallback) const
		{
			const auto it = values_.find(key);
			return it == values_.end() ? fallback : it->second;
		}

		int64_t number(const std::string& key, const int64_t fallback) const
		{
			const auto it = values_.find(key);
			if (it == values_.end()) return fallback;
			return strtoll(it->second.c_str(), nullptr, 10);
		}

		xfloat real(const std::string& key, const xfloat fallback) const
		{
			const auto it = values_.find(key);
			if (it == values_.end()) return fallback;
			xfloat out = fallback;
			parse_real(it->second, out);
			return out;
		}

		const std::map<std::string, std::string>& all() const { return values_; }

	private:
		std::map<std::string, std::string> values_;
	};

	int usage()
	{
		std::cout <<
			"spq cli - runs experiments\n\n"
			"  cli bars   --in <dir|file> --out <file> [--interval 1s] [--symbol ETHUSDT]\n"
			"      Aggregate Binance trade CSVs into a causal bar file.\n\n"
			"  cli walk   --bars <file> [--agent dqn] [--seeds 3] [--train 40000]\n"
			"             [--test 10000] [--gap 2000] [--episode 3600] [--epochs 2]\n"
			"             [--cost-bps 5] [--hold-penalty 0] [--lr 3e-4] [--hidden 64,64]\n"
			"             [--gamma 0.99] [--epsilon-decay 200000] [--batch 32]\n"
			"             [--target-sync 2000] [--q-lr 0.1] [--seed 1] [--out runs]\n"
			"      Walk-forward train and evaluate, with baselines, a cost curve\n"
			"      (1, 2, 5, 10 bps) and a shuffled-return control.\n"
			"      Agents: flat, long, short, random, crossover, qtable, dqn\n\n"
			"  cli audit  --bars <file>\n"
			"      Shift audit: recompute each feature over a truncated series and\n"
			"      prove the value at t is unchanged. Fails loudly on look-ahead.\n\n"
			"  cli report --run <id> [--root runs]\n"
			"  cli list   [--root runs]\n"
			"  cli verify --run <id> [--root runs]\n";
		return 1;
	}

	std::vector<std::string> gather_inputs(const std::string& input)
	{
		std::vector<std::string> files;
		std::error_code code;

		if (fs::is_directory(fs::path(input), code))
		{
			for (const auto& entry : fs::directory_iterator(fs::path(input), code))
			{
				if (!entry.is_regular_file()) continue;
				if (entry.path().extension() != ".csv") continue;
				files.push_back(entry.path().string());
			}
			std::sort(files.begin(), files.end());
		}
		else
		{
			files.push_back(input);
		}

		return files;
	}

	int command_bars(const options& args)
	{
		const std::string input = args.text("in", "data/raw");
		const std::string output = args.text("out", "data/bars/bars.bin");
		const int64_t interval = parse_duration_ms(args.text("interval", "1s"));

		if (interval <= 0)
		{
			std::cerr << "bad --interval\n";
			return 2;
		}

		const std::vector<std::string> files = gather_inputs(input);
		if (files.empty())
		{
			std::cerr << "no csv files under " << input << '\n';
			return 2;
		}

		bar_builder builder(interval);
		size_t rows = 0;

		for (const std::string& file : files)
		{
			std::string error;
			const size_t parsed = load_trades_csv(file, builder, &error);
			if (parsed == 0 && !error.empty())
			{
				std::cerr << error << '\n';
				return 2;
			}
			std::cout << "  " << fs::path(file).filename().string() << "  " << parsed << " trades\n";
			rows += parsed;
		}

		builder.finish();

		bar_series series;
		series.interval_ms = interval;
		series.symbol = args.text("symbol", "ETHUSDT");
		series.bars = std::move(builder.bars());

		if (series.bars.empty())
		{
			std::cerr << "no bars produced\n";
			return 2;
		}

		std::string error;
		const fs::path out_path = fs::path(output);
		if (out_path.has_parent_path() && !ensure_directory(out_path.parent_path().string(), &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		if (!write_bars(output, series, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		size_t quiet = 0;
		for (const bar& b : series.bars) if (b.filled) quiet += 1;

		std::cout << "\n" << rows << " trades -> " << series.bars.size() << " bars ("
			<< quiet << " empty)\n"
			<< format_time_ms(series.bars.front().time) << " .. "
			<< format_time_ms(series.bars.back().time) << '\n'
			<< "hash " << hex64(hash_bars(series)) << '\n'
			<< "wrote " << output << '\n';

		return 0;
	}

	std::vector<int> parse_hidden(const std::string& text)
	{
		std::vector<int> sizes;
		for (const std::string& part : split(text, ','))
		{
			const int value = static_cast<int>(strtol(trim(part).c_str(), nullptr, 10));
			if (value > 0) sizes.push_back(value);
		}
		if (sizes.empty()) sizes.push_back(64);
		return sizes;
	}

	experiment_config config_from(const options& args)
	{
		experiment_config config;
		config.bars_path = args.text("bars", "data/bars/bars.bin");
		config.agent = args.text("agent", "dqn");
		config.runs_root = args.text("out", "runs");

		config.train_bars = static_cast<int>(args.number("train", config.train_bars));
		config.test_bars = static_cast<int>(args.number("test", config.test_bars));
		config.gap_bars = static_cast<int>(args.number("gap", config.gap_bars));
		config.episode_bars = static_cast<int>(args.number("episode", config.episode_bars));
		config.epochs = static_cast<int>(args.number("epochs", config.epochs));
		config.seeds = static_cast<int>(args.number("seeds", config.seeds));
		config.base_seed = static_cast<uint64_t>(args.number("seed", 1));

		config.cost_bps = args.real("cost-bps", config.cost_bps);
		config.hold_penalty = args.real("hold-penalty", config.hold_penalty);

		config.dqn.hidden = parse_hidden(args.text("hidden", "64,64"));
		config.dqn.learning_rate = args.real("lr", config.dqn.learning_rate);
		config.dqn.discount = args.real("gamma", config.dqn.discount);
		config.dqn.epsilon_decay_steps = args.number("epsilon-decay", config.dqn.epsilon_decay_steps);
		config.dqn.batch_size = static_cast<int>(args.number("batch", config.dqn.batch_size));
		config.dqn.target_sync = static_cast<int>(args.number("target-sync", config.dqn.target_sync));

		config.tabular.learning_rate = args.real("q-lr", config.tabular.learning_rate);
		config.tabular.discount = config.dqn.discount;
		config.tabular.epsilon_decay_steps = config.dqn.epsilon_decay_steps;

		return config;
	}

	int command_walk(const options& args)
	{
		const experiment_config config = config_from(args);

		std::string error;
		const int code = run_experiment(config, &error);
		if (!error.empty()) std::cerr << error << '\n';
		return code;
	}

	int command_audit(const options& args)
	{
		const std::string path = args.text("bars", "data/bars/bars.bin");

		bar_series series;
		std::string error;
		if (!read_bars(path, series, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		const feature_matrix full = build_features(series);
		const auto total = static_cast<int>(series.bars.size());

		// A causal feature computed over a truncated series must produce exactly
		// the same value at t. Any look-ahead makes the two disagree.
		std::vector<int> probes;
		for (int fraction = 1; fraction <= 8; fraction += 1)
		{
			const int t = full.warmup + (total - full.warmup - 1) * fraction / 9;
			if (t > full.warmup && t < total) probes.push_back(t);
		}

		int failures = 0;
		xfloat worst = 0.0;
		std::string worst_name;

		for (const int t : probes)
		{
			bar_series truncated;
			truncated.interval_ms = series.interval_ms;
			truncated.symbol = series.symbol;
			truncated.bars.assign(series.bars.begin(), series.bars.begin() + t + 1);

			const feature_matrix partial = build_features(truncated);
			const xfloat* a = full.row(static_cast<size_t>(t));
			const xfloat* b = partial.row(static_cast<size_t>(t));

			int bad = 0;
			for (int c = 0; c < full.columns; c += 1)
			{
				const xfloat difference = std::abs(a[c] - b[c]);
				if (difference > worst)
				{
					worst = difference;
					worst_name = full.names[c];
				}
				if (difference > 1e-12) bad += 1;
			}

			printf("  t=%-8d %s\n", t, bad == 0 ? "ok" : "LOOK-AHEAD");
			failures += bad;
		}

		std::cout << "\n" << probes.size() << " probes, " << full.columns << " features\n"
			<< "largest disagreement " << worst;
		if (!worst_name.empty()) std::cout << " in " << worst_name;
		std::cout << '\n';

		if (failures > 0)
		{
			std::cerr << "\nSHIFT AUDIT FAILED: a feature reads bars later than its own.\n";
			return 1;
		}

		std::cout << "shift audit passed: every feature at t depends only on bars up to t\n";
		return 0;
	}

	int command_list(const options& args)
	{
		const std::string root = args.text("root", "runs");
		const std::vector<std::string> runs = list_runs(root);

		if (runs.empty())
		{
			std::cout << "no runs under " << root << '\n';
			return 0;
		}

		for (const std::string& id : runs)
		{
			run_summary summary;
			read_manifest(root + "/" + id + "/manifest.txt", summary, nullptr);
			std::cout << id
				<< "  agent=" << summary.get("agent")
				<< "  folds=" << summary.get("folds")
				<< "  cost=" << summary.get("cost_bps") << "bps\n";
		}

		return 0;
	}

	int command_report(const options& args)
	{
		const std::string root = args.text("root", "runs");
		const std::string id = args.text("run", "");
		if (id.empty()) return usage();

		run_summary summary;
		std::string error;
		if (!read_manifest(root + "/" + id + "/manifest.txt", summary, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		std::cout << id << "\n\n";
		for (const manifest_entry& e : summary.manifest)
		{
			printf("  %-16s %s\n", e.key.c_str(), e.value.c_str());
		}

		std::ifstream metrics(root + "/" + id + "/metrics.csv");
		if (!metrics) return 0;

		std::cout << "\npooled results (fold -1)\n";
		std::string line;
		std::getline(metrics, line);

		std::cout << "  variant                cost  net_return   sharpe    maxdd turnover\n";
		while (std::getline(metrics, line))
		{
			const std::vector<std::string> cells = split(line, ',');
			if (cells.size() < 15 || cells[3] != "-1") continue;

			printf("  %-20s %5s %11s %8s %8s %8s\n",
				cells[0].c_str(), cells[2].c_str(), cells[6].c_str(),
				cells[11].c_str(), cells[13].c_str(), cells[14].c_str());
		}

		return 0;
	}

	int command_verify(const options& args)
	{
		const std::string root = args.text("root", "runs");
		const std::string id = args.text("run", "");
		if (id.empty()) return usage();

		run_summary summary;
		std::string error;
		if (!read_manifest(root + "/" + id + "/manifest.txt", summary, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		bar_series series;
		if (!read_bars(summary.get("bars"), series, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		const std::string recorded = summary.get("data_hash");
		const std::string actual = hex64(hash_bars(series));

		std::cout << "data_hash recorded " << recorded << "\n"
			<< "data_hash actual   " << actual << '\n';

		if (recorded != actual)
		{
			std::cerr << "\nDATA CHANGED since the run; the result cannot be reproduced.\n";
			return 1;
		}

		experiment_config config;
		config.bars_path = summary.get("bars");
		config.agent = summary.get("agent");
		config.runs_root = root + "/verify";
		config.base_seed = static_cast<uint64_t>(strtoll(summary.get("base_seed").c_str(), nullptr, 10));
		config.seeds = static_cast<int>(strtoll(summary.get("seeds").c_str(), nullptr, 10));
		config.train_bars = static_cast<int>(strtoll(summary.get("train_bars").c_str(), nullptr, 10));
		config.test_bars = static_cast<int>(strtoll(summary.get("test_bars").c_str(), nullptr, 10));
		config.gap_bars = static_cast<int>(strtoll(summary.get("gap_bars").c_str(), nullptr, 10));
		config.episode_bars = static_cast<int>(strtoll(summary.get("episode_bars").c_str(), nullptr, 10));
		config.epochs = static_cast<int>(strtoll(summary.get("epochs").c_str(), nullptr, 10));
		parse_real(summary.get("cost_bps"), config.cost_bps);

		std::cout << "\nre-running " << config.agent << " ...\n";
		const int code = run_experiment(config, &error);
		if (!error.empty()) std::cerr << error << '\n';
		return code;
	}
}

int main(const int argc, char** argv)
{
	if (argc < 2) return usage();

	const std::string command = argv[1];
	const options args(argc, argv);

	if (command == "bars") return command_bars(args);
	if (command == "walk") return command_walk(args);
	if (command == "audit") return command_audit(args);
	if (command == "list") return command_list(args);
	if (command == "report") return command_report(args);
	if (command == "verify") return command_verify(args);

	return usage();
}
