from __future__ import print_function



import gdbremote_testcase
from lldbsuite.test.lldbtest import *

class TestGdbRemoteSingleStep(gdbremote_testcase.GdbRemoteTestCaseBase):

    mydir = TestBase.compute_mydir(__file__)

    @debugserver_test
    def test_single_step_only_steps_one_instruction_with_s_debugserver(self):
        self.init_debugserver_test()
        self.build()
        self.set_inferior_startup_launch()
        self.single_step_only_steps_one_instruction(use_Hc_packet=True, step_instruction="s")

    @llgs_test
    @expectedFailureAndroid(bugnumber="llvm.com/pr24739", archs=["arm", "aarch64"])
    def test_single_step_only_steps_one_instruction_with_s_llgs(self):
        self.init_llgs_test()
        self.build()
        self.set_inferior_startup_launch()
        self.single_step_only_steps_one_instruction(use_Hc_packet=True, step_instruction="s")
