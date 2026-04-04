// FSEvents / CoreFoundation shim: implements the FSEvents API surface
// that fsevents.c uses, backed by Linux inotify.  For testing only.
// CF strings and arrays are Tcl_Objs (string / list).

#include "shim_inotify.h"

#include <sys/inotify.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Globals <<<

const CFArrayCallBacks	kCFTypeArrayCallBacks = {0};
CFRunLoopMode			kCFRunLoopDefaultMode = NULL;

static __thread struct _CFRunLoop* g_tls_runloop = NULL;

// >>>
// CoreFoundation shims — backed by Tcl_Obj <<<

CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char* str, CFStringEncoding encoding) //<<<
{
	(void)alloc; (void)encoding;
	Tcl_Obj* obj = Tcl_NewStringObj(str, -1);
	Tcl_IncrRefCount(obj);
	return obj;
}

//>>>
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef alloc, CFIndex capacity, const CFArrayCallBacks* callbacks) //<<<
{
	(void)alloc; (void)capacity; (void)callbacks;
	Tcl_Obj* obj = Tcl_NewListObj(0, NULL);
	Tcl_IncrRefCount(obj);
	return obj;
}

//>>>
void CFArrayAppendValue(CFMutableArrayRef array, const void* value) //<<<
{
	Tcl_ListObjAppendElement(NULL, array, (Tcl_Obj*)value);
}

//>>>
void CFRelease(const void* cf) //<<<
{
	if (cf) Tcl_DecrRefCount((Tcl_Obj*)cf);
}

//>>>
CFRunLoopRef CFRunLoopGetCurrent(void) //<<<
{
	if (!g_tls_runloop) {
		g_tls_runloop = calloc(1, sizeof(*g_tls_runloop));
		pipe(g_tls_runloop->control_pipe);
	}
	return g_tls_runloop;
}

//>>>
// >>>
// Internal helpers <<<

static void ensure_wd_map(FSEventStreamRef stream, int wd) //<<<
{
	if (wd < stream->wd_map_size && stream->wd_to_path[wd]) return;

	if (wd >= stream->wd_map_size) {
		int new_size = (wd + 1) * 2;
		stream->wd_to_path = realloc(stream->wd_to_path, new_size * sizeof(char*));
		memset(stream->wd_to_path + stream->wd_map_size, 0,
			(new_size - stream->wd_map_size) * sizeof(char*));
		stream->wd_map_size = new_size;
	}
}

//>>>
static void add_watch_recursive(FSEventStreamRef stream, const char* path) //<<<
{
	uint32_t mask =
		IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
		IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
		IN_DELETE_SELF | IN_MOVE_SELF;

	int wd = inotify_add_watch(stream->inotify_fd, path, mask);
	if (wd < 0) return;

	ensure_wd_map(stream, wd);
	free(stream->wd_to_path[wd]);
	stream->wd_to_path[wd] = strdup(path);

	DIR* d = opendir(path);
	if (!d) return;

	struct dirent* de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
			(de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;
		if (de->d_type != DT_DIR) continue;

		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		add_watch_recursive(stream, child);
	}
	closedir(d);
}

//>>>
static void translate_and_callback(FSEventStreamRef stream, const char* buf, ssize_t len) //<<<
{
	const char* ptr = buf;
	const char* end = buf + len;

	char*						paths[256];
	FSEventStreamEventFlags		flags[256];
	FSEventStreamEventId		ids[256];
	int							count = 0;

	while (ptr < end && count < 256) {
		const struct inotify_event* ev = (const struct inotify_event*)ptr;
		size_t evsize = sizeof(*ev) + ev->len;
		if (ptr + evsize > end) break;
		ptr += evsize;

		if (ev->wd < 0 || ev->wd >= stream->wd_map_size || !stream->wd_to_path[ev->wd])
			continue;

		char fullpath[PATH_MAX];
		if (ev->len > 0)
			snprintf(fullpath, sizeof(fullpath), "%s/%s", stream->wd_to_path[ev->wd], ev->name);
		else
			snprintf(fullpath, sizeof(fullpath), "%s", stream->wd_to_path[ev->wd]);

		FSEventStreamEventFlags f = 0;
		if (ev->mask & IN_CREATE)		f |= kFSEventStreamEventFlagItemCreated;
		if (ev->mask & IN_DELETE)		f |= kFSEventStreamEventFlagItemRemoved;
		if (ev->mask & IN_MODIFY)		f |= kFSEventStreamEventFlagItemModified;
		if (ev->mask & IN_CLOSE_WRITE)	f |= kFSEventStreamEventFlagItemModified;
		if (ev->mask & IN_MOVED_FROM)	f |= kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemRemoved;
		if (ev->mask & IN_MOVED_TO)		f |= kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemCreated;
		if (ev->mask & IN_ATTRIB)		f |= kFSEventStreamEventFlagItemInodeMetaMod;
		if (ev->mask & IN_ISDIR)		f |= kFSEventStreamEventFlagItemIsDir;
		else							f |= kFSEventStreamEventFlagItemIsFile;

		paths[count] = strdup(fullpath);
		flags[count] = f;
		ids[count] = 0;
		count++;

		if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR))
			add_watch_recursive(stream, fullpath);
	}

	if (count > 0) {
		stream->callback(stream, stream->callback_info, count, paths, flags, ids);
		for (int i = 0; i < count; i++)
			free(paths[i]);
	}
}

//>>>
// >>>
// FSEvents API shims <<<

FSEventStreamRef FSEventStreamCreate( //<<<
	CFAllocatorRef				allocator,
	FSEventStreamCallback		callback,
	FSEventStreamContext*		context,
	CFArrayRef					pathsToWatch,
	FSEventStreamEventId		sinceWhen,
	CFTimeInterval				latency,
	FSEventStreamCreateFlags	flags
) {
	(void)allocator; (void)sinceWhen; (void)latency;

	FSEventStreamRef stream = calloc(1, sizeof(*stream));
	stream->inotify_fd = inotify_init1(IN_CLOEXEC);
	if (stream->inotify_fd < 0) { free(stream); return NULL; }

	stream->callback = callback;
	stream->callback_info = context ? context->info : NULL;
	stream->create_flags = flags;

	// pathsToWatch is a Tcl list of Tcl string objects
	Tcl_Size pathc;
	Tcl_ListObjLength(NULL, pathsToWatch, &pathc);

	stream->num_watch_paths = pathc;
	stream->watch_paths = calloc(pathc, sizeof(char*));
	for (Tcl_Size i = 0; i < pathc; i++) {
		Tcl_Obj* elem;
		Tcl_ListObjIndex(NULL, pathsToWatch, i, &elem);
		const char* path = Tcl_GetString(elem);
		stream->watch_paths[i] = strdup(path);
		add_watch_recursive(stream, path);
	}

	return stream;
}

//>>>
void FSEventStreamScheduleWithRunLoop(FSEventStreamRef stream, CFRunLoopRef rl, CFRunLoopMode mode) //<<<
{
	(void)mode;
	stream->run_loop = rl;
	rl->stream = stream;
}

//>>>
Boolean FSEventStreamStart(FSEventStreamRef stream) //<<<
{
	(void)stream;
	return 1;
}

//>>>
void CFRunLoopRun(void) //<<<
{
	CFRunLoopRef rl = CFRunLoopGetCurrent();
	FSEventStreamRef stream = (FSEventStreamRef)rl->stream;
	if (!stream) return;

	rl->running = 1;
	char buf[sizeof(struct inotify_event) * 256 + NAME_MAX * 256];

	while (rl->running) {
		struct pollfd fds[2] = {
			{ .fd = stream->inotify_fd,		.events = POLLIN },
			{ .fd = rl->control_pipe[0],	.events = POLLIN },
		};

		int nfds = poll(fds, 2, -1);
		if (nfds <= 0) continue;

		if (fds[1].revents & POLLIN) break;

		if (fds[0].revents & POLLIN) {
			ssize_t len = read(stream->inotify_fd, buf, sizeof(buf));
			if (len > 0)
				translate_and_callback(stream, buf, len);
		}
	}
}

//>>>
void CFRunLoopStop(CFRunLoopRef rl) //<<<
{
	rl->running = 0;
	char c = 'x';
	(void)write(rl->control_pipe[1], &c, 1);
}

//>>>
void FSEventStreamStop(FSEventStreamRef stream) //<<<
{
	(void)stream;
}

//>>>
void FSEventStreamInvalidate(FSEventStreamRef stream) //<<<
{
	(void)stream;
}

//>>>
void FSEventStreamRelease(FSEventStreamRef stream) //<<<
{
	if (!stream) return;

	if (stream->inotify_fd >= 0) {
		close(stream->inotify_fd);
		stream->inotify_fd = -1;
	}

	for (int i = 0; i < stream->wd_map_size; i++)
		free(stream->wd_to_path[i]);
	free(stream->wd_to_path);

	for (int i = 0; i < stream->num_watch_paths; i++)
		free(stream->watch_paths[i]);
	free(stream->watch_paths);

	if (g_tls_runloop) {
		close(g_tls_runloop->control_pipe[0]);
		close(g_tls_runloop->control_pipe[1]);
		free(g_tls_runloop);
		g_tls_runloop = NULL;
	}

	free(stream);
}

//>>>
// >>>

// vim: ts=4 shiftwidth=4 noexpandtab foldmethod=marker foldmarker=<<<,>>>
