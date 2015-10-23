"""
                     The LLVM Compiler Infrastructure

This file is distributed under the University of Illinois Open Source
License. See LICENSE.TXT for details.

Provides classes used by the test results reporting infrastructure
within the LLDB test suite.
"""

from __future__ import print_function

import lldb_shared

import argparse
import inspect
import os
import pprint
import re
import sys
import threading
import time
import traceback
import xml.sax.saxutils

from six.moves import cPickle


class EventBuilder(object):
    """Helper class to build test result event dictionaries."""

    BASE_DICTIONARY = None

    @staticmethod
    def _get_test_name_info(test):
        """Returns (test-class-name, test-method-name) from a test case instance.

        @param test a unittest.TestCase instance.

        @return tuple containing (test class name, test method name)
        """
        test_class_components = test.id().split(".")
        test_class_name = ".".join(test_class_components[:-1])
        test_name = test_class_components[-1]
        return (test_class_name, test_name)

    @staticmethod
    def bare_event(event_type):
        """Creates an event with default additions, event type and timestamp.

        @param event_type the value set for the "event" key, used
        to distinguish events.

        @returns an event dictionary with all default additions, the "event"
        key set to the passed in event_type, and the event_time value set to
        time.time().
        """
        if EventBuilder.BASE_DICTIONARY is not None:
            # Start with a copy of the "always include" entries.
            event = dict(EventBuilder.BASE_DICTIONARY)
        else:
            event = {}

        event.update({
            "event": event_type,
            "event_time": time.time()
            })
        return event

    @staticmethod
    def _event_dictionary_common(test, event_type):
        """Returns an event dictionary setup with values for the given event type.

        @param test the unittest.TestCase instance

        @param event_type the name of the event type (string).

        @return event dictionary with common event fields set.
        """
        test_class_name, test_name = EventBuilder._get_test_name_info(test)

        event = EventBuilder.bare_event(event_type)
        event.update({
            "test_class": test_class_name,
            "test_name": test_name,
            "test_filename": inspect.getfile(test.__class__)
        })
        return event

    @staticmethod
    def _error_tuple_class(error_tuple):
        """Returns the unittest error tuple's error class as a string.

        @param error_tuple the error tuple provided by the test framework.

        @return the error type (typically an exception) raised by the
        test framework.
        """
        type_var = error_tuple[0]
        module = inspect.getmodule(type_var)
        if module:
            return "{}.{}".format(module.__name__, type_var.__name__)
        else:
            return type_var.__name__

    @staticmethod
    def _error_tuple_message(error_tuple):
        """Returns the unittest error tuple's error message.

        @param error_tuple the error tuple provided by the test framework.

        @return the error message provided by the test framework.
        """
        return str(error_tuple[1])

    @staticmethod
    def _error_tuple_traceback(error_tuple):
        """Returns the unittest error tuple's error message.

        @param error_tuple the error tuple provided by the test framework.

        @return the error message provided by the test framework.
        """
        return error_tuple[2]

    @staticmethod
    def _event_dictionary_test_result(test, status):
        """Returns an event dictionary with common test result fields set.

        @param test a unittest.TestCase instance.

        @param status the status/result of the test
        (e.g. "success", "failure", etc.)

        @return the event dictionary
        """
        event = EventBuilder._event_dictionary_common(test, "test_result")
        event["status"] = status
        return event

    @staticmethod
    def _event_dictionary_issue(test, status, error_tuple):
        """Returns an event dictionary with common issue-containing test result
        fields set.

        @param test a unittest.TestCase instance.

        @param status the status/result of the test
        (e.g. "success", "failure", etc.)

        @param error_tuple the error tuple as reported by the test runner.
        This is of the form (type<error>, error).

        @return the event dictionary
        """
        event = EventBuilder._event_dictionary_test_result(test, status)
        event["issue_class"] = EventBuilder._error_tuple_class(error_tuple)
        event["issue_message"] = EventBuilder._error_tuple_message(error_tuple)
        backtrace = EventBuilder._error_tuple_traceback(error_tuple)
        if backtrace is not None:
            event["issue_backtrace"] = traceback.format_tb(backtrace)
        return event

    @staticmethod
    def event_for_start(test):
        """Returns an event dictionary for the test start event.

        @param test a unittest.TestCase instance.

        @return the event dictionary
        """
        return EventBuilder._event_dictionary_common(test, "test_start")

    @staticmethod
    def event_for_success(test):
        """Returns an event dictionary for a successful test.

        @param test a unittest.TestCase instance.

        @return the event dictionary
        """
        return EventBuilder._event_dictionary_test_result(test, "success")

    @staticmethod
    def event_for_unexpected_success(test, bugnumber):
        """Returns an event dictionary for a test that succeeded but was
        expected to fail.

        @param test a unittest.TestCase instance.

        @param bugnumber the issue identifier for the bug tracking the
        fix request for the test expected to fail (but is in fact
        passing here).

        @return the event dictionary

        """
        event = EventBuilder._event_dictionary_test_result(
            test, "unexpected_success")
        if bugnumber:
            event["bugnumber"] = str(bugnumber)
        return event

    @staticmethod
    def event_for_failure(test, error_tuple):
        """Returns an event dictionary for a test that failed.

        @param test a unittest.TestCase instance.

        @param error_tuple the error tuple as reported by the test runner.
        This is of the form (type<error>, error).

        @return the event dictionary
        """
        return EventBuilder._event_dictionary_issue(
            test, "failure", error_tuple)

    @staticmethod
    def event_for_expected_failure(test, error_tuple, bugnumber):
        """Returns an event dictionary for a test that failed as expected.

        @param test a unittest.TestCase instance.

        @param error_tuple the error tuple as reported by the test runner.
        This is of the form (type<error>, error).

        @param bugnumber the issue identifier for the bug tracking the
        fix request for the test expected to fail.

        @return the event dictionary

        """
        event = EventBuilder._event_dictionary_issue(
            test, "expected_failure", error_tuple)
        if bugnumber:
            event["bugnumber"] = str(bugnumber)
        return event

    @staticmethod
    def event_for_skip(test, reason):
        """Returns an event dictionary for a test that was skipped.

        @param test a unittest.TestCase instance.

        @param reason the reason why the test is being skipped.

        @return the event dictionary
        """
        event = EventBuilder._event_dictionary_test_result(test, "skip")
        event["skip_reason"] = reason
        return event

    @staticmethod
    def event_for_error(test, error_tuple):
        """Returns an event dictionary for a test that hit a test execution error.

        @param test a unittest.TestCase instance.

        @param error_tuple the error tuple as reported by the test runner.
        This is of the form (type<error>, error).

        @return the event dictionary
        """
        return EventBuilder._event_dictionary_issue(test, "error", error_tuple)

    @staticmethod
    def event_for_cleanup_error(test, error_tuple):
        """Returns an event dictionary for a test that hit a test execution error
        during the test cleanup phase.

        @param test a unittest.TestCase instance.

        @param error_tuple the error tuple as reported by the test runner.
        This is of the form (type<error>, error).

        @return the event dictionary
        """
        event = EventBuilder._event_dictionary_issue(
            test, "error", error_tuple)
        event["issue_phase"] = "cleanup"
        return event

    @staticmethod
    def add_entries_to_all_events(entries_dict):
        """Specifies a dictionary of entries to add to all test events.

        This provides a mechanism for, say, a parallel test runner to
        indicate to each inferior dotest.py that it should add a
        worker index to each.

        Calling this method replaces all previous entries added
        by a prior call to this.

        Event build methods will overwrite any entries that collide.
        Thus, the passed in dictionary is the base, which gets merged
        over by event building when keys collide.

        @param entries_dict a dictionary containing key and value
        pairs that should be merged into all events created by the
        event generator.  May be None to clear out any extra entries.
        """
        EventBuilder.BASE_DICTIONARY = dict(entries_dict)


class ResultsFormatter(object):

    """Provides interface to formatting test results out to a file-like object.

    This class allows the LLDB test framework's raw test-realted
    events to be processed and formatted in any manner desired.
    Test events are represented by python dictionaries, formatted
    as in the EventBuilder class above.

    ResultFormatter instances are given a file-like object in which
    to write their results.

    ResultFormatter lifetime looks like the following:

    # The result formatter is created.
    # The argparse options dictionary is generated from calling
    # the SomeResultFormatter.arg_parser() with the options data
    # passed to dotest.py via the "--results-formatter-options"
    # argument.  See the help on that for syntactic requirements
    # on getting that parsed correctly.
    formatter = SomeResultFormatter(file_like_object, argpared_options_dict)

    # Single call to session start, before parsing any events.
    formatter.begin_session()

    formatter.handle_event({"event":"initialize",...})

    # Zero or more calls specified for events recorded during the test session.
    # The parallel test runner manages getting results from all the inferior
    # dotest processes, so from a new format perspective, don't worry about
    # that.  The formatter will be presented with a single stream of events
    # sandwiched between a single begin_session()/end_session() pair in the
    # parallel test runner process/thread.
    for event in zero_or_more_test_events():
        formatter.handle_event(event)

    # Single call to terminate/wrap-up. Formatters that need all the
    # data before they can print a correct result (e.g. xUnit/JUnit),
    # this is where the final report can be generated.
    formatter.handle_event({"event":"terminate",...})

    It is not the formatter's responsibility to close the file_like_object.
    (i.e. do not close it).

    The lldb test framework passes these test events in real time, so they
    arrive as they come in.

    In the case of the parallel test runner, the dotest inferiors
    add a 'pid' field to the dictionary that indicates which inferior
    pid generated the event.

    Note more events may be added in the future to support richer test
    reporting functionality. One example: creating a true flaky test
    result category so that unexpected successes really mean the test
    is marked incorrectly (either should be marked flaky, or is indeed
    passing consistently now and should have the xfail marker
    removed). In this case, a flaky_success and flaky_fail event
    likely will be added to capture these and support reporting things
    like percentages of flaky test passing so we can see if we're
    making some things worse/better with regards to failure rates.

    Another example: announcing all the test methods that are planned
    to be run, so we can better support redo operations of various kinds
    (redo all non-run tests, redo non-run tests except the one that
    was running [perhaps crashed], etc.)

    Implementers are expected to override all the public methods
    provided in this class. See each method's docstring to see
    expectations about when the call should be chained.

    """

    @classmethod
    def arg_parser(cls):
        """@return arg parser used to parse formatter-specific options."""
        parser = argparse.ArgumentParser(
            description='{} options'.format(cls.__name__),
            usage=('dotest.py --results-formatter-options='
                   '"--option1 value1 [--option2 value2 [...]]"'))
        return parser

    def __init__(self, out_file, options):
        super(ResultsFormatter, self).__init__()
        self.out_file = out_file
        self.options = options
        self.using_terminal = False
        if not self.out_file:
            raise Exception("ResultsFormatter created with no file object")
        self.start_time_by_test = {}
        self.terminate_called = False

        # Lock that we use while mutating inner state, like the
        # total test count and the elements.  We minimize how
        # long we hold the lock just to keep inner state safe, not
        # entirely consistent from the outside.
        self.lock = threading.Lock()

    def handle_event(self, test_event):
        """Handles the test event for collection into the formatter output.

        Derived classes may override this but should call down to this
        implementation first.

        @param test_event the test event as formatted by one of the
        event_for_* calls.
        """
        # Keep track of whether terminate was received.  We do this so
        # that a process can call the 'terminate' event on its own, to
        # close down a formatter at the appropriate time.  Then the
        # atexit() cleanup can call the "terminate if it hasn't been
        # called yet".
        if test_event is not None:
            if test_event.get("event", "") == "terminate":
                self.terminate_called = True

    def track_start_time(self, test_class, test_name, start_time):
        """Tracks the start time of a test so elapsed time can be computed.

        This alleviates the need for test results to be processed serially
        by test.  It will save the start time for the test so that
        elapsed_time_for_test() can compute the elapsed time properly.
        """
        if test_class is None or test_name is None:
            return

        test_key = "{}.{}".format(test_class, test_name)
        with self.lock:
            self.start_time_by_test[test_key] = start_time

    def elapsed_time_for_test(self, test_class, test_name, end_time):
        """Returns the elapsed time for a test.

        This function can only be called once per test and requires that
        the track_start_time() method be called sometime prior to calling
        this method.
        """
        if test_class is None or test_name is None:
            return -2.0

        test_key = "{}.{}".format(test_class, test_name)
        with self.lock:
            if test_key not in self.start_time_by_test:
                return -1.0
            else:
                start_time = self.start_time_by_test[test_key]
            del self.start_time_by_test[test_key]
        return end_time - start_time

    def is_using_terminal(self):
        """Returns True if this results formatter is using the terminal and
        output should be avoided."""
        return self.using_terminal

    def send_terminate_as_needed(self):
        """Sends the terminate event if it hasn't been received yet."""
        if not self.terminate_called:
            terminate_event = EventBuilder.bare_event("terminate")
            self.handle_event(terminate_event)


class XunitFormatter(ResultsFormatter):
    """Provides xUnit-style formatted output.
    """

    # Result mapping arguments
    RM_IGNORE = 'ignore'
    RM_SUCCESS = 'success'
    RM_FAILURE = 'failure'
    RM_PASSTHRU = 'passthru'

    @staticmethod
    def _build_illegal_xml_regex():
        """Contructs a regex to match all illegal xml characters.

        Expects to be used against a unicode string."""
        # Construct the range pairs of invalid unicode chareacters.
        illegal_chars_u = [
            (0x00, 0x08), (0x0B, 0x0C), (0x0E, 0x1F), (0x7F, 0x84),
            (0x86, 0x9F), (0xFDD0, 0xFDDF), (0xFFFE, 0xFFFF)]

        # For wide builds, we have more.
        if sys.maxunicode >= 0x10000:
            illegal_chars_u.extend(
                [(0x1FFFE, 0x1FFFF), (0x2FFFE, 0x2FFFF), (0x3FFFE, 0x3FFFF),
                 (0x4FFFE, 0x4FFFF), (0x5FFFE, 0x5FFFF), (0x6FFFE, 0x6FFFF),
                 (0x7FFFE, 0x7FFFF), (0x8FFFE, 0x8FFFF), (0x9FFFE, 0x9FFFF),
                 (0xAFFFE, 0xAFFFF), (0xBFFFE, 0xBFFFF), (0xCFFFE, 0xCFFFF),
                 (0xDFFFE, 0xDFFFF), (0xEFFFE, 0xEFFFF), (0xFFFFE, 0xFFFFF),
                 (0x10FFFE, 0x10FFFF)])

        # Build up an array of range expressions.
        illegal_ranges = [
            "%s-%s" % (unichr(low), unichr(high))
            for (low, high) in illegal_chars_u]

        # Compile the regex
        return re.compile(u'[%s]' % u''.join(illegal_ranges))

    @staticmethod
    def _quote_attribute(text):
        """Returns the given text in a manner safe for usage in an XML attribute.

        @param text the text that should appear within an XML attribute.
        @return the attribute-escaped version of the input text.
        """
        return xml.sax.saxutils.quoteattr(text)

    def _replace_invalid_xml(self, str_or_unicode):
        """Replaces invalid XML characters with a '?'.

        @param str_or_unicode a string to replace invalid XML
        characters within.  Can be unicode or not.  If not unicode,
        assumes it is a byte string in utf-8 encoding.

        @returns a utf-8-encoded byte string with invalid
        XML replaced with '?'.
        """
        # Get the content into unicode
        if isinstance(str_or_unicode, str):
            unicode_content = str_or_unicode.decode('utf-8')
        else:
            unicode_content = str_or_unicode
        return self.invalid_xml_re.sub(u'?', unicode_content).encode('utf-8')

    @classmethod
    def arg_parser(cls):
        """@return arg parser used to parse formatter-specific options."""
        parser = super(XunitFormatter, cls).arg_parser()

        # These are valid choices for results mapping.
        results_mapping_choices = [
            XunitFormatter.RM_IGNORE,
            XunitFormatter.RM_SUCCESS,
            XunitFormatter.RM_FAILURE,
            XunitFormatter.RM_PASSTHRU]
        parser.add_argument(
            "--assert-on-unknown-events",
            action="store_true",
            help=('cause unknown test events to generate '
                  'a python assert.  Default is to ignore.'))
        parser.add_argument(
            "--ignore-skip-name",
            "-n",
            metavar='PATTERN',
            action="append",
            dest='ignore_skip_name_patterns',
            help=('a python regex pattern, where '
                  'any skipped test with a test method name where regex '
                  'matches (via search) will be ignored for xUnit test '
                  'result purposes.  Can be specified multiple times.'))
        parser.add_argument(
            "--ignore-skip-reason",
            "-r",
            metavar='PATTERN',
            action="append",
            dest='ignore_skip_reason_patterns',
            help=('a python regex pattern, where '
                  'any skipped test with a skip reason where the regex '
                  'matches (via search) will be ignored for xUnit test '
                  'result purposes.  Can be specified multiple times.'))
        parser.add_argument(
            "--xpass", action="store", choices=results_mapping_choices,
            default=XunitFormatter.RM_FAILURE,
            help=('specify mapping from unexpected success to jUnit/xUnit '
                  'result type'))
        parser.add_argument(
            "--xfail", action="store", choices=results_mapping_choices,
            default=XunitFormatter.RM_IGNORE,
            help=('specify mapping from expected failure to jUnit/xUnit '
                  'result type'))
        return parser

    @staticmethod
    def _build_regex_list_from_patterns(patterns):
        """Builds a list of compiled regexes from option value.

        @param option string containing a comma-separated list of regex
        patterns. Zero-length or None will produce an empty regex list.

        @return list of compiled regular expressions, empty if no
        patterns provided.
        """
        regex_list = []
        if patterns is not None:
            for pattern in patterns:
                regex_list.append(re.compile(pattern))
        return regex_list

    def __init__(self, out_file, options):
        """Initializes the XunitFormatter instance.
        @param out_file file-like object where formatted output is written.
        @param options_dict specifies a dictionary of options for the
        formatter.
        """
        # Initialize the parent
        super(XunitFormatter, self).__init__(out_file, options)
        self.text_encoding = "UTF-8"
        self.invalid_xml_re = XunitFormatter._build_illegal_xml_regex()
        self.total_test_count = 0
        self.ignore_skip_name_regexes = (
            XunitFormatter._build_regex_list_from_patterns(
                options.ignore_skip_name_patterns))
        self.ignore_skip_reason_regexes = (
            XunitFormatter._build_regex_list_from_patterns(
                options.ignore_skip_reason_patterns))

        self.elements = {
            "successes": [],
            "errors": [],
            "failures": [],
            "skips": [],
            "unexpected_successes": [],
            "expected_failures": [],
            "all": []
            }

        self.status_handlers = {
            "success": self._handle_success,
            "failure": self._handle_failure,
            "error": self._handle_error,
            "skip": self._handle_skip,
            "expected_failure": self._handle_expected_failure,
            "unexpected_success": self._handle_unexpected_success
            }

    def handle_event(self, test_event):
        super(XunitFormatter, self).handle_event(test_event)

        event_type = test_event["event"]
        if event_type is None:
            return

        if event_type == "terminate":
            self._finish_output()
        elif event_type == "test_start":
            self.track_start_time(
                test_event["test_class"],
                test_event["test_name"],
                test_event["event_time"])
        elif event_type == "test_result":
            self._process_test_result(test_event)
        else:
            # This is an unknown event.
            if self.options.assert_on_unknown_events:
                raise Exception("unknown event type {} from {}\n".format(
                    event_type, test_event))

    def _handle_success(self, test_event):
        """Handles a test success.
        @param test_event the test event to handle.
        """
        result = self._common_add_testcase_entry(test_event)
        with self.lock:
            self.elements["successes"].append(result)

    def _handle_failure(self, test_event):
        """Handles a test failure.
        @param test_event the test event to handle.
        """
        message = self._replace_invalid_xml(test_event["issue_message"])
        backtrace = self._replace_invalid_xml(
            "".join(test_event.get("issue_backtrace", [])))

        result = self._common_add_testcase_entry(
            test_event,
            inner_content=(
                '<failure type={} message={}><![CDATA[{}]]></failure>'.format(
                    XunitFormatter._quote_attribute(test_event["issue_class"]),
                    XunitFormatter._quote_attribute(message),
                    backtrace)
            ))
        with self.lock:
            self.elements["failures"].append(result)

    def _handle_error(self, test_event):
        """Handles a test error.
        @param test_event the test event to handle.
        """
        message = self._replace_invalid_xml(test_event["issue_message"])
        backtrace = self._replace_invalid_xml(
            "".join(test_event.get("issue_backtrace", [])))

        result = self._common_add_testcase_entry(
            test_event,
            inner_content=(
                '<error type={} message={}><![CDATA[{}]]></error>'.format(
                    XunitFormatter._quote_attribute(test_event["issue_class"]),
                    XunitFormatter._quote_attribute(message),
                    backtrace)
            ))
        with self.lock:
            self.elements["errors"].append(result)

    @staticmethod
    def _ignore_based_on_regex_list(test_event, test_key, regex_list):
        """Returns whether to ignore a test event based on patterns.

        @param test_event the test event dictionary to check.
        @param test_key the key within the dictionary to check.
        @param regex_list a list of zero or more regexes.  May contain
        zero or more compiled regexes.

        @return True if any o the regex list match based on the
        re.search() method; false otherwise.
        """
        for regex in regex_list:
            match = regex.search(test_event.get(test_key, ''))
            if match:
                return True
        return False

    def _handle_skip(self, test_event):
        """Handles a skipped test.
        @param test_event the test event to handle.
        """

        # Are we ignoring this test based on test name?
        if XunitFormatter._ignore_based_on_regex_list(
                test_event, 'test_name', self.ignore_skip_name_regexes):
            return

        # Are we ignoring this test based on skip reason?
        if XunitFormatter._ignore_based_on_regex_list(
                test_event, 'skip_reason', self.ignore_skip_reason_regexes):
            return

        # We're not ignoring this test.  Process the skip.
        reason = self._replace_invalid_xml(test_event.get("skip_reason", ""))
        result = self._common_add_testcase_entry(
            test_event,
            inner_content='<skipped message={} />'.format(
                XunitFormatter._quote_attribute(reason)))
        with self.lock:
            self.elements["skips"].append(result)

    def _handle_expected_failure(self, test_event):
        """Handles a test that failed as expected.
        @param test_event the test event to handle.
        """
        if self.options.xfail == XunitFormatter.RM_PASSTHRU:
            # This is not a natively-supported junit/xunit
            # testcase mode, so it might fail a validating
            # test results viewer.
            if "bugnumber" in test_event:
                bug_id_attribute = 'bug-id={} '.format(
                    XunitFormatter._quote_attribute(test_event["bugnumber"]))
            else:
                bug_id_attribute = ''

            result = self._common_add_testcase_entry(
                test_event,
                inner_content=(
                    '<expected-failure {}type={} message={} />'.format(
                        bug_id_attribute,
                        XunitFormatter._quote_attribute(
                            test_event["issue_class"]),
                        XunitFormatter._quote_attribute(
                            test_event["issue_message"]))
                ))
            with self.lock:
                self.elements["expected_failures"].append(result)
        elif self.options.xfail == XunitFormatter.RM_SUCCESS:
            result = self._common_add_testcase_entry(test_event)
            with self.lock:
                self.elements["successes"].append(result)
        elif self.options.xfail == XunitFormatter.RM_FAILURE:
            result = self._common_add_testcase_entry(
                test_event,
                inner_content='<failure type={} message={} />'.format(
                    XunitFormatter._quote_attribute(test_event["issue_class"]),
                    XunitFormatter._quote_attribute(
                        test_event["issue_message"])))
            with self.lock:
                self.elements["failures"].append(result)
        elif self.options.xfail == XunitFormatter.RM_IGNORE:
            pass
        else:
            raise Exception(
                "unknown xfail option: {}".format(self.options.xfail))

    def _handle_unexpected_success(self, test_event):
        """Handles a test that passed but was expected to fail.
        @param test_event the test event to handle.
        """
        if self.options.xpass == XunitFormatter.RM_PASSTHRU:
            # This is not a natively-supported junit/xunit
            # testcase mode, so it might fail a validating
            # test results viewer.
            result = self._common_add_testcase_entry(
                test_event,
                inner_content=("<unexpected-success />"))
            with self.lock:
                self.elements["unexpected_successes"].append(result)
        elif self.options.xpass == XunitFormatter.RM_SUCCESS:
            # Treat the xpass as a success.
            result = self._common_add_testcase_entry(test_event)
            with self.lock:
                self.elements["successes"].append(result)
        elif self.options.xpass == XunitFormatter.RM_FAILURE:
            # Treat the xpass as a failure.
            if "bugnumber" in test_event:
                message = "unexpected success (bug_id:{})".format(
                    test_event["bugnumber"])
            else:
                message = "unexpected success (bug_id:none)"
            result = self._common_add_testcase_entry(
                test_event,
                inner_content='<failure type={} message={} />'.format(
                    XunitFormatter._quote_attribute("unexpected_success"),
                    XunitFormatter._quote_attribute(message)))
            with self.lock:
                self.elements["failures"].append(result)
        elif self.options.xpass == XunitFormatter.RM_IGNORE:
            # Ignore the xpass result as far as xUnit reporting goes.
            pass
        else:
            raise Exception("unknown xpass option: {}".format(
                self.options.xpass))

    def _process_test_result(self, test_event):
        """Processes the test_event known to be a test result.

        This categorizes the event appropriately and stores the data needed
        to generate the final xUnit report.  This method skips events that
        cannot be represented in xUnit output.
        """
        if "status" not in test_event:
            raise Exception("test event dictionary missing 'status' key")

        status = test_event["status"]
        if status not in self.status_handlers:
            raise Exception("test event status '{}' unsupported".format(
                status))

        # Call the status handler for the test result.
        self.status_handlers[status](test_event)

    def _common_add_testcase_entry(self, test_event, inner_content=None):
        """Registers a testcase result, and returns the text created.

        The caller is expected to manage failure/skip/success counts
        in some kind of appropriate way.  This call simply constructs
        the XML and appends the returned result to the self.all_results
        list.

        @param test_event the test event dictionary.

        @param inner_content if specified, gets included in the <testcase>
        inner section, at the point before stdout and stderr would be
        included.  This is where a <failure/>, <skipped/>, <error/>, etc.
        could go.

        @return the text of the xml testcase element.
        """

        # Get elapsed time.
        test_class = test_event["test_class"]
        test_name = test_event["test_name"]
        event_time = test_event["event_time"]
        time_taken = self.elapsed_time_for_test(
            test_class, test_name, event_time)

        # Plumb in stdout/stderr once we shift over to only test results.
        test_stdout = ''
        test_stderr = ''

        # Formulate the output xml.
        if not inner_content:
            inner_content = ""
        result = (
            '<testcase classname="{}" name="{}" time="{:.3f}">'
            '{}{}{}</testcase>'.format(
                test_class,
                test_name,
                time_taken,
                inner_content,
                test_stdout,
                test_stderr))

        # Save the result, update total test count.
        with self.lock:
            self.total_test_count += 1
            self.elements["all"].append(result)

        return result

    def _finish_output_no_lock(self):
        """Flushes out the report of test executions to form valid xml output.

        xUnit output is in XML.  The reporting system cannot complete the
        formatting of the output without knowing when there is no more input.
        This call addresses notifcation of the completed test run and thus is
        when we can finish off the report output.
        """

        # Figure out the counts line for the testsuite.  If we have
        # been counting either unexpected successes or expected
        # failures, we'll output those in the counts, at the risk of
        # being invalidated by a validating test results viewer.
        # These aren't counted by default so they won't show up unless
        # the user specified a formatter option to include them.
        xfail_count = len(self.elements["expected_failures"])
        xpass_count = len(self.elements["unexpected_successes"])
        if xfail_count > 0 or xpass_count > 0:
            extra_testsuite_attributes = (
                ' expected-failures="{}"'
                ' unexpected-successes="{}"'.format(xfail_count, xpass_count))
        else:
            extra_testsuite_attributes = ""

        # Output the header.
        self.out_file.write(
            '<?xml version="1.0" encoding="{}"?>\n'
            '<testsuites>'
            '<testsuite name="{}" tests="{}" errors="{}" failures="{}" '
            'skip="{}"{}>\n'.format(
                self.text_encoding,
                "LLDB test suite",
                self.total_test_count,
                len(self.elements["errors"]),
                len(self.elements["failures"]),
                len(self.elements["skips"]),
                extra_testsuite_attributes))

        # Output each of the test result entries.
        for result in self.elements["all"]:
            self.out_file.write(result + '\n')

        # Close off the test suite.
        self.out_file.write('</testsuite></testsuites>\n')

    def _finish_output(self):
        """Finish writing output as all incoming events have arrived."""
        with self.lock:
            self._finish_output_no_lock()


class RawPickledFormatter(ResultsFormatter):
    """Formats events as a pickled stream.

    The parallel test runner has inferiors pickle their results and send them
    over a socket back to the parallel test.  The parallel test runner then
    aggregates them into the final results formatter (e.g. xUnit).
    """

    @classmethod
    def arg_parser(cls):
        """@return arg parser used to parse formatter-specific options."""
        parser = super(RawPickledFormatter, cls).arg_parser()
        return parser

    def __init__(self, out_file, options):
        super(RawPickledFormatter, self).__init__(out_file, options)
        self.pid = os.getpid()

    def handle_event(self, test_event):
        super(RawPickledFormatter, self).handle_event(test_event)

        # Convert initialize/terminate events into job_begin/job_end events.
        event_type = test_event["event"]
        if event_type is None:
            return

        if event_type == "initialize":
            test_event["event"] = "job_begin"
        elif event_type == "terminate":
            test_event["event"] = "job_end"

        # Tack on the pid.
        test_event["pid"] = self.pid

        # Send it as {serialized_length_of_serialized_bytes}#{serialized_bytes}
        pickled_message = cPickle.dumps(test_event)
        self.out_file.send(
            "{}#{}".format(len(pickled_message), pickled_message))


class DumpFormatter(ResultsFormatter):
    """Formats events to the file as their raw python dictionary format."""

    def handle_event(self, test_event):
        super(DumpFormatter, self).handle_event(test_event)
        self.out_file.write("\n" + pprint.pformat(test_event) + "\n")
