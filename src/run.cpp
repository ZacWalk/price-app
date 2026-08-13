#include "run.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

bool ensure_directory(const std::string& path, std::string* error)
{
	std::error_code code;
	fs::create_directories(fs::path(path), code);

	if (code && !fs::exists(fs::path(path)))
	{
		if (error) *error = "cannot create " + path + ": " + code.message();
		return false;
	}

	return true;
}

std::string make_run_id(const std::string& agent, const uint64_t seed)
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

	char buf[128];
	snprintf(buf, sizeof buf, "%s-%s", agent.c_str(), format_time_ms(ms).c_str());

	std::string id = buf;
	std::replace(id.begin(), id.end(), ' ', '-');
	id.erase(std::remove(id.begin(), id.end(), ':'), id.end());
	id += "-s" + std::to_string(seed);

	return id;
}

bool run_writer::open(const std::string& root, const std::string& id, std::string* error)
{
	directory_ = root + "/" + id;
	return ensure_directory(directory_, error);
}

void run_writer::set(const std::string& key, const std::string& value)
{
	for (manifest_entry& e : entries_)
	{
		if (e.key == key)
		{
			e.value = value;
			return;
		}
	}
	entries_.push_back({ key, value });
}

void run_writer::set(const std::string& key, const int64_t value)
{
	set(key, std::to_string(value));
}

void run_writer::set(const std::string& key, const int value)
{
	set(key, std::to_string(value));
}

void run_writer::set(const std::string& key, const xfloat value)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%.10g", value);
	set(key, std::string(buf));
}

bool run_writer::write_manifest()
{
	std::ofstream file(path("manifest.txt"), std::ios::trunc);
	if (!file) return false;

	for (const manifest_entry& e : entries_) file << e.key << '=' << e.value << '\n';
	return true;
}

bool run_writer::write_equity(const trade_log& log)
{
	std::ofstream file(path("equity.csv"), std::ios::trunc);
	if (!file) return false;

	file << "time,price,position,gross,cost,equity\n";
	for (size_t i = 0; i < log.size(); i += 1)
	{
		char buf[192];
		snprintf(buf, sizeof buf, "%lld,%.8f,%d,%.10f,%.10f,%.10f",
			static_cast<long long>(log.time[i]), log.price[i], log.position[i],
			log.gross[i], log.cost[i], log.equity[i]);
		file << buf << '\n';
	}

	return true;
}

bool run_writer::write_metrics_header()
{
	std::ofstream file(path("metrics.csv"), std::ios::trunc);
	if (!file) return false;

	file << "variant,seed,cost_bps,fold," << metrics_header() << '\n';
	metrics_started_ = true;
	return true;
}

bool run_writer::append_metrics(const std::string& variant, const int seed, const xfloat cost_bps,
	const int fold, const metrics& m)
{
	if (!metrics_started_ && !write_metrics_header()) return false;

	std::ofstream file(path("metrics.csv"), std::ios::app);
	if (!file) return false;

	char prefix[128];
	snprintf(prefix, sizeof prefix, "%s,%d,%.2f,%d,", variant.c_str(), seed, cost_bps, fold);
	file << prefix << metrics_row(m) << '\n';
	return true;
}

bool run_writer::append_learning(const int episode, const xfloat mean_reward, const xfloat epsilon, const xfloat loss)
{
	const bool first = !learning_started_;
	std::ofstream file(path("learning.csv"), first ? std::ios::trunc : std::ios::app);
	if (!file) return false;

	if (first)
	{
		file << "episode,mean_reward,epsilon,loss\n";
		learning_started_ = true;
	}

	char buf[128];
	snprintf(buf, sizeof buf, "%d,%.10f,%.6f,%.10f", episode, mean_reward, epsilon, loss);
	file << buf << '\n';
	return true;
}

std::string run_summary::get(const std::string& key) const
{
	for (const manifest_entry& e : manifest) if (e.key == key) return e.value;
	return {};
}

bool read_manifest(const std::string& path, run_summary& summary, std::string* error)
{
	std::ifstream file(path);
	if (!file)
	{
		if (error) *error = "cannot open " + path;
		return false;
	}

	std::string line;
	while (std::getline(file, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		summary.manifest.push_back({ trim(line.substr(0, eq)), trim(line.substr(eq + 1)) });
	}

	return true;
}

std::vector<std::string> list_runs(const std::string& root)
{
	std::vector<std::string> out;
	std::error_code code;

	for (const auto& entry : fs::directory_iterator(fs::path(root), code))
	{
		if (!entry.is_directory()) continue;
		if (!fs::exists(entry.path() / "manifest.txt")) continue;
		out.push_back(entry.path().filename().string());
	}

	std::sort(out.begin(), out.end());
	return out;
}
