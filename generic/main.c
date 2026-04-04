#include <filewatchInt.h>

TCL_DECLARE_MUTEX(pkgdir_mutex)
static Tcl_Obj* g_pkgdir = NULL;

int pkgdir_path(Tcl_Interp* interp, const char* tail, Tcl_Obj** res) //<<<
{
	int			code = TCL_OK;
	Tcl_Obj*	tailobj = NULL;

	Tcl_MutexLock(&pkgdir_mutex);

	if (!g_pkgdir)
		THROW_ERROR_LABEL(finally, code, "Package directory not set.");

	replace_tclobj(&tailobj, Tcl_NewStringObj(tail, -1));
	replace_tclobj(res, Tcl_FSJoinToPath(g_pkgdir, 1, &tailobj));

finally:
	Tcl_MutexUnlock(&pkgdir_mutex);
	replace_tclobj(&tailobj,	NULL);
	return code;
}

//>>>

static struct cmd {
	const char*		name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{0}
};

DLLEXPORT int Filewatch_Init(Tcl_Interp* interp) //<<<
{
	int			code = TCL_OK;
	Tcl_Obj*	get_pkgdir = NULL;
	Tcl_Obj*	script_fn = NULL;

#ifdef USE_TCL_STUBS
	if (!Tcl_InitStubs(interp, TCL_VERSION, 0)) { code=TCL_ERROR; goto finally; }
#endif

	// dup, getstring, free int rep: paranoia to make sure we store
	// a private, pure string Tcl_Obj with no entaglements with any interp
	// or thread context.  Given that this likely comes to us from the VFS
	// subsystem, this paranoia is not unwarranted.
	TEST_OK_LABEL(finally, code, Tcl_EvalEx(interp, "file dirname [file normalize [info script]]", -1, 0));
	Tcl_MutexLock(&pkgdir_mutex);
	replace_tclobj(&g_pkgdir, Tcl_DuplicateObj(Tcl_GetObjResult(interp)));
	Tcl_GetString(g_pkgdir);
	Tcl_FreeInternalRep(g_pkgdir);
	Tcl_MutexUnlock(&pkgdir_mutex);
	Tcl_ResetResult(interp);

	Tcl_Namespace* ns = Tcl_CreateNamespace(interp, NS_STR, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));
	Tcl_CreateEnsemble(interp, NS_STR, ns, 0);

	for (struct cmd* c=cmds; c->name; c++)
		Tcl_CreateObjCommand(interp, c->name, c->proc, NULL, NULL);

	TEST_OK_LABEL(finally, code, pkgdir_path(interp, "init.tcl", &script_fn));
	TEST_OK_LABEL(finally, code, Tcl_EvalFile(interp, Tcl_GetString(script_fn)));

	TEST_OK_LABEL(finally, code, pkgdir_path(interp, "watchdir_base.tcl", &script_fn));
	TEST_OK_LABEL(finally, code, Tcl_EvalFile(interp, Tcl_GetString(script_fn)));

#if BACKEND_INOTIFY
	TEST_OK_LABEL(finally, code, Inotify_Init(interp));
#elif BACKEND_FSEVENTS
	TEST_OK_LABEL(finally, code, Fsevents_Init(interp));
#elif BACKEND_DIRWATCH
	TEST_OK_LABEL(finally, code, Dirwatch_Init(interp));
#endif

	TEST_OK_LABEL(finally, code, Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION));

finally:
	replace_tclobj(&get_pkgdir,	NULL);
	replace_tclobj(&script_fn,	NULL);
	return code;
}

//>>>
DLLEXPORT int Filewatch_Unload(Tcl_Interp* interp, int flags) //<<<
{
	int		code = TCL_OK;

	if (flags != TCL_UNLOAD_DETACH_FROM_INTERPRETER &&
		flags != TCL_UNLOAD_DETACH_FROM_PROCESS)
		THROW_ERROR_LABEL(finally, code, "Unhandled flags");

#if BACKEND_INOTIFY
	TEST_OK_LABEL(finally, code, Inotify_Unload(interp));
#elif BACKEND_FSEVENTS
	TEST_OK_LABEL(finally, code, Fsevents_Unload(interp));
#elif BACKEND_DIRWATCH
	TEST_OK_LABEL(finally, code, Dirwatch_Unload(interp));
#endif

	Tcl_Namespace* ns = Tcl_FindNamespace(interp, NS_STR, NULL, TCL_GLOBAL_ONLY);
	if (ns) {
		Tcl_DeleteNamespace(ns);
		ns = NULL;
	}

	Tcl_MutexLock(&pkgdir_mutex);
	replace_tclobj(&g_pkgdir, NULL);
	Tcl_MutexUnlock(&pkgdir_mutex);
	Tcl_MutexFinalize(&pkgdir_mutex);

finally:
	return code;
}

//>>>

// vim: ts=4 shiftwidth=4 noexpandtab foldmethod=marker foldmarker=<<<,>>>
