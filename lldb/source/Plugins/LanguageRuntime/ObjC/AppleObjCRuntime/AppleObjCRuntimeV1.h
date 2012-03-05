//===-- AppleObjCRuntimeV1.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_AppleObjCRuntimeV1_h_
#define liblldb_AppleObjCRuntimeV1_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "AppleObjCRuntime.h"

namespace lldb_private {
    
class AppleObjCRuntimeV1 :
        public AppleObjCRuntime
{
public:
    virtual ~AppleObjCRuntimeV1() { }
    
    // These are generic runtime functions:
    virtual bool
    GetDynamicTypeAndAddress (ValueObject &in_value, 
                              lldb::DynamicValueType use_dynamic, 
                              TypeAndOrName &class_type_or_name, 
                              Address &address);

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
    
    virtual ObjCRuntimeVersions
    GetRuntimeVersion ()
    {
        return eAppleObjC_V1;
    }
    
    virtual bool
    IsValidISA(ObjCISA isa)
    {
        return false;
    }
    
    virtual ObjCISA
    GetISA(ValueObject& valobj)
    {
        return 0;
    }
    
    virtual ConstString
    GetActualTypeName(ObjCISA isa)
    {
        return ConstString(NULL);
    }
    
    virtual ObjCISA
    GetParentClass(ObjCISA isa)
    {
        return 0;
    }

protected:
    virtual lldb::BreakpointResolverSP
    CreateExceptionResolver (Breakpoint *bkpt, bool catch_bp, bool throw_bp);
            
private:
    AppleObjCRuntimeV1(Process *process) : 
        lldb_private::AppleObjCRuntime (process)
     { } // Call CreateInstance instead.
};
    
} // namespace lldb_private

#endif  // liblldb_AppleObjCRuntimeV1_h_
