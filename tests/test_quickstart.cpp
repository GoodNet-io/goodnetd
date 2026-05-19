/// @file   tests/test_quickstart.cpp
/// @brief  Unit tests for `cmd_quickstart --non-interactive`.
///
/// Drives quickstart against a temporary `XDG_DATA_HOME` and asserts
/// the side-effects:
///
///   1. quickstart returns 0 in non-interactive mode;
///   2. the default config.json is written under data_root/goodnet/;
///   3. the JSON parses and contains the documented top-level keys
///      (`identity.path`, `manifest_path`, `listeners[0].uri`);
///   4. re-running quickstart is idempotent — second invocation still
///      returns 0 and does not clobber the existing config.
///
/// Identity generation in quickstart goes through `cmd_identity`, which
/// links against the kernel SDK. The test does not link the kernel —
/// instead we pre-create the identity file so quickstart's step 1
/// notices it and prints "already configured", taking the cmd_identity
/// call out of the picture. A weak stub `cmd_identity` is provided
/// below for the linker; it should not be reached at runtime.

#include "../subcommands.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace gn::apps::goodnet {

/// Test-only `cmd_identity` stub. quickstart only reaches it when the
/// identity file is missing; the test pre-creates the file so this
/// stub stays dormant. If it is reached, fail loudly — that means
/// the idempotency contract broke.
int cmd_identity(std::span<const std::string_view> /*args*/) {
    (void)std::fputs("test_quickstart: stub cmd_identity hit unexpectedly\n",
                      stderr);
    return 1;
}

/// Doctor is declared in subcommands.hpp but the doctor TU is not in
/// this test's source list. Provide a no-op stub so the linker is
/// happy without dragging doctor.cpp + sha256 dependencies in.
int cmd_doctor(std::span<const std::string_view> /*args*/) {
    return 0;
}

/// Other subcommands are referenced via subcommands.hpp but never
/// called from the quickstart code path. Stub them out so the linker
/// closes — these never fire at test runtime.
int cmd_version(std::span<const std::string_view>)         { return 0; }
int cmd_config_validate(std::span<const std::string_view>) { return 0; }
int cmd_plugin_hash(std::span<const std::string_view>)     { return 0; }
int cmd_manifest_gen(std::span<const std::string_view>)    { return 0; }
int cmd_run(std::span<const std::string_view>)             { return 0; }

/// `cmd_identity_import_hsm` is reachable from quickstart's HSM
/// branch (option 2 in the interactive prompt). The non-interactive
/// test path never hits it; stub the symbol so the link closes.
int cmd_identity_import_hsm(std::span<const std::string_view>) {
    (void)std::fputs(
        "test_quickstart: stub cmd_identity_import_hsm hit unexpectedly\n",
        stderr);
    return 1;
}

/// Doctor's test seam — referenced from `subcommands.hpp` but never
/// reached from the quickstart test. No-op satisfies the linker.
void set_doctor_provider_query_hook_for_test(
    int (*)(const std::string&, const std::string&)) {}

}  // namespace gn::apps::goodnet

using namespace gn::apps::goodnet;

namespace {

[[nodiscard]] fs::path make_tmp_root() {
    char tmpl[] = "/tmp/goodnetd-quickstart-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    return fs::path{dir};
}

void redirect_stdout_to_null() {
    const int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
}

void seed_identity(const fs::path& data_root) {
    /// Pre-create the identity so quickstart's step 1 skips the
    /// kernel-linked cmd_identity path.
    const auto p = data_root / "goodnet" / "identity" / "default.bin";
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    const std::vector<char> buf(32, 'i');
    f.write(buf.data(), buf.size());
}

void setenv_or_die(const char* k, const std::string& v) {
    const int rc = ::setenv(k, v.c_str(), 1);
    assert(rc == 0);
}

[[nodiscard]] int run_quickstart_noninteractive() {
    const std::string_view flag{"--non-interactive"};
    const std::array<std::string_view, 1> argv = { flag };
    return cmd_quickstart(
        std::span<const std::string_view>{argv.data(), argv.size()});
}

void test_quickstart_produces_config() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    seed_identity(root);

    const int rc = run_quickstart_noninteractive();
    assert(rc == 0 && "quickstart --non-interactive must exit 0");

    const auto cfg_path = root / "goodnet" / "config.json";
    assert(fs::exists(cfg_path) && "config.json must be created");

    std::ifstream f(cfg_path);
    nlohmann::json doc;
    f >> doc;
    assert(doc.contains("identity"));
    assert(doc["identity"].contains("path"));
    assert(doc.contains("manifest_path"));
    assert(doc.contains("listeners"));
    assert(doc["listeners"].is_array());
    assert(doc["listeners"].size() == 1);
    assert(doc["listeners"][0].contains("uri"));

    (void)std::fprintf(stderr, "[ok] test_quickstart_produces_config (root=%s)\n",
                       root.string().c_str());
    fs::remove_all(root);
}

void test_quickstart_is_idempotent() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    seed_identity(root);

    /// First run produces the config file.
    int rc = run_quickstart_noninteractive();
    assert(rc == 0);
    const auto cfg_path = root / "goodnet" / "config.json";
    assert(fs::exists(cfg_path));

    /// Stamp the file so we can verify the second run does not rewrite.
    {
        std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
        f << "{\"sentinel\": true}";
    }

    /// Second run must skip (file exists) and still exit 0.
    rc = run_quickstart_noninteractive();
    assert(rc == 0 && "re-running quickstart must still exit 0");

    /// Sentinel must survive — confirms the skip path was taken
    /// rather than a blind rewrite.
    std::ifstream f(cfg_path);
    nlohmann::json doc;
    f >> doc;
    assert(doc.contains("sentinel"));
    assert(doc["sentinel"].get<bool>() == true);

    (void)std::fprintf(stderr, "[ok] test_quickstart_is_idempotent (root=%s)\n",
                       root.string().c_str());
    fs::remove_all(root);
}

}  // namespace

int main() {
    redirect_stdout_to_null();
    test_quickstart_produces_config();
    test_quickstart_is_idempotent();
    (void)std::fprintf(stderr, "test_quickstart: all checks passed\n");
    return 0;
}
