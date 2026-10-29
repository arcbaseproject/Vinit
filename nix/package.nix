{ lib, stdenv }:
stdenv.mkDerivation {
  pname = "vinit";
  version = "0.2.0";
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let name = baseNameOf path;
      in (type == "directory" && (path == toString ../. || builtins.elem name [ "src" "cmd" "man" ]))
        || builtins.elem name [ "Makefile" "LICENSE" ]
        || lib.hasSuffix ".c" name || lib.hasSuffix ".h" name
        || lib.hasSuffix ".5" name || lib.hasSuffix ".8" name;
  };
  # Nix tracks the runtime libc in the closure; static libc is not required.
  makeFlags = [ "CC=${stdenv.cc.targetPrefix}cc" "LDFLAGS=" ];
  installPhase = ''
    runHook preInstall
    install -Dm755 vinit "$out/bin/vinit"
    install -Dm755 vinitctl "$out/bin/vinitctl"
    install -Dm644 man/vinit.8 man/vinitctl.8 -t "$out/share/man/man8"
    install -Dm644 man/vinit.conf.5 -t "$out/share/man/man5"
    runHook postInstall
  '';
  meta = {
    description = "Small Linux init and service supervisor";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "vinitctl";
  };
}
