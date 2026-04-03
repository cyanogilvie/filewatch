# NAME

inotify — Listen for events on files and directories

# SYNOPSIS

**inotify::queue create** *objectName* *callback*  
**inotify::queue new** *callback*  
*objectName* **add_watch** *path* ?*mask*?  
*objectName* **rm_watch** *path*  
*objectName* **destroy**

**inotify::watchdir create** *objectName* *dir*  
**inotify::watchdir new** *dir*  
*objectName* **destroy**

# DESCRIPTION

The Linux kernel (since 2.6.13) provides a mechanism to listen for
events that happen on files and directories, without needing to poll for
updates. This Tcl extension implements a wrapper to allow Tcl programs
to access this facility.

# WATCHING FILES VS DIRECTORIES

When a watch is placed on an ordinary file, the events it generates
relate directly to the watched file itself. If, however, the path being
watched is a directory, then in addition to receiving events about the
directory itself, it also generates events for its direct children (but
not recursively for the whole subtree under the directory). In this case
the event generated contains a **name** field which identifies the
relative name of the child under the watched directory that generated
the event.

# MASKS

Masks are a mechanism that the inotify syscalls provide to allow
application programmers to express which event types they are interested
in receiving. In the Tcl wrapper, masks are represented as lists, with
an element for each flag that can be set in the mask.

The following flags are valid to specify to an **add_watch** call, and
may be set on an event returned by the system. The **Scope** column
indicates whether the flag applies to the watched path itself (**s**),
to children of the watched path (**c**), or both.

| Flag                 | Scope | Description                                                           |
|----------------------|-------|-----------------------------------------------------------------------|
| **IN_ACCESS**        | s c   | File was accessed (read).                                             |
| **IN_ATTRIB**        | s c   | Metadata changed (permissions, timestamps, extended attributes, etc). |
| **IN_CLOSE_WRITE**   | s c   | File opened for writing was closed.                                   |
| **IN_CLOSE_NOWRITE** | s c   | File not opened for writing was closed.                               |
| **IN_CREATE**        | c     | File/directory created in watched directory.                          |
| **IN_DELETE**        | c     | File/directory deleted from watched directory.                        |
| **IN_DELETE_SELF**   | s     | Watched file/directory was itself deleted.                            |
| **IN_MODIFY**        | s c   | File was modified.                                                    |
| **IN_MOVE_SELF**     | s     | Watched file/directory was itself moved.                              |
| **IN_MOVED_FROM**    | c     | File moved out of watched directory.                                  |
| **IN_MOVED_TO**      | c     | File moved into watched directory.                                    |
| **IN_OPEN**          | s c   | File was opened.                                                      |

There are three convenience flags: **IN_ALL_EVENTS**, which selects all
the above flags; **IN_MOVE**, which is equivalent to **{IN_MOVED_FROM
IN_MOVED_TO}**; and **IN_CLOSE**, which is equivalent to
**{IN_CLOSE_WRITE IN_CLOSE_NOWRITE}**.

The following additional flags are valid in the *mask* parameter when
calling **add_watch**:

| Flag               | Description                                                                                                   |
|--------------------|---------------------------------------------------------------------------------------------------------------|
| **IN_DONT_FOLLOW** | If the watched path is a symbolic link, watch the link itself rather than the path it points to.              |
| **IN_MASK_ADD**    | If this path is already watched, add these flags to those already set, otherwise replace what was set before. |
| **IN_ONESHOT**     | Watch this path for one event, then remove it from the watch list.                                            |
| **IN_ONLYDIR**     | Only watch this path if it is a directory.                                                                    |

In the **mask** field returned when an event occurs, the following flags
may be set in addition to those in the first table above:

| Flag              | Description                                                                                                                       |
|-------------------|-----------------------------------------------------------------------------------------------------------------------------------|
| **IN_IGNORED**    | The watch was removed, either explicitly with rm_watch, or because the watched path was deleted, or the filesystem was unmounted. |
| **IN_ISDIR**      | The subject of this event is a directory.                                                                                         |
| **IN_Q_OVERFLOW** | Event queue overflowed (too many events occurred before they could be processed). See `/proc/sys/fs/inotify/max_queued_events`.   |
| **IN_UNMOUNT**    | The filesystem containing the watched path was unmounted.                                                                         |

# QUEUES

Queues are first in, first out buffers of events. One queue can (and is
intended to have) many watches that feed it. Ordering of events is
guaranteed within a queue.

If two or more identical events occur in sequence in a queue, they are
collapsed into one.

When a queue is deleted, all watches registered for it are removed.

# inotify::queue CLASS

The **inotify::queue** class wraps a raw inotify file descriptor in a
TclOO object that delivers events asynchronously through a callback.

**inotify::queue create** *objectName* *callback*  
Create a queue object named *objectName*.

**inotify::queue new** *callback*  
Create a queue object with an auto-generated name.

Both forms return the name of the queue object created. The *callback*
will be invoked with each event that arrives on the queue, with the
event details appended as a dictionary argument:

> *callback event-details*

The dictionary *event-details* contains the following keys:

| Key        | Description                                                                                                                      |
|------------|----------------------------------------------------------------------------------------------------------------------------------|
| **path**   | The path of the file/directory that the watch was registered on.                                                                 |
| **mask**   | A list of the flags that identify what the event is, from the tables above.                                                      |
| **cookie** | Related events (such as a rename) share a unique cookie value.                                                                   |
| **name**   | When the watch is on a directory and an event occurs on one of its children, this field contains the relative name of the child. |

*objectName* **add_watch** *path* ?*mask*?  
Add *path* to the list of paths watched in this queue, or update the
flags of an existing watch if this path was already watched. If *mask*
is not specified, it defaults to **IN_ALL_EVENTS**, otherwise it must be
a list containing flags from the event and add_watch flag tables above.

*objectName* **rm_watch** *path*  
Remove a path from the list of watched paths. If *path* is not watched
then an error is raised.

*objectName* **destroy**  
Delete the queue. All watches feeding this queue are removed and the
underlying channel is closed.

# inotify::watchdir CLASS

The **inotify::watchdir** class recursively watches a directory tree for
file creation and deletion. It is designed to be subclassed — override
the **new_file** and **remove_file** methods to respond to events. It
uses an **inotify::queue** internally to manage watches.

When constructed, it scans the directory tree for existing files
(calling **new_file** for each) and installs watches on every
subdirectory. New subdirectories created after construction are
automatically watched.

Files larger than *maxsize* bytes (default 1048576, i.e. 1 MiB) are
ignored. To change the limit, set the *maxsize* variable before calling
**next** in a subclass constructor.

**inotify::watchdir create** *objectName* *dir*  
Create a watchdir object named *objectName* watching *dir*.

**inotify::watchdir new** *dir*  
Create a watchdir object with an auto-generated name watching *dir*.

*objectName* **destroy**  
Stop watching and clean up. The internal queue and all watches are
destroyed.

## Methods for subclass override

The following methods are called by the watchdir when file events occur.
The default implementations are no-ops. Override them in a subclass to
take action.

*objectName* **new_file** *normpath*  
Called when a file is created or written (**IN_CLOSE_WRITE**,
**IN_MOVED_TO**). *normpath* is the full path after **normalize_path**
processing.

*objectName* **remove_file** *normpath*  
Called when a file is deleted or moved away (**IN_DELETE**,
**IN_MOVED_FROM**). *normpath* is the full path after **normalize_path**
processing.

*objectName* **normalize_path** *fqpath*  
Called to transform a fully qualified path before it is passed to
**new_file** or **remove_file**. The default implementation returns
*fqpath* unchanged. Override to apply project-specific path
normalization.

# EXAMPLE

Create a watch on the /tmp directory and print out any events as they
arrive:

``` tcl
% package require inotify
2.3
% proc something_happened {event_details} {
    puts "Event happened:"
    dict for {k v} $event_details {
        puts [format "  %-10s %s" $k $v]
    }
}
% inotify::queue create queue something_happened
queue
% queue add_watch /tmp
/tmp
% vwait ::forever

 ... another process does a "touch /tmp/foo" ...

Event happened:
  path       /tmp
  mask       IN_CREATE
  cookie     0
  name       foo
Event happened:
  path       /tmp
  mask       IN_OPEN
  cookie     0
  name       foo
Event happened:
  path       /tmp
  mask       IN_ATTRIB
  cookie     0
  name       foo
Event happened:
  path       /tmp
  mask       IN_CLOSE_WRITE
  cookie     0
  name       foo
```

Subclass **inotify::watchdir** to react to files appearing in a
directory tree:

``` tcl
package require inotify

oo::class create my_watcher {
    superclass inotify::watchdir

    method new_file normpath {
        puts "New file: $normpath"
    }

    method remove_file normpath {
        puts "Removed: $normpath"
    }
}

my_watcher create watcher /path/to/watched/dir
vwait ::forever
```

# LIMITS

The kernel provides three knobs to tune the memory limits of the inotify
system:

**/proc/sys/fs/inotify/max_queued_events**  
The number of events that will be queued up by the kernel for a given
queue before the **IN_Q_OVERFLOW** event is generated and events are
dropped.

**/proc/sys/fs/inotify/max_user_instances**  
This limits the number of queues that can be created by a user.

**/proc/sys/fs/inotify/max_user_watches**  
This is the limit of the number of watches that can be associated with a
single queue.

# BUILDING

This package requires Tcl 8.6 or 9.0 and a Linux system (inotify is a
Linux kernel API). The build system is `meson`, which will find Tcl
using its pkg-config file. Use the `PKG_CONFIG_PATH` environment
variable to point meson to it if Tcl is installed in a nonstandard
location.

## From a Release Tarball

Download and extract [the
release](https://github.com/cyanogilvie/inotify/releases), then:

``` sh
tar xf inotify-v2.3.tar.gz
cd inotify-v2.3
meson setup builddir --buildtype=release
meson compile -C builddir
meson install -C builddir
```

## From the Git Sources

Fetch [the code](https://github.com/cyanogilvie/inotify) and submodules
recursively, then build:

``` sh
git clone --recurse-submodules https://github.com/cyanogilvie/inotify
cd inotify
meson setup builddir --buildtype=release
meson compile -C builddir
meson install -C builddir
```

## Testing

Run the test suite after building:

``` sh
meson test -C builddir
```

# PLATFORM

This package uses the Linux inotify API and is only supported on Linux.

# LICENSE

This package is Copyright 2007-2026 Cyan Ogilvie, and is made available
under the same license terms as the Tcl Core.

# SEE ALSO

inotify(7), Linux kernel inotify documentation

# KEYWORDS

file, notification, event, inotify, tcl
