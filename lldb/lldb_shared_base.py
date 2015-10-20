import inspect
import os
import sys

def add_third_party_module_dirs(lldb_root):
    third_party_modules_dir = os.path.join(lldb_root, "third_party", "Python", "module")
    if not os.path.isdir(third_party_modules_dir):
        return

    module_dirs = os.listdir(third_party_modules_dir)
    for module_dir in module_dirs:
        module_dir = os.path.join(third_party_modules_dir, module_dir)
        sys.path.append(module_dir)
lldb_root = os.path.dirname(inspect.getfile(inspect.currentframe()))
add_third_party_module_dirs(lldb_root)
