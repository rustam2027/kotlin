/*
 * Copyright 2010-2020 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#ifndef RUNTIME_MM_THREAD_DATA_H
#define RUNTIME_MM_THREAD_DATA_H

#include <atomic>
#include <cstdint>
#include <vector>

#include "Common.h"
#include "KAssert.h"
#include "mm/GlobalData.hpp"
#include "mm/GlobalsRegistry.hpp"
#include "gc/GC.hpp"
#include "mm/ShadowStack.hpp"
#include "mm/ExternalRCRefRegistry.hpp"
#include "mm/ThreadLocalStorage.hpp"
#include "Utils.hpp"
#include "mm/ThreadSuspension.hpp"

struct ObjHeader;

namespace kotlin {
namespace mm {

#if defined(__aarch64__)
struct KotlinFrameAnchor {
    uint64_t* fp;
    uint64_t* pc;

    KotlinFrameAnchor() = default;
    KotlinFrameAnchor(uint64_t* fp, uint64_t* pc) : fp(fp), pc(pc) {}

    KotlinFrameAnchor next() {
        RuntimeAssert(fp != nullptr, "Current fp is null, cannot get next");
        return KotlinFrameAnchor((uint64_t*)(*fp), (uint64_t*) (*(fp + 1)));
    }
};

// Reads {fp, pc} from this function's own stack frame. This works only
// because the function is ALWAYS_INLINE: after inlining, its frame is
// gone, so __builtin_frame_address(0) gives the caller's frame.
//
// This function must always be called from one of two places, or the
// result is the wrong frame:
//
//   1. Inside a NO_INLINE function that Kotlin code calls directly, such
//      as Kotlin_mm_switchThreadStateNative_delta_main. That function
//      must be kept NO_INLINE. If it were inlined, the result would be
//      one frame too high: the caller of the caller, not the caller.
//
//   2. Inside slowPathImpl (SafePoint.cpp), the only NO_INLINE function
//      in the safepoint slow path. Every function between the Kotlin
//      call and slowPathImpl must be ALWAYS_INLINE, such as
//      mm::safePoint. PERFORMANCE_INLINE is not enough.
//
// A new function added to either chain must follow the same rule: keep
// it NO_INLINE if it reads its own frame this way; make it ALWAYS_INLINE
// if it only calls shared code.
ALWAYS_INLINE inline KotlinFrameAnchor captureCallerFrameAnchor() {
    uint64_t* fp = reinterpret_cast<uint64_t*>(__builtin_frame_address(0));
    return KotlinFrameAnchor{(uint64_t*) fp[0], (uint64_t*) fp[1]};
}
#endif

// `ThreadData` is supposed to be thread local singleton.
// Pin it in memory to prevent accidental copying.
class ThreadData final : private Pinned {
public:
    explicit ThreadData(uintptr_t threadId) noexcept :
        threadId_(threadId),
        globalsThreadQueue_(GlobalsRegistry::Instance()),
        externalRCRefRegistry_(ExternalRCRefRegistry::instance()),
        gcScheduler_(GlobalData::Instance().gcScheduler(), *this),
        allocator_(GlobalData::Instance().allocator()),
        gc_(GlobalData::Instance().gc(), *this),
        suspensionData_(ThreadState::kNative, *this) {}

    ~ThreadData() = default;

    uintptr_t threadId() const noexcept { return threadId_; }

    GlobalsRegistry::ThreadQueue& globalsThreadQueue() noexcept { return globalsThreadQueue_; }

    ThreadLocalStorage& tls() noexcept { return tls_; }

    ExternalRCRefRegistry::ThreadQueue& externalRCRefRegistry() noexcept { return externalRCRefRegistry_; }

    ThreadState state() noexcept { return suspensionData_.state(); }

    ALWAYS_INLINE ThreadState setState(ThreadState state) noexcept { return suspensionData_.setState(state); }

    ShadowStack& shadowStack() noexcept { return shadowStack_; }

    std::vector<std::pair<ObjHeader**, ObjHeader*>>& initializingSingletons() noexcept { return initializingSingletons_; }

    gcScheduler::GCScheduler::ThreadData& gcScheduler() noexcept { return gcScheduler_; }

    alloc::Allocator::ThreadData& allocator() noexcept { return allocator_; }

    gc::GC::ThreadData& gc() noexcept { return gc_; }

    ThreadSuspensionData& suspensionData() { return suspensionData_; }

    void Publish() noexcept {
        // TODO: These use separate locks, which is inefficient.
        globalsThreadQueue_.Publish();
        externalRCRefRegistry_.publish();
    }

    void ClearForTests() noexcept {
        globalsThreadQueue_.ClearForTests();
        externalRCRefRegistry_.clearForTests();
        allocator_.clearForTests();
    }

#if defined(__aarch64__)
    void pushStackMapAnchor(const KotlinFrameAnchor& anchor) noexcept {
        RuntimeLogInfo({logging::Tag::kLogging}, "Pushing new anchor: fp=%p pc=%p", anchor.fp, anchor.pc);
        frameAnchors_.emplace_back(anchor);
    }

    void pushLastStackMapAnchor() noexcept {
        RuntimeAssert(lastFrame_.fp != nullptr, "Trying push last anchor, but last anchor is not initialized");
        RuntimeLogInfo({logging::Tag::kLogging}, "Pushing last frame anchor: fp=%p pc=%p", lastFrame_.fp, lastFrame_.pc);
        frameAnchors_.emplace_back(lastFrame_);
    }

    void popStackMapAnchor() noexcept {
        RuntimeLogInfo({logging::Tag::kLogging}, "Poping last anchor: fp=%p pc=%p", frameAnchors_.back().fp, frameAnchors_.back().pc);
        frameAnchors_.pop_back();
    }

    const std::vector<KotlinFrameAnchor>& frameAnchors() {
        return frameAnchors_;
    }

    void setLastFrame(KotlinFrameAnchor anchor) noexcept {
        RuntimeLogInfo({logging::Tag::kLogging}, "Setting last frame anchor: fp=%p pc=%p", anchor.fp, anchor.pc);
        lastFrame_ = anchor;
    }
#endif

private:
    const uintptr_t threadId_;
    GlobalsRegistry::ThreadQueue globalsThreadQueue_;
    ThreadLocalStorage tls_;
    ExternalRCRefRegistry::ThreadQueue externalRCRefRegistry_;
    ShadowStack shadowStack_;
    gcScheduler::GCScheduler::ThreadData gcScheduler_;
    alloc::Allocator::ThreadData allocator_;
    gc::GC::ThreadData gc_;
    std::vector<std::pair<ObjHeader**, ObjHeader*>> initializingSingletons_;
    ThreadSuspensionData suspensionData_;
#if defined(__aarch64__)
    std::vector<KotlinFrameAnchor> frameAnchors_;
    KotlinFrameAnchor lastFrame_ = {};
#endif
};

} // namespace mm
} // namespace kotlin

#endif // RUNTIME_MM_THREAD_DATA_H
