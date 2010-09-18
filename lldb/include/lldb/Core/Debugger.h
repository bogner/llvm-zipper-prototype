//===-- Debugger.h ----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Debugger_h_
#define liblldb_Debugger_h_
#if defined(__cplusplus)


#include <stdint.h>
#include <unistd.h>

#include <stack>

#include "lldb/Core/Communication.h"
#include "lldb/Core/Listener.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Core/UserID.h"
#include "lldb/Core/UserSettingsController.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/TargetList.h"

namespace lldb_private {

//----------------------------------------------------------------------
/// @class Debugger Debugger.h "lldb/Core/Debugger.h"
/// @brief A class to manage flag bits.
///
/// Provides a global root objects for the debugger core.
//----------------------------------------------------------------------
    

class DebuggerInstanceSettings : public InstanceSettings
{
public:
    
    DebuggerInstanceSettings (UserSettingsController &owner, bool live_instance = true, const char *name = NULL);

    DebuggerInstanceSettings (const DebuggerInstanceSettings &rhs);

    virtual
    ~DebuggerInstanceSettings ();

    DebuggerInstanceSettings&
    operator= (const DebuggerInstanceSettings &rhs);

    void
    UpdateInstanceSettingsVariable (const ConstString &var_name,
                                    const char *index_value,
                                    const char *value,
                                    const ConstString &instance_name,
                                    const SettingEntry &entry,
                                    lldb::VarSetOperationType op,
                                    Error &err,
                                    bool pending);

    void
    GetInstanceSettingsValue (const SettingEntry &entry,
                              const ConstString &var_name,
                              StringList &value);

    uint32_t
    GetTerminalWidth () const
    {
        return m_term_width;
    }

    void
    SetTerminalWidth (uint32_t term_width)
    {
        m_term_width = term_width;
    }
    
    const char *
    GetPrompt() const
    {
        return m_prompt.c_str();
    }

    void
    SetPrompt(const char *p)
    {
        if (p)
            m_prompt.assign (p);
        else
            m_prompt.assign ("(lldb) ");
    }
        
    lldb::ScriptLanguage 
    GetScriptLanguage() const
    {
        return m_script_lang;
    }

    void
    SetScriptLanguage (lldb::ScriptLanguage script_lang)
    {
        m_script_lang = script_lang;
    }

protected:

    void
    CopyInstanceSettings (const lldb::InstanceSettingsSP &new_settings,
                          bool pending);

    bool
    BroadcastPromptChange (const ConstString &instance_name, const char *new_prompt);

    bool
    ValidTermWidthValue (const char *value, Error err);

    const ConstString
    CreateInstanceName ();

    static const ConstString &
    PromptVarName ();

    static const ConstString &
    ScriptLangVarName ();
  
    static const ConstString &
    TermWidthVarName ();
  
private:

    uint32_t m_term_width;
    std::string m_prompt;
    lldb::ScriptLanguage m_script_lang;
};



class Debugger :
    public UserID,
    public DebuggerInstanceSettings
{
public:

    class DebuggerSettingsController : public UserSettingsController
    {
    public:

        DebuggerSettingsController ();

        virtual
        ~DebuggerSettingsController ();

        void
        UpdateGlobalVariable (const ConstString &var_name,
                              const char *index_value,
                              const char *value,
                              const SettingEntry &entry,
                              lldb::VarSetOperationType op,
                              Error&err);

        void
        GetGlobalSettingsValue (const ConstString &var_name, 
                                StringList &value);

        static SettingEntry global_settings_table[];
        static SettingEntry instance_settings_table[];

    protected:

        lldb::InstanceSettingsSP
        CreateNewInstanceSettings (const char *instance_name);

    private:

        // Class-wide settings.

        DISALLOW_COPY_AND_ASSIGN (DebuggerSettingsController);
    };

    static lldb::UserSettingsControllerSP &
    GetSettingsController (bool finish = false);

    static lldb::DebuggerSP
    CreateInstance ();

    static lldb::TargetSP
    FindTargetWithProcessID (lldb::pid_t pid);

    static void
    Initialize ();
    
    static void 
    Terminate ();

    ~Debugger ();

    lldb::DebuggerSP
    GetSP ();

    bool
    GetAsyncExecution ();

    void
    SetAsyncExecution (bool async);

    void
    SetInputFileHandle (FILE *fh, bool tranfer_ownership);

    void
    SetOutputFileHandle (FILE *fh, bool tranfer_ownership);

    void
    SetErrorFileHandle (FILE *fh, bool tranfer_ownership);

    FILE *
    GetInputFileHandle ();

    FILE *
    GetOutputFileHandle ();

    FILE *
    GetErrorFileHandle ();

    Stream&
    GetOutputStream ()
    {
        return m_output_file;
    }

    Stream&
    GetErrorStream ()
    {
        return m_error_file;
    }

    CommandInterpreter &
    GetCommandInterpreter ();

    Listener &
    GetListener ();

    SourceManager &
    GetSourceManager ();

    lldb::TargetSP
    GetSelectedTarget ();

    ExecutionContext
    GetSelectedExecutionContext();
    //------------------------------------------------------------------
    /// Get accessor for the target list.
    ///
    /// The target list is part of the global debugger object. This
    /// the single debugger shared instance to control where targets
    /// get created and to allow for tracking and searching for targets
    /// based on certain criteria.
    ///
    /// @return
    ///     A global shared target list.
    //------------------------------------------------------------------
    TargetList&
    GetTargetList ();

    void
    DispatchInput (const char *bytes, size_t bytes_len);

    void
    WriteToDefaultReader (const char *bytes, size_t bytes_len);

    void
    PushInputReader (const lldb::InputReaderSP& reader_sp);

    bool
    PopInputReader (const lldb::InputReaderSP& reader_sp);

    ExecutionContext &
    GetExecutionContext()
    {
        return m_exe_ctx;
    }

    void
    UpdateExecutionContext (ExecutionContext *override_context);

    static lldb::DebuggerSP
    FindDebuggerWithID (lldb::user_id_t id);
    
    bool
    SetUseExternalEditor (bool value)
    {
        bool old_value = m_use_external_editor;
        m_use_external_editor = value;
        return old_value;
    }
    
    bool
    UseExternalEditor ()
    {
        return m_use_external_editor;
    }

    static lldb::DebuggerSP
    FindDebuggerWithInstanceName (const ConstString &instance_name);

protected:

    static void
    DispatchInputCallback (void *baton, const void *bytes, size_t bytes_len);

    void
    ActivateInputReader (const lldb::InputReaderSP &reader_sp);

    bool
    CheckIfTopInputReaderIsDone ();
    
    void
    DisconnectInput();

    Communication m_input_comm;
    StreamFile m_input_file;
    StreamFile m_output_file;
    StreamFile m_error_file;
    TargetList m_target_list;
    Listener m_listener;
    SourceManager m_source_manager;
    std::auto_ptr<CommandInterpreter> m_command_interpreter_ap;
    ExecutionContext m_exe_ctx;

    std::stack<lldb::InputReaderSP> m_input_readers;
    std::string m_input_reader_data;
    bool m_use_external_editor;   // FIXME: Convert this to a set/show variable on the debugger.

private:

    // Use Debugger::CreateInstance() to get a shared pointer to a new
    // debugger object
    Debugger ();

    DISALLOW_COPY_AND_ASSIGN (Debugger);
};

} // namespace lldb_private

#endif  // #if defined(__cplusplus)
#endif  // liblldb_Debugger_h_
