//
// Copyright (c) 2018 Manuel Pöter.
// Licensed under the MIT License. See LICENSE file in the project root for full license information.
//

#ifndef XENIUM_HAZARD_ERAS_HPP
#define XENIUM_HAZARD_ERAS_HPP

#include <xenium/reclamation/detail/concurrent_ptr.hpp>
#include <xenium/reclamation/detail/guard_ptr.hpp>
#include <xenium/reclamation/detail/deletable_object.hpp>
#include <xenium/reclamation/detail/thread_block_list.hpp>
#include <xenium/reclamation/detail/allocation_tracker.hpp>

#include <xenium/acquire_guard.hpp>
#include <xenium/parameter.hpp>
#include <xenium/policy.hpp>

#include <memory>
#include <stdexcept>

namespace xenium { namespace reclamation {

  class bad_hazard_era_alloc : public std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  namespace detail {
    struct deletable_object_with_eras;

    template <class Strategy, class Derived>
    struct basic_he_thread_control_block;

    template <size_t K_, size_t A, size_t B, template <class> class ThreadControlBlock>
    struct generic_hazard_era_allocation_strategy
    {
      static constexpr size_t K = K_;

      static size_t retired_nodes_threshold() {
        return A * number_of_active_hazard_eras() + B;
      }

      static size_t number_of_active_hazard_eras() {
        return number_of_active_hes.load(std::memory_order_relaxed);
      }

      using thread_control_block = ThreadControlBlock<generic_hazard_era_allocation_strategy>;
    private:
      friend thread_control_block;
      friend basic_he_thread_control_block<generic_hazard_era_allocation_strategy, thread_control_block>;

      static std::atomic<size_t> number_of_active_hes;
    };

    template <class Strategy>
    struct static_he_thread_control_block;

    template <class Strategy>
    struct dynamic_he_thread_control_block;
  }

  namespace he_allocation {
    template <size_t K = 2, size_t A = 2, size_t B = 100>
    struct static_strategy :
      detail::generic_hazard_era_allocation_strategy<K, A, B, detail::static_he_thread_control_block> {};

    template <size_t K = 2, size_t A = 2, size_t B = 100>
    struct dynamic_strategy :
      detail::generic_hazard_era_allocation_strategy<K, A, B, detail::dynamic_he_thread_control_block> {};
  }

  template <class AllocationStrategy = he_allocation::static_strategy<3>>
  struct hazard_era_traits {
    using allocation_strategy = AllocationStrategy;

    template <class... Policies>
    using with = hazard_era_traits<
      parameter::type_param_t<policy::allocation_strategy, AllocationStrategy, Policies...>
    >;
  };

  /**
   * @brief An implementation of the hazard eras scheme proposed by Ramalhete and Correia
   * \[[RC17](index.html#ref-ramalhete-2017)\].
   *
   * @tparam Traits
   */
  template <class Traits = hazard_era_traits<>>
  class hazard_eras
  {
    using allocation_strategy = typename Traits::allocation_strategy;
    using thread_control_block = typename allocation_strategy::thread_control_block;
    friend detail::basic_he_thread_control_block<allocation_strategy, thread_control_block>;

    template <class T, class MarkedPtr>
    class guard_ptr;

  public:
    template <class... Policies>
    using with = hazard_eras<typename Traits::template with<Policies...>>;

    template <class T, std::size_t N = 0, class Deleter = std::default_delete<T>>
    class enable_concurrent_ptr;

    class region_guard {};

    template <class T, std::size_t N = T::number_of_mark_bits>
    using concurrent_ptr = detail::concurrent_ptr<T, N, guard_ptr>;

    ALLOCATION_TRACKER;
  private:
    struct thread_data;

    using era_t = uint64_t;
    static std::atomic<era_t> era_clock;
    static detail::thread_block_list<thread_control_block, detail::deletable_object_with_eras> global_thread_block_list;
    static thread_local thread_data local_thread_data;

    ALLOCATION_TRACKING_FUNCTIONS;
  };

  template <class Traits>
  template <class T, std::size_t N, class Deleter>
  class hazard_eras<Traits>::enable_concurrent_ptr :
    public detail::deletable_object_impl<T, Deleter, detail::deletable_object_with_eras>,
    private detail::tracked_object<hazard_eras>
  {
  public:
    static constexpr std::size_t number_of_mark_bits = N;
  protected:
    enable_concurrent_ptr() noexcept {
      this->construction_era = era_clock.load(std::memory_order_relaxed);
    }
    enable_concurrent_ptr(const enable_concurrent_ptr&) noexcept = default;
    enable_concurrent_ptr(enable_concurrent_ptr&&) noexcept = default;
    enable_concurrent_ptr& operator=(const enable_concurrent_ptr&) noexcept = default;
    enable_concurrent_ptr& operator=(enable_concurrent_ptr&&) noexcept = default;
    ~enable_concurrent_ptr() noexcept = default;
  private:
    friend detail::deletable_object_impl<T, Deleter>;

    template <class, class>
    friend class guard_ptr;
  };

  template <class Traits>
  template <class T, class MarkedPtr>
  class hazard_eras<Traits>::guard_ptr : public detail::guard_ptr<T, MarkedPtr, guard_ptr<T, MarkedPtr>>
  {
    using base = detail::guard_ptr<T, MarkedPtr, guard_ptr>;
    using Deleter = typename T::Deleter;
  public:
    guard_ptr() noexcept = default;

    // Guard a marked ptr.
    guard_ptr(const MarkedPtr& p);
    explicit guard_ptr(const guard_ptr& p);
    guard_ptr(guard_ptr&& p) noexcept;

    guard_ptr& operator=(const guard_ptr& p) noexcept;
    guard_ptr& operator=(guard_ptr&& p) noexcept;

    // Atomically take snapshot of p, and *if* it points to unreclaimed object, acquire shared ownership of it.
    void acquire(const concurrent_ptr<T>& p, std::memory_order order = std::memory_order_seq_cst);

    // Like acquire, but quit early if a snapshot != expected.
    bool acquire_if_equal(const concurrent_ptr<T>& p,
                          const MarkedPtr& expected,
                          std::memory_order order = std::memory_order_seq_cst);

    // Release ownership. Postcondition: get() == nullptr.
    void reset() noexcept;

    // Reset. Deleter d will be applied some time after all owners release their ownership.
    void reclaim(Deleter d = Deleter()) noexcept;

  private:
    using enable_concurrent_ptr = hazard_eras::enable_concurrent_ptr<T, MarkedPtr::number_of_mark_bits, Deleter>;

    friend base;
    void do_swap(guard_ptr& g) noexcept;

    typename thread_control_block::hazard_era* he = nullptr;
  };

  namespace detail {
    struct deletable_object_with_eras
    {
      virtual void delete_self() = 0;
      deletable_object_with_eras* next = nullptr;
    protected:
      virtual ~deletable_object_with_eras() = default;
      using era_t = size_t;
      era_t construction_era;
      era_t retirement_era;
      template <class>
      friend class hazard_eras;

      template <class T>
      friend class hazard_eras<T>::thread_data;
    };
  }
}}

#define HAZARD_ERAS_IMPL
#include <xenium/reclamation/impl/hazard_eras.hpp>
#undef HAZARD_ERAS_IMPL

#endif
