"""
Test SBBreakpoint APIs.
"""

import lldb_shared

import os, time
import re
import lldb, lldbutil
from lldbtest import *

class BreakpointAPITestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @python_api_test
    def test_breakpoint_is_valid(self):
        """Make sure that if an SBBreakpoint gets deleted its IsValid returns false."""
        self.build()
        exe = os.path.join(os.getcwd(), "a.out")

        # Create a target by the debugger.
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Now create a breakpoint on main.c by name 'AFunction'.
        breakpoint = target.BreakpointCreateByName('AFunction', 'a.out')
        #print "breakpoint:", breakpoint
        self.assertTrue(breakpoint and
                        breakpoint.GetNumLocations() == 1,
                        VALID_BREAKPOINT)

        # Now delete it:
        did_delete = target.BreakpointDelete(breakpoint.GetID())
        self.assertTrue (did_delete, "Did delete the breakpoint we just created.")

        # Make sure we can't find it:
        del_bkpt = target.FindBreakpointByID (breakpoint.GetID())
        self.assertTrue (not del_bkpt, "We did delete the breakpoint.")

        # Finally make sure the original breakpoint is no longer valid.
        self.assertTrue (not breakpoint, "Breakpoint we deleted is no longer valid.")
