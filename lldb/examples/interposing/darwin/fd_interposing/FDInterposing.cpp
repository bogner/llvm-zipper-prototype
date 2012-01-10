//===-- FDInterposing.cpp ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file helps with catching double close calls on unix integer file 
// descriptors by interposing functions for all file descriptor create and 
// close operations. A stack backtrace for every create and close function is
// maintained, and every create and close operation is logged. When a double
// file descriptor close is encountered, it will be logged.
//
// To enable the interposing in a darwin program, set the DYLD_INSERT_LIBRARIES
// environment variable as follows:
// For sh:
//  DYLD_INSERT_LIBRARIES=/path/to/FDInterposing.dylib /path/to/executable
// For tcsh:
//  (setenv DYLD_INSERT_LIBRARIES=/path/to/FDInterposing.dylib ; /path/to/executable)
//
// Other environment variables that can alter the default actions of this
// interposing shared library include:
//
// "FileDescriptorStackLoggingNoCompact"
//
//      With this environment variable set, all file descriptor create and 
//      delete operations will be permanantly maintained in the event map. 
//      The default action is to compact the create/delete events by removing
//      any previous file descriptor create events that are matched with a 
//      corresponding file descriptor delete event when the next valid file
//      descriptor create event is detected.
// 
// "FileDescriptorMinimalLogging"
//
//      By default every file descriptor create and delete operation is logged
//      (to STDOUT by default, see the "FileDescriptorLogFile"). This can be
//      suppressed to only show errors and warnings by setting this environment
//      variable (the value in not important).
//
// "FileDescriptorLogFile=<path>"
//
//      By default logging goes to STDOUT_FILENO, but this can be changed by
//      setting FileDescriptorLogFile. The value is a path to a file that
//      will be opened and used for logging.
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <execinfo.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld-interposing.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <tr1/memory> // for std::tr1::shared_ptr
#include <unistd.h>
#include <string>
#include <vector>
#include <map>

//----------------------------------------------------------------------
/// @def DISALLOW_COPY_AND_ASSIGN(TypeName)
///     Macro definition for easily disallowing copy constructor and
///     assignment operators in C++ classes.
//----------------------------------------------------------------------
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
TypeName(const TypeName&); \
const TypeName& operator=(const TypeName&)

namespace fd_interposing {

//----------------------------------------------------------------------
// String class so we can get formatted strings without having to worry
// about the memory storage since it will allocate the memory it needs.
//----------------------------------------------------------------------
class String
{
public:
    String () : 
        m_str (NULL) 
    {}

    String (const char *format, ...) : 
        m_str (NULL)
    {
        va_list args;
        va_start (args, format);
        vprintf (format, args);
        va_end (args);
    }
    
    ~String()
    {
        reset();
    }
    
    void
    reset (char *s = NULL)
    {
        if (m_str)
            ::free (m_str);
        m_str = s;
    }
    
    const char *
    c_str () const
    {
        return m_str;
    }

    void
    printf (const char *format, ...)
    {
        va_list args;
        va_start (args, format);
        vprintf (format, args);
        va_end (args);        
    }
    void
    vprintf (const char *format, va_list args)
    {
        reset();
        ::vasprintf (&m_str, format, args);
    }

    void
    log (int log_fd)
    {
        if (m_str && log_fd >= 0)
        {
            const int len = strlen(m_str);
            if (len > 0)
            {
                write (log_fd, m_str, len);
                const char last_char = m_str[len-1];
                if (!(last_char == '\n' || last_char == '\r'))
                    write (log_fd, "\n", 1);
            }
        }
    }
protected:
    char *m_str;
    
private:
    DISALLOW_COPY_AND_ASSIGN (String);
};

//----------------------------------------------------------------------
// Type definitions
//----------------------------------------------------------------------
typedef std::vector<void *> Frames;
class FDEvent;
typedef std::vector<void *> Frames;
typedef std::tr1::shared_ptr<FDEvent> FDEventSP;
typedef std::tr1::shared_ptr<String> StringSP;


//----------------------------------------------------------------------
// FDEvent
//
// A class that describes a file desciptor event.
//
// File descriptor events fall into one of two categories: create events
// and delete events.
//----------------------------------------------------------------------
class FDEvent
{
public:
    FDEvent (int fd, int err, const StringSP &string_sp, bool is_create, const Frames& frames) :
        m_string_sp (string_sp),
        m_frames (frames.begin(), frames.end()),
        m_fd (fd),
        m_err (err),
        m_is_create (is_create)
    {}
    
    ~FDEvent () {}
    
    bool
    IsCreateEvent() const
    {
        return m_is_create;
    }

    bool
    IsDeleteEvent() const
    {
        return !m_is_create;
    }

    Frames &
    GetFrames ()
    {
        return m_frames;
    }

    const Frames &
    GetFrames () const
    {
        return m_frames;
    }
    
    int
    GetFD () const
    {
        return m_fd;
    }

    int
    GetError () const
    {
        return m_err;
    }
    
    void
    Dump (int log_fd) const;

    void
    SetCreateEvent (FDEventSP &create_event_sp)
    {
        m_create_event_sp = create_event_sp;
    }

private:
    // A shared pointer to a String that describes this event in 
    // detail (all args and return and error values)
    StringSP m_string_sp;
    // The frames for the stack backtrace for this event
    Frames m_frames;
    // If this is a file descriptor delete event, this might contain 
    // the correspoding file descriptor create event
    FDEventSP m_create_event_sp;    
    // The file descriptor for this event
    int m_fd;
    // The error code (if any) for this event
    int m_err;
    // True if this event is a file descriptor create event, false 
    // if it is a file descriptor delete event
    bool m_is_create;
};

//----------------------------------------------------------------------
// Templatized class that will save errno only if the "value" it is 
// constructed with is equal to INVALID. When the class goes out of 
// scope, it will restore errno if it was saved.
//----------------------------------------------------------------------
template <int INVALID>
class Errno
{
public:
    // Save errno only if we are supposed to
    Errno (int value) :
        m_saved_errno ((value == INVALID) ? errno : 0),
        m_restore (value == INVALID)
    {
    }
    
    // Restore errno only if we are supposed to
    ~Errno()
    {
        if (m_restore)
            errno = m_saved_errno;
    }

    // Accessor for the saved value of errno
    int
    get_errno() const
    {
        return m_saved_errno;
    }

protected:
    const int m_saved_errno;
    const bool m_restore;
};

typedef Errno<-1> InvalidFDErrno;
typedef Errno<-1> NegativeErrorErrno;
typedef std::vector<FDEventSP> FDEventArray;
typedef std::map<int, FDEventArray> FDEventMap;

//----------------------------------------------------------------------
// Globals
//----------------------------------------------------------------------
// Global event map that contains all file descriptor events. As file
// descriptor create and close events come in, they will get filled
// into this map (protected by g_mutex). When a file descriptor close
// event is detected, the open event will be removed and placed into
// the close event so if something tries to double close a file 
// descriptor we can show the previous close event and the file 
// desctiptor event that created it. When a new file descriptor create
// event comes in, we will remove the previous one for that file 
// desctiptor unless the environment variable "FileDescriptorStackLoggingNoCompact"
// is set. The file desctiptor history can be accessed using the 
// get_fd_history() function.
static FDEventMap g_fd_event_map;
// A mutex to protect access to our data structures in g_fd_event_map
// and also our logging messages
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
// Log all file descriptor create and close events by default. Only log
// warnings and erros if the "FileDescriptorMinimalLogging" environment
// variable is set.
static int g_log_all_calls = 1;
// We compact the file descriptor events by default. Set the environment
// varible "FileDescriptorStackLoggingNoCompact" to keep a full history.
static int g_compact = 1;

//----------------------------------------------------------------------
// Mutex class that will lock a mutex when it is constructed, and unlock
// it when is goes out of scope
//----------------------------------------------------------------------
class Locker
{
public:
    Locker (pthread_mutex_t *mutex_ptr) :
        m_mutex_ptr(mutex_ptr)
    {
        ::pthread_mutex_lock (m_mutex_ptr);
    }

    // This allows clients to test try and acquire the mutex...
    Locker (pthread_mutex_t *mutex_ptr, bool &lock_acquired) :
        m_mutex_ptr(NULL)
    {
        lock_acquired = ::pthread_mutex_trylock(mutex_ptr) == 0;
        if (lock_acquired)
            m_mutex_ptr = mutex_ptr;
    }

    ~Locker ()
    {
        if (m_mutex_ptr)
            ::pthread_mutex_unlock (m_mutex_ptr);
    }
protected:
    pthread_mutex_t *m_mutex_ptr;
};

static void
log (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static void
log (int log_fd, const FDEvent *event, const char *format, ...) __attribute__ ((format (printf, 3, 4)));

static void
backtrace_log (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static void
log_to_fd (int log_fd, const char *format, ...) __attribute__ ((format (printf, 2, 3)));

static inline size_t
get_backtrace (Frames &frame_buffer, size_t frames_to_remove)
{
    void *frames[2048];
    int count = ::backtrace (&frames[0], sizeof(frames)/sizeof(void*));
    if (count > frames_to_remove)
        frame_buffer.assign (&frames[frames_to_remove], &frames[count]);
    else
        frame_buffer.assign (&frames[0], &frames[count]);
    while (frame_buffer.back() < (void *)1024)
        frame_buffer.pop_back();
    return frame_buffer.size();
}

static int
get_logging_fd ()
{
    static int g_log_fd = STDOUT_FILENO;
    static char program_fullpath[PATH_MAX];
    static int initialized = 0;
    
    const int pid = getpid();
    
    if (!initialized) 
    {
        initialized = 1;

        // Keep all stack info around for all fd create and delete calls.
        // Otherwise we will remove the fd create call when a corresponding
        // fd delete call is received
        if (getenv("FileDescriptorStackLoggingNoCompact"))
            g_compact = 0;

        if (getenv("FileDescriptorMinimalLogging"))
            g_log_all_calls = 0;

        char program_basename[PATH_MAX];
        // If DST is NULL, then return the number of bytes needed.
        uint32_t len = sizeof(program_fullpath);
        if (_NSGetExecutablePath (program_fullpath, &len) == 0)
        {
            strncpy (program_basename, program_fullpath, sizeof(program_basename));
            const char *program_basename_cstr = basename(program_basename);
            if (program_basename_cstr)
            {
                // Only let this interposing happen on the first time this matches
                // and stop this from happening so any child processes don't also
                // log their file descriptors
                ::unsetenv ("DYLD_INSERT_LIBRARIES");
                const char *log_path = getenv ("FileDescriptorLogFile");
                if (log_path)
                    g_log_fd = ::creat (log_path, 0660);
                if (g_log_fd >= 0)
                    log ("Logging file descriptor functions process '%s' (pid = %i)\n", program_fullpath, pid);
            }
        }
    }
    return g_log_fd;
}

void
log_to_fd (int log_fd, const char *format, va_list args)
{
    if (format && format[0] && log_fd >= 0)
    {
        char buffer[PATH_MAX];
        const int count = ::vsnprintf (buffer, sizeof(buffer), format, args);
        if (count > 0)
            write (log_fd, buffer, count);
    }
}

void
log_to_fd (int log_fd, const char *format, ...)
{
    if (format && format[0])
    {
        va_list args;
        va_start (args, format);
        log_to_fd (log_fd, format, args);
        va_end (args);
    }
}

void
log (const char *format, va_list args)
{
    log_to_fd (get_logging_fd (), format, args);
}

void
log (const char *format, ...)
{
    if (format && format[0])
    {
        va_list args;
        va_start (args, format);
        log (format, args);
        va_end (args);
    }
}

void
log (int log_fd, const FDEvent *event, const char *format, ...)
{
    if (format && format[0])
    {
        va_list args;
        va_start (args, format);
        log_to_fd (log_fd, format, args);
        va_end (args);
    }
    if (event)
        event->Dump(log_fd);
}

void
FDEvent::Dump (int log_fd) const
{
    if (log_fd >= 0)
    {
        log_to_fd (log_fd, "%s\n", m_string_sp->c_str());
        if (!m_frames.empty())
            ::backtrace_symbols_fd (m_frames.data(), m_frames.size(), log_fd);
        
        if (m_create_event_sp)
        {
            log_to_fd (log_fd, "\nfd=%i was created with this event:\n", m_fd);
            m_create_event_sp->Dump (log_fd);
            log_to_fd (log_fd, "\n");
        }
    }
}


void
backtrace_log (const char *format, ...)
{
    const int log_fd = get_logging_fd ();
    if (log_fd >= 0)
    {
        if (format && format[0])
        {
            va_list args;
            va_start (args, format);
            log (format, args);
            va_end (args);
        }
        
        Frames frames;
        if (get_backtrace(frames, 3))
            ::backtrace_symbols_fd (frames.data(), frames.size(), log_fd);
    }

}

void
save_backtrace (int fd, int err, const StringSP &string_sp, bool is_create);

void
save_backtrace (int fd, int err, const StringSP &string_sp, bool is_create)
{
    Frames frames;
    get_backtrace(frames, 2);

    FDEventSP fd_event_sp (new FDEvent (fd, err, string_sp, is_create, frames));

    FDEventMap::iterator pos = g_fd_event_map.find (fd);
    
    if (pos != g_fd_event_map.end())
    {
        // We have history for this fd...
        
        FDEventArray &event_array = g_fd_event_map[fd];
        if (fd_event_sp->IsCreateEvent())
        {
            // The current fd event is a function that creates 
            // a descriptor, check in case last event was
            // a create event.
            if (event_array.back()->IsCreateEvent())
            {
                const int log_fd = get_logging_fd();
                // Two fd create functions in a row, we missed
                // a function that closes a fd...
                log (log_fd, fd_event_sp.get(), "\nwarning: unmatched file descriptor create event fd=%i (we missed a file descriptor close event):\n", fd);
            }
            else if (g_compact)
            {
                // We are compacting so we remove previous create event
                // when we get the correspinding delete event
                event_array.pop_back();
            }
        }
        else
        {
            // The current fd event is a function that deletes 
            // a descriptor, check in case last event for this
            // fd was a delete event (double close!)
            if (event_array.back()->IsDeleteEvent())
            {
                const int log_fd = get_logging_fd();
                // Two fd delete functions in a row, we must 
                // have missed some function that opened a descriptor
                log (log_fd, fd_event_sp.get(), "\nwarning: unmatched file descriptor close event for fd=%d (we missed the file descriptor create event):\n", fd);
            }
            else if (g_compact)
            {
                // Since this is a close event, we want to remember the open event 
                // that this close if for...
                fd_event_sp->SetCreateEvent(event_array.back());
                // We are compacting so we remove previous create event
                // when we get the correspinding delete event
                event_array.pop_back();
            }
        }
        
        event_array.push_back(fd_event_sp);
    }
    else
    {
        g_fd_event_map[fd].push_back(fd_event_sp);
    }
}

//----------------------------------------------------------------------
// socket() interpose function
//----------------------------------------------------------------------
extern "C" int
socket$__interposed__ (int domain, int type, int protocol)
{
    Locker locker (&g_mutex);
    const int fd = ::socket (domain, type, protocol);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String);
    if (fd == -1)
        description_sp->printf("socket (domain = %i, type = %i, protocol = %i) => fd=%i  errno = %i", domain, type, protocol, fd, fd_errno.get_errno());
    else
        description_sp->printf("socket (domain = %i, type = %i, protocol = %i) => fd=%i", domain, type, protocol, fd);
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// socketpair() interpose function
//----------------------------------------------------------------------
extern "C" int
socketpair$__interposed__ (int domain, int type, int protocol, int fds[2])
{
    Locker locker (&g_mutex);
    fds[0] = -1;
    fds[1] = -1;
    const int err = socketpair (domain, type, protocol, fds);
    NegativeErrorErrno err_errno(err);
    StringSP description_sp(new String ("socketpair (domain=%i, type=%i, protocol=%i, {fd=%i, fd=%i}) -> err=%i", domain, type, protocol, fds[0], fds[1], err));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fds[0] >= 0)
        save_backtrace (fds[0], err_errno.get_errno(), description_sp, true);
    if (fds[1] >= 0)
        save_backtrace (fds[1], err_errno.get_errno(), description_sp, true);
    return err;
}

//----------------------------------------------------------------------
// open() interpose function
//----------------------------------------------------------------------
extern "C" int	
open$__interposed__ (const char *path, int oflag, int mode)
{
    Locker locker (&g_mutex);
    int fd = -2;
    StringSP description_sp(new String);
    if (oflag & O_CREAT)
    {
        fd = open (path, oflag, mode);
        description_sp->printf("open (path = '%s', oflag = %i, mode = %i) -> fd=%i", path, oflag, mode, fd);
    }
    else
    {
        fd = open (path, oflag);
        description_sp->printf("open (path = '%s', oflag = %i) -> fd=%i", path, oflag, fd);
    }
    
    InvalidFDErrno fd_errno(fd);
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// open$NOCANCEL() interpose function
//----------------------------------------------------------------------
extern "C" int open$NOCANCEL(const char *, int, ...);
extern "C" int __open_nocancel(const char *, int, ...);
extern "C" int	
open$NOCANCEL$__interposed__ (const char *path, int oflag, int mode)
{
    Locker locker (&g_mutex);
    const int fd = open$NOCANCEL (path, oflag, mode);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("open$NOCANCEL (path = '%s', oflag = %i, mode = %i) -> fd=%i", path, oflag, mode, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

extern "C" int __open_extended(const char *, int, uid_t, gid_t, int, struct kauth_filesec *);

//----------------------------------------------------------------------
// __open_extended() interpose function
//----------------------------------------------------------------------
extern "C" int 
__open_extended$__interposed__ (const char *path, int oflag, uid_t uid, gid_t gid, int mode, struct kauth_filesec *fsacl)
{
    Locker locker (&g_mutex);
    const int fd = __open_extended (path, oflag, uid, gid, mode, fsacl);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("__open_extended (path='%s', oflag=%i, uid=%i, gid=%i, mode=%i, fsacl=%p) -> fd=%i", path, oflag, uid, gid, mode, fsacl, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// kqueue() interpose function
//----------------------------------------------------------------------
extern "C" int
kqueue$__interposed__ (void)
{
    Locker locker (&g_mutex);
    const int fd = ::kqueue ();
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("kqueue () -> fd=%i", fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
    
}

//----------------------------------------------------------------------
// shm_open() interpose function
//----------------------------------------------------------------------
extern "C" int	
shm_open$__interposed__ (const char *path, int oflag, int mode)
{
    Locker locker (&g_mutex);
    const int fd = shm_open (path, oflag, mode);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("shm_open (path = '%s', oflag = %i, mode = %i) -> fd=%i", path, oflag, mode, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// accept() interpose function
//----------------------------------------------------------------------
extern "C" int
accept$__interposed__ (int socket, struct sockaddr *address, socklen_t *address_len)
{
    Locker locker (&g_mutex);
    const int fd = accept (socket, address, address_len);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("accept (socket=%i, ...) -> fd=%i", socket, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}


//----------------------------------------------------------------------
// accept$NOCANCEL() interpose function
//----------------------------------------------------------------------
extern "C" int accept$NOCANCEL (int, struct sockaddr * __restrict, socklen_t * __restrict);
extern "C" int
accept$NOCANCEL$__interposed__ (int socket, struct sockaddr *address, socklen_t *address_len)
{
    Locker locker (&g_mutex);
    const int fd = accept$NOCANCEL (socket, address, address_len);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("accept$NOCANCEL (socket=%i, ...) -> fd=%i", socket, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// dup() interpose function
//----------------------------------------------------------------------
extern "C" int	
dup$__interposed__ (int fd2)
{
    Locker locker (&g_mutex);
    const int fd = dup (fd2);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("dup (fd2=%i) -> fd=%i", fd2, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// dup2() interpose function
//----------------------------------------------------------------------
extern "C" int	
dup2$__interposed__ (int fd1, int fd2)
{
    Locker locker (&g_mutex);

    const int fd = dup2(fd1, fd2);
    InvalidFDErrno fd_errno(fd);
    StringSP description_sp(new String ("dup2 fd1=%i, fd2=%i) -> fd=%i", fd1, fd2, fd));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    // If "fd2" is already opened, it will be closed during the
    // dup2 call below, so we need to see if we have fd2 in our
    // open map and treat it as a close(fd2)
    FDEventMap::iterator pos = g_fd_event_map.find (fd2);
    if (pos != g_fd_event_map.end() && pos->second.back()->IsCreateEvent())
        save_backtrace (fd2, 0, description_sp, false);

    if (fd >= 0)
        save_backtrace (fd, fd_errno.get_errno(), description_sp, true);
    return fd;
}

//----------------------------------------------------------------------
// close() interpose function
//----------------------------------------------------------------------
extern "C" int 
close$__interposed__ (int fd) 
{
    Locker locker (&g_mutex);
    const int err = close(fd);
    NegativeErrorErrno err_errno(err);
    StringSP description_sp (new String);
    if (err == -1)
        description_sp->printf("close (fd=%i) => %i errno = %i (%s))", fd, err, err_errno.get_errno(), strerror(err_errno.get_errno()));
    else
        description_sp->printf("close (fd=%i) => %i", fd, err);
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());

    if (err == 0)
    {
        if (fd >= 0)
            save_backtrace (fd, err, description_sp, false);
    }
    else if (err == -1)
    {
        if (err_errno.get_errno() == EBADF && fd != -1) 
        {
            backtrace_log ("\nerror: close on fd=%d resulted in EBADF in process %i\n", fd, getpid());

            FDEventMap::iterator pos = g_fd_event_map.find (fd);
            if (pos != g_fd_event_map.end())
            {
                log (get_logging_fd(), pos->second.back().get(), "\nfd=%d was previously closed with this event:\n", fd);
            }
        }
    }
    return err;
}

//----------------------------------------------------------------------
// close$NOCANCEL() interpose function
//----------------------------------------------------------------------
extern "C" int close$NOCANCEL(int);
extern "C" int
close$NOCANCEL$__interposed__ (int fd)
{
    Locker locker (&g_mutex);
    const int err = close$NOCANCEL(fd);
    NegativeErrorErrno err_errno(err);
    StringSP description_sp (new String);
    if (err == -1)
        description_sp->printf("close$NOCANCEL (fd=%i) => %i errno = %i (%s))", fd, err, err_errno.get_errno(), strerror(err_errno.get_errno()));
    else
        description_sp->printf("close$NOCANCEL (fd=%i) => %i", fd, err);
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    
    if (err == 0)
    {
        if (fd >= 0)
            save_backtrace (fd, err, description_sp, false);
    }
    else if (err == -1)
    {
        if (err_errno.get_errno() == EBADF && fd != -1) 
        {
            backtrace_log ("\nInvoking close$NOCANCEL (fd=%d) in process %i resulted in %i %s\n", fd, getpid(), err_errno.get_errno(), err_errno.get_errno() ? strerror (err_errno.get_errno()) : "");
            
            FDEventMap::iterator pos = g_fd_event_map.find (fd);
            if (pos != g_fd_event_map.end())
            {
                log (get_logging_fd(), pos->second.back().get(), "\nPrevious close(fd=%d) was done here:\n", fd);
            }
        }
    }
    return err;
    
}

//----------------------------------------------------------------------
// pipe() interpose function
//----------------------------------------------------------------------
extern "C" int
pipe$__interposed__ (int fds[2])
{
    Locker locker (&g_mutex);
    fds[0] = -1;
    fds[1] = -1;
    const int err = pipe (fds);
    const int saved_errno = errno;
    StringSP description_sp(new String ("pipe ({fd=%i, fd=%i}) -> err=%i", fds[0], fds[1], err));
    if (g_log_all_calls)
        description_sp->log (get_logging_fd());
    if (fds[0] >= 0)
        save_backtrace (fds[0], saved_errno, description_sp, true);
    if (fds[1] >= 0)
        save_backtrace (fds[1], saved_errno, description_sp, true);
    errno = saved_errno;
    return err;
}

//----------------------------------------------------------------------
// get_fd_history()
//
// This function allows runtime access to the file descriptor history.
//
// @param[in] log_fd
//      The file descriptor to log to
//
// @param[in] fd
//      The file descriptor whose history should be dumped
//----------------------------------------------------------------------
extern "C" void
get_fd_history (int log_fd, int fd)
{
    // "create" below needs to be outside of the mutex locker scope
    if (log_fd >= 0)
    {
        bool got_lock = false;
        Locker locker (&g_mutex, got_lock);
        if (got_lock)
        {
            FDEventMap::iterator pos = g_fd_event_map.find (fd);
            log_to_fd (log_fd, "Dumping file descriptor history for fd=%i:\n", fd);
            if (pos != g_fd_event_map.end())
            {
                FDEventArray &event_array = g_fd_event_map[fd];
                const size_t num_events = event_array.size();
                for (size_t i=0; i<num_events; ++i)
                    event_array[i]->Dump (log_fd);
            }
            else
            {
                log_to_fd (log_fd, "error: no file descriptor events found for fd=%i\n", fd);
            }
        }
        else
        {
            log_to_fd (log_fd, "error: fd event mutex is locked...\n");
        }
    }
}

//----------------------------------------------------------------------
// Interposing
//----------------------------------------------------------------------
// FD creation routines
DYLD_INTERPOSE(accept$__interposed__, accept);
DYLD_INTERPOSE(accept$NOCANCEL$__interposed__, accept$NOCANCEL);
DYLD_INTERPOSE(dup$__interposed__, dup);
DYLD_INTERPOSE(dup2$__interposed__, dup2);
DYLD_INTERPOSE(kqueue$__interposed__, kqueue);
DYLD_INTERPOSE(open$__interposed__, open);
DYLD_INTERPOSE(open$NOCANCEL$__interposed__, open$NOCANCEL);
DYLD_INTERPOSE(open$NOCANCEL$__interposed__, __open_nocancel);
DYLD_INTERPOSE(__open_extended$__interposed__, __open_extended);
DYLD_INTERPOSE(pipe$__interposed__, pipe);
DYLD_INTERPOSE(shm_open$__interposed__, shm_open);
DYLD_INTERPOSE(socket$__interposed__, socket);
DYLD_INTERPOSE(socketpair$__interposed__, socketpair);

// FD deleting routines
DYLD_INTERPOSE(close$__interposed__, close);
DYLD_INTERPOSE(close$NOCANCEL$__interposed__, close$NOCANCEL);

} // namespace fd_interposing


