//===- tools/lld/lld-core.cpp - Linker Core Test Driver -----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Core/Atom.h"
#include "lld/Core/LLVM.h"
#include "lld/Core/NativeReader.h"
#include "lld/Core/NativeWriter.h"
#include "lld/Core/Pass.h"
#include "lld/Core/Resolver.h"
#include "lld/Core/YamlReader.h"
#include "lld/Core/YamlWriter.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/system_error.h"

#include <vector>

using namespace lld;

static void error(Twine message) {
  llvm::errs() << "lld-core: " << message << ".\n";
}

static bool error(error_code ec) {
  if (ec) {
    error(ec.message());
    return true;
  }
  return false;
}

namespace {


//
// Simple atom created by the stubs pass.
//
class TestingStubAtom : public DefinedAtom {
public:
        TestingStubAtom(const File& f, const Atom& shlib) :
                        _file(f), _shlib(shlib) {
          static uint32_t lastOrdinal = 0;
          _ordinal = lastOrdinal++; 
        }

  virtual const File& file() const {
    return _file;
  }

  virtual StringRef name() const {
    return StringRef();
  }
  
  virtual uint64_t ordinal() const {
    return _ordinal;
  }

  virtual uint64_t size() const {
    return 0;
  }

  virtual Scope scope() const {
    return DefinedAtom::scopeLinkageUnit;
  }
  
  virtual Interposable interposable() const {
    return DefinedAtom::interposeNo;
  }
  
  virtual Merge merge() const {
    return DefinedAtom::mergeNo;
  }
  
  virtual ContentType contentType() const  {
    return DefinedAtom::typeStub;
  }

  virtual Alignment alignment() const {
    return Alignment(0,0);
  }
  
  virtual SectionChoice sectionChoice() const {
    return DefinedAtom::sectionBasedOnContent;
  }
    
  virtual StringRef customSectionName() const {
    return StringRef();
  }
  virtual DeadStripKind deadStrip() const {
    return DefinedAtom::deadStripNormal;
  }
    
  virtual ContentPermissions permissions() const  {
    return DefinedAtom::permR_X;
  }
  
  virtual bool isThumb() const {
    return false;
  }
    
  virtual bool isAlias() const {
    return false;
  }
  
  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>();
  }
  
  virtual reference_iterator begin() const {
    return reference_iterator(*this, nullptr);
  }
  
  virtual reference_iterator end() const {
    return reference_iterator(*this, nullptr);
  }
  
  virtual const Reference* derefIterator(const void* iter) const {
    return nullptr;
  }
  
  virtual void incrementIterator(const void*& iter) const {
  
  }
  
private:
  const File&               _file;
  const Atom&               _shlib;
  uint32_t                  _ordinal;
};




//
// Simple atom created by the GOT pass.
//
class TestingGOTAtom : public DefinedAtom {
public:
        TestingGOTAtom(const File& f, const Atom& shlib) :
                        _file(f), _shlib(shlib) {
          static uint32_t lastOrdinal = 0;
          _ordinal = lastOrdinal++; 
        }

  virtual const File& file() const {
    return _file;
  }

  virtual StringRef name() const {
    return StringRef();
  }
  
  virtual uint64_t ordinal() const {
    return _ordinal;
  }

  virtual uint64_t size() const {
    return 0;
  }

  virtual Scope scope() const {
    return DefinedAtom::scopeLinkageUnit;
  }
  
  virtual Interposable interposable() const {
    return DefinedAtom::interposeNo;
  }
  
  virtual Merge merge() const {
    return DefinedAtom::mergeNo;
  }
  
  virtual ContentType contentType() const  {
    return DefinedAtom::typeGOT;
  }

  virtual Alignment alignment() const {
    return Alignment(3,0);
  }
  
  virtual SectionChoice sectionChoice() const {
    return DefinedAtom::sectionBasedOnContent;
  }
    
  virtual StringRef customSectionName() const {
    return StringRef();
  }
  virtual DeadStripKind deadStrip() const {
    return DefinedAtom::deadStripNormal;
  }
    
  virtual ContentPermissions permissions() const  {
    return DefinedAtom::permRW_;
  }
  
  virtual bool isThumb() const {
    return false;
  }
    
  virtual bool isAlias() const {
    return false;
  }
  
  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>();
  }
  
  virtual reference_iterator begin() const {
    return reference_iterator(*this, nullptr);
  }
  
  virtual reference_iterator end() const {
    return reference_iterator(*this, nullptr);
  }
  
  virtual const Reference* derefIterator(const void* iter) const {
    return nullptr;
  }
  
  virtual void incrementIterator(const void*& iter) const {
  
  }
  
private:
  const File&               _file;
  const Atom&               _shlib;
  uint32_t                  _ordinal;
};

//
// A simple platform for testing.
//
class TestingPlatform : public Platform {
public:

  virtual void addFiles(InputFiles&) {
  }

  struct KindMapping {
    const char*           string;
    Reference::Kind       value;
    bool                  isBranch;
    bool                  isGotLoad;
    bool                  isGotUse;
  };

  static const KindMapping _s_kindMappings[]; 
  
  virtual Reference::Kind kindFromString(StringRef kindName) {
    for (const KindMapping* p = _s_kindMappings; p->string != nullptr; ++p) {
      if ( kindName.equals(p->string) )
        return p->value;
    }
    int k;
    if (kindName.getAsInteger(0, k))
      k = 0;
    return k;
  }
  
  virtual StringRef kindToString(Reference::Kind value) {
    for (const KindMapping* p = _s_kindMappings; p->string != nullptr; ++p) {
      if ( value == p->value)
        return p->string;
    }
    return StringRef("???");
  }

  virtual bool noTextRelocs() {
    return true;
  }
  
  virtual bool isCallSite(Reference::Kind kind) {
    for (const KindMapping* p = _s_kindMappings; p->string != nullptr; ++p) {
      if ( kind == p->value )
        return p->isBranch;
    }
    return false;
  }

  virtual bool isGOTAccess(Reference::Kind kind, bool& canBypassGOT) {
    for (const KindMapping* p = _s_kindMappings; p->string != nullptr; ++p) {
      if ( kind == p->value ) {
        canBypassGOT = p->isGotLoad;
        return p->isGotUse;
      }
    }
    return false;
  }
  
  virtual void updateReferenceToGOT(const Reference* ref, bool targetIsNowGOT) {
    if ( targetIsNowGOT )
      (const_cast<Reference*>(ref))->setKind(kindFromString("pcrel32"));
    else
      (const_cast<Reference*>(ref))->setKind(kindFromString("lea32wasGot"));
  }



  virtual const DefinedAtom *getStub(const Atom& shlibAtom, File& file) {
    const DefinedAtom *result = new TestingStubAtom(file, shlibAtom);
    _stubs.push_back(result);
    return result;
  }
  
  virtual const DefinedAtom* makeGOTEntry(const Atom& shlibAtom, File& file) {
    return new TestingGOTAtom(file, shlibAtom);
  }
  
  virtual void addStubAtoms(File &file) {
    for (const DefinedAtom *stub : _stubs) {
      file.addAtom(*stub);
    }
  }
  
  virtual void writeExecutable(const lld::File &, raw_ostream &out) {
  }
private:
  std::vector<const DefinedAtom*> _stubs;
};


//
// Table of fixup kinds in YAML documents used for testing
//
const TestingPlatform::KindMapping TestingPlatform::_s_kindMappings[] = {
    { "call32",         1,    true,  false, false},
    { "pcrel32",        2,    false, false, false },
    { "gotLoad32",      3,    false, true,  true },
    { "gotUse32",       4,    false, false, true },
    { "lea32wasGot",    5,    false, false, false },
    { nullptr,          0,    false, false, false }
  };


} // anon namespace


llvm::cl::opt<std::string> 
cmdLineInputFilePath(llvm::cl::Positional,
              llvm::cl::desc("<input file>"),
              llvm::cl::init("-"));

llvm::cl::opt<std::string> 
cmdLineOutputFilePath("o", 
              llvm::cl::desc("Specify output filename"), 
              llvm::cl::value_desc("filename"));

llvm::cl::opt<bool> 
cmdLineDoStubsPass("stubs-pass", 
          llvm::cl::desc("Run pass to create stub atoms"));

llvm::cl::opt<bool> 
cmdLineDoGotPass("got-pass", 
          llvm::cl::desc("Run pass to create GOT atoms"));

llvm::cl::opt<bool> 
cmdLineUndefinesIsError("undefines-are-errors", 
          llvm::cl::desc("Any undefined symbols at end is an error"));

llvm::cl::opt<bool> 
cmdLineCommonsSearchArchives("commons-search-archives", 
          llvm::cl::desc("Tentative definitions trigger archive search"));

llvm::cl::opt<bool> 
cmdLineDeadStrip("dead-strip", 
          llvm::cl::desc("Remove unreachable code and data"));

llvm::cl::opt<bool> 
cmdLineGlobalsNotDeadStrip("keep-globals", 
          llvm::cl::desc("All global symbols are roots for dead-strip"));


enum PlatformChoice {
  platformTesting, platformDarwin
};

llvm::cl::opt<PlatformChoice> 
platformSelected("platform",
  llvm::cl::desc("Select platform"),
  llvm::cl::values(
    clEnumValN(platformTesting, "none", "link for testing"),
    clEnumValN(platformDarwin, "darwin", "link as darwin would"),
    clEnumValEnd));
    


class TestingResolverOptions : public ResolverOptions {
public:
  TestingResolverOptions() {
    _undefinesAreErrors = cmdLineUndefinesIsError;
    _searchArchivesToOverrideTentativeDefinitions = cmdLineCommonsSearchArchives;
    _deadCodeStrip = cmdLineDeadStrip;
    _globalsAreDeadStripRoots = cmdLineGlobalsNotDeadStrip;
  }

};




    
int main(int argc, char *argv[]) {
  // Print a stack trace if we signal out.
  llvm::sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // parse options
  llvm::cl::ParseCommandLineOptions(argc, argv);

  // create platform for testing
  Platform* platform = NULL;
  switch ( platformSelected ) {
    case platformTesting:
      platform = new TestingPlatform();
      break;
    case platformDarwin:
      platform = createDarwinPlatform();
      break;
  }
  
  // read input YAML doc into object file(s)
  std::vector<const File *> files;
  if (error(yaml::parseObjectTextFileOrSTDIN(cmdLineInputFilePath, 
                                            *platform, files))) {
    return 1;
  }

  // create options for resolving
  TestingResolverOptions options;

  // create object to mange input files
  InputFiles inputFiles;
  for (const File *file : files) {
    inputFiles.appendFile(*file);
  }
  
  platform->addFiles(inputFiles);

  // merge all atom graphs
  Resolver resolver(options, inputFiles);
  resolver.resolve();

  // run passes
  if ( cmdLineDoGotPass ) {
    GOTPass  addGot(resolver.resultFile(), *platform);
    addGot.perform();
  }
  if ( cmdLineDoStubsPass ) {
    StubsPass  addStubs(resolver.resultFile(), *platform);
    addStubs.perform();
  }

  
//  yaml::writeObjectText(resolver.resultFile(), *platform, llvm::errs());

  // make unique temp .o file to put generated object file
  int fd;
  SmallString<128> tempPath;
  llvm::sys::fs::unique_file("temp%%%%%.o", fd, tempPath);
  llvm::raw_fd_ostream  binaryOut(fd, /*shouldClose=*/true);
  
  // write native file
  writeNativeObjectFile(resolver.resultFile(), binaryOut);
  binaryOut.close();  // manually close so that file can be read next

//  out << "native file: " << tempPath.str() << "\n";
  
  // read native file
  std::unique_ptr<lld::File> natFile;
  if ( error(parseNativeObjectFileOrSTDIN(tempPath, natFile)) ) 
    return 1;

  // write new atom graph
  std::string errorInfo;
  const char* outPath = (cmdLineOutputFilePath.empty() ? "-" 
                                              : cmdLineOutputFilePath.c_str());
  llvm::raw_fd_ostream out(outPath, errorInfo);
  if ( platformSelected == platformTesting) {
    // write atom graph out as YAML doc
    yaml::writeObjectText(resolver.resultFile() /* *natFile */, *platform, out);
  }
  else {
    // write atom graph as an executable
    platform->writeExecutable(resolver.resultFile() /* *natFile */, out);
    // HACK.  I don't see any way to set the 'executable' bit on files 
    // in raw_fd_ostream or in llvm/Support.
#if HAVE_SYS_STAT_H
    ::chmod(outPath, 0777);
#endif
  }

  // delete temp .o file
  bool existed;
  llvm::sys::fs::remove(tempPath.str(), existed);
  
  return 0;
}
