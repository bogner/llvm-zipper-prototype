//===-- source/Host/linux/Host.cpp ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <stdio.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <execinfo.h>

// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Target/Process.h"

#include "lldb/Host/Host.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"

#include "lldb/Core/ModuleSpec.h"
#include "lldb/Symbol/ObjectFile.h"
#include "Plugins/Process/Linux/ProcFileReader.h"

using namespace lldb;
using namespace lldb_private;

typedef enum ProcessStateFlags
{
    eProcessStateRunning           = (1u << 0), // Running
    eProcessStateSleeping          = (1u << 1), // Sleeping in an interruptible wait
    eProcessStateWaiting           = (1u << 2), // Waiting in an uninterruptible disk sleep
    eProcessStateZombie            = (1u << 3), // Zombie
    eProcessStateTracedOrStopped   = (1u << 4), // Traced or stopped (on a signal)
    eProcessStatePaging            = (1u << 5)  // Paging
} ProcessStateFlags;

typedef struct ProcessStatInfo
{
    lldb::pid_t ppid;           // Parent Process ID
    uint32_t fProcessState;     // ProcessStateFlags
} ProcessStatInfo;

// Get the process info with additional information from /proc/$PID/stat (like process state, and tracer pid).
static bool GetProcessAndStatInfo (lldb::pid_t pid, ProcessInstanceInfo &process_info, ProcessStatInfo &stat_info, lldb::pid_t &tracerpid);

static bool
ReadProcPseudoFileStat (lldb::pid_t pid, ProcessStatInfo& stat_info)
{
    // Read the /proc/$PID/stat file.
    lldb::DataBufferSP buf_sp = ProcFileReader::ReadIntoDataBuffer (pid, "stat");

    // The filename of the executable is stored in parenthesis right after the pid. We look for the closing
    // parenthesis for the filename and work from there in case the name has something funky like ')' in it.
    const char *filename_end = strrchr ((const char *)buf_sp->GetBytes(), ')');
    if (filename_end)
    {
        char state = '\0';
        int ppid = LLDB_INVALID_PROCESS_ID;

        // Read state and ppid.
        sscanf (filename_end + 1, " %c %d", &state, &ppid);

        stat_info.ppid = ppid;

        switch (state)
        {
            case 'R':
                stat_info.fProcessState |= eProcessStateRunning;
                break;
            case 'S':
                stat_info.fProcessState |= eProcessStateSleeping;
                break;
            case 'D':
                stat_info.fProcessState |= eProcessStateWaiting;
                break;
            case 'Z':
                stat_info.fProcessState |= eProcessStateZombie;
                break;
            case 'T':
                stat_info.fProcessState |= eProcessStateTracedOrStopped;
                break;
            case 'W':
                stat_info.fProcessState |= eProcessStatePaging;
                break;
        }

        return true;
    }

    return false;
}

static void
GetLinuxProcessUserAndGroup (lldb::pid_t pid, ProcessInstanceInfo &process_info, lldb::pid_t &tracerpid)
{
    tracerpid = 0;
    uint32_t rUid = UINT32_MAX;     // Real User ID
    uint32_t eUid = UINT32_MAX;     // Effective User ID
    uint32_t rGid = UINT32_MAX;     // Real Group ID
    uint32_t eGid = UINT32_MAX;     // Effective Group ID

    // Read the /proc/$PID/status file and parse the Uid:, Gid:, and TracerPid: fields.
    lldb::DataBufferSP buf_sp = ProcFileReader::ReadIntoDataBuffer (pid, "status");

    static const char uid_token[] = "Uid:";
    char *buf_uid = strstr ((char *)buf_sp->GetBytes(), uid_token);
    if (buf_uid)
    {
        // Real, effective, saved set, and file system UIDs. Read the first two.
        buf_uid += sizeof(uid_token);
        rUid = strtol (buf_uid, &buf_uid, 10);
        eUid = strtol (buf_uid, &buf_uid, 10);
    }

    static const char gid_token[] = "Gid:";
    char *buf_gid = strstr ((char *)buf_sp->GetBytes(), gid_token);
    if (buf_gid)
    {
        // Real, effective, saved set, and file system GIDs. Read the first two.
        buf_gid += sizeof(gid_token);
        rGid = strtol (buf_gid, &buf_gid, 10);
        eGid = strtol (buf_gid, &buf_gid, 10);
    }

    static const char tracerpid_token[] = "TracerPid:";
    char *buf_tracerpid = strstr((char *)buf_sp->GetBytes(), tracerpid_token);
    if (buf_tracerpid)
    {
        // Tracer PID. 0 if we're not being debugged.
        buf_tracerpid += sizeof(tracerpid_token);
        tracerpid = strtol (buf_tracerpid, &buf_tracerpid, 10);
    }

    process_info.SetUserID (rUid);
    process_info.SetEffectiveUserID (eUid);
    process_info.SetGroupID (rGid);
    process_info.SetEffectiveGroupID (eGid);
}

bool
Host::GetOSVersion(uint32_t &major,
                   uint32_t &minor,
                   uint32_t &update)
{
    struct utsname un;
    int status;

    if (uname(&un))
        return false;

    status = sscanf(un.release, "%u.%u.%u", &major, &minor, &update);
    if (status == 3)
        return true;

    // Some kernels omit the update version, so try looking for just "X.Y" and
    // set update to 0.
    update = 0;
    status = sscanf(un.release, "%u.%u", &major, &minor);
    return status == 2;
}

lldb::DataBufferSP
Host::GetAuxvData(lldb_private::Process *process)
{
    return ProcFileReader::ReadIntoDataBuffer (process->GetID(), "auxv");
}

lldb::DataBufferSP
Host::GetAuxvData (lldb::pid_t pid)
{
    return ProcFileReader::ReadIntoDataBuffer (pid, "auxv");
}

static bool
IsDirNumeric(const char *dname)
{
    for (; *dname; dname++)
    {
        if (!isdigit (*dname))
            return false;
    }
    return true;
}

uint32_t
Host::FindProcesses (const ProcessInstanceInfoMatch &match_info, ProcessInstanceInfoList &process_infos)
{
    static const char procdir[] = "/proc/";

    DIR *dirproc = opendir (procdir);
    if (dirproc)
    {
        struct dirent *direntry = NULL;
        const uid_t our_uid = getuid();
        const lldb::pid_t our_pid = getpid();
        bool all_users = match_info.GetMatchAllUsers();

        while ((direntry = readdir (dirproc)) != NULL)
        {
            if (direntry->d_type != DT_DIR || !IsDirNumeric (direntry->d_name))
                continue;

            lldb::pid_t pid = atoi (direntry->d_name);

            // Skip this process.
            if (pid == our_pid)
                continue;

            lldb::pid_t tracerpid;
            ProcessStatInfo stat_info;
            ProcessInstanceInfo process_info;

            if (!GetProcessAndStatInfo (pid, process_info, stat_info, tracerpid))
                continue;

            // Skip if process is being debugged.
            if (tracerpid != 0)
                continue;

            // Skip zombies.
            if (stat_info.fProcessState & eProcessStateZombie)
                continue;

            // Check for user match if we're not matching all users and not running as root.
            if (!all_users && (our_uid != 0) && (process_info.GetUserID() != our_uid))
                continue;

            if (match_info.Matches (process_info))
            {
                process_infos.Append (process_info);
            }
        }

        closedir (dirproc);
    }

    return process_infos.GetSize();
}

bool
Host::FindProcessThreads (const lldb::pid_t pid, TidMap &tids_to_attach)
{
    bool tids_changed = false;
    static const char procdir[] = "/proc/";
    static const char taskdir[] = "/task/";
    std::string process_task_dir = procdir + std::to_string(pid) + taskdir;
    DIR *dirproc = opendir (process_task_dir.c_str());

    if (dirproc)
    {
        struct dirent *direntry = NULL;
        while ((direntry = readdir (dirproc)) != NULL)
        {
            if (direntry->d_type != DT_DIR || !IsDirNumeric (direntry->d_name))
                continue;

            lldb::tid_t tid = atoi(direntry->d_name);
            TidMap::iterator it = tids_to_attach.find(tid);
            if (it == tids_to_attach.end())
            {
                tids_to_attach.insert(TidPair(tid, false));
                tids_changed = true;
            }
        }
        closedir (dirproc);
    }

    return tids_changed;
}

static bool
GetELFProcessCPUType (const char *exe_path, ProcessInstanceInfo &process_info)
{
    // Clear the architecture.
    process_info.GetArchitecture().Clear();

    ModuleSpecList specs;
    FileSpec filespec (exe_path, false);
    const size_t num_specs = ObjectFile::GetModuleSpecifications (filespec, 0, 0, specs);
    // GetModuleSpecifications() could fail if the executable has been deleted or is locked.
    // But it shouldn't return more than 1 architecture.
    assert(num_specs <= 1 && "Linux plugin supports only a single architecture");
    if (num_specs == 1)
    {
        ModuleSpec module_spec;
        if (specs.GetModuleSpecAtIndex (0, module_spec) && module_spec.GetArchitecture().IsValid())
        {
            process_info.GetArchitecture () = module_spec.GetArchitecture();
            return true;
        }
    }
    return false;
}

static bool
GetProcessAndStatInfo (lldb::pid_t pid, ProcessInstanceInfo &process_info, ProcessStatInfo &stat_info, lldb::pid_t &tracerpid)
{
    tracerpid = 0;
    process_info.Clear();
    ::memset (&stat_info, 0, sizeof(stat_info));
    stat_info.ppid = LLDB_INVALID_PROCESS_ID;

    // Use special code here because proc/[pid]/exe is a symbolic link.
    char link_path[PATH_MAX];
    char exe_path[PATH_MAX] = "";
    if (snprintf (link_path, PATH_MAX, "/proc/%" PRIu64 "/exe", pid) <= 0)
        return false;

    ssize_t len = readlink (link_path, exe_path, sizeof(exe_path) - 1);
    if (len <= 0)
        return false;

    // readlink does not append a null byte.
    exe_path[len] = 0;

    // If the binary has been deleted, the link name has " (deleted)" appended.
    //  Remove if there.
    static const ssize_t deleted_len = strlen(" (deleted)");
    if (len > deleted_len &&
        !strcmp(exe_path + len - deleted_len, " (deleted)"))
    {
        exe_path[len - deleted_len] = 0;
    }
    else
    {
        GetELFProcessCPUType (exe_path, process_info);
    }

    process_info.SetProcessID(pid);
    process_info.GetExecutableFile().SetFile(exe_path, false);

    lldb::DataBufferSP buf_sp;

    // Get the process environment.
    buf_sp = ProcFileReader::ReadIntoDataBuffer(pid, "environ");
    Args &info_env = process_info.GetEnvironmentEntries();
    char *next_var = (char *)buf_sp->GetBytes();
    char *end_buf = next_var + buf_sp->GetByteSize();
    while (next_var < end_buf && 0 != *next_var)
    {
        info_env.AppendArgument(next_var);
        next_var += strlen(next_var) + 1;
    }

    // Get the commond line used to start the process.
    buf_sp = ProcFileReader::ReadIntoDataBuffer(pid, "cmdline");

    // Grab Arg0 first, if there is one.
    char *cmd = (char *)buf_sp->GetBytes();
    if (cmd)
    {
        process_info.SetArg0(cmd);

        // Now process any remaining arguments.
        Args &info_args = process_info.GetArguments();
        char *next_arg = cmd + strlen(cmd) + 1;
        end_buf = cmd + buf_sp->GetByteSize();
        while (next_arg < end_buf && 0 != *next_arg)
        {
            info_args.AppendArgument(next_arg);
            next_arg += strlen(next_arg) + 1;
        }
    }

    // Read /proc/$PID/stat to get our parent pid.
    if (ReadProcPseudoFileStat (pid, stat_info))
    {
        process_info.SetParentProcessID (stat_info.ppid);
    }

    // Get User and Group IDs and get tracer pid.
    GetLinuxProcessUserAndGroup (pid, process_info, tracerpid);

    return true;
}

bool
Host::GetProcessInfo (lldb::pid_t pid, ProcessInstanceInfo &process_info)
{
    lldb::pid_t tracerpid;
    ProcessStatInfo stat_info;

    return GetProcessAndStatInfo (pid, process_info, stat_info, tracerpid);
}

void
Host::ThreadCreated (const char *thread_name)
{
    if (!Host::SetThreadName (LLDB_INVALID_PROCESS_ID, LLDB_INVALID_THREAD_ID, thread_name))
    {
        Host::SetShortThreadName (LLDB_INVALID_PROCESS_ID, LLDB_INVALID_THREAD_ID, thread_name, 16);
    }
}

std::string
Host::GetThreadName (lldb::pid_t pid, lldb::tid_t tid)
{
    assert(pid != LLDB_INVALID_PROCESS_ID);
    assert(tid != LLDB_INVALID_THREAD_ID);

    // Read /proc/$TID/comm file.
    lldb::DataBufferSP buf_sp = ProcFileReader::ReadIntoDataBuffer (tid, "comm");
    const char *comm_str = (const char *)buf_sp->GetBytes();
    const char *cr_str = ::strchr(comm_str, '\n');
    size_t length = cr_str ? (cr_str - comm_str) : strlen(comm_str);

    std::string thread_name(comm_str, length);
    return thread_name;
}

void
Host::Backtrace (Stream &strm, uint32_t max_frames)
{
    if (max_frames > 0)
    {
        std::vector<void *> frame_buffer (max_frames, NULL);
        int num_frames = ::backtrace (&frame_buffer[0], frame_buffer.size());
        char** strs = ::backtrace_symbols (&frame_buffer[0], num_frames);
        if (strs)
        {
            // Start at 1 to skip the "Host::Backtrace" frame
            for (int i = 1; i < num_frames; ++i)
                strm.Printf("%s\n", strs[i]);
            ::free (strs);
        }
    }
}

size_t
Host::GetEnvironment (StringList &env)
{
    char **host_env = environ;
    char *env_entry;
    size_t i;
    for (i=0; (env_entry = host_env[i]) != NULL; ++i)
        env.AppendString(env_entry);
    return i;
}

const ConstString &
Host::GetDistributionId ()
{
    // Try to run 'lbs_release -i', and use that response
    // for the distribution id.

    static bool s_evaluated;
    static ConstString s_distribution_id;

    if (!s_evaluated)
    {
        s_evaluated = true;

        Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_HOST));
        if (log)
            log->Printf ("attempting to determine Linux distribution...");

        // check if the lsb_release command exists at one of the
        // following paths
        const char *const exe_paths[] = {
            "/bin/lsb_release",
            "/usr/bin/lsb_release"
        };

        for (size_t exe_index = 0;
             exe_index < sizeof (exe_paths) / sizeof (exe_paths[0]);
             ++exe_index)
        {
            const char *const get_distribution_info_exe = exe_paths[exe_index];
            if (access (get_distribution_info_exe, F_OK))
            {
                // this exe doesn't exist, move on to next exe
                if (log)
                    log->Printf ("executable doesn't exist: %s",
                            get_distribution_info_exe);
                continue;
            }

            // execute the distribution-retrieval command, read output
            std::string get_distribution_id_command (get_distribution_info_exe);
            get_distribution_id_command += " -i";

            FILE *file = popen (get_distribution_id_command.c_str (), "r");
            if (!file)
            {
                if (log)
                    log->Printf (
                        "failed to run command: \"%s\", cannot retrieve "
                        "platform information",
                        get_distribution_id_command.c_str ());
                return s_distribution_id;
            }

            // retrieve the distribution id string.
            char distribution_id[256] = { '\0' };
            if (fgets (distribution_id, sizeof (distribution_id) - 1, file)
                    != NULL)
            {
                if (log)
                    log->Printf ("distribution id command returned \"%s\"",
                            distribution_id);

                const char *const distributor_id_key = "Distributor ID:\t";
                if (strstr (distribution_id, distributor_id_key))
                {
                    // strip newlines
                    std::string id_string (distribution_id +
                            strlen (distributor_id_key));
                    id_string.erase(
                        std::remove (
                            id_string.begin (),
                            id_string.end (),
                            '\n'),
                        id_string.end ());

                    // lower case it and convert whitespace to underscores
                    std::transform (
                        id_string.begin(),
                        id_string.end (),
                        id_string.begin (),
                        [] (char ch)
                        { return tolower ( isspace (ch) ? '_' : ch ); });

                    s_distribution_id.SetCString (id_string.c_str ());
                    if (log)
                        log->Printf ("distribion id set to \"%s\"",
                                s_distribution_id.GetCString ());
                }
                else
                {
                    if (log)
                        log->Printf ("failed to find \"%s\" field in \"%s\"",
                                distributor_id_key, distribution_id);
                }
            }
            else
            {
                if (log)
                    log->Printf (
                        "failed to retrieve distribution id, \"%s\" returned no"
                        " lines", get_distribution_id_command.c_str ());
            }

            // clean up the file
            pclose(file);
        }
    }

    return s_distribution_id;
}
