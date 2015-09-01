//===-- Language.h ---------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Language_h_
#define liblldb_Language_h_

// C Includes
// C++ Includes
#include <functional>

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-public.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/lldb-private.h"

namespace lldb_private {
    
    class Language :
    public PluginInterface
    {
    public:
        
        ~Language() override;
        
        static Language*
        FindPlugin (lldb::LanguageType language);
        
        // return false from callback to stop iterating
        static void
        ForEach (std::function<bool(Language*)> callback);
        
        virtual lldb::LanguageType
        GetLanguageType () const = 0;
        
        virtual lldb::TypeCategoryImplSP
        GetFormatters ();

    protected:
        //------------------------------------------------------------------
        // Classes that inherit from Language can see and modify these
        //------------------------------------------------------------------
        
        Language();
    private:
        
        DISALLOW_COPY_AND_ASSIGN (Language);
    };
    
} // namespace lldb_private

#endif // liblldb_Language_h_
