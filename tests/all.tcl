package require tcltest
namespace import ::tcltest::*

::tcltest::configure -singleproc 1 {*}$argv -testdir [file dirname [file normalize [info script]]]

set failed [::tcltest::runAllTests]

::tcltest::cleanupTests 0

if {$failed} {
	# Exit non-zero so that meson test (and CI) see the failure
	puts $::tcltest::outputChannel "[file tail [info script]]: test(s) failed"
	close stderr
	error "test run failed"
}
