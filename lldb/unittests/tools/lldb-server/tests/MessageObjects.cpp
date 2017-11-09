//===-- MessageObjects.cpp --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MessageObjects.h"
#include "lldb/Utility/StructuredData.h"
#include "llvm/ADT/StringExtras.h"
#include "gtest/gtest.h"

using namespace lldb_private;
using namespace llvm;
using namespace llvm::support;
namespace llgs_tests {

Expected<ProcessInfo> ProcessInfo::Create(StringRef response) {
  ProcessInfo process_info;
  auto elements_or_error = SplitUniquePairList("ProcessInfo", response);
  if (!elements_or_error)
    return elements_or_error.takeError();

  auto &elements = *elements_or_error;
  if (elements["pid"].getAsInteger(16, process_info.m_pid))
    return make_parsing_error("ProcessInfo: pid");
  if (elements["parent-pid"].getAsInteger(16, process_info.m_parent_pid))
    return make_parsing_error("ProcessInfo: parent-pid");
  if (elements["real-uid"].getAsInteger(16, process_info.m_real_uid))
    return make_parsing_error("ProcessInfo: real-uid");
  if (elements["real-gid"].getAsInteger(16, process_info.m_real_gid))
    return make_parsing_error("ProcessInfo: real-uid");
  if (elements["effective-uid"].getAsInteger(16, process_info.m_effective_uid))
    return make_parsing_error("ProcessInfo: effective-uid");
  if (elements["effective-gid"].getAsInteger(16, process_info.m_effective_gid))
    return make_parsing_error("ProcessInfo: effective-gid");
  if (elements["ptrsize"].getAsInteger(10, process_info.m_ptrsize))
    return make_parsing_error("ProcessInfo: ptrsize");

  process_info.m_triple = fromHex(elements["triple"]);
  StringRef endian_str = elements["endian"];
  if (endian_str == "little")
    process_info.m_endian = support::little;
  else if (endian_str == "big")
    process_info.m_endian = support::big;
  else
    return make_parsing_error("ProcessInfo: endian");

  return process_info;
}

lldb::pid_t ProcessInfo::GetPid() const { return m_pid; }

endianness ProcessInfo::GetEndian() const { return m_endian; }

//====== ThreadInfo ============================================================
ThreadInfo::ThreadInfo(StringRef name, StringRef reason,
                       const RegisterMap &registers, unsigned int signal)
    : m_name(name.str()), m_reason(reason.str()), m_registers(registers),
      m_signal(signal) {}

StringRef ThreadInfo::ReadRegister(unsigned int register_id) const {
  return m_registers.lookup(register_id);
}

Expected<uint64_t>
ThreadInfo::ReadRegisterAsUint64(unsigned int register_id) const {
  uint64_t value;
  std::string value_str(m_registers.lookup(register_id));
  if (!llvm::to_integer(value_str, value, 16))
    return make_parsing_error("ThreadInfo value for register {0}: {1}",
                              register_id, value_str);

  sys::swapByteOrder(value);
  return value;
}

//====== JThreadsInfo ==========================================================
Expected<JThreadsInfo> JThreadsInfo::Create(StringRef response,
                                            endianness endian) {
  JThreadsInfo jthreads_info;

  StructuredData::ObjectSP json = StructuredData::ParseJSON(response);
  StructuredData::Array *array = json->GetAsArray();
  if (!array)
    return make_parsing_error("JThreadsInfo: JSON array");

  for (size_t i = 0; i < array->GetSize(); i++) {
    StructuredData::Dictionary *thread_info;
    array->GetItemAtIndexAsDictionary(i, thread_info);
    if (!thread_info)
      return make_parsing_error("JThreadsInfo: JSON obj at {0}", i);

    StringRef name, reason;
    thread_info->GetValueForKeyAsString("name", name);
    thread_info->GetValueForKeyAsString("reason", reason);
    uint64_t signal;
    thread_info->GetValueForKeyAsInteger("signal", signal);
    uint64_t tid;
    thread_info->GetValueForKeyAsInteger("tid", tid);

    StructuredData::Dictionary *register_dict;
    thread_info->GetValueForKeyAsDictionary("registers", register_dict);
    if (!register_dict)
      return make_parsing_error("JThreadsInfo: registers JSON obj");

    RegisterMap registers;

    auto keys_obj = register_dict->GetKeys();
    auto keys = keys_obj->GetAsArray();
    for (size_t i = 0; i < keys->GetSize(); i++) {
      StringRef key_str, value_str;
      keys->GetItemAtIndexAsString(i, key_str);
      register_dict->GetValueForKeyAsString(key_str, value_str);
      unsigned int register_id;
      if (key_str.getAsInteger(10, register_id))
        return make_parsing_error("JThreadsInfo: register key[{0}]", i);

      registers[register_id] = value_str.str();
    }

    jthreads_info.m_thread_infos[tid] =
        ThreadInfo(name, reason, registers, signal);
  }

  return jthreads_info;
}

const ThreadInfoMap &JThreadsInfo::GetThreadInfos() const {
  return m_thread_infos;
}

//====== StopReply =============================================================
const U64Map &StopReply::GetThreadPcs() const { return m_thread_pcs; }

Expected<StopReply> StopReply::Create(StringRef response,
                                      llvm::support::endianness endian) {
  if (response.size() < 3 || !response.consume_front("T"))
    return make_parsing_error("StopReply: Invalid packet");

  StopReply stop_reply;

  StringRef signal = response.take_front(2);
  response = response.drop_front(2);
  if (!llvm::to_integer(signal, stop_reply.m_signal, 16))
    return make_parsing_error("StopReply: stop signal");

  auto elements = SplitPairList(response);
  for (StringRef field :
       {"name", "reason", "thread", "threads", "thread-pcs"}) {
    // This will insert an empty field if there is none. In the future, we
    // should probably differentiate between these fields not being present and
    // them being empty, but right now no tests depends on this.
    if (elements.insert({field, {""}}).first->second.size() != 1)
      return make_parsing_error(
          "StopReply: got multiple responses for the {0} field", field);
  }
  stop_reply.m_name = elements["name"][0];
  stop_reply.m_reason = elements["reason"][0];

  if (!llvm::to_integer(elements["thread"][0], stop_reply.m_thread, 16))
    return make_parsing_error("StopReply: thread");

  SmallVector<StringRef, 20> threads;
  SmallVector<StringRef, 20> pcs;
  elements["threads"][0].split(threads, ',');
  elements["thread-pcs"][0].split(pcs, ',');
  if (threads.size() != pcs.size())
    return make_parsing_error("StopReply: thread/PC count mismatch");

  for (size_t i = 0; i < threads.size(); i++) {
    lldb::tid_t thread_id;
    uint64_t pc;
    if (threads[i].getAsInteger(16, thread_id))
      return make_parsing_error("StopReply: thread ID at [{0}].", i);
    if (pcs[i].getAsInteger(16, pc))
      return make_parsing_error("StopReply: thread PC at [{0}].", i);

    stop_reply.m_thread_pcs[thread_id] = pc;
  }

  for (auto i = elements.begin(); i != elements.end(); i++) {
    StringRef key = i->getKey();
    const auto &val = i->getValue();
    if (key.size() == 2) {
      unsigned int reg;
      if (key.getAsInteger(16, reg))
        continue;
      if (val.size() != 1)
        return make_parsing_error(
            "StopReply: multiple entries for register field [{0:x}]", reg);

      stop_reply.m_registers[reg] = val[0].str();
    }
  }

  return stop_reply;
}

//====== Globals ===============================================================
Expected<StringMap<StringRef>> SplitUniquePairList(StringRef caller,
                                                   StringRef str) {
  SmallVector<StringRef, 20> elements;
  str.split(elements, ';');

  StringMap<StringRef> pairs;
  for (StringRef s : elements) {
    std::pair<StringRef, StringRef> pair = s.split(':');
    if (pairs.count(pair.first))
      return make_parsing_error("{0}: Duplicate Key: {1}", caller, pair.first);

    pairs.insert(pair);
  }

  return pairs;
}

StringMap<SmallVector<StringRef, 2>> SplitPairList(StringRef str) {
  SmallVector<StringRef, 20> elements;
  str.split(elements, ';');

  StringMap<SmallVector<StringRef, 2>> pairs;
  for (StringRef s : elements) {
    std::pair<StringRef, StringRef> pair = s.split(':');
    pairs[pair.first].push_back(pair.second);
  }

  return pairs;
}
} // namespace llgs_tests
