#pragma once

#include <string>
#include <vector>

#include "common.h"

struct bar
{
	int64_t time = 0;        // open time, epoch ms
	double open = 0.0;
	double high = 0.0;
	double low = 0.0;
	double close = 0.0;
	double volume = 0.0;     // base asset traded
	double quote = 0.0;      // quote asset traded
	double buy_volume = 0.0; // base asset bought by the aggressor
	int32_t trades = 0;
	int32_t filled = 0;      // 1 when the interval had no trades
};

static_assert(sizeof(bar) == 72, "bar layout is written to disk verbatim");

struct bar_series
{
	int64_t interval_ms = 1000;
	std::string symbol;
	std::vector<bar> bars;

	bool empty() const { return bars.empty(); }
	size_t size() const { return bars.size(); }
};

// Streams one Binance daily trades CSV into the builder. Returns rows parsed.
class bar_builder
{
public:
	explicit bar_builder(int64_t interval_ms);

	void push(int64_t time_ms, double price, double qty, double quote, bool buyer_is_maker);
	void finish();

	std::vector<bar>& bars() { return bars_; }
	int64_t interval() const { return interval_ms_; }

private:
	void open_bar(int64_t slot, double price);
	void close_bar();

	int64_t interval_ms_ = 1000;
	int64_t slot_ = -1;
	bar current_ = {};
	bool open_ = false;
	std::vector<bar> bars_;
};

size_t load_trades_csv(const std::string& path, bar_builder& builder, std::string* error);

bool write_bars(const std::string& path, const bar_series& series, std::string* error);
bool read_bars(const std::string& path, bar_series& series, std::string* error);

uint64_t hash_bars(const bar_series& series);
