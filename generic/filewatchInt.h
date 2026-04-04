#ifndef FILEWATCHINT_H
#define FILEWATCHINT_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <tclstuff.h>

#ifndef INT2PTR
#define INT2PTR(p) ((void *)(intptr_t)(p))
#define PTR2INT(p) ((intptr_t)(p))
#endif

extern int pkgdir_path(Tcl_Interp* interp, const char* tail, Tcl_Obj** res);

#if BACKEND_INOTIFY
# include <backend_inotify.h>
#elif BACKEND_FSEVENTS
# include <backend_fsevents.h>
#elif BACKEND_DIRWATCH
# include <backend_dirwatch.h>
#endif

#endif
