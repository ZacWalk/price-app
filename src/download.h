#pragma once

#include <cstddef>
#include <string>

struct download_config
{
	std::string symbol = "ETHUSDT";
	std::string market = "um";
	std::string kind = "trades";
	std::string period = "auto";
	std::string interval = "1m";
	std::string from = "2024-07-22";
	std::string to = "2024-07-26";
	std::string output;
	std::string fixtures;
	bool force = false;
	bool skip_missing = false;
	size_t max_archive_bytes = 256u * 1024 * 1024;
	size_t max_csv_bytes = 512u * 1024 * 1024;
	size_t max_rows = 10000000;
};

int download_binance(const download_config& config);
