#include "features.h"

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

	// Bounded or already-periodic columns are left out of the z-scoring.
	bool is_standardised(const int column)
	{
		return column < 8 || column == 9 || column == 10;
	}

	xfloat safe_divide(const xfloat a, const xfloat b)
	{
		return b > 1e-12 ? a / b : 0.0;
	}
}

const std::vector<std::string>& feature_builder::base_names()
{
	static const std::vector<std::string> names = {
		"ret_1", "ret_5", "ret_15", "ret_60", "ret_300",
		"trend_fast", "trend_slow", "vol_ratio",
		"range_pos", "flow", "activity",
		"tod_sin", "tod_cos"
	};
	return names;
}

int feature_builder::base_count()
{
	return static_cast<int>(base_names().size());
}

feature_builder::feature_builder(const int64_t interval_ms)
	: interval_ms_(interval_ms > 0 ? interval_ms : 1000)
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
	, stats_(base_names().size())
{
}

void feature_builder::push(const bar& b, std::vector<xfloat>& out)
{
	out.assign(base_names().size(), 0.0);

	const xfloat close = b.close;
	const xfloat previous = seen_ == 0 ? close : previous_close_;
	const xfloat r1 = previous > 0.0 ? std::log(close / previous) : 0.0;

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

	const xfloat volume = volume_.push(b.volume);
	const xfloat buys = buy_volume_.push(b.buy_volume);
	const xfloat trades = trade_count_.push(b.trades);
	const xfloat rate = trade_rate_.push(b.trades);

	// Returns are divided by their own horizon's volatility so a quiet hour and
	// a violent one produce comparable numbers.
	for (int i = 0; i < RETURN_LAG_COUNT; i += 1)
	{
		const int lag = RETURN_LAGS[i];
		const xfloat past = closes_.ago(lag);
		const xfloat raw = past > 0.0 ? std::log(close / past) : 0.0;
		out[i] = safe_divide(raw, short_vol * std::sqrt(static_cast<xfloat>(lag)));
	}

	out[5] = safe_divide(fast - mid, atr);
	out[6] = safe_divide(mid - slow, atr);
	out[7] = short_vol > 0.0 && long_vol > 0.0 ? std::log(short_vol / long_vol) : 0.0;

	const xfloat low = range_.min();
	const xfloat high = range_.max();
	out[8] = high > low ? (close - low) / (high - low) - 0.5 : 0.0;

	out[9] = safe_divide(2.0 * buys - volume, volume);
	out[10] = std::log((trades + 1.0) / (rate * FLOW_WINDOW + 1.0));

	const xfloat day_fraction = static_cast<xfloat>(b.time % 86400000) / 86400000.0;
	out[11] = std::sin(day_fraction * 6.283185307179586);
	out[12] = std::cos(day_fraction * 6.283185307179586);

	for (size_t i = 0; i < out.size(); i += 1)
	{
		if (!finitex(out[i])) out[i] = 0.0;

		if (!is_standardised(static_cast<int>(i))) continue;

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
	matrix.names = feature_builder::base_names();
	matrix.columns = feature_builder::base_count();
	matrix.data.resize(series.bars.size() * matrix.columns);

	feature_builder builder(series.interval_ms);
	std::vector<xfloat> row;

	for (size_t i = 0; i < series.bars.size(); i += 1)
	{
		builder.push(series.bars[i], row);
		std::copy(row.begin(), row.end(), matrix.data.begin() + i * matrix.columns);
	}

	matrix.warmup = builder.warmup();
	return matrix;
}
