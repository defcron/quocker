# Contributing to Quocker

Quocker is developed on the `quocker` branch of this QEMU fork. Its scope is
QEMU/KVM configuration and orchestration of VM workloads and their networks;
container lifecycle management is out of scope. Changes should preserve the
QEMU `master` branch and base Quocker updates on the current QEMU stable branch.

## Code and licensing

Quocker implementation code is C. New source files should carry the SPDX
identifier `GPL-2.0-or-later`, consistent with the repository's `COPYING` file
and existing Quocker sources. YAML, JSON, and other interchange data may use
their appropriate formats. New dependencies and copied code must be reviewed
for license compatibility before inclusion; keep third-party notices with the
relevant source or dependency metadata.

Quocker is an optional Meson feature. Keep its dependencies and build rules
behind the Quocker build option so a normal QEMU build does not acquire
Quocker-only requirements. Add or update focused tests for behavioral changes
and document user-visible CLI or configuration changes.

## Patch and review process

Submit Quocker changes as pull requests against the `quocker` branch of this
repository. Keep changes scoped, buildable, and reviewable; include the reason
for the change, its user-visible effect, and the validation performed. Use
QEMU's coding conventions and secure C practices. Quocker-specific ownership
and review are handled in this repository; QEMU's upstream `MAINTAINERS` entries
do not by themselves designate Quocker maintainers.

With a Quocker-enabled Meson build directory, the focused checks are:

```sh
ninja -C <build-dir> scripts/quocker
meson test -C <build-dir> --suite quocker --print-errorlogs
```

The Quocker test suite supplements, rather than replaces, the normal QEMU
build and test requirements for changes that touch shared QEMU code.
