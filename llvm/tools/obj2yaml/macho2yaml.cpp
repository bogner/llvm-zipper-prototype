//===------ macho2yaml.cpp - obj2yaml conversion tool -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Error.h"
#include "obj2yaml.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/ObjectYAML/MachOYAML.h"

using namespace llvm;

class MachODumper {

  const object::MachOObjectFile &Obj;

public:
  MachODumper(const object::MachOObjectFile &O) : Obj(O) {}
  Expected<MachOYAML::Object *> dump();
};

Expected<MachOYAML::Object *> MachODumper::dump() {
  auto Y = make_unique<MachOYAML::Object>();
  Y->Header.magic = Obj.getHeader().magic;
  Y->Header.cputype = Obj.getHeader().cputype;
  Y->Header.cpusubtype = Obj.getHeader().cpusubtype;
  Y->Header.filetype = Obj.getHeader().filetype;
  Y->Header.ncmds = Obj.getHeader().ncmds;
  Y->Header.sizeofcmds = Obj.getHeader().sizeofcmds;
  Y->Header.flags = Obj.getHeader().flags;

  return Y.release();
}

Error macho2yaml(raw_ostream &Out, const object::MachOObjectFile &Obj) {
  MachODumper Dumper(Obj);
  Expected<MachOYAML::Object *> YAML = Dumper.dump();
  if (!YAML)
    return YAML.takeError();

  yaml::Output Yout(Out);
  Yout << *(YAML.get());
  return Error::success();
}

Error macho2yaml(raw_ostream &Out, const object::MachOUniversalBinary &Obj) {
  return make_error<Obj2YamlError>(obj2yaml_error::not_implemented);
}

std::error_code macho2yaml(raw_ostream &Out, const object::ObjectFile &Obj) {
  if (const auto *MachOObj = dyn_cast<object::MachOUniversalBinary>(&Obj)) {
    if (auto Err = macho2yaml(Out, *MachOObj)) {
      return errorToErrorCode(std::move(Err));
    }
    return obj2yaml_error::success;
  }

  if (const auto *MachOObj = dyn_cast<object::MachOObjectFile>(&Obj)) {
    if (auto Err = macho2yaml(Out, *MachOObj)) {
      return errorToErrorCode(std::move(Err));
    }
    return obj2yaml_error::success;
  }

  return obj2yaml_error::unsupported_obj_file_format;
}
