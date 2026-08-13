#pragma once

#include <string>
#include <vector>

#include "common.h"

enum class activation
{
	linear,
	relu,
	tanh_unit
};

// Fully connected network with real bias parameters, linear outputs and Adam.
// Outputs are unbounded because it exists to approximate Q-values.
class mlp
{
public:
	void build(const std::vector<int>& sizes, activation hidden, uint64_t seed);

	const std::vector<xfloat>& forward(const std::vector<xfloat>& x);

	// Adds one sample's gradients given dLoss/dOutput. Call after forward().
	void accumulate(const std::vector<xfloat>& grad_output);

	void step(xfloat learning_rate);
	void copy_from(const mlp& src);

	bool save(const std::string& path) const;
	bool load(const std::string& path);

	int input_size() const { return layers_.empty() ? 0 : layers_.front().in; }
	int output_size() const { return layers_.empty() ? 0 : layers_.back().out; }
	int parameter_count() const;
	bool built() const { return !layers_.empty(); }

	const std::vector<xfloat>& output() const { return a_.back(); }

private:
	struct layer
	{
		int in = 0;
		int out = 0;
		std::vector<xfloat> w, b;    // parameters
		std::vector<xfloat> gw, gb;  // accumulated gradients
		std::vector<xfloat> mw, mb;  // Adam first moment
		std::vector<xfloat> vw, vb;  // Adam second moment
	};

	std::vector<layer> layers_;
	std::vector<std::vector<xfloat>> a_;      // a_[0] is the input
	std::vector<std::vector<xfloat>> z_;
	std::vector<std::vector<xfloat>> delta_;
	activation hidden_ = activation::relu;
	int accumulated_ = 0;
	int64_t adam_step_ = 0;
};
