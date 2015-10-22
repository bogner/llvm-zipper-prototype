"""
Test SBprocess and SBThread APIs with printing of the stack traces using lldbutil.
"""

import lldb_shared

import os, time
import re
import lldb
from lldbtest import *

class ThreadsStackTracesTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)
        # Find the line number to break inside main().
        self.line = line_number('main.cpp', '// Set break point at this line.')

    @expectedFailureAll("llvm.org/pr23043", ["linux"], archs=["i386"]) # We are unable to produce a backtrace of the main thread when the thread is blocked in fgets
    @expectedFailureWindows("llvm.org/pr24778")
    @python_api_test
    def test_stack_traces(self):
        """Test SBprocess and SBThread APIs with printing of the stack traces."""
        self.build()
        exe = os.path.join(os.getcwd(), "a.out")

        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        breakpoint = target.BreakpointCreateByLocation("main.cpp", self.line)
        self.assertTrue(breakpoint, VALID_BREAKPOINT)

        # Now launch the process, and do not stop at entry point.
        process = target.LaunchSimple (["abc", "xyz"], None, self.get_process_working_directory())

        if not process:
            self.fail("SBTarget.LaunchProcess() failed")

        import lldbutil
        if process.GetState() != lldb.eStateStopped:
            self.fail("Process should be in the 'stopped' state, "
                      "instead the actual state is: '%s'" %
                      lldbutil.state_type_to_str(process.GetState()))

        stacktraces = lldbutil.print_stacktraces(process, string_buffer=True)
        self.expect(stacktraces, exe=False,
            substrs = ['(int)argc=3'])
