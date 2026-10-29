# Nix and NixOS

Build and install the package without changing the host init:

```sh
nix-build
```

Import the NixOS module from a checkout kept at a permanent path:

```nix
{ pkgs, ... }: {
  imports = [ /path/to/Vinit/nix/module.nix ];
  services.vinit = {
    enable = true;
    services.worker = ''
      [service]
      command = ${pkgs.coreutils}/bin/sleep infinity
      [restart]
      policy = always
    '';
  };
}
```

Apply with `sudo nixos-rebuild switch`. Use `vinitctl list` and
`vinitctl status worker` to inspect services, and `journalctl -u vinit`
for their console output. NixOS generates `/etc/vinit/services`; edit
the Nix configuration instead of these immutable files. Changing service
definitions and rebuilding restarts Vinit and all its services.

Systemd remains the host init. The module launches Vinit as PID 1 in a
private PID and mount namespace; it supervises only the services defined
under `services.vinit.services`. Existing NixOS systemd services are not
converted. This is experimental integration, not a replacement NixOS init.

The module drops `CAP_SYS_ADMIN` before starting Vinit, preventing its
shutdown from unmounting host filesystems or disabling host swap. Shutdown
can therefore log permission errors for these operations. Services inherit
this restriction and `NoNewPrivileges`; setuid elevation is unavailable.
The namespace is not a security sandbox: services still share host files,
networking and users. Configure `[execution] user` for unprivileged daemons.
Commands should reference Nix store executables as in the example.

`sudo systemctl stop vinit` stops the supervisor and its services.
`sudo vinitctl poweroff` powers off only its PID namespace. Host power
commands continue to use systemd. Do not install the Void power wrappers.

Build and run the disposable NixOS VM test:

```sh
nix-build tests/nix
```
