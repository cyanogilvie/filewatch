pkg_sources += files(
  'dirwatch.c',
)
teaconf_files += files(
  'watcher_dirwatch.tcl.in',
  'watchdir.tcl.in',
)
conf.set10('BACKEND_DIRWATCH', true)
conf.set('NS_DRIVER', '::' + meson.project_name() + '::dirwatch')
conf.set_quoted('NS_DRIVER_STR', '::' + meson.project_name() + '::dirwatch')
inc += include_directories('.')
if host_machine.system() == 'linux'
  pkg_sources += files('shim_inotify.c')
  conf.set10('SHIM_INOTIFY', true)
endif
