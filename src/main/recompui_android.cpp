/* Activity-backed settings bridge for the native Android runner. */

#include "recompui_launcher.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "app_paths.h"

namespace {
std::unordered_map<std::string, std::string> read_config() {
    std::unordered_map<std::string, std::string> values;
    std::ifstream file(pkmnstadium::exe_dir() / "launcher.cfg");
    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        auto trim = [](std::string& text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        };
        trim(key);
        trim(value);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!key.empty()) values[std::move(key)] = std::move(value);
    }
    return values;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

namespace pkmnstadium::recompui {

bool run(const char* rom_path_in, char* out_rom, std::size_t out_len) {
    if (out_rom != nullptr && out_len != 0) {
        const char* source = rom_path_in != nullptr ? rom_path_in : "";
        std::strncpy(out_rom, source, out_len - 1);
        out_rom[out_len - 1] = '\0';
    }
    return true;
}

PortAssignment port_assignment(int) { return {}; }
bool startup_fullscreen() { return true; }
std::string startup_graphics_api() { return "vulkan"; }
int startup_ds_option() {
    const auto config = read_config();
    const auto it = config.find("supersampling");
    const std::string value = it == config.end() ? "off" : lower(it->second);
    if (value == "4x") return 4;
    if (value == "2x") return 2;
    return 1;
}
int startup_msaa() {
    const auto config = read_config();
    const auto it = config.find("antialiasing");
    const std::string value = it == config.end() ? "off" : lower(it->second);
    if (value == "8x" || value == "8") return 8;
    if (value == "4x" || value == "4") return 4;
    if (value == "2x" || value == "2") return 2;
    return 0;
}
std::string startup_audio_device() { return {}; }

} // namespace pkmnstadium::recompui
