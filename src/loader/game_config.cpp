#include "scarlettx/loader/game_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace scarlettx {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

constexpr std::string_view kXmlWhitespace = " \t\r\n";

std::string DecodeAttributeValue(std::string_view value) {
    static constexpr std::array<std::pair<std::string_view, char>, 5> entities = {{
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    }};

    std::string decoded;
    decoded.reserve(value.size());
    while (!value.empty()) {
        if (value.front() == '&') {
            const auto entity = std::find_if(entities.begin(), entities.end(), [value](const auto& entry) {
                return value.starts_with(entry.first);
            });
            if (entity != entities.end()) {
                decoded.push_back(entity->second);
                value.remove_prefix(entity->first.size());
                continue;
            }
        }
        decoded.push_back(value.front());
        value.remove_prefix(1);
    }
    return decoded;
}

bool ExtractAttribute(std::string_view tag, std::string_view attr_name, std::string& out_value) {
    size_t pos = tag.find_first_of(kXmlWhitespace);
    while ((pos = tag.find_first_not_of(kXmlWhitespace, pos)) != std::string_view::npos) {
        const size_t name_end = tag.find_first_of(" \t\r\n=/>", pos);
        if (name_end == pos || name_end == std::string_view::npos) {
            return false;
        }

        const size_t equals = tag.find_first_not_of(kXmlWhitespace, name_end);
        if (equals == std::string_view::npos || tag[equals] != '=') {
            return false;
        }

        const size_t quote = tag.find_first_not_of(kXmlWhitespace, equals + 1);
        if (quote == std::string_view::npos || (tag[quote] != '"' && tag[quote] != '\'')) {
            return false;
        }
        const size_t value_end = tag.find(tag[quote], quote + 1);
        if (value_end == std::string_view::npos) {
            return false;
        }
        if (tag.substr(pos, name_end - pos) == attr_name) {
            out_value = DecodeAttributeValue(tag.substr(quote + 1, value_end - quote - 1));
            return true;
        }
        pos = value_end + 1;
    }
    return false;
}

bool ExtractElementText(const std::string& xml, const std::string& element, std::string& out_text) {
    const std::string open_prefix = "<" + element;
    size_t start = xml.find(open_prefix);
    if (start == std::string::npos) {
        return false;
    }
    size_t gt = xml.find('>', start);
    if (gt == std::string::npos) {
        return false;
    }
    const std::string close_tag = "</" + element + ">";
    size_t close = xml.find(close_tag, gt + 1);
    if (close == std::string::npos) {
        return false;
    }
    out_text = Trim(xml.substr(gt + 1, close - (gt + 1)));
    return true;
}

size_t FindTagEnd(const std::string& xml, size_t pos) {
    while ((pos = xml.find_first_of(">\"'", pos)) != std::string::npos) {
        if (xml[pos] == '>') {
            return pos;
        }
        pos = xml.find(xml[pos], pos + 1);
        if (pos == std::string::npos) {
            break;
        }
        ++pos;
    }
    return std::string::npos;
}

bool ExtractTagBlock(const std::string& xml, const std::string& element, std::string& out_tag, size_t search_from = 0, size_t* out_next = nullptr) {
    const std::string open_prefix = "<" + element;
    size_t pos = xml.find(open_prefix, search_from);
    while (pos != std::string::npos) {
        const size_t after = pos + open_prefix.size();
        if (after < xml.size() && (std::isspace(static_cast<unsigned char>(xml[after])) || xml[after] == '>' || xml[after] == '/')) {
            const size_t gt = FindTagEnd(xml, after);
            if (gt == std::string::npos) {
                return false;
            }
            out_tag = xml.substr(pos, gt - pos + 1);
            if (out_next) {
                *out_next = gt + 1;
            }
            return true;
        }
        pos = xml.find(open_prefix, after);
    }
    return false;
}

}  // namespace

const GameExecutableEntry* GameConfig::PrimaryExecutable() const noexcept {
    for (const auto& exe : executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            return &exe;
        }
    }
    if (!executables.empty()) {
        return &executables.front();
    }
    return nullptr;
}

Status ParseGameConfigXml(const std::string& xml_content, GameConfig& out_config) {
    out_config = GameConfig{};
    if (xml_content.find("<Game") == std::string::npos) {
        return Status::Error(StatusCode::ParseError, "Missing <Game> root element in MicrosoftGame.Config");
    }

    std::string game_tag;
    if (ExtractTagBlock(xml_content, "Game", game_tag)) {
        ExtractAttribute(game_tag, "configVersion", out_config.config_version);
    }

    std::string identity_tag;
    if (ExtractTagBlock(xml_content, "Identity", identity_tag)) {
        ExtractAttribute(identity_tag, "Name", out_config.identity_name);
        ExtractAttribute(identity_tag, "Publisher", out_config.publisher);
        ExtractAttribute(identity_tag, "Version", out_config.version);
    }

    ExtractElementText(xml_content, "TitleId", out_config.title_id);

    std::string shell_tag;
    if (ExtractTagBlock(xml_content, "ShellVisuals", shell_tag)) {
        ExtractAttribute(shell_tag, "DefaultDisplayName", out_config.display_name);
    }

    size_t cursor = 0;
    std::string exe_tag;
    while (ExtractTagBlock(xml_content, "Executable", exe_tag, cursor, &cursor)) {
        GameExecutableEntry entry;
        ExtractAttribute(exe_tag, "Name", entry.name);
        ExtractAttribute(exe_tag, "Id", entry.id);
        ExtractAttribute(exe_tag, "TargetDeviceFamilyForXboxOne", entry.target_device_family);
        if (entry.target_device_family.empty()) {
            ExtractAttribute(exe_tag, "TargetDeviceFamily", entry.target_device_family);
        }
        ExtractAttribute(exe_tag, "Alias", entry.alias);
        std::string dev_only;
        if (ExtractAttribute(exe_tag, "IsDevOnly", dev_only)) {
            entry.is_dev_only = (ToLower(dev_only) == "true");
        }
        if (!entry.name.empty()) {
            out_config.executables.push_back(std::move(entry));
        }
    }

    // Determine ConsoleTarget from executable TargetDeviceFamily or package metadata.
    const std::string lower_xml = ToLower(xml_content);
    bool has_gen9 = false;
    bool has_gen9_aware = false;
    bool has_xbox_one = false;

    for (const auto& exe : out_config.executables) {
        const std::string tdf = ToLower(exe.target_device_family);
        if (tdf == "xboxgen9" || tdf == "scarlett") {
            has_gen9 = true;
        } else if (tdf == "xboxonegen9aware") {
            has_gen9_aware = true;
        } else if (tdf == "xboxone" || tdf == "durango") {
            has_xbox_one = true;
        }
    }

    if (has_gen9 || lower_xml.find("xboxgen9") != std::string::npos ||
        lower_xml.find("gaming.xbox.scarlett.x64") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxGen9Scarlett;
    } else if (has_gen9_aware || lower_xml.find("xboxonegen9aware") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOneGen9Aware;
    } else if (has_xbox_one || lower_xml.find("xboxone") != std::string::npos) {
        out_config.console_target = ConsoleTarget::XboxOne;
    }

    if (out_config.executables.empty()) {
        return Status::Error(StatusCode::ParseError, "MicrosoftGame.Config contains no <Executable> entries");
    }

    return Status::Ok();
}

Status LoadGameConfigFile(const std::string& file_path, GameConfig& out_config) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open MicrosoftGame.Config: " + file_path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    Status st = ParseGameConfigXml(oss.str(), out_config);
    if (st.ok()) {
        out_config.source_path = file_path;
    }
    return st;
}

}  // namespace scarlettx
