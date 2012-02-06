//===-- SWIG Interface for SBSymbolContext ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

namespace lldb {

%feature("docstring",
"A context object that provides access to core debugger entities.

Manay debugger functions require a context when doing lookups. This class
provides a common structure that can be used as the result of a query that
can contain a single result.

For example,

        exe = os.path.join(os.getcwd(), 'a.out')

        # Create a target for the debugger.
        target = self.dbg.CreateTarget(exe)

        # Now create a breakpoint on main.c by name 'c'.
        breakpoint = target.BreakpointCreateByName('c', 'a.out')

        # Now launch the process, and do not stop at entry point.
        process = target.LaunchSimple(None, None, os.getcwd())

        # The inferior should stop on 'c'.
        from lldbutil import get_stopped_thread
        thread = get_stopped_thread(process, lldb.eStopReasonBreakpoint)
        frame0 = thread.GetFrameAtIndex(0)

        # Now get the SBSymbolContext from this frame.  We want everything. :-)
        context = frame0.GetSymbolContext(lldb.eSymbolContextEverything)

        # Get the module.
        module = context.GetModule()
        ...

        # And the compile unit associated with the frame.
        compileUnit = context.GetCompileUnit()
        ...
"
) SBSymbolContext;
class SBSymbolContext
{
public:
    SBSymbolContext ();

    SBSymbolContext (const lldb::SBSymbolContext& rhs);

    ~SBSymbolContext ();

    bool
    IsValid () const;

    lldb::SBModule        GetModule ();
    lldb::SBCompileUnit   GetCompileUnit ();
    lldb::SBFunction      GetFunction ();
    lldb::SBBlock         GetBlock ();
    lldb::SBLineEntry     GetLineEntry ();
    lldb::SBSymbol        GetSymbol ();

    void SetModule      (lldb::SBModule module);
    void SetCompileUnit (lldb::SBCompileUnit compile_unit);
    void SetFunction    (lldb::SBFunction function);
    void SetBlock       (lldb::SBBlock block);
    void SetLineEntry   (lldb::SBLineEntry line_entry);
    void SetSymbol      (lldb::SBSymbol symbol);
    
    lldb::SBSymbolContext
    GetParentOfInlinedScope (const lldb::SBAddress &curr_frame_pc, 
                             lldb::SBAddress &parent_frame_addr) const;
    

    bool
    GetDescription (lldb::SBStream &description);
    
    
    %pythoncode %{
        __swig_getmethods__["module"] = GetModule
        __swig_setmethods__["module"] = SetModule
        if _newclass: x = property(GetModule, SetModule)

        __swig_getmethods__["compile_unit"] = GetCompileUnit
        __swig_setmethods__["compile_unit"] = SetCompileUnit
        if _newclass: x = property(GetCompileUnit, SetCompileUnit)

        __swig_getmethods__["function"] = GetFunction
        __swig_setmethods__["function"] = SetFunction
        if _newclass: x = property(GetFunction, SetFunction)

        __swig_getmethods__["block"] = GetBlock
        __swig_setmethods__["block"] = SetBlock
        if _newclass: x = property(GetBlock, SetBlock)
            
        __swig_getmethods__["symbol"] = GetSymbol
        __swig_setmethods__["symbol"] = SetSymbol
        if _newclass: x = property(GetSymbol, SetSymbol)

        __swig_getmethods__["line_entry"] = GetLineEntry
        __swig_setmethods__["line_entry"] = SetLineEntry
        if _newclass: x = property(GetLineEntry, SetLineEntry)
    %}

};

} // namespace lldb
