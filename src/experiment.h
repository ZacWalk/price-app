#pragma once

#include <string>
#include <vector>

#include "agents.h"
#include "bars.h"
#include "env.h"
#include "features.h"
#include "metrics.h"

struct fold
{
	int train_start = 0;
	int train_end = 0;
	int test_start = 0;
	int test_end = 0;
};

struct experiment_config
{
	std::string bars_path;
	std::string agent = "dqn";
	std::string runs_root = "runs";

	int train_bars = 40000;
	int test_bars = 10000;
	int gap_bars = 2000;
	int episode_bars = 3600;
	int epochs = 2;
	int seeds = 3;
	uint64_t base_seed = 1;

	xfloat cost_bps = 5.0;
	xfloat hold_penalty = 0.0;
	std::vector<xfloat> cost_curve = { 1.0, 2.0, 5.0, 10.0 };

	dqn_config dqn;
	tabular_config tabular;
};

std::vector<fold> make_folds(const experiment_config& config, int first_bar, int last_bar);

// Trains where the agent learns, then evaluates greedily on the held-out window.
void train_on_fold(agent& learner, env& environment, const fold& f,
	const experiment_config& config, class run_writer* writer, int& episode_counter);

void evaluate(agent& learner, env& environment, const bar_series& series,
	int start, int end, trade_log& log);

int run_experiment(const experiment_config& config, std::string* error);
