#pragma once

#include <cstddef>
#include <filesystem>

enum class publication_phase { reserved, staged, backed_up };
using publication_observer = void (*)(void*, publication_phase);

// Observer is an internal test seam; CLI callers never install one.
void publish_download(const std::filesystem::path& destination, const char* bytes,
	size_t size, bool force, publication_observer observer = nullptr, void* context = nullptr);
