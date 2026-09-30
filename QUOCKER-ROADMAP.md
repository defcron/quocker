# Quocker development roadmap

**Status:** draft for project-owner review
**Written:** 2026-09-29
**Applies to:** the `quocker` branch of this QEMU fork

This document inventories the work required to take Quocker from its current
prototype to a documented, supportable Compose-style VM manager. It is a
planning baseline, not a promise that every Docker Compose container feature
has a meaningful VM equivalent.

## Execution ledger

- **Upstream base:** fetched the official QEMU `stable-11.1` branch at
  `4fc49f46dc` (QEMU 11.1.2). A live `git ls-remote` check still reports that
  exact commit as `stable-11.1`; only `quocker` was fast-forwarded and the
  repository's `master` branch was not changed.
- **Product boundary confirmed by the owner:** Quocker manages QEMU/KVM
  configuration and orchestrates applications and networks running in or with
  those VMs. Container management is out of scope.
- **Image direction requested by the owner:** allow familiar OCI/Docker Hub
  image references in Compose-shaped files and turn their userland filesystem
  into a bootable VM guest by supplying a kernel and Quocker guest init. The
  guest is a VM; Quocker does not manage containers.
- **Bootstrap direction:** start a trusted Quocker initramfs, mount the
  converted OCI root disk, and exec the OCI `Entrypoint`/`Cmd` as the guest
  workload. This supports minimal and distroless image filesystems without
  requiring their own init system. Kernel downloads come only from an
  administrator-trusted signed catalog or explicit local profile; image
  metadata can constrain selection but cannot supply executable kernel URLs
  or trust keys. Compose networks and volumes must map to explicit VM
  resources, and untranslatable container-only fields must be reported.
- **OCI architecture:** initial design is recorded in
  `QUOCKER-OCI-VM-DESIGN.md`. The C CLI now has a first OCI pull path for
  HTTPS registries: it parses image refs, selects Linux platform manifests,
  obtains Bearer tokens, follows bounded HTTPS blob redirects without
  forwarding credentials across hosts, verifies cached blobs, and materializes
  OCI layers to staging rootfs directories with whiteouts, path protections,
  and resource limits. ``pull`` now also converts the staged filesystem to a
  content-addressed raw ext4 guest disk after preflighting the configured OCI
  cache quota. This disk is structurally checked in tests and in the public
  registry smoke flow. A statically linked guest-init binary now mounts the
  ext4 root and supervises the OCI command; a C initrd builder appends it and
  the bounded runtime config to a digest-verified catalog initrd. OCI-backed
  `up` now selects catalog assets, downloads missing remote bundles from signed
  catalog HTTPS URLs, verifies declared sizes and digests, assembles that
  initrd, converts the rootfs when needed, and passes the kernel, initrd, and
  root disk to QEMU. OCI UID/GID/mode plus extended
  attributes are retained as inert Quocker xattrs in the staging tree; this
  does not apply privileged attributes to the host. A bounded C helper reads
  guest distro ID, ID_LIKE, and VERSION_ID through a confined os-release path
  for kernel-selection hints. A local catalog selector verifies and ranks
  kernel/initrd assets, and OCI `up` uses that result for VM startup.
  OCI runtime defaults (Entrypoint, Cmd, Env, WorkingDir, User, and Volumes)
  are parsed in C and merged with supported Compose overrides for OCI `up`.
  The owner has clarified the OCI compatibility goal: reuse
  Docker Hub image declarations as VM guest userlands, with a Quocker-supplied
  kernel. The evidence-based kernel matching and signed remote bundle-fetch
  design is now documented in `QUOCKER-OCI-VM-DESIGN.md`. Its new Docker Hub
  compatibility-layer contract defines OCI images as reusable VM userlands,
  with a catalog kernel/initrd and Quocker bootstrap assembled at runtime;
  selection uses platform constraints, explicit requirements, rootfs evidence,
  then a compatible generic profile. `quocker kernel update` verifies the
  signed catalog and installs it atomically; `quocker kernel fetch` supports
  explicit bundle prefetch. OCI `up` fetches selected bundles automatically.
  The OCI image config
  parser now validates Quocker kernel labels for exact catalog ID, minimum
  numeric version, and required feature tokens; `pull` summarizes them without
  exposing runtime environment values. The lower-level catalog selector now
  enforces minimum version and feature requirements, and `pull` passes OCI
  labels plus Compose `x-quocker.kernel` overrides into that selector when a
  local signed catalog is available. Rootfs inspection detects shipped
  kernel module releases through confined paths. These releases are advisory;
  matching catalog entries win ties, and the selector reports matched or
  unmatched evidence. `quocker kernel select --rootfs`, `pull`, and OCI `up`
  apply image labels plus Compose kernel hints.
  OCI images do not generally declare a unique kernel requirement, so platform
  is a hard constraint, labels can declare requirements, distro identity ranks
  candidates, and module directories provide advisory compatibility evidence.
  OCI ``io.quocker.kernel.module_releases`` and Compose
  ``x-quocker.kernel.module_releases`` can declare hard acceptable ABI
  requirements. `quocker kernel select --json` reports constraints, selection
  evidence, module matches, and verified asset digests.
- **Language/build:** the Python Quocker entry point has been replaced by
  `scripts/quocker.c`, `scripts/quocker-oci.c`, `scripts/quocker-rootfs.c`,
  `scripts/quocker-kernel.c`, `scripts/quocker-image.c`, and
  `scripts/quocker-disk.c`, `scripts/quocker-registry-auth.c`, using GLib,
  libyaml, libcurl, json-glib,
  libarchive, OpenSSL, and libext2fs as an optional Meson target. The full QEMU
  11.1.2 build and the updated Quocker target compile completed. The Quocker
  Meson suite currently has 76 focused tests; its latest run reported 50
  passes, 26 expected failures, and no unexpected failures.
  Tests include Docker config registry-auth parsing, Compose dependency order,
  and QEMU user-network port mapping validation. The rootfs suite covers
  whiteouts, guest ownership and extended-attribute metadata, traversal,
  symlink-parent, and hard-link target rejection, gzip-compressed layer
  materialization, manifest/layer provenance metadata and cache validation,
  inert device placeholders, pruning, and the cache-limit completion-marker
  regression. Rootfs detection cases cover OS-release confinement, the standard
  usr/lib symlink, embedded-NUL rejection, and module-release detection. Five kernel-catalog cases cover signed catalogs,
  distro priority, family/pin selection, image minimum-version/feature
  matching, CLI output, platform
  matching, and asset digest verification. Two disk-conversion cases verify
  ext4 metadata replay, character-device/FIFO inode creation, and no-host-node
  behavior. The updated public
  Docker Hub `hello-world:latest` pull verified manifest/config/layer
  download, cache verification, and rootfs materialization. OCI-to-QEMU launch
  is wired, but no trusted production kernel catalog is provisioned here, so a
  complete boot smoke test and full Compose runtime compatibility remain open.
- **Roadmap state:** Phase 0 has a confirmed VM-only boundary and an OCI-to-VM
  design, with platform/kernel/init decisions still open. Phase 1 now installs
  a Sphinx-generated man page and Bash completion with the optional target;
  structured diagnostics and broader packaging polish remain. Phase 2 is
  a partial parser that validates merged YAML against the pinned Compose JSON
  Schema 2020-12 document. Its C validator implements the assertion keywords
  used by the pinned schema and refuses to run if the snapshot introduces an
  unimplemented standard assertion keyword. Validation errors include source
  file, line, and column, with regressions for top-level, nested, merged, and
  included-file errors. `config --capabilities` reports the runtime status of
  service fields present in the merged configuration. Service-level
  `x-quocker` version 1 now validates allowed fields and value types at config
  load time, with explicit unknown-field/version rejection.
  Phase 3 now orders
  selected services after their `depends_on` dependencies and offers
  `up --dry-run`; optional dependencies marked `required: false` warn and skip
  missing or inactive-profile services. VM lifecycle `start`, `stop`,
  `restart`, and `kill` commands
  now use saved QEMU process/disk state; stop preserves VM state, restart
  follows dependency ordering, and kill accepts named/numeric signals. A
  per-project lifecycle lock serializes state-changing commands and volume
  removal checks across local Quocker processes. A fake
  QEMU integration test covers stop, start, down, wait, and restarting a VM
  after its QEMU process is abruptly killed while state remains on disk. The
  `wait` command blocks until selected saved VM processes stop, revalidates PID
  identity, and prints the guest workload exit code when an OCI guest reported
  one (signal termination maps to `128 + signal`); local bootable disks and
  abrupt guest exits may have no workload code.
  `service_completed_successfully` waits for the dependency VM to exit and
  starts dependents only after guest-reported status zero. `up --wait` waits
  for OCI workload exec readiness and accepts `--wait-timeout`; local bootable
  disks can only be checked for a live QEMU process. Guest init reports
  readiness only after successful `exec`. Crash recovery has a fake-QEMU
  regression for detecting the stopped process and starting the saved VM
  again; recovery from ambiguous/reused PIDs and daemon restarts remain open.
  `service_healthy` and most command compatibility remain open.
  `version` now
  reports the Quocker interface and QEMU base versions, with a short form and
  the existing `--version` alias covered by a CLI test. New state files
  record Linux process start-time ticks; stop/status compare the recorded value
  before signaling or reporting a VM process, with a regression proving a
  mismatched/recycled PID is left untouched. New VM state is written to a
  private temporary file, fsynced, atomically renamed, and directory-synced
  before `up` reports success; the next lifecycle operation removes abandoned
  state-write temporary files under the project lock. State reads are size-
  bounded and reject symlinks and non-user-owned files. Signals bind to pidfds where the
  host kernel supports them; older kernels fall back to immediate identity
  revalidation. Older state without start-time metadata remains readable but
  has only the VM-name process check. QEMU now exposes a private
  per-service QMP socket; `pause` and `unpause` negotiate QMP capabilities and
  issue `stop`/`cont`, with a fake QMP integration check. Phase 6 now translates short and long Compose port mappings,
  numeric equal-sized port ranges, and numeric IPv4 host bindings into QEMU
  user-network forwarding rules, including ephemeral host ports (`HOST:0` or
  target-only declarations). `quocker port` reads QEMU's `info usernet` reply
  over QMP to report the allocated endpoint while the VM is running; static
  mappings remain queryable while stopped. A QEMU 11.1 monitor smoke check
  confirmed ephemeral allocation and the reported host/guest port columns.
  IPv6 host binding is rejected because the current QEMU user-network
  host-forward parser does not support it. Isolated
  Compose networks and other networking backends remain open. Phases 4–5 and
  7–10 remain open.

## 1. Product goal and compatibility contract

Quocker's intended interface is Docker Compose-shaped: users should be able to
use familiar Compose filenames, YAML, project conventions, and command forms
to describe and manage QEMU virtual machines. QEMU remains the VM engine and
Quocker remains an optional interface; ordinary QEMU tools and workflows must
continue to work.

The phrase “drop-in Docker Compose file” needs a precise contract before
implementation is considered complete. Docker Compose describes containers,
while Quocker describes VMs. For example, an OCI image such as `nginx:latest`
does not itself provide the guest kernel and boot process needed by QEMU, and a
container `command` does not automatically become a VM boot command. There are
three distinct compatibility targets:

1. **File compatibility:** accept and validate the current Compose
   Specification, including its YAML forms, extensions, and merge behavior.
2. **Command compatibility:** provide familiar command names, flags, project
   behavior, output, and lifecycle semantics where those concepts apply to VMs.
3. **Runtime compatibility:** make each accepted service setting produce the
   expected result in a guest VM.

The first two can be made broad. The third requires explicit decisions about
OCI images, guest provisioning, filesystems, and networking. Quocker must not
claim full runtime compatibility while silently dropping meaningful settings.
Each Compose attribute should be classified as **implemented**, **translated
with documented VM semantics**, **preserved but unsupported at runtime**, or
**invalid**. A strict mode should reject unsupported runtime settings; a
permissive config/inspection mode can retain them and explain their status.

The Compose Specification itself recognizes that implementations may support
different subsets and recommends reporting unsupported attributes. Quocker
vendors a schema snapshot from Compose Specification commit
``914ec15d1fa498969c0df5c1d672306db3256089``. Its checksum, license, and update
process are recorded in ``docs/compose-spec/README.rst``. This pins the
baseline but does not yet validate files against the schema:
[Compose file reference](https://docs.docker.com/reference/compose-file/),
[Compose Specification](https://github.com/compose-spec/compose-spec/blob/main/spec.md),
[Compose JSON schema](https://github.com/compose-spec/compose-spec/blob/main/schema/compose-spec.json).

## 2. Current implementation inventory

The working tree contains a native C CLI at `scripts/quocker.c` and a C
validator at `scripts/quocker-compose-schema.c`,
installed as an optional Meson target, with a user page at
`docs/tools/quocker.rst`. The prototype currently:

- searches for `compose.yaml`, `compose.yml`, both legacy `docker-compose`
  names, and two `quocker-compose` aliases;
- parses YAML mappings, validates the merged configuration against the pinned
  Compose schema, interpolates a subset of environment syntax, merges multiple
  files with mapping, sequence, command, unique-resource, and reset/override
  rules, expands YAML aliases and merge-key anchors, recursively loads Compose
  includes with resource-collision warnings, retains schema-valid fields for
  `config`, and recognizes `!reset` / `!override` tags;
- provides `up`, `down`, `rm`, `ps`, `logs`, and `config`, with direct and
  `quocker compose` command forms;
- resolves Docker Hub and generic HTTPS registry image references through
  `pull`, selects a Linux platform manifest, downloads its config and layers,
  caches the selected manifest, verifies SHA-256 content digests into the XDG
  user cache, and rehashes cache hits before reuse;
- starts QEMU from local bootable disks or OCI images converted to guest disks,
  makes qcow2 overlays, applies basic memory/CPU settings, and forwards a
  limited set of ports with QEMU user networking;
- stores state under `.quocker/PROJECT` and captures serial output.

This is prototype groundwork, not a finished Compose implementation. It has no
full Compose normalization or broad compatibility fixture suite, no
Quockerfile build workflow, no real Compose volume/network model, and only
partial file merge and interpolation semantics. The YAML reader rejects
duplicate mapping keys and extra documents, `COMPOSE_FILE` honors
`COMPOSE_PATH_SEPARATOR`, and interpolation handles nested Compose operators
on YAML values while leaving comments and keys untouched.
`.env`, repeated `--env-file`, `COMPOSE_ENV_FILES`, and
`COMPOSE_DISABLE_ENV_FILE` now use shell-over-file and later-file-over-earlier
precedence, quoted/unquoted parsing, comments, common double-quoted escapes,
single-quoted literals, unset entries, and strict explicit-file errors.
OCI-to-VM preparation and QEMU launch are integrated, but production boot,
process/state recovery, cleanup, and packaging need more coverage and review.
OCI service ``env_file`` now supports ordered path lists, interpolation,
optional long-form files, raw format, and service-environment precedence in C;
explicit unresolved environment entries remove image defaults. The Quocker
suite contains 54 tests: 32 pass, 22 produce expected failures,
and none fail unexpectedly. It includes JSON config serialization, static and
dynamic port queries, plus a fake-QEMU lifecycle integration
test covering stop, start, down, pause, unpause, kill, state preservation, and
PID reuse protection. This does not establish Compose-wide compatibility.

## 3. Phased work plan

### Phase 0 — Set the product boundary and compatibility matrix

1. **Baseline pinned; validation pending.** The vendored Compose Specification
   JSON Schema snapshot is from commit
   ``914ec15d1fa498969c0df5c1d672306db3256089`` and uses JSON Schema 2020-12.
   Review schema changes and update fixtures together. Implement runtime schema
   validation before claiming Compose file compatibility.
2. Define the supported host operating systems and architectures for the first
   release. Start with Linux x86_64 unless the project chooses a wider target.
3. Quocker manages VMs and application/network configuration for those VMs;
   container lifecycle and container runtime management are out of scope.
   Define the guest integration boundary for managing applications without
   taking responsibility for containers.
4. Define OCI image compatibility as conversion into a VM root filesystem,
   never as container execution. Specify registry/mirror lookup, OCI platform
   selection, supported manifest versions, layer materialization, boot assets,
   cache policy, and lifecycle semantics.
5. Specify how Docker `command`, `entrypoint`, `environment`, `env_file`,
   `user`, `working_dir`, health checks, and startup dependencies map into a
   guest. The initial design is a Quocker initramfs bootstrap with a narrow
   guest channel; define its configuration transport, readiness protocol, and
   behavior when it is unavailable.
6. Define capability classes for every Compose attribute: syntax-only,
   configuration-time, VM runtime, platform-dependent, unsupported, or
   invalid. Set warning and strict-mode behavior.
7. Publish explicit non-goals and a compatibility matrix so “100% compatible”
   has measurable meaning instead of implying that container and VM semantics
   are identical.

**Exit condition:** product decisions above are written down and are reflected
in user-facing compatibility promises.

### Phase 1 — Establish the Quocker code and build integration

1. Decide which Quocker code belongs in QEMU's `scripts/` tree and which
   components should be independent modules or a separate package.
2. Set the native language and dependency policy. The CLI is C and uses GLib,
   libyaml, libcurl, json-glib, libarchive, and OpenSSL; define supported library
   versions and behavior when optional dependencies are absent.
3. Make Quocker an intentional optional Meson feature or install component,
   with clear configure output and packaging rules. Avoid making the normal
   QEMU build fail when Quocker dependencies are unavailable unless enabled.
4. Define public CLI/API boundaries between the Compose frontend and QEMU
   process management. Keep configuration parsing separate from side effects.
5. **Partially implemented:** `quocker version`, `version --short`, and the
   `--version` alias report the Quocker interface version and build's QEMU base
   version. A Bash completion script installs with the optional target, and
   Sphinx generates `quocker(1)` when docs are enabled. Structured diagnostics,
   logging levels, full exit-code conventions, and other shell completions
   remain.
6. Add licensing, contribution, code ownership, and security-review notes for
   the new files.

### Phase 2 — Implement the Compose configuration model

1. **Partially implemented:** merged documents are validated against the pinned
   Compose schema. The C validator handles every assertion keyword used by the
   snapshot and fails closed if the schema adds a recognized but unsupported
   JSON Schema assertion. YAML scalar resolution and merge behavior still need
   compatibility fixtures against Docker Compose.
2. **Partially implemented:** canonical file discovery, `-f`, `COMPOSE_FILE`,
   path separators, standard input, and `--project-directory` are supported.
   Project names now follow Compose precedence, normalize config/env/directory
   names, reject invalid explicit `-p` values, update `config` output, and are
   exposed as `COMPOSE_PROJECT_NAME` before per-file interpolation. Include
   file paths are relative to their declaring file; included service path
   scopes and full project naming fixtures remain.
3. Implement `.env`, repeated `--env-file`, environment precedence, quoting,
   comments, unset variables, and the full Compose interpolation grammar,
   including nested defaults, required forms, `$$`, and interpolation only
   where the specification requires it.
4. Keep project interpolation distinct from per-service `environment` and
   `env_file` processing. Resolve all relative paths using the correct base
   file/project rules.
5. **Partially implemented:** mappings merge recursively; ordinary sequences
   append; command, entrypoint, and healthcheck test sequences replace; ports
   merge by IP/target/published/protocol; and volumes, secrets, and configs
   merge by target across short/long forms. Supported capability, device-rule,
   exposure, external-link, security, placement, and generic-resource lists
   deduplicate identical entries. `!reset` and `!override` are recognized.
   Full Compose short/long normalization and compatibility fixtures for every
   resource and merge field remain.
6. **Partially implemented:** `x-` extension fields are retained; YAML aliases
   and `<<` merge keys with mapping/sequence sources work within one file, with
   explicit fields overriding anchor defaults; and `include` supports recursive
   short syntax and long-form `path` values, including path lists, with
   file-relative lookup, project-specific `.env`/`env_file` interpolation,
   `project_directory`, and collision warnings. Included local disk image,
   service `env_file`, and top-level config/secret file paths use their
   included project directory; broader included-service path resolution and
   reusable fragments remain unimplemented. Service `extends` supports
   same-file and external-file bases, recursive resolution, mapping merge,
   sequence append/deduplication, Compose keyed-resource merge behavior, and
   cycle/depth checks. External base services are not imported into the final
   service model, and their local image and `env_file` paths are resolved from
   the declaring file.
7. **Schema validation implemented; capability policy partial.** The pinned
   schema validates top-level resources and service attributes, including
   nested fields and `x-` extensions. Errors include an instance path and the
   source YAML file, line, and column, preserved through file merges and
   Compose includes. `config --capabilities` now labels each service field
   present in the configuration as QEMU-mapped, partially supported,
   OCI-workload-only, unsupported, Quocker-specific, selection-only, or
   preserved-only, and separately reports declared top-level volumes,
   networks, configs, and secrets with runtime status. Nested service-value
   paths are also emitted and inherit their parent field's status. Runtime
   preflight now accounts for the complete pinned Compose service-field set:
   every field is mapped, partially supported, OCI-workload-only,
   selection-only, Quocker-specific, unsupported, or explicitly preserved-only.
   Non-empty unsupported settings produce field-specific errors during a
   preflight of all selected services before any VM starts. Per-leaf runtime
   classifications, source-aware reporting for other schema diagnostics,
   strict versus permissive handling, and exhaustive reporting for every
   Compose resource remain.
8. **Partially implemented:** profiles and project-name resolution work for
   service selection and interpolation. `config` emits YAML or JSON, lists
   sorted services, profiles, images, volumes, or networks, and prints the
   interpolation environment; `-o`/`--output` atomically writes rendered YAML
   or JSON. Schema-aware normalization, image resolution, path output modes,
   and the remaining Docker Compose config flags remain.
9. **Partially implemented:** service-level `x-quocker` is versioned as
   version 1, with omitted version treated as legacy shorthand for version 1.
   The strict C validator accepts only `image` and `kernel` extension fields,
   validates kernel IDs, minimum versions, feature lists, and module-release
   lists, and rejects unknown fields or versions during `config`. Kernel
   overrides are rejected for local bootable disks, where QEMU uses the guest
   kernel contained in the disk. Top-level namespace design and compatibility
   fixtures for future extension versions remain.

### Phase 3 — Reach useful Compose CLI compatibility

1. **Command and flag inventories documented; full parity remains open.** The
   Quocker manual maps the current Docker Compose CLI command set to VM-adapted,
   partial, or unimplemented behavior and records the implemented per-command
   options plus the major unsupported options, using official CLI references
   checked on 2026-09-30. `ps -q`/`--quiet` now prints generated VM names, with
   `ps` defaults to running saved VMs and accepts `-a`/`--all` to include
   stopped VMs. `--status` and `--filter status=` select the representable
   `running` and `exited` process states; Docker-only lifecycle statuses are
   rejected. `ps --services` prints matching service names, and `ps --format
   json` emits documented VM-specific JSON Lines. Keep this inventory current
   as flags are implemented. `ps` also lists orphaned saved VMs by default;
   `--orphans=false` filters those out. `up` and `down` accept
   `--remove-orphans`; cleanup uses the same guarded shutdown path as declared
   VMs. Dry runs display orphan cleanup without changing saved state. Removing
   an orphan deletes its VM state but preserves its disk unless ``--volumes``
   is requested. `rm` removes stopped VM state and its service overlay, refuses
   running VMs unless `--stop` is passed, supports a stop timeout, and
   preserves named project volumes; `rm --volumes` removes only the selected
   service's generated anonymous volume disks, including volumes declared by
   the OCI image.
   The option inventory for less common commands still needs exact
   option-by-option coverage.
2. Complete core commands and their normal flags: `up`, `down`, `ps`, `logs`,
   `config`, `start`, `stop`, `restart`, `kill`, `rm`, `pause`, `unpause`,
   `pull`, `build`, `create`, `run`, `exec`, `cp`, `port`, `events`, `top`,
   `wait`, `ls`, and `version`. `images` lists selected services' declared
   image references and saved VM running/stopped/not-created state; `volumes`
   aliases the project-scoped `volume ls` disk inventory. `start`,
   `stop`, `restart`, `kill`,
   `pause`, and `unpause` now have initial saved-VM process handling; pause and
   resume use QMP, and the fake-QEMU integration test exercises these commands.
   `logs --tail N|all` selects saved serial lines, `--no-log-prefix` controls
   service prefixes, and `--follow` streams complete lines with consistent
   formatting. `wait` blocks until selected VM processes stop and checks the
   recorded process identity. It prints OCI guest workload exit codes when the
   guest completion marker is present and maps signal termination to
   `128 + signal`; local bootable disks and abrupt guest exits may not provide
   a workload exit code.
   `up --wait` waits for OCI workload exec readiness and accepts
   `--wait-timeout`; local bootable disks can only be checked for a live QEMU
   process. `stop`, `restart`, and `down` accept `-t`/`--timeout SECONDS`;
   the default grace period is ten seconds and zero requests immediate forced
   termination. Other timeout behavior and complete flag compatibility remain
   unfinished.
   Implement only after Phase 0 defines
   VM semantics; explain commands with no meaningful VM equivalent.
   `port SERVICE PRIVATE_PORT[/PROTOCOL]` reports configured static host
   bindings and queries QMP for dynamically allocated host ports while a VM is
   running. Crash recovery and complete flag compatibility remain unfinished.
3. **Partially implemented:** `up` selects required `depends_on` dependencies,
   orders them before dependents, and rejects cycles and missing required
   services. Short syntax and long syntax with `service_started` work;
   `up --dry-run` exposes startup order and `down --dry-run` exposes reverse
   teardown order. `service_completed_successfully` waits for the dependency
   VM to stop and requires guest-reported exit status zero. `service_healthy`
   and dependency restart behavior await guest health and recreation tracking.
   Explicitly targeted services bypass inactive-profile filtering; selected
   target profiles become active for dependency validation, and incompatible
   required profiled dependencies are rejected. Optional dependencies marked
   ``required: false`` warn and skip missing or inactive-profile services.
   Complete profile validation, scaling
   where appropriate, recreation policies, readiness timeout
   handling for other lifecycle operations, and interruption/signals.
4. Match project naming, labels/metadata, project listing, working directory,
   file precedence, exit codes, and common output/TTY behavior.
5. **Entry-point compatibility verified:** both `quocker compose ...` and the
   direct `quocker ...` form run through the same option parser; a regression
   test compares normalized `config` output for both forms. Broader command
   flag parity, including global versus subcommand flag collisions, remains
   open.
6. Specify behavior for Docker Compose integrations or plugins (`watch`,
   `convert`, `alpha`, and future commands); do not silently advertise
   compatibility for unimplemented commands.

### Phase 4 — Define images and guest lifecycle

1. Build a VM image model with explicit architecture, firmware, machine type,
   CPU model, acceleration, boot device, and guest OS requirements.
2. **Partially implemented:** OCI registry acquisition for Docker Hub and
   per-registry HTTPS mirror mappings from `$XDG_CONFIG_HOME/quocker/registries.json`,
   plus the backward-compatible global mirror environment override. Bearer
   challenge token requests, platform manifest selection, bounded HTTPS
   redirect handling, and verified config/layer blob caching, a 20 GiB default
   aggregate cache limit, and `quocker prune`. Pruning records per-service
   overlay/base-disk references, retains complete OCI manifest/config/layer
   blobs plus rootfs and ext4 base data for referenced overlays, and removes
   unreferenced content. Stale references are reconciled. Unmarked legacy base
   disks are kept for safety. Inline
   credentials from Docker CLI `config.json` auths
   are sent only to the matching HTTPS token realm; external credential
   helpers and `credsStore`/`credHelpers` executables in `PATH` are supported
   through bounded, timed, direct subprocess execution. Reference-aware
   pruning now records per-service overlay/base-disk leases and keeps base
   disks and OCI image content while corresponding overlays exist; unreferenced
   staged trees and compressed blobs remain reclaimable. Unmarked legacy disks
   remain protected until that VM is started and Quocker records its reference.
  Bounded retries now cover transient connection failures and HTTP 408, 425,
   429, and selected 5xx responses; ``Retry-After`` is honored up to 30 seconds
   with bounded exponential fallback. Remaining work: broader migration/reconciliation coverage, authenticated
   mirror-specific credentials, and additional registry conformance coverage.
3. Finish the OCI registry acquisition requirements: Docker Hub and configured mirrors,
   registry authentication, platform-specific index selection, content
   negotiation, digest/size verification, and safe handling
   of cross-host blob redirects. Store immutable blobs by digest with bounded,
   inspectable cache and explicit prune/quota controls.
4. **Partially implemented:** OCI layer tar changesets are materialized into a
   staging rootfs with whiteout behavior, directory-relative path/symlink
   protections, entry/expanded-size limits, digest-addressed staging, and
   rollback on extraction failure. OCI UID/GID/mode and extended attributes
   are retained in Quocker user xattrs. Character/block devices and FIFOs use
   inert placeholder files with guest type/device metadata; sockets remain
   unsupported. A C converter now builds a raw ext4 image with mke2fs and
   libext2fs, restoring guest ownership, mode, and OCI xattrs without creating
   host device nodes. The converter maps staged character/block devices and
   FIFOs to ext4 special inodes, and `pull` stores the converted guest disk
   beside the manifest-addressed rootfs after cache-quota preflight. OCI `up`
   now invokes disk conversion when needed. Regressions reject path traversal,
   symlink-parent traversal, and hard-link target traversal, and materialize a
   gzip-compressed layer and reject a truncated gzip archive without retaining
   partial output. A `.provenance` sidecar records the selected manifest digest
   and each materialization layer's SHA-256; cache reuse requires the record to
   match the supplied layer files. Additional malformed archive and compression
   fixtures remain open.
   Add broader malicious-archive and compression fixtures.
5. **Partially implemented:** a C kernel catalog selector matches OS,
   architecture, and variant, prefers exact distro IDs over `ID_LIKE` family
   matches and generic entries, permits an explicit catalog ID, and verifies
   selected kernel/initrd SHA-256 digests. Version 1 catalogs require detached
   Ed25519 signatures and are trusted through a protected local public key.
   `pull`, `quocker kernel select`, and OCI `up` apply local catalog selection.
   A statically linked guest-init program
   mounts the ext4 root, applies the OCI user/working directory/environment,
   launches the command as a child, forwards signals, reaps children, and
   reports readiness only after successful exec plus exit status on serial. The
   host supports `up --wait` and `--wait-timeout`, using that readiness marker
   for OCI guests; local bootable disks can only be checked for a live QEMU
   process. Serial readiness and exit-status parsing have focused C unit tests.
   A C initrd builder appends this init and a
   bounded runtime config to the digest-verified catalog initrd. Remaining
   work: provision trust keys, maintain kernel/initrd assets and catalog
   updates, implement guest network configuration, and persist structured
   readiness/shutdown in host state. The host consumes the guest exit marker
   for `service_completed_successfully` and `wait`. OCI `up` currently relies
   on QEMU user networking and a guest DHCP kernel command line.
6. Kernel selection must be inspectable and overrideable. OCI metadata does not
   specify a kernel or boot process, so image-specific automatic detection is
   heuristic: architecture and OS are reliable inputs, while distro identity
   may be absent (especially in distroless images). Provide generic-kernel
   fallback, explicit kernel pinning, validation/boot probes, and a clear error
   when no supported kernel/init combination exists; never imply perfect
   per-image kernel inference.
7. Map OCI image config fields (Env, WorkingDir, User, Entrypoint, and Cmd)
   into VM guest launch configuration. Translate declared Volumes into
   explicit VM disks or reject them with an actionable diagnostic. Document
   where VM behavior intentionally differs from Docker container behavior.
8. Implement `build` only with a specified reproducible guest-image build
   format and cache policy. A Dockerfile may be reused only through a defined
   Quockerfile subset whose filesystem-changing steps run in disposable build
   VMs; Quocker must not start a container runtime to build or run images.
9. Implement guest provisioning for commands, environment, users, hostnames,
   files, and boot configuration. Support idempotence, retries, guest-agent
   readiness, and safe secret delivery.
10. Define QEMU acceleration fallback (KVM versus TCG), host capability checks,
   architecture emulation, firmware delivery, and diagnostics for boot failure.
11. Implement service readiness and health states, dependency ordering,
   restart policies, graceful shutdown, and startup rollback.

### Phase 5 — Build the storage model and lifecycle controls

1. **Partially implemented:** OCI services boot from immutable cached base
   disks with per-service qcow2 overlays. Named and anonymous Compose volume
   mounts, including OCI image-declared volume targets and read-only mounts,
   use persistent project-scoped sparse ext4 disks attached over virtio. The
   guest init mounts those disks before pivoting into the root filesystem.
   Bind mounts and tmpfs remain unsupported.
2. **Partially implemented:** Compose volume short syntax and a limited long
   syntax support local named and anonymous volumes with project scoping.
   External volumes, non-local drivers, driver options, and volume suboptions
   fail explicitly. Existing disks retain their original size; new disks
   default to 1 GiB and ``QUOCKER_VOLUME_SIZE`` can select 64 MiB through
   1 TiB.
3. **Partially implemented:** volumes are created on demand with logical-name
   metadata, listed and inspected per project, and removed with ``down
   --volumes`` or ``quocker volume rm``. Removal has a dry-run preview and
   refuses while a project service has a live or unverified PID. A volume lock
   serializes disk creation/removal and quota checks; a project lifecycle lock
   protects VM state transitions and live-VM checks across local Quocker
   processes. QEMU file locks are enabled on managed volume attachments, and
   deletion takes exclusive locks compatible with QEMU's raw-file permission
   locks. Write-side recovery repairs interrupted metadata replacement and
   cleans abandoned staging files and orphan sidecars. Richer ownership
   records, full VM/storage crash recovery, and cross-host coordination remain.
4. **Partially implemented:** project-scoped volume disk quotas default to
   20 GiB and are configurable with ``QUOCKER_VOLUME_QUOTA``; ``volume ls``
   and ``volume df`` report virtual capacity and actual filesystem allocation.
   Volume creation and removal use a project lock so concurrent creates cannot
   exceed the quota. The global OCI cache quota also defaults to 20 GiB and is
   configurable with ``QUOCKER_OCI_CACHE_LIMIT``. It counts logical bytes for
   registry blobs and materialized cache files; ``quocker prune`` removes
   unreferenced content while preserving content used by VM overlays. Cache
   admission now fails closed when a recursive usage scan cannot be completed,
   instead of treating an unreadable subtree as empty. Per-image accounting,
   configurable retention policies, and broader lifecycle/garbage-collection
   policy remain.
5. **Partially implemented:** project-local locks serialize volume mutations
   and CLI VM lifecycle commands; QEMU-compatible disk locks prevent deleting
   active volume files. Write-side recovery repairs volume metadata and cleans
   stale create/delete artifacts. Handle backing-chain compaction, snapshots,
   storage consistency, VM crash recovery, image migration, cross-host
   coordination, and safe deletion.
6. Define encryption-at-rest and secret handling for base disks, overlays,
   volume content, and provisioning data.

### Phase 6 — Implement networking as a first-class VM feature

1. **Partially implemented:** the default backend is per-VM QEMU user
   networking; explicit ``network_mode: none`` starts QEMU with no network
   device. Define isolated project networks, service DNS names, aliases, DHCP,
   and inter-service connectivity.
2. **Partially implemented:** translate short and long port mappings, equal-
   sized numeric ranges, TCP/UDP protocols, and numeric IPv4 host IPs to QEMU
   user networking, including dynamic host-port allocation and QMP lookup of
   assigned endpoints. `up` preflights selected services (including selected
   dependencies) for overlapping static host-port bindings before launching
   any VM and probes static IPv4 TCP/UDP bindings against the host socket table
   to report ports already held by unrelated local processes. This probe cannot
   reserve a port, so QEMU's bind remains authoritative if a race occurs. IPv6
   host bindings are rejected because QEMU's current user-network host-forward
   parser does not support them. Broader syntax fixtures remain.
3. Select and document backends (QEMU user networking, bridge/tap, or another
   backend) with install and privilege requirements. Make backend choice
   explicit and inspectable.
4. Define `host`, `none`, external, internal, attachable, and network-driver
   semantics; reject unsupported modes before launching any VM.
5. Add firewall integration, least privilege, collision-safe resource names,
   teardown on crashes, and diagnostics for host permission failures.
6. Ensure network state is cleaned up without affecting unrelated host or QEMU
   resources.

### Phase 7 — Complete Compose resources and service settings

1. Map `configs` and `secrets` into guest files with explicit ownership,
   permissions, read-only rules, lifetime, and secure delivery.
2. Map Compose CPU, memory, block I/O, device, security, logging, and
   deployment resource settings to QEMU and guest controls where possible.
3. Implement or classify each service field, including annotations, labels,
   dependencies, health checks, logging, namespace settings, platform,
   pull policy, scale/replicas, stop behavior, and deploy placement/update
   policies.
4. Make every unsupported field visible in `config` and produce actionable
   runtime diagnostics; never silently discard settings that can change
   observable behavior.
5. Keep Compose fields and Quocker extensions distinguishable in output,
   documentation, and error messages.

### Phase 8 — Harden process management and security

1. **Partially implemented:** new state records Linux `/proc` start-time ticks
   and validates them with the PID and QEMU name; signals use pidfds when
   supported and otherwise revalidate immediately before signaling. QEMU gets
   a private per-VM QMP socket for pause/resume. A fake-QEMU regression now
   kills a running VM abruptly, confirms `ps --all` reports it stopped, and
   restarts it from saved state. Recover stale state safely when a PID has been
   reused, close the fallback race on older kernels where possible, cover
   daemon restarts, and finish concurrent CLI invocation locking.
2. Define foreground and detached modes, console access, serial/agent logs,
   log rotation, event streams, and cleanup on signals.
3. Run QEMU with least privilege and a reviewed sandbox configuration. Define
   access to KVM, host files, sockets, devices, and networking.
4. Protect YAML parsing, interpolation, image paths, symlinks, state files,
   lock files, generated command lines, and cleanup operations against unsafe
   input and path traversal.
5. Review guest/host boundaries and threat models for shared folders, device
   passthrough, clipboard/console access, secrets, and network exposure.
6. Add resource limits and safe failure behavior so partial stack startup,
   disk-full conditions, and failed cleanup do not corrupt unrelated projects.

### Phase 9 — Compatibility, integration, and regression coverage

1. Create a pinned Compose fixture corpus covering every schema section,
   interpolation form, merge rule, short/long syntax, extension, profile, and
   invalid configuration class.
2. Compare Quocker `config` output with Docker Compose's normalized model for
   compatible fixtures; record intentional differences explicitly.
3. Add unit tests for parser, validation, merge, path, state, storage, network,
   and command construction logic.
4. Add QEMU integration tests for boot, guest provisioning, health, networking,
   storage persistence, stop/restart, cleanup, and crash recovery.
5. Run tests with KVM and without KVM (TCG), and exercise supported host/guest
   architectures and minimum dependency versions.
6. Add security and failure tests for malicious YAML, unsafe paths, port
   conflicts, interrupted launches, stale state, exhausted disk, and failed
   QEMU/qemu-img operations.
7. **Partially implemented:** a fork-only GitLab CI job configures Quocker on
   Ubuntu 24.04 with x86_64 system emulation, builds QEMU and Quocker with
   documentation enabled, and runs the Quocker test suite. It also configures
   a Clang build and runs Clang's core static analyzer checks over every
   Quocker C source. CI also verifies the vendored Compose schema against its
   pinned checksum. Formatting, KVM/TCG coverage, and a broader distribution
   and architecture matrix remain.

### Phase 10 — Documentation, packaging, and stable-QEMU maintenance

1. Write a quick start and complete Quocker CLI reference, Compose support
   matrix, VM image guide, guest provisioning guide, storage and networking
   guides, security guide, troubleshooting guide, and migration examples.
2. Document every semantic difference from Docker Compose, including commands
   and fields that parse but cannot run.
3. Package Quocker dependencies and completion/man pages for supported
   distributions. Define uninstall behavior and preserve user VM state.
4. Track the latest stable upstream QEMU release/branch and define a regular
   update process: identify stable updates, rebase/cherry-pick Quocker work on
   `quocker`, resolve conflicts, run QEMU and Quocker CI, and publish the base
   QEMU revision. Do not develop on or merge work into QEMU `master`.
5. Maintain a small, reviewable Quocker patch series where feasible; document
   any changes to QEMU core separately from standalone CLI additions.
6. Define versioning, release notes, support window, compatibility guarantees,
   security response, and stable branch backport policy.

## 4. Definition of project completion

Quocker is ready to call finished only when all agreed requirements below have
evidence and the project owner has accepted any explicit exclusions:

1. A pinned current Compose schema is accepted and validated, and every field
   has a documented capability classification.
2. File discovery, environment handling, normalization, merge behavior, and
   core CLI conventions match Docker Compose for the compatibility matrix.
3. A Docker Compose file advertised as drop-in compatible runs with equivalent
   observable behavior, or Quocker clearly identifies each required change.
4. VM images, guest provisioning, dependencies, networks, storage, secrets,
   health, lifecycle, cleanup, and failure recovery have supported semantics.
5. Storage use is inspectable and bounded by the chosen policy; cleanup cannot
   remove data outside the selected Quocker project/resources.
6. CI covers parser compatibility, QEMU builds, VM integration, supported
   hosts, and failure/security cases. The full documented test suite passes.
7. Installation, upgrade, rollback, removal, documentation, and stable-QEMU
   update procedures are reproducible.
8. No Compose setting in the advertised support level is silently ignored,
   and no ordinary QEMU workflow is broken by the optional interface.

## 5. Review topics for this draft

The owner has confirmed that container lifecycle and container runtime
management are out of scope and that OCI image references should be converted
to full VM guests with a supplied kernel and Quocker init. Remaining product
choices include the Quockerfile/build semantics, kernel catalog and trust
policy, guest-init channel, first host/guest platforms, and whether unsupported
runtime fields fail by default. These choices still affect the later phases.
