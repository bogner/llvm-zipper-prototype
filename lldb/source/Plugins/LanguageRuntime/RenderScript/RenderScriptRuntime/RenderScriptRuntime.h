//===-- RenderScriptRuntime.h -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_RenderScriptRuntime_h_
#define liblldb_RenderScriptRuntime_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/CPPLanguageRuntime.h"
#include "lldb/Core/Module.h"

namespace lldb_private
{

namespace lldb_renderscript
{

typedef uint32_t RSSlot;
class RSModuleDescriptor;
struct RSGlobalDescriptor;
struct RSKernelDescriptor;

typedef std::shared_ptr<RSModuleDescriptor> RSModuleDescriptorSP;
typedef std::shared_ptr<RSGlobalDescriptor> RSGlobalDescriptorSP;
typedef std::shared_ptr<RSKernelDescriptor> RSKernelDescriptorSP;

// Breakpoint Resolvers decide where a breakpoint is placed,
// so having our own allows us to limit the search scope to RS kernel modules.
// As well as check for .expand kernels as a fallback.
class RSBreakpointResolver : public BreakpointResolver
{
  public:

    RSBreakpointResolver(Breakpoint *bkpt, ConstString name):
                         BreakpointResolver (bkpt, BreakpointResolver::NameResolver),
                         m_kernel_name(name)
    {
    }

    void
    GetDescription(Stream *strm) override
    {
        if (strm)
            strm->Printf("RenderScript kernel breakpoint for '%s'", m_kernel_name.AsCString());
    }

    void
    Dump(Stream *s) const override
    {
    }

    Searcher::CallbackReturn
    SearchCallback(SearchFilter &filter,
                   SymbolContext &context,
                   Address *addr,
                   bool containing) override;

    Searcher::Depth
    GetDepth() override
    {
        return Searcher::eDepthModule;
    }

    lldb::BreakpointResolverSP
    CopyForBreakpoint(Breakpoint &breakpoint) override
    {
        lldb::BreakpointResolverSP ret_sp(new RSBreakpointResolver(&breakpoint, m_kernel_name));
        return ret_sp;
    }

  protected:
    ConstString m_kernel_name;
};

struct RSKernelDescriptor
{
  public:
    RSKernelDescriptor(const RSModuleDescriptor *module, const char *name, uint32_t slot)
        : m_module(module)
        , m_name(name)
        , m_slot(slot)
    {
    }

    void Dump(Stream &strm) const;

    const RSModuleDescriptor *m_module;
    ConstString m_name;
    RSSlot m_slot;
};

struct RSGlobalDescriptor
{
  public:
    RSGlobalDescriptor(const RSModuleDescriptor *module, const char *name )
        : m_module(module)
        , m_name(name)
    {
    }

    void Dump(Stream &strm) const;

    const RSModuleDescriptor *m_module;
    ConstString m_name;
};

class RSModuleDescriptor
{
  public:
    RSModuleDescriptor(const lldb::ModuleSP &module)
        : m_module(module)
    {
    }

    ~RSModuleDescriptor() {}

    bool ParseRSInfo();

    void Dump(Stream &strm) const;

    const lldb::ModuleSP m_module;
    std::vector<RSKernelDescriptor> m_kernels;
    std::vector<RSGlobalDescriptor> m_globals;
    std::map<std::string, std::string> m_pragmas;
    std::string m_resname;
};

} // end lldb_renderscript namespace

class RenderScriptRuntime : public lldb_private::CPPLanguageRuntime
{
  public:

    enum ModuleKind
    {
        eModuleKindIgnored,
        eModuleKindLibRS,
        eModuleKindDriver,
        eModuleKindImpl,
        eModuleKindKernelObj
    };

    ~RenderScriptRuntime();

    //------------------------------------------------------------------
    // Static Functions
    //------------------------------------------------------------------
    static void Initialize();

    static void Terminate();

    static lldb_private::LanguageRuntime *CreateInstance(Process *process, lldb::LanguageType language);

    static lldb::CommandObjectSP GetCommandObject(CommandInterpreter& interpreter);

    static lldb_private::ConstString GetPluginNameStatic();

    static bool IsRenderScriptModule(const lldb::ModuleSP &module_sp);

    static ModuleKind GetModuleKind(const lldb::ModuleSP &module_sp);

    static void ModulesDidLoad(const lldb::ProcessSP& process_sp, const ModuleList &module_list );

    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    virtual lldb_private::ConstString GetPluginName();

    virtual uint32_t GetPluginVersion();

    virtual bool IsVTableName(const char *name);

    virtual bool GetDynamicTypeAndAddress(ValueObject &in_value, lldb::DynamicValueType use_dynamic,
                                          TypeAndOrName &class_type_or_name, Address &address,
                                          Value::ValueType &value_type);

    virtual bool CouldHaveDynamicValue(ValueObject &in_value);

    virtual lldb::BreakpointResolverSP CreateExceptionResolver(Breakpoint *bkpt, bool catch_bp, bool throw_bp);

    bool LoadModule(const lldb::ModuleSP &module_sp);

    bool ProbeModules(const ModuleList module_list);

    void DumpModules(Stream &strm) const;

    void DumpContexts(Stream &strm) const;

    void DumpKernels(Stream &strm) const;

    void AttemptBreakpointAtKernelName(Stream &strm, const char *name, Error &error, lldb::TargetSP target);

    void SetBreakAllKernels(bool do_break, lldb::TargetSP target);

    void Status(Stream &strm) const;

    virtual size_t GetAlternateManglings(const ConstString &mangled, std::vector<ConstString> &alternates) {
        return static_cast<size_t>(0);
    }

    virtual void ModulesDidLoad(const ModuleList &module_list );

    void Update();

    void Initiate();
    
  protected:

    void InitSearchFilter(lldb::TargetSP target)
    {
        if (!m_filtersp)
            m_filtersp.reset(new SearchFilterForUnconstrainedSearches(target));
    }
    
    void FixupScriptDetails(lldb_renderscript::RSModuleDescriptorSP rsmodule_sp);

    void LoadRuntimeHooks(lldb::ModuleSP module, ModuleKind kind);

    lldb::BreakpointSP CreateKernelBreakpoint(const ConstString& name);

    void BreakOnModuleKernels(const lldb_renderscript::RSModuleDescriptorSP rsmodule_sp);
    
    struct RuntimeHook;
    typedef void (RenderScriptRuntime::*CaptureStateFn)(RuntimeHook* hook_info, ExecutionContext &context);  // Please do this!

    struct HookDefn
    {
        const char * name;
        const char * symbol_name_m32; // mangled name for the 32 bit architectures
        const char * symbol_name_m64; // mangled name for the 64 bit archs
        uint32_t version;
        ModuleKind kind;
        CaptureStateFn grabber;
    };

    struct RuntimeHook
    {
        lldb::addr_t address;
        const HookDefn  *defn;
        lldb::BreakpointSP bp_sp;
    };
    
    typedef std::shared_ptr<RuntimeHook> RuntimeHookSP;

    struct ScriptDetails;
    struct AllocationDetails;

    lldb::ModuleSP m_libRS;
    lldb::ModuleSP m_libRSDriver;
    lldb::ModuleSP m_libRSCpuRef;
    std::vector<lldb_renderscript::RSModuleDescriptorSP> m_rsmodules;

    std::vector<std::unique_ptr<ScriptDetails>> m_scripts;
    std::vector<std::unique_ptr<AllocationDetails>> m_allocations;

    std::map<lldb::addr_t, lldb_renderscript::RSModuleDescriptorSP> m_scriptMappings;
    std::map<lldb::addr_t, RuntimeHookSP> m_runtimeHooks;

    lldb::SearchFilterSP m_filtersp; // Needed to create breakpoints through Target API

    bool m_initiated;
    bool m_debuggerPresentFlagged;
    bool m_breakAllKernels;
    static const HookDefn s_runtimeHookDefns[];
    static const size_t s_runtimeHookCount;

  private:
    RenderScriptRuntime(Process *process); // Call CreateInstance instead.
    
    static bool HookCallback(void *baton, StoppointCallbackContext *ctx, lldb::user_id_t break_id,
                             lldb::user_id_t break_loc_id);

    void HookCallback(RuntimeHook* hook_info, ExecutionContext& context);

    bool GetArgSimple(ExecutionContext& context, uint32_t arg, uint64_t* data);

    void CaptureScriptInit1(RuntimeHook* hook_info, ExecutionContext& context);
    void CaptureAllocationInit1(RuntimeHook* hook_info, ExecutionContext& context);
    void CaptureSetGlobalVar1(RuntimeHook* hook_info, ExecutionContext& context);

    // Search for a script detail object using a target address.
    // If a script does not currently exist this function will return nullptr.
    // If 'create' is true and there is no previous script with this address,
    // then a new Script detail object will be created for this address and returned.
    ScriptDetails* LookUpScript(lldb::addr_t address, bool create);

    // Search for a previously saved allocation detail object using a target address.
    // If an allocation does not exist for this address then nullptr will be returned.
    // If 'create' is true and there is no previous allocation then a new allocation
    // detail object will be created for this address and returned.
    AllocationDetails* LookUpAllocation(lldb::addr_t address, bool create);
};

} // namespace lldb_private

#endif // liblldb_RenderScriptRuntime_h_
