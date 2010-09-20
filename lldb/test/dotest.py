#!/usr/bin/env python

"""
A simple testing framework for lldb using python's unit testing framework.

Tests for lldb are written as python scripts which take advantage of the script
bridging provided by LLDB.framework to interact with lldb core.

A specific naming pattern is followed by the .py script to be recognized as
a module which implements a test scenario, namely, Test*.py.

To specify the directories where "Test*.py" python test scripts are located,
you need to pass in a list of directory names.  By default, the current
working directory is searched if nothing is specified on the command line.

Type:

./dotest.py -h

for available options.
"""

import os, signal, sys, time
import unittest2

class _WritelnDecorator(object):
    """Used to decorate file-like objects with a handy 'writeln' method"""
    def __init__(self,stream):
        self.stream = stream

    def __getattr__(self, attr):
        if attr in ('stream', '__getstate__'):
            raise AttributeError(attr)
        return getattr(self.stream,attr)

    def writeln(self, arg=None):
        if arg:
            self.write(arg)
        self.write('\n') # text-mode streams translate to \r\n if needed

#
# Global variables:
#

# The test suite.
suite = unittest2.TestSuite()

# The config file is optional.
configFile = None

# Delay startup in order for the debugger to attach.
delay = False

# Ignore the build search path relative to this script to locate the lldb.py module.
ignore = False

# Default verbosity is 0.
verbose = 0

# By default, search from the current working directory.
testdirs = [ os.getcwd() ]

# Separator string.
separator = '-' * 70

# Decorated sys.stderr for our consumption.
err = _WritelnDecorator(sys.stderr)


def usage():
    print """
Usage: dotest.py [option] [args]
where options:
-h   : print this help message and exit (also --help)
-c   : read a config file specified after this option
-d   : delay startup for 10 seconds (in order for the debugger to attach)
-i   : ignore (don't bailout) if 'lldb.py' module cannot be located in the build
       tree relative to this script; use PYTHONPATH to locate the module
-t   : trace lldb command execution and result
-v   : do verbose mode of unittest framework

and:
args : specify a list of directory names to search for python Test*.py scripts
       if empty, search from the curret working directory, instead

Running of this script also sets up the LLDB_TEST environment variable so that
individual test cases can locate their supporting files correctly.  The script
tries to set up Python's search paths for modules by looking at the build tree
relative to this script.  See also the '-i' option.

Environment variables related to loggings:

o LLDB_LOG: if defined, specifies the log file pathname for the 'lldb' subsystem
  with a default option of 'event process' if LLDB_LOG_OPTION is not defined.

o GDB_REMOTE_LOG: if defined, specifies the log file pathname for the
  'process.gdb-remote' subsystem with a default option of 'packets' if
  GDB_REMOTE_LOG_OPTION is not defined.
"""
    sys.exit(0)


def parseOptionsAndInitTestdirs():
    """Initialize the list of directories containing our unittest scripts.

    '-h/--help as the first option prints out usage info and exit the program.
    """

    global configFile
    global delay
    global inore
    global verbose
    global testdirs

    if len(sys.argv) == 1:
        return

    # Process possible trace and/or verbose flag, among other things.
    index = 1
    for i in range(1, len(sys.argv)):
        if not sys.argv[index].startswith('-'):
            # End of option processing.
            break

        if sys.argv[index].find('-h') != -1:
            usage()
        elif sys.argv[index].startswith('-c'):
            # Increment by 1 to fetch the config file name option argument.
            index += 1
            if index >= len(sys.argv) or sys.argv[index].startswith('-'):
                usage()
            configFile = sys.argv[index]
            if not os.path.isfile(configFile):
                print "Config file:", configFile, "does not exist!"
                usage()
            index += 1
        elif sys.argv[index].startswith('-d'):
            delay = True
            index += 1
        elif sys.argv[index].startswith('-i'):
            ignore = True
            index += 1
        elif sys.argv[index].startswith('-t'):
            os.environ["LLDB_COMMAND_TRACE"] = "YES"
            index += 1
        elif sys.argv[index].startswith('-v'):
            verbose = 2
            index += 1
        else:
            print "Unknown option: ", sys.argv[index]
            usage()

    # Gather all the dirs passed on the command line.
    if len(sys.argv) > index:
        testdirs = map(os.path.abspath, sys.argv[index:])


def setupSysPath():
    """Add LLDB.framework/Resources/Python to the search paths for modules."""

    # Get the directory containing the current script.
    scriptPath = sys.path[0]
    if not scriptPath.endswith('test'):
        print "This script expects to reside in lldb's test directory."
        sys.exit(-1)

    os.environ["LLDB_TEST"] = scriptPath
    pluginPath = os.path.join(scriptPath, 'plugins')

    # Append script dir and plugin dir to the sys.path.
    sys.path.append(scriptPath)
    sys.path.append(pluginPath)
    
    global ignore

    # The '-i' option is used to skip looking for lldb.py in the build tree.
    if ignore:
        return
        
    base = os.path.abspath(os.path.join(scriptPath, os.pardir))
    dbgPath = os.path.join(base, 'build', 'Debug', 'LLDB.framework',
                           'Resources', 'Python')
    relPath = os.path.join(base, 'build', 'Release', 'LLDB.framework',
                           'Resources', 'Python')
    baiPath = os.path.join(base, 'build', 'BuildAndIntegration',
                           'LLDB.framework', 'Resources', 'Python')

    lldbPath = None
    if os.path.isfile(os.path.join(dbgPath, 'lldb.py')):
        lldbPath = dbgPath
    elif os.path.isfile(os.path.join(relPath, 'lldb.py')):
        lldbPath = relPath
    elif os.path.isfile(os.path.join(baiPath, 'lldb.py')):
        lldbPath = baiPath

    if not lldbPath:
        print 'This script requires lldb.py to be in either ' + dbgPath + ',',
        print relPath + ', or ' + baiPath
        sys.exit(-1)

    # This is to locate the lldb.py module.  Insert it right after sys.path[0].
    sys.path[1:1] = [lldbPath]


def visit(prefix, dir, names):
    """Visitor function for os.path.walk(path, visit, arg)."""

    global suite

    for name in names:
        if os.path.isdir(os.path.join(dir, name)):
            continue

        if '.py' == os.path.splitext(name)[1] and name.startswith(prefix):
            # We found a pattern match for our test case.  Add it to the suite.
            if not sys.path.count(dir):
                sys.path.append(dir)
            base = os.path.splitext(name)[0]
            suite.addTests(unittest2.defaultTestLoader.loadTestsFromName(base))


#
# Start the actions by first parsing the options while setting up the test
# directories, followed by setting up the search paths for lldb utilities;
# then, we walk the directory trees and collect the tests into our test suite.
#
parseOptionsAndInitTestdirs()
setupSysPath()

#
# If '-d' is specified, do a delay of 10 seconds for the debugger to attach.
#
if delay:
    def alarm_handler(*args):
        raise Exception("timeout")

    signal.signal(signal.SIGALRM, alarm_handler)
    signal.alarm(10)
    sys.stdout.write("pid=" + str(os.getpid()) + '\n')
    sys.stdout.write("Enter RET to proceed (or timeout after 10 seconds):")
    sys.stdout.flush()
    try:
        text = sys.stdin.readline()
    except:
        text = ""
    signal.alarm(0)
    sys.stdout.write("proceeding...\n")
    pass

#
# Walk through the testdirs while collecting test cases.
#
for testdir in testdirs:
    os.path.walk(testdir, visit, 'Test')

# Now that we have loaded all the test cases, run the whole test suite.
err.writeln(separator)
err.writeln("Collected %d test%s" % (suite.countTestCases(),
                                     suite.countTestCases() != 1 and "s" or ""))
err.writeln()

# For the time being, let's bracket the test runner within the
# lldb.SBDebugger.Initialize()/Terminate() pair.
import lldb, atexit
lldb.SBDebugger.Initialize()
atexit.register(lambda: lldb.SBDebugger.Terminate())

# Create a singleton SBDebugger in the lldb namespace.
lldb.DBG = lldb.SBDebugger.Create()

# Turn on logging for debugging purposes if ${LLDB_LOG} environment variable is
# defined.  Use ${LLDB_LOG} to specify the log file.
ci = lldb.DBG.GetCommandInterpreter()
res = lldb.SBCommandReturnObject()
if ("LLDB_LOG" in os.environ):
    if ("LLDB_LOG_OPTION" in os.environ):
        lldb_log_option = os.environ["LLDB_LOG_OPTION"]
    else:
        lldb_log_option = "event process"
    ci.HandleCommand(
        "log enable -f " + os.environ["LLDB_LOG"] + " lldb " + lldb_log_option,
        res)
    if not res.Succeeded():
        raise Exception('log enable failed (check LLDB_LOG env variable.')
# Ditto for gdb-remote logging if ${GDB_REMOTE_LOG} environment variable is defined.
# Use ${GDB_REMOTE_LOG} to specify the log file.
if ("GDB_REMOTE_LOG" in os.environ):
    if ("GDB_REMOTE_LOG_OPTION" in os.environ):
        gdb_remote_log_option = os.environ["GDB_REMOTE_LOG_OPTION"]
    else:
        gdb_remote_log_option = "packets"
    ci.HandleCommand(
        "log enable -f " + os.environ["GDB_REMOTE_LOG"] + " process.gdb-remote "
        + gdb_remote_log_option,
        res)
    if not res.Succeeded():
        raise Exception('log enable failed (check GDB_REMOTE_LOG env variable.')

# Install the control-c handler.
unittest2.signals.installHandler()

# Invoke the default TextTestRunner to run the test suite.
result = unittest2.TextTestRunner(verbosity=verbose).run(suite)

if ("LLDB_TESTSUITE_FORCE_FINISH" in os.environ):
    import subprocess
    print "Terminating Test suite..."
    subprocess.Popen(["/bin/sh", "-c", "kill %s; exit 0" % (os.getpid())])

# Exiting.
sys.exit(not result.wasSuccessful)
