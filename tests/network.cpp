#include "cmlp_adapter.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;
int checks = 0;
void require(bool ok, const char* label)
{
	++checks;
	if (!ok) throw std::runtime_error(label);
}
template<class F> void rejects(F&& operation, const char* label)
{
	bool rejected = false;
	try { operation(); } catch (const std::exception&) { rejected = true; }
	require(rejected, label);
}

int main(int argc, char** argv)
{
	try
	{
		const auto root = fs::path(argc > 1 ? argv[1] : "build/test-artifacts") /
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		fs::create_directories(root);
		require(sizeof(xfloat) == 8, "experiment precision remains binary64");
		for (uint64_t seed : std::vector<uint64_t>{0, 1, 7, UINT64_C(0x100000007), UINT64_MAX})
		{
			require(dqn_network::network_seed(seed) != 0, "nonzero deterministic network seed");
			dqn_network a, b;
			a.build({2, 8, 3}, seed);
			b.build({2, 8, 3}, seed);
			require(a.forward({0.25, -2}) == b.forward({0.25, -2}), "same seed exact output");
		}
		require(dqn_network::network_seed(7) != dqn_network::network_seed(0x100000007ull), "seed upper half used");
		for (double invalid : {std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max(),
			std::numeric_limits<double>::denorm_min()})
			rejects([&] { dqn_network::checked_float(invalid); }, "checked float conversion");
		require(dqn_network::checked_float(1.25) == 1.25f, "exact representable conversion");
		dqn_network model, target;
		model.build({2, 8, 3}, 7);
		target.build({2, 8, 3}, 9);
		target.copy_from(model);
		const std::vector<double> input{0.25, -2};
		const auto initial = model.forward(input);
		require(initial == target.forward(input), "target copy");
		rejects([&] { model.forward({1}); }, "input width");
		rejects([&] { model.forward_batch(nullptr, 1); }, "null input");
		rejects([&] { model.forward_batch(input.data(), 0); }, "empty batch");
		rejects([&] { model.output_row(0); }, "failed forward invalidates output");
		rejects([&] { model.set_learning_rate(-1); }, "negative learning rate");
		rejects([&] { model.scale_learning_rate(0); }, "zero decay");
		dqn_network other;
		other.build({2, 9, 3}, 7);
		rejects([&] { other.copy_from(model); }, "shape mismatch copy");

		int layers[] = {2, 8, 3};
		cmlp_config reference_config = cmlp_default_config();
		reference_config.layers = layers; reference_config.layer_count = 3;
		reference_config.hidden_activation = CMLP_RELU;
		reference_config.output_activation = CMLP_LINEAR;
		reference_config.optimizer = CMLP_ADAM;
		reference_config.learning_rate = 1e-3f;
		reference_config.gradient_clip = 10.0f;
		reference_config.seed = dqn_network::network_seed(7);
		cmlp_model* raw = nullptr;
		require(cmlp_create(&reference_config, &raw) == CMLP_OK, "reference create");
		std::unique_ptr<cmlp_model, decltype(&cmlp_destroy)> reference(raw, cmlp_destroy);
		std::vector<double> batch{0.25, -2, 4, 1}, gradient{100, -500, 800, 1, -300, 2000};
		std::vector<float> batch32;
		for (const auto value : batch) batch32.push_back(dqn_network::checked_float(value));
		const auto large_gradient = gradient;
		std::vector<float> gradient32(gradient.size());
		for (int step = 0; step < 5; ++step)
		{
			// Alternating clipped/unclipped updates defeats Adam's constant-scale invariance.
			for (size_t i = 0; i < gradient.size(); ++i)
			{
				gradient[i] = large_gradient[i] * (step % 2 ? 0.00001 : 1.0);
				gradient32[i] = dqn_network::checked_float(gradient[i]);
			}
			model.forward_batch(batch.data(), 2);
			float output[6];
			require(cmlp_forward(reference.get(), batch32.data(), 2, output) == CMLP_OK, "reference forward");
			for (int i = 0; i < 6; ++i)
				require(model.output_row(i / 3)[i % 3] == output[i], "exact C API batch/config parity");
			model.accumulate_batch(gradient.data(), 2);
			require(cmlp_backward(reference.get(), gradient32.data(), 2) == CMLP_OK, "reference backward");
			model.step();
			require(cmlp_step(reference.get()) == CMLP_OK, "reference Adam clip10");
			model.scale_learning_rate(0.95);
			require(cmlp_scale_learning_rate(reference.get(), 0.95f) == CMLP_OK, "reference decay");
			require(model.learning_rate() == cmlp_learning_rate(reference.get()), "learning rate parity");
		}
		require(model.forward(input) != initial, "weights really update");
		require(target.forward(input) == initial, "target frozen until sync");
		target.copy_from(model);
		require(target.forward(input) == model.forward(input), "updated target copy");
		rejects([&] { model.accumulate_batch(gradient.data(), 2); }, "backward sample mismatch");
		const auto checkpoint = (root / "weights.bin").string();
		require(model.save(checkpoint), "checkpoint save");
		dqn_network loaded;
		loaded.load(checkpoint);
		require(loaded.forward(input) == model.forward(input), "checkpoint roundtrip");
		require(loaded.learning_rate() == model.learning_rate(), "checkpoint optimizer config");
		rejects([&] { model.save(checkpoint); }, "never overwrite checkpoints");
		for (auto* value : {&loaded, &model})
		{
			value->forward_batch(batch.data(), 2);
			value->accumulate_batch(gradient.data(), 2);
			value->step();
		}
		require(loaded.forward(input) == model.forward(input), "checkpoint Adam state resumes exactly");
		const auto legacy = (root / "legacy.bin").string();
		{ std::ofstream file(legacy, std::ios::binary); file << "SPQN\1\0\0\0"; }
		const auto before = loaded.forward(input);
		rejects([&] { loaded.load(legacy); }, "legacy checkpoint explicit rejection");
		require(loaded.forward(input) == before && fs::file_size(legacy) == 5, "legacy file/model unchanged");
		const auto corrupt = (root / "corrupt.bin").string();
		fs::copy_file(checkpoint, corrupt);
		{ std::fstream file(corrupt, std::ios::binary | std::ios::in | std::ios::out); file.put('X'); }
		rejects([&] { loaded.load(corrupt); }, "corrupt checkpoint rejection");
		require(loaded.forward(input) == before, "failed load transactional");
		std::cout << checks << " network checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
