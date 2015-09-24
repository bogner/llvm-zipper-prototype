# On OS X SDKs can be installed anywhere on the base system and xcode-select can
# set the default Xcode to use. This function finds the SDKs that are present in
# the current Xcode.
function(find_darwin_sdk_dir var sdk_name)
  # Let's first try the internal SDK, otherwise use the public SDK.
  execute_process(
    COMMAND xcodebuild -version -sdk ${sdk_name}.internal Path
    OUTPUT_VARIABLE var_internal
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_FILE /dev/null
  )
  if("" STREQUAL "${var_internal}")
    execute_process(
      COMMAND xcodebuild -version -sdk ${sdk_name} Path
      OUTPUT_VARIABLE var_internal
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_FILE /dev/null
    )
  endif()
  set(${var} ${var_internal} PARENT_SCOPE)
endfunction()

# There isn't a clear mapping of what architectures are supported with a given
# target platform, but ld's version output does list the architectures it can
# link for.
function(darwin_get_toolchain_supported_archs output_var)
  execute_process(
    COMMAND ld -v
    ERROR_VARIABLE LINKER_VERSION)

  string(REGEX MATCH "configured to support archs: ([^\n]+)"
         ARCHES_MATCHED "${LINKER_VERSION}")
  if(ARCHES_MATCHED)
    set(ARCHES "${CMAKE_MATCH_1}")
    message(STATUS "Got ld supported ARCHES: ${ARCHES}")
    string(REPLACE " " ";" ARCHES ${ARCHES})
  else()
    # If auto-detecting fails, fall back to a default set
    message(WARNING "Detecting supported architectures from 'ld -v' failed. Returning default set.")
    set(ARCHES "i386;x86_64;armv7;armv7s;arm64")
  endif()
  
  set(${output_var} ${ARCHES} PARENT_SCOPE)
endfunction()

# This function takes an OS and a list of architectures and identifies the
# subset of the architectures list that the installed toolchain can target.
function(darwin_test_archs os valid_archs)
  set(archs ${ARGN})
  message(STATUS "Finding valid architectures for ${os}...")
  set(SIMPLE_CPP ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/src.cpp)
  file(WRITE ${SIMPLE_CPP} "#include <iostream>\nint main() { std::cout << std::endl; return 0; }\n")

  set(os_linker_flags)
  foreach(flag ${DARWIN_${os}_LINKFLAGS})
    set(os_linker_flags "${os_linker_flags} ${flag}")
  endforeach()

  # The simple program will build for x86_64h on the simulator because it is 
  # compatible with x86_64 libraries (mostly), but since x86_64h isn't actually
  # a valid or useful architecture for the iOS simulator we should drop it.
  if(${os} STREQUAL "iossim")
    list(REMOVE_ITEM archs "x86_64h")
  endif()

  set(working_archs)
  foreach(arch ${archs})
    
    set(arch_linker_flags "-arch ${arch} ${os_linker_flags}")
    try_compile(CAN_TARGET_${os}_${arch} ${CMAKE_BINARY_DIR} ${SIMPLE_CPP}
                COMPILE_DEFINITIONS "-v -arch ${arch}" ${DARWIN_${os}_CFLAGS}
                CMAKE_FLAGS "-DCMAKE_EXE_LINKER_FLAGS=${arch_linker_flags}"
                OUTPUT_VARIABLE TEST_OUTPUT)
    if(${CAN_TARGET_${os}_${arch}})
      list(APPEND working_archs ${arch})
    endif()
  endforeach()
  set(${valid_archs} ${working_archs} PARENT_SCOPE)
endfunction()

# This function checks the host cpusubtype to see if it is post-haswell. Haswell
# and later machines can run x86_64h binaries. Haswell is cpusubtype 8.
function(darwin_filter_host_archs input output)
  list_union(tmp_var DARWIN_osx_ARCHS ${input})
  execute_process(
    COMMAND sysctl hw.cpusubtype
    OUTPUT_VARIABLE SUBTYPE)

  string(REGEX MATCH "hw.cpusubtype: ([0-9]*)"
         SUBTYPE_MATCHED "${SUBTYPE}")
  set(HASWELL_SUPPORTED Off)
  if(SUBTYPE_MATCHED)
    if(${CMAKE_MATCH_1} GREATER 7)
      set(HASWELL_SUPPORTED On)
    endif()
  endif()
  if(NOT HASWELL_SUPPORTED)
    list(REMOVE_ITEM tmp_var x86_64h)
  endif()
  set(${output} ${tmp_var} PARENT_SCOPE)
endfunction()

set(DARWIN_EXCLUDE_DIR ${CMAKE_SOURCE_DIR}/lib/builtins/Darwin-excludes)

# Read and process the exclude file into a list of symbols
function(darwin_read_exclude_file output_var file)
  if(EXISTS ${DARWIN_EXCLUDE_DIR}/${file}.txt)
    file(READ ${DARWIN_EXCLUDE_DIR}/${file}.txt ${file}_EXCLUDES)
    string(REPLACE "\n" ";" ${file}_EXCLUDES ${${file}_EXCLUDES})
    set(${output_var} ${${file}_EXCLUDES} PARENT_SCOPE)
  endif()
endfunction()

# this function takes an OS, architecture and minimum version and provides a
# list of builtin functions to exclude
function(darwin_find_excluded_builtins_list output_var)
  cmake_parse_arguments(LIB
    ""
    "OS;ARCH;MIN_VERSION"
    ""
    ${ARGN})

  if(NOT LIB_OS OR NOT LIB_ARCH)
    message(FATAL_ERROR "Must specify OS and ARCH to darwin_find_excluded_builtins_list!")
  endif()

  darwin_read_exclude_file(${LIB_OS}_BUILTINS ${LIB_OS})
  darwin_read_exclude_file(${LIB_OS}_${LIB_ARCH}_BASE_BUILTINS ${LIB_OS}-${LIB_ARCH})

  if(LIB_MIN_VERSION)
    file(GLOB builtin_lists ${DARWIN_EXCLUDE_DIR}/${LIB_OS}*-${LIB_ARCH}.txt)
    foreach(builtin_list ${builtin_lists})
      string(REGEX MATCH "${LIB_OS}([0-9\\.]*)-${LIB_ARCH}.txt" VERSION_MATCHED "${builtin_list}")
      if (VERSION_MATCHED AND NOT CMAKE_MATCH_1 VERSION_LESS LIB_MIN_VERSION)
        if(NOT smallest_version)
          set(smallest_version ${CMAKE_MATCH_1})
        elseif(CMAKE_MATCH_1 VERSION_LESS smallest_version)
          set(smallest_version ${CMAKE_MATCH_1})
        endif()
      endif()
    endforeach()

    if(smallest_version)
      darwin_read_exclude_file(${LIB_ARCH}_${LIB_OS}_BUILTINS ${LIB_OS}${smallest_version}-${LIB_ARCH})
    endif()
  endif()
  
  set(${output_var}
      ${${LIB_ARCH}_${LIB_OS}_BUILTINS}
      ${${LIB_OS}_${LIB_ARCH}_BASE_BUILTINS}
      ${${LIB_OS}_BUILTINS} PARENT_SCOPE)
endfunction()

# adds a single builtin library for a single OS & ARCH
macro(darwin_add_builtin_library name suffix)
  cmake_parse_arguments(LIB
    ""
    "PARENT_TARGET;OS;ARCH"
    "SOURCES;CFLAGS;DEFS"
    ${ARGN})
  set(libname "${name}.${suffix}_${LIB_ARCH}_${LIB_OS}")
  add_library(${libname} STATIC ${LIB_SOURCES})
  set_target_compile_flags(${libname}
    -isysroot ${DARWIN_${LIB_OS}_SYSROOT}
    ${DARWIN_${LIB_OS}_BUILTIN_MIN_VER_FLAG}
    ${LIB_CFLAGS})
  set_property(TARGET ${libname} APPEND PROPERTY
      COMPILE_DEFINITIONS ${LIB_DEFS})
  set_target_properties(${libname} PROPERTIES
      OUTPUT_NAME ${libname}${COMPILER_RT_OS_SUFFIX})
  set_target_properties(${libname} PROPERTIES
    OSX_ARCHITECTURES ${LIB_ARCH})

  if(LIB_PARENT_TARGET)
    add_dependencies(${LIB_PARENT_TARGET} ${libname})
  endif()

  list(APPEND ${os}_${suffix}_libs ${libname})
  list(APPEND ${os}_${suffix}_lipo_flags -arch ${arch} $<TARGET_FILE:${libname}>)
endmacro()

function(darwin_lipo_libs name)
  cmake_parse_arguments(LIB
    ""
    "PARENT_TARGET"
    "LIPO_FLAGS;DEPENDS"
    ${ARGN})
  add_custom_command(OUTPUT ${COMPILER_RT_LIBRARY_OUTPUT_DIR}/lib${name}.a
    COMMAND lipo -output
            ${COMPILER_RT_LIBRARY_OUTPUT_DIR}/lib${name}.a
            -create ${LIB_LIPO_FLAGS}
    DEPENDS ${LIB_DEPENDS}
    )
  add_custom_target(${name}
    DEPENDS ${COMPILER_RT_LIBRARY_OUTPUT_DIR}/lib${name}.a)
  add_dependencies(${LIB_PARENT_TARGET} ${name})
endfunction()

# Filter out generic versions of routines that are re-implemented in
# architecture specific manner.  This prevents multiple definitions of the
# same symbols, making the symbol selection non-deterministic.
function(darwin_filter_builtin_sources output_var excluded_list)
  set(intermediate ${ARGN})
  foreach (_file ${intermediate})
    get_filename_component(_name_we ${_file} NAME_WE)
    list(FIND ${excluded_list} ${_name_we} _found)
    if(_found GREATER -1)
      list(REMOVE_ITEM intermediate ${_file})
    elseif(${_file} MATCHES ".*/.*\\.S")
      get_filename_component(_name ${_file} NAME)
      string(REPLACE ".S" ".c" _cname "${_name}")
      list(REMOVE_ITEM intermediate ${_cname})
    endif ()
  endforeach ()
  set(${output_var} ${intermediate} PARENT_SCOPE)
endfunction()

# Generates builtin libraries for all operating systems specified in ARGN. Each
# OS library is constructed by lipo-ing together single-architecture libraries.
macro(darwin_add_builtin_libraries)
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(type ${CMAKE_CONFIGURATION_TYPES})
      set(CMAKE_C_FLAGS_${type} -O3)
      set(CMAKE_CXX_FLAGS_${type} -O3)
    endforeach()
  else()
    set(CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE} -O3)
  endif()

  set(CMAKE_C_FLAGS "-fPIC -fvisibility=hidden -DVISIBILITY_HIDDEN -Wall -fomit-frame-pointer")
  set(CMAKE_CXX_FLAGS ${CMAKE_C_FLAGS})
  set(CMAKE_ASM_FLAGS ${CMAKE_C_FLAGS})
  foreach (os ${ARGN})
    list_union(DARWIN_BUILTIN_ARCHS DARWIN_${os}_ARCHS BUILTIN_SUPPORTED_ARCH)
    foreach (arch ${DARWIN_BUILTIN_ARCHS})
      # do cc_kext
      darwin_add_builtin_library(clang_rt cc_kext
                              OS ${os}
                              ARCH ${arch}
                              SOURCES ${${arch}_SOURCES}
                              CFLAGS -arch ${arch} -mkernel
                              DEFS KERNEL_USE
                              PARENT_TARGET builtins)


      darwin_find_excluded_builtins_list(${arch}_${os}_EXCLUDED_BUILTINS
                              OS ${os}
                              ARCH ${arch}
                              MIN_VERSION ${DARWIN_${os}_BUILTIN_MIN_VER})

      darwin_filter_builtin_sources(filtered_sources ${arch}_${os}_EXCLUDED_BUILTINS ${${arch}_SOURCES})

      darwin_add_builtin_library(clang_rt builtins
                              OS ${os}
                              ARCH ${arch}
                              SOURCES ${filtered_sources}
                              CFLAGS -arch ${arch}
                              PARENT_TARGET builtins)
    endforeach()

    darwin_lipo_libs(clang_rt.cc_kext_${os}
                    PARENT_TARGET builtins
                    LIPO_FLAGS ${${os}_cc_kext_lipo_flags}
                    DEPENDS ${${os}_cc_kext_libs})
    darwin_lipo_libs(clang_rt.${os}
                    PARENT_TARGET builtins
                    LIPO_FLAGS ${${os}_builtins_lipo_flags}
                    DEPENDS ${${os}_builtins_libs})
  endforeach()
endmacro()

