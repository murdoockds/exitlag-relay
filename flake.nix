{
  description = "Play Linux games through ExitLag running under Wine";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system} = {
        default = pkgs.callPackage ./package.nix { };
        gui = pkgs.callPackage ./gui.nix { };
        exitlag-wine = pkgs.callPackage ./wine/package.nix { };
      };

      nixosModules.default = ./module.nix;
    };
}
