/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Tracer_h
#define js_Tracer_h

#include "mozilla/DebugOnly.h"

#include "jsutil.h"

#include "js/GCAPI.h"
#include "js/SliceBudget.h"
#include "js/TracingAPI.h"

namespace js {
class ObjectImpl;
namespace gc {
class ArenaHeader;
}
namespace jit {
class JitCode;
}
namespace types {
class TypeObject;
}

static const size_t NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY = 4096;
static const size_t INCREMENTAL_MARK_STACK_BASE_CAPACITY = 32768;

/*
 * When the native stack is low, the GC does not call JS_TraceChildren to mark
 * the reachable "children" of the thing. Rather the thing is put aside and
 * JS_TraceChildren is called later with more space on the C stack.
 *
 * To implement such delayed marking of the children with minimal overhead for
 * the normal case of sufficient native stack, the code adds a field per arena.
 * The field markingDelay->link links all arenas with delayed things into a
 * stack list with the pointer to stack top in GCMarker::unmarkedArenaStackTop.
 * GCMarker::delayMarkingChildren adds arenas to the stack as necessary while
 * markDelayedChildren pops the arenas from the stack until it empties.
 */
template<class T>
struct MarkStack {
    T *stack_;
    T *tos_;
    T *end_;

    // The capacity we start with and reset() to.
    size_t baseCapacity_;
    size_t maxCapacity_;

    MarkStack(size_t maxCapacity)
      : stack_(nullptr),
        tos_(nullptr),
        end_(nullptr),
        baseCapacity_(0),
        maxCapacity_(maxCapacity)
    {}

    ~MarkStack() {
        js_free(stack_);
    }

    size_t capacity() { return end_ - stack_; }

    ptrdiff_t position() const { return tos_ - stack_; }

    void setStack(T *stack, size_t tosIndex, size_t capacity) {
        stack_ = stack;
        tos_ = stack + tosIndex;
        end_ = stack + capacity;
    }

    void setBaseCapacity(JSGCMode mode) {
        switch (mode) {
          case JSGC_MODE_GLOBAL:
          case JSGC_MODE_COMPARTMENT:
            baseCapacity_ = NON_INCREMENTAL_MARK_STACK_BASE_CAPACITY;
            break;
          case JSGC_MODE_INCREMENTAL:
            baseCapacity_ = INCREMENTAL_MARK_STACK_BASE_CAPACITY;
            break;
          default:
            MOZ_ASSUME_UNREACHABLE("bad gc mode");
        }

        if (baseCapacity_ > maxCapacity_)
            baseCapacity_ = maxCapacity_;
    }

    bool init(JSGCMode gcMode) {
        setBaseCapacity(gcMode);

        JS_ASSERT(!stack_);
        T *newStack = js_pod_malloc<T>(baseCapacity_);
        if (!newStack)
            return false;

        setStack(newStack, 0, baseCapacity_);
        return true;
    }

    void setMaxCapacity(size_t maxCapacity) {
        JS_ASSERT(isEmpty());
        maxCapacity_ = maxCapacity;
        if (baseCapacity_ > maxCapacity_)
            baseCapacity_ = maxCapacity_;

        reset();
    }

    bool push(T item) {
        if (tos_ == end_) {
            if (!enlarge(1))
                return false;
        }
        JS_ASSERT(tos_ < end_);
        *tos_++ = item;
        return true;
    }

    bool push(T item1, T item2, T item3) {
        T *nextTos = tos_ + 3;
        if (nextTos > end_) {
            if (!enlarge(3))
                return false;
            nextTos = tos_ + 3;
        }
        JS_ASSERT(nextTos <= end_);
        tos_[0] = item1;
        tos_[1] = item2;
        tos_[2] = item3;
        tos_ = nextTos;
        return true;
    }

    bool isEmpty() const {
        return tos_ == stack_;
    }

    T pop() {
        JS_ASSERT(!isEmpty());
        return *--tos_;
    }

    void reset() {
        if (capacity() == baseCapacity_) {
            // No size change; keep the current stack.
            setStack(stack_, 0, baseCapacity_);
            return;
        }

        T *newStack = (T *)js_realloc(stack_, sizeof(T) * baseCapacity_);
        if (!newStack) {
            // If the realloc fails, just keep using the existing stack; it's
            // not ideal but better than failing.
            newStack = stack_;
            baseCapacity_ = capacity();
        }
        setStack(newStack, 0, baseCapacity_);
    }

    /* Grow the stack, ensuring there is space for at least count elements. */
    bool enlarge(unsigned count) {
        size_t newCapacity = Min(maxCapacity_, capacity() * 2);
        if (newCapacity < capacity() + count)
            return false;

        size_t tosIndex = position();

        T *newStack = (T *)js_realloc(stack_, sizeof(T) * newCapacity);
        if (!newStack)
            return false;

        setStack(newStack, tosIndex, newCapacity);
        return true;
    }

    void setGCMode(JSGCMode gcMode) {
        // The mark stack won't be resized until the next call to reset(), but
        // that will happen at the end of the next GC.
        setBaseCapacity(gcMode);
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(stack_);
    }
};

struct GCMarker : public JSTracer {
  private:
    /*
     * We use a common mark stack to mark GC things of different types and use
     * the explicit tags to distinguish them when it cannot be deduced from
     * the context of push or pop operation.
     */
    enum StackTag {
        ValueArrayTag,
        ObjectTag,
        TypeTag,
        XmlTag,
        SavedValueArrayTag,
        JitCodeTag,
        LastTag = JitCodeTag
    };

    static const uintptr_t StackTagMask = 7;

    static void staticAsserts() {
        JS_STATIC_ASSERT(StackTagMask >= uintptr_t(LastTag));
        JS_STATIC_ASSERT(StackTagMask <= gc::CellMask);
    }

  public:
    explicit GCMarker(JSRuntime *rt);
    bool init(JSGCMode gcMode);

    void setMaxCapacity(size_t maxCap) { stack.setMaxCapacity(maxCap); }
    size_t maxCapacity() const { return stack.maxCapacity_; }

    void start();
    void stop();
    void reset();

    void pushObject(ObjectImpl *obj) {
        pushTaggedPtr(ObjectTag, obj);
    }

    void pushType(types::TypeObject *type) {
        pushTaggedPtr(TypeTag, type);
    }

    void pushJitCode(jit::JitCode *code) {
        pushTaggedPtr(JitCodeTag, code);
    }

    uint32_t getMarkColor() const {
        return color;
    }

    /*
     * Care must be taken changing the mark color from gray to black. The cycle
     * collector depends on the invariant that there are no black to gray edges
     * in the GC heap. This invariant lets the CC not trace through black
     * objects. If this invariant is violated, the cycle collector may free
     * objects that are still reachable.
     */
    void setMarkColorGray() {
        JS_ASSERT(isDrained());
        JS_ASSERT(color == gc::BLACK);
        color = gc::GRAY;
    }

    void setMarkColorBlack() {
        JS_ASSERT(isDrained());
        JS_ASSERT(color == gc::GRAY);
        color = gc::BLACK;
    }

    inline void delayMarkingArena(gc::ArenaHeader *aheader);
    void delayMarkingChildren(const void *thing);
    void markDelayedChildren(gc::ArenaHeader *aheader);
    bool markDelayedChildren(SliceBudget &budget);
    bool hasDelayedChildren() const {
        return !!unmarkedArenaStackTop;
    }

    bool isDrained() {
        return isMarkStackEmpty() && !unmarkedArenaStackTop;
    }

    bool drainMarkStack(SliceBudget &budget);

    /*
     * Gray marking must be done after all black marking is complete. However,
     * we do not have write barriers on XPConnect roots. Therefore, XPConnect
     * roots must be accumulated in the first slice of incremental GC. We
     * accumulate these roots in the each compartment's gcGrayRoots vector and
     * then mark them later, after black marking is complete for each
     * compartment. This accumulation can fail, but in that case we switch to
     * non-incremental GC.
     */
    bool hasBufferedGrayRoots() const;
    void startBufferingGrayRoots();
    void endBufferingGrayRoots();
    void resetBufferedGrayRoots();
    void markBufferedGrayRoots(JS::Zone *zone);

    static void GrayCallback(JSTracer *trc, void **thing, JSGCTraceKind kind);

    void setGCMode(JSGCMode mode) { stack.setGCMode(mode); }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

    MarkStack<uintptr_t> stack;

  private:
#ifdef DEBUG
    void checkZone(void *p);
#else
    void checkZone(void *p) {}
#endif

    void pushTaggedPtr(StackTag tag, void *ptr) {
        checkZone(ptr);
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        JS_ASSERT(!(addr & StackTagMask));
        if (!stack.push(addr | uintptr_t(tag)))
            delayMarkingChildren(ptr);
    }

    void pushValueArray(JSObject *obj, void *start, void *end) {
        checkZone(obj);

        JS_ASSERT(start <= end);
        uintptr_t tagged = reinterpret_cast<uintptr_t>(obj) | GCMarker::ValueArrayTag;
        uintptr_t startAddr = reinterpret_cast<uintptr_t>(start);
        uintptr_t endAddr = reinterpret_cast<uintptr_t>(end);

        /*
         * Push in the reverse order so obj will be on top. If we cannot push
         * the array, we trigger delay marking for the whole object.
         */
        if (!stack.push(endAddr, startAddr, tagged))
            delayMarkingChildren(obj);
    }

    bool isMarkStackEmpty() {
        return stack.isEmpty();
    }

    bool restoreValueArray(JSObject *obj, void **vpp, void **endp);
    void saveValueRanges();
    inline void processMarkStackTop(SliceBudget &budget);
    void processMarkStackOther(uintptr_t tag, uintptr_t addr);

    void appendGrayRoot(void *thing, JSGCTraceKind kind);

    /* The color is only applied to objects and functions. */
    uint32_t color;

    mozilla::DebugOnly<bool> started;

    /* Pointer to the top of the stack of arenas we are delaying marking on. */
    js::gc::ArenaHeader *unmarkedArenaStackTop;
    /* Count of arenas that are currently in the stack. */
    mozilla::DebugOnly<size_t> markLaterArenas;

    enum GrayBufferState
    {
        GRAY_BUFFER_UNUSED,
        GRAY_BUFFER_OK,
        GRAY_BUFFER_FAILED
    };

    GrayBufferState grayBufferState;
};

void
SetMarkStackLimit(JSRuntime *rt, size_t limit);

} /* namespace js */

/*
 * Macro to test if a traversal is the marking phase of the GC.
 */
#define IS_GC_MARKING_TRACER(trc) \
    ((trc)->callback == nullptr || (trc)->callback == GCMarker::GrayCallback)

#endif /* js_Tracer_h */
