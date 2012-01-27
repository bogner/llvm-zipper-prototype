//===-- ScriptInterpreter.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ScriptInterpreter_h_
#define liblldb_ScriptInterpreter_h_

#include "lldb/lldb-private.h"

#include "lldb/Core/Broadcaster.h"
#include "lldb/Core/Error.h"

#include "lldb/Utility/PseudoTerminal.h"


namespace lldb_private {

class ScriptInterpreter
{
public:

    typedef void (*SWIGInitCallback) (void);

    typedef bool (*SWIGBreakpointCallbackFunction) (const char *python_function_name,
                                                    const char *session_dictionary_name,
                                                    const lldb::StackFrameSP& frame_sp,
                                                    const lldb::BreakpointLocationSP &bp_loc_sp);
    
    typedef std::string (*SWIGPythonTypeScriptCallbackFunction) (const char *python_function_name,
                                                                 const char *session_dictionary_name,
                                                                 const lldb::ValueObjectSP& valobj_sp);
    
    typedef void* (*SWIGPythonCreateSyntheticProvider) (const std::string python_class_name,
                                                        const char *session_dictionary_name,
                                                        const lldb::ValueObjectSP& valobj_sp);
    
    typedef uint32_t       (*SWIGPythonCalculateNumChildren)        (void *implementor);
    typedef void*          (*SWIGPythonGetChildAtIndex)             (void *implementor, uint32_t idx);
    typedef int            (*SWIGPythonGetIndexOfChildWithName)     (void *implementor, const char* child_name);
    typedef void*          (*SWIGPythonCastPyObjectToSBValue)       (void* data);
    typedef void           (*SWIGPythonUpdateSynthProviderInstance) (void* data);    
    
    typedef bool           (*SWIGPythonCallCommand)                 (const char *python_function_name,
                                                                     const char *session_dictionary_name,
                                                                     lldb::DebuggerSP& debugger,
                                                                     const char* args,
                                                                     std::string& err_msg,
                                                                     lldb_private::CommandReturnObject& cmd_retobj);
    
    typedef bool           (*SWIGPythonCallModuleInit)              (const std::string python_module_name,
                                                                     const char *session_dictionary_name,
                                                                     lldb::DebuggerSP& debugger);

    typedef enum
    {
        eScriptReturnTypeCharPtr,
        eScriptReturnTypeBool,
        eScriptReturnTypeShortInt,
        eScriptReturnTypeShortIntUnsigned,
        eScriptReturnTypeInt,
        eScriptReturnTypeIntUnsigned,
        eScriptReturnTypeLongInt,
        eScriptReturnTypeLongIntUnsigned,
        eScriptReturnTypeLongLong,
        eScriptReturnTypeLongLongUnsigned,
        eScriptReturnTypeFloat,
        eScriptReturnTypeDouble,
        eScriptReturnTypeChar,
        eScriptReturnTypeCharStrOrNone
    } ScriptReturnType;
    
    ScriptInterpreter (CommandInterpreter &interpreter, lldb::ScriptLanguage script_lang);

    virtual ~ScriptInterpreter ();

    virtual bool
    ExecuteOneLine (const char *command, CommandReturnObject *result) = 0;

    virtual void
    ExecuteInterpreterLoop () = 0;

    virtual bool
    ExecuteOneLineWithReturn (const char *in_string, ScriptReturnType return_type, void *ret_value)
    {
        return true;
    }

    virtual bool
    ExecuteMultipleLines (const char *in_string)
    {
        return true;
    }

    virtual bool
    ExportFunctionDefinitionToInterpreter (StringList &function_def)
    {
        return false;
    }

    virtual bool
    GenerateBreakpointCommandCallbackData (StringList &input, StringList &output)
    {
        return false;
    }
    
    virtual bool
    GenerateTypeScriptFunction (StringList &input, StringList &output)
    {
        return false;
    }
    
    virtual bool
    GenerateScriptAliasFunction (StringList &input, StringList &output)
    {
        return false;
    }
    
    virtual bool
    GenerateTypeSynthClass (StringList &input, StringList &output)
    {
        return false;
    }
    
    virtual void*
    CreateSyntheticScriptedProvider (std::string class_name,
                                     lldb::ValueObjectSP valobj)
    {
        return NULL;
    }
    
    // use this if the function code is just a one-liner script
    virtual bool
    GenerateTypeScriptFunction (const char* oneliner, StringList &output)
    {
        return false;
    }
    
    virtual bool
    GenerateFunction(std::string& signature, StringList &input, StringList &output)
    {
        return false;
    }

    virtual void 
    CollectDataForBreakpointCommandCallback (BreakpointOptions *bp_options,
                                             CommandReturnObject &result);

    /// Set a one-liner as the callback for the breakpoint.
    virtual void 
    SetBreakpointCommandCallback (BreakpointOptions *bp_options,
                                  const char *oneliner)
    {
        return;
    }
    
    virtual uint32_t
    CalculateNumChildren (void *implementor)
    {
        return 0;
    }
    
    virtual lldb::ValueObjectSP
    GetChildAtIndex (void *implementor, uint32_t idx)
    {
        return lldb::ValueObjectSP();
    }
    
    virtual int
    GetIndexOfChildWithName (void *implementor, const char* child_name)
    {
        return UINT32_MAX;
    }
    
    virtual void
    UpdateSynthProviderInstance (void* implementor)
    {
    }
        
    virtual bool
    RunScriptBasedCommand (const char* impl_function,
                           const char* args,
                           ScriptedCommandSynchronicity synchronicity,
                           lldb_private::CommandReturnObject& cmd_retobj,
                           Error& error)
    {
        return false;
    }
    
    virtual std::string
    GetDocumentationForItem (const char* item)
    {
        return std::string("");
    }

    virtual bool
    LoadScriptingModule (const char* filename,
                         bool can_reload,
                         lldb_private::Error& error)
    {
        error.SetErrorString("loading unimplemented");
        return false;
    }

    const char *
    GetScriptInterpreterPtyName ();

    int
    GetMasterFileDescriptor ();

	CommandInterpreter &
	GetCommandInterpreter ();

    static std::string
    LanguageToString (lldb::ScriptLanguage language);
    
    static void
    InitializeInterpreter (SWIGInitCallback python_swig_init_callback,
                           SWIGBreakpointCallbackFunction python_swig_breakpoint_callback,
                           SWIGPythonTypeScriptCallbackFunction python_swig_typescript_callback,
                           SWIGPythonCreateSyntheticProvider python_swig_synthetic_script,
                           SWIGPythonCalculateNumChildren python_swig_calc_children,
                           SWIGPythonGetChildAtIndex python_swig_get_child_index,
                           SWIGPythonGetIndexOfChildWithName python_swig_get_index_child,
                           SWIGPythonCastPyObjectToSBValue python_swig_cast_to_sbvalue,
                           SWIGPythonUpdateSynthProviderInstance python_swig_update_provider,
                           SWIGPythonCallCommand python_swig_call_command,
                           SWIGPythonCallModuleInit python_swig_call_mod_init);

    static void
    TerminateInterpreter ();

    virtual void
    ResetOutputFileHandle (FILE *new_fh) { } //By default, do nothing.

protected:
    CommandInterpreter &m_interpreter;
    lldb::ScriptLanguage m_script_lang;
};

} // namespace lldb_private

#endif // #ifndef liblldb_ScriptInterpreter_h_
