#include "publication.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

struct barrier
{
	fs::path directory;
	publication_phase pause;
};

void observe(void* context, publication_phase phase)
{
	const auto& state = *static_cast<barrier*>(context);
	if (phase != state.pause) return;
	{ std::ofstream file(state.directory / "ready"); file << "ready\n"; }
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!fs::exists(state.directory / "release"))
	{
		if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("publication barrier timeout");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc != 5) throw std::invalid_argument("publication_probe destination payload barrier-directory phase");
		barrier state{argv[3], publication_phase::reserved};
		const std::string phase = argv[4], bytes = argv[2];
		if (phase == "staged") state.pause = publication_phase::staged;
		else if (phase == "backed_up") state.pause = publication_phase::backed_up;
		else if (phase != "reserved" && phase != "none") throw std::invalid_argument("invalid barrier phase");
		publish_download(argv[1], bytes.data(), bytes.size(), true, phase == "none" ? nullptr : observe, &state);
		std::cout << "published\n";
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
