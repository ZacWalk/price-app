#pragma once

#include <memory>
#include <string>
#include <vector>

#include <cmlp.h>
#include "common.h"

inline constexpr const char* DQN_NUMERICS = "cmlp-f32-scalar-he-splitmix64-v1";

// The experiment remains binary64. Only network I/O crosses this checked boundary.
class dqn_network
{
public:
	dqn_network() = default;
	dqn_network(const dqn_network&) = delete;
	dqn_network& operator=(const dqn_network&) = delete;
	dqn_network(dqn_network&&) noexcept = default;
	dqn_network& operator=(dqn_network&&) noexcept = default;

	void build(const std::vector<int>& sizes, uint64_t seed);
	const std::vector<xfloat>& forward(const std::vector<xfloat>& input);
	void forward_batch(const xfloat* input, int samples);
	const xfloat* output_row(int sample) const;
	void accumulate_batch(const xfloat* gradient, int samples);
	void step();
	void copy_from(const dqn_network& source);
	void set_learning_rate(xfloat rate);
	void scale_learning_rate(xfloat factor);
	xfloat learning_rate() const;
	int parameter_count() const;
	bool save(const std::string& path) const;
	void load(const std::string& path);

	static uint32_t network_seed(uint64_t seed);
	static float checked_float(xfloat value);

private:
	struct deleter { void operator()(cmlp_model* value) const { cmlp_destroy(value); } };
	std::unique_ptr<cmlp_model, deleter> model_;
	std::vector<float> inputs_, outputs_, gradients_;
	std::vector<xfloat> result_;
	int samples_ = 0;
};
