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
	std::vector<int64_t> elapsed_ms;
	std::vector<xfloat> market_return;
	std::vector<xfloat> units;    // position change charged, in cost-scaled units

	void clear();
	void add(int64_t t, xfloat p, int32_t pos, xfloat g, xfloat c,
		int64_t elapsed = 0, xfloat underlying_return = 0.0, xfloat cost_units = 0.0);
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
	xfloat cost_units = 0.0;      // console only; metrics.csv keeps its columns
	xfloat market_return = 0.0;   // summed underlying log return over the scored bars
};

metrics compute_metrics(const trade_log& log, int64_t interval_ms);

// Round-trip cost in bps at which net would be zero, for the same one-way
// slippage. NaN when no nonnegative cost pays: gross not positive, or no trades.
xfloat breakeven_cost_bps(const metrics& m, xfloat slippage_bps);

std::string metrics_header();
std::string metrics_row(const metrics& m);
