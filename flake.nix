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

      # Detect local .goodnet/ overrides and print the dev shell command.
      devShellHook = ''
        if [ -d .goodnet/goodnet ] && [ -d .goodnet/protocol-gnet ]; then
          echo "goodnetd: local .goodnet/ overrides present."
          echo "  Re-enter with: nix develop \\"
          echo "    --override-input goodnet       path:.goodnet/goodnet \\"
          echo "    --override-input protocol-gnet path:.goodnet/protocol-gnet"
        fi
      '';

    in {
      packages = nixpkgs.lib.genAttrs allSystems (system:
        let
          pkgs   = import nixpkgs { inherit system; };
          kernel = goodnet.packages.${system}.goodnet-core
                or goodnet.packages.${system}.default;
          gnet   = protocol-gnet.packages.${system}.default;
          src    = ./.;
          native = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          meta   = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license     = pkgs.lib.licenses.mit;
          };
        in {
          default = pkgs.stdenv.mkDerivation {
            pname   = "goodnetd";
            version = "0.1.0";
            inherit src meta;
            nativeBuildInputs = native;
            buildInputs       = [ kernel gnet pkgs.libsodium pkgs.nlohmann_json ];
          };
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          # Truly-static binary: `nix build .#static` / `nix profile add .#static`.
          # goodnet-core-static uses pkgsStatic + musl; gnet is rebuilt the same way.
          static =
            let
              kernelStatic = goodnet.packages.${system}.goodnet-core-static
                          or kernel;
              gnetStatic   = pkgs.pkgsStatic.stdenv.mkDerivation {
                pname   = "goodnet-protocol-gnet";
                version = "1.0.0-rc6";
                src     = protocol-gnet;
                nativeBuildInputs = native;
                buildInputs       = [ kernelStatic ];
                cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" "-DBUILD_TESTING=OFF" ];
              };
            in pkgs.pkgsStatic.stdenv.mkDerivation {
              pname   = "goodnetd-static";
              version = "0.1.0";
              inherit src;
              nativeBuildInputs = native;
              buildInputs = [ kernelStatic gnetStatic
                              pkgs.pkgsStatic.libsodium
                              pkgs.pkgsStatic.nlohmann_json ];
              cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release"
                             "-DCMAKE_EXE_LINKER_FLAGS=-static" ];
              meta = meta // { description = "Static goodnetd (nix profile add .#static)."; };
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
            packages  = [ kernel gnet
                          pkgs.libsodium pkgs.nlohmann_json
                          pkgs.cmake pkgs.ninja pkgs.pkg-config
                          pkgs.clang-tools ];
            shellHook = devShellHook;
          };
        }
      );

      apps = nixpkgs.lib.genAttrs allSystems (system:
        let
          pkgs           = import nixpkgs { inherit system; };
          install-deps   = pkgs.callPackage ./nix/install-deps.nix   { };
          install-hooks  = pkgs.callPackage ./nix/install-hooks.nix  { };
          gn-setup       = pkgs.callPackage ./nix/setup.nix          {
            inherit install-deps install-hooks;
          };

          gn-build = pkgs.writeShellApplication {
            name = "goodnetd-build";
            runtimeInputs = [ pkgs.cmake pkgs.ninja pkgs.git ];
            text = ''
              set -euo pipefail
              overrides=""
              [ -d .goodnet/goodnet ]       && overrides="$overrides --override-input goodnet path:.goodnet/goodnet"
              [ -d .goodnet/protocol-gnet ] && overrides="$overrides --override-input protocol-gnet path:.goodnet/protocol-gnet"
              variant="''${1:-debug}"
              case "$variant" in
                release) flags="-DCMAKE_BUILD_TYPE=Release" ; dir=build-release ;;
                static)  exec nix build $overrides .#static "$@" ; exit $? ;;
                *)       flags="-DCMAKE_BUILD_TYPE=Debug"   ; dir=build ;;
              esac
              mkdir -p "$dir"
              # shellcheck disable=SC2086
              nix develop $overrides --command bash -c "
                cmake -G Ninja $flags -B $dir . &&
                cmake --build $dir
              "
            '';
          };
        in {
          setup = { type = "app"; program = "${gn-setup}/bin/goodnetd-setup"; };
          build = { type = "app"; program = "${gn-build}/bin/goodnetd-build"; };
        }
      );
    };
}
