#pragma once

#include "poller.h"

#include <util/system/mutex.h>
#include <util/system/error.h>
#include <util/system/context.h>
#include <util/system/defaults.h>
#include <util/system/valgrind.h>
#include <util/network/iovec.h>
#include <util/memory/tempbuf.h>
#include <util/memory/smallobj.h>
#include <util/memory/addstorage.h>
#include <util/network/socket.h>
#include <util/network/nonblock.h>
#include <util/generic/ptr.h>
#include <util/generic/buffer.h>
#include <util/generic/vector.h>
#include <library/containers/intrusive_rb_tree/rb_tree.h>
#include <util/generic/utility.h>
#include <util/generic/intrlist.h>
#include <util/generic/yexception.h>
#include <util/datetime/base.h>
#include <util/stream/format.h>

class TCont;
struct TContRep;
class TContEvent;
class TContExecutor;
class TContPollEvent;

typedef void (*TContFunc)(TCont*, void*);

#include "iostatus.h"

#if defined(_win_)
#define IOV_MAX 16
#endif

#if defined(_bionic_)
#define IOV_MAX 1024
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4265) // class has virtual functions, but destructor is not virtual
#endif

#if defined(DEBUG_CONT)
#define PCORO(p) Hex(size_t(p)) << " (" << p->Name() << ")"
#define DBGOUT(x) Cdbg << x << Endl
#else
#define DBGOUT(x)
#endif

struct TContPollEventCompare {
    template <class T>
    static inline bool Compare(const T& l, const T& r) noexcept {
        return l.DeadLine() < r.DeadLine() || (l.DeadLine() == r.DeadLine() && &l < &r);
    }
};

class TContPollEvent: public TRbTreeItem<TContPollEvent, TContPollEventCompare> {
public:
    inline TContPollEvent(TCont* cont, TInstant deadLine) noexcept
        : Cont_(cont)
        , DeadLine_(deadLine)
        , Status_(EINPROGRESS)
    {
    }

    inline int Status() const noexcept {
        return Status_;
    }

    inline void SetStatus(int status) noexcept {
        Status_ = status;
    }

    inline TCont* Cont() noexcept {
        return Cont_;
    }

    inline TInstant DeadLine() const noexcept {
        return DeadLine_;
    }

    inline void Wake(int status) noexcept {
        SetStatus(status);
        Wake();
    }

private:
    inline void Wake() noexcept;

private:
    TCont* Cont_;
    TInstant DeadLine_;
    int Status_;
};

template <class T>
inline int ExecuteEvent(T* event) noexcept;

class IPollEvent: public TIntrusiveListItem<IPollEvent> {
public:
    inline IPollEvent(SOCKET fd, ui16 what)
        : Fd_(fd)
        , What_(what)
    {
    }

    inline SOCKET Fd() const noexcept {
        return Fd_;
    }

    inline int What() const noexcept {
        return What_;
    }

    virtual void OnPollEvent(int status) noexcept = 0;

private:
    SOCKET Fd_;
    ui16 What_;
};

class TFdEvent final: public TContPollEvent, public IPollEvent {
public:
    inline TFdEvent(TCont* cont, SOCKET fd, ui16 what, TInstant deadLine) noexcept
        : TContPollEvent(cont, deadLine)
        , IPollEvent(fd, what)
    {
    }

    inline ~TFdEvent() {
        RemoveFromIOWait();
    }

    inline void RemoveFromIOWait() noexcept;

    void OnPollEvent(int status) noexcept override {
        Wake(status);
    }
};

class TTimerEvent: public TContPollEvent {
public:
    inline TTimerEvent(TCont* cont, TInstant deadLine) noexcept
        : TContPollEvent(cont, deadLine)
    {
    }
};

class TContPollEventHolder {
public:
    TContPollEventHolder(void* memory, TCont* rep, SOCKET fds[], int what[], size_t nfds, TInstant deadline);
    ~TContPollEventHolder();

    void ScheduleIoWait(TContExecutor* executor);
    TFdEvent* TriggeredEvent();

private:
    TFdEvent* Events_;
    size_t Count_;
};

class TInterruptibleEvent {
public:
    TInterruptibleEvent(TCont* cont)
        : Cont_(cont)
    {
    }

    bool Interrupted() const noexcept {
        return Interrupted_;
    }

    void Interrupt();

    template <typename F>
    TContIOStatus Wait(F&& f);

private:
    TCont* Cont_ = nullptr;
    bool Interrupted_ = false;
};

class TCont {
    struct TJoinWait: public TIntrusiveListItem<TJoinWait> {
        inline TJoinWait(TCont* c) noexcept
            : C(c)
        {
        }

        inline void Wake() noexcept {
            C->ReSchedule();
        }

        TCont* C;
    };

    friend struct TContRep;
    friend class TContExecutor;
    friend class TContPollEvent;

public:
    inline TCont(TContExecutor* executor, TContRep* rep, TContFunc func, void* arg, const char* name)
        : Executor_(executor)
        , Rep_(rep)
        , Func_(func)
        , Arg_(arg)
        , Name_(name)
        , Cancelled_(false)
        , Scheduled_(false)
    {
    }

    inline ~TCont() {
        Executor_ = nullptr;
        Rep_ = nullptr;
    }

    inline void SetExecutor(TContExecutor* e) noexcept {
        Executor_ = e;
    }

    inline void SwitchTo(TCont* next) noexcept {
        DBGOUT(PCORO(this) << " switch to " << PCORO(next));

        Context()->SwitchTo(next->Context());
    }

    inline TExceptionSafeContext* Context() noexcept {
#if defined(STACK_GROW_DOWN)
        return (TExceptionSafeContext*)(((char*)(this)) + Align(sizeof(TCont)));
#else
#error todo
#endif
    }

    inline const TExceptionSafeContext* Context() const noexcept {
        return const_cast<TCont*>(this)->Context();
    }

    inline TContExecutor* Executor() noexcept {
        return Executor_;
    }

    inline const TContExecutor* Executor() const noexcept {
        return Executor_;
    }

    inline TContRep* Rep() noexcept {
        return Rep_;
    }

    inline const TContRep* Rep() const noexcept {
        return Rep_;
    }

    inline const char* Name() const noexcept {
        return Name_;
    }

    void PrintMe(IOutputStream& out) const noexcept;

    inline void Yield() noexcept {
        if (SleepD(TInstant::Zero())) {
            ReScheduleAndSwitch();
        }
    }

    inline void ReScheduleAndSwitch() noexcept;

    int SelectD(SOCKET fds[], int what[], size_t nfds, SOCKET* outfd, TInstant deadline);

    inline int SelectT(SOCKET fds[], int what[], size_t nfds, SOCKET* outfd, TDuration timeout) {
        return SelectD(fds, what, nfds, outfd, timeout.ToDeadLine());
    }

    inline int SelectT(SOCKET fds[], int what[], size_t nfds, SOCKET* outfd) {
        return SelectD(fds, what, nfds, outfd, TInstant::Max());
    }

    inline int PollD(SOCKET fd, int what, TInstant deadline) {
        DBGOUT(PCORO(this) << " prepare poll");

        TFdEvent event(this, fd, (ui16)what, deadline);

        return ExecuteEvent(&event);
    }

    inline int PollT(SOCKET fd, int what, TDuration timeout) {
        return PollD(fd, what, timeout.ToDeadLine());
    }

    inline int PollI(SOCKET fd, int what) {
        return PollD(fd, what, TInstant::Max());
    }

    /// @return ETIMEDOUT on success
    inline int SleepD(TInstant deadline) {
        DBGOUT(PCORO(this) << " do sleep");

        TTimerEvent event(this, deadline);

        return ExecuteEvent(&event);
    }

    inline int SleepT(TDuration timeout) {
        return SleepD(timeout.ToDeadLine());
    }

    inline int SleepI() {
        return SleepD(TInstant::Max());
    }

    TContIOStatus ReadD(SOCKET fd, void* buf, size_t len, TInstant deadline);

    inline TContIOStatus ReadT(SOCKET fd, void* buf, size_t len, TDuration timeout) {
        return ReadD(fd, buf, len, timeout.ToDeadLine());
    }

    inline TContIOStatus ReadI(SOCKET fd, void* buf, size_t len) {
        return ReadD(fd, buf, len, TInstant::Max());
    }

    TContIOStatus WriteVectorD(SOCKET fd, TContIOVector* vec, TInstant deadline);

    inline TContIOStatus WriteVectorT(SOCKET fd, TContIOVector* vec, TDuration timeOut) {
        return WriteVectorD(fd, vec, timeOut.ToDeadLine());
    }

    inline TContIOStatus WriteVectorI(SOCKET fd, TContIOVector* vec) {
        return WriteVectorD(fd, vec, TInstant::Max());
    }

    TContIOStatus WriteD(SOCKET fd, const void* buf, size_t len, TInstant deadline);

    inline TContIOStatus WriteT(SOCKET fd, const void* buf, size_t len, TDuration timeout) {
        return WriteD(fd, buf, len, timeout.ToDeadLine());
    }

    inline TContIOStatus WriteI(SOCKET fd, const void* buf, size_t len) {
        return WriteD(fd, buf, len, TInstant::Max());
    }

    inline void Exit();

    int Connect(TSocketHolder& s, const struct addrinfo& ai, TInstant deadLine);
    int Connect(TSocketHolder& s, const TNetworkAddress& addr, TInstant deadLine);

    inline int Connect(TSocketHolder& s, const TNetworkAddress& addr, TDuration timeOut) {
        return Connect(s, addr, timeOut.ToDeadLine());
    }

    inline int Connect(TSocketHolder& s, const TNetworkAddress& addr) {
        return Connect(s, addr, TInstant::Max());
    }

    int ConnectD(SOCKET s, const struct sockaddr* name, socklen_t namelen, TInstant deadline);

    inline int ConnectT(SOCKET s, const struct sockaddr* name, socklen_t namelen, TDuration timeout) {
        return ConnectD(s, name, namelen, timeout.ToDeadLine());
    }

    inline int ConnectI(SOCKET s, const struct sockaddr* name, socklen_t namelen) {
        return ConnectD(s, name, namelen, TInstant::Max());
    }

    int AcceptD(SOCKET s, struct sockaddr* addr, socklen_t* addrlen, TInstant deadline);

    inline int AcceptT(SOCKET s, struct sockaddr* addr, socklen_t* addrlen, TDuration timeout) {
        return AcceptD(s, addr, addrlen, timeout.ToDeadLine());
    }

    inline int AcceptI(SOCKET s, struct sockaddr* addr, socklen_t* addrlen) {
        return AcceptD(s, addr, addrlen, TInstant::Max());
    }

    static inline SOCKET Socket(int domain, int type, int protocol) {
        return Socket4(domain, type, protocol);
    }

    static inline SOCKET Socket(const struct addrinfo& ai) {
        return Socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol);
    }

    static inline bool IsBlocked() noexcept {
        return IsBlocked(LastSystemError());
    }

    static inline bool IsBlocked(int lasterr) noexcept {
        return lasterr == EAGAIN || lasterr == EWOULDBLOCK;
    }

    /*
     * useful for keep-alive connections
     */
    static inline bool SocketNotClosedByOtherSide(SOCKET s) noexcept {
        const int r = MsgPeek(s);

        return r > 0 || (r == -1 && IsBlocked());
    }

    static inline bool HavePendingData(SOCKET s) noexcept {
        return MsgPeek(s) > 0;
    }

    static inline int MsgPeek(SOCKET s) noexcept {
        char c;
        return recv(s, &c, 1, MSG_PEEK);
    }

    inline bool IAmRunning() const noexcept;

    inline void Cancel() noexcept;

    inline bool Cancelled() const noexcept {
        return Cancelled_;
    }

    inline bool Scheduled() const noexcept {
        return Scheduled_;
    }

    inline void WakeAllWaiters() noexcept {
        while (!Waiters_.Empty()) {
            Waiters_.PopFront()->Wake();
        }
    }

    inline bool Join(TCont* c) noexcept {
        return Join(c, TInstant::Max());
    }

    inline bool Join(TCont* c, TInstant deadLine) noexcept {
        DBGOUT(PCORO(this) << " join " << PCORO(c));
        TJoinWait ev(this);

        c->Waiters_.PushBack(&ev);

        if (SleepD(deadLine) == ETIMEDOUT || Cancelled()) {
            if (!ev.Empty()) {
                c->Cancel();

                do {
                    SwitchToScheduler();
                } while (!ev.Empty());
            }

            return false;
        }

        return true;
    }

    /*
     * please dont use this :)
     */
    inline void SwitchToScheduler() noexcept;
    inline void ReSchedule() noexcept;

private:
    inline void ReScheduleNow() noexcept;

    inline void Execute() {
        Y_ASSERT(Func_);

        Func_(this, Arg_);
    }

public:
    static ssize_t DoRead(SOCKET fd, char* buf, size_t len) noexcept;
    static ssize_t DoWrite(SOCKET fd, const char* buf, size_t len) noexcept;
    static ssize_t DoWriteVector(SOCKET fd, TContIOVector* vec) noexcept;

private:
    TContExecutor* Executor_;
    TContRep* Rep_;
    TContFunc Func_;
    void* Arg_;
    const char* Name_;
    TIntrusiveList<TJoinWait> Waiters_;
    bool Cancelled_;
    bool Scheduled_;
};

#include "stack.h"

struct TContRep: public TIntrusiveListItem<TContRep>, public ITrampoLine {
    TContRep(TContStackAllocator* alloc);

    void DoRun() override;

    void Construct(TContExecutor* executor, TContFunc func, void* arg, const char* name);
    void Destruct() noexcept;

    inline TCont* ContPtr() noexcept {
        return (TCont*)cont.Data();
    }

    inline const TCont* ContPtr() const noexcept {
        return (const TCont*)cont.Data();
    }

    inline TExceptionSafeContext* MachinePtr() noexcept {
        return (TExceptionSafeContext*)machine.Data();
    }

    static inline size_t OverHead() noexcept {
        return Align(sizeof(TCont)) + Align(sizeof(TExceptionSafeContext));
    }

    static inline size_t EffectiveStackLength(size_t alloced) noexcept {
        return alloced - OverHead();
    }

    static inline size_t ToAllocate(size_t stackLen) noexcept {
        return Align(stackLen) + OverHead();
    }

    inline bool IAmRuning() const noexcept {
        return ContPtr()->IAmRunning();
    }

    TContStackAllocator::TStackPtr real;

    TMemRegion full;
#if defined(STACK_GROW_DOWN)
    TMemRegion stack;
    TMemRegion cont;
    TMemRegion machine;
#else
#error todo
#endif
};

struct TPollEventList: public TIntrusiveList<IPollEvent> {
    inline ui16 Flags() const noexcept {
        ui16 ret = 0;

        for (TConstIterator it = Begin(); it != End(); ++it) {
            ret |= it->What();
        }

        return ret;
    }
};

class TEventWaitQueue {
    struct TCancel {
        inline void operator()(TContPollEvent* e) noexcept {
            e->Cont()->Cancel();
        }

        inline void operator()(TContPollEvent& e) noexcept {
            operator()(&e);
        }
    };

    typedef TRbTree<TContPollEvent, TContPollEventCompare> TIoWait;

public:
    inline void Register(TContPollEvent* event) {
        IoWait_.Insert(event);
        event->Cont()->Rep()->Unlink();
    }

    inline bool Empty() const noexcept {
        return IoWait_.Empty();
    }

    inline void Abort() noexcept {
        TCancel visitor;

        IoWait_.ForEach(visitor);
    }

    inline TInstant CancelTimedOut(TInstant now) noexcept {
        TIoWait::TIterator it = IoWait_.Begin();

        if (it != IoWait_.End()) {
            if (it->DeadLine() > now) {
                return it->DeadLine();
            }

            do {
                (it++)->Wake(ETIMEDOUT);
            } while (it != IoWait_.End() && it->DeadLine() <= now);
        }

        return now;
    }

private:
    TIoWait IoWait_;
};

#include "sockmap.h"

template <class T>
class TBigArray {
    struct TValue: public T, public TObjectFromPool<TValue> {
        inline TValue() {
        }
    };

public:
    inline TBigArray()
        : Pool_(TMemoryPool::TExpGrow::Instance(), TDefaultAllocator::Instance())
    {
    }

    inline T* Get(size_t index) {
        TRef& ret = Lst_.Get(index);

        if (!ret) {
            ret = new (&Pool_) TValue();
        }

        return ret.Get();
    }

private:
    typedef TAutoPtr<TValue> TRef;
    typename TValue::TPool Pool_;
    TSocketMap<TRef> Lst_;
};

class TContPoller {
public:
    typedef IPollerFace::TEvent TEvent;
    typedef IPollerFace::TEvents TEvents;

    inline TContPoller()
        : P_(IPollerFace::Default())
    {
    }

    inline explicit TContPoller(THolder<IPollerFace> poller)
        : P_(std::move(poller))
    {}

    inline void Schedule(IPollEvent* event) {
        TPollEventList* lst = List(event->Fd());
        const ui16 oldFlags = lst->Flags();
        lst->PushFront(event);
        const ui16 newFlags = lst->Flags();

        if (newFlags != oldFlags) {
            P_->Set(lst, event->Fd(), newFlags);
        }
    }

    inline void Remove(IPollEvent* event) noexcept {
        TPollEventList* lst = List(event->Fd());
        const ui16 oldFlags = lst->Flags();
        event->Unlink();
        const ui16 newFlags = lst->Flags();

        if (newFlags != oldFlags) {
            P_->Set(lst, event->Fd(), newFlags);
        }
    }

    inline size_t Wait(TEvents& events, TInstant deadLine) {
        events.clear();

        P_->Wait(events, deadLine);

        return events.size();
    }

private:
    inline TPollEventList* List(size_t fd) {
        return Lists_.Get(fd);
    }

private:
    TBigArray<TPollEventList> Lists_;
    THolder<IPollerFace> P_;
};

class TContRepPool {
    typedef TIntrusiveListWithAutoDelete<TContRep, TDelete> TFreeReps;

public:
    inline TContRepPool(TContStackAllocator* alloc)
        : Alloc_(alloc)
    {
    }

    inline TContRepPool(size_t stackLen)
        : MyAlloc_(new TDefaultStackAllocator(TContRep::ToAllocate(AlignUp<size_t>(stackLen, 2 * STACK_ALIGN))))
        , Alloc_(MyAlloc_.Get())
    {
    }

    ~TContRepPool() {
        unsigned long long all = Allocated_;
        Y_VERIFY(Allocated_ == 0, "leaked coroutines: %llu", all);
    }

    inline TContRep* Allocate() {
        Allocated_ += 1;

        if (Free_.Empty()) {
            return new TContRep(Alloc_);
        }

        return Free_.PopFront();
    }

    inline void Release(TContRep* cont) noexcept {
        Allocated_ -= 1;
        Free_.PushFront(cont);
    }

    size_t Allocated() const Y_WARN_UNUSED_RESULT {
        return Allocated_;
    }

private:
    THolder<TContStackAllocator> MyAlloc_;
    TContStackAllocator* Alloc_;
    TFreeReps Free_;
    ui64 Allocated_ = 0;
};

template <class Functor>
static void ContHelperFunc(TCont* cont, void* arg) {
    (*((Functor*)(arg)))(cont);
}

template <typename T, void (T::*M)(TCont*)>
static void ContHelperMemberFunc(TCont* c, void* arg) {
    ((ReinterpretCast<T*>(arg))->*M)(c);
}

/// Central coroutine class.
/// Note, coroutines are single-threaded, and all methods must be called from the single thread
class TContExecutor {
    friend class TCont;
    friend struct TContRep;
    friend class TContEvent;
    friend class TContPollEvent;
    friend class TContPollEventHolder;
    typedef TIntrusiveList<TContRep> TContList;

    struct TCancel {
        inline void operator()(TContRep* c) noexcept {
            c->ContPtr()->Cancel();
        }
    };

    struct TNoOp {
        template <class T>
        inline void operator()(T*) noexcept {
        }
    };

    struct TReleaseAll {
        inline void operator()(TContRep* c) noexcept {
            c->ContPtr()->Executor()->Release(c);
        }
    };

public:
    TContExecutor(size_t stackSize, THolder<IPollerFace> poller = IPollerFace::Default());
    TContExecutor(TContRepPool* pool, THolder<IPollerFace> poller = IPollerFace::Default());

    ~TContExecutor();

    /*
     * assume we already create all necessary coroutines
     */
    inline void Execute() {
        TNoOp nop;

        Execute(nop);
    }

    inline void Execute(TContFunc func, void* arg = nullptr) {
        Create(func, arg, "sys_main");

        RunScheduler();
    }

    template <class Functor>
    inline void Execute(Functor& f) {
        Execute((TContFunc)ContHelperFunc<Functor>, (void*)&f);
    }

    template <typename T, void (T::*M)(TCont*)>
    inline void Execute(T* obj) {
        Execute(ContHelperMemberFunc<T, M>, obj);
    }

    inline TExceptionSafeContext* SchedCont() noexcept {
        return &SchedContext_;
    }

    template <class Functor>
    inline TContRep* Create(Functor& f, const char* name) {
        return Create((TContFunc)ContHelperFunc<Functor>, (void*)&f, name);
    }

    template <typename T, void (T::*M)(TCont*)>
    inline TContRep* Create(T* obj, const char* name) {
        return Create(ContHelperMemberFunc<T, M>, obj, name);
    }

    inline TContRep* Create(TContFunc func, void* arg, const char* name) {
        TContRep* cont = CreateImpl(func, arg, name);

        ScheduleExecution(cont);

        return cont;
    }

    inline TContPoller* Poller() noexcept {
        return &Poller_;
    }

    inline TEventWaitQueue* WaitQueue() noexcept {
        return &WaitQueue_;
    }

    inline TContRep* Running() noexcept {
        return Current_;
    }

    inline const TContRep* Running() const noexcept {
        return Current_;
    }

    inline void Abort() noexcept {
        WaitQueue_.Abort();
        TCancel visitor;
        Ready_.ForEach(visitor);
        ReadyNext_.ForEach(visitor);
    }

    inline void SetFailOnError(bool fail) noexcept {
        FailOnError_ = fail;
    }

    inline bool FailOnError() const noexcept {
        return FailOnError_;
    }

    inline void ScheduleIoWait(TFdEvent* event) {
        DBGOUT(PCORO(event->Cont()) << " schedule iowait");
        WaitQueue_.Register(event);
        Poller_.Schedule(event);
    }

    inline void ScheduleIoWait(TTimerEvent* event) noexcept {
        DBGOUT(PCORO(event->Cont()) << " schedule timer");
        WaitQueue_.Register(event);
    }

private:
    inline TContRep* CreateImpl(TContFunc func, void* arg, const char* name) {
        TContRep* cont = Pool_.Allocate();

        cont->Construct(this, func, arg, name);
        cont->Unlink();

        return cont;
    }

    inline void Release(TContRep* cont) noexcept {
        DBGOUT(PCORO(cont->ContPtr()) << " release");

        cont->Unlink();
        cont->Destruct();

        Pool_.Release(cont);
    }

    inline void Exit(TContRep* cont) noexcept {
        DBGOUT(PCORO(cont->ContPtr()) << " exit");

        ScheduleToDelete(cont);
        cont->ContPtr()->SwitchToScheduler();

        Y_FAIL("can not return from exit");
    }

    void RunScheduler();

    inline void ScheduleToDelete(TContRep* cont) noexcept {
        DBGOUT(PCORO(cont->ContPtr()) << " schedule to delete");
        ToDelete_.PushBack(cont);
    }

    inline void ScheduleExecution(TContRep* cont) noexcept {
        DBGOUT(PCORO(cont->ContPtr()) << " schedule execution");
        cont->ContPtr()->Scheduled_ = true;
        ReadyNext_.PushBack(cont);
    }

    inline void ScheduleExecutionNow(TContRep* cont) noexcept {
        DBGOUT(PCORO(cont->ContPtr()) << " schedule execution now");
        cont->ContPtr()->Scheduled_ = true;
        Ready_.PushBack(cont);
    }

    inline void Activate(TContRep* cont) noexcept {
        DBGOUT("scheduler: activate " << PCORO(cont->ContPtr()));
        Current_ = cont;
        TCont* contPtr = cont->ContPtr();
        contPtr->Scheduled_ = false;
        DBGOUT("scheduler: switch to " << PCORO(cont->ContPtr()));
        SchedContext_.SwitchTo(contPtr->Context());
    }

    inline void DeleteScheduled() noexcept {
        TReleaseAll functor;

        ToDelete_.ForEach(functor);
    }

    void WaitForIO();

    void ProcessEvents(size_t evCnt);

private:
    TContList ToDelete_;
    TContList Ready_;
    TContList ReadyNext_;
    TEventWaitQueue WaitQueue_;
    TContPoller Poller_;
    THolder<TContRepPool> MyPool_;
    TContRepPool& Pool_;
    TExceptionSafeContext SchedContext_;
    TContRep* Current_;
    typedef TContPoller::TEvents TEvents;
    TEvents Events_;
    bool FailOnError_;
};

template <class T>
inline int ExecuteEvent(T* event) noexcept {
    TCont* c = event->Cont();

    if (c->Cancelled()) {
        return ECANCELED;
    }

    /*
     * schedule wait
     */
    c->Executor()->ScheduleIoWait(event);

    /*
     * go to scheduler
     */
    c->SwitchToScheduler();
    /*
     * wait complete
     */

    if (c->Cancelled()) {
        return ECANCELED;
    }

    return event->Status();
}

inline void TFdEvent::RemoveFromIOWait() noexcept {
    Cont()->Executor()->Poller()->Remove(this);
}

inline void TContPollEvent::Wake() noexcept {
    UnLink();
    Cont()->ReSchedule();
}

inline void TCont::Exit() {
    Executor()->Exit(Rep());
}

inline bool TCont::IAmRunning() const noexcept {
    return Rep() == Executor()->Running();
}

inline void TCont::Cancel() noexcept {
    if (Cancelled()) {
        return;
    }

    DBGOUT(PCORO(this) << " do cancel");

    Cancelled_ = true;

    if (!IAmRunning()) {
        DBGOUT(PCORO(this) << " do cancel from " << PCORO(Executor()->Running()->ContPtr()));

        // Some legacy code expects a Cancelled coroutine to be scheduled without delay.
        ReScheduleNow();
    }
}

inline void TCont::ReSchedule() noexcept {
    DBGOUT(PCORO(this) << " reschedule");

    Executor()->ScheduleExecution(Rep());
}

inline void TCont::ReScheduleNow() noexcept {
    DBGOUT(PCORO(this) << " reschedule now");

    Executor()->ScheduleExecutionNow(Rep());
}

inline void TCont::SwitchToScheduler() noexcept {
    DBGOUT(PCORO(this) << " switch to scheduler");

    Context()->SwitchTo(Executor()->SchedCont());
}

inline void TCont::ReScheduleAndSwitch() noexcept {
    ReSchedule();
    SwitchToScheduler();
}

#define EWAKEDUP 34567

#include "events.h"
#include "mutex.h"
#include "condvar.h"
#include "sockpool.h"

#if !defined(FROM_IMPL_CPP)
#undef DBGOUT
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

template <typename F>
TContIOStatus TInterruptibleEvent::Wait(F&& f) {
    if (Interrupted_) {
        return TContIOStatus(0, EWAKEDUP);
    }

    auto ret = f(Cont_);
    if (ret.Status() == EINPROGRESS && Interrupted_) {
        return TContIOStatus(0, EWAKEDUP);
    }

    return ret;
}
