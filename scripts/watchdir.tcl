oo::class create ::inotify::watchdir {
	variable {*}{
		queue
		files
		maxsize
	}

	constructor dir { #<<<
		if {[self next] ne ""} next
		if {![info exists maxsize]} {
			set maxsize	1048576
		}
		set files	{}
		set queue	[inotify::queue new [namespace code {my _fs_event}]]
		my _add_watches $dir
	}

	#>>>
	destructor { #<<<
		if {[info exists queue] && [info object isa object $queue]} {
			$queue destroy
			unset queue
		}
		if {[self next] ne ""} next
	}

	#>>>
	method _fs_event ev { #<<<
		set fqpath	[file join [dict get $ev path] [dict get $ev name]]
		set mask	[dict get $ev mask]
		if {"IN_ISDIR" in $mask} {
			if {"IN_CREATE" in $mask || "IN_MOVED_TO" in $mask} {
				my _add_watches $fqpath
			} elseif {"IN_MOVE_SELF" in $mask} {
				my _deep_remove $fqpath
			}
		} else {
			if {"IN_MOVED_TO" in $mask || "IN_CLOSE_WRITE" in $mask} {
				my _new_file $fqpath
			} elseif {"IN_DELETE" in $mask || "IN_MOVED_FROM" in $mask} {
				my _remove_file $fqpath
			}
		}
	}

	#>>>
	method _add_watches dir { #<<<
		$queue add_watch $dir {
			IN_CLOSE_WRITE
			IN_CREATE
			IN_DELETE
			IN_DELETE_SELF
			IN_MOVE_SELF
			IN_MOVED_FROM
			IN_MOVED_TO
		}
		foreach subdir [glob -nocomplain -type d [file join $dir *]] {
			my _add_watches $subdir
		}
		foreach file [glob -nocomplain -type f [file join $dir *]] {
			my _new_file $file
		}
	}

	#>>>
	method _new_file fqpath { #<<<
		if {![file readable $fqpath]} {
			#puts stderr "New file is not readable: \"$fqpath\""
			return
		}
		if {[file size $fqpath] > $maxsize} {
			#puts stderr "New file is bigger than the 1 MiB threshold, ignoring: \"$fqpath\""
			return
		}
		dict set files $fqpath 1
		my new_file [my normalize_path $fqpath]
	}

	#>>>
	method _remove_file fqpath { #<<<
		if {[dict exists $files $fqpath]} {
			dict unset files $fqpath
			my remove_file [my normalize_path $fqpath]
		}
	}

	#>>>
	method _deep_remove fqpath { #<<<
		foreach key [dict keys $files [file join $fqpath *]] {
			my _remove_file $fqpath
		}
	}

	#>>>

	# Override these methods as needed to do something useful with the file events
	method new_file normpath { #<<<
	}

	#>>>
	method remove_file normpath { #<<<
	}

	#>>>
	method normalize_path fqpath { #<<<
		set fqpath
	}

	#>>>
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
