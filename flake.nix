{
  description = "goodnetd — operator-facing daemon + multicall CLI for the GoodNet kernel.";

  inputs = {
    goodnet.url     = "github:GoodNet-io/goodnet/dev";
    nixpkgs.follows = "goodnet/nixpkgs";
  };

  outputs = { self, nixpkgs, goodnet }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll  = f: nixpkgs.lib.genAttrs systems
        (system: f system (import nixpkgs { inherit system; }));

      kernel = system:
        goodnet.packages.${system}.goodnet-core
          or goodnet.packages.${system}.default;
    in {
      packages = forAll (system: pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname   = "goodnetd";
          version = "0.1.0";
          src     = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs       = [ (kernel system) pkgs.libsodium pkgs.nlohmann_json ];
          meta = {
            description = "Operator-facing daemon and multicall CLI for the GoodNet kernel.";
            license     = pkgs.lib.licenses.mit;
          };
        };
      });

      devShells = forAll (system: pkgs:
        let k = kernel system; in {
          default = pkgs.mkShell {
            packages = [ k pkgs.libsodium pkgs.nlohmann_json pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            shellHook = ''
              export LD_LIBRARY_PATH="${k}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            '';
          };
        });
    };
}
