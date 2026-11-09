{
  lib,
  buildGoModule,
}:

buildGoModule {
  pname = "exitlag-relay";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./go.mod
      ./cmd
      ./internal
    ];
  };

  vendorHash = null;
  subPackages = [ "cmd/exitlag-relayd" ];
  env.CGO_ENABLED = 0;

  meta = {
    description = "Relay Linux games' UDP traffic through ExitLag running under Wine";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "exitlag-relayd";
  };
}
