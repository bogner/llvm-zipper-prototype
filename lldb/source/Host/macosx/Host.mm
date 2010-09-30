//===-- Host.mm -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/Host.h"
#include "lldb/Core/FileSpec.h"
#include "lldb/Core/Log.h"

#include "cfcpp/CFCBundle.h"
#include "cfcpp/CFCMutableDictionary.h"
#include "cfcpp/CFCReleaser.h"
#include "cfcpp/CFCString.h"

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
            // On SnowLoepard and later we just call the thread registration function.
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
Host::ResolveExecutableInBundle (FileSpec *file)
{
#if defined (__APPLE__)
  if (file->GetFileType () == FileSpec::eFileTypeDirectory)
  {
    char path[PATH_MAX];
    if (file->GetPath(path, sizeof(path)))
    {
      CFCBundle bundle (path);
      CFCReleaser<CFURLRef> url(bundle.CopyExecutableURL ());
      if (url.get())
      {
        if (::CFURLGetFileSystemRepresentation (url.get(), YES, (UInt8*)path, sizeof(path)))
        {
          file->SetFile(path);
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
    ::bzero (&app_params, sizeof (app_params));
    app_params.flags = kLSLaunchDefaults | 
                       kLSLaunchDontAddToRecents | 
                       kLSLaunchDontSwitch |
                       kLSLaunchNewInstance;// | 0x00001000;
    
    FSRef app_fsref;
    CFCString app_cfstr (app_path, kCFStringEncodingUTF8);
    
    OSStatus error = ::FSPathMakeRef ((const UInt8 *)app_path, &app_fsref, false);
    
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
    
    Log *log = lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_HOST);
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
    ::bzero (&app_params, sizeof (app_params));
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
    bzero(&which_process, sizeof(which_process));
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
