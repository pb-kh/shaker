project(
  'shaker',
  'cpp',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.0.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

libinput_dep = dependency(
  'libinput',
  version: '>=1.24',
)

# Needed to obtain a struct udev context for the udev backend.
libudev_dep = dependency(
  'libudev',
)

cc = meson.get_compiler('cpp')

# Detect the C++ standard actually used, for a sanity check at build time.
cpp_std = get_option('cpp_std')
if not cc.compiles(
  'static_assert(__cplusplus >= 202302L, "C++23 required");',
  name: 'compiler supports ' + cpp_std,
)
  error('The selected compiler does not support C++23')
endif

inc = include_directories('include')

cpp_args = ['-DSHAKER_VERSION="' + meson.project_version() + '"']

executable(
  'shaker',
  'src/main.cpp',
  dependencies: [libinput_dep, libudev_dep],
  include_directories: inc,
  cpp_args: cpp_args,
  install: true,
)

summary(
  {
    'libinput': libinput_dep.version(),
    'C++ standard': cpp_std,
  },
  section: 'Configuration',
)
