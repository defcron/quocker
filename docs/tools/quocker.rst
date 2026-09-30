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
When Quocker is enabled, installation adds Bash completion at
``share/bash-completion/completions/quocker``. If the QEMU documentation build
is enabled, it also generates and installs ``quocker(1)``.

Docker Compose command compatibility
------------------------------------

The table records the command surface from the `Docker Compose CLI reference
<https://docs.docker.com/reference/cli/docker/compose/>`_ as checked on
2026-09-30. “VM-adapted” means the command has a Quocker equivalent with VM
semantics; it does not imply Docker-compatible behavior or complete flag
parity. Commands marked “not implemented” are rejected instead of silently
treated as successful.

.. list-table:: Docker Compose commands and Quocker status
   :header-rows: 1
   :widths: 18 20 62

   * - Docker Compose command
     - Quocker status
     - Current Quocker behavior
   * - ``up``
     - VM-adapted, partial
     - Starts selected VMs, resolves supported dependencies, and can wait for
       guest workload readiness.
   * - ``down``
     - VM-adapted, partial
     - Stops VMs and removes saved VM state; named VM volumes require the
       explicit volume-removal option.
   * - ``ps``
     - VM-adapted, partial
     - Lists configured services with saved VM process state.
   * - ``logs``
     - VM-adapted, partial
     - Reads captured serial output; container log drivers and full formatting
       options do not apply. ``--tail`` and ``-n`` accept a non-negative line
       count or ``all``; ``--no-log-prefix`` omits the service prefix;
       ``-f``/``--follow`` streams subsequent output with consistent formatting.
   * - ``config``
     - VM-adapted, partial
     - Validates, resolves, merges, renders, and lists selected Compose model
       information; ``-o``/``--output`` writes YAML or JSON atomically. Full
       canonical normalization and option parity remain.
   * - ``start``, ``stop``, ``restart``, ``kill``
     - VM-adapted, partial
     - Operate on saved QEMU process state; restart follows supported
       dependency ordering.
   * - ``pause``, ``unpause``
     - VM-adapted, partial
     - Use QMP VM stop and continue commands.
   * - ``pull``
     - VM-adapted, partial
     - Pulls OCI userland images and prepares VM root disks and kernel assets.
   * - ``port``
     - VM-adapted, partial
     - Reports configured or QMP-discovered host forwarding endpoints.
   * - ``wait``
     - VM-adapted, partial
     - Waits for selected QEMU VMs to stop; it does not return OCI workload
       exit codes as container status.
   * - ``rm``
     - VM-adapted, partial
     - Removes stopped VM state and service overlays. Running VMs require
       ``--stop``/``-s``. ``--volumes``/``-v`` removes anonymous service
       volumes and preserves named project volumes.
   * - ``version``
     - Implemented
     - Reports the Quocker interface and its QEMU base version.
   * - ``volumes``
     - VM-adapted, partial
     - Lists project-scoped persistent VM volumes with disk sizes, matching
       ``quocker volume ls``. Bind and tmpfs mounts are not included.
   * - ``build``, ``commit``, ``push``, ``publish``
     - Not implemented
     - Image build and publication need an explicitly defined VM artifact
       workflow; Quocker does not run Docker builds.
   * - ``create``, ``run``
     - Not implemented
     - There is no create-without-start or one-off VM command yet.
   * - ``attach``, ``exec``, ``cp``
     - Not implemented
     - Guest agent/console transport and file-copy semantics are not available.
   * - ``images``
     - VM-adapted, partial
     - Lists selected services' configured image references and whether saved
       VM state is running, stopped, or not yet created. It does not list
       unreferenced cache entries or resolved registry digests.
   * - ``events``, ``ls``, ``stats``, ``top``
     - Not implemented
     - QEMU event streaming, global project discovery, guest resource
       statistics, and guest process listing are not implemented.
   * - ``scale``, ``watch``
     - Not implemented
     - VM instance identity and safe recreation/watch behavior are not defined.
   * - ``alpha``, ``bridge``, ``convert``
     - Not implemented
     - Docker Compose experimental commands and conversion tools have no
       Quocker equivalent yet.

This command inventory does not describe option parity. The flag matrix below
records the implemented Quocker option surface against the official Compose
references checked on 2026-09-30. A flag is listed as supported only where it
has an implemented VM behavior; accepted no-op flags are not compatibility
claims. Docker-specific flags without a VM equivalent remain unsupported.

.. list-table:: Quocker options and Compose flag gaps
   :header-rows: 1
   :widths: 16 42 42

   * - Command
     - Quocker options with behavior
     - Not implemented from Docker Compose
   * - Global
     - ``-h``, ``--help``, repeatable ``-f``/``--file``, ``-p``/
       ``--project-name``, ``--project-directory``, repeatable
       ``--env-file``, repeatable ``--profile``. ``--dry-run`` applies to
       supported lifecycle commands.
     - ``--ansi``, ``--compatibility``, ``--parallel``, ``--progress``.
       Docker's global ``--all-resources`` has no VM equivalent.
   * - ``up``
     - ``-d``/``--detach``, ``--wait``, ``--wait-timeout SECONDS``,
       ``--dry-run``, ``--remove-orphans``, and service names.
     - Build, pull-policy, recreate, attach selection,
       scaling, timeout, menu, watch, and interactive confirmation options.
       See the `Compose up reference <https://docs.docker.com/reference/cli/docker/compose/up/>`_.
   * - ``down``
     - ``--volumes``/``-v``, ``--dry-run``, ``-t``/``--timeout SECONDS``
       (default 10), ``--remove-orphans``, and service names.
     - ``--rmi``. See the
       `Compose down reference <https://docs.docker.com/reference/cli/docker/compose/down/>`_.
   * - ``ps``
     - Service names; by default lists running saved VMs, including saved VMs
       whose service is no longer declared (orphans). ``-a``/``--all`` includes
       stopped VMs with saved state. ``--orphans=false`` suppresses orphans.
       Output includes VM state, PID,
       and disk path; ``-q``/``--quiet`` prints generated VM names, one per
       line; ``--services`` prints matching service names only and cannot be
       combined with quiet output. ``--format table`` selects the default
       table; ``--format json`` prints JSON Lines with VM name, project,
       service, running/exited state, PID, and disk path. This VM-specific
       object schema does not include Docker container command, health, exit
       code, or publisher fields. ``--status running|exited`` and
       ``--filter status=running`` /
       ``status=exited`` select VM process states. Other Docker statuses
       (including ``paused``, ``created``, and ``dead``) are rejected because
       Quocker tracks QEMU process state rather than container lifecycle state.
     - Go-template formatting and ``--no-trunc``.
       Project selection uses Quocker's
       shared ``-p``/``--project-name`` option. See the
       `Compose ps reference <https://docs.docker.com/reference/cli/docker/compose/ps/>`_.
   * - ``logs``
     - Service names, ``-f``/``--follow``, ``-n``/``--tail N`` (including
       ``all``), and ``--no-log-prefix``.
     - ``--index``, ``--no-color``, ``--since``, ``--timestamps``, and
       ``--until``. See the
       `Compose logs reference <https://docs.docker.com/reference/cli/docker/compose/logs/>`_.
   * - ``config``
     - ``--format yaml|json``, ``-o``/``--output FILE``, ``--quiet``,
       ``--environment``, ``--services``, ``--profiles``, ``--images``,
       ``--volumes``, ``--networks``, and Quocker-specific ``--capabilities``.
     - ``--hash``, ``--lock-image-digests``, ``--models``, ``--variables``,
       ``--no-consistency``, ``--no-env-resolution``, ``--no-interpolate``,
       ``--no-normalize``, ``--no-path-resolution``, and image-digest
       resolution. See the
       `Compose config reference <https://docs.docker.com/reference/cli/docker/compose/config/>`_.
   * - ``start``, ``stop``, ``restart``, ``pause``, ``unpause``
     - Service names; ``--dry-run`` for lifecycle planning. ``stop`` and
       ``restart`` accept ``-t``/``--timeout SECONDS`` (default 10).
     - Attach and interactive controls. These commands manage existing QEMU
       processes and do not create containers.
   * - ``kill``
     - Service names, ``-s``/``--signal SIGNAL``, and ``--dry-run``.
     - Container-specific signal behavior and attach/output controls.
   * - ``rm``
     - Service names, ``-s``/``--stop``, and ``-t``/``--timeout SECONDS``
       (only with ``--stop``). ``-f``/``--force`` is accepted as a compatibility
       no-op; ``-v``/``--volumes`` removes anonymous service volumes only.
       Running VMs are refused unless ``--stop`` is supplied.
     - Container-specific anonymous-volume behavior beyond Quocker's
       service-scoped VM volume names.
   * - ``pull``
     - Service names; downloads OCI layers and prepares VM guest disks.
     - ``--ignore-buildable``, ``--ignore-pull-failures``, ``--include-deps``,
       ``--policy``, and ``-q``/``--quiet``. See the
       `Compose pull reference <https://docs.docker.com/reference/cli/docker/compose/pull/>`_.
   * - ``wait``
     - Service names; waits for saved QEMU processes to stop.
     - Container-specific selection options.
   * - ``port``
     - ``SERVICE PRIVATE_PORT[/PROTOCOL]``.
     - Container index selection and container port semantics.
   * - ``images``, ``volumes``
     - ``images`` accepts service names; ``volumes`` takes no arguments.
       Both display VM-oriented inventory.
     - Docker image IDs, formatting, quiet output, and container volume
       attachment details.
   * - ``volume``
     - ``ls``, ``df``, ``inspect NAME``, ``rm NAME``; ``rm`` accepts
       ``--dry-run``. These are Quocker-specific VM disk management commands.
     - Docker volume-driver, label, and container-mount options.
   * - ``prune``, ``version``
     - ``prune`` removes unreferenced Quocker OCI cache content; ``version``
       accepts ``--short``.
     - These commands have no direct Compose counterparts. Docker system-wide
       cache controls, JSON/template formats, and engine version information
       are not provided by Quocker.

Commands shown as not implemented above (such as ``build``, ``run``,
``exec``, and ``cp``) have no Quocker flag compatibility surface yet. The
official references are the
`Compose command index <https://docs.docker.com/reference/cli/docker/compose/>`_
and the command pages linked above. This inventory is the current baseline;
it does not promise parity for flags listed as missing.

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
fields remain accepted. Validation errors also identify the source YAML file,
line, and column, including errors in merged or included Compose files.
``quocker config`` retains schema-valid fields it does not interpret.
Environment interpolation supports Compose scalar values only: ``$VAR``,
``${VAR}``, nested default and required-value forms, and ``$$``. Unset values
without a default warn and become empty. ``.env`` and repeated ``--env-file``
inputs support unquoted/double-quoted interpolation, literal single quotes,
comments, double-quoted escapes, and unset entries; shell variables take
precedence and later env files override earlier ones. ``COMPOSE_ENV_FILES``
selects default env files, and ``COMPOSE_DISABLE_ENV_FILE`` disables implicit
``.env`` loading.
``quocker config --capabilities`` prints a tab-separated report of service
fields present in the merged configuration. It distinguishes QEMU settings,
partially supported Compose fields, OCI guest workload settings, unsupported
settings, and fields that are preserved without runtime behavior. OCI workload
settings such as ``command`` and ``volumes`` do not apply to local disk
services; partial support details are listed below and in each resource's
runtime diagnostics. ``up`` rejects non-empty unsupported service settings
with the field name in the error, after preflighting every selected service
and before starting any VM. Nested service values are listed by their YAML
path and inherit the containing service field's status. A second
``RESOURCE`` table reports declared top-level
``volumes``, ``networks``, ``configs``, and ``secrets`` with their current VM
runtime status, including unsupported guest config and secret provisioning.
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

``quocker version`` prints the Quocker interface version and the QEMU base
version used to build it. ``quocker version --short`` prints only the Quocker
version; ``quocker --version`` remains an alias for the full output.

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
and enforces entry and expanded-size limits. It rejects path, symlink-parent,
and hard-link target traversal, supports gzip-compressed layers, and rejects a
truncated gzip archive without leaving a partial rootfs.
Character/block devices and FIFOs are stored as ordinary placeholder files
with guest type/device metadata; sockets and unknown types are rejected. A
``.provenance`` sidecar records the selected manifest digest and SHA-256 digest
of each layer used to materialize the tree; cache reuse requires that metadata
to match the current layer inputs. OCI
UID/GID/mode metadata and OCI extended attributes are retained in Quocker user
xattrs because extraction does not run as host root. Privileged attributes such
as ``security.capability`` are recorded as inert metadata, never applied to the
host. When present, safely parsed guest ``/etc/os-release`` adds distro ID,
version, and ID_LIKE hints to the pull summary. Pull and OCI-backed ``up``
detect shipped kernel module releases, apply image kernel labels and
``x-quocker.kernel`` overrides, and select a matching entry from the signed
local catalog. Set ``QUOCKER_KERNEL_CATALOG`` and
``QUOCKER_KERNEL_CATALOG_PUBKEY`` to override the catalog and trust-key paths.
The service-level ``x-quocker`` extension is version 1; ``version: 1`` is
recommended, while omitted ``version`` remains a version-1 shorthand for
existing files. It accepts only ``image`` and ``kernel`` alongside ``version``.
The kernel mapping accepts ``id``, dotted-numeric ``minimum``, ``require``
feature names, and ``module_releases``. Extension structure and value types are
validated during ``config`` as well as before runtime, and unknown fields or
unsupported extension versions are errors. Kernel overrides apply to OCI guest
images; Quocker rejects them for local bootable disks, whose kernel is part of
the disk image.
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
``down`` preserves these disks; ``down --volumes`` removes named volumes used
by selected services and their anonymous service volumes, while preserving
unreferenced project disks. It refuses volume deletion if another declared VM
is still running. Bind mounts, tmpfs, external volumes,
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
Before launching selected VMs, ``up`` also probes static IPv4 TCP/UDP host
bindings and fails with an unavailable-port diagnostic when another process
already holds the requested address and port. The probe releases its socket
before QEMU starts, so it cannot prevent a separate process from claiming the
port in the meantime; QEMU performs the final bind.
``quocker port SERVICE PORT[/tcp|udp]`` prints static bindings even when the
VM is stopped; for ephemeral bindings it queries QEMU over the private QMP
socket and therefore requires a running VM. Guests receive QEMU user-mode
networking by default. ``network_mode: none`` starts QEMU with no network
device and omits OCI's DHCP kernel argument; it cannot be combined with
published ports or service network attachments. Other ``network_mode`` values
and Compose ``networks`` declarations are rejected until a shared network
backend is available. OCI guests apply
``command``, ``entrypoint``, ``environment``, ``user``, and ``working_dir`` in
the guest. Local-disk services do not yet implement those application
overrides. Volumes and Compose network declarations are not yet translated to
VM resources and are rejected for VM startup.

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
Compose schema before rendering. Use ``-o FILE`` or ``--output FILE`` to write
the rendered document to a file instead of standard output. Quocker writes a
private temporary file in the destination directory and renames it into place
after serialization succeeds. Schema validation checks file structure and
syntax; it does not mean every accepted field has a VM runtime implementation.
``quocker up -d`` starts all services. ``quocker ps`` lists running VMs;
``quocker ps --all`` includes stopped VMs that still have saved state.
``quocker ps --status exited`` or ``quocker ps --filter status=exited`` lists
stopped saved VMs without requiring ``--all``. The ``running`` and ``exited``
filters describe whether the recorded QEMU process is alive; other Docker
container statuses do not map to Quocker's saved VM state and are rejected.
``quocker ps --services`` prints the Compose service names for matching saved
VMs, respecting the running default, ``--all``, status filters, and orphan
selection. By default, saved VMs whose service is absent from the Compose file
are included; use ``--orphans=false`` to suppress them.
``quocker ps --format json`` writes one JSON object per matching VM, with
``ID``, ``Name``, ``Project``, ``Service``, ``State``, ``PID``, and ``Disk``
fields. ``ID`` and ``Name`` use Quocker's generated VM name. Docker container
command, health, exit-code, and publisher fields have no equivalent in this
output schema.
``quocker logs [SERVICE]`` shows serial output;
``quocker logs --tail 20`` shows only its last twenty non-empty lines.
``quocker logs --no-log-prefix`` omits the service name prefix, and
``quocker logs -f``/``--follow`` follows subsequent output using the same
prefix setting.
``quocker down`` stops services while retaining
their disks. ``quocker down --volumes`` stops services and removes their
overlays and project volume disks. ``quocker volume ls`` lists persistent VM
volumes for the current Compose project with their logical names and virtual
and allocated sizes. ``quocker volume df`` summarizes project virtual
capacity, allocated disk space, and configured quota. ``quocker volume inspect
NAME`` shows one volume's backing disk and size. ``quocker volume rm
[--dry-run] NAME`` removes a volume from the current
project; it refuses while a project service has a live or unverified PID.
``quocker volumes`` is a Compose-shaped alias for ``quocker volume ls``; it
lists persistent project VM disks and does not list bind or tmpfs mounts.
``quocker images [SERVICE...]`` lists each selected service's configured image
reference and whether saved VM state is running, stopped, or not yet created.
This command reads project state without creating a state directory; it does
not inventory unreferenced cache entries or resolve registry references to
digests.
``quocker pull`` downloads OCI manifests/config/layer blobs and
materializes rootfs data and a raw ext4 guest disk in
``$XDG_CACHE_HOME/quocker/oci`` (or the platform cache directory). OCI-backed
``up`` can boot this guest disk when a compatible signed kernel catalog and
its kernel/initrd assets are installed.
The VM lifecycle commands are ``start``, ``stop``, ``restart``, ``kill``,
``pause``, and ``unpause``. ``quocker wait [SERVICE...]`` blocks until the
selected saved QEMU processes stop and checks their recorded process identity.
It requires saved state for each selected service. When an OCI guest reports
workload completion, ``wait`` prints ``SERVICE: exit code N``. Signal
termination is reported as ``128 + signal``. Local bootable disks and guests
that stop before reporting completion have no workload exit code to print.
``start`` starts a previously created VM from its saved state and disk;
``up`` is required to create the VM the first time. ``stop``, ``restart``, and
``down`` request graceful QEMU termination, wait ten seconds by default, then
force-kill QEMU if necessary. Set ``-t``/``--timeout SECONDS`` to change the
grace period; zero skips directly to SIGKILL. ``stop`` preserves VM state and
disks. ``restart`` stops selected services in reverse dependency order and
starts them in dependency order. ``kill`` sends
``SIGKILL`` by default; ``-s``/``--signal`` accepts a signal name or number.
State-changing lifecycle commands and volume removal serialize on a
per-project lock, so separate local Quocker processes cannot update the same
saved VM state simultaneously.
``pause`` and ``unpause`` use a private QMP socket to stop and resume guest
execution without suspending the host QEMU process.
These lifecycle commands accept service names and ``--dry-run``. They manage
QEMU processes and VM resources; they do not operate a container runtime. On Linux,
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
``up --wait`` returns after selected services are ready. For OCI-backed VMs,
readiness means the bundled guest init successfully executed the configured
workload; it reports this over the serial channel only after the workload's
``exec`` succeeds. ``--wait-timeout SECONDS`` sets a maximum wait, with zero
meaning no timeout. The option implies detached startup. For local bootable
disk images Quocker can verify only that the QEMU process is running; it cannot
assert that an application inside that VM is ready. Compose health checks
remain unsupported.
Short syntax and long syntax with ``condition: service_started`` are
supported. This means the dependency's QEMU process has started; it does not
claim that an application inside the guest is ready. Long syntax with
``condition: service_completed_successfully`` waits for the dependency VM to
stop and starts its dependents only when the guest reports exit status zero.
For OCI-backed services, the bundled guest init writes this completion record;
local disk images need to provide an equivalent serial record to use this
condition. A service that stops without a completion record fails.
``service_healthy`` remains unsupported until guest health reporting is
available. Dependency cycles and missing required services fail before any VM
is started. Long-form ``required: false`` skips a missing optional dependency.
``up --dry-run`` prints the dependency-ordered start plan without creating
project state or starting VMs. ``down --dry-run``
shows the corresponding reverse order, stopping dependents before dependencies.
An explicitly named service starts even when its profile was not globally
enabled; Quocker starts only that service and its declared dependencies.
Dependencies in an inactive profile fail unless that profile is enabled by
``--profile``, ``COMPOSE_PROFILES``, or a profile shared with the targeted
service. Other services that share the target's profile are not started just
because one service was named.
