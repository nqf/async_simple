/*
 * Copyright (c) 2022, Alibaba Group Holding Limited;
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef ASYNC_SIMPLE_FUTURE_H
#define ASYNC_SIMPLE_FUTURE_H

#include <async_simple/Executor.h>
#include <async_simple/FutureState.h>
#include <async_simple/LocalState.h>
#include <async_simple/Promise.h>
#include <async_simple/Traits.h>
#include <type_traits>

namespace async_simple {

// The well-known Future/Promise pairs mimic a producer/consuerm pair.
// The Future stands for the consumer-side.
//
// Future's implementation is thread-safe so that Future and Promise
// could be able to appear in different thread.
//
// To get the value of Future synchronously, user should use `get()`
// method. It would blocking the current thread by using condition variable.
//
// To get the value of Future asynchronously, user could use `thenValue(F)`
// or `thenTry(F)`. See the seperate comments for details.
//
// User shouldn't access Future after Future called `get()`, `thenValue(F)`,
// or `thenTry(F)`.
//
// User should get a Future by calling `Promise::getFuture()` instead of
// constructing a Future directly. If the user want a ready future indeed,
// he should call makeReadyFuture().
template <typename T>
class Future {
public:
    using value_type = T;
    Future(FutureState<T>* fs) : _sharedState(fs) {
        if (_sharedState) {
            _sharedState->attachOne();
        }
    }
    Future(Try<T>&& t) : _sharedState(nullptr), _localState(std::move(t)) {}

    ~Future() {
        if (_sharedState) {
            _sharedState->detachOne();
        }
    }

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    Future(Future&& other)
        : _sharedState(other._sharedState),
          _localState(std::move(other._localState)) {
        other._sharedState = nullptr;
    }

    Future& operator=(Future&& other) {
        if (this != &other) {
            std::swap(_sharedState, other._sharedState);
            _localState = std::move(other._localState);
        }
        return *this;
    }

public:
    bool valid() const {
        return _sharedState != nullptr || _localState.hasResult();
    }

    bool hasResult() const {
        return _localState.hasResult() || _sharedState->hasResult();
    }

    T&& value() && { return std::move(result().value()); }
    T& value() & { return result().value(); }
    const T& value() const& { return result().value(); }

    Try<T>&& result() && { return std::move(getTry(*this)); }
    Try<T>& result() & { return getTry(*this); }
    const Try<T>& result() const& { return getTry(*this); }

    // get is only allowed on rvalue, aka, Future is not valid after get
    // invoked.
    //
    // Get value blocked thread when the future doesn't have a value.
    // If future in uthread context, use await(future) to get value without
    // thread blocked.
    T get() && {
        wait();
        return (std::move(*this)).value();
    }
    // Implementation for get() to wait synchronously.
    void wait() {
        logicAssert(valid(), "Future is broken");

        if (hasResult()) {
            return;
        }

        // wait in the same executor may cause deadlock
        assert(!currentThreadInExecutor());

        // The state is a shared state
        Promise<T> promise;
        auto future = promise.getFuture();

        _sharedState->setExecutor(
            nullptr);  // following continuation is simple, execute inplace
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> done{false};
        _sharedState->setContinuation(
            [&mtx, &cv, &done, p = std::move(promise)](Try<T>&& t) mutable {
                std::unique_lock<std::mutex> lock(mtx);
                p.setValue(std::move(t));
                done.store(true, std::memory_order_relaxed);
                cv.notify_one();
            });
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock,
                [&done]() { return done.load(std::memory_order_relaxed); });
        *this = std::move(future);
        assert(_sharedState->hasResult());
    }

    // Set the executor for the future. This only works for rvalue.
    // So the original future shouldn't be accessed after setting
    // an executor. The user should use the returned future instead.
    Future<T> via(Executor* executor) && {
        setExecutor(executor);
        Future<T> ret(std::move(*this));
        return ret;
    }

    // thenTry() is only allowed on rvalues, do not access a future after
    // thenTry() called. F is a callback function which takes Try<T>&& as
    // parameter.
    //
    template <typename F, typename R = TryCallableResult<T, F>>
    Future<typename R::ReturnsFuture::Inner> thenTry(F&& f) && {
        return thenImpl<F, R>(std::forward<F>(f));
    }

    // Similar to thenTry, but F takes a T&&. If exception throws, F will not be
    // called.
    template <typename F, typename R = ValueCallableResult<T, F>>
    Future<typename R::ReturnsFuture::Inner> thenValue(F&& f) && {
        auto lambda = [func = std::forward<F>(f)](Try<T>&& t) mutable {
            return std::forward<F>(func)(std::move(t).value());
        };
        using Func = decltype(lambda);
        return thenImpl<Func, TryCallableResult<T, Func>>(std::move(lambda));
    }

public:
    // This section is public because they may invoked by other type of Future.
    // They are not suppose to be public.
    // FIXME: mark the section as private.
    void setExecutor(Executor* ex) {
        if (_sharedState) {
            _sharedState->setExecutor(ex);
        } else {
            _localState.setExecutor(ex);
        }
    }

    Executor* getExecutor() {
        if (_sharedState) {
            return _sharedState->getExecutor();
        } else {
            return _localState.getExecutor();
        }
    }

    template <typename F>
    void setContinuation(F&& func) {
        assert(valid());
        if (_sharedState) {
            _sharedState->setContinuation(std::forward<F>(func));
        } else {
            _localState.setContinuation(std::forward<F>(func));
        }
    }

    bool currentThreadInExecutor() const {
        assert(valid());
        if (_sharedState) {
            return _sharedState->currentThreadInExecutor();
        } else {
            return _localState.currentThreadInExecutor();
        }
    }

    bool TEST_hasLocalState() const { return _localState.hasResult(); }

private:
    template <typename Clazz>
    static decltype(auto) getTry(Clazz& self) {
        logicAssert(self.valid(), "Future is broken");
        logicAssert(
            self._localState.hasResult() || self._sharedState->hasResult(),
            "Future is not ready");
        if (self._sharedState) {
            return self._sharedState->getTry();
        } else {
            return self._localState.getTry();
        }
    }

    // continaution returns a future
    template <typename F, typename R>
    std::enable_if_t<R::ReturnsFuture::value,
                     Future<typename R::ReturnsFuture::Inner>>
    thenImpl(F&& func) {
        logicAssert(valid(), "Future is broken");
        using T2 = typename R::ReturnsFuture::Inner;

        if (!_sharedState) {
            try {
                auto newFuture =
                    std::forward<F>(func)(std::move(_localState.getTry()));
                if (!newFuture.getExecutor()) {
                    newFuture.setExecutor(_localState.getExecutor());
                }
                return newFuture;
            } catch (...) {
                return Future<T2>(Try<T2>(std::current_exception()));
            }
        }

        Promise<T2> promise;
        auto newFuture = promise.getFuture();
        newFuture.setExecutor(_sharedState->getExecutor());
        _sharedState->setContinuation(
            [p = std::move(promise),
             f = std::forward<F>(func)](Try<T>&& t) mutable {
                if (!R::isTry && t.hasError()) {
                    p.setException(t.getException());
                } else {
                    try {
                        auto f2 = f(std::move(t));
                        f2.setContinuation(
                            [pm = std::move(p)](Try<T2>&& t2) mutable {
                                pm.setValue(std::move(t2));
                            });
                    } catch (...) {
                        p.setException(std::current_exception());
                    }
                }
            });
        return newFuture;
    }

    // continaution returns a value
    template <typename F, typename R>
    std::enable_if_t<!(R::ReturnsFuture::value),
                     Future<typename R::ReturnsFuture::Inner>>
    thenImpl(F&& func) {
        logicAssert(valid(), "Future is broken");
        using T2 = typename R::ReturnsFuture::Inner;
        if (!_sharedState) {
            Future<T2> newFuture(makeTryCall(std::forward<F>(func),
                                             std::move(_localState.getTry())));
            newFuture.setExecutor(_localState.getExecutor());
            return newFuture;
        }
        Promise<T2> promise;
        auto newFuture = promise.getFuture();
        newFuture.setExecutor(_sharedState->getExecutor());
        _sharedState->setContinuation(
            [p = std::move(promise),
             f = std::forward<F>(func)](Try<T>&& t) mutable {
                if (!R::isTry && t.hasError()) {
                    p.setException(t.getException());
                } else {
                    p.setValue(makeTryCall(std::forward<F>(f),
                                           std::move(t)));  // Try<Unit>
                }
            });
        return newFuture;
    }

private:
    FutureState<T>* _sharedState;

    // Ready-Future does not have a Promise, an inline state is faster.
    LocalState<T> _localState;

private:
    template <typename Iter>
    friend Future<std::vector<
        Try<typename std::iterator_traits<Iter>::value_type::value_type>>>
    collectAll(Iter begin, Iter end);
};

// Make a ready Future
template <typename T>
Future<T> makeReadyFuture(T&& v) {
    return Future<T>(Try<T>(std::forward<T>(v)));
}
template <typename T>
Future<T> makeReadyFuture(Try<T>&& t) {
    return Future<T>(std::move(t));
}
template <typename T>
Future<T> makeReadyFuture(std::exception_ptr ex) {
    return Future<T>(Try<T>(ex));
}

}  // namespace async_simple

#endif  // ASYNC_SIMPLE_FUTURE_H
