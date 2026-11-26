#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

namespace Common::Package {

bool IsFilename(const std::filesystem::path& path);
enum class Kind { Invalid, Debug, Retail };
Kind Inspect(const std::filesystem::path& path);
std::vector<char> ReadMetadata(const std::filesystem::path& path, std::string_view member, uint64_t limit);

using Extract = std::function<void(const std::filesystem::path&, const std::filesystem::path&)>;
// Returns a complete app0 tree or throws with the reason preparation failed.
std::filesystem::path Prepare(const std::filesystem::path& path, const std::filesystem::path& cache, const Extract& extract);

} // namespace Common::Package
