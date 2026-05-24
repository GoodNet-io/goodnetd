{
  description = "goodnetd — operator-facing daemon + multicall CLI for the GoodNet kernel.";

  inputs = {
    nixpkgs.url       = "github:NixOS/nixpkgs/nixos-unstable";
    goodnet.url       = "github:GoodNet-io/goodnet/dev";
    protocol-gnet.url = "github:GoodNet-io/protocol-gnet";

    # Loadable plugins. Each plugin's in-tree flake references the kernel via
    # `git+file:../../..?dir=nix/kernel-only` which only resolves inside the
    # monorepo. `follows` redirects those inputs to this flake's `goodnet`,
    # making the plugins work from any external consumer.
    link-tcp.url       = "github:GoodNet-io/link-tcp";
    link-tcp.inputs.goodnet.follows   = "goodnet";
    link-tcp.inputs.nixpkgs.follows   = "nixpkgs";

    link-udp.url       = "github:GoodNet-io/link-udp";
    link-udp.inputs.goodnet.follows   = "goodnet";
    link-udp.inputs.nixpkgs.follows   = "nixpkgs";

    link-ws.url        = "github:GoodNet-io/link-ws";
    link-ws.inputs.goodnet.follows    = "goodnet";
    link-ws.inputs.nixpkgs.follows    = "nixpkgs";

    link-ipc.url       = "github:GoodNet-io/link-ipc";
    link-ipc.inputs.goodnet.follows   = "goodnet";
    link-ipc.inputs.nixpkgs.follows   = "nixpkgs";

    link-tls.url       = "github:GoodNet-io/link-tls";
    link-tls.inputs.goodnet.follows   = "goodnet";
    link-tls.inputs.nixpkgs.follows   = "nixpkgs";

    link-ice.url       = "github:GoodNet-io/link-ice";
    link-ice.inputs.goodnet.follows   = "goodnet";
    link-ice.inputs.nixpkgs.follows   = "nixpkgs";

    security-noise.url = "github:GoodNet-io/security-noise";
    security-noise.inputs.goodnet.follows  = "goodnet";
    security-noise.inputs.nixpkgs.follows  = "nixpkgs";

    security-null.url  = "github:GoodNet-io/security-null";
    security-null.inputs.goodnet.follows   = "goodnet";
    security-null.inputs.nixpkgs.follows   = "nixpkgs";

    handler-heartbeat.url = "github:GoodNet-io/handler-heartbeat";
    handler-heartbeat.inputs.goodnet.follows  = "goodnet";
    handler-heartbeat.inputs.nixpkgs.follows  = "nixpkgs";

    handler-store.url     = "github:GoodNet-io/handler-store";
    handler-store.inputs.goodnet.follows      = "goodnet";
    handler-store.inputs.nixpkgs.follows      = "nixpkgs";

    handler-dns.url       = "github:GoodNet-io/handler-dns";
    handler-dns.inputs.goodnet.follows        = "goodnet";
    handler-dns.inputs.nixpkgs.follows        = "nixpkgs";
  };

  outputs = { self, nixpkgs, goodnet, protocol-gnet
            , link-tcp, link-udp, link-ws, link-ipc, link-tls, link-ice
            , security-noise, security-null
            , handler-heartbeat, handler-store, handler-dns }:
    let
      allSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      devShellHook = ''
        if [ -d .goodnet/goodnet ] && [ -d .goodnet/protocol-gnet ]; then
          echo "goodnetd: local .goodnet/ overrides present."
          echo "  nix develop \\"
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
          native = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          src    = ./.;
          meta   = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license     = pkgs.lib.licenses.mit;
          };

          daemon = pkgs.stdenv.mkDerivation {
            pname   = "goodnetd";
            version = "0.1.0";
            inherit src meta;
            nativeBuildInputs = native;
            buildInputs       = [ kernel gnet pkgs.libsodium pkgs.nlohmann_json ];
          };

          plugins = pkgs.lib.optionals pkgs.stdenv.isLinux [
            link-tcp.packages.${system}.default
            link-udp.packages.${system}.default
            link-ws.packages.${system}.default
            link-ipc.packages.${system}.default
            link-tls.packages.${system}.default
            link-ice.packages.${system}.default
            security-noise.packages.${system}.default
            security-null.packages.${system}.default
            handler-heartbeat.packages.${system}.default
            handler-store.packages.${system}.default
            handler-dns.packages.${system}.default
          ];

        in {
          default = daemon;

          # Full ecosystem: daemon + all plugins composed via goodnet.lib.compose.
          # nix profile add .#full  →  goodnet-node in PATH with all plugins bundled.
          full = goodnet.lib.compose pkgs {
            kernel  = daemon;
            inherit plugins;
          };

        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          static =
            let
              kernelStatic = goodnet.packages.${system}.goodnet-core-static or kernel;
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
          pkgs          = import nixpkgs { inherit system; };
          install-deps  = pkgs.callPackage ./nix/install-deps.nix  { };
          install-hooks = pkgs.callPackage ./nix/install-hooks.nix { };
          gn-setup      = pkgs.callPackage ./nix/setup.nix {
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
                static)  nix build $overrides .#static ; exit $? ;;
                full)    nix build $overrides .#full   ; exit $? ;;
                *)       flags="-DCMAKE_BUILD_TYPE=Debug"   ; dir=build ;;
              esac
              mkdir -p "$dir"
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
