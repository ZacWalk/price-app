#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "env.h"
#include "nn.h"

class agent
{
public:
	virtual ~agent() = default;

	virtual std::string name() const = 0;
	virtual int act(const std::vector<xfloat>& state, bool greedy) = 0;

	virtual void observe(const std::vector<xfloat>& /*state*/, int /*action*/, xfloat /*reward*/,
		const std::vector<xfloat>& /*next*/, bool /*done*/) {}

	virtual void begin_episode() {}
	virtual bool learns() const { return false; }
	virtual xfloat exploration() const { return 0.0; }
	virtual xfloat recent_loss() const { return 0.0; }
	virtual bool save(const std::string& /*path*/) const { return true; }
};

// Always takes the same side. The bar every learner has to clear.
class fixed_agent final : public agent
{
public:
	fixed_agent(const int action, std::string label) : action_(action), label_(std::move(label)) {}

	std::string name() const override { return label_; }
	int act(const std::vector<xfloat>&, bool) override { return action_; }

private:
	int action_;
	std::string label_;
};

// Trades at a chosen rate without an opinion: the real cost floor to beat.
class random_agent final : public agent
{
public:
	random_agent(const xfloat flip_probability, const uint64_t seed)
		: flip_(flip_probability), rng_(seed) {}

	std::string name() const override { return "random"; }
	int act(const std::vector<xfloat>& state, bool greedy) override;
	void begin_episode() override { held_ = 1; }

private:
	xfloat flip_;
	rng rng_;
	int held_ = 1;
};

// The rule agent rung: trade the fast/slow trend spread with a dead band.
class crossover_agent final : public agent
{
public:
	explicit crossover_agent(const xfloat threshold) : threshold_(threshold) {}

	std::string name() const override { return "crossover"; }
	int act(const std::vector<xfloat>& state, bool greedy) override;

private:
	xfloat threshold_;
};

struct tabular_config
{
	xfloat learning_rate = 0.1;
	xfloat discount = 0.99;
	xfloat epsilon_start = 1.0;
	xfloat epsilon_end = 0.05;
	int64_t epsilon_decay_steps = 200000;
	xfloat reward_scale = 10000.0;
};

// Small enough that the whole learned policy can be printed and read.
class tabular_q_agent final : public agent
{
public:
	tabular_q_agent(const tabular_config& config, int feature_columns, uint64_t seed);

	std::string name() const override { return "qtable"; }
	int act(const std::vector<xfloat>& state, bool greedy) override;
	void observe(const std::vector<xfloat>& state, int action, xfloat reward,
		const std::vector<xfloat>& next, bool done) override;

	bool learns() const override { return true; }
	xfloat exploration() const override;
	xfloat recent_loss() const override { return loss_; }
	bool save(const std::string& path) const override;

	int state_count() const { return static_cast<int>(q_.size() / ACTION_COUNT); }
	int visited_states() const;

private:
	int discretise(const std::vector<xfloat>& state) const;
	int best_action(int index) const;

	tabular_config config_;
	int feature_columns_;
	std::vector<xfloat> q_;
	std::vector<int32_t> visits_;
	rng rng_;
	int64_t steps_ = 0;
	xfloat loss_ = 0.0;
};

struct dqn_config
{
	std::vector<int> hidden = { 64, 64 };
	xfloat learning_rate = 3e-4;
	xfloat discount = 0.99;
	xfloat epsilon_start = 1.0;
	xfloat epsilon_end = 0.05;
	int64_t epsilon_decay_steps = 200000;
	xfloat reward_scale = 10000.0;   // rewards become basis points, so Q is O(1)
	int replay_capacity = 200000;
	int replay_warmup = 5000;
	int batch_size = 32;
	int train_every = 4;
	int target_sync = 2000;
	xfloat huber_delta = 1.0;
};

// Double DQN with experience replay and a target network.
class dqn_agent final : public agent
{
public:
	dqn_agent(const dqn_config& config, int state_size, uint64_t seed);

	std::string name() const override { return "dqn"; }
	int act(const std::vector<xfloat>& state, bool greedy) override;
	void observe(const std::vector<xfloat>& state, int action, xfloat reward,
		const std::vector<xfloat>& next, bool done) override;

	bool learns() const override { return true; }
	xfloat exploration() const override;
	xfloat recent_loss() const override { return loss_; }
	bool save(const std::string& path) const override { return online_.save(path); }

	int parameter_count() const { return online_.parameter_count(); }

private:
	void train_batch();
	int argmax(const std::vector<xfloat>& q) const;

	dqn_config config_;
	int state_size_;
	mlp online_;
	mlp target_;
	rng rng_;

	std::vector<xfloat> states_, next_states_;
	std::vector<int32_t> actions_;
	std::vector<xfloat> rewards_;
	std::vector<uint8_t> dones_;
	int stored_ = 0;
	int cursor_ = 0;

	int64_t steps_ = 0;
	xfloat loss_ = 0.0;
};

std::unique_ptr<agent> make_agent(const std::string& name, int state_size, int feature_columns,
	const dqn_config& dqn, const tabular_config& tabular, uint64_t seed);
