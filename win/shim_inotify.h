// Win32 API shim for testing dirwatch on Linux via inotify
// Only implements the subset that dirwatch.c actually uses.

#ifndef SHIM_INOTIFY_WIN_H
#define SHIM_INOTIFY_WIN_H

#include <tcl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

// Win32 basic types <<<

typedef void*			HANDLE;
typedef int				BOOL;
typedef unsigned long	DWORD;
typedef unsigned char	BYTE;
typedef void*			LPVOID;
typedef unsigned short	WCHAR;
typedef DWORD*			LPDWORD;
typedef void*			LPSECURITY_ATTRIBUTES;
typedef const char*		LPCSTR;

#define TRUE	1
#define FALSE	0
#define INVALID_HANDLE_VALUE	((HANDLE)(intptr_t)-1)
#define INFINITE				0xFFFFFFFF
#define WAIT_OBJECT_0			0
#define MAX_PATH				260
#define CP_UTF8					65001

// >>>
// OVERLAPPED <<<

typedef struct _OVERLAPPED {
	HANDLE	hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef DWORD (WINAPI_FUNC)(LPVOID);
#define WINAPI

// >>>
// File notification constants <<<

#define FILE_LIST_DIRECTORY			0x0001
#define FILE_SHARE_READ				0x0001
#define FILE_SHARE_WRITE			0x0002
#define FILE_SHARE_DELETE			0x0004
#define OPEN_EXISTING				3
#define FILE_FLAG_BACKUP_SEMANTICS	0x02000000
#define FILE_FLAG_OVERLAPPED		0x40000000

#define FILE_NOTIFY_CHANGE_FILE_NAME	0x0001
#define FILE_NOTIFY_CHANGE_DIR_NAME		0x0002
#define FILE_NOTIFY_CHANGE_ATTRIBUTES	0x0004
#define FILE_NOTIFY_CHANGE_SIZE			0x0008
#define FILE_NOTIFY_CHANGE_LAST_WRITE	0x0010
#define FILE_NOTIFY_CHANGE_CREATION		0x0040
#define FILE_NOTIFY_CHANGE_SECURITY		0x0100

#define FILE_ACTION_ADDED				1
#define FILE_ACTION_REMOVED				2
#define FILE_ACTION_MODIFIED			3
#define FILE_ACTION_RENAMED_OLD_NAME	4
#define FILE_ACTION_RENAMED_NEW_NAME	5

// >>>
// FILE_NOTIFY_INFORMATION <<<

typedef struct _FILE_NOTIFY_INFORMATION {
	DWORD	NextEntryOffset;
	DWORD	Action;
	DWORD	FileNameLength;		// in bytes, not characters
	WCHAR	FileName[1];
} FILE_NOTIFY_INFORMATION;

// >>>
// Win32 function shims <<<

HANDLE	CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
			LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
			DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL	ReadDirectoryChangesW(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength,
			BOOL bWatchSubtree, DWORD dwNotifyFilter, LPDWORD lpBytesReturned,
			LPOVERLAPPED lpOverlapped, void* lpCompletionRoutine);
BOOL	GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped,
			LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
BOOL	CreatePipe(HANDLE* hReadPipe, HANDLE* hWritePipe,
			LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize);
BOOL	WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
			LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
HANDLE	CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
			BOOL bInitialState, const void* lpName);
BOOL	SetEvent(HANDLE hEvent);
BOOL	ResetEvent(HANDLE hEvent);
DWORD	WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles,
			BOOL bWaitAll, DWORD dwMilliseconds);
DWORD	WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
BOOL	CloseHandle(HANDLE hObject);
HANDLE	CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, size_t dwStackSize,
			DWORD (WINAPI *lpStartAddress)(LPVOID), LPVOID lpParameter,
			DWORD dwCreationFlags, DWORD* lpThreadId);
int		WideCharToMultiByte(unsigned int CodePage, DWORD dwFlags,
			const WCHAR* lpWideCharStr, int cchWideChar,
			char* lpMultiByteStr, int cbMultiByte,
			const char* lpDefaultChar, BOOL* lpUsedDefaultChar);

// >>>
// Tcl_MakeFileChannel bridge <<<
// On real Windows, Tcl_MakeFileChannel takes a HANDLE.
// On Linux, it takes INT2PTR(fd).  Bridge the shim pipe handles.

// Must come after Tcl headers so we can undef the stubs macro
#undef Tcl_MakeFileChannel
static inline Tcl_Channel Shim_MakeFileChannel(HANDLE h, int mode) {
	// Extract the raw fd from a shim pipe handle and free the wrapper
	struct { int type; int fd; }* sp = (void*)h;
	int fd = sp->fd;
	sp->fd = -1;	// prevent CloseHandle from closing the fd
	free(sp);		// free the shim_pipe wrapper
	return tclStubsPtr->tcl_MakeFileChannel((void*)(intptr_t)fd, mode);
}
#define Tcl_MakeFileChannel(h, m) Shim_MakeFileChannel(h, m)

// >>>

#endif // SHIM_INOTIFY_WIN_H
// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4 noexpandtab
