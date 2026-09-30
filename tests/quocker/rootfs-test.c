/*
 * Unit tests for safe OCI layer materialization.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-rootfs.h"
#include "../../scripts/quocker-oci.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/xattr.h>
#include <unistd.h>

static gboolean write_layer(const char *path, const char *const *names,
                            const char *const *contents, const mode_t *types,
                            guint count) {
  struct archive *writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
    archive_write_free(writer);
    return FALSE;
  }
  gboolean ok = TRUE;
  for (guint i = 0; i < count; i++) {
    struct archive_entry *entry = archive_entry_new();
    const char *content = contents[i] ? contents[i] : "";
    archive_entry_set_pathname(entry, names[i]);
    archive_entry_set_filetype(entry, types[i]);
    archive_entry_set_perm(entry, types[i] == AE_IFDIR ? 0755 : 0644);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, types[i] == AE_IFREG ? strlen(content) : 0);
    if (archive_write_header(writer, entry) != ARCHIVE_OK) {
      ok = FALSE;
    } else if (types[i] == AE_IFREG && *content &&
               archive_write_data(writer, content, strlen(content)) !=
                   (la_ssize_t)strlen(content)) {
      ok = FALSE;
    }
    archive_entry_free(entry);
    if (!ok) {
      break;
    }
  }
  if (archive_write_close(writer) != ARCHIVE_OK) {
    ok = FALSE;
  }
  archive_write_free(writer);
  return ok;
}

static gboolean write_symlink_traversal_layer(const char *path) {
  struct archive *writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
    archive_write_free(writer);
    return FALSE;
  }
  struct archive_entry *link = archive_entry_new();
  archive_entry_set_pathname(link, "link");
  archive_entry_set_filetype(link, AE_IFLNK);
  archive_entry_set_perm(link, 0777);
  archive_entry_set_symlink(link, "../../outside");
  gboolean ok = archive_write_header(writer, link) == ARCHIVE_OK;
  archive_entry_free(link);
  struct archive_entry *file = archive_entry_new();
  archive_entry_set_pathname(file, "link/escape");
  archive_entry_set_filetype(file, AE_IFREG);
  archive_entry_set_perm(file, 0644);
  archive_entry_set_size(file, 3);
  if (ok) {
    ok = archive_write_header(writer, file) == ARCHIVE_OK &&
         archive_write_data(writer, "bad", 3) == 3;
  }
  archive_entry_free(file);
  if (archive_write_close(writer) != ARCHIVE_OK) {
    ok = FALSE;
  }
  archive_write_free(writer);
  return ok;
}

static gboolean write_xattr_layer(const char *path) {
  struct archive *writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
    archive_write_free(writer);
    return FALSE;
  }
  struct archive_entry *entry = archive_entry_new();
  archive_entry_set_pathname(entry, "xattr-file");
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  archive_entry_set_uid(entry, 123);
  archive_entry_set_gid(entry, 456);
  archive_entry_set_size(entry, 1);
  archive_entry_xattr_add_entry(entry, "user.example", "user-value", 10);
  archive_entry_xattr_add_entry(entry, "security.capability", "cap-value", 9);
  gboolean ok = archive_write_header(writer, entry) == ARCHIVE_OK &&
                archive_write_data(writer, "x", 1) == 1;
  archive_entry_free(entry);
  if (archive_write_close(writer) != ARCHIVE_OK) {
    ok = FALSE;
  }
  archive_write_free(writer);
  return ok;
}

static gboolean write_device_layer(const char *path) {
  struct archive *writer = archive_write_new();
  archive_write_set_format_pax_restricted(writer);
  if (archive_write_open_filename(writer, path) != ARCHIVE_OK) {
    archive_write_free(writer);
    return FALSE;
  }
  struct archive_entry *entry = archive_entry_new();
  archive_entry_set_pathname(entry, "dev/null");
  archive_entry_set_filetype(entry, AE_IFCHR);
  archive_entry_set_perm(entry, 0666);
  archive_entry_set_uid(entry, 0);
  archive_entry_set_gid(entry, 0);
  archive_entry_set_rdevmajor(entry, 1);
  archive_entry_set_rdevminor(entry, 3);
  gboolean ok = archive_write_header(writer, entry) == ARCHIVE_OK;
  archive_entry_free(entry);
  if (archive_write_close(writer) != ARCHIVE_OK) {
    ok = FALSE;
  }
  archive_write_free(writer);
  return ok;
}

static char *new_cache_directory(void) {
  GError *error = NULL;
  char *path = g_dir_make_tmp("quocker-rootfs-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(path);
  return path;
}

static void test_oci_prune_preserves_referenced_vm_disk(void) {
  char *cache = new_cache_directory();
  char *rootfs = g_build_filename(cache, "rootfs", NULL);
  char *blobs = g_build_filename(cache, "blobs", "sha256", NULL);
  char *refs = g_build_filename(cache, "refs", NULL);
  char *project = g_build_filename(cache, "project", ".quocker", "demo", NULL);
  g_assert_cmpint(g_mkdir_with_parents(rootfs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(blobs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(project, 0700), ==, 0);
  const char *config_contents = "cached config";
  const char *layer_contents = "cached layer";
  char *config_digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, config_contents, -1);
  char *layer_digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, layer_contents, -1);
  char *manifest_contents = g_strdup_printf(
      "{\"config\":{\"digest\":\"sha256:%s\"},\"layers\":[{"
      "\"digest\":\"sha256:%s\"}]}",
      config_digest, layer_digest);
  char *manifest_digest = g_compute_checksum_for_string(
      G_CHECKSUM_SHA256, manifest_contents, -1);
  char *base = g_strdup_printf("%s/%s.ext4", rootfs, manifest_digest);
  char *rootfs_tree = g_build_filename(rootfs, manifest_digest, NULL);
  char *marker = g_strconcat(rootfs_tree, ".complete", NULL);
  char *blob = g_build_filename(blobs, "unused-layer", NULL);
  char *manifest_blob = g_build_filename(blobs, manifest_digest, NULL);
  char *config_blob = g_build_filename(blobs, config_digest, NULL);
  char *layer_blob = g_build_filename(blobs, layer_digest, NULL);
  char *overlay = g_build_filename(project, "web.qcow2", NULL);
  g_assert_true(g_file_set_contents(base, "base-disk", -1, NULL));
  g_assert_cmpint(g_mkdir(rootfs_tree, 0700), ==, 0);
  g_assert_true(g_file_set_contents(marker, "complete", -1, NULL));
  g_assert_true(g_file_set_contents(blob, "compressed-layer", -1, NULL));
  g_assert_true(g_file_set_contents(manifest_blob, manifest_contents, -1,
                                    NULL));
  g_assert_true(g_file_set_contents(config_blob, config_contents, -1, NULL));
  g_assert_true(g_file_set_contents(layer_blob, layer_contents, -1, NULL));
  g_assert_true(g_file_set_contents(overlay, "overlay", -1, NULL));
  g_assert_true(quocker_oci_cache_reference(cache, project, "web", overlay,
                                            base));

  g_assert_true(quocker_oci_cache_prune(cache));
  g_assert_true(g_file_test(base, G_FILE_TEST_IS_REGULAR));
  g_assert_true(g_file_test(overlay, G_FILE_TEST_IS_REGULAR));
  g_assert_true(g_file_test(manifest_blob, G_FILE_TEST_IS_REGULAR));
  g_assert_true(g_file_test(config_blob, G_FILE_TEST_IS_REGULAR));
  g_assert_true(g_file_test(layer_blob, G_FILE_TEST_IS_REGULAR));
  g_assert_false(g_file_test(blob, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(rootfs_tree, G_FILE_TEST_IS_DIR));
  g_assert_true(g_file_test(marker, G_FILE_TEST_IS_REGULAR));

  g_assert_cmpint(g_unlink(overlay), ==, 0);
  g_assert_true(quocker_oci_cache_prune(cache));
  g_assert_false(g_file_test(base, G_FILE_TEST_EXISTS));
  char *legacy_disk = g_build_filename(
      rootfs, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.ext4",
      NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_true(g_file_set_contents(legacy_disk, "legacy", -1, NULL));
  g_assert_true(quocker_oci_cache_prune(cache));
  g_assert_true(g_file_test(legacy_disk, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpint(g_unlink(legacy_disk), ==, 0);
  g_free(legacy_disk);
  g_assert_true(quocker_oci_cache_prune(cache));
  char *lock = g_build_filename(cache, ".lock", NULL);
  g_assert_cmpint(g_unlink(lock), ==, 0);
  g_assert_cmpint(g_rmdir(refs), ==, 0);
  g_assert_cmpint(g_rmdir(blobs), ==, 0);
  char *blob_root = g_build_filename(cache, "blobs", NULL);
  g_assert_cmpint(g_rmdir(blob_root), ==, 0);
  g_assert_cmpint(g_rmdir(project), ==, 0);
  char *quocker_dir = g_build_filename(cache, "project", ".quocker", NULL);
  char *project_parent = g_build_filename(cache, "project", NULL);
  g_assert_cmpint(g_rmdir(quocker_dir), ==, 0);
  g_assert_cmpint(g_rmdir(project_parent), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_free(project_parent);
  g_free(quocker_dir);
  g_free(blob_root);
  g_free(lock);
  g_free(overlay);
  g_free(layer_blob);
  g_free(config_blob);
  g_free(manifest_blob);
  g_free(blob);
  g_free(marker);
  g_free(rootfs_tree);
  g_free(base);
  g_free(manifest_digest);
  g_free(manifest_contents);
  g_free(layer_digest);
  g_free(config_digest);
  g_free(project);
  g_free(refs);
  g_free(blobs);
  g_free(rootfs);
  g_free(cache);
}

static void test_whiteouts_and_metadata(void) {
  char *cache = new_cache_directory();
  char *lower = g_build_filename(cache, "lower.tar", NULL);
  char *upper = g_build_filename(cache, "upper.tar", NULL);
  const char *lower_names[] = {"remove-me", "dir/old", "dir/keep"};
  const char *lower_data[] = {"gone", "old", "kept"};
  mode_t regular[] = {AE_IFREG, AE_IFREG, AE_IFREG};
  g_assert_true(write_layer(lower, lower_names, lower_data, regular,
                            G_N_ELEMENTS(lower_names)));
  const char *upper_names[] = {".wh.remove-me", "dir/.wh..wh..opq", "dir/new"};
  const char *upper_data[] = {"", "", "new"};
  g_assert_true(write_layer(upper, upper_names, upper_data, regular,
                            G_N_ELEMENTS(upper_names)));
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, lower);
  g_ptr_array_add(layers, upper);
  char *rootfs = NULL;
  const char *digest =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  g_assert_true(quocker_rootfs_materialize(digest, layers, cache, &rootfs));
  char *path = g_build_filename(rootfs, "remove-me", NULL);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(path);
  path = g_build_filename(rootfs, "dir", "old", NULL);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(path);
  path = g_build_filename(rootfs, "dir", "keep", NULL);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(path);
  path = g_build_filename(rootfs, "dir", "new", NULL);
  gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_assert_cmpstr(contents, ==, "new");
  g_free(contents);
  char uid[16] = {0};
  ssize_t uid_size = getxattr(path, "user.quocker.oci.uid", uid, sizeof(uid));
  g_assert_cmpint(uid_size, >, 0);
  g_assert_cmpstr(uid, ==, "0");
  g_free(path);
  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpuint(removed, >, 0);
  g_assert_cmpint(g_unlink(lower), ==, 0);
  g_assert_cmpint(g_unlink(upper), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(rootfs);
  g_free(lower);
  g_free(upper);
  g_free(cache);
}

static void test_traversal_is_rejected(void) {
  char *cache = new_cache_directory();
  char *layer = g_build_filename(cache, "unsafe.tar", NULL);
  const char *names[] = {"../outside"};
  const char *data[] = {"bad"};
  mode_t types[] = {AE_IFREG};
  g_assert_true(write_layer(layer, names, data, types, G_N_ELEMENTS(names)));
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, layer);
  const char *digest =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  g_assert_false(quocker_rootfs_materialize(digest, layers, cache, NULL));
  char *escaped = g_build_filename(cache, "outside", NULL);
  g_assert_false(g_file_test(escaped, G_FILE_TEST_EXISTS));
  g_free(escaped);
  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpint(g_unlink(layer), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(layer);
  g_free(cache);
}

static void test_symlink_parent_is_rejected(void) {
  char *cache = new_cache_directory();
  char *layer = g_build_filename(cache, "symlink.tar", NULL);
  g_assert_true(write_symlink_traversal_layer(layer));
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, layer);
  const char *digest =
      "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  g_assert_false(quocker_rootfs_materialize(digest, layers, cache, NULL));
  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpint(g_unlink(layer), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(layer);
  g_free(cache);
}

static void test_completion_marker_obeys_cache_limit(void) {
  char *cache = new_cache_directory();
  char *layer = g_build_filename(cache, "marker.tar", NULL);
  const char *names[] = {"payload"};
  const char *contents[] = {"abc"};
  mode_t types[] = {AE_IFREG};
  g_assert_true(
      write_layer(layer, names, contents, types, G_N_ELEMENTS(names)));
  struct stat layer_stat;
  g_assert_cmpint(g_stat(layer, &layer_stat), ==, 0);

  /* Admit the layer and extracted file, but not the completion marker. */
  char *limit =
      g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)layer_stat.st_size + 3);
  g_setenv("QUOCKER_OCI_CACHE_LIMIT", limit, TRUE);
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, layer);
  const char *digest =
      "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  g_assert_false(quocker_rootfs_materialize(digest, layers, cache, NULL));
  g_unsetenv("QUOCKER_OCI_CACHE_LIMIT");

  char *rootfs_name = g_strndup(digest + strlen("sha256:"), 64);
  char *rootfs = g_build_filename(cache, "rootfs", rootfs_name, NULL);
  g_assert_false(g_file_test(rootfs, G_FILE_TEST_EXISTS));
  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpint(g_unlink(layer), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(rootfs);
  g_free(rootfs_name);
  g_free(limit);
  g_free(layer);
  g_free(cache);
}

static void test_cache_accounting_fails_closed(void) {
  char *cache_file = NULL;
  GError *error = NULL;
  int fd = g_file_open_tmp("quocker-cache-accounting-XXXXXX", &cache_file,
                           &error);
  g_assert_cmpint(fd, >=, 0);
  g_assert_no_error(error);
  close(fd);
  g_setenv("QUOCKER_OCI_CACHE_LIMIT", "1073741824", TRUE);
  g_assert_false(quocker_oci_cache_has_room(cache_file, 1));
  g_unsetenv("QUOCKER_OCI_CACHE_LIMIT");
  g_assert_cmpint(g_unlink(cache_file), ==, 0);
  g_free(cache_file);
}

static void test_extended_attributes_are_staged_without_host_privilege(void) {
  char *cache = new_cache_directory();
  char *layer = g_build_filename(cache, "xattrs.tar", NULL);
  g_assert_true(write_xattr_layer(layer));
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, layer);
  const char *digest =
      "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  char *rootfs = NULL;
  g_assert_true(quocker_rootfs_materialize(digest, layers, cache, &rootfs));

  char *file = g_build_filename(rootfs, "xattr-file", NULL);
  const char *names[] = {"user.example", "security.capability"};
  const char *values[] = {"user-value", "cap-value"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)names[i], strlen(names[i]));
    char *staged_name = g_strdup_printf("user.quocker.oci.xattr.%s",
                                        g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    gsize expected_size = strlen(names[i]) + 1 + strlen(values[i]);
    guchar *record = g_malloc(expected_size);
    ssize_t size = getxattr(file, staged_name, record, expected_size);
    g_assert_cmpint(size, ==, (ssize_t)expected_size);
    g_assert_cmpmem(record, strlen(names[i]) + 1, names[i],
                    strlen(names[i]) + 1);
    g_assert_cmpmem(record + strlen(names[i]) + 1, strlen(values[i]), values[i],
                    strlen(values[i]));
    g_free(record);
    g_free(staged_name);
  }

  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpint(g_unlink(layer), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(file);
  g_free(rootfs);
  g_free(layer);
  g_free(cache);
}

static void test_device_nodes_are_inert_staging_placeholders(void) {
  char *cache = new_cache_directory();
  char *layer = g_build_filename(cache, "device.tar", NULL);
  g_assert_true(write_device_layer(layer));
  GPtrArray *layers = g_ptr_array_new();
  g_ptr_array_add(layers, layer);
  const char *digest =
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  char *rootfs = NULL;
  g_assert_true(quocker_rootfs_materialize(digest, layers, cache, &rootfs));

  char *device = g_build_filename(rootfs, "dev", "null", NULL);
  struct stat st;
  g_assert_cmpint(g_lstat(device, &st), ==, 0);
  g_assert_true(S_ISREG(st.st_mode));
  char type[16] = {0};
  char major_text[16] = {0};
  char minor_text[16] = {0};
  g_assert_cmpint(getxattr(device, "user.quocker.oci.type", type, sizeof(type)),
                  >, 0);
  g_assert_cmpstr(type, ==, "char");
  g_assert_cmpint(getxattr(device, "user.quocker.oci.devmajor", major_text,
                           sizeof(major_text)),
                  >, 0);
  g_assert_cmpint(getxattr(device, "user.quocker.oci.devminor", minor_text,
                           sizeof(minor_text)),
                  >, 0);
  g_assert_cmpstr(major_text, ==, "1");
  g_assert_cmpstr(minor_text, ==, "3");

  guint64 removed = 0;
  g_assert_true(quocker_rootfs_cache_prune(cache, &removed));
  g_assert_cmpint(g_unlink(layer), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_ptr_array_free(layers, TRUE);
  g_free(device);
  g_free(rootfs);
  g_free(layer);
  g_free(cache);
}

static void test_os_release_detection_is_confined_to_guest_rootfs(void) {
  char *cache = new_cache_directory();
  char *rootfs = g_build_filename(cache, "guest", NULL);
  char *etc = g_build_filename(rootfs, "etc", NULL);
  char *usr = g_build_filename(rootfs, "usr", NULL);
  char *lib = g_build_filename(usr, "lib", NULL);
  char *os_release = g_build_filename(etc, "os-release", NULL);
  char *target_release = g_build_filename(lib, "os-release", NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(etc, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(usr, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(lib, 0700), ==, 0);
  const char *release = "NAME=Fedora\nID=Fedora\nID_LIKE=\"rhel fedora\"\n"
                        "VERSION_ID=42\nPRETTY_NAME=ignored\n";
  g_assert_true(g_file_set_contents(target_release, release, -1, NULL));

  QuockerDistroInfo info = {0};
  g_assert_true(g_file_set_contents(os_release, release, -1, NULL));
  g_assert_true(quocker_rootfs_detect_distro(rootfs, &info));
  g_assert_cmpstr(info.id, ==, "fedora");
  g_assert_cmpstr(info.id_like, ==, "rhel fedora");
  g_assert_cmpstr(info.version_id, ==, "42");
  quocker_distro_info_clear(&info);

  g_assert_cmpint(g_unlink(os_release), ==, 0);
  g_assert_cmpint(symlink("/etc/passwd", os_release), ==, 0);
  g_assert_false(quocker_rootfs_detect_distro(rootfs, &info));
  g_assert_cmpint(g_unlink(os_release), ==, 0);
  g_assert_cmpint(symlink("../usr/lib/os-release", os_release), ==, 0);
  g_assert_true(quocker_rootfs_detect_distro(rootfs, &info));
  g_assert_cmpstr(info.id, ==, "fedora");
  quocker_distro_info_clear(&info);

  g_assert_cmpint(g_unlink(os_release), ==, 0);
  const char binary_release[] = "ID=evil\0ID=alpine\n";
  g_assert_true(g_file_set_contents(os_release, binary_release,
                                    sizeof(binary_release) - 1, NULL));
  g_assert_false(quocker_rootfs_detect_distro(rootfs, &info));

  g_assert_cmpint(g_unlink(os_release), ==, 0);
  g_assert_cmpint(g_unlink(target_release), ==, 0);
  g_assert_cmpint(g_rmdir(etc), ==, 0);
  g_assert_cmpint(g_rmdir(lib), ==, 0);
  g_assert_cmpint(g_rmdir(usr), ==, 0);
  g_assert_cmpint(g_rmdir(rootfs), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_free(target_release);
  g_free(os_release);
  g_free(lib);
  g_free(usr);
  g_free(etc);
  g_free(rootfs);
  g_free(cache);
}

static void test_kernel_module_releases_are_confined_and_detected(void) {
  char *cache = new_cache_directory();
  char *rootfs = g_build_filename(cache, "modules-guest", NULL);
  char *usr = g_build_filename(rootfs, "usr", NULL);
  char *library = g_build_filename(usr, "lib", NULL);
  char *modules = g_build_filename(library, "modules", NULL);
  char *release = g_build_filename(modules, "6.8.1-quocker", NULL);
  char *lib_link = g_build_filename(rootfs, "lib", NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(usr, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(library, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(modules, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(release, 0700), ==, 0);
  g_assert_cmpint(symlink("usr/lib", lib_link), ==, 0);

  QuockerDistroInfo info = {0};
  g_assert_true(quocker_rootfs_detect_distro(rootfs, &info));
  g_assert_null(info.id);
  g_assert_cmpuint(info.kernel_module_releases->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(info.kernel_module_releases, 0), ==,
                  "6.8.1-quocker");
  quocker_distro_info_clear(&info);

  g_assert_cmpint(g_rmdir(release), ==, 0);
  g_assert_cmpint(g_rmdir(modules), ==, 0);
  char *outside = g_build_filename(cache, "outside", NULL);
  char *outside_modules = g_build_filename(outside, "modules", NULL);
  char *outside_release = g_build_filename(outside_modules, "9.9.9-host", NULL);
  g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(outside_modules, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(outside_release, 0700), ==, 0);
  g_assert_cmpint(g_unlink(lib_link), ==, 0);
  g_assert_cmpint(symlink("../outside", lib_link), ==, 0);
  g_assert_false(quocker_rootfs_detect_distro(rootfs, &info));

  g_assert_cmpint(g_unlink(lib_link), ==, 0);
  g_assert_cmpint(g_rmdir(outside_release), ==, 0);
  g_assert_cmpint(g_rmdir(outside_modules), ==, 0);
  g_assert_cmpint(g_rmdir(outside), ==, 0);
  g_assert_cmpint(g_rmdir(library), ==, 0);
  g_assert_cmpint(g_rmdir(usr), ==, 0);
  g_assert_cmpint(g_rmdir(rootfs), ==, 0);
  g_assert_cmpint(g_rmdir(cache), ==, 0);
  g_free(lib_link);
  g_free(outside_release);
  g_free(outside_modules);
  g_free(outside);
  g_free(release);
  g_free(modules);
  g_free(library);
  g_free(usr);
  g_free(rootfs);
  g_free(cache);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/rootfs/whiteouts-metadata",
                  test_whiteouts_and_metadata);
  g_test_add_func("/quocker/rootfs/cache-reference-prune",
                  test_oci_prune_preserves_referenced_vm_disk);
  g_test_add_func("/quocker/rootfs/path-traversal", test_traversal_is_rejected);
  g_test_add_func("/quocker/rootfs/symlink-parent",
                  test_symlink_parent_is_rejected);
  g_test_add_func("/quocker/rootfs/marker-cache-limit",
                  test_completion_marker_obeys_cache_limit);
  g_test_add_func("/quocker/rootfs/cache-accounting-fails-closed",
                  test_cache_accounting_fails_closed);
  g_test_add_func("/quocker/rootfs/extended-attributes",
                  test_extended_attributes_are_staged_without_host_privilege);
  g_test_add_func("/quocker/rootfs/device-placeholders",
                  test_device_nodes_are_inert_staging_placeholders);
  g_test_add_func("/quocker/rootfs/os-release",
                  test_os_release_detection_is_confined_to_guest_rootfs);
  g_test_add_func("/quocker/rootfs/kernel-module-releases",
                  test_kernel_module_releases_are_confined_and_detected);
  return g_test_run();
}
