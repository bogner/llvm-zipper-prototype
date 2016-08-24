//===-- Executor.h - The Executor class -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Executor class which represents a single device of a specific platform.
///
//===----------------------------------------------------------------------===//

#ifndef STREAMEXECUTOR_EXECUTOR_H
#define STREAMEXECUTOR_EXECUTOR_H

#include "streamexecutor/KernelSpec.h"
#include "streamexecutor/PlatformInterfaces.h"
#include "streamexecutor/Utils/Error.h"

namespace streamexecutor {

class KernelInterface;
class Stream;

class Executor {
public:
  explicit Executor(PlatformExecutor *PExecutor);
  virtual ~Executor();

  /// Gets the kernel implementation for the underlying platform.
  virtual Expected<std::unique_ptr<KernelInterface>>
  getKernelImplementation(const MultiKernelLoaderSpec &Spec) {
    // TODO(jhen): Implement this.
    return nullptr;
  }

  Expected<std::unique_ptr<Stream>> createStream();

  /// Allocates an array of ElementCount entries of type T in device memory.
  template <typename T>
  Expected<GlobalDeviceMemory<T>> allocateDeviceMemory(size_t ElementCount) {
    return PExecutor->allocateDeviceMemory(ElementCount * sizeof(T));
  }

  /// Frees memory previously allocated with allocateDeviceMemory.
  template <typename T> Error freeDeviceMemory(GlobalDeviceMemory<T> Memory) {
    return PExecutor->freeDeviceMemory(Memory);
  }

  /// Allocates an array of ElementCount entries of type T in host memory.
  ///
  /// Host memory allocated by this function can be used for asynchronous memory
  /// copies on streams. See Stream::thenCopyD2H and Stream::thenCopyH2D.
  template <typename T> Expected<T *> allocateHostMemory(size_t ElementCount) {
    return PExecutor->allocateHostMemory(ElementCount * sizeof(T));
  }

  /// Frees memory previously allocated with allocateHostMemory.
  template <typename T> Error freeHostMemory(T *Memory) {
    return PExecutor->freeHostMemory(Memory);
  }

  /// Registers a previously allocated host array of type T for asynchronous
  /// memory operations.
  ///
  /// Host memory registered by this function can be used for asynchronous
  /// memory copies on streams. See Stream::thenCopyD2H and Stream::thenCopyH2D.
  template <typename T>
  Error registerHostMemory(T *Memory, size_t ElementCount) {
    return PExecutor->registerHostMemory(Memory, ElementCount * sizeof(T));
  }

  /// Unregisters host memory previously registered by registerHostMemory.
  template <typename T> Error unregisterHostMemory(T *Memory) {
    return PExecutor->unregisterHostMemory(Memory);
  }

  /// \anchor ExecutorHostSyncCopyGroup
  /// \name Host-synchronous device memory copying functions
  ///
  /// These methods block the calling host thread while copying data to or from
  /// device memory. On the device side, these methods do not block any ongoing
  /// device calls.
  ///
  /// There are no restrictions on the host memory that is used as a source or
  /// destination in these copy methods, so there is no need to allocate that
  /// host memory using allocateHostMemory or register it with
  /// registerHostMemory.
  ///
  /// Each of these methods has a single template parameter, T, that specifies
  /// the type of data being copied. The ElementCount arguments specify the
  /// number of objects of type T to be copied.
  ///
  /// For ease of use, each of the methods is overloaded to take either a
  /// GlobalDeviceMemorySlice or a GlobalDeviceMemory argument in the device
  /// memory argument slots, and the GlobalDeviceMemory arguments are just
  /// converted to GlobalDeviceMemorySlice arguments internally by using
  /// GlobalDeviceMemory::asSlice.
  ///
  /// These methods perform bounds checking to make sure that the ElementCount
  /// is not too large for the source or destination. For methods that do not
  /// take an ElementCount argument, an error is returned if the source size
  /// does not exactly match the destination size.
  ///@{

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemorySlice<T> Src,
                           llvm::MutableArrayRef<T> Dst, size_t ElementCount) {
    if (ElementCount > Src.getElementCount())
      return make_error("copying too many elements, " +
                        llvm::Twine(ElementCount) +
                        ", from a device array of element count " +
                        llvm::Twine(Src.getElementCount()));
    if (ElementCount > Dst.size())
      return make_error(
          "copying too many elements, " + llvm::Twine(ElementCount) +
          ", to a host array of element count " + llvm::Twine(Dst.size()));
    return PExecutor->synchronousCopyD2H(
        Src.getBaseMemory(), Src.getElementOffset() * sizeof(T), Dst.data(), 0,
        ElementCount * sizeof(T));
  }

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemorySlice<T> Src,
                           llvm::MutableArrayRef<T> Dst) {
    if (Src.getElementCount() != Dst.size())
      return make_error(
          "array size mismatch for D2H, device source has element count " +
          llvm::Twine(Src.getElementCount()) +
          " but host destination has element count " + llvm::Twine(Dst.size()));
    return synchronousCopyD2H(Src, Dst, Src.getElementCount());
  }

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemorySlice<T> Src, T *Dst,
                           size_t ElementCount) {
    return synchronousCopyD2H(Src, llvm::MutableArrayRef<T>(Dst, ElementCount),
                              ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemory<T> Src,
                           llvm::MutableArrayRef<T> Dst, size_t ElementCount) {
    return synchronousCopyD2H(Src.asSlice(), Dst, ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemory<T> Src,
                           llvm::MutableArrayRef<T> Dst) {
    return synchronousCopyD2H(Src.asSlice(), Dst);
  }

  template <typename T>
  Error synchronousCopyD2H(GlobalDeviceMemory<T> Src, T *Dst,
                           size_t ElementCount) {
    return synchronousCopyD2H(Src.asSlice(), Dst, ElementCount);
  }

  template <typename T>
  Error synchronousCopyH2D(llvm::ArrayRef<T> Src,
                           GlobalDeviceMemorySlice<T> Dst,
                           size_t ElementCount) {
    if (ElementCount > Src.size())
      return make_error(
          "copying too many elements, " + llvm::Twine(ElementCount) +
          ", from a host array of element count " + llvm::Twine(Src.size()));
    if (ElementCount > Dst.getElementCount())
      return make_error("copying too many elements, " +
                        llvm::Twine(ElementCount) +
                        ", to a device array of element count " +
                        llvm::Twine(Dst.getElementCount()));
    return PExecutor->synchronousCopyH2D(Src.data(), 0, Dst.getBaseMemory(),
                                         Dst.getElementOffset() * sizeof(T),
                                         ElementCount * sizeof(T));
  }

  template <typename T>
  Error synchronousCopyH2D(llvm::ArrayRef<T> Src,
                           GlobalDeviceMemorySlice<T> Dst) {
    if (Src.size() != Dst.getElementCount())
      return make_error(
          "array size mismatch for H2D, host source has element count " +
          llvm::Twine(Src.size()) +
          " but device destination has element count " +
          llvm::Twine(Dst.getElementCount()));
    return synchronousCopyH2D(Src, Dst, Dst.getElementCount());
  }

  template <typename T>
  Error synchronousCopyH2D(T *Src, GlobalDeviceMemorySlice<T> Dst,
                           size_t ElementCount) {
    return synchronousCopyH2D(llvm::ArrayRef<T>(Src, ElementCount), Dst,
                              ElementCount);
  }

  template <typename T>
  Error synchronousCopyH2D(llvm::ArrayRef<T> Src, GlobalDeviceMemory<T> Dst,
                           size_t ElementCount) {
    return synchronousCopyH2D(Src, Dst.asSlice(), ElementCount);
  }

  template <typename T>
  Error synchronousCopyH2D(llvm::ArrayRef<T> Src, GlobalDeviceMemory<T> Dst) {
    return synchronousCopyH2D(Src, Dst.asSlice());
  }

  template <typename T>
  Error synchronousCopyH2D(T *Src, GlobalDeviceMemory<T> Dst,
                           size_t ElementCount) {
    return synchronousCopyH2D(Src, Dst.asSlice(), ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemorySlice<T> Src,
                           GlobalDeviceMemorySlice<T> Dst,
                           size_t ElementCount) {
    if (ElementCount > Src.getElementCount())
      return make_error("copying too many elements, " +
                        llvm::Twine(ElementCount) +
                        ", from a device array of element count " +
                        llvm::Twine(Src.getElementCount()));
    if (ElementCount > Dst.getElementCount())
      return make_error("copying too many elements, " +
                        llvm::Twine(ElementCount) +
                        ", to a device array of element count " +
                        llvm::Twine(Dst.getElementCount()));
    return PExecutor->synchronousCopyD2D(
        Src.getBaseMemory(), Src.getElementOffset() * sizeof(T),
        Dst.getBaseMemory(), Dst.getElementOffset() * sizeof(T),
        ElementCount * sizeof(T));
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemorySlice<T> Src,
                           GlobalDeviceMemorySlice<T> Dst) {
    if (Src.getElementCount() != Dst.getElementCount())
      return make_error(
          "array size mismatch for D2D, device source has element count " +
          llvm::Twine(Src.getElementCount()) +
          " but device destination has element count " +
          llvm::Twine(Dst.getElementCount()));
    return synchronousCopyD2D(Src, Dst, Src.getElementCount());
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemory<T> Src,
                           GlobalDeviceMemorySlice<T> Dst,
                           size_t ElementCount) {
    return synchronousCopyD2D(Src.asSlice(), Dst, ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemory<T> Src,
                           GlobalDeviceMemorySlice<T> Dst) {
    return synchronousCopyD2D(Src.asSlice(), Dst);
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemorySlice<T> Src,
                           GlobalDeviceMemory<T> Dst, size_t ElementCount) {
    return synchronousCopyD2D(Src, Dst.asSlice(), ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemorySlice<T> Src,
                           GlobalDeviceMemory<T> Dst) {
    return synchronousCopyD2D(Src, Dst.asSlice());
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemory<T> Src, GlobalDeviceMemory<T> Dst,
                           size_t ElementCount) {
    return synchronousCopyD2D(Src.asSlice(), Dst.asSlice(), ElementCount);
  }

  template <typename T>
  Error synchronousCopyD2D(GlobalDeviceMemory<T> Src,
                           GlobalDeviceMemory<T> Dst) {
    return synchronousCopyD2D(Src.asSlice(), Dst.asSlice());
  }

  ///@} End host-synchronous device memory copying functions

private:
  PlatformExecutor *PExecutor;
};

} // namespace streamexecutor

#endif // STREAMEXECUTOR_EXECUTOR_H
