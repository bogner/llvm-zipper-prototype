//===-- sanitizer_allocator_size_class_map.h --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Part of the Sanitizer Allocator.
//
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_ALLOCATOR_H
#error This file must be included inside sanitizer_allocator.h
#endif

// SizeClassMap maps allocation sizes into size classes and back.
// Class 0 corresponds to size 0.
// Classes 1 - 16 correspond to sizes 16 to 256 (size = class_id * 16).
// Next 4 classes: 256 + i * 64  (i = 1 to 4).
// Next 4 classes: 512 + i * 128 (i = 1 to 4).
// ...
// Next 4 classes: 2^k + i * 2^(k-2) (i = 1 to 4).
// Last class corresponds to kMaxSize = 1 << kMaxSizeLog.
//
// This structure of the size class map gives us:
//   - Efficient table-free class-to-size and size-to-class functions.
//   - Difference between two consequent size classes is betweed 14% and 25%
//
// This class also gives a hint to a thread-caching allocator about the amount
// of chunks that need to be cached per-thread:
//  - kMaxNumCached is the maximal number of chunks per size class.
//  - (1 << kMaxBytesCachedLog) is the maximal number of bytes per size class.
//
// There is one extra size class kBatchClassID that is used for allocating
// objects of TransferBatch type when kUseSeparateSizeClassForBatch is true.
//
// Part of output of SizeClassMap::Print():
// c00 => s: 0 diff: +0 00% l 0 cached: 0 0; id 0
// c01 => s: 16 diff: +16 00% l 4 cached: 256 4096; id 1
// c02 => s: 32 diff: +16 100% l 5 cached: 256 8192; id 2
// c03 => s: 48 diff: +16 50% l 5 cached: 256 12288; id 3
// c04 => s: 64 diff: +16 33% l 6 cached: 256 16384; id 4
// c05 => s: 80 diff: +16 25% l 6 cached: 256 20480; id 5
// c06 => s: 96 diff: +16 20% l 6 cached: 256 24576; id 6
// c07 => s: 112 diff: +16 16% l 6 cached: 256 28672; id 7
//
// c08 => s: 128 diff: +16 14% l 7 cached: 256 32768; id 8
// c09 => s: 144 diff: +16 12% l 7 cached: 256 36864; id 9
// c10 => s: 160 diff: +16 11% l 7 cached: 256 40960; id 10
// c11 => s: 176 diff: +16 10% l 7 cached: 256 45056; id 11
// c12 => s: 192 diff: +16 09% l 7 cached: 256 49152; id 12
// c13 => s: 208 diff: +16 08% l 7 cached: 256 53248; id 13
// c14 => s: 224 diff: +16 07% l 7 cached: 256 57344; id 14
// c15 => s: 240 diff: +16 07% l 7 cached: 256 61440; id 15
//
// c16 => s: 256 diff: +16 06% l 8 cached: 256 65536; id 16
// c17 => s: 320 diff: +64 25% l 8 cached: 204 65280; id 17
// c18 => s: 384 diff: +64 20% l 8 cached: 170 65280; id 18
// c19 => s: 448 diff: +64 16% l 8 cached: 146 65408; id 19
//
// c20 => s: 512 diff: +64 14% l 9 cached: 128 65536; id 20
// c21 => s: 640 diff: +128 25% l 9 cached: 102 65280; id 21
// c22 => s: 768 diff: +128 20% l 9 cached: 85 65280; id 22
// c23 => s: 896 diff: +128 16% l 9 cached: 73 65408; id 23
//
// c24 => s: 1024 diff: +128 14% l 10 cached: 64 65536; id 24
// c25 => s: 1280 diff: +256 25% l 10 cached: 51 65280; id 25
// c26 => s: 1536 diff: +256 20% l 10 cached: 42 64512; id 26
// c27 => s: 1792 diff: +256 16% l 10 cached: 36 64512; id 27
//
// ...
//
// c48 => s: 65536 diff: +8192 14% l 16 cached: 1 65536; id 48
// c49 => s: 81920 diff: +16384 25% l 16 cached: 1 81920; id 49
// c50 => s: 98304 diff: +16384 20% l 16 cached: 1 98304; id 50
// c51 => s: 114688 diff: +16384 16% l 16 cached: 1 114688; id 51
//
// c52 => s: 131072 diff: +16384 14% l 17 cached: 1 131072; id 52

template <uptr kMaxSizeLog, uptr kMaxNumCachedT, uptr kMaxBytesCachedLog>
class SizeClassMap {
  static const uptr kMinSizeLog = 4;
  static const uptr kMidSizeLog = kMinSizeLog + 4;
  static const uptr kMinSize = 1 << kMinSizeLog;
  static const uptr kMidSize = 1 << kMidSizeLog;
  static const uptr kMidClass = kMidSize / kMinSize;
  static const uptr S = 2;
  static const uptr M = (1 << S) - 1;

 public:
  static const uptr kMaxNumCached = kMaxNumCachedT;
  COMPILER_CHECK(((kMaxNumCached + 2) & (kMaxNumCached + 1)) == 0);
  // We transfer chunks between central and thread-local free lists in batches.
  // For small size classes we allocate batches separately.
  // For large size classes we use one of the chunks to store the batch.
  // sizeof(TransferBatch) must be a power of 2 for more efficient allocation.
  struct TransferBatch {
    void SetFromRange(uptr region_beg, uptr beg_offset, uptr step, uptr count) {
      count_ = count;
      for (uptr i = 0; i < count; i++)
        batch_[i] = (void*)(region_beg + beg_offset + i * step);
    }
    void SetFromArray(void *batch[], uptr count) {
      count_ = count;
      for (uptr i = 0; i < count; i++)
        batch_[i] = batch[i];
    }
    void *Get(uptr idx) {
      CHECK_LT(idx, count_);
      return batch_[idx];
    }
    uptr Count() const { return count_; }
    TransferBatch *next;
   private:
    uptr count_;
    void *batch_[kMaxNumCached];
  };
  static const uptr kBatchSize = sizeof(TransferBatch);
  COMPILER_CHECK((kBatchSize & (kBatchSize - 1)) == 0);

  // If true, all TransferBatch objects are allocated from kBatchClassID
  // size class (except for those that are needed for kBatchClassID itself).
  // The goal is to have TransferBatches in a totally different region of RAM
  // to improve security and allow more efficient RAM reclamation.
  // This is experimental and may currently increase memory usage by up to 3%
  // in extreme cases.
  static const bool kUseSeparateSizeClassForBatch = false;


  static const uptr kMaxSize = 1UL << kMaxSizeLog;
  static const uptr kNumClasses =
      kMidClass + ((kMaxSizeLog - kMidSizeLog) << S) + 1 + 1;
  static const uptr kBatchClassID = kNumClasses - 1;
  COMPILER_CHECK(kNumClasses >= 32 && kNumClasses <= 256);
  static const uptr kNumClassesRounded =
      kNumClasses == 32  ? 32 :
      kNumClasses <= 64  ? 64 :
      kNumClasses <= 128 ? 128 : 256;

  static uptr Size(uptr class_id) {
    if (class_id <= kMidClass)
      return kMinSize * class_id;
    if (class_id == kBatchClassID)
      return kBatchSize;
    class_id -= kMidClass;
    uptr t = kMidSize << (class_id >> S);
    return t + (t >> S) * (class_id & M);
  }

  static uptr ClassID(uptr size) {
    if (size <= kMidSize)
      return (size + kMinSize - 1) >> kMinSizeLog;
    if (size > kMaxSize) return 0;
    uptr l = MostSignificantSetBitIndex(size);
    uptr hbits = (size >> (l - S)) & M;
    uptr lbits = size & ((1 << (l - S)) - 1);
    uptr l1 = l - kMidSizeLog;
    return kMidClass + (l1 << S) + hbits + (lbits > 0);
  }

  static uptr MaxCached(uptr class_id) {
    if (class_id == 0) return 0;
    uptr n = (1UL << kMaxBytesCachedLog) / Size(class_id);
    return Max<uptr>(1, Min(kMaxNumCached, n));
  }

  static void Print() {
    uptr prev_s = 0;
    uptr total_cached = 0;
    for (uptr i = 0; i < kNumClasses; i++) {
      uptr s = Size(i);
      if (s >= kMidSize / 2 && (s & (s - 1)) == 0)
        Printf("\n");
      uptr d = s - prev_s;
      uptr p = prev_s ? (d * 100 / prev_s) : 0;
      uptr l = s ? MostSignificantSetBitIndex(s) : 0;
      uptr cached = MaxCached(i) * s;
      if (i == kBatchClassID)
        d = l = p = 0;
      Printf("c%02zd => s: %zd diff: +%zd %02zd%% l %zd "
             "cached: %zd %zd; id %zd\n",
             i, Size(i), d, p, l, MaxCached(i), cached, ClassID(s));
      total_cached += cached;
      prev_s = s;
    }
    Printf("Total cached: %zd\n", total_cached);
  }

  static uptr SizeClassForTransferBatch(uptr class_id) {
    if (kUseSeparateSizeClassForBatch)
      return class_id == kBatchClassID ? 0 : kBatchClassID;
    if (Size(class_id) < sizeof(TransferBatch) -
        sizeof(uptr) * (kMaxNumCached - MaxCached(class_id)))
      return ClassID(sizeof(TransferBatch));
    return 0;
  }

  static void Validate() {
    for (uptr c = 1; c < kNumClasses; c++) {
      if (c == kBatchClassID) continue;
      // Printf("Validate: c%zd\n", c);
      uptr s = Size(c);
      CHECK_NE(s, 0U);
      CHECK_EQ(ClassID(s), c);
      if (c != kBatchClassID - 1 && c != kNumClasses - 1)
        CHECK_EQ(ClassID(s + 1), c + 1);
      CHECK_EQ(ClassID(s - 1), c);
      if (c)
        CHECK_GT(Size(c), Size(c-1));
    }
    CHECK_EQ(ClassID(kMaxSize + 1), 0);

    for (uptr s = 1; s <= kMaxSize; s++) {
      uptr c = ClassID(s);
      // Printf("s%zd => c%zd\n", s, c);
      CHECK_LT(c, kNumClasses);
      CHECK_GE(Size(c), s);
      if (c > 0)
        CHECK_LT(Size(c-1), s);
    }
  }
};

typedef SizeClassMap<17, 126, 16> DefaultSizeClassMap;
typedef SizeClassMap<17, 62,  14> CompactSizeClassMap;
template<class SizeClassAllocator> struct SizeClassAllocatorLocalCache;
