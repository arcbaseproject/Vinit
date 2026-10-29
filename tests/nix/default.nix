{ pkgs ? import <nixpkgs> { } }:
pkgs.testers.runNixOSTest {
  name = "vinit";
  nodes.machine = { pkgs, ... }: {
    imports = [ ../../nix/module.nix ];
    services.vinit = {
      enable = true;
      services.worker = ''
        [service]
        command = ${pkgs.coreutils}/bin/sleep infinity
        [restart]
        policy = always
      '';
    };
  };
  testScript = ''
    machine.start()
    machine.wait_for_unit("vinit.service")
    machine.wait_until_succeeds("vinitctl status worker | grep 'state: running'")
    machine.succeed("test $(cat /proc/1/comm) = systemd")
    pid = machine.succeed("pgrep -x vinit").strip()
    status = machine.succeed(f"cat /proc/{pid}/status")
    for line in status.splitlines():
        if line.startswith(("CapEff:", "CapBnd:")):
            assert int(line.split()[1], 16) & (1 << 21) == 0, line
    machine.succeed(f"test $(readlink /proc/{pid}/ns/pid) != $(readlink /proc/1/ns/pid)")
    machine.succeed("vinitctl restart worker")
    machine.wait_until_succeeds("vinitctl status worker | grep 'state: running'")
    machine.succeed("systemctl stop vinit.service")
    machine.succeed("test $(cat /proc/1/comm) = systemd")
    machine.fail("test -e /run/vinit/control")
    machine.fail("pgrep -x vinit")
    machine.succeed("findmnt / && findmnt /proc")
    machine.succeed("systemctl start vinit.service")
    machine.wait_until_succeeds("vinitctl status worker | grep 'state: running'")
  '';
}
