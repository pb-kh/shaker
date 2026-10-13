project(
  'shaker',
  'cpp',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.1.0',
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

# ---- Unit tests (constitution V + test gate) -------------------------------

# Build-time only dependency (constitution II: declared via dependency()).
# doctest is the header-only unit-test framework (R-02); never linked into
# the runtime binary. The TOML contract needs no dependency (R-01 revised).
# Lookup order: system doctest (pkg-config/CMake) first, then the WrapDB
# subproject by explicit name — newer Meson no longer resolves dependency()
# fallback wraps automatically. RPM packaging passes -Dtests=disabled, which
# skips the lookup entirely (no download needed inside %build); the CI jobs
# run the full test suite.
tests_opt = get_option('tests')
doctest_found = false
if not tests_opt.disabled()
  doctest_dep = dependency('doctest', static: true, required: false)
  if not doctest_dep.found()
    doctest_sp = subproject(
      'doctest',
      required: tests_opt,
      default_options: ['default_library=static'],
    )
    if doctest_sp.found()
      doctest_dep = doctest_sp.get_variable('doctest_dep')
    endif
  endif
  doctest_found = doctest_dep.found()
endif

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

# Internal core shared by the runtime binary and the unit tests, so tests
# exercise the exact translation units that ship in `shaker` (constitution V).
core_sources = [
  'src/detection.cpp',
  'src/config.cpp',
  'src/exec_command.cpp',
  'src/instance_lock.cpp',
]

shaker_core = static_library(
  'shaker_core',
  core_sources,
  include_directories: inc,
)

executable(
  'shaker',
  'src/main.cpp',
  link_with: shaker_core,
  dependencies: [libinput_dep, libudev_dep],
  include_directories: inc,
  cpp_args: cpp_args,
  install: true,
)

# Grants read access to mouse event devices (see udev/ and README).
install_data(
  'udev/99-shaker-mouse-access.rules',
  install_dir: get_option('udev_rules_dir'),
)

summary(
  {
    'libinput': libinput_dep.version(),
    'C++ standard': cpp_std,
  },
  section: 'Configuration',
)

# ---- Unit tests (constitution V + test gate) -------------------------------

# Register the test targets only when the doctest dependency could be
# satisfied (skipped entirely under -Dtests=disabled).
if doctest_found
  foreach test_case : [
    {'name': 'test_detection', 'sources': ['tests/test_detection.cpp']},
    {'name': 'test_config', 'sources': ['tests/test_config.cpp']},
    {'name': 'test_instance_lock', 'sources': ['tests/test_instance_lock.cpp']},
  ]
    exe = executable(
      test_case['name'],
      test_case['sources'],
      link_with: shaker_core,
      dependencies: [doctest_dep],
      include_directories: inc,
    )
    test(test_case['name'], exe)
  endforeach
endif
