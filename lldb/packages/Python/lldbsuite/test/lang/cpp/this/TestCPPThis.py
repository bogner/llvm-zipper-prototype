"""
Tests that C++ member and static variables are available where they should be.
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.lldbutil as lldbutil

class CPPThisTestCase(TestBase):
    
    mydir = TestBase.compute_mydir(__file__)
    
    #rdar://problem/9962849
    @expectedFailureGcc # llvm.org/pr15439 The 'this' pointer isn't available during expression evaluation when stopped in an inlined member function.
    @expectedFailureIcc # ICC doesn't emit correct DWARF inline debug info for inlined member functions
    @expectedFailureWindows("llvm.org/pr24489: Name lookup not working correctly on Windows")
    @expectedFailureWindows("llvm.org/pr24490: We shouldn't be using platform-specific names like `getpid` in tests")
    @expectedFlakeyClang(bugnumber='llvm.org/pr23012', compiler_version=['>=','3.6']) # failed with totclang - clang3.7
    def test_with_run_command(self):
        """Test that the appropriate member variables are available when stopped in C++ static, inline, and const methods"""
        self.build()
        self.runCmd("file a.out", CURRENT_EXECUTABLE_SET)

        self.set_breakpoint(line_number('main.cpp', '// breakpoint 1'))
        self.set_breakpoint(line_number('main.cpp', '// breakpoint 2'))
        self.set_breakpoint(line_number('main.cpp', '// breakpoint 3'))
        self.set_breakpoint(line_number('main.cpp', '// breakpoint 4'))

        self.runCmd("process launch", RUN_SUCCEEDED)

        self.expect("expression -- m_a = 2",
                    startstr = "(int) $0 = 2")
        
        self.runCmd("process continue")
        
        # This would be disallowed if we enforced const.  But we don't.
        self.expect("expression -- m_a = 2",
                    startstr = "(int) $1 = 2")
        
        self.expect("expression -- (int)getpid(); m_a", 
                    startstr = "(int) $2 = 2")

        self.runCmd("process continue")

        self.expect("expression -- s_a",
                    startstr = "(int) $3 = 5")

        self.runCmd("process continue")

        self.expect("expression -- m_a",
                    startstr = "(int) $4 = 2")
    
    def set_breakpoint(self, line):
        lldbutil.run_break_set_by_file_and_line (self, "main.cpp", line, num_expected_locations=1, loc_exact=False)
