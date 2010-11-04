//===-- AppleObjCRuntimeV2.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_AppleObjCRuntimeV2_h_
#define liblldb_AppleObjCRuntimeV2_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Core/ValueObject.h"
#include "AppleObjCRuntime.h"
#include "AppleObjCTrampolineHandler.h"
#include "AppleThreadPlanStepThroughObjCTrampoline.h"

namespace lldb_private {
    
class AppleObjCRuntimeV2 :
        public AppleObjCRuntime
{
public:
    ~AppleObjCRuntimeV2() { }
    
    // These are generic runtime functions:
    virtual bool
    GetObjectDescription (Stream &str, Value &value, ExecutionContextScope *exe_scope);
    
    virtual bool
    GetObjectDescription (Stream &str, ValueObject &object, ExecutionContextScope *exe_scope);
    
    virtual lldb::ValueObjectSP
    GetDynamicValue (lldb::ValueObjectSP in_value, ExecutionContextScope *exe_scope);
    
    virtual ClangUtilityFunction *
    CreateObjectChecker (const char *);


    //------------------------------------------------------------------
    // Static Functions
    //------------------------------------------------------------------
    static void
    Initialize();
    
    static void
    Terminate();
    
    static lldb_private::LanguageRuntime *
    CreateInstance (Process *process, lldb::LanguageType language);
    
    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    virtual const char *
    GetPluginName();
    
    virtual const char *
    GetShortPluginName();
    
    virtual uint32_t
    GetPluginVersion();
    
    virtual void
    SetExceptionBreakpoints ();
    
protected:
    
private:
    AppleObjCRuntimeV2(Process *process) : 
        lldb_private::AppleObjCRuntime (process)
     { } // Call CreateInstance instead.
};
    
} // namespace lldb_private

#endif  // liblldb_AppleObjCRuntimeV2_h_
