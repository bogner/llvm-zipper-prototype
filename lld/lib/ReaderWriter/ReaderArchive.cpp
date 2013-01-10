//===- lib/ReaderWriter/ReaderArchive.cpp - Archive Library Reader--------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//

#include "lld/ReaderWriter/ReaderArchive.h"

#include "llvm/ADT/Hashing.h"

#include <unordered_map>

namespace std {
  template<>
  struct hash<llvm::StringRef> {
  public:
    size_t operator()(const llvm::StringRef &s) const {
      using llvm::hash_value;
      return hash_value(s);
    }
  };
}

namespace lld {
/// \brief The FileArchive class represents an Archive Library file
class FileArchive : public ArchiveLibraryFile {
public:

  virtual ~FileArchive() { }

  /// \brief Check if any member of the archive contains an Atom with the
  /// specified name and return the File object for that member, or nullptr.
  virtual const File *find(StringRef name, bool dataSymbolOnly) const {
    auto member = _symbolMemberMap.find(name);
    if (member == _symbolMemberMap.end())
      return nullptr;

    error_code ec;
    llvm::object::Archive::child_iterator ci = member->second;
    
    if (dataSymbolOnly && (ec = isDataSymbol(ci->getBuffer(), name)))
      return nullptr;
    
    std::vector<std::unique_ptr<File>> result;

    if ((ec = _options.reader()->parseFile(std::unique_ptr<MemoryBuffer>
                                           (ci->getBuffer()), result)))
      return nullptr;

    assert(result.size() == 1);

    // give up the pointer so that this object no longer manages it
    return result[0].release();
  }

  virtual void addAtom(const Atom&) {
    llvm_unreachable("cannot add atoms to archive files");
  }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

protected:
  error_code isDataSymbol(MemoryBuffer *mb, StringRef symbol) const {
    std::unique_ptr<llvm::object::ObjectFile> 
                    obj(llvm::object::ObjectFile::createObjectFile(mb));
    error_code ec;
    llvm::object::SymbolRef::Type symtype;
    uint32_t symflags;
    llvm::object::symbol_iterator ibegin = obj->begin_symbols();
    llvm::object::symbol_iterator iend = obj->end_symbols();
    StringRef symbolname;

    for (llvm::object::symbol_iterator i = ibegin; i != iend; i.increment(ec)) {
      if (ec) return ec;
      
      // Get symbol name
      if ((ec = (i->getName(symbolname)))) return ec;
      
      if (symbolname != symbol) 
          continue;
      
      // Get symbol flags
      if ((ec = (i->getFlags(symflags)))) return ec;
      
      if (symflags <= llvm::object::SymbolRef::SF_Undefined)
          continue;
      
      // Get Symbol Type
      if ((ec = (i->getType(symtype)))) return ec;
      
      if (symtype == llvm::object::SymbolRef::ST_Data) {
        return error_code::success();
      }
    }
    return llvm::object::object_error::parse_failed;
  }

private:
  std::unique_ptr<llvm::object::Archive> _archive;
  const ReaderOptionsArchive _options;
  atom_collection_vector<DefinedAtom>       _definedAtoms;
  atom_collection_vector<UndefinedAtom>     _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom>      _absoluteAtoms;

public:
  /// only subclasses of ArchiveLibraryFile can be instantiated 
  explicit FileArchive(llvm::MemoryBuffer *mb, 
                       const ReaderOptionsArchive &options, 
                       error_code &ec)
                      :ArchiveLibraryFile(mb->getBufferIdentifier()),
                       _options(options) { 
    std::unique_ptr<llvm::object::Archive> archive_obj(
      new llvm::object::Archive(mb, ec));
    if (ec)
      return;
    _archive.swap(archive_obj);

    // Cache symbols.
    for (auto i = _archive->begin_symbols(), e = _archive->end_symbols();
              i != e; ++i) {
      StringRef name;
      llvm::object::Archive::child_iterator member;
      if ((ec = i->getName(name)))
        return;
      if ((ec = i->getMember(member)))
        return;
      _symbolMemberMap[name] = member;
    }
  }

  std::unordered_map<StringRef, llvm::object::Archive::child_iterator> _symbolMemberMap;
}; // class FileArchive

// Returns a vector of Files that are contained in the archive file 
// pointed to by the MemoryBuffer
error_code ReaderArchive::parseFile(std::unique_ptr<llvm::MemoryBuffer> mb,
		std::vector<std::unique_ptr<File>> &result) {
  error_code ec;
  
  if (_options.isForceLoad()) {
    _archive.reset(new llvm::object::Archive(mb.release(), ec));
    if (ec)
      return ec;
    
    for (auto mf = _archive->begin_children(), 
              me = _archive->end_children(); mf != me; ++mf) {
    	if ((ec = _options.reader()->parseFile(std::unique_ptr<MemoryBuffer>
                                             (mf->getBuffer()), result)))
        return ec;
    }
  } else {
    std::unique_ptr<File> f;
    f.reset(new FileArchive(mb.release(), _options, ec));
    if (ec)
      return ec;

    result.push_back(std::move(f));
  }
  return llvm::error_code::success();
}

} // namespace lld
