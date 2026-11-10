{
  description = "Portable Markdown Tangler";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
  };
  outputs =
    { nixpkgs, ... }:
    let
      supportedSystems = [ "x86_64-linux" ];
      forSupportedSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forSupportedSystems (system: {
        default = nixpkgs.legacyPackages.${system}.stdenv.mkDerivation {
          pname = "mangle";
          version = "0.1.0";
          src = ./.;
          buildPhase = ''
            make
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp a.out $out/bin/mangle
          '';
        };
      });
      devShells = forSupportedSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              clang
              clang-tools
            ];
            shellHook = ''
              export TERM=xterm-256color
            '';
          };
        }
      );
    };
}
