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

struct market_info
{
	std::string source = "binance";
	std::string currency = "unspecified";
	std::string calendar = "utc";
	std::string price_basis = "raw";
	bool volume_available = true;
};

struct bar_series
{
	int64_t interval_ms = 1000;
	std::string symbol;
	std::vector<bar> bars;
	bool generic = false;
	market_info market;
	std::vector<int64_t> sessions;

	bool empty() const { return bars.empty(); }
	size_t size() const { return bars.size(); }
	int64_t session(const size_t i) const { return generic ? sessions.at(i) : bars[i].time / 86400000; }
};

size_t bar_index_at_time(const bar_series& series, int64_t time);
const char* bar_format_name(const bar_series& series);
bool load_ohlcv_csv(const std::string& path, bar_series& series, std::string* error);

// Streams one Binance daily trades CSV into the builder. Returns rows parsed.
class bar_builder
{
public:
	explicit bar_builder(int64_t interval_ms);

	void push(int64_t time_ms, double price, double qty, double quote, bool buyer_is_maker);

	// Merges an already-complete source bar, so a fine-grained kline file can be
	// aggregated to any wider interval by the same code path. A kline that does
	// not follow the previous one is refused, never merged into the open bar.
	bool push_bar(const bar& source);

	// Whether a source file starting at this time follows what was already read.
	bool continues(const int64_t time_ms) const { return time_ms >= last_time_; }

	void finish();

	std::vector<bar>& bars() { return bars_; }
	int64_t interval() const { return interval_ms_; }

private:
	void open_bar(int64_t slot, double price);
	void close_bar();
	void advance(int64_t slot, double price);

	int64_t interval_ms_ = 1000;
	int64_t slot_ = -1;
	int64_t last_time_ = INT64_MIN;
	bar current_ = {};
	bool open_ = false;
	std::vector<bar> bars_;
};

enum class csv_kind
{
	unknown,
	trades,
	klines
};

csv_kind detect_csv_kind(const std::string& path);

size_t load_trades_csv(const std::string& path, bar_builder& builder, std::string* error);

// Binance kline columns carry taker buy volume and trade count, so a kline file
// produces the same bar as the raw trades it was aggregated from.
size_t load_klines_csv(const std::string& path, bar_builder& builder, std::string* error);

// The interval a kline file was published at, from its first row's open and
// close times; 0 when no row carries both.
int64_t kline_interval_ms(const std::string& path);

bool write_bars(const std::string& path, const bar_series& series, std::string* error);
bool read_bars(const std::string& path, bar_series& series, std::string* error);

uint64_t hash_bars(const bar_series& series);
