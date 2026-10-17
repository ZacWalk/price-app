#include "metrics.h"

#include <cmath>
#include <cstdio>

void trade_log::clear()
{
	time.clear();
	price.clear();
	position.clear();
	gross.clear();
	cost.clear();
	equity.clear();
	elapsed_ms.clear();
	market_return.clear();
	units.clear();
}

void trade_log::add(const int64_t t, const xfloat p, const int32_t pos, const xfloat g, const xfloat c,
	const int64_t elapsed, const xfloat underlying_return, const xfloat cost_units)
{
	const xfloat previous = equity.empty() ? 0.0 : equity.back();
	time.push_back(t);
	price.push_back(p);
	position.push_back(pos);
	gross.push_back(g);
	cost.push_back(c);
	equity.push_back(previous + g - c);
	elapsed_ms.push_back(elapsed);
	market_return.push_back(underlying_return);
	units.push_back(cost_units);
}

metrics compute_metrics(const trade_log& log, const int64_t interval_ms)
{
	metrics m;
	m.bars = static_cast<int64_t>(log.size());
	if (m.bars == 0) return m;

	xfloat sum = 0.0;
	xfloat sum_sq = 0.0;
	xfloat downside_sq = 0.0;
	int64_t downside_count = 0;
	int64_t long_bars = 0;
	int64_t short_bars = 0;
	xfloat turnover_total = 0.0;
	int32_t previous_position = 0;

	// Trades are runs of a constant non-zero position; pnl is measured per run.
	xfloat trade_pnl = 0.0;
	xfloat wins = 0.0;
	xfloat losses = 0.0;
	int32_t win_count = 0;
	int32_t loss_count = 0;

	const auto close_trade = [&]()
		{
			if (previous_position == 0) return;
			m.trades += 1;
			if (trade_pnl >= 0.0) { wins += trade_pnl; win_count += 1; }
			else { losses += -trade_pnl; loss_count += 1; }
			trade_pnl = 0.0;
		};

	for (size_t i = 0; i < log.size(); i += 1)
	{
		const xfloat net = log.gross[i] - log.cost[i];
		sum += net;
		sum_sq += net * net;
		if (net < 0.0) { downside_sq += net * net; downside_count += 1; }

		m.gross_return += log.gross[i];
		m.cost_paid += log.cost[i];
		m.cost_units += log.units[i];
		m.market_return += log.market_return[i];

		const int32_t position = log.position[i];
		if (position > 0) long_bars += 1;
		else if (position < 0) short_bars += 1;

		turnover_total += std::abs(static_cast<xfloat>(position - previous_position));

		if (position != previous_position) close_trade();
		previous_position = position;
		trade_pnl += net;
	}

	close_trade();

	m.net_return = log.equity.back();
	m.cost_fraction = m.gross_return > 0.0 ? m.cost_paid / m.gross_return : 0.0;
	m.turnover = turnover_total / m.bars;
	m.time_long = static_cast<xfloat>(long_bars) / m.bars;
	m.time_short = static_cast<xfloat>(short_bars) / m.bars;
	m.time_flat = 1.0 - m.time_long - m.time_short;

	const xfloat mean = sum / m.bars;
	const xfloat variance = m.bars > 1 ? (sum_sq - m.bars * mean * mean) / (m.bars - 1) : 0.0;
	const xfloat sd = variance > 0.0 ? std::sqrt(variance) : 0.0;

	xfloat elapsed = 0.0;
	for (const int64_t duration : log.elapsed_ms) elapsed += static_cast<xfloat>(duration);
	const xfloat year_ms = 365.25 * 24.0 * 60.0 * 60.0 * 1000.0;
	const xfloat bars_per_year = elapsed > 0.0
		? year_ms * static_cast<xfloat>(log.size()) / elapsed
		: (interval_ms > 0 ? year_ms / static_cast<xfloat>(interval_ms) : 0.0);

	m.annual_return = mean * bars_per_year;
	m.sharpe = sd > 1e-15 ? mean / sd * std::sqrt(bars_per_year) : 0.0;

	const xfloat downside_sd = downside_count > 0 ? std::sqrt(downside_sq / m.bars) : 0.0;
	m.sortino = downside_sd > 1e-15 ? mean / downside_sd * std::sqrt(bars_per_year) : 0.0;

	xfloat peak = log.equity[0];
	for (const xfloat e : log.equity)
	{
		if (e > peak) peak = e;
		const xfloat drawdown = peak - e;
		if (drawdown > m.max_drawdown) m.max_drawdown = drawdown;
	}

	const int32_t closed = win_count + loss_count;
	m.hit_rate = closed > 0 ? static_cast<xfloat>(win_count) / closed : 0.0;
	m.average_win = win_count > 0 ? wins / win_count : 0.0;
	m.average_loss = loss_count > 0 ? losses / loss_count : 0.0;

	return m;
}

xfloat breakeven_cost_bps(const metrics& m, const xfloat slippage_bps)
{
	if (!(m.gross_return > 0.0) || !(m.cost_units > 0.0)) return std::nan("");

	// Costs are linear in the units charged, so gross / units is the one-way
	// cost that consumes the whole gross; slippage takes its share first.
	const xfloat round_trip = 2.0 * 10000.0 * m.gross_return / m.cost_units - 2.0 * slippage_bps;
	return round_trip > 0.0 ? round_trip : std::nan("");
}

std::string metrics_header()
{
	return "bars,trades,net_return,gross_return,cost_paid,cost_fraction,"
		"annual_return,sharpe,sortino,max_drawdown,turnover,hit_rate,"
		"average_win,average_loss,time_long,time_short,time_flat";
}

std::string metrics_row(const metrics& m)
{
	char buf[512];
	snprintf(buf, sizeof buf,
		"%lld,%d,%.8f,%.8f,%.8f,%.4f,%.6f,%.4f,%.4f,%.8f,%.6f,%.4f,%.8f,%.8f,%.4f,%.4f,%.4f",
		static_cast<long long>(m.bars), m.trades, m.net_return, m.gross_return, m.cost_paid,
		m.cost_fraction, m.annual_return, m.sharpe, m.sortino, m.max_drawdown, m.turnover,
		m.hit_rate, m.average_win, m.average_loss, m.time_long, m.time_short, m.time_flat);
	return buf;
}
