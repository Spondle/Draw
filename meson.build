project('paint', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

gnome = import('gnome')

gtk4_dep = dependency('gtk4', version: '>= 4.12')
adw_dep = dependency('libadwaita-1', version: '>= 1.6')
cairo_dep = dependency('cairo')
math_dep = meson.get_compiler('c').find_library('m', required: false)

resources = gnome.compile_resources('paint-resources',
  'data/paint.gresource.xml',
  source_dir: 'data',
)

core_sources = files(
  'src/paint-tools.c',
  'src/paint-canvas.c',
)

app_sources = core_sources + files(
  'src/main.c',
  'src/paint-application.c',
  'src/paint-window.c',
  'src/paint-canvas-page.c',
  'src/paint-preferences.c',
) + [resources]

executable('paint',
  app_sources,
  dependencies: [gtk4_dep, adw_dep, cairo_dep, math_dep],
  include_directories: include_directories('src'),
  install: true,
)

test_canvas = executable('test-canvas',
  core_sources + files('tests/test-canvas.c'),
  dependencies: [gtk4_dep, cairo_dep, math_dep],
  include_directories: include_directories('src'),
)

test('canvas-core', test_canvas)
