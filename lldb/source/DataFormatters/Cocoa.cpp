//===-- Cocoa.cpp -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/DataFormatters/CXXFormatterFunctions.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/DataFormatters/TypeSummary.h"

#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Stream.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/Host/Endian.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Target.h"

#include "lldb/Utility/ProcessStructReader.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;

bool
lldb_private::formatters::NSBundleSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (!strcmp(class_name,"NSBundle"))
    {
        uint64_t offset = 5 * ptr_size;
        ValueObjectSP text(valobj.GetSyntheticChildAtOffset(offset, valobj.GetCompilerType().GetBasicTypeFromAST(lldb::eBasicTypeObjCID), true));

        StreamString summary_stream;
        bool was_nsstring_ok = NSStringSummaryProvider(*text.get(), summary_stream, options);
        if (was_nsstring_ok && summary_stream.GetSize() > 0)
        {
            stream.Printf("%s",summary_stream.GetData());
            return true;
        }
    }
    // this is either an unknown subclass or an NSBundle that comes from [NSBundle mainBundle]
    // which is encoded differently and needs to be handled by running code
    return ExtractSummaryFromObjCExpression(valobj, "NSString*", "bundlePath", stream);
}

bool
lldb_private::formatters::NSTimeZoneSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (!strcmp(class_name,"__NSTimeZone"))
    {
        uint64_t offset = ptr_size;
        ValueObjectSP text(valobj.GetSyntheticChildAtOffset(offset, valobj.GetCompilerType(), true));
        StreamString summary_stream;
        bool was_nsstring_ok = NSStringSummaryProvider(*text.get(), summary_stream, options);
        if (was_nsstring_ok && summary_stream.GetSize() > 0)
        {
            stream.Printf("%s",summary_stream.GetData());
            return true;
        }
    }
    return ExtractSummaryFromObjCExpression(valobj, "NSString*", "name", stream);
}

bool
lldb_private::formatters::NSNotificationSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (!strcmp(class_name,"NSConcreteNotification"))
    {
        uint64_t offset = ptr_size;
        ValueObjectSP text(valobj.GetSyntheticChildAtOffset(offset, valobj.GetCompilerType(), true));
        StreamString summary_stream;
        bool was_nsstring_ok = NSStringSummaryProvider(*text.get(), summary_stream, options);
        if (was_nsstring_ok && summary_stream.GetSize() > 0)
        {
            stream.Printf("%s",summary_stream.GetData());
            return true;
        }
    }
    // this is either an unknown subclass or an NSBundle that comes from [NSBundle mainBundle]
    // which is encoded differently and needs to be handled by running code
    return ExtractSummaryFromObjCExpression(valobj, "NSString*", "name", stream);
}

bool
lldb_private::formatters::NSMachPortSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    uint64_t port_number = 0;
    
    do
    {
        if (!strcmp(class_name,"NSMachPort"))
        {
            uint64_t offset = (ptr_size == 4 ? 12 : 20);
            Error error;
            port_number = process_sp->ReadUnsignedIntegerFromMemory(offset+valobj_addr, 4, 0, error);
            if (error.Success())
                break;
        }
        if (!ExtractValueFromObjCExpression(valobj, "int", "machPort", port_number))
            return false;
    } while (false);
    
    stream.Printf("mach port: %u",(uint32_t)(port_number & 0x00000000FFFFFFFF));
    return true;
}

bool
lldb_private::formatters::NSIndexSetSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    uint64_t count = 0;
    
    do {
        if (!strcmp(class_name,"NSIndexSet") || !strcmp(class_name,"NSMutableIndexSet"))
        {
            Error error;
            uint32_t mode = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr+ptr_size, 4, 0, error);
            if (error.Fail())
                return false;
            // this means the set is empty - count = 0
            if ((mode & 1) == 1)
            {
                count = 0;
                break;
            }
            if ((mode & 2) == 2)
                mode = 1; // this means the set only has one range
            else
                mode = 2; // this means the set has multiple ranges
            if (mode == 1)
            {
                count = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr+3*ptr_size, ptr_size, 0, error);
                if (error.Fail())
                    return false;
            }
            else
            {
                // read a pointer to the data at 2*ptr_size
                count = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr+2*ptr_size, ptr_size, 0, error);
                if (error.Fail())
                    return false;
                // read the data at 2*ptr_size from the first location
                count = process_sp->ReadUnsignedIntegerFromMemory(count+2*ptr_size, ptr_size, 0, error);
                if (error.Fail())
                    return false;
            }
        }
        else
        {
            if (!ExtractValueFromObjCExpression(valobj, "unsigned long long int", "count", count))
                return false;
        }
    }  while (false);
    stream.Printf("%" PRIu64 " index%s",
                  count,
                  (count == 1 ? "" : "es"));
    return true;
}

bool
lldb_private::formatters::NSNumberSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (!strcmp(class_name,"NSNumber") || !strcmp(class_name,"__NSCFNumber"))
    {
        uint64_t value = 0;
        uint64_t i_bits = 0;
        if (descriptor->GetTaggedPointerInfo(&i_bits,&value))
        {
            switch (i_bits)
            {
                case 0:
                    stream.Printf("(char)%hhd",(char)value);
                    break;
                case 1:
                case 4:
                    stream.Printf("(short)%hd",(short)value);
                    break;
                case 2:
                case 8:
                    stream.Printf("(int)%d",(int)value);
                    break;
                case 3:
                case 12:
                    stream.Printf("(long)%" PRId64,value);
                    break;
                default:
                    return false;
            }
            return true;
        }
        else
        {
            Error error;
            uint8_t data_type = (process_sp->ReadUnsignedIntegerFromMemory(valobj_addr + ptr_size, 1, 0, error) & 0x1F);
            uint64_t data_location = valobj_addr + 2*ptr_size;
            uint64_t value = 0;
            if (error.Fail())
                return false;
            switch (data_type)
            {
                case 1: // 0B00001
                    value = process_sp->ReadUnsignedIntegerFromMemory(data_location, 1, 0, error);
                    if (error.Fail())
                        return false;
                    stream.Printf("(char)%hhd",(char)value);
                    break;
                case 2: // 0B0010
                    value = process_sp->ReadUnsignedIntegerFromMemory(data_location, 2, 0, error);
                    if (error.Fail())
                        return false;
                    stream.Printf("(short)%hd",(short)value);
                    break;
                case 3: // 0B0011
                    value = process_sp->ReadUnsignedIntegerFromMemory(data_location, 4, 0, error);
                    if (error.Fail())
                        return false;
                    stream.Printf("(int)%d",(int)value);
                    break;
                case 17: // 0B10001
                    data_location += 8;
                case 4: // 0B0100
                    value = process_sp->ReadUnsignedIntegerFromMemory(data_location, 8, 0, error);
                    if (error.Fail())
                        return false;
                    stream.Printf("(long)%" PRId64,value);
                    break;
                case 5: // 0B0101
                {
                    uint32_t flt_as_int = process_sp->ReadUnsignedIntegerFromMemory(data_location, 4, 0, error);
                    if (error.Fail())
                        return false;
                    float flt_value = *((float*)&flt_as_int);
                    stream.Printf("(float)%f",flt_value);
                    break;
                }
                case 6: // 0B0110
                {
                    uint64_t dbl_as_lng = process_sp->ReadUnsignedIntegerFromMemory(data_location, 8, 0, error);
                    if (error.Fail())
                        return false;
                    double dbl_value = *((double*)&dbl_as_lng);
                    stream.Printf("(double)%g",dbl_value);
                    break;
                }
                default:
                    return false;
            }
            return true;
        }
    }
    else
    {
        return ExtractSummaryFromObjCExpression(valobj, "NSString*", "stringValue", stream);
    }
}

bool
lldb_private::formatters::NSURLSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (strcmp(class_name, "NSURL") == 0)
    {
        uint64_t offset_text = ptr_size + ptr_size + 8; // ISA + pointer + 8 bytes of data (even on 32bit)
        uint64_t offset_base = offset_text + ptr_size;
        CompilerType type(valobj.GetCompilerType());
        ValueObjectSP text(valobj.GetSyntheticChildAtOffset(offset_text, type, true));
        ValueObjectSP base(valobj.GetSyntheticChildAtOffset(offset_base, type, true));
        if (!text)
            return false;
        if (text->GetValueAsUnsigned(0) == 0)
            return false;
        StreamString summary;
        if (!NSStringSummaryProvider(*text, summary, options))
            return false;
        if (base && base->GetValueAsUnsigned(0))
        {
            if (summary.GetSize() > 0)
                summary.GetString().resize(summary.GetSize()-1);
            summary.Printf(" -- ");
            StreamString base_summary;
            if (NSURLSummaryProvider(*base, base_summary, options) && base_summary.GetSize() > 0)
                summary.Printf("%s",base_summary.GetSize() > 2 ? base_summary.GetData() + 2 : base_summary.GetData());
        }
        if (summary.GetSize())
        {
            stream.Printf("%s",summary.GetData());
            return true;
        }
    }
    else
    {
        return ExtractSummaryFromObjCExpression(valobj, "NSString*", "description", stream);
    }
    return false;
}

bool
lldb_private::formatters::NSDateSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    uint64_t date_value_bits = 0;
    double date_value = 0.0;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (strcmp(class_name,"NSDate") == 0 ||
        strcmp(class_name,"__NSDate") == 0 ||
        strcmp(class_name,"__NSTaggedDate") == 0)
    {
        uint64_t info_bits=0,value_bits = 0;
        if (descriptor->GetTaggedPointerInfo(&info_bits,&value_bits))
        {
            date_value_bits = ((value_bits << 8) | (info_bits << 4));
            date_value = *((double*)&date_value_bits);
        }
        else
        {
            Error error;
            date_value_bits = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr+ptr_size, 8, 0, error);
            date_value = *((double*)&date_value_bits);
            if (error.Fail())
                return false;
        }
    }
    else if (!strcmp(class_name,"NSCalendarDate"))
    {
        Error error;
        date_value_bits = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr+2*ptr_size, 8, 0, error);
        date_value = *((double*)&date_value_bits);
        if (error.Fail())
            return false;
    }
    else
    {
        if (ExtractValueFromObjCExpression(valobj, "NSTimeInterval", "ExtractValueFromObjCExpression", date_value_bits) == false)
            return false;
        date_value = *((double*)&date_value_bits);
    }
    if (date_value == -63114076800)
    {
        stream.Printf("0001-12-30 00:00:00 +0000");
        return true;
    }
    // this snippet of code assumes that time_t == seconds since Jan-1-1970
    // this is generally true and POSIXly happy, but might break if a library
    // vendor decides to get creative
    time_t epoch = GetOSXEpoch();
    epoch = epoch + (time_t)date_value;
    tm *tm_date = gmtime(&epoch);
    if (!tm_date)
        return false;
    std::string buffer(1024,0);
    if (strftime (&buffer[0], 1023, "%Z", tm_date) == 0)
        return false;
    stream.Printf("%04d-%02d-%02d %02d:%02d:%02d %s", tm_date->tm_year+1900, tm_date->tm_mon+1, tm_date->tm_mday, tm_date->tm_hour, tm_date->tm_min, tm_date->tm_sec, buffer.c_str());
    return true;
}

bool
lldb_private::formatters::ObjCClassSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptorFromISA(valobj.GetValueAsUnsigned(0)));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    stream.Printf("%s",class_name);
    return true;
}

class ObjCClassSyntheticChildrenFrontEnd : public SyntheticChildrenFrontEnd
{
public:
    ObjCClassSyntheticChildrenFrontEnd (lldb::ValueObjectSP valobj_sp) :
    SyntheticChildrenFrontEnd(*valobj_sp.get())
    {
    }
    
    virtual size_t
    CalculateNumChildren ()
    {
        return 0;
    }
    
    virtual lldb::ValueObjectSP
    GetChildAtIndex (size_t idx)
    {
        return lldb::ValueObjectSP();
    }
    
    virtual bool
    Update()
    {
        return false;
    }
    
    virtual bool
    MightHaveChildren ()
    {
        return false;
    }
    
    virtual size_t
    GetIndexOfChildWithName (const ConstString &name)
    {
        return UINT32_MAX;
    }
    
    virtual
    ~ObjCClassSyntheticChildrenFrontEnd ()
    {
    }
};

SyntheticChildrenFrontEnd*
lldb_private::formatters::ObjCClassSyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP valobj_sp)
{
    return new ObjCClassSyntheticChildrenFrontEnd(valobj_sp);
}

template<bool needs_at>
bool
lldb_private::formatters::NSDataSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    bool is_64bit = (process_sp->GetAddressByteSize() == 8);
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    uint64_t value = 0;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    if (!strcmp(class_name,"NSConcreteData") ||
        !strcmp(class_name,"NSConcreteMutableData") ||
        !strcmp(class_name,"__NSCFData"))
    {
        uint32_t offset = (is_64bit ? 16 : 8);
        Error error;
        value = process_sp->ReadUnsignedIntegerFromMemory(valobj_addr + offset, is_64bit ? 8 : 4, 0, error);
        if (error.Fail())
            return false;
    }
    else
    {
        if (!ExtractValueFromObjCExpression(valobj, "int", "length", value))
            return false;
    }
    
    stream.Printf("%s%" PRIu64 " byte%s%s",
                  (needs_at ? "@\"" : ""),
                  value,
                  (value != 1 ? "s" : ""),
                  (needs_at ? "\"" : ""));
    
    return true;
}

bool
lldb_private::formatters::NSTaggedString_SummaryProvider (ObjCLanguageRuntime::ClassDescriptorSP descriptor, Stream& stream)
{
    if (!descriptor)
        return false;
    uint64_t len_bits = 0, data_bits = 0;
    if (!descriptor->GetTaggedPointerInfo(&len_bits,&data_bits,nullptr))
        return false;
    
    static const int g_MaxNonBitmaskedLen = 7; //TAGGED_STRING_UNPACKED_MAXLEN
    static const int g_SixbitMaxLen = 9;
    static const int g_fiveBitMaxLen = 11;
    
    static const char *sixBitToCharLookup = "eilotrm.apdnsIc ufkMShjTRxgC4013" "bDNvwyUL2O856P-B79AFKEWV_zGJ/HYX";
    
    if (len_bits > g_fiveBitMaxLen)
        return false;
    
    // this is a fairly ugly trick - pretend that the numeric value is actually a char*
    // this works under a few assumptions:
    // little endian architecture
    // sizeof(uint64_t) > g_MaxNonBitmaskedLen
    if (len_bits <= g_MaxNonBitmaskedLen)
    {
        stream.Printf("@\"%s\"",(const char*)&data_bits);
        return true;
    }
    
    // if the data is bitmasked, we need to actually process the bytes
    uint8_t bitmask = 0;
    uint8_t shift_offset = 0;
    
    if (len_bits <= g_SixbitMaxLen)
    {
        bitmask = 0x03f;
        shift_offset = 6;
    }
    else
    {
        bitmask = 0x01f;
        shift_offset = 5;
    }
    
    std::vector<uint8_t> bytes;
    bytes.resize(len_bits);
    for (; len_bits > 0; data_bits >>= shift_offset, --len_bits)
    {
        uint8_t packed = data_bits & bitmask;
        bytes.insert(bytes.begin(), sixBitToCharLookup[packed]);
    }
    
    stream.Printf("@\"%s\"",&bytes[0]);
    return true;
}

static CompilerType
GetNSPathStore2Type (Target &target)
{
    static ConstString g_type_name("__lldb_autogen_nspathstore2");
    
    ClangASTContext *ast_ctx = target.GetScratchClangASTContext();
    
    if (!ast_ctx)
        return CompilerType();
    
    CompilerType voidstar = ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
    CompilerType uint32 = ast_ctx->GetIntTypeFromBitSize(32, false);
    
    return ast_ctx->GetOrCreateStructForIdentifier(g_type_name, {
        {"isa",voidstar},
        {"lengthAndRef",uint32},
        {"buffer",voidstar}
    });
}

bool
lldb_private::formatters::NSStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& summary_options)
{
    ProcessSP process_sp = valobj.GetProcessSP();
    if (!process_sp)
        return false;
    
    ObjCLanguageRuntime* runtime = (ObjCLanguageRuntime*)process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
    
    if (!runtime)
        return false;
    
    ObjCLanguageRuntime::ClassDescriptorSP descriptor(runtime->GetClassDescriptor(valobj));
    
    if (!descriptor.get() || !descriptor->IsValid())
        return false;
    
    uint32_t ptr_size = process_sp->GetAddressByteSize();
    
    lldb::addr_t valobj_addr = valobj.GetValueAsUnsigned(0);
    
    if (!valobj_addr)
        return false;
    
    const char* class_name = descriptor->GetClassName().GetCString();
    
    if (!class_name || !*class_name)
        return false;
    
    bool is_tagged_ptr = (0 == strcmp(class_name,"NSTaggedPointerString")) && descriptor->GetTaggedPointerInfo();
    // for a tagged pointer, the descriptor has everything we need
    if (is_tagged_ptr)
        return NSTaggedString_SummaryProvider(descriptor, stream);
    
    // if not a tagged pointer that we know about, try the normal route
    uint64_t info_bits_location = valobj_addr + ptr_size;
    if (process_sp->GetByteOrder() != lldb::eByteOrderLittle)
        info_bits_location += 3;
    
    Error error;
    
    uint8_t info_bits = process_sp->ReadUnsignedIntegerFromMemory(info_bits_location, 1, 0, error);
    if (error.Fail())
        return false;
    
    bool is_mutable = (info_bits & 1) == 1;
    bool is_inline = (info_bits & 0x60) == 0;
    bool has_explicit_length = (info_bits & (1 | 4)) != 4;
    bool is_unicode = (info_bits & 0x10) == 0x10;
    bool is_path_store = strcmp(class_name,"NSPathStore2") == 0;
    bool has_null = (info_bits & 8) == 8;
    
    size_t explicit_length = 0;
    if (!has_null && has_explicit_length && !is_path_store)
    {
        lldb::addr_t explicit_length_offset = 2*ptr_size;
        if (is_mutable && !is_inline)
            explicit_length_offset = explicit_length_offset + ptr_size; //  notInlineMutable.length;
        else if (is_inline)
            explicit_length = explicit_length + 0; // inline1.length;
        else if (!is_inline && !is_mutable)
            explicit_length_offset = explicit_length_offset + ptr_size; // notInlineImmutable1.length;
        else
            explicit_length_offset = 0;
        
        if (explicit_length_offset)
        {
            explicit_length_offset = valobj_addr + explicit_length_offset;
            explicit_length = process_sp->ReadUnsignedIntegerFromMemory(explicit_length_offset, 4, 0, error);
        }
    }
    
    if (strcmp(class_name,"NSString") &&
        strcmp(class_name,"CFStringRef") &&
        strcmp(class_name,"CFMutableStringRef") &&
        strcmp(class_name,"__NSCFConstantString") &&
        strcmp(class_name,"__NSCFString") &&
        strcmp(class_name,"NSCFConstantString") &&
        strcmp(class_name,"NSCFString") &&
        strcmp(class_name,"NSPathStore2"))
    {
        // not one of us - but tell me class name
        stream.Printf("class name = %s",class_name);
        return true;
    }
    
    if (is_mutable)
    {
        uint64_t location = 2 * ptr_size + valobj_addr;
        location = process_sp->ReadPointerFromMemory(location, error);
        if (error.Fail())
            return false;
        if (has_explicit_length && is_unicode)
        {
            ReadStringAndDumpToStreamOptions options(valobj);
            options.SetLocation(location);
            options.SetProcessSP(process_sp);
            options.SetStream(&stream);
            options.SetPrefixToken('@');
            options.SetQuote('"');
            options.SetSourceSize(explicit_length);
            options.SetNeedsZeroTermination(false);
            options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
            options.SetBinaryZeroIsTerminator(false);
            return ReadStringAndDumpToStream<StringElementType::UTF16>(options);
        }
        else
        {
            ReadStringAndDumpToStreamOptions options(valobj);
            options.SetLocation(location+1);
            options.SetProcessSP(process_sp);
            options.SetStream(&stream);
            options.SetPrefixToken('@');
            options.SetSourceSize(explicit_length);
            options.SetNeedsZeroTermination(false);
            options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
            options.SetBinaryZeroIsTerminator(false);
            return ReadStringAndDumpToStream<StringElementType::ASCII>(options);
        }
    }
    else if (is_inline && has_explicit_length && !is_unicode && !is_path_store && !is_mutable)
    {
        uint64_t location = 3 * ptr_size + valobj_addr;
        
        ReadStringAndDumpToStreamOptions options(valobj);
        options.SetLocation(location);
        options.SetProcessSP(process_sp);
        options.SetStream(&stream);
        options.SetPrefixToken('@');
        options.SetQuote('"');
        options.SetSourceSize(explicit_length);
        options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
        return ReadStringAndDumpToStream<StringElementType::ASCII> (options);
    }
    else if (is_unicode)
    {
        uint64_t location = valobj_addr + 2*ptr_size;
        if (is_inline)
        {
            if (!has_explicit_length)
            {
                stream.Printf("found new combo");
                return true;
            }
            else
                location += ptr_size;
        }
        else
        {
            location = process_sp->ReadPointerFromMemory(location, error);
            if (error.Fail())
                return false;
        }
        ReadStringAndDumpToStreamOptions options(valobj);
        options.SetLocation(location);
        options.SetProcessSP(process_sp);
        options.SetStream(&stream);
        options.SetPrefixToken('@');
        options.SetQuote('"');
        options.SetSourceSize(explicit_length);
        options.SetNeedsZeroTermination(has_explicit_length == false);
        options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
        options.SetBinaryZeroIsTerminator(has_explicit_length == false);
        return ReadStringAndDumpToStream<StringElementType::UTF16> (options);
    }
    else if (is_path_store)
    {
        ProcessStructReader reader(valobj.GetProcessSP().get(), valobj.GetValueAsUnsigned(0), GetNSPathStore2Type(*valobj.GetTargetSP()));
        explicit_length = reader.GetField<uint32_t>(ConstString("lengthAndRef")) >> 20;
        lldb::addr_t location = valobj.GetValueAsUnsigned(0) + ptr_size + 4;
        
        ReadStringAndDumpToStreamOptions options(valobj);
        options.SetLocation(location);
        options.SetProcessSP(process_sp);
        options.SetStream(&stream);
        options.SetPrefixToken('@');
        options.SetQuote('"');
        options.SetSourceSize(explicit_length);
        options.SetNeedsZeroTermination(has_explicit_length == false);
        options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
        options.SetBinaryZeroIsTerminator(has_explicit_length == false);
        return ReadStringAndDumpToStream<StringElementType::UTF16> (options);
    }
    else if (is_inline)
    {
        uint64_t location = valobj_addr + 2*ptr_size;
        if (!has_explicit_length)
        {
            // in this kind of string, the byte before the string content is a length byte
            // so let's try and use it to handle the embedded NUL case
            Error error;
            explicit_length = process_sp->ReadUnsignedIntegerFromMemory(location, 1, 0, error);
            if (error.Fail() || explicit_length == 0)
                has_explicit_length = false;
            else
                has_explicit_length = true;
            location++;
        }
        ReadStringAndDumpToStreamOptions options(valobj);
        options.SetLocation(location);
        options.SetProcessSP(process_sp);
        options.SetStream(&stream);
        options.SetPrefixToken('@');
        options.SetSourceSize(explicit_length);
        options.SetNeedsZeroTermination(!has_explicit_length);
        options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
        options.SetBinaryZeroIsTerminator(!has_explicit_length);
        if (has_explicit_length)
            return ReadStringAndDumpToStream<StringElementType::UTF8>(options);
        else
            return ReadStringAndDumpToStream<StringElementType::ASCII>(options);
    }
    else
    {
        uint64_t location = valobj_addr + 2*ptr_size;
        location = process_sp->ReadPointerFromMemory(location, error);
        if (error.Fail())
            return false;
        if (has_explicit_length && !has_null)
            explicit_length++; // account for the fact that there is no NULL and we need to have one added
        ReadStringAndDumpToStreamOptions options(valobj);
        options.SetLocation(location);
        options.SetProcessSP(process_sp);
        options.SetPrefixToken('@');
        options.SetStream(&stream);
        options.SetSourceSize(explicit_length);
        options.SetIgnoreMaxLength(summary_options.GetCapping() == TypeSummaryCapping::eTypeSummaryUncapped);
        return ReadStringAndDumpToStream<StringElementType::ASCII>(options);
    }
}

bool
lldb_private::formatters::NSAttributedStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    TargetSP target_sp(valobj.GetTargetSP());
    if (!target_sp)
        return false;
    uint32_t addr_size = target_sp->GetArchitecture().GetAddressByteSize();
    uint64_t pointer_value = valobj.GetValueAsUnsigned(0);
    if (!pointer_value)
        return false;
    pointer_value += addr_size;
    CompilerType type(valobj.GetCompilerType());
    ExecutionContext exe_ctx(target_sp,false);
    ValueObjectSP child_ptr_sp(valobj.CreateValueObjectFromAddress("string_ptr", pointer_value, exe_ctx, type));
    if (!child_ptr_sp)
        return false;
    DataExtractor data;
    Error error;
    child_ptr_sp->GetData(data, error);
    if (error.Fail())
        return false;
    ValueObjectSP child_sp(child_ptr_sp->CreateValueObjectFromData("string_data", data, exe_ctx, type));
    child_sp->GetValueAsUnsigned(0);
    if (child_sp)
        return NSStringSummaryProvider(*child_sp, stream, options);
    return false;
}

bool
lldb_private::formatters::NSMutableAttributedStringSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    return NSAttributedStringSummaryProvider(valobj, stream, options);
}

bool
lldb_private::formatters::ObjCBOOLSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    const uint32_t type_info = valobj.GetCompilerType().GetTypeInfo();
    
    ValueObjectSP real_guy_sp = valobj.GetSP();
    
    if (type_info & eTypeIsPointer)
    {
        Error err;
        real_guy_sp = valobj.Dereference(err);
        if (err.Fail() || !real_guy_sp)
            return false;
    }
    else if (type_info & eTypeIsReference)
    {
        real_guy_sp =  valobj.GetChildAtIndex(0, true);
        if (!real_guy_sp)
            return false;
    }
    uint64_t value = real_guy_sp->GetValueAsUnsigned(0);
    if (value == 0)
    {
        stream.Printf("NO");
        return true;
    }
    stream.Printf("YES");
    return true;
}

template <bool is_sel_ptr>
bool
lldb_private::formatters::ObjCSELSummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    lldb::ValueObjectSP valobj_sp;
    
    CompilerType charstar (valobj.GetCompilerType().GetBasicTypeFromAST(eBasicTypeChar).GetPointerType());
    
    if (!charstar)
        return false;
    
    ExecutionContext exe_ctx(valobj.GetExecutionContextRef());
    
    if (is_sel_ptr)
    {
        lldb::addr_t data_address = valobj.GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
        if (data_address == LLDB_INVALID_ADDRESS)
            return false;
        valobj_sp = ValueObject::CreateValueObjectFromAddress("text", data_address, exe_ctx, charstar);
    }
    else
    {
        DataExtractor data;
        Error error;
        valobj.GetData(data, error);
        if (error.Fail())
            return false;
        valobj_sp = ValueObject::CreateValueObjectFromData("text", data, exe_ctx, charstar);
    }
    
    if (!valobj_sp)
        return false;
    
    stream.Printf("%s",valobj_sp->GetSummaryAsCString());
    return true;
}

// POSIX has an epoch on Jan-1-1970, but Cocoa prefers Jan-1-2001
// this call gives the POSIX equivalent of the Cocoa epoch
time_t
lldb_private::formatters::GetOSXEpoch ()
{
    static time_t epoch = 0;
    if (!epoch)
    {
#ifndef _WIN32
        tzset();
        tm tm_epoch;
        tm_epoch.tm_sec = 0;
        tm_epoch.tm_hour = 0;
        tm_epoch.tm_min = 0;
        tm_epoch.tm_mon = 0;
        tm_epoch.tm_mday = 1;
        tm_epoch.tm_year = 2001-1900; // for some reason, we need to subtract 1900 from this field. not sure why.
        tm_epoch.tm_isdst = -1;
        tm_epoch.tm_gmtoff = 0;
        tm_epoch.tm_zone = NULL;
        epoch = timegm(&tm_epoch);
#endif
    }
    return epoch;
}

template bool
lldb_private::formatters::NSDataSummaryProvider<true> (ValueObject&, Stream&, const TypeSummaryOptions&) ;

template bool
lldb_private::formatters::NSDataSummaryProvider<false> (ValueObject&, Stream&, const TypeSummaryOptions&) ;

template bool
lldb_private::formatters::ObjCSELSummaryProvider<true> (ValueObject&, Stream&, const TypeSummaryOptions&) ;

template bool
lldb_private::formatters::ObjCSELSummaryProvider<false> (ValueObject&, Stream&, const TypeSummaryOptions&) ;
