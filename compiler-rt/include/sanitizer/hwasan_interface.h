//===-- sanitizer/asan_interface.h ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of HWAddressSanitizer.
//
// Public interface header.
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_HWASAN_INTERFACE_H
#define SANITIZER_HWASAN_INTERFACE_H

#include <sanitizer/common_interface_defs.h>

#ifdef __cplusplus
extern "C" {
#endif
  // Initialize shadow but not the rest of the runtime.
  // Does not call libc unless there is an error.
  // Can be called multiple times, or not at all (in which case shadow will
  // be initialized in compiler-inserted __hwasan_init() call).
  void __hwasan_shadow_init(void);

  // This function may be optionally provided by user and should return
  // a string containing HWASan runtime options. See asan_flags.h for details.
  const char* __hwasan_default_options(void);

  void __hwasan_enable_allocator_tagging(void);
  void __hwasan_disable_allocator_tagging(void);

  int __sanitizer_posix_memalign(void **memptr, size_t alignment, size_t size);
  void * __sanitizer_memalign(size_t alignment, size_t size);
  void * __sanitizer_aligned_alloc(size_t alignment, size_t size);
  void * __sanitizer___libc_memalign(size_t alignment, size_t size);
  void * __sanitizer_valloc(size_t size);
  void * __sanitizer_pvalloc(size_t size);
  void __sanitizer_free(void *ptr);
  void __sanitizer_cfree(void *ptr);
  size_t __sanitizer_malloc_usable_size(const void *ptr);
  struct mallinfo __sanitizer_mallinfo();
  int __sanitizer_mallopt(int cmd, int value);
  void __sanitizer_malloc_stats(void);
  void * __sanitizer_calloc(size_t nmemb, size_t size);
  void * __sanitizer_realloc(void *ptr, size_t size);
  void * __sanitizer_malloc(size_t size);
#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SANITIZER_HWASAN_INTERFACE_H
