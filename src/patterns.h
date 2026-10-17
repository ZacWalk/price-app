#pragma once

#include <string>

#include "common.h"

struct patterns_config
{
	std::string bars_path;
	xfloat bucket_width = 0.25;   // level buckets, in previous-day ranges
	int shuffles = 200;
	int block = 0;                // permutation block, 0 means one day of bars
	xfloat cost_bps = 5.0;        // round trip, for the tradeable verdict
	uint64_t seed = 1;
};

// Measures whether calendar and previous-day levels carry forward return,
// before any of it is offered to an agent.
int run_patterns(const patterns_config& config, std::string* error);
