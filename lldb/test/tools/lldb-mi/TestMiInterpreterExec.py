"""
Test that the lldb-mi driver works with -interpreter-exec command
"""

import lldbmi_testcase
from lldbtest import *
import unittest2

class MiInterpreterExecTestCase(lldbmi_testcase.MiTestCaseBase):

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    @unittest2.skip("lldb-mi handles double quotes in passed app path incorrectly")
    def test_lldbmi_target_create(self):
        """Test that 'lldb-mi --interpreter' can create target by 'target create' command."""

        self.spawnLldbMi(args = None)

        # Test that "target create" loads executable
        self.runCmd("-interpreter-exec console \"target create \\\"%s\\\"\"" % self.myexe)
        self.expect("\^done")

        # Test that executable was loaded properly
        self.runCmd("-break-insert -f main")
        self.expect("\^done,bkpt={number=\"1\"")
        self.runCmd("-exec-run")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_breakpoint_set(self):
        """Test that 'lldb-mi --interpreter' can set breakpoint by 'breakpoint set' command."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Test that "breakpoint set" sets a breakpoint
        self.runCmd("-interpreter-exec console \"breakpoint set --name main\"")
        self.expect("\^done")

        # Test that breakpoint was set properly
        self.runCmd("-exec-run")
        self.expect("\^running")
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_settings_set_target_run_args_before(self):
        """Test that 'lldb-mi --interpreter' can set target arguments by 'setting set target.run-args' command before than target was created."""

        self.spawnLldbMi(args = None)

        # Test that "settings set target.run-args" passes arguments to executable
        #FIXME: "--arg1 \"2nd arg\" third_arg fourth=\"4th arg\"" causes an error
        self.runCmd("-interpreter-exec console \"setting set target.run-args arg1\"")
        self.expect("\^done")

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Run to BP_argctest
        line = line_number('main.c', '//BP_argctest')
        self.runCmd("-break-insert --file main.c:%d" % line)
        self.expect("\^done")
        self.runCmd("-exec-run")
        self.expect("\^running")

        # Test that arguments were passed properly
        self.expect("~\"argc=2\\\\r\\\\n\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_settings_set_target_run_args_after(self):
        """Test that 'lldb-mi --interpreter' can set target arguments by 'setting set target.run-args' command after than target was created."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Test that "settings set target.run-args" passes arguments to executable
        #FIXME: "--arg1 \"2nd arg\" third_arg fourth=\"4th arg\"" causes an error
        self.runCmd("-interpreter-exec console \"setting set target.run-args arg1\"")
        self.expect("\^done")

        # Run to BP_argctest
        line = line_number('main.c', '//BP_argctest')
        self.runCmd("-break-insert --file main.c:%d" % line)
        self.expect("\^done")
        self.runCmd("-exec-run")
        self.expect("\^running")

        # Test that arguments were passed properly
        self.expect("~\"argc=2\\\\r\\\\n\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_process_launch(self):
        """Test that 'lldb-mi --interpreter' can launch process by "process launch" command."""

        self.spawnLldbMi(args = None)

        # Load executable
        self.runCmd("-file-exec-and-symbols %s" % self.myexe)
        self.expect("\^done")

        # Set breakpoint
        self.runCmd("-break-insert -f main")
        self.expect("\^done,bkpt={number=\"1\"")

        # Test that "process launch" launches executable
        self.runCmd("-interpreter-exec console \"process launch\"")
        self.expect("\^done")

        # Test that breakpoint hit
        self.expect("\*stopped,reason=\"breakpoint-hit\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    @skipIfFreeBSD # llvm.org/pr22411: Failure presumably due to known thread races
    @skipIfLinux # llvm.org/pr22411: Failure presumably due to known thread races
    def test_lldbmi_thread_step_in(self):
        """Test that 'lldb-mi --interpreter' can step in by "thread step-in" command."""

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

        # Test that "thread step-in" steps in
        #FIXME: is this supposed to step into printf?
        self.runCmd("-interpreter-exec console \"thread step-in\"")
        self.expect("\^done")
        self.expect("~\"argc=1\\\\r\\\\n\"")
        self.expect("\*stopped,reason=\"end-stepping-range\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_thread_step_over(self):
        """Test that 'lldb-mi --interpreter' can step over by "thread step-over" command."""

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

        # Test that "thread step-over" steps over
        self.runCmd("-interpreter-exec console \"thread step-over\"")
        self.expect("\^done")
        self.expect("~\"argc=1\\\\r\\\\n\"")
        self.expect("\*stopped,reason=\"end-stepping-range\"")

    @lldbmi_test
    @expectedFailureWindows("llvm.org/pr22274: need a pexpect replacement for windows")
    def test_lldbmi_thread_continue(self):
        """Test that 'lldb-mi --interpreter' can continue execution by "thread continue" command."""

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

        # Test that "thread continue" continues execution
        self.runCmd("-interpreter-exec console \"thread continue\"")
        self.expect("\^done")
        self.expect("~\"argc=1\\\\r\\\\n")
        self.expect("\*stopped,reason=\"exited-normally\"")

if __name__ == '__main__':
    unittest2.main()
