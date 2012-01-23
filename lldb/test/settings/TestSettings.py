"""
Test lldb settings command.
"""

import os, time
import unittest2
import lldb
from lldbtest import *

class SettingsCommandTestCase(TestBase):

    mydir = "settings"

    @classmethod
    def classCleanup(cls):
        """Cleanup the test byproducts."""
        system(["/bin/sh", "-c", "rm -f output1.txt"])
        system(["/bin/sh", "-c", "rm -f output2.txt"])
        system(["/bin/sh", "-c", "rm -f stderr.txt"])
        system(["/bin/sh", "-c", "rm -f stdout.txt"])

    def test_apropos_should_also_search_settings_description(self):
        """Test that 'apropos' command should also search descriptions for the settings variables."""

        self.expect("apropos 'environment variable'",
            substrs = ["target.env-vars",
                       "environment variables",
                       "executable's environment"])

    def test_append_target_env_vars(self):
        """Test that 'replace target.run-args' works."""
        # Append the env-vars.
        self.runCmd('settings append target.env-vars MY_ENV_VAR=YES')
        # And add hooks to restore the settings during tearDown().
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.env-vars"))

        # Check it immediately!
        self.expect('settings show target.env-vars',
            substrs = ['MY_ENV_VAR=YES'])

    def test_insert_before_and_after_target_run_args(self):
        """Test that 'insert-before/after target.run-args' works."""
        # Set the run-args first.
        self.runCmd('settings set target.run-args a b c')
        # And add hooks to restore the settings during tearDown().
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.run-args"))

        # Now insert-before the index-0 element with '__a__'.
        self.runCmd('settings insert-before target.run-args 0 __a__')
        # And insert-after the index-1 element with '__A__'.
        self.runCmd('settings insert-after target.run-args 1 __A__')
        # Check it immediately!
        self.expect('settings show target.run-args',
            substrs = ['target.run-args (array) = ',
                       '[0]: "__a__"',
                       '[1]: "a"',
                       '[2]: "__A__"',
                       '[3]: "b"',
                       '[4]: "c"'])

    def test_replace_target_run_args(self):
        """Test that 'replace target.run-args' works."""
        # Set the run-args and then replace the index-0 element.
        self.runCmd('settings set target.run-args a b c')
        # And add hooks to restore the settings during tearDown().
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.run-args"))

        # Now replace the index-0 element with 'A', instead.
        self.runCmd('settings replace target.run-args 0 A')
        # Check it immediately!
        self.expect('settings show target.run-args',
            substrs = ['target.run-args (array) = ',
                       '[0]: "A"',
                       '[1]: "b"',
                       '[2]: "c"'])

    def test_set_prompt(self):
        """Test that 'set prompt' actually changes the prompt."""

        # Set prompt to 'lldb2'.
        self.runCmd("settings set prompt lldb2")

        # Immediately test the setting.
        self.expect("settings show prompt", SETTING_MSG("prompt"),
            startstr = 'prompt (string) = "lldb2"')

        # The overall display should also reflect the new setting.
        self.expect("settings show", SETTING_MSG("prompt"),
            substrs = ['prompt (string) = "lldb2"'])

        # Use '-r' option to reset to the original default prompt.
        self.runCmd("settings set -r prompt")

    def test_set_term_width(self):
        """Test that 'set term-width' actually changes the term-width."""

        self.runCmd("settings set term-width 70")

        # Immediately test the setting.
        self.expect("settings show term-width", SETTING_MSG("term-width"),
            startstr = "term-width (int) = 70")

        # The overall display should also reflect the new setting.
        self.expect("settings show", SETTING_MSG("term-width"),
            substrs = ["term-width (int) = 70"])

    #rdar://problem/10712130
    def test_set_frame_format(self):
        """Test that 'set frame-format' with a backtick char in the format string works as well as fullpath."""
        self.buildDefault()

        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        format_string = "frame #${frame.index}: ${frame.pc}{ ${module.file.basename}`${function.name-with-args}{${function.pc-offset}}}{ at ${line.file.fullpath}:${line.number}}\n"
        self.runCmd("settings set frame-format %s" % format_string)

        # Immediately test the setting.
        self.expect("settings show frame-format", SETTING_MSG("frame-format"),
            substrs = [format_string])

        self.runCmd("breakpoint set -n main")
        self.runCmd("run")
        self.expect("thread backtrace",
            substrs = ["`main", os.getcwd()])

    def test_set_auto_confirm(self):
        """Test that after 'set auto-confirm true', manual confirmation should not kick in."""
        self.buildDefault()

        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        self.runCmd("settings set auto-confirm true")

        # Immediately test the setting.
        self.expect("settings show auto-confirm", SETTING_MSG("auto-confirm"),
            startstr = "auto-confirm (boolean) = true")

        # Now 'breakpoint delete' should just work fine without confirmation
        # prompt from the command interpreter.
        self.runCmd("breakpoint set -n main")
        self.expect("breakpoint delete",
            startstr = "All breakpoints removed")

        # Restore the original setting of auto-confirm.
        self.runCmd("settings set -r auto-confirm")
        self.expect("settings show auto-confirm", SETTING_MSG("auto-confirm"),
            startstr = "auto-confirm (boolean) = false")

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    def test_run_args_and_env_vars_with_dsym(self):
        """Test that run-args and env-vars are passed to the launched process."""
        self.buildDsym()
        self.pass_run_args_and_env_vars()

    def test_run_args_and_env_vars_with_dwarf(self):
        """Test that run-args and env-vars are passed to the launched process."""
        self.buildDwarf()
        self.pass_run_args_and_env_vars()

    def pass_run_args_and_env_vars(self):
        """Test that run-args and env-vars are passed to the launched process."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # Set the run-args and the env-vars.
        # And add hooks to restore the settings during tearDown().
        self.runCmd('settings set target.run-args A B C')
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.run-args"))
        self.runCmd('settings set target.env-vars ["MY_ENV_VAR"]=YES')
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.env-vars"))

        self.runCmd("run", RUN_SUCCEEDED)

        # Read the output file produced by running the program.
        with open('output2.txt', 'r') as f:
            output = f.read()

        self.expect(output, exe=False,
            substrs = ["argv[1] matches",
                       "argv[2] matches",
                       "argv[3] matches",
                       "Environment variable 'MY_ENV_VAR' successfully passed."])

    def test_pass_host_env_vars(self):
        """Test that the host env vars are passed to the launched process."""
        self.buildDefault()

        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # By default, inherit-env is 'true'.
        self.expect('settings show target.inherit-env', "Default inherit-env is 'true'",
            startstr = "target.inherit-env (boolean) = true")

        # Set some host environment variables now.
        os.environ["MY_HOST_ENV_VAR1"] = "VAR1"
        os.environ["MY_HOST_ENV_VAR2"] = "VAR2"

        # This is the function to unset the two env variables set above.
        def unset_env_variables():
            os.environ.pop("MY_HOST_ENV_VAR1")
            os.environ.pop("MY_HOST_ENV_VAR2")

        self.addTearDownHook(unset_env_variables)
        self.runCmd("run", RUN_SUCCEEDED)

        # Read the output file produced by running the program.
        with open('output1.txt', 'r') as f:
            output = f.read()

        self.expect(output, exe=False,
            substrs = ["The host environment variable 'MY_HOST_ENV_VAR1' successfully passed.",
                       "The host environment variable 'MY_HOST_ENV_VAR2' successfully passed."])

    def test_set_error_output_path(self):
        """Test that setting target.error/output-path for the launched process works."""
        self.buildDefault()

        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # Set the error-path and output-path and verify both are set.
        self.runCmd("settings set target.error-path stderr.txt")
        self.runCmd("settings set target.output-path stdout.txt")
        # And add hooks to restore the original settings during tearDown().
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.output-path"))
        self.addTearDownHook(
            lambda: self.runCmd("settings set -r target.error-path"))

        self.expect("settings show target.error-path",
                    SETTING_MSG("target.error-path"),
            startstr = 'target.error-path (string) = "stderr.txt"')

        self.expect("settings show target.output-path",
                    SETTING_MSG("target.output-path"),
            startstr = 'target.output-path (string) = "stdout.txt"')

        self.runCmd("run", RUN_SUCCEEDED)

        # The 'stderr.txt' file should now exist.
        self.assertTrue(os.path.isfile("stderr.txt"),
                        "'stderr.txt' exists due to target.error-path.")

        # Read the output file produced by running the program.
        with open('stderr.txt', 'r') as f:
            output = f.read()

        self.expect(output, exe=False,
            startstr = "This message should go to standard error.")

        # The 'stdout.txt' file should now exist.
        self.assertTrue(os.path.isfile("stdout.txt"),
                        "'stdout.txt' exists due to target.output-path.")

        # Read the output file produced by running the program.
        with open('stdout.txt', 'r') as f:
            output = f.read()

        self.expect(output, exe=False,
            startstr = "This message should go to standard out.")

    def test_print_dictionary_setting(self):
        self.runCmd ("settings set -r target.env-vars")
        self.runCmd ("settings set target.env-vars [\"MY_VAR\"]=some-value")
        self.expect ("settings show target.env-vars",
                     substrs = [ "MY_VAR=some-value" ])
        self.runCmd ("settings set -r target.env-vars")

    def test_print_array_setting(self):
        self.runCmd ("settings set -r target.run-args")
        self.runCmd ("settings set target.run-args gobbledy-gook")
        self.expect ("settings show target.run-args",
                     substrs = [ '[0]: "gobbledy-gook"' ])
        self.runCmd ("settings set -r target.run-args")

    def test_settings_with_quotes (self):
        self.runCmd ("settings set -r target.run-args")
        self.runCmd ("settings set target.run-args a b c")
        self.expect ("settings show target.run-args",
                     substrs = [ '[0]: "a"',
                                 '[1]: "b"',
                                 '[2]: "c"' ])
        self.runCmd ("settings set target.run-args 'a b c'")
        self.expect ("settings show target.run-args",
                     substrs = [ '[0]: "a b c"' ])
        self.runCmd ("settings set -r target.run-args")
        self.runCmd ("settings set -r target.env-vars")
        self.runCmd ('settings set target.env-vars ["MY_FILE"]="this is a file name with spaces.txt"')
        self.expect ("settings show target.env-vars",
                     substrs = [ 'MY_FILE=this is a file name with spaces.txt' ])
        self.runCmd ("settings set -r target.env-vars")


    def test_all_settings_exist (self):
        self.expect ("settings show",
                     substrs = [ "frame-format (string) = ",
                                 "prompt (string) = ",
                                 "script-lang (string) = ",
                                 "term-width (int) = ",
                                 "thread-format (string) = ",
                                 "use-external-editor (boolean) = ",
                                 "auto-confirm (boolean) = ",
                                 "target.default-arch (string) =",
                                 "target.expr-prefix (string) = ",
                                 "target.run-args (array) =",
                                 "target.env-vars (dictionary) =",
                                 "target.inherit-env (boolean) = ",
                                 "target.input-path (string) = ",
                                 "target.output-path (string) = ",
                                 "target.error-path (string) = ",
                                 "target.disable-aslr (boolean) = ",
                                 "target.disable-stdio (boolean) = ",
                                 "target.process.thread.step-avoid-regexp (string) =",
                                 "target.process.thread.trace-thread (boolean) =" ])
        

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
