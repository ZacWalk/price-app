#pragma once

#include <string>
#include <vector>

#include "common.h"

// Per-bar record of what the agent did and what it earned.
struct trade_log
{
	std::vector<int64_t> time;
	std::vector<xfloat> price;
	std::vector<int32_t> position;
	std::vector<xfloat> gross;    // log return earned this bar, before costs
	std::vector<xfloat> cost;
	std::vector<xfloat> equity;   // cumulative net log return

	void clear();
	void add(int64_t t, xfloat p, int32_t pos, xfloat g, xfloat c);
	size_t size() const { return time.size(); }
};

struct metrics
{
	int64_t bars = 0;
	int32_t trades = 0;
	xfloat net_return = 0.0;      // cumulative net log return
	xfloat gross_return = 0.0;
	xfloat cost_paid = 0.0;
	xfloat cost_fraction = 0.0;   // costs as a share of gross, when gross > 0
	xfloat annual_return = 0.0;
	xfloat sharpe = 0.0;
	xfloat sortino = 0.0;
	xfloat max_drawdown = 0.0;
	xfloat turnover = 0.0;        // mean |position change| per bar
	xfloat hit_rate = 0.0;
	xfloat average_win = 0.0;
	xfloat average_loss = 0.0;
	xfloat time_long = 0.0;
	xfloat time_short = 0.0;
	xfloat time_flat = 0.0;
};

metrics compute_metrics(const trade_log& log, int64_t interval_ms);

std::string metrics_header();
std::string metrics_row(const metrics& m);
