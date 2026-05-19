/// @file   tests/test_doctor.cpp
/// @brief  Unit tests for `cmd_doctor`.
///
/// Drives the doctor against two fixtures rooted at a temporary
/// `XDG_DATA_HOME`:
///
///   1. clean — identity present (32 zero bytes, enough to clear the
///      "implausible size" check), plugin dir populated with a fake
///      .so, manifest references that .so with a real sha256, config
///      references the identity / manifest paths above. Doctor must
///      exit 0.
///
///   2. broken — config references a nonexistent identity path. Doctor
///      must exit 1 (the broken `identity.path` reference is flagged
///      as `[error]`).
///
/// The test uses no external framework — POSIX `assert()` + explicit
/// exit codes. Each fixture sets `XDG_DATA_HOME` / `HOME` /
/// `XDG_RUNTIME_DIR` to a fresh tmp dir, runs `cmd_doctor`, asserts
/// on the return code. stdout is redirected to /dev/null so the
/// CTest log stays readable.

#include "../subcommands.hpp"
#include "../subcommands/sha256_file.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace gn::apps::goodnet;

namespace {

[[nodiscard]] fs::path make_tmp_root() {
    /// `mkdtemp` + cleanup-by-test-driver (CTest discards the dir on
    /// success; on failure the dir is left for postmortem).
    char tmpl[] = "/tmp/goodnetd-doctor-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    assert(dir != nullptr);
    return fs::path{dir};
}

void redirect_stdout_to_null() {
    /// Quiet doctor's output during the test — both `--json` and the
    /// human report fire to stdout, and the CTest log is more
    /// readable without them.
    const int fd = ::open("/dev/null", O_WRONLY);
    assert(fd >= 0);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
}

void seed_clean_fixture(const fs::path& data_root) {
    /// Layout under data_root (=== $XDG_DATA_HOME):
    ///   data_root/goodnet/identity/default.bin   (32 bytes)
    ///   data_root/goodnet/plugins/dummy.so       (real file)
    ///   data_root/goodnet/manifests/baseline.json (refs dummy.so)
    ///   data_root/goodnet/config.json            (refs identity + manifest)
    const auto gd = data_root / "goodnet";
    fs::create_directories(gd / "identity");
    fs::create_directories(gd / "plugins");
    fs::create_directories(gd / "manifests");

    /// Identity blob: 32 bytes is the lower bound of doctor's
    /// plausibility window. Content does not matter — doctor never
    /// decodes the seed.
    {
        std::ofstream f(gd / "identity" / "default.bin", std::ios::binary);
        const std::vector<char> buf(32, 'x');
        f.write(buf.data(), buf.size());
    }
    /// Fake plugin .so — just a non-empty regular file with a .so
    /// extension. Doctor only counts `.so` files in the plugin dir.
    {
        std::ofstream f(gd / "plugins" / "dummy.so", std::ios::binary);
        f << "ELF-NOT-REALLY";
    }
    /// Manifest references dummy.so with its real SHA-256 so the
    /// hash-cross-check inside `check_manifest` passes.
    std::uint8_t digest[32]{};
    const std::string so_path = (gd / "plugins" / "dummy.so").string();
    const bool hashed = sha256_file(so_path, digest);
    assert(hashed);
    nlohmann::json manifest = {
        {"plugins", nlohmann::json::array({
            {
                {"path",   so_path},
                {"sha256", sha256_hex(digest)},
            },
        })},
    };
    {
        std::ofstream f(gd / "manifests" / "baseline.json", std::ios::binary);
        f << manifest.dump(2);
    }
    /// Config references the identity + manifest above.
    nlohmann::json cfg = {
        {"identity", {
            {"path", (gd / "identity" / "default.bin").string()},
        }},
        {"manifest", {
            {"path", (gd / "manifests" / "baseline.json").string()},
        }},
    };
    {
        std::ofstream f(gd / "config.json", std::ios::binary);
        f << cfg.dump(2);
    }
}

void seed_broken_fixture(const fs::path& data_root) {
    /// Same layout as clean, but config.json's `identity.path`
    /// points at a file that does not exist — doctor must flag
    /// that as `[error]` and exit 1.
    seed_clean_fixture(data_root);
    const auto gd = data_root / "goodnet";
    nlohmann::json cfg = {
        {"identity", {
            {"path", (gd / "identity" / "does-not-exist.bin").string()},
        }},
        {"manifest", {
            {"path", (gd / "manifests" / "baseline.json").string()},
        }},
    };
    std::ofstream f(gd / "config.json",
                    std::ios::binary | std::ios::trunc);
    f << cfg.dump(2);
}

void setenv_or_die(const char* k, const std::string& v) {
    const int rc = ::setenv(k, v.c_str(), 1);
    assert(rc == 0);
}

[[nodiscard]] int run_doctor() {
    /// Build an argv-equivalent span and call `cmd_doctor` directly.
    const std::span<const std::string_view> empty{};
    return cmd_doctor(empty);
}

void test_clean_returns_zero() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    /// Force runtime dir to a path that almost certainly has no
    /// goodnetd.sock — keeps the control-socket check at `[warn]`
    /// (warnings do not flip the exit code).
    setenv_or_die("XDG_RUNTIME_DIR", root.string());
    seed_clean_fixture(root);
    const int rc = run_doctor();
    assert(rc == 0 && "clean fixture must exit 0");
    (void)std::fprintf(stderr, "[ok] test_clean_returns_zero (root=%s)\n",
                       root.string().c_str());
    fs::remove_all(root);
}

void test_broken_returns_nonzero() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    setenv_or_die("XDG_RUNTIME_DIR", root.string());
    seed_broken_fixture(root);
    const int rc = run_doctor();
    assert(rc == 1 && "broken-config fixture must exit 1");
    (void)std::fprintf(stderr, "[ok] test_broken_returns_nonzero (root=%s)\n",
                       root.string().c_str());
    fs::remove_all(root);
}

/// Provider-backend doctor test seam state.
/// Captures the (extension_id, key_label) the doctor's
/// `check_identity_provider` queries, and a programmable return code
/// the next stub call surfaces. Tests set `g_stub_rc` before driving
/// the doctor and inspect `g_stub_called_*` after.
int g_stub_rc           = 0;
int g_stub_call_count   = 0;
std::string g_stub_called_extension;
std::string g_stub_called_label;

int provider_query_stub(const std::string& extension_id,
                         const std::string& key_label) {
    ++g_stub_call_count;
    g_stub_called_extension = extension_id;
    g_stub_called_label     = key_label;
    return g_stub_rc;
}

void seed_provider_descriptor(const fs::path& data_root,
                               bool             with_pin_env) {
    /// Build a `identity-config.json` next to the file-backed fixture
    /// `seed_clean_fixture` laid down. Doctor reports BOTH paths —
    /// the file-backed `[ok]` line and the provider-backend overlay
    /// — when both exist.
    const auto gd = data_root / "goodnet";
    nlohmann::json doc = {
        {"backend",      "provider"},
        {"extension_id", "gn.identity.pkcs11"},
        {"key_label",    "goodnet"},
    };
    if (with_pin_env) {
        doc["extra"] = {
            {"pin_env",     "GOODNETD_TEST_PKCS11_PIN"},
            {"module_path", "/usr/lib/softhsm/libsofthsm2.so"},
        };
    } else {
        doc["extra"] = {
            {"module_path", "/usr/lib/softhsm/libsofthsm2.so"},
        };
    }
    std::ofstream f(gd / "identity-config.json", std::ios::binary);
    f << doc.dump(2);
}

void test_provider_backend_shown_as_ok() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    setenv_or_die("XDG_RUNTIME_DIR", root.string());
    /// PIN env present + non-empty: the PIN-env sub-check should land
    /// at `[ok]` rather than `[warn]`.
    setenv_or_die("GOODNETD_TEST_PKCS11_PIN", "1234");
    seed_clean_fixture(root);
    seed_provider_descriptor(root, /*with_pin_env=*/true);

    g_stub_rc = 0;
    g_stub_call_count = 0;
    g_stub_called_extension.clear();
    g_stub_called_label.clear();
    set_doctor_provider_query_hook_for_test(&provider_query_stub);

    const int rc = run_doctor();
    set_doctor_provider_query_hook_for_test(nullptr);

    /// `[ok]` reachability + PIN env set + file-backed identity also
    /// present => no `[error]` findings => exit 0.
    assert(rc == 0 && "provider backend with reachable extension must exit 0");
    assert(g_stub_call_count == 1 && "doctor must query the stub exactly once");
    assert(g_stub_called_extension == "gn.identity.pkcs11");
    assert(g_stub_called_label == "goodnet");
    (void)std::fprintf(stderr,
        "[ok] test_provider_backend_shown_as_ok (root=%s)\n",
        root.string().c_str());
    fs::remove_all(root);
}

void test_provider_backend_missing_pin_env_warns() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    setenv_or_die("XDG_RUNTIME_DIR", root.string());
    /// Force the PIN env to be empty — the missing-env warn path is
    /// exactly what we want to exercise.
    ::unsetenv("GOODNETD_TEST_PKCS11_PIN");
    seed_clean_fixture(root);
    seed_provider_descriptor(root, /*with_pin_env=*/true);

    g_stub_rc = 0;  // extension reachable
    g_stub_call_count = 0;
    set_doctor_provider_query_hook_for_test(&provider_query_stub);

    const int rc = run_doctor();
    set_doctor_provider_query_hook_for_test(nullptr);

    /// PIN env missing is `[warn]` (not `[error]`); doctor exits 0
    /// because no `[error]` findings landed.
    assert(rc == 0 && "missing PIN env must warn, not error");
    assert(g_stub_call_count == 1);
    (void)std::fprintf(stderr,
        "[ok] test_provider_backend_missing_pin_env_warns (root=%s)\n",
        root.string().c_str());
    fs::remove_all(root);
}

void test_provider_backend_unreachable_errors() {
    const auto root = make_tmp_root();
    setenv_or_die("XDG_DATA_HOME", root.string());
    setenv_or_die("HOME", root.string());
    setenv_or_die("XDG_RUNTIME_DIR", root.string());
    setenv_or_die("GOODNETD_TEST_PKCS11_PIN", "1234");
    seed_clean_fixture(root);
    seed_provider_descriptor(root, /*with_pin_env=*/true);

    g_stub_rc = -1;  // extension NOT reachable
    g_stub_call_count = 0;
    set_doctor_provider_query_hook_for_test(&provider_query_stub);

    const int rc = run_doctor();
    set_doctor_provider_query_hook_for_test(nullptr);

    /// Unreachable extension => `[error]` => doctor exits 1.
    assert(rc == 1 && "unreachable provider extension must surface as error");
    (void)std::fprintf(stderr,
        "[ok] test_provider_backend_unreachable_errors (root=%s)\n",
        root.string().c_str());
    fs::remove_all(root);
}

}  // namespace

int main() {
    redirect_stdout_to_null();
    test_clean_returns_zero();
    test_broken_returns_nonzero();
    test_provider_backend_shown_as_ok();
    test_provider_backend_missing_pin_env_warns();
    test_provider_backend_unreachable_errors();
    (void)std::fprintf(stderr, "test_doctor: all checks passed\n");
    return 0;
}
