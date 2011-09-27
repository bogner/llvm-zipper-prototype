"""
Test some SBModule and SBSection APIs.
"""

import os, time
import re
import unittest2
import lldb
from lldbtest import *

class ModuleAndSectionAPIsTestCase(TestBase):

    mydir = os.path.join("python_api", "module_section")

    @python_api_test
    def test_module_and_section(self):
        """Test module and section APIs."""
        self.buildDefault()
        self.module_and_section()

    def module_and_section(self):
        exe = os.path.join(os.getcwd(), "a.out")

        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)
        self.assertTrue(target.GetNumModules() > 0)

        # Hide stdout if not running with '-t' option.
        if not self.TraceOn():
            self.HideStdout()

        print "Number of modules for the target: %d" % target.GetNumModules()
        for module in target.module_iter():
            print module

        # Get the executable module at index 0.
        exe_module = target.GetModuleAtIndex(0)

        print "Exe module: %s" % repr(exe_module)
        print "Number of sections: %d" % exe_module.GetNumSections()
        INDENT = ' ' * 4
        for sec in exe_module.section_iter():
            print sec
            if sec.GetName() == "__TEXT":
                print INDENT + "Number of subsections: %d" % sec.GetNumSubSections()
                for subsec in sec:
                    print INDENT + repr(subsec)


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
