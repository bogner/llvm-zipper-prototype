//===-- ObjCLanguageRuntime.h ---------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ObjCLanguageRuntime_h_
#define liblldb_ObjCLanguageRuntime_h_

// C Includes
// C++ Includes
#include <map>

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Target/LanguageRuntime.h"

namespace lldb_private {
    
class ClangUtilityFunction;

class ObjCLanguageRuntime :
    public LanguageRuntime
{
public:
    virtual
    ~ObjCLanguageRuntime();
    
    virtual lldb::LanguageType
    GetLanguageType () const
    {
        return lldb::eLanguageTypeObjC;
    }
    
    virtual bool
    IsModuleObjCLibrary (const lldb::ModuleSP &module_sp) = 0;
    
    virtual bool
    ReadObjCLibrary (const lldb::ModuleSP &module_sp) = 0;
    
    virtual bool
    HasReadObjCLibrary () = 0;
    
    virtual lldb::ThreadPlanSP
    GetStepThroughTrampolinePlan (Thread &thread, bool stop_others) = 0;
    
    lldb::addr_t
    LookupInMethodCache (lldb::addr_t class_addr, lldb::addr_t sel);

    void
    AddToMethodCache (lldb::addr_t class_addr, lldb::addr_t sel, lldb::addr_t impl_addr);
    
    TypeAndOrName
    LookupInClassNameCache (lldb::addr_t class_addr);
    
    void
    AddToClassNameCache (lldb::addr_t class_addr, const char *name, lldb::TypeSP type_sp);
    
    void
    AddToClassNameCache (lldb::addr_t class_addr, const TypeAndOrName &class_or_type_name);
    
    virtual ClangUtilityFunction *
    CreateObjectChecker (const char *) = 0;
    
    virtual ObjCRuntimeVersions
    GetRuntimeVersion ()
    {
        return eObjC_VersionUnknown;
    }
        
    typedef lldb::addr_t ObjCISA;
    
    virtual bool
    IsValidISA(ObjCISA isa) = 0;
    
    virtual ObjCISA
    GetISA(ValueObject& valobj) = 0;
    
    virtual ConstString
    GetActualTypeName(ObjCISA isa) = 0;
    
    virtual ObjCISA
    GetParentClass(ObjCISA isa) = 0;
    
    virtual SymbolVendor *
    GetSymbolVendor()
    {
        return NULL;
    }
    
    // Finds the byte offset of the child_type ivar in parent_type.  If it can't find the
    // offset, returns LLDB_INVALID_IVAR_OFFSET.
    
    virtual size_t
    GetByteOffsetForIvar (ClangASTType &parent_qual_type, const char *ivar_name);
    
    // If the passed in "name" is an ObjC method, return true.  Also, fill in any of the
    // sub-parts that are passed in non-NULL.  The base_name means the name stripped of
    // category attributes.
    static bool
    ParseMethodName (const char *name, 
                     ConstString *class_name,               // Class name (with category if there is one)
                     ConstString *selector_name,            // selector only
                     ConstString *name_sans_category,       // full function name with no category
                     ConstString *class_name_sans_category);// Class name without category (empty if no category)
    
    static bool
    IsPossibleObjCMethodName (const char *name)
    {
        if (!name)
            return false;
        bool starts_right = (name[0] == '+' || name[0] == '-') && name[1] == '[';
        bool ends_right = (name[strlen(name) - 1] == ']');
        return (starts_right && ends_right);
    }
    
    static bool
    IsPossibleObjCSelector (const char *name)
    {
        if (!name)
            return false;
            
        if (strchr(name, ':') == NULL)
            return true;
        else if (name[strlen(name) - 1] == ':')
            return true;
        else
            return false;
    }
    
protected:
    //------------------------------------------------------------------
    // Classes that inherit from ObjCLanguageRuntime can see and modify these
    //------------------------------------------------------------------
    ObjCLanguageRuntime(Process *process);
private:
    // We keep a map of <Class,Selector>->Implementation so we don't have to call the resolver
    // function over and over.
    
    // FIXME: We need to watch for the loading of Protocols, and flush the cache for any
    // class that we see so changed.
    
    struct ClassAndSel
    {
        ClassAndSel()
        {
            sel_addr = LLDB_INVALID_ADDRESS;
            class_addr = LLDB_INVALID_ADDRESS;
        }
        ClassAndSel (lldb::addr_t in_sel_addr, lldb::addr_t in_class_addr) :
            class_addr (in_class_addr),
            sel_addr(in_sel_addr)
        {
        }
        bool operator== (const ClassAndSel &rhs)
        {
            if (class_addr == rhs.class_addr
                && sel_addr == rhs.sel_addr)
                return true;
            else
                return false;
        }
        
        bool operator< (const ClassAndSel &rhs) const
        {
            if (class_addr < rhs.class_addr)
                return true;
            else if (class_addr > rhs.class_addr)
                return false;
            else
            {
                if (sel_addr < rhs.sel_addr)
                    return true;
                else
                    return false;
            }
        }
        
        lldb::addr_t class_addr;
        lldb::addr_t sel_addr;
    };

    typedef std::map<ClassAndSel,lldb::addr_t> MsgImplMap;
    MsgImplMap m_impl_cache;
    
protected:
    typedef std::map<lldb::addr_t,TypeAndOrName> ClassNameMap;
    typedef ClassNameMap::iterator ClassNameIterator;
    ClassNameMap m_class_name_cache;

    DISALLOW_COPY_AND_ASSIGN (ObjCLanguageRuntime);
};

} // namespace lldb_private

#endif  // liblldb_ObjCLanguageRuntime_h_
