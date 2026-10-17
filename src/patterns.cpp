#include "patterns.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

#include "bars.h"
#include "features.h"

namespace
{
	constexpr int DISTANCE_HALF_BUCKETS = 4;   // so 2*K+1 buckets, the ends open

	struct bucket
	{
		int64_t n = 0;
		xfloat sum = 0.0;
		xfloat sum_sq = 0.0;

		void add(const xfloat r)
		{
			n += 1;
			sum += r;
			sum_sq += r * r;
		}

		xfloat mean() const { return n > 0 ? sum / static_cast<xfloat>(n) : 0.0; }

		xfloat sd() const
		{
			if (n < 2) return 0.0;
			const xfloat m = mean();
			const xfloat variance = (sum_sq - static_cast<xfloat>(n) * m * m) / static_cast<xfloat>(n - 1);
			return variance > 0.0 ? std::sqrt(variance) : 0.0;
		}

		xfloat standard_error() const
		{
			return n > 1 ? sd() / std::sqrt(static_cast<xfloat>(n)) : 0.0;
		}

		xfloat t_stat() const
		{
			const xfloat se = standard_error();
			return se > 0.0 ? mean() / se : 0.0;
		}
	};

	// One way of slicing the samples. index is parallel to the return series;
	// -1 means the sample does not belong to any bucket.
	struct grouping
	{
		std::string name;
		std::vector<std::string> labels;
		std::vector<int> index;
	};

	// Buckets below this are reported but excluded from the multiple-testing
	// scan, because a handful of samples produces a meaningless t.
	constexpr int64_t MIN_BUCKET = 200;

	std::vector<bucket> accumulate(const grouping& group, const std::vector<xfloat>& values,
		const size_t from, const size_t to)
	{
		std::vector<bucket> out(group.labels.size());

		for (size_t i = from; i < to; i += 1)
		{
			const int b = group.index[i];
			if (b >= 0) out[b].add(values[i]);
		}

		return out;
	}

	xfloat max_abs_t(const std::vector<bucket>& buckets)
	{
		xfloat worst = 0.0;
		for (const bucket& b : buckets)
		{
			if (b.n >= MIN_BUCKET) worst = std::max(worst, std::abs(b.t_stat()));
		}
		return worst;
	}

	// Permuting the returns while holding the bucket assignment fixed gives the
	// distribution of the largest t any bucket reaches by chance. Comparing
	// against that, rather than against 1.96, is what accounts for the fact that
	// every bucket is a separate test.

	// One permutation serves every grouping. Reshuffling per test costs as many
	// passes as there are tests and buys nothing: each grouping's own null is
	// still a valid draw from the same shuffled series.
	//
	// Blocks are whole days. Shuffling single bars would assume they are
	// exchangeable, but bucket membership persists for hours and volatility
	// clusters, so a bar-level null is far too narrow and calls almost
	// everything significant.
	std::vector<xfloat> null_thresholds(const std::vector<const grouping*>& groups,
		const std::vector<xfloat>& values, const int shuffles, const int block, const uint64_t seed)
	{
		rng generator(seed);
		const size_t n = values.size();
		const size_t width = static_cast<size_t>(block > 0 ? block : 1);
		const size_t count = n / width;
		if (count < 2) return std::vector<xfloat>(groups.size(), std::nan(""));

		std::vector<size_t> order(count);
		for (size_t i = 0; i < count; i += 1) order[i] = i;

		std::vector<xfloat> permuted(n, 0.0);
		std::vector<std::vector<xfloat>> maxima(groups.size());

		for (int s = 0; s < shuffles; s += 1)
		{
			for (size_t i = count; i > 1; i -= 1)
			{
				const size_t j = static_cast<size_t>(generator.index(static_cast<int>(i)));
				std::swap(order[i - 1], order[j]);
			}

			for (size_t b = 0; b < count; b += 1)
			{
				std::copy_n(values.begin() + static_cast<ptrdiff_t>(order[b] * width), width,
					permuted.begin() + static_cast<ptrdiff_t>(b * width));
			}

			std::copy(values.begin() + static_cast<ptrdiff_t>(count * width), values.end(),
				permuted.begin() + static_cast<ptrdiff_t>(count * width));

			for (size_t g = 0; g < groups.size(); g += 1)
			{
				maxima[g].push_back(max_abs_t(accumulate(*groups[g], permuted, 0, n)));
			}
		}

		// A null that never moves the statistic cannot test it. Whole-day blocks keep
		// every bar's hour, so the hour-of-day maximum came back from every shuffle
		// unchanged and its verdict, SURVIVES included, was decided by rounding.
		std::vector<xfloat> thresholds;
		for (size_t g = 0; g < groups.size(); g += 1)
		{
			std::vector<xfloat>& m = maxima[g];
			const xfloat observed = max_abs_t(accumulate(*groups[g], values, 0, n));
			const bool moved = std::any_of(m.begin(), m.end(), [&](const xfloat v)
				{ return std::abs(v - observed) > 1e-9 * std::max<xfloat>(1.0, observed); });
			std::sort(m.begin(), m.end());
			thresholds.push_back(moved ? m[static_cast<size_t>(static_cast<double>(m.size()) * 0.95)] : std::nan(""));
		}

		return thresholds;
	}

	// How long the implied position lasts, not how long the bucket lasts. A
	// momentum bucket can persist all day while the position it implies flips
	// every bar, and it is the position that pays the round trip.
	std::vector<xfloat> mean_position_run(const grouping& group, const std::vector<int8_t>& side)
	{
		std::vector<int64_t> runs(group.labels.size(), 0);
		std::vector<int64_t> total(group.labels.size(), 0);
		int previous = -1;
		int8_t previous_side = 0;

		for (size_t i = 0; i < group.index.size(); i += 1)
		{
			const int b = group.index[i];
			const int8_t s = side[i];

			if (b >= 0 && s != 0)
			{
				total[b] += 1;
				if (b != previous || s != previous_side) runs[b] += 1;
			}

			previous = b;
			previous_side = s;
		}

		std::vector<xfloat> out(group.labels.size(), 0.0);
		for (size_t b = 0; b < out.size(); b += 1)
		{
			out[b] = runs[b] > 0 ? static_cast<xfloat>(total[b]) / static_cast<xfloat>(runs[b]) : 0.0;
		}

		return out;
	}

	void report(const grouping& group, const std::vector<xfloat>& values,
		const std::vector<int8_t>& side, const xfloat threshold, const xfloat cost_bps,
		const char* statistic)
	{
		const size_t total = values.size();
		const size_t half = total / 2;

		const auto full = accumulate(group, values, 0, total);
		const auto first = accumulate(group, values, 0, half);
		const auto second = accumulate(group, values, half, total);

		bucket pooled;
		for (const bucket& b : full)
		{
			pooled.n += b.n;
			pooled.sum += b.sum;
			pooled.sum_sq += b.sum_sq;
		}

		std::cout << "\n" << statistic << " " << group.name << "\n";
		std::cout << "  bucket              n    mean       se       t      1st      2nd     hold    need\n";

		const std::vector<xfloat> runs = mean_position_run(group, side);

		for (size_t b = 0; b < group.labels.size(); b += 1)
		{
			const bucket& f = full[b];
			if (f.n == 0) continue;

			const xfloat bps = f.mean() * 10000.0;
			const xfloat need = std::abs(bps) > 1e-9 ? cost_bps / std::abs(bps) : 1e9;
			const bool real = std::abs(f.t_stat()) > threshold && f.n >= MIN_BUCKET;

			printf("  %-14s %7lld %7.3f  %7.3f  %6.2f  %7.3f  %7.3f %8.1f %7.0f%s\n",
				group.labels[b].c_str(),
				static_cast<long long>(f.n),
				bps,
				f.standard_error() * 10000.0,
				f.t_stat(),
				first[b].mean() * 10000.0,
				second[b].mean() * 10000.0,
				runs[b],
				need,
				real && runs[b] > need ? "  PAYS" : "");
		}

		// The permutation null preserves the pooled mean, so it can only find a
		// difference between buckets. Whether the whole sample sits away from
		// zero is a separate question, so it gets its own line.
		printf("  %-14s %7lld %7.3f  %7.3f  %6.2f\n", "ALL",
			static_cast<long long>(pooled.n), pooled.mean() * 10000.0,
			pooled.standard_error() * 10000.0, pooled.t_stat());
		const xfloat observed = max_abs_t(full);

		if (std::isnan(threshold))
		{
			printf("  largest |t| %.2f  ->  UNTESTED: the block shuffle never moved it; whole-day blocks keep\n"
				"  every bar's hour and fewer than two blocks keep everything, so pass a --block of part of a day\n",
				observed);
			return;
		}

		printf("  largest |t| %.2f against a shuffled 95%% of %.2f  ->  %s\n",
			observed, threshold,
			observed > threshold ? "SURVIVES the permutation test" : "no better than chance");
	}

	int distance_bucket(const xfloat distance, const xfloat width, const int half)
	{
		const int raw = static_cast<int>(std::floor(distance / width)) + half;
		return raw < 0 ? 0 : (raw > 2 * half ? 2 * half : raw);
	}

	std::vector<std::string> distance_labels(const xfloat width, const int half)
	{
		std::vector<std::string> labels;
		char buffer[64];

		for (int b = 0; b <= 2 * half; b += 1)
		{
			const xfloat low = static_cast<xfloat>(b - half) * width;

			if (b == 0) snprintf(buffer, sizeof buffer, "< %+.2f", low + width);
			else if (b == 2 * half) snprintf(buffer, sizeof buffer, ">= %+.2f", low);
			else snprintf(buffer, sizeof buffer, "%+.2f..%+.2f", low, low + width);

			labels.emplace_back(buffer);
		}

		return labels;
	}
}

int run_patterns(const patterns_config& config, std::string* error)
{
	bar_series series;
	if (!read_bars(config.bars_path, series, error)) return 2;
	if (series.generic && config.block == 0)
	{
		if (error) *error = "OHLCV patterns require an explicit --block in observed bars; a session is not necessarily 24 hours";
		return 2;
	}

	if (series.bars.size() < 5000)
	{
		if (error) *error = "not enough bars; build a longer range first";
		return 2;
	}

	const size_t count = series.bars.size();

	std::cout << "bars      " << count << " x " << series.interval_ms << "ms  "
		<< format_time_ms(series.bars.front().time) << " .. "
		<< format_time_ms(series.bars.back().time) << "\n";

	// Everything below is streamed in bar order, so no statistic can read a bar
	// later than the one it is attributed to.
	daily_levels levels;
	std::vector<expanding_stats> hourly_activity(24);

	std::vector<xfloat> returns;
	std::vector<xfloat> momentum;   // payoff of holding the previous bar's direction
	std::vector<int8_t> drift_side;
	std::vector<int8_t> momentum_side;
	grouping hour{ "by hour of day (UTC)", {}, {} };
	grouping weekday{ "by day of week", {}, {} };
	grouping to_high{ "by distance to previous day high (previous day ranges)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping to_low{ "by distance to previous day low (previous day ranges)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping to_close{ "by distance to previous day close (previous day ranges)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping in_range{ "by position in the previous day range (0 = low, 1 = high)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping surprise{ "by activity against this hour's own norm (log ratio)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping trade_size{ "by average trade size against its norm (log ratio)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping vwap_side{ "by close against the bar's own vwap (bar ranges)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	grouping impact{ "by price impact per unit volume against its norm (log ratio)", distance_labels(config.bucket_width, DISTANCE_HALF_BUCKETS), {} };
	if (series.generic)
	{
		to_high.name = "by distance to previous session high (previous session ranges)";
		to_low.name = "by distance to previous session low (previous session ranges)";
		to_close.name = "by distance to previous session close (previous session ranges)";
		in_range.name = "by position in the previous session range (0 = low, 1 = high)";
		std::cout << "UNAVAILABLE: UTC seasonality and trade-flow/activity/VWAP/impact studies in the OHLCV profile\n";
	}

	expanding_stats size_norm;
	expanding_stats impact_norm;

	for (int h = 0; h < 24; h += 1)
	{
		char buffer[16];
		snprintf(buffer, sizeof buffer, "%02d:00", h);
		hour.labels.emplace_back(buffer);
	}

	for (const char* day : { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" })
	{
		weekday.labels.emplace_back(day);
	}

	for (size_t i = 0; i + 1 < count; i += 1)
	{
		const bar& b = series.bars[i];
		levels.push(b, series.session(i));

		const int h = static_cast<int>((b.time / 3600000) % 24);
		const xfloat norm = hourly_activity[h].mean();
		const int64_t seen = hourly_activity[h].count();
		hourly_activity[h].push(static_cast<xfloat>(b.trades));

		if (i == 0 || !levels.ready() || b.close <= 0.0) continue;

		// Yesterday's own range is the natural scale for yesterday's levels. An
		// intraday ATR puts almost every sample in the tails and measures nothing.
		const xfloat span = levels.previous_high() - levels.previous_low();
		if (span <= 0.0) continue;

		const xfloat previous = series.bars[i - 1].close;
		const xfloat next = series.bars[i + 1].close;
		if (previous <= 0.0 || next <= 0.0) continue;

		const xfloat forward = series.generic ? std::log(next) - std::log(b.close) : std::log(next / b.close);
		const xfloat behind = series.generic ? std::log(b.close) - std::log(previous) : std::log(b.close / previous);

		returns.push_back(forward);
		momentum.push_back(behind > 0.0 ? forward : (behind < 0.0 ? -forward : 0.0));

		// Drift implies a position held for as long as the bucket holds; momentum
		// implies one that flips with the previous bar.
		drift_side.push_back(1);
		momentum_side.push_back(behind > 0.0 ? 1 : (behind < 0.0 ? -1 : 0));

		const int64_t day = b.time / 86400000;
		hour.index.push_back(h);
		weekday.index.push_back(static_cast<int>((day + 3) % 7));   // epoch day 0 was a Thursday

		to_high.index.push_back(distance_bucket((b.close - levels.previous_high()) / span, config.bucket_width, DISTANCE_HALF_BUCKETS));
		to_low.index.push_back(distance_bucket((b.close - levels.previous_low()) / span, config.bucket_width, DISTANCE_HALF_BUCKETS));
		to_close.index.push_back(distance_bucket((b.close - levels.previous_close()) / span, config.bucket_width, DISTANCE_HALF_BUCKETS));
		in_range.index.push_back(distance_bucket((b.close - levels.previous_low()) / span - 0.5, config.bucket_width, DISTANCE_HALF_BUCKETS));

		// The norm is built only from earlier days, so this is a surprise the
		// agent could actually have known about at the time.
		surprise.index.push_back(seen > 50 && norm > 0.0
			? distance_bucket(std::log((b.trades + 1.0) / (norm + 1.0)), config.bucket_width, DISTANCE_HALF_BUCKETS)
			: -1);

		// Participant proxies. Average trade size separates institutional from
		// retail flow; close against the bar's own vwap says which side was
		// aggressive and when; impact per unit volume is a depth measure, so it
		// stands in for how much market maker capacity is present.
		const xfloat size = b.trades > 0 ? b.volume / b.trades : 0.0;
		const xfloat log_size = size > 0.0 ? std::log(size) : 0.0;
		trade_size.index.push_back(size > 0.0 && size_norm.count() > 500
			? distance_bucket(log_size - size_norm.mean(), config.bucket_width, DISTANCE_HALF_BUCKETS)
			: -1);
		if (size > 0.0) size_norm.push(log_size);

		const xfloat bar_range = b.high - b.low;
		const xfloat vwap = b.volume > 0.0 ? b.quote / b.volume : 0.0;
		vwap_side.index.push_back(bar_range > 0.0 && vwap > 0.0
			? distance_bucket((b.close - vwap) / bar_range, config.bucket_width, DISTANCE_HALF_BUCKETS)
			: -1);

		const xfloat behind_abs = std::abs(behind);
		const xfloat depth = b.volume > 0.0 ? std::log((behind_abs + 1e-9) / std::sqrt(b.volume)) : 0.0;
		impact.index.push_back(b.volume > 0.0 && impact_norm.count() > 500
			? distance_bucket(depth - impact_norm.mean(), config.bucket_width, DISTANCE_HALF_BUCKETS)
			: -1);
		if (b.volume > 0.0) impact_norm.push(depth);
	}

	if (returns.size() < 1000)
	{
		if (error) *error = "too few usable bars after the first session";
		return 2;
	}

	std::cout << "samples   " << returns.size() << "  (mean and se are basis points per bar)\n";
	std::cout << "control   " << config.shuffles << " permutations, bucket assignment held fixed\n";
	std::cout << "\ndrift     mean forward return: does the bucket say which way\n";
	std::cout << "momentum  mean of the previous bar's direction held one bar: positive is\n";
	std::cout << "          trend, negative is reversion, and either is tradeable\n";

	const std::vector<const grouping*> groups = series.generic
		? std::vector<const grouping*>{ &to_high, &to_low, &to_close, &in_range }
		: std::vector<const grouping*>{ &hour, &weekday, &to_high, &to_low, &to_close,
			&in_range, &surprise, &trade_size, &vwap_side, &impact };

	const int block = config.block > 0
		? config.block
		: static_cast<int>(86400000 / (series.interval_ms > 0 ? series.interval_ms : 1));
	std::cout << "block     " << block << " bars per permutation block\n";

	const std::vector<xfloat> drift_null = null_thresholds(groups, returns, config.shuffles, block, config.seed);
	for (size_t g = 0; g < groups.size(); g += 1) report(*groups[g], returns, drift_side, drift_null[g], config.cost_bps, "drift");

	const std::vector<xfloat> momentum_null = null_thresholds(groups, momentum, config.shuffles, block, config.seed + 1);
	for (size_t g = 0; g < groups.size(); g += 1) report(*groups[g], momentum, momentum_side, momentum_null[g], config.cost_bps, "momentum");

	std::cout << "\nhold is how long the implied position lasts, need is how many bars it must be\n"
		<< "held to cover a " << config.cost_bps << " bps round trip. PAYS needs hold > need AND a t\n"
		<< "beyond the permutation threshold. Significance without persistence is not an edge.\n";

	return 0;
}
