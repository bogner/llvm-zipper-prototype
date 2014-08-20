//===-- HostInfoMacOSX.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef lldb_Host_macosx_HostInfoMacOSX_h_
#define lldb_Host_macosx_HostInfoMacOSX_h_

#include "lldb/Host/posix/HostInfoPosix.h"

namespace lldb_private
{

class ArchSpec;

class HostInfoMacOSX : public HostInfoPosix
{
    friend class HostInfoBase;

  private:
    // Static class, unconstructable.
    HostInfoMacOSX();
    ~HostInfoMacOSX();

  public:
    static bool GetOSVersion(uint32_t &major, uint32_t &minor, uint32_t &update);
    static bool GetOSBuildString(std::string &s);
    static bool GetOSKernelDescription(std::string &s);

  protected:
    static void ComputeHostArchitectureSupport(ArchSpec &arch_32, ArchSpec &arch_64);
};
}

#endif
