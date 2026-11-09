{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.exitlag-relay;
in
{
  options.services.exitlag-relay = {
    enable = lib.mkEnableOption "relaying Linux games' UDP traffic through ExitLag running under Wine";

    user = lib.mkOption {
      type = lib.types.str;
      example = "alice";
      description = ''
        The desktop user running the ExitLag Relay GUI. Only this user (and
        root) can use the daemon's control socket, and only their processes
        are diverted.
      '';
    };

    listenAddress = lib.mkOption {
      type = lib.types.str;
      default = "127.0.0.1";
      description = ''
        Address the daemon listens on for the nfapi.dll shim inside ExitLag.
        Only peers in its /24 are accepted.
      '';
    };

    wine = {
      package = lib.mkOption {
        type = lib.types.package;
        default = pkgs.callPackage ./wine/package.nix {
          relayHost = "${cfg.listenAddress}:${toString cfg.port}";
          inherit (cfg.wine) home;
        };
        defaultText = lib.literalExpression "built from wine/package.nix for listenAddress, port and wine.home";
        description = "The exitlag-wine package.";
      };

      home = lib.mkOption {
        type = lib.types.str;
        default = "${config.users.users.${cfg.user}.home}/.local/share/exitlag-wine";
        defaultText = lib.literalExpression "~user/.local/share/exitlag-wine";
        description = "exitlag-wine's state directory (EXITLAG_WINE_HOME); its Wine prefix is never diverted.";
      };
    };

    autoStart = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Start the relay daemon at boot. By default the ExitLag Relay GUI
        starts it when it opens and stops it on Exit (allowed for `user`
        without a password through polkit).
      '';
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 27999;
      description = "UDP port the daemon listens on for the shim.";
    };

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "built from package.nix";
      description = "The exitlag-relayd package.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package
      cfg.wine.package
      (pkgs.callPackage ./gui.nix { })
    ];

    # The GUI starts the daemon when it opens and stops it on Exit; let its
    # user do that (and only that) without a password.
    security.polkit.enable = true;
    security.polkit.extraConfig = ''
      polkit.addRule(function (action, subject) {
        if (action.id == "org.freedesktop.systemd1.manage-units" &&
            action.lookup("unit") == "exitlag-relay.service" &&
            subject.user == ${builtins.toJSON cfg.user} &&
            ["start", "stop", "restart"].indexOf(action.lookup("verb")) >= 0) {
          return polkit.Result.YES;
        }
      });
    '';

    # Replies are injected into exitlag0, which has no address: with the
    # usual loose reverse-path filter (systemd sets net.ipv4.conf.*.rp_filter
    # = 2 on every new interface) the kernel drops every one of them. A
    # setting for this interface name is applied when it appears.
    boot.kernel.sysctl = {
      "net.ipv4.conf.exitlag0.rp_filter" = 0;
      "net.ipv6.conf.exitlag0.disable_ipv6" = 1;
    };

    # nft_socket: matching packets by the cgroup of the socket that sent them.
    boot.kernelModules = [
      "tun"
      "nft_socket"
    ];

    systemd.services.exitlag-relay = {
      description = "ExitLag game relay (idle until a game is selected in the GUI)";
      wantedBy = lib.optional cfg.autoStart "multi-user.target";
      after = [ "network.target" ];
      path = [
        pkgs.iproute2
        pkgs.nftables
      ];

      serviceConfig = {
        ExecStart = lib.escapeShellArgs (
          [
            (lib.getExe cfg.package)
            "-listen"
            "${cfg.listenAddress}:${toString cfg.port}"
            "-control"
            "/run/exitlag-relay/control.sock"
            "-control-user"
            cfg.user
            "-state"
            "/var/lib/exitlag-relay/origins.json"
          ]
          ++ [
            "-ignore-wine-prefix"
            cfg.wine.home
          ]
        );
        RuntimeDirectory = "exitlag-relay";
        StateDirectory = "exitlag-relay";
        Restart = "on-failure";
        RestartSec = 5;
        PrivateTmp = true;
        ProtectHome = true;
      };
    };
  };
}
