//===-- Cocoa.h ---------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Cocoa_h_
#define liblldb_Cocoa_h_

#include "lldb/Core/Stream.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Target/ObjCLanguageRuntime.h"

namespace lldb_private {
    namespace formatters
    {
        template<bool name_entries>
        bool
        NSDictionarySummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSIndexSetSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSArraySummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        template<bool cf_style>
        bool
        NSSetSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        template<bool needs_at>
        bool
        NSDataSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSNumberSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSNotificationSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSTimeZoneSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSMachPortSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSDateSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSBundleSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSTaggedString_SummaryProvider (ObjCLanguageRuntime::ClassDescriptorSP descriptor, Stream& stream);
        
        bool
        NSAttributedStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSMutableAttributedStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        bool
        NSURLSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        extern template bool
        NSDictionarySummaryProvider<true> (ValueObject&, Stream&, const TypeSummaryOptions&) ;
        
        extern template bool
        NSDictionarySummaryProvider<false> (ValueObject&, Stream&, const TypeSummaryOptions&) ;
        
        extern template bool
        NSDataSummaryProvider<true> (ValueObject&, Stream&, const TypeSummaryOptions&) ;
        
        extern template bool
        NSDataSummaryProvider<false> (ValueObject&, Stream&, const TypeSummaryOptions&) ;
        
        SyntheticChildrenFrontEnd* NSArraySyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP);
        
        SyntheticChildrenFrontEnd* NSDictionarySyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP);
        
        SyntheticChildrenFrontEnd* NSSetSyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP);
        
        SyntheticChildrenFrontEnd* NSIndexPathSyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP);
        
        bool
        ObjCClassSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        SyntheticChildrenFrontEnd* ObjCClassSyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP);
        
        bool
        ObjCBOOLSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        template <bool is_sel_ptr>
        bool
        ObjCSELSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
        
        extern template bool
        ObjCSELSummaryProvider<true> (ValueObject&, Stream&, const TypeSummaryOptions&);
        
        extern template bool
        ObjCSELSummaryProvider<false> (ValueObject&, Stream&, const TypeSummaryOptions&);
        
        bool
        RuntimeSpecificDescriptionSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options);
    } // namespace formatters
} // namespace lldb_private

#endif // liblldb_Cocoa_h_
