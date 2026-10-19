#include "download.h"
#include "publication.h"

#include "binance/fg_binance.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace
{
	struct buffer
	{
		fg_buffer value{};
		~buffer() { fg_buffer_free(&value); }
	};
	struct client_deleter { void operator()(fg_client* client) const { fg_client_free(client); } };

	void check(fg_status status, const fg_error& error)
	{
		if (status != FG_OK)
			throw std::runtime_error(std::string("libcfindata ") + fg_status_name(status) + ": " + error.message);
	}

	std::chrono::year_month_day date(const std::string& value)
	{
		int y = 0, m = 0, d = 0;
		if (value.size() != 10 || value[4] != '-' || value[7] != '-')
			throw std::invalid_argument("dates must be YYYY-MM-DD");
		for (size_t i = 0; i < value.size(); ++i)
			if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9'))
				throw std::invalid_argument("dates must be YYYY-MM-DD");
		y = std::stoi(value.substr(0, 4));
		m = std::stoi(value.substr(5, 2));
		d = std::stoi(value.substr(8, 2));
		std::chrono::year_month_day result{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(m)},
			std::chrono::day{static_cast<unsigned>(d)}};
		if (y < 1970 || !result.ok()) throw std::invalid_argument("invalid archive date");
		return result;
	}

	fg_status fixture_get(void* context, const fg_request* request, fg_response* response, fg_error* error)
	{
		try
		{
			const std::string url(request->url.data, request->url.len);
			const auto filename = url.substr(url.find_last_of('/') + 1);
			if (filename.empty() || filename.find('\\') != std::string::npos || filename.find("..") != std::string::npos)
				return fg_fail(error, FG_INVALID, "unsafe fixture filename");
			const fs::path path = *static_cast<const fs::path*>(context) / filename;
			if (!fs::exists(path)) { response->status = 404; return FG_OK; }
			response->status = 200;
			const std::string text = path.string();
			return fg_read_file(FG_STR(text.c_str()), request->max_bytes, &response->body, error);
		}
		catch (const std::exception& exception)
		{
			return fg_fail(error, FG_IO, exception.what());
		}
	}

	fg_status count_row(void* context, const fg_binance_file*, const fg_binance_record*, fg_error*)
	{
		++*static_cast<size_t*>(context);
		return FG_OK;
	}

}

int download_binance(const download_config& config)
{
	const bool klines = config.kind == "klines";
	fg_binance_kind kind;
	if (klines) kind = FG_BINANCE_KLINES;
	else if (config.kind == "trades") kind = FG_BINANCE_TRADES;
	else if (config.kind == "aggTrades") kind = FG_BINANCE_AGG_TRADES;
	else if (config.kind == "fundingRate") kind = FG_BINANCE_FUNDING_RATE;
	else if (config.kind == "metrics") kind = FG_BINANCE_METRICS;
	else throw std::invalid_argument("unsupported Binance archive kind");
	const bool typed = klines || config.kind == "trades";
	const bool monthly = config.period == "monthly" ||
		(config.period == "auto" && (klines || config.kind == "fundingRate"));
	if (config.period != "auto" && config.period != "monthly" && config.period != "daily")
		throw std::invalid_argument("period must be auto, daily or monthly");
	if (config.market != "um" && config.market != "cm" && config.market != "spot")
		throw std::invalid_argument("market must be um, cm or spot");
	if (!config.max_archive_bytes || !config.max_csv_bytes || !config.max_rows)
		throw std::invalid_argument("download limits must be positive");
	const auto first = date(config.from), last = date(config.to);
	if (std::chrono::sys_days{last} < std::chrono::sys_days{first})
		throw std::invalid_argument("--to precedes --from");
	const auto archive_count = monthly
		? (int(last.year()) - int(first.year())) * 12 + int(unsigned(last.month())) - int(unsigned(first.month())) + 1
		: (std::chrono::sys_days{last} - std::chrono::sys_days{first}).count() + 1;
	if (archive_count > 3660) throw std::invalid_argument("archive window exceeds 3660 files");

	fs::path output = config.output;
	if (output.empty())
	{
		if (klines) output = fs::path("data") / "klines" / config.symbol / config.interval;
		else if (config.kind == "trades") output = fs::path("data") / "raw";
		else output = fs::path("data") / config.kind / config.symbol;
	}
	fs::path fixture_root = config.fixtures;
	fg_transport fixture{&fixture_root, fixture_get, nullptr};
	fg_client_options options{};
	options.user_agent = FG_STR("price-app/1.0");
	options.max_response_bytes = config.max_archive_bytes;
	options.timeout_ms = 600000;
	fg_client* raw = nullptr;
	fg_error error{};
	check(fg_client_create(config.fixtures.empty() ? nullptr : &fixture, &options, &raw, &error), error);
	std::unique_ptr<fg_client, client_deleter> client(raw);
	fg_binance_limits limits{};
	limits.max_archive_bytes = config.max_archive_bytes;
	limits.max_csv_bytes = config.max_csv_bytes;
	limits.max_rows = config.max_rows;

	auto current = monthly ? std::chrono::year_month_day{first.year(), first.month(), std::chrono::day{1}} : first;
	size_t files = 0, downloaded = 0, skipped = 0, missing = 0;
	while (std::chrono::sys_days{current} <= std::chrono::sys_days{last})
	{
		if (++files > 3660) throw std::invalid_argument("archive window exceeds 3660 files");
		char stamp[16];
		if (monthly) std::snprintf(stamp, sizeof stamp, "%04d-%02u", int(current.year()), unsigned(current.month()));
		else std::snprintf(stamp, sizeof stamp, "%04d-%02u-%02u", int(current.year()), unsigned(current.month()), unsigned(current.day()));
		fg_binance_file spec{};
		spec.market = config.market == "um" ? FG_BINANCE_UM : config.market == "cm" ? FG_BINANCE_CM : FG_BINANCE_SPOT;
		spec.kind = kind;
		spec.period = monthly ? FG_BINANCE_MONTHLY : FG_BINANCE_DAILY;
		spec.symbol = FG_STR(config.symbol.c_str());
		spec.interval = FG_STR(klines ? config.interval.c_str() : "");
		spec.date = FG_STR(stamp);
		fg_binance_names names{};
		check(fg_binance_build_names(&spec, &names, &error), error);
		const fs::path destination = output / names.csv_name;
		if (fs::exists(destination) && !config.force)
		{
			std::cout << names.csv_name << " already present\n";
			++skipped;
		}
		else
		{
			buffer csv;
			const auto status = fg_binance_download(client.get(), &spec, &limits, &csv.value, &error);
			if (status == FG_NOT_FOUND && config.skip_missing)
			{
				std::cout << names.zip_name << " unavailable (404)\n";
				++missing;
			}
			else
			{
				check(status, error);
				size_t rows = 0;
				if (typed)
					check(fg_binance_parse_csv(&spec, {csv.value.data, csv.value.len}, &limits, count_row, &rows, &error), error);
				fs::create_directories(output);
				publish_download(destination, csv.value.data, csv.value.len, config.force);
				std::cout << names.csv_name << " ";
				if (typed) std::cout << rows << " rows";
				else std::cout << csv.value.len << " raw CSV bytes (schema unchanged)";
				std::cout << ", SHA256 verified\n";
				++downloaded;
			}
		}
		current = monthly ? current + std::chrono::months{1}
			: std::chrono::year_month_day{std::chrono::sys_days{current} + std::chrono::days{1}};
	}
	std::cout << "downloaded " << downloaded << ", already had " << skipped << ", unavailable " << missing << '\n';
	return downloaded || skipped ? 0 : 2;
}
