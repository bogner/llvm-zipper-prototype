//===-- sanitizer_symbolizer.cc -------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is shared between AddressSanitizer and ThreadSanitizer
// run-time libraries.
//===----------------------------------------------------------------------===//

#include "sanitizer_allocator_internal.h"
#include "sanitizer_platform.h"
#include "sanitizer_internal_defs.h"
#include "sanitizer_libc.h"
#include "sanitizer_placement_new.h"
#include "sanitizer_symbolizer_internal.h"

namespace __sanitizer {

AddressInfo::AddressInfo() {
  internal_memset(this, 0, sizeof(AddressInfo));
  function_offset = kUnknown;
}

void AddressInfo::Clear() {
  InternalFree(module);
  InternalFree(function);
  InternalFree(file);
  internal_memset(this, 0, sizeof(AddressInfo));
  function_offset = kUnknown;
}

void AddressInfo::FillModuleInfo(const char *mod_name, uptr mod_offset) {
  module = internal_strdup(mod_name);
  module_offset = mod_offset;
}

SymbolizedStack::SymbolizedStack() : next(nullptr), info() {}

SymbolizedStack *SymbolizedStack::New(uptr addr) {
  void *mem = InternalAlloc(sizeof(SymbolizedStack));
  SymbolizedStack *res = new(mem) SymbolizedStack();
  res->info.address = addr;
  return res;
}

void SymbolizedStack::ClearAll() {
  info.Clear();
  if (next)
    next->ClearAll();
  InternalFree(this);
}

DataInfo::DataInfo() {
  internal_memset(this, 0, sizeof(DataInfo));
}

void DataInfo::Clear() {
  InternalFree(module);
  InternalFree(name);
  internal_memset(this, 0, sizeof(DataInfo));
}

Symbolizer *Symbolizer::symbolizer_;
StaticSpinMutex Symbolizer::init_mu_;
LowLevelAllocator Symbolizer::symbolizer_allocator_;

void Symbolizer::AddHooks(Symbolizer::StartSymbolizationHook start_hook,
                          Symbolizer::EndSymbolizationHook end_hook) {
  CHECK(start_hook_ == 0 && end_hook_ == 0);
  start_hook_ = start_hook;
  end_hook_ = end_hook;
}

const char *Symbolizer::ModuleNameOwner::GetOwnedCopy(const char *str) {
  mu_->CheckLocked();

  // 'str' will be the same string multiple times in a row, optimize this case.
  if (last_match_ && !internal_strcmp(last_match_, str))
    return last_match_;

  // FIXME: this is linear search.
  // We should optimize this further if this turns out to be a bottleneck later.
  for (uptr i = 0; i < storage_.size(); ++i) {
    if (!internal_strcmp(storage_[i], str)) {
      last_match_ = storage_[i];
      return last_match_;
    }
  }
  last_match_ = internal_strdup(str);
  storage_.push_back(last_match_);
  return last_match_;
}

bool Symbolizer::FindModuleNameAndOffsetForAddress(uptr address,
                                                   const char **module_name,
                                                   uptr *module_offset) {
  LoadedModule *module = FindModuleForAddress(address);
  if (module == 0)
    return false;
  *module_name = module->full_name();
  *module_offset = address - module->base_address();
  return true;
}

LoadedModule *Symbolizer::FindModuleForAddress(uptr address) {
  bool modules_were_reloaded = false;
  if (!modules_fresh_) {
    for (uptr i = 0; i < n_modules_; i++)
      modules_[i].clear();
    n_modules_ = PlatformGetListOfModules(modules_, kMaxNumberOfModules);
    CHECK_GT(n_modules_, 0);
    CHECK_LT(n_modules_, kMaxNumberOfModules);
    modules_fresh_ = true;
    modules_were_reloaded = true;
  }
  for (uptr i = 0; i < n_modules_; i++) {
    if (modules_[i].containsAddress(address)) {
      return &modules_[i];
    }
  }
  // Reload the modules and look up again, if we haven't tried it yet.
  if (!modules_were_reloaded) {
    // FIXME: set modules_fresh_ from dlopen()/dlclose() interceptors.
    // It's too aggressive to reload the list of modules each time we fail
    // to find a module for a given address.
    modules_fresh_ = false;
    return FindModuleForAddress(address);
  }
  return 0;
}

Symbolizer::Symbolizer(IntrusiveList<SymbolizerTool> tools)
    : module_names_(&mu_), n_modules_(0), modules_fresh_(false), tools_(tools),
      start_hook_(0), end_hook_(0) {}

Symbolizer::SymbolizerScope::SymbolizerScope(const Symbolizer *sym)
    : sym_(sym) {
  if (sym_->start_hook_)
    sym_->start_hook_();
}

Symbolizer::SymbolizerScope::~SymbolizerScope() {
  if (sym_->end_hook_)
    sym_->end_hook_();
}

SymbolizedStack *Symbolizer::SymbolizePC(uptr addr) {
  BlockingMutexLock l(&mu_);
  const char *module_name;
  uptr module_offset;
  SymbolizedStack *res = SymbolizedStack::New(addr);
  if (!FindModuleNameAndOffsetForAddress(addr, &module_name, &module_offset))
    return res;
  // Always fill data about module name and offset.
  res->info.FillModuleInfo(module_name, module_offset);
  for (auto iter = Iterator(&tools_); iter.hasNext();) {
    auto *tool = iter.next();
    SymbolizerScope sym_scope(this);
    if (tool->SymbolizePC(addr, res)) {
      return res;
    }
  }
  return res;
}

bool Symbolizer::SymbolizeData(uptr addr, DataInfo *info) {
  BlockingMutexLock l(&mu_);
  const char *module_name;
  uptr module_offset;
  if (!FindModuleNameAndOffsetForAddress(addr, &module_name, &module_offset))
    return false;
  info->Clear();
  info->module = internal_strdup(module_name);
  info->module_offset = module_offset;
  for (auto iter = Iterator(&tools_); iter.hasNext();) {
    auto *tool = iter.next();
    SymbolizerScope sym_scope(this);
    if (tool->SymbolizeData(addr, info)) {
      return true;
    }
  }
  return true;
}

bool Symbolizer::GetModuleNameAndOffsetForPC(uptr pc, const char **module_name,
                                             uptr *module_address) {
  BlockingMutexLock l(&mu_);
  const char *internal_module_name = nullptr;
  if (!FindModuleNameAndOffsetForAddress(pc, &internal_module_name,
                                         module_address))
    return false;

  if (module_name)
    *module_name = module_names_.GetOwnedCopy(internal_module_name);
  return true;
}

void Symbolizer::Flush() {
  BlockingMutexLock l(&mu_);
  for (auto iter = Iterator(&tools_); iter.hasNext();) {
    auto *tool = iter.next();
    SymbolizerScope sym_scope(this);
    tool->Flush();
  }
}

const char *Symbolizer::Demangle(const char *name) {
  BlockingMutexLock l(&mu_);
  for (auto iter = Iterator(&tools_); iter.hasNext();) {
    auto *tool = iter.next();
    SymbolizerScope sym_scope(this);
    if (const char *demangled = tool->Demangle(name))
      return demangled;
  }
  return PlatformDemangle(name);
}

void Symbolizer::PrepareForSandboxing() {
  BlockingMutexLock l(&mu_);
  PlatformPrepareForSandboxing();
}

}  // namespace __sanitizer
