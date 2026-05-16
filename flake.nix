{
  description = "goodnetd — operator-facing daemon + multicall CLI for the GoodNet kernel.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    goodnet.url = "github:GoodNet-io/goodnet";
  };

  outputs = { self, nixpkgs, flake-utils, goodnet }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        kernel = goodnet.packages.${system}.default;
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "goodnetd";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [ kernel pkgs.libsodium pkgs.openssl pkgs.asio ];
          meta = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license = pkgs.lib.licenses.mit;
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [ kernel pkgs.libsodium pkgs.openssl pkgs.asio pkgs.cmake pkgs.ninja ];
        };
      });
}
