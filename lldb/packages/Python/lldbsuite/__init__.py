# Module level initialization for the `lldbsuite` module.

import inspect
import os
import sys

def find_lldb_root():
    lldb_root = os.path.dirname(inspect.getfile(inspect.currentframe()))
    while True:
        lldb_root = os.path.dirname(lldb_root)
        if lldb_root is None:
            return None

        test_path = os.path.join(lldb_root, "lldb.root")
        if os.path.isfile(test_path):
            return lldb_root
    return None

# lldbsuite.lldb_root refers to the root of the git/svn source checkout
lldb_root = find_lldb_root()
