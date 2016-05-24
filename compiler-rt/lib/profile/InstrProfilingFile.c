/*===- InstrProfilingFile.c - Write instrumentation to a file -------------===*\
|*
|*                     The LLVM Compiler Infrastructure
|*
|* This file is distributed under the University of Illinois Open Source
|* License. See LICENSE.TXT for details.
|*
\*===----------------------------------------------------------------------===*/

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"
#include "InstrProfilingUtil.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
/* For _alloca */
#include <malloc.h>
#endif


#define UNCONST(ptr) ((void *)(uintptr_t)(ptr))

/* Return 1 if there is an error, otherwise return  0.  */
static uint32_t fileWriter(ProfDataIOVec *IOVecs, uint32_t NumIOVecs,
                           void **WriterCtx) {
  uint32_t I;
  FILE *File = (FILE *)*WriterCtx;
  for (I = 0; I < NumIOVecs; I++) {
    if (fwrite(IOVecs[I].Data, IOVecs[I].ElmSize, IOVecs[I].NumElm, File) !=
        IOVecs[I].NumElm)
      return 1;
  }
  return 0;
}

COMPILER_RT_VISIBILITY ProfBufferIO *
lprofCreateBufferIOInternal(void *File, uint32_t BufferSz) {
  FreeHook = &free;
  DynamicBufferIOBuffer = (uint8_t *)calloc(BufferSz, 1);
  VPBufferSize = BufferSz;
  return lprofCreateBufferIO(fileWriter, File);
}

static void setupIOBuffer() {
  const char *BufferSzStr = 0;
  BufferSzStr = getenv("LLVM_VP_BUFFER_SIZE");
  if (BufferSzStr && BufferSzStr[0]) {
    VPBufferSize = atoi(BufferSzStr);
    DynamicBufferIOBuffer = (uint8_t *)calloc(VPBufferSize, 1);
  }
}

static int writeFile(FILE *File) {
  FreeHook = &free;
  setupIOBuffer();
  return lprofWriteData(fileWriter, File, lprofGetVPDataReader());
}

static int writeFileWithName(const char *OutputName) {
  int RetVal;
  FILE *OutputFile;
  if (!OutputName || !OutputName[0])
    return -1;

  /* Append to the file to support profiling multiple shared objects. */
  OutputFile = fopen(OutputName, "ab");
  if (!OutputFile)
    return -1;

  RetVal = writeFile(OutputFile);

  fclose(OutputFile);
  return RetVal;
}

COMPILER_RT_WEAK int __llvm_profile_OwnsFilename = 0;
COMPILER_RT_WEAK const char *__llvm_profile_CurrentFilename = NULL;

static void truncateCurrentFile(void) {
  const char *Filename;
  FILE *File;

  Filename = __llvm_profile_CurrentFilename;
  if (!Filename || !Filename[0])
    return;

  /* Create the directory holding the file, if needed. */
  if (strchr(Filename, '/') || strchr(Filename, '\\')) {
    char *Copy = (char *)COMPILER_RT_ALLOCA(strlen(Filename) + 1);
    strcpy(Copy, Filename);
    __llvm_profile_recursive_mkdir(Copy);
  }

  /* Truncate the file.  Later we'll reopen and append. */
  File = fopen(Filename, "w");
  if (!File)
    return;
  fclose(File);
}

static void setFilename(const char *Filename, int OwnsFilename) {
  /* Check if this is a new filename and therefore needs truncation. */
  int NewFile = !__llvm_profile_CurrentFilename ||
      (Filename && strcmp(Filename, __llvm_profile_CurrentFilename));
  if (__llvm_profile_OwnsFilename)
    free(UNCONST(__llvm_profile_CurrentFilename));

  __llvm_profile_CurrentFilename = Filename;
  __llvm_profile_OwnsFilename = OwnsFilename;

  /* If not a new file, append to support profiling multiple shared objects. */
  if (NewFile)
    truncateCurrentFile();
}

static void resetFilenameToDefault(void) { setFilename("default.profraw", 0); }

int getpid(void);
static int setFilenamePossiblyWithPid(const char *Filename) {
#define MAX_PID_SIZE 16
  char PidChars[MAX_PID_SIZE] = {0};
  int NumPids = 0, PidLength = 0, NumHosts = 0, HostNameLength = 0;
  char *Allocated;
  int I, J;
  char Hostname[COMPILER_RT_MAX_HOSTLEN];

  /* Reset filename on NULL, except with env var which is checked by caller. */
  if (!Filename) {
    resetFilenameToDefault();
    return 0;
  }

  /* Check the filename for "%p", which indicates a pid-substitution. */
  for (I = 0; Filename[I]; ++I)
    if (Filename[I] == '%') {
      if (Filename[++I] == 'p') {
        if (!NumPids++) {
          PidLength = snprintf(PidChars, MAX_PID_SIZE, "%d", getpid());
          if (PidLength <= 0)
            return -1;
        }
      } else if (Filename[I] == 'h') {
        if (!NumHosts++)
          if (COMPILER_RT_GETHOSTNAME(Hostname, COMPILER_RT_MAX_HOSTLEN))
            return -1;
          HostNameLength = strlen(Hostname);
      }
    }

  if (!(NumPids || NumHosts)) {
    setFilename(Filename, 0);
    return 0;
  }

  /* Allocate enough space for the substituted filename. */
  Allocated = malloc(I + NumPids*(PidLength - 2) +
                     NumHosts*(HostNameLength - 2) + 1);
  if (!Allocated)
    return -1;

  /* Construct the new filename. */
  for (I = 0, J = 0; Filename[I]; ++I)
    if (Filename[I] == '%') {
      if (Filename[++I] == 'p') {
        memcpy(Allocated + J, PidChars, PidLength);
        J += PidLength;
      }
      else if (Filename[I] == 'h') {
        memcpy(Allocated + J, Hostname, HostNameLength);
        J += HostNameLength;
      }
      /* Drop any unknown substitutions. */
    } else
      Allocated[J++] = Filename[I];
  Allocated[J] = 0;

  /* Use the computed name. */
  setFilename(Allocated, 1);
  return 0;
}

static const char *getFilenameFromEnv(void) {
  const char *Filename = getenv("LLVM_PROFILE_FILE");
  if (!Filename || !Filename[0])
    return 0;
  return Filename;
}

/* This method is invoked by the runtime initialization hook
 * InstrProfilingRuntime.o if it is linked in. Both user specified
 * profile path via -fprofile-instr-generate= and LLVM_PROFILE_FILE
 * environment variable can override this default value. */
COMPILER_RT_VISIBILITY
void __llvm_profile_initialize_file(void) {
  const char *Filename;
  /* Check if the filename has been initialized. */
  if (__llvm_profile_CurrentFilename)
    return;

  /* Detect the filename and truncate. */
  Filename = getFilenameFromEnv();
  if (!Filename || setFilenamePossiblyWithPid(Filename))
    resetFilenameToDefault();
}

/* This API is directly called by the user application code. It has the
 * highest precedence compared with LLVM_PROFILE_FILE environment variable
 * and command line option -fprofile-istr-generate=<profile_name>.
 */
COMPILER_RT_VISIBILITY
void __llvm_profile_set_filename(const char *Filename) {
  setFilenamePossiblyWithPid(Filename);
}

/* 
 * This API is invoked by the global initializers emitted by Clang/LLVM when
 * -fprofile-instr-generate=<..> is specified (vs -fprofile-instr-generate
 *  without an argument). This option has lower precedence than the
 *  LLVM_PROFILE_FILE environment variable.
 */
COMPILER_RT_VISIBILITY
void __llvm_profile_override_default_filename(const char *Filename) {
  /* If the env var is set, skip setting filename from argument. */
  const char *Env_Filename = getFilenameFromEnv();
  if (Env_Filename)
    return;
  setFilenamePossiblyWithPid(Filename);
}

COMPILER_RT_VISIBILITY
int __llvm_profile_write_file(void) {
  int rc;

  GetEnvHook = &getenv;
  /* Check the filename. */
  if (!__llvm_profile_CurrentFilename) {
    PROF_ERR("Failed to write file : %s\n", "Filename not set");
    return -1;
  }

  /* Check if there is llvm/runtime version mismatch.  */
  if (GET_VERSION(__llvm_profile_get_version()) != INSTR_PROF_RAW_VERSION) {
    PROF_ERR("Runtime and instrumentation version mismatch : "
             "expected %d, but get %d\n",
             INSTR_PROF_RAW_VERSION,
             (int)GET_VERSION(__llvm_profile_get_version()));
    return -1;
  }

  /* Write the file. */
  rc = writeFileWithName(__llvm_profile_CurrentFilename);
  if (rc)
    PROF_ERR("Failed to write file \"%s\": %s\n",
            __llvm_profile_CurrentFilename, strerror(errno));
  return rc;
}

static void writeFileWithoutReturn(void) { __llvm_profile_write_file(); }

COMPILER_RT_VISIBILITY
int __llvm_profile_register_write_file_atexit(void) {
  static int HasBeenRegistered = 0;

  if (HasBeenRegistered)
    return 0;

  lprofSetupValueProfiler();

  HasBeenRegistered = 1;
  return atexit(writeFileWithoutReturn);
}
