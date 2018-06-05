//===-- xray_fdr_logging.cc ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of XRay, a dynamic runtime instrumentation system.
//
// Here we implement the Flight Data Recorder mode for XRay, where we use
// compact structures to store records in memory as well as when writing out the
// data to files.
//
//===----------------------------------------------------------------------===//
#include "xray_fdr_logging.h"
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sanitizer_common/sanitizer_atomic.h"
#include "sanitizer_common/sanitizer_common.h"
#include "xray/xray_interface.h"
#include "xray/xray_records.h"
#include "xray_buffer_queue.h"
#include "xray_defs.h"
#include "xray_fdr_flags.h"
#include "xray_fdr_logging_impl.h"
#include "xray_flags.h"
#include "xray_tsc.h"
#include "xray_utils.h"

namespace __xray {

// Global BufferQueue.
BufferQueue *BQ = nullptr;

atomic_sint32_t LogFlushStatus = {XRayLogFlushStatus::XRAY_LOG_NOT_FLUSHING};

FDRLoggingOptions FDROptions;

SpinMutex FDROptionsMutex;

namespace {
XRayFileHeader &fdrCommonHeaderInfo() {
  static XRayFileHeader Header = [] {
    XRayFileHeader H;
    // Version 2 of the log writes the extents of the buffer, instead of
    // relying on an end-of-buffer record.
    H.Version = 2;
    H.Type = FileTypes::FDR_LOG;

    // Test for required CPU features and cache the cycle frequency
    static bool TSCSupported = probeRequiredCPUFeatures();
    static uint64_t CycleFrequency =
        TSCSupported ? getTSCFrequency() : __xray::NanosecondsPerSecond;
    H.CycleFrequency = CycleFrequency;

    // FIXME: Actually check whether we have 'constant_tsc' and
    // 'nonstop_tsc' before setting the values in the header.
    H.ConstantTSC = 1;
    H.NonstopTSC = 1;
    return H;
  }();
  return Header;
}

} // namespace

// This is the iterator implementation, which knows how to handle FDR-mode
// specific buffers. This is used as an implementation of the iterator function
// needed by __xray_set_buffer_iterator(...). It maintains a global state of the
// buffer iteration for the currently installed FDR mode buffers. In particular:
//
//   - If the argument represents the initial state of XRayBuffer ({nullptr, 0})
//     then the iterator returns the header information.
//   - If the argument represents the header information ({address of header
//     info, size of the header info}) then it returns the first FDR buffer's
//     address and extents.
//   - It will keep returning the next buffer and extents as there are more
//     buffers to process. When the input represents the last buffer, it will
//     return the initial state to signal completion ({nullptr, 0}).
//
// See xray/xray_log_interface.h for more details on the requirements for the
// implementations of __xray_set_buffer_iterator(...) and
// __xray_log_process_buffers(...).
XRayBuffer fdrIterator(const XRayBuffer B) {
  DCHECK(internal_strcmp(__xray_log_get_current_mode(), "xray-fdr") == 0);
  DCHECK(BQ->finalizing());

  if (BQ == nullptr || !BQ->finalizing()) {
    if (Verbosity())
      Report(
          "XRay FDR: Failed global buffer queue is null or not finalizing!\n");
    return {nullptr, 0};
  }

  // We use a global scratch-pad for the header information, which only gets
  // initialized the first time this function is called. We'll update one part
  // of this information with some relevant data (in particular the number of
  // buffers to expect).
  static XRayFileHeader Header = fdrCommonHeaderInfo();
  if (B.Data == nullptr && B.Size == 0) {
    Header.FdrData = FdrAdditionalHeaderData{BQ->ConfiguredBufferSize()};
    return XRayBuffer{static_cast<void *>(&Header), sizeof(Header)};
  }

  static BufferQueue::const_iterator It{};
  static BufferQueue::const_iterator End{};
  if (B.Data == static_cast<void *>(&Header) && B.Size == sizeof(Header)) {

    // From this point on, we provide raw access to the raw buffer we're getting
    // from the BufferQueue. We're relying on the iterators from the current
    // Buffer queue.
    It = BQ->cbegin();
    End = BQ->cend();
  }

  if (It == End)
    return {nullptr, 0};

  XRayBuffer Result;
  Result.Data = It->Data;
  Result.Size = atomic_load(&It->Extents->Size, memory_order_acquire);
  ++It;
  return Result;
}

// Must finalize before flushing.
XRayLogFlushStatus fdrLoggingFlush() XRAY_NEVER_INSTRUMENT {
  if (atomic_load(&LoggingStatus, memory_order_acquire) !=
      XRayLogInitStatus::XRAY_LOG_FINALIZED) {
    if (Verbosity())
      Report("Not flushing log, implementation is not finalized.\n");
    return XRayLogFlushStatus::XRAY_LOG_NOT_FLUSHING;
  }

  s32 Result = XRayLogFlushStatus::XRAY_LOG_NOT_FLUSHING;
  if (!atomic_compare_exchange_strong(&LogFlushStatus, &Result,
                                      XRayLogFlushStatus::XRAY_LOG_FLUSHING,
                                      memory_order_release)) {
    if (Verbosity())
      Report("Not flushing log, implementation is still finalizing.\n");
    return static_cast<XRayLogFlushStatus>(Result);
  }

  if (BQ == nullptr) {
    if (Verbosity())
      Report("Cannot flush when global buffer queue is null.\n");
    return XRayLogFlushStatus::XRAY_LOG_NOT_FLUSHING;
  }

  // We wait a number of milliseconds to allow threads to see that we've
  // finalised before attempting to flush the log.
  SleepForMillis(fdrFlags()->grace_period_ms);

  // At this point, we're going to uninstall the iterator implementation, before
  // we decide to do anything further with the global buffer queue.
  __xray_log_remove_buffer_iterator();

  if (fdrFlags()->no_file_flush) {
    if (Verbosity())
      Report("XRay FDR: Not flushing to file, 'no_file_flush=true'.\n");

    // Clean up the buffer queue, and do not bother writing out the files!
    delete BQ;
    BQ = nullptr;
    atomic_store(&LogFlushStatus, XRayLogFlushStatus::XRAY_LOG_FLUSHED,
                 memory_order_release);
    return XRayLogFlushStatus::XRAY_LOG_FLUSHED;
  }

  // We write out the file in the following format:
  //
  //   1) We write down the XRay file header with version 1, type FDR_LOG.
  //   2) Then we use the 'apply' member of the BufferQueue that's live, to
  //      ensure that at this point in time we write down the buffers that have
  //      been released (and marked "used") -- we dump the full buffer for now
  //      (fixed-sized) and let the tools reading the buffers deal with the data
  //      afterwards.
  //
  int Fd = -1;
  {
    // FIXME: Remove this section of the code, when we remove the struct-based
    // configuration API.
    SpinMutexLock Guard(&FDROptionsMutex);
    Fd = FDROptions.Fd;
  }
  if (Fd == -1)
    Fd = getLogFD();
  if (Fd == -1) {
    auto Result = XRayLogFlushStatus::XRAY_LOG_NOT_FLUSHING;
    atomic_store(&LogFlushStatus, Result, memory_order_release);
    return Result;
  }

  XRayFileHeader Header = fdrCommonHeaderInfo();
  Header.FdrData = FdrAdditionalHeaderData{BQ->ConfiguredBufferSize()};
  retryingWriteAll(Fd, reinterpret_cast<char *>(&Header),
                   reinterpret_cast<char *>(&Header) + sizeof(Header));

  BQ->apply([&](const BufferQueue::Buffer &B) {
    // Starting at version 2 of the FDR logging implementation, we only write
    // the records identified by the extents of the buffer. We use the Extents
    // from the Buffer and write that out as the first record in the buffer.  We
    // still use a Metadata record, but fill in the extents instead for the
    // data.
    MetadataRecord ExtentsRecord;
    auto BufferExtents = atomic_load(&B.Extents->Size, memory_order_acquire);
    assert(BufferExtents <= B.Size);
    ExtentsRecord.Type = uint8_t(RecordType::Metadata);
    ExtentsRecord.RecordKind =
        uint8_t(MetadataRecord::RecordKinds::BufferExtents);
    std::memcpy(ExtentsRecord.Data, &BufferExtents, sizeof(BufferExtents));
    if (BufferExtents > 0) {
      retryingWriteAll(Fd, reinterpret_cast<char *>(&ExtentsRecord),
                       reinterpret_cast<char *>(&ExtentsRecord) +
                           sizeof(MetadataRecord));
      retryingWriteAll(Fd, reinterpret_cast<char *>(B.Data),
                       reinterpret_cast<char *>(B.Data) + BufferExtents);
    }
  });

  atomic_store(&LogFlushStatus, XRayLogFlushStatus::XRAY_LOG_FLUSHED,
               memory_order_release);
  return XRayLogFlushStatus::XRAY_LOG_FLUSHED;
}

XRayLogInitStatus fdrLoggingFinalize() XRAY_NEVER_INSTRUMENT {
  s32 CurrentStatus = XRayLogInitStatus::XRAY_LOG_INITIALIZED;
  if (!atomic_compare_exchange_strong(&LoggingStatus, &CurrentStatus,
                                      XRayLogInitStatus::XRAY_LOG_FINALIZING,
                                      memory_order_release)) {
    if (Verbosity())
      Report("Cannot finalize log, implementation not initialized.\n");
    return static_cast<XRayLogInitStatus>(CurrentStatus);
  }

  // Do special things to make the log finalize itself, and not allow any more
  // operations to be performed until re-initialized.
  BQ->finalize();

  atomic_store(&LoggingStatus, XRayLogInitStatus::XRAY_LOG_FINALIZED,
               memory_order_release);
  return XRayLogInitStatus::XRAY_LOG_FINALIZED;
}

struct TSCAndCPU {
  uint64_t TSC = 0;
  unsigned char CPU = 0;
};

static TSCAndCPU getTimestamp() XRAY_NEVER_INSTRUMENT {
  // We want to get the TSC as early as possible, so that we can check whether
  // we've seen this CPU before. We also do it before we load anything else,
  // to allow for forward progress with the scheduling.
  TSCAndCPU Result;

  // Test once for required CPU features
  static bool TSCSupported = probeRequiredCPUFeatures();

  if (TSCSupported) {
    Result.TSC = __xray::readTSC(Result.CPU);
  } else {
    // FIXME: This code needs refactoring as it appears in multiple locations
    timespec TS;
    int result = clock_gettime(CLOCK_REALTIME, &TS);
    if (result != 0) {
      Report("clock_gettime(2) return %d, errno=%d", result, int(errno));
      TS = {0, 0};
    }
    Result.CPU = 0;
    Result.TSC = TS.tv_sec * __xray::NanosecondsPerSecond + TS.tv_nsec;
  }
  return Result;
}

void fdrLoggingHandleArg0(int32_t FuncId,
                          XRayEntryType Entry) XRAY_NEVER_INSTRUMENT {
  auto TC = getTimestamp();
  __xray_fdr_internal::processFunctionHook(FuncId, Entry, TC.TSC, TC.CPU, 0,
                                           clock_gettime, BQ);
}

void fdrLoggingHandleArg1(int32_t FuncId, XRayEntryType Entry,
                          uint64_t Arg) XRAY_NEVER_INSTRUMENT {
  auto TC = getTimestamp();
  __xray_fdr_internal::processFunctionHook(FuncId, Entry, TC.TSC, TC.CPU, Arg,
                                           clock_gettime, BQ);
}

void fdrLoggingHandleCustomEvent(void *Event,
                                 std::size_t EventSize) XRAY_NEVER_INSTRUMENT {
  using namespace __xray_fdr_internal;
  auto TC = getTimestamp();
  auto &TSC = TC.TSC;
  auto &CPU = TC.CPU;
  RecursionGuard Guard{Running};
  if (!Guard)
    return;
  if (EventSize > std::numeric_limits<int32_t>::max()) {
    using Empty = struct {};
    static Empty Once = [&] {
      Report("Event size too large = %zu ; > max = %d\n", EventSize,
             std::numeric_limits<int32_t>::max());
      return Empty();
    }();
    (void)Once;
  }
  int32_t ReducedEventSize = static_cast<int32_t>(EventSize);
  auto &TLD = getThreadLocalData();
  if (!isLogInitializedAndReady(TLD.BQ, TSC, CPU, clock_gettime))
    return;

  // Here we need to prepare the log to handle:
  //   - The metadata record we're going to write. (16 bytes)
  //   - The additional data we're going to write. Currently, that's the size
  //   of the event we're going to dump into the log as free-form bytes.
  if (!prepareBuffer(TSC, CPU, clock_gettime, MetadataRecSize + EventSize)) {
    TLD.BQ = nullptr;
    return;
  }

  // Write the custom event metadata record, which consists of the following
  // information:
  //   - 8 bytes (64-bits) for the full TSC when the event started.
  //   - 4 bytes (32-bits) for the length of the data.
  MetadataRecord CustomEvent;
  CustomEvent.Type = uint8_t(RecordType::Metadata);
  CustomEvent.RecordKind =
      uint8_t(MetadataRecord::RecordKinds::CustomEventMarker);
  constexpr auto TSCSize = sizeof(TC.TSC);
  std::memcpy(&CustomEvent.Data, &ReducedEventSize, sizeof(int32_t));
  std::memcpy(&CustomEvent.Data[sizeof(int32_t)], &TSC, TSCSize);
  std::memcpy(TLD.RecordPtr, &CustomEvent, sizeof(CustomEvent));
  TLD.RecordPtr += sizeof(CustomEvent);
  std::memcpy(TLD.RecordPtr, Event, ReducedEventSize);
  incrementExtents(MetadataRecSize + EventSize);
  endBufferIfFull();
}

void fdrLoggingHandleTypedEvent(
    uint16_t EventType, const void *Event,
    std::size_t EventSize) noexcept XRAY_NEVER_INSTRUMENT {
  using namespace __xray_fdr_internal;
  auto TC = getTimestamp();
  auto &TSC = TC.TSC;
  auto &CPU = TC.CPU;
  RecursionGuard Guard{Running};
  if (!Guard)
    return;
  if (EventSize > std::numeric_limits<int32_t>::max()) {
    using Empty = struct {};
    static Empty Once = [&] {
      Report("Event size too large = %zu ; > max = %d\n", EventSize,
             std::numeric_limits<int32_t>::max());
      return Empty();
    }();
    (void)Once;
  }
  int32_t ReducedEventSize = static_cast<int32_t>(EventSize);
  auto &TLD = getThreadLocalData();
  if (!isLogInitializedAndReady(TLD.BQ, TSC, CPU, clock_gettime))
    return;

  // Here we need to prepare the log to handle:
  //   - The metadata record we're going to write. (16 bytes)
  //   - The additional data we're going to write. Currently, that's the size
  //   of the event we're going to dump into the log as free-form bytes.
  if (!prepareBuffer(TSC, CPU, clock_gettime, MetadataRecSize + EventSize)) {
    TLD.BQ = nullptr;
    return;
  }
  // Write the custom event metadata record, which consists of the following
  // information:
  //   - 8 bytes (64-bits) for the full TSC when the event started.
  //   - 4 bytes (32-bits) for the length of the data.
  //   - 2 bytes (16-bits) for the event type. 3 bytes remain since one of the
  //       bytes has the record type (Metadata Record) and kind (TypedEvent).
  //       We'll log the error if the event type is greater than 2 bytes.
  //       Event types are generated sequentially, so 2^16 is enough.
  MetadataRecord TypedEvent;
  TypedEvent.Type = uint8_t(RecordType::Metadata);
  TypedEvent.RecordKind =
      uint8_t(MetadataRecord::RecordKinds::TypedEventMarker);
  constexpr auto TSCSize = sizeof(TC.TSC);
  std::memcpy(&TypedEvent.Data, &ReducedEventSize, sizeof(int32_t));
  std::memcpy(&TypedEvent.Data[sizeof(int32_t)], &TSC, TSCSize);
  std::memcpy(&TypedEvent.Data[sizeof(int32_t) + TSCSize], &EventType,
              sizeof(EventType));
  std::memcpy(TLD.RecordPtr, &TypedEvent, sizeof(TypedEvent));

  TLD.RecordPtr += sizeof(TypedEvent);
  std::memcpy(TLD.RecordPtr, Event, ReducedEventSize);
  incrementExtents(MetadataRecSize + EventSize);
  endBufferIfFull();
}

XRayLogInitStatus fdrLoggingInit(size_t BufferSize, size_t BufferMax,
                                 void *Options,
                                 size_t OptionsSize) XRAY_NEVER_INSTRUMENT {
  if (Options == nullptr)
    return XRayLogInitStatus::XRAY_LOG_UNINITIALIZED;

  s32 CurrentStatus = XRayLogInitStatus::XRAY_LOG_UNINITIALIZED;
  if (!atomic_compare_exchange_strong(&LoggingStatus, &CurrentStatus,
                                      XRayLogInitStatus::XRAY_LOG_INITIALIZING,
                                      memory_order_release)) {
    if (Verbosity())
      Report("Cannot initialize already initialized implementation.\n");
    return static_cast<XRayLogInitStatus>(CurrentStatus);
  }

  // Because of __xray_log_init_mode(...) which guarantees that this will be
  // called with BufferSize == 0 and BufferMax == 0 we parse the configuration
  // provided in the Options pointer as a string instead.
  if (BufferSize == 0 && BufferMax == 0) {
    if (Verbosity())
      Report("Initializing FDR mode with options: %s\n",
             static_cast<const char *>(Options));

    // TODO: Factor out the flags specific to the FDR mode implementation. For
    // now, use the global/single definition of the flags, since the FDR mode
    // flags are already defined there.
    FlagParser FDRParser;
    FDRFlags FDRFlags;
    registerXRayFDRFlags(&FDRParser, &FDRFlags);
    FDRFlags.setDefaults();

    // Override first from the general XRAY_DEFAULT_OPTIONS compiler-provided
    // options until we migrate everyone to use the XRAY_FDR_OPTIONS
    // compiler-provided options.
    FDRParser.ParseString(useCompilerDefinedFlags());
    FDRParser.ParseString(useCompilerDefinedFDRFlags());
    auto *EnvOpts = GetEnv("XRAY_FDR_OPTIONS");
    if (EnvOpts == nullptr)
      EnvOpts = "";
    FDRParser.ParseString(EnvOpts);

    // FIXME: Remove this when we fully remove the deprecated flags.
    if (internal_strlen(EnvOpts) == 0) {
      FDRFlags.func_duration_threshold_us =
          flags()->xray_fdr_log_func_duration_threshold_us;
      FDRFlags.grace_period_ms = flags()->xray_fdr_log_grace_period_ms;
    }

    // The provided options should always override the compiler-provided and
    // environment-variable defined options.
    FDRParser.ParseString(static_cast<const char *>(Options));
    *fdrFlags() = FDRFlags;
    BufferSize = FDRFlags.buffer_size;
    BufferMax = FDRFlags.buffer_max;
    SpinMutexLock Guard(&FDROptionsMutex);
    FDROptions.Fd = -1;
    FDROptions.ReportErrors = true;
  } else if (OptionsSize != sizeof(FDRLoggingOptions)) {
    // FIXME: This is deprecated, and should really be removed.
    // At this point we use the flag parser specific to the FDR mode
    // implementation.
    if (Verbosity())
      Report("Cannot initialize FDR logging; wrong size for options: %d\n",
             OptionsSize);
    return static_cast<XRayLogInitStatus>(
        atomic_load(&LoggingStatus, memory_order_acquire));
  } else {
    if (Verbosity())
      Report("XRay FDR: struct-based init is deprecated, please use "
             "string-based configuration instead.\n");
    SpinMutexLock Guard(&FDROptionsMutex);
    memcpy(&FDROptions, Options, OptionsSize);
  }

  bool Success = false;

  if (BQ != nullptr) {
    delete BQ;
    BQ = nullptr;
  }

  if (BQ == nullptr)
    BQ = new BufferQueue(BufferSize, BufferMax, Success);

  if (!Success) {
    Report("BufferQueue init failed.\n");
    if (BQ != nullptr) {
      delete BQ;
      BQ = nullptr;
    }
    return XRayLogInitStatus::XRAY_LOG_UNINITIALIZED;
  }

  static bool UNUSED Once = [] {
    pthread_key_create(&__xray_fdr_internal::Key, +[](void *) {
      auto &TLD = __xray_fdr_internal::getThreadLocalData();
      if (TLD.BQ == nullptr)
        return;
      auto EC = TLD.BQ->releaseBuffer(TLD.Buffer);
      if (EC != BufferQueue::ErrorCode::Ok)
        Report("At thread exit, failed to release buffer at %p; error=%s\n",
               TLD.Buffer.Data, BufferQueue::getErrorString(EC));
    });
    return false;
  }();

  // Arg1 handler should go in first to avoid concurrent code accidentally
  // falling back to arg0 when it should have ran arg1.
  __xray_set_handler_arg1(fdrLoggingHandleArg1);
  // Install the actual handleArg0 handler after initialising the buffers.
  __xray_set_handler(fdrLoggingHandleArg0);
  __xray_set_customevent_handler(fdrLoggingHandleCustomEvent);
  __xray_set_typedevent_handler(fdrLoggingHandleTypedEvent);

  // Install the buffer iterator implementation.
  __xray_log_set_buffer_iterator(fdrIterator);

  atomic_store(&LoggingStatus, XRayLogInitStatus::XRAY_LOG_INITIALIZED,
               memory_order_release);

  if (Verbosity())
    Report("XRay FDR init successful.\n");
  return XRayLogInitStatus::XRAY_LOG_INITIALIZED;
}

bool fdrLogDynamicInitializer() XRAY_NEVER_INSTRUMENT {
  using namespace __xray;
  XRayLogImpl Impl{
      fdrLoggingInit,
      fdrLoggingFinalize,
      fdrLoggingHandleArg0,
      fdrLoggingFlush,
  };
  auto RegistrationResult = __xray_log_register_mode("xray-fdr", Impl);
  if (RegistrationResult != XRayLogRegisterStatus::XRAY_REGISTRATION_OK &&
      Verbosity())
    Report("Cannot register XRay FDR mode to 'xray-fdr'; error = %d\n",
           RegistrationResult);
  if (flags()->xray_fdr_log || !internal_strcmp(flags()->xray_mode, "xray-fdr"))
    __xray_set_log_impl(Impl);
  return true;
}

} // namespace __xray

static auto UNUSED Unused = __xray::fdrLogDynamicInitializer();
