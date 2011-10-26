//===-- CommandObjectWatchpoint.cpp -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CommandObjectWatchpoint.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Breakpoint/Watchpoint.h"
#include "lldb/Breakpoint/WatchpointList.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Target/Target.h"
#include "lldb/Interpreter/CommandCompletions.h"

#include <vector>

using namespace lldb;
using namespace lldb_private;

static void
AddWatchpointDescription(Stream *s, Watchpoint *wp, lldb::DescriptionLevel level)
{
    s->IndentMore();
    wp->GetDescription(s, level);
    s->IndentLess();
    s->EOL();
}

static bool
CheckTargetForWatchpointOperations(Target *target, CommandReturnObject &result)
{
    if (target == NULL)
    {
        result.AppendError ("Invalid target.  No existing target or watchpoints.");
        result.SetStatus (eReturnStatusFailed);
        return false;
    }
    bool process_is_valid = target->GetProcessSP() && target->GetProcessSP()->IsAlive();
    if (!process_is_valid)
    {
        result.AppendError ("Thre's no process or it is not alive.");
        result.SetStatus (eReturnStatusFailed);
        return false;
    }
    // Target passes our checks, return true.
    return true;
}

#include "llvm/ADT/StringRef.h"

// Equivalent class: {"-", "to", "To", "TO"} of range specifier array.
static const char* RSA[4] = { "-", "to", "To", "TO" };

// Return the index to RSA if found; otherwise -1 is returned.
static int32_t
WithRSAIndex(llvm::StringRef &Arg)
{
    
    uint32_t i;
    for (i = 0; i < 4; ++i)
        if (Arg.find(RSA[i]) != llvm::StringRef::npos)
            return i;
    return -1;
}

// Return true if wp_ids is successfully populated with the watch ids.
// False otherwise.
static bool
VerifyWatchpointIDs(Args &args, std::vector<uint32_t> &wp_ids)
{
    // Pre-condition: args.GetArgumentCount() > 0.
    assert(args.GetArgumentCount() > 0);

    llvm::StringRef Minus("-");
    std::vector<llvm::StringRef> StrRefArgs;
    std::pair<llvm::StringRef, llvm::StringRef> Pair;
    size_t i;
    int32_t idx;
    // Go through the argments and make a canonical form of arg list containing
    // only numbers with possible "-" in between.
    for (i = 0; i < args.GetArgumentCount(); ++i) {
        llvm::StringRef Arg(args.GetArgumentAtIndex(i));
        if ((idx = WithRSAIndex(Arg)) == -1) {
            StrRefArgs.push_back(Arg);
            continue;
        }
        // The Arg contains the range specifier, split it, then.
        Pair = Arg.split(RSA[idx]);
        if (!Pair.first.empty())
            StrRefArgs.push_back(Pair.first);
        StrRefArgs.push_back(Minus);
        if (!Pair.second.empty())
            StrRefArgs.push_back(Pair.second);
    }
    // Now process the canonical list and fill in the vector of uint32_t's.
    // If there is any error, return false and the client should ignore wp_ids.
    uint32_t beg, end, id;
    size_t size = StrRefArgs.size();
    bool in_range = false;
    for (i = 0; i < size; ++i) {
        llvm::StringRef Arg = StrRefArgs[i];
        if (in_range) {
            // Look for the 'end' of the range.  Note StringRef::getAsInteger()
            // returns true to signify error while parsing.
            if (Arg.getAsInteger(0, end))
                return false;
            // Found a range!  Now append the elements.
            for (id = beg; id <= end; ++id)
                wp_ids.push_back(id);
            in_range = false;
            continue;
        }
        if (i < (size - 1) && StrRefArgs[i+1] == Minus) {
            if (Arg.getAsInteger(0, beg))
                return false;
            // Turn on the in_range flag, we are looking for end of range next.
            ++i; in_range = true;
            continue;
        }
        // Otherwise, we have a simple ID.  Just append it.
        if (Arg.getAsInteger(0, beg))
            return false;
        wp_ids.push_back(beg);
    }
    // It is an error if after the loop, we're still in_range.
    if (in_range)
        return false;

    return true; // Success!
}

//-------------------------------------------------------------------------
// CommandObjectMultiwordWatchpoint
//-------------------------------------------------------------------------
#pragma mark MultiwordWatchpoint

CommandObjectMultiwordWatchpoint::CommandObjectMultiwordWatchpoint(CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter, 
                            "watchpoint",
                            "A set of commands for operating on watchpoints.",
                            "watchpoint <command> [<command-options>]")
{
    bool status;

    CommandObjectSP list_command_object (new CommandObjectWatchpointList (interpreter));
    CommandObjectSP enable_command_object (new CommandObjectWatchpointEnable (interpreter));
    CommandObjectSP disable_command_object (new CommandObjectWatchpointDisable (interpreter));
    CommandObjectSP delete_command_object (new CommandObjectWatchpointDelete (interpreter));
    CommandObjectSP ignore_command_object (new CommandObjectWatchpointIgnore (interpreter));
    CommandObjectSP modify_command_object (new CommandObjectWatchpointModify (interpreter));

    list_command_object->SetCommandName ("watchpoint list");
    enable_command_object->SetCommandName("watchpoint enable");
    disable_command_object->SetCommandName("watchpoint disable");
    delete_command_object->SetCommandName("watchpoint delete");
    ignore_command_object->SetCommandName("watchpoint ignore");
    modify_command_object->SetCommandName("watchpoint modify");

    status = LoadSubCommand ("list",       list_command_object);
    status = LoadSubCommand ("enable",     enable_command_object);
    status = LoadSubCommand ("disable",    disable_command_object);
    status = LoadSubCommand ("delete",     delete_command_object);
    status = LoadSubCommand ("ignore",     ignore_command_object);
    status = LoadSubCommand ("modify",     modify_command_object);
}

CommandObjectMultiwordWatchpoint::~CommandObjectMultiwordWatchpoint()
{
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointList::Options
//-------------------------------------------------------------------------
#pragma mark List::CommandOptions

CommandObjectWatchpointList::CommandOptions::CommandOptions(CommandInterpreter &interpreter) :
    Options(interpreter),
    m_level(lldb::eDescriptionLevelBrief) // Watchpoint List defaults to brief descriptions
{
}

CommandObjectWatchpointList::CommandOptions::~CommandOptions()
{
}

OptionDefinition
CommandObjectWatchpointList::CommandOptions::g_option_table[] =
{
    { LLDB_OPT_SET_1, false, "brief",    'b', no_argument, NULL, 0, eArgTypeNone,
        "Give a brief description of the watchpoint (no location info)."},

    { LLDB_OPT_SET_2, false, "full",    'f', no_argument, NULL, 0, eArgTypeNone,
        "Give a full description of the watchpoint and its locations."},

    { LLDB_OPT_SET_3, false, "verbose", 'v', no_argument, NULL, 0, eArgTypeNone,
        "Explain everything we know about the watchpoint (for debugging debugger bugs)." },

    { 0, false, NULL, 0, 0, NULL, 0, eArgTypeNone, NULL }
};

const OptionDefinition*
CommandObjectWatchpointList::CommandOptions::GetDefinitions()
{
    return g_option_table;
}

Error
CommandObjectWatchpointList::CommandOptions::SetOptionValue(uint32_t option_idx, const char *option_arg)
{
    Error error;
    char short_option = (char) m_getopt_table[option_idx].val;

    switch (short_option)
    {
        case 'b':
            m_level = lldb::eDescriptionLevelBrief;
            break;
        case 'f':
            m_level = lldb::eDescriptionLevelFull;
            break;
        case 'v':
            m_level = lldb::eDescriptionLevelVerbose;
            break;
        default:
            error.SetErrorStringWithFormat("unrecognized option '%c'", short_option);
            break;
    }

    return error;
}

void
CommandObjectWatchpointList::CommandOptions::OptionParsingStarting()
{
    m_level = lldb::eDescriptionLevelFull;
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointList
//-------------------------------------------------------------------------
#pragma mark List

CommandObjectWatchpointList::CommandObjectWatchpointList(CommandInterpreter &interpreter) :
    CommandObject(interpreter, 
                  "watchpoint list",
                  "List all watchpoints at configurable levels of detail.",
                  NULL),
    m_options(interpreter)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back(arg);
}

CommandObjectWatchpointList::~CommandObjectWatchpointList()
{
}

Options *
CommandObjectWatchpointList::GetOptions()
{
    return &m_options;
}

bool
CommandObjectWatchpointList::Execute(Args& args, CommandReturnObject &result)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (target == NULL)
    {
        result.AppendError ("Invalid target. No current target or watchpoints.");
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
        return true;
    }

    const WatchpointList &watchpoints = target->GetWatchpointList();
    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);

    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendMessage("No watchpoints currently set.");
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
        return true;
    }

    Stream &output_stream = result.GetOutputStream();

    if (args.GetArgumentCount() == 0)
    {
        // No watchpoint selected; show info about all currently set watchpoints.
        result.AppendMessage ("Current watchpoints:");
        for (size_t i = 0; i < num_watchpoints; ++i)
        {
            Watchpoint *wp = watchpoints.GetByIndex(i).get();
            AddWatchpointDescription(&output_stream, wp, m_options.m_level);
        }
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
    }
    else
    {
        // Particular watchpoints selected; enable them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
        {
            Watchpoint *wp = watchpoints.FindByID(wp_ids[i]).get();
            if (wp)
                AddWatchpointDescription(&output_stream, wp, m_options.m_level);
            result.SetStatus(eReturnStatusSuccessFinishNoResult);
        }
    }

    return result.Succeeded();
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointEnable
//-------------------------------------------------------------------------
#pragma mark Enable

CommandObjectWatchpointEnable::CommandObjectWatchpointEnable(CommandInterpreter &interpreter) :
    CommandObject(interpreter,
                  "enable",
                  "Enable the specified disabled watchpoint(s). If no watchpoints are specified, enable all of them.",
                  NULL)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back(arg);
}

CommandObjectWatchpointEnable::~CommandObjectWatchpointEnable()
{
}

bool
CommandObjectWatchpointEnable::Execute(Args& args, CommandReturnObject &result)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (!CheckTargetForWatchpointOperations(target, result))
        return false;

    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);

    const WatchpointList &watchpoints = target->GetWatchpointList();

    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendError("No watchpoints exist to be enabled.");
        result.SetStatus(eReturnStatusFailed);
        return false;
    }

    if (args.GetArgumentCount() == 0)
    {
        // No watchpoint selected; enable all currently set watchpoints.
        target->EnableAllWatchpoints();
        result.AppendMessageWithFormat("All watchpoints enabled. (%lu watchpoints)\n", num_watchpoints);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
    }
    else
    {
        // Particular watchpoints selected; enable them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        int count = 0;
        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
            if (target->EnableWatchpointByID(wp_ids[i]))
                ++count;
        result.AppendMessageWithFormat("%d watchpoints enabled.\n", count);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
    }

    return result.Succeeded();
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointDisable
//-------------------------------------------------------------------------
#pragma mark Disable

CommandObjectWatchpointDisable::CommandObjectWatchpointDisable(CommandInterpreter &interpreter) :
    CommandObject(interpreter,
                  "watchpoint disable",
                  "Disable the specified watchpoint(s) without removing it/them.  If no watchpoints are specified, disable them all.",
                  NULL)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back(arg);
}

CommandObjectWatchpointDisable::~CommandObjectWatchpointDisable()
{
}

bool
CommandObjectWatchpointDisable::Execute(Args& args, CommandReturnObject &result)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (!CheckTargetForWatchpointOperations(target, result))
        return false;

    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);

    const WatchpointList &watchpoints = target->GetWatchpointList();
    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendError("No watchpoints exist to be disabled.");
        result.SetStatus(eReturnStatusFailed);
        return false;
    }

    if (args.GetArgumentCount() == 0)
    {
        // No watchpoint selected; disable all currently set watchpoints.
        if (target->DisableAllWatchpoints())
        {
            result.AppendMessageWithFormat("All watchpoints disabled. (%lu watchpoints)\n", num_watchpoints);
            result.SetStatus(eReturnStatusSuccessFinishNoResult);
        }
        else
        {
            result.AppendError("Disable all watchpoints failed\n");
            result.SetStatus(eReturnStatusFailed);
        }
    }
    else
    {
        // Particular watchpoints selected; disable them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        int count = 0;
        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
            if (target->DisableWatchpointByID(wp_ids[i]))
                ++count;
        result.AppendMessageWithFormat("%d watchpoints disabled.\n", count);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
    }

    return result.Succeeded();
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointDelete
//-------------------------------------------------------------------------
#pragma mark Delete

CommandObjectWatchpointDelete::CommandObjectWatchpointDelete(CommandInterpreter &interpreter) :
    CommandObject(interpreter,
                  "watchpoint delete",
                  "Delete the specified watchpoint(s).  If no watchpoints are specified, delete them all.",
                  NULL)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back(arg);
}

CommandObjectWatchpointDelete::~CommandObjectWatchpointDelete()
{
}

bool
CommandObjectWatchpointDelete::Execute(Args& args, CommandReturnObject &result)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (!CheckTargetForWatchpointOperations(target, result))
        return false;

    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);
    
    const WatchpointList &watchpoints = target->GetWatchpointList();

    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendError("No watchpoints exist to be deleted.");
        result.SetStatus(eReturnStatusFailed);
        return false;
    }

    if (args.GetArgumentCount() == 0)
    {
        if (!m_interpreter.Confirm("About to delete all watchpoints, do you want to do that?", true))
        {
            result.AppendMessage("Operation cancelled...");
        }
        else
        {
            target->RemoveAllWatchpoints();
            result.AppendMessageWithFormat("All watchpoints removed. (%lu watchpoints)\n", num_watchpoints);
        }
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }
    else
    {
        // Particular watchpoints selected; delete them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        int count = 0;
        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
            if (target->RemoveWatchpointByID(wp_ids[i]))
                ++count;
        result.AppendMessageWithFormat("%d watchpoints deleted.\n",count);
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }

    return result.Succeeded();
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointIgnore::CommandOptions
//-------------------------------------------------------------------------
#pragma mark Ignore::CommandOptions

CommandObjectWatchpointIgnore::CommandOptions::CommandOptions(CommandInterpreter &interpreter) :
    Options (interpreter),
    m_ignore_count (0)
{
}

CommandObjectWatchpointIgnore::CommandOptions::~CommandOptions ()
{
}

OptionDefinition
CommandObjectWatchpointIgnore::CommandOptions::g_option_table[] =
{
    { LLDB_OPT_SET_ALL, true, "ignore-count", 'i', required_argument, NULL, NULL, eArgTypeCount, "Set the number of times this watchpoint is skipped before stopping." },
    { 0,                false, NULL,            0 , 0,                 NULL, 0,    eArgTypeNone, NULL }
};

const OptionDefinition*
CommandObjectWatchpointIgnore::CommandOptions::GetDefinitions ()
{
    return g_option_table;
}

Error
CommandObjectWatchpointIgnore::CommandOptions::SetOptionValue (uint32_t option_idx, const char *option_arg)
{
    Error error;
    char short_option = (char) m_getopt_table[option_idx].val;

    switch (short_option)
    {
        case 'i':
        {
            m_ignore_count = Args::StringToUInt32(option_arg, UINT32_MAX, 0);
            if (m_ignore_count == UINT32_MAX)
               error.SetErrorStringWithFormat ("invalid ignore count '%s'", option_arg);
        }
        break;
        default:
            error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
            break;
    }

    return error;
}

void
CommandObjectWatchpointIgnore::CommandOptions::OptionParsingStarting ()
{
    m_ignore_count = 0;
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointIgnore
//-------------------------------------------------------------------------
#pragma mark Ignore

CommandObjectWatchpointIgnore::CommandObjectWatchpointIgnore(CommandInterpreter &interpreter) :
    CommandObject(interpreter,
                  "watchpoint ignore",
                  "Set ignore count on the specified watchpoint(s).  If no watchpoints are specified, set them all.",
                  NULL),
    m_options (interpreter)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back(arg);
}

CommandObjectWatchpointIgnore::~CommandObjectWatchpointIgnore()
{
}

Options *
CommandObjectWatchpointIgnore::GetOptions ()
{
    return &m_options;
}

bool
CommandObjectWatchpointIgnore::Execute(Args& args, CommandReturnObject &result)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (!CheckTargetForWatchpointOperations(target, result))
        return false;

    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);
    
    const WatchpointList &watchpoints = target->GetWatchpointList();

    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendError("No watchpoints exist to be ignored.");
        result.SetStatus(eReturnStatusFailed);
        return false;
    }

    if (args.GetArgumentCount() == 0)
    {
        target->IgnoreAllWatchpoints(m_options.m_ignore_count);
        result.AppendMessageWithFormat("All watchpoints ignored. (%lu watchpoints)\n", num_watchpoints);
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }
    else
    {
        // Particular watchpoints selected; ignore them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        int count = 0;
        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
            if (target->IgnoreWatchpointByID(wp_ids[i], m_options.m_ignore_count))
                ++count;
        result.AppendMessageWithFormat("%d watchpoints ignored.\n",count);
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }

    return result.Succeeded();
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointModify::CommandOptions
//-------------------------------------------------------------------------
#pragma mark Modify::CommandOptions

CommandObjectWatchpointModify::CommandOptions::CommandOptions(CommandInterpreter &interpreter) :
    Options (interpreter),
    m_condition (),
    m_condition_passed (false)
{
}

CommandObjectWatchpointModify::CommandOptions::~CommandOptions ()
{
}

OptionDefinition
CommandObjectWatchpointModify::CommandOptions::g_option_table[] =
{
{ LLDB_OPT_SET_ALL, false, "condition",    'c', required_argument, NULL, NULL, eArgTypeExpression, "The watchpoint stops only if this condition expression evaluates to true."},
{ 0,                false, NULL,            0 , 0,                 NULL, 0,    eArgTypeNone, NULL }
};

const OptionDefinition*
CommandObjectWatchpointModify::CommandOptions::GetDefinitions ()
{
    return g_option_table;
}

Error
CommandObjectWatchpointModify::CommandOptions::SetOptionValue (uint32_t option_idx, const char *option_arg)
{
    Error error;
    char short_option = (char) m_getopt_table[option_idx].val;

    switch (short_option)
    {
        case 'c':
            if (option_arg != NULL)
                m_condition.assign (option_arg);
            else
                m_condition.clear();
            m_condition_passed = true;
            break;
        default:
            error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
            break;
    }

    return error;
}

void
CommandObjectWatchpointModify::CommandOptions::OptionParsingStarting ()
{
    m_condition.clear();
    m_condition_passed = false;
}

//-------------------------------------------------------------------------
// CommandObjectWatchpointModify
//-------------------------------------------------------------------------
#pragma mark Modify

CommandObjectWatchpointModify::CommandObjectWatchpointModify (CommandInterpreter &interpreter) :
    CommandObject (interpreter,
                   "watchpoint modify", 
                   "Modify the options on a watchpoint or set of watchpoints in the executable.  "
                   "If no watchpoint is specified, act on the last created watchpoint.  "
                   "Passing an empty argument clears the modification.", 
                   NULL),
    m_options (interpreter)
{
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeWatchpointID, eArgTypeWatchpointIDRange);
    // Add the entry for the first argument for this command to the object's arguments vector.
    m_arguments.push_back (arg);   
}

CommandObjectWatchpointModify::~CommandObjectWatchpointModify ()
{
}

Options *
CommandObjectWatchpointModify::GetOptions ()
{
    return &m_options;
}

bool
CommandObjectWatchpointModify::Execute
(
    Args& args,
    CommandReturnObject &result
)
{
    Target *target = m_interpreter.GetDebugger().GetSelectedTarget().get();
    if (!CheckTargetForWatchpointOperations(target, result))
        return false;

    Mutex::Locker locker;
    target->GetWatchpointList().GetListMutex(locker);
    
    const WatchpointList &watchpoints = target->GetWatchpointList();

    size_t num_watchpoints = watchpoints.GetSize();

    if (num_watchpoints == 0)
    {
        result.AppendError("No watchpoints exist to be modified.");
        result.SetStatus(eReturnStatusFailed);
        return false;
    }

    if (args.GetArgumentCount() == 0)
    {
        WatchpointSP wp_sp = target->GetLastCreatedWatchpoint();
        wp_sp->SetCondition(m_options.m_condition.c_str());
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }
    else
    {
        // Particular watchpoints selected; set condition on them.
        std::vector<uint32_t> wp_ids;
        if (!VerifyWatchpointIDs(args, wp_ids))
        {
            result.AppendError("Invalid watchpoints specification.");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        int count = 0;
        const size_t size = wp_ids.size();
        for (size_t i = 0; i < size; ++i)
        {
            WatchpointSP wp_sp = watchpoints.FindByID(wp_ids[i]);
            if (wp_sp)
            {
                wp_sp->SetCondition(m_options.m_condition.c_str());
                ++count;
            }
        }
        result.AppendMessageWithFormat("%d watchpoints modified.\n",count);
        result.SetStatus (eReturnStatusSuccessFinishNoResult);
    }

    return result.Succeeded();
}
