#include "nn.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace
{
	constexpr xfloat ADAM_BETA1 = 0.9;
	constexpr xfloat ADAM_BETA2 = 0.999;
	constexpr xfloat ADAM_EPS = 1e-8;
	constexpr xfloat GRAD_CLIP = 10.0;

	constexpr char MLP_MAGIC[4] = { 'S', 'P', 'Q', 'N' };
	constexpr uint32_t MLP_VERSION = 1;

	xfloat activate(const activation kind, const xfloat x)
	{
		switch (kind)
		{
		case activation::relu: return x > 0.0 ? x : 0.0;
		case activation::tanh_unit: return std::tanh(x);
		default: return x;
		}
	}

	xfloat activate_gradient(const activation kind, const xfloat z, const xfloat a)
	{
		switch (kind)
		{
		case activation::relu: return z > 0.0 ? 1.0 : 0.0;
		case activation::tanh_unit: return 1.0 - a * a;
		default: return 1.0;
		}
	}
}

void mlp::build(const std::vector<int>& sizes, const activation hidden, const uint64_t seed)
{
	layers_.clear();
	a_.clear();
	z_.clear();
	delta_.clear();

	if (sizes.size() < 2) return;

	hidden_ = hidden;
	accumulated_ = 0;
	adam_step_ = 0;

	rng r(seed);

	for (size_t i = 1; i < sizes.size(); i += 1)
	{
		layer l;
		l.in = sizes[i - 1];
		l.out = sizes[i];
		l.w.resize(static_cast<size_t>(l.in) * l.out);
		l.b.assign(l.out, 0.0);
		l.gw.assign(l.w.size(), 0.0);
		l.gb.assign(l.b.size(), 0.0);
		l.mw.assign(l.w.size(), 0.0);
		l.mb.assign(l.b.size(), 0.0);
		l.vw.assign(l.w.size(), 0.0);
		l.vb.assign(l.b.size(), 0.0);

		// He for ReLU, Xavier otherwise.
		const xfloat scale = hidden == activation::relu
			? std::sqrt(2.0 / l.in)
			: std::sqrt(1.0 / l.in);

		for (auto& w : l.w) w = r.normal() * scale;

		layers_.push_back(std::move(l));
	}

	a_.resize(sizes.size());
	z_.resize(sizes.size());
	delta_.resize(sizes.size());

	for (size_t i = 0; i < sizes.size(); i += 1)
	{
		a_[i].assign(sizes[i], 0.0);
		z_[i].assign(sizes[i], 0.0);
		delta_[i].assign(sizes[i], 0.0);
	}
}

const std::vector<xfloat>& mlp::forward(const std::vector<xfloat>& x)
{
	const size_t n = a_[0].size();
	for (size_t i = 0; i < n; i += 1) a_[0][i] = i < x.size() ? x[i] : 0.0;

	for (size_t li = 0; li < layers_.size(); li += 1)
	{
		const layer& l = layers_[li];
		const std::vector<xfloat>& in = a_[li];
		std::vector<xfloat>& z = z_[li + 1];
		std::vector<xfloat>& out = a_[li + 1];
		const bool last = li + 1 == layers_.size();

		for (int o = 0; o < l.out; o += 1)
		{
			const xfloat* w = &l.w[static_cast<size_t>(o) * l.in];
			xfloat sum = l.b[o];
			for (int i = 0; i < l.in; i += 1) sum += w[i] * in[i];

			z[o] = sum;
			out[o] = last ? sum : activate(hidden_, sum);
		}
	}

	return a_.back();
}

void mlp::accumulate(const std::vector<xfloat>& grad_output)
{
	if (layers_.empty()) return;

	const size_t top = layers_.size();
	for (size_t i = 0; i < delta_[top].size(); i += 1)
	{
		delta_[top][i] = i < grad_output.size() ? grad_output[i] : 0.0;
	}

	for (size_t li = top; li-- > 0;)
	{
		layer& l = layers_[li];
		const std::vector<xfloat>& in = a_[li];
		const std::vector<xfloat>& d = delta_[li + 1];

		for (int o = 0; o < l.out; o += 1)
		{
			const xfloat g = d[o];
			if (g == 0.0) continue;

			xfloat* gw = &l.gw[static_cast<size_t>(o) * l.in];
			for (int i = 0; i < l.in; i += 1) gw[i] += g * in[i];
			l.gb[o] += g;
		}

		if (li == 0) break;

		std::vector<xfloat>& dprev = delta_[li];
		for (int i = 0; i < l.in; i += 1)
		{
			xfloat sum = 0.0;
			for (int o = 0; o < l.out; o += 1) sum += l.w[static_cast<size_t>(o) * l.in + i] * d[o];
			dprev[i] = sum * activate_gradient(hidden_, z_[li][i], a_[li][i]);
		}
	}

	accumulated_ += 1;
}

void mlp::step(const xfloat learning_rate)
{
	if (accumulated_ == 0 || layers_.empty()) return;

	const xfloat inv = 1.0 / accumulated_;

	xfloat norm_sq = 0.0;
	for (const layer& l : layers_)
	{
		for (const xfloat g : l.gw) norm_sq += (g * inv) * (g * inv);
		for (const xfloat g : l.gb) norm_sq += (g * inv) * (g * inv);
	}

	const xfloat norm = std::sqrt(norm_sq);
	const xfloat clip = norm > GRAD_CLIP ? GRAD_CLIP / norm : 1.0;

	adam_step_ += 1;
	const xfloat bc1 = 1.0 - std::pow(ADAM_BETA1, static_cast<xfloat>(adam_step_));
	const xfloat bc2 = 1.0 - std::pow(ADAM_BETA2, static_cast<xfloat>(adam_step_));

	for (layer& l : layers_)
	{
		for (size_t i = 0; i < l.w.size(); i += 1)
		{
			const xfloat g = l.gw[i] * inv * clip;
			l.mw[i] = ADAM_BETA1 * l.mw[i] + (1.0 - ADAM_BETA1) * g;
			l.vw[i] = ADAM_BETA2 * l.vw[i] + (1.0 - ADAM_BETA2) * g * g;
			l.w[i] -= learning_rate * (l.mw[i] / bc1) / (std::sqrt(l.vw[i] / bc2) + ADAM_EPS);
			l.gw[i] = 0.0;
		}

		for (size_t i = 0; i < l.b.size(); i += 1)
		{
			const xfloat g = l.gb[i] * inv * clip;
			l.mb[i] = ADAM_BETA1 * l.mb[i] + (1.0 - ADAM_BETA1) * g;
			l.vb[i] = ADAM_BETA2 * l.vb[i] + (1.0 - ADAM_BETA2) * g * g;
			l.b[i] -= learning_rate * (l.mb[i] / bc1) / (std::sqrt(l.vb[i] / bc2) + ADAM_EPS);
			l.gb[i] = 0.0;
		}
	}

	accumulated_ = 0;
}

void mlp::copy_from(const mlp& src)
{
	if (layers_.size() != src.layers_.size()) return;

	for (size_t i = 0; i < layers_.size(); i += 1)
	{
		layers_[i].w = src.layers_[i].w;
		layers_[i].b = src.layers_[i].b;
	}
}

int mlp::parameter_count() const
{
	int total = 0;
	for (const layer& l : layers_) total += static_cast<int>(l.w.size() + l.b.size());
	return total;
}

bool mlp::save(const std::string& path) const
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) return false;

	const uint32_t version = MLP_VERSION;
	const uint32_t hidden = static_cast<uint32_t>(hidden_);
	const uint32_t count = static_cast<uint32_t>(layers_.size());

	file.write(MLP_MAGIC, 4);
	file.write(reinterpret_cast<const char*>(&version), sizeof version);
	file.write(reinterpret_cast<const char*>(&hidden), sizeof hidden);
	file.write(reinterpret_cast<const char*>(&count), sizeof count);

	for (const layer& l : layers_)
	{
		file.write(reinterpret_cast<const char*>(&l.in), sizeof l.in);
		file.write(reinterpret_cast<const char*>(&l.out), sizeof l.out);
		file.write(reinterpret_cast<const char*>(l.w.data()), l.w.size() * sizeof(xfloat));
		file.write(reinterpret_cast<const char*>(l.b.data()), l.b.size() * sizeof(xfloat));
	}

	return file.good();
}

bool mlp::load(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;

	char magic[4] = {};
	uint32_t version = 0;
	uint32_t hidden = 0;
	uint32_t count = 0;

	file.read(magic, 4);
	file.read(reinterpret_cast<char*>(&version), sizeof version);
	file.read(reinterpret_cast<char*>(&hidden), sizeof hidden);
	file.read(reinterpret_cast<char*>(&count), sizeof count);

	if (!file || memcmp(magic, MLP_MAGIC, 4) != 0 || version != MLP_VERSION || count == 0 || count > 64)
	{
		return false;
	}

	std::vector<int> sizes;
	std::vector<std::pair<std::vector<xfloat>, std::vector<xfloat>>> params;

	for (uint32_t i = 0; i < count; i += 1)
	{
		int in = 0;
		int out = 0;
		file.read(reinterpret_cast<char*>(&in), sizeof in);
		file.read(reinterpret_cast<char*>(&out), sizeof out);
		if (!file || in <= 0 || out <= 0 || in > 1 << 20 || out > 1 << 20) return false;

		std::vector<xfloat> w(static_cast<size_t>(in) * out);
		std::vector<xfloat> b(out);
		file.read(reinterpret_cast<char*>(w.data()), w.size() * sizeof(xfloat));
		file.read(reinterpret_cast<char*>(b.data()), b.size() * sizeof(xfloat));
		if (!file) return false;

		if (i == 0) sizes.push_back(in);
		sizes.push_back(out);
		params.emplace_back(std::move(w), std::move(b));
	}

	build(sizes, static_cast<activation>(hidden), 1);

	for (size_t i = 0; i < layers_.size(); i += 1)
	{
		layers_[i].w = std::move(params[i].first);
		layers_[i].b = std::move(params[i].second);
	}

	return true;
}
