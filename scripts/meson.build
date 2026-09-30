if stap.found()
  install_data('qemu-trace-stap', install_dir: get_option('bindir'))
endif

# Quocker is an optional, Compose-style interface for managing QEMU VMs.
if not get_option('quocker').disabled()
  quocker_yaml = dependency('yaml-0.1', required: get_option('quocker'),
                            method: 'pkg-config')
  quocker_curl = dependency('libcurl', required: get_option('quocker'),
                            method: 'pkg-config')
  quocker_json = dependency('json-glib-1.0', required: get_option('quocker'),
                            method: 'pkg-config')
  quocker_archive = dependency('libarchive', required: get_option('quocker'),
                               method: 'pkg-config')
  quocker_crypto = dependency('openssl', required: get_option('quocker'),
                              method: 'pkg-config')
  quocker_ext2fs = dependency('ext2fs', required: get_option('quocker'),
                              method: 'pkg-config')
  if quocker_yaml.found() and quocker_curl.found() and quocker_json.found() and \
     quocker_archive.found() and quocker_crypto.found() and quocker_ext2fs.found()
    quocker_schema = configure_file(
      input: '../docs/compose-spec/compose-spec.json',
      output: 'quocker-compose-spec.json',
      copy: true,
      install: true,
      install_dir: get_option('datadir') / 'quocker')
    quocker_schema_build_path = meson.current_build_dir() / \
      'quocker-compose-spec.json'
    quocker_schema_install_path = get_option('prefix') / \
      get_option('datadir') / 'quocker' / 'compose-spec.json'
    quocker_guest_init = executable(
      'quocker-guest-init', 'quocker-init.c', link_args: ['-static'],
      install: true, install_dir: get_option('libexecdir') / 'quocker')
    quocker = executable('quocker', ['quocker.c', 'quocker-compose-schema.c',
                                    'quocker-oci.c',
                                    'quocker-rootfs.c', 'quocker-kernel.c',
                                    'quocker-image.c', 'quocker-disk.c',
                                    'quocker-initrd.c', 'quocker-env.c',
                                    'quocker-registry-auth.c',
                                    'quocker-registry-config.c',
                                    'quocker-registry-http.c'],
                         dependencies: [glib, quocker_yaml, quocker_curl,
                                       quocker_json, quocker_archive,
                                       quocker_crypto, quocker_ext2fs],
                         c_args: [
                           '-DQUOCKER_SCHEMA_BUILD_PATH="' + quocker_schema_build_path + '"',
                           '-DQUOCKER_SCHEMA_INSTALL_PATH="' + quocker_schema_install_path + '"',
                         ],
                         install: true)

    test('quocker-help', quocker, args: ['--help'], suite: ['quocker'])
    test('quocker-config-valid', quocker,
         args: ['-f', files('../tests/quocker/valid.yaml'), 'config', '--quiet'],
         suite: ['quocker'])
    test('quocker-config-render', quocker,
         args: ['-f', files('../tests/quocker/valid.yaml'), 'config'],
         suite: ['quocker'])
    quocker_config_json_test = executable(
      'quocker-config-json-test', files('../tests/quocker/config-json-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-config-json', quocker_config_json_test,
         args: [files('../tests/quocker/config-json.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    quocker_port_test = executable(
      'quocker-port-test', files('../tests/quocker/port-test.c'),
      dependencies: [glib])
    test('quocker-port-query', quocker_port_test,
         args: [files('../tests/quocker/ports-query.yaml'),
                files('../tests/quocker/ports-dynamic.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    test('quocker-config-merge', quocker,
         args: ['-f', files('../tests/quocker/base.yaml'),
                '-f', files('../tests/quocker/override.yaml'), 'config', '--quiet'],
         suite: ['quocker'])
    quocker_merge_unique_test = executable(
      'quocker-merge-unique-test',
      files('../tests/quocker/merge-unique-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-config-unique-merge', quocker_merge_unique_test,
         args: [files('../tests/quocker/merge-base.yaml'),
                files('../tests/quocker/merge-override.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    quocker_yaml_anchors_test = executable(
      'quocker-yaml-anchors-test',
      files('../tests/quocker/yaml-anchors-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-yaml-anchors', quocker_yaml_anchors_test,
         args: [files('../tests/quocker/yaml-anchors.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    quocker_compose_include_test = executable(
      'quocker-compose-include-test',
      files('../tests/quocker/compose-include-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-compose-include', quocker_compose_include_test,
         args: [files('../tests/quocker/includes/project.yaml'),
                files('../tests/quocker/includes/override.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path(),
               'INCLUDED_IMAGE=./parent-override.qcow2'],
         depends: [quocker],
         suite: ['quocker'])
    quocker_extends_test = executable(
      'quocker-extends-test', files('../tests/quocker/extends-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-compose-extends', quocker_extends_test,
         args: [files('../tests/quocker/extends.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    quocker_config_list_test = executable(
      'quocker-config-list-test', files('../tests/quocker/config-list-test.c'),
      dependencies: [glib])
    test('quocker-config-list-options', quocker_config_list_test,
         args: [files('../tests/quocker/config-list.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    test('quocker-config-list-invalid-combination', quocker,
         args: ['-f', files('../tests/quocker/config-list.yaml'), 'config',
                '--services', '--format', 'yaml'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-env-file-list', quocker,
         args: ['config', '--quiet'],
         env: ['COMPOSE_FILE=../tests/quocker/base.yaml|../tests/quocker/override.yaml',
               'COMPOSE_PATH_SEPARATOR=|'],
         suite: ['quocker'])
    test('quocker-config-nested-interpolation', quocker,
         args: ['-f', files('../tests/quocker/nested-interpolation.yaml'),
                'config', '--quiet'],
         suite: ['quocker'])
    test('quocker-config-values-only-interpolation', quocker,
         args: ['-f', files('../tests/quocker/interpolation-values-only.yaml'),
                'config', '--quiet'],
         suite: ['quocker'])
    test('quocker-config-unset-interpolation', quocker,
         args: ['-f', files('../tests/quocker/unset-interpolation.yaml'),
                'config', '--quiet'],
         suite: ['quocker'])
    quocker_env_test = executable('quocker-env-test',
                                  files('../tests/quocker/env-test.c'),
                                  dependencies: [glib])
    test('quocker-env-files', quocker_env_test,
         args: [files('../tests/quocker/env-compose.yaml'),
                'env-base.env', 'env-override.env'],
         env: ['QUOCKER_CLI=' + quocker.full_path()],
         depends: [quocker], suite: ['quocker'])
    quocker_env_unit_test = executable(
      'quocker-env-unit-test',
      [files('../tests/quocker/env-unit-test.c'), 'quocker-env.c'],
      dependencies: [glib])
    test('quocker-env-parser', quocker_env_unit_test, suite: ['quocker'])
    test('quocker-env-files-environment', quocker,
         args: ['-f', files('../tests/quocker/env-default-compose.yaml'),
                'config', '--quiet'],
         env: ['COMPOSE_ENV_FILES=' + (meson.current_source_dir() / '../tests/quocker/env-default.env'),
               'COMPOSE_DISABLE_ENV_FILE=1'],
         suite: ['quocker'])
    test('quocker-env-file-invalid', quocker,
         args: ['-f', files('../tests/quocker/env-compose.yaml'),
                '--env-file', files('../tests/quocker/env-invalid.env'),
                'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-env-file-missing', quocker,
         args: ['-f', files('../tests/quocker/env-compose.yaml'),
                '--env-file', 'missing.env', 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-invalid', quocker,
         args: ['-f', files('../tests/quocker/invalid.yaml'), 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-schema-unknown-service-field', quocker,
         args: ['-f', files('../tests/quocker/schema-unknown-service.yaml'),
                'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-schema-unknown-top-level-field', quocker,
         args: ['-f', files('../tests/quocker/schema-unknown-top-level.yaml'),
                'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-schema-invalid-type', quocker,
         args: ['-f', files('../tests/quocker/schema-invalid-type.yaml'),
                'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-schema-fails-closed-on-new-keyword', quocker,
         args: ['-f', files('../tests/quocker/valid.yaml'), 'config',
                '--quiet'],
         env: ['QUOCKER_COMPOSE_SCHEMA=' +
               (meson.current_source_dir() /
                '../tests/quocker/schema-unsupported-keyword.json')],
         should_fail: true, suite: ['quocker'])
    test('quocker-project-name-invalid-explicit', quocker,
         args: ['-f', files('../tests/quocker/project-name.yaml'), '-p',
                '../unsafe', 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    quocker_project_name_test = executable(
      'quocker-project-name-test',
      files('../tests/quocker/project-name-test.c'),
      dependencies: [glib, quocker_json])
    test('quocker-project-name', quocker_project_name_test,
         args: [files('../tests/quocker/project-name.yaml'),
                files('../tests/quocker/Project.Name/compose.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    test('quocker-dependencies-cycle', quocker,
         args: ['-f', files('../tests/quocker/dependencies-cycle.yaml'), 'up'],
         should_fail: true, suite: ['quocker'])
    test('quocker-dependencies-missing', quocker,
         args: ['-f', files('../tests/quocker/dependencies-missing.yaml'), 'up'],
         should_fail: true, suite: ['quocker'])
    test('quocker-ports-invalid-host-ip', quocker,
         args: ['-f', files('../tests/quocker/ports-invalid-hostip.yaml'),
                'up', '--dry-run'],
         should_fail: true, suite: ['quocker'])
    test('quocker-ports-ipv6-host-ip', quocker,
         args: ['-f', files('../tests/quocker/ports-ipv6-hostip.yaml'),
                'up', '--dry-run'],
         should_fail: true, suite: ['quocker'])
    test('quocker-ports-conflict', quocker,
         args: ['-f', files('../tests/quocker/ports-conflict.yaml'),
                'up', '--dry-run'],
         should_fail: true, suite: ['quocker'])
    test('quocker-ports-dynamic-dry-run', quocker,
         args: ['-f', files('../tests/quocker/ports-dynamic.yaml'), 'up',
                '--dry-run'], suite: ['quocker'])
    test('quocker-dependencies-health-unsupported', quocker,
         args: ['-f', files('../tests/quocker/dependencies-health.yaml'), 'up'],
         should_fail: true, suite: ['quocker'])
    quocker_dependencies_test = executable(
      'quocker-dependencies-test',
      files('../tests/quocker/dependencies-test.c'), dependencies: [glib])
    test('quocker-dependencies-dry-run', quocker_dependencies_test,
         args: [files('../tests/quocker/dependencies-order.yaml')],
         env: ['QUOCKER_CLI=' + quocker.full_path()], depends: [quocker],
         suite: ['quocker'])
    test('quocker-config-duplicate-key', quocker,
         args: ['-f', files('../tests/quocker/duplicate-key.yaml'), 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-multiple-documents', quocker,
         args: ['-f', files('../tests/quocker/multiple-documents.yaml'), 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-empty-document', quocker,
         args: ['-f', files('../tests/quocker/empty-document.yaml'), 'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-invalid-format', quocker,
         args: ['-f', files('../tests/quocker/valid.yaml'), 'config',
                '--format', 'toml'],
         should_fail: true, suite: ['quocker'])
    test('quocker-config-nested-required-interpolation', quocker,
         args: ['-f', files('../tests/quocker/nested-required-interpolation.yaml'),
                'config', '--quiet'],
         should_fail: true, suite: ['quocker'])
    test('quocker-pull-invalid-platform', quocker,
         args: ['-f', files('../tests/quocker/invalid-platform.yaml'), 'pull'],
         should_fail: true, suite: ['quocker'])

    quocker_rootfs_test = executable(
      'quocker-rootfs-test',
      [files('../tests/quocker/rootfs-test.c'), 'quocker-oci.c',
       'quocker-registry-auth.c', 'quocker-registry-config.c',
       'quocker-registry-http.c', 'quocker-rootfs.c', 'quocker-image.c'],
      dependencies: [glib, quocker_curl, quocker_json, quocker_archive])
    test('quocker-rootfs', quocker_rootfs_test, suite: ['quocker'])

    quocker_test_credential_helper = executable(
      'docker-credential-quocker-test',
      files('../tests/quocker/fake-credential-helper.c'))
    quocker_registry_auth_test = executable(
      'quocker-registry-auth-test',
      [files('../tests/quocker/registry-auth-test.c'),
       'quocker-registry-auth.c'], dependencies: [glib, quocker_json])
    test('quocker-registry-auth', quocker_registry_auth_test,
         env: ['QUOCKER_TEST_HELPER_DIR=' + meson.current_build_dir()],
         depends: [quocker_test_credential_helper], suite: ['quocker'])
    quocker_registry_config_test = executable(
      'quocker-registry-config-test',
      [files('../tests/quocker/registry-config-test.c'),
       'quocker-registry-config.c'], dependencies: [glib, quocker_json])
    test('quocker-registry-config', quocker_registry_config_test,
         suite: ['quocker'])
    quocker_registry_http_test = executable(
      'quocker-registry-http-test',
      [files('../tests/quocker/registry-http-test.c'),
       'quocker-registry-http.c'], dependencies: [glib, quocker_curl])
    test('quocker-registry-http', quocker_registry_http_test,
         suite: ['quocker'])

    quocker_fake_qemu = executable(
      'quocker-fake-qemu', files('../tests/quocker/fake-qemu-process.c'))
    quocker_fake_qemu_img = executable(
      'quocker-fake-qemu-img', files('../tests/quocker/fake-qemu-img.c'))
    quocker_lifecycle_test = executable(
      'quocker-lifecycle-test', files('../tests/quocker/lifecycle-test.c'),
      dependencies: [glib])
    test('quocker-lifecycle-stop', quocker_lifecycle_test,
         env: ['QUOCKER_CLI=' + quocker.full_path(),
               'QUOCKER_FAKE_QEMU=' + quocker_fake_qemu.full_path(),
               'QUOCKER_FAKE_QEMU_IMG=' + quocker_fake_qemu_img.full_path()],
         depends: [quocker, quocker_fake_qemu, quocker_fake_qemu_img],
         suite: ['quocker'])

    quocker_kernel_test = executable(
      'quocker-kernel-test',
      [files('../tests/quocker/kernel-test.c'), 'quocker-kernel.c'],
      dependencies: [glib, quocker_json, quocker_crypto, quocker_curl])
    test('quocker-kernel', quocker_kernel_test,
         env: ['QUOCKER_CLI=' + quocker.full_path()], suite: ['quocker'])

    quocker_image_test = executable(
      'quocker-image-test',
      [files('../tests/quocker/image-test.c'), 'quocker-image.c'],
      dependencies: [glib, quocker_json])
    test('quocker-image', quocker_image_test, suite: ['quocker'])

    quocker_disk_test = executable(
      'quocker-disk-test',
      [files('../tests/quocker/disk-test.c'), 'quocker-disk.c'],
      dependencies: [glib, quocker_ext2fs])
    test('quocker-disk', quocker_disk_test, suite: ['quocker'])

    quocker_initrd_test = executable(
      'quocker-initrd-test',
      [files('../tests/quocker/initrd-test.c'), 'quocker-initrd.c',
       'quocker-image.c'],
      dependencies: [glib, quocker_json, quocker_archive, quocker_crypto])
    test('quocker-initrd', quocker_initrd_test,
         env: ['QUOCKER_GUEST_INIT=' + quocker_guest_init.full_path()],
         depends: [quocker_guest_init],
         suite: ['quocker'])
  endif
endif

test('xml-preprocess', files('xml-preprocess-test.py'), suite: ['unit'])
