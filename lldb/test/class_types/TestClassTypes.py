"""Test breakpoint on a class constructor; and variable list the this object."""

import os, time
import unittest2
import lldb
from lldbtest import *

class ClassTypesTestCase(TestBase):

    mydir = "class_types"

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    def test_with_dsym_and_run_command(self):
        """Test 'frame variable this' when stopped on a class constructor."""
        self.buildDsym()
        self.class_types()

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    def test_with_dsym_and_python_api(self):
        """Use Python APIs to create a breakpoint by (filespec, line)."""
        self.buildDsym()
        self.breakpoint_creation_by_filespec_python()

    # rdar://problem/8378863
    # "frame variable this" returns
    # error: unable to find any variables named 'this'
    @unittest2.expectedFailure
    def test_with_dwarf_and_run_command(self):
        """Test 'frame variable this' when stopped on a class constructor."""
        self.buildDwarf()
        self.class_types()

    def test_with_dwarf_and_python_api(self):
        """Use Python APIs to create a breakpoint by (filespec, line)."""
        self.buildDwarf()
        self.breakpoint_creation_by_filespec_python()

    def class_types(self):
        """Test 'frame variable this' when stopped on a class constructor."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # Break on the ctor function of class C.
        self.expect("breakpoint set -f main.cpp -l 73", BREAKPOINT_CREATED,
            startstr = "Breakpoint created: 1: file ='main.cpp', line = 73, locations = 1")

        self.runCmd("run", RUN_SUCCEEDED)

        # The stop reason of the thread should be breakpoint.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
            substrs = ['state is Stopped',
                       'stop reason = breakpoint'])

        # The breakpoint should have a hit count of 1.
        self.expect("breakpoint list", BREAKPOINT_HIT_ONCE,
            substrs = [' resolved, hit count = 1'])

        # We should be stopped on the ctor function of class C.
        self.expect("frame variable this", VARIABLES_DISPLAYED_CORRECTLY,
            startstr = '(class C *const) this = ')

    def breakpoint_creation_by_filespec_python(self):
        """Use Python APIs to create a breakpoint by (filespec, line)."""
        exe = os.path.join(os.getcwd(), "a.out")

        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target.IsValid(), VALID_TARGET)

        filespec = target.GetExecutable()
        self.assertTrue(filespec.IsValid(), VALID_FILESPEC)

        breakpoint = target.BreakpointCreateByLocation(filespec, 73)
        self.assertTrue(breakpoint.IsValid(), VALID_BREAKPOINT)

        fsDir = filespec.GetDirectory()
        fsFile = filespec.GetFilename()

        self.assertTrue(fsDir == os.getcwd() and fsFile == "a.out",
                        "FileSpec matches the executable")


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
