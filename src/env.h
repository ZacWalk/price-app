#pragma once

#include <vector>

#include "bars.h"
#include "common.h"
#include "features.h"

constexpr int ACTION_COUNT = 3;

// 0 short, 1 flat, 2 long.
inline int action_to_position(const int action) { return action - 1; }
inline int position_to_action(const int position) { return position + 1; }

const char* action_name(int action);

struct env_config
{
	xfloat cost_bps = 5.0;   // round trip, so one-way is half of this
	xfloat slippage_bps = 0.0;  // one-way, on top of cost; a fill is never at the close
	xfloat cost_activity = 0.0; // >0 scales cost by how thin the bar is, 1 is inverse-proportional
	xfloat hold_penalty = 0.0;
	int episode_bars = 3600;
};

// The decision at bar t is made from information up to bar t's close and earns
// the return from t to t+1. That ordering is the whole causality guarantee.
class env
{
public:
	env(const bar_series& series, const feature_matrix& features, const env_config& config);

	void reset(int start, int end);
	xfloat step(int action, bool& done);

	// Control: substitutes a different return stream, leaving costs intact.
	void set_return_override(const std::vector<xfloat>* returns) { override_ = returns; }

	const std::vector<xfloat>& state() const { return state_; }
	int state_size() const { return static_cast<int>(state_.size()); }
	int index() const { return t_; }
	int position() const { return position_; }
	xfloat last_cost() const { return last_cost_; }
	// Position units charged this step, scaled like the cost, liquidation included.
	xfloat last_units() const { return last_units_; }
	xfloat last_gross() const { return last_gross_; }
	xfloat cost_per_unit() const { return cost_per_unit_; }
	xfloat step_return(const int t) const { return override_ ? (*override_)[t] : returns_[t]; }

	int first_bar() const;
	int last_bar() const { return static_cast<int>(series_.bars.size()) - 2; }

private:
	void observe();

	const bar_series& series_;
	const feature_matrix& features_;
	env_config config_;

	std::vector<xfloat> returns_;   // log(close[t+1] / close[t])
	std::vector<xfloat> atr_;       // trailing, for scaling unrealised pnl
	std::vector<xfloat> cost_scale_; // per bar, 1.0 unless cost_activity is set
	const std::vector<xfloat>* override_ = nullptr;

	std::vector<xfloat> state_;
	int t_ = 0;
	int end_ = 0;
	int position_ = 0;
	int bars_held_ = 0;
	xfloat entry_price_ = 0.0;
	xfloat cost_per_unit_ = 0.0;
	xfloat last_cost_ = 0.0;
	xfloat last_units_ = 0.0;
	xfloat last_gross_ = 0.0;
};

std::vector<xfloat> shuffled_returns(const std::vector<xfloat>& source, uint64_t seed);
std::vector<xfloat> shuffled_returns(const std::vector<xfloat>& source, const bar_series& series, uint64_t seed);
