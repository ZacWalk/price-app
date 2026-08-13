#include "common.h"

#include <cstdio>
#include <ctime>

std::string hex64(const uint64_t v)
{
	char buf[17];
	for (int i = 0; i < 16; i += 1)
	{
		buf[15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xf];
	}
	buf[16] = '\0';
	return buf;
}

std::vector<std::string> split(const std::string& s, const char sep)
{
	std::vector<std::string> out;
	size_t start = 0;

	while (true)
	{
		const size_t pos = s.find(sep, start);
		if (pos == std::string::npos)
		{
			out.emplace_back(s.substr(start));
			break;
		}
		out.emplace_back(s.substr(start, pos - start));
		start = pos + 1;
	}

	return out;
}

std::string trim(const std::string& s)
{
	size_t a = 0;
	size_t b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a += 1;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b -= 1;
	return s.substr(a, b - a);
}

bool parse_real(const std::string& s, xfloat& out)
{
	const std::string t = trim(s);
	if (t.empty()) return false;

	char* end = nullptr;
	const double v = strtod(t.c_str(), &end);
	if (end == t.c_str()) return false;

	out = v;
	return true;
}

int64_t parse_duration_ms(const std::string& s)
{
	const std::string t = trim(s);
	if (t.empty()) return 0;

	size_t digits = 0;
	while (digits < t.size() && t[digits] >= '0' && t[digits] <= '9') digits += 1;
	if (digits == 0) return 0;

	const int64_t value = strtoll(t.substr(0, digits).c_str(), nullptr, 10);
	const std::string unit = t.substr(digits);

	if (unit.empty() || unit == "ms") return value;
	if (unit == "s") return value * 1000;
	if (unit == "m") return value * 60 * 1000;
	if (unit == "h") return value * 60 * 60 * 1000;
	if (unit == "d") return value * 24 * 60 * 60 * 1000;

	return 0;
}

std::string format_time_ms(const int64_t ms)
{
	const time_t seconds = static_cast<time_t>(ms / 1000);
	tm parts = {};
	if (gmtime_s(&parts, &seconds) != 0) return "?";

	char buf[32];
	snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
		parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
		parts.tm_hour, parts.tm_min, parts.tm_sec);

	return buf;
}
