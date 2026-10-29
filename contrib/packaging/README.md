# Distribution packages

Package recipes for v0.2.0. They install `vinit`, `vinitctl` and the man
pages. None of them makes vinit the default init: add
`init=/usr/bin/vinit` to the kernel command line to boot it.

| Distribution | Recipe |
|---|---|
| Arch Linux | [arch/PKGBUILD](arch/PKGBUILD) |
| Void Linux | [void/template](void/template) |
| Gentoo | [gentoo/sys-apps/vinit](gentoo/sys-apps/vinit), with its Manifest |

The AUR package is not published yet. Until then, build it from this
directory on Arch:

```sh
cd contrib/packaging/arch && makepkg -si
```

The recipes link dynamically, because these distributions keep `/usr`
on the root filesystem. The Nix package is in [nix/](../../nix).
