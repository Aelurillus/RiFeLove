#include "common/logging.hpp"

#include "common/json.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>

namespace rifelove::logging {
namespace {
constexpr char loggerName[] = "rifelove.local";
std::mutex loggerMutex;
std::shared_ptr<spdlog::logger> activeLogger;

const char* levelName(Level level) {
    switch (level) {
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warning: return "warning";
    case Level::error: return "error";
    case Level::critical: return "critical";
    }
    return "info";
}

spdlog::level::level_enum spdlogLevel(Level level) {
    switch (level) {
    case Level::debug: return spdlog::level::debug;
    case Level::info: return spdlog::level::info;
    case Level::warning: return spdlog::level::warn;
    case Level::error: return spdlog::level::err;
    case Level::critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

std::string timestampText(std::chrono::system_clock::time_point timestamp) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch());
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(timestamp);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &time) != 0) return "1970-01-01T00:00:00.000Z";
#else
    if (!gmtime_r(&time, &utc)) return "1970-01-01T00:00:00.000Z";
#endif
    auto fractional = milliseconds.count() % 1000;
    if (fractional < 0) fractional += 1000;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << fractional << 'Z';
    return output.str();
}

std::string valueJson(const Value& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return jsonString(item);
        } else if constexpr (std::is_same_v<T, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(item)) return "null";
            std::array<char, 64> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), item,
                std::chars_format::general, std::numeric_limits<double>::max_digits10);
            return error == std::errc{} ? std::string(buffer.data(), end) : "null";
        } else {
            std::array<char, 32> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), item);
            return error == std::errc{} ? std::string(buffer.data(), end) : "null";
        }
    }, value);
}

class JsonPayloadFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& message, spdlog::memory_buf_t& destination) override {
        destination.append(message.payload.data(), message.payload.data() + message.payload.size());
        destination.push_back('\n');
    }

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<JsonPayloadFormatter>();
    }
};
} // namespace

bool initialize(const std::filesystem::path& file, Options options) noexcept {
    if (file.empty() || options.maxFileSizeBytes == 0 || options.retainedFiles > spdlog::sinks::rotating_file_sink_mt::MaxFiles)
        return false;
    try {
        std::error_code filesystemError;
        const auto parent = file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) return false;

        std::scoped_lock lock(loggerMutex);
        if (activeLogger) {
            activeLogger->flush();
            activeLogger.reset();
            spdlog::drop(loggerName);
        }
        auto logger = spdlog::rotating_logger_mt(loggerName, file.native(), options.maxFileSizeBytes, options.retainedFiles);
        logger->set_level(spdlog::level::trace);
        logger->set_formatter(std::make_unique<JsonPayloadFormatter>());
        logger->flush_on(spdlog::level::warn);
        activeLogger = std::move(logger);
        return true;
    } catch (...) {
        return false;
    }
}

void shutdown() noexcept {
    try {
        std::scoped_lock lock(loggerMutex);
        if (activeLogger) activeLogger->flush();
        activeLogger.reset();
        spdlog::drop(loggerName);
    } catch (...) {
    }
}

std::string serializeJsonLine(Level level, std::string_view component, std::string_view event,
    std::string_view message, const std::vector<Field>& fields, std::chrono::system_clock::time_point timestamp) {
    std::string result = "{\"timestamp\":" + jsonString(timestampText(timestamp)) +
        ",\"level\":" + jsonString(levelName(level)) +
        ",\"component\":" + jsonString(component) +
        ",\"event\":" + jsonString(event) +
        ",\"message\":" + jsonString(message) + ",\"fields\":{";
    bool first = true;
    for (const auto& field : fields) {
        if (!first) result.push_back(',');
        first = false;
        result += jsonString(field.key);
        result.push_back(':');
        result += valueJson(field.value);
    }
    result += "}}";
    return result;
}

void write(Level level, std::string_view component, std::string_view event, std::string_view message,
    const std::vector<Field>& fields) noexcept {
    try {
        std::shared_ptr<spdlog::logger> logger;
        {
            std::scoped_lock lock(loggerMutex);
            logger = activeLogger;
        }
        if (!logger) return;
        const auto payload = serializeJsonLine(level, component, event, message, fields);
        logger->log(spdlogLevel(level), "{}", payload);
    } catch (...) {
    }
}

} // namespace rifelove::logging
