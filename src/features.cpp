#include "features.h"

#include <algorithm>
#include <stdexcept>

namespace
{
	constexpr int RETURN_LAGS[] = { 1, 5, 15, 60, 300 };
	constexpr int RETURN_LAG_COUNT = static_cast<int>(std::size(RETURN_LAGS));

	constexpr int EMA_FAST = 12;
	constexpr int EMA_MID = 48;
	constexpr int EMA_SLOW = 192;
	constexpr int VOL_SHORT = 60;
	constexpr int VOL_LONG = 1440;
	constexpr int RANGE_WINDOW = 300;
	constexpr int FLOW_WINDOW = 60;

	constexpr int WARMUP_BARS = VOL_LONG + RANGE_WINDOW;
	constexpr xfloat CLAMP = 5.0;
	constexpr xfloat LEVEL_CLAMP = 2.0;    // previous-day distances, in day ranges
	constexpr int64_t DAY_MS = 86400000;
	constexpr xfloat TAU = 6.283185307179586;

	// Bounded or already-periodic columns are left out of the z-scoring.
	bool is_standardised(const int column, const bool price_only)
	{
		return column < 8 || (!price_only && (column == 9 || column == 10 || column == 15));
	}

	xfloat safe_divide(const xfloat a, const xfloat b)
	{
		return b > 1e-12 ? a / b : 0.0;
	}
}

int feature_matrix::column(const std::string& name) const
{
	const auto it = std::find(names.begin(), names.end(), name);
	if (it == names.end()) throw std::invalid_argument("feature profile " + profile + " lacks " + name);
	return static_cast<int>(it - names.begin());
}

std::string feature_matrix::schema() const
{
	std::string out;
	for (const std::string& name : names)
	{
		if (!out.empty()) out += ',';
		out += name;
	}
	return out;
}

const std::vector<std::string>& feature_builder::base_names(const bool price_only)
{
	static const std::vector<std::string> price_names = {
		"ret_1", "ret_5", "ret_15", "ret_60", "ret_300",
		"trend_fast", "trend_slow", "vol_ratio", "range_pos",
		"pd_range_pos", "pd_high_dist"
	};
	static const std::vector<std::string> names = {
		"ret_1", "ret_5", "ret_15", "ret_60", "ret_300",
		"trend_fast", "trend_slow", "vol_ratio",
		"range_pos", "flow", "activity",
		"tod_sin", "tod_cos",
		"pd_range_pos", "pd_high_dist", "activity_surprise",
		"tod_sin2", "tod_cos2", "dow_sin", "dow_cos"
	};
	return price_only ? price_names : names;
}

int feature_builder::base_count(const bool price_only)
{
	return static_cast<int>(base_names(price_only).size());
}

feature_builder::feature_builder(const int64_t interval_ms, const bool price_only)
	: interval_ms_(interval_ms > 0 ? interval_ms : 1000)
	, price_only_(price_only)
	, warmup_(WARMUP_BARS)
	, closes_(RETURN_LAGS[RETURN_LAG_COUNT - 1] + 2)
	, ema_fast_(EMA_FAST)
	, ema_mid_(EMA_MID)
	, ema_slow_(EMA_SLOW)
	, sq_return_short_(VOL_SHORT)
	, sq_return_long_(VOL_LONG)
	, true_range_(EMA_MID)
	, volume_(FLOW_WINDOW)
	, buy_volume_(FLOW_WINDOW)
	, trade_count_(FLOW_WINDOW)
	, trade_rate_(VOL_LONG)
	, range_(RANGE_WINDOW)
	, hourly_activity_(24)
	, stats_(base_names(price_only).size())
{
	// The previous session's levels need a rollover before they exist, which at
	// a fine interval takes longer than the indicator warmup.
	const int64_t per_day = DAY_MS / interval_ms_;
	if (!price_only_ && per_day + 1 > warmup_) warmup_ = static_cast<int>(per_day + 1);
}

void feature_builder::push(const bar& b, std::vector<xfloat>& out, const int64_t session)
{
	out.assign(base_names(price_only_).size(), 0.0);

	const xfloat close = b.close;
	const xfloat previous = seen_ == 0 ? close : previous_close_;
	const xfloat r1 = previous > 0.0
		? (price_only_ ? std::log(close) - std::log(previous) : std::log(close / previous)) : 0.0;

	closes_.push(close);
	range_.push(close);

	const xfloat fast = ema_fast_.push(close);
	const xfloat mid = ema_mid_.push(close);
	const xfloat slow = ema_slow_.push(close);

	const xfloat short_var = sq_return_short_.push(r1 * r1) / VOL_SHORT;
	const xfloat long_var = sq_return_long_.push(r1 * r1) / VOL_LONG;
	const xfloat short_vol = std::sqrt(short_var);
	const xfloat long_vol = std::sqrt(long_var);

	const xfloat range = safe_divide(b.high - b.low, close);
	const xfloat atr = safe_divide(true_range_.push(range), EMA_MID) * close;

	// Returns are divided by their own horizon's volatility so a quiet hour and
	// a violent one produce comparable numbers.
	for (int i = 0; i < RETURN_LAG_COUNT; i += 1)
	{
		const int lag = RETURN_LAGS[i];
		const xfloat past = closes_.ago(lag);
		const xfloat raw = past > 0.0
			? (price_only_ ? std::log(close) - std::log(past) : std::log(close / past)) : 0.0;
		out[i] = safe_divide(raw, short_vol * std::sqrt(static_cast<xfloat>(lag)));
	}

	out[5] = safe_divide(fast - mid, atr);
	out[6] = safe_divide(mid - slow, atr);
	out[7] = short_vol > 0.0 && long_vol > 0.0 ? std::log(short_vol / long_vol) : 0.0;

	const xfloat low = range_.min();
	const xfloat high = range_.max();
	out[8] = high > low ? (close - low) / (high - low) - 0.5 : 0.0;

	levels_.push(b, session);
	if (price_only_ && !levels_.ready()) warmup_ = std::max(warmup_, static_cast<int>(seen_ + 1));
	const xfloat span = levels_.ready() ? levels_.previous_high() - levels_.previous_low() : 0.0;
	if (span > 0.0)
	{
		out[price_only_ ? 9 : 13] = clampx((close - levels_.previous_low()) / span - 0.5, -LEVEL_CLAMP, LEVEL_CLAMP);
		out[price_only_ ? 10 : 14] = clampx((close - levels_.previous_high()) / span, -LEVEL_CLAMP, LEVEL_CLAMP);
	}

	if (!price_only_)
	{
		const xfloat volume = volume_.push(b.volume);
		const xfloat buys = buy_volume_.push(b.buy_volume);
		const xfloat trades = trade_count_.push(b.trades);
		const xfloat rate = trade_rate_.push(b.trades);
		out[9] = safe_divide(2.0 * buys - volume, volume);
		out[10] = std::log((trades + 1.0) / (rate * FLOW_WINDOW + 1.0));
		const xfloat day_fraction = static_cast<xfloat>(b.time % DAY_MS) / static_cast<xfloat>(DAY_MS);
		out[11] = std::sin(day_fraction * TAU);
		out[12] = std::cos(day_fraction * TAU);

		// Remove this hour's own causal activity norm, not a whole-file average.
		const int hour = static_cast<int>((b.time / 3600000) % 24);
		const xfloat norm = hourly_activity_[hour].mean();
		if (hourly_activity_[hour].count() > 24 && norm > 0.0)
		{
			out[15] = std::log((b.trades + 1.0) / (norm + 1.0));
		}
		hourly_activity_[hour].push(static_cast<xfloat>(b.trades));

		out[16] = std::sin(day_fraction * 2.0 * TAU);
		out[17] = std::cos(day_fraction * 2.0 * TAU);

		const xfloat weekday = static_cast<xfloat>((b.time / DAY_MS + 3) % 7) / 7.0;
		out[18] = std::sin(weekday * TAU);
		out[19] = std::cos(weekday * TAU);
	}

	for (size_t i = 0; i < out.size(); i += 1)
	{
		if (!finitex(out[i]))
		{
			if (price_only_) throw std::invalid_argument("nonfinite OHLC feature " + base_names(true)[i]);
			out[i] = 0.0;
		}

		if (!is_standardised(static_cast<int>(i), price_only_)) continue;

		stats_[i].push(out[i]);
		const xfloat sd = stats_[i].sd();
		out[i] = sd > 1e-12 ? clampx((out[i] - stats_[i].mean()) / sd, -CLAMP, CLAMP) : 0.0;
	}

	previous_close_ = close;
	seen_ += 1;
}

feature_matrix build_features(const bar_series& series)
{
	feature_matrix matrix;
	matrix.profile = series.generic ? "ohlc-v1" : "binance-v1";
	matrix.names = feature_builder::base_names(series.generic);
	matrix.columns = feature_builder::base_count(series.generic);
	matrix.data.resize(series.bars.size() * matrix.columns);

	feature_builder builder(series.interval_ms, series.generic);
	std::vector<xfloat> row;

	for (size_t i = 0; i < series.bars.size(); i += 1)
	{
		builder.push(series.bars[i], row, series.session(i));
		std::copy(row.begin(), row.end(), matrix.data.begin() + i * matrix.columns);
	}

	matrix.warmup = builder.warmup();
	return matrix;
}
