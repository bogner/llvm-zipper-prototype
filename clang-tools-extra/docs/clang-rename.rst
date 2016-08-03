============
Clang-Rename
============

.. contents::

See also:

.. toctree::
   :maxdepth: 1


:program:`clang-rename` is a C++ refactoring tool. Its purpose is to perform
efficient renaming actions in large-scale projects such as renaming classes,
functions, variables, arguments, namespaces etc.

The tool is in a very early development stage, so you might encounter bugs and
crashes. Submitting reports with information about how to reproduce the issue
to `the LLVM bugtracker <https://llvm.org/bugs>`_ will definitely help the
project. If you have any ideas or suggestions, you might want to put a feature
request there.

Using Clang-Rename
==================

:program:`clang-rename` is a `LibTooling
<http://clang.llvm.org/docs/LibTooling.html>`_-based tool, and it's easier to
work with if you set up a compile command database for your project (for an
example of how to do this see `How To Setup Tooling For LLVM
<http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html>`_). You can also
specify compilation options on the command line after `--`:

.. code-block:: console

  $ clang-rename -offset=42 -new-name=foo test.cpp -- -Imy_project/include -DMY_DEFINES ...


To get an offset of a symbol in a file run

.. code-block:: console

  $ grep -FUbo 'foo' file.cpp


You can also identify one or more symbols to be renamed by giving the fully qualified
name:

.. code-block:: console

  $ clang-rename rename-all -old-name=foo -new-name=bar test.cpp


The tool currently supports renaming actions inside a single Translation Unit
only. It is planned to extend the tool's functionality to support multi-TU
renaming actions in the future.

:program:`clang-rename` also aims to be easily integrated into popular text
editors, such as Vim and Emacs, and improve the workflow of users.

Although a command line interface exists, it is highly recommended to use the
text editor interface instead for better experience.

.. code-block:: console

  $ clang-rename -help
  Usage: clang-rename {rename-at|rename-all} [OPTION]...

  A tool to rename symbols in C/C++ code.

  Subcommands:
    rename-at:  Perform rename off of a location in a file. (This is the default.)
    rename-all: Perform rename of all symbols matching one or more fully qualified names.


.. code-block:: console

  $ clang-rename rename-at -help
  OVERVIEW: A tool to rename symbols in C/C++ code.
  clang-rename renames every occurrence of a symbol found at <offset> in
  <source0>. If -i is specified, the edited files are overwritten to disk.
  Otherwise, the results are written to stdout.
  
  USAGE: clang-rename rename-at [subcommand] [options] <source0> [... <sourceN>]
  
  OPTIONS:
  
  Generic Options:
  
    -help                      - Display available options (-help-hidden for more)
    -help-list                 - Display list of available options (-help-list-hidden for more)
    -version                   - Display the version of this program

  clang-rename rename-at options:

    -export-fixes=<filename>   - YAML file to store suggested fixes in.
    -extra-arg=<string>        - Additional argument to append to the compiler command line
    -extra-arg-before=<string> - Additional argument to prepend to the compiler command line
    -i                         - Overwrite edited <file>s.
    -new-name=<string>         - The new name to change the symbol to.
    -offset=<uint>             - Locates the symbol by offset as opposed to <line>:<column>.
    -p=<string>                - Build path
    -pl                        - Print the locations affected by renaming to stderr.
    -pn                        - Print the found symbol's name prior to renaming to stderr.


.. code-block:: console

  $ clang-rename rename-all -help
  OVERVIEW: A tool to rename symbols in C/C++ code.
  clang-rename renames every occurrence of a symbol named <old-name>.

  USAGE: clang-rename rename-all [subcommand] [options] <source0> [... <sourceN>]

  OPTIONS:

  Generic Options:

    -help                      - Display available options (-help-hidden for more)
    -help-list                 - Display list of available options (-help-list-hidden for more)
    -version                   - Display the version of this program

  clang-rename rename-all options:

    -export-fixes=<filename>   - YAML file to store suggested fixes in.
    -extra-arg=<string>        - Additional argument to append to the compiler command line
    -extra-arg-before=<string> - Additional argument to prepend to the compiler command line
    -i                         - Overwrite edited <file>s.
    -new-name=<string>         - The new name to change the symbol to.
    -offset=<uint>             - Locates the symbol by offset as opposed to <line>:<column>.
    -old-name=<string>         - The fully qualified name of the symbol, if -offset is not used.
    -p=<string>                - Build path


Vim Integration
===============

You can call :program:`clang-rename` directly from Vim! To set up
:program:`clang-rename` integration for Vim see
`clang-rename/tool/clang-rename.py
<http://reviews.llvm.org/diffusion/L/browse/clang-tools-extra/trunk/clang-rename/tool/clang-rename.py>`_.

Please note that **you have to save all buffers, in which the replacement will
happen before running the tool**.

Once installed, you can point your cursor to symbols you want to rename, press
`<leader>cr` and type new desired name. The `<leader> key
<http://vim.wikia.com/wiki/Mapping_keys_in_Vim_-_Tutorial_(Part_3)#Map_leader>`_
is a reference to a specific key defined by the mapleader variable and is bound
to backslash by default.

Emacs Integration
=================

You can also use :program:`clang-rename` while using Emacs! To set up
:program:`clang-rename` integration for Emacs see
`clang-rename/tool/clang-rename.el
<http://reviews.llvm.org/diffusion/L/browse/clang-tools-extra/trunk/clang-rename/tool/clang-rename.el>`_.

Once installed, you can point your cursor to symbols you want to rename, press
`M-X`, type `clang-rename` and new desired name.

Please note that **you have to save all buffers, in which the replacement will
happen before running the tool**.
