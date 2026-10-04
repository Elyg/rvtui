{
  description = "rvtui — terminal image viewer (C++20 + FTXUI + OpenEXR)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    let
      # Nix builds have no tags; identify them by commit instead.
      version = "git-${self.shortRev or self.dirtyShortRev or "dev"}";
    in
    {
      overlays.default = final: prev: {
        rvtui = final.callPackage ./nix/package.nix { inherit version; };
      };
    }
    // flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        hostLlvm = pkgs.llvmPackages_20;

        tools = with pkgs; [
          cmake
          ninja
          just
          conan
          git
          hostLlvm.clang-tools # clang-format / clang-tidy
          doxygen # just docs
        ];

        commonSetup = ''
          export SHELL_TYPE="host"
          export CONAN_HOME="''${CONAN_HOME:-$HOME/.conan2}"
        '';
        rvtui = pkgs.callPackage ./nix/package.nix { inherit version; };
      in
      {
        packages =
          {
            default = rvtui;
            rvtui = rvtui;
          }
          // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            # Fully static musl binary: copy anywhere, no runtime deps.
            static = pkgs.pkgsStatic.callPackage ./nix/package.nix { inherit version; };
          };

        apps.default = flake-utils.lib.mkApp { drv = rvtui; };

        # Deps from nixpkgs instead of conan: what CI and `nix build` use. clang-tidy parses
        # this tree cleanly (on macOS the Apple-clang compile DB trips nix clang-tidy).
        devShells.ci = pkgs.mkShell {
          inputsFrom = [ rvtui ];
          packages = tools ++ [ pkgs.gtest ];
        };

        devShells.default =
          if pkgs.stdenv.hostPlatform.isDarwin then
            # On macOS build with Apple's clang (Xcode CLT) so binaries link against the system SDK.
            pkgs.mkShellNoCC {
              packages = tools;
              shellHook = commonSetup + ''
                export CC=/usr/bin/clang
                export CXX=/usr/bin/clang++
                unset SDKROOT DEVELOPER_DIR
              '';
            }
          else
            (pkgs.mkShell.override { stdenv = hostLlvm.stdenv; }) {
              packages = tools;
              shellHook = commonSetup + ''
                export CC=clang
                export CXX=clang++
              '';
            };
      }
    );
}
