# Contributing

Bug reports, test results and patches are welcome. vinit is experimental,
so reports from trying it in a VM or a container are useful too.

## Before you send a patch

Run the tests. You do not need root:

```sh
make test   # unit tests under ASan and UBSan, then vinit as PID 1 in namespaces
```

If you change a tool in `contrib/`, also run `make test-contrib`.

[README.md](README.md#test) lists what `make test` needs. If you change
the parser or the dependency resolver, also run `make fuzz` (needs clang).
CI runs `make test` with glibc and musl on every push and pull request.

## Style

- C17, kernel-like style: tabs, `/* */` comments, no new dependencies.
- PID 1 must never crash or block. Check every system call, and log
  failures with `warn()`.
- Add a test for new behavior. Integration tests are a service in
  [tests/services](tests/services) plus a check in
  [tests/checks.sh](tests/checks.sh).
- Update the README when behavior or configuration changes.

## Good first issues

The [known limitations](README.md#known-limitations) are open work. A
good start is to report how vinit behaves with a daemon you use.

The most useful report right now is a boot on real hardware. Use the
"Boot report" issue form, whether it worked or not.

## Security

Do not open a public issue for a security problem. Use GitHub's private
vulnerability reporting on this repository instead. See
[docs/security.md](docs/security.md) for the threat model.
