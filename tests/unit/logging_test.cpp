#include "common/logging.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace rifelove::logging;

TEST(StructuredLogging, SerializesEscapedJsonAndTypedFields) {
    const std::vector<Field> fields{
        {"attempts", std::uint64_t{3}},
        {"available", true},
        {"detail", std::string("bad \"encoder\"\n")}
    };
    const auto line = serializeJsonLine(Level::warning,"media","encoder.fallback","NVENC failed",fields,
        std::chrono::system_clock::time_point{});
    EXPECT_EQ(line,
        R"({"timestamp":"1970-01-01T00:00:00.000Z","level":"warning","component":"media","event":"encoder.fallback","message":"NVENC failed","fields":{"attempts":3,"available":true,"detail":"bad \"encoder\"\n"}})");
}

TEST(StructuredLogging, RotatesLocalJsonLinesAndBoundsRetainedFiles) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("rifelove-logs-" + std::to_string(unique));
    const auto file = directory / "rifelove.jsonl";
    ASSERT_TRUE(initialize(file,{512,2}));
    for (std::uint64_t index = 0; index < 30; ++index) {
        write(Level::info,"test","rotation",std::string(220,'x'),{{"index",index}});
    }
    shutdown();

    std::size_t fileCount = 0;
    std::size_t lineCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        ++fileCount;
        std::ifstream input(entry.path(),std::ios::binary);
        ASSERT_TRUE(input.good());
        std::string line;
        while (std::getline(input,line)) {
            ++lineCount;
            EXPECT_TRUE(line.starts_with("{\"timestamp\":"));
            EXPECT_TRUE(line.ends_with("}"));
            EXPECT_NE(line.find("\"component\":\"test\""),std::string::npos);
            EXPECT_NE(line.find("\"event\":\"rotation\""),std::string::npos);
            EXPECT_NE(line.find("\"index\":"),std::string::npos);
        }
    }
    EXPECT_GT(fileCount,1U);
    EXPECT_LE(fileCount,3U);
    EXPECT_GT(lineCount,0U);
    EXPECT_LT(lineCount,30U);
    std::filesystem::remove_all(directory);
}
} // namespace
