/*
 * Tests for Quocker's platform-aware kernel catalog selection.
 *
 * Copyright (C) 2026 The Quocker Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../../scripts/quocker-kernel.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

static char *new_directory(void) {
  GError *error = NULL;
  char *path = g_dir_make_tmp("quocker-kernel-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(path);
  return path;
}

static char *sha256_file(const char *path) {
  gchar *contents = NULL;
  gsize size = 0;
  g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)contents, size);
  char *digest = g_strdup_printf("sha256:%s", g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  g_free(contents);
  return digest;
}

static char *write_catalog(const char *directory, const char *kernel_path,
                           const char *initrd_path) {
  char *kernel_digest = sha256_file(kernel_path);
  char *initrd_digest = sha256_file(initrd_path);
  char *contents = g_strdup_printf(
      "{\"version\":1,\"kernels\":["
      "{\"id\":\"generic\",\"os\":\"linux\","
      "\"architecture\":\"amd64\",\"kernel\":\"%s\","
      "\"kernel_sha256\":\"%s\",\"initrd\":\"%s\","
      "\"initrd_sha256\":\"%s\",\"priority\":100,"
      "\"version\":\"6.8.1\",\"features\":[\"virtio_blk\",\"virtio_net\"],"
      "\"module_releases\":[\"6.8.1-quocker\"]},"
      "{\"id\":\"rhel-family\",\"os\":\"linux\","
      "\"architecture\":\"x86_64\",\"kernel\":\"%s\","
      "\"kernel_sha256\":\"%s\",\"initrd\":\"%s\","
      "\"initrd_sha256\":\"%s\",\"distro_ids\":[\"rhel\"]},"
      "{\"id\":\"fedora\",\"os\":\"linux\","
      "\"architecture\":\"x86_64\",\"kernel\":\"%s\","
      "\"kernel_sha256\":\"%s\",\"initrd\":\"%s\","
      "\"initrd_sha256\":\"%s\",\"distro_ids\":[\"fedora\"]}]}\n",
      kernel_path, kernel_digest, initrd_path, initrd_digest, kernel_path,
      kernel_digest, initrd_path, initrd_digest, kernel_path, kernel_digest,
      initrd_path, initrd_digest);
  char *catalog = g_build_filename(directory, "kernels.json", NULL);
  g_assert_true(g_file_set_contents(catalog, contents, -1, NULL));
  g_free(contents);
  g_free(kernel_digest);
  g_free(initrd_digest);
  return catalog;
}

static char *sign_catalog(const char *directory, const char *catalog);
static void remove_signature_files(const char *catalog, char *public_key);

static void test_image_requirements_filter_catalog(void) {
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *catalog = write_catalog(directory, kernel_path, initrd_path);
  char *public_key = sign_catalog(directory, catalog);
  GPtrArray *features = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *module_releases = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *required_module_releases =
      g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(features, g_strdup("virtio_blk"));
  g_ptr_array_add(features, g_strdup("virtio_net"));
  g_ptr_array_add(module_releases, g_strdup("6.8.1-quocker"));
  g_ptr_array_add(required_module_releases, g_strdup("6.8.1-quocker"));
  QuockerDistroInfo distro = {.kernel_module_releases = module_releases};
  QuockerKernel *kernel = NULL;
  GError *error = NULL;
  g_assert_true(quocker_kernel_select_for_image(
      catalog, public_key, "linux/amd64", &distro, NULL, "6.8", features,
      required_module_releases, &kernel, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(kernel->id, ==, "generic");
  g_assert_cmpstr(kernel->version, ==, "6.8.1");
  g_assert_cmpuint(kernel->features->len, ==, 2);
  g_assert_cmpuint(kernel->module_releases->len, ==, 1);
  quocker_kernel_free(kernel);

  kernel = NULL;
  g_assert_false(quocker_kernel_select_for_image(
      catalog, public_key, "linux/amd64", &distro, NULL, "6.9", features,
      required_module_releases, &kernel, &error));
  g_assert_error(error, g_quark_from_static_string("quocker-kernel-error"), 1);
  g_clear_error(&error);
  g_assert_null(kernel);

  g_free(g_ptr_array_index(module_releases, 0));
  g_ptr_array_index(module_releases, 0) = g_strdup("6.8.1-other-kernel");
  g_assert_true(quocker_kernel_select_for_image(
      catalog, public_key, "linux/amd64", &distro, NULL, "6.8", features,
      required_module_releases, &kernel, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(kernel->id, ==, "generic");
  g_assert_true(kernel->module_release_evidence);
  g_assert_false(kernel->module_release_match);
  g_assert_nonnull(strstr(kernel->reason, "unmatched (advisory)"));
  quocker_kernel_free(kernel);

  g_free(g_ptr_array_index(required_module_releases, 0));
  g_ptr_array_index(required_module_releases, 0) =
      g_strdup("6.8.1-not-catalogued");
  kernel = NULL;
  g_assert_false(quocker_kernel_select_for_image(
      catalog, public_key, "linux/amd64", &distro, NULL, "6.8", features,
      required_module_releases, &kernel, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_null(kernel);

  g_ptr_array_free(features, TRUE);
  g_ptr_array_free(module_releases, TRUE);
  g_ptr_array_free(required_module_releases, TRUE);
  remove_signature_files(catalog, public_key);
  g_unlink(catalog);
  g_unlink(kernel_path);
  g_unlink(initrd_path);
  g_rmdir(directory);
  g_free(catalog);
  g_free(kernel_path);
  g_free(initrd_path);
  g_free(directory);
}

static char *sign_catalog(const char *directory, const char *catalog) {
  EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  EVP_PKEY *key = NULL;
  g_assert_nonnull(key_context);
  g_assert_cmpint(EVP_PKEY_keygen_init(key_context), ==, 1);
  g_assert_cmpint(EVP_PKEY_keygen(key_context, &key), ==, 1);
  EVP_PKEY_CTX_free(key_context);

  char *public_key = g_build_filename(directory, "kernel-catalog.pub", NULL);
  BIO *key_bio = BIO_new_file(public_key, "w");
  g_assert_nonnull(key_bio);
  g_assert_cmpint(PEM_write_bio_PUBKEY(key_bio, key), ==, 1);
  BIO_free(key_bio);

  gchar *contents = NULL;
  gsize contents_size = 0;
  g_assert_true(g_file_get_contents(catalog, &contents, &contents_size, NULL));
  EVP_MD_CTX *sign_context = EVP_MD_CTX_new();
  g_assert_nonnull(sign_context);
  g_assert_cmpint(EVP_DigestSignInit(sign_context, NULL, NULL, NULL, key), ==,
                  1);
  size_t signature_size = 0;
  g_assert_cmpint(EVP_DigestSign(sign_context, NULL, &signature_size,
                                 (const unsigned char *)contents,
                                 contents_size),
                  ==, 1);
  guchar *signature = g_malloc(signature_size);
  g_assert_cmpint(EVP_DigestSign(sign_context, signature, &signature_size,
                                 (const unsigned char *)contents,
                                 contents_size),
                  ==, 1);
  char *encoded = g_base64_encode(signature, signature_size);
  char *signature_path = g_strconcat(catalog, ".sig", NULL);
  char *signature_file = g_strconcat(encoded, "\n", NULL);
  g_assert_true(g_file_set_contents(signature_path, signature_file, -1, NULL));

  g_free(signature_file);
  g_free(signature_path);
  g_free(encoded);
  g_free(signature);
  g_free(contents);
  EVP_MD_CTX_free(sign_context);
  EVP_PKEY_free(key);
  return public_key;
}

static void remove_signature_files(const char *catalog, char *public_key) {
  char *signature_path = g_strconcat(catalog, ".sig", NULL);
  g_assert_cmpint(g_unlink(signature_path), ==, 0);
  g_assert_cmpint(g_unlink(public_key), ==, 0);
  g_free(signature_path);
  g_free(public_key);
}

static void test_distro_match_precedes_generic_priority(void) {
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *catalog = write_catalog(directory, kernel_path, initrd_path);
  char *public_key = sign_catalog(directory, catalog);
  QuockerDistroInfo distro = {.id = g_strdup("fedora"),
                              .id_like = g_strdup("rhel fedora"),
                              .version_id = g_strdup("42")};
  QuockerKernel *kernel = NULL;
  GError *error = NULL;
  g_assert_true(quocker_kernel_select(catalog, public_key, "linux/amd64",
                                      &distro, NULL, &kernel, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(kernel->id, ==, "fedora");
  g_assert_cmpstr(kernel->architecture, ==, "x86_64");
  g_assert_nonnull(strstr(kernel->reason, "exact guest distro match"));
  g_assert_false(kernel->module_release_evidence);
  g_assert_cmpstr(kernel->initrd_path, ==, initrd_path);
  quocker_kernel_free(kernel);
  g_free(distro.id);
  g_free(distro.id_like);
  g_free(distro.version_id);
  remove_signature_files(catalog, public_key);
  g_assert_cmpint(g_unlink(catalog), ==, 0);
  g_assert_cmpint(g_unlink(kernel_path), ==, 0);
  g_assert_cmpint(g_unlink(initrd_path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(catalog);
  g_free(kernel_path);
  g_free(initrd_path);
  g_free(directory);
}

static void test_family_fallback_and_explicit_pin(void) {
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *catalog = write_catalog(directory, kernel_path, initrd_path);
  char *public_key = sign_catalog(directory, catalog);
  QuockerDistroInfo distro = {.id = g_strdup("centos"),
                              .id_like = g_strdup("rhel")};
  QuockerKernel *kernel = NULL;
  GError *error = NULL;
  g_assert_true(quocker_kernel_select(catalog, public_key, "linux/x86_64",
                                      &distro, NULL, &kernel, &error));
  g_assert_cmpstr(kernel->id, ==, "rhel-family");
  g_assert_cmpstr(kernel->reason, ==, "guest distro-family match");
  quocker_kernel_free(kernel);

  kernel = NULL;
  g_assert_true(quocker_kernel_select(catalog, public_key, "linux/amd64",
                                      &distro, "generic", &kernel, &error));
  g_assert_cmpstr(kernel->id, ==, "generic");
  g_assert_cmpstr(kernel->reason, ==, "explicit kernel selection");
  quocker_kernel_free(kernel);
  g_free(distro.id);
  g_free(distro.id_like);
  g_assert_no_error(error);
  remove_signature_files(catalog, public_key);
  g_assert_cmpint(g_unlink(catalog), ==, 0);
  g_assert_cmpint(g_unlink(kernel_path), ==, 0);
  g_assert_cmpint(g_unlink(initrd_path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(catalog);
  g_free(kernel_path);
  g_free(initrd_path);
  g_free(directory);
}

static void test_platform_and_digest_must_match(void) {
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *catalog = write_catalog(directory, kernel_path, initrd_path);
  char *public_key = sign_catalog(directory, catalog);
  QuockerKernel *kernel = NULL;
  GError *error = NULL;
  g_assert_false(quocker_kernel_select(catalog, public_key, "linux/arm64", NULL,
                                       NULL, &kernel, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  g_assert_true(g_file_set_contents(kernel_path, "tampered", -1, NULL));
  g_assert_false(quocker_kernel_select(catalog, public_key, "linux/amd64", NULL,
                                       NULL, &kernel, &error));
  g_assert_nonnull(error);
  g_assert_nonnull(strstr(error->message, "digest mismatch"));
  g_clear_error(&error);
  g_assert_true(g_file_set_contents(catalog, "{}\n", -1, NULL));
  g_assert_false(quocker_kernel_select(catalog, public_key, "linux/amd64", NULL,
                                       NULL, &kernel, &error));
  g_assert_nonnull(error);
  g_assert_nonnull(strstr(error->message, "signature verification failed"));
  g_clear_error(&error);
  remove_signature_files(catalog, public_key);
  g_assert_cmpint(g_unlink(catalog), ==, 0);
  g_assert_cmpint(g_unlink(kernel_path), ==, 0);
  g_assert_cmpint(g_unlink(initrd_path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(catalog);
  g_free(kernel_path);
  g_free(initrd_path);
  g_free(directory);
}

static void test_signed_catalog_install_and_remote_assets(void) {
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *kernel_digest = sha256_file(kernel_path);
  char *initrd_digest = sha256_file(initrd_path);
  char *contents = g_strdup_printf(
      "{\"version\":1,\"kernels\":[{\"id\":\"remote-generic\","
      "\"os\":\"linux\",\"architecture\":\"amd64\","
      "\"kernel_url\":\"https://assets.example.invalid/kernel\","
      "\"kernel_sha256\":\"%s\",\"kernel_size\":12,"
      "\"initrd_url\":\"https://assets.example.invalid/initrd\","
      "\"initrd_sha256\":\"%s\",\"initrd_size\":12,"
      "\"module_releases\":[\"6.8.1-quocker\"]}]}\n",
      kernel_digest, initrd_digest);
  char *source_catalog = g_build_filename(directory, "source.json", NULL);
  g_assert_true(g_file_set_contents(source_catalog, contents, -1, NULL));
  char *public_key = sign_catalog(directory, source_catalog);
  char *source_signature_path = g_strconcat(source_catalog, ".sig", NULL);
  gchar *signature = NULL;
  gsize signature_size = 0;
  g_assert_true(g_file_get_contents(source_signature_path, &signature,
                                    &signature_size, NULL));
  char *installed_directory = g_build_filename(directory, "trusted", NULL);
  char *installed_catalog =
      g_build_filename(installed_directory, "kernels.json", NULL);
  GError *error = NULL;
  g_assert_true(quocker_kernel_catalog_install_signed(
      contents, strlen(contents), signature, signature_size, installed_catalog,
      public_key, &error));
  g_assert_no_error(error);
  gchar *installed_contents = NULL;
  g_assert_true(g_file_get_contents(installed_catalog, &installed_contents,
                                    NULL, NULL));
  g_assert_cmpstr(installed_contents, ==, contents);
  g_free(installed_contents);

  QuockerKernel *kernel = NULL;
  g_assert_true(quocker_kernel_select(installed_catalog, public_key,
                                      "linux/amd64", NULL, NULL, &kernel,
                                      &error));
  g_assert_no_error(error);
  g_assert_cmpstr(kernel->id, ==, "remote-generic");
  g_assert_false(kernel->assets_verified);
  g_assert_true(g_str_has_prefix(kernel->kernel_path,
                                 g_get_user_cache_dir()));
  quocker_kernel_free(kernel);

  char *bad_signature = g_strdup(signature);
  bad_signature[0] = bad_signature[0] == 'A' ? 'B' : 'A';
  g_assert_false(quocker_kernel_catalog_install_signed(
      contents, strlen(contents), bad_signature, strlen(bad_signature),
      installed_catalog, public_key, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_true(g_file_get_contents(installed_catalog, &installed_contents,
                                    NULL, NULL));
  g_assert_cmpstr(installed_contents, ==, contents);
  g_free(installed_contents);

  g_assert_false(quocker_kernel_catalog_update(
      "http://catalog.example.invalid/kernels.json", installed_catalog,
      public_key, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);

  g_free(bad_signature);
  g_free(signature);
  g_free(source_signature_path);
  char *installed_signature = g_strconcat(installed_catalog, ".sig", NULL);
  g_assert_cmpint(g_unlink(installed_signature), ==, 0);
  g_assert_cmpint(g_unlink(installed_catalog), ==, 0);
  g_assert_cmpint(g_rmdir(installed_directory), ==, 0);
  g_free(installed_signature);
  g_free(installed_catalog);
  g_free(installed_directory);
  remove_signature_files(source_catalog, public_key);
  g_assert_cmpint(g_unlink(source_catalog), ==, 0);
  g_free(source_catalog);
  g_free(contents);
  g_free(kernel_digest);
  g_free(initrd_digest);
  g_assert_cmpint(g_unlink(kernel_path), ==, 0);
  g_assert_cmpint(g_unlink(initrd_path), ==, 0);
  g_free(kernel_path);
  g_free(initrd_path);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(directory);
}

static void test_kernel_select_cli_reports_verified_assets(void) {
  const char *cli = g_getenv("QUOCKER_CLI");
  g_assert_nonnull(cli);
  char *directory = new_directory();
  char *kernel_path = g_build_filename(directory, "vmlinuz", NULL);
  char *initrd_path = g_build_filename(directory, "initrd", NULL);
  g_assert_true(g_file_set_contents(kernel_path, "kernel bytes", -1, NULL));
  g_assert_true(g_file_set_contents(initrd_path, "initrd bytes", -1, NULL));
  char *catalog = write_catalog(directory, kernel_path, initrd_path);
  char *public_key = sign_catalog(directory, catalog);
  char *rootfs = g_build_filename(directory, "guest", NULL);
  char *usr = g_build_filename(rootfs, "usr", NULL);
  char *library = g_build_filename(usr, "lib", NULL);
  char *modules = g_build_filename(library, "modules", NULL);
  char *release = g_build_filename(modules, "6.8.1-quocker", NULL);
  g_assert_cmpint(g_mkdir(rootfs, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(usr, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(library, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(modules, 0700), ==, 0);
  g_assert_cmpint(g_mkdir(release, 0700), ==, 0);
  const char *arguments[] = {
      cli,          "kernel",      "select",   "--catalog", catalog,
      "--platform", "linux/amd64", "--pubkey", public_key,  "--rootfs",
      rootfs,       "--id",        "generic",  "--minimum", "6.8",
      "--require",  "virtio_blk",  NULL};
  char *stdout_text = NULL;
  char *stderr_text = NULL;
  int wait_status = 0;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, (char **)arguments, NULL, 0, NULL, NULL,
                             &stdout_text, &stderr_text, &wait_status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(wait_status, &error));
  g_assert_no_error(error);
  g_assert_nonnull(strstr(stdout_text, "Kernel: generic"));
  g_assert_nonnull(strstr(stdout_text, "Reason: explicit kernel selection"));
  g_assert_nonnull(strstr(stdout_text, "Version: 6.8.1"));
  g_assert_nonnull(strstr(stdout_text, "Features: virtio_blk virtio_net"));
  g_assert_nonnull(strstr(stdout_text,
                          "Module evidence: matched a catalog release "
                          "(advisory)"));
  g_assert_nonnull(strstr(stdout_text, "Observed module releases: 6.8.1-quocker"));
  g_assert_nonnull(strstr(stdout_text, kernel_path));
  g_assert_nonnull(strstr(stdout_text, initrd_path));
  g_free(stdout_text);
  g_free(stderr_text);
  const char *json_arguments[] = {
      cli,          "kernel",      "select",   "--catalog", catalog,
      "--platform", "linux/amd64", "--pubkey", public_key,  "--rootfs",
      rootfs,       "--id",        "generic",  "--minimum", "6.8",
      "--require",  "virtio_blk", "--module-release",
      "6.8.1-quocker", "--json", NULL};
  stdout_text = NULL;
  stderr_text = NULL;
  wait_status = 0;
  g_assert_true(g_spawn_sync(NULL, (char **)json_arguments, NULL, 0, NULL, NULL,
                             &stdout_text, &stderr_text, &wait_status, &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(wait_status, &error));
  g_assert_no_error(error);
  JsonParser *json_parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(json_parser, stdout_text, -1, &error));
  g_assert_no_error(error);
  JsonObject *report = json_node_get_object(json_parser_get_root(json_parser));
  JsonObject *selection = json_object_get_object_member(report, "selection");
  JsonObject *module_evidence =
      json_object_get_object_member(report, "module_evidence");
  g_assert_cmpstr(json_object_get_string_member(selection, "id"), ==,
                  "generic");
  g_assert_true(json_object_get_boolean_member(module_evidence, "detected"));
  g_assert_true(json_object_get_boolean_member(module_evidence, "matched"));
  g_assert_true(json_object_get_boolean_member(module_evidence, "advisory"));
  g_assert_cmpstr(json_array_get_string_element(
                      json_object_get_array_member(
                          json_object_get_object_member(report, "requirements"),
                          "required_module_releases"),
                      0),
                  ==, "6.8.1-quocker");
  g_assert_cmpstr(json_array_get_string_element(
                      json_object_get_array_member(module_evidence,
                                                   "observed_releases"),
                      0),
                  ==, "6.8.1-quocker");
  g_object_unref(json_parser);
  g_free(stdout_text);
  g_free(stderr_text);
  g_assert_cmpint(g_rmdir(release), ==, 0);
  g_assert_cmpint(g_rmdir(modules), ==, 0);
  g_assert_cmpint(g_rmdir(library), ==, 0);
  g_assert_cmpint(g_rmdir(usr), ==, 0);
  g_assert_cmpint(g_rmdir(rootfs), ==, 0);
  remove_signature_files(catalog, public_key);
  g_assert_cmpint(g_unlink(catalog), ==, 0);
  g_assert_cmpint(g_unlink(kernel_path), ==, 0);
  g_assert_cmpint(g_unlink(initrd_path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
  g_free(release);
  g_free(modules);
  g_free(library);
  g_free(usr);
  g_free(rootfs);
  g_free(catalog);
  g_free(kernel_path);
  g_free(initrd_path);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/quocker/kernel/distro-priority",
                  test_distro_match_precedes_generic_priority);
  g_test_add_func("/quocker/kernel/family-and-pin",
                  test_family_fallback_and_explicit_pin);
  g_test_add_func("/quocker/kernel/image-requirements",
                  test_image_requirements_filter_catalog);
  g_test_add_func("/quocker/kernel/platform-and-digest",
                  test_platform_and_digest_must_match);
  g_test_add_func("/quocker/kernel/signed-catalog-install",
                  test_signed_catalog_install_and_remote_assets);
  g_test_add_func("/quocker/kernel/cli-output",
                  test_kernel_select_cli_reports_verified_assets);
  return g_test_run();
}
