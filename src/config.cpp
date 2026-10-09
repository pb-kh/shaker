#include "shaker/config.hpp"

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace shaker {

namespace {

[[nodiscard]] ConfigError parse_error_at(const std::string& path, int line,
                                        const std::string& reason)
{
    std::ostringstream detail;
    detail << "config parse error '" << path << "': line " << line << ": "
           << reason;
    return ConfigError{.detail = detail.str(), .path = path};
}

[[nodiscard]] std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

// The template written when the config file does not exist (contract §1
// auto-initialization): the documented example values, commented keys, and
// an example command the user may edit.
constexpr std::string_view kDefaultConfigTemplate =
    "# shaker configuration (auto-generated on first run)\n"
    "# Shaker detects pointer oscillation (back-and-forth movement) and\n"
    "# runs `command`. Edit and restart to apply (no hot reload).\n"
    "\n"
    "# How far each swing leg must travel, in pointer units, to count as\n"
    "# a direction change. Raise if ordinary wiggles trigger it, lower if\n"
    "# your shakes are missed.\n"
    "sensitivity = 40.0\n"
    "\n"
    "# How long (seconds) the swings may take to complete the shake.\n"
    "duration = 0.25\n"
    "\n"
    "# How long (seconds) to ignore further shakes after one triggers.\n"
    "cooldown = 2.0\n"
    "\n"
    "# Run through /bin/sh -c when a shake triggers (the only required key).\n"
    "command = \"notify-send 'Shake detected'\"\n";

[[nodiscard]] bool create_default_config(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec && !std::filesystem::is_directory(path.parent_path())) {
        return false;
    }
    std::ofstream created(path, std::ios::binary | std::ios::trunc);
    if (!created.is_open()) {
        return false;
    }
    created << kDefaultConfigTemplate;
    created.close();
    return created.good();
}

// The v0/v1 template (auto-generated before the v2 sensitivity rework).
// Files that match it verbatim are auto-migrated to the current template —
// explicitly NOT rewriting hand-edited files (an exact match can only be
// an untouched generated file, never user work).
constexpr std::string_view kDefaultConfigTemplateV1 =
    "# shaker configuration (auto-generated on first run)\n"
    "# Shaker detects fast sustained movement of your pointer devices and\n"
    "# runs `command`. Edit and restart to apply (no hot reload).\n"
    "\n"
    "# How fast movement must be, in device units per second, to count\n"
    "# toward a shake. Raise for stronger shakes only, lower to tune in.\n"
    "sensitivity = 8000.0\n"
    "\n"
    "# How long (seconds) movement must stay above sensitivity to be a shake.\n"
    "duration = 0.25\n"
    "\n"
    "# How long (seconds) to ignore further shakes after one triggers.\n"
    "cooldown = 2.0\n"
    "\n"
    "# Run through /bin/sh -c when a shake triggers (the only required key).\n"
    "command = \"notify-send 'Shake detected'\"\n";

[[nodiscard]] bool migrate_v1_default_config(const std::filesystem::path& path,
                                             const std::string& contents)
{
    if (contents != kDefaultConfigTemplateV1) {
        return false;
    }
    std::ofstream rewritten(path, std::ios::binary | std::ios::trunc);
    if (!rewritten.is_open()) {
        return false;
    }
    rewritten << kDefaultConfigTemplate;
    rewritten.close();
    return rewritten.good();
}

// Strip TOML `_` separators between digits (permissive subset).
[[nodiscard]] std::string strip_underscores(std::string_view number)
{
    std::string clean;
    clean.reserve(number.size());
    for (const char c : number) {
        if (c != '_') {
            clean.push_back(c);
        }
    }
    return clean;
}

// Scan one logical line: locate a `#` comment outside string literals and
// ensure quotes are balanced and strings don't span lines.
struct ScanResult {
    size_t comment_at = std::string_view::npos;
    std::string_view error; // empty = ok
};

[[nodiscard]] ScanResult scan_line(std::string_view line)
{
    bool in_quote = false;
    char quote = '\0';
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (in_quote) {
            if (c == '\\') {
                if (i + 1 == line.size()) {
                    return {.error = "string escape runs past end of line"};
                }
                i += 2; // consume the escaped character
                continue;
            }
            if (c == quote) {
                in_quote = false;
            }
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
            ++i;
            continue;
        }
        if (c == '#') {
            return {.comment_at = i,.error = {}};
        }
        if (c == '[' || c == '{') {
            return {.error = "arrays and inline tables are not accepted "
                             "in a shaker config"};
        }
        ++i;
    }
    if (in_quote) {
        return {.error = "unterminated string (or unsupported construct) "
                         "in a shaker config"};
    }
    return {};
}

[[nodiscard]] bool is_valid_key(std::string_view key)
{
    if (key.empty()) {
        return false;
    }
    for (const char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'
              || c == '-')) {
            return false;
        }
    }
    return true;
}

// Parse a double from a TOML bare value; number = true consumes the whole
// token. Returns false when the token is not (only) a number.
[[nodiscard]] bool parse_number(std::string_view value, double& out)
{
    if (value.empty() || value.find('"') != std::string_view::npos
        || value.find('\'') != std::string_view::npos
        || value[0] == 't' || value[0] == 'f' || (!std::isdigit(static_cast<unsigned char>(
                                 value[value[0] == '+' || value[0] == '-' ? 1 : 0]))
                             && value[0] != '+' && value[0] != '-')) {
        if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0]))
                               != 0 || value[0] == '+' || value[0] == '-')) {
            // fall through to real parsing below
        } else {
            return false;
        }
    }
    // Classic TOML vetoes on words: inf / nan
    if (value == "inf" || value == "+inf" || value == "-inf" || value == "nan"
        || value == "true" || value == "false") {
        return false;
    }

    const std::string clean = strip_underscores(value);
    double parsed_value = 0.0;
    const auto* begin = clean.data();
    const auto* end = begin + clean.size();
    const auto [ptr, result] = std::from_chars(begin, end, parsed_value);
    if (result != std::errc() || ptr != end) {
        return false;
    }
    out = parsed_value;
    return true;
}

// Extract the unescaped string content of a quoted TOML value spanning the
// whole (already comment-stripped, trimmed) token.
[[nodiscard]] std::string_view::const_pointer parse_string(
    std::string_view value, std::string& out)
{
    if (value.size() < 2) {
        return nullptr;
    }
    const char quote = value.front();
    if (quote != '"' && quote != '\'') {
        return nullptr;
    }
    if (value.back() != quote) {
        return nullptr;
    }
    value.remove_prefix(1);
    value.remove_suffix(1);

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && quote == '"') {
            ++i;
            if (i == value.size()) {
                return nullptr; // dangling escape (should not happen post-scan)
            }
            switch (value[i]) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            default:
                return nullptr; // unsupported escape
            }
            continue;
        }
        if (value[i] == quote) {
            return nullptr; // impossible post-scan; defensive
        }
        out.push_back(value[i]);
    }
    return value.data();
}

} // namespace

std::filesystem::path config_path()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
        xdg != nullptr && xdg[0] == '/') {
        return std::filesystem::path(xdg) / "shaker" / "config.toml";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
        return std::filesystem::path(home) / ".config" / "shaker" / "config.toml";
    }
    // No usable configuration home: surface as a read error at load.
    return "/__shaker_unresolvable_config_home__/shaker/config.toml";
}

std::expected<Config, ConfigError> parse_config(
    const std::string& path, const std::string& contents,
    std::vector<std::string>* warnings)
{
    Config config;

    // Multi-line string capture (''' literal / """ basic): the `command`
    // value may be a full shell script (executed via /bin/sh -c), so
    // natural multi-line shell text is accepted verbatim (literal) or with
    // TOML escapes (basic).
    enum class Capture { None, Literal, Basic };
    Capture capture = Capture::None;
    std::string captured;
    std::string capture_key;
    int capture_start_line = 0;
    // Basic-string walker state across lines.
    bool pending_escape = false;      // previous char was an escape backslash
    bool pending_escape_trim = false; // backslash-newline: trim following ws

    // Dispatch an already-unescaped string value to its key (multi-line
    // strings only exist for the shell command; numeric keys reject them).
    const auto dispatch_string = [&](const std::string& key,
                                     const std::string& content,
                                     [[maybe_unused]] int line_number)
        -> std::optional<ConfigError>
    {
        if (key == "command") {
            config.command = content;
            return std::nullopt;
        }
        if (key == "sensitivity" || key == "duration" || key == "cooldown") {
            return ConfigError{
                .detail = "config '" + path + "': key \"" + key
                              + "\" has wrong type: expected number",
                .path = path,
            };
        }
        if (warnings != nullptr) {
            warnings->push_back("config '" + path + "': ignoring unknown key \""
                                + key + "\"");
        }
        return std::nullopt;
    };

    const auto remainder_error = [&](std::string_view rest, int line_number)
        -> std::optional<ConfigError>
    {
        if (!rest.empty() && rest.front() != '#') {
            return parse_error_at(path, line_number,
                                  "trailing content after the string terminator");
        }
        return std::nullopt;
    };

    int line_number = 0;
    std::istringstream stream(contents);
    std::string raw_line;
    while (std::getline(stream, raw_line)) {
        ++line_number;
        std::string_view line = raw_line;
        // Normalize CRLF: getline keeps the carriage return.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // ---- continuation of a captured multi-line string ---------------
        if (capture == Capture::Literal) {
            const size_t term = line.find("'''");
            if (term == std::string_view::npos) {
                captured += line;
                captured += '\n';
                continue;
            }
            captured += line.substr(0, term);
            const std::string_view rest = trim(line.substr(term + 3));
            if (const auto err = remainder_error(rest, line_number); err) {
                return std::unexpected(*err);
            }
            capture = Capture::None;
            if (const auto err = dispatch_string(capture_key, captured,
                                                 line_number);
                err) {
                return std::unexpected(*err);
            }
            continue;
        }
        if (capture == Capture::Basic) {
            size_t term_pos = std::string_view::npos;
            size_t i = 0;
            for (; i < line.size(); ++i) {
                const char c = line[i];
                if (pending_escape) {
                    pending_escape = false;
                    switch (c) {
                    case 'n': captured += '\n'; break;
                    case 't': captured += '\t'; break;
                    case 'r': captured += '\r'; break;
                    case '"': captured += '"'; break;
                    case '\\': captured += '\\'; break;
                    default:
                        return std::unexpected(parse_error_at(
                            path, line_number,
                            std::string("unsupported escape \\") + c));
                    }
                    continue;
                }
                if (pending_escape_trim) {
                    // Line continuation: skip whitespace until content.
                    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                        continue;
                    }
                    pending_escape_trim = false;
                }
                if (c == '\\') {
                    pending_escape = true;
                    continue;
                }
                if (c == '"') {
                    // Count consecutive quotes: three = terminator.
                    size_t run = 1;
                    while (i + run < line.size() && line[i + run] == '"') {
                        ++run;
                    }
                    if (run >= 3) {
                        term_pos = i;
                        i += 2; // loop ++i lands after the terminator
                        break;
                    }
                    captured.append(run, '"');
                    i += run - 1;
                    continue;
                }
                captured += c;
            }
            if (term_pos == std::string_view::npos) {
                if (pending_escape) {
                    // Backslash at end of line: trim the newline and the
                    // following whitespace (TOML line continuation).
                    pending_escape = false;
                    pending_escape_trim = true;
                } else if (!pending_escape_trim) {
                    captured += '\n';
                }
                continue;
            }
            const std::string_view rest = trim(line.substr(term_pos + 3));
            if (const auto err = remainder_error(rest, line_number); err) {
                return std::unexpected(*err);
            }
            capture = Capture::None;
            pending_escape_trim = false;
            if (const auto err = dispatch_string(capture_key, captured,
                                                 line_number);
                err) {
                return std::unexpected(*err);
            }
            continue;
        }

        // ---- fresh line --------------------------------------------------
        // Probe for a multi-line string opening BEFORE scan_line: triple
        // quotes leave scan_line's quote tracking unbalanced by design.
        if (const size_t eq_probe = line.find('='); eq_probe != std::string_view::npos) {
            const std::string_view key_probe = trim(line.substr(0, eq_probe));
            const std::string_view value_probe = trim(line.substr(eq_probe + 1));
            const bool command_line =
                is_valid_key(key_probe) && key_probe != "sensitivity"
                && key_probe != "duration" && key_probe != "cooldown";
            if (command_line
                && (value_probe.starts_with("'''")
                    || value_probe.starts_with("\"\"\""))) {
                const bool literal = value_probe.starts_with("'''");
                std::string_view rest = value_probe.substr(3);
                capture = literal ? Capture::Literal : Capture::Basic;
                capture_key = std::string(key_probe);
                capture_start_line = line_number;
                captured.clear();

                // Terminator on the opening line itself?
                size_t term_pos = std::string_view::npos;
                if (literal) {
                    term_pos = rest.find("'''");
                } else {
                    // escape-aware scan for three consecutive quotes
                    size_t run = 0;
                    for (size_t i = 0; i < rest.size(); ++i) {
                        if (rest[i] == '\\') {
                            ++i; // skip escaped char
                            continue;
                        }
                        if (rest[i] == '"') {
                            ++run;
                            if (run >= 3) {
                                term_pos = i - 2;
                                break;
                            }
                        } else {
                            run = 0;
                        }
                    }
                }
                if (term_pos != std::string_view::npos) {
                    // Full string on the opening line.
                    if (literal) {
                        captured = std::string(rest.substr(0, term_pos));
                    } else {
                        // Re-walk with escapes for the basic case.
                        captured.clear();
                        for (size_t i = 0; i < term_pos; ++i) {
                            const char c = rest[i];
                            if (c == '\\' && i + 1 < term_pos) {
                                ++i;
                                switch (rest[i]) {
                                case 'n': captured += '\n'; break;
                                case 't': captured += '\t'; break;
                                case 'r': captured += '\r'; break;
                                case '"': captured += '"'; break;
                                case '\\': captured += '\\'; break;
                                default:
                                    return std::unexpected(parse_error_at(
                                        path, line_number,
                                        std::string("unsupported escape \\")
                                            + rest[i]));
                                }
                                continue;
                            }
                            captured += c;
                        }
                    }
                    const std::string_view tail =
                        trim(rest.substr(term_pos + 3));
                    if (const auto err = remainder_error(tail, line_number);
                        err) {
                        return std::unexpected(*err);
                    }
                    capture = Capture::None;
                    if (const auto err = dispatch_string(capture_key, captured,
                                                         line_number);
                        err) {
                        return std::unexpected(*err);
                    }
                    continue;
                }
                // Content starts here (rest may be empty — the newline
                // immediately following the opening delimiter is trimmed,
                // which falls out of the continuation loop naturally).
                captured = std::string(rest);
                continue;
            }
        }

        const ScanResult scan = scan_line(raw_line);
        if (!scan.error.empty()) {
            return std::unexpected(parse_error_at(path, line_number, std::string(scan.error)));
        }
        std::string_view content = line;
        content = content.substr(0, scan.comment_at == std::string_view::npos
                              ? content.size()
                              : scan.comment_at);
        const std::string_view trimmed = trim(content);
        if (trimmed.empty()) {
            continue;
        }

        const size_t eq = trimmed.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected(
                parse_error_at(path, line_number,
                               "expected `key = value` (missing '=')"));
        }
        const std::string_view key = trim(trimmed.substr(0, eq));
        const std::string_view value = trim(trimmed.substr(eq + 1));
        if (!is_valid_key(key)) {
            return std::unexpected(
                parse_error_at(path, line_number, "invalid key (use [a-z A-Z 0-9 _ -])"));
        }
        if (value.empty()) {
            return std::unexpected(
                parse_error_at(path, line_number, "missing value"));
        }

        if (key == "sensitivity" || key == "duration" || key == "cooldown") {
            double number = 0.0;
            if (!parse_number(value, number)) {
                const char* got_class = "string";
                if (value.front() == '"' || value.front() == '\'') {
                    got_class = "string";
                } else if (value == "true" || value == "false") {
                    got_class = "boolean";
                } else {
                    got_class = "value";
                }
                return std::unexpected(ConfigError{
                    .detail = "config '" + path + "': key \""
                                  + std::string(key)
                                  + "\" has wrong type: expected number, got "
                                  + got_class,
                    .path = path,
                });
            }
            if (key == "sensitivity") {
                config.sensitivity = number;
            } else if (key == "duration") {
                config.duration_sec = number;
            } else {
                config.cooldown_sec = number;
            }
            continue;
        }

        if (key == "command") {
            if (value.front() != '"' && value.front() != '\'') {
                return std::unexpected(ConfigError{
                    .detail = "config '" + path + "': key \"command\" has wrong "
                                  "type: expected string",
                    .path = path,
                });
            }
            std::string command_text;
            if (parse_string(value, command_text) == nullptr) {
                return std::unexpected(
                    parse_error_at(path, line_number,
                                   "malformed string value for \"command\""));
            }
            config.command = command_text;
            continue;
        }

        // Known-shape line for an undocumented key: warn, don't fail.
        if (warnings != nullptr) {
            warnings->push_back("config '" + path + "': ignoring unknown key \""
                                + std::string(key) + "\"");
        }
    }

    if (capture != Capture::None) {
        return std::unexpected(parse_error_at(
            path, capture_start_line,
            "unterminated multi-line string (no closing " + capture_key + " "
                + std::string(capture == Capture::Literal ? "'''" : "\"\"\"")
                + ")"));
    }

    // Contract §3: values carry explicit constraints.
    if (config.sensitivity <= 0.0) {
        std::ostringstream detail;
        detail << "config '" << path << "': key \"sensitivity\" must be > 0 (got "
               << config.sensitivity << ")";
        return std::unexpected(ConfigError{.detail = detail.str(), .path = path});
    }
    if (config.duration_sec <= 0.0) {
        std::ostringstream detail;
        detail << "config '" << path << "': key \"duration\" must be > 0 (got "
               << config.duration_sec << ")";
        return std::unexpected(ConfigError{.detail = detail.str(), .path = path});
    }
    if (config.cooldown_sec < 0.0) {
        std::ostringstream detail;
        detail << "config '" << path << "': key \"cooldown\" must be >= 0 (got "
               << config.cooldown_sec << ")";
        return std::unexpected(ConfigError{.detail = detail.str(), .path = path});
    }
    if (config.command.empty()) {
        return std::unexpected(ConfigError{
            .detail = "config '" + path + "': key \"command\" is required",
            .path = path,
        });
    }

    return config;
}

std::expected<Config, ConfigError> load_config(std::vector<std::string>* warnings)
{
    const std::filesystem::path path = config_path();
    const std::string path_str = path.string();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (errno != ENOENT) {
            // Existing but unreadable (permissions etc.): contract §3 #1.
            return std::unexpected(ConfigError{
                .detail = "cannot read '" + path_str + "': " + std::strerror(errno),
                .path = path_str,
            });
        }
        // Missing file → auto-initialize (contract §1, user-requested
        // amendment). Never overwrites an existing file; a write failure
        // is fatal with its own error shape.
        if (!create_default_config(path)) {
            return std::unexpected(ConfigError{
                .detail = "cannot write default config '" + path_str + "': "
                              + std::strerror(errno),
                .path = path_str,
            });
        }
        if (warnings != nullptr) {
            warnings->push_back("created default config '" + path_str
                                + "'; edit its \"command\" to set the shake "
                                  "action");
        }
        file.open(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(ConfigError{
                .detail = "cannot read '" + path_str + "': "
                              + std::strerror(errno),
                .path = path_str,
            });
        }
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string contents_str = contents.str();

    // An untouched v1-generated template carries a sensitivity value in
    // v1 units (units/second), which v2 reads as pointer units per swing
    // leg — an impossible threshold that silently swallowed every real
    // shake (user-validated regression). Migrate it verbatim; hand-edited
    // files are never rewritten.
    if (migrate_v1_default_config(path, contents_str)) {
        // Parse the migrated content (the disk file and the returned
        // config must agree).
        contents_str.assign(kDefaultConfigTemplate);
        if (warnings != nullptr) {
            warnings->push_back("migrated v1 default config '" + path_str
                                + "' to the v2 swing-based sensitivity "
                                  "(sensitivity = 40.0 pointer units per "
                                  "swing leg)");
        }
    }

    return parse_config(path_str, contents_str, warnings);
}

DetectionParams to_detection_params(const Config& config)
{
    constexpr double kUsecPerSec = 1'000'000.0;
    return DetectionParams{
        .sensitivity = config.sensitivity,
        .duration_usec = static_cast<int64_t>(config.duration_sec * kUsecPerSec),
        .cooldown_usec = static_cast<int64_t>(config.cooldown_sec * kUsecPerSec),
    };
}

} // namespace shaker
