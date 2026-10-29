{ config, lib, pkgs, ... }:
let
  cfg = config.services.vinit;
  serviceDir = pkgs.runCommand "vinit-services" { } ''
    mkdir -p "$out"
    ${lib.concatStringsSep "\n" (lib.mapAttrsToList (name: text:
      "ln -s ${pkgs.writeText "vinit-${name}.conf" text} \"$out/${name}.conf\""
    ) cfg.services)}
  '';
  stop = pkgs.writeShellScript "vinit-stop" ''
    ${cfg.package}/bin/vinitctl poweroff
    while kill -0 "$MAINPID" 2>/dev/null; do
      ${pkgs.coreutils}/bin/sleep 0.1
    done
  '';
in {
  options.services.vinit = {
    enable = lib.mkEnableOption "Vinit package and declarative service files";
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      description = "Vinit package to install.";
    };
    services = lib.mkOption {
      type = lib.types.attrsOf lib.types.lines;
      default = { };
      description = "Service definitions in Vinit's INI format, keyed by service name.";
    };
  };
  config = lib.mkIf cfg.enable {
    assertions = [{
      assertion = cfg.services != { };
      message = "Define at least one services.vinit.services entry before enabling Vinit.";
    } {
      assertion = builtins.length (builtins.attrNames cfg.services) <= 63
        && lib.all (name: builtins.match "[A-Za-z0-9_][A-Za-z0-9_.@-]*" name != null
          && builtins.stringLength name < 32) (builtins.attrNames cfg.services);
      message = "Vinit supports at most 63 services, with valid names shorter than 32 characters.";
    }];
    environment.systemPackages = [ cfg.package ];
    environment.etc."vinit/services".source = serviceDir;
    systemd.services.vinit = {
      description = "Vinit service supervisor in a private PID namespace";
      wantedBy = [ "multi-user.target" ];
      after = [ "local-fs.target" ];
      restartTriggers = [ serviceDir ];
      path = [ pkgs.coreutils ] ++ config.environment.systemPackages;
      serviceConfig = {
        Type = "simple";
        RuntimeDirectory = "vinit";
        RuntimeDirectoryMode = "0755";
        # Mount propagation must be private before replacing /proc.
        ExecStart = "${pkgs.util-linux}/bin/unshare --mount --propagation private --pid --fork --mount-proc ${pkgs.util-linux}/bin/setpriv --bounding-set=-sys_admin ${cfg.package}/bin/vinit";
        ExecStop = stop;
        TimeoutStopSec = 45;
        KillMode = "control-group";
        Restart = "on-failure";
        NoNewPrivileges = true;
      };
    };
  };
}
