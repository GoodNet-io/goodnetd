{
  description = "goodnetd — operator-facing daemon + multicall CLI for the GoodNet kernel.";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # `goodnet-core` flake — the kernel published as a Nix package.
    # Pulls in `GoodNet::sdk` + `GoodNet::kernel` (libgoodnet_kernel.so
    # + transitive runtime deps). goodnetd consumes only the public
    # SDK surface from this input — no path-based reach into the
    # kernel monorepo's `core/` / `plugins/` subdirectories.
    goodnet.url = "github:GoodNet-io/goodnet";
  };

  outputs = { self, nixpkgs, flake-utils, goodnet }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs   = import nixpkgs { inherit system; };
        # goodnet-core derivation: ships GoodNet::sdk + GoodNet::kernel
        # CMake targets, plus libsodium / nlohmann_json / openssl /
        # asio / spdlog / fmt through propagatedBuildInputs. goodnetd
        # gets the whole closure at link time without listing each
        # transitive dep here.
        kernel = goodnet.packages.${system}.goodnet-core or goodnet.packages.${system}.default;
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname   = "goodnetd";
          version = "0.1.0";
          src     = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs       = [ kernel pkgs.libsodium pkgs.nlohmann_json ];
          meta = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license     = pkgs.lib.licenses.mit;
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [
            kernel
            pkgs.libsodium
            pkgs.nlohmann_json
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
        };
      });
}
