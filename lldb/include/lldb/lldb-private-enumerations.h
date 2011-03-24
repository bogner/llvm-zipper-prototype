//===-- lldb-enumerations.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_lldb_private_enumerations_h_
#define LLDB_lldb_private_enumerations_h_

namespace lldb_private {

//----------------------------------------------------------------------
// Thread Step Types
//----------------------------------------------------------------------
typedef enum StepType
{
    eStepTypeNone,
    eStepTypeTrace,     ///< Single step one instruction.
    eStepTypeTraceOver, ///< Single step one instruction, stepping over.
    eStepTypeInto,      ///< Single step into a specified context.
    eStepTypeOver,      ///< Single step over a specified context.
    eStepTypeOut        ///< Single step out a specified context.
} StepType;

//----------------------------------------------------------------------
// Address Types
//----------------------------------------------------------------------
typedef enum AddressType
{
    eAddressTypeInvalid = 0,
    eAddressTypeFile, ///< Address is an address as found in an object or symbol file
    eAddressTypeLoad, ///< Address is an address as in the current target inferior process
    eAddressTypeHost  ///< Address is an address in the process that is running this code
} AddressType;

//----------------------------------------------------------------------
// Address Class
//
// A way of classifying an address used for disassembling and setting 
// breakpoints. Many object files can track exactly what parts of their
// object files are code, data and other information. This is of course
// above and beyond just looking at the section types. For example, code
// might contain PC relative data and the object file might be able to
// tell us that an address in code is data.
//----------------------------------------------------------------------
typedef enum AddressClass
{
    eAddressClassInvalid,
    eAddressClassUnknown,
    eAddressClassCode,
    eAddressClassCodeAlternateISA,
    eAddressClassData,
    eAddressClassDebug,
    eAddressClassRuntime
} AddressClass;

//----------------------------------------------------------------------
// Votes - Need a tri-state, yes, no, no opinion...
//----------------------------------------------------------------------
typedef enum Vote
{
    eVoteNo         = -1,
    eVoteNoOpinion  =  0,
    eVoteYes        =  1
} Vote;

//----------------------------------------------------------------------
// Symbol types
//----------------------------------------------------------------------
typedef enum SymbolType
{
    eSymbolTypeAny = 0,
    eSymbolTypeInvalid = 0,
    eSymbolTypeAbsolute,
    eSymbolTypeExtern,
    eSymbolTypeCode,
    eSymbolTypeData,
    eSymbolTypeTrampoline,
    eSymbolTypeRuntime,
    eSymbolTypeException,
    eSymbolTypeSourceFile,
    eSymbolTypeHeaderFile,
    eSymbolTypeObjectFile,
    eSymbolTypeCommonBlock,
    eSymbolTypeBlock,
    eSymbolTypeLocal,
    eSymbolTypeParam,
    eSymbolTypeVariable,
    eSymbolTypeVariableType,
    eSymbolTypeLineEntry,
    eSymbolTypeLineHeader,
    eSymbolTypeScopeBegin,
    eSymbolTypeScopeEnd,
    eSymbolTypeAdditional, // When symbols take more than one entry, the extra entries get this type
    eSymbolTypeCompiler,
    eSymbolTypeInstrumentation,
    eSymbolTypeUndefined
} SymbolType;

typedef enum SectionType
{
    eSectionTypeInvalid,
    eSectionTypeCode,
    eSectionTypeContainer,              // The section contains child sections
    eSectionTypeData,
    eSectionTypeDataCString,            // Inlined C string data
    eSectionTypeDataCStringPointers,    // Pointers to C string data
    eSectionTypeDataSymbolAddress,      // Address of a symbol in the symbol table
    eSectionTypeData4,
    eSectionTypeData8,
    eSectionTypeData16,
    eSectionTypeDataPointers,
    eSectionTypeDebug,
    eSectionTypeZeroFill,
    eSectionTypeDataObjCMessageRefs,    // Pointer to function pointer + selector
    eSectionTypeDataObjCCFStrings,      // Objective C const CFString/NSString objects
    eSectionTypeDWARFDebugAbbrev,
    eSectionTypeDWARFDebugAranges,
    eSectionTypeDWARFDebugFrame,
    eSectionTypeDWARFDebugInfo,
    eSectionTypeDWARFDebugLine,
    eSectionTypeDWARFDebugLoc,
    eSectionTypeDWARFDebugMacInfo,
    eSectionTypeDWARFDebugPubNames,
    eSectionTypeDWARFDebugPubTypes,
    eSectionTypeDWARFDebugRanges,
    eSectionTypeDWARFDebugStr,
    eSectionTypeEHFrame,
    eSectionTypeOther

} SectionType;


typedef enum ArchitectureType 
{
    eArchTypeInvalid,
    eArchTypeMachO,
    eArchTypeELF,
    kNumArchTypes
} ArchitectureType;

typedef enum FunctionNameType 
{
    eFunctionNameTypeNone       = 0u,
    eFunctionNameTypeAuto       = (1u << 1),    // Automatically figure out which FunctionNameType
                                                // bits to set based on the function name.
    eFunctionNameTypeFull       = (1u << 2),    // The function name.
                                                // For C this is the same as just the name of the function
                                                // For C++ this is the demangled version of the mangled name.
                                                // For ObjC this is the full function signature with the + or
                                                // - and the square brackets and the class and selector
    eFunctionNameTypeBase       = (1u << 3),    // The function name only, no namespaces or arguments and no class 
                                                // methods or selectors will be searched.
    eFunctionNameTypeMethod     = (1u << 4),    // Find function by method name (C++) with no namespace or arguments
    eFunctionNameTypeSelector   = (1u << 5)     // Find function by selector name (ObjC) names
} FunctionNameType;

//----------------------------------------------------------------------
/// Settable state variable types.
///
//----------------------------------------------------------------------

typedef enum SettableVariableType
{
    eSetVarTypeInt,
    eSetVarTypeBoolean,
    eSetVarTypeString,
    eSetVarTypeArray,
    eSetVarTypeDictionary,
    eSetVarTypeEnum,
    eSetVarTypeNone
} SettableVariableType;

typedef enum VarSetOperationType
{
    eVarSetOperationReplace,
    eVarSetOperationInsertBefore,
    eVarSetOperationInsertAfter,
    eVarSetOperationRemove,
    eVarSetOperationAppend,
    eVarSetOperationClear,
    eVarSetOperationAssign,
    eVarSetOperationInvalid
} VarSetOperationType;

typedef enum ArgumentRepetitionType
{
    eArgRepeatPlain,            // Exactly one occurrence
    eArgRepeatOptional,         // At most one occurrence, but it's optional
    eArgRepeatPlus,             // One or more occurrences
    eArgRepeatStar,             // Zero or more occurrences
    eArgRepeatRange,            // Repetition of same argument, from 1 to n
    eArgRepeatPairPlain,        // A pair of arguments that must always go together ([arg-type arg-value]), occurs exactly once
    eArgRepeatPairOptional,     // A pair that occurs at most once (optional)
    eArgRepeatPairPlus,         // One or more occurrences of a pair
    eArgRepeatPairStar,         // Zero or more occurrences of a pair
    eArgRepeatPairRange,        // A pair that repeats from 1 to n
    eArgRepeatPairRangeOptional // A pair that repeats from 1 to n, but is optional
} ArgumentRepetitionType;

typedef enum SortOrder
{
    eSortOrderNone,
    eSortOrderByAddress,
    eSortOrderByName
} SortOrder;


//----------------------------------------------------------------------
// Used in conjunction with Host::GetLLDBResource () to find files that
// are related to 
//----------------------------------------------------------------------
typedef enum PathType
{
    ePathTypeLLDBShlibDir,          // The directory where the lldb.so (unix) or LLDB mach-o file in LLDB.framework (MacOSX) exists
    ePathTypeSupportExecutableDir,  // Find LLDB support executable directory (debugserver, etc)
    ePathTypeHeaderDir,             // Find LLDB header file directory
    ePathTypePythonDir,             // Find Python modules (PYTHONPATH) directory
    ePathTypeLLDBSystemPlugins,     // System plug-ins directory
    ePathTypeLLDBUserPlugins        // User plug-ins directory
} PathType;


//----------------------------------------------------------------------
// We can execute ThreadPlans on one thread with various fall-back modes 
// (try other threads after timeout, etc.) This enum gives the result of 
// thread plan executions.
//----------------------------------------------------------------------
typedef enum ExecutionResults
{
    eExecutionSetupError,
    eExecutionCompleted,
    eExecutionDiscarded,
    eExecutionInterrupted,
    eExecutionTimedOut
} ExecutionResults;

typedef enum ObjCRuntimeVersions {
    eObjC_VersionUnknown = 0,
    eAppleObjC_V1 = 1,
    eAppleObjC_V2 = 2
} ObjCRuntimeVersions;

    
//----------------------------------------------------------------------
// LazyBool is for boolean values that need to be calculated lazily.
// Values start off set to eLazyBoolCalculate, and then they can be
// calculated once and set to eLazyBoolNo or eLazyBoolYes.
//----------------------------------------------------------------------
typedef enum LazyBool {
    eLazyBoolCalculate  = -1,
    eLazyBoolNo         = 0,
    eLazyBoolYes        = 1
} LazyBool;

//------------------------------------------------------------------
/// Name matching
//------------------------------------------------------------------
typedef enum NameMatchType
{
    eNameMatchIgnore,
    eNameMatchEquals,
    eNameMatchContains,
    eNameMatchStartsWith,
    eNameMatchEndsWith,
    eNameMatchRegularExpression
    
} NameMatchType;


} // namespace lldb


#endif  // LLDB_lldb_private_enumerations_h_
