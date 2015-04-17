"""
Test lldb-mi -var-xxx commands.
"""

import lldbmi_testcase
from lldbtest import *
import unittest2

class MiVarTestCase(lldbmi_testcase.MiTestCaseBase):

    mydir = TestBase.compute_mydir(__file__)

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    @skipIfFreeBSD # llvm.org/pr22411: Failure presumably due to known thread races
    @skipIfGcc #https://llvm.org/bugs/show_bug.cgi?id=23239
    def test_lldbmi_eval(self):
        """Test that 'lldb-mi --interpreter' works for evaluating."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Run to program return
        line = line_number('main.cpp', '// BP_return')
        self.runCmd("-break-insert main.cpp:%d" % line)
        self.expect("\^done,bkpt={number=\"1\"")
        self.runCmd("-exec-run")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Print non-existant variable
        self.runCmd("-var-create var1 * undef")
        #self.expect("\^error") #FIXME: shows undef as {...}
        self.runCmd("-data-evaluate-expression undef")
        self.expect("\^error,msg=\"Could not evaluate expression\"")

        # Print global "g_MyVar", modify, delete and create again
        self.runCmd("-data-evaluate-expression g_MyVar")
        self.expect("\^done,value=\"3\"")
        self.runCmd("-var-create var2 * g_MyVar")
        self.expect("\^done,name=\"var2\",numchild=\"0\",value=\"3\",type=\"int\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-evaluate-expression var2")
        self.expect("\^done,value=\"3\"")
        self.runCmd("-var-show-attributes var2")
        self.expect("\^done,status=\"editable\"")
        self.runCmd("-var-list-children var2")
        self.expect("\^done,numchild=\"0\",children=\"\[\]\"")
        self.runCmd("-data-evaluate-expression \"g_MyVar=30\"")
        self.expect("\^done,value=\"30\"")
        self.runCmd("-var-update --all-values var2")
        #self.expect("\^done,changelist=\[\{name=\"var2\",value=\"30\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]") #FIXME -var-update doesn't work
        self.runCmd("-var-delete var2")
        self.expect("\^done")
        self.runCmd("-var-create var2 * g_MyVar")
        self.expect("\^done,name=\"var2\",numchild=\"0\",value=\"30\",type=\"int\",thread-id=\"1\",has_more=\"0\"")

        # Print static "s_MyVar", modify, delete and create again
        self.runCmd("-data-evaluate-expression s_MyVar")
        self.expect("\^done,value=\"30\"")
        self.runCmd("-var-create var3 * s_MyVar")
        self.expect("\^done,name=\"var3\",numchild=\"0\",value=\"30\",type=\"int\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-evaluate-expression var3")
        self.expect("\^done,value=\"30\"")
        self.runCmd("-var-show-attributes var3")
        self.expect("\^done,status=\"editable\"")
        self.runCmd("-var-list-children var3")
        self.expect("\^done,numchild=\"0\",children=\"\[\]\"")
        self.runCmd("-data-evaluate-expression \"s_MyVar=3\"")
        self.expect("\^done,value=\"3\"")
        self.runCmd("-var-update --all-values var3")
        #self.expect("\^done,changelist=\[\{name=\"var3\",value=\"3\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]") #FIXME -var-update doesn't work
        self.runCmd("-var-delete var3")
        self.expect("\^done")
        self.runCmd("-var-create var3 * s_MyVar")
        self.expect("\^done,name=\"var3\",numchild=\"0\",value=\"3\",type=\"int\",thread-id=\"1\",has_more=\"0\"")

        # Print local "b", modify, delete and create again
        self.runCmd("-data-evaluate-expression b")
        self.expect("\^done,value=\"20\"")
        self.runCmd("-var-create var4 * b")
        self.expect("\^done,name=\"var4\",numchild=\"0\",value=\"20\",type=\"int\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-evaluate-expression var4")
        self.expect("\^done,value=\"20\"")
        self.runCmd("-var-show-attributes var4")
        self.expect("\^done,status=\"editable\"")
        self.runCmd("-var-list-children var4")
        self.expect("\^done,numchild=\"0\",children=\"\[\]\"")
        self.runCmd("-data-evaluate-expression \"b=2\"")
        self.expect("\^done,value=\"2\"")
        self.runCmd("-var-update --all-values var4")
        #self.expect("\^done,changelist=\[\{name=\"var4\",value=\"2\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]") #FIXME -var-update doesn't work
        self.runCmd("-var-delete var4")
        self.expect("\^done")
        self.runCmd("-var-create var4 * b")
        self.expect("\^done,name=\"var4\",numchild=\"0\",value=\"2\",type=\"int\",thread-id=\"1\",has_more=\"0\"")

        # Print temp "a + b"
        self.runCmd("-data-evaluate-expression \"a + b\"")
        self.expect("\^done,value=\"12\"")
        self.runCmd("-var-create var5 * \"a + b\"")
        self.expect("\^done,name=\"var5\",numchild=\"0\",value=\"12\",type=\"int\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-evaluate-expression var5")
        self.expect("\^done,value=\"12\"")
        self.runCmd("-var-show-attributes var5")
        self.expect("\^done,status=\"editable\"") #FIXME editable or not?
        self.runCmd("-var-list-children var5")
        self.expect("\^done,numchild=\"0\",children=\"\[\]\"")

        # Print argument "argv[0]"
        self.runCmd("-data-evaluate-expression \"argv[0]\"")
        self.expect("\^done,value=\"0x[0-9a-f]+\"")
        self.runCmd("-var-create var6 * \"argv[0]\"")
        self.expect("\^done,name=\"var6\",numchild=\"1\",value=\"0x[0-9a-f]+ \\\\\\\".*?%s\\\\\\\"\",type=\"const char \*\",thread-id=\"1\",has_more=\"0\"" % self.myexe)
        self.runCmd("-var-evaluate-expression var6")
        self.expect("\^done,value=\"0x[0-9a-f]+ \\\\\\\".*?%s\\\\\\\"\"" % self.myexe)
        self.runCmd("-var-show-attributes var6")
        self.expect("\^done,status=\"editable\"")
        self.runCmd("-var-list-children --all-values var6")
        self.expect("\^done,numchild=\"1\",children=\[child=\{name=\"var6\.\*\$11\",exp=\"\*\$11\",numchild=\"0\",type=\"const char\",thread-id=\"4294967295\",value=\"47 '/'\",has_more=\"0\"\}\]") #FIXME -var-list-children shows invalid thread-id

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    @skipIfFreeBSD # llvm.org/pr22411: Failure presumably due to known thread races
    @skipIfLinux # llvm.org/pr22841: lldb-mi tests fail on all Linux buildbots
    def test_lldbmi_var_update(self):
        """Test that 'lldb-mi --interpreter' works for -var-update."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Run to BP_var_update_test_init
        line = line_number('main.cpp', '// BP_var_update_test_init')
        self.runCmd("-break-insert main.cpp:%d" % line)
        self.expect("\^done,bkpt={number=\"1\"")
        self.runCmd("-exec-run")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Setup variables
        self.runCmd("-var-create var_l * l")
        self.expect("\^done,name=\"var_l\",numchild=\"0\",value=\"1\",type=\"long\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-create var_complx * complx")
        self.expect("\^done,name=\"var_complx\",numchild=\"3\",value=\"\{\.\.\.\}\",type=\"complex_type\",thread-id=\"1\",has_more=\"0\"")
        self.runCmd("-var-create var_complx_array * complx_array")
        self.expect("\^done,name=\"var_complx_array\",numchild=\"2\",value=\"\[2\]\",type=\"complex_type \[2\]\",thread-id=\"1\",has_more=\"0\"")

        # Go to BP_var_update_test_l
        line = line_number('main.cpp', '// BP_var_update_test_l')
        self.runCmd("-break-insert main.cpp:%d" % line)
        self.expect("\^done,bkpt={number=\"2\"")
        self.runCmd("-exec-continue")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Test that var_l was updated
        self.runCmd("-var-update --all-values var_l")
        self.expect("\^done,changelist=\[\{name=\"var_l\",value=\"0\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]")

        # Go to BP_var_update_test_complx
        line = line_number('main.cpp', '// BP_var_update_test_complx')
        self.runCmd("-break-insert main.cpp:%d" % line)
        self.expect("\^done,bkpt={number=\"3\"")
        self.runCmd("-exec-continue")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Test that var_complx was updated
        self.runCmd("-var-update --all-values var_complx")
        self.expect("\^done,changelist=\[\{name=\"var_complx\",value=\"\{\.\.\.\}\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]")

        # Go to BP_var_update_test_complx_array
        line = line_number('main.cpp', '// BP_var_update_test_complx_array')
        self.runCmd("-break-insert main.cpp:%d" % line)
        self.expect("\^done,bkpt={number=\"4\"")
        self.runCmd("-exec-continue")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Test that var_complex_array was updated
        self.runCmd("-var-update --all-values var_complx_array")
        self.expect("\^done,changelist=\[\{name=\"var_complx_array\",value=\"\[2\]\",in_scope=\"true\",type_changed=\"false\",has_more=\"0\"\}\]")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    @skipIfFreeBSD # llvm.org/pr22411: Failure presumably due to known thread races
    def test_lldbmi_var_create_register(self):
        """Test that 'lldb-mi --interpreter' works for -var-create $regname."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Run to main
        self.runCmd("-break-insert -f main")
        self.expect("\^done,bkpt={number=\"1\"")
        self.runCmd("-exec-run")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

        # Find name of register 0
        self.runCmd("-data-list-register-names 0")
        self.expect("\^done,register-names=\[\".+?\"\]")
        register_name = self.child.after.split("\"")[1]

        # Create variable for register 0
        # Note that message is different in Darwin and Linux:
        # Darwin: "^done,name=\"var_reg\",numchild=\"0\",value=\"0x[0-9a-f]+\",type=\"unsigned long\",thread-id=\"1\",has_more=\"0\"
        # Linux:  "^done,name=\"var_reg\",numchild=\"0\",value=\"0x[0-9a-f]+\",type=\"unsigned int\",thread-id=\"1\",has_more=\"0\"
        self.runCmd("-var-create var_reg * $%s" % register_name)
        self.expect("\^done,name=\"var_reg\",numchild=\"0\",value=\"0x[0-9a-f]+\",type=\"unsigned (long|int)\",thread-id=\"1\",has_more=\"0\"")

        # Assign value to variable
        self.runCmd("-var-assign var_reg \"6\"")
        self.expect("\^done,value=\"0x0000000000000006\"")

        # Assert register 0 updated
        self.runCmd("-data-list-register-values d 0")
        self.expect("\^done,register-values=\[{number=\"0\",value=\"6\"")

if __name__ == '__main__':
    unittest2.main()
