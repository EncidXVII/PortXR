# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

# PortXR for Nix and NixOS, from the published Linux release (the same files as the tarball on GitHub Releases).
#   nix run github:EncidXVII/PortXR
# PortXR copies the files next to it into each game folder, and keeps them writable there. Files in the Nix store are
# read-only and their path changes with every update. So the `portxr` command keeps a per-user copy in
# ~/.local/share/portxr, refreshes it when the package changes, and runs that copy.
{
  description = "PortXR: play flat PC games in VR";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      version = "0.6.1";
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      meta = {
        description = "Play flat PC games in VR";
        homepage = "https://everyvr.net/portxr";
        license = pkgs.lib.licenses.gpl3Only;
        sourceProvenance = [ pkgs.lib.sourceTypes.binaryNativeCode ];
        platforms = [ system ];
        mainProgram = "portxr";
      };
      # The release as published, with the app's libraries pointed at the Nix store. The window system libraries
      # are loaded at run time, so they go on the library path too. The Windows files are for games under Proton
      # and stay as they are.
      app = pkgs.stdenv.mkDerivation {
        pname = "portxr-app";
        inherit version meta;
        src = pkgs.fetchurl {
          url = "https://github.com/EncidXVII/PortXR/releases/download/v${version}/PortXR-${version}-linux.tar.gz";
          hash = "sha256-Y01BfCRrB032nk7ajhfD5RdpWijpc7B5NXkMJTVs5yc=";
        };
        # The tarball has no top folder.
        sourceRoot = "source";
        unpackPhase = ''
          runHook preUnpack
          mkdir source
          tar -xzf $src -C source
          runHook postUnpack
        '';
        nativeBuildInputs = [ pkgs.autoPatchelfHook ];
        buildInputs = [ pkgs.stdenv.cc.cc.lib ];
        runtimeDependencies = [
          pkgs.libGL
          pkgs.libxkbcommon
          pkgs.wayland
          pkgs.dbus.lib
          (pkgs.libx11 or pkgs.xorg.libX11)
          (pkgs.libxcursor or pkgs.xorg.libXcursor)
          (pkgs.libxi or pkgs.xorg.libXi)
          (pkgs.libxrender or pkgs.xorg.libXrender)
        ];
        dontConfigure = true;
        dontBuild = true;
        dontStrip = true;
        installPhase = ''
          runHook preInstall
          rm -f "Uninstall PortXR.cmd"
          mkdir -p $out/share/portxr
          cp -r . $out/share/portxr/
          runHook postInstall
        '';
      };
      launcher = pkgs.writeShellApplication {
        name = "portxr";
        runtimeInputs = [ pkgs.coreutils ];
        inherit meta;
        text = ''
          dest="''${XDG_DATA_HOME:-$HOME/.local/share}/portxr"
          if [ "$(cat "$dest/.nix-package" 2>/dev/null || true)" != "${app}" ]; then
            mkdir -p "$dest"
            cp -R --remove-destination "${app}/share/portxr/." "$dest/"
            chmod -R u+w "$dest"
            echo "${app}" > "$dest/.nix-package"
          fi
          exec "$dest/PortXR" "$@"
        '';
      };
      desktop = pkgs.makeDesktopItem {
        name = "portxr";
        desktopName = "PortXR";
        comment = meta.description;
        exec = "portxr";
        icon = "portxr";
        categories = [
          "Game"
          "Utility"
        ];
      };
      # The command plus the menu entry and icon from this repository.
      portxr = pkgs.runCommand "portxr-${version}" { inherit meta; } ''
        mkdir -p $out/share/icons/hicolor/256x256/apps
        ln -s ${launcher}/bin $out/bin
        ln -s ${desktop}/share/applications $out/share/applications
        cp ${self}/app/assets/brand/portxr-256.png $out/share/icons/hicolor/256x256/apps/portxr.png
      '';
    in
    {
      packages.${system} = {
        inherit app portxr;
        default = portxr;
      };
    };
}
