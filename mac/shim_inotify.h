// FSEvents / CoreFoundation shim for testing on Linux via inotify
// Only implements the subset that fsevents.c actually uses.
// CF strings and arrays are backed by Tcl_Objs (string / list).

#ifndef SHIM_INOTIFY_H
#define SHIM_INOTIFY_H

#include <tcl.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/uio.h>

// CoreFoundation type shims <<<

typedef const void*		CFAllocatorRef;
typedef double			CFTimeInterval;
typedef uint32_t		CFStringEncoding;
typedef long			CFIndex;
typedef unsigned char	Boolean;

// CFString and CFArray are backed by Tcl_Obj (string and list)
typedef Tcl_Obj*		CFStringRef;
typedef Tcl_Obj*		CFMutableArrayRef;
typedef Tcl_Obj*		CFArrayRef;

// CFRunLoop: event loop with control pipe (not a Tcl_Obj — has fd state)
typedef struct _CFRunLoop {
	int		running;
	int		control_pipe[2];	// [0]=read, [1]=write
	void*	stream;				// the FSEventStream scheduled on this loop
}* CFRunLoopRef;

typedef const void* CFRunLoopMode;

// CFArrayCallBacks (dummy — Tcl_Obj list handles everything)
typedef struct { int dummy; } CFArrayCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;

// >>>
// FSEvents type shims <<<

typedef uint32_t	FSEventStreamCreateFlags;
typedef uint32_t	FSEventStreamEventFlags;
typedef uint64_t	FSEventStreamEventId;

typedef struct _FSEventStream* FSEventStreamRef;
typedef const struct _FSEventStream* ConstFSEventStreamRef;

typedef void (*FSEventStreamCallback)(
	ConstFSEventStreamRef			streamRef,
	void*							clientCallBackInfo,
	size_t							numEvents,
	void*							eventPaths,
	const FSEventStreamEventFlags	eventFlags[],
	const FSEventStreamEventId		eventIds[]
);

typedef struct {
	CFIndex		version;
	void*		info;
	void*		retain;
	void*		release;
	void*		copyDescription;
} FSEventStreamContext;

struct _FSEventStream {
	int						inotify_fd;
	FSEventStreamCallback	callback;
	void*					callback_info;
	FSEventStreamCreateFlags create_flags;
	char**					watch_paths;
	int						num_watch_paths;
	char**					wd_to_path;		// indexed by watch descriptor
	int						wd_map_size;
	CFRunLoopRef			run_loop;
};

// >>>
// FSEvents constants <<<

#define kFSEventStreamEventIdSinceNow	UINT64_MAX

#define kFSEventStreamCreateFlagFileEvents	0x00000010
#define kFSEventStreamCreateFlagNoDefer		0x00000002
#define kFSEventStreamCreateFlagWatchRoot	0x00000004
#define kFSEventStreamCreateFlagIgnoreSelf	0x00000008

#define kFSEventStreamEventFlagItemCreated			0x00000100
#define kFSEventStreamEventFlagItemRemoved			0x00000200
#define kFSEventStreamEventFlagItemModified			0x00001000
#define kFSEventStreamEventFlagItemRenamed			0x00000800
#define kFSEventStreamEventFlagItemIsDir			0x00020000
#define kFSEventStreamEventFlagItemIsFile			0x00010000
#define kFSEventStreamEventFlagItemIsSymlink		0x00040000
#define kFSEventStreamEventFlagItemInodeMetaMod		0x00000400
#define kFSEventStreamEventFlagItemChangeOwner		0x00004000
#define kFSEventStreamEventFlagItemXattrMod			0x00008000

#define kCFStringEncodingUTF8	0x08000100

// >>>
// CoreFoundation function shims <<<

CFStringRef			CFStringCreateWithCString(CFAllocatorRef alloc, const char* str, CFStringEncoding encoding);
CFMutableArrayRef	CFArrayCreateMutable(CFAllocatorRef alloc, CFIndex capacity, const CFArrayCallBacks* callbacks);
void				CFArrayAppendValue(CFMutableArrayRef array, const void* value);
void				CFRelease(const void* cf);

CFRunLoopRef	CFRunLoopGetCurrent(void);
void			CFRunLoopRun(void);
void			CFRunLoopStop(CFRunLoopRef rl);

extern CFRunLoopMode kCFRunLoopDefaultMode;

// >>>
// FSEvents function shims <<<

FSEventStreamRef FSEventStreamCreate(
	CFAllocatorRef				allocator,
	FSEventStreamCallback		callback,
	FSEventStreamContext*		context,
	CFArrayRef					pathsToWatch,
	FSEventStreamEventId		sinceWhen,
	CFTimeInterval				latency,
	FSEventStreamCreateFlags	flags
);

void	FSEventStreamScheduleWithRunLoop(FSEventStreamRef stream, CFRunLoopRef rl, CFRunLoopMode mode);
Boolean	FSEventStreamStart(FSEventStreamRef stream);
void	FSEventStreamStop(FSEventStreamRef stream);
void	FSEventStreamInvalidate(FSEventStreamRef stream);
void	FSEventStreamRelease(FSEventStreamRef stream);

// >>>

#endif // SHIM_INOTIFY_H
// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4 noexpandtab
