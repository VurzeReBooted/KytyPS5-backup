#include "common/package.h"
#include "common/archive.h"
#include "loader/elf.h"

#include <algorithm>

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
	WriteFixture(pkg);
	Check(P::IsFilename(pkg) && !P::IsFilename("title.pkg.old"), "filename match");
	Check(P::Inspect(pkg) == P::Kind::Debug, "debug scan");
	Check(P::ReadMetadata(pkg, "sce_sys/param.json", 2) == std::vector<char>{'{', '}'}, "metadata");
	Check(P::ReadMetadata(pkg, "sce_sys/param.json", 1).empty(), "metadata limit");

	WriteFixture(pkg, true);
	Check(P::Inspect(pkg) == P::Kind::Retail, "retail detection");
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

int TestPackageMount(const fs::path& path) {
	const auto reader = Common::OpenArchive(path);
	Check(reader != nullptr, "open PKG");
	const auto root = Common::MakeArchivePath(path);
	const auto children = Common::GetArchiveDirEntries(root);
	Check(std::ranges::any_of(children, [](const auto& e) { return !e.is_file && e.name == "data"; }), "directory listing");
	auto read = [&](const fs::path& name, uint64_t offset, uint32_t size) {
		auto f = Common::OpenArchiveFile(root / name);
		Check(f && f->Seek(offset), "open member");
		std::string result(size, ' ');
		uint32_t got = 0;
		f->Read(result.data(), size, &got);
		result.resize(got);
		return result;
	};
	Check(read("data/asset", 2, 8) == "foreAFTE", "cross-block read");
	Loader::Elf64 executable;
	executable.Open(root / "eboot.bin");
	Check(executable.IsValid() && executable.IsNextGen() && executable.IsSelf(), "load PS5 SELF");
	std::string segment(4, ' ');
	executable.LoadSegment(reinterpret_cast<uintptr_t>(segment.data()), 0x1000, segment.size());
	Check(segment == "TEST", "load SELF segment from PKG");
	Check(read("sce_sys/param.json", 0, 8) == "{}", "read CNT metadata to EOF");
	Check(read("data/asset", 6, 5) == "AFTER", "out-of-order seek");
	return 0;
}
