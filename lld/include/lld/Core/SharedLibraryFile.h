//===- Core/SharedLibraryFile.h - Models shared libraries as Atoms --------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_SHARED_LIBRARY_FILE_H_
#define LLD_CORE_SHARED_LIBRARY_FILE_H_

#include "lld/Core/File.h"
#include "lld/Core/SharedLibraryAtom.h"

namespace lld {


///
/// The SharedLibraryFile subclass of File is used to represent dynamic
/// shared libraries being linked against.  
///
class SharedLibraryFile : public File {
public:
           SharedLibraryFile(StringRef path) : File(path) {
  }
  virtual ~SharedLibraryFile() {
  }

  virtual Kind kind() const {
    return kindSharedLibrary;
  }

  static inline bool classof(const File *f) {
    return f->kind() == kindSharedLibrary;
  }
  static inline bool classof(const SharedLibraryFile *) { 
    return true; 
  }


  /// Check if the shared library exports a symbol with the specified name.
  /// If so, return a SharedLibraryAtom which represents that exported
  /// symbol.  Otherwise return nullptr.
  virtual const SharedLibraryAtom *exports(StringRef name, 
                                          bool dataSymbolOnly) const;
  
};

} // namespace lld

#endif // LLD_CORE_SHARED_LIBRARY_FILE_H_
