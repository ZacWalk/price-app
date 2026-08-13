#include "agents.h"

#include <algorithm>
#include <fstream>

namespace
{
	// Column indices into the observation, which is base features then the
	// agent's own position block.
	constexpr int TREND_FAST = 5;
	constexpr int RET_60 = 3;
	constexpr int VOL_RATIO = 7;

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
	const xfloat trend = state[TREND_FAST];
	if (trend > threshold_) return position_to_action(1);
	if (trend < -threshold_) return position_to_action(-1);

	// Inside the dead band, keep whatever we already hold.
	const int held = static_cast<int>(state[state.size() - 3]);
	return position_to_action(held);
}

tabular_q_agent::tabular_q_agent(const tabular_config& config, const int feature_columns, const uint64_t seed)
	: config_(config)
	, feature_columns_(feature_columns)
	, rng_(seed)
{
	const int states = 5 * 5 * 3 * 3;
	q_.assign(static_cast<size_t>(states) * ACTION_COUNT, 0.0);
	visits_.assign(states, 0);
}

int tabular_q_agent::discretise(const std::vector<xfloat>& state) const
{
	const int trend = bucket(state[TREND_FAST], TREND_EDGES, 4);
	const int ret = bucket(state[RET_60], RETURN_EDGES, 4);
	const int vol = bucket(state[VOL_RATIO], VOL_EDGES, 2);
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
	std::vector<int> sizes;
	sizes.push_back(state_size);
	for (const int h : config_.hidden) sizes.push_back(h);
	sizes.push_back(ACTION_COUNT);

	online_.build(sizes, activation::relu, seed);
	target_.build(sizes, activation::relu, seed);
	target_.copy_from(online_);

	states_.assign(static_cast<size_t>(config_.replay_capacity) * state_size, 0.0);
	next_states_.assign(static_cast<size_t>(config_.replay_capacity) * state_size, 0.0);
	actions_.assign(config_.replay_capacity, 0);
	rewards_.assign(config_.replay_capacity, 0.0);
	dones_.assign(config_.replay_capacity, 0);
}

int dqn_agent::argmax(const std::vector<xfloat>& q) const
{
	int best = 0;
	for (int a = 1; a < ACTION_COUNT; a += 1) if (q[a] > q[best]) best = a;
	return best;
}

xfloat dqn_agent::exploration() const
{
	return decayed(config_.epsilon_start, config_.epsilon_end, steps_, config_.epsilon_decay_steps);
}

int dqn_agent::act(const std::vector<xfloat>& state, const bool greedy)
{
	if (!greedy && rng_.uniform() < exploration()) return rng_.index(ACTION_COUNT);
	return argmax(online_.forward(state));
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
	std::vector<xfloat> state(state_size_);
	std::vector<xfloat> next(state_size_);
	std::vector<xfloat> grad(ACTION_COUNT);
	xfloat batch_loss = 0.0;

	for (int b = 0; b < config_.batch_size; b += 1)
	{
		const int i = rng_.index(stored_);
		const size_t base = static_cast<size_t>(i) * state_size_;
		std::copy(states_.begin() + base, states_.begin() + base + state_size_, state.begin());
		std::copy(next_states_.begin() + base, next_states_.begin() + base + state_size_, next.begin());

		xfloat target_value = rewards_[i];

		if (!dones_[i])
		{
			// Double DQN: the online net picks, the target net scores.
			const std::vector<xfloat> online_next = online_.forward(next);
			const int pick = argmax(online_next);
			const std::vector<xfloat> target_next = target_.forward(next);
			target_value += config_.discount * target_next[pick];
		}

		const std::vector<xfloat>& q = online_.forward(state);
		const int a = actions_[i];
		const xfloat error = q[a] - target_value;

		// Huber, so one violent bar cannot dominate the update.
		const xfloat d = config_.huber_delta;
		const xfloat g = clampx(error, -d, d);

		std::fill(grad.begin(), grad.end(), 0.0);
		grad[a] = g;
		online_.accumulate(grad);

		batch_loss += std::abs(error) <= d ? 0.5 * error * error : d * (std::abs(error) - 0.5 * d);
	}

	online_.step(config_.learning_rate);
	loss_ += (batch_loss / config_.batch_size - loss_) * 0.01;
}

std::unique_ptr<agent> make_agent(const std::string& name, const int state_size, const int feature_columns,
	const dqn_config& dqn, const tabular_config& tabular, const uint64_t seed)
{
	if (name == "flat") return std::make_unique<fixed_agent>(position_to_action(0), "flat");
	if (name == "long") return std::make_unique<fixed_agent>(position_to_action(1), "long");
	if (name == "short") return std::make_unique<fixed_agent>(position_to_action(-1), "short");
	if (name == "random") return std::make_unique<random_agent>(0.02, seed);
	if (name == "crossover") return std::make_unique<crossover_agent>(0.5);
	if (name == "qtable") return std::make_unique<tabular_q_agent>(tabular, feature_columns, seed);
	if (name == "dqn") return std::make_unique<dqn_agent>(dqn, state_size, seed);

	return nullptr;
}
