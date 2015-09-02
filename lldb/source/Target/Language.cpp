//===-- Language.cpp -------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <functional>
#include <map>
#include <mutex>

#include "lldb/Target/Language.h"

#include "lldb/Host/Mutex.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Stream.h"

using namespace lldb;
using namespace lldb_private;

typedef std::unique_ptr<Language> LanguageUP;
typedef std::map<lldb::LanguageType, LanguageUP> LanguagesMap;

static LanguagesMap&
GetLanguagesMap ()
{
    static LanguagesMap *g_map = nullptr;
    static std::once_flag g_initialize;
    
    std::call_once(g_initialize, [] {
        g_map = new LanguagesMap(); // NOTE: INTENTIONAL LEAK due to global destructor chain
    });
    
    return *g_map;
}
static Mutex&
GetLanguagesMutex ()
{
    static Mutex *g_mutex = nullptr;
    static std::once_flag g_initialize;
    
    std::call_once(g_initialize, [] {
        g_mutex = new Mutex(); // NOTE: INTENTIONAL LEAK due to global destructor chain
    });
    
    return *g_mutex;
}

Language*
Language::FindPlugin (lldb::LanguageType language)
{
    Mutex::Locker locker(GetLanguagesMutex());
    LanguagesMap& map(GetLanguagesMap());
    auto iter = map.find(language), end = map.end();
    if (iter != end)
        return iter->second.get();
    
    Language *language_ptr = nullptr;
    LanguageCreateInstance create_callback;
    
    for (uint32_t idx = 0;
         (create_callback = PluginManager::GetLanguageCreateCallbackAtIndex(idx)) != nullptr;
         ++idx)
    {
        language_ptr = create_callback(language);
        
        if (language_ptr)
        {
            map[language] = std::unique_ptr<Language>(language_ptr);
            return language_ptr;
        }
    }
    
    return nullptr;
}

void
Language::ForEach (std::function<bool(Language*)> callback)
{
    Mutex::Locker locker(GetLanguagesMutex());
    LanguagesMap& map(GetLanguagesMap());
    for (const auto& entry : map)
    {
        if (!callback(entry.second.get()))
            break;
    }
}

lldb::TypeCategoryImplSP
Language::GetFormatters ()
{
    return nullptr;
}

struct language_name_pair {
    const char *name;
    LanguageType type;
};

struct language_name_pair language_names[] =
{
    // To allow GetNameForLanguageType to be a simple array lookup, the first
    // part of this array must follow enum LanguageType exactly.
    {   "unknown",          eLanguageTypeUnknown        },
    {   "c89",              eLanguageTypeC89            },
    {   "c",                eLanguageTypeC              },
    {   "ada83",            eLanguageTypeAda83          },
    {   "c++",              eLanguageTypeC_plus_plus    },
    {   "cobol74",          eLanguageTypeCobol74        },
    {   "cobol85",          eLanguageTypeCobol85        },
    {   "fortran77",        eLanguageTypeFortran77      },
    {   "fortran90",        eLanguageTypeFortran90      },
    {   "pascal83",         eLanguageTypePascal83       },
    {   "modula2",          eLanguageTypeModula2        },
    {   "java",             eLanguageTypeJava           },
    {   "c99",              eLanguageTypeC99            },
    {   "ada95",            eLanguageTypeAda95          },
    {   "fortran95",        eLanguageTypeFortran95      },
    {   "pli",              eLanguageTypePLI            },
    {   "objective-c",      eLanguageTypeObjC           },
    {   "objective-c++",    eLanguageTypeObjC_plus_plus },
    {   "upc",              eLanguageTypeUPC            },
    {   "d",                eLanguageTypeD              },
    {   "python",           eLanguageTypePython         },
    {   "opencl",           eLanguageTypeOpenCL         },
    {   "go",               eLanguageTypeGo             },
    {   "modula3",          eLanguageTypeModula3        },
    {   "haskell",          eLanguageTypeHaskell        },
    {   "c++03",            eLanguageTypeC_plus_plus_03 },
    {   "c++11",            eLanguageTypeC_plus_plus_11 },
    {   "ocaml",            eLanguageTypeOCaml          },
    {   "rust",             eLanguageTypeRust           },
    {   "c11",              eLanguageTypeC11            },
    {   "swift",            eLanguageTypeSwift          },
    {   "julia",            eLanguageTypeJulia          },
    {   "dylan",            eLanguageTypeDylan          },
    {   "c++14",            eLanguageTypeC_plus_plus_14 },
    {   "fortran03",        eLanguageTypeFortran03      },
    {   "fortran08",        eLanguageTypeFortran08      },
    // Vendor Extensions
    {   "mipsassem",        eLanguageTypeMipsAssembler  },
    {   "renderscript",     eLanguageTypeExtRenderScript},
    // Now synonyms, in arbitrary order
    {   "objc",             eLanguageTypeObjC           },
    {   "objc++",           eLanguageTypeObjC_plus_plus },
    {   "pascal",           eLanguageTypePascal83       }
};

static uint32_t num_languages = sizeof(language_names) / sizeof (struct language_name_pair);

LanguageType
Language::GetLanguageTypeFromString (const char *string)
{
    for (uint32_t i = 0; i < num_languages; i++)
    {
        if (strcasecmp (language_names[i].name, string) == 0)
            return (LanguageType) language_names[i].type;
    }
    return eLanguageTypeUnknown;
}

const char *
Language::GetNameForLanguageType (LanguageType language)
{
    if (language < num_languages)
        return language_names[language].name;
    else
        return language_names[eLanguageTypeUnknown].name;
}

void
Language::PrintAllLanguages (Stream &s, const char *prefix, const char *suffix)
{
    for (uint32_t i = 1; i < num_languages; i++)
    {
        s.Printf("%s%s%s", prefix, language_names[i].name, suffix);
    }
}

bool
Language::LanguageIsCPlusPlus (LanguageType language)
{
    switch (language)
    {
        case eLanguageTypeC_plus_plus:
        case eLanguageTypeC_plus_plus_03:
        case eLanguageTypeC_plus_plus_11:
        case eLanguageTypeC_plus_plus_14:
            return true;
        default:
            return false;
    }
}

bool
Language::LanguageIsObjC (LanguageType language)
{
    switch (language)
    {
        case eLanguageTypeObjC:
        case eLanguageTypeObjC_plus_plus:
            return true;
        default:
            return false;
    }
}

bool
Language::LanguageIsC (LanguageType language)
{
    switch (language)
    {
        case eLanguageTypeC:
        case eLanguageTypeC89:
        case eLanguageTypeC99:
        case eLanguageTypeC11:
            return true;
        default:
            return false;
    }
}

bool
Language::LanguageIsPascal (LanguageType language)
{
    switch (language)
    {
        case eLanguageTypePascal83:
            return true;
        default:
            return false;
    }
}

//----------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------
Language::Language()
{
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
Language::~Language()
{
}
