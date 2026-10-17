#include "bars.h"
#include <spike_db.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;
void check(SpikeDB_Status status)
{
	if (status != SPIKEDB_OK) throw std::runtime_error("spike-db status " + std::to_string(status));
}
double elapsed(clock_type::time_point start)
{
	return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}
uint64_t peak_rss()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS info{};
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info)) throw std::runtime_error("process memory query failed");
	return info.PeakWorkingSetSize;
#else
	rusage info{};
	if (getrusage(RUSAGE_SELF, &info)) throw std::runtime_error("process memory query failed");
	return static_cast<uint64_t>(info.ru_maxrss) * 1024;
#endif
}
using database = std::unique_ptr<SpikeDB, decltype(&spike_db_close)>;
database open(const fs::path& path, uint32_t cache, uint32_t flags)
{
	SpikeDB* raw = nullptr;
	check(spike_db_open(&raw, path.string().c_str(), cache, flags));
	return database(raw, spike_db_close);
}

int main(int argc, char** argv)
{
	try
	{
		if (argc < 3) throw std::runtime_error("storage_bench create ROOT [COUNT|SPQB_FILE] | scan ROOT spqb|spike CACHE_PAGES materialize|stream");
		const std::string operation = argv[1];
		const fs::path root = argv[2];
		const auto spqb = root / "bars.bin", spike = root / "bars.spike";
		if (operation == "create")
		{
			if (fs::exists(spqb) || fs::exists(spike)) throw std::runtime_error("refusing to replace benchmark files");
			fs::create_directories(root);
			bar_series series;
			series.symbol = "ETHUSDT"; series.interval_ms = 60000;
			std::string error;
			const bool source_file = argc > 3 && fs::is_regular_file(argv[3]);
			if (source_file)
			{
				if (!read_bars(argv[3], series, &error)) throw std::runtime_error(error);
				if (series.generic) throw std::invalid_argument("storage comparison supports SPQB v1 only; SPQB v2 market metadata must not be discarded");
			}
			else
			{
				const size_t count = argc > 3 ? std::stoull(argv[3]) : 3461760;
				if (count > 10000000 || count == 0) throw std::invalid_argument("benchmark count must be 1..10000000");
				series.bars.resize(count);
				for (size_t i = 0; i < count; ++i)
				{
					bar& b = series.bars[i];
					b.time = INT64_C(1577836800000) + static_cast<int64_t>(i) * 60000;
					b.open = 1000 + 20 * std::sin(i * 0.017);
					b.close = b.open + std::sin(i * 0.31);
					b.high = std::max(b.open, b.close) + 0.5; b.low = std::min(b.open, b.close) - 0.5;
					b.volume = 100 + i % 37; b.quote = b.volume * b.close; b.buy_volume = 40 + i % 31;
					b.trades = static_cast<int>(20 + i % 17);
				}
			}
			auto start = clock_type::now();
			if (!write_bars(spqb.string(), series, &error)) throw std::runtime_error(error);
			const double spqb_ms = elapsed(start);
			start = clock_type::now();
			auto db = open(spike, 64, 0);
			std::unique_ptr<SpikeDB_Batch, decltype(&spike_db_batch_destroy)> batch(spike_db_batch_create(), spike_db_batch_destroy);
			if (!batch) throw std::bad_alloc();
			check(spike_db_batch_put_meta(batch.get(), "symbol", series.symbol.data(), series.symbol.size()));
			check(spike_db_batch_put_meta(batch.get(), "interval_ms", &series.interval_ms, sizeof series.interval_ms));
			for (size_t i = 0; i < series.size(); ++i)
			{
				const bar& b = series.bars[i];
				check(spike_db_batch_put(batch.get(), 1, b.time, reinterpret_cast<const char*>(&b) + 8, sizeof(bar) - 8));
				if ((i + 1) % 32768 == 0 || i + 1 == series.size())
				{
					check(spike_db_write(db.get(), batch.get()));
					spike_db_batch_clear(batch.get());
				}
			}
			db.reset();
			std::cout << "source=" << (source_file ? "market-file" : "synthetic") << ",records=" << series.size()
				<< ",spqb_write_ms=" << spqb_ms << ",spike_durable_write_ms=" << elapsed(start)
				<< ",spqb_bytes=" << fs::file_size(spqb) << ",spike_bytes=" << fs::file_size(spike) << '\n';
			return 0;
		}
		if (operation != "scan" || argc != 6) throw std::invalid_argument("invalid scan arguments");
		const std::string format = argv[3];
		const auto cache = static_cast<uint32_t>(std::stoul(argv[4]));
		const bool materialize = std::string(argv[5]) == "materialize";
		if (cache < 16 || cache > 16384) throw std::invalid_argument("cache pages must be 16..16384");
		if (!materialize && std::string(argv[5]) != "stream") throw std::invalid_argument("invalid scan mode");
		const auto start = clock_type::now();
		hasher hash;
		size_t count = 0;
		std::vector<bar> collected;
		auto consume = [&](const bar& b) { hash.feed(&b, sizeof b); ++count; };
		if (format == "spqb")
		{
			if (materialize)
			{
				bar_series series;
				std::string error;
				if (!read_bars(spqb.string(), series, &error)) throw std::runtime_error(error);
				if (series.generic) throw std::invalid_argument("storage comparison supports SPQB v1 only");
				collected = std::move(series.bars);
			}
			else
			{
				std::ifstream file(spqb, std::ios::binary);
				uint32_t version = 0;
				file.seekg(4); file.read(reinterpret_cast<char*>(&version), sizeof version);
				if (!file || version != 1) throw std::invalid_argument("storage comparison supports SPQB v1 only");
				uint32_t symbol_len = 0;
				file.seekg(24); file.read(reinterpret_cast<char*>(&symbol_len), sizeof symbol_len);
				if (!file || symbol_len > 64) throw std::runtime_error("invalid SPQB header");
				file.seekg(28 + symbol_len);
				bar buffer[1024];
				while (file.read(reinterpret_cast<char*>(buffer), sizeof buffer) || file.gcount())
				{
					if (file.gcount() % sizeof(bar)) throw std::runtime_error("truncated SPQB record");
					for (size_t i = 0; i < static_cast<size_t>(file.gcount()) / sizeof(bar); ++i) consume(buffer[i]);
				}
			}
		}
		else if (format == "spike")
		{
			auto db = open(spike, cache, SPIKEDB_OPEN_READONLY);
			uint64_t expected = 0;
			check(spike_db_count(db.get(), 1, &expected));
			if (materialize) collected.reserve(static_cast<size_t>(expected));
			std::unique_ptr<SpikeDB_Iter, decltype(&spike_db_iter_close)> it(spike_db_scan(db.get(), 1, 0, UINT64_MAX), spike_db_iter_close);
			if (!it) throw std::runtime_error("spike scan failed");
			SpikeDB_Rec records[1024];
			size_t seen = 0, n = 0;
			while ((n = spike_db_iter_next_batch(it.get(), records, 1024)) != 0)
				for (size_t i = 0; i < n; ++i)
				{
					if (records[i].len != sizeof(bar) - 8 || records[i].seq != 0)
						throw std::runtime_error("invalid spike bar");
					bar b{};
					b.time = static_cast<int64_t>(records[i].time);
					std::memcpy(reinterpret_cast<char*>(&b) + 8, records[i].value, sizeof(bar) - 8);
					if (materialize) collected.push_back(b); else consume(b);
					++seen;
				}
			if (seen != expected) throw std::runtime_error("incomplete spike scan");
		}
		else throw std::invalid_argument("format must be spqb or spike");
		for (const auto& b : collected) consume(b);
		std::cout << "format=" << format << ",mode=" << argv[5] << ",cache_pages=" << cache
			<< ",records=" << count << ",hash=" << hex64(hash.digest()) << ",ms=" << elapsed(start)
			<< ",peak_rss_bytes=" << peak_rss() << ",file_bytes=" << fs::file_size(format == "spqb" ? spqb : spike) << '\n';
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
