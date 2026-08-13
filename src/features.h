#pragma once

#include <deque>
#include <string>
#include <vector>

#include "bars.h"
#include "common.h"

// Every indicator here is a streaming object: it takes one value at a time and
// never holds the series, so it is structurally incapable of reading forwards.

class ema_line
{
public:
	explicit ema_line(const int span) : alpha_(2.0 / (span + 1.0)) {}

	xfloat push(const xfloat x)
	{
		if (!started_)
		{
			value_ = x;
			started_ = true;
		}
		else
		{
			value_ += alpha_ * (x - value_);
		}
		return value_;
	}

	xfloat value() const { return value_; }

private:
	xfloat alpha_;
	xfloat value_ = 0.0;
	bool started_ = false;
};

class rolling_sum
{
public:
	explicit rolling_sum(const int window) : window_(window < 1 ? 1 : window)
	{
		buffer_.assign(window_, 0.0);
	}

	xfloat push(const xfloat x)
	{
		sum_ += x - buffer_[index_];
		buffer_[index_] = x;
		index_ = (index_ + 1) % window_;
		if (count_ < window_) count_ += 1;
		return sum_;
	}

	xfloat sum() const { return sum_; }
	xfloat mean() const { return count_ > 0 ? sum_ / count_ : 0.0; }
	bool full() const { return count_ >= window_; }

private:
	int window_;
	std::vector<xfloat> buffer_;
	int index_ = 0;
	int count_ = 0;
	xfloat sum_ = 0.0;
};

// Monotonic deques, so the extremes cost O(1) amortised rather than O(window).
class rolling_extremes
{
public:
	explicit rolling_extremes(const int window) : window_(window < 1 ? 1 : window) {}

	void push(const xfloat x)
	{
		while (!min_.empty() && min_.back().second >= x) min_.pop_back();
		while (!max_.empty() && max_.back().second <= x) max_.pop_back();
		min_.emplace_back(step_, x);
		max_.emplace_back(step_, x);

		const int64_t cutoff = step_ - window_ + 1;
		while (min_.front().first < cutoff) min_.pop_front();
		while (max_.front().first < cutoff) max_.pop_front();

		step_ += 1;
	}

	xfloat min() const { return min_.empty() ? 0.0 : min_.front().second; }
	xfloat max() const { return max_.empty() ? 0.0 : max_.front().second; }

private:
	int window_;
	int64_t step_ = 0;
	std::deque<std::pair<int64_t, xfloat>> min_;
	std::deque<std::pair<int64_t, xfloat>> max_;
};

class history
{
public:
	explicit history(const int depth) : depth_(depth < 1 ? 1 : depth)
	{
		buffer_.assign(depth_ + 1, 0.0);
	}

	void push(const xfloat x)
	{
		index_ = (index_ + 1) % static_cast<int>(buffer_.size());
		buffer_[index_] = x;
		if (count_ < static_cast<int>(buffer_.size())) count_ += 1;
	}

	// ago = 0 is the value just pushed.
	xfloat ago(const int n) const
	{
		const int size = static_cast<int>(buffer_.size());
		const int want = n < 0 ? 0 : (n >= size ? size - 1 : n);
		return buffer_[((index_ - want) % size + size) % size];
	}

	bool has(const int n) const { return count_ > n; }

private:
	int depth_;
	std::vector<xfloat> buffer_;
	int index_ = 0;
	int count_ = 0;
};

// Trailing mean and variance over everything seen so far. Whole-file statistics
// would leak the future; this cannot, because it only ever sees the past.
class expanding_stats
{
public:
	void push(const xfloat x)
	{
		count_ += 1;
		const xfloat delta = x - mean_;
		mean_ += delta / count_;
		m2_ += delta * (x - mean_);
	}

	xfloat mean() const { return mean_; }
	xfloat sd() const { return count_ > 1 ? std::sqrt(m2_ / (count_ - 1)) : 0.0; }
	int64_t count() const { return count_; }

private:
	int64_t count_ = 0;
	xfloat mean_ = 0.0;
	xfloat m2_ = 0.0;
};

struct feature_matrix
{
	int columns = 0;
	int warmup = 0;               // rows before this index are not usable
	std::vector<std::string> names;
	std::vector<xfloat> data;     // row-major, columns wide

	const xfloat* row(const size_t i) const { return &data[i * columns]; }
	size_t rows() const { return columns > 0 ? data.size() / columns : 0; }
};

// Base observation, before the environment appends the agent's own position.
class feature_builder
{
public:
	explicit feature_builder(int64_t interval_ms);

	void push(const bar& b, std::vector<xfloat>& out);

	static const std::vector<std::string>& base_names();
	static int base_count();
	int warmup() const { return warmup_; }

private:
	int64_t interval_ms_;
	int warmup_;
	int64_t seen_ = 0;

	history closes_;
	ema_line ema_fast_, ema_mid_, ema_slow_;
	rolling_sum sq_return_short_, sq_return_long_;
	rolling_sum true_range_;
	rolling_sum volume_, buy_volume_, trade_count_;
	ema_line trade_rate_;
	rolling_extremes range_;
	std::vector<expanding_stats> stats_;
	xfloat previous_close_ = 0.0;
};

feature_matrix build_features(const bar_series& series);
