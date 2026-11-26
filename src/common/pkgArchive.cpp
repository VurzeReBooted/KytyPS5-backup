#include "common/archiveReader.h"
#include "common/stringUtils.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>

#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace Common {
namespace {

class PackageReader final: public ArchiveReader {
public:
	explicit PackageReader(SDL_Process* process): m_process(process) {}
	~PackageReader() override {
		// Closing stdin releases the reader even when the mount is only used briefly.
		if (auto* input = SDL_GetProcessInput(m_process)) SDL_CloseIO(input);
		int status;
		SDL_WaitProcess(m_process, true, &status);
		SDL_DestroyProcess(m_process);
	}

	bool LoadIndex() {
		std::array<char, 4> magic {};
		uint32_t count = 0;
		if (!Receive(magic.data(), 4) || std::string_view(magic.data(), 4) != "KPR1" ||
		    !Receive(&count, 4) || count > (1u << 20)) return false;
		m_index.emplace("", Entry {0, 0, false});
		for (uint32_t id = 0; id < count; ++id) {
			uint32_t length;
			uint64_t size;
			if (!Receive(&length, 4) || length == 0 || length > 8192 || !Receive(&size, 8)) return false;
			std::string name(length, '\0');
			if (!Receive(name.data(), length) || !Insert(std::move(name), {id, size, true})) return false;
		}
		return true;
	}

	std::optional<Entry> Find(std::string_view path) override {
		auto it = m_index.find(path);
		return it == m_index.end() ? std::nullopt : std::optional(it->second);
	}

	std::vector<File::DirEntry> List(std::string_view path) override {
		auto it = m_children.find(path);
		return it == m_children.end() ? std::vector<File::DirEntry>{} : it->second;
	}

	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		// All supported Kyty hosts are little endian. No native struct padding goes on the wire.
		std::array<uint8_t, 16> request {};
		const auto file = static_cast<uint32_t>(id);
		std::memcpy(request.data(), &file, 4);
		std::memcpy(request.data() + 4, &offset, 8);
		std::memcpy(request.data() + 12, &size, 4);
		std::lock_guard lock(m_io);
		if (m_failed) return 0;
		m_failed = !Transfer(SDL_GetProcessInput(m_process), request.data(), request.size(), SDL_WriteIO) ||
		           !Receive(data, size);
		return m_failed ? 0 : size;
	}

private:
	bool Receive(void* data, size_t size) {
		return Transfer(SDL_GetProcessOutput(m_process), static_cast<uint8_t*>(data), size, SDL_ReadIO);
	}

	template<typename Operation>
	static bool Transfer(SDL_IOStream* stream, uint8_t* data, size_t size, Operation operation) {
		if (!stream) return false;
		while (size != 0) {
			const auto got = operation(stream, data, size);
			if (got == 0) {
				if (SDL_GetIOStatus(stream) != SDL_IO_STATUS_NOT_READY) return false;
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}
			data += got;
			size -= got;
		}
		return true;
	}

	bool Insert(std::string name, Entry entry) {
		for (;;) {
			auto [it, added] = m_index.emplace(name, entry);
			if (!added) return !entry.is_file && !it->second.is_file;
			const auto split = name.rfind('/');
			const auto parent = split == std::string::npos ? std::string{} : name.substr(0, split);
			m_children[parent].push_back({name.substr(split == std::string::npos ? 0 : split + 1), entry.is_file});
			if (parent.empty()) return true;
			name = parent;
			entry = {0, 0, false};
		}
	}

	SDL_Process* m_process;
	std::mutex m_io;
	bool m_failed = false;
	std::map<std::string, Entry, std::less<>> m_index;
	std::map<std::string, std::vector<File::DirEntry>, std::less<>> m_children;
};
} // namespace

std::shared_ptr<ArchiveReader> OpenPackageReader(const std::filesystem::path& path) {
	const char* base = SDL_GetBasePath();
	if (!base) return {};
#if defined(_WIN32)
	constexpr auto name = "kyty_pkg_reader.exe";
#else
	constexpr auto name = "kyty_pkg_reader";
#endif
	const auto helper = PathToString(PathFromUtf8(base) / name);
	const auto input = PathToString(path);
	const char* args[] = {helper.c_str(), input.c_str(), nullptr};
	auto* process = SDL_CreateProcess(args, true);
	if (!process) {
		std::fprintf(stderr, "Could not start %s: %s\n", helper.c_str(), SDL_GetError());
		return {};
	}
	auto reader = std::make_shared<PackageReader>(process);
	if (reader->LoadIndex()) return reader;
	// A malformed index may leave the child blocked writing to stdout.
	SDL_KillProcess(process, true);
	return {};
}
} // namespace Common
