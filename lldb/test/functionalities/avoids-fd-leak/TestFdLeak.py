"""
Test whether a process started by lldb has no extra file descriptors open.
"""

import os
import unittest2
import lldb
from lldbtest import *
import lldbutil

class AvoidsFdLeakTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @skipIfWindows # The check for descriptor leakage needs to be implemented differently here.
    def test_fd_leak_basic (self):
        self.do_test([])

    @skipIfWindows # The check for descriptor leakage needs to be implemented differently here.
    def test_fd_leak_log (self):
        self.do_test(["log enable -f '/dev/null' lldb commands"])

    def do_test (self, commands):
        self.buildDefault()
        exe = os.path.join (os.getcwd(), "a.out")

        for c in commands:
            self.runCmd(c)

        target = self.dbg.CreateTarget(exe)

        process = target.LaunchSimple (None, None, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)

        self.assertTrue(process.GetState() == lldb.eStateExited, "Process should have exited.")
        self.assertTrue(process.GetExitStatus() == 0,
                "Process returned non-zero status. Were incorrect file descriptors passed?")

    @skipIfWindows # The check for descriptor leakage needs to be implemented differently here.
    def test_fd_leak_multitarget (self):
        self.buildDefault()
        exe = os.path.join (os.getcwd(), "a.out")

        target = self.dbg.CreateTarget(exe)

        listener = lldb.SBListener()
        error = lldb.SBError()
        process1 = target.Launch (listener, None, None, None, None, None,
                self.get_process_working_directory(), 0, True, # stop at entry
                error)
        self.assertTrue(process1, PROCESS_IS_VALID)
        self.assertTrue(process1.GetState() == lldb.eStateStopped, "Process should have been stopped.")

        target2 = self.dbg.CreateTarget(exe)
        process2 = target2.LaunchSimple (None, None, self.get_process_working_directory())
        self.assertTrue(process2, PROCESS_IS_VALID)

        self.assertTrue(process2.GetState() == lldb.eStateExited, "Process should have exited.")
        self.assertTrue(process2.GetExitStatus() == 0,
                "Process returned non-zero status. Were incorrect file descriptors passed?")


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
