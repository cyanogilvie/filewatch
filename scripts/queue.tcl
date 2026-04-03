oo::class create ::inotify::queue {
	variable {*}{
		cb
		queue_handle
		wd_map
		path_map
	}

	constructor a_cb { #<<<
		set cb			$a_cb
		set wd_map		{}
		set path_map	{}

		set queue_handle	[inotify::create_queue]

		chan configure $queue_handle \
				-blocking		0 \
				-buffering		none \
				-translation	binary

		coroutine consumer my _readable
		chan event $queue_handle readable [namespace code consumer]
	}

	#>>>
	destructor { #<<<
		if {[info exists queue_handle]} {
			dict for {wd path} $wd_map {
				try {
					inotify::rm_watch $queue_handle $wd
				} on error {errmsg options} {
					my log error "Error removing watch on \"$path\": $errmsg"
				}
				dict unset wd_map $wd
			}
			try {
				if {
					[info exists queue_handle] &&
					$queue_handle in [chan names]
				} {
					close $queue_handle
				}
			} on error {errmsg options} {
				my log error "Error closing inotify queue handle: $errmsg"
			}
		}
	}

	#>>>

	method add_watch {path {mask {IN_ALL_EVENTS}}} { #<<<
		set wd	[inotify::add_watch $queue_handle $path $mask]
		dict set path_map $path	$wd
		dict set wd_map $wd		$path
	}

	#>>>
	method rm_watch path { #<<<
		if {![dict exists $path_map $path]} {
			throw [list no_watches $path] "No watches on path \"$path\""
		}
		set wd	[dict get $path_map $path]
		inotify::rm_watch $queue_handle $wd
		dict unset path_map $path
		dict unset wd_map $wd
	}

	#>>>

	method _readable {} { #<<<
		while 1 {
			yield

			if {[eof $queue_handle]} {
				my log error "Queue handle closed"
				close $queue_handle
				unset queue_handle
				my destroy
				return
			}

			foreach event [inotify::decode_events [read $queue_handle]] {
				set wd	[lindex $event 0]
				if {$wd == -1} {
					# This happens when we get a synthetic IN_Q_OVERFLOW event
					set event	[lreplace $event 0 0 {}]
				} elseif {![dict exists $wd_map $wd]} {
					# IN_IGNORED is the kernel confirming a watch removal — expected after rm_watch
					if {"IN_IGNORED" ni [lindex $event 1]} {
						my log error "No path map for watch descriptor ($wd)"
					}
					continue
				} else {
					set event	[lreplace $event 0 0 [dict get $wd_map $wd]]
				}

				set evdict	{}
				foreach field {path mask cookie name} value $event {
					lappend evdict $field $value
				}

				if {[info exists cb]} {
					try {
						uplevel #0 $cb [list $evdict]
					} on error {errmsg options} {
						my log error "Error invoking callback for event ($evdict): $errmsg\n[dict get $options -errorinfo]"
					}
				}
			}
		}
	}

	#>>>
	method log {lvl msg} { #<<<
		puts stderr $msg
	}

	unexport log
	#>>>
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
