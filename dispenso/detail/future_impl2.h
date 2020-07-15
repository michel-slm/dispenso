// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE.md file in the root directory of this source tree.

namespace dispenso {
namespace detail {
template <typename Result>
template <typename RetResult, typename F, typename Schedulable>
FutureImplBase<RetResult>* FutureBase<Result>::thenImpl(
    F&& f,
    Schedulable& sched,
    std::launch asyncPolicy,
    std::launch deferredPolicy) {
  Future<Result> copy(*this);
  auto func = [f = std::move(f), copy = std::move(copy)]() mutable -> RetResult {
    copy.wait();
    return f(std::move(copy));
  };

  auto* retImpl = createFutureImpl<RetResult>(
      std::move(func), (deferredPolicy & std::launch::deferred) == std::launch::deferred);
  impl_->addToThenChainOrExecute(retImpl, sched, asyncPolicy);
  return retImpl;
}

template <size_t index, typename... Ts>
struct ForEachApply {
  template <typename F>
  void operator()(std::tuple<Ts...>& t, F f) {
    if (f(std::get<index>(t))) {
      ForEachApply<index - 1, Ts...>{}(t, f);
    }
  }
};

template <typename... Ts>
struct ForEachApply<size_t{0}, Ts...> {
  template <typename F>
  void operator()(std::tuple<Ts...>& t, F f) {
    f(std::get<0>(t));
  }
};

template <typename F, typename... Ts>
void forEach(std::tuple<Ts...>& t, F f) {
  constexpr size_t size = std::tuple_size<std::tuple<Ts...>>::value;
  ForEachApply<size - 1, Ts...>{}(t, f);
}

template <typename VecType>
struct WhenAllSharedVec {
  VecType vec;
  std::atomic<size_t> count;
  OnceFunction f;

  template <typename InputIt, typename T = typename std::iterator_traits<InputIt>::value_type>
  WhenAllSharedVec(InputIt first, InputIt last) : vec(first, last), count(vec.size()) {}
};

template <typename Tuple>
struct WhenAllSharedTuple {
  Tuple tuple;
  std::atomic<size_t> count;
  OnceFunction f;
  template <typename... Types>
  WhenAllSharedTuple(Types&&... args)
      : tuple(std::make_tuple(std::forward<Types>(args)...)),
        count(std::tuple_size<Tuple>::value) {}
};

struct InterceptionInvoker {
  void schedule(OnceFunction f) {
    savedOffFn = std::move(f);
  }
  void schedule(OnceFunction f, ForceQueuingTag) {
    savedOffFn = std::move(f);
  }
  OnceFunction savedOffFn;
};

} // namespace detail

template <typename InputIt>
Future<std::vector<typename std::iterator_traits<InputIt>::value_type>> when_all(
    InputIt first,
    InputIt last) {
  using VecType = std::vector<typename std::iterator_traits<InputIt>::value_type>;
  using ResultFuture = Future<VecType>;

  if (first == last) {
    return make_ready_future(VecType());
  }

  // TODO(bbudge): Can write something faster than make_shared using SmallBufferAllocator.
  auto shared = std::make_shared<detail::WhenAllSharedVec<VecType>>(first, last);

  auto whenComplete = [shared]() -> VecType {
    for (auto& f : shared->vec) {
      if (0 == shared->count.load(std::memory_order_relaxed)) {
        break;
      }
      f.wait();
    }
    return std::move(shared->vec);
  };

  detail::InterceptionInvoker interceptor;
  ResultFuture res(std::move(whenComplete), interceptor);

  shared->f = std::move(interceptor.savedOffFn);
  for (auto& s : shared->vec) {
    s.then(
        [shared](auto&&) {
          if (shared->count.fetch_sub(1, std::memory_order_relaxed) == 1) {
            shared->f();
          }
        },
        kImmediateInvoker);
  }

  return res;
}

inline auto when_all() -> Future<std::tuple<>> {
  return make_ready_future(std::tuple<>());
}

template <class... Futures>
auto when_all(Futures&&... futures) -> Future<std::tuple<std::decay_t<Futures>...>> {
  using TupleType = std::tuple<std::decay_t<Futures>...>;
  using ResultFuture = Future<TupleType>;

  // TODO(bbudge): Can write something faster than make_shared using SmallBufferAllocator.
  auto shared =
      std::make_shared<detail::WhenAllSharedTuple<TupleType>>(std::forward<Futures>(futures)...);

  auto whenComplete = [shared]() -> TupleType {
    forEach(shared->tuple, [&shared](auto& future) {
      if (0 == shared->count.load(std::memory_order_relaxed)) {
        return false;
      }
      future.wait();
      return true;
    });
    return std::move(shared->tuple);
  };

  detail::InterceptionInvoker interceptor;
  ResultFuture res(std::move(whenComplete), interceptor);

  shared->f = std::move(interceptor.savedOffFn);
  forEach(shared->tuple, [shared = std::move(shared)](auto& future) {
    future.then(
        [shared](auto&&) {
          if (shared->count.fetch_sub(1, std::memory_order_relaxed) == 1) {
            shared->f();
          }
        },
        kImmediateInvoker);
    return true;
  });

  return res;
}

} // namespace dispenso
