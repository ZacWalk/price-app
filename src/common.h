#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using xfloat = double;

// Deterministic by design: the standard distributions are not specified
// bit-exactly, so anything a run's result depends on is written out here.
class rng
{
public:
	explicit rng(const uint64_t seed) : state_(seed + 0x9e3779b97f4a7c15ull) {}

	uint64_t next_u64()
	{
		state_ += 0x9e3779b97f4a7c15ull;
		uint64_t z = state_;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	xfloat uniform() { return static_cast<xfloat>(next_u64() >> 11) * (1.0 / 9007199254740992.0); }

	xfloat uniform(const xfloat lo, const xfloat hi) { return lo + (hi - lo) * uniform(); }

	xfloat normal()
	{
		if (has_spare_)
		{
			has_spare_ = false;
			return spare_;
		}

		xfloat u, v, s;
		do
		{
			u = uniform(-1.0, 1.0);
			v = uniform(-1.0, 1.0);
			s = u * u + v * v;
		} while (s >= 1.0 || s == 0.0);

		const xfloat f = std::sqrt(-2.0 * std::log(s) / s);
		spare_ = v * f;
		has_spare_ = true;
		return u * f;
	}

	int index(const int n) { return n <= 0 ? 0 : static_cast<int>(next_u64() % static_cast<uint64_t>(n)); }

private:
	uint64_t state_;
	xfloat spare_ = 0.0;
	bool has_spare_ = false;
};

inline xfloat clampx(const xfloat v, const xfloat lo, const xfloat hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

inline bool finitex(const xfloat v)
{
	return v == v && v > -1e300 && v < 1e300;
}

// FNV-1a. Used to fingerprint input data in the run manifest.
class hasher
{
public:
	void feed(const void* data, const size_t bytes)
	{
		const auto* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < bytes; i += 1)
		{
			h_ ^= p[i];
			h_ *= 0x100000001b3ull;
		}
	}

	void feed(const std::string& s) { feed(s.data(), s.size()); }

	uint64_t digest() const { return h_; }

private:
	uint64_t h_ = 0xcbf29ce484222325ull;
};

std::string hex64(uint64_t v);
std::vector<std::string> split(const std::string& s, char sep);
std::string trim(const std::string& s);
bool parse_real(const std::string& s, xfloat& out);

// "1s", "500ms", "5m", "1h" -> milliseconds. Returns 0 if unparseable.
int64_t parse_duration_ms(const std::string& s);

std::string format_time_ms(int64_t ms);

