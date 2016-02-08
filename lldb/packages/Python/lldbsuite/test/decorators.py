from __future__ import print_function
from __future__ import absolute_import

# System modules
from distutils.version import LooseVersion, StrictVersion
from functools import wraps
import os
import re
import sys

# Third-party modules
import six
import unittest2

# LLDB modules
import use_lldb_suite

import lldb
from . import configuration
from . import test_categories
from .result_formatter import EventBuilder
from lldbsuite.support import funcutils
from lldbsuite.test import lldbplatformutil

class DecorateMode:
    Skip, Xfail = range(2)


def _check_expected_version(comparison, expected, actual):
    def fn_leq(x,y): return x <= y
    def fn_less(x,y): return x < y
    def fn_geq(x,y): return x >= y
    def fn_greater(x,y): return x > y
    def fn_eq(x,y): return x == y
    def fn_neq(x,y): return x != y

    op_lookup = {
        "==": fn_eq,
        "=": fn_eq,
        "!=": fn_neq,
        "<>": fn_neq,
        ">": fn_greater,
        "<": fn_less,
        ">=": fn_geq,
        "<=": fn_leq
        }
    expected_str = '.'.join([str(x) for x in expected])
    actual_str = '.'.join([str(x) for x in actual])

    return op_lookup[comparison](LooseVersion(actual_str), LooseVersion(expected_str))

def _check_list_or_lambda(list_or_lambda, value):
    if six.callable(list_or_lambda):
        return list_or_lambda(value)
    elif isinstance(list_or_lambda, list):
        for item in list_or_lambda:
            if value in item:
                return True
        return False
    elif isinstance(list_or_lambda, str):
        return value is None or value in list_or_lambda
    else:
        return list_or_lambda is None or value is None or list_or_lambda == value

def expectedFailure(expected_fn, bugnumber=None):
    def expectedFailure_impl(func):
        if isinstance(func, type) and issubclass(func, unittest2.TestCase):
            raise Exception("Decorator can only be used to decorate a test method")
        @wraps(func)
        def wrapper(*args, **kwargs):
            from unittest2 import case
            self = args[0]
            if funcutils.requires_self(expected_fn):
                xfail_reason = expected_fn(self)
            else:
                xfail_reason = expected_fn()
            if xfail_reason is not None:
                if configuration.results_formatter_object is not None:
                    # Mark this test as expected to fail.
                    configuration.results_formatter_object.handle_event(
                        EventBuilder.event_for_mark_test_expected_failure(self))
                xfail_func = unittest2.expectedFailure(func)
                xfail_func(*args, **kwargs)
            else:
                func(*args, **kwargs)
        return wrapper
    # Some decorators can be called both with no arguments (e.g. @expectedFailureWindows)
    # or with arguments (e.g. @expectedFailureWindows(compilers=['gcc'])).  When called
    # the first way, the first argument will be the actual function because decorators are
    # weird like that.  So this is basically a check that says "which syntax was the original
    # function decorated with?"
    if six.callable(bugnumber):
        return expectedFailure_impl(bugnumber)
    else:
        return expectedFailure_impl

def skipTestIfFn(expected_fn, bugnumber=None):
    def skipTestIfFn_impl(func):
        if isinstance(func, type) and issubclass(func, unittest2.TestCase):
            raise Exception("@skipTestIfFn can only be used to decorate a test method")

        @wraps(func)
        def wrapper(*args, **kwargs):
            from unittest2 import case
            self = args[0]
            if funcutils.requires_self(expected_fn):
                reason = expected_fn(self)
            else:
                reason = expected_fn()

            if reason is not None:
               self.skipTest(reason)
            else:
                func(*args, **kwargs)
        return wrapper

    # Some decorators can be called both with no arguments (e.g. @expectedFailureWindows)
    # or with arguments (e.g. @expectedFailureWindows(compilers=['gcc'])).  When called
    # the first way, the first argument will be the actual function because decorators are
    # weird like that.  So this is basically a check that says "how was the decorator used"
    if six.callable(bugnumber):
        return skipTestIfFn_impl(bugnumber)
    else:
        return skipTestIfFn_impl

def _decorateTest(mode,
                 bugnumber=None, oslist=None, hostoslist=None,
                 compiler=None, compiler_version=None,
                 archs=None, triple=None,
                 debug_info=None,
                 swig_version=None, py_version=None,
                 remote=None):
    def fn(self):
        skip_for_os = _check_list_or_lambda(oslist, self.getPlatform())
        skip_for_hostos = _check_list_or_lambda(hostoslist, lldbplatformutil.getHostPlatform())
        skip_for_compiler = _check_list_or_lambda(self.getCompiler(), compiler) and self.expectedCompilerVersion(compiler_version)
        skip_for_arch = _check_list_or_lambda(archs, self.getArchitecture())
        skip_for_debug_info = _check_list_or_lambda(debug_info, self.debug_info)
        skip_for_triple = triple is None or re.match(triple, lldb.DBG.GetSelectedPlatform().GetTriple())
        skip_for_swig_version = (swig_version is None) or (not hasattr(lldb, 'swig_version')) or (_check_expected_version(swig_version[0], swig_version[1], lldb.swig_version))
        skip_for_py_version = (py_version is None) or _check_expected_version(py_version[0], py_version[1], sys.version_info)
        skip_for_remote = (remote is None) or (remote == (lldb.remote_platform is not None))

        # For the test to be skipped, all specified (e.g. not None) parameters must be True.
        # An unspecified parameter means "any", so those are marked skip by default.  And we skip
        # the final test if all conditions are True.
        conditions = [(oslist, skip_for_os, "target o/s"),
                      (hostoslist, skip_for_hostos, "host o/s"),
                      (compiler, skip_for_compiler, "compiler or version"),
                      (archs, skip_for_arch, "architecture"),
                      (debug_info, skip_for_debug_info, "debug info format"),
                      (triple, skip_for_triple, "target triple"),
                      (swig_version, skip_for_swig_version, "swig version"),
                      (py_version, skip_for_py_version, "python version"),
                      (remote, skip_for_remote, "platform locality (remote/local)")]
        reasons = []
        final_skip_result = True
        for this_condition in conditions:
            final_skip_result = final_skip_result and this_condition[1]
            if this_condition[0] is not None and this_condition[1]:
                reasons.append(this_condition[2])
        reason_str = None
        if final_skip_result:
            mode_str = {DecorateMode.Skip : "skipping", DecorateMode.Xfail : "xfailing"}[mode]
            if len(reasons) > 0:
                reason_str = ",".join(reasons)
                reason_str = "{} due to the following parameter(s): {}".format(mode_str, reason_str)
            else:
                reason_str = "{} unconditionally"
            if bugnumber is not None and not six.callable(bugnumber):
                reason_str = reason_str + " [" + str(bugnumber) + "]"
        return reason_str

    if mode == DecorateMode.Skip:
        return skipTestIfFn(fn, bugnumber)
    elif mode == DecorateMode.Xfail:
        return expectedFailure(fn, bugnumber)
    else:
        return None

# provide a function to xfail on defined oslist, compiler version, and archs
# if none is specified for any argument, that argument won't be checked and thus means for all
# for example,
# @expectedFailureAll, xfail for all platform/compiler/arch,
# @expectedFailureAll(compiler='gcc'), xfail for gcc on all platform/architecture
# @expectedFailureAll(bugnumber, ["linux"], "gcc", ['>=', '4.9'], ['i386']), xfail for gcc>=4.9 on linux with i386
def expectedFailureAll(bugnumber=None,
                       oslist=None, hostoslist=None,
                       compiler=None, compiler_version=None,
                       archs=None, triple=None,
                       debug_info=None,
                       swig_version=None, py_version=None,
                       remote=None):
    return _decorateTest(DecorateMode.Xfail,
                        bugnumber=bugnumber,
                        oslist=oslist, hostoslist=hostoslist,
                        compiler=compiler, compiler_version=compiler_version,
                        archs=archs, triple=triple,
                        debug_info=debug_info,
                        swig_version=swig_version, py_version=py_version,
                        remote=remote)


# provide a function to skip on defined oslist, compiler version, and archs
# if none is specified for any argument, that argument won't be checked and thus means for all
# for example,
# @skipIf, skip for all platform/compiler/arch,
# @skipIf(compiler='gcc'), skip for gcc on all platform/architecture
# @skipIf(bugnumber, ["linux"], "gcc", ['>=', '4.9'], ['i386']), skip for gcc>=4.9 on linux with i386
def skipIf(bugnumber=None,
           oslist=None, hostoslist=None,
           compiler=None, compiler_version=None,
           archs=None, triple=None,
           debug_info=None,
           swig_version=None, py_version=None,
           remote=None):
    return _decorateTest(DecorateMode.Skip,
                        bugnumber=bugnumber,
                        oslist=oslist, hostoslist=hostoslist,
                        compiler=compiler, compiler_version=compiler_version,
                        archs=archs, triple=triple,
                        debug_info=debug_info,
                        swig_version=swig_version, py_version=py_version,
                        remote=remote)

def _skip_for_android(reason, api_levels, archs):
    def impl(obj):
        result = lldbplatformutil.match_android_device(obj.getArchitecture(), valid_archs=archs, valid_api_levels=api_levels)
        return reason if result else None
    return impl

def add_test_categories(cat):
    """Add test categories to a TestCase method"""
    cat = test_categories.validate(cat, True)
    def impl(func):
        if isinstance(func, type) and issubclass(func, unittest2.TestCase):
            raise Exception("@add_test_categories can only be used to decorate a test method")
        if hasattr(func, "categories"):
            cat.extend(func.categories)
        func.categories = cat
        return func

    return impl

def benchmarks_test(func):
    """Decorate the item as a benchmarks test."""
    def should_skip_benchmarks_test():
        return "benchmarks test"

    # Mark this function as such to separate them from the regular tests.
    result = skipTestIfFn(should_skip_benchmarks_test)(func)
    result.__benchmarks_test__ = True
    return result

def no_debug_info_test(func):
    """Decorate the item as a test what don't use any debug info. If this annotation is specified
       then the test runner won't generate a separate test for each debug info format. """
    if isinstance(func, type) and issubclass(func, unittest2.TestCase):
        raise Exception("@no_debug_info_test can only be used to decorate a test method")
    @wraps(func)
    def wrapper(self, *args, **kwargs):
        return func(self, *args, **kwargs)

    # Mark this function as such to separate them from the regular tests.
    wrapper.__no_debug_info_test__ = True
    return wrapper

def debugserver_test(func):
    """Decorate the item as a debugserver test."""
    def should_skip_debugserver_test():
        return "debugserver tests" if configuration.dont_do_debugserver_test else None
    return skipTestIfFn(should_skip_debugserver_test)(func)

def llgs_test(func):
    """Decorate the item as a lldb-server test."""
    def should_skip_llgs_tests():
        return "llgs tests" if configuration.dont_do_llgs_test else None
    return skipTestIfFn(should_skip_llgs_tests)(func)

def not_remote_testsuite_ready(func):
    """Decorate the item as a test which is not ready yet for remote testsuite."""
    def is_remote():
        return "Not ready for remote testsuite" if lldb.remote_platform else None
    return skipTestIfFn(is_remote)(func)

# You can also pass not_in(list) to reverse the sense of the test for the arguments that
# are simple lists, namely oslist, compiler, and debug_info.

def not_in(iterable):
    return lambda x : x not in iterable

def _match_architectures(archs, actual_arch):
    retype = type(re.compile('hello, world'))
    list_passes = isinstance(archs, list) and actual_arch in archs
    basestring_passes = isinstance(archs, six.string_types) and actual_arch == archs
    regex_passes = isinstance(archs, retype) and re.match(archs, actual_arch)

    return (list_passes or basestring_passes or regex_passes)

def expectedFailureDwarf(bugnumber=None):
    return expectedFailureAll(bugnumber=bugnumber, debug_info="dwarf")

def expectedFailureDwo(bugnumber=None):
    return expectedFailureAll(bugnumber=bugnumber, debug_info="dwo")

def expectedFailureDsym(bugnumber=None):
    return expectedFailureAll(bugnumber=bugnumber, debug_info="dsym")

def expectedFailureCompiler(compiler, compiler_version=None, bugnumber=None):
    if compiler_version is None:
        compiler_version=['=', None]
    return expectedFailureAll(bugnumber=bugnumber, compiler=compiler, compiler_version=compiler_version)

# to XFAIL a specific clang versions, try this
# @expectedFailureClang('bugnumber', ['<=', '3.4'])
def expectedFailureClang(bugnumber=None, compiler_version=None):
    return expectedFailureCompiler('clang', compiler_version, bugnumber)

def expectedFailureGcc(bugnumber=None, compiler_version=None):
    return expectedFailureCompiler('gcc', compiler_version, bugnumber)

def expectedFailureIcc(bugnumber=None):
    return expectedFailureCompiler('icc', None, bugnumber)

def expectedFailureArch(arch, bugnumber=None):
    return expectedFailureAll(archs=arch, bugnumber=bugnumber)

def expectedFailurei386(bugnumber=None):
    return expectedFailureArch('i386', bugnumber)

def expectedFailurex86_64(bugnumber=None):
    return expectedFailureArch('x86_64', bugnumber)

def expectedFailureOS(oslist, bugnumber=None, compilers=None, debug_info=None, archs=None):
    return expectedFailureAll(oslist=oslist, bugnumber=bugnumber, compiler=compilers, archs=archs, debug_info=debug_info)

def expectedFailureHostOS(oslist, bugnumber=None, compilers=None):
    return expectedFailureAll(hostoslist=oslist, bugnumber=bugnumber)

def expectedFailureDarwin(bugnumber=None, compilers=None, debug_info=None):
    # For legacy reasons, we support both "darwin" and "macosx" as OS X triples.
    return expectedFailureOS(lldbplatformutil.getDarwinOSTriples(), bugnumber, compilers, debug_info=debug_info)

def expectedFailureFreeBSD(bugnumber=None, compilers=None, debug_info=None):
    return expectedFailureOS(['freebsd'], bugnumber, compilers, debug_info=debug_info)

def expectedFailureLinux(bugnumber=None, compilers=None, debug_info=None, archs=None):
    return expectedFailureOS(['linux'], bugnumber, compilers, debug_info=debug_info, archs=archs)

def expectedFailureNetBSD(bugnumber=None, compilers=None, debug_info=None):
    return expectedFailureOS(['netbsd'], bugnumber, compilers, debug_info=debug_info)

def expectedFailureHostWindows(bugnumber=None, compilers=None):
    return expectedFailureHostOS(['windows'], bugnumber, compilers)

def expectedFailureAndroid(bugnumber=None, api_levels=None, archs=None):
    """ Mark a test as xfail for Android.

    Arguments:
        bugnumber - The LLVM pr associated with the problem.
        api_levels - A sequence of numbers specifying the Android API levels
            for which a test is expected to fail. None means all API level.
        arch - A sequence of architecture names specifying the architectures
            for which a test is expected to fail. None means all architectures.
    """
    return expectedFailure(_skip_for_android("xfailing on android", api_levels, archs), bugnumber)

# Flakey tests get two chances to run. If they fail the first time round, the result formatter
# makes sure it is run one more time.
def expectedFlakey(expected_fn, bugnumber=None):
    def expectedFailure_impl(func):
        @wraps(func)
        def wrapper(*args, **kwargs):
            self = args[0]
            if expected_fn(self):
                # Send event marking test as explicitly eligible for rerunning.
                if configuration.results_formatter_object is not None:
                    # Mark this test as rerunnable.
                    configuration.results_formatter_object.handle_event(
                        EventBuilder.event_for_mark_test_rerun_eligible(self))
            func(*args, **kwargs)
        return wrapper
    # Some decorators can be called both with no arguments (e.g. @expectedFailureWindows)
    # or with arguments (e.g. @expectedFailureWindows(compilers=['gcc'])).  When called
    # the first way, the first argument will be the actual function because decorators are
    # weird like that.  So this is basically a check that says "which syntax was the original
    # function decorated with?"
    if six.callable(bugnumber):
        return expectedFailure_impl(bugnumber)
    else:
        return expectedFailure_impl

def expectedFlakeyDwarf(bugnumber=None):
    def fn(self):
        return self.debug_info == "dwarf"
    return expectedFlakey(fn, bugnumber)

def expectedFlakeyDsym(bugnumber=None):
    def fn(self):
        return self.debug_info == "dwarf"
    return expectedFlakey(fn, bugnumber)

def expectedFlakeyOS(oslist, bugnumber=None, compilers=None):
    def fn(self):
        return (self.getPlatform() in oslist and
                self.expectedCompiler(compilers))
    return expectedFlakey(fn, bugnumber)

def expectedFlakeyDarwin(bugnumber=None, compilers=None):
    # For legacy reasons, we support both "darwin" and "macosx" as OS X triples.
    return expectedFlakeyOS(lldbplatformutil.getDarwinOSTriples(), bugnumber, compilers)

def expectedFlakeyFreeBSD(bugnumber=None, compilers=None):
    return expectedFlakeyOS(['freebsd'], bugnumber, compilers)

def expectedFlakeyLinux(bugnumber=None, compilers=None):
    return expectedFlakeyOS(['linux'], bugnumber, compilers)

def expectedFlakeyNetBSD(bugnumber=None, compilers=None):
    return expectedFlakeyOS(['netbsd'], bugnumber, compilers)

def expectedFlakeyCompiler(compiler, compiler_version=None, bugnumber=None):
    if compiler_version is None:
        compiler_version=['=', None]
    def fn(self):
        return compiler in self.getCompiler() and self.expectedCompilerVersion(compiler_version)
    return expectedFlakey(fn, bugnumber)

# @expectedFlakeyClang('bugnumber', ['<=', '3.4'])
def expectedFlakeyClang(bugnumber=None, compiler_version=None):
    return expectedFlakeyCompiler('clang', compiler_version, bugnumber)

# @expectedFlakeyGcc('bugnumber', ['<=', '3.4'])
def expectedFlakeyGcc(bugnumber=None, compiler_version=None):
    return expectedFlakeyCompiler('gcc', compiler_version, bugnumber)

def expectedFlakeyAndroid(bugnumber=None, api_levels=None, archs=None):
    return expectedFlakey(_skip_for_android("flakey on android", api_levels, archs), bugnumber)

def skipIfRemote(func):
    """Decorate the item to skip tests if testing remotely."""
    def is_remote():
        return "skip on remote platform" if lldb.remote_platform else None
    return skipTestIfFn(is_remote)(func)

def skipUnlessListedRemote(remote_list=None):
    def is_remote_unlisted(self):
        if remote_list and lldb.remote_platform:
            triple = self.dbg.GetSelectedPlatform().GetTriple()
            for r in remote_list:
                if r in triple:
                    return None
            return "skipping because remote is not listed"
        else:
            return None
    return skipTestIfFn(is_remote_unlisted)

def skipIfRemoteDueToDeadlock(func):
    """Decorate the item to skip tests if testing remotely due to the test deadlocking."""
    def is_remote():
        return "skip on remote platform (deadlocks)" if lldb.remote_platform else None
    return skipTestIfFn(is_remote)(func)

def skipIfNoSBHeaders(func):
    """Decorate the item to mark tests that should be skipped when LLDB is built with no SB API headers."""
    def are_sb_headers_missing():
        if lldbplatformutil.getHostPlatform() == 'darwin':
            header = os.path.join(os.environ["LLDB_LIB_DIR"], 'LLDB.framework', 'Versions','Current','Headers','LLDB.h')
        else:
            header = os.path.join(os.environ["LLDB_SRC"], "include", "lldb", "API", "LLDB.h")
        if not os.path.exists(header):
            return "skip because LLDB.h header not found"
        return None

    return skipTestIfFn(are_sb_headers_missing)(func)

def skipIfiOSSimulator(func):
    """Decorate the item to skip tests that should be skipped on the iOS Simulator."""
    def is_ios_simulator():
        return "skip on the iOS Simulator" if configuration.lldb_platform_name == 'ios-simulator' else None
    return skipTestIfFn(is_ios_simulator)(func)

def skipIfFreeBSD(func):
    """Decorate the item to skip tests that should be skipped on FreeBSD."""
    return skipIfPlatform(["freebsd"])(func)

def skipIfNetBSD(func):
    """Decorate the item to skip tests that should be skipped on NetBSD."""
    return skipIfPlatform(["netbsd"])(func)

def skipIfDarwin(func):
    """Decorate the item to skip tests that should be skipped on Darwin."""
    return skipIfPlatform(lldbplatformutil.getDarwinOSTriples())(func)

def skipIfLinux(func):
    """Decorate the item to skip tests that should be skipped on Linux."""
    return skipIfPlatform(["linux"])(func)

def skipUnlessHostLinux(func):
    """Decorate the item to skip tests that should be skipped on any non Linux host."""
    return skipUnlessHostPlatform(["linux"])(func)

def skipIfWindows(func):
    """Decorate the item to skip tests that should be skipped on Windows."""
    return skipIfPlatform(["windows"])(func)

def skipIfHostWindows(func):
    """Decorate the item to skip tests that should be skipped on Windows."""
    return skipIfHostPlatform(["windows"])(func)

def skipUnlessWindows(func):
    """Decorate the item to skip tests that should be skipped on any non-Windows platform."""
    return skipUnlessPlatform(["windows"])(func)

def skipUnlessDarwin(func):
    """Decorate the item to skip tests that should be skipped on any non Darwin platform."""
    return skipUnlessPlatform(lldbplatformutil.getDarwinOSTriples())(func)

def skipUnlessGoInstalled(func):
    """Decorate the item to skip tests when no Go compiler is available."""
    def is_go_missing(self):
        compiler = self.getGoCompilerVersion()
        if not compiler:
            return "skipping because go compiler not found"
        match_version = re.search(r"(\d+\.\d+(\.\d+)?)", compiler)
        if not match_version:
            # Couldn't determine version.
            return "skipping because go version could not be parsed out of {}".format(compiler)
        else:
            min_strict_version = StrictVersion("1.4.0")
            compiler_strict_version = StrictVersion(match_version.group(1))
            if compiler_strict_version < min_strict_version:
                return "skipping because available version ({}) does not meet minimum required version ({})".format(
                    compiler_strict_version, min_strict_version)
        return None
    return skipTestIfFn(is_go_missing)(func)

def skipIfHostIncompatibleWithRemote(func):
    """Decorate the item to skip tests if binaries built on this host are incompatible."""
    def is_host_incompatible_with_remote(self):
        host_arch = self.getLldbArchitecture()
        host_platform = lldbplatformutil.getHostPlatform()
        target_arch = self.getArchitecture()
        target_platform = 'darwin' if self.platformIsDarwin() else self.getPlatform()
        if not (target_arch == 'x86_64' and host_arch == 'i386') and host_arch != target_arch:
            return "skipping because target %s is not compatible with host architecture %s" % (target_arch, host_arch)
        elif target_platform != host_platform:
            return "skipping because target is %s but host is %s" % (target_platform, host_platform)
        return None
    return skipTestIfFn(is_host_incompatible_with_remote)(func)

def skipIfHostPlatform(oslist):
    """Decorate the item to skip tests if running on one of the listed host platforms."""
    return skipIf(hostoslist=oslist)

def skipUnlessHostPlatform(oslist):
    """Decorate the item to skip tests unless running on one of the listed host platforms."""
    return skipIf(hostoslist=not_in(oslist))

def skipUnlessArch(archs):
    """Decorate the item to skip tests unless running on one of the listed architectures."""
    # This decorator cannot be ported to `skipIf` yet because it is uused with regular
    # expressions, which the common matcher does not yet support.
    def myImpl(func):
        if isinstance(func, type) and issubclass(func, unittest2.TestCase):
            raise Exception("@skipUnlessArch can only be used to decorate a test method")

        @wraps(func)
        def wrapper(*args, **kwargs):
            self = args[0]
            if not _match_architectures(archs, self.getArchitecture()):
                self.skipTest("skipping for architecture %s" % (self.getArchitecture())) 
            else:
                func(*args, **kwargs)
        return wrapper

    return myImpl

def skipIfPlatform(oslist):
    """Decorate the item to skip tests if running on one of the listed platforms."""
    # This decorator cannot be ported to `skipIf` yet because it is used on entire
    # classes, which `skipIf` explicitly forbids.
    return unittest2.skipIf(lldbplatformutil.getPlatform() in oslist,
                            "skip on %s" % (", ".join(oslist)))

def skipUnlessPlatform(oslist):
    """Decorate the item to skip tests unless running on one of the listed platforms."""
    # This decorator cannot be ported to `skipIf` yet because it is used on entire
    # classes, which `skipIf` explicitly forbids.
    return unittest2.skipUnless(lldbplatformutil.getPlatform() in oslist,
                                "requires on of %s" % (", ".join(oslist)))


def skipIfDebugInfo(bugnumber=None, debug_info=None):
    return skipIf(bugnumber=bugnumber, debug_info=debug_info)

def skipIfDWO(bugnumber=None):
    return skipIfDebugInfo(bugnumber, ["dwo"])

def skipIfDwarf(bugnumber=None):
    return skipIfDebugInfo(bugnumber, ["dwarf"])

def skipIfDsym(bugnumber=None):
    return skipIfDebugInfo(bugnumber, ["dsym"])

def skipIfGcc(func):
    """Decorate the item to skip tests that should be skipped if building with gcc ."""
    return skipIf(compiler="gcc")(func)

def skipIfIcc(func):
    """Decorate the item to skip tests that should be skipped if building with icc ."""
    return skipIf(compiler="icc")(func)

def skipIfi386(func):
    """Decorate the item to skip tests that should be skipped if building 32-bit."""
    return skipIf(archs="i386")(func)

def skipIfTargetAndroid(api_levels=None, archs=None):
    """Decorator to skip tests when the target is Android.

    Arguments:
        api_levels - The API levels for which the test should be skipped. If
            it is None, then the test will be skipped for all API levels.
        arch - A sequence of architecture names specifying the architectures
            for which a test is skipped. None means all architectures.
    """
    return skipTestIfFn(_skip_for_android("skipping for android", api_levels, archs))

def skipUnlessCompilerRt(func):
    """Decorate the item to skip tests if testing remotely."""
    def is_compiler_rt_missing():
        compilerRtPath = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "llvm","projects","compiler-rt")
        return "compiler-rt not found" if not os.path.exists(compilerRtPath) else None
    return skipTestIfFn(is_compiler_rt_missing)(func)
