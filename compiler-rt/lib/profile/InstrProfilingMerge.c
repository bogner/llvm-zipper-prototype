/*===- InstrProfilingMerge.c - Profile in-process Merging  ---------------===*\
|*
|*                     The LLVM Compiler Infrastructure
|*
|* This file is distributed under the University of Illinois Open Source
|* License. See LICENSE.TXT for details.
|*
|*===----------------------------------------------------------------------===*
|* This file defines the API needed for in-process merging of profile data
|* stored in memory buffer.
\*===---------------------------------------------------------------------===*/

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"
#include "InstrProfilingUtil.h"

#define INSTR_PROF_VALUE_PROF_DATA
#include "InstrProfData.inc"

COMPILER_RT_WEAK void (*VPMergeHook)(ValueProfData *,
                                     __llvm_profile_data *) = NULL;

void __llvm_profile_merge_from_buffer(const char *ProfileData,
                                      uint64_t ProfileSize) {
  __llvm_profile_data *SrcDataStart, *SrcDataEnd, *SrcData, *DstData;
  __llvm_profile_header *Header = (__llvm_profile_header *)ProfileData;
  uint64_t *SrcCountersStart;
  const char *SrcNameStart;
  ValueProfData *SrcValueProfDataStart, *SrcValueProfData;

  SrcDataStart =
      (__llvm_profile_data *)(ProfileData + sizeof(__llvm_profile_header));
  SrcDataEnd = SrcDataStart + Header->DataSize;
  SrcCountersStart = (uint64_t *)SrcDataEnd;
  SrcNameStart = (const char *)(SrcCountersStart + Header->CountersSize);
  SrcValueProfDataStart =
      (ValueProfData *)(SrcNameStart + Header->NamesSize +
                        __llvm_profile_get_num_padding_bytes(
                            Header->NamesSize));

  for (SrcData = SrcDataStart,
      DstData = (__llvm_profile_data *)__llvm_profile_begin_data(),
      SrcValueProfData = SrcValueProfDataStart;
       SrcData < SrcDataEnd; ++SrcData, ++DstData) {
    uint64_t *SrcCounters;
    uint64_t *DstCounters = (uint64_t *)DstData->CounterPtr;
    unsigned I, NC, NVK = 0;

    NC = SrcData->NumCounters;
    SrcCounters = SrcCountersStart +
                  ((size_t)SrcData->CounterPtr - Header->CountersDelta) /
                      sizeof(uint64_t);
    for (I = 0; I < NC; I++)
      DstCounters[I] += SrcCounters[I];

    /* Now merge value profile data.  */
    if (!VPMergeHook)
      continue;

    for (I = 0; I <= IPVK_Last; I++)
      NVK += (SrcData->NumValueSites[I] != 0);

    if (!NVK)
      continue;

    VPMergeHook(SrcValueProfData, DstData);
    SrcValueProfData = (ValueProfData *)((char *)SrcValueProfData +
                                         SrcValueProfData->TotalSize);
  }
}
