"""
Test that dynamically discovered ivars of type IMP do not crash LLDB
"""

from __future__ import print_function

import use_lldb_suite

import os, time
import re
import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.lldbtest import *
import lldbsuite.support.seven as seven

def execute_command (command):
    # print('%% %s' % (command))
    (exit_status, output) = seven.get_command_status_output(command)
    # if output:
    #     print(output)
    # print('status = %u' % (exit_status))
    return exit_status

class ObjCiVarIMPTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @skipUnlessDarwin
    @no_debug_info_test
    def test_imp_ivar_type(self):
        """Test that dynamically discovered ivars of type IMP do not crash LLDB"""
        if self.getArchitecture() == 'i386':
            # rdar://problem/9946499
            self.skipTest("Dynamic types for ObjC V1 runtime not implemented")
        
        execute_command("make repro")
        def cleanup():
            execute_command("make cleanup")
        self.addTearDownHook(cleanup)

        exe = os.path.join(os.getcwd(), "a.out")

        # Create a target from the debugger.
        target = self.dbg.CreateTarget (exe)
        self.assertTrue(target, VALID_TARGET)

        # Set up our breakpoint

        bkpt = lldbutil.run_break_set_by_source_regexp (self, "break here")

        # Now launch the process, and do not stop at the entry point.
        process = target.LaunchSimple (None, None, self.get_process_working_directory())

        self.assertTrue(process.GetState() == lldb.eStateStopped,
                        PROCESS_STOPPED)

        self.expect('frame variable --ptr-depth=1 --show-types -d run -- object', substrs=[
            '(MyClass *) object = 0x',
            '(void *) myImp = 0x'
        ])
        self.expect('disassemble --start-address `((MyClass*)object)->myImp`', substrs=[
            '-[MyClass init]'
        ])
