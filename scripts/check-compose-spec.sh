#!/bin/sh
# Verify the vendored Compose schema still matches its recorded pinned digest.
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(dirname "$script_dir")
schema_file="$repo_root/docs/compose-spec/compose-spec.json"
readme_file="$repo_root/docs/compose-spec/README.rst"

expected_hash=$(sed -n \
  's/^``\([[:xdigit:]]\{64\}\)``\.$/\1/p' "$readme_file")
if [ -z "$expected_hash" ]; then
  printf '%s\n' "Compose schema digest is missing from $readme_file" >&2
  exit 1
fi

actual_hash=$(sha256sum "$schema_file" | awk '{print $1}')
if [ "$actual_hash" != "$expected_hash" ]; then
  printf '%s\n' \
    "Compose schema digest mismatch: expected $expected_hash, got $actual_hash" \
    >&2
  exit 1
fi

printf 'Compose schema checksum OK (%s)\n' "$actual_hash"
