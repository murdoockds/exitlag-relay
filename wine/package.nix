{
  lib,
  stdenvNoCC,
  zig,
  makeWrapper,
  umu-launcher,
  procps,
  coreutils,
  diffutils,
  gnugrep,
  gnused,
  jq,
  python3,
  # exitlag-relayd's address, as the nfapi.dll shim inside Wine reaches it.
  relayHost ? "127.0.0.1:27999",
  # State directory (EXITLAG_WINE_HOME); null keeps ~/.local/share/exitlag-wine.
  home ? null,
}:

# ExitLag under Wine: the nfapi.dll shim (and its helpers), cross-compiled
# with zig's MinGW target, plus the exitlag-wine launcher.
stdenvNoCC.mkDerivation {
  pname = "exitlag-wine";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./exitlag-wine.sh
      ./exitlag-tray.py
      ./nfshim/build.sh
      ./nfshim/nfapi.c
      ./nfshim/nfapi.def
      ./nfshim/nfapi.rc
      ./nfshim/wmifix.c
      ./nfshim/vblank.c
      ./nfshim/dpifix.c
      ./nfshim/tray.c
      ./nfshim/profile.c
      ./nfshim/windiag.c
      ./nfshim/ipv4only.c
      ./nfshim/idle.c
      ./nfshim/nfextlag.c
    ];
  };

  nativeBuildInputs = [
    zig
    makeWrapper
  ];

  buildPhase = ''
    runHook preBuild
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
    patchShebangs nfshim/build.sh
    nfshim/build.sh "$PWD/build"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 -t $out/share/exitlag-wine build/nfapi.dll build/idle.exe build/nfextlag-wine.sys
    install -Dm755 exitlag-wine.sh $out/libexec/exitlag-wine
    install -Dm755 exitlag-tray.py $out/bin/exitlag-tray
    substituteInPlace $out/bin/exitlag-tray \
      --replace-fail "#!/usr/bin/env python3" "#!${python3.withPackages (ps: [ ps.dbus-fast ])}/bin/python3"
    makeWrapper $out/libexec/exitlag-wine $out/bin/exitlag-wine \
      --set-default EXITLAG_WINE_SHIM $out/share/exitlag-wine \
      --set-default EXITLAG_RELAY_HOST ${lib.escapeShellArg relayHost} \
      ${lib.optionalString (home != null) "--set-default EXITLAG_WINE_HOME ${lib.escapeShellArg home}"} \
      --prefix PATH : $out/bin:${
        lib.makeBinPath [
          umu-launcher
          procps
          coreutils
          diffutils
          gnugrep
          gnused
          jq
        ]
      }
    runHook postInstall
  '';

  meta = {
    description = "Run ExitLag under Wine, fed by exitlag-relayd";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "exitlag-wine";
  };
}
