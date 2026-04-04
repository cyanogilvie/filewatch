# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4

oo::class create ::filewatch::dirwatch::watcher {
	variable {*}{
		cb
		watch_chan
	}

	constructor {callback args} { #<<<
		# args: ?-recursive bool? ?-filter filterlist? path
		set cb			$callback
		set watch_chan	[::filewatch::dirwatch::create_watch {*}$args]

		chan configure $watch_chan \
				-blocking		0 \
				-buffering		none \
				-translation	binary

		coroutine consumer my _readable
		chan event $watch_chan readable [namespace code consumer]
	}

	#>>>
	destructor { #<<<
		if {[info exists watch_chan]} {
			try {
				if {$watch_chan in [chan names]} {
					close $watch_chan
				}
			} on error {errmsg options} {
				puts stderr "Error closing dirwatch handle: $errmsg"
			}
		}
	}

	#>>>

	method _readable {} { #<<<
		while 1 {
			yield

			if {[eof $watch_chan]} {
				close $watch_chan
				unset watch_chan
				my destroy
				return
			}

			set dat [chan read $watch_chan]
			if {$dat eq ""} continue

			foreach ev [::filewatch::dirwatch::decode_events $dat] {
				if {[info exists cb]} {
					try {
						uplevel #0 $cb [list $ev]
					} on error {errmsg options} {
						puts stderr "dirwatch callback error: $errmsg\n[dict get $options -errorinfo]"
					}
				}
			}
		}
	}

	#>>>
}
