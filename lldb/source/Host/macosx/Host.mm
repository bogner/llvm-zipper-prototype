//===-- Host.mm -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/Host.h"

#include <crt_externs.h>
#include <execinfo.h>
#include <grp.h>
#include <libproc.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Communication.h"
#include "lldb/Core/ConnectionFileDescriptor.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Target/Process.h"

#include "cfcpp/CFCBundle.h"
#include "cfcpp/CFCMutableArray.h"
#include "cfcpp/CFCMutableDictionary.h"
#include "cfcpp/CFCReleaser.h"
#include "cfcpp/CFCString.h"

#include "llvm/Support/Host.h"
#include "llvm/Support/MachO.h"

#include <objc/objc-auto.h>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <Foundation/Foundation.h>

using namespace lldb;
using namespace lldb_private;

class MacOSXDarwinThread
{
public:
    MacOSXDarwinThread(const char *thread_name) :
        m_pool (nil)
    {
        // Register our thread with the collector if garbage collection is enabled.
        if (objc_collectingEnabled())
        {
#if MAC_OS_X_VERSION_MAX_ALLOWED <= MAC_OS_X_VERSION_10_5
            // On Leopard and earlier there is no way objc_registerThreadWithCollector
            // function, so we do it manually.
            auto_zone_register_thread(auto_zone());
#else
            // On SnowLeopard and later we just call the thread registration function.
            objc_registerThreadWithCollector();
#endif
        }
        else
        {
            m_pool = [[NSAutoreleasePool alloc] init];
        }


        Host::SetThreadName (LLDB_INVALID_PROCESS_ID, LLDB_INVALID_THREAD_ID, thread_name);
    }

    ~MacOSXDarwinThread()
    {
        if (m_pool)
            [m_pool release];
    }

    static void PThreadDestructor (void *v)
    {
        delete (MacOSXDarwinThread*)v;
    }

protected:
    NSAutoreleasePool * m_pool;
private:
    DISALLOW_COPY_AND_ASSIGN (MacOSXDarwinThread);
};

static pthread_once_t g_thread_create_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_thread_create_key = 0;

static void
InitThreadCreated()
{
    ::pthread_key_create (&g_thread_create_key, MacOSXDarwinThread::PThreadDestructor);
}

void
Host::ThreadCreated (const char *thread_name)
{
    ::pthread_once (&g_thread_create_once, InitThreadCreated);
    if (g_thread_create_key)
    {
        ::pthread_setspecific (g_thread_create_key, new MacOSXDarwinThread(thread_name));
    }
}


bool
Host::ResolveExecutableInBundle (FileSpec &file)
{
#if defined (__APPLE__)
    if (file.GetFileType () == FileSpec::eFileTypeDirectory)
    {
        char path[PATH_MAX];
        if (file.GetPath(path, sizeof(path)))
        {
            CFCBundle bundle (path);
            CFCReleaser<CFURLRef> url(bundle.CopyExecutableURL ());
            if (url.get())
            {
                if (::CFURLGetFileSystemRepresentation (url.get(), YES, (UInt8*)path, sizeof(path)))
                {
                    file.SetFile(path, false);
                    return true;
                }
            }
        }
    }
#endif
  return false;
}

lldb::pid_t
Host::LaunchApplication (const FileSpec &app_file_spec)
{
    char app_path[PATH_MAX];
    app_file_spec.GetPath(app_path, sizeof(app_path));

    LSApplicationParameters app_params;
    ::memset (&app_params, 0, sizeof (app_params));
    app_params.flags = kLSLaunchDefaults | 
                       kLSLaunchDontAddToRecents | 
                       kLSLaunchNewInstance;
    
    
    FSRef app_fsref;
    CFCString app_cfstr (app_path, kCFStringEncodingUTF8);
    
    OSStatus error = ::FSPathMakeRef ((const UInt8 *)app_path, &app_fsref, NULL);
    
    // If we found the app, then store away the name so we don't have to re-look it up.
    if (error != noErr)
        return LLDB_INVALID_PROCESS_ID;
    
    app_params.application = &app_fsref;

    ProcessSerialNumber psn;

    error = ::LSOpenApplication (&app_params, &psn);

    if (error != noErr)
        return LLDB_INVALID_PROCESS_ID;

    ::pid_t pid = LLDB_INVALID_PROCESS_ID;
    error = ::GetProcessPID(&psn, &pid);
    return pid;
}


static void *
AcceptPIDFromInferior (void *arg)
{
    const char *connect_url = (const char *)arg;
    ConnectionFileDescriptor file_conn;
    Error error;
    if (file_conn.Connect (connect_url, &error) == eConnectionStatusSuccess)
    {
        char pid_str[256];
        ::memset (pid_str, 0, sizeof(pid_str));
        ConnectionStatus status;
        const size_t pid_str_len = file_conn.Read (pid_str, sizeof(pid_str), status, NULL);
        if (pid_str_len > 0)
        {
            int pid = atoi (pid_str);
            return (void *)(intptr_t)pid;
        }
    }
    return NULL;
}

static bool
WaitForProcessToSIGSTOP (const lldb::pid_t pid, const int timeout_in_seconds)
{
    const int time_delta_usecs = 100000;
    const int num_retries = timeout_in_seconds/time_delta_usecs;
    for (int i=0; i<num_retries; i++)
    {
        struct proc_bsdinfo bsd_info;
        int error = ::proc_pidinfo (pid, PROC_PIDTBSDINFO, 
                                    (uint64_t) 0, 
                                    &bsd_info, 
                                    PROC_PIDTBSDINFO_SIZE);
        
        switch (error)
        {
        case EINVAL:
        case ENOTSUP:
        case ESRCH:
        case EPERM:
            return false;
        
        default:
            break;

        case 0:
            if (bsd_info.pbi_status == SSTOP)
                return true;
        }
        ::usleep (time_delta_usecs);
    }
    return false;
}

static lldb::pid_t
LaunchInNewTerminalWithCommandFile 
(
    const char **argv, 
    const char **envp,
    const char *working_dir,
    const ArchSpec *arch_spec,
    bool stop_at_entry,
    bool disable_aslr
)
{
    if (!argv || !argv[0])
        return LLDB_INVALID_PROCESS_ID;

    OSStatus error = 0;
    
    FileSpec program (argv[0], false);
    
    
    std::string unix_socket_name;

    char temp_file_path[PATH_MAX];
    const char *tmpdir = ::getenv ("TMPDIR");
    if (tmpdir == NULL)
        tmpdir = "/tmp/";
    ::snprintf (temp_file_path, sizeof(temp_file_path), "%s%s-XXXXXX", tmpdir, program.GetFilename().AsCString());
    
    if (::mktemp (temp_file_path) == NULL)
        return LLDB_INVALID_PROCESS_ID;

    unix_socket_name.assign (temp_file_path);

    ::strncat (temp_file_path, ".command", sizeof (temp_file_path));

    StreamFile command_file;
    command_file.GetFile().Open (temp_file_path, 
                                 File::eOpenOptionWrite | File::eOpenOptionCanCreate,
                                 File::ePermissionsDefault);
    
    if (!command_file.GetFile().IsValid())
        return LLDB_INVALID_PROCESS_ID;
    
    FileSpec darwin_debug_file_spec;
    if (!Host::GetLLDBPath (ePathTypeSupportExecutableDir, darwin_debug_file_spec))
        return LLDB_INVALID_PROCESS_ID;
    darwin_debug_file_spec.GetFilename().SetCString("darwin-debug");
        
    if (!darwin_debug_file_spec.Exists())
        return LLDB_INVALID_PROCESS_ID;
    
    char launcher_path[PATH_MAX];
    darwin_debug_file_spec.GetPath(launcher_path, sizeof(launcher_path));
    command_file.Printf("\"%s\" ", launcher_path);
    
    command_file.Printf("--unix-socket=%s ", unix_socket_name.c_str());
    
    if (arch_spec && arch_spec->IsValid())
    {
        command_file.Printf("--arch=%s ", arch_spec->GetArchitectureName());
    }

    if (disable_aslr)
    {
        command_file.PutCString("--disable-aslr ");
    }
        
    command_file.PutCString("-- ");

    if (argv)
    {
        for (size_t i=0; argv[i] != NULL; ++i)
        {
            command_file.Printf("\"%s\" ", argv[i]);
        }
    }
    command_file.PutCString("\necho Process exited with status $?\n");
    command_file.GetFile().Close();
    if (::chmod (temp_file_path, S_IRWXU | S_IRWXG) != 0)
        return LLDB_INVALID_PROCESS_ID;
            
    CFCMutableDictionary cf_env_dict;
    
    const bool can_create = true;
    if (envp)
    {
        for (size_t i=0; envp[i] != NULL; ++i)
        {
            const char *env_entry = envp[i];            
            const char *equal_pos = strchr(env_entry, '=');
            if (equal_pos)
            {
                std::string env_key (env_entry, equal_pos);
                std::string env_val (equal_pos + 1);
                CFCString cf_env_key (env_key.c_str(), kCFStringEncodingUTF8);
                CFCString cf_env_val (env_val.c_str(), kCFStringEncodingUTF8);
                cf_env_dict.AddValue (cf_env_key.get(), cf_env_val.get(), can_create);
            }
        }
    }
    
    LSApplicationParameters app_params;
    ::memset (&app_params, 0, sizeof (app_params));
    app_params.flags = kLSLaunchDontAddToRecents | kLSLaunchAsync;
    app_params.argv = NULL;
    app_params.environment = (CFDictionaryRef)cf_env_dict.get();

    CFCReleaser<CFURLRef> command_file_url (::CFURLCreateFromFileSystemRepresentation (NULL, 
                                                                                       (const UInt8 *)temp_file_path, 
                                                                                       strlen(temp_file_path),
                                                                                       false));
    
    CFCMutableArray urls;
    
    // Terminal.app will open the ".command" file we have created
    // and run our process inside it which will wait at the entry point
    // for us to attach.
    urls.AppendValue(command_file_url.get());


    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;

    Error lldb_error;
    // Sleep and wait a bit for debugserver to start to listen...
    char connect_url[128];
    ::snprintf (connect_url, sizeof(connect_url), "unix-accept://%s", unix_socket_name.c_str());

    // Spawn a new thread to accept incoming connection on the connect_url
    // so we can grab the pid from the inferior
    lldb::thread_t accept_thread = Host::ThreadCreate (unix_socket_name.c_str(),
                                                       AcceptPIDFromInferior,
                                                       connect_url,
                                                       &lldb_error);
    
    ProcessSerialNumber psn;
    error = LSOpenURLsWithRole(urls.get(), kLSRolesShell, NULL, &app_params, &psn, 1);
    if (error == noErr)
    {
        thread_result_t accept_thread_result = NULL;
        if (Host::ThreadJoin (accept_thread, &accept_thread_result, &lldb_error))
        {
            if (accept_thread_result)
            {
                pid = (intptr_t)accept_thread_result;
            
                // Wait for process to be stopped the the entry point by watching
                // for the process status to be set to SSTOP which indicates it it
                // SIGSTOP'ed at the entry point
                WaitForProcessToSIGSTOP (pid, 5);
            }
        }
    }
    else
    {
        Host::ThreadCancel (accept_thread, &lldb_error);
    }

    return pid;
}

const char *applscript_in_new_tty = 
"tell application \"Terminal\"\n"
"	do script \"%s\"\n"
"end tell\n";


const char *applscript_in_existing_tty = "\
set the_shell_script to \"%s\"\n\
tell application \"Terminal\"\n\
	repeat with the_window in (get windows)\n\
		repeat with the_tab in tabs of the_window\n\
			set the_tty to tty in the_tab\n\
			if the_tty contains \"%s\" then\n\
				if the_tab is not busy then\n\
					set selected of the_tab to true\n\
					set frontmost of the_window to true\n\
					do script the_shell_script in the_tab\n\
					return\n\
				end if\n\
			end if\n\
		end repeat\n\
	end repeat\n\
	do script the_shell_script\n\
end tell\n";

lldb::pid_t
LaunchInNewTerminalWithAppleScript
(
    const char *tty_name,
    const char **argv, 
    const char **envp,
    const char *working_dir,
    const ArchSpec *arch_spec,
    bool stop_at_entry,
    bool disable_aslr
)
{
    if (!argv || !argv[0])
        return LLDB_INVALID_PROCESS_ID;
    
    std::string unix_socket_name;

    char temp_file_path[PATH_MAX] = "/tmp/XXXXXX";    
    if (::mktemp (temp_file_path) == NULL)
        return LLDB_INVALID_PROCESS_ID;

    unix_socket_name.assign (temp_file_path);
    
    StreamString command;
    FileSpec darwin_debug_file_spec;
    if (!Host::GetLLDBPath (ePathTypeSupportExecutableDir, darwin_debug_file_spec))
        return LLDB_INVALID_PROCESS_ID;
    darwin_debug_file_spec.GetFilename().SetCString("darwin-debug");
        
    if (!darwin_debug_file_spec.Exists())
        return LLDB_INVALID_PROCESS_ID;
    
    char launcher_path[PATH_MAX];
    darwin_debug_file_spec.GetPath(launcher_path, sizeof(launcher_path));

    if (arch_spec)
        command.Printf("arch -arch %s ", arch_spec->GetArchitectureName());

    command.Printf("'%s' --unix-socket=%s", launcher_path, unix_socket_name.c_str());

    if (arch_spec && arch_spec->IsValid())
        command.Printf(" --arch=%s", arch_spec->GetArchitectureName());

    if (working_dir)
        command.Printf(" --working-dir '%s'", working_dir);
    
    if (disable_aslr)
        command.PutCString(" --disable-aslr");
        
    command.PutCString(" --");

    if (argv)
    {
        for (size_t i=0; argv[i] != NULL; ++i)
        {
            command.Printf(" '%s'", argv[i]);
        }
    }
    command.PutCString (" ; echo Process exited with status $?");
    
    StreamString applescript_source;

    const char *tty_command = command.GetString().c_str();
    if (tty_name && tty_name[0])
    {
        applescript_source.Printf (applscript_in_existing_tty, 
                                   tty_command,
                                   tty_name);
    }
    else
    {
        applescript_source.Printf (applscript_in_new_tty, 
                                   tty_command);
    }

    

    const char *script_source = applescript_source.GetString().c_str();
    //puts (script_source);
    NSAppleScript* applescript = [[NSAppleScript alloc] initWithSource:[NSString stringWithCString:script_source encoding:NSUTF8StringEncoding]];

    lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;

    Error lldb_error;
    // Sleep and wait a bit for debugserver to start to listen...
    ConnectionFileDescriptor file_conn;
    char connect_url[128];
    ::snprintf (connect_url, sizeof(connect_url), "unix-accept://%s", unix_socket_name.c_str());

    // Spawn a new thread to accept incoming connection on the connect_url
    // so we can grab the pid from the inferior
    lldb::thread_t accept_thread = Host::ThreadCreate (unix_socket_name.c_str(),
                                                       AcceptPIDFromInferior,
                                                       connect_url,
                                                       &lldb_error);
    

    [applescript executeAndReturnError:nil];
    
    thread_result_t accept_thread_result = NULL;
    if (Host::ThreadJoin (accept_thread, &accept_thread_result, &lldb_error))
    {
        if (accept_thread_result)
        {
            pid = (intptr_t)accept_thread_result;
        
            // Wait for process to be stopped the the entry point by watching
            // for the process status to be set to SSTOP which indicates it it
            // SIGSTOP'ed at the entry point
            WaitForProcessToSIGSTOP (pid, 5);
        }
    }
    ::unlink (unix_socket_name.c_str());
    [applescript release];
    return pid;
}


#define LLDB_HOST_USE_APPLESCRIPT

lldb::pid_t
Host::LaunchInNewTerminal 
(
    const char *tty_name,
    const char **argv, 
    const char **envp,
    const char *working_dir,
    const ArchSpec *arch_spec,
    bool stop_at_entry,
    bool disable_aslr
)
{
#if defined (LLDB_HOST_USE_APPLESCRIPT)
    return LaunchInNewTerminalWithAppleScript (tty_name, argv, envp, working_dir, arch_spec, stop_at_entry, disable_aslr);
#else
    return LaunchInNewTerminalWithCommandFile (argv, envp, working_dir, arch_spec, stop_at_entry, disable_aslr);
#endif
}

// On MacOSX CrashReporter will display a string for each shared library if
// the shared library has an exported symbol named "__crashreporter_info__".

static Mutex&
GetCrashReporterMutex ()
{
    static Mutex g_mutex;
    return g_mutex;
}

extern "C" {
    const char *__crashreporter_info__ = NULL;
};

asm(".desc ___crashreporter_info__, 0x10");

void
Host::SetCrashDescriptionWithFormat (const char *format, ...)
{
    static StreamString g_crash_description;
    Mutex::Locker locker (GetCrashReporterMutex ());
    
    if (format)
    {
        va_list args;
        va_start (args, format);
        g_crash_description.GetString().clear();
        g_crash_description.PrintfVarArg(format, args);
        va_end (args);
        __crashreporter_info__ = g_crash_description.GetData();
    }
    else
    {
        __crashreporter_info__ = NULL;
    }
}

void
Host::SetCrashDescription (const char *cstr)
{
    Mutex::Locker locker (GetCrashReporterMutex ());
    __crashreporter_info__ = cstr;
}

bool
Host::OpenFileInExternalEditor (const FileSpec &file_spec, uint32_t line_no)
{
    // We attach this to an 'odoc' event to specify a particular selection
    typedef struct {
        int16_t   reserved0;  // must be zero
        int16_t   fLineNumber;
        int32_t   fSelStart;
        int32_t   fSelEnd;
        uint32_t  reserved1;  // must be zero
        uint32_t  reserved2;  // must be zero
    } BabelAESelInfo;
    
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_HOST));
    char file_path[PATH_MAX];
    file_spec.GetPath(file_path, PATH_MAX);
    CFCString file_cfstr (file_path, kCFStringEncodingUTF8);
    CFCReleaser<CFURLRef> file_URL (::CFURLCreateWithFileSystemPath (NULL, 
                                                                     file_cfstr.get(), 
                                                                     kCFURLPOSIXPathStyle, 
                                                                     false));
                                                                     
    if (log)
        log->Printf("Sending source file: \"%s\" and line: %d to external editor.\n", file_path, line_no);
    
    OSStatus error;	
    BabelAESelInfo file_and_line_info = 
    {
        0,              // reserved0
        line_no - 1,    // fLineNumber (zero based line number)
        1,              // fSelStart
        1024,           // fSelEnd
        0,              // reserved1
        0               // reserved2
    };

    AEKeyDesc file_and_line_desc;
    
    error = ::AECreateDesc (typeUTF8Text, 
                            &file_and_line_info, 
                            sizeof (file_and_line_info), 
                            &(file_and_line_desc.descContent));
    
    if (error != noErr)
    {
        if (log)
            log->Printf("Error creating AEDesc: %d.\n", error);
        return false;
    }
    
    file_and_line_desc.descKey = keyAEPosition;
    
    static std::string g_app_name;
    static FSRef g_app_fsref;

    LSApplicationParameters app_params;
    ::memset (&app_params, 0, sizeof (app_params));
    app_params.flags = kLSLaunchDefaults | 
                       kLSLaunchDontAddToRecents | 
                       kLSLaunchDontSwitch;
    
    char *external_editor = ::getenv ("LLDB_EXTERNAL_EDITOR");
    
    if (external_editor)
    {
        if (log)
            log->Printf("Looking for external editor \"%s\".\n", external_editor);

        if (g_app_name.empty() || strcmp (g_app_name.c_str(), external_editor) != 0)
        {
            CFCString editor_name (external_editor, kCFStringEncodingUTF8);
            error = ::LSFindApplicationForInfo (kLSUnknownCreator, 
                                                NULL, 
                                                editor_name.get(), 
                                                &g_app_fsref, 
                                                NULL);
            
            // If we found the app, then store away the name so we don't have to re-look it up.
            if (error != noErr)
            {
                if (log)
                    log->Printf("Could not find External Editor application, error: %d.\n", error);
                return false;
            }
                
        }
        app_params.application = &g_app_fsref;
    }

    ProcessSerialNumber psn;
    CFCReleaser<CFArrayRef> file_array(CFArrayCreate (NULL, (const void **) file_URL.ptr_address(false), 1, NULL));
    error = ::LSOpenURLsWithRole (file_array.get(), 
                                  kLSRolesAll, 
                                  &file_and_line_desc, 
                                  &app_params, 
                                  &psn, 
                                  1);
    
    AEDisposeDesc (&(file_and_line_desc.descContent));

    if (error != noErr)
    {
        if (log)
            log->Printf("LSOpenURLsWithRole failed, error: %d.\n", error);

        return false;
    }
    
    ProcessInfoRec which_process;
    ::memset(&which_process, 0, sizeof(which_process));
    unsigned char ap_name[PATH_MAX];
    which_process.processName = ap_name;
    error = ::GetProcessInformation (&psn, &which_process);
    
    bool using_xcode;
    if (error != noErr)
    {
        if (log)
            log->Printf("GetProcessInformation failed, error: %d.\n", error);
        using_xcode = false;
    }
    else
        using_xcode = strncmp((char *) ap_name+1, "Xcode", (int) ap_name[0]) == 0;
    
    // Xcode doesn't obey the line number in the Open Apple Event.  So I have to send
    // it an AppleScript to focus on the right line.
    
    if (using_xcode)
    {
        static ComponentInstance osa_component = NULL;
        static const char *as_template = "tell application \"Xcode\"\n"
                                   "set doc to the first document whose path is \"%s\"\n"
                                   "set the selection to paragraph %d of doc\n"
                                   "--- set the selected paragraph range to {%d, %d} of doc\n"
                                   "end tell\n";
        const int chars_for_int = 32;
        static int as_template_len = strlen (as_template);

      
        char *as_str;
        AEDesc as_desc;
      
        if (osa_component == NULL)
        {
            osa_component = ::OpenDefaultComponent (kOSAComponentType,
                                                    kAppleScriptSubtype);
        }
        
        if (osa_component == NULL)
        {
            if (log)
                log->Printf("Could not get default AppleScript component.\n");
            return false;
        }

        uint32_t as_str_size = as_template_len + strlen (file_path) + 3 * chars_for_int + 1;     
        as_str = (char *) malloc (as_str_size);
        ::snprintf (as_str, 
                    as_str_size - 1, 
                    as_template, 
                    file_path, 
                    line_no, 
                    line_no, 
                    line_no);

        error = ::AECreateDesc (typeChar, 
                                as_str, 
                                strlen (as_str),
                                &as_desc);
        
        ::free (as_str);

        if (error != noErr)
        {
            if (log)
                log->Printf("Failed to create AEDesc for Xcode AppleEvent: %d.\n", error);
            return false;
        }
            
        OSAID ret_OSAID;
        error = ::OSACompileExecute (osa_component, 
                                     &as_desc, 
                                     kOSANullScript, 
                                     kOSAModeNeverInteract, 
                                     &ret_OSAID);
        
        ::OSADispose (osa_component, ret_OSAID);

        ::AEDisposeDesc (&as_desc);

        if (error != noErr)
        {
            if (log)
                log->Printf("Sending AppleEvent to Xcode failed, error: %d.\n", error);
            return false;
        }
    }
      
    return true;
}


void
Host::Backtrace (Stream &strm, uint32_t max_frames)
{
    char backtrace_path[] = "/tmp/lldb-backtrace-tmp-XXXXXX";
    int backtrace_fd = ::mkstemp (backtrace_path);
    if (backtrace_fd != -1)
    {
        std::vector<void *> frame_buffer (max_frames, NULL);
        int count = ::backtrace (&frame_buffer[0], frame_buffer.size());
        ::backtrace_symbols_fd (&frame_buffer[0], count, backtrace_fd);
        
        const off_t buffer_size = ::lseek(backtrace_fd, 0, SEEK_CUR);

        if (::lseek(backtrace_fd, 0, SEEK_SET) == 0)
        {
            char *buffer = (char *)::malloc (buffer_size);
            if (buffer)
            {
                ssize_t bytes_read = ::read (backtrace_fd, buffer, buffer_size);
                if (bytes_read > 0)
                    strm.Write(buffer, bytes_read);
                ::free (buffer);
            }
        }
        ::close (backtrace_fd);
        ::unlink (backtrace_path);
    }
}

size_t
Host::GetEnvironment (StringList &env)
{
    char **host_env = *_NSGetEnviron();
    char *env_entry;
    size_t i;
    for (i=0; (env_entry = host_env[i]) != NULL; ++i)
        env.AppendString(env_entry);
    return i;
        
}


bool
Host::GetOSBuildString (std::string &s)
{
    int mib[2] = { CTL_KERN, KERN_OSVERSION };
    char cstr[PATH_MAX];
    size_t cstr_len = sizeof(cstr);
    if (::sysctl (mib, 2, cstr, &cstr_len, NULL, 0) == 0)
    {
        s.assign (cstr, cstr_len);
        return true;
    }
    
    s.clear();
    return false;
}

bool
Host::GetOSKernelDescription (std::string &s)
{
    int mib[2] = { CTL_KERN, KERN_VERSION };
    char cstr[PATH_MAX];
    size_t cstr_len = sizeof(cstr);
    if (::sysctl (mib, 2, cstr, &cstr_len, NULL, 0) == 0)
    {
        s.assign (cstr, cstr_len);
        return true;
    }
    s.clear();
    return false;
}
    
bool
Host::GetOSVersion 
(
    uint32_t &major, 
    uint32_t &minor, 
    uint32_t &update
)
{
    
    SInt32 version;
    
    OSErr err = ::Gestalt (gestaltSystemVersion, &version);
    if (err != noErr) 
        return false;

    if (version < 0x1040)
    {
        major = ((version & 0xF000) >> 12) * 10 + ((version & 0x0F00) >> 8);
        minor = (version & 0x00F0) >> 4;
        update = (version & 0x000F);
    }
    else
    {
        if (::Gestalt (gestaltSystemVersionMajor, &version) != noErr)
            return false;
        major = version;

        if (::Gestalt (gestaltSystemVersionMinor, &version) == noErr)
            minor = version;
        else
            minor = 0;

        if (::Gestalt (gestaltSystemVersionBugFix, &version) == noErr)
            update = version;
        else
            update = 0;
    }
    
    return true;
}

static bool
GetMacOSXProcessName (NameMatchType name_match_type,
                      const char *name_match, 
                      ProcessInfo &process_info)
{
    if (process_info.ProcessIDIsValid())
    {
        char process_name[MAXCOMLEN * 2 + 1];
        int name_len = ::proc_name(process_info.GetProcessID(), process_name, MAXCOMLEN * 2);
        if (name_len == 0)
            return false;
        
        if (NameMatches(process_name, name_match_type, name_match))
        {
            process_info.SetName (process_name);
            return true;
        }
    }
    process_info.SetName (NULL);
    return false;
}


static bool
GetMacOSXProcessCPUType (ProcessInfo &process_info)
{
    if (process_info.ProcessIDIsValid())
    {
        // Make a new mib to stay thread safe
        int mib[CTL_MAXNAME]={0,};
        size_t mib_len = CTL_MAXNAME;
        if (::sysctlnametomib("sysctl.proc_cputype", mib, &mib_len)) 
            return false;
    
        mib[mib_len] = process_info.GetProcessID();
        mib_len++;
    
        cpu_type_t cpu, sub;
        size_t cpu_len = sizeof(cpu);
        if (::sysctl (mib, mib_len, &cpu, &cpu_len, 0, 0) == 0)
        {
            switch (cpu)
            {
                case llvm::MachO::CPUTypeI386:      sub = llvm::MachO::CPUSubType_I386_ALL;     break;
                case llvm::MachO::CPUTypeX86_64:    sub = llvm::MachO::CPUSubType_X86_64_ALL;   break;
                default: break;
            }
            process_info.GetArchitecture ().SetArchitecture (eArchTypeMachO, cpu, sub);
            return true;
        }
    }
    process_info.GetArchitecture().Clear();
    return false;
}

// TODO: move this into the platform
static bool
GetGroupName (uint32_t gid, std::string &group_name)
{
    char group_buffer[PATH_MAX];
    size_t group_buffer_size = sizeof(group_buffer);
    struct group group_info;
    struct group *group_info_ptr = &group_info;
    // User the real user ID here, not the effective user ID
    if (::getgrgid_r (gid,
                      &group_info,
                      group_buffer,
                      group_buffer_size,
                      &group_info_ptr) == 0)
    {
        if (group_info_ptr)
        {
            group_name.assign (group_info_ptr->gr_name);
            return true;
        }
    }
    group_name.clear();
    return false;
}

// TODO: move this into the platform
static bool
GetUserName (uint32_t uid, std::string &user_name)
{
    struct passwd user_info;
    struct passwd *user_info_ptr = &user_info;
    char user_buffer[PATH_MAX];
    size_t user_buffer_size = sizeof(user_buffer);
    if (::getpwuid_r (uid,
                      &user_info,
                      user_buffer,
                      user_buffer_size,
                      &user_info_ptr) == 0)
    {
        if (user_info_ptr)
        {
            user_name.assign (user_info_ptr->pw_name);
            return true;
        }
    }
    user_name.clear();
    return false;
}


static bool
GetMacOSXProcessUserAndGroup (ProcessInfo &process_info)
{
    if (process_info.ProcessIDIsValid())
    {
        int mib[4];
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PID;
        mib[3] = process_info.GetProcessID();
        struct kinfo_proc proc_kinfo;
        size_t proc_kinfo_size = sizeof(struct kinfo_proc);

        if (::sysctl (mib, 4, &proc_kinfo, &proc_kinfo_size, NULL, 0) == 0)
        {
            if (proc_kinfo_size > 0)
            {
                process_info.SetParentProcessID (proc_kinfo.kp_eproc.e_ppid);
                process_info.SetRealUserID (proc_kinfo.kp_eproc.e_pcred.p_ruid);
                process_info.SetRealGroupID (proc_kinfo.kp_eproc.e_pcred.p_rgid);
                process_info.SetEffectiveUserID (proc_kinfo.kp_eproc.e_ucred.cr_uid);
                if (proc_kinfo.kp_eproc.e_ucred.cr_ngroups > 0)
                    process_info.SetEffectiveGroupID (proc_kinfo.kp_eproc.e_ucred.cr_groups[0]);
                else
                    process_info.SetEffectiveGroupID (UINT32_MAX);            
                return true;
            }
        }
    }
    process_info.SetParentProcessID (LLDB_INVALID_PROCESS_ID);
    process_info.SetRealUserID (UINT32_MAX);
    process_info.SetRealGroupID (UINT32_MAX);
    process_info.SetEffectiveUserID (UINT32_MAX);
    process_info.SetEffectiveGroupID (UINT32_MAX);            
    return false;
}


uint32_t
Host::FindProcessesByName (const char *name, NameMatchType name_match_type, ProcessInfoList &process_infos)
{
    int num_pids;
    int size_of_pids;
    std::vector<int> pid_list;
    
    size_of_pids = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (size_of_pids == -1)
        return 0;
        
    num_pids = size_of_pids/sizeof(int);
    
    pid_list.resize (size_of_pids);
    size_of_pids = proc_listpids(PROC_ALL_PIDS, 0, &pid_list[0], size_of_pids);
    if (size_of_pids == -1)
        return 0;
        
    lldb::pid_t our_pid = getpid();
    
    for (int i = 0; i < num_pids; i++)
    {
        struct proc_bsdinfo bsd_info;
        int error = proc_pidinfo (pid_list[i], PROC_PIDTBSDINFO, (uint64_t) 0, &bsd_info, PROC_PIDTBSDINFO_SIZE);
        if (error == 0)
            continue;
        
        // Don't offer to attach to zombie processes, already traced or exiting
        // processes, and of course, ourselves...  It looks like passing the second arg of
        // 0 to proc_listpids will exclude zombies anyway, but that's not documented so...
        if (((bsd_info.pbi_flags & (PROC_FLAG_TRACED | PROC_FLAG_INEXIT)) != 0)
             || (bsd_info.pbi_status == SZOMB)
             || (bsd_info.pbi_pid == our_pid))
             continue;
        
        ProcessInfo process_info;
        process_info.SetProcessID (bsd_info.pbi_pid);
        if (GetMacOSXProcessName (name_match_type, name, process_info))
        {
            GetMacOSXProcessCPUType (process_info);
            GetMacOSXProcessUserAndGroup (process_info);
            process_infos.Append (process_info);
        }
    }    
    return process_infos.GetSize();
}

bool
Host::GetProcessInfo (lldb::pid_t pid, ProcessInfo &process_info)
{
    process_info.SetProcessID(pid);
    if (GetMacOSXProcessName (eNameMatchIgnore, NULL, process_info))
    {
        GetMacOSXProcessCPUType (process_info);
        GetMacOSXProcessUserAndGroup (process_info);
        return true;
    }    
    process_info.Clear();
    return false;
}


