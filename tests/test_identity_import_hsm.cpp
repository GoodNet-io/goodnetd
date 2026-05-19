/// @file   tests/test_identity_import_hsm.cpp
/// @brief  Unit tests for `cmd_identity_import_hsm`.
///
/// Three scenarios:
///   1. WritesCorrectConfig — invoke with fixture args, parse the
///      resulting JSON, assert backend / extension_id / key_label /
///      extra.pin_env / extra.module_path are populated as written.
///   2. RefusesClobberWithoutForce — second invocation with different
///      args against an existing descriptor must exit non-zero and
///      leave the descriptor unchanged.
///   3. MissingRequiredArgsRejected — argv lacking either
///      --extension-id or --key-label exits 2 (usage error).
///
/// The test pre-points `XDG_DATA_HOME` at a fresh tmp dir so the
/// production code path (which writes under `$XDG_DATA_HOME/goodnet/
/// identity-config.json`) lands in a sandbox we can clean up.

#include "../subcommands.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace gn::apps::goodnet {
/// Linker stubs for the other subcommands (the test pulls in only
/// `identity_import_hsm.cpp`, not the rest of the dispatch surface).
int cmd_version(std::span<const std::string_view>)           { return 0; }
int cmd_config_validate(std::span<const std::string_view>)   { return 0; }
int cmd_plugin_hash(std::span<const std::string_view>)       { return 0; }
int cmd_manifest_gen(std::span<const std::string_view>)      { return 0; }
int cmd_identity(std::span<const std::string_view>)          { return 0; }
int cmd_run(std::span<const std::string_view>)               { return 0; }
int cmd_doctor(std::span<const std::string_view>)            { return 0; }
int cmd_quickstart(std::span<const std::string_view>)        { return 0; }
void set_doctor_provider_query_hook_for_test(
    int (*)(const std::string&, const std::string&)) {}
}  // namespace gn::apps::goodnet

using namespace gn::apps::goodnet;

namespace {

[[nodiscard]] fs::path make_tmp_root() {
    char tmpl[] = "/tmp/goodnetd-import-hsm-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    return fs::path{dir};
}

void redirect_stdout_to_null() {
    /// Quiet stdout — the production code emits human banners there.
    /// stderr stays open: the test's own `[ok]` markers and any
    /// production diagnostics flow through it, both useful in the
    /// CTest log on failure.
    const int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
}

void setenv_or_die(const char* k, const std::string& v) {
    const int rc = ::setenv(k, v.c_str(), 1);
    assert(rc == 0);
}

[[nodiscard]] fs::path descriptor_path(const fs::path& root) {
    return root / "goodnet" / "identity-config.json";
}

void test_writes_correct_config() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());

    const std::array<std::string_view, 8> argv = {
        std::string_view{"--extension-id"},
        std::string_view{"gn.identity.pkcs11"},
        std::string_view{"--key-label"},
        std::string_view{"goodnet"},
        std::string_view{"--pin-env"},
        std::string_view{"GOODNET_PKCS11_PIN"},
        std::string_view{"--module"},
        std::string_view{"/usr/lib/softhsm/libsofthsm2.so"},
    };
    const int rc = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv.data(), argv.size()});
    assert(rc == 0 && "import-hsm with valid args must exit 0");

    const auto path = descriptor_path(root);
    assert(fs::exists(path) && "descriptor must be written");

    std::ifstream f(path);
    nlohmann::json doc;
    f >> doc;
    assert(doc.is_object());
    assert(doc.contains("backend"));
    assert(doc["backend"].get<std::string>() == "provider");
    assert(doc.contains("extension_id"));
    assert(doc["extension_id"].get<std::string>() == "gn.identity.pkcs11");
    assert(doc.contains("key_label"));
    assert(doc["key_label"].get<std::string>() == "goodnet");
    assert(doc.contains("extra"));
    assert(doc["extra"].is_object());
    assert(doc["extra"].contains("pin_env"));
    assert(doc["extra"]["pin_env"].get<std::string>() == "GOODNET_PKCS11_PIN");
    assert(doc["extra"].contains("module_path"));
    assert(doc["extra"]["module_path"].get<std::string>() ==
           "/usr/lib/softhsm/libsofthsm2.so");

    (void)std::fprintf(stderr,
        "[ok] test_writes_correct_config (root=%s)\n", root.string().c_str());
    fs::remove_all(root);
}

void test_refuses_clobber_without_force() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());

    /// First invocation lands a valid provider descriptor.
    const std::array<std::string_view, 4> argv1 = {
        std::string_view{"--extension-id"},
        std::string_view{"gn.identity.pkcs11"},
        std::string_view{"--key-label"},
        std::string_view{"goodnet"},
    };
    const int rc1 = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv1.data(), argv1.size()});
    assert(rc1 == 0 && "first import must succeed");

    /// Second invocation with a different key_label MUST fail without
    /// --force. The clobber gate is exactly what we are testing.
    const std::array<std::string_view, 4> argv2 = {
        std::string_view{"--extension-id"},
        std::string_view{"gn.identity.pkcs11"},
        std::string_view{"--key-label"},
        std::string_view{"different-label"},
    };
    const int rc2 = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv2.data(), argv2.size()});
    assert(rc2 != 0 && "second import without --force must fail");

    /// Verify the on-disk descriptor is the FIRST one (unchanged).
    std::ifstream f(descriptor_path(root));
    nlohmann::json doc;
    f >> doc;
    assert(doc["key_label"].get<std::string>() == "goodnet" &&
           "first descriptor must survive the refused clobber");

    /// Third invocation with the same args (re-running the command
    /// idempotently) should succeed even without --force.
    const int rc3 = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv1.data(), argv1.size()});
    assert(rc3 == 0 && "identical re-run must be accepted as idempotent");

    /// Fourth invocation with the different args BUT --force must
    /// succeed and overwrite.
    const std::array<std::string_view, 5> argv4 = {
        std::string_view{"--extension-id"},
        std::string_view{"gn.identity.pkcs11"},
        std::string_view{"--key-label"},
        std::string_view{"different-label"},
        std::string_view{"--force"},
    };
    const int rc4 = cmd_identity_import_hsm(
        std::span<const std::string_view>{argv4.data(), argv4.size()});
    assert(rc4 == 0 && "--force must permit overwrite");
    {
        std::ifstream f2(descriptor_path(root));
        nlohmann::json doc2;
        f2 >> doc2;
        assert(doc2["key_label"].get<std::string>() == "different-label");
    }

    (void)std::fprintf(stderr,
        "[ok] test_refuses_clobber_without_force (root=%s)\n",
        root.string().c_str());
    fs::remove_all(root);
}

void test_missing_required_args_rejected() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());

    /// Missing --extension-id.
    {
        const std::array<std::string_view, 2> argv = {
            std::string_view{"--key-label"},
            std::string_view{"goodnet"},
        };
        const int rc = cmd_identity_import_hsm(
            std::span<const std::string_view>{argv.data(), argv.size()});
        assert(rc == 2 && "missing --extension-id must exit 2");
    }
    /// Missing --key-label.
    {
        const std::array<std::string_view, 2> argv = {
            std::string_view{"--extension-id"},
            std::string_view{"gn.identity.pkcs11"},
        };
        const int rc = cmd_identity_import_hsm(
            std::span<const std::string_view>{argv.data(), argv.size()});
        assert(rc == 2 && "missing --key-label must exit 2");
    }
    /// Empty argv.
    {
        const std::span<const std::string_view> empty{};
        const int rc = cmd_identity_import_hsm(empty);
        assert(rc == 2 && "empty argv must exit 2");
    }
    /// Descriptor must NOT have been written for any of the rejected
    /// invocations — the clobber gate is post-validation, so a
    /// usage error never reaches disk.
    assert(!fs::exists(descriptor_path(root)) &&
           "no descriptor must be written on usage errors");

    (void)std::fprintf(stderr,
        "[ok] test_missing_required_args_rejected (root=%s)\n",
        root.string().c_str());
    fs::remove_all(root);
}

}  // namespace

int main() {
    redirect_stdout_to_null();
    test_writes_correct_config();
    test_refuses_clobber_without_force();
    test_missing_required_args_rejected();
    (void)std::fprintf(stderr,
        "test_identity_import_hsm: all checks passed\n");
    return 0;
}
