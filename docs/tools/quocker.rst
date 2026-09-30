Quocker
-------

Quocker is an optional Compose-style command line interface for creating and
managing QEMU virtual machines. It is installed as a separate ``quocker``
program; normal QEMU command line tools remain available.

Quocker runs services from local bootable QEMU disks or from OCI images
converted into VM guests. A service's first start creates a qcow2
copy-on-write overlay backed by its image. The overlay is kept when a project
is stopped and removed only with ``quocker down --volumes`` or ``quocker rm``.

Requirements
------------

Quocker is implemented in C and uses GLib, libyaml, libcurl, json-glib,
libarchive, OpenSSL, and libext2fs.
Enable it at configure time with ``-Dquocker=enabled``; the build requires the
``yaml-0.1``, ``libcurl``, ``json-glib-1.0``, ``libarchive``, ``openssl``, and
``ext2fs`` pkg-config packages. On Arch Linux, install
``libyaml curl json-glib libarchive openssl e2fsprogs`` with pacman. The
``mke2fs`` tool from e2fsprogs is needed for OCI rootfs-to-ext4 conversion
and for creating persistent volume disks.
It also requires ``qemu-system-x86_64`` and ``qemu-img`` at runtime. QEMU
executables can be selected with the ``QUOCKER_QEMU`` and
``QUOCKER_QEMU_IMG`` environment variables.

Compose file
------------

Quocker reads Docker Compose's preferred ``compose.yaml`` and ``compose.yml``
names and the backward-compatible ``docker-compose.yaml`` and
``docker-compose.yml`` names. It also recognizes ``quocker-compose.yaml`` and
``quocker-compose.yml``. It searches the current directory and its parents.
Use ``-f`` more than once to combine files in order, as with Docker Compose.
``--project-directory`` selects the base directory; relative paths in the
configuration are resolved from there (or from the first Compose file).

The YAML loader accepts Compose extension fields, anchors and aliases, and
Compose's ``!reset`` and ``!override`` merge tags. Every merged configuration
is validated against the pinned Compose Specification JSON Schema installed
with Quocker. Schema errors include the invalid field path; ``x-`` extension
fields remain accepted. ``quocker config`` retains schema-valid fields it does
not interpret. Environment
interpolation supports Compose scalar values only: ``$VAR``, ``${VAR}``, nested
default and required-value forms, and ``$$``. Unset values without a default
warn and become empty. ``.env`` and repeated ``--env-file`` inputs support
unquoted/double-quoted interpolation, literal single quotes, comments,
double-quoted escapes, and unset entries; shell variables take precedence and
later env files override earlier ones. ``COMPOSE_ENV_FILES`` selects default
env files, and ``COMPOSE_DISABLE_ENV_FILE`` disables implicit ``.env`` loading.
Quocker also reads ``COMPOSE_FILE``, ``COMPOSE_PATH_SEPARATOR``,
``COMPOSE_PROJECT_NAME`` and ``COMPOSE_PROFILES``. The file format is intended
to be a broad Compose-compatible superset, but the QEMU runtime does not
implement every Docker Compose behavior yet.

Project names follow Compose precedence: ``-p``, ``COMPOSE_PROJECT_NAME``,
top-level ``name``, then the project directory name. Explicit ``-p`` values
must already be lowercase and contain only letters, digits, hyphens, and
underscores, starting with a letter or digit. Environment, file, and directory
names are lowercased and stripped to those characters. The resolved value is
written to ``name`` in ``config`` output and exposed as
``COMPOSE_PROJECT_NAME`` during file interpolation.

``config --services``, ``config --profiles``, ``config --images``,
``config --volumes``, and ``config --networks`` print the corresponding names
one per line in sorted order. Repeated profile names are emitted once.
``config --environment`` prints the merged process and
environment-file values used for interpolation as sorted ``KEY=VALUE`` lines;
process environment values take precedence over explicit env-file values.
These output selection modes cannot be combined with ``--format``; without an
output selection option, ``config`` renders YAML by default or JSON with
``--format json``.

.. code-block:: yaml

   name: dev-stack
   services:
     web:
       image: ./images/alpine.qcow2
       mem_limit: 1G
       cpus: 2
       ports:
         - "8080:80"

For ``up``, ``image`` may name a local QEMU disk or an OCI registry image. Local
VM disks currently boot as x86_64 machines; OCI guests use their selected Linux
platform and a matching guest init. Local paths are resolved relative to the
project directory, and ``x-quocker.image`` can override ``image``. ``quocker pull`` can fetch OCI/Docker v2
images from Docker Hub or another HTTPS registry, select the requested Linux
platform, verify manifest/config/layer digests, and safely materialize layer
tar files into a rootfs directory under the content cache. The extractor
applies OCI whiteouts, rejects path traversal and symlink-parent traversal,
and enforces entry and expanded-size limits. Character/block devices and
FIFOs are stored as ordinary placeholder files with guest type/device metadata;
sockets and unknown types are rejected. OCI
UID/GID/mode metadata and OCI extended attributes are retained in Quocker user
xattrs because extraction does not run as host root. Privileged attributes such
as ``security.capability`` are recorded as inert metadata, never applied to the
host. When present, safely parsed guest ``/etc/os-release`` adds distro ID,
version, and ID_LIKE hints to the pull summary. Pull and OCI-backed ``up``
detect shipped kernel module releases, apply image kernel labels and
``x-quocker.kernel`` overrides, and select a matching entry from the signed
local catalog. Set ``QUOCKER_KERNEL_CATALOG`` and
``QUOCKER_KERNEL_CATALOG_PUBKEY`` to override the catalog and trust-key paths.
``quocker kernel update --url https://HOST/path/kernels.json`` fetches a catalog
and its adjacent ``.sig`` file, verifies the Ed25519 signature against the
configured trusted key, validates every entry, then installs the pair
atomically under ``$XDG_CACHE_HOME/quocker/kernels.json``. A user-cache catalog
takes precedence over ``/etc/quocker/kernels.json``. The trusted key defaults
to ``/etc/quocker/kernel-catalog.pub``; set
``QUOCKER_KERNEL_CATALOG_PUBKEY`` or pass ``--pubkey`` to select another
administrator-provisioned key. OCI ``up`` fetches missing kernel/initrd assets
only from HTTPS URLs in that signed catalog, verifies their declared sizes and
SHA-256 digests, and caches them by digest before starting QEMU. It never
downloads from image-provided URLs. Run ``quocker kernel fetch --platform linux/amd64 --id PROFILE``
to prefetch a bundle explicitly. Set
``QUOCKER_REGISTRY_MIRROR`` to an HTTPS registry mirror base URL.
For per-registry routing, create ``$XDG_CONFIG_HOME/quocker/registries.json``
(normally ``~/.config/quocker/registries.json``) with a ``mirrors`` object
keyed by registry hostname. Docker Hub aliases normalize to
``registry-1.docker.io``; mirror URLs must use HTTPS and cannot contain
userinfo, a query, or a fragment. For example::

   {"mirrors": {
     "docker.io": "https://registry-cache.example/dockerhub",
     "ghcr.io": "https://registry-cache.example/ghcr"
   }}

Set ``QUOCKER_REGISTRY_CONFIG`` to use a different configuration file. The
single ``QUOCKER_REGISTRY_MIRROR`` value takes precedence over per-registry
entries for backward compatibility. Registry credentials are not forwarded to
an unrelated mirror host; authenticated mirror-specific credentials are not
implemented yet.
Registry credentials are read from inline Docker CLI ``config.json`` auths
entries and used only at a registry's HTTPS Bearer-token realm when its host
matches the registry (or Docker Hub's ``auth.docker.io``). Credential-helper
entries from ``credHelpers`` and ``credsStore`` are also supported when the
configured ``docker-credential-HELPER`` executable is in ``PATH``. Helper
execution is direct (without a shell), with bounded output and a timeout.
Registry requests retry transient HTTP 408, 425, 429, and selected 5xx responses,
as well as transient connection failures, up to three times. A server-provided
``Retry-After`` value is honored up to 30 seconds; otherwise Quocker uses
bounded exponential backoff. Persistent rate limits and server errors are
reported with the final HTTP status.
``quocker pull`` now
also converts supported staged trees into a raw ext4 guest disk beside the
content-addressed rootfs and replays uid, gid, mode, and OCI xattrs. Staged
device and FIFO placeholders become special inodes in the guest filesystem;
no host device nodes are created. The estimated virtual disk size is checked
against the OCI cache limit before conversion. A static C guest-init binary and
an initrd appender are available; the appender verifies the base initrd digest
and checks that the guest-init ELF architecture matches the OCI image.
OCI-backed ``up`` turns a registry image into a guest VM: it converts the
rootfs, builds a per-service initrd with the image launch defaults, and starts
QEMU with the selected kernel, initrd, and writable disk overlay. OCI
``Entrypoint``/``Cmd`` and supported Compose command, entrypoint, environment,
``env_file``, user, and working-directory overrides are applied in the guest.
Service ``env_file`` accepts a path or ordered path sequence; its long form
supports ``required`` and ``format: raw``. Relative paths currently resolve
from the project directory. Env-file values are overridden by the service's
``environment`` field, which in turn overrides image defaults; an explicitly
unresolved environment entry removes that value from the guest. These settings
apply to OCI-backed services; local-disk services do not interpret application
environment fields. OCI-backed services support Compose named and anonymous
volume mounts as separate persistent ext4 disks attached through virtio. Named
disks are scoped to the Compose project; anonymous disk names are derived from
the service and guest target. OCI image-declared volume targets receive
anonymous disks unless the service explicitly mounts another volume at that
target. ``ro`` mounts are mounted read-only inside the guest. Disks default to
a sparse 1 GiB virtual size; ``QUOCKER_VOLUME_SIZE`` sets the size for newly
created disks (for example, ``512MiB`` or ``2GiB``, from 64 MiB to 1 TiB).
Existing disks retain their original size. New project volume disks count
toward a 20 GiB project quota by default; ``QUOCKER_VOLUME_QUOTA`` changes
that limit and accepts byte, KiB, MiB, GiB, or TiB sizes. Set it to ``0`` for
no limit. The quota counts virtual disk capacity, while ``volume ls`` and
``volume df`` also report filesystem allocated bytes so sparse disk growth is
visible. The quota is checked before each new disk is created.
Volume creation and removal are serialized with a project lock, including the
quota check, so concurrent Quocker processes cannot create volumes past the
configured limit.
Quocker starts QEMU with file locking enabled for managed volume disks.
``volume rm`` and ``down --volumes`` take QEMU-compatible exclusive locks
before deleting disk files and refuse removal while a locking-aware QEMU
process has them open. This protection cannot cover external QEMU invocations
that explicitly disable file locking.
Write-side volume operations repair interrupted metadata updates and remove
orphan sidecars and abandoned staging files after a crash. Recovery runs under
the project volume lock; read-only listing does not alter the volume directory.
``down`` preserves these disks; ``down --volumes`` removes project volume disks
after all project VMs have stopped. Bind mounts, tmpfs, external volumes,
non-local drivers, driver options, and long-form volume suboptions are
currently rejected for OCI guests. Local QEMU disk services do not interpret
Compose volume mounts. New disks start empty: Quocker does not implement
Compose's initial copy-up behavior from image contents into a new volume yet.
OCI pulls default to the host architecture when it maps to a supported Linux
platform. Cross-architecture guests require matching guest-init and kernel
catalog assets.
``build`` contexts are not supported. Quocker will not
start or manage containers. ``mem_limit`` (or ``memory``)
sets guest RAM; ``cpus`` sets virtual CPUs. Memory values accept units such as
``1G``; bare numbers follow Compose's byte-based memory syntax.
``ports`` accepts short and long host-to-guest TCP or UDP mappings, including
numeric equal-sized ranges, IPv4 host addresses, and long-form
``mode: host``. Host IPs must be numeric addresses. QEMU user-mode networking
implements the forwarding; long-form ``mode: ingress`` is unsupported. IPv6
host bindings are rejected because the QEMU user network forwarding interface
does not support them. ``up --dry-run`` prints the generated QEMU forwarding
rules. A missing published port (for example, ``"80"`` or long-form
``target: 80``) asks QEMU to allocate an ephemeral host port.
``quocker port SERVICE PORT[/tcp|udp]`` prints static bindings even when the
VM is stopped; for ephemeral bindings it queries QEMU over the private QMP
socket and therefore requires a running VM. Guests receive QEMU user-mode
networking. OCI guests apply
``command``, ``entrypoint``, ``environment``, ``user``, and ``working_dir`` in
the guest. Local-disk services do not yet implement those application
overrides. Volumes and Compose network declarations are not yet translated to
VM resources and are rejected for OCI startup.

Kernel catalog selection
-------------------------

``quocker kernel select`` evaluates a local JSON catalog and prints the
platform-compatible kernel and initrd assets plus the reason for the choice::

   quocker kernel select --platform linux/amd64 --rootfs .cache/rootfs/image
   quocker kernel select --platform linux/amd64 --id linux-amd64-generic
   quocker kernel select --platform linux/amd64 --minimum 6.6 --require virtio_blk,virtio_net
   quocker kernel select --platform linux/amd64 --module-release 6.8.1-quocker --json
   quocker kernel select --platform linux/amd64 --rootfs ./rootfs --json
   quocker kernel update --url https://downloads.example/kernels.json
   quocker kernel fetch --platform linux/amd64 --id linux-amd64-generic

Selection uses a verified user-cache catalog when present, otherwise
``/etc/quocker/kernels.json``; set ``QUOCKER_KERNEL_CATALOG`` or use
``--catalog`` to choose another. The update command installs under
``$XDG_CACHE_HOME/quocker/kernels.json`` by default. Its Ed25519 public key defaults
to ``/etc/quocker/kernel-catalog.pub`` and can be overridden with
``QUOCKER_KERNEL_CATALOG_PUBKEY`` or ``--pubkey``. The detached base64 signature
is read from the catalog filename with ``.sig`` appended. Catalog version 1 has a
``kernels`` array. Each entry requires ``id``, ``os``, ``architecture``,
``kernel_sha256`` and ``initrd_sha256`` digests, plus either absolute local
``kernel``/``initrd`` paths or HTTPS ``kernel_url``/``initrd_url`` values with
declared byte sizes. Optional ``variant``, ``distro_ids``, and integer
``priority`` fields refine matching. Optional numeric dotted ``version`` and
string-array ``features`` and ``module_releases`` fields let the C selection
API enforce image minimum versions and required feature tokens, and compare
rootfs-shipped module releases as advisory evidence. For remote bundles, an
entry may use ``kernel_url``/``initrd_url`` with positive ``kernel_size``/
``initrd_size`` byte counts instead of local ``kernel``/``initrd`` paths;
URLs must use HTTPS and sizes are capped at 1 GiB for kernels and 512 MiB for
initrds. The cache uses content-addressed filenames, but kernel-cache quota and
garbage collection are not implemented yet. An OCI image can declare
hard module ABI requirements with ``io.quocker.kernel.module_releases``;
Compose uses ``x-quocker.kernel.module_releases``. One catalog release must
match one declared acceptable release. ``--module-release`` adds the same
requirement to a selection query. ``--json`` emits a machine-readable report
with constraints, the selection reason, observed and matched module releases,
declared asset digests, and an asset verification flag. An exact distro ID wins over an ``ID_LIKE`` family
match, which wins over a generic platform entry; priority breaks ties. ``--id``
pins an entry but cannot bypass OS, architecture, or variant matching.
``--minimum`` and repeatable/comma-separated ``--require`` filter by a catalog
entry's numeric dotted version and feature set. Asset
digests and the catalog signature are checked on each selection. The catalog
and public key must be owned by root or the invoking
user and must not be group/world writable. Signatures cover the exact catalog
file bytes.

OCI-backed ``up`` uses this selector and verifies the selected kernel/initrd
digests before launching QEMU. OCI images do not specify a required kernel;
distro metadata is a preference hint. Trust-key provisioning, maintained
kernel/initrd packaging, production boot testing, and catalog update scheduling
remain unfinished. ``quocker kernel update`` fetches a catalog and detached
signature over HTTPS and installs it only after verification against the
configured administrator public key. Signed catalog entries can point to
HTTPS kernel/initrd bundles with pinned sizes and SHA-256 digests; ``up``
fetches missing assets into the user cache and verifies them before boot.
``quocker kernel fetch`` can prefetch a selected entry explicitly.

OCI image compatibility direction
----------------------------------

OCI image declarations identify guest userlands. Quocker supplies a guest
kernel from a signed catalog and boots the filesystem as a VM. There is
no reliable way to infer one required kernel from a normal Docker image, so
Quocker's selector uses the OCI platform as a hard constraint,
Quocker-specific kernel labels or ``x-quocker.kernel`` as explicit hints,
and rootfs evidence such as distro metadata and shipped kernel modules. Distro
metadata ranks profiles; module directories are evidence but are not proof
that an application needs that exact kernel ABI. The selector reports
detected module releases and prefers catalog matches when other compatibility
scores are equal, but an unmatched release is advisory and does not block a
platform-compatible kernel. It falls back to a maintained generic kernel only
if the platform and declared requirements match. Image metadata cannot provide
arbitrary kernel URLs or trust keys.
``pull`` validates image labels ``io.quocker.kernel.id``,
``io.quocker.kernel.minimum``, ``io.quocker.kernel.features``, and
``io.quocker.kernel.module_releases`` and passes them, along with
``x-quocker.kernel`` overrides, to the C catalog selector when a local signed
catalog is available. The selector filters by explicit minimum-version,
feature, and module-release requirements; detected rootfs module releases are
advisory tie-break evidence. Without a catalog, pull reports that selection
was deferred. Remote bundle fetching is implemented, and OCI boot has not yet
passed a full guest smoke test. The
fuller compatibility and trust design
is in ``QUOCKER-OCI-VM-DESIGN.md``.

Commands
--------

Commands may be called directly (``quocker up``) or with Docker Compose's
subcommand shape (``quocker compose up``).

``quocker config`` parses and prints the resolved project configuration. It
accepts ``--format yaml`` (the default) and ``--format json``. JSON output
retains quoted scalars as strings and emits plain YAML booleans, numbers, and
nulls as JSON primitives. It validates the merged file against the pinned
Compose schema before rendering. Schema validation checks file structure and
syntax; it does not mean every accepted field has a VM runtime implementation.
``quocker up -d`` starts all services. ``quocker ps`` shows their state and
disk overlays. ``quocker logs [SERVICE]`` shows serial output, and
``quocker logs -f`` follows it. ``quocker down`` stops services while retaining
their disks. ``quocker down --volumes`` stops services and removes their
overlays and project volume disks. ``quocker volume ls`` lists persistent VM
volumes for the current Compose project with their logical names and virtual
and allocated sizes. ``quocker volume df`` summarizes project virtual
capacity, allocated disk space, and configured quota. ``quocker volume inspect
NAME`` shows one volume's backing disk and size. ``quocker volume rm
[--dry-run] NAME`` removes a volume from the current
project; it refuses while a project service has a live or unverified PID.
``quocker pull`` downloads OCI manifests/config/layer blobs and
materializes rootfs data and a raw ext4 guest disk in
``$XDG_CACHE_HOME/quocker/oci`` (or the platform cache directory). OCI-backed
``up`` can boot this guest disk when a compatible signed kernel catalog and
its kernel/initrd assets are installed.
The VM lifecycle commands are ``start``, ``stop``, ``restart``, ``kill``,
``pause``, and ``unpause``.
``start`` starts a previously created VM from its saved state and disk;
``up`` is required to create the VM the first time. ``stop`` requests graceful
QEMU termination, waits up to ten seconds, then force-kills QEMU if necessary,
while preserving the VM state and disk. ``restart`` stops selected services
in reverse dependency order and starts them in dependency order. ``kill`` sends
``SIGKILL`` by default; ``-s``/``--signal`` accepts a signal name or number.
State-changing lifecycle commands and volume removal serialize on a
per-project lock, so separate local Quocker processes cannot update the same
saved VM state simultaneously.
``pause`` and ``unpause`` use a private QMP socket to stop and resume guest
execution without suspending the host QEMU process.
These commands accept service names and ``--dry-run``. They manage QEMU
processes and VM resources; they do not operate a container runtime. On Linux,
new state records the process start time as well as the PID and QEMU VM name,
so a stale or recycled PID observed during identity checks is not treated as
that VM's QEMU process. Signals use Linux pidfds when available, binding the
signal to the verified process even if its PID is later reused. On kernels
without pidfd support, Quocker rechecks process identity immediately before
signaling.
VM state records are written through a private temporary file, flushed, and
atomically renamed before Quocker reports a successful start. State mutations
remove abandoned private state-write temporary files while holding the
project lifecycle lock. State reads are bounded and reject symlinks, non-regular
files, and records owned by another user.
Use
``QUOCKER_REGISTRY_MIRROR=https://mirror.example`` to route all registry
requests through one HTTPS mirror, or use ``registries.json`` to route each
registry to its own HTTPS mirror. The 20 GiB cache limit covers compressed blobs,
materialized rootfs files, and their ext4 guest disks; set
``QUOCKER_OCI_CACHE_LIMIT`` to a positive byte count to change it. Pulls fail
before exceeding the limit. ``quocker prune`` removes downloaded OCI blobs
and staged rootfs trees that are not referenced by VM overlays. For referenced
images, it keeps the manifest, config/layer blobs, rootfs tree, and ext4 base
disk so the VM can be started again. Stopped VM overlays keep their image
content until ``quocker down --volumes`` removes the overlays. Cache
references are reconciled during pruning; references whose overlay or base
disk has been removed are discarded. Unreferenced content can be downloaded
or rebuilt later. ``quocker -f path/to/file.yaml`` selects
another Compose file;
``quocker -f base.yaml -f dev.yaml config`` merges files in order. ``-p`` or
``COMPOSE_PROJECT_NAME`` overrides the project name, and ``--profile`` enables
a service profile. On Linux, ``COMPOSE_FILE`` accepts a colon-separated file
list; ``COMPOSE_PATH_SEPARATOR`` can select another separator. The YAML reader
rejects duplicate mapping keys and files containing multiple YAML documents.
When merging service definitions, ``command``, ``entrypoint``, and
``healthcheck.test`` use the later value. Ports merge by IP, target, published
port, and protocol; volumes, secrets, and configs merge by target, including
when the same resource uses short syntax in one file and long syntax in
another. Other sequences append, with duplicate entries removed for the
supported capability, device-cgroup-rule, expose, external-link, security,
placement-constraint, placement-preference, and generic-resource lists.
``!reset`` clears an overridden value and ``!override`` replaces it.
YAML aliases and ``<<`` merge-key anchors are expanded within one file;
explicit service fields override values inherited from an anchor. Compose
``include`` supports recursive short syntax and long-form ``path`` strings or
lists. Include paths are resolved from the file that declares them. Conflicting
resource names produce a warning and keep the current project's resource.
Include ``env_file`` and ``project_directory`` are supported; the including
project's environment overrides values from the included project's optional
``.env`` or explicit environment files. Relative local disk image, service
``env_file``, and top-level config/secret file paths use the included project
directory. Service ``extends`` supports a service name for same-file
inheritance or a mapping with ``service`` and optional ``file`` for external
inheritance. External files are resolved from the project directory, while
local image and ``env_file`` paths on the external base service are resolved
from that file's directory. Inheritance recursively merges mappings, appends
ordinary sequences, deduplicates supported unique sequences, and merges
Compose keyed resource sequences by their keys. External base services are
used as templates and are not added to the project's service list. Missing
services, invalid declarations, cycles, and excessive inheritance depth are
reported as errors. Other path-valued attributes in included services are not
implemented yet.

Runtime state, logs, PID files, and disk overlays live under
``.quocker/PROJECT`` under the project directory.

``up`` processes ``depends_on`` dependencies before their dependent services
and automatically includes required dependencies when a service is selected.
Short syntax and long syntax with ``condition: service_started`` are
supported. This means the dependency's QEMU process has started; it does not
claim that an application inside the guest is ready. ``service_healthy`` and
``service_completed_successfully`` are rejected until Quocker has guest
readiness and completion reporting. Dependency cycles and missing required
services fail before any VM is started. Long-form ``required: false`` skips a
missing optional dependency. ``up --dry-run`` prints the dependency-ordered
start plan without creating project state or starting VMs. ``down --dry-run``
shows the corresponding reverse order, stopping dependents before dependencies.
An explicitly named service starts even when its profile was not globally
enabled; Quocker starts only that service and its declared dependencies.
Dependencies in an inactive profile fail unless that profile is enabled by
``--profile``, ``COMPOSE_PROFILES``, or a profile shared with the targeted
service. Other services that share the target's profile are not started just
because one service was named.
