# Quocker OCI-to-VM image compatibility design

**Status:** implementation contract
**Scope:** Linux OCI images referenced from Compose-shaped Quocker files

## Docker Hub image compatibility layer

Quocker treats a Docker Hub (or configured registry mirror) reference as an
application filesystem input to a VM assembly pipeline. The input continues to
be an ordinary OCI image: no Docker daemon, container runtime, or container
storage backend is involved. The compatibility layer resolves the reference,
selects the OCI platform manifest, verifies and caches its content, and then
assembles a bootable VM from that userland plus Quocker-supplied boot assets.

The intended user-facing flow is:

```yaml
services:
  web:
    image: nginx:latest
    ports:
      - "8080:80"
```

Quocker resolves this exactly as an OCI image reference, fetches its layers
from Docker Hub or the configured mirror, converts the root filesystem to a
managed VM disk, chooses a trusted Linux kernel/initrd compatible with the
selected platform and requirements, injects the Quocker guest bootstrap, and
starts the configured image command inside QEMU/KVM. The image declaration is
reused; the host execution model is a VM. Familiar Dockerfile/Compose syntax
does not make a container image bootable by itself, and Quocker must explain
which settings are translated to VM behavior.

### Kernel decision model

“Image-specific kernel” means a kernel selected for that image's platform and
declared/evidenced constraints. It does not mean that Quocker can discover a
unique kernel from every image. OCI has no standard kernel requirement field,
and `uname` in a Docker build describes the builder/host kernel, not a kernel
that the image needs. A detected module directory can also be incidental and
does not prove that the app loads those modules.

The compatibility layer evaluates evidence in this order:

1. OCI index/config platform and Compose `platform` establish the hard OS and
   architecture constraint. A mismatch or unsupported platform is an error.
2. Explicit `x-quocker.kernel` configuration and `io.quocker.kernel.*` image
   labels express requirements or an exact catalog profile. An exact pin is
   accepted only when it passes platform, signature, digest, minimum-version,
   and feature checks.
3. Rootfs inspection records distro identity, userspace architecture, and
   kernel module releases. Distro identity ranks compatible catalog profiles.
   Module releases are reported as evidence; they become hard constraints only
   when the image explicitly declares that the workload requires those modules
   (or an administrator chooses strict module matching).
4. Otherwise, Quocker chooses the highest-priority maintained generic profile
   in the trusted catalog that supports the platform and all explicit
   requirements. This is the normal path for app images such as `nginx`,
   `redis`, and distroless images.

Selection produces an inspectable decision: selected catalog ID and version,
platform, catalog/signature identity, constraints satisfied, evidence used,
weak hints ignored, and whether the generic fallback was used. Catalog and
kernel/initrd downloads are trusted only after signature and digest
verification. Image metadata may request a profile but cannot supply a URL,
trust key, or executable bootstrap hook. If no candidate satisfies hard
constraints, Quocker fails before launching the VM and reports the conflict.

### Bootstrap boundary

Quocker does not mutate or republish the upstream image to make it bootable.
Instead, it composes a VM at run time from four separately tracked inputs:

- the immutable OCI manifest/config/layers and the resulting rootfs disk;
- a catalog-selected, verified guest kernel;
- a verified Quocker initramfs containing the guest init/bootstrap; and
- a bounded runtime configuration derived from OCI image defaults and Compose
  overrides.

The bootstrap mounts the VM's root disk and essential guest filesystems, then
executes the image's effective `Entrypoint` and `Cmd` with its `Env`, `User`,
and `WorkingDir`. This supports images without a shell or conventional init.
The VM disk overlay and state belong to Quocker's project lifecycle. The OCI
image remains content-addressed and reusable. This is the compatibility layer:
an OCI image becomes a VM workload without turning it into a container or
requiring Docker-specific internals.

### Quockerfile relationship

For the initial compatibility milestone, `image:` consumes already-built OCI
images, including images built from Dockerfiles. A Quockerfile is a later build
interface, not a prerequisite for image use. Any accepted Dockerfile-like
build instruction must execute in a disposable QEMU build VM and produce
OCI-compatible layers or a Quocker-managed VM artifact. `RUN` must never run
on the host, and build-time commands must not silently acquire container
semantics. This keeps the image declaration portable while making the VM
boundary explicit.

## Goal

Let a familiar service declaration such as `image: nginx:latest` resolve from
Docker Hub or a configured OCI registry mirror, then boot that application's
filesystem as a QEMU/KVM virtual machine. The OCI image supplies userspace and
launch metadata. Quocker supplies the guest kernel, boot setup, and a small
guest init process. Quocker orchestrates VMs, application processes, and
networking; it does not run or manage containers.

This gives the same image-reference workflow, but cannot make every Docker
container behavior identical: an OCI image does not encode a guest kernel,
firmware, or system boot process, and container namespace/cgroup settings do
not map one-to-one to VM settings.

## Resolution and trust

1. Resolve short names using Docker-compatible defaults (`docker.io`,
   `library/` for unqualified official images, and `latest` when no tag or
   digest is supplied). Preserve the fully qualified registry/repository and
   immutable selected digest in state.
2. Consult configured mirror rules before the upstream registry. Support
   standard registry endpoints and explicit per-registry mirror mappings.
   Credentials come from the user's registry auth configuration or a Quocker
   credential provider; never write secrets to Compose output or VM metadata.
3. Negotiate OCI/Docker manifest and index media types, select by requested
   guest OS and architecture, and download config/layers by digest. Verify
   every digest and declared size. Do not forward authorization headers across
   an untrusted cross-host redirect.
4. Keep blobs in a content-addressed cache with per-project/image usage
   reporting, locking, quotas, and explicit garbage collection. Mutable tags
   resolve to a digest at pull time; running guests continue to refer to that
   digest until explicitly updated.

## Build a bootable guest

1. Unpack layers into a staged root filesystem, applying OCI whiteouts in layer
   order. Reject absolute/traversing archive paths, unsafe links, decompression
   bombs, and limits on file count, expanded size, and nesting. Preserve
   character/block devices and FIFOs as inert placeholders with guest metadata;
   never create host device nodes. Publish only after validation completes.
2. Select a kernel using the OCI platform (`linux` plus architecture) as the
   required compatibility key. Optional `/etc/os-release` and image labels may
   narrow the preferred kernel family/version. They are hints, not a reliable
   statement of kernel ABI requirements.
3. Use an administrator-configured, signed kernel catalog. The default policy
   should select a maintained generic kernel for the target architecture,
   with an explicit image/Compose override to pin a catalog entry or local
   kernel. Record kernel version, digest, signature identity, and selection
   reason in image inspection and service status.
4. Attach the root filesystem as a VM disk and boot it with the selected
   kernel. Inject a small, auditable Quocker init as PID 1 (or use a supported
   guest init integration). It mounts `/proc`, `/sys`, and `/dev`, configures
   the guest network, applies OCI `User`, `WorkingDir`, and `Env`, launches
   `Entrypoint` plus `Cmd`, forwards signals, reaps children, and reports exit
   and readiness status over a narrow guest channel.
5. When metadata is missing or boot fails, report the detected platform,
   selected kernel and evidence. Permit a generic fallback only when its
   architecture/OS match; otherwise fail clearly and allow an explicit
   override. Never execute image `RUN` instructions on the host.

## VM bootstrap and application launch

An OCI image is not a bootable operating system. Quocker therefore boots a
small, trusted initramfs first, mounts the converted OCI root disk as the guest
root, then transfers control to the image's configured application command.
This avoids requiring the image to contain systemd, a shell, a package manager,
or even an init binary. It also avoids editing image layers to inject Quocker
files.

The initramfs contains only the Quocker guest bootstrap and the tools or kernel
features needed to mount the configured root disk, bring up the chosen virtual
NIC, and enter the root filesystem. Quocker supplies the resolved OCI runtime
configuration through a read-only, per-VM boot configuration artifact. The
guest bootstrap mounts `/proc`, `/sys`, and `/dev`, mounts the root disk,
switches to it, applies `User`, `WorkingDir`, `Env`, and the effective
`Entrypoint` plus `Cmd`, and `exec`s the application as PID 1. It forwards
signals and reports exit status/readiness over a narrow QEMU guest channel.
The bootstrap must not interpret shell strings unless the image explicitly
declares a shell; argument arrays remain argument arrays.

The C implementation provides a statically linked guest init and host-side
initrd builder. The builder appends a `newc` archive containing `/init` and a
bounded binary runtime configuration to the catalog initrd, and rechecks the
catalog initrd digest while copying it. OCI-backed `up` builds this initrd,
passes the verified catalog kernel and initrd to QEMU, and starts the guest
from the converted ext4 root disk. Linux defines initramfs as a sequence of
compressed or uncompressed CPIO archives; later entries can replace an earlier
`/init` without unpacking or modifying the signed base initrd.

Guest init must match the selected OCI architecture. The initrd builder checks
the ELF class, byte order, and machine ID before embedding it. An unspecified
OCI platform now defaults to the host architecture when Quocker can map it;
cross-architecture emulation requires a separately built matching guest-init
binary and an explicitly selected platform.

Each Compose service maps to a VM boundary. Compose networks become isolated
QEMU networks with explicit addressing and port exposure; named volumes become
managed VM disks or supported guest shares. A setting that has no well-defined
VM meaning is reported unsupported, rather than emulated with a container
runtime. A service restart policy restarts the VM/application workload under
Quocker's lifecycle manager. Health checks run through a guest channel or a
documented network probe, not through a container engine.

The kernel resolver selects only from an administrator-trusted, signed catalog
or an explicit local profile. Image contents cannot nominate a download URL or
signing key. The selection order is: reject OS/architecture/variant conflicts;
apply explicit Quocker constraints; inspect distro, shipped modules, and
userspace ABI as compatibility evidence; then select the highest-priority
maintained generic kernel that satisfies all hard requirements. A module
release match is required when the guest needs those modules, but mere presence
of unused module files should be reported as evidence rather than silently
forcing a particular kernel. The result records the reason and evidence, and
an unsatisfied explicit requirement is an error. The catalog and kernel bundle
are fetched and digest-verified before the VM starts.

The current prototype treats each detected `/lib/modules` release as a hard
exact catalog constraint. This is conservative but can reject images that
merely contain unused module files; a later compatibility policy should
distinguish modules required by the declared workload from unused files and
report the evidence either way.

The supported Dockerfile compatibility boundary is the image artifact itself:
Quocker consumes the built OCI image reference and its OCI config defaults.
It does not run Dockerfile `RUN` steps on the host or claim that Dockerfile
build semantics create a bootable VM. A future Quockerfile may reuse familiar
image/build declarations, but build steps must run inside disposable QEMU VMs
and produce OCI-compatible layers or a Quocker VM image.

## Image compatibility and kernel resolution

An OCI image is a user-space filesystem and launch configuration. It does not
declare a kernel dependency in the general case. Quocker must not pretend that
it can infer a unique "correct kernel" from a Dockerfile or image name. Instead,
it resolves a *compatible guest profile* from several ordered evidence sources:

1. **Hard platform constraints:** OCI config `os` and `architecture`, the
   selected manifest platform, and any Compose `platform` value. These must
   agree. Unsupported OS values fail because this initial path boots Linux
   guests.
2. **Explicit Quocker requirements:** OCI config labels under the
   `io.quocker.*` namespace, or an equivalent `x-quocker.kernel` service
   declaration. These can name a kernel catalog channel/profile, minimum
   kernel version, required kernel features, acceptable kernel module ABI
   releases, and an exact catalog entry. An explicit exact ID wins only if it
   satisfies all declared requirements, the hard platform constraints, and
   catalog signature and asset digest checks.
3. **Rootfs evidence:** `/etc/os-release` and `/usr/lib/os-release` identify a
   distro family; `/lib/modules/<version>` and `/usr/lib/modules/<version>`
   identify kernel modules shipped by the image; ELF headers can expose
   userspace architecture and ABI mismatches. These are hints or constraints,
   not proof that a specific host kernel is required. Modules matching a
   kernel version are a strong incompatibility signal unless their matching
   kernel is selected.
4. **Generic maintained profile:** if no image-specific requirement exists,
   select the maintained, signed generic kernel for the platform. Record that
   it was a generic choice. Never pull and execute an arbitrary kernel
   referenced by an image label.

The catalog is the trust boundary and the source of fetchable kernel bundles.
Each signed entry should include an immutable ID, supported OS/architecture/
variant, kernel version, optional distro IDs, feature set, module ABI/version,
kernel and initrd URLs, SHA-256 digests, and priority/channel. Catalog updates
and bundle downloads use HTTPS, verify the detached signature before trusting
URLs, verify size and digest before installation, and publish atomically into
a content-addressed cache. A local administrator may configure trusted signing
keys and mirrors. Image metadata can narrow selection but cannot add trust
keys, URLs, or executable hooks.

Selection should return a structured report containing the chosen profile,
catalog digest/signing key, kernel version, platform, evidence used, ignored
weak hints, and fallback reason. Strict mode fails on conflicting hard
constraints or an unsatisfied declared feature. Default mode may use the
generic profile only when it still satisfies platform and declared minimum
requirements. An explicit Compose pin is validated by the same rules. Quocker
does not use Docker daemon storage or container execution; the OCI rootfs is
converted to a VM disk and the kernel/initrd are boot inputs to QEMU.

Example image metadata (advisory minimum plus required features):

```dockerfile
LABEL io.quocker.kernel.minimum="6.6"
LABEL io.quocker.kernel.features="virtio_blk,virtio_net"
LABEL io.quocker.kernel.module_releases="6.8.1-quocker"
```

Example Compose override:

```yaml
services:
  web:
    image: nginx:latest
    platform: linux/amd64
    x-quocker:
      kernel:
        id: linux-amd64-generic
        minimum: "6.6"
        require: [virtio_blk, virtio_net]
        module_releases: ["6.8.1-quocker"]
```

These labels and extension are Quocker-specific requirements; normal Docker
Compose ignores them. Detected module directories remain advisory unless a
release is explicitly declared here. The only universally portable
declarations remain the image reference and Compose fields that already have
compatible VM semantics.

## Compose and Quockerfile surface

- `services.<name>.image` accepts an OCI reference or an existing local VM
  image. Inspection makes the resolved source and digest explicit.
- `platform` selects a supported OCI platform; a mismatch is an error unless
  the user explicitly requests emulation.
- `x-quocker.kernel` may pin a catalog kernel or local kernel/init pair.
- OCI `Entrypoint`, `Cmd`, `Env`, `WorkingDir`, and `User` provide defaults;
  Compose `entrypoint`, `command`, `environment`, and `working_dir` override
  them with documented Compose precedence.
- OCI-declared volumes become explicit VM volume declarations or are reported
  as unsupported. They must not be silently created as ephemeral container
  mounts.
- A future `build:` implementation uses Quockerfile instructions to produce
  VM rootfs layers. Familiar Dockerfile forms can be accepted only where their
  semantics are well-defined; build commands execute inside disposable VMs,
  not containers. No Docker daemon or container runtime is required.

## Compatibility and rollout

The first stage is implemented in `scripts/quocker-oci.c`: `quocker pull`
fetches an index/manifest, validates the selected config platform, and stores
SHA-256-verified config/layer blobs. It supports HTTPS Bearer challenge
authentication and an HTTPS mirror selected with
`QUOCKER_REGISTRY_MIRROR`. Layer tar files are materialized into a staging
rootfs with whiteout support, directory-relative safe extraction, and limits
on entries, expanded bytes, and per-entry extended attributes. UID/GID/mode
and OCI xattrs are stored as Quocker user xattrs; privileged OCI xattrs such as
``security.capability`` remain inert metadata on the host. Character/block
devices and FIFOs are inert placeholders with guest metadata; host device nodes
are never created. A C converter now creates a raw ext4 image from staged
rootfs trees, restores guest uid, gid, mode, and OCI xattrs through libext2fs,
and maps staged devices and FIFOs to ext4 special inodes. ``quocker pull`` now
creates this raw ext4 artifact beside the content-addressed staged rootfs after
checking its estimated virtual size against the OCI cache quota. The derived
disk is content-addressed by the selected manifest and is removed by cache
pruning. Conversion is not yet connected to ``up``. A bounded C helper reads
guest ``/etc/os-release`` (or its
conventional ``/usr/lib/os-release`` symlink) without following arbitrary guest
symlinks and reports ``ID``, ``ID_LIKE``, and ``VERSION_ID`` as kernel-selection
hints. A first C catalog selector is available as ``quocker kernel select``.
It matches OS, architecture, and variant; ranks exact distro IDs above
``ID_LIKE`` family matches and generic platform entries; accepts an explicit
catalog ID; and verifies the selected kernel/initrd SHA-256 digests. Version 1
catalogs require a detached Ed25519 signature verified against a protected
local public key. Trust-key provisioning and update scheduling remain manual.
``pull`` and OCI-backed ``up`` can select a local catalog kernel. ``up``
converts the staged rootfs to ext4 when needed, downloads missing catalog
kernel/initrd assets over HTTPS, verifies declared sizes and SHA-256 digests,
builds the guest initrd with OCI runtime defaults and Compose overrides,
launches QEMU with the selected kernel/initrd, and uses a digest-specific
writable overlay. OCI volumes are
rejected until translated into explicit VM disks. Catalog and kernel/initrd
assets can be fetched by ``quocker kernel fetch`` or automatically by OCI
``up``. ``quocker kernel update`` verifies and installs a signed catalog from
an administrator-provided HTTPS source using a pre-provisioned Ed25519 key.
Trust-key provisioning and catalog-source policy remain administrator
responsibilities. The code does not yet read Docker credential
files or credential helpers. The 20 GiB aggregate cache limit
(`QUOCKER_OCI_CACHE_LIMIT` overrides it) includes compressed blobs and staged
rootfs files, and `quocker prune` removes both. It does not yet have
reference-aware garbage collection. `tests/quocker/registry-hello.yaml` is a
manual public-registry smoke fixture; it is deliberately not part of offline
Meson tests.

The C image-config reader also parses OCI runtime defaults (``Entrypoint``,
``Cmd``, ``Env``, ``WorkingDir``, ``User``, and ``Volumes``) and has a helper
for merging Compose overrides by precedence. ``pull`` reports these defaults
without exposing environment values, and OCI ``up`` merges them with supported
Compose overrides. OCI labels ``io.quocker.kernel.id``,
``io.quocker.kernel.minimum``, and ``io.quocker.kernel.features`` are now
validated and parsed by the C image-config reader and summarized by ``pull``.
The lower-level C catalog API now filters candidates by the declared minimum
numeric version and required feature set; catalog entries may carry those
fields. Rootfs inspection detects kernel module release directories under
``/lib/modules`` and ``/usr/lib/modules`` through confined paths. These
releases are advisory: the selector prefers a catalog match when the stronger
compatibility scores tie, and reports both matches and unmatched releases.
The ``/lib -> usr/lib`` symlink is followed only when it has that exact
conventional target. ``quocker pull`` and OCI ``up`` apply image labels plus
``x-quocker.kernel`` overrides to the configured local catalog. OCI ``up``
fails closed when the catalog is missing, a hard requirement is unsatisfied,
or an asset digest check fails. ``quocker kernel select --json`` exposes
platform constraints, explicit requirements, the selection reason, module
evidence, declared asset digests, and an explicit verification state. Signed
catalog refresh and HTTPS bundle fetching are implemented; automated trust-key
provisioning and catalog-source discovery are not.

Remaining stages: (1) publish and maintain a production trust-rooted kernel
catalog and document its source/update policy; (2) implement explicit VM
network and volume translations, guest readiness, and exit/status reporting;
(3) add registry credential-file and credential-helper support plus
reference-aware cache garbage collection; and (4) define and expand the
Quockerfile build subset. Kernel asset cache quota and garbage collection also
remain open. OCI-backed ``up`` now covers catalog selection, bundle fetching,
selection, rootfs conversion, initrd construction, and QEMU launch. Maintain
fixtures for multi-architecture indexes, private registries, distroless
images, whiteouts, malformed archives, and image config precedence.
Compatibility reporting must say which fields are translated, unsupported, or
invalid. No feature may silently fall back to container execution.

## Decisions still needed

- Initial guest architecture/platform matrix and whether emulation ships in the
  first OCI release.
- Kernel catalog source, signing authority, update policy, and supported local
  kernel override format.
- Raw ext4 is the base filesystem output; a per-project qcow2 overlay is the
  intended writable layer. Final disk sizing, cache accounting, and lifecycle
  details still need implementation.
- The exact guest-init channel (virtio-serial, vsock, or boot-time config) and
  its security/update model.
- Whether Quockerfile accepts selected Dockerfile instructions and how each
  instruction is represented in reproducible VM build layers.

## Standards references

- [OCI Image Configuration](https://github.com/opencontainers/image-spec/blob/main/config.md)
  defines the OS/architecture, rootfs layer references, and runtime defaults,
  but not a guest kernel or boot process.
- [OCI Image Manifest](https://github.com/opencontainers/image-spec/blob/main/manifest.md)
  and [Image Index](https://github.com/opencontainers/image-spec/blob/main/image-index.md)
  define config/layer references and platform selection.
- [OCI Image Layer](https://github.com/opencontainers/image-spec/blob/main/layer.md)
  defines layer changesets and whiteout behavior.
- [OCI Distribution Specification](https://github.com/opencontainers/distribution-spec/blob/main/spec.md)
  defines registry manifest and blob distribution APIs.
- [Linux initramfs buffer format](https://docs.kernel.org/driver-api/early-userspace/buffer-format.html)
  defines concatenated compressed/uncompressed CPIO archives and later-entry
  replacement behavior used by the Quocker initrd builder.
