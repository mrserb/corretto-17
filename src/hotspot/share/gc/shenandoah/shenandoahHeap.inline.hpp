/*
 * Copyright (c) 2015, 2020, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP

#include "gc/shenandoah/shenandoahHeap.hpp"

#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/threadLocalAllocBuffer.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.inline.hpp"
#include "gc/shenandoah/shenandoahForwarding.inline.hpp"
#include "gc/shenandoah/shenandoahWorkGroup.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/prefetch.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/globalDefinitions.hpp"

inline ShenandoahHeap* ShenandoahHeap::heap() {
  return named_heap<ShenandoahHeap>(CollectedHeap::Shenandoah);
}

inline ShenandoahHeapRegion* ShenandoahRegionIterator::next() {
  size_t new_index = Atomic::add(&_index, (size_t) 1, memory_order_relaxed);
  // get_region() provides the bounds-check and returns NULL on OOB.
  return _heap->get_region(new_index - 1);
}

inline bool ShenandoahHeap::has_forwarded_objects() const {
  return _gc_state.is_set(HAS_FORWARDED);
}

inline WorkGang* ShenandoahHeap::workers() const {
  return _workers;
}

inline WorkGang* ShenandoahHeap::safepoint_workers() {
  return _safepoint_workers;
}

inline size_t ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) base()) >> ShenandoahHeapRegion::region_size_bytes_shift();
  assert(index < num_regions(), "Region index is in bounds: " PTR_FORMAT, p2i(addr));
  return index;
}

inline ShenandoahHeapRegion* const ShenandoahHeap::heap_region_containing(const void* addr) const {
  size_t index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* const result = get_region(index);
  assert(addr >= result->bottom() && addr < result->end(), "Heap region contains the address: " PTR_FORMAT, p2i(addr));
  return result;
}

inline void ShenandoahHeap::enter_evacuation(Thread* t) {
  _oom_evac_handler.enter_evacuation(t);
}

inline void ShenandoahHeap::leave_evacuation(Thread* t) {
  _oom_evac_handler.leave_evacuation(t);
}

template <class T>
inline void ShenandoahHeap::update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    if (in_collection_set(obj)) {
      // Corner case: when evacuation fails, there are objects in collection
      // set that are not really forwarded. We can still go and try and update them
      // (uselessly) to simplify the common path.
      shenandoah_assert_forwarded_except(p, obj, cancelled_gc());
      oop fwd = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      shenandoah_assert_not_in_cset_except(p, fwd, cancelled_gc());

      // Unconditionally store the update: no concurrent updates expected.
      RawAccess<IS_NOT_NULL>::oop_store(p, fwd);
    }
  }
}

template <class T>
inline void ShenandoahHeap::conc_update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    if (in_collection_set(obj)) {
      // Corner case: when evacuation fails, there are objects in collection
      // set that are not really forwarded. We can still go and try CAS-update them
      // (uselessly) to simplify the common path.
      shenandoah_assert_forwarded_except(p, obj, cancelled_gc());
      oop fwd = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      shenandoah_assert_not_in_cset_except(p, fwd, cancelled_gc());

      // Sanity check: we should not be updating the cset regions themselves,
      // unless we are recovering from the evacuation failure.
      shenandoah_assert_not_in_cset_loc_except(p, !is_in(p) || cancelled_gc());

      // Either we succeed in updating the reference, or something else gets in our way.
      // We don't care if that is another concurrent GC update, or another mutator update.
      // We only check that non-NULL store still updated with non-forwarded reference.
      oop witness = cas_oop(fwd, p, obj);
      shenandoah_assert_not_forwarded_except(p, witness, (witness == NULL) || (witness == obj));
    }
  }
}

inline oop ShenandoahHeap::cas_oop(oop n, oop* addr, oop c) {
  assert(is_aligned(addr, HeapWordSize), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  return (oop) Atomic::cmpxchg(addr, c, n);
}

inline oop ShenandoahHeap::cas_oop(oop n, narrowOop* addr, narrowOop c) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop val = CompressedOops::encode(n);
  return CompressedOops::decode(Atomic::cmpxchg(addr, c, val));
}

inline oop ShenandoahHeap::cas_oop(oop n, narrowOop* addr, oop c) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop cmp = CompressedOops::encode(c);
  narrowOop val = CompressedOops::encode(n);
  return CompressedOops::decode(Atomic::cmpxchg(addr, cmp, val));
}

inline bool ShenandoahHeap::cancelled_gc() const {
  return _cancelled_gc.get() == CANCELLED;
}

inline bool ShenandoahHeap::check_cancelled_gc_and_yield(bool sts_active) {
  if (! (sts_active && ShenandoahSuspendibleWorkers)) {
    return cancelled_gc();
  }

  jbyte prev = _cancelled_gc.cmpxchg(NOT_CANCELLED, CANCELLABLE);
  if (prev == CANCELLABLE || prev == NOT_CANCELLED) {
    if (SuspendibleThreadSet::should_yield()) {
      SuspendibleThreadSet::yield();
    }

    // Back to CANCELLABLE. The thread that poked NOT_CANCELLED first gets
    // to restore to CANCELLABLE.
    if (prev == CANCELLABLE) {
      _cancelled_gc.set(CANCELLABLE);
    }
    return false;
  } else {
    return true;
  }
}

inline void ShenandoahHeap::clear_cancelled_gc(bool clear_oom_handler) {
  _cancelled_gc.set(CANCELLABLE);
  if (_cancel_requested_time > 0) {
    double cancel_time = os::elapsedTime() - _cancel_requested_time;
    log_info(gc)("GC cancellation took %.3fs", cancel_time);
    _cancel_requested_time = 0;
  }

  if (clear_oom_handler) {
    _oom_evac_handler.clear();
  }
}

inline HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
  if (gclab == NULL) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(),
           "Performance: thread should have GCLAB: %s", thread->name());
    // No GCLABs in this thread, fallback to shared allocation
    return NULL;
  }
  HeapWord* obj = gclab->allocate(size);
  if (obj != NULL) {
    return obj;
  }
  return allocate_from_gclab_slow(thread, size);
}

inline HeapWord* ShenandoahHeap::allocate_from_plab(Thread* thread, size_t size, bool is_promotion) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  if (is_promotion && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
    return NULL;
  } else if (plab == NULL) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(),
           "Performance: thread should have PLAB: %s", thread->name());
    // No PLABs in this thread, fallback to shared allocation
    return NULL;
  }
  HeapWord* obj = plab->allocate(size);
  if (obj == NULL) {
    obj = allocate_from_plab_slow(thread, size, is_promotion);
  }
  if (is_promotion) {
    ShenandoahThreadLocalData::add_to_plab_promoted(thread, size * HeapWordSize);
  } else {
    ShenandoahThreadLocalData::add_to_plab_evacuated(thread, size * HeapWordSize);
  }
  return obj;
}

inline oop ShenandoahHeap::evacuate_object(oop p, Thread* thread) {
  assert(thread == Thread::current(), "Expected thread parameter to be current thread.");
  if (ShenandoahThreadLocalData::is_oom_during_evac(thread)) {
    // This thread went through the OOM during evac protocol and it is safe to return
    // the forward pointer. It must not attempt to evacuate any more.
    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  assert(ShenandoahThreadLocalData::is_evac_allowed(thread), "must be enclosed in oom-evac scope");

  ShenandoahHeapRegion* r = heap_region_containing(p);
  assert(!r->is_humongous(), "never evacuate humongous objects");

  ShenandoahRegionAffiliation target_gen = r->affiliation();
  if (mode()->is_generational() && ShenandoahHeap::heap()->is_gc_generation_young() &&
      target_gen == YOUNG_GENERATION && ShenandoahPromoteTenuredObjects) {
    markWord mark = p->mark();
    if (mark.is_marked()) {
      // Already forwarded.
      return ShenandoahBarrierSet::resolve_forwarded(p);
    }
    if (mark.has_displaced_mark_helper()) {
      // We don't want to deal with MT here just to ensure we read the right mark word.
      // Skip the potential promotion attempt for this one.
    } else if (r->age() + mark.age() >= InitialTenuringThreshold) {
      oop result = try_evacuate_object(p, thread, r, OLD_GENERATION);
      if (result != NULL) {
        return result;
      }
      // If we failed to promote this aged object, we'll fall through to code below and evacuat to young-gen.
    }
  }
  return try_evacuate_object(p, thread, r, target_gen);
}

// try_evacuate_object registers the object and dirties the associated remembered set information when evacuating
// to OLD_GENERATION.
inline oop ShenandoahHeap::try_evacuate_object(oop p, Thread* thread, ShenandoahHeapRegion* from_region,
                                               ShenandoahRegionAffiliation target_gen) {
  bool alloc_from_lab = true;
  HeapWord* copy = NULL;
  size_t size = p->size();
  bool is_promotion = (target_gen == OLD_GENERATION) && from_region->is_young();

#ifdef ASSERT
  if (ShenandoahOOMDuringEvacALot &&
      (os::random() & 1) == 0) { // Simulate OOM every ~2nd slow-path call
        copy = NULL;
  } else {
#endif
    if (UseTLAB) {
      switch (target_gen) {
        case YOUNG_GENERATION: {
           copy = allocate_from_gclab(thread, size);
           if ((copy == nullptr) && (size < ShenandoahThreadLocalData::gclab_size(thread))) {
             // GCLAB allocation failed because we are bumping up against the limit on young evacuation reserve.  Try resetting
             // the desired GCLAB size and retry GCLAB allocation to avoid cascading of shared memory allocations.
             ShenandoahThreadLocalData::set_gclab_size(thread, PLAB::min_size());
             copy = allocate_from_gclab(thread, size);
             // If we still get nullptr, we'll try a shared allocation below.
           }
           break;
        }
        case OLD_GENERATION: {
           if (ShenandoahUsePLAB) {
             copy = allocate_from_plab(thread, size, is_promotion);
             if ((copy == nullptr) && (size < ShenandoahThreadLocalData::plab_size(thread))) {
               // PLAB allocation failed because we are bumping up against the limit on old evacuation reserve.  Try resetting
               // the desired PLAB size and retry PLAB allocation to avoid cascading of shared memory allocations.
               ShenandoahThreadLocalData::set_plab_size(thread, PLAB::min_size());
               copy = allocate_from_plab(thread, size, is_promotion);
               // If we still get nullptr, we'll try a shared allocation below.
             }
           }
           break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    }

    if (copy == NULL) {
      // If we failed to allocated in LAB, we'll try a shared allocation.
      ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared_gc(size, target_gen);
      copy = allocate_memory(req, is_promotion);
      alloc_from_lab = false;
    }
#ifdef ASSERT
  }
#endif

  if (copy == NULL) {
    if (target_gen == OLD_GENERATION) {
      assert(mode()->is_generational(), "Should only be here in generational mode.");
      if (from_region->is_young()) {
        // Signal that promotion failed. Will evacuate this old object somewhere in young gen.
        handle_promotion_failure();
        return NULL;
      } else {
        // Remember that evacuation to old gen failed. We'll want to trigger a full gc to recover from this
        // after the evacuation threads have finished.
        handle_old_evacuation_failure();
      }
    }

    control_thread()->handle_alloc_failure_evac(size);

    _oom_evac_handler.handle_out_of_memory_during_evacuation();

    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  // Copy the object:
  Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(p), copy, size);

  oop copy_val = cast_to_oop(copy);

  // Try to install the new forwarding pointer.
  oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  if (result == copy_val) {
    // Successfully evacuated. Our copy is now the public one!
    if (mode()->is_generational()) {
      if (target_gen == OLD_GENERATION) {
        handle_old_evacuation(copy, size, from_region->is_young());
      } else if (target_gen == YOUNG_GENERATION) {
        if (is_aging_cycle()) {
          ShenandoahHeap::increase_object_age(copy_val, from_region->age() + 1);
        }
      } else {
        ShouldNotReachHere();
      }
    }
    shenandoah_assert_correct(NULL, copy_val);
    return copy_val;
  }  else {
    // Failed to evacuate. We need to deal with the object that is left behind. Since this
    // new allocation is certainly after TAMS, it will be considered live in the next cycle.
    // But if it happens to contain references to evacuated regions, those references would
    // not get updated for this stale copy during this cycle, and we will crash while scanning
    // it the next cycle.
    if (alloc_from_lab) {
       // For LAB allocations, it is enough to rollback the allocation ptr. Either the next
       // object will overwrite this stale copy, or the filler object on LAB retirement will
       // do this.
       switch (target_gen) {
         case YOUNG_GENERATION: {
             ShenandoahThreadLocalData::gclab(thread)->undo_allocation(copy, size);
            break;
         }
         case OLD_GENERATION: {
            ShenandoahThreadLocalData::plab(thread)->undo_allocation(copy, size);
            if (is_promotion) {
              ShenandoahThreadLocalData::subtract_from_plab_promoted(thread, size * HeapWordSize);
            } else {
              ShenandoahThreadLocalData::subtract_from_plab_evacuated(thread, size * HeapWordSize);
            }
            break;
         }
         default: {
           ShouldNotReachHere();
           break;
         }
       }
    } else {
      // For non-LAB allocations, we have no way to retract the allocation, and
      // have to explicitly overwrite the copy with the filler object. With that overwrite,
      // we have to keep the fwdptr initialized and pointing to our (stale) copy.
      fill_with_object(copy, size);
      shenandoah_assert_correct(NULL, copy_val);
      // For non-LAB allocations, the object has already been registered
    }
    shenandoah_assert_correct(NULL, result);
    return result;
  }
}

void ShenandoahHeap::increase_object_age(oop obj, uint additional_age) {
  markWord w = obj->has_displaced_mark() ? obj->displaced_mark() : obj->mark();
  w = w.set_age(MIN2(markWord::max_age, w.age() + additional_age));
  if (obj->has_displaced_mark()) {
    obj->set_displaced_mark(w);
  } else {
    obj->set_mark(w);
  }
}

inline bool ShenandoahHeap::clear_old_evacuation_failure() {
  return _old_gen_oom_evac.try_unset();
}

inline bool ShenandoahHeap::is_old(oop obj) const {
  return is_gc_generation_young() && is_in_old(obj);
}

inline bool ShenandoahHeap::requires_marking(const void* entry) const {
  oop obj = cast_to_oop(entry);
  return !_marking_context->is_marked_strong(obj);
}

inline bool ShenandoahHeap::in_collection_set(oop p) const {
  assert(collection_set() != NULL, "Sanity");
  return collection_set()->is_in(p);
}

inline bool ShenandoahHeap::in_collection_set_loc(void* p) const {
  assert(collection_set() != NULL, "Sanity");
  return collection_set()->is_in_loc(p);
}

inline bool ShenandoahHeap::is_stable() const {
  return _gc_state.is_clear();
}

inline bool ShenandoahHeap::is_idle() const {
  return _gc_state.is_unset(YOUNG_MARKING | OLD_MARKING | EVACUATION | UPDATEREFS);
}

inline bool ShenandoahHeap::is_concurrent_mark_in_progress() const {
  return _gc_state.is_set(YOUNG_MARKING | OLD_MARKING);
}

inline bool ShenandoahHeap::is_concurrent_young_mark_in_progress() const {
  return _gc_state.is_set(YOUNG_MARKING);
}

inline bool ShenandoahHeap::is_concurrent_old_mark_in_progress() const {
  return _gc_state.is_set(OLD_MARKING);
}

inline bool ShenandoahHeap::is_evacuation_in_progress() const {
  return _gc_state.is_set(EVACUATION);
}

inline bool ShenandoahHeap::is_gc_in_progress_mask(uint mask) const {
  return _gc_state.is_set(mask);
}

inline bool ShenandoahHeap::is_degenerated_gc_in_progress() const {
  return _degenerated_gc_in_progress.is_set();
}

inline bool ShenandoahHeap::is_full_gc_in_progress() const {
  return _full_gc_in_progress.is_set();
}

inline bool ShenandoahHeap::is_full_gc_move_in_progress() const {
  return _full_gc_move_in_progress.is_set();
}

inline bool ShenandoahHeap::is_update_refs_in_progress() const {
  return _gc_state.is_set(UPDATEREFS);
}

inline bool ShenandoahHeap::is_stw_gc_in_progress() const {
  return is_full_gc_in_progress() || is_degenerated_gc_in_progress();
}

inline bool ShenandoahHeap::is_concurrent_strong_root_in_progress() const {
  return _concurrent_strong_root_in_progress.is_set();
}

inline bool ShenandoahHeap::is_concurrent_weak_root_in_progress() const {
  return _gc_state.is_set(WEAK_ROOTS);
}

inline bool ShenandoahHeap::is_aging_cycle() const {
  return _is_aging_cycle.is_set();
}

inline size_t ShenandoahHeap::set_promotion_reserve(size_t new_val) {
  size_t orig = _promotion_reserve;
  _promotion_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_promotion_reserve() const {
  return _promotion_reserve;
}

// returns previous value
size_t ShenandoahHeap::capture_old_usage(size_t old_usage) {
  size_t previous_value = _captured_old_usage;
  _captured_old_usage = old_usage;
  return previous_value;
}

void ShenandoahHeap::set_previous_promotion(size_t promoted_bytes) {
  _previous_promotion = promoted_bytes;
}

size_t ShenandoahHeap::get_previous_promotion() const {
  return _previous_promotion;
}

inline size_t ShenandoahHeap::set_old_evac_reserve(size_t new_val) {
  size_t orig = _old_evac_reserve;
  _old_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_old_evac_reserve() const {
  return _old_evac_reserve;
}

inline void ShenandoahHeap::reset_old_evac_expended() {
  _old_evac_expended = 0;
}

inline size_t ShenandoahHeap::expend_old_evac(size_t increment) {
  _old_evac_expended += increment;
  return _old_evac_expended;
}

inline size_t ShenandoahHeap::get_old_evac_expended() const {
  return _old_evac_expended;
}

inline size_t ShenandoahHeap::set_young_evac_reserve(size_t new_val) {
  size_t orig = _young_evac_reserve;
  _young_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_young_evac_reserve() const {
  return _young_evac_reserve;
}

inline void ShenandoahHeap::reset_young_evac_expended() {
  _young_evac_expended = 0;
}

inline size_t ShenandoahHeap::expend_young_evac(size_t increment) {
  _young_evac_expended += increment;
  return _young_evac_expended;
}

inline size_t ShenandoahHeap::get_young_evac_expended() const {
  return _young_evac_expended;
}

inline intptr_t ShenandoahHeap::set_alloc_supplement_reserve(intptr_t new_val) {
  intptr_t orig = _alloc_supplement_reserve;
  _alloc_supplement_reserve = new_val;
  return orig;
}

inline intptr_t ShenandoahHeap::get_alloc_supplement_reserve() const {
  return _alloc_supplement_reserve;
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_iterate(region, cl, region->top());
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit) {
  assert(! region->is_humongous_continuation(), "no humongous continuation regions here");

  ShenandoahMarkingContext* const ctx = marking_context();

  HeapWord* tams = ctx->top_at_mark_start(region);

  size_t skip_bitmap_delta = 1;
  HeapWord* start = region->bottom();
  HeapWord* end = MIN2(tams, region->end());

  // Step 1. Scan below the TAMS based on bitmap data.
  HeapWord* limit_bitmap = MIN2(limit, tams);

  // Try to scan the initial candidate. If the candidate is above the TAMS, it would
  // fail the subsequent "< limit_bitmap" checks, and fall through to Step 2.
  HeapWord* cb = ctx->get_next_marked_addr(start, end);

  intx dist = ShenandoahMarkScanPrefetch;
  if (dist > 0) {
    // Batched scan that prefetches the oop data, anticipating the access to
    // either header, oop field, or forwarding pointer. Not that we cannot
    // touch anything in oop, while it still being prefetched to get enough
    // time for prefetch to work. This is why we try to scan the bitmap linearly,
    // disregarding the object size. However, since we know forwarding pointer
    // preceeds the object, we can skip over it. Once we cannot trust the bitmap,
    // there is no point for prefetching the oop contents, as oop->size() will
    // touch it prematurely.

    // No variable-length arrays in standard C++, have enough slots to fit
    // the prefetch distance.
    static const int SLOT_COUNT = 256;
    guarantee(dist <= SLOT_COUNT, "adjust slot count");
    HeapWord* slots[SLOT_COUNT];

    int avail;
    do {
      avail = 0;
      for (int c = 0; (c < dist) && (cb < limit_bitmap); c++) {
        Prefetch::read(cb, oopDesc::mark_offset_in_bytes());
        slots[avail++] = cb;
        cb += skip_bitmap_delta;
        if (cb < limit_bitmap) {
          cb = ctx->get_next_marked_addr(cb, limit_bitmap);
        }
      }

      for (int c = 0; c < avail; c++) {
        assert (slots[c] < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(tams));
        assert (slots[c] < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(limit));
        oop obj = cast_to_oop(slots[c]);
        assert(oopDesc::is_oop(obj), "sanity");
        assert(ctx->is_marked(obj), "object expected to be marked");
        cl->do_object(obj);
      }
    } while (avail > 0);
  } else {
    while (cb < limit_bitmap) {
      assert (cb < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(tams));
      assert (cb < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(limit));
      oop obj = cast_to_oop(cb);
      assert(oopDesc::is_oop(obj), "sanity");
      assert(ctx->is_marked(obj), "object expected to be marked");
      cl->do_object(obj);
      cb += skip_bitmap_delta;
      if (cb < limit_bitmap) {
        cb = ctx->get_next_marked_addr(cb, limit_bitmap);
      }
    }
  }

  // Step 2. Accurate size-based traversal, happens past the TAMS.
  // This restarts the scan at TAMS, which makes sure we traverse all objects,
  // regardless of what happened at Step 1.
  HeapWord* cs = tams;
  while (cs < limit) {
    assert (cs >= tams, "only objects past TAMS here: "   PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(tams));
    assert (cs < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(limit));
    oop obj = cast_to_oop(cs);
    assert(oopDesc::is_oop(obj), "sanity");
    assert(ctx->is_marked(obj), "object expected to be marked");
    int size = obj->size();
    cl->do_object(obj);
    cs += size;
  }
}

template <class T>
class ShenandoahObjectToOopClosure : public ObjectClosure {
  T* _cl;
public:
  ShenandoahObjectToOopClosure(T* cl) : _cl(cl) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl);
  }
};

template <class T>
class ShenandoahObjectToOopBoundedClosure : public ObjectClosure {
  T* _cl;
  MemRegion _bounds;
public:
  ShenandoahObjectToOopBoundedClosure(T* cl, HeapWord* bottom, HeapWord* top) :
    _cl(cl), _bounds(bottom, top) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl, _bounds);
  }
};

template<class T>
inline void ShenandoahHeap::marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* top) {
  if (region->is_humongous()) {
    HeapWord* bottom = region->bottom();
    if (top > bottom) {
      region = region->humongous_start_region();
      ShenandoahObjectToOopBoundedClosure<T> objs(cl, bottom, top);
      marked_object_iterate(region, &objs);
    }
  } else {
    ShenandoahObjectToOopClosure<T> objs(cl);
    marked_object_iterate(region, &objs, top);
  }
}

inline ShenandoahHeapRegion* const ShenandoahHeap::get_region(size_t region_idx) const {
  if (region_idx < _num_regions) {
    return _regions[region_idx];
  } else {
    return NULL;
  }
}

inline ShenandoahMarkingContext* ShenandoahHeap::complete_marking_context() const {
  assert (_marking_context->is_complete()," sanity");
  return _marking_context;
}

inline ShenandoahMarkingContext* ShenandoahHeap::marking_context() const {
  return _marking_context;
}

inline void ShenandoahHeap::clear_cards_for(ShenandoahHeapRegion* region) {
  if (mode()->is_generational()) {
    _card_scan->mark_range_as_empty(region->bottom(), pointer_delta(region->end(), region->bottom()));
  }
}

inline void ShenandoahHeap::dirty_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_dirty(start, words);
}

inline void ShenandoahHeap::clear_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_clean(start, words);
}

inline void ShenandoahHeap::mark_card_as_dirty(void* location) {
  if (mode()->is_generational()) {
    _card_scan->mark_card_as_dirty((HeapWord*)location);
  }
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
