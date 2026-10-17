#include "cmlp_adapter.h"

#include <cmath>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace
{
	void check(cmlp_status status)
	{
		if (status != CMLP_OK)
			throw std::runtime_error(std::string("libcmlp: ") + cmlp_status_string(status));
	}

	size_t elements(int samples, size_t width)
	{
		if (samples <= 0 || width == 0 || static_cast<size_t>(samples) > SIZE_MAX / width)
			throw std::invalid_argument("invalid network batch shape");
		return static_cast<size_t>(samples) * width;
	}
}

float dqn_network::checked_float(xfloat value)
{
	if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
		throw std::invalid_argument("network conversion requires finite binary32-range values");
	const float converted = static_cast<float>(value);
	if (value != 0 && converted == 0)
		throw std::invalid_argument("network conversion would underflow to zero");
	return converted;
}

uint32_t dqn_network::network_seed(uint64_t seed)
{
	// Mix both halves; zero must never request libcmlp's nondeterministic seed path.
	seed ^= seed >> 30;
	seed *= UINT64_C(0xbf58476d1ce4e5b9);
	seed ^= seed >> 27;
	seed *= UINT64_C(0x94d049bb133111eb);
	seed ^= seed >> 31;
	const auto result = static_cast<uint32_t>(seed ^ (seed >> 32));
	return result == 0 ? 1 : result;
}

void dqn_network::build(const std::vector<int>& sizes, uint64_t seed)
{
	cmlp_config config = cmlp_default_config();
	config.layers = sizes.data();
	config.layer_count = sizes.size();
	config.hidden_activation = CMLP_RELU;
	config.output_activation = CMLP_LINEAR;
	config.optimizer = CMLP_ADAM;
	config.learning_rate = 1e-3f;
	config.beta1 = 0.9f;
	config.beta2 = 0.999f;
	config.epsilon = 1e-8f;
	config.gradient_clip = 10.0f;
	config.seed = network_seed(seed);
	cmlp_model* raw = nullptr;
	check(cmlp_create(&config, &raw));
	model_.reset(raw);
	samples_ = 0;
	result_.clear();
}

const std::vector<xfloat>& dqn_network::forward(const std::vector<xfloat>& input)
{
	if (input.size() != cmlp_input_size(model_.get()))
		throw std::invalid_argument("network input width mismatch");
	forward_batch(input.data(), 1);
	return result_;
}

void dqn_network::forward_batch(const xfloat* input, int samples)
{
	samples_ = 0;
	if (!input) throw std::invalid_argument("null network input");
	inputs_.resize(elements(samples, cmlp_input_size(model_.get())));
	outputs_.resize(elements(samples, cmlp_output_size(model_.get())));
	for (size_t i = 0; i < inputs_.size(); ++i) inputs_[i] = checked_float(input[i]);
	check(cmlp_forward(model_.get(), inputs_.data(), samples, outputs_.data()));
	result_.assign(outputs_.begin(), outputs_.end());
	samples_ = samples;
}

const xfloat* dqn_network::output_row(int sample) const
{
	if (sample < 0 || sample >= samples_) throw std::out_of_range("network output row");
	return result_.data() + static_cast<size_t>(sample) * cmlp_output_size(model_.get());
}

void dqn_network::accumulate_batch(const xfloat* gradient, int samples)
{
	if (!gradient || samples != samples_ || samples <= 0)
		throw std::invalid_argument("network backward requires its retained forward batch");
	gradients_.resize(elements(samples, cmlp_output_size(model_.get())));
	for (size_t i = 0; i < gradients_.size(); ++i) gradients_[i] = checked_float(gradient[i]);
	check(cmlp_backward(model_.get(), gradients_.data(), samples));
	samples_ = 0;
}

void dqn_network::step() { check(cmlp_step(model_.get())); }
void dqn_network::copy_from(const dqn_network& source)
{
	check(cmlp_copy_from(model_.get(), source.model_.get()));
	samples_ = 0;
}
void dqn_network::set_learning_rate(xfloat rate)
{
	check(cmlp_set_learning_rate(model_.get(), checked_float(rate)));
}
void dqn_network::scale_learning_rate(xfloat factor)
{
	check(cmlp_scale_learning_rate(model_.get(), checked_float(factor)));
}
xfloat dqn_network::learning_rate() const { return cmlp_learning_rate(model_.get()); }
int dqn_network::parameter_count() const
{
	const size_t count = cmlp_parameter_count(model_.get());
	if (count > INT_MAX) throw std::overflow_error("network parameter count");
	return static_cast<int>(count);
}

bool dqn_network::save(const std::string& path) const
{
	// A new run owns a new checkpoint. Never truncate an existing/legacy checkpoint.
	if (std::filesystem::exists(path)) throw std::runtime_error("refusing to replace checkpoint: " + path);
	check(cmlp_save(model_.get(), path.c_str()));
	return true;
}

void dqn_network::load(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	char magic[4] = {};
	file.read(magic, sizeof magic);
	if (std::memcmp(magic, "SPQN", sizeof magic) == 0)
		throw std::runtime_error("legacy SPQN v1 binary64 checkpoint: use crypto-app 0a1bae3; no implicit migration");
	file.close();
	cmlp_model* raw = nullptr;
	check(cmlp_load(path.c_str(), &raw));
	model_.reset(raw);
	samples_ = 0;
	result_.clear();
}
