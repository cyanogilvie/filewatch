#if HAVE_CONFIG_H
#include <config.h>
#endif

#include "tclstuff.h"
#include <stddef.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <stdint.h>

#ifndef INT2PTR
#define INT2PTR(p) ((void *)(intptr_t)(p))
#define PTR2INT(p) ((intptr_t)(p))
#endif


#define LITSTRS \
	X( L_EMPTY,				""					) \
	X( L_IN_ACCESS,			"IN_ACCESS"			) \
	X( L_IN_ATTRIB,			"IN_ATTRIB"			) \
	X( L_IN_CLOSE_WRITE,	"IN_CLOSE_WRITE"	) \
	X( L_IN_CLOSE_NOWRITE,	"IN_CLOSE_NOWRITE"	) \
	X( L_IN_CREATE,			"IN_CREATE"			) \
	X( L_IN_DELETE,			"IN_DELETE"			) \
	X( L_IN_DELETE_SELF,	"IN_DELETE_SELF"	) \
	X( L_IN_MODIFY,			"IN_MODIFY"			) \
	X( L_IN_MOVE_SELF,		"IN_MOVE_SELF"		) \
	X( L_IN_MOVED_FROM,		"IN_MOVED_FROM"		) \
	X( L_IN_MOVED_TO,		"IN_MOVED_TO"		) \
	X( L_IN_OPEN,			"IN_OPEN"			) \
	X( L_IN_IGNORED,		"IN_IGNORED"		) \
	X( L_IN_ISDIR,			"IN_ISDIR"			) \
	X( L_IN_Q_OVERFLOW,		"IN_Q_OVERFLOW"		) \
	X( L_IN_UNMOUNT,		"IN_UNMOUNT"		) \
	/* line intentionally left blank */

enum litstr {
#define X(name, str) name,
	LITSTRS
#undef X
	L_size
};

static const char* litstrs[] = {
#define X(name, str) str,
	LITSTRS
#undef X
};

struct interp_cx {
	Tcl_Obj*	lit[L_size];
};

static int list2mask(Tcl_Interp* interp, Tcl_Obj* list, uint32_t* mask) //<<<
{
	int			code = TCL_OK;

#define BITNAMES \
	X( IN_ACCESS		) \
	X( IN_MODIFY		) \
	X( IN_ATTRIB		) \
	X( IN_CLOSE_WRITE	) \
	X( IN_CLOSE_NOWRITE	) \
	X( IN_OPEN			) \
	X( IN_MOVED_FROM	) \
	X( IN_MOVED_TO		) \
	X( IN_CREATE		) \
	X( IN_DELETE		) \
	X( IN_DELETE_SELF	) \
	X( IN_MOVE_SELF		) \
	X( IN_CLOSE			) \
	X( IN_MOVE			) \
	X( IN_ONLYDIR		) \
	X( IN_DONT_FOLLOW	) \
	X( IN_MASK_ADD		) \
	X( IN_ISDIR			) \
	X( IN_ONESHOT		) \
	X( IN_ALL_EVENTS	) \
	/* line intentionally left blank */
	static const char* mask_bits[] = {
#define X(name) #name,
		BITNAMES
#undef X
		(char*)NULL
	};
	uint32_t map[] = {
#define X(name) name,
		BITNAMES
#undef X
	};
#undef BITNAMES

	Tcl_Size	oc;
	Tcl_Obj**	ov;
	TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, list, &oc, &ov));

	uint32_t	build = 0;
	for (Tcl_Size i=0; i<oc; i++) {
		int		index;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], mask_bits, "mask bit", TCL_EXACT, &index));
		build |= map[index];
	}

	*mask = build;

finally:
	return code;
}

//>>>
static int mask2list(Tcl_Interp* interp, struct interp_cx* l, uint32_t mask, Tcl_Obj** res) //<<<
{
	int			code = TCL_OK;
	Tcl_Obj*	result = Tcl_NewListObj(5, NULL);

#define CHECK_BIT(bit, strobj)	if ((mask & bit) == bit) TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, result, strobj))
	CHECK_BIT( IN_ACCESS,			l->lit[L_IN_ACCESS]			);
	CHECK_BIT( IN_ATTRIB,			l->lit[L_IN_ATTRIB]			);
	CHECK_BIT( IN_CLOSE_WRITE,		l->lit[L_IN_CLOSE_WRITE]	);
	CHECK_BIT( IN_CLOSE_NOWRITE,	l->lit[L_IN_CLOSE_NOWRITE]	);
	CHECK_BIT( IN_CREATE,			l->lit[L_IN_CREATE]			);
	CHECK_BIT( IN_DELETE,			l->lit[L_IN_DELETE]			);
	CHECK_BIT( IN_DELETE_SELF,		l->lit[L_IN_DELETE_SELF]	);
	CHECK_BIT( IN_MODIFY,			l->lit[L_IN_MODIFY]			);
	CHECK_BIT( IN_MOVE_SELF,		l->lit[L_IN_MOVE_SELF]		);
	CHECK_BIT( IN_MOVED_FROM,		l->lit[L_IN_MOVED_FROM]		);
	CHECK_BIT( IN_MOVED_TO,			l->lit[L_IN_MOVED_TO]		);
	CHECK_BIT( IN_OPEN,				l->lit[L_IN_OPEN]			);
	CHECK_BIT( IN_IGNORED,			l->lit[L_IN_IGNORED]		);
	CHECK_BIT( IN_ISDIR,			l->lit[L_IN_ISDIR]			);
	CHECK_BIT( IN_Q_OVERFLOW,		l->lit[L_IN_Q_OVERFLOW]		);
	CHECK_BIT( IN_UNMOUNT,			l->lit[L_IN_UNMOUNT]		);
#undef CHECK_BIT

	replace_tclobj(res, result);

finally:
	return code;
}

//>>>
static int get_queue_fd_from_chan(Tcl_Interp* interp, Tcl_Obj* handle, int* queue_fd) //<<<
{
	int			code = TCL_OK;
	Tcl_Channel	channel;
	int			chan_mode;

	channel = Tcl_GetChannel(interp, Tcl_GetString(handle), &chan_mode);
	if (!channel)
		THROW_ERROR_LABEL(finally, code, "Invalid queue handle: ", Tcl_GetString(handle));

	if ((chan_mode & TCL_READABLE) != TCL_READABLE)
		THROW_ERROR_LABEL(finally, code, "Queue exists, but is not readable.");

	if (Tcl_GetChannelHandle(channel, TCL_READABLE, (ClientData*)queue_fd) != TCL_OK)
		THROW_ERROR_LABEL(finally, code, "Couldn't retrieve queue fd from channel");

finally:
	return code;
}

//>>>

static OBJCMD(cmd_create_queue) //<<<
{
	int			code = TCL_OK;
	int			queue_fd = -1;

	(void)cdata;
	enum {A_cmd, A_objc};
	CHECK_ARGS_LABEL(finally, code, "");

	queue_fd = inotify_init();
	if (-1 == queue_fd) THROW_POSIX_LABEL(finally, code, "inotify_init");

	Tcl_Channel channel = Tcl_MakeFileChannel(INT2PTR((ptrdiff_t)queue_fd), TCL_READABLE);
	Tcl_RegisterChannel(interp, channel);
	queue_fd = -1;	// channel owns the fd now

	Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(channel), -1));

finally:
	if (queue_fd != -1) close(queue_fd);
	return code;
}

//>>>
static OBJCMD(cmd_add_watch) //<<<
{
	int			code = TCL_OK;
	int			queue_fd, wd;
	uint32_t	mask;

	(void)cdata;
	enum {A_cmd, A_QUEUE, A_PATH, A_MASK, A_objc};
	CHECK_ARGS_LABEL(finally, code, "queue path mask");

	TEST_OK_LABEL(finally, code, get_queue_fd_from_chan(interp, objv[A_QUEUE], &queue_fd));
	const char* path = Tcl_GetString(objv[A_PATH]);
	TEST_OK_LABEL(finally, code, list2mask(interp, objv[A_MASK], &mask));

	wd = inotify_add_watch(queue_fd, path, mask);
	if (-1 == wd) THROW_POSIX_LABEL(finally, code, "inotify_add_watch");

	Tcl_SetObjResult(interp, Tcl_NewIntObj(wd));

finally:
	return code;
}

//>>>
static OBJCMD(cmd_rm_watch) //<<<
{
	int			code = TCL_OK;
	int			queue_fd, wd;

	(void)cdata;
	enum {A_cmd, A_QUEUE, A_WD, A_objc};
	CHECK_ARGS_LABEL(finally, code, "queue wd");

	TEST_OK_LABEL(finally, code, get_queue_fd_from_chan(interp, objv[A_QUEUE], &queue_fd));
	TEST_OK_LABEL(finally, code, Tcl_GetIntFromObj(interp, objv[A_WD], &wd));

	if (-1 == inotify_rm_watch(queue_fd, wd))
		THROW_POSIX_LABEL(finally, code, "inotify_rm_watch");

	Tcl_SetObjResult(interp, Tcl_NewIntObj(0));

finally:
	return code;
}

//>>>
static OBJCMD(cmd_decode_events) //<<<
{
	int						code = TCL_OK;
	struct interp_cx*		l = cdata;
	unsigned char*			raw;
	Tcl_Obj*				res = NULL;
	Tcl_Obj*				tmp = NULL;
	Tcl_Obj*				evdata = NULL;

	enum {A_cmd, A_EVDATA, A_objc};
	CHECK_ARGS_LABEL(finally, code, "raw_event_data");

	Tcl_Size	raw_len;
#if TCL_MAJOR_VERSION >= 9
	raw = Tcl_GetBytesFromObj(interp, objv[A_EVDATA], &raw_len);
	if (!raw) { code=TCL_ERROR; goto finally; }
#else
	raw = Tcl_GetByteArrayFromObj(objv[A_EVDATA], &raw_len);
#endif

	replace_tclobj(&res, Tcl_NewListObj(5, NULL));

	size_t	offset		= 0;
	ssize_t	remaining	= raw_len;
	while (remaining > 0) {
		struct inotify_event*	event = (struct inotify_event*)(raw + offset);
		const size_t			eventsize = sizeof(struct inotify_event) + event->len;
		remaining	-= eventsize;
		offset		+= eventsize;

		replace_tclobj(&evdata, Tcl_NewListObj(4, NULL));

		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, evdata, Tcl_NewIntObj(event->wd)));
		TEST_OK_LABEL(finally, code, mask2list(interp, l, event->mask, &tmp));
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, evdata, tmp));
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, evdata, Tcl_NewIntObj(event->cookie)));
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, evdata,
				event->len
					? Tcl_NewStringObj(event->name, -1) // Why not give event->len as the length? len includes null padding
					: l->lit[L_EMPTY]
		));

		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, res, evdata));
	}

	Tcl_SetObjResult(interp, res);

finally:
	replace_tclobj(&tmp,	NULL);
	replace_tclobj(&res,	NULL);
	replace_tclobj(&evdata,	NULL);
	return code;
}

//>>>

#define NS	"::inotify"

static struct cmd {
	const char*		name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{NS "::create_queue",	cmd_create_queue	},
	{NS "::add_watch",		cmd_add_watch		},
	{NS "::rm_watch",		cmd_rm_watch		},
	{NS "::decode_events",	cmd_decode_events	},
	{0}
};

static void free_interp_cx(ClientData clientdata, Tcl_Interp* interp) //<<<
{
	(void)interp;
	struct interp_cx* l = clientdata;

	for (int i=0; i<L_size; i++) replace_tclobj(&l->lit[i], NULL);
	ckfree(l);	l = NULL;
}

//>>>

DLLEXPORT int Inotify_Init(Tcl_Interp* interp) //<<<
{
	int				code = TCL_OK;
	Tcl_Namespace*	ns = NULL;

#ifdef USE_TCL_STUBS
	if (!Tcl_InitStubs(interp, TCL_VERSION, 0)) { code=TCL_ERROR; goto finally; }
#endif

	if (sizeof(ClientData) < sizeof(int))
		THROW_ERROR_LABEL(finally, code, "On this platform ints are bigger than pointers.");

	ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));
	Tcl_CreateEnsemble(interp, NS, ns, 0);

	struct interp_cx* l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};
	Tcl_SetAssocData(interp, PACKAGE_NAME, free_interp_cx, l);

	for (int i=0; i<L_size; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(litstrs[i], -1));

	for (struct cmd* c=cmds; c->name; c++)
		Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);

	TEST_OK_LABEL(finally, code, Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION));

finally:
	if (code != TCL_OK) {
		if (ns) { Tcl_DeleteNamespace(ns); ns = NULL; }
		Tcl_SetAssocData(interp, PACKAGE_NAME, NULL, NULL);
	}
	return code;
}

//>>>
DLLEXPORT int Inotify_Unload(Tcl_Interp* interp, int flags) //<<<
{
	int		code = TCL_OK;

	if (flags != TCL_UNLOAD_DETACH_FROM_INTERPRETER &&
		flags != TCL_UNLOAD_DETACH_FROM_PROCESS)
		THROW_ERROR_LABEL(finally, code, "Unhandled flags");

	Tcl_Namespace* ns = Tcl_FindNamespace(interp, NS, NULL, TCL_GLOBAL_ONLY);
	if (ns) {
		Tcl_DeleteNamespace(ns);
		ns = NULL;
	}

	Tcl_SetAssocData(interp, PACKAGE_NAME, NULL, NULL);

finally:
	return code;
}

//>>>
// vim: foldmethod=marker foldmarker=<<<,>>>
