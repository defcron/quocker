# Security policy for Quocker

This policy covers the Quocker additions in this QEMU fork. QEMU's own security
documentation remains authoritative for the emulator and its deployment modes.

Quocker adds a host-side orchestration layer to QEMU. Treat Compose YAML,
interpolation and service environment files, OCI metadata and layers, registry
responses, kernel catalogs, guest disks, and guest output as untrusted inputs.
Quocker must not interpret guest content as host commands, expose registry
credentials in logs or diagnostics, or trust an image-provided kernel URL or
signing key. Keep parsing, decompression, filesystem materialization, and
network responses bounded and reject path traversal or unsafe host-file access.

Security-sensitive changes should receive focused review and regression
coverage. In particular, review host/guest boundary assumptions, privilege
changes, credential handling, signature and digest checks, symlink and hard
link handling, decompression limits, and failure cleanup. Do not add real
credentials or exploitable payloads to public test fixtures.

Report suspected vulnerabilities privately rather than opening a public issue.
For vulnerabilities affecting QEMU or the QEMU/guest security boundary, follow
the [QEMU Security Process](https://wiki.qemu.org/SecurityProcess); the QEMU
maintainers list routes those reports through `qemu-security@nongnu.org`.
For a Quocker-only vulnerability, contact the repository owner privately before
public disclosure so it can be coordinated with the QEMU security process when
the issue crosses into QEMU.

Quocker does not create a security guarantee beyond the QEMU deployment mode.
Consult QEMU's [security requirements](docs/system/security.rst) and use a
supported virtualization accelerator and machine type when relying on guest
isolation.
