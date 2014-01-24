//===-- lldb-gdbserver.cpp --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/lldb-python.h"

// C Includes
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C++ Includes

// Other libraries and framework includes
#include "lldb/lldb-private-log.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/ConnectionFileDescriptor.h"
#include "lldb/Core/ConnectionMachPort.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "Plugins/Process/gdb-remote/GDBRemoteCommunicationServer.h"
#include "Plugins/Process/gdb-remote/ProcessGDBRemoteLog.h"
using namespace lldb;
using namespace lldb_private;

//----------------------------------------------------------------------
// option descriptors for getopt_long_only()
//----------------------------------------------------------------------

int g_debug = 0;
int g_verbose = 0;

static struct option g_long_options[] =
{
    { "debug",              no_argument,        &g_debug,           1   },
    { "verbose",            no_argument,        &g_verbose,         1   },
    { "lldb-command",       required_argument,  NULL,               'c' },
    { "log-file",           required_argument,  NULL,               'l' },
    { "log-flags",          required_argument,  NULL,               'f' },
    { NULL,                 0,                  NULL,               0   }
};


//----------------------------------------------------------------------
// Watch for signals
//----------------------------------------------------------------------
int g_sigpipe_received = 0;
void
signal_handler(int signo)
{
    switch (signo)
    {
    case SIGPIPE:
        g_sigpipe_received = 1;
        break;
    case SIGHUP:
        // Use SIGINT first, if that does not work, use SIGHUP as a last resort.
        // And we should not call exit() here because it results in the global destructors
        // to be invoked and wreaking havoc on the threads still running.
        Host::SystemLog(Host::eSystemLogWarning, "SIGHUP received, exiting lldb-gdbserver...\n");
        abort();
        break;
    }
}

static void
display_usage (const char *progname)
{
    fprintf(stderr, "Usage:\n  %s [--log-file log-file-path] [--log-flags flags] [--lldb-command command]* HOST:PORT "
            "[-- PROGRAM ARG1 ARG2 ...]\n", progname);
    exit(0);
}

//----------------------------------------------------------------------
// main
//----------------------------------------------------------------------
int
main (int argc, char *argv[])
{
    const char *progname = argv[0];
    signal (SIGPIPE, signal_handler);
    signal (SIGHUP, signal_handler);
    int long_option_index = 0;
    StreamSP log_stream_sp;
    Args log_args;
    Error error;
    int ch;
    Debugger::Initialize(NULL);

    lldb::DebuggerSP debugger_sp = Debugger::CreateInstance ();

    debugger_sp->SetInputFileHandle(stdin, false);
    debugger_sp->SetOutputFileHandle(stdout, false);
    debugger_sp->SetErrorFileHandle(stderr, false);

    // ProcessLaunchInfo launch_info;
    ProcessAttachInfo attach_info;

    bool show_usage = false;
    int option_error = 0;
#if __GLIBC__
    optind = 0;
#else
    optreset = 1;
    optind = 1;
#endif

    std::string short_options(OptionParser::GetShortOptionString(g_long_options));

    std::vector<std::string> lldb_commands;

    while ((ch = getopt_long_only(argc, argv, short_options.c_str(), g_long_options, &long_option_index)) != -1)
    {
        switch (ch)
        {
        case 0:   // Any optional that auto set themselves will return 0
            break;

        case 'l': // Set Log File
            if (optarg && optarg[0])
            {
                if ((strcasecmp(optarg, "stdout") == 0) || (strcmp(optarg, "/dev/stdout") == 0))
                {
                    log_stream_sp.reset (new StreamFile (stdout, false));
                }
                else if ((strcasecmp(optarg, "stderr") == 0) || (strcmp(optarg, "/dev/stderr") == 0))
                {
                    log_stream_sp.reset (new StreamFile (stderr, false));
                }
                else
                {
                    FILE *log_file = fopen(optarg, "w");
                    if (log_file)
                    {
                        setlinebuf(log_file);
                        log_stream_sp.reset (new StreamFile (log_file, true));
                    }
                    else
                    {
                        const char *errno_str = strerror(errno);
                        fprintf (stderr, "Failed to open log file '%s' for writing: errno = %i (%s)", optarg, errno, errno_str ? errno_str : "unknown error");
                    }

                }
            }
            break;

        case 'f': // Log Flags
            if (optarg && optarg[0])
                log_args.AppendArgument(optarg);
            break;

        case 'c': // lldb commands
            if (optarg && optarg[0])
                lldb_commands.push_back(optarg);
            break;

        case 'h':   /* fall-through is intentional */
        case '?':
            show_usage = true;
            break;
        }
    }

    if (show_usage || option_error)
    {
        display_usage(progname);
        exit(option_error);
    }

    if (log_stream_sp)
    {
        if (log_args.GetArgumentCount() == 0)
            log_args.AppendArgument("default");
        ProcessGDBRemoteLog::EnableLog (log_stream_sp, 0,log_args.GetConstArgumentVector(), log_stream_sp.get());
    }

    // Skip any options we consumed with getopt_long_only
    printf ("optind = %d\n", optind);
    argc -= optind;
    argv += optind;

    if (argc == 0)
    {
        display_usage(progname);
        exit(255);
    }

    // Run any commands requested
    for (const auto &lldb_command : lldb_commands)
    {
        printf("(lldb) %s\n", lldb_command.c_str ());

        lldb_private::CommandReturnObject result;
        debugger_sp->GetCommandInterpreter ().HandleCommand (lldb_command.c_str (), eLazyBoolNo, result);
        const char *output = result.GetOutputData ();
        if (output && output[0])
            puts (output);
    }

    const bool is_platform = false;
    GDBRemoteCommunicationServer gdb_server (is_platform);

    const char *host_and_port = argv[0];
    argc -= 1;
    argv += 1;
    // Any arguments left over are for the the program that we need to launch. If there
    // are no arguments, then the GDB server will start up and wait for an 'A' packet
    // to launch a program, or a vAttach packet to attach to an existing process.
    if (argc > 0)
    {
        error = gdb_server.SetLaunchArguments (argv, argc);
        if (error.Fail ())
        {
            fprintf (stderr, "error: failed to set launch args for '%s': %s\n", argv[0], error.AsCString());
            exit(1);
        }

        unsigned int launch_flags = eLaunchFlagStopAtEntry;
#if !defined(__linux__)
        // linux doesn't yet handle eLaunchFlagDebug
        launch_flags |= eLaunchFlagDebug;
#endif
        error = gdb_server.SetLaunchFlags (launch_flags);
        if (error.Fail ())
        {
            fprintf (stderr, "error: failed to set launch flags for '%s': %s\n", argv[0], error.AsCString());
            exit(1);
        }

        error = gdb_server.LaunchProcess ();
        if (error.Fail ())
        {
            fprintf (stderr, "error: failed to launch '%s': %s\n", argv[0], error.AsCString());
            exit(1);
        }
    }

    if (host_and_port && host_and_port[0])
    {
        std::unique_ptr<ConnectionFileDescriptor> conn_ap(new ConnectionFileDescriptor());
        if (conn_ap.get())
        {
            std::string connect_url ("listen://");
            connect_url.append(host_and_port);

            printf ("Listening for a connection on %s...\n", host_and_port);
            if (conn_ap->Connect(connect_url.c_str(), &error) == eConnectionStatusSuccess)
            {
                printf ("Connection established.\n");
                gdb_server.SetConnection (conn_ap.release());
            }
        }

        if (gdb_server.IsConnected())
        {
            // After we connected, we need to get an initial ack from...
            if (gdb_server.HandshakeWithClient(&error))
            {
                bool interrupt = false;
                bool done = false;
                while (!interrupt && !done)
                {
                    if (!gdb_server.GetPacketAndSendResponse (UINT32_MAX, error, interrupt, done))
                        break;
                }

                if (error.Fail())
                {
                    fprintf(stderr, "error: %s\n", error.AsCString());
                }
            }
            else
            {
                fprintf(stderr, "error: handshake with client failed\n");
            }
        }
    }

    Debugger::Terminate();

    fprintf(stderr, "lldb-gdbserver exiting...\n");

    return 0;
}
