"""
Set breakpoints on objective-c class and instance methods in foundation.
Also lookup objective-c data types and evaluate expressions.
"""

import os, time
import unittest2
import lldb
from lldbtest import *

@unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
class FoundationTestCase(TestBase):

    mydir = "foundation"

    def test_break_with_dsym(self):
        """Test setting objc breakpoints using 'regexp-break' and 'breakpoint set'."""
        self.buildDsym()
        self.break_on_objc_methods()

    def test_break_with_dwarf(self):
        """Test setting objc breakpoints using 'regexp-break' and 'breakpoint set'."""
        self.buildDwarf()
        self.break_on_objc_methods()

    @unittest2.expectedFailure
    # rdar://problem/8492646
    def test_data_type_and_expr_with_dsym(self):
        """Lookup objective-c data types and evaluate expressions."""
        self.buildDsym()
        self.data_type_and_expr_objc()

    @unittest2.expectedFailure
    # rdar://problem/8492646
    def test_data_type_and_expr_with_dwarf(self):
        """Lookup objective-c data types and evaluate expressions."""
        self.buildDwarf()
        self.data_type_and_expr_objc()

    def break_on_objc_methods(self):
        """Test setting objc breakpoints using 'regexp-break' and 'breakpoint set'."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # Stop at +[NSString stringWithFormat:].
        self.expect("regexp-break +[NSString stringWithFormat:]", BREAKPOINT_CREATED,
            substrs = ["Breakpoint created: 1: name = '+[NSString stringWithFormat:]', locations = 1"])

        # Stop at -[MyString initWithNSString:].
        self.expect("breakpoint set -n '-[MyString initWithNSString:]'", BREAKPOINT_CREATED,
            startstr = "Breakpoint created: 2: name = '-[MyString initWithNSString:]', locations = 1")

        # Stop at the "description" selector.
        self.expect("breakpoint set -S description", BREAKPOINT_CREATED,
            startstr = "Breakpoint created: 3: name = 'description', locations = 1")

        # Stop at -[NSAutoreleasePool release].
        self.expect("regexp-break -[NSAutoreleasePool release]", BREAKPOINT_CREATED,
            substrs = ["Breakpoint created: 4: name = '-[NSAutoreleasePool release]', locations = 1"])

        self.runCmd("run", RUN_SUCCEEDED)

        # First stop is +[NSString stringWithFormat:].
        self.expect("thread backtrace", "Stop at +[NSString stringWithFormat:]",
            substrs = ["Foundation`+[NSString stringWithFormat:]"])

        self.runCmd("process continue")

        # Followed by a.out`-[MyString initWithNSString:].
        self.expect("thread backtrace", "Stop at a.out`-[MyString initWithNSString:]",
            substrs = ["a.out`-[MyString initWithNSString:]"])

        self.runCmd("process continue")

        # Followed by -[MyString description].
        self.expect("thread backtrace", "Stop at -[MyString description]",
            substrs = ["a.out`-[MyString description]"])

        self.runCmd("process continue")

        # Followed by -[NSAutoreleasePool release].
        self.expect("thread backtrace", "Stop at -[NSAutoreleasePool release]",
            substrs = ["Foundation`-[NSAutoreleasePool release]"])

    def data_type_and_expr_objc(self):
        """Lookup objective-c data types and evaluate expressions."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # Stop at -[MyString description].
        self.expect("breakpoint set -n '-[MyString description]", BREAKPOINT_CREATED,
            startstr = "Breakpoint created: 1: name = '-[MyString description]', locations = 1")

        self.runCmd("run", RUN_SUCCEEDED)

        # The backtrace should show we stop at -[MyString description].
        self.expect("thread backtrace", "Stop at -[MyString description]",
            substrs = ["a.out`-[MyString description]"])

        # Lookup objc data type MyString and evaluate some expressions.

        self.expect("image lookup -t NSString", DATA_TYPES_DISPLAYED_CORRECTLY,
            substrs = ['name = "NSString"',
                       'clang_type = "@interface NSString@end"'])

        self.expect("image lookup -t MyString", DATA_TYPES_DISPLAYED_CORRECTLY,
            substrs = ['name = "MyString"',
                       'clang_type = "@interface MyString'])

        self.expect("frame variable -s", VARIABLES_DISPLAYED_CORRECTLY,
            substrs = ["ARG: (MyString *) self"],
            patterns = ["ARG: \(.*\) _cmd",
                        "(struct objc_selector *)|(SEL)"])

        # Test new feature with r115115:
        # Add "-o" option to "expression" which prints the object description if available.
        self.expect("expr -o -- self", "Object description displayed correctly",
            startstr = "Hello from ",
            substrs = ["a.out", "with timestamp: "])

        self.expect("expr self->non_existent_member", COMMAND_FAILED_AS_EXPECTED, error=True,
            startstr = "error: 'MyString' does not have a member named 'non_existent_member'")

        # rdar://problem/8492646
        # test/foundation fails after updating to tot r115023
        # self->str displays nothing as output
        self.expect("frame variable self->str", VARIABLES_DISPLAYED_CORRECTLY,
            startstr = "(NSString *) self->str")

        # rdar://problem/8447030
        # 'frame variable self->date' displays the wrong data member
        self.expect("frame variable self->date", VARIABLES_DISPLAYED_CORRECTLY,
            startstr = "(NSDate *) self->date")

        # TODO: use expression parser.
        # self.runCmd("expr self->str")
        # self.runCmd("expr self->date")

        # (lldb) expr self->str
        # error: 'MyString' does not have a member named 'str'
        # error: 1 errors parsing expression
        # Couldn't parse the expresssion
        # (lldb) expr self->date
        # error: 'MyString' does not have a member named 'date'
        # error: 1 errors parsing expression
        # Couldn't parse the expresssion


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
