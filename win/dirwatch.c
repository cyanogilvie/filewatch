#include <filewatchInt.h>

#if SHIM_INOTIFY
#	include <shim_inotify.h>
#else
#	include <windows.h>
#endif

#include <string.h>

#define LITSTRS \
	X( L_EV_TEMPLATE,	"name {} action {}"	) \
	X( L_NAME,			"name"				) \
	X( L_ACTION,		"action"			) \
	X( L_PACKAGE_NAME,	PACKAGE_NAME		) \
	X( L_CALLBACK,		"CALLBACK"			) \
	X( L_DIRWATCH,		"DIRWATCH"			) \
	/* line intentionally left blank */

#define ACTIONS \
	X( ADDED,			FILE_ACTION_ADDED			) \
	X( REMOVED,			FILE_ACTION_REMOVED			) \
	X( MODIFIED,		FILE_ACTION_MODIFIED		) \
	X( RENAMED_OLD,		FILE_ACTION_RENAMED_OLD_NAME) \
	X( RENAMED_NEW,		FILE_ACTION_RENAMED_NEW_NAME) \
	/* line intentionally left blank */

#define FILTERS \
	X( FILE_NAME,		FILE_NOTIFY_CHANGE_FILE_NAME	) \
	X( DIR_NAME,		FILE_NOTIFY_CHANGE_DIR_NAME		) \
	X( ATTRIBUTES,		FILE_NOTIFY_CHANGE_ATTRIBUTES	) \
	X( SIZE,			FILE_NOTIFY_CHANGE_SIZE			) \
	X( LAST_WRITE,		FILE_NOTIFY_CHANGE_LAST_WRITE	) \
	X( CREATION,		FILE_NOTIFY_CHANGE_CREATION		) \
	X( SECURITY,		FILE_NOTIFY_CHANGE_SECURITY		) \
	/* line intentionally left blank */

enum litstr {
#define X(name, str) name,
	LITSTRS
#undef X
#define X(name, value) L_ACTION_##name,
	ACTIONS
#undef X
#define X(name, value) L_FILTER_##name,
	FILTERS
#undef X
	L_size
};

static const char* litstrs[L_size] = {
#define X(name, str) str,
	LITSTRS
#undef X
#define X(name, value) #name,
	ACTIONS
#undef X
#define X(name, value) #name,
	FILTERS
#undef X
};

static const struct {
	const char*	name;
	DWORD		value;
	enum litstr	namekey;
} action_map[] = {
#define X(name, value) {#name, value, L_ACTION_##name},
	ACTIONS
#undef X
	{0}
};

static const struct {
	const char*	name;
	DWORD		value;
	enum litstr	namekey;
} filter_map[] = {
#define X(name, value) {#name, value, L_FILTER_##name},
	FILTERS
#undef X
	{0}
};

struct interp_cx {
	Tcl_Obj*	lit[L_size];
};

// Wire format for events through the pipe <<<
// Each event:
//   uint32_t action    - FILE_ACTION_* value
//   uint32_t name_len  - byte length of name (UTF-8)
//   char     name[]    - UTF-8, not null-terminated

struct wire_event {
	uint32_t	action;
	uint32_t	name_len;
};

// >>>
// Watch state <<<

struct watch_state {
	HANDLE		pipe_write;
	HANDLE		ready_event;
	HANDLE		dir_handle;
	HANDLE		stop_event;
	HANDLE		thread_handle;
	BOOL		recursive;
	DWORD		filter;
	BYTE		buffer[65536];
};

// >>>
// Background thread <<<

static DWORD WINAPI dirwatch_thread(LPVOID param) //<<<
{
	struct watch_state* ws = param;

	OVERLAPPED overlapped = {0};
	overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!overlapped.hEvent) return 1;

	HANDLE events[2] = { overlapped.hEvent, ws->stop_event };
	int first = 1;

	while (1) {
		ResetEvent(overlapped.hEvent);
		BOOL ok = ReadDirectoryChangesW(
			ws->dir_handle,
			ws->buffer,
			sizeof(ws->buffer),
			ws->recursive,
			ws->filter,
			NULL,
			&overlapped,
			NULL
		);
		if (!ok) break;

		// Signal main thread after first ReadDirectoryChangesW sets up watches
		if (first) {
			first = 0;
			SetEvent(ws->ready_event);
		}

		DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (wait != WAIT_OBJECT_0) break;

		DWORD bytes_returned;
		if (!GetOverlappedResult(ws->dir_handle, &overlapped, &bytes_returned, FALSE))
			break;

		if (bytes_returned == 0) continue;

		// Walk FILE_NOTIFY_INFORMATION chain
		FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)ws->buffer;
		for (;;) {
			int utf8_len = WideCharToMultiByte(CP_UTF8, 0,
				fni->FileName, fni->FileNameLength / sizeof(WCHAR),
				NULL, 0, NULL, NULL);

			if (utf8_len > 0) {
				BYTE wire_buf[sizeof(struct wire_event) + MAX_PATH * 4];
				struct wire_event* hdr = (struct wire_event*)wire_buf;
				hdr->action   = fni->Action;
				hdr->name_len = (uint32_t)utf8_len;

				WideCharToMultiByte(CP_UTF8, 0,
					fni->FileName, fni->FileNameLength / sizeof(WCHAR),
					(char*)(wire_buf + sizeof(struct wire_event)), utf8_len,
					NULL, NULL);

				// Normalize path separators
				char* name = (char*)(wire_buf + sizeof(struct wire_event));
				for (int i = 0; i < utf8_len; i++)
					if (name[i] == '\\') name[i] = '/';

				DWORD written;
				WriteFile(ws->pipe_write, wire_buf,
					sizeof(struct wire_event) + utf8_len, &written, NULL);
			}

			if (fni->NextEntryOffset == 0) break;
			fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
		}
	}

	CloseHandle(overlapped.hEvent);
	return 0;
}

//>>>
// >>>
// Channel close handler <<<

static void watch_close_handler(ClientData cd) //<<<
{
	struct watch_state* ws = cd;

	SetEvent(ws->stop_event);
	WaitForSingleObject(ws->thread_handle, 5000);

	CloseHandle(ws->thread_handle);
	CloseHandle(ws->pipe_write);
	CloseHandle(ws->dir_handle);
	CloseHandle(ws->stop_event);
	ckfree(ws);
}

//>>>
// >>>
// Commands <<<

static OBJCMD(cmd_create_watch) //<<<
{
	int			code = TCL_OK;
	struct interp_cx* l = cdata;
	HANDLE		dir_handle = INVALID_HANDLE_VALUE;
	HANDLE		pipe_read = INVALID_HANDLE_VALUE;
	HANDLE		pipe_write = INVALID_HANDLE_VALUE;
	struct watch_state* ws = NULL;
	Tcl_Channel	chan = NULL;

	(void)l;

#define OPTS \
	X( "-recursive",	OPT_RECURSIVE	) \
	X( "-filter",		OPT_FILTER		) \
	X( "--",			OPT_END			) \
	/* line intentionally left blank */

	static const char* opts[] = {
#define X(name, en) name,
		OPTS
#undef X
		NULL
	};
	enum opt {
#define X(name, en) en,
		OPTS
#undef X
	} opt;
#undef OPTS

	if (objc < 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "?-recursive bool? ?-filter filterlist? ?--? path");
		code = TCL_ERROR;
		goto finally;
	}

	BOOL recursive = TRUE;
	DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
	               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
	int path_idx = objc - 1;

	for (int i = 1; i < path_idx; i++) {
		const char* arg = Tcl_GetString(objv[i]);
		if (arg[0] != '-') { path_idx = i; break; }

		int optidx;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optidx));
		opt = optidx;
		switch (opt) {
			case OPT_RECURSIVE: {
				if (++i >= objc) THROW_ERROR_LABEL(finally, code, "-recursive requires a value");
				int bval;
				TEST_OK_LABEL(finally, code, Tcl_GetBooleanFromObj(interp, objv[i], &bval));
				recursive = bval ? TRUE : FALSE;
				break;
			}
			case OPT_FILTER: {
				if (++i >= objc) THROW_ERROR_LABEL(finally, code, "-filter requires a value");
				Tcl_Size fc; Tcl_Obj** fv;
				TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, objv[i], &fc, &fv));
				filter = 0;
				for (Tcl_Size j = 0; j < fc; j++) {
					int k;
					TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObjStruct(interp, fv[j],
						filter_map, sizeof(filter_map[0]), "filter", 0, &k));
					filter |= filter_map[k].value;
				}
				break;
			}
			case OPT_END:
				path_idx = i + 1;
				break;
		}
	}

	if (path_idx >= objc)
		THROW_ERROR_LABEL(finally, code, "no path specified");

	const char* path = Tcl_GetString(objv[path_idx]);

	dir_handle = CreateFileA(path,
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
	if (dir_handle == INVALID_HANDLE_VALUE)
		THROW_POSIX_LABEL(finally, code, "CreateFile");

	if (!CreatePipe(&pipe_read, &pipe_write, NULL, 0))
		THROW_POSIX_LABEL(finally, code, "CreatePipe");

	ws = (struct watch_state*)ckalloc(sizeof(*ws));
	*ws = (struct watch_state){
		.pipe_write		= pipe_write,
		.dir_handle		= dir_handle,
		.recursive		= recursive,
		.filter			= filter,
	};
	pipe_write = INVALID_HANDLE_VALUE;
	dir_handle = INVALID_HANDLE_VALUE;

	ws->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!ws->stop_event) THROW_POSIX_LABEL(finally, code, "CreateEvent");

	ws->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!ws->ready_event) THROW_POSIX_LABEL(finally, code, "CreateEvent");

	chan = Tcl_MakeFileChannel(pipe_read, TCL_READABLE);
	pipe_read = INVALID_HANDLE_VALUE;
	Tcl_RegisterChannel(NULL, chan);
	Tcl_RegisterChannel(interp, chan);

	ws->thread_handle = CreateThread(NULL, 0, dirwatch_thread, ws, 0, NULL);
	if (!ws->thread_handle)
		THROW_ERROR_LABEL(finally, code, "Failed to create dirwatch thread");

	// Wait for thread to finish stream initialization
	WaitForSingleObject(ws->ready_event, INFINITE);
	CloseHandle(ws->ready_event);
	ws->ready_event = NULL;

	Tcl_CreateCloseHandler(chan, watch_close_handler, ws);
	ws = NULL;

	Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));

finally:
	if (dir_handle != INVALID_HANDLE_VALUE) CloseHandle(dir_handle);
	if (pipe_read != INVALID_HANDLE_VALUE) CloseHandle(pipe_read);
	if (pipe_write != INVALID_HANDLE_VALUE) CloseHandle(pipe_write);
	if (ws) {
		if (ws->ready_event) CloseHandle(ws->ready_event);
		ckfree(ws);
	}
	if (chan) {
		if (code != TCL_OK) Tcl_UnregisterChannel(interp, chan);
		Tcl_UnregisterChannel(NULL, chan);
	}
	return code;
}

//>>>
static OBJCMD(cmd_dispatch_events) //<<<
{
	int					code = TCL_OK;
	struct interp_cx*	l = cdata;
	Tcl_Obj*			buf = NULL;
	Tcl_Obj*			bufdup = NULL;
	Tcl_Obj*			ev = NULL;
	Tcl_Obj*			cb = NULL;
	Tcl_Obj*			tmp = NULL;

	enum {A_cmd, A_BUFVAR, A_CB, A_objc};
	CHECK_ARGS_LABEL(finally, code, "bufvar cb");

	buf = Tcl_ObjGetVar2(interp, objv[A_BUFVAR], NULL, TCL_LEAVE_ERR_MSG);
	if (!buf) { code = TCL_ERROR; goto finally; }
	if (Tcl_IsShared(buf)) {
		replace_tclobj(&bufdup, Tcl_DuplicateObj(buf));
		buf = bufdup;
	}

	Tcl_Size raw_len;
	unsigned char* raw;
#if TCL_MAJOR_VERSION >= 9
	raw = Tcl_GetBytesFromObj(interp, buf, &raw_len);
	if (!raw) { code = TCL_ERROR; goto finally; }
#else
	raw = Tcl_GetByteArrayFromObj(buf, &raw_len);
#endif

	size_t offset = 0;
	while (offset + sizeof(struct wire_event) <= (size_t)raw_len) {
		struct wire_event hdr;
		memcpy(&hdr, raw + offset, sizeof(hdr));
		size_t o = offset + sizeof(hdr);

		if (o + hdr.name_len > (size_t)raw_len) break;

		replace_tclobj(&ev, Tcl_DuplicateObj(l->lit[L_EV_TEMPLATE]));

		// name
		replace_tclobj(&tmp, Tcl_NewStringObj((const char*)(raw + o), hdr.name_len));
		TEST_OK_LABEL(finally, code, Tcl_DictObjPut(interp, ev, l->lit[L_NAME], tmp));
		o += hdr.name_len;

		// action
		Tcl_Obj* action_lit = NULL;
		for (int i = 0; action_map[i].name; i++) {
			if (action_map[i].value == hdr.action) {
				action_lit = l->lit[action_map[i].namekey];
				break;
			}
		}
		TEST_OK_LABEL(finally, code, Tcl_DictObjPut(interp, ev,
			l->lit[L_ACTION], action_lit ? action_lit : Tcl_NewStringObj("UNKNOWN", -1)));
		offset = o;

		// Invoke callback
		replace_tclobj(&cb, Tcl_DuplicateObj(objv[A_CB]));
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, cb, ev));
		int cb_code = Tcl_EvalObjEx(interp, cb, TCL_EVAL_DIRECT);
		if (cb_code != TCL_OK) {
			replace_tclobj(&tmp, Tcl_NewListObj(4, (Tcl_Obj*[]){
				l->lit[L_PACKAGE_NAME],
				l->lit[L_CALLBACK],
				l->lit[L_DIRWATCH],
				Tcl_GetReturnOptions(interp, cb_code),
			}));
			Tcl_SetObjErrorCode(interp, tmp);
			Tcl_AddErrorInfo(interp, "\n    (while processing " PACKAGE_NAME " dirwatch callback)");
			Tcl_BackgroundException(interp, cb_code);
			Tcl_ResetResult(interp);
		}
	}

	const size_t remaining = raw_len - offset;
	if (remaining) {
		memmove(raw, raw + offset, remaining);
		raw = Tcl_SetByteArrayLength(buf, remaining);
	} else {
		raw = Tcl_SetByteArrayLength(buf, 0);
	}

	buf = Tcl_ObjSetVar2(interp, objv[A_BUFVAR], NULL, buf, TCL_LEAVE_ERR_MSG);
	if (!buf) { code = TCL_ERROR; goto finally; }

finally:
	replace_tclobj(&bufdup,	NULL);
	replace_tclobj(&ev,		NULL);
	replace_tclobj(&cb,		NULL);
	replace_tclobj(&tmp,	NULL);
	return code;
}

//>>>
// >>>

static struct cmd {
	const char*		name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{NS_DRIVER_STR "::create_watch",		cmd_create_watch},
	{NS_DRIVER_STR "::dispatch_events",		cmd_dispatch_events},
	{0}
};

static void free_interp_cx(ClientData clientdata, Tcl_Interp* interp) //<<<
{
	(void)interp;
	struct interp_cx* l = clientdata;
	for (int i = 0; i < L_size; i++) replace_tclobj(&l->lit[i], NULL);
	ckfree(l);
}

//>>>

int Dirwatch_Init(Tcl_Interp* interp) //<<<
{
	int				code = TCL_OK;
	Tcl_Namespace*	ns = NULL;
	Tcl_Obj*		script_fn = NULL;

	ns = Tcl_CreateNamespace(interp, NS_DRIVER_STR, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	struct interp_cx* l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};
	Tcl_SetAssocData(interp, NS_DRIVER_STR, free_interp_cx, l);

	for (int i = 0; i < L_size; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(litstrs[i], -1));

	Tcl_Size dummy;
	(void)Tcl_DictObjSize(NULL, l->lit[L_EV_TEMPLATE], &dummy);

	for (struct cmd* c = cmds; c->name; c++)
		Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);

	TEST_OK_LABEL(finally, code, pkgdir_path(interp, "watcher_dirwatch.tcl", &script_fn));
	TEST_OK_LABEL(finally, code, Tcl_EvalFile(interp, Tcl_GetString(script_fn)));
	TEST_OK_LABEL(finally, code, pkgdir_path(interp, "watchdir.tcl", &script_fn));
	TEST_OK_LABEL(finally, code, Tcl_EvalFile(interp, Tcl_GetString(script_fn)));

finally:
	if (code != TCL_OK) {
		if (ns) Tcl_DeleteNamespace(ns);
		Tcl_SetAssocData(interp, NS_DRIVER_STR, NULL, NULL);
	}
	replace_tclobj(&script_fn, NULL);
	return code;
}

//>>>
int Dirwatch_Unload(Tcl_Interp* interp) //<<<
{
	Tcl_Namespace* ns = Tcl_FindNamespace(interp, NS_DRIVER_STR, NULL, TCL_GLOBAL_ONLY);
	if (ns) Tcl_DeleteNamespace(ns);
	Tcl_SetAssocData(interp, NS_DRIVER_STR, NULL, NULL);
	return TCL_OK;
}

//>>>

// vim: ts=4 shiftwidth=4 noexpandtab foldmethod=marker foldmarker=<<<,>>>
