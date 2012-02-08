"""
Test the lldb command line completion mechanism.
"""

import os
import unittest2
import lldb
import pexpect
from lldbtest import *

class CommandLineCompletionTestCase(TestBase):

    mydir = os.path.join("functionalities", "completion")

    @classmethod
    def classCleanup(cls):
        """Cleanup the test byproducts."""
        system(["/bin/sh", "-c", "rm -f child_send.txt"])
        system(["/bin/sh", "-c", "rm -f child_read.txt"])

    def test_frame_variable_dash_w(self):
        """Test that 'frame variable -w' completes to 'frame variable -w '."""
        self.complete_from_to('frame variable -w', 'frame variable -w ')

    def test_frame_variable_dash_w_space(self):
        """Test that 'frame variable -w ' completes to ['Available completions:', 'read', 'write', 'read_write']."""
        self.complete_from_to('frame variable -w ', ['Available completions:', 'read', 'write', 'read_write'])

    def test_watchpoint_set_ex(self):
        """Test that 'watchpoint set ex' completes to 'watchpoint set expression '."""
        self.complete_from_to('watchpoint set ex', 'watchpoint set expression ')

    def test_watchpoint_set_var(self):
        """Test that 'watchpoint set var' completes to 'watchpoint set variable '."""
        self.complete_from_to('watchpoint set var', 'watchpoint set variable ')

    def test_watchpoint_set_variable_dash_w_read_underbar(self):
        """Test that 'watchpoint set variable -w read_' completes to 'watchpoint set variable -w read_write'."""
        self.complete_from_to('watchpoint set variable -w read_', 'watchpoint set variable -w read_write')

    def test_help_fi(self):
        """Test that 'help fi' completes to ['Available completions:', 'file', 'finish']."""
        self.complete_from_to('help fi', ['Available completions:', 'file', 'finish'])

    def test_help_watchpoint_s(self):
        """Test that 'help watchpoint s' completes to 'help watchpoint set '."""
        self.complete_from_to('help watchpoint s', 'help watchpoint set ')

    def test_settings_append_target_er(self):
        """Test that 'settings append target.er' completes to 'settings append target.error-path'."""
        self.complete_from_to('settings append target.er', 'settings append target.error-path')

    def test_settings_insert_after_target_en(self):
        """Test that 'settings insert-after target.en' completes to 'settings insert-after target.env-vars'."""
        self.complete_from_to('settings insert-after target.en', 'settings insert-after target.env-vars')

    def test_settings_insert_before_target_en(self):
        """Test that 'settings insert-before target.en' completes to 'settings insert-before target.env-vars'."""
        self.complete_from_to('settings insert-before target.en', 'settings insert-before target.env-vars')

    def test_settings_replace_target_ru(self):
        """Test that 'settings replace target.ru' completes to 'settings replace target.run-args'."""
        self.complete_from_to('settings replace target.ru', 'settings replace target.run-args')

    def test_settings_s(self):
        """Test that 'settings s' completes to ['Available completions:', 'set', 'show']."""
        self.complete_from_to('settings s', ['Available completions:', 'set', 'show'])

    def test_settings_set_th(self):
        """Test that 'settings set th' completes to 'settings set thread-format'."""
        self.complete_from_to('settings set th', 'settings set thread-format')

    def test_settings_s_dash(self):
        """Test that 'settings set -' completes to ['Available completions:', '-n', '-r']."""
        self.complete_from_to('settings set -', ['Available completions:', '-n', '-r'])

    def test_settings_set_dash_r_th(self):
        """Test that 'settings set -r th' completes to 'settings set -r thread-format'."""
        self.complete_from_to('settings set -r th', 'settings set -r thread-format')

    def test_settings_set_ta(self):
        """Test that 'settings set ta' completes to 'settings set target.'."""
        self.complete_from_to('settings set ta', 'settings set target.')

    def test_settings_set_target_pr(self):
        """Test that 'settings set target.pr' completes to ['Available completions:',
        'target.prefer-dynamic-value', 'target.process.']."""
        self.complete_from_to('settings set target.pr',
                              ['Available completions:',
                               'target.prefer-dynamic-value',
                               'target.process.'])

    def test_settings_set_target_process(self):
        """Test that 'settings set target.process' completes to 'settings set target.process.'."""
        self.complete_from_to('settings set target.process', 'settings set target.process.')

    def test_settings_set_target_process_dot(self):
        """Test that 'settings set target.process.' completes to 'settings set target.process.thread.'."""
        self.complete_from_to('settings set target.process.', 'settings set target.process.thread.')

    def test_settings_set_target_process_thread_dot(self):
        """Test that 'settings set target.process.thread.' completes to ['Available completions:',
        'target.process.thread.step-avoid-regexp', 'target.process.thread.trace-thread']."""
        self.complete_from_to('settings set target.process.thread.',
                              ['Available completions:',
                               'target.process.thread.step-avoid-regexp',
                               'target.process.thread.trace-thread'])

    def test_target_space(self):
        """Test that 'target ' completes to ['Available completions:', 'create', 'delete', 'list',
        'modules', 'select', 'stop-hook', 'variable']."""
        self.complete_from_to('target ',
                              ['Available completions:', 'create', 'delete', 'list',
                               'modules', 'select', 'stop-hook', 'variable'])

    def test_target_va(self):
        """Test that 'target va' completes to 'target variable '."""
        self.complete_from_to('target va', 'target variable ')

    def complete_from_to(self, str_input, patterns):
        """Test that the completion mechanism completes str_input to patterns,
        where patterns could be a pattern-string or a list of pattern-strings"""
        # Patterns should not be None in order to proceed.
        self.assertFalse(patterns is None)
        # And should be either a string or list of strings.  Check for list type
        # below, if not, make a list out of the singleton string.  If patterns
        # is not a string or not a list of strings, there'll be runtime errors
        # later on.
        if not isinstance(patterns, list):
            patterns = [patterns]
        
        # The default lldb prompt.
        prompt = "(lldb) "

        # So that the child gets torn down after the test.
        self.child = pexpect.spawn('%s %s' % (self.lldbHere, self.lldbOption))
        child = self.child
        # Turn on logging for input/output to/from the child.
        with open('child_send.txt', 'w') as f_send:
            with open('child_read.txt', 'w') as f_read:
                child.logfile_send = f_send
                child.logfile_read = f_read

                child.expect_exact(prompt)
                child.setecho(True)
                # Sends str_input and a Tab to invoke the completion machinery.
                child.send("%s\t" % str_input)
                child.sendline('')
                child.expect_exact(prompt)

        # Now that the necessary logging is done, restore logfile to None to
        # stop further logging.
        child.logfile_send = None
        child.logfile_read = None
        
        with open('child_send.txt', 'r') as fs:
            if self.TraceOn():
                print "\n\nContents of child_send.txt:"
                print fs.read()
        with open('child_read.txt', 'r') as fr:
            from_child = fr.read()
            if self.TraceOn():
                print "\n\nContents of child_read.txt:"
                print from_child

            # Test that str_input completes to our patterns.
            # If each pattern matches from_child, the completion mechanism works!
            for p in patterns:
                self.expect(from_child, msg=COMPLETIOND_MSG(str_input, p), exe=False,
                    patterns = [p])


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
