#include "agents.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace
{
	constexpr xfloat TREND_EDGES[] = { -1.0, -0.3, 0.3, 1.0 };
	constexpr xfloat RETURN_EDGES[] = { -1.0, -0.3, 0.3, 1.0 };
	constexpr xfloat VOL_EDGES[] = { -0.3, 0.3 };

	int bucket(const xfloat value, const xfloat* edges, const int count)
	{
		int index = 0;
		while (index < count && value >= edges[index]) index += 1;
		return index;
	}

	xfloat decayed(const xfloat start, const xfloat end, const int64_t step, const int64_t over)
	{
		if (over <= 0) return end;
		const xfloat t = clampx(static_cast<xfloat>(step) / static_cast<xfloat>(over), 0.0, 1.0);
		return start + (end - start) * t;
	}

	// Epsilon is a probability. A decay over zero steps starts at the end rate;
	// a negative one means nothing.
	bool valid_exploration(const xfloat start, const xfloat end, const int64_t over)
	{
		return start >= 0.0 && start <= 1.0 && end >= 0.0 && end <= 1.0 && over >= 0;
	}

	// Each check names the flag to change; written so NaN fails too.
	void require(const bool valid, const char* learner, const char* rule)
	{
		if (!valid) throw std::invalid_argument(std::string("invalid ") + learner + " configuration: " + rule);
	}
}

int random_agent::act(const std::vector<xfloat>& state, bool)
{
	if (rng_.uniform() < flip_)
	{
		held_ = rng_.index(ACTION_COUNT);
	}
	return held_;
}

int crossover_agent::act(const std::vector<xfloat>& state, bool)
{
	const xfloat trend = state[trend_column_];
	if (trend > threshold_) return position_to_action(1);
	if (trend < -threshold_) return position_to_action(-1);

	// Inside the dead band, keep whatever we already hold.
	const int held = static_cast<int>(state[state.size() - 3]);
	return position_to_action(held);
}

int breakout_agent::act(const std::vector<xfloat>& state, bool)
{
	// Distance beyond the previous session's high, in previous-day ranges.
	const xfloat beyond = state[high_column_];
	if (beyond > threshold_) return position_to_action(1);

	const int held = static_cast<int>(state[state.size() - 3]);
	if (held > 0 && beyond > 0.0) return position_to_action(1);

	return position_to_action(0);
}

tabular_q_agent::tabular_q_agent(const tabular_config& config, const feature_matrix& features, const uint64_t seed)
	: config_(config)
	, feature_columns_(features.columns)
	, trend_column_(features.column("trend_fast"))
	, return_column_(features.column("ret_60"))
	, volatility_column_(features.column("vol_ratio"))
	, rng_(seed)
{
	// A rate outside (0, 1] or a discount above 1 diverges or freezes the table
	// instead of training it.
	require(config.learning_rate > 0.0 && config.learning_rate <= 1.0, "Q-table", "--q-lr must be in (0, 1]");
	require(config.discount >= 0.0 && config.discount <= 1.0, "Q-table", "--gamma must be in [0, 1]");
	require(valid_exploration(config.epsilon_start, config.epsilon_end, config.epsilon_decay_steps), "Q-table",
		"--epsilon-decay must be at least 0");
	require(config.reward_scale > 0.0 && std::isfinite(config.reward_scale), "Q-table", "the reward scale must be positive");
	const int states = 5 * 5 * 3 * 3;
	q_.assign(static_cast<size_t>(states) * ACTION_COUNT, 0.0);
	visits_.assign(states, 0);
}

int tabular_q_agent::discretise(const std::vector<xfloat>& state) const
{
	const int trend = bucket(state[trend_column_], TREND_EDGES, 4);
	const int ret = bucket(state[return_column_], RETURN_EDGES, 4);
	const int vol = bucket(state[volatility_column_], VOL_EDGES, 2);
	const int position = static_cast<int>(state[feature_columns_]) + 1;

	return ((trend * 5 + ret) * 3 + vol) * 3 + position;
}

int tabular_q_agent::best_action(const int index) const
{
	const xfloat* q = &q_[static_cast<size_t>(index) * ACTION_COUNT];
	int best = 0;
	for (int a = 1; a < ACTION_COUNT; a += 1) if (q[a] > q[best]) best = a;
	return best;
}

xfloat tabular_q_agent::exploration() const
{
	return decayed(config_.epsilon_start, config_.epsilon_end, steps_, config_.epsilon_decay_steps);
}

int tabular_q_agent::act(const std::vector<xfloat>& state, const bool greedy)
{
	const int index = discretise(state);
	if (!greedy && rng_.uniform() < exploration()) return rng_.index(ACTION_COUNT);
	return best_action(index);
}

void tabular_q_agent::observe(const std::vector<xfloat>& state, const int action, const xfloat reward,
	const std::vector<xfloat>& next, const bool done)
{
	const int index = discretise(state);
	const int next_index = discretise(next);

	visits_[index] += 1;
	steps_ += 1;

	const xfloat bootstrap = done ? 0.0 : config_.discount * q_[static_cast<size_t>(next_index) * ACTION_COUNT + best_action(next_index)];
	const xfloat target = reward * config_.reward_scale + bootstrap;
	xfloat& value = q_[static_cast<size_t>(index) * ACTION_COUNT + action];
	const xfloat error = target - value;

	value += config_.learning_rate * error;
	loss_ += (error * error - loss_) * 0.001;
}

int tabular_q_agent::visited_states() const
{
	int count = 0;
	for (const int32_t v : visits_) if (v > 0) count += 1;
	return count;
}

bool tabular_q_agent::save(const std::string& path) const
{
	std::ofstream file(path, std::ios::trunc);
	if (!file) return false;

	file << "state,visits,q_short,q_flat,q_long,policy\n";
	for (size_t s = 0; s < visits_.size(); s += 1)
	{
		const xfloat* q = &q_[s * ACTION_COUNT];
		file << s << ',' << visits_[s] << ','
			<< q[0] << ',' << q[1] << ',' << q[2] << ','
			<< action_name(best_action(static_cast<int>(s))) << '\n';
	}

	return true;
}

dqn_agent::dqn_agent(const dqn_config& config, const int state_size, const uint64_t seed)
	: config_(config)
	, state_size_(state_size)
	, rng_(seed)
{
	require(config.batch_size > 0, "DQN", "--batch must be at least 1");
	require(config.target_sync > 0, "DQN", "--target-sync must be at least 1");
	require(std::isfinite(config.learning_rate) && config.learning_rate > 0, "DQN", "--lr must be positive");
	require(std::isfinite(config.lr_decay) && config.lr_decay > 0, "DQN", "--lr-decay must be positive");
	require(config.discount >= 0 && config.discount <= 1, "DQN", "--gamma must be in [0, 1]");
	require(valid_exploration(config.epsilon_start, config.epsilon_end, config.epsilon_decay_steps), "DQN",
		"--epsilon-decay must be at least 0");
	require(config.switch_margin >= 0, "DQN", "--switch-margin must be at least 0");
	require(state_size > 0 && config.replay_capacity > 0 && config.replay_warmup > 0 &&
		config.replay_warmup <= config.replay_capacity && config.train_every > 0, "DQN", "bad replay settings");
	std::vector<int> sizes;
	sizes.push_back(state_size);
	for (const int h : config_.hidden) sizes.push_back(h);
	sizes.push_back(ACTION_COUNT);

	online_.build(sizes, seed);
	target_.build(sizes, seed);
	target_.copy_from(online_);
	online_.set_learning_rate(config_.learning_rate);

	states_.assign(static_cast<size_t>(config_.replay_capacity) * state_size, 0.0);
	next_states_.assign(static_cast<size_t>(config_.replay_capacity) * state_size, 0.0);
	actions_.assign(config_.replay_capacity, 0);
	rewards_.assign(config_.replay_capacity, 0.0);
	dones_.assign(config_.replay_capacity, 0);

	const size_t batch = static_cast<size_t>(config_.batch_size);
	batch_states_.assign(batch * state_size, 0.0);
	batch_next_.assign(batch * state_size, 0.0);
	batch_grad_.assign(batch * ACTION_COUNT, 0.0);
	batch_target_.assign(batch, 0.0);
	batch_index_.assign(batch, 0);
	batch_pick_.assign(batch, 0);
}

int dqn_agent::argmax(const xfloat* q) const
{
	int best = 0;
	for (int a = 1; a < ACTION_COUNT; a += 1) if (q[a] > q[best]) best = a;
	return best;
}

void dqn_agent::end_epoch()
{
	if (config_.lr_decay != 1.0) online_.scale_learning_rate(config_.lr_decay);
}

xfloat dqn_agent::exploration() const
{
	return decayed(config_.epsilon_start, config_.epsilon_end, steps_, config_.epsilon_decay_steps);
}

int dqn_agent::act(const std::vector<xfloat>& state, const bool greedy)
{
	if (!greedy && rng_.uniform() < exploration()) return rng_.index(ACTION_COUNT);

	const xfloat* q = online_.forward(state).data();
	const int best = argmax(q);
	if (config_.switch_margin <= 0.0 || state.size() < 3) return best;

	const int held = position_to_action(static_cast<int>(state[state.size() - 3]));
	if (held < 0 || held >= ACTION_COUNT || best == held) return best;

	const xfloat move = std::abs(static_cast<xfloat>(action_to_position(best) - action_to_position(held)));
	return q[best] - q[held] < config_.switch_margin * move ? held : best;
}

void dqn_agent::observe(const std::vector<xfloat>& state, const int action, const xfloat reward,
	const std::vector<xfloat>& next, const bool done)
{
	const size_t base = static_cast<size_t>(cursor_) * state_size_;
	std::copy(state.begin(), state.end(), states_.begin() + base);
	std::copy(next.begin(), next.end(), next_states_.begin() + base);
	actions_[cursor_] = static_cast<int32_t>(action);
	rewards_[cursor_] = reward * config_.reward_scale;
	dones_[cursor_] = done ? 1 : 0;

	cursor_ = (cursor_ + 1) % config_.replay_capacity;
	if (stored_ < config_.replay_capacity) stored_ += 1;

	steps_ += 1;

	if (stored_ >= config_.replay_warmup && steps_ % config_.train_every == 0) train_batch();
	if (steps_ % config_.target_sync == 0) target_.copy_from(online_);
}

void dqn_agent::train_batch()
{
	const int n = config_.batch_size;
	const size_t width = static_cast<size_t>(state_size_);

	for (int b = 0; b < n; b += 1)
	{
		const int i = rng_.index(stored_);
		batch_index_[b] = static_cast<int32_t>(i);

		const size_t src = static_cast<size_t>(i) * width;
		const size_t dst = static_cast<size_t>(b) * width;
		std::copy_n(states_.begin() + static_cast<ptrdiff_t>(src), width, batch_states_.begin() + static_cast<ptrdiff_t>(dst));
		std::copy_n(next_states_.begin() + static_cast<ptrdiff_t>(src), width, batch_next_.begin() + static_cast<ptrdiff_t>(dst));
	}

	// Double DQN: the online net picks the action, the target net scores it.
	// Weights do not move inside a batch, so the three passes can be grouped.
	online_.forward_batch(batch_next_.data(), n);
	for (int b = 0; b < n; b += 1) batch_pick_[b] = argmax(online_.output_row(b));

	target_.forward_batch(batch_next_.data(), n);
	for (int b = 0; b < n; b += 1)
	{
		const int i = batch_index_[b];
		batch_target_[b] = rewards_[i];
		if (!dones_[i]) batch_target_[b] += config_.discount * target_.output_row(b)[batch_pick_[b]];
	}

	online_.forward_batch(batch_states_.data(), n);

	std::fill(batch_grad_.begin(), batch_grad_.end(), 0.0);
	const xfloat d = config_.huber_delta;
	xfloat batch_loss = 0.0;

	for (int b = 0; b < n; b += 1)
	{
		const int a = actions_[batch_index_[b]];
		const xfloat error = online_.output_row(b)[a] - batch_target_[b];

		// Huber, so one violent bar cannot dominate the update.
		batch_grad_[static_cast<size_t>(b) * ACTION_COUNT + a] = clampx(error, -d, d);
		batch_loss += std::abs(error) <= d ? 0.5 * error * error : d * (std::abs(error) - 0.5 * d);
	}

	online_.accumulate_batch(batch_grad_.data(), n);
	online_.step();
	loss_ += (batch_loss / n - loss_) * 0.01;
}

int ensemble_agent::act(const std::vector<xfloat>& state, const bool greedy)
{
	int votes[ACTION_COUNT] = {};
	for (agent* member : members_) votes[member->act(state, greedy)] += 1;

	int best = 0;
	for (int a = 1; a < ACTION_COUNT; a += 1) if (votes[a] > votes[best]) best = a;

	int leaders = 0;
	for (const int v : votes) if (v == votes[best]) leaders += 1;

	// A split vote is not an opinion. Holding is what the members disagreed
	// about, so changing on a tie would manufacture turnover out of noise.
	if (leaders > 1) return position_to_action(static_cast<int>(state[state.size() - 3]));

	return best;
}

void ensemble_agent::begin_episode()
{
	for (agent* member : members_) member->begin_episode();
}

std::unique_ptr<agent> make_agent(const std::string& name, const feature_matrix& features,
	const dqn_config& dqn, const tabular_config& tabular, const uint64_t seed)
{
	if (name == "flat") return std::make_unique<fixed_agent>(position_to_action(0), "flat");
	if (name == "long") return std::make_unique<fixed_agent>(position_to_action(1), "long");
	if (name == "short") return std::make_unique<fixed_agent>(position_to_action(-1), "short");
	if (name == "random") return std::make_unique<random_agent>(0.02, seed);
	if (name == "crossover") return std::make_unique<crossover_agent>(0.5, features.column("trend_fast"));
	if (name == "breakout") return std::make_unique<breakout_agent>(0.0, features.column("pd_high_dist"));
	if (name == "qtable") return std::make_unique<tabular_q_agent>(tabular, features, seed);
	if (name == "dqn") return std::make_unique<dqn_agent>(dqn, features.columns + 3, seed);

	return nullptr;
}
