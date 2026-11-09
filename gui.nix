{
  lib,
  python3Packages,
  qt6,
  makeDesktopItem,
  copyDesktopItems,
}:

python3Packages.buildPythonApplication {
  pname = "exitlag-relay-gui";
  version = "0.1.0";
  pyproject = false;

  src = ./gui;

  nativeBuildInputs = [
    qt6.wrapQtAppsHook
    copyDesktopItems
  ];
  buildInputs = [ qt6.qtwayland ];
  dependencies = [ python3Packages.pyside6 ];

  installPhase = ''
    runHook preInstall
    install -Dm755 exitlag_relay.py $out/bin/exitlag-relay
    install -Dm755 exitlag_run.py $out/bin/exitlag-run
    runHook postInstall
  '';

  dontWrapQtApps = true;
  preFixup = ''
    makeWrapperArgs+=("''${qtWrapperArgs[@]}")
  '';

  desktopItems = [
    (makeDesktopItem {
      name = "exitlag-relay";
      desktopName = "ExitLag Relay";
      comment = "Play Linux games through ExitLag under Wine";
      exec = "exitlag-relay";
      icon = "network-transmit-receive";
      categories = [
        "Network"
        "Game"
      ];
    })
  ];

  meta = {
    description = "Control panel for relaying Linux game traffic through ExitLag under Wine";
    license = lib.licenses.mit;
    mainProgram = "exitlag-relay";
  };
}
