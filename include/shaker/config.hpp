// Configuration module (FR-002…FR-006, FR-010…FR-012).
//
// Reads exactly one TOML file per invocation, resolved from
// $XDG_CONFIG_HOME/shaker/config.toml (fallback $HOME/.config/shaker/
// config.toml). Contract: contracts/config-schema.md. Parsed values are
// converted to DetectionParams; all failures are return-value errors
// (std::expected) with contract-shaped messages — no exceptions leave this
// module.
#pragma once

#include <shaker/detection.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace shaker {

struct Config {
    // Documented defaults (contracts/config-schema.md §2, v2 semantics).
    double sensitivity = 4.0;           // shake factor: path / trail diagonal
    double duration_sec = 1.0;
    double cooldown_sec = 2.0;
    std::string command;                // no default — required (US3)
};

struct ConfigError {
    // Single-line detail WITHOUT the `shaker: ` prefix, shaped exactly as
    // contracts/config-schema.md §3 (e.g. `config '/path': key "duration"
    // must be > 0 (got -1)`). The runtime adds the `shaker: ` prefix.
    std::string detail;
    // Resolved file path of the failure (for diagnostics).
    std::string path;
};

// Absolute location of the configuration file (contract §1):
//   $XDG_CONFIG_HOME/shaker/config.toml
// with $HOME/.config/shaker/config.toml when the variable is unset, empty,
// or not absolute.
[[nodiscard]] std::filesystem::path config_path();

// Parse TOML *text* straight from memory (used by tests and by load below).
// Unknown top-level keys produce warning entries in `warnings` (non-fatal);
// pass nullptr to use the normal full behavior without collecting them.
[[nodiscard]] std::expected<Config, ConfigError>
parse_config(const std::string& path, const std::string& contents,
             std::vector<std::string>* warnings);

// Load the config from its resolved location (contract §3 error ladder);
// on success returns the parsed configuration. Unknown-key warnings are
// optionally collected into `warnings` (callers usually print them on
// stderr at startup); pass nullptr to skip collection.
[[nodiscard]] std::expected<Config, ConfigError>
load_config(std::vector<std::string>* warnings = nullptr);

// Convert to the pure core's parameter set (seconds → microseconds).
[[nodiscard]] DetectionParams to_detection_params(const Config& config);

} // namespace shaker
