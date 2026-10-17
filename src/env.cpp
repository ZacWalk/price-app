#include "env.h"

#include <map>
#include <stdexcept>

const char* action_name(const int action)
{
	switch (action)
	{
	case 0: return "short";
	case 2: return "long";
	default: return "flat";
	}
}

env::env(const bar_series& series, const feature_matrix& features, const env_config& config)
	: series_(series)
	, features_(features)
	, config_(config)
{
	if (series_.generic && config_.cost_activity != 0.0)
		throw std::invalid_argument("--cost-activity requires trade counts, unavailable in the OHLCV profile");
	// A position change is assumed to fill at the bar close plus slippage. No
	// execution price is ever derived from the bar's high or low: picking a price
	// inside the range reads the bar to choose the fill, which is look-ahead.
	cost_per_unit_ = config_.cost_bps / 2.0 / 10000.0 + config_.slippage_bps / 10000.0;

	const size_t n = series_.bars.size();
	returns_.assign(n > 0 ? n - 1 : 0, 0.0);

	for (size_t i = 0; i + 1 < n; i += 1)
	{
		const xfloat a = series_.bars[i].close;
		const xfloat b = series_.bars[i + 1].close;
		returns_[i] = a > 0.0 && b > 0.0
			? (series_.generic ? std::log(b) - std::log(a) : std::log(b / a)) : 0.0;
	}

	// A trailing average true range, used only to express unrealised pnl in
	// units the agent can compare across price levels.
	atr_.assign(n, 0.0);
	xfloat running = 0.0;
	for (size_t i = 0; i < n; i += 1)
	{
		const bar& b = series_.bars[i];
		const xfloat range = b.high - b.low;
		running = i == 0 ? range : running + (range - running) / 48.0;
		atr_[i] = running > 1e-9 ? running : 1e-9;
	}

	state_.assign(static_cast<size_t>(features_.columns) + 3, 0.0);

	// A flat cost across every hour is wrong: `cli patterns` measures a 2x swing
	// in activity by hour of day, and the thin hours are the expensive ones. The
	// trailing norm only ever sees the past, and this bar's own trade count is
	// known at its close, which is when the fill happens.
	cost_scale_.assign(n, 1.0);
	if (config_.cost_activity > 0.0)
	{
		ema_line norm(static_cast<int>(86400000 / (series_.interval_ms > 0 ? series_.interval_ms : 1)));
		for (size_t i = 0; i < n; i += 1)
		{
			const xfloat rate = norm.push(static_cast<xfloat>(series_.bars[i].trades));
			const xfloat here = static_cast<xfloat>(series_.bars[i].trades);
			const xfloat ratio = here > 0.0 && rate > 0.0 ? rate / here : 1.0;
			cost_scale_[i] = clampx(std::pow(ratio, config_.cost_activity), 0.5, 4.0);
		}
	}
}

int env::first_bar() const
{
	return features_.warmup;
}

void env::reset(const int start, const int end)
{
	t_ = start < features_.warmup ? features_.warmup : start;
	end_ = end > last_bar() ? last_bar() : end;
	if (end_ < t_) end_ = t_;

	position_ = 0;
	bars_held_ = 0;
	entry_price_ = series_.bars[t_].close;
	last_cost_ = 0.0;
	last_units_ = 0.0;
	last_gross_ = 0.0;

	observe();
}

void env::observe()
{
	const xfloat* row = features_.row(static_cast<size_t>(t_));
	for (int i = 0; i < features_.columns; i += 1) state_[i] = row[i];

	const xfloat close = series_.bars[t_].close;
	const xfloat unrealised = position_ == 0 || entry_price_ <= 0.0
		? 0.0
		: position_ * (close - entry_price_) / atr_[t_];

	state_[features_.columns + 0] = static_cast<xfloat>(position_);
	state_[features_.columns + 1] = clampx(unrealised, -10.0, 10.0);
	state_[features_.columns + 2] = std::log1p(static_cast<xfloat>(bars_held_)) / 5.0;
}

xfloat env::step(const int action, bool& done)
{
	const int target = action_to_position(action);
	const int previous = position_;

	last_cost_ = cost_per_unit_ * cost_scale_[t_] * std::abs(static_cast<xfloat>(target - previous));
	last_units_ = cost_scale_[t_] * std::abs(static_cast<xfloat>(target - previous));

	if (target != previous)
	{
		entry_price_ = series_.bars[t_].close;
		bars_held_ = 0;
	}
	else
	{
		bars_held_ += 1;
	}

	position_ = target;
	last_gross_ = target * step_return(t_);

	xfloat reward = last_gross_ - last_cost_
		- config_.hold_penalty * std::abs(static_cast<xfloat>(target));

	t_ += 1;
	done = t_ >= end_;

	if (done)
	{
		// Episodes finish flat, so no unrealised position can be banked.
		const xfloat exit_cost = cost_per_unit_ * cost_scale_[t_ - 1] * std::abs(static_cast<xfloat>(position_));
		last_cost_ += exit_cost;
		last_units_ += cost_scale_[t_ - 1] * std::abs(static_cast<xfloat>(position_));
		if (series_.generic) reward -= exit_cost;
		position_ = 0;
	}

	observe();
	return reward;
}

std::vector<xfloat> shuffled_returns(const std::vector<xfloat>& source, const uint64_t seed)
{
	std::vector<xfloat> out = source;
	rng r(seed);

	for (size_t i = out.size(); i > 1; i -= 1)
	{
		const size_t j = static_cast<size_t>(r.index(static_cast<int>(i)));
		std::swap(out[i - 1], out[j]);
	}

	return out;
}

std::vector<xfloat> shuffled_returns(const std::vector<xfloat>& source, const bar_series& series, const uint64_t seed)
{
	if (!series.generic) return shuffled_returns(source, seed);
	if (source.size() + 1 != series.size()) throw std::invalid_argument("return series does not match OHLCV bars");
	std::map<int64_t, std::vector<size_t>> groups;
	for (size_t i = 0; i < source.size(); ++i)
		groups[series.bars[i + 1].time - series.bars[i].time].push_back(i);
	std::vector<xfloat> out = source;
	rng generator(seed);
	for (const auto& [duration, indices] : groups)
	{
		for (size_t i = indices.size(); i > 1; --i)
		{
			const auto j = static_cast<size_t>(generator.index(static_cast<int>(i)));
			std::swap(out[indices[i - 1]], out[indices[j]]);
		}
	}
	return out;
}
