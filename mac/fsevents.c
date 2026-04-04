#include <filewatchInt.h>

#if SHIM_INOTIFY
#	include <shim_inotify.h>
#else
#	include <CoreServices/CoreServices.h>
#endif

#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#define LITSTRS \
	X( L_EV_TEMPLATE,	"path {} flags {}"	) \
	X( L_PATH,			"path"				) \
	X( L_FLAGS,			"flags"				) \
	X( L_PACKAGE_NAME,	PACKAGE_NAME		) \
	X( L_CALLBACK,		"CALLBACK"			) \
	X( L_FSEVENTS,		"FSEVENTS"			) \
	/* line intentionally left blank */

#define CREATEFLAGS \
	X( FILE_EVENTS,		kFSEventStreamCreateFlagFileEvents	) \
	X( NO_DEFER,		kFSEventStreamCreateFlagNoDefer		) \
	X( WATCH_ROOT,		kFSEventStreamCreateFlagWatchRoot	) \
	X( IGNORE_SELF,		kFSEventStreamCreateFlagIgnoreSelf	) \
	/* line intentionally left blank */

#define EVENTFLAGS \
	X( CREATED,			kFSEventStreamEventFlagItemCreated		) \
	X( REMOVED,			kFSEventStreamEventFlagItemRemoved		) \
	X( MODIFIED,		kFSEventStreamEventFlagItemModified		) \
	X( RENAMED,			kFSEventStreamEventFlagItemRenamed		) \
	X( ISDIR,			kFSEventStreamEventFlagItemIsDir		) \
	X( ISFILE,			kFSEventStreamEventFlagItemIsFile		) \
	X( ISSYMLINK,		kFSEventStreamEventFlagItemIsSymlink	) \
	X( ATTRIB,			kFSEventStreamEventFlagItemInodeMetaMod	) \
	X( OWNER_CHANGE,	kFSEventStreamEventFlagItemChangeOwner	) \
	X( XATTR_MOD,		kFSEventStreamEventFlagItemXattrMod		) \
	/* line intentionally left blank */

enum litstr {
#define X(name, str) name,
	LITSTRS
#undef X
#define X(name, value) L_EVENTFLAG_##name,
	EVENTFLAGS
#undef X
#define X(name, value) L_CREATEFLAG_##name,
	CREATEFLAGS
#undef X
	L_size
};

static const char* litstrs[L_size] = {
#define X(name, str) str,
	LITSTRS
#undef X
#define X(name, value) #name,
	EVENTFLAGS
#undef X
#define X(name, value) #name,
	CREATEFLAGS
#undef X
};

static const struct {
	const char*	name;
	uint32_t	value;
	enum litstr	namekey;
} create_flag_map[] = {
#define X(name, value) {#name, value, L_CREATEFLAG_##name},
	CREATEFLAGS
#undef X
	{0}
};

static const struct {
	const char*	name;
	uint32_t	value;
	enum litstr	namekey;
} event_flag_map[] = {
#define X(name, value) {#name, value, L_EVENTFLAG_##name},
	EVENTFLAGS
#undef X
	{0}
};

struct interp_cx {
	Tcl_Obj*	lit[L_size];
};

// Wire format for events through the pipe:
// Each event:
//   uint32_t flags     - FSEventStreamEventFlags
//   uint32_t path_len  - byte length of path
//   char     path[]    - UTF-8, not null-terminated

struct wire_event {
	uint32_t	flags;
	uint32_t	path_len;
};

struct stream_state {
	int							pipe_write;
	int							ready_pipe[2];	// thread signals readiness
	volatile int				running;
	Tcl_ThreadId				thread;
	CFRunLoopRef				run_loop;
	FSEventStreamRef			stream;
	CFArrayRef					paths;
	double						latency;
	FSEventStreamCreateFlags	create_flags;
};


// FSEvents callback and thread <<<
static void fsevents_callback( //<<<
	ConstFSEventStreamRef			streamRef,
	void*							info,
	size_t							numEvents,
	void*							eventPaths,
	const FSEventStreamEventFlags	eventFlags[],
	const FSEventStreamEventId		eventIds[]
) {
	(void)streamRef; (void)eventIds;
	struct stream_state*	ss = info;
	char**					paths = eventPaths;

	for (size_t i=0; i<numEvents; i++) {
		uint32_t path_len = (uint32_t)strlen(paths[i]);
		struct wire_event hdr = {
			.flags    = (uint32_t)eventFlags[i],
			.path_len = path_len,
		};
		// Write header + path atomically if possible (fits in PIPE_BUF)
		struct iovec iov[2] = {
			{ .iov_base = &hdr,      .iov_len = sizeof(hdr) },
			{ .iov_base = paths[i],  .iov_len = path_len },
		};
		writev(ss->pipe_write, iov, 2);
	}
}

//>>>
static Tcl_ThreadCreateType fsevents_thread(ClientData clientData) //<<<
{
	struct stream_state* ss = clientData;

	FSEventStreamContext ctx = {
		.info = ss,
	};

	ss->stream = FSEventStreamCreate(
		NULL,							/* allocator */
		fsevents_callback,				/* callback */
		&ctx,							/* context */
		ss->paths,						/* pathsToWatch */
		kFSEventStreamEventIdSinceNow,	/* sinceWhen */
		ss->latency,					/* latency */
		ss->create_flags				/* flags */
	);

	ss->run_loop = CFRunLoopGetCurrent();
	FSEventStreamScheduleWithRunLoop(ss->stream, ss->run_loop, kCFRunLoopDefaultMode);
	FSEventStreamStart(ss->stream);

	// Signal the main thread that we're ready to receive events
	{
		char c = 'r';
		(void)write(ss->ready_pipe[1], &c, 1);
		close(ss->ready_pipe[1]);
		ss->ready_pipe[1] = -1;
	}

	CFRunLoopRun();

	FSEventStreamStop(ss->stream);
	FSEventStreamInvalidate(ss->stream);
	FSEventStreamRelease(ss->stream);
	ss->stream = NULL;

	TCL_THREAD_CREATE_RETURN;
}

//>>>
// >>>

static void stream_close_handler(ClientData cd) //<<<
{
	struct stream_state* ss = cd;

	ss->running = 0;
	if (ss->run_loop)
		CFRunLoopStop(ss->run_loop);

	int status;
	Tcl_JoinThread(ss->thread, &status);

	close(ss->pipe_write);
	CFRelease(ss->paths);
	ckfree(ss);
}

//>>>

// Tcl Commands
static OBJCMD(cmd_create_stream) // fsevents::create_stream ?-latency N? ?-flags flaglist? ?--? path ?path ...? <<<
{
	int						code = TCL_OK;
	(void)cdata;
	int						pipefd[2] = {-1, -1};
	CFMutableArrayRef		cf_paths = NULL;
	struct stream_state*	ss = NULL;
	Tcl_Channel				chan = NULL;

#define OPTS \
	X( "-latency",	OPT_LATENCY	) \
	X( "-flags",	OPT_FLAGS	) \
	X( "--",		OPT_END		) \
	/* line intentionally left blank */

	static const char* opts[] = {
#define X(name, enum) name,
		OPTS
#undef X
		NULL
	};
	enum opt {
#define X(name, enum) enum,
		OPTS
#undef X
	} opt;
#undef OPTS

	if (objc < 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "?-latency seconds? ?-flags flaglist? ?--? path ?path ...?");
		code = TCL_ERROR;
		goto finally;
	}

	double latency = 0.1;
	FSEventStreamCreateFlags create_flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer;
	int path_start = 1;

	// Parse options
	for (int i=1; i<objc; i++) {
		const char* arg = Tcl_GetString(objv[i]);
		if (arg[0] != '-') { path_start = i; break; }

		int optidx;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optidx));
		opt = optidx;
		switch (opt) {
			case OPT_LATENCY:
				if (++i >= objc) THROW_ERROR_LABEL(finally, code, "-latency requires a value");
				TEST_OK_LABEL(finally, code, Tcl_GetDoubleFromObj(interp, objv[i], &latency));
				break;

			case OPT_FLAGS:
				{
					if (++i >= objc) THROW_ERROR_LABEL(finally, code, "-flags requires a value");
					Tcl_Size fc; Tcl_Obj** fv;
					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, objv[i], &fc, &fv));
					create_flags = 0;
					for (Tcl_Size j=0; j<fc; j++) {
						int k;
						TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObjStruct(interp, fv[j], create_flag_map, sizeof(create_flag_map[0]), "flag", 0, &k));
						create_flags |= create_flag_map[k].value;
					}
				}
				break;

			case OPT_END:
				path_start = i;
				break;
		}
	}

	if (path_start >= objc)
		THROW_ERROR_LABEL(finally, code, "no paths specified");

	// Build CFArray of paths
	cf_paths = CFArrayCreateMutable(NULL, objc - path_start, &kCFTypeArrayCallBacks);
	for (int i=path_start; i<objc; i++) {
		const char* path = Tcl_GetString(objv[i]);
		CFStringRef cf_path = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
		CFArrayAppendValue(cf_paths, cf_path);
		CFRelease(cf_path);
	}

	// Create pipe
#if HAVE_PIPE2
	if (pipe2(pipefd, O_CLOEXEC) != 0) THROW_POSIX_LABEL(finally, code, "pipe2");
#else
	if (pipe(pipefd) != 0) THROW_POSIX_LABEL(finally, code, "pipe");
	fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
#endif

	// Allocate state
	ss = (struct stream_state*)ckalloc(sizeof(*ss));
	*ss = (struct stream_state){
		.pipe_write		= pipefd[1],
		.ready_pipe		= {-1, -1},
		.running		= 1,
		.paths			= cf_paths,
		.latency		= latency,
		.create_flags	= create_flags,
	};
	cf_paths = NULL;	// ownership transferred
	pipefd[1] = -1;		// stream thread owns this fd now

	// Ready pipe: thread signals when stream is initialized
#if HAVE_PIPE2
	if (pipe2(ss->ready_pipe, O_CLOEXEC) != 0) THROW_POSIX_LABEL(finally, code, "pipe2");
#else
	if (pipe(ss->ready_pipe) != 0) THROW_POSIX_LABEL(finally, code, "pipe");
	fcntl(ss->ready_pipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(ss->ready_pipe[1], F_SETFD, FD_CLOEXEC);
#endif

	// Create Tcl channel from pipe read end
	chan = Tcl_MakeFileChannel(INT2PTR((intptr_t)pipefd[0]), TCL_READABLE);
	pipefd[0] = -1;		// channel owns this fd
	Tcl_RegisterChannel(NULL, chan);	// Our ref
	Tcl_RegisterChannel(interp, chan);

	// Start background thread
	if (Tcl_CreateThread(&ss->thread, fsevents_thread, ss, TCL_THREAD_STACK_DEFAULT, TCL_THREAD_JOINABLE) != TCL_OK)
		THROW_ERROR_LABEL(finally, code, "Failed to create FSEvents thread");

	// Wait for the thread to finish stream initialization
	{
		char c;
		(void)read(ss->ready_pipe[0], &c, 1);
		close(ss->ready_pipe[0]);
		ss->ready_pipe[0] = -1;
	}

	Tcl_CreateCloseHandler(chan, stream_close_handler, ss);
	ss = NULL;	// ownership transferred to close handler

	Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetChannelName(chan), -1));

finally:
	if (cf_paths) CFRelease(cf_paths);
	if (pipefd[0] != -1) close(pipefd[0]);
	if (pipefd[1] != -1) close(pipefd[1]);
	if (ss) {
		close(ss->pipe_write);
		if (ss->ready_pipe[0] != -1) close(ss->ready_pipe[0]);
		if (ss->ready_pipe[1] != -1) close(ss->ready_pipe[1]);
		ckfree(ss);
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
	Tcl_Obj*			buf = NULL;	// borrowed ref: either the Tcl variable or the private duplicate "bufdup"
	Tcl_Obj*			bufdup = NULL;
	Tcl_Obj*			ev = NULL;
	Tcl_Obj*			cb = NULL;
	Tcl_Obj*			tmp = NULL;

	enum {A_cmd, A_BUFVAR, A_CB, A_objc};
	CHECK_ARGS_LABEL(finally, code, "bufvar cb");

	buf = Tcl_ObjGetVar2(interp, objv[A_BUFVAR], NULL, TCL_LEAVE_ERR_MSG);
	if (!buf) { code=TCL_ERROR; goto finally; }
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

		if (o + hdr.path_len > (size_t)raw_len) break;  // incomplete

		replace_tclobj(&ev, Tcl_DuplicateObj(l->lit[L_EV_TEMPLATE]));

		// path
		replace_tclobj(&tmp, Tcl_NewStringObj((const char*)(raw + o), hdr.path_len));
		TEST_OK_LABEL(finally, code, Tcl_DictObjPut(interp, ev, l->lit[L_PATH], tmp))
		o += hdr.path_len;

		// flags → list
		Tcl_Obj* flags_list = Tcl_NewListObj(5, NULL);
		for (int i = 0; event_flag_map[i].name; i++)
			if (event_flag_map[i].value && (hdr.flags & event_flag_map[i].value))
				TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, flags_list, l->lit[event_flag_map[i].namekey]));
		TEST_OK_LABEL(finally, code, Tcl_DictObjPut(interp, ev, l->lit[L_FLAGS], flags_list));
		offset = o;

		// Call callback with event dict
		replace_tclobj(&cb, Tcl_DuplicateObj(objv[A_CB]));
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, cb, ev));
		int cb_code = Tcl_EvalObjEx(interp, cb, TCL_EVAL_DIRECT);
		if (cb_code != TCL_OK) {
			// Raise background exception and continue processing events
			replace_tclobj(&tmp, Tcl_NewListObj(4, (Tcl_Obj*[]){
				l->lit[L_PACKAGE_NAME],
				l->lit[L_CALLBACK],
				l->lit[L_FSEVENTS],
				Tcl_GetReturnOptions(interp, cb_code),
			}));
			Tcl_SetObjErrorCode(interp, tmp);
			Tcl_AddErrorInfo(interp, "\n    (while processing filewatch FSEvents callback)");
			Tcl_BackgroundException(interp, cb_code);
			Tcl_ResetResult(interp);
		}
	}

	const size_t remaining = raw_len - offset;
	if (remaining) {
		// Shift remaining incomplete data to the start of the buffer for the next read
		memmove(raw, raw + offset, remaining);
		raw = Tcl_SetByteArrayLength(buf, remaining);
	} else {
		raw = Tcl_SetByteArrayLength(buf, 0);
	}

	// Put the updated buffer back into the variable.  Required in all cases to support
	// variable write trace semantics
	buf = Tcl_ObjSetVar2(interp, objv[A_BUFVAR], NULL, buf, TCL_LEAVE_ERR_MSG);
	if (!buf) { code = TCL_ERROR; goto finally; }

finally:
	replace_tclobj(&bufdup,		NULL);
	replace_tclobj(&ev,			NULL);
	replace_tclobj(&cb,			NULL);
	replace_tclobj(&tmp,		NULL);
	return code;
}

//>>>

static struct cmd {
	const char*		name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{ NS_DRIVER_STR "::create_stream",		cmd_create_stream	},
	{ NS_DRIVER_STR "::dispatch_events",	cmd_dispatch_events	},
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

int Fsevents_Init(Tcl_Interp* interp) //<<<
{
	int				code = TCL_OK;
	Tcl_Namespace*	ns = NULL;
	Tcl_Obj*		script_fn = NULL;

	ns = Tcl_CreateNamespace(interp, NS_DRIVER_STR, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	// Set up interp_cx
	struct interp_cx* l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};
	Tcl_SetAssocData(interp, NS_DRIVER_STR, free_interp_cx, l);

	for (int i=0; i<L_size; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(litstrs[i], -1));

	Tcl_Size dummy;
	(void)Tcl_DictObjSize(NULL, l->lit[L_EV_TEMPLATE], &dummy);  // ensure the template obj is a dict for efficient dups

	// Register commands
	for (struct cmd* c = cmds; c->name; c++)
		Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);

	// Source driver scripts
	TEST_OK_LABEL(finally, code, pkgdir_path(interp, "watcher_fsevents.tcl", &script_fn));
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
int Fsevents_Unload(Tcl_Interp* interp) //<<<
{
	Tcl_Namespace* ns = Tcl_FindNamespace(interp, NS_DRIVER_STR, NULL, TCL_GLOBAL_ONLY);
	if (ns) Tcl_DeleteNamespace(ns);

	Tcl_SetAssocData(interp, NS_DRIVER_STR, NULL, NULL);

	return TCL_OK;
}

//>>>

// vim: ts=4 shiftwidth=4 noexpandtab foldmethod=marker foldmarker=<<<,>>>
