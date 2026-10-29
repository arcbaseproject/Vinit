{ lib, stdenv }:
stdenv.mkDerivation {
  pname = "vinit";
  version = "0.1.0";
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let name = baseNameOf path;
      in (type == "directory" && (path == toString ../. || builtins.elem name [ "src" "cmd" ]))
        || builtins.elem name [ "Makefile" "LICENSE" ]
        || lib.hasSuffix ".c" name || lib.hasSuffix ".h" name;
  };
  # Nix tracks the runtime libc in the closure; static libc is not required.
  makeFlags = [ "CC=${stdenv.cc.targetPrefix}cc" "LDFLAGS=" ];
  installPhase = ''
    runHook preInstall
    install -Dm755 vinit "$out/bin/vinit"
    install -Dm755 vinitctl "$out/bin/vinitctl"
    runHook postInstall
  '';
  meta = {
    description = "Small Linux init and service supervisor";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "vinitctl";
  };
}
