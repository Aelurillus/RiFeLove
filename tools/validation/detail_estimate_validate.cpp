// Development-only local RGB fixture evaluator; not included in the desktop package.
#include "restoration/detail_estimate.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("Expected RGB24_FILE WIDTH HEIGHT");
        const int width = std::stoi(argv[2]), height = std::stoi(argv[3]);
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
            throw std::invalid_argument("Invalid fixture dimensions");
        const auto plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(plane * 3))
            throw std::invalid_argument("RGB fixture size does not match dimensions");
        input.seekg(0);
        std::vector<std::uint8_t> bytes(plane * 3);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("Could not read RGB fixture");
        rifelove::media::RifeSdrFrame frame{width,height,width,height,{},std::vector<float>(plane * 3)};
        for (std::size_t index = 0; index < plane; ++index)
            for (std::size_t channel = 0; channel < 3; ++channel)
                frame.pixels[channel * plane + index] = bytes[index * 3 + channel] / 255.0F;
        const auto result = rifelove::restoration::estimateDirectionalSoftEdges(frame);
        std::cout << "{\"strength\":" << result.strength << ",\"confidence\":" << result.confidence
                  << ",\"sampledPositions\":" << result.sampledPositions << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
