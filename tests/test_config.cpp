// Unit tests for the configuration module: TOML parsing, defaults, the
// contract §3 error ladder, and the XDG path resolution rules (R-03).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shaker/config.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <cstdio>

namespace {

using shaker::ConfigError;
using shaker::Config;

// TOML document with all four documented keys (contract §2).
constexpr std::string_view kFullConfig = R"toml(
# How fast movement must be, in device units per second, to count toward a shake.
sensitivity = 8000.0

# How long (seconds) movement must stay above sensitivity to be a shake.
duration = 0.25

# How long (seconds) to ignore further shakes after one triggers.
cooldown = 2.0

# Run through /bin/sh -c when a shake triggers.
command = "notify-send 'Shake detected'"
)toml";

constexpr std::string_view kMinimalConfig =
    "command = \"echo hi\"\n";

} // namespace

TEST_CASE("US1: full config parses all four keys with the documented values")
{
    const auto parse = shaker::parse_config("/tmp/x.toml",
                                            std::string(kFullConfig), nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->sensitivity == doctest::Approx(8000.0));
    CHECK(parse->duration_sec == doctest::Approx(0.25));
    CHECK(parse->cooldown_sec == doctest::Approx(2.0));
    CHECK(parse->command == "notify-send 'Shake detected'");

    // seconds → µs conversion
    const auto params = shaker::to_detection_params(*parse);
    CHECK(params.sensitivity == doctest::Approx(8000.0));
    CHECK(params.duration_usec == 250'000);
    CHECK(params.cooldown_usec == 2'000'000);
}

TEST_CASE("US3-3 / SC-003: omitted numeric keys fall back to documented defaults")
{
    const auto parse = shaker::parse_config("/tmp/x.toml",
                                            std::string(kMinimalConfig), nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->sensitivity == doctest::Approx(4.0));
    CHECK(parse->duration_sec == doctest::Approx(1.0));
    CHECK(parse->cooldown_sec == doctest::Approx(2.0));
    CHECK(parse->command == "echo hi");
}

TEST_CASE("US3 #3: a missing or empty command is fatal (no safe default)")
{
    const ConfigError* err = nullptr;
    {
        auto parse = shaker::parse_config("/tmp/x.toml", "duration = 0.5\n", nullptr);
        REQUIRE(!parse.has_value());
        err = &parse.error();
    }
    CHECK(err->detail.find("key \"command\" is required") != std::string::npos);

    {
        auto parse = shaker::parse_config("/tmp/x.toml", "command = \"\"\n", nullptr);
        REQUIRE(!parse.has_value());
        CHECK(parse.error().detail.find("key \"command\" is required")
              != std::string::npos);
    }
}

TEST_CASE("US3 #2: broken TOML syntax produces a parse error shaped per contract")
{
    // A line without '=' is a bare syntax error under the v1 parser scope.
    auto parse = shaker::parse_config("/tmp/broken.toml",
                                      "a malformed line\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().detail.find("config parse error '/tmp/broken.toml'")
          != std::string::npos);

    // A malformed value is at least an error (classified in this parser as
    // a wrong-type message; see contract §3 #7).
    auto weird = shaker::parse_config("/tmp/broken.toml",
                                      "sensitivity = = 8000\n", nullptr);
    CHECK(!weird.has_value());
}

TEST_CASE("US3 #4–#6: numeric range violations carry key + value")
{
    {
        auto parse = shaker::parse_config("/tmp/x.toml",
                                          "sensitivity = -3\nduration = 1\ncooldown = 0\n"
                                          "command = ''",
                                          nullptr);
        REQUIRE(!parse.has_value());
        CHECK(parse.error().detail == "config '/tmp/x.toml': key \"sensitivity\" must be "
                                      "> 0 (got -3)");
    }
    {
        auto parse = shaker::parse_config("/tmp/x.toml",
                                          "sensitivity = 1\nduration = 0\ncooldown = 0\n"
                                          "command = \"x\"",
                                          nullptr);
        REQUIRE(!parse.has_value());
        CHECK(parse.error().detail == "config '/tmp/x.toml': key \"duration\" must be "
                                      "> 0 (got 0)");
    }
    {
        auto parse = shaker::parse_config("/tmp/x.toml",
                                          "sensitivity = 1\nduration = 1\ncooldown = -0.5\n"
                                          "command = \"x\"",
                                          nullptr);
        REQUIRE(!parse.has_value());
        CHECK(parse.error().detail == "config '/tmp/x.toml': key \"cooldown\" must be "
                                      ">= 0 (got -0.5)");
    }
}

TEST_CASE("US3 #7: wrong TOML type for a known key is an error")
{
    auto parse = shaker::parse_config("/tmp/x.toml", "sensitivity = 'fast'\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().detail.find("key \"sensitivity\" has wrong type")
          != std::string::npos);

    auto parse2 = shaker::parse_config("/tmp/x.toml", "command = 42\n", nullptr);
    REQUIRE(!parse2.has_value());
    CHECK(parse2.error().detail.find("key \"command\" has wrong type")
          != std::string::npos);
}

TEST_CASE("unknown keys warn but do not fail (contract §2)")
{
    std::vector<std::string> warnings;
    auto parse = shaker::parse_config(
        "/tmp/extra.toml",
        "sensitivity = 1\nduration = 1\ncooldown = 0\ncommand = 'x'\n"
        "duracion = 0.9\n",
        &warnings);
    REQUIRE(parse.has_value());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "config '/tmp/extra.toml': ignoring unknown key "
                         "\"duracion\"");
}

TEST_CASE("US3: every error detail names the resolved path verbatim")
{
    const std::string path = "/tmp/opencode/cfg/verbatim/config.toml";
    auto parse = shaker::parse_config(path, "duration = 0\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().path == path);
    CHECK(parse.error().detail.find("config '" + path
                                    + "': key \"duration\" ") != std::string::npos);
}

TEST_CASE("multi-line literal string carries a full shell script verbatim")
{
    // Full sh support (otter-launcher-style exec via /bin/sh -c): natural
    // shell text, including double quotes, subshells, pipes and '#'.
    const auto parse = shaker::parse_config("/tmp/x.toml", R"toml(
sensitivity = 40.0
duration = 0.25
cooldown = 2.0
command = '''
grim -g "$(slurp)" "$HOME/Pictures/shake-$(date +%s).png"
notify-send "saved #$(date +%s)"
'''
)toml", nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->command
          == "grim -g \"$(slurp)\" \"$HOME/Pictures/shake-$(date +%s).png\"\n"
             "notify-send \"saved #$(date +%s)\"\n");
}

TEST_CASE("multi-line literal: single-line form with terminator on the opening line")
{
    const auto parse = shaker::parse_config(
        "/tmp/x.toml", "command = '''echo one-liner'''\n", nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->command == "echo one-liner");
}

TEST_CASE("multi-line basic string processes escapes and trims the first newline")
{
    // TOML basic-string semantics: \n/\t are escapes; a backslash at EOL
    // trims all whitespace up to the next content (strict TOML
    // continuation). For shell scripts prefer ''' (verbatim) — see the
    // literal-string test above.
    const auto parse = shaker::parse_config("/tmp/x.toml", R"toml(
command = """
echo one\
     two
printf '%s\n' "tab\there"
"""
)toml", nullptr);
    REQUIRE(parse.has_value());
    // "one" and "two" fold onto one line with the whitespace between them
    // trimmed (strict TOML); \n inside the string is a real newline and
    // \t a real tab.
    CHECK(parse->command
          == "echo onetwo\nprintf '%s\n' \"tab\there\"\n");
}

TEST_CASE("multi-line: unterminated string reports the opening line")
{
    auto parse = shaker::parse_config("/tmp/x.toml", "command = '''echo hi\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().detail.find("unterminated multi-line string") != std::string::npos);
    CHECK(parse.error().detail.find("line 1") != std::string::npos);
}

TEST_CASE("multi-line: trailing content after the terminator is an error")
{
    auto parse = shaker::parse_config(
        "/tmp/x.toml", "command = '''echo hi''' extra\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().detail.find("trailing content") != std::string::npos);
}

TEST_CASE("multi-line string on a numeric key is a wrong-type error")
{
    auto parse = shaker::parse_config("/tmp/x.toml", "duration = '''3'''\n", nullptr);
    REQUIRE(!parse.has_value());
    CHECK(parse.error().detail.find("key \"duration\" has wrong type: expected number")
          != std::string::npos);
}

TEST_CASE("grim regression: single-line TOML literal with double-quoted subshell")
{
    // The exact fix for the reported broken command: $(slurp) must be
    // double-quoted AT THE SHELL LEVEL; the TOML literal string (single
    // quotes) passes the double quotes through untouched.
    const auto parse = shaker::parse_config(
        "/tmp/x.toml",
        "command = 'grim -g \"$(slurp)\" \"$HOME/Pictures/shake-$(date +%s).png\"'\n",
        nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->command
          == "grim -g \"$(slurp)\" \"$HOME/Pictures/shake-$(date +%s).png\"");
}

TEST_CASE("CRLF line endings parse cleanly")
{
    const auto parse = shaker::parse_config("/tmp/x.toml",
                                            "sensitivity = 5\r\ncommand = \"true\"\r\n",
                                            nullptr);
    REQUIRE(parse.has_value());
    CHECK(parse->sensitivity == doctest::Approx(5.0));
    CHECK(parse->command == "true");
}

TEST_CASE("US3: unsupported TOML constructs fail with parse errors")
{
    // A table (documented parser scope: flat key = value only).
    auto table = shaker::parse_config("/tmp/t.toml", "[shake]\n", nullptr);
    REQUIRE(!table.has_value());
    CHECK(table.error().detail.find("config parse error") != std::string::npos);

    // An inline array value.
    auto array = shaker::parse_config("/tmp/t.toml", "tags = [\"a\"]\n", nullptr);
    REQUIRE(!array.has_value());
    CHECK(array.error().detail.find("config parse error") != std::string::npos);
}

TEST_CASE("R-03: config_path honors absolute XDG_CONFIG_HOME")
{
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfgtest", 1);
    const auto path = shaker::config_path();
    CHECK(path == "/tmp/opencode/cfgtest/shaker/config.toml");
}

TEST_CASE("R-03: XDG_CONFIG_HOME unset / empty / relative → $HOME/.config fallback")
{
    // unset
    ::unsetenv("XDG_CONFIG_HOME");
    ::setenv("HOME", "/home/testuser", 1);
    CHECK(shaker::config_path() == "/home/testuser/.config/shaker/config.toml");

    // empty
    ::setenv("XDG_CONFIG_HOME", "", 1);
    CHECK(shaker::config_path() == "/home/testuser/.config/shaker/config.toml");

    // relative (not absolute) must be ignored per the XDG spec
    ::setenv("XDG_CONFIG_HOME", "relative/path", 1);
    CHECK(shaker::config_path() == "/home/testuser/.config/shaker/config.toml");
}

TEST_CASE("US3: load path — unknown key warning is non-fatal, defaults apply")
{
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/warnings", 1);
    std::error_code mkdir_ec;
    std::filesystem::create_directories("/tmp/opencode/cfg/warnings/shaker",
                                        mkdir_ec);
    std::ofstream file("/tmp/opencode/cfg/warnings/shaker/config.toml");
    file << "# comment only\n\n"
         << "command = \"true\"\n"
         << "duracion = 0.9\n";
    file.close();

    std::vector<std::string> warnings;
    auto loaded = shaker::load_config(&warnings);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sensitivity == doctest::Approx(4.0)); // default (FR-012)
    CHECK(warnings.size() == 1);
    CHECK(warnings[0].find("ignoring unknown key \"duracion\"") != std::string::npos);
    CHECK(warnings[0].find("/tmp/opencode/cfg/warnings/shaker/config.toml")
          != std::string::npos);
}

TEST_CASE("US3: a missing config file is auto-initialized with defaults")
{
    // Fresh location: the file (and parent dirs) must be created.
    std::error_code rm_ec;
    std::filesystem::remove_all("/tmp/opencode/cfg/autocreate", rm_ec);
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/autocreate/fresh", 1);
    std::vector<std::string> warnings;
    auto loaded = shaker::load_config(&warnings);
    REQUIRE(loaded.has_value());
    CHECK(std::filesystem::exists("/tmp/opencode/cfg/autocreate/fresh/shaker/config.toml"));
    CHECK(loaded->sensitivity == doctest::Approx(4.0));
    CHECK(loaded->duration_sec == doctest::Approx(1.0));
    CHECK(loaded->cooldown_sec == doctest::Approx(2.0));
    CHECK(loaded->command == "notify-send 'Shake detected'");
    REQUIRE(!warnings.empty());
    CHECK(warnings[0].find("created default config") != std::string::npos);

    // Re-running does not warn again and never overwrites.
    std::vector<std::string> second_run_warnings;
    auto again = shaker::load_config(&second_run_warnings);
    REQUIRE(again.has_value());
    CHECK(again->command == "notify-send 'Shake detected'");
    CHECK(second_run_warnings.empty());
}

TEST_CASE("US3: an unwritable location fails with 'cannot write default config'")
{
    // /proc never accepts new directories for an unprivileged process.
    ::setenv("XDG_CONFIG_HOME", "/proc/shaker-cannot-create", 1);
    auto failed = shaker::load_config(nullptr);
    REQUIRE(!failed.has_value());
    CHECK(failed.error().detail.find("cannot write default config")
          != std::string::npos);
    CHECK(failed.error().detail.find("/proc/shaker-cannot-create/shaker/config.toml")
          != std::string::npos);
}

TEST_CASE("US3: a v1-template config is auto-migrated to the current units")
{
    // Install the exact v1-generated template (old units: units/second).
    std::error_code rm_ec;
    std::filesystem::remove_all("/tmp/opencode/cfg/migrate", rm_ec);
    std::filesystem::create_directories("/tmp/opencode/cfg/migrate/shaker");
    {
        std::ofstream file("/tmp/opencode/cfg/migrate/shaker/config.toml");
        file << "# shaker configuration (auto-generated on first run)\n"
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
    }
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/migrate", 1);

    std::vector<std::string> warnings;
    auto loaded = shaker::load_config(&warnings);
    REQUIRE(loaded.has_value());
    // The migrated template carries the current default threshold.
    CHECK(loaded->sensitivity == doctest::Approx(4.0));
    bool migrated = false;
    for (const std::string& warning : warnings) {
        if (warning.find("migrated old default config") != std::string::npos) {
            migrated = true;
        }
    }
    CHECK(migrated);

    // Second load: the file is now the current template, nothing to migrate.
    std::vector<std::string> second_warnings;
    auto again = shaker::load_config(&second_warnings);
    REQUIRE(again.has_value());
    CHECK(again->sensitivity == doctest::Approx(4.0));
    for (const std::string& warning : second_warnings) {
        CHECK(warning.find("migrated") == std::string::npos);
    }
}

TEST_CASE("US3: a hand-edited config is never migrated or rewritten")
{
    std::error_code rm_ec;
    std::filesystem::remove_all("/tmp/opencode/cfg/nomigrate", rm_ec);
    std::filesystem::create_directories("/tmp/opencode/cfg/nomigrate/shaker");
    const std::string user_config =
        "# my own tuning\n"
        "sensitivity = 8000.0\n"
        "duration = 0.5\n"
        "command = \"echo mine\"\n";
    {
        std::ofstream file("/tmp/opencode/cfg/nomigrate/shaker/config.toml");
        file << user_config;
    }
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/nomigrate", 1);

    std::vector<std::string> warnings;
    auto loaded = shaker::load_config(&warnings);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sensitivity == doctest::Approx(8000.0)); // untouched
    CHECK(loaded->command == "echo mine");
    // Only the stale-value hint, never a migration.
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("is a shake factor") != std::string::npos);
    std::ifstream reread("/tmp/opencode/cfg/nomigrate/shaker/config.toml");
    const std::string on_disk((std::istreambuf_iterator<char>(reread)),
                              std::istreambuf_iterator<char>());
    CHECK(on_disk == user_config);
}

TEST_CASE("US3: a v2-template config is auto-migrated to the v3 shake factor")
{
    std::error_code rm_ec;
    std::filesystem::remove_all("/tmp/opencode/cfg/migrate2", rm_ec);
    std::filesystem::create_directories("/tmp/opencode/cfg/migrate2/shaker");
    {
        std::ofstream file("/tmp/opencode/cfg/migrate2/shaker/config.toml");
        file << "# shaker configuration (auto-generated on first run)\n"
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
    }
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/migrate2", 1);

    std::vector<std::string> warnings;
    auto loaded = shaker::load_config(&warnings);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sensitivity == doctest::Approx(4.0));
    CHECK(loaded->duration_sec == doctest::Approx(1.0));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("migrated old default config") != std::string::npos);
}

TEST_CASE("US3 #1: an existing unreadable file still errors with cannot read")
{
    // Create a target parent we cannot enter, so the config path itself is
    // unreadable (and NOT missing — auto-init must not kick in).
    ::setenv("XDG_CONFIG_HOME", "/tmp/opencode/cfg/readfail", 1);
    std::filesystem::create_directories("/tmp/opencode/cfg/readfail/shaker");
    std::ofstream base("/tmp/opencode/cfg/readfail/shaker/config.toml");
    base << "command = \"true\"\n";
    base.close();
    std::filesystem::permissions("/tmp/opencode/cfg/readfail/shaker",
                                 std::filesystem::perms::none);
    auto failed = shaker::load_config(nullptr);
    std::filesystem::permissions("/tmp/opencode/cfg/readfail/shaker",
                                 std::filesystem::perms::owner_all);
    REQUIRE(!failed.has_value());
    CHECK(failed.error().detail.find("cannot read") != std::string::npos);
}
