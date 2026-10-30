#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rifelove::logging {

enum class Level { debug, info, warning, error, critical };

using Value = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct Field {
    std::string key;
    Value value;
};

struct Options {
    std::size_t maxFileSizeBytes = 4U * 1024U * 1024U;
    std::size_t retainedFiles = 3;
};

[[nodiscard]] bool initialize(const std::filesystem::path& file, Options options = {}) noexcept;
void shutdown() noexcept;

[[nodiscard]] std::string serializeJsonLine(Level level, std::string_view component, std::string_view event,
    std::string_view message, const std::vector<Field>& fields,
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());

void write(Level level, std::string_view component, std::string_view event, std::string_view message,
    const std::vector<Field>& fields = {}) noexcept;

} // namespace rifelove::logging
