#include "common/package.h"

#include <array>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace Common::Package {
namespace fs = std::filesystem;

bool IsFilename(const fs::path& path) {
	auto ext = path.extension().u8string();
	for (auto& c: ext) {
		if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
	}
	return ext == u8".pkg";
}

namespace {
uint32_t Big32(const unsigned char* p) {
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Little64(const unsigned char* p) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; --i) value = value << 8 | p[i];
	return value;
}

struct Header {
	Kind kind = Kind::Invalid;
	uint64_t cnt = 0;
	uint64_t size = 0;
	uint32_t count = 0;
	uint32_t table = 0;
};

Header ReadHeader(std::ifstream& file) {
	std::array<unsigned char, 256> data {};
	if (!file.read(reinterpret_cast<char*>(data.data()), data.size()) ||
	    Big32(data.data()) != 0x7f464948) return {};
	const auto flag = data[5];
	const auto image = Little64(data.data() + 0x10);
	const auto length = Little64(data.data() + 0x18);
	const auto cnt = Little64(data.data() + 0x58);
	const auto end = file.seekg(0, std::ios::end).tellg();
	if (end < 0) return {};
	const uint64_t size = static_cast<uint64_t>(end);
	if (image < data.size() || image > size || !length || length > size - image ||
	    cnt < image + length || cnt > size || size - cnt < 128) return {};
	file.seekg(static_cast<std::streamoff>(cnt));
	if (!file.read(reinterpret_cast<char*>(data.data()), 128) || Big32(data.data()) != 0x7f434e54) return {};
	const auto count = Big32(data.data() + 16);
	const auto table = Big32(data.data() + 24);
	if (!count || count > 4096 || table < 128 || table > size - cnt || count * 32 > size - cnt - table) return {};
	return {flag == 0 ? Kind::Debug : flag == 0x80 ? Kind::Retail : Kind::Invalid, cnt, size, count, table};
}

std::string Identity(const fs::path& path) {
	const auto name = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(name.data()), name.size()) + "\n" +
	       std::to_string(fs::file_size(path)) + "\n" +
	       std::to_string(fs::last_write_time(path).time_since_epoch().count());
}

bool Ready(const fs::path& target, const std::string& identity) {
	std::ifstream marker(target / "package.id", std::ios::binary);
	std::string stored(identity.size(), '\0');
	return marker.read(stored.data(), stored.size()) && stored == identity &&
	       marker.peek() == std::ifstream::traits_type::eof() && fs::is_regular_file(target / "app0/eboot.bin");
}
} // namespace

Kind Inspect(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	return ReadHeader(file).kind;
}

std::vector<char> ReadMetadata(const fs::path& path, std::string_view member, uint64_t limit) {
	uint32_t id = 0;
	if (member == "sce_sys/param.json") id = 0x2000;
	else if (member == "sce_sys/icon0.png") id = 0x1200;
	else if (member == "sce_sys/pic0.png") id = 0x1220;
	else if (member == "sce_sys/snd0.at9") id = 0x1240;
	else return {};
	std::ifstream file(path, std::ios::binary);
	const auto h = ReadHeader(file);
	if (h.kind == Kind::Invalid) return {};
	file.seekg(static_cast<std::streamoff>(h.cnt + h.table));
	std::array<unsigned char, 32> entry {};
	for (uint32_t i = 0; i < h.count && file.read(reinterpret_cast<char*>(entry.data()), entry.size()); ++i) {
		if (Big32(entry.data()) != id || (entry[8] & 0x80)) continue;
		const auto start = Big32(entry.data() + 16);
		const auto size = Big32(entry.data() + 20);
		if (size > limit || start > h.size - h.cnt || size > h.size - h.cnt - start) return {};
		std::vector<char> result(size);
		file.seekg(static_cast<std::streamoff>(h.cnt + start));
		if (file.read(result.data(), size)) return result;
		break;
	}
	return {};
}

fs::path Prepare(const fs::path& input, const fs::path& cache, const Extract& extract) {
	const auto path = fs::canonical(input);
	switch (Inspect(path)) {
	case Kind::Invalid: throw std::runtime_error("Invalid or unsupported PS5 PKG header");
	case Kind::Retail: throw std::runtime_error("Encrypted retail PKG cannot be booted");
	case Kind::Debug: break;
	}
	const auto identity = Identity(path);
	uint64_t hash = 14695981039346656037ULL;
	for (unsigned char byte: identity) hash = (hash ^ byte) * 1099511628211ULL;
	const auto target = cache / std::to_string(hash);
	if (Ready(target, identity)) return target / "app0";
	fs::create_directories(cache);
	std::random_device random;
	fs::path stage;
	do {
		stage = cache / ("unpacking-" + std::to_string(random()) + "-" + std::to_string(random()));
	} while (!fs::create_directory(stage));
	struct Cleanup {
		fs::path stage;
		~Cleanup() { std::error_code ec; fs::remove_all(stage, ec); }
	} cleanup {stage};
	extract(path, stage);
	const auto source = stage / "source";
	const auto app = stage / "app0";
	if (!fs::is_regular_file(source / "eboot.bin") || !fs::is_directory(app))
		throw std::runtime_error("Package has no bootable application");
	for (const auto& entry: fs::recursive_directory_iterator(source)) {
		if (!entry.is_regular_file()) continue;
		const auto dest = app / entry.path().lexically_relative(source);
		fs::create_directories(dest.parent_path());
		// A bundled module may also appear in app0 in its original SELF form.
		if (fs::exists(dest)) fs::remove(dest);
		fs::rename(entry.path(), dest);
	}
	fs::remove_all(source);
	if (Identity(path) != identity) throw std::runtime_error("Package changed during extraction");
	std::ofstream marker(stage / "package.id", std::ios::binary);
	marker << identity;
	marker.close();
	if (!marker) throw std::runtime_error("Could not record package cache identity");
	std::error_code ec;
	fs::rename(stage, target, ec);
	if (ec && !Ready(target, identity)) throw fs::filesystem_error("Could not publish package cache", target, ec);
	return target / "app0";
}
} // namespace Common::Package
