#include "bars.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace
{
	constexpr char BARS_MAGIC[4] = { 'S', 'P', 'Q', 'B' };
	constexpr uint32_t BARS_VERSION = 1;

	// Binance publishes at most a few days of continuous silence; anything
	// larger is a hole in the archive rather than a quiet market.
	constexpr int64_t MAX_FILL_BARS = 400000;
}

bar_builder::bar_builder(const int64_t interval_ms)
	: interval_ms_(interval_ms > 0 ? interval_ms : 1000)
{
}

void bar_builder::open_bar(const int64_t slot, const double price)
{
	current_ = {};
	current_.time = slot * interval_ms_;
	current_.open = price;
	current_.high = price;
	current_.low = price;
	current_.close = price;
	slot_ = slot;
	open_ = true;
}

void bar_builder::close_bar()
{
	if (!open_) return;
	bars_.push_back(current_);
	open_ = false;
}

void bar_builder::push(const int64_t time_ms, const double price, const double qty,
	const double quote, const bool buyer_is_maker)
{
	const int64_t slot = time_ms / interval_ms_;

	if (!open_)
	{
		open_bar(slot, price);
	}
	else if (slot > slot_)
	{
		const double carry = current_.close;
		close_bar();

		const int64_t gap = slot - slot_ - 1;
		if (gap > 0 && gap < MAX_FILL_BARS)
		{
			for (int64_t i = 1; i <= gap; i += 1)
			{
				bar quiet = {};
				quiet.time = (slot_ + i) * interval_ms_;
				quiet.open = quiet.high = quiet.low = quiet.close = carry;
				quiet.filled = 1;
				bars_.push_back(quiet);
			}
		}

		open_bar(slot, price);
	}

	// Trades within a file are ordered, so anything earlier than the open bar
	// is a timestamp artifact; fold it into the bar in progress.
	if (price > current_.high) current_.high = price;
	if (price < current_.low) current_.low = price;
	current_.close = price;
	current_.volume += qty;
	current_.quote += quote;
	if (!buyer_is_maker) current_.buy_volume += qty;
	current_.trades += 1;
}

void bar_builder::finish()
{
	close_bar();
}

size_t load_trades_csv(const std::string& path, bar_builder& builder, std::string* error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		if (error) *error = "cannot open " + path;
		return 0;
	}

	std::string line;
	size_t rows = 0;
	bool first = true;

	while (std::getline(file, line))
	{
		if (line.empty()) continue;

		if (first)
		{
			first = false;
			// Newer archives carry a header row; older ones do not.
			if (line[0] < '0' || line[0] > '9') continue;
		}

		// id,price,qty,quote_qty,time,is_buyer_maker
		const char* p = line.c_str();
		double field[5] = {};
		int found = 0;

		for (; found < 5; found += 1)
		{
			char* end = nullptr;
			field[found] = strtod(p, &end);
			if (end == p) break;
			p = end;
			if (*p == ',') p += 1;
			else if (found < 4) break;
		}

		if (found < 5) continue;

		bool buyer_is_maker = false;
		while (*p == ' ') p += 1;
		if (*p == 't' || *p == 'T' || *p == '1') buyer_is_maker = true;

		const double price = field[1];
		const double qty = field[2];
		const double quote = field[3];
		const auto time_ms = static_cast<int64_t>(field[4]);

		if (price <= 0.0 || qty < 0.0 || time_ms <= 0) continue;

		builder.push(time_ms, price, qty, quote > 0.0 ? quote : price * qty, buyer_is_maker);
		rows += 1;
	}

	return rows;
}

bool write_bars(const std::string& path, const bar_series& series, std::string* error)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file)
	{
		if (error) *error = "cannot write " + path;
		return false;
	}

	const uint32_t version = BARS_VERSION;
	const int64_t interval = series.interval_ms;
	const int64_t count = static_cast<int64_t>(series.bars.size());
	const uint32_t symbol_len = static_cast<uint32_t>(series.symbol.size());

	file.write(BARS_MAGIC, 4);
	file.write(reinterpret_cast<const char*>(&version), sizeof version);
	file.write(reinterpret_cast<const char*>(&interval), sizeof interval);
	file.write(reinterpret_cast<const char*>(&count), sizeof count);
	file.write(reinterpret_cast<const char*>(&symbol_len), sizeof symbol_len);
	file.write(series.symbol.data(), symbol_len);
	file.write(reinterpret_cast<const char*>(series.bars.data()), count * static_cast<int64_t>(sizeof(bar)));

	return file.good();
}

bool read_bars(const std::string& path, bar_series& series, std::string* error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		if (error) *error = "cannot open " + path;
		return false;
	}

	char magic[4] = {};
	uint32_t version = 0;
	int64_t interval = 0;
	int64_t count = 0;
	uint32_t symbol_len = 0;

	file.read(magic, 4);
	file.read(reinterpret_cast<char*>(&version), sizeof version);
	file.read(reinterpret_cast<char*>(&interval), sizeof interval);
	file.read(reinterpret_cast<char*>(&count), sizeof count);
	file.read(reinterpret_cast<char*>(&symbol_len), sizeof symbol_len);

	if (!file || memcmp(magic, BARS_MAGIC, 4) != 0 || version != BARS_VERSION)
	{
		if (error) *error = path + " is not a spq bar file";
		return false;
	}

	if (count < 0 || symbol_len > 64)
	{
		if (error) *error = path + " has an implausible header";
		return false;
	}

	series.symbol.resize(symbol_len);
	if (symbol_len > 0) file.read(series.symbol.data(), symbol_len);

	series.interval_ms = interval;
	series.bars.resize(static_cast<size_t>(count));
	file.read(reinterpret_cast<char*>(series.bars.data()), count * static_cast<int64_t>(sizeof(bar)));

	if (!file)
	{
		if (error) *error = path + " is truncated";
		return false;
	}

	return true;
}

uint64_t hash_bars(const bar_series& series)
{
	hasher h;
	h.feed(series.symbol);
	h.feed(&series.interval_ms, sizeof series.interval_ms);
	h.feed(series.bars.data(), series.bars.size() * sizeof(bar));
	return h.digest();
}
