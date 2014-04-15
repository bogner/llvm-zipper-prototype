//===--- Allocator.h - Simple memory allocation abstraction -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines the MallocAllocator and BumpPtrAllocator interfaces. Both
/// of these conform to an LLVM "Allocator" concept which consists of an
/// Allocate method accepting a size and alignment, and a Deallocate accepting
/// a pointer and size. Further, the LLVM "Allocator" concept has overloads of
/// Allocate and Deallocate for setting size and alignment based on the final
/// type. These overloads are typically provided by a base class template \c
/// AllocatorBase.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_ALLOCATOR_H
#define LLVM_SUPPORT_ALLOCATOR_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Memory.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>

namespace llvm {
template <typename T> struct ReferenceAdder {
  typedef T &result;
};
template <typename T> struct ReferenceAdder<T &> {
  typedef T result;
};

/// \brief CRTP base class providing obvious overloads for the core \c
/// Allocate() methods of LLVM-style allocators.
///
/// This base class both documents the full public interface exposed by all
/// LLVM-style allocators, and redirects all of the overloads to a single core
/// set of methods which the derived class must define.
template <typename DerivedT> class AllocatorBase {
public:
  /// \brief Allocate \a Size bytes of \a Alignment aligned memory. This method
  /// must be implemented by \c DerivedT.
  void *Allocate(size_t Size, size_t Alignment) {
#ifdef __clang__
    static_assert(static_cast<void *(AllocatorBase::*)(size_t, size_t)>(
                      &AllocatorBase::Allocate) !=
                      static_cast<void *(DerivedT::*)(size_t, size_t)>(
                          &DerivedT::Allocate),
                  "Class derives from AllocatorBase without implementing the "
                  "core Allocate(size_t, size_t) overload!");
#endif
    return static_cast<DerivedT *>(this)->Allocate(Size, Alignment);
  }

  /// \brief Allocate space for one object without constructing it.
  template <typename T> T *Allocate() {
    return static_cast<T *>(Allocate(sizeof(T), AlignOf<T>::Alignment));
  }

  /// \brief Allocate space for an array of objects without constructing them.
  template <typename T> T *Allocate(size_t Num) {
    return static_cast<T *>(Allocate(Num * sizeof(T), AlignOf<T>::Alignment));
  }

  /// \brief Allocate space for an array of objects with the specified alignment
  /// and without constructing them.
  template <typename T> T *Allocate(size_t Num, size_t Alignment) {
    // Round EltSize up to the specified alignment.
    size_t EltSize = (sizeof(T) + Alignment - 1) & (-Alignment);
    return static_cast<T *>(Allocate(Num * EltSize, Alignment));
  }
};

class MallocAllocator : public AllocatorBase<MallocAllocator> {
public:
  MallocAllocator() {}
  ~MallocAllocator() {}

  void Reset() {}

  void *Allocate(size_t Size, size_t /*Alignment*/) { return malloc(Size); }

  // Pull in base class overloads.
  using AllocatorBase<MallocAllocator>::Allocate;

  void Deallocate(const void *Ptr) { free(const_cast<void *>(Ptr)); }

  void PrintStats() const {}
};

/// MallocSlabAllocator - The default slab allocator for the bump allocator
/// is an adapter class for MallocAllocator that just forwards the method
/// calls and translates the arguments.
class MallocSlabAllocator {
  /// Allocator - The underlying allocator that we forward to.
  ///
  MallocAllocator Allocator;

public:
  void *Allocate(size_t Size) { return Allocator.Allocate(Size, 0); }
  void Deallocate(void *Slab, size_t Size) { Allocator.Deallocate(Slab); }
};

namespace detail {

// We call out to an external function to actually print the message as the
// printing code uses Allocator.h in its implementation.
void printBumpPtrAllocatorStats(unsigned NumSlabs, size_t BytesAllocated,
                                size_t TotalMemory);
} // End namespace detail.

/// \brief Allocate memory in an ever growing pool, as if by bump-pointer.
///
/// This isn't strictly a bump-pointer allocator as it uses backing slabs of
/// memory rather than relying on boundless contiguous heap. However, it has
/// bump-pointer semantics in that is a monotonically growing pool of memory
/// where every allocation is found by merely allocating the next N bytes in
/// the slab, or the next N bytes in the next slab.
///
/// Note that this also has a threshold for forcing allocations above a certain
/// size into their own slab.
///
/// The BumpPtrAllocatorImpl template defaults to using a MallocSlabAllocator
/// object, which wraps malloc, to allocate memory, but it can be changed to
/// use a custom allocator.
template <typename AllocatorT = MallocSlabAllocator, size_t SlabSize = 4096,
          size_t SizeThreshold = SlabSize>
class BumpPtrAllocatorImpl
    : public AllocatorBase<
          BumpPtrAllocatorImpl<AllocatorT, SlabSize, SizeThreshold>> {
  BumpPtrAllocatorImpl(const BumpPtrAllocatorImpl &) LLVM_DELETED_FUNCTION;
  void operator=(const BumpPtrAllocatorImpl &) LLVM_DELETED_FUNCTION;

public:
  static_assert(SizeThreshold <= SlabSize,
                "The SizeThreshold must be at most the SlabSize to ensure "
                "that objects larger than a slab go into their own memory "
                "allocation.");

  BumpPtrAllocatorImpl()
      : CurPtr(nullptr), End(nullptr), BytesAllocated(0), Allocator() {}
  template <typename T>
  BumpPtrAllocatorImpl(T &&Allocator)
      : CurPtr(nullptr), End(nullptr), BytesAllocated(0),
        Allocator(std::forward<T &&>(Allocator)) {}
  ~BumpPtrAllocatorImpl() {
    DeallocateSlabs(Slabs.begin(), Slabs.end());
    DeallocateCustomSizedSlabs();
  }

  /// \brief Deallocate all but the current slab and reset the current pointer
  /// to the beginning of it, freeing all memory allocated so far.
  void Reset() {
    if (Slabs.empty())
      return;

    // Reset the state.
    BytesAllocated = 0;
    CurPtr = (char *)Slabs.front();
    End = CurPtr + SlabSize;

    // Deallocate all but the first slab, and all custome sized slabs.
    DeallocateSlabs(std::next(Slabs.begin()), Slabs.end());
    Slabs.erase(std::next(Slabs.begin()), Slabs.end());
    DeallocateCustomSizedSlabs();
    CustomSizedSlabs.clear();
  }

  /// \brief Allocate space at the specified alignment.
  void *Allocate(size_t Size, size_t Alignment) {
    if (!CurPtr) // Start a new slab if we haven't allocated one already.
      StartNewSlab();

    // Keep track of how many bytes we've allocated.
    BytesAllocated += Size;

    // 0-byte alignment means 1-byte alignment.
    if (Alignment == 0)
      Alignment = 1;

    // Allocate the aligned space, going forwards from CurPtr.
    char *Ptr = alignPtr(CurPtr, Alignment);

    // Check if we can hold it.
    if (Ptr + Size <= End) {
      CurPtr = Ptr + Size;
      // Update the allocation point of this memory block in MemorySanitizer.
      // Without this, MemorySanitizer messages for values originated from here
      // will point to the allocation of the entire slab.
      __msan_allocated_memory(Ptr, Size);
      return Ptr;
    }

    // If Size is really big, allocate a separate slab for it.
    size_t PaddedSize = Size + Alignment - 1;
    if (PaddedSize > SizeThreshold) {
      void *NewSlab = Allocator.Allocate(PaddedSize);
      CustomSizedSlabs.push_back(std::make_pair(NewSlab, PaddedSize));

      Ptr = alignPtr((char *)NewSlab, Alignment);
      assert((uintptr_t)Ptr + Size <= (uintptr_t)NewSlab + PaddedSize);
      __msan_allocated_memory(Ptr, Size);
      return Ptr;
    }

    // Otherwise, start a new slab and try again.
    StartNewSlab();
    Ptr = alignPtr(CurPtr, Alignment);
    CurPtr = Ptr + Size;
    assert(CurPtr <= End && "Unable to allocate memory!");
    __msan_allocated_memory(Ptr, Size);
    return Ptr;
  }

  // Pull in base class overloads.
  using AllocatorBase<BumpPtrAllocatorImpl>::Allocate;

  void Deallocate(const void * /*Ptr*/) {}

  size_t GetNumSlabs() const { return Slabs.size() + CustomSizedSlabs.size(); }

  size_t getTotalMemory() const {
    size_t TotalMemory = 0;
    for (auto I = Slabs.begin(), E = Slabs.end(); I != E; ++I)
      TotalMemory += computeSlabSize(std::distance(Slabs.begin(), I));
    for (auto &PtrAndSize : CustomSizedSlabs)
      TotalMemory += PtrAndSize.second;
    return TotalMemory;
  }

  void PrintStats() const {
    detail::printBumpPtrAllocatorStats(Slabs.size(), BytesAllocated,
                                       getTotalMemory());
  }

private:
  /// \brief The current pointer into the current slab.
  ///
  /// This points to the next free byte in the slab.
  char *CurPtr;

  /// \brief The end of the current slab.
  char *End;

  /// \brief The slabs allocated so far.
  SmallVector<void *, 4> Slabs;

  /// \brief Custom-sized slabs allocated for too-large allocation requests.
  SmallVector<std::pair<void *, size_t>, 0> CustomSizedSlabs;

  /// \brief How many bytes we've allocated.
  ///
  /// Used so that we can compute how much space was wasted.
  size_t BytesAllocated;

  /// \brief The allocator instance we use to get slabs of memory.
  AllocatorT Allocator;

  static size_t computeSlabSize(unsigned SlabIdx) {
    // Scale the actual allocated slab size based on the number of slabs
    // allocated. Every 128 slabs allocated, we double the allocated size to
    // reduce allocation frequency, but saturate at multiplying the slab size by
    // 2^30.
    return SlabSize * ((size_t)1 << std::min<size_t>(30, SlabIdx / 128));
  }

  /// \brief Allocate a new slab and move the bump pointers over into the new
  /// slab, modifying CurPtr and End.
  void StartNewSlab() {
    size_t AllocatedSlabSize = computeSlabSize(Slabs.size());

    void *NewSlab = Allocator.Allocate(AllocatedSlabSize);
    Slabs.push_back(NewSlab);
    CurPtr = (char *)(NewSlab);
    End = ((char *)NewSlab) + AllocatedSlabSize;
  }

  /// \brief Deallocate a sequence of slabs.
  void DeallocateSlabs(SmallVectorImpl<void *>::iterator I,
                       SmallVectorImpl<void *>::iterator E) {
    for (; I != E; ++I) {
      size_t AllocatedSlabSize =
          computeSlabSize(std::distance(Slabs.begin(), I));
#ifndef NDEBUG
      // Poison the memory so stale pointers crash sooner.  Note we must
      // preserve the Size and NextPtr fields at the beginning.
      sys::Memory::setRangeWritable(*I, AllocatedSlabSize);
      memset(*I, 0xCD, AllocatedSlabSize);
#endif
      Allocator.Deallocate(*I, AllocatedSlabSize);
    }
  }

  /// \brief Deallocate all memory for custom sized slabs.
  void DeallocateCustomSizedSlabs() {
    for (auto &PtrAndSize : CustomSizedSlabs) {
      void *Ptr = PtrAndSize.first;
      size_t Size = PtrAndSize.second;
#ifndef NDEBUG
      // Poison the memory so stale pointers crash sooner.  Note we must
      // preserve the Size and NextPtr fields at the beginning.
      sys::Memory::setRangeWritable(Ptr, Size);
      memset(Ptr, 0xCD, Size);
#endif
      Allocator.Deallocate(Ptr, Size);
    }
  }

  template <typename T> friend class SpecificBumpPtrAllocator;
};

/// \brief The standard BumpPtrAllocator which just uses the default template
/// paramaters.
typedef BumpPtrAllocatorImpl<> BumpPtrAllocator;

/// \brief A BumpPtrAllocator that allows only elements of a specific type to be
/// allocated.
///
/// This allows calling the destructor in DestroyAll() and when the allocator is
/// destroyed.
template <typename T> class SpecificBumpPtrAllocator {
  BumpPtrAllocator Allocator;

public:
  SpecificBumpPtrAllocator() : Allocator() {}

  ~SpecificBumpPtrAllocator() { DestroyAll(); }

  /// Call the destructor of each allocated object and deallocate all but the
  /// current slab and reset the current pointer to the beginning of it, freeing
  /// all memory allocated so far.
  void DestroyAll() {
    auto DestroyElements = [](char *Begin, char *End) {
      assert(Begin == alignPtr(Begin, alignOf<T>()));
      for (char *Ptr = Begin; Ptr + sizeof(T) <= End; Ptr += sizeof(T))
        reinterpret_cast<T *>(Ptr)->~T();
    };

    for (auto I = Allocator.Slabs.begin(), E = Allocator.Slabs.end(); I != E;
         ++I) {
      size_t AllocatedSlabSize = BumpPtrAllocator::computeSlabSize(
          std::distance(Allocator.Slabs.begin(), I));
      char *Begin = alignPtr((char *)*I, alignOf<T>());
      char *End = *I == Allocator.Slabs.back() ? Allocator.CurPtr
                                               : (char *)*I + AllocatedSlabSize;

      DestroyElements(Begin, End);
    }

    for (auto &PtrAndSize : Allocator.CustomSizedSlabs) {
      void *Ptr = PtrAndSize.first;
      size_t Size = PtrAndSize.second;
      DestroyElements(alignPtr((char *)Ptr, alignOf<T>()), (char *)Ptr + Size);
    }

    Allocator.Reset();
  }

  /// \brief Allocate space for an array of objects without constructing them.
  T *Allocate(size_t num = 1) { return Allocator.Allocate<T>(num); }

private:
};

}  // end namespace llvm

template <typename AllocatorT, size_t SlabSize, size_t SizeThreshold>
void *operator new(size_t Size,
                   llvm::BumpPtrAllocatorImpl<AllocatorT, SlabSize,
                                              SizeThreshold> &Allocator) {
  struct S {
    char c;
    union {
      double D;
      long double LD;
      long long L;
      void *P;
    } x;
  };
  return Allocator.Allocate(
      Size, std::min((size_t)llvm::NextPowerOf2(Size), offsetof(S, x)));
}

template <typename AllocatorT, size_t SlabSize, size_t SizeThreshold>
void operator delete(
    void *, llvm::BumpPtrAllocatorImpl<AllocatorT, SlabSize, SizeThreshold> &) {
}

#endif // LLVM_SUPPORT_ALLOCATOR_H
