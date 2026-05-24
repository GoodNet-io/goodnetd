{
  description = "goodnetd — operator-facing daemon + multicall CLI for the GoodNet kernel.";

  inputs = {
    nixpkgs.url       = "github:NixOS/nixpkgs/nixos-unstable";
    goodnet.url       = "github:GoodNet-io/goodnet/dev";
    protocol-gnet.url = "github:GoodNet-io/protocol-gnet";
  };

  outputs = { self, nixpkgs, goodnet, protocol-gnet }:
    let
      allSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      setupDevScript = pkgs: pkgs.writeShellScript "setup-dev" ''
        set -euo pipefail
        mkdir -p .goodnet
        if [ ! -d .goodnet/goodnet ]; then
          echo "Cloning goodnet kernel..." >&2
          git clone git@github.com:GoodNet-io/goodnet.git .goodnet/goodnet
        fi
        if [ ! -d .goodnet/protocol-gnet ]; then
          echo "Cloning protocol-gnet..." >&2
          git clone git@github.com:GoodNet-io/protocol-gnet.git .goodnet/protocol-gnet
        fi
        echo ""
        echo "Dev shell with local overrides:"
        echo "  nix develop \\"
        echo "    --override-input goodnet path:.goodnet/goodnet \\"
        echo "    --override-input protocol-gnet path:.goodnet/protocol-gnet"
      '';

    in {
      packages = nixpkgs.lib.genAttrs allSystems (system:
        let
          pkgs   = import nixpkgs { inherit system; };
          kernel = goodnet.packages.${system}.goodnet-core
                or goodnet.packages.${system}.default;
          gnet   = protocol-gnet.packages.${system}.default;

          commonMeta = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license     = pkgs.lib.licenses.mit;
          };

          gnetStatic = pkgs.pkgsStatic.stdenv.mkDerivation {
            pname   = "goodnet-protocol-gnet";
            version = "1.0.0-rc6";
            src     = protocol-gnet;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs       = [ (goodnet.packages.${system}.goodnet-core-static
                                or kernel) ];
            cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" "-DBUILD_TESTING=OFF" ];
          };

        in {
          default = pkgs.stdenv.mkDerivation {
            pname   = "goodnetd";
            version = "0.1.0";
            src     = ./.;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs       = [ kernel gnet pkgs.libsodium pkgs.nlohmann_json ];
            meta = commonMeta;
          };
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          # Truly-static binary for `nix profile add .#static`.
          # Uses pkgsStatic (musl) for goodnetd + gnet; kernel pulls its
          # own static derivation from goodnet-core-static.
          static = pkgs.pkgsStatic.stdenv.mkDerivation {
            pname   = "goodnetd-static";
            version = "0.1.0";
            src     = ./.;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs = [
              (goodnet.packages.${system}.goodnet-core-static or kernel)
              gnetStatic
              pkgs.pkgsStatic.libsodium
              pkgs.pkgsStatic.nlohmann_json
            ];
            cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release"
                           "-DCMAKE_EXE_LINKER_FLAGS=-static" ];
            meta = commonMeta // { description = "Static goodnetd for nix profile add."; };
          };
        }
      );

      devShells = nixpkgs.lib.genAttrs allSystems (system:
        let
          pkgs   = import nixpkgs { inherit system; };
          kernel = goodnet.packages.${system}.goodnet-core
                or goodnet.packages.${system}.default;
          gnet   = protocol-gnet.packages.${system}.default;
        in {
          default = pkgs.mkShell {
            packages = [ kernel gnet
                         pkgs.libsodium pkgs.nlohmann_json
                         pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          };
        }
      );

      apps = nixpkgs.lib.genAttrs allSystems (system:
        let pkgs = import nixpkgs { inherit system; }; in {
          setup-dev = {
            type    = "app";
            program = "${setupDevScript pkgs}";
          };
        }
      );
    };
}
