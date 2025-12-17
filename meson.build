project('ovpn-manager', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c99',
    'warning_level=2',
    'werror=false',
  ],
  license: 'MIT',
  meson_version: '>= 0.60.0'
)

# Project information
project_name = meson.project_name()
project_version = meson.project_version()

# Compiler
cc = meson.get_compiler('c')

# Build configuration
conf_data = configuration_data()
conf_data.set_quoted('PROJECT_NAME', project_name)
conf_data.set_quoted('PROJECT_VERSION', project_version)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.58')
gio_dep = dependency('gio-2.0', version: '>= 2.58')
libsystemd_dep = dependency('libsystemd', version: '>= 239')
gtk_dep = dependency('gtk+-3.0', version: '>= 3.22')
libnotify_dep = dependency('libnotify', version: '>= 0.7.0', required: false)
appindicator_dep = dependency('ayatana-appindicator3-0.1',
  required: false)

# Fallback to older appindicator if ayatana not found
if not appindicator_dep.found()
  appindicator_dep = dependency('appindicator3-0.1',
    required: false)
endif

# Math library (for bandwidth calculations)
math_dep = cc.find_library('m', required: false)

# Thread support
thread_dep = dependency('threads')

# Collect all dependencies
deps = [
  glib_dep,
  gio_dep,
  libsystemd_dep,
  gtk_dep,
  libnotify_dep,
  appindicator_dep,
  math_dep,
  thread_dep,
]

# Compiler flags
add_project_arguments(
  cc.get_supported_arguments([
    '-D_GNU_SOURCE',
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
  ]),
  language: 'c'
)

# Build type specific flags
if get_option('buildtype') == 'release'
  add_project_arguments(
    cc.get_supported_arguments([
      '-ffunction-sections',
      '-fdata-sections',
    ]),
    language: 'c'
  )

  add_project_link_arguments(
    cc.get_supported_link_arguments([
      '-Wl,--gc-sections',
      '-Wl,--strip-all',
    ]),
    language: 'c'
  )
endif

# Include directories
inc = include_directories('include', 'vendor')

# Subdirectories
subdir('src')

# Installation
install_data(
  'data/ovpn-manager.desktop',
  install_dir: join_paths(get_option('sysconfdir'), 'xdg', 'autostart')
)

# Summary
summary({
  'prefix': get_option('prefix'),
  'bindir': get_option('bindir'),
  'sysconfdir': get_option('sysconfdir'),
  'buildtype': get_option('buildtype'),
}, section: 'Directories')

summary({
  'GLib': glib_dep.version(),
  'libsystemd': libsystemd_dep.version(),
  'libnotify': libnotify_dep.found() ? libnotify_dep.version() : 'not found',
  'appindicator': appindicator_dep.found() ? 'yes' : 'no',
}, section: 'Dependencies')
