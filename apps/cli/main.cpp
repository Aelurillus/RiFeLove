#include "database/task_store.hpp"
#include "timeline/timeline.hpp"

#include <iostream>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string jsonString(const std::string& input) {
    std::string result = "\"";
    for (const unsigned char value : input) {
        switch (value) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result += digits[value >> 4];
                result += digits[value & 0x0f];
            } else result += static_cast<char>(value);
        }
    }
    result += '"';
    return result;
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
        << "  rifelove-cli tasks DATABASE\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { usage(std::cout); return 0; }
        const std::string command = argv[1];
        if (command == "timeline") {
            std::string fps, end, pts;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 >= argc) throw std::invalid_argument("missing value for " + option);
                if (option == "--fps") fps = argv[++index];
                else if (option == "--end") end = argv[++index];
                else if (option == "--pts") pts = argv[++index];
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
            if (argc != 5) throw std::invalid_argument("task-create requires DATABASE SOURCE OUTPUT");
            rifelove::TaskStore store(argv[2]);
            std::cout << "{\"id\":\"" << store.create(argv[3], argv[4]) << "\",\"state\":\"queued\"}\n";
            return 0;
        }
        if (command == "tasks") {
            if (argc != 3) throw std::invalid_argument("tasks requires DATABASE");
            rifelove::TaskStore store(argv[2]);
            (void)store.recoverInterrupted();
            for (const auto& task : store.list())
                std::cout << "{\"id\":" << jsonString(task.id) << ",\"source\":" << jsonString(task.sourcePath)
                          << ",\"output\":" << jsonString(task.outputPath) << ",\"state\":" << jsonString(rifelove::toString(task.state))
                          << ",\"error\":" << jsonString(task.error) << "}\n";
            return 0;
        }
        usage(std::cerr);
        throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "rifelove-cli: " << error.what() << '\n';
        return 2;
    }
}
