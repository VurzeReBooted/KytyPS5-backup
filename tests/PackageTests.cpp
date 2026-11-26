#include "common/package.h"

#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace P = Common::Package;

static void Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}

static void WriteFixture(const fs::path& path, bool retail = false) {
	std::vector<unsigned char> bytes(1024);
	auto big = [&](size_t offset, unsigned value) {
		for (int i = 3; i >= 0; --i, value >>= 8) bytes[offset + i] = value & 255;
	};
	big(0, 0x7f464948);
	bytes[5] = retail ? 0x80 : 0;
	bytes[0x11] = bytes[0x19] = 1;
	bytes[0x59] = 2;
	big(512, 0x7f434e54);
	big(528, 1);
	big(536, 128);
	big(640, 0x2000);
	big(656, 160);
	big(660, 2);
	bytes[672] = '{';
	bytes[673] = '}';
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<char*>(bytes.data()), bytes.size());
}

static void Test(const fs::path& root) {
	const auto pkg = root / "a space.PkG";
	const auto cache = root / "cache";
	WriteFixture(pkg);
	Check(P::IsFilename(pkg) && !P::IsFilename("title.pkg.old"), "filename match");
	Check(P::Inspect(pkg) == P::Kind::Debug, "debug scan");
	Check(P::ReadMetadata(pkg, "sce_sys/param.json", 2) == std::vector<char>{'{', '}'}, "metadata");
	Check(P::ReadMetadata(pkg, "sce_sys/param.json", 1).empty(), "metadata limit");

	int runs = 0;
	auto extract = [&](const fs::path& input, const fs::path& out) {
		Check(input == pkg, "input path");
		++runs;
		fs::create_directories(out / "source/modules");
		fs::create_directories(out / "app0/modules");
		std::ofstream(out / "source/eboot.bin") << "elf";
		std::ofstream(out / "source/modules/lib.prx") << "decoded";
		std::ofstream(out / "app0/modules/lib.prx") << "original";
		std::ofstream(out / "app0/asset") << "asset";
	};
	const auto app = P::Prepare(pkg, cache, extract);
	std::string module;
	std::ifstream(app / "modules/lib.prx") >> module;
	Check(module == "decoded" && fs::is_regular_file(app / "asset"), "merged tree");
	Check(P::Prepare(pkg, cache, extract) == app && runs == 1, "cache reuse");
	std::ofstream(pkg, std::ios::app) << "x";
	Check(P::Prepare(pkg, cache, extract) != app && runs == 2, "cache invalidation");

	const auto failed = root / "failed";
	try {
		P::Prepare(pkg, failed, [](const auto&, const auto&) {});
		throw std::logic_error("booted without executable");
	} catch (const std::runtime_error&) {}
	Check(fs::is_empty(failed), "partial cache after failure");
	Check(fs::exists(P::Prepare(pkg, failed, extract) / "eboot.bin"), "retry");

	WriteFixture(pkg, true);
	Check(P::Inspect(pkg) == P::Kind::Retail, "retail detection");
	try {
		P::Prepare(pkg, cache, extract);
		throw std::logic_error("accepted encrypted input");
	} catch (const std::runtime_error&) {}
	Check(runs == 3, "invoked helper for retail");
	fs::resize_file(pkg, 650);
	Check(P::Inspect(pkg) == P::Kind::Invalid, "truncated entry table");
}

void TestPackages() {
	const auto root = fs::temp_directory_path() / ("kyty-package-tests-" + std::to_string(std::random_device{}()));
	Check(fs::create_directory(root), "create test directory");
	struct Cleanup {
		fs::path root;
		~Cleanup() { std::error_code ec; fs::remove_all(root, ec); }
	} cleanup {root};
	Test(root);
}
