
test_names = [
  'errors',
  'fdio',
  'macros',
  'shutil',
  'xattrs',
]

foreach test_name : test_names
  exe = executable(test_name,
    ['test-libglnx-' + test_name + '.c', 'libglnx-testlib.h'],
    dependencies: [
      libglnx_dep,
      libglnx_deps,
    ],
  )
  test(test_name, exe)
endforeach
