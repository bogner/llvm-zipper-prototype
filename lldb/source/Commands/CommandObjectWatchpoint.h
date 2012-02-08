//===-- CommandObjectWatchpoint.h -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_CommandObjectWatchpoint_h_
#define liblldb_CommandObjectWatchpoint_h_

// C Includes
// C++ Includes

// Other libraries and framework includes
// Project includes
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Interpreter/OptionGroupWatchpoint.h"

namespace lldb_private {

//-------------------------------------------------------------------------
// CommandObjectMultiwordWatchpoint
//-------------------------------------------------------------------------

class CommandObjectMultiwordWatchpoint : public CommandObjectMultiword
{
public:
    CommandObjectMultiwordWatchpoint (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectMultiwordWatchpoint ();
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointList
//-------------------------------------------------------------------------

class CommandObjectWatchpointList : public CommandObject
{
public:
    CommandObjectWatchpointList (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointList ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

    virtual Options *
    GetOptions ();

    class CommandOptions : public Options
    {
    public:

        CommandOptions (CommandInterpreter &interpreter);

        virtual
        ~CommandOptions ();

        virtual Error
        SetOptionValue (uint32_t option_idx, const char *option_arg);

        void
        OptionParsingStarting ();

        const OptionDefinition *
        GetDefinitions ();

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];

        // Instance variables to hold the values for command options.

        lldb::DescriptionLevel m_level;
    };

private:
    CommandOptions m_options;
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointEnable
//-------------------------------------------------------------------------

class CommandObjectWatchpointEnable : public CommandObject
{
public:
    CommandObjectWatchpointEnable (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointEnable ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

private:
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointDisable
//-------------------------------------------------------------------------

class CommandObjectWatchpointDisable : public CommandObject
{
public:
    CommandObjectWatchpointDisable (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointDisable ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

private:
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointDelete
//-------------------------------------------------------------------------

class CommandObjectWatchpointDelete : public CommandObject
{
public:
    CommandObjectWatchpointDelete (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointDelete ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

private:
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointIgnore
//-------------------------------------------------------------------------

class CommandObjectWatchpointIgnore : public CommandObject
{
public:
    CommandObjectWatchpointIgnore (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointIgnore ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

    virtual Options *
    GetOptions ();

    class CommandOptions : public Options
    {
    public:

        CommandOptions (CommandInterpreter &interpreter);

        virtual
        ~CommandOptions ();

        virtual Error
        SetOptionValue (uint32_t option_idx, const char *option_arg);

        void
        OptionParsingStarting ();

        const OptionDefinition *
        GetDefinitions ();

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];

        // Instance variables to hold the values for command options.

        uint32_t m_ignore_count;
    };

private:
    CommandOptions m_options;
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointModify
//-------------------------------------------------------------------------

class CommandObjectWatchpointModify : public CommandObject
{
public:

    CommandObjectWatchpointModify (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointModify ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

    virtual Options *
    GetOptions ();

    class CommandOptions : public Options
    {
    public:

        CommandOptions (CommandInterpreter &interpreter);

        virtual
        ~CommandOptions ();

        virtual Error
        SetOptionValue (uint32_t option_idx, const char *option_arg);

        void
        OptionParsingStarting ();

        const OptionDefinition*
        GetDefinitions ();

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];

        // Instance variables to hold the values for command options.

        std::string m_condition;
        bool m_condition_passed;
    };

private:
    CommandOptions m_options;
};

//-------------------------------------------------------------------------
// CommandObjectWatchpointSet
//-------------------------------------------------------------------------

class CommandObjectWatchpointSet : public CommandObject
{
public:

    class CommandOptions : public OptionGroup
    {
    public:

        CommandOptions ();

        virtual
        ~CommandOptions ();

        virtual uint32_t
        GetNumDefinitions ();
        
        virtual const OptionDefinition*
        GetDefinitions ();
        
        virtual Error
        SetOptionValue (CommandInterpreter &interpreter,
                        uint32_t option_idx,
                        const char *option_value);
        
        virtual void
        OptionParsingStarting (CommandInterpreter &interpreter);

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];
        bool m_do_expression;
        bool m_do_variable;
    };

    CommandObjectWatchpointSet (CommandInterpreter &interpreter);

    virtual
    ~CommandObjectWatchpointSet ();

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result);

    virtual Options *
    GetOptions ();

private:
    OptionGroupOptions m_option_group;
    OptionGroupWatchpoint m_option_watchpoint;
    CommandOptions m_command_options;
};

} // namespace lldb_private

#endif  // liblldb_CommandObjectWatchpoint_h_
