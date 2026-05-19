# goodnetd — BusyBox-style multicall CLI.
#
# Single binary covers every operator-facing subcommand (run / config
# validate / plugin hash / manifest gen / identity gen|show / version).
# Flat dispatch — `git`-style two-token (`<binary> <subcommand>`),
# never namespaced (no `goodnetd identity gen`-as-its-own-binary).
# Shared linkage of kernel/SDK, single PATH entry, no «which goodnetd
# bin do I have» systemd surprise.
#
# Downstream consumer rules: goodnetd is an SDK-only consumer of the
# GoodNet kernel. The CMake setup uses `find_package(GoodNet)` to
# pull in `GoodNet::sdk` (public headers) and `GoodNet::kernel`
# (libgoodnet_kernel.so + transitive runtime deps). No `core/` or
# `plugins/` private headers are reached — every kernel-side call
# goes through the C ABI exposed in `sdk/core.h`. libsodium is a
# transitive dependency of `GoodNet::kernel` and is used directly
# for the SHA-256 helper backing `plugin hash` / `manifest gen`.

cmake_minimum_required(VERSION 3.25)
project(goodnetd LANGUAGES CXX VERSION 0.1.0)

find_package(GoodNet REQUIRED)

# libsodium is already pulled in transitively through GoodNet::kernel
# (`GoodNetConfig.cmake.in` runs `pkg_check_modules(SODIUM ...)`), but
# we want it as a direct dependency of goodnetd too — the SHA-256
# helper for `plugin hash` / `manifest gen` consumes libsodium without
# touching anything else from the kernel surface.
include(CMakeFindDependencyMacro)
if(NOT TARGET PkgConfig::SODIUM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
endif()

# nlohmann_json is propagated from GoodNet::kernel; the JSON we parse
# for `config validate` is a pure structural well-formedness check,
# nothing deeper than "is this a JSON object".
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(goodnetd
    main.cpp
    subcommands/version.cpp
    subcommands/config_validate.cpp
    subcommands/plugin_hash.cpp
    subcommands/manifest_gen.cpp
    subcommands/identity.cpp
    subcommands/run.cpp
    subcommands/doctor.cpp
    subcommands/quickstart.cpp
)

target_link_libraries(goodnetd
    PRIVATE
        GoodNet::kernel
        PkgConfig::SODIUM
        nlohmann_json::nlohmann_json
)

target_compile_features(goodnetd PRIVATE cxx_std_23)

# Windows mingw: roll the C++/C runtime, libgcc, and winpthread
# into the executable so the shipped `goodnetd.exe` is a single file
# with no neighbouring .dll requirements. mingw provides static
# archives (`libstdc++.a`, `libgcc.a`, `libwinpthread.a`) alongside
# the .dll variants — `-static -static-libgcc -static-libstdc++`
# pins ld.bfd to the archives. Combined with `--disable-shared`
# rebuilds of spdlog / fmt / libsodium in `nix/goodnetd-windows.nix`,
# the final goodnetd.exe carries every dependency inline.
if(WIN32)
    target_link_options(goodnetd PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

# GOODNET_VERSION_STRING is set at compile time as a fallback for
# `goodnet version`; the runtime path prefers `gn_version()` from
# `sdk/core.h`, which reports the kernel's own version. The
# compile-time string is what ships in the goodnetd binary itself
# (project version above) — printed alongside the kernel version so
# operators can correlate a deployed goodnetd against the kernel it
# was built against.
target_compile_definitions(goodnetd
    PRIVATE
        "GOODNETD_VERSION_STRING=\"${PROJECT_VERSION}\""
)

set_target_properties(goodnetd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "goodnetd"
)

# `nix build .#default` lands the binary under `result/bin/` so
# composed deployments resolve `${goodnetd}/bin/goodnetd` through
# CMake's install layout.
install(TARGETS goodnetd RUNTIME DESTINATION bin)

# ---- Tests -----------------------------------------------------------------
#
# Tests are off by default to keep the slim production build cheap;
# `cmake -DGOODNETD_BUILD_TESTS=ON` (or the devShell's CI preset) flips
# them on. Each test is a self-contained executable that links against
# the same subcommand .cpp files goodnetd uses, so the test binary
# reuses the dispatch surface 1:1 with the shipped CLI. No external
# test framework — POSIX asserts + exit codes, dropped under
# `add_test()` so CTest runs them with the rest of the kernel suite.
option(GOODNETD_BUILD_TESTS "Build goodnetd unit tests" OFF)
if(GOODNETD_BUILD_TESTS)
    enable_testing()

    add_executable(test_doctor
        tests/test_doctor.cpp
        subcommands/doctor.cpp
    )
    target_link_libraries(test_doctor
        PRIVATE
            PkgConfig::SODIUM
            nlohmann_json::nlohmann_json
    )
    target_compile_features(test_doctor PRIVATE cxx_std_23)
    target_include_directories(test_doctor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME goodnetd.doctor COMMAND test_doctor)

    add_executable(test_quickstart
        tests/test_quickstart.cpp
        subcommands/quickstart.cpp
    )
    target_link_libraries(test_quickstart
        PRIVATE
            nlohmann_json::nlohmann_json
    )
    target_compile_features(test_quickstart PRIVATE cxx_std_23)
    target_include_directories(test_quickstart PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME goodnetd.quickstart COMMAND test_quickstart)
endif()
