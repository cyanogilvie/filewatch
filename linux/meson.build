pkg_sources += files(
  'inotify.c',
)
teaconf_files += files(
  'queue.tcl',
  'watchdir.tcl.in',
)
conf.set10('BACKEND_INOTIFY', true)
conf.set('NS_DRIVER', '::inotify')
conf.set_quoted('NS_DRIVER_STR', '::inotify')
inc += include_directories('.')
