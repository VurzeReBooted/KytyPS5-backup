#pragma once

#include "common/archive.h"

#include <string_view>

namespace Common {

// Backends expose immutable metadata and offset-based reads. Cursor management, path routing,
// reader lifetime and the host-stack worker are shared by every archive format.
class ArchiveReader {
public:
	struct Entry {
		uint64_t id;
		uint64_t size;
		bool     is_file;
	};

	virtual ~ArchiveReader()                                                       = default;
	virtual std::optional<Entry>        Find(std::string_view member)              = 0;
	virtual std::vector<File::DirEntry> List(std::string_view member)              = 0;
	virtual uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) = 0;
};

std::shared_ptr<ArchiveReader> OpenPackageReader(const std::filesystem::path& path);

} // namespace Common
