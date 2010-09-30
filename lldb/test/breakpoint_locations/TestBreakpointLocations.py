"""
Test breakpoint commands for a breakpoint ID with multiple locations.
"""

import os, time
import unittest2
import lldb
from lldbtest import *

class BreakpointLocationsTestCase(TestBase):

    mydir = "breakpoint_locations"

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    def test_with_dsym(self):
        """Test breakpoint enable/disable for a breakpoint ID with multiple locations."""
        self.buildDsym()
        self.breakpoint_locations_test()

    def test_with_dwarf(self):
        """Test breakpoint enable/disable for a breakpoint ID with multiple locations."""
        self.buildDwarf()
        self.breakpoint_locations_test()

    def breakpoint_locations_test(self):
        """Test breakpoint enable/disable for a breakpoint ID with multiple locations."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # This should create a breakpoint with 3 locations.
        self.expect("breakpoint set -f main.c -l 19", BREAKPOINT_CREATED,
            startstr = "Breakpoint created: 1: file ='main.c', line = 19, locations = 3")

        # The breakpoint list should show 3 locations.
        self.expect("breakpoint list", "Breakpoint locations shown correctly",
            substrs = ["1: file ='main.c', line = 19, locations = 3"],
            patterns = ["1\.1: where = a.out`func_inlined .+unresolved, hit count = 0",
                        "1\.2: where = a.out`main .+\[inlined\].+unresolved, hit count = 0",
                        "1\.3: where = a.out`main .+\[inlined\].+unresolved, hit count = 0"])

        # The 'breakpoint disable 3.*' command should fail gracefully.
        self.expect("breakpoint disable 3.*",
                    "Disabling an invalid breakpoint should fail gracefully",
                    error=True,
            startstr = "error: '3' is not a valid breakpoint ID.")

        # The 'breakpoint disable 1.*' command should disable all 3 locations.
        self.expect("breakpoint disable 1.*", "All 3 breakpoint locatons disabled correctly",
            startstr = "3 breakpoints disabled.")

        # Run the program.
        self.runCmd("run", RUN_SUCCEEDED)

        # We should not stopped on any breakpoint at all.
        self.expect("process status", "No stopping on any disabled breakpoint",
            patterns = ["^Process [0-9]+ exited with status = 0"])

        # The 'breakpoint enable 1.*' command should enable all 3 breakpoints.
        self.expect("breakpoint enable 1.*", "All 3 breakpoint locatons enabled correctly",
            startstr = "3 breakpoints enabled.")

        # The 'breakpoint disable 1.1' command should disable 1 location.
        self.expect("breakpoint disable 1.1", "1 breakpoint locatons disabled correctly",
            startstr = "1 breakpoints disabled.")

        # Run the program againt.  We should stop on the two breakpoint locations.
        self.runCmd("run", RUN_SUCCEEDED)

        self.expect("thread backtrace", "Stopped on an inlined location",
            substrs = ["stop reason = breakpoint 1.2"],
            patterns = ["frame #0: .+\[inlined\]"])

        self.runCmd("process continue")

        self.expect("thread backtrace", "Stopped on an inlined location",
            substrs = ["stop reason = breakpoint 1.3"],
            patterns = ["frame #0: .+\[inlined\]"])

        # At this point, the 3 locations should all have "hit count = 2".
        self.expect("breakpoint list", "Expect all 3 breakpoints with hit count of 2",
            patterns = ["1\.1: where = a.out`func_inlined .+ resolved, hit count = 2 +Options: disabled",
                        "1\.2: where = a.out`main .+\[inlined\].+ resolved, hit count = 2",
                        "1\.3: where = a.out`main .+\[inlined\].+ resolved, hit count = 2"])


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
