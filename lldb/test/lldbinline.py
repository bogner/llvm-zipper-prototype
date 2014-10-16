import lldb
from lldbtest import *
import lldbutil
import os
import new

def source_type(filename):
    _, extension = os.path.splitext(filename)
    return {
        '.c' : 'C_SOURCES',
        '.cpp' : 'CXX_SOURCES',
        '.cxx' : 'CXX_SOURCES',
        '.cc' : 'CXX_SOURCES',
        '.m' : 'OBJC_SOURCES',
        '.mm' : 'OBJCXX_SOURCES'
    }.get(extension, None)

class CommandParser:
    def __init__(self):
        self.breakpoints = []

    def parse_one_command(self, line):
        parts = line.split('//%')
        if len(parts) != 2:
            return None
        else:
            return parts[1].strip() # take off trailing whitespace

    def parse_source_files(self, source_files):
        for source_file in source_files:
            file_handle = open(source_file)
            lines = file_handle.readlines()
            line_number = 0
            for line in lines:
                line_number = line_number + 1 # 1-based, so we do this first
                command = self.parse_one_command(line)
                if command != None:
                    breakpoint = {}
                    breakpoint['file_name'] = source_file
                    breakpoint['line_number'] = line_number
                    breakpoint['command'] = command
                    self.breakpoints.append(breakpoint)

    def set_breakpoints(self, target):
        for breakpoint in self.breakpoints:
            breakpoint['breakpoint'] = target.BreakpointCreateByLocation(breakpoint['file_name'], breakpoint['line_number'])

    def handle_breakpoint(self, test, breakpoint_id):
        for breakpoint in self.breakpoints:
            if breakpoint['breakpoint'].GetID() == breakpoint_id:
                test.execute_user_command(breakpoint['command'])
                return

def BuildMakefile(mydir):
    categories = {}

    for f in os.listdir(os.getcwd()):
        t = source_type(f)
        if t:
            if t in categories.keys():
                categories[t].append(f)
            else:
                categories[t] = [f]

    makefile = open("Makefile", 'w+')

    level = os.sep.join([".."] * len(mydir.split(os.sep))) + os.sep + "make"

    makefile.write("LEVEL = " + level + "\n")
   
    for t in categories.keys():
        line = t + " := " + " ".join(categories[t])
        makefile.write(line + "\n")

    if ('OBJCXX_SOURCES' in categories.keys()) or ('OBJC_SOURCES' in categories.keys()):
        makefile.write("LDFLAGS = $(CFLAGS) -lobjc -framework Foundation\n")

    if ('CXX_SOURCES' in categories.keys()):
        makefile.write("CXXFLAGS += -std-c++11\n")

    makefile.write("include $(LEVEL)/Makefile.rules\n")
    makefile.flush()
    makefile.close()

def CleanMakefile():
    if (os.path.isfile("Makefile")):
        os.unlink("Makefile")

class InlineTest(TestBase):
    # Internal implementation

    def buildDsymWithImplicitMakefile(self):
        BuildMakefile(self.mydir)
        self.buildDsym()

    def buildDwarfWithImplicitMakefile(self):
        BuildMakefile(self.mydir)
        self.buildDwarf()

    def test_with_dsym(self):
        self.buildDsymWithImplicitMakefile()
        self.do_test()

    def test_with_dwarf(self):
        self.buildDwarfWithImplicitMakefile()
        self.do_test()

    def execute_user_command(self, __command):
        exec __command in globals(), locals()

    def do_test(self):
        exe_name = "a.out"
        exe = os.path.join(os.getcwd(), exe_name)
        source_files = [ f for f in os.listdir(os.getcwd()) if source_type(f) ]
        target = self.dbg.CreateTarget(exe)

        parser = CommandParser()
        parser.parse_source_files(source_files)
        parser.set_breakpoints(target)

        process = target.LaunchSimple(None, None, os.getcwd())

        while lldbutil.get_stopped_thread(process, lldb.eStopReasonBreakpoint):
            thread = lldbutil.get_stopped_thread(process, lldb.eStopReasonBreakpoint)
            breakpoint_id = thread.GetStopReasonDataAtIndex (0)
            parser.handle_breakpoint(self, breakpoint_id)
            process.Continue()

    @classmethod
    def classCleanup(cls):
        CleanMakefile()

    # Utilities for testcases

    def check_expression (self, expression, expected_result, use_summary = True):
        value = self.frame().EvaluateExpression (expression)
        self.assertTrue(value.IsValid(), expression+"returned a valid value")
        if self.TraceOn():
            print value.GetSummary()
            print value.GetValue()
        if use_summary:
            answer = value.GetSummary()
        else:
            answer = value.GetValue()
        report_str = "%s expected: %s got: %s"%(expression, expected_result, answer)
        self.assertTrue(answer == expected_result, report_str)

def MakeInlineTest(__file, __globals):
    # Derive the test name from the current file name
    file_basename = os.path.basename(__file)
    InlineTest.mydir = TestBase.compute_mydir(__file)

    test_name, _ = os.path.splitext(file_basename)
    # Build the test case 
    test = new.classobj(test_name, (InlineTest,), {})
    test.name = test_name
    # Add the test case to the globals, and hide InlineTest
    __globals.update({test_name : test})
    del globals()["InlineTest"]


