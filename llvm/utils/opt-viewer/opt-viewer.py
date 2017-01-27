#!/usr/bin/env python2.7

from __future__ import print_function

desc = '''Generate HTML output to visualize optimization records from the YAML files
generated with -fsave-optimization-record and -fdiagnostics-show-hotness.

The tools requires PyYAML and Pygments Python packages.

For faster parsing, you may want to use libYAML with PyYAML.'''

import yaml
# Try to use the C parser.
try:
    from yaml import CLoader as Loader
except ImportError:
    from yaml import Loader

import functools
from collections import defaultdict
import itertools
from multiprocessing import Pool
from multiprocessing import Lock, cpu_count
import errno
import argparse
import os.path
import re
import subprocess
import shutil
from pygments import highlight
from pygments.lexers.c_cpp import CppLexer
from pygments.formatters import HtmlFormatter

p = subprocess.Popen(['c++filt', '-n'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p_lock = Lock()


def demangle(name):
    with p_lock:
        p.stdin.write(name + '\n')
        return p.stdout.readline().rstrip()


class Remark(yaml.YAMLObject):
    max_hotness = 0

    # Work-around for http://pyyaml.org/ticket/154.
    yaml_loader = Loader

    @classmethod
    def should_display_hotness(cls):
        # If max_hotness is 0 at the end, we assume hotness information is
        # missing and no relative hotness information is displayed
        return cls.max_hotness != 0

    # Map function names to their source location for function where inlining happened
    caller_loc = dict()

    def __getattr__(self, name):
        # If hotness is missing, assume 0
        if name == 'Hotness':
            return 0
        raise AttributeError

    @property
    def File(self):
        return self.DebugLoc['File']

    @property
    def Line(self):
        return int(self.DebugLoc['Line'])

    @property
    def Column(self):
        return self.DebugLoc['Column']

    @property
    def DebugLocString(self):
        return "{}:{}:{}".format(self.File, self.Line, self.Column)

    @property
    def DemangledFunctionName(self):
        return demangle(self.Function)

    @classmethod
    def make_link(cls, File, Line):
        return "{}#L{}".format(SourceFileRenderer.html_file_name(File), Line)

    @property
    def Link(self):
        return Remark.make_link(self.File, self.Line)

    def getArgString(self, mapping):
        mapping = mapping.copy()
        dl = mapping.get('DebugLoc')
        if dl:
            del mapping['DebugLoc']

        assert(len(mapping) == 1)
        (key, value) = mapping.items()[0]

        if key == 'Caller' or key == 'Callee':
            value = demangle(value)

        if dl and key != 'Caller':
            return "<a href={}>{}</a>".format(
                Remark.make_link(dl['File'], dl['Line']), value)
        else:
            return value

    @property
    def message(self):
        # Args is a list of mappings (dictionaries)
        values = [self.getArgString(mapping) for mapping in self.Args]
        return "".join(values)

    @property
    def RelativeHotness(self):
        if Remark.should_display_hotness():
            return "{}%".format(int(round(self.Hotness * 100 / Remark.max_hotness)))
        else:
            return ''

    @property
    def key(self):
        return (self.__class__, self.Pass, self.Name, self.File, self.Line, self.Column, self.Function, self.message)


class Analysis(Remark):
    yaml_tag = '!Analysis'

    @property
    def color(self):
        return "white"


class AnalysisFPCommute(Analysis):
    yaml_tag = '!AnalysisFPCommute'


class AnalysisAliasing(Analysis):
    yaml_tag = '!AnalysisAliasing'


class Passed(Remark):
    yaml_tag = '!Passed'

    @property
    def color(self):
        return "green"


class Missed(Remark):
    yaml_tag = '!Missed'

    @property
    def color(self):
        return "red"


class SourceFileRenderer:
    def __init__(self, source_dir, output_dir, filename):
        existing_filename = None
        if os.path.exists(filename):
            existing_filename = filename
        else:
            fn = os.path.join(source_dir, filename)
            if os.path.exists(fn):
                existing_filename = fn

        self.stream = open(os.path.join(output_dir, SourceFileRenderer.html_file_name(filename)), 'w')
        if existing_filename:
            self.source_stream = open(existing_filename)
        else:
            self.source_stream = None
            print('''
<html>
<h1>Unable to locate file {}</h1>
</html>
            '''.format(filename), file=self.stream)

        self.html_formatter = HtmlFormatter()
        self.cpp_lexer = CppLexer()

    def render_source_line(self, linenum, line):
        html_line = highlight(line, self.cpp_lexer, self.html_formatter)
        print('''
<tr>
<td><a name=\"L{linenum}\">{linenum}</a></td>
<td></td>
<td></td>
<td>{html_line}</td>
</tr>'''.format(**locals()), file=self.stream)

    def render_inline_remarks(self, r, line):
        inlining_context = r.DemangledFunctionName
        dl = Remark.caller_loc.get(r.Function)
        if dl:
            link = Remark.make_link(dl['File'], dl['Line'] - 2)
            inlining_context = "<a href={link}>{r.DemangledFunctionName}</a>".format(**locals())

        # Column is the number of characters *including* tabs, keep those and
        # replace everything else with spaces.
        indent = line[:r.Column - 1]
        indent = re.sub('\S', ' ', indent)
        print('''
<tr>
<td></td>
<td>{r.RelativeHotness}</td>
<td class=\"column-entry-{r.color}\">{r.Pass}</td>
<td><pre style="display:inline">{indent}</pre><span class=\"column-entry-yellow\"> {r.message}&nbsp;</span></td>
<td class=\"column-entry-yellow\">{inlining_context}</td>
</tr>'''.format(**locals()), file=self.stream)

    def render(self, line_remarks):
        if not self.source_stream:
            return

        print('''
<html>
<head>
<link rel='stylesheet' type='text/css' href='style.css'>
</head>
<body>
<div class="centered">
<table>
<tr>
<td>Line</td>
<td>Hotness</td>
<td>Optimization</td>
<td>Source</td>
<td>Inline Context</td>
</tr>''', file=self.stream)
        for (linenum, line) in enumerate(self.source_stream.readlines(), start=1):
            self.render_source_line(linenum, line)
            for remark in line_remarks.get(linenum, []):
                self.render_inline_remarks(remark, line)
        print('''
</table>
</body>
</html>''', file=self.stream)

    @classmethod
    def html_file_name(cls, filename):
        return filename.replace('/', '_') + ".html"


class IndexRenderer:
    def __init__(self, output_dir):
        self.stream = open(os.path.join(output_dir, 'index.html'), 'w')

    def render_entry(self, r):
        print('''
<tr>
<td><a href={r.Link}>{r.DebugLocString}</a></td>
<td>{r.RelativeHotness}</td>
<td>{r.DemangledFunctionName}</td>
<td class=\"column-entry-{r.color}\">{r.Pass}</td>
</tr>'''.format(**locals()), file=self.stream)

    def render(self, all_remarks):
        print('''
<html>
<head>
<link rel='stylesheet' type='text/css' href='style.css'>
</head>
<body>
<div class="centered">
<table>
<tr>
<td>Source Location</td>
<td>Hotness</td>
<td>Function</td>
<td>Pass</td>
</tr>''', file=self.stream)
        for remark in all_remarks:
            self.render_entry(remark)
        print('''
</table>
</body>
</html>''', file=self.stream)


def get_remarks(input_file):
    max_hotness = 0
    all_remarks = dict()
    file_remarks = defaultdict(functools.partial(defaultdict, list))

    with open(input_file) as f:
        docs = yaml.load_all(f, Loader=Loader)

        for remark in docs:
            # Avoid remarks withoug debug location or if they are duplicated
            if not hasattr(remark, 'DebugLoc') or remark.key in all_remarks:
                continue
            all_remarks[remark.key] = remark

            file_remarks[remark.File][remark.Line].append(remark)

            max_hotness = max(max_hotness, remark.Hotness)

    return max_hotness, all_remarks, file_remarks


def _render_file(source_dir, output_dir, entry):
    filename, remarks = entry
    SourceFileRenderer(source_dir, output_dir, filename).render(remarks)


def gather_results(pool, filenames):
    all_remarks = dict()
    remarks = pool.map(get_remarks, filenames)

    def merge_dicts(dicts):
        ''' Takes an iterable of dicts and merges them into
            a single dict.  Nested dicts are merged as well.
        >>> merge_dicts([ {'a': [3], }, {'a': [4], }, {'b': [6] }])
        {'a': [3,4,], 'b': [6]}
        >>> merge_dicts([ {'a': {'q': [6,3], 'f': [30],}, }, {'a': {'f': [4,10]}, }, {'b': [6] }])
        {'a': [{'q': [6,3]}, {'f': [4,10,30]}], 'b': [6]}

        '''
        merged = defaultdict(functools.partial(defaultdict, list))

        for k, v in itertools.chain(*[d.iteritems() for d in dicts]):
            for k_, v_ in v.items():
                merged[k][k_] += v_

        return merged

    file_remark_dicts = [entry[2] for entry in remarks]
    # merge the list of remarks at each line of each file
    file_remarks = merge_dicts(file_remark_dicts)

    # merge individual 'all_remark' results:
    for _, all_rem, _ in remarks:
        all_remarks.update(all_rem)

    Remark.max_hotness = max(entry[0] for entry in remarks)

    return all_remarks, file_remarks


def map_remarks(all_remarks):
    # Set up a map between function names and their source location for
    # function where inlining happened
    for remark in all_remarks.itervalues():
        if isinstance(remark, Passed) and remark.Pass == "inline" and remark.Name == "Inlined":
            for arg in remark.Args:
                caller = arg.get('Caller')
                if caller:
                    Remark.caller_loc[caller] = arg['DebugLoc']


def generate_report(pool, all_remarks, file_remarks, source_dir, output_dir):
    try:
        os.makedirs(output_dir)
    except OSError as e:
        if e.errno == errno.EEXIST and os.path.isdir(output_dir):
            pass
        else:
            raise

    _render_file_bound = functools.partial(_render_file, source_dir, output_dir)
    pool.map(_render_file_bound, file_remarks.items())

    if Remark.should_display_hotness():
        sorted_remarks = sorted(all_remarks.itervalues(), key=lambda r: (r.Hotness, r.__dict__), reverse=True)
    else:
        sorted_remarks = sorted(all_remarks.itervalues(), key=lambda r: (r.File, r.Line, r.Column, r.__dict__))
    IndexRenderer(args.output_dir).render(sorted_remarks)

    shutil.copy(os.path.join(os.path.dirname(os.path.realpath(__file__)),
            "style.css"), output_dir)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=desc)
    parser.add_argument('yaml_files', nargs='+')
    parser.add_argument('output_dir')
    parser.add_argument(
        '--jobs',
        '-j',
        default=cpu_count(),
        type=int,
        help='Max job count (defaults to current CPU count)')
    parser.add_argument(
        '-source-dir',
        '-s',
        default='',
        help='set source directory')
    args = parser.parse_args()

    if len(args.yaml_files) == 0:
        parser.print_help()
        sys.exit(1)

    pool = Pool(processes=args.jobs)
    all_remarks, file_remarks = gather_results(pool, args.yaml_files)

    map_remarks(all_remarks)

    generate_report(pool, all_remarks, file_remarks, args.source_dir, args.output_dir)
