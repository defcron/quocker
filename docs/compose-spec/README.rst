Schema snapshot
===============

``compose-spec.json`` is copied from the `Compose Specification`_ repository
at commit ``914ec15d1fa498969c0df5c1d672306db3256089``. Its SHA-256 is
``d61cc3df8c6e6a727043e84f3405c69ffd63d341f629db8480f1d2ee5405b10c``.
The snapshot uses JSON Schema 2020-12 and is Quocker's active compatibility
baseline. Quocker validates merged Compose files against it. Updates must pin
a commit, record the new checksum, review schema changes, and update
validation fixtures together. ``scripts/check-compose-spec.sh`` verifies that
the vendored file matches the checksum recorded here; the fork-only CI job runs
this check on every pipeline.

The upstream schema is distributed under Apache License 2.0; see ``LICENSE``.

.. _Compose Specification: https://github.com/compose-spec/compose-spec
