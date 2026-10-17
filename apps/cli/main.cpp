#include "database/task_store.hpp"
#include "timeline/timeline.hpp"
#include "common/json.hpp"
#include "gpu/cuda_device.hpp"
#ifdef RIFELOVE_HAS_MEDIA
#include "media_commands.hpp"
#endif

#include <iostream>
#include <cstdint>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using rifelove::jsonString;

std::vector<std::string> commandLineUtf8(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    struct WideArguments {
        LPWSTR* values;
        ~WideArguments() { LocalFree(values); }
    };
    int count = 0;
    WideArguments wide{CommandLineToArgvW(GetCommandLineW(), &count)};
    if (!wide.values) throw std::runtime_error("could not read Windows command line");
    std::vector<std::string> result;
    for (int i = 0; i < count; ++i) {
        const int length = lstrlenW(wide.values[i]);
        if (length == 0) { result.emplace_back(); continue; }
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.values[i], length, nullptr, 0, nullptr, nullptr);
        if (bytes == 0) throw std::runtime_error("command line contains invalid Unicode");
        std::string value(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.values[i], length, value.data(), bytes, nullptr, nullptr) != bytes)
            throw std::runtime_error("could not convert command line to UTF-8");
        result.push_back(std::move(value));
    }
    return result;
#else
    return {argv, argv + argc};
#endif
}

std::vector<rifelove::Rational> parsePts(const std::string& input) {
    std::vector<rifelove::Rational> values;
    std::istringstream stream(input);
    std::string value;
    while (std::getline(stream, value, ',')) values.push_back(rifelove::Rational::parse(value));
    if (values.empty()) throw std::invalid_argument("--pts must contain at least one rational timestamp");
    return values;
}

void usage(std::ostream& out) {
    out << "RifeLove development CLI (timeline planning and task records; no pixel interpolation yet)\n"
        << "  rifelove-cli timeline --fps RATE --end SECONDS --pts PTS[,PTS...]\n"
        << "  rifelove-cli task-create DATABASE SOURCE OUTPUT\n"
        << "  rifelove-cli tasks DATABASE\n"
        << "  rifelove-cli tasks-recover DATABASE (maintenance: all workers must be stopped)\n";
    out << "  rifelove-cli gpu-info [DEVICE_INDEX]\n";
#ifdef RIFELOVE_HAS_MEDIA
    out << "  rifelove-cli media-probe INPUT\n"
        << "  rifelove-cli media-scan INPUT [--frames] [--stream INDEX]\n"
        << "  rifelove-cli remux INPUT OUTPUT\n"
        << "  rifelove-cli transcode INPUT OUTPUT [--encoder NAME] [--pixel-format FORMAT] [--bitrate N] [--bframes N] [--option KEY=VALUE] [--allow-precision-loss] [--progress]\n";
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = commandLineUtf8(argc, argv);
        if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") { usage(std::cout); return 0; }
        const std::string& command = args[1];
        if (command == "gpu-info") {
            if (args.size() > 3) throw std::invalid_argument("gpu-info accepts one optional device index");
            int ordinal = 0;
            if (args.size() == 3) {
                const auto& number = args[2];
                const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), ordinal);
                if (error != std::errc{} || end != number.data() + number.size() || ordinal < 0) throw std::invalid_argument("invalid GPU device index");
            }
            try {
                rifelove::gpu::CudaDevice device(ordinal);
                const auto& info = device.info();
                const auto memory = device.memory();
                std::cout << "{\"scope\":\"cuda_driver_device\",\"driver_available\":true,\"ordinal\":" << info.ordinal
                    << ",\"name\":" << jsonString(info.name) << ",\"pci_bus_id\":" << jsonString(info.pciBusId)
                    << ",\"compute_capability\":" << jsonString(std::to_string(info.computeMajor) + "." + std::to_string(info.computeMinor))
                    << ",\"driver_cuda_api_version\":" << info.driverCudaApiVersion << ",\"total_bytes\":" << info.totalBytes
                    << ",\"free_bytes\":" << memory.freeBytes << ",\"buffer_budget_bytes\":" << device.bufferBudget() << "}\n";
                return 0;
            } catch (const rifelove::gpu::GpuError& error) {
                std::cout << "{\"scope\":\"cuda_driver_device\",\"driver_available\":false,\"reason\":" << jsonString(error.what()) << "}\n";
                return 0;
            }
        }
        if (command == "media-probe" || command == "media-scan" || command == "remux" || command == "transcode") {
#ifdef RIFELOVE_HAS_MEDIA
            return runMediaCommand(args);
#else
            throw std::runtime_error("media commands require a build with RIFELOVE_BUILD_MEDIA=ON and a local FFmpeg SDK");
#endif
        }
        if (command == "timeline") {
            std::string fps, end, pts;
            for (std::size_t index = 2; index < args.size(); ++index) {
                const std::string& option = args[index];
                if (index + 1 >= args.size()) throw std::invalid_argument("missing value for " + option);
                if (option == "--fps") fps = args[++index];
                else if (option == "--end") end = args[++index];
                else if (option == "--pts") pts = args[++index];
                else throw std::invalid_argument("unknown option: " + option);
            }
            if (fps.empty() || end.empty() || pts.empty()) throw std::invalid_argument("timeline requires --fps, --end and --pts");
            const rifelove::Timeline timeline(parsePts(pts), rifelove::Rational::parse(end), rifelove::Rational::parse(fps));
            for (std::size_t i = 0; i < timeline.outputFrameCount(); ++i) {
                const auto frame = timeline.sample(i);
                std::cout << "{\"index\":" << frame.outputIndex << ",\"pts\":\"" << frame.timestamp.toString()
                          << "\",\"frame0\":" << frame.frame0 << ",\"frame1\":" << frame.frame1
                          << ",\"timestep\":\"" << frame.timestep.toString() << "\",\"source\":"
                          << (frame.sourceFrame ? "true" : "false") << ",\"terminal_hold\":" << (frame.terminalHold ? "true" : "false") << "}\n";
            }
            return 0;
        }
        if (command == "task-create") {
            if (args.size() != 5) throw std::invalid_argument("task-create requires DATABASE SOURCE OUTPUT");
            rifelove::TaskStore store(args[2]);
            std::cout << "{\"id\":\"" << store.create(args[3], args[4]) << "\",\"state\":\"queued\"}\n";
            return 0;
        }
        if (command == "tasks") {
            if (args.size() != 3) throw std::invalid_argument("tasks requires DATABASE");
            rifelove::TaskStore store(args[2], rifelove::TaskStore::OpenMode::readOnly);
            for (const auto& task : store.list())
                std::cout << "{\"id\":" << jsonString(task.id) << ",\"source\":" << jsonString(task.sourcePath)
                          << ",\"output\":" << jsonString(task.outputPath) << ",\"state\":" << jsonString(rifelove::toString(task.state))
                          << ",\"error\":" << jsonString(task.error) << "}\n";
            return 0;
        }
        if (command == "tasks-recover") {
            if (args.size() != 3) throw std::invalid_argument("tasks-recover requires DATABASE");
            rifelove::TaskStore store(args[2], rifelove::TaskStore::OpenMode::readWrite);
            std::cout << "{\"recovered\":" << store.recoverInterrupted() << "}\n";
            return 0;
        }
        usage(std::cerr);
        throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "rifelove-cli: " << error.what() << '\n';
        return 2;
    }
}
