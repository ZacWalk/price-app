// cli.exe - runs experiments. Headless, deterministic, writes to runs/.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
#include <cerrno>
#include <string>
#include <vector>

#include "bars.h"
#include "download.h"
#include "common.h"
#include "experiment.h"
#include "features.h"
#include "patterns.h"
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
				if (token.rfind("--", 0) != 0)
				{
					stray_.push_back(token);
					continue;
				}

				token = token.substr(2);
				if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
				{
					values_[token] = argv[i + 1];
					i += 1;
				}
				else
				{
					values_[token] = "";
				}
			}
		}

		// A misspelt or misplaced argument must fail, not silently run the defaults:
		// `walk --agnet breakout` once trained a DQN, and `audit file.bin` audited
		// data/bars/bars.bin.
		void reject_stray() const
		{
			if (!stray_.empty())
				throw std::invalid_argument("unexpected argument '" + stray_.front() + "'; options take the form --name value");
		}

		void require_known(const std::vector<std::string>& allowed) const
		{
			reject_stray();
			for (const auto& [key, value] : values_)
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					throw std::invalid_argument("unknown option --" + key);
		}

		// Switches such as --force are only ever asked has(); every other option needs
		// a value. A bare `--q-lr` once trained at 1, and a bare --out wrote into ./1.
		bool has(const std::string& key) const { return values_.count(key) > 0; }
		bool blank(const std::string& key) const { return has(key) && values_.at(key).empty(); }

		std::string text(const std::string& key, const std::string& fallback) const
		{
			const auto it = values_.find(key);
			if (it == values_.end()) return fallback;
			if (it->second.empty()) throw std::invalid_argument("--" + key + " needs a value");
			return it->second;
		}

		int64_t number(const std::string& key, const int64_t fallback) const
		{
			if (!has(key)) return fallback;
			const std::string value = text(key, "");
			char* end = nullptr;
			errno = 0;
			const auto parsed = strtoll(value.c_str(), &end, 10);
			if (errno || end == value.c_str() || *end)
				throw std::invalid_argument("bad integer --" + key + " '" + value + "'");
			return parsed;
		}

		// An int option outside int's range would wrap: --seeds 4294967297 once ran one seed.
		int whole(const std::string& key, const int fallback) const
		{
			const int64_t value = number(key, fallback);
			if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
				throw std::invalid_argument("--" + key + " " + std::to_string(value) + " is out of range");
			return static_cast<int>(value);
		}

		xfloat real(const std::string& key, const xfloat fallback) const
		{
			if (!has(key)) return fallback;
			const std::string value = text(key, "");
			xfloat out = fallback;
			if (!parse_real(value, out) || !std::isfinite(out))
				throw std::invalid_argument("bad finite number --" + key + " '" + value + "'");
			return out;
		}

		const std::map<std::string, std::string>& all() const { return values_; }

	private:
		std::map<std::string, std::string> values_;
		std::vector<std::string> stray_;
	};

	// Help asked for is a success; a usage error is bad input, exit 2, never the
	// exit 1 that means a control or the audit failed.
	int usage(const int code = 2)
	{
		std::ostream& out = code == 0 ? std::cout : std::cerr;
		out <<
			"price-app cli - predicts short/flat/long positions in offline experiments\n\n"
			"  cli download --symbol ETHUSDT --kind trades|aggTrades|klines|fundingRate|metrics\n"
			"               --from YYYY-MM-DD\n"
			"               --to YYYY-MM-DD [--market um|cm|spot] [--source 1m]\n"
			"               [--period auto|daily|monthly] [--out DIR] [--fixtures DIR]\n"
			"               [--force] [--skip-missing] [--max-archive-bytes N] [--max-csv-bytes N]\n"
			"               [--max-rows N] (positive; default 10000000 typed rows per archive)\n"
			"      The ONLY network command. Mandatory SHA256; failed downloads never replace data.\n\n"
			"  cli bars   --in <dir|file> --out <file> [--interval 1s] [--symbol ETHUSDT]\n"
			"      Aggregate Binance trade or kline CSVs into a causal bar file. Klines\n"
			"      default to their own interval and accept only whole multiples of it.\n\n"
			"  cli bars --format ohlcv --in <csv> --out <file> --symbol <name>\n"
			"           --interval <duration> --calendar <utc|supplied-name> --currency <unit>\n"
			"           [--source-name local-csv] [--price-basis raw|adjusted]\n"
			"      Import time,open,high,low,close[,volume][,session]. Time is UTC epoch ms\n"
			"      at bar OPEN. Non-UTC calendars require integer session IDs supplied\n"
			"      by the data producer. Gaps remain gaps; no bars or flow are invented.\n\n"
			"  cli walk   --bars <file> [--agent dqn] [--seeds 3] [--train 40000]\n"
			"             [--test 10000] [--valid 0] [--gap 2000] [--episode 3600]\n"
			"             [--epochs 2] [--cost-bps 5] [--slippage-bps 0]\n"
			"             [--cost-activity 0] [--hold-penalty 0] [--lr 3e-4]\n"
			"             [--lr-decay 1] [--switch-margin 0] [--hidden 64,64]\n"
			"             [--gamma 0.99] [--epsilon-decay 200000] [--batch 32]\n"
			"             [--target-sync 2000] [--q-lr 0.1] [--seed 1] [--out runs]\n"
			"      Walk-forward train and evaluate, with baselines, a cost curve\n"
			"      (1, 2, 5, 10 bps) and shuffled and rotated controls. The headline\n"
			"      is the vote of all seeds; no artifact is picked on the test window.\n"
			"      Parameters are chosen on --valid, never on the test table. Prints\n"
			"      each variant's break-even round-trip cost and the headline by\n"
			"      calendar year with a rotated timing control inside each year.\n"
			"      Agents: flat, long, short, random, crossover, breakout, qtable, dqn\n\n"
			"  cli audit  --bars <file>\n"
			"      Shift audit: recompute each feature over a truncated series and\n"
			"      prove the value at t is unchanged. Fails loudly on look-ahead.\n\n"
			"  cli patterns --bars <file> [--width 0.5] [--shuffles 200] [--block 0]\n"
			"               [--cost-bps 5] [--seed 1]\n"
			"      Measure whether hour of day, day of week, distance to the previous\n"
			"      day's high, low and close, or the participant proxies carry forward\n"
			"      return. Reports each bucket over both halves of the sample against a\n"
			"      permutation null that accounts for testing every bucket, and flags\n"
			"      PAYS only when the implied position also outlasts its own costs.\n\n"
			"  cli report --run <id> [--root runs]\n"
			"  cli list   [--root runs]\n"
			"  cli verify --run <id> [--root runs]\n"
			"      Re-run a recorded run under <root>/verify and require byte-identical\n"
			"      metrics, equity, learning and policy artifacts.\n"
			"  cli help\n\n"
			"Exit codes: 0 success, 1 a control or the audit failed, 2 bad input.\n";
		return code;
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

	// The inverse of parse_duration_ms, in the largest unit that divides exactly.
	std::string duration_text(const int64_t ms)
	{
		const std::pair<int64_t, const char*> units[] = { {86400000, "d"}, {3600000, "h"}, {60000, "m"}, {1000, "s"} };
		for (const auto& [size, unit] : units)
			if (ms != 0 && ms % size == 0) return std::to_string(ms / size) + unit;
		return std::to_string(ms) + "ms";
	}

	bool same_bytes(const fs::path& a, const fs::path& b)
	{
		std::error_code size_a, size_b;
		const auto bytes_a = fs::file_size(a, size_a);
		const auto bytes_b = fs::file_size(b, size_b);
		if (size_a || size_b || bytes_a != bytes_b) return false;

		std::ifstream x(a, std::ios::binary);
		std::ifstream y(b, std::ios::binary);
		std::vector<char> p(1 << 16), q(1 << 16);
		while (x && y)
		{
			x.read(p.data(), static_cast<std::streamsize>(p.size()));
			y.read(q.data(), static_cast<std::streamsize>(q.size()));
			if (x.gcount() != y.gcount() || !std::equal(p.begin(), p.begin() + x.gcount(), q.begin())) return false;
		}
		return x.eof() && y.eof();
	}

	int command_bars(const options& args)
	{
		const std::string format = args.text("format", "binance");
		if (format != "binance" && format != "ohlcv")
			throw std::invalid_argument("--format must be binance or ohlcv");
		if (format == "ohlcv")
		{
			for (const char* flag : { "in", "out", "symbol", "interval", "calendar", "currency" })
				if (!args.has(flag)) throw std::invalid_argument(std::string("--format ohlcv requires --") + flag);
		}
		else
		{
			for (const char* flag : { "calendar", "currency", "source-name", "price-basis" })
				if (args.has(flag)) throw std::invalid_argument(std::string("--") + flag + " requires --format ohlcv");
		}
		const std::string input = args.text("in", "data/raw");
		const std::string output = args.text("out", "data/bars/bars.bin");
		const std::string symbol = args.text("symbol", "ETHUSDT");
		const std::string interval_text = args.text("interval", "1s");
		int64_t interval = parse_duration_ms(interval_text);

		// Capped at a year, so neither slot arithmetic nor the advice below can overflow.
		if (interval <= 0 || interval > 365LL * 86400000)
		{
			std::cerr << "bad --interval '" << interval_text
				<< "'; expected a whole number and a unit (ms, s, m, h or d) up to 365d, such as 10s, 15m or 1h\n";
			return 2;
		}

		std::error_code exists_error;
		if (!fs::exists(fs::path(input), exists_error))
		{
			std::cerr << "cannot open " << input << '\n';
			return 2;
		}

		if (format == "ohlcv")
		{
			if (!fs::is_regular_file(input))
				throw std::invalid_argument("--format ohlcv requires a single CSV file");
			if (fs::exists(output))
				throw std::invalid_argument("refusing to overwrite bar file " + output);
			bar_series series;
			series.generic = true;
			series.interval_ms = interval;
			series.symbol = symbol;
			series.market.source = args.text("source-name", "local-csv");
			series.market.currency = args.text("currency", "");
			series.market.calendar = args.text("calendar", "");
			series.market.price_basis = args.text("price-basis", "raw");
			std::string error;
			if (!load_ohlcv_csv(input, series, &error))
			{
				std::cerr << error << '\n';
				return 2;
			}
			const fs::path out_path(output);
			if ((out_path.has_parent_path() && !ensure_directory(out_path.parent_path().string(), &error)) ||
				!write_bars(output, series, &error))
			{
				std::cerr << error << '\n';
				return 2;
			}
			size_t gaps = 0;
			for (size_t i = 1; i < series.size(); ++i)
				if (series.bars[i].time - series.bars[i - 1].time != interval) ++gaps;
			std::cout << series.size() << " bars x " << interval << "ms, " << gaps << " gaps preserved\n"
				<< "format " << bar_format_name(series) << ", calendar " << series.market.calendar
				<< ", volume " << (series.market.volume_available ? "available" : "unavailable") << '\n'
				<< "hash " << hex64(hash_bars(series)) << "\nwrote " << output << '\n';
			return 0;
		}

		const std::vector<std::string> files = gather_inputs(input);
		if (files.empty())
		{
			std::cerr << "no csv files under " << input << '\n';
			return 2;
		}

		// Classified before any is read, because klines decide the interval.
		std::vector<csv_kind> kinds;
		std::vector<int64_t> sources;
		for (const std::string& file : files)
		{
			kinds.push_back(detect_csv_kind(file));
			if (kinds.back() == csv_kind::unknown)
			{
				std::cerr << "unrecognised csv layout in " << file << '\n';
				return 2;
			}
			if (kinds.back() == csv_kind::klines) sources.push_back(kline_interval_ms(file));
		}

		// A kline cannot be split or shared: at 1s each 1m kline became one bar
		// and 59 synthetic copies of its close.
		const int64_t native = sources.empty() ? 0 : *std::max_element(sources.begin(), sources.end());
		if (!args.has("interval") && native > 0) interval = native;
		for (const int64_t source : sources)
		{
			if (source > 0 && interval % source != 0)
			{
				// Say what to pass: through dd the trades-only 10s default arrives here unasked.
				const int64_t below = interval / source * source;
				std::cerr << "--interval " << duration_text(interval) << " is not a whole multiple of the "
					<< duration_text(source) << " klines in " << input << ", and a kline cannot be split between bars.\n"
					<< "Use --interval " << (below > 0 ? duration_text(below) + " or " : "")
					<< duration_text(below + source) << ", or any other whole multiple of " << duration_text(source) << ".\n";
				return 2;
			}
		}

		bar_builder builder(interval);
		size_t trade_rows = 0;
		size_t kline_rows = 0;

		for (size_t i = 0; i < files.size(); i += 1)
		{
			const std::string& file = files[i];
			std::string error;
			const csv_kind kind = kinds[i];

			const size_t parsed = kind == csv_kind::klines
				? load_klines_csv(file, builder, &error)
				: load_trades_csv(file, builder, &error);

			if (parsed == 0 && !error.empty())
			{
				std::cerr << error << '\n';
				return 2;
			}
			std::cout << "  " << fs::path(file).filename().string() << "  " << parsed
				<< (kind == csv_kind::klines ? " klines\n" : " trades\n");
			if (kind == csv_kind::klines) kline_rows += parsed;
			else trade_rows += parsed;
		}

		builder.finish();

		bar_series series;
		series.interval_ms = interval;
		series.symbol = symbol;
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

		std::cout << '\n';
		if (trade_rows > 0 || kline_rows == 0) std::cout << trade_rows << " trades";
		if (trade_rows > 0 && kline_rows > 0) std::cout << " + ";
		if (kline_rows > 0) std::cout << kline_rows << " klines";
		std::cout << " -> " << series.bars.size() << " bars x " << interval << "ms ("
			<< quiet << " empty)\n"
			<< format_time_ms(series.bars.front().time) << " .. "
			<< format_time_ms(series.bars.back().time) << '\n'
			<< "hash " << hex64(hash_bars(series)) << '\n'
			<< "wrote " << output << '\n';

		return 0;
	}

	// Empty text keeps the default; anything else must be a list of positive
	// widths, so a typo cannot quietly train the default 64,64 network.
	std::vector<int> parse_hidden(const std::string& text, const std::vector<int>& fallback)
	{
		if (trim(text).empty()) return fallback;

		std::vector<int> sizes;
		for (const std::string& part : split(text, ','))
		{
			const std::string digits = trim(part);
			char* end = nullptr;
			errno = 0;
			const long value = strtol(digits.c_str(), &end, 10);
			if (digits.empty() || errno || *end || value <= 0 || value > 1 << 20)
				throw std::invalid_argument("bad --hidden '" + text + "'; expected positive widths such as 64,64");
			sizes.push_back(static_cast<int>(value));
		}
		return sizes;
	}

	// Seeds are unsigned; -1 would wrap to 18446744073709551615 and still look deliberate.
	uint64_t seed_from(const options& args)
	{
		const int64_t seed = args.number("seed", 1);
		if (seed < 0) throw std::invalid_argument("--seed must be at least 0");
		return static_cast<uint64_t>(seed);
	}

	experiment_config config_from(const options& args)
	{
		experiment_config config;
		config.bars_path = args.text("bars", "data/bars/bars.bin");
		config.agent = args.text("agent", "dqn");
		config.runs_root = args.text("out", "runs");

		config.train_bars = args.whole("train", config.train_bars);
		config.valid_bars = args.whole("valid", config.valid_bars);
		config.test_bars = args.whole("test", config.test_bars);
		config.gap_bars = args.whole("gap", config.gap_bars);
		config.episode_bars = args.whole("episode", config.episode_bars);
		config.epochs = args.whole("epochs", config.epochs);
		config.seeds = args.whole("seeds", config.seeds);
		config.base_seed = seed_from(args);

		config.cost_bps = args.real("cost-bps", config.cost_bps);
		config.slippage_bps = args.real("slippage-bps", config.slippage_bps);
		config.cost_activity = args.real("cost-activity", config.cost_activity);
		config.hold_penalty = args.real("hold-penalty", config.hold_penalty);

		config.dqn.hidden = parse_hidden(args.text("hidden", ""), config.dqn.hidden);
		config.dqn.learning_rate = args.real("lr", config.dqn.learning_rate);
		config.dqn.lr_decay = args.real("lr-decay", config.dqn.lr_decay);
		config.dqn.discount = args.real("gamma", config.dqn.discount);
		config.dqn.epsilon_decay_steps = args.number("epsilon-decay", config.dqn.epsilon_decay_steps);
		config.dqn.batch_size = args.whole("batch", config.dqn.batch_size);
		config.dqn.target_sync = args.whole("target-sync", config.dqn.target_sync);
		config.dqn.switch_margin = args.real("switch-margin", config.dqn.switch_margin);

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

	int command_download(const options& args)
	{
		const std::vector<std::string> allowed = {"symbol", "market", "kind", "period", "source",
			"from", "to", "out", "fixtures", "force", "skip-missing", "max-archive-bytes", "max-csv-bytes", "max-rows"};
		for (const auto& [key, value] : args.all())
			if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
				throw std::invalid_argument("unsupported download option --" + key + "; checksum bypass is forbidden");
		// Switches take no value: `--force false` once forced a replacement anyway.
		for (const char* flag : { "force", "skip-missing" })
			if (args.has(flag) && !args.blank(flag))
				throw std::invalid_argument(std::string("--") + flag + " takes no value");
		download_config config;
		config.symbol = args.text("symbol", config.symbol);
		config.market = args.text("market", config.market);
		config.kind = args.text("kind", config.kind);
		config.period = args.text("period", config.period);
		config.interval = args.text("source", config.interval);
		config.from = args.text("from", config.from);
		config.to = args.text("to", config.to);
		config.output = args.text("out", "");
		if (args.blank("fixtures"))
			throw std::invalid_argument("--fixtures requires a non-empty directory");
		config.fixtures = args.text("fixtures", "");
		config.force = args.has("force");
		config.skip_missing = args.has("skip-missing");
		const auto archive_limit = args.number("max-archive-bytes", config.max_archive_bytes);
		const auto csv_limit = args.number("max-csv-bytes", config.max_csv_bytes);
		if (archive_limit <= 0 || csv_limit <= 0) throw std::invalid_argument("download byte limits must be positive");
		config.max_archive_bytes = static_cast<size_t>(archive_limit);
		config.max_csv_bytes = static_cast<size_t>(csv_limit);
		const auto row_limit = args.number("max-rows", config.max_rows);
		if (row_limit <= 0 || static_cast<uint64_t>(row_limit) > std::numeric_limits<size_t>::max())
			throw std::invalid_argument("--max-rows must be a positive integer within the supported range");
		config.max_rows = static_cast<size_t>(row_limit);
		return download_binance(config);
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
		if (probes.empty())
		{
			std::cerr << "not enough bars for a causal shift audit\n";
			return 2;
		}
		xfloat worst = 0.0;
		std::string worst_name;

		for (const int t : probes)
		{
			bar_series truncated;
			truncated.interval_ms = series.interval_ms;
			truncated.symbol = series.symbol;
			truncated.generic = series.generic;
			truncated.market = series.market;
			truncated.bars.assign(series.bars.begin(), series.bars.begin() + t + 1);
			if (series.generic) truncated.sessions.assign(series.sessions.begin(), series.sessions.begin() + t + 1);

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
				if (!std::isfinite(a[c]) || !std::isfinite(b[c]) || difference > 1e-12) bad += 1;
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
		if (id.empty())
		{
			std::cerr << "report needs --run <id>\n\n";
			return usage();
		}

		run_summary summary;
		std::string error;
		if (!read_manifest(root + "/" + id + "/manifest.txt", summary, &error))
		{
			std::cerr << error << '\n';
			return 2;
		}

		std::cout << id << "\n\n";

		// Every column is as wide as its widest cell, so a long key or variant
		// name cannot push the rest of its row out of line.
		int key_width = 0;
		for (const manifest_entry& e : summary.manifest) key_width = std::max(key_width, static_cast<int>(e.key.size()));
		for (const manifest_entry& e : summary.manifest)
		{
			printf("  %-*s %s\n", key_width, e.key.c_str(), e.value.c_str());
		}

		std::ifstream metrics(root + "/" + id + "/metrics.csv");
		if (!metrics)
		{
			std::cerr << "\nno metrics.csv in " << root << "/" << id << "; the run has no results to report\n";
			return 2;
		}

		std::cout << "\npooled results (fold -1)\n";
		std::string line;
		std::getline(metrics, line);

		// variant, seed, cost_bps, net_return, sharpe, max_drawdown, turnover
		const size_t picked[] = { 0, 1, 2, 6, 11, 13, 14 };
		std::vector<std::vector<std::string>> table = { { "variant", "seed", "cost", "net_return", "sharpe", "maxdd", "turnover" } };
		while (std::getline(metrics, line))
		{
			const std::vector<std::string> cells = split(line, ',');
			if (cells.size() < 15 || cells[3] != "-1") continue;

			std::vector<std::string> row;
			for (const size_t c : picked) row.push_back(cells[c]);
			table.push_back(std::move(row));
		}

		std::vector<int> width(table.front().size(), 0);
		for (const auto& row : table)
			for (size_t c = 0; c < row.size(); c += 1) width[c] = std::max(width[c], static_cast<int>(row[c].size()));

		for (const auto& row : table)
		{
			printf("  %-*s", width[0], row[0].c_str());
			for (size_t c = 1; c < row.size(); c += 1) printf(" %*s", width[c], row[c].c_str());
			printf("\n");
		}

		return 0;
	}

	int command_verify(const options& args)
	{
		const std::string root = args.text("root", "runs");
		const std::string id = args.text("run", "");
		if (id.empty())
		{
			std::cerr << "verify needs --run <id>\n\n";
			return usage();
		}

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

		if (series.generic || !summary.get("feature_profile").empty())
		{
			const feature_matrix features = build_features(series);
			if (summary.get("feature_profile") != features.profile ||
				summary.get("feature_schema") != features.schema() ||
				summary.get("feature_columns") != std::to_string(features.columns) ||
				summary.get("warmup_bars") != std::to_string(features.warmup) ||
				summary.get("normalisation") != "causal-expanding-clamped-v1" ||
				summary.get("bar_format") != bar_format_name(series) ||
				summary.get("prediction") != "short-flat-long" ||
				summary.get("decision_time") != "bar-close" || summary.get("horizon") != "next-observed-bar" ||
				summary.get("annualisation") != (series.generic ? "scored-elapsed-time" : "continuous-interval"))
			{
				std::cerr << "feature or prediction contract mismatch; use the recorded compatible revision\n";
				return 2;
			}
			if (series.generic &&
				(summary.get("source") != series.market.source || summary.get("currency") != series.market.currency ||
				 summary.get("calendar") != series.market.calendar || summary.get("price_basis") != series.market.price_basis ||
				 summary.get("volume_available") != (series.market.volume_available ? "1" : "0") ||
				 summary.get("flow_available") != "0" || summary.get("carry_costs") != "not-modelled" ||
				 summary.get("reward_costs") != "entry-switch-and-terminal" ||
				 summary.get("exposure_basis") != "observed-bars" ||
				 summary.get("execution") != "decision-close-plus-slippage" ||
				 summary.get("shuffled_control") != "equal-elapsed-duration" ||
				 summary.get("rotated_control") != "bar-exposure-and-recorded-cost-matched"))
			{
				std::cerr << "market metadata mismatch; cannot reproduce the recorded experiment\n";
				return 2;
			}
		}

		experiment_config config;

		// Every field the run took from the command line has to come back, or the
		// re-run is a different experiment wearing the same name.
		const auto integer = [&](const char* key, const int64_t fallback)
			{
				const std::string value = summary.get(key);
				return value.empty() ? fallback : strtoll(value.c_str(), nullptr, 10);
			};

		const auto decimal = [&](const char* key, const xfloat fallback)
			{
				xfloat out = fallback;
				const std::string value = summary.get(key);
				if (!value.empty()) parse_real(value, out);
				return out;
			};

		config.bars_path = summary.get("bars");
		config.agent = summary.get("agent");
		if (config.agent == "dqn" && summary.get("dqn_numerics") != DQN_NUMERICS)
		{
			std::cerr << "DQN numerical version mismatch: legacy runs require crypto-app 0a1bae3; no implicit migration\n";
			return 2;
		}
		if (config.agent == "dqn" && summary.get("libcmlp_revision") != CRYPTO_CMLP_REVISION)
		{
			std::cerr << "libcmlp revision mismatch: use the recorded compatible application/library revision; no implicit migration\n";
			return 2;
		}
		config.runs_root = root + "/verify";
		config.base_seed = static_cast<uint64_t>(integer("base_seed", 1));
		config.seeds = static_cast<int>(integer("seeds", config.seeds));
		config.train_bars = static_cast<int>(integer("train_bars", config.train_bars));
		config.valid_bars = static_cast<int>(integer("valid_bars", config.valid_bars));
		config.test_bars = static_cast<int>(integer("test_bars", config.test_bars));
		config.gap_bars = static_cast<int>(integer("gap_bars", config.gap_bars));
		config.episode_bars = static_cast<int>(integer("episode_bars", config.episode_bars));
		config.epochs = static_cast<int>(integer("epochs", config.epochs));

		config.cost_bps = decimal("cost_bps", config.cost_bps);
		config.slippage_bps = decimal("slippage_bps", config.slippage_bps);
		config.cost_activity = decimal("cost_activity", config.cost_activity);
		config.hold_penalty = decimal("hold_penalty", config.hold_penalty);

		config.dqn.hidden = parse_hidden(summary.get("hidden"), config.dqn.hidden);
		config.dqn.learning_rate = decimal("lr", config.dqn.learning_rate);
		config.dqn.lr_decay = decimal("lr_decay", config.dqn.lr_decay);
		config.dqn.discount = decimal("gamma", config.dqn.discount);
		config.dqn.epsilon_decay_steps = integer("epsilon_decay", config.dqn.epsilon_decay_steps);
		config.dqn.batch_size = static_cast<int>(integer("batch", config.dqn.batch_size));
		config.dqn.target_sync = static_cast<int>(integer("target_sync", config.dqn.target_sync));
		config.dqn.switch_margin = decimal("switch_margin", config.dqn.switch_margin);

		config.tabular.learning_rate = decimal("q_lr", config.tabular.learning_rate);
		config.tabular.discount = config.dqn.discount;
		config.tabular.epsilon_decay_steps = config.dqn.epsilon_decay_steps;

		const fs::path recorded_run = fs::path(root) / id;
		if (!fs::exists(recorded_run / "metrics.csv"))
		{
			std::cerr << "no metrics.csv in " << recorded_run.string() << "; there is no recorded result to reproduce\n";
			return 2;
		}

		std::cout << "\nre-running " << config.agent << " ...\n";
		std::string rerun;
		const int code = run_experiment(config, &error, &rerun);
		if (!error.empty()) std::cerr << error << '\n';
		if (code == 2) return code;

		// Completing is not reproducing: the same configuration on the same bars
		// must write the same bytes, or the recorded numbers cannot be trusted.
		std::cout << "\ncompared with " << recorded_run.string() << '\n';
		int compared = 0;
		int differing = 0;
		for (const char* name : { "metrics.csv", "equity.csv", "learning.csv", "policy.csv", "weights.bin" })
		{
			if (!fs::exists(recorded_run / name)) continue;
			const bool same = same_bytes(recorded_run / name, fs::path(rerun) / name);
			compared += 1;
			if (!same) differing += 1;
			std::cout << "  " << name << (same ? "  identical\n" : "  DIFFERS\n");
		}
		if (differing > 0)
		{
			std::cerr << "\nNOT REPRODUCED: " << differing << " of " << compared
				<< " recorded artifacts differ; use the recorded compatible revision\n";
			return 1;
		}
		std::cout << "reproduced: " << compared << " recorded artifacts byte-identical\n";
		return code;
	}

	int command_patterns(const options& args)
	{
		patterns_config config;
		config.bars_path = args.text("bars", "data/bars/bars.bin");
		config.bucket_width = args.real("width", config.bucket_width);
		config.shuffles = args.whole("shuffles", config.shuffles);
		config.block = args.whole("block", config.block);
		config.cost_bps = args.real("cost-bps", config.cost_bps);
		config.seed = seed_from(args);

		if (config.bucket_width <= 0.0)
		{
			std::cerr << "--width must be positive\n";
			return 2;
		}
		// The threshold is the 95th percentile of the shuffled maxima: fewer than 20
		// draws cannot place it, and against a single draw noise survives half the time.
		if (config.shuffles < 20)
		{
			std::cerr << "--shuffles must be at least 20 to place a 95% threshold\n";
			return 2;
		}
		if (config.block < 0 || config.cost_bps < 0.0)
		{
			std::cerr << "--block and --cost-bps must be at least 0 (--block 0 is one day of bars)\n";
			return 2;
		}

		std::string error;
		const int code = run_patterns(config, &error);
		if (!error.empty()) std::cerr << error << '\n';
		return code;
	}
}

int main(const int argc, char** argv)
{
	try
	{
	if (argc < 2) return usage();

	const std::string command = argv[1];
	if (command == "help" || command == "--help" || command == "-h") return usage(0);

	const options args(argc, argv);

	if (command == "bars")
	{
		args.require_known({"in", "out", "interval", "symbol", "format", "calendar", "currency", "source-name", "price-basis"});
		return command_bars(args);
	}
	if (command == "download")
	{
		// Its own allow-list names the forbidden checksum bypass explicitly.
		args.reject_stray();
		return command_download(args);
	}
	if (command == "walk")
	{
		args.require_known({"bars", "agent", "seeds", "train", "test", "valid", "gap", "episode", "epochs",
			"cost-bps", "slippage-bps", "cost-activity", "hold-penalty", "lr", "lr-decay", "switch-margin",
			"hidden", "gamma", "epsilon-decay", "batch", "target-sync", "q-lr", "seed", "out"});
		return command_walk(args);
	}
	if (command == "audit")
	{
		args.require_known({"bars"});
		return command_audit(args);
	}
	if (command == "patterns")
	{
		args.require_known({"bars", "width", "shuffles", "block", "cost-bps", "seed"});
		return command_patterns(args);
	}
	if (command == "list")
	{
		args.require_known({"root"});
		return command_list(args);
	}
	if (command == "report")
	{
		args.require_known({"run", "root"});
		return command_report(args);
	}
	if (command == "verify")
	{
		args.require_known({"run", "root"});
		return command_verify(args);
	}

	std::cerr << "unknown command '" << command << "'\n\n";
	return usage();
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << '\n';
		return 2;
	}
}
