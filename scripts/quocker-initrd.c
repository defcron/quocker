/*
 * Quocker guest initramfs construction.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quocker-initrd.h"

#include <archive.h>
#include <archive_entry.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INITRD_MAX_BASE_BYTES (1024ULL * 1024ULL * 1024ULL)
#define INITRD_MAX_INIT_BYTES (128ULL * 1024ULL * 1024ULL)
#define INITRD_MAX_CONFIG_BYTES (16ULL * 1024ULL * 1024ULL)
#define INITRD_MAX_ITEMS 65536u
#define INITRD_MAX_STRING (64u * 1024u)

static GQuark initrd_error_quark(void) {
  return g_quark_from_static_string("quocker-initrd-error");
}

static void initrd_error(GError **error, const char *format, ...)
    G_GNUC_PRINTF(2, 3);

static void initrd_error(GError **error, const char *format, ...) {
  if (!error || *error) {
    return;
  }
  va_list args;
  va_start(args, format);
  char *message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error_literal(error, initrd_error_quark(), 1, message);
  g_free(message);
}

static void append_be32(GByteArray *bytes, guint32 value) {
  guint32 encoded = GUINT32_TO_BE(value);
  g_byte_array_append(bytes, (const guint8 *)&encoded, sizeof(encoded));
}

static gboolean append_string(GByteArray *bytes, const char *value,
                              gboolean with_length, GError **error) {
  gsize length = value ? strlen(value) : 0;
  if (length > INITRD_MAX_STRING || length > G_MAXUINT32 ||
      bytes->len > INITRD_MAX_CONFIG_BYTES - length - (with_length ? 4 : 0)) {
    initrd_error(error, "guest runtime configuration exceeds its size limit");
    return FALSE;
  }
  if (with_length) {
    append_be32(bytes, (guint32)length);
  }
  if (length) {
    g_byte_array_append(bytes, (const guint8 *)value, (guint)length);
  }
  return TRUE;
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
  return g_strcmp0(*(char *const *)left, *(char *const *)right);
}

static gboolean valid_environment_name(const char *name) {
  if (!name || !*name || !(g_ascii_isalpha(*name) || *name == '_')) {
    return FALSE;
  }
  for (const char *p = name + 1; *p; p++) {
    if (!(g_ascii_isalnum(*p) || *p == '_')) {
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean valid_mount_target(const char *target) {
  if (!target || target[0] != '/' || target[1] == '\0' ||
      strlen(target) > INITRD_MAX_STRING) {
    return FALSE;
  }
  const char *component = target + 1;
  for (const char *cursor = component;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      gsize length = (gsize)(cursor - component);
      if (!length || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.')) {
        return FALSE;
      }
      if (!*cursor) {
        return TRUE;
      }
      component = cursor + 1;
    }
  }
}

gboolean quocker_guest_config_encode(const QuockerRuntimeConfig *runtime,
                                     GByteArray **config_out, GError **error) {
  if (config_out) {
    *config_out = NULL;
  }
  if (!runtime || !runtime->argv || !runtime->environment || !config_out ||
      !runtime->argv->len || runtime->argv->len > INITRD_MAX_ITEMS ||
      g_hash_table_size(runtime->environment) > INITRD_MAX_ITEMS ||
      (runtime->mounts && runtime->mounts->len > 25)) {
    initrd_error(error,
                 "guest runtime configuration is incomplete or too large");
    return FALSE;
  }
  if (!g_ptr_array_index(runtime->argv, 0) ||
      !*(char *)g_ptr_array_index(runtime->argv, 0)) {
    initrd_error(error, "guest command must include a nonempty executable");
    return FALSE;
  }
  GByteArray *bytes = g_byte_array_sized_new(256);
  g_byte_array_append(bytes, (const guint8 *)"QCFG", 4);
  append_be32(bytes, 2);
  append_be32(bytes, runtime->argv->len);
  append_be32(bytes, g_hash_table_size(runtime->environment));
  append_be32(bytes, runtime->mounts ? runtime->mounts->len : 0);
  gsize working_size =
      runtime->working_directory ? strlen(runtime->working_directory) : 0;
  gsize user_size = runtime->user ? strlen(runtime->user) : 0;
  if (working_size > INITRD_MAX_STRING || user_size > INITRD_MAX_STRING) {
    g_byte_array_free(bytes, TRUE);
    initrd_error(error,
                 "guest working directory or user exceeds the size limit");
    return FALSE;
  }
  append_be32(bytes, (guint32)working_size);
  append_be32(bytes, (guint32)user_size);
  for (guint i = 0; i < runtime->argv->len; i++) {
    if (!append_string(bytes, g_ptr_array_index(runtime->argv, i), TRUE,
                       error)) {
      g_byte_array_free(bytes, TRUE);
      return FALSE;
    }
  }
  GPtrArray *environment = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init(&iter, runtime->environment);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    if (!valid_environment_name(key) || !value) {
      g_ptr_array_free(environment, TRUE);
      g_byte_array_free(bytes, TRUE);
      initrd_error(error,
                   "guest environment contains an invalid name or value");
      return FALSE;
    }
    char *assignment = g_strdup_printf("%s=%s", (char *)key, (char *)value);
    g_ptr_array_add(environment, assignment);
  }
  g_ptr_array_sort(environment, compare_strings);
  for (guint i = 0; i < environment->len; i++) {
    if (!append_string(bytes, g_ptr_array_index(environment, i), TRUE, error)) {
      g_ptr_array_free(environment, TRUE);
      g_byte_array_free(bytes, TRUE);
      return FALSE;
    }
  }
  g_ptr_array_free(environment, TRUE);
  for (guint i = 0; runtime->mounts && i < runtime->mounts->len; i++) {
    QuockerGuestMount *mount = g_ptr_array_index(runtime->mounts, i);
    if (!mount || !valid_mount_target(mount->target) ||
        !append_string(bytes, mount->target, TRUE, error)) {
      g_byte_array_free(bytes, TRUE);
      if (error && !*error) {
        initrd_error(error, "guest volume target is invalid");
      }
      return FALSE;
    }
    append_be32(bytes, mount->read_only ? 1 : 0);
  }
  if (!append_string(bytes, runtime->working_directory, FALSE, error) ||
      !append_string(bytes, runtime->user, FALSE, error)) {
    g_byte_array_free(bytes, TRUE);
    return FALSE;
  }
  *config_out = bytes;
  return TRUE;
}

static gboolean write_all(int fd, const guint8 *data, gsize length) {
  while (length) {
    ssize_t written = write(fd, data, length);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return FALSE;
    }
    data += written;
    length -= (gsize)written;
  }
  return TRUE;
}

static gboolean copy_verified_file(int source_fd, int output_fd,
                                   const char *expected_digest,
                                   guint64 maximum_size, GError **error) {
  struct stat st;
  if (fstat(source_fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (guint64)st.st_size > maximum_size) {
    initrd_error(error, "initramfs input is unsafe or exceeds its size limit");
    return FALSE;
  }
  EVP_MD_CTX *checksum = EVP_MD_CTX_new();
  if (!checksum || EVP_DigestInit_ex(checksum, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(checksum);
    initrd_error(error, "could not initialize initramfs digest verification");
    return FALSE;
  }
  guint8 buffer[64 * 1024];
  guint64 remaining = (guint64)st.st_size;
  gboolean ok = TRUE;
  while (remaining) {
    size_t request = (size_t)MIN((guint64)sizeof(buffer), remaining);
    ssize_t count = read(source_fd, buffer, request);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0 || EVP_DigestUpdate(checksum, buffer, (size_t)count) != 1 ||
        !write_all(output_fd, buffer, (gsize)count)) {
      ok = FALSE;
      break;
    }
    remaining -= (guint64)count;
  }
  guint8 digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  ok = ok && EVP_DigestFinal_ex(checksum, digest, &digest_length) == 1 &&
       digest_length == 32;
  EVP_MD_CTX_free(checksum);
  char *actual = NULL;
  if (ok) {
    GString *hex = g_string_sized_new(64);
    for (guint i = 0; i < digest_length; i++) {
      g_string_append_printf(hex, "%02x", digest[i]);
    }
    actual = g_strdup_printf("sha256:%s", hex->str);
    g_string_free(hex, TRUE);
    ok = g_strcmp0(actual, expected_digest) == 0;
  }
  g_free(actual);
  if (!ok) {
    initrd_error(error, "base initrd could not be copied or its SHA-256 digest "
                        "does not match the signed catalog");
  }
  return ok;
}

static gboolean read_guest_init(const char *path, guint8 **data_out,
                                gsize *size_out, GError **error) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    initrd_error(error, "cannot open Quocker guest init: %s",
                 g_strerror(errno));
    return FALSE;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      (guint64)st.st_size > INITRD_MAX_INIT_BYTES) {
    close(fd);
    initrd_error(error,
                 "Quocker guest init is unsafe or exceeds its size limit");
    return FALSE;
  }
  guint8 *data = g_malloc((gsize)st.st_size);
  gsize offset = 0;
  while (offset < (gsize)st.st_size) {
    ssize_t count = read(fd, data + offset, (gsize)st.st_size - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      g_free(data);
      close(fd);
      initrd_error(error, "could not read Quocker guest init");
      return FALSE;
    }
    offset += (gsize)count;
  }
  close(fd);
  *data_out = data;
  *size_out = offset;
  return TRUE;
}

static gboolean guest_init_architecture_matches(const guint8 *data, gsize size,
                                                const char *architecture) {
  if (size < EI_NIDENT + 4 || memcmp(data, ELFMAG, SELFMAG) != 0 ||
      (data[EI_DATA] != ELFDATA2LSB && data[EI_DATA] != ELFDATA2MSB)) {
    return FALSE;
  }
  guint16 machine =
      data[EI_DATA] == ELFDATA2LSB
          ? (guint16)data[EI_NIDENT + 2] | ((guint16)data[EI_NIDENT + 3] << 8)
          : ((guint16)data[EI_NIDENT + 2] << 8) | (guint16)data[EI_NIDENT + 3];
  if ((g_str_equal(architecture, "amd64") ||
       g_str_equal(architecture, "x86_64")) &&
      data[EI_CLASS] == ELFCLASS64 && data[EI_DATA] == ELFDATA2LSB &&
      machine == EM_X86_64) {
    return TRUE;
  }
  if ((g_str_equal(architecture, "arm64") ||
       g_str_equal(architecture, "aarch64")) &&
      data[EI_CLASS] == ELFCLASS64 && data[EI_DATA] == ELFDATA2LSB &&
      machine == EM_AARCH64) {
    return TRUE;
  }
  if ((g_str_equal(architecture, "386") || g_str_equal(architecture, "i386")) &&
      data[EI_CLASS] == ELFCLASS32 && data[EI_DATA] == ELFDATA2LSB &&
      machine == EM_386) {
    return TRUE;
  }
  if ((g_str_equal(architecture, "arm") ||
       g_str_equal(architecture, "armv7")) &&
      data[EI_CLASS] == ELFCLASS32 && data[EI_DATA] == ELFDATA2LSB &&
      machine == EM_ARM) {
    return TRUE;
  }
  if (g_str_equal(architecture, "ppc64le") && data[EI_CLASS] == ELFCLASS64 &&
      data[EI_DATA] == ELFDATA2LSB && machine == EM_PPC64) {
    return TRUE;
  }
  if (g_str_equal(architecture, "s390x") && data[EI_CLASS] == ELFCLASS64 &&
      data[EI_DATA] == ELFDATA2MSB && machine == EM_S390) {
    return TRUE;
  }
  return FALSE;
}

static gboolean archive_write_entry(struct archive *archive, const char *path,
                                    mode_t mode, const guint8 *data, gsize size,
                                    GError **error) {
  struct archive_entry *entry = archive_entry_new();
  archive_entry_set_pathname(entry, path);
  archive_entry_set_uid(entry, 0);
  archive_entry_set_gid(entry, 0);
  archive_entry_set_mtime(entry, 0, 0);
  archive_entry_set_perm(entry, mode & 07777);
  archive_entry_set_filetype(entry, S_ISDIR(mode) ? AE_IFDIR : AE_IFREG);
  archive_entry_set_nlink(entry, 1);
  archive_entry_set_size(entry, size);
  int status = archive_write_header(archive, entry);
  if (status == ARCHIVE_OK && size &&
      archive_write_data(archive, data, size) != (la_ssize_t)size) {
    status = ARCHIVE_FATAL;
  }
  archive_entry_free(entry);
  if (status != ARCHIVE_OK) {
    initrd_error(error, "could not write Quocker initramfs archive: %s",
                 archive_error_string(archive));
    return FALSE;
  }
  return TRUE;
}

gboolean quocker_initrd_build(const char *base_initrd_path,
                              const char *base_initrd_digest,
                              const char *guest_init_path,
                              const char *guest_architecture,
                              const QuockerRuntimeConfig *runtime,
                              const char *output_path, GError **error) {
  if (error) {
    *error = NULL;
  }
  if (!base_initrd_path || !base_initrd_digest || !guest_init_path ||
      !guest_architecture || !output_path || !g_path_is_absolute(output_path)) {
    initrd_error(error, "verified initrd, guest init, and absolute output path "
                        "are required");
    return FALSE;
  }
  GByteArray *config = NULL;
  guint8 *guest_init = NULL;
  gsize guest_init_size = 0;
  if (!quocker_guest_config_encode(runtime, &config, error) ||
      !read_guest_init(guest_init_path, &guest_init, &guest_init_size, error)) {
    g_clear_pointer(&config, g_byte_array_unref);
    g_free(guest_init);
    return FALSE;
  }
  if (!guest_init_architecture_matches(guest_init, guest_init_size,
                                       guest_architecture)) {
    initrd_error(error,
                 "Quocker guest init architecture does not match image %s",
                 guest_architecture);
    g_byte_array_unref(config);
    g_free(guest_init);
    return FALSE;
  }
  int base_fd = open(base_initrd_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (base_fd < 0) {
    initrd_error(error, "cannot open catalog initrd: %s", g_strerror(errno));
    g_byte_array_unref(config);
    g_free(guest_init);
    return FALSE;
  }
  char *temporary_path = g_strdup_printf("%s.tmp.XXXXXX", output_path);
  int output_fd = g_mkstemp(temporary_path);
  if (output_fd < 0 || fchmod(output_fd, 0600) < 0 ||
      !copy_verified_file(base_fd, output_fd, base_initrd_digest,
                          INITRD_MAX_BASE_BYTES, error)) {
    if (output_fd >= 0) {
      close(output_fd);
      unlink(temporary_path);
    }
    close(base_fd);
    g_unlink(temporary_path);
    g_free(temporary_path);
    g_byte_array_unref(config);
    g_free(guest_init);
    if (error && !*error) {
      initrd_error(error, "cannot create temporary initramfs: %s",
                   g_strerror(errno));
    }
    return FALSE;
  }
  close(base_fd);
  struct archive *archive = archive_write_new();
  gboolean ok = archive &&
                archive_write_set_format_cpio_newc(archive) == ARCHIVE_OK &&
                lseek(output_fd, 0, SEEK_END) >= 0 &&
                archive_write_open_fd(archive, output_fd) == ARCHIVE_OK;
  if (!ok) {
    initrd_error(error, "could not initialize the Quocker initramfs archive");
  }
  if (ok) {
    ok = archive_write_entry(archive, "init", S_IFREG | 0755, guest_init,
                             guest_init_size, error) &&
         archive_write_entry(archive, "quocker", S_IFDIR | 0755, NULL, 0,
                             error) &&
         archive_write_entry(archive, "quocker/config", S_IFREG | 0400,
                             config->data, config->len, error);
  }
  if (archive) {
    int close_status = archive_write_close(archive);
    if (close_status != ARCHIVE_OK && ok) {
      initrd_error(error, "could not finish the Quocker initramfs archive");
      ok = FALSE;
    }
    archive_write_free(archive);
  }
  if (ok) {
    ok = fsync(output_fd) == 0;
    if (!ok) {
      initrd_error(error, "could not flush completed Quocker initramfs: %s",
                   g_strerror(errno));
    }
  }
  close(output_fd);
  if (ok && link(temporary_path, output_path) < 0) {
    initrd_error(error, errno == EEXIST
                            ? "Quocker initramfs output already exists"
                            : "could not publish completed Quocker initramfs");
    ok = FALSE;
  }
  g_unlink(temporary_path);
  g_free(temporary_path);
  g_byte_array_unref(config);
  g_free(guest_init);
  return ok;
}
