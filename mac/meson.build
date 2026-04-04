pkg_sources += files(
  'fsevents.c',
)
teaconf_files += files(
  'watcher_fsevents.tcl.in',
  'watchdir.tcl.in',
)
conf.set10('BACKEND_FSEVENTS', true)
conf.set('NS_DRIVER', '::' + meson.project_name() + '::fsevents')
conf.set_quoted('NS_DRIVER_STR', '::' + meson.project_name() + '::fsevents')
inc += include_directories('.')
if host_machine.system() == 'linux'
  pkg_sources += files('shim_inotify.c')
  conf.set10('SHIM_INOTIFY', true)
else
  deps += dependency('appleframeworks', modules: ['CoreServices'])
endif
