//===-- GDBRemoteCommunicationClient.cpp ------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#include "GDBRemoteCommunicationClient.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
#include "llvm/ADT/Triple.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Core/ConnectionFileDescriptor.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/State.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Host/Endian.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/TimeValue.h"

// Project includes
#include "Utility/StringExtractorGDBRemote.h"
#include "ProcessGDBRemote.h"
#include "ProcessGDBRemoteLog.h"

using namespace lldb;
using namespace lldb_private;

//----------------------------------------------------------------------
// GDBRemoteCommunicationClient constructor
//----------------------------------------------------------------------
GDBRemoteCommunicationClient::GDBRemoteCommunicationClient() :
    GDBRemoteCommunication("gdb-remote.client", "gdb-remote.client.rx_packet"),
    m_supports_not_sending_acks (eLazyBoolCalculate),
    m_supports_thread_suffix (eLazyBoolCalculate),
    m_supports_qHostInfo (eLazyBoolCalculate),
    m_supports_vCont_all (eLazyBoolCalculate),
    m_supports_vCont_any (eLazyBoolCalculate),
    m_supports_vCont_c (eLazyBoolCalculate),
    m_supports_vCont_C (eLazyBoolCalculate),
    m_supports_vCont_s (eLazyBoolCalculate),
    m_supports_vCont_S (eLazyBoolCalculate),
    m_async_mutex (Mutex::eMutexTypeRecursive),
    m_async_packet_predicate (false),
    m_async_packet (),
    m_async_response (),
    m_async_signal (-1),
    m_host_arch(),
    m_os_version_major (UINT32_MAX),
    m_os_version_minor (UINT32_MAX),
    m_os_version_update (UINT32_MAX)
{
    m_rx_packet_listener.StartListeningForEvents(this,
                                                 Communication::eBroadcastBitPacketAvailable  |
                                                 Communication::eBroadcastBitReadThreadDidExit);
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
GDBRemoteCommunicationClient::~GDBRemoteCommunicationClient()
{
    m_rx_packet_listener.StopListeningForEvents(this,
                                                Communication::eBroadcastBitPacketAvailable  |
                                                Communication::eBroadcastBitReadThreadDidExit);
    if (IsConnected())
    {
        StopReadThread();
        Disconnect();
    }
}

bool
GDBRemoteCommunicationClient::HandshakeWithServer (Error *error_ptr)
{
    // Start the read thread after we send the handshake ack since if we
    // fail to send the handshake ack, there is no reason to continue...
    if (SendAck())
        return StartReadThread (error_ptr);
    
    if (error_ptr)
        error_ptr->SetErrorString("failed to send the handshake ack");
    return false;
}

void
GDBRemoteCommunicationClient::QueryNoAckModeSupported ()
{
    if (m_supports_not_sending_acks == eLazyBoolCalculate)
    {
        m_send_acks = true;
        m_supports_not_sending_acks = eLazyBoolNo;

        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse("QStartNoAckMode", response, false))
        {
            if (response.IsOKResponse())
            {
                m_send_acks = false;
                m_supports_not_sending_acks = eLazyBoolYes;
            }
        }
    }
}

void
GDBRemoteCommunicationClient::ResetDiscoverableSettings()
{
    m_supports_not_sending_acks = eLazyBoolCalculate;
    m_supports_thread_suffix = eLazyBoolCalculate;
    m_supports_qHostInfo = eLazyBoolCalculate;
    m_supports_vCont_c = eLazyBoolCalculate;
    m_supports_vCont_C = eLazyBoolCalculate;
    m_supports_vCont_s = eLazyBoolCalculate;
    m_supports_vCont_S = eLazyBoolCalculate;
    m_host_arch.Clear();
}


bool
GDBRemoteCommunicationClient::GetThreadSuffixSupported ()
{
    if (m_supports_thread_suffix == eLazyBoolCalculate)
    {
        StringExtractorGDBRemote response;
        m_supports_thread_suffix = eLazyBoolNo;
        if (SendPacketAndWaitForResponse("QThreadSuffixSupported", response, false))
        {
            if (response.IsOKResponse())
                m_supports_thread_suffix = eLazyBoolYes;
        }
    }
    return m_supports_thread_suffix;
}
bool
GDBRemoteCommunicationClient::GetVContSupported (char flavor)
{
    if (m_supports_vCont_c == eLazyBoolCalculate)
    {
        StringExtractorGDBRemote response;
        m_supports_vCont_any = eLazyBoolNo;
        m_supports_vCont_all = eLazyBoolNo;
        m_supports_vCont_c = eLazyBoolNo;
        m_supports_vCont_C = eLazyBoolNo;
        m_supports_vCont_s = eLazyBoolNo;
        m_supports_vCont_S = eLazyBoolNo;
        if (SendPacketAndWaitForResponse("vCont?", response, false))
        {
            const char *response_cstr = response.GetStringRef().c_str();
            if (::strstr (response_cstr, ";c"))
                m_supports_vCont_c = eLazyBoolYes;

            if (::strstr (response_cstr, ";C"))
                m_supports_vCont_C = eLazyBoolYes;

            if (::strstr (response_cstr, ";s"))
                m_supports_vCont_s = eLazyBoolYes;

            if (::strstr (response_cstr, ";S"))
                m_supports_vCont_S = eLazyBoolYes;

            if (m_supports_vCont_c == eLazyBoolYes &&
                m_supports_vCont_C == eLazyBoolYes &&
                m_supports_vCont_s == eLazyBoolYes &&
                m_supports_vCont_S == eLazyBoolYes)
            {
                m_supports_vCont_all = eLazyBoolYes;
            }
            
            if (m_supports_vCont_c == eLazyBoolYes ||
                m_supports_vCont_C == eLazyBoolYes ||
                m_supports_vCont_s == eLazyBoolYes ||
                m_supports_vCont_S == eLazyBoolYes)
            {
                m_supports_vCont_any = eLazyBoolYes;
            }
        }
    }
    
    switch (flavor)
    {
    case 'a': return m_supports_vCont_any;
    case 'A': return m_supports_vCont_all;
    case 'c': return m_supports_vCont_c;
    case 'C': return m_supports_vCont_C;
    case 's': return m_supports_vCont_s;
    case 'S': return m_supports_vCont_S;
    default: break;
    }
    return false;
}


size_t
GDBRemoteCommunicationClient::SendPacketAndWaitForResponse
(
    const char *payload,
    StringExtractorGDBRemote &response,
    bool send_async
)
{
    return SendPacketAndWaitForResponse (payload, 
                                         ::strlen (payload),
                                         response,
                                         send_async);
}

size_t
GDBRemoteCommunicationClient::SendPacketAndWaitForResponse
(
    const char *payload,
    size_t payload_length,
    StringExtractorGDBRemote &response,
    bool send_async
)
{
    Mutex::Locker locker;
    TimeValue timeout_time;
    timeout_time = TimeValue::Now();
    timeout_time.OffsetWithSeconds (m_packet_timeout);
    LogSP log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));

    if (GetSequenceMutex (locker))
    {
        if (SendPacketNoLock (payload, strlen(payload)))
            return WaitForPacketNoLock (response, &timeout_time);
    }
    else
    {
        if (send_async)
        {
            Mutex::Locker async_locker (m_async_mutex);
            m_async_packet.assign(payload, payload_length);
            m_async_packet_predicate.SetValue (true, eBroadcastNever);
            
            if (log) 
                log->Printf ("async: async packet = %s", m_async_packet.c_str());

            bool timed_out = false;
            bool sent_interrupt = false;
            if (SendInterrupt(locker, 2, sent_interrupt, timed_out))
            {
                if (sent_interrupt)
                {
                    if (log) 
                        log->Printf ("async: sent interrupt");
                    if (m_async_packet_predicate.WaitForValueEqualTo (false, &timeout_time, &timed_out))
                    {
                        if (log) 
                            log->Printf ("async: got response");
                        response = m_async_response;
                        return response.GetStringRef().size();
                    }
                    else
                    {
                        if (log) 
                            log->Printf ("async: timed out waiting for response");
                    }
                    
                    // Make sure we wait until the continue packet has been sent again...
                    if (m_private_is_running.WaitForValueEqualTo (true, &timeout_time, &timed_out))
                    {
                        if (log) 
                            log->Printf ("async: timed out waiting for process to resume");
                    }
                }
                else
                {
                    // We had a racy condition where we went to send the interrupt
                    // yet we were able to get the loc
                }
            }
            else
            {
                if (log) 
                    log->Printf ("async: failed to interrupt");
            }
        }
        else
        {
            if (log) 
                log->Printf ("mutex taken and send_async == false, aborting packet");
        }
    }
    return 0;
}

//template<typename _Tp>
//class ScopedValueChanger
//{
//public:
//    // Take a value reference and the value to assign it to when this class
//    // instance goes out of scope.
//    ScopedValueChanger (_Tp &value_ref, _Tp value) :
//        m_value_ref (value_ref),
//        m_value (value)
//    {
//    }
//
//    // This object is going out of scope, change the value pointed to by
//    // m_value_ref to the value we got during construction which was stored in
//    // m_value;
//    ~ScopedValueChanger ()
//    {
//        m_value_ref = m_value;
//    }
//protected:
//    _Tp &m_value_ref;   // A reference to the value we will change when this object destructs
//    _Tp m_value;        // The value to assign to m_value_ref when this goes out of scope.
//};

StateType
GDBRemoteCommunicationClient::SendContinuePacketAndWaitForResponse
(
    ProcessGDBRemote *process,
    const char *payload,
    size_t packet_length,
    StringExtractorGDBRemote &response
)
{
    LogSP log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("GDBRemoteCommunicationClient::%s ()", __FUNCTION__);

    Mutex::Locker locker(m_sequence_mutex);
    StateType state = eStateRunning;

    BroadcastEvent(eBroadcastBitRunPacketSent, NULL);
    m_public_is_running.SetValue (true, eBroadcastNever);
    // Set the starting continue packet into "continue_packet". This packet
    // make change if we are interrupted and we continue after an async packet...
    std::string continue_packet(payload, packet_length);
    
    while (state == eStateRunning)
    {
        if (log)
            log->Printf ("GDBRemoteCommunicationClient::%s () sending continue packet: %s", __FUNCTION__, continue_packet.c_str());
        if (SendPacket(continue_packet.c_str(), continue_packet.size()) == 0)
            state = eStateInvalid;
        
        m_private_is_running.SetValue (true, eBroadcastNever);

        if (log)
            log->Printf ("GDBRemoteCommunicationClient::%s () WaitForPacket(%.*s)", __FUNCTION__);

        if (WaitForPacket (response, (TimeValue*)NULL))
        {
            if (response.Empty())
                state = eStateInvalid;
            else
            {
                const char stop_type = response.GetChar();
                if (log)
                    log->Printf ("GDBRemoteCommunicationClient::%s () got packet: %s", __FUNCTION__, response.GetStringRef().c_str());
                switch (stop_type)
                {
                case 'T':
                case 'S':
                    if (process->GetStopID() == 0)
                    {
                        if (process->GetID() == LLDB_INVALID_PROCESS_ID)
                        {
                            lldb::pid_t pid = GetCurrentProcessID ();
                            if (pid != LLDB_INVALID_PROCESS_ID)
                                process->SetID (pid);
                        }
                        process->BuildDynamicRegisterInfo (true);
                    }

                    // Privately notify any internal threads that we have stopped
                    // in case we wanted to interrupt our process, yet we might
                    // send a packet and continue without returning control to the
                    // user.
                    m_private_is_running.SetValue (false, eBroadcastAlways);
                    if (m_async_signal != -1)
                    {
                        if (log) 
                            log->Printf ("async: send signo = %s", Host::GetSignalAsCString (m_async_signal));

                        // Save off the async signal we are supposed to send
                        const int async_signal = m_async_signal;
                        // Clear the async signal member so we don't end up
                        // sending the signal multiple times...
                        m_async_signal = -1;
                        // Check which signal we stopped with
                        uint8_t signo = response.GetHexU8(255);
                        if (signo == async_signal)
                        {
                            if (log) 
                                log->Printf ("async: stopped with signal %s, we are done running", Host::GetSignalAsCString (signo));

                            // We already stopped with a signal that we wanted
                            // to stop with, so we are done
                            response.SetFilePos (0);
                        }
                        else
                        {
                            // We stopped with a different signal that the one
                            // we wanted to stop with, so now we must resume
                            // with the signal we want
                            char signal_packet[32];
                            int signal_packet_len = 0;
                            signal_packet_len = ::snprintf (signal_packet,
                                                            sizeof (signal_packet),
                                                            "C%2.2x",
                                                            async_signal);

                            if (log) 
                                log->Printf ("async: stopped with signal %s, resume with %s", 
                                                   Host::GetSignalAsCString (signo),
                                                   Host::GetSignalAsCString (async_signal));

                            // Set the continue packet to resume...
                            continue_packet.assign(signal_packet, signal_packet_len);
                            continue;
                        }
                    }
                    else if (m_async_packet_predicate.GetValue())
                    {
                        // We are supposed to send an asynchronous packet while
                        // we are running. 
                        m_async_response.Clear();
                        if (m_async_packet.empty())
                        {
                            if (log) 
                                log->Printf ("async: error: empty async packet");                            

                        }
                        else
                        {
                            if (log) 
                                log->Printf ("async: sending packet: %s", 
                                             m_async_packet.c_str());
                            
                            SendPacketAndWaitForResponse (&m_async_packet[0], 
                                                          m_async_packet.size(),
                                                          m_async_response,
                                                          false);
                        }
                        // Let the other thread that was trying to send the async
                        // packet know that the packet has been sent and response is
                        // ready...
                        m_async_packet_predicate.SetValue(false, eBroadcastAlways);

                        // Set the continue packet to resume...
                        continue_packet.assign (1, 'c');
                        continue;
                    }
                    // Stop with signal and thread info
                    state = eStateStopped;
                    break;

                case 'W':
                case 'X':
                    // process exited
                    state = eStateExited;
                    break;

                case 'O':
                    // STDOUT
                    {
                        std::string inferior_stdout;
                        inferior_stdout.reserve(response.GetBytesLeft () / 2);
                        char ch;
                        while ((ch = response.GetHexU8()) != '\0')
                            inferior_stdout.append(1, ch);
                        process->AppendSTDOUT (inferior_stdout.c_str(), inferior_stdout.size());
                    }
                    break;

                case 'E':
                    // ERROR
                    state = eStateInvalid;
                    break;

                default:
                    if (log)
                        log->Printf ("GDBRemoteCommunicationClient::%s () unrecognized async packet", __FUNCTION__);
                    state = eStateInvalid;
                    break;
                }
            }
        }
        else
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationClient::%s () WaitForPacket(...) => false", __FUNCTION__);
            state = eStateInvalid;
        }
    }
    if (log)
        log->Printf ("GDBRemoteCommunicationClient::%s () => %s", __FUNCTION__, StateAsCString(state));
    response.SetFilePos(0);
    m_private_is_running.SetValue (false, eBroadcastAlways);
    m_public_is_running.SetValue (false, eBroadcastAlways);
    return state;
}

bool
GDBRemoteCommunicationClient::SendAsyncSignal (int signo)
{
    m_async_signal = signo;
    bool timed_out = false;
    bool sent_interrupt = false;
    Mutex::Locker locker;
    if (SendInterrupt (locker, 1, sent_interrupt, timed_out))
        return true;
    m_async_signal = -1;
    return false;
}

// This function takes a mutex locker as a parameter in case the GetSequenceMutex
// actually succeeds. If it doesn't succeed in acquiring the sequence mutex 
// (the expected result), then it will send the halt packet. If it does succeed
// then the caller that requested the interrupt will want to keep the sequence
// locked down so that no one else can send packets while the caller has control.
// This function usually gets called when we are running and need to stop the 
// target. It can also be used when we are running and and we need to do something
// else (like read/write memory), so we need to interrupt the running process
// (gdb remote protocol requires this), and do what we need to do, then resume.

bool
GDBRemoteCommunicationClient::SendInterrupt 
(
    Mutex::Locker& locker, 
    uint32_t seconds_to_wait_for_stop,             
    bool &sent_interrupt,
    bool &timed_out
)
{
    sent_interrupt = false;
    timed_out = false;
    LogSP log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));

    if (IsRunning())
    {
        // Only send an interrupt if our debugserver is running...
        if (GetSequenceMutex (locker) == false)
        {
            // Someone has the mutex locked waiting for a response or for the
            // inferior to stop, so send the interrupt on the down low...
            char ctrl_c = '\x03';
            ConnectionStatus status = eConnectionStatusSuccess;
            TimeValue timeout;
            if (seconds_to_wait_for_stop)
            {
                timeout = TimeValue::Now();
                timeout.OffsetWithSeconds (seconds_to_wait_for_stop);
            }
            size_t bytes_written = Write (&ctrl_c, 1, status, NULL);
            ProcessGDBRemoteLog::LogIf (GDBR_LOG_PACKETS | GDBR_LOG_PROCESS, "send packet: \\x03");
            if (bytes_written > 0)
            {
                sent_interrupt = true;
                if (seconds_to_wait_for_stop)
                {
                    if (m_private_is_running.WaitForValueEqualTo (false, &timeout, &timed_out))
                    {
                        if (log)
                            log->Printf ("GDBRemoteCommunicationClient::%s () - sent interrupt, private state stopped", __FUNCTION__);
                        return true;
                    }
                    else
                    {
                        if (log)
                            log->Printf ("GDBRemoteCommunicationClient::%s () - sent interrupt, timed out wating for async thread resume", __FUNCTION__);
                    }
                }
                else
                {
                    if (log)
                        log->Printf ("GDBRemoteCommunicationClient::%s () - sent interrupt, not waiting for stop...", __FUNCTION__);                    
                    return true;
                }
            }
            else
            {
                if (log)
                    log->Printf ("GDBRemoteCommunicationClient::%s () - failed to write interrupt", __FUNCTION__);
            }
            return false;
        }
        else
        {
            if (log)
                log->Printf ("GDBRemoteCommunicationClient::%s () - got sequence mutex without having to interrupt", __FUNCTION__);
        }
    }
    return true;
}

lldb::pid_t
GDBRemoteCommunicationClient::GetCurrentProcessID ()
{
    StringExtractorGDBRemote response;
    if (SendPacketAndWaitForResponse("qC", strlen("qC"), response, false))
    {
        if (response.GetChar() == 'Q')
            if (response.GetChar() == 'C')
                return response.GetHexMaxU32 (false, LLDB_INVALID_PROCESS_ID);
    }
    return LLDB_INVALID_PROCESS_ID;
}

bool
GDBRemoteCommunicationClient::GetLaunchSuccess (std::string &error_str)
{
    error_str.clear();
    StringExtractorGDBRemote response;
    if (SendPacketAndWaitForResponse("qLaunchSuccess", strlen("qLaunchSuccess"), response, false))
    {
        if (response.IsOKResponse())
            return true;
        if (response.GetChar() == 'E')
        {
            // A string the describes what failed when launching...
            error_str = response.GetStringRef().substr(1);
        }
        else
        {
            error_str.assign ("unknown error occurred launching process");
        }
    }
    else
    {
        error_str.assign ("failed to send the qLaunchSuccess packet");
    }
    return false;
}

int
GDBRemoteCommunicationClient::SendArgumentsPacket (char const *argv[])
{
    if (argv && argv[0])
    {
        StreamString packet;
        packet.PutChar('A');
        const char *arg;
        for (uint32_t i = 0; (arg = argv[i]) != NULL; ++i)
        {
            const int arg_len = strlen(arg);
            if (i > 0)
                packet.PutChar(',');
            packet.Printf("%i,%i,", arg_len * 2, i);
            packet.PutBytesAsRawHex8 (arg, arg_len);
        }

        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

int
GDBRemoteCommunicationClient::SendEnvironmentPacket (char const *name_equal_value)
{
    if (name_equal_value && name_equal_value[0])
    {
        StreamString packet;
        packet.Printf("QEnvironment:%s", name_equal_value);
        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

bool
GDBRemoteCommunicationClient::GetOSVersion (uint32_t &major, 
                                            uint32_t &minor, 
                                            uint32_t &update)
{
    if (GetHostInfo ())
    {
        if (m_os_version_major != UINT32_MAX)
        {
            major = m_os_version_major;
            minor = m_os_version_minor;
            update = m_os_version_update;
            return true;
        }
    }
    return false;
}

bool
GDBRemoteCommunicationClient::GetOSBuildString (std::string &s)
{
    if (GetHostInfo ())
    {
        if (!m_os_build.empty())
        {
            s = m_os_build;
            return true;
        }
    }
    s.clear();
    return false;
}


bool
GDBRemoteCommunicationClient::GetOSKernelDescription (std::string &s)
{
    if (GetHostInfo ())
    {
        if (!m_os_kernel.empty())
        {
            s = m_os_kernel;
            return true;
        }
    }
    s.clear();
    return false;
}

bool
GDBRemoteCommunicationClient::GetHostname (std::string &s)
{
    if (GetHostInfo ())
    {
        if (!m_hostname.empty())
        {
            s = m_hostname;
            return true;
        }
    }
    s.clear();
    return false;
}

ArchSpec
GDBRemoteCommunicationClient::GetSystemArchitecture ()
{
    if (GetHostInfo ())
        return m_host_arch;
    return ArchSpec();
}


bool
GDBRemoteCommunicationClient::GetHostInfo ()
{
    if (m_supports_qHostInfo == eLazyBoolCalculate)
    {
        m_supports_qHostInfo = eLazyBoolNo;

        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse ("qHostInfo", response, false))
        {
            if (response.IsUnsupportedResponse())
                return false;

            m_supports_qHostInfo = eLazyBoolYes;

            std::string name;
            std::string value;
            uint32_t cpu = LLDB_INVALID_CPUTYPE;
            uint32_t sub = 0;
            std::string arch_name;
            std::string os_name;
            std::string vendor_name;
            std::string triple;
            uint32_t pointer_byte_size = 0;
            StringExtractor extractor;
            ByteOrder byte_order = eByteOrderInvalid;
            while (response.GetNameColonValue(name, value))
            {
                if (name.compare("cputype") == 0)
                {
                    // exception type in big endian hex
                    cpu = Args::StringToUInt32 (value.c_str(), LLDB_INVALID_CPUTYPE, 0);
                }
                else if (name.compare("cpusubtype") == 0)
                {
                    // exception count in big endian hex
                    sub = Args::StringToUInt32 (value.c_str(), 0, 0);
                }
                else if (name.compare("arch") == 0)
                {
                    arch_name.swap (value);
                }
                else if (name.compare("triple") == 0)
                {
                    // The triple comes as ASCII hex bytes since it contains '-' chars
                    extractor.GetStringRef().swap(value);
                    extractor.SetFilePos(0);
                    extractor.GetHexByteString (triple);
                }
                else if (name.compare("os_build") == 0)
                {
                    extractor.GetStringRef().swap(value);
                    extractor.SetFilePos(0);
                    extractor.GetHexByteString (m_os_build);
                }
                else if (name.compare("hostname") == 0)
                {
                    extractor.GetStringRef().swap(value);
                    extractor.SetFilePos(0);
                    extractor.GetHexByteString (m_hostname);
                }
                else if (name.compare("os_kernel") == 0)
                {
                    extractor.GetStringRef().swap(value);
                    extractor.SetFilePos(0);
                    extractor.GetHexByteString (m_os_kernel);
                }
                else if (name.compare("ostype") == 0)
                {
                    os_name.swap (value);
                }
                else if (name.compare("vendor") == 0)
                {
                    vendor_name.swap(value);
                }
                else if (name.compare("endian") == 0)
                {
                    if (value.compare("little") == 0)
                        byte_order = eByteOrderLittle;
                    else if (value.compare("big") == 0)
                        byte_order = eByteOrderBig;
                    else if (value.compare("pdp") == 0)
                        byte_order = eByteOrderPDP;
                }
                else if (name.compare("ptrsize") == 0)
                {
                    pointer_byte_size = Args::StringToUInt32 (value.c_str(), 0, 0);
                }
                else if (name.compare("os_version") == 0)
                {
                    Args::StringToVersion (value.c_str(), 
                                           m_os_version_major,
                                           m_os_version_minor,
                                           m_os_version_update);
                }
            }
            
            if (triple.empty())
            {
                if (arch_name.empty())
                {
                    if (cpu != LLDB_INVALID_CPUTYPE)
                    {
                        m_host_arch.SetArchitecture (lldb::eArchTypeMachO, cpu, sub);
                        if (pointer_byte_size)
                        {
                            assert (pointer_byte_size == m_host_arch.GetAddressByteSize());
                        }
                        if (byte_order != eByteOrderInvalid)
                        {
                            assert (byte_order == m_host_arch.GetByteOrder());
                        }
                        if (!vendor_name.empty())
                            m_host_arch.GetTriple().setVendorName (llvm::StringRef (vendor_name));
                        if (!os_name.empty())
                            m_host_arch.GetTriple().setVendorName (llvm::StringRef (os_name));
                            
                    }
                }
                else
                {
                    std::string triple;
                    triple += arch_name;
                    triple += '-';
                    if (vendor_name.empty())
                        triple += "unknown";
                    else
                        triple += vendor_name;
                    triple += '-';
                    if (os_name.empty())
                        triple += "unknown";
                    else
                        triple += os_name;
                    m_host_arch.SetTriple (triple.c_str());
                    if (pointer_byte_size)
                    {
                        assert (pointer_byte_size == m_host_arch.GetAddressByteSize());
                    }
                    if (byte_order != eByteOrderInvalid)
                    {
                        assert (byte_order == m_host_arch.GetByteOrder());
                    }
                    
                }
            }
            else
            {
                m_host_arch.SetTriple (triple.c_str());
                if (pointer_byte_size)
                {
                    assert (pointer_byte_size == m_host_arch.GetAddressByteSize());
                }
                if (byte_order != eByteOrderInvalid)
                {
                    assert (byte_order == m_host_arch.GetByteOrder());
                }
            }            
        }
    }
    return m_supports_qHostInfo == eLazyBoolYes;
}

int
GDBRemoteCommunicationClient::SendAttach 
(
    lldb::pid_t pid, 
    StringExtractorGDBRemote& response
)
{
    if (pid != LLDB_INVALID_PROCESS_ID)
    {
        StreamString packet;
        packet.Printf("vAttach;%x", pid);
        
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsErrorResponse())
                return response.GetError();
            return 0;
        }
    }
    return -1;
}

const lldb_private::ArchSpec &
GDBRemoteCommunicationClient::GetHostArchitecture ()
{
    if (m_supports_qHostInfo == lldb::eLazyBoolCalculate)
        GetHostInfo ();
    return m_host_arch;
}

addr_t
GDBRemoteCommunicationClient::AllocateMemory (size_t size, uint32_t permissions)
{
    char packet[64];
    ::snprintf (packet, sizeof(packet), "_M%zx,%s%s%s", size,
                permissions & lldb::ePermissionsReadable ? "r" : "",
                permissions & lldb::ePermissionsWritable ? "w" : "",
                permissions & lldb::ePermissionsExecutable ? "x" : "");
    StringExtractorGDBRemote response;
    if (SendPacketAndWaitForResponse (packet, response, false))
    {
        if (!response.IsErrorResponse())
            return response.GetHexMaxU64(false, LLDB_INVALID_ADDRESS);
    }
    return LLDB_INVALID_ADDRESS;
}

bool
GDBRemoteCommunicationClient::DeallocateMemory (addr_t addr)
{
    char packet[64];
    snprintf(packet, sizeof(packet), "_m%llx", (uint64_t)addr);
    StringExtractorGDBRemote response;
    if (SendPacketAndWaitForResponse (packet, response, false))
    {
        if (response.IsOKResponse())
            return true;
    }
    return false;
}

int
GDBRemoteCommunicationClient::SetSTDIN (char const *path)
{
    if (path && path[0])
    {
        StreamString packet;
        packet.PutCString("QSetSTDIN:");
        packet.PutBytesAsRawHex8(path, strlen(path));

        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

int
GDBRemoteCommunicationClient::SetSTDOUT (char const *path)
{
    if (path && path[0])
    {
        StreamString packet;
        packet.PutCString("QSetSTDOUT:");
        packet.PutBytesAsRawHex8(path, strlen(path));
        
        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

int
GDBRemoteCommunicationClient::SetSTDERR (char const *path)
{
    if (path && path[0])
    {
        StreamString packet;
        packet.PutCString("QSetSTDERR:");
        packet.PutBytesAsRawHex8(path, strlen(path));
        
        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

int
GDBRemoteCommunicationClient::SetWorkingDir (char const *path)
{
    if (path && path[0])
    {
        StreamString packet;
        packet.PutCString("QSetWorkingDir:");
        packet.PutBytesAsRawHex8(path, strlen(path));
        
        StringExtractorGDBRemote response;
        if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
        {
            if (response.IsOKResponse())
                return 0;
            uint8_t error = response.GetError();
            if (error)
                return error;
        }
    }
    return -1;
}

int
GDBRemoteCommunicationClient::SetDisableASLR (bool enable)
{
    StreamString packet;
    packet.Printf("QSetDisableASLR:%i", enable ? 1 : 0);
       
    StringExtractorGDBRemote response;
    if (SendPacketAndWaitForResponse (packet.GetData(), packet.GetSize(), response, false))
    {
        if (response.IsOKResponse())
            return 0;
        uint8_t error = response.GetError();
        if (error)
            return error;
    }
    return -1;
}
