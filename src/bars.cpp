#include "bars.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

namespace
{
	constexpr char BARS_MAGIC[4] = { 'S', 'P', 'Q', 'B' };
	constexpr uint32_t BARS_VERSION = 1;

	// Binance publishes at most a few days of continuous silence; anything
	// larger is a hole in the archive rather than a quiet market.
	constexpr int64_t MAX_FILL_BARS = 400000;

	bool valid_text(const std::string& text, const size_t limit = 256)
	{
		return !text.empty() && text.size() <= limit &&
			std::all_of(text.begin(), text.end(), [](const unsigned char c) { return c >= 32 && c != 127; });
	}

	bool valid_generic(const bar_series& series, std::string* error)
	{
		const auto fail = [&](const std::string& message) {
			if (error) *error = message;
			return false;
		};
		if (series.interval_ms <= 0 || series.interval_ms > 365LL * 86400000 ||
			!valid_text(series.symbol, 64) || !valid_text(series.market.source) ||
			!valid_text(series.market.currency) || !valid_text(series.market.calendar) ||
			(series.market.price_basis != "raw" && series.market.price_basis != "adjusted"))
			return fail("invalid OHLCV interval, symbol or market metadata");
		if (series.sessions.size() != series.size() || series.size() > INT32_MAX)
			return fail("OHLCV sessions must match the bar count");
		for (size_t i = 0; i < series.size(); ++i)
		{
			const bar& b = series.bars[i];
			const std::string row = "OHLCV record " + std::to_string(i + 1) + ": ";
			if (b.time < 0 || b.time > INT64_MAX - series.interval_ms ||
				(i && b.time - series.bars[i - 1].time < series.interval_ms))
				return fail(row + "timestamps must be increasing, non-overlapping epoch milliseconds");
			if (!(b.low > 0.0) || !finitex(b.open) || !finitex(b.high) || !finitex(b.low) || !finitex(b.close) ||
				b.low > std::min(b.open, b.close) || b.high < std::max(b.open, b.close) || b.high < b.low)
				return fail(row + "expected finite positive OHLC prices with low <= open/close <= high");
			if (!std::isfinite(b.volume) || b.volume < 0.0 || (!series.market.volume_available && b.volume != 0.0) ||
				b.quote != 0.0 || b.buy_volume != 0.0 || b.trades != 0 || b.filled != 0)
				return fail(row + "invalid volume or unsupported flow/filled fields");
			if (series.sessions[i] < 0 || (i && series.sessions[i] < series.sessions[i - 1]) ||
				(series.market.calendar == "utc" && series.sessions[i] != b.time / 86400000))
				return fail(row + "session identifiers must be nonnegative and nondecreasing and match the declared calendar");
		}
		return true;
	}

	void write_text(std::ostream& file, const std::string& text)
	{
		const auto size = static_cast<uint32_t>(text.size());
		file.write(reinterpret_cast<const char*>(&size), sizeof size);
		file.write(text.data(), size);
	}

	bool read_text(std::istream& file, std::string& text)
	{
		uint32_t size = 0;
		file.read(reinterpret_cast<char*>(&size), sizeof size);
		if (!file || size == 0 || size > 256) return false;
		text.resize(size);
		file.read(text.data(), size);
		return file.good() && valid_text(text);
	}
}

const char* bar_format_name(const bar_series& series)
{
	return series.generic ? "SPQB-v2" : "SPQB-v1";
}

size_t bar_index_at_time(const bar_series& series, const int64_t time)
{
	const auto it = std::lower_bound(series.bars.begin(), series.bars.end(), time,
		[](const bar& b, const int64_t t) { return b.time < t; });
	return it != series.bars.end() && it->time == time
		? static_cast<size_t>(it - series.bars.begin()) : series.size();
}

bool load_ohlcv_csv(const std::string& path, bar_series& series, std::string* error)
{
	std::ifstream file(path, std::ios::binary);
	size_t line_number = 1;
	const auto fail = [&](const std::string& message) {
		if (error) *error = path + ":" + std::to_string(line_number) + ": " + message;
		return false;
	};
	if (!file) return fail("cannot open OHLCV CSV");
	std::string line;
	if (!std::getline(file, line)) return fail("missing OHLCV header");
	if (line.rfind("\xef\xbb\xbf", 0) == 0) line.erase(0, 3);
	std::vector<std::string> columns = split(trim(line), ',');
	for (std::string& column : columns) column = trim(column);
	const std::vector<std::string> allowed = { "time", "open", "high", "low", "close", "volume", "session" };
	for (const std::string& column : columns)
	{
		if (std::find(allowed.begin(), allowed.end(), column) == allowed.end() ||
			std::count(columns.begin(), columns.end(), column) != 1)
			return fail("unknown or duplicate OHLCV column: " + column);
	}
	const auto index = [&](const std::string& column) {
		return static_cast<size_t>(std::find(columns.begin(), columns.end(), column) - columns.begin());
	};
	for (const char* name : { "time", "open", "high", "low", "close" })
		if (index(name) == columns.size()) return fail(std::string("missing OHLCV column: ") + name);
	const bool has_session = index("session") != columns.size();
	if (has_session != (series.market.calendar != "utc"))
		return fail("--calendar utc requires no session column; a supplied calendar requires a session column");
	bar_series candidate;
	candidate.generic = true;
	candidate.interval_ms = series.interval_ms;
	candidate.symbol = series.symbol;
	candidate.market = series.market;
	candidate.market.volume_available = index("volume") != columns.size();
	const auto integer = [](const std::string& cell, int64_t& value) {
		const auto result = std::from_chars(cell.data(), cell.data() + cell.size(), value);
		return result.ec == std::errc{} && result.ptr == cell.data() + cell.size();
	};
	while (std::getline(file, line))
	{
		++line_number;
		if (trim(line).empty()) continue;
		std::vector<std::string> cells = split(line, ',');
		if (cells.size() != columns.size()) return fail("OHLCV row width differs from the header");
		for (std::string& cell : cells) cell = trim(cell);
		bar b;
		int64_t session = 0;
		if (!integer(cells[index("time")], b.time)) return fail("time must be integer epoch milliseconds");
		if (has_session && !integer(cells[index("session")], session)) return fail("session must be an integer identifier");
		if (!has_session) session = b.time / 86400000;
		for (const auto& [name, value] : std::vector<std::pair<std::string, double*>>{
			{ "open", &b.open }, { "high", &b.high }, { "low", &b.low }, { "close", &b.close }, { "volume", &b.volume } })
		{
			if (name == "volume" && !candidate.market.volume_available) continue;
			if (!parse_real(cells[index(name)], *value) || !std::isfinite(*value))
				return fail(name + " must be a finite number, not a missing value");
		}
		candidate.bars.push_back(b);
		candidate.sessions.push_back(session);
	}
	if (!file.eof()) return fail("failed reading OHLCV CSV");
	if (candidate.empty()) return fail("no OHLCV rows");
	if (!valid_generic(candidate, error)) return false;
	series = std::move(candidate);
	return true;
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

void bar_builder::advance(const int64_t slot, const double price)
{
	if (!open_)
	{
		open_bar(slot, price);
		return;
	}

	if (slot <= slot_) return;

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

void bar_builder::push(const int64_t time_ms, const double price, const double qty,
	const double quote, const bool buyer_is_maker)
{
	advance(time_ms / interval_ms_, price);
	if (time_ms > last_time_) last_time_ = time_ms;

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

bool bar_builder::push_bar(const bar& source)
{
	if (source.time <= last_time_) return false;
	last_time_ = source.time;
	advance(source.time / interval_ms_, source.open);

	if (source.high > current_.high) current_.high = source.high;
	if (source.low < current_.low) current_.low = source.low;
	current_.close = source.close;
	current_.volume += source.volume;
	current_.quote += source.quote;
	current_.buy_volume += source.buy_volume;
	current_.trades += source.trades;
	return true;
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

		// strtod accepts "nan", "inf" and overflows to infinity; none is a trade,
		// and the gap fill would carry one forward into every quiet bar after it.
		if (!std::isfinite(price) || !std::isfinite(qty) || !std::isfinite(quote) ||
			!std::isfinite(field[4]) || field[4] <= 0.0 || field[4] >= 9.2e18) continue;

		const auto time_ms = static_cast<int64_t>(field[4]);

		if (price <= 0.0 || qty < 0.0 || time_ms <= 0) continue;

		// A file that starts inside data already read overlaps it, typically a
		// daily archive beside the monthly one covering the same dates.
		if (rows == 0 && !builder.continues(time_ms))
		{
			if (error) *error = path + " starts at " + format_time_ms(time_ms) +
				", inside trades already read; remove overlapping daily/monthly archives";
			return 0;
		}

		builder.push(time_ms, price, qty, quote > 0.0 ? quote : price * qty, buyer_is_maker);
		rows += 1;
	}

	return rows;
}

csv_kind detect_csv_kind(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	std::string line;
	if (!file || !std::getline(file, line)) return csv_kind::unknown;

	if (line.rfind("open_time", 0) == 0) return csv_kind::klines;
	if (line.rfind("id,", 0) == 0) return csv_kind::trades;
	// Aggregate trades are wide enough for the trades layout, which would read
	// their trade ids as timestamps and publish bars dated 1970.
	if (line.rfind("agg_trade_id", 0) == 0) return csv_kind::unknown;

	// Older archives ship no header, but the two layouts differ in width.
	const auto commas = std::count(line.begin(), line.end(), ',');
	if (commas >= 10) return csv_kind::klines;
	if (commas >= 4) return csv_kind::trades;
	return csv_kind::unknown;
}

size_t load_klines_csv(const std::string& path, bar_builder& builder, std::string* error)
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
			if (line[0] < '0' || line[0] > '9') continue;
		}

		// open_time,open,high,low,close,volume,close_time,quote_volume,count,
		// taker_buy_volume,...
		const char* p = line.c_str();
		double field[10] = {};
		int found = 0;

		for (; found < 10; found += 1)
		{
			char* end = nullptr;
			field[found] = strtod(p, &end);
			if (end == p) break;
			p = end;
			if (*p == ',') p += 1;
			else if (found < 9) break;
		}

		if (found < 10) continue;

		bool finite = field[0] > 0.0 && field[0] < 9.2e18;
		for (const double value : field) finite = finite && std::isfinite(value);
		if (!finite) continue;

		bar b = {};
		b.time = static_cast<int64_t>(field[0]);
		b.open = field[1];
		b.high = field[2];
		b.low = field[3];
		b.close = field[4];
		b.volume = field[5];
		b.quote = field[7];
		b.trades = static_cast<int32_t>(field[8]);
		b.buy_volume = field[9];

		if (b.time <= 0 || b.open <= 0.0 || b.close <= 0.0) continue;

		if (!builder.push_bar(b))
		{
			if (error) *error = path + ": kline at " + format_time_ms(b.time) +
				" does not follow the previous one; remove overlapping daily/monthly archives or duplicate rows";
			return 0;
		}
		rows += 1;
	}

	return rows;
}

int64_t kline_interval_ms(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	std::string line;

	while (std::getline(file, line))
	{
		if (line.empty() || line[0] < '0' || line[0] > '9') continue;

		// open_time,open,high,low,close,volume,close_time,...
		const char* p = line.c_str();
		double field[7] = {};
		int found = 0;

		for (; found < 7; found += 1)
		{
			char* end = nullptr;
			field[found] = strtod(p, &end);
			if (end == p) break;
			p = end;
			if (*p == ',') p += 1;
			else if (found < 6) break;
		}

		// Binance closes a kline one millisecond before the next one opens.
		const double span = field[6] - field[0] + 1.0;
		if (found == 7 && std::isfinite(span) && span >= 1.0 && span < 9.2e18) return static_cast<int64_t>(span);
	}

	return 0;
}

bool write_bars(const std::string& path, const bar_series& series, std::string* error)
{
	if (series.generic && !valid_generic(series, error)) return false;
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file)
	{
		if (error) *error = "cannot write " + path;
		return false;
	}

	const uint32_t version = series.generic ? 2 : BARS_VERSION;
	const int64_t interval = series.interval_ms;
	const int64_t count = static_cast<int64_t>(series.bars.size());
	const uint32_t symbol_len = static_cast<uint32_t>(series.symbol.size());

	file.write(BARS_MAGIC, 4);
	file.write(reinterpret_cast<const char*>(&version), sizeof version);
	file.write(reinterpret_cast<const char*>(&interval), sizeof interval);
	file.write(reinterpret_cast<const char*>(&count), sizeof count);
	file.write(reinterpret_cast<const char*>(&symbol_len), sizeof symbol_len);
	file.write(series.symbol.data(), symbol_len);
	if (series.generic)
	{
		const uint32_t volume = series.market.volume_available ? 1 : 0;
		file.write(reinterpret_cast<const char*>(&volume), sizeof volume);
		for (const std::string* text : { &series.market.source, &series.market.currency,
			&series.market.calendar, &series.market.price_basis }) write_text(file, *text);
	}
	file.write(reinterpret_cast<const char*>(series.bars.data()), count * static_cast<int64_t>(sizeof(bar)));
	if (series.generic)
		file.write(reinterpret_cast<const char*>(series.sessions.data()), count * static_cast<int64_t>(sizeof(int64_t)));

	file.close();
	if (!file)
	{
		if (error) *error = "failed writing bar file " + path;
		return false;
	}
	return true;
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

	if (!file || memcmp(magic, BARS_MAGIC, 4) != 0 || (version != BARS_VERSION && version != 2))
	{
		if (error) *error = path + " is not a supported price-app bar file";
		return false;
	}

	if (count < 0 || interval <= 0 || symbol_len > 64)
	{
		if (error) *error = path + " has an implausible header";
		return false;
	}

	bar_series candidate;
	candidate.generic = version == 2;
	candidate.symbol.resize(symbol_len);
	if (symbol_len > 0) file.read(candidate.symbol.data(), symbol_len);
	if (candidate.generic)
	{
		uint32_t volume = 0;
		file.read(reinterpret_cast<char*>(&volume), sizeof volume);
		if (!file || volume > 1 || !read_text(file, candidate.market.source) ||
			!read_text(file, candidate.market.currency) || !read_text(file, candidate.market.calendar) ||
			!read_text(file, candidate.market.price_basis))
		{
			if (error) *error = path + " has invalid SPQB-v2 market metadata";
			return false;
		}
		candidate.market.volume_available = volume == 1;
	}
	const auto data_start = file.tellg();
	const auto header_bytes = data_start < 0 ? UINT64_MAX : static_cast<uint64_t>(data_start);
	const uint64_t record_bytes = sizeof(bar) + (candidate.generic ? sizeof(int64_t) : 0);
	file.seekg(0, std::ios::end);
	const auto file_bytes = file.tellg();
	if (file_bytes < 0 || static_cast<uint64_t>(file_bytes) < header_bytes ||
		static_cast<uint64_t>(count) > (static_cast<uint64_t>(file_bytes) - header_bytes) / record_bytes ||
		static_cast<uint64_t>(count) > SIZE_MAX / sizeof(bar) ||
		(candidate.generic && count > INT32_MAX) ||
		header_bytes + static_cast<uint64_t>(count) * record_bytes != static_cast<uint64_t>(file_bytes))
	{
		if (error) *error = path + " has a truncated or inconsistent record count";
		return false;
	}
	file.seekg(data_start);

	candidate.interval_ms = interval;
	candidate.bars.resize(static_cast<size_t>(count));
	file.read(reinterpret_cast<char*>(candidate.bars.data()), count * static_cast<int64_t>(sizeof(bar)));
	if (candidate.generic)
	{
		candidate.sessions.resize(static_cast<size_t>(count));
		file.read(reinterpret_cast<char*>(candidate.sessions.data()), count * static_cast<int64_t>(sizeof(int64_t)));
	}

	if (!file)
	{
		if (error) *error = path + " is truncated";
		return false;
	}
	if (candidate.generic && !valid_generic(candidate, error)) return false;

	series = std::move(candidate);
	return true;
}

uint64_t hash_bars(const bar_series& series)
{
	hasher h;
	h.feed(series.symbol);
	h.feed(&series.interval_ms, sizeof series.interval_ms);
	h.feed(series.bars.data(), series.bars.size() * sizeof(bar));
	if (series.generic)
	{
		h.feed("SPQB-v2");
		const uint32_t volume = series.market.volume_available ? 1 : 0;
		h.feed(&volume, sizeof volume);
		for (const std::string* text : { &series.market.source, &series.market.currency,
			&series.market.calendar, &series.market.price_basis })
		{
			const auto size = static_cast<uint32_t>(text->size());
			h.feed(&size, sizeof size);
			h.feed(*text);
		}
		h.feed(series.sessions.data(), series.sessions.size() * sizeof(int64_t));
	}
	return h.digest();
}
