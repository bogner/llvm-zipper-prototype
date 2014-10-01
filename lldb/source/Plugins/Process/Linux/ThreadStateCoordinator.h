//===-- ThreadStateCoordinator.h --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef lldb_ThreadStateCoordinator_h
#define lldb_ThreadStateCoordinator_h

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "lldb/lldb-types.h"

namespace lldb_private
{
    class ThreadStateCoordinator
    {
    public:

        // Typedefs.
        typedef std::unordered_set<lldb::tid_t> ThreadIDSet;

        // Protocols.


        // Callback/block definitions.
        typedef std::function<void (lldb::tid_t tid)> ThreadIDFunction;
        typedef std::function<void (const char *format, va_list args)> LogFunction;

        // constructors
        ThreadStateCoordinator (const LogFunction &log_function);

        // The main purpose of the class: triggering an action after
        // a given set of threads stop.
        void
        CallAfterThreadsStop (lldb::tid_t triggering_tid,
                              const ThreadIDSet &wait_for_stop_tids,
                              const ThreadIDFunction &request_thread_stop_function,
                              const ThreadIDFunction &call_after_function);

        // Notifications called when various state changes occur.
        void
        NotifyThreadStop (lldb::tid_t tid);

        void
        RequestThreadResume (lldb::tid_t tid, const ThreadIDFunction &request_thread_resume_func);

        void
        NotifyThreadCreate (lldb::tid_t tid);

        void
        NotifyThreadDeath (lldb::tid_t tid);

        // Indicate the calling process did an exec and that the thread state
        // should be 100% cleared.
        //
        // Note this will clear out any pending notifications, but will not stop
        // a notification currently in progress via ProcessNextEvent().
        void
        ResetForExec ();

        // Indicate when the coordinator should shut down.
        void
        StopCoordinator ();

        // Process the next event, returning false when the coordinator is all done.
        // This call is synchronous and blocks when there are no events pending.
        // Expected usage is to run this in a separate thread until the function
        // returns false.  Always call this from the same thread.  The processing
        // logic assumes the execution of this is implicitly serialized.
        bool
        ProcessNextEvent ();

    private:

        // Typedefs.
        class EventBase;

        class EventCallAfterThreadsStop;
        class EventThreadStopped;
        class EventThreadCreate;
        class EventThreadDeath;
        class EventRequestResume;

        class EventStopCoordinator;
        class EventReset;

        typedef std::shared_ptr<EventBase> EventBaseSP;

        typedef std::queue<EventBaseSP> QueueType;

        typedef std::unordered_map<lldb::tid_t, bool> TIDBoolMap;


        // Private member functions.
        void
        EnqueueEvent (EventBaseSP event_sp);

        EventBaseSP
        DequeueEventWithWait ();

        void
        SetPendingNotification (const EventBaseSP &event_sp);

        void
        ThreadDidStop (lldb::tid_t tid);

        void
        ThreadWasCreated (lldb::tid_t tid);

        void
        ThreadDidDie (lldb::tid_t tid);

        void
        ResetNow ();

        void
        Log (const char *format, ...);

        EventCallAfterThreadsStop *
        GetPendingThreadStopNotification ();

        // Member variables.
        LogFunction m_log_function;

        QueueType m_event_queue;
        // For now we do simple read/write lock strategy with efficient wait-for-data.
        // We can replace with an entirely non-blocking queue later but we still want the
        // reader to sleep when nothing is available - this will be a bursty but infrequent
        // event mechanism.
        std::condition_variable m_queue_condition;
        std::mutex m_queue_mutex;

        EventBaseSP m_pending_notification_sp;

        // Maps known TIDs to stop (true) or not-stopped (false) state.
        TIDBoolMap m_tid_stop_map;
    };
}

#endif
