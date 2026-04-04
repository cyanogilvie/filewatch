// Win32 API shim: implements the Win32 API surface that dirwatch.c uses,
// backed by Linux inotify.  For testing only.

#include "shim_inotify.h"

#include <sys/inotify.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Internal state for shimmed handles <<<

enum handle_type { H_EVENT, H_PIPE, H_DIR, H_THREAD };

struct shim_event {
	enum handle_type	type;
	int					pipe[2];	// signaling pipe: write to set, read to wait
	int					signaled;
	// For overlapped I/O: an associated fd to poll for readability
	int					assoc_fd;	// -1 if none
	struct shim_dir*	assoc_dir;	// dir handle for deferred ReadDirectoryChangesW
	BYTE*				assoc_buf;
	DWORD				assoc_bufsize;
};

struct shim_dir {
	enum handle_type	type;
	int					inotify_fd;
	char*				path;
	BOOL				recursive;
	char**				wd_to_path;
	int					wd_map_size;
	// Pending event data for GetOverlappedResult
	BYTE*				pending_buf;
	DWORD				pending_len;
};

struct shim_pipe {
	enum handle_type	type;
	int					fd;
};

struct shim_thread {
	enum handle_type	type;
	pthread_t			thread;
};

// >>>
// Internal helpers <<<

static void ensure_wd_map(struct shim_dir* sd, int wd) //<<<
{
	if (wd >= sd->wd_map_size) {
		int new_size = (wd + 1) * 2;
		sd->wd_to_path = realloc(sd->wd_to_path, new_size * sizeof(char*));
		memset(sd->wd_to_path + sd->wd_map_size, 0,
			(new_size - sd->wd_map_size) * sizeof(char*));
		sd->wd_map_size = new_size;
	}
}

//>>>
static void add_watch_recursive(struct shim_dir* sd, const char* path) //<<<
{
	uint32_t mask =
		IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
		IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
		IN_DELETE_SELF | IN_MOVE_SELF;

	int wd = inotify_add_watch(sd->inotify_fd, path, mask);
	if (wd < 0) return;

	ensure_wd_map(sd, wd);
	free(sd->wd_to_path[wd]);
	sd->wd_to_path[wd] = strdup(path);

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
		add_watch_recursive(sd, child);
	}
	closedir(d);
}

//>>>
static DWORD inotify_to_fni( //<<<
	struct shim_dir* sd, const char* buf, ssize_t len, BYTE* out, DWORD out_size)
{
	// Convert inotify events to FILE_NOTIFY_INFORMATION chain
	const char*	ptr = buf;
	const char*	end = buf + len;
	DWORD		offset = 0;

	while (ptr < end) {
		const struct inotify_event* ev = (const struct inotify_event*)ptr;
		size_t evsize = sizeof(*ev) + ev->len;
		if (ptr + evsize > end) break;
		ptr += evsize;

		if (ev->wd < 0 || ev->wd >= sd->wd_map_size || !sd->wd_to_path[ev->wd])
			continue;
		if (ev->len == 0) continue;	// directory-level event, no filename

		// Map inotify mask to FILE_ACTION
		DWORD action;
		if (ev->mask & IN_CREATE)		action = FILE_ACTION_ADDED;
		else if (ev->mask & IN_DELETE)	action = FILE_ACTION_REMOVED;
		else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB))
										action = FILE_ACTION_MODIFIED;
		else if (ev->mask & IN_MOVED_FROM) action = FILE_ACTION_RENAMED_OLD_NAME;
		else if (ev->mask & IN_MOVED_TO)   action = FILE_ACTION_RENAMED_NEW_NAME;
		else continue;

		// Build path relative to watched root: wd_path/name → relative from root
		char fullpath[PATH_MAX];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", sd->wd_to_path[ev->wd], ev->name);

		// Make relative to sd->path
		const char* relpath = fullpath;
		size_t root_len = strlen(sd->path);
		if (strncmp(fullpath, sd->path, root_len) == 0 && fullpath[root_len] == '/')
			relpath = fullpath + root_len + 1;

		// Convert to UTF-16 (just widen ASCII/UTF-8 bytes for the shim)
		size_t name_len = strlen(relpath);
		DWORD fni_size = (DWORD)(offsetof(FILE_NOTIFY_INFORMATION, FileName) + name_len * sizeof(WCHAR));

		if (offset + fni_size > out_size) break;

		FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)(out + offset);
		fni->NextEntryOffset = 0;
		fni->Action = action;
		fni->FileNameLength = (DWORD)(name_len * sizeof(WCHAR));
		for (size_t i = 0; i < name_len; i++)
			fni->FileName[i] = (WCHAR)(unsigned char)relpath[i];

		// Align to DWORD boundary
		DWORD entry_size = (DWORD)(offsetof(FILE_NOTIFY_INFORMATION, FileName) + name_len * sizeof(WCHAR));
		entry_size = (entry_size + 3) & ~3u;

		// Patch previous NextEntryOffset
		if (offset > 0) {
			// Walk chain to find the last entry and patch it
			DWORD prev_off = 0;
			while (1) {
				FILE_NOTIFY_INFORMATION* p = (FILE_NOTIFY_INFORMATION*)(out + prev_off);
				if (p->NextEntryOffset == 0) {
					p->NextEntryOffset = offset - prev_off;
					break;
				}
				prev_off += p->NextEntryOffset;
			}
		}

		offset += entry_size;

		// If new directory created, add recursive watches
		if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR))
			add_watch_recursive(sd, fullpath);
	}

	return offset;
}

//>>>
// >>>
// Win32 API shims <<<

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, //<<<
	LPSECURITY_ATTRIBUTES lpSecAttr, DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
	(void)dwDesiredAccess; (void)dwShareMode; (void)lpSecAttr;
	(void)dwCreationDisposition; (void)dwFlagsAndAttributes; (void)hTemplateFile;

	struct shim_dir* sd = calloc(1, sizeof(*sd));
	sd->type = H_DIR;
	sd->inotify_fd = inotify_init1(IN_CLOEXEC);
	if (sd->inotify_fd < 0) { free(sd); return INVALID_HANDLE_VALUE; }
	sd->path = strdup(lpFileName);
	return (HANDLE)sd;
}

//>>>
BOOL ReadDirectoryChangesW(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength, //<<<
	BOOL bWatchSubtree, DWORD dwNotifyFilter, LPDWORD lpBytesReturned,
	LPOVERLAPPED lpOverlapped, void* lpCompletionRoutine)
{
	(void)dwNotifyFilter; (void)lpBytesReturned; (void)lpCompletionRoutine;
	struct shim_dir* sd = (struct shim_dir*)hDirectory;
	sd->recursive = bWatchSubtree;
	sd->pending_buf = lpBuffer;
	sd->pending_len = 0;

	// Add watches on first call
	if (!sd->wd_to_path)
		add_watch_recursive(sd, sd->path);

	// Non-blocking: associate the inotify fd with the overlapped event
	// so WaitForMultipleObjects will poll it
	if (lpOverlapped && lpOverlapped->hEvent) {
		struct shim_event* se = (struct shim_event*)lpOverlapped->hEvent;
		se->assoc_fd = sd->inotify_fd;
		se->assoc_dir = sd;
		se->assoc_buf = (BYTE*)lpBuffer;
		se->assoc_bufsize = nBufferLength;
	}

	return TRUE;
}

//>>>
BOOL GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, //<<<
	LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
	(void)lpOverlapped; (void)bWait;
	struct shim_dir* sd = (struct shim_dir*)hFile;
	*lpNumberOfBytesTransferred = sd->pending_len;
	return TRUE;
}

//>>>
BOOL CreatePipe(HANDLE* hReadPipe, HANDLE* hWritePipe, //<<<
	LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize)
{
	(void)lpPipeAttributes; (void)nSize;
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) != 0) return FALSE;

	struct shim_pipe* r = calloc(1, sizeof(*r));
	r->type = H_PIPE;
	r->fd = pipefd[0];

	struct shim_pipe* w = calloc(1, sizeof(*w));
	w->type = H_PIPE;
	w->fd = pipefd[1];

	*hReadPipe = (HANDLE)r;
	*hWritePipe = (HANDLE)w;
	return TRUE;
}

//>>>
BOOL WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite, //<<<
	LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
	(void)lpOverlapped;
	struct shim_pipe* sp = (struct shim_pipe*)hFile;
	ssize_t n = write(sp->fd, lpBuffer, nNumberOfBytesToWrite);
	if (n < 0) return FALSE;
	if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = (DWORD)n;
	return TRUE;
}

//>>>
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, //<<<
	BOOL bInitialState, const void* lpName)
{
	(void)lpEventAttributes; (void)bManualReset; (void)bInitialState; (void)lpName;
	struct shim_event* se = calloc(1, sizeof(*se));
	se->type = H_EVENT;
	se->assoc_fd = -1;
	pipe2(se->pipe, O_CLOEXEC);
	return (HANDLE)se;
}

//>>>
BOOL SetEvent(HANDLE hEvent) //<<<
{
	struct shim_event* se = (struct shim_event*)hEvent;
	if (!se->signaled) {
		se->signaled = 1;
		char c = 'e';
		(void)write(se->pipe[1], &c, 1);
	}
	return TRUE;
}

//>>>
BOOL ResetEvent(HANDLE hEvent) //<<<
{
	struct shim_event* se = (struct shim_event*)hEvent;
	if (se->signaled) {
		se->signaled = 0;
		char c;
		(void)read(se->pipe[0], &c, 1);
	}
	return TRUE;
}

//>>>
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, //<<<
	BOOL bWaitAll, DWORD dwMilliseconds)
{
	(void)bWaitAll; (void)dwMilliseconds;

	// Build poll set: for each event, poll the pipe AND any associated fd
	struct pollfd fds[16];
	int event_idx[16];	// maps fds[] index → event index
	int nfds = 0;

	if (nCount > 8) nCount = 8;
	for (DWORD i = 0; i < nCount; i++) {
		struct shim_event* se = (struct shim_event*)lpHandles[i];
		// Poll the event's signaling pipe
		event_idx[nfds] = (int)i;
		fds[nfds++] = (struct pollfd){ .fd = se->pipe[0], .events = POLLIN };
		// Also poll any associated fd (inotify fd from ReadDirectoryChangesW)
		if (se->assoc_fd >= 0) {
			event_idx[nfds] = (int)i;
			fds[nfds++] = (struct pollfd){ .fd = se->assoc_fd, .events = POLLIN };
		}
	}

	int ret = poll(fds, nfds, -1);
	if (ret <= 0) return (DWORD)-1;

	for (int i = 0; i < nfds; i++) {
		if (!(fds[i].revents & POLLIN)) continue;

		int eidx = event_idx[i];
		struct shim_event* se = (struct shim_event*)lpHandles[eidx];

		// If this was the associated fd (inotify), read and convert data
		if (se->assoc_fd >= 0 && fds[i].fd == se->assoc_fd && se->assoc_dir) {
			char inotify_buf[sizeof(struct inotify_event) * 64 + NAME_MAX * 64];
			ssize_t len = read(se->assoc_fd, inotify_buf, sizeof(inotify_buf));
			if (len > 0) {
				se->assoc_dir->pending_len = inotify_to_fni(
					se->assoc_dir, inotify_buf, len,
					se->assoc_buf, se->assoc_bufsize);
			}
		}

		return WAIT_OBJECT_0 + eidx;
	}
	return (DWORD)-1;
}

//>>>
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) //<<<
{
	(void)dwMilliseconds;
	enum handle_type type = *(enum handle_type*)hHandle;
	if (type == H_THREAD) {
		struct shim_thread* st = (struct shim_thread*)hHandle;
		pthread_join(st->thread, NULL);
		return WAIT_OBJECT_0;
	}
	return WaitForMultipleObjects(1, &hHandle, FALSE, dwMilliseconds);
}

//>>>
BOOL CloseHandle(HANDLE hObject) //<<<
{
	if (!hObject || hObject == INVALID_HANDLE_VALUE) return FALSE;
	enum handle_type type = *(enum handle_type*)hObject;

	switch (type) {
		case H_EVENT: {
			struct shim_event* se = (struct shim_event*)hObject;
			close(se->pipe[0]);
			close(se->pipe[1]);
			free(se);
			break;
		}
		case H_PIPE: {
			struct shim_pipe* sp = (struct shim_pipe*)hObject;
			if (sp->fd >= 0) close(sp->fd);
			free(sp);
			break;
		}
		case H_DIR: {
			struct shim_dir* sd = (struct shim_dir*)hObject;
			close(sd->inotify_fd);
			for (int i = 0; i < sd->wd_map_size; i++)
				free(sd->wd_to_path[i]);
			free(sd->wd_to_path);
			free(sd->path);
			free(sd);
			break;
		}
		case H_THREAD: {
			struct shim_thread* st = (struct shim_thread*)hObject;
			free(st);
			break;
		}
	}
	return TRUE;
}

//>>>
struct thread_start_ctx {
	DWORD (WINAPI *proc)(LPVOID);
	LPVOID param;
};

static void* thread_start_wrapper(void* arg) //<<<
{
	struct thread_start_ctx ctx = *(struct thread_start_ctx*)arg;
	free(arg);
	ctx.proc(ctx.param);
	return NULL;
}

//>>>
HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, size_t dwStackSize, //<<<
	DWORD (WINAPI *lpStartAddress)(LPVOID), LPVOID lpParameter,
	DWORD dwCreationFlags, DWORD* lpThreadId)
{
	(void)lpThreadAttributes; (void)dwStackSize; (void)dwCreationFlags; (void)lpThreadId;

	struct thread_start_ctx* ctx = malloc(sizeof(*ctx));
	ctx->proc = lpStartAddress;
	ctx->param = lpParameter;

	struct shim_thread* st = calloc(1, sizeof(*st));
	st->type = H_THREAD;

	if (pthread_create(&st->thread, NULL, thread_start_wrapper, ctx) != 0) {
		free(ctx);
		free(st);
		return NULL;
	}
	return (HANDLE)st;
}

//>>>
int WideCharToMultiByte(unsigned int CodePage, DWORD dwFlags, //<<<
	const WCHAR* lpWideCharStr, int cchWideChar,
	char* lpMultiByteStr, int cbMultiByte,
	const char* lpDefaultChar, BOOL* lpUsedDefaultChar)
{
	(void)CodePage; (void)dwFlags; (void)lpDefaultChar; (void)lpUsedDefaultChar;

	// Simple narrow: the shim's "UTF-16" is just widened ASCII
	int len = cchWideChar;
	if (len < 0) {
		for (len = 0; lpWideCharStr[len]; len++) {}
	}

	if (!lpMultiByteStr || cbMultiByte == 0)
		return len;

	int copy = len < cbMultiByte ? len : cbMultiByte;
	for (int i = 0; i < copy; i++)
		lpMultiByteStr[i] = (char)(unsigned char)lpWideCharStr[i];

	return copy;
}

//>>>
// >>>

// vim: ts=4 shiftwidth=4 noexpandtab foldmethod=marker foldmarker=<<<,>>>
