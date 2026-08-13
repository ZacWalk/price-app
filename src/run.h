#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "metrics.h"

// One experiment writes one directory of plain text plus the policy weights.
// Text first, so results stay diffable and readable without this program.

struct manifest_entry
{
	std::string key;
	std::string value;
};

class run_writer
{
public:
	bool open(const std::string& root, const std::string& id, std::string* error);

	void set(const std::string& key, const std::string& value);
	void set(const std::string& key, int value);
	void set(const std::string& key, int64_t value);
	void set(const std::string& key, xfloat value);

	bool write_manifest();
	bool write_equity(const trade_log& log);
	bool write_metrics_header();
	bool append_metrics(const std::string& variant, int seed, xfloat cost_bps, int fold, const metrics& m);
	bool append_learning(int episode, xfloat mean_reward, xfloat epsilon, xfloat loss);

	const std::string& directory() const { return directory_; }
	std::string path(const std::string& file) const { return directory_ + "/" + file; }

private:
	std::string directory_;
	std::vector<manifest_entry> entries_;
	bool metrics_started_ = false;
	bool learning_started_ = false;
};

struct run_summary
{
	std::string id;
	std::vector<manifest_entry> manifest;

	std::string get(const std::string& key) const;
};

bool read_manifest(const std::string& path, run_summary& summary, std::string* error);
std::vector<std::string> list_runs(const std::string& root);
std::string make_run_id(const std::string& agent, uint64_t seed);
bool ensure_directory(const std::string& path, std::string* error);
