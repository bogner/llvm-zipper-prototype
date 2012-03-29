//===- tools/lld/lld-core.cpp - Linker Core Test Driver -----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Core/Atom.h"
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

static void error(llvm::Twine message) {
  llvm::errs() << "lld-core: " << message << ".\n";
}

static bool error(llvm::error_code ec) {
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

  virtual llvm::StringRef name() const {
    return llvm::StringRef();
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
    
  virtual llvm::StringRef customSectionName() const {
    return llvm::StringRef();
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
  
  virtual llvm::ArrayRef<uint8_t> rawContent() const {
    return llvm::ArrayRef<uint8_t>();
  }
  
  virtual reference_iterator referencesBegin() const {
    return reference_iterator(*this, NULL);
  }
  
  virtual reference_iterator referencesEnd() const {
    return reference_iterator(*this, NULL);
  }
  
  virtual const Reference* derefIterator(const void* iter) const {
    return NULL;
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

  virtual llvm::StringRef name() const {
    return llvm::StringRef();
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
    
  virtual llvm::StringRef customSectionName() const {
    return llvm::StringRef();
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
  
  virtual llvm::ArrayRef<uint8_t> rawContent() const {
    return llvm::ArrayRef<uint8_t>();
  }
  
  virtual reference_iterator referencesBegin() const {
    return reference_iterator(*this, NULL);
  }
  
  virtual reference_iterator referencesEnd() const {
    return reference_iterator(*this, NULL);
  }
  
  virtual const Reference* derefIterator(const void* iter) const {
    return NULL;
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

  virtual void initialize() { }

  // tell platform object another file has been added
  virtual void fileAdded(const File &file) { }

  // tell platform object another atom has been added
  virtual void atomAdded(const Atom &file) { }

  // give platform a chance to change each atom's scope
  virtual void adjustScope(const DefinedAtom &atom) { }

  // if specified atom needs alternate names, return AliasAtom(s)
  virtual bool getAliasAtoms(const Atom &atom,
                             std::vector<const DefinedAtom *>&) {
    return false;
  }

  // give platform a chance to resolve platform-specific undefs
  virtual bool getPlatformAtoms(llvm::StringRef undefined,
                                std::vector<const DefinedAtom *>&) {
    return false;
  }

  // resolver should remove unreferenced atoms
  virtual bool deadCodeStripping() {
    return false;
  }

  // atom must be kept so should be root of dead-strip graph
  virtual bool isDeadStripRoot(const Atom &atom) {
    return false;
  }

  // if target must have some atoms, denote here
  virtual bool getImplicitDeadStripRoots(std::vector<const DefinedAtom *>&) {
    return false;
  }

  // return entry point for output file (e.g. "main") or NULL
  virtual llvm::StringRef entryPointName() {
    return NULL;
  }

  // for iterating must-be-defined symbols ("main" or -u command line option)
  typedef llvm::StringRef const *UndefinesIterator;
  virtual UndefinesIterator initialUndefinesBegin() const {
    return NULL;
  }
  virtual UndefinesIterator initialUndefinesEnd() const {
    return NULL;
  }

  // if platform wants resolvers to search libraries for overrides
  virtual bool searchArchivesToOverrideTentativeDefinitions() {
    return false;
  }

  virtual bool searchSharedLibrariesToOverrideTentativeDefinitions() {
    return false;
  }

  // if platform allows symbol to remain undefined (e.g. -r)
  virtual bool allowUndefinedSymbol(llvm::StringRef name) {
    return true;
  }

  // for debugging dead code stripping, -why_live
  virtual bool printWhyLive(llvm::StringRef name) {
    return false;
  }

  virtual const Atom& handleMultipleDefinitions(const Atom& def1, 
                                                const Atom& def2) {
    llvm::report_fatal_error("symbol '" 
                            + llvm::Twine(def1.name()) 
                            + "' multiply defined");
  }

  // print out undefined symbol error messages in platform specific way
  virtual void errorWithUndefines(const std::vector<const Atom *> &undefs,
                                  const std::vector<const Atom *> &all) {}

  // print out undefined can-be-null mismatches
  virtual void undefineCanBeNullMismatch(const UndefinedAtom& undef1,
                                         const UndefinedAtom& undef2,
                                         bool& useUndef2) { }

  // print out shared library mismatches
  virtual void sharedLibrarylMismatch(const SharedLibraryAtom& shLib1,
                                      const SharedLibraryAtom& shLib2,
                                      bool& useShlib2) { }


  // last chance for platform to tweak atoms
  virtual void postResolveTweaks(std::vector<const Atom *> &all) {}
  

  struct KindMapping {
    const char*           string;
    Reference::Kind       value;
    bool                  isBranch;
    bool                  isGotLoad;
    bool                  isGotUse;
  };

  static const KindMapping _s_kindMappings[]; 
  
  virtual Reference::Kind kindFromString(llvm::StringRef kindName) {
    for (const KindMapping* p = _s_kindMappings; p->string != NULL; ++p) {
      if ( kindName.equals(p->string) )
        return p->value;
    }
    int k;
    if (kindName.getAsInteger(0, k))
      k = 0;
    return k;
  }
  
  virtual llvm::StringRef kindToString(Reference::Kind value) {
    for (const KindMapping* p = _s_kindMappings; p->string != NULL; ++p) {
      if ( value == p->value)
        return p->string;
    }
    return llvm::StringRef("???");
  }

  virtual bool noTextRelocs() {
    return true;
  }
  
  virtual bool isCallSite(Reference::Kind kind) {
    for (const KindMapping* p = _s_kindMappings; p->string != NULL; ++p) {
      if ( kind == p->value )
        return p->isBranch;
    }
    return false;
  }

  virtual bool isGOTAccess(Reference::Kind kind, bool& canBypassGOT) {
    for (const KindMapping* p = _s_kindMappings; p->string != NULL; ++p) {
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



  virtual const DefinedAtom* makeStub(const Atom& shlibAtom, File& file) {
    return new TestingStubAtom(file, shlibAtom);
  }
  
  virtual const DefinedAtom* makeGOTEntry(const Atom& shlibAtom, File& file) {
    return new TestingGOTAtom(file, shlibAtom);
  }

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
    { NULL,             0,    false, false, false }
  };



//
// A simple input files wrapper for testing.
//
class TestingInputFiles : public InputFiles {
public:
  TestingInputFiles(std::vector<const File*>& f) : _files(f) { }

  // InputFiles interface
  virtual void forEachInitialAtom(InputFiles::Handler& handler) const {
    for (std::vector<const File *>::iterator fit = _files.begin();
                                       fit != _files.end(); ++fit) {
      const File* file = *fit;
      handler.doFile(*file);
      for(auto it=file->definedAtomsBegin(),end=file->definedAtomsEnd(); 
                                 it != end; ++it) {
        handler.doDefinedAtom(**it);
      }
      for(auto it=file->undefinedAtomsBegin(), end=file->undefinedAtomsEnd(); 
                                 it != end; ++it) {
        handler.doUndefinedAtom(**it);
      }
      for(auto it=file->sharedLibraryAtomsBegin(), end=file->sharedLibraryAtomsEnd(); 
                                 it != end; ++it) {
        handler.doSharedLibraryAtom(**it);
      }
      for(auto it=file->absoluteAtomsBegin(),end=file->absoluteAtomsEnd(); 
                                 it != end; ++it) {
        handler.doAbsoluteAtom(**it);
      }
    }
  }

  virtual bool searchLibraries(llvm::StringRef name, bool searchDylibs,
                               bool searchArchives, bool dataSymbolOnly,
                               InputFiles::Handler &) const {
    return false;
  }


private:
  std::vector<const File*>&        _files;
};
}






llvm::cl::opt<std::string> 
gInputFilePath(llvm::cl::Positional,
              llvm::cl::desc("<input file>"),
              llvm::cl::init("-"));

llvm::cl::opt<std::string> 
gOutputFilePath("o", 
              llvm::cl::desc("Specify output filename"), 
              llvm::cl::value_desc("filename"));

llvm::cl::opt<bool> 
gDoStubsPass("stubs_pass", 
          llvm::cl::desc("Run pass to create stub atoms"));

llvm::cl::opt<bool> 
gDoGotPass("got_pass", 
          llvm::cl::desc("Run pass to create GOT atoms"));

int main(int argc, char *argv[]) {
  // Print a stack trace if we signal out.
  llvm::sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // parse options
  llvm::cl::ParseCommandLineOptions(argc, argv);

  // create platform for testing
  TestingPlatform testingPlatform;
  
  // read input YAML doc into object file(s)
  std::vector<const File *> files;
  if (error(yaml::parseObjectTextFileOrSTDIN(gInputFilePath, 
                                            testingPlatform, files))) {
    return 1;
  }
  
  // create object to mange input files
  TestingInputFiles inputFiles(files);
  
  // merge all atom graphs
  Resolver resolver(testingPlatform, inputFiles);
  resolver.resolve();

  // run passes
  if ( gDoGotPass ) {
    GOTPass  addGot(resolver.resultFile(), testingPlatform);
    addGot.perform();
  }
  if ( gDoStubsPass ) {
    StubsPass  addStubs(resolver.resultFile(), testingPlatform);
    addStubs.perform();
  }

  
  // write new atom graph out as YAML doc
  std::string errorInfo;
  const char* outPath = gOutputFilePath.empty() ? "-" : gOutputFilePath.c_str();
  llvm::raw_fd_ostream out(outPath, errorInfo);
//  yaml::writeObjectText(resolver.resultFile(), out);

  // make unique temp .o file to put generated object file
  int fd;
  llvm::SmallString<128> tempPath;
  llvm::sys::fs::unique_file("temp%%%%%.o", fd, tempPath);
  llvm::raw_fd_ostream  binaryOut(fd, /*shouldClose=*/true);
  
  // write native file
  writeNativeObjectFile(resolver.resultFile(), binaryOut);
  binaryOut.close();  // manually close so that file can be read next

//  out << "native file: " << tempPath.str() << "\n";
  
  // read native file
  llvm::OwningPtr<lld::File> natFile;
  if ( error(parseNativeObjectFileOrSTDIN(tempPath, natFile)) ) 
	return 1;
	
  // write new atom graph out as YAML doc
  yaml::writeObjectText(*natFile, testingPlatform, out);

  // delete temp .o file
  bool existed;
  llvm::sys::fs::remove(tempPath.str(), existed);
  
  return 0;
}
