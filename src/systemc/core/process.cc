/*
 * Copyright 2018 Google, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Gabe Black
 */

#include "systemc/core/process.hh"

#include "base/logging.hh"
#include "systemc/core/event.hh"
#include "systemc/core/scheduler.hh"

namespace sc_gem5
{

SensitivityTimeout::SensitivityTimeout(Process *p, ::sc_core::sc_time t) :
    Sensitivity(p), timeoutEvent(this), time(t)
{
    Tick when = scheduler.getCurTick() + time.value();
    scheduler.schedule(&timeoutEvent, when);
}

SensitivityTimeout::~SensitivityTimeout()
{
    if (timeoutEvent.scheduled())
        scheduler.deschedule(&timeoutEvent);
}

void
SensitivityTimeout::timeout()
{
    scheduler.eventHappened();
    notify();
}

SensitivityEvent::SensitivityEvent(
        Process *p, const ::sc_core::sc_event *e) : Sensitivity(p), event(e)
{
    Event::getFromScEvent(event)->addSensitivity(this);
}

SensitivityEvent::~SensitivityEvent()
{
    Event::getFromScEvent(event)->delSensitivity(this);
}

SensitivityEventAndList::SensitivityEventAndList(
        Process *p, const ::sc_core::sc_event_and_list *list) :
    Sensitivity(p), list(list), count(0)
{
    for (auto e: list->events)
        Event::getFromScEvent(e)->addSensitivity(this);
}

SensitivityEventAndList::~SensitivityEventAndList()
{
    for (auto e: list->events)
        Event::getFromScEvent(e)->delSensitivity(this);
}

void
SensitivityEventAndList::notifyWork(Event *e)
{
    e->delSensitivity(this);
    count++;
    if (count == list->events.size())
        process->satisfySensitivity(this);
}

SensitivityEventOrList::SensitivityEventOrList(
        Process *p, const ::sc_core::sc_event_or_list *list) :
    Sensitivity(p), list(list)
{
    for (auto e: list->events)
        Event::getFromScEvent(e)->addSensitivity(this);
}

SensitivityEventOrList::~SensitivityEventOrList()
{
    for (auto e: list->events)
        Event::getFromScEvent(e)->delSensitivity(this);
}


class UnwindExceptionReset : public ::sc_core::sc_unwind_exception
{
  public:
    const char *what() const throw() override { return "RESET"; }
    bool is_reset() const override { return true; }
};

class UnwindExceptionKill : public ::sc_core::sc_unwind_exception
{
  public:
    const char *what() const throw() override { return "KILL"; }
    bool is_reset() const override { return false; }
};

template <typename T>
struct BuiltinExceptionWrapper : public ExceptionWrapperBase
{
  public:
    T t;
    void throw_it() override { throw t; }
};

BuiltinExceptionWrapper<UnwindExceptionReset> resetException;
BuiltinExceptionWrapper<UnwindExceptionKill> killException;


void
Process::forEachKid(const std::function<void(Process *)> &work)
{
    for (auto &kid: get_child_objects()) {
        Process *p_kid = dynamic_cast<Process *>(kid);
        if (p_kid)
            work(p_kid);
    }
}

void
Process::suspend(bool inc_kids)
{
    if (inc_kids)
        forEachKid([](Process *p) { p->suspend(true); });

    if (!_suspended) {
        _suspended = true;
        _suspendedReady = false;
    }

    if (procKind() != ::sc_core::SC_METHOD_PROC_ &&
            scheduler.current() == this) {
        scheduler.yield();
    }
}

void
Process::resume(bool inc_kids)
{
    if (inc_kids)
        forEachKid([](Process *p) { p->resume(true); });

    if (_suspended) {
        _suspended = false;
        if (_suspendedReady)
            ready();
        _suspendedReady = false;
    }
}

void
Process::disable(bool inc_kids)
{
    if (inc_kids)
        forEachKid([](Process *p) { p->disable(true); });

    _disabled = true;
}

void
Process::enable(bool inc_kids)
{

    if (inc_kids)
        forEachKid([](Process *p) { p->enable(true); });

    _disabled = false;
}

void
Process::kill(bool inc_kids)
{
    // Update our state.
    _terminated = true;
    _isUnwinding = true;

    // Propogate the kill to our children no matter what happens to us.
    if (inc_kids)
        forEachKid([](Process *p) { p->kill(true); });

    // If we're in the middle of unwinding, ignore the kill request.
    if (_isUnwinding)
        return;

    // Inject the kill exception into this process.
    injectException(killException);

    _terminatedEvent.notify();
}

void
Process::reset(bool inc_kids)
{
    // Update our state.
    _isUnwinding = true;

    // Propogate the reset to our children no matter what happens to us.
    if (inc_kids)
        forEachKid([](Process *p) { p->reset(true); });

    // If we're in the middle of unwinding, ignore the reset request.
    if (_isUnwinding)
        return;

    // Inject the reset exception into this process.
    injectException(resetException);

    _resetEvent.notify();
}

void
Process::throw_it(ExceptionWrapperBase &exc, bool inc_kids)
{
    if (inc_kids)
        forEachKid([&exc](Process *p) { p->throw_it(exc, true); });
}

void
Process::injectException(ExceptionWrapperBase &exc)
{
    excWrapper = &exc;
    // Let this process preempt us.
};

void
Process::syncResetOn(bool inc_kids)
{
    if (inc_kids)
        forEachKid([](Process *p) { p->syncResetOn(true); });

    _syncReset = true;
}

void
Process::syncResetOff(bool inc_kids)
{
    if (inc_kids)
        forEachKid([](Process *p) { p->syncResetOff(true); });

    _syncReset = false;
}

void
Process::dontInitialize()
{
    scheduler.dontInitialize(this);
}

void
Process::finalize()
{
    for (auto &s: pendingStaticSensitivities) {
        s->finalize(staticSensitivities);
        delete s;
        s = nullptr;
    }
    pendingStaticSensitivities.clear();
};

void
Process::run()
{
    bool reset;
    do {
        reset = false;
        try {
            func->call();
        } catch(::sc_core::sc_unwind_exception exc) {
            reset = exc.is_reset();
        }
    } while (reset);
    _terminated = true;
}

void
Process::addStatic(PendingSensitivity *s)
{
    pendingStaticSensitivities.push_back(s);
}

void
Process::setDynamic(Sensitivity *s)
{
    delete dynamicSensitivity;
    dynamicSensitivity = s;
}

void
Process::satisfySensitivity(Sensitivity *s)
{
    // If there's a dynamic sensitivity and this wasn't it, ignore.
    if (dynamicSensitivity && dynamicSensitivity != s)
        return;

    setDynamic(nullptr);
    ready();
}

void
Process::ready()
{
    if (suspended())
        _suspendedReady = true;
    else
        scheduler.ready(this);
}

Process::Process(const char *name, ProcessFuncWrapper *func,
        bool _dynamic, bool needs_start) :
    ::sc_core::sc_object(name), excWrapper(nullptr), func(func),
    _needsStart(needs_start), _dynamic(_dynamic), _isUnwinding(false),
    _terminated(false), _suspended(false), _disabled(false),
    _syncReset(false), refCount(0), stackSize(::Fiber::DefaultStackSize),
    dynamicSensitivity(nullptr)
{
    _newest = this;
    if (_dynamic)
        finalize();
    else
        scheduler.reg(this);
}

Process *Process::_newest;

void
throw_it_wrapper(Process *p, ExceptionWrapperBase &exc, bool inc_kids)
{
    p->throw_it(exc, inc_kids);
}

} // namespace sc_gem5