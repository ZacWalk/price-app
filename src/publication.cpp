#include "publication.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
	[[noreturn]] void io_error(const char* operation)
	{
#ifdef _WIN32
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
#else
		throw std::system_error(errno, std::generic_category(), operation);
#endif
	}

	class native_file
	{
	public:
		native_file(const fs::path& path, bool reservation)
		{
#ifdef _WIN32
			value_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				reservation ? OPEN_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (value_ == INVALID_HANDLE_VALUE) io_error(reservation ? "destination reservation unavailable" : "exclusive stage creation failed");
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(value_, &info) ||
				(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
			{
				close();
				throw std::runtime_error("publication files must be regular files, not links");
			}
#else
			value_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
				(reservation ? 0 : O_EXCL), 0600);
			if (value_ < 0) io_error(reservation ? "destination reservation unavailable" : "exclusive stage creation failed");
			struct stat info{};
			if (::fstat(value_, &info) != 0 || !S_ISREG(info.st_mode))
			{
				close();
				throw std::runtime_error("publication files must be regular files, not links");
			}
			if (reservation && ::flock(value_, LOCK_EX | LOCK_NB) != 0)
			{
				const int error = errno;
				close();
				errno = error;
				io_error("destination reservation unavailable");
			}
#endif
		}
		~native_file() { close(); }
		native_file(const native_file&) = delete;
		native_file& operator=(const native_file&) = delete;

		void write(const char* bytes, size_t size)
		{
			while (size)
			{
				const auto chunk = static_cast<unsigned>(std::min(size, static_cast<size_t>(INT_MAX)));
#ifdef _WIN32
				DWORD written = 0;
				if (!WriteFile(value_, bytes, chunk, &written, nullptr)) io_error("stage write failed");
#else
				const auto written = ::write(value_, bytes, chunk);
				if (written < 0)
				{
					if (errno == EINTR) continue;
					io_error("stage write failed");
				}
#endif
				if (!written) throw std::runtime_error("stage write made no progress");
				bytes += written;
				size -= static_cast<size_t>(written);
			}
#ifdef _WIN32
			if (!FlushFileBuffers(value_)) io_error("stage flush failed");
#else
			if (::fsync(value_) != 0) io_error("stage flush failed");
#endif
		}

	private:
		void close() noexcept
		{
#ifdef _WIN32
			if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
			value_ = INVALID_HANDLE_VALUE;
#else
			if (value_ >= 0) ::close(value_);
			value_ = -1;
#endif
		}
#ifdef _WIN32
		HANDLE value_ = INVALID_HANDLE_VALUE;
#else
		int value_ = -1;
#endif
	};

	void move_no_replace(const fs::path& source, const fs::path& destination)
	{
#ifdef _WIN32
		if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
			io_error("no-replace publication move failed");
#else
		// Atomic destination creation: unlike POSIX rename, link never replaces
		// an existing entry. A crash between link/unlink only leaves both names.
		if (::link(source.c_str(), destination.c_str()) != 0)
			io_error("no-replace publication link failed");
		if (::unlink(source.c_str()) != 0)
			io_error("publication source unlink failed (both names preserved)");
#endif
	}

	bool occupied(const fs::path& path)
	{
		return fs::symlink_status(path).type() != fs::file_type::not_found;
	}
}

void publish_download(const fs::path& destination, const char* bytes, size_t size,
	bool force, publication_observer observer, void* context)
{
	if (!bytes && size) throw std::invalid_argument("null publication data");
	const fs::path path = fs::canonical(destination.parent_path()) / destination.filename();
	const fs::path stage = path.string() + ".part";
	const fs::path previous = path.string() + ".previous";
	// Never unlink this inode: persistent lock names prevent lock-splitting races.
	native_file reservation(path.string() + ".lock", true);
	const auto notify = [&](publication_phase phase) { if (observer) observer(context, phase); };
	notify(publication_phase::reserved);

	const bool replacing = occupied(path);
	if (replacing && !force) throw std::runtime_error("output appeared during download; refusing to replace it");
	if (replacing && !fs::is_regular_file(fs::symlink_status(path)))
		throw std::runtime_error("refusing to replace a non-regular destination");
	if (occupied(previous)) throw std::runtime_error("preserving existing .previous file; inspect it before retrying");
	{
		native_file file(stage, false);
		file.write(bytes, size);
	}
	notify(publication_phase::staged);
	if (replacing) move_no_replace(path, previous);
	notify(publication_phase::backed_up);
	try
	{
		move_no_replace(stage, path);
	}
	catch (const std::system_error& error)
	{
		throw std::runtime_error(std::string(error.what()) + "; staged data retained at " +
			stage.string() + (replacing ? "; original retained at " + previous.string() : ""));
	}
}
