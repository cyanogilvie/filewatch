project('filewatch', 'c',
  version:          '2.3',
  default_options:  ['c_std=c11', 'warning_level=3', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

add_project_arguments('-Wno-missing-field-initializers', language: 'c')

conf = configuration_data()

# Check for glibc and enable _GNU_SOURCE (needed for getaddrinfo_a)
is_glibc = cc.compiles('''
#include <features.h>
#if ! (defined __GLIBC__ || defined __GNU_LIBRARY__)
#  error "Not glibc"
#endif
''', name: 'glibc check')

if is_glibc
  add_project_arguments('-D_GNU_SOURCE', language: 'c')
endif

add_project_arguments('-DHAVE_CONFIG_H', language: 'c')

conf.set('NS', '::' + meson.project_name())
conf.set_quoted('NS_STR', '::' + meson.project_name())
conf.set10('HAVE_PIPE2', cc.has_function('pipe2', args: '-D_GNU_SOURCE', prefix: '#include <fcntl.h>\n#include <unistd.h>'))

deps = []
inc = [include_directories('generic')]
pkg_sources = []
teaconf_files = []

driver = get_option('driver')
if driver == 'native'
  driver = host_machine.kernel()
endif

if driver == 'linux'
  subdir('linux')
elif driver == 'nt'
  subdir('win')
elif driver == 'xnu'
  subdir('mac')
else
  error('No supported backend for this platform:', driver)
endif

subdir('generic')

subdir('teabase')
subdir('doc')
