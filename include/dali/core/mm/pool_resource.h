// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DALI_CORE_MM_POOL_RESOURCE_H_
#define DALI_CORE_MM_POOL_RESOURCE_H_

#include <mutex>
#include <condition_variable>
#include "dali/core/mm/memory_resource.h"
#include "dali/core/mm/detail/free_list.h"
#include "dali/core/small_vector.h"
#include "dali/core/device_guard.h"
#include "dali/core/util.h"

namespace dali {
namespace mm {

enum class sync_scope {
  none  = 0,   ///< no synchronization required
  device = 1,  ///< synchronize with the current device
  system = 2   ///< synchronize with all devices in the system
};

struct pool_options {
  /**
   * @brief Maximum block size
   *
   * Growth stops at this point; larger blocks are allocated only when allocate is called with
   * a larger memory requirements.
   */
  size_t max_block_size = static_cast<size_t>(-1);  // no limit
  /// Minimum size of blocks requested from upstream
  size_t min_block_size = (1 << 12);
  /// The factor by which the allocation size grows until it reaches max_block_size
  float growth_factor = 2;
  /**
   * @brief Whether to try to allocate smaller blocks from upstream if default upcoming
   *        block is unavailable.
   */
  bool try_smaller_on_failure = true;
  /**
   * @brief Whether to try to return completely free blocks to the upstream when an allocation
   *        from upstream failed. This may effectively flush the pool.
   *
   * @remarks This option is ignored when `try_smaller_on_failure` is set to `false`.
   */
  bool return_to_upstream_on_failure = true;

  /**
   * @brief To what extent should `deallocate` synchronize before making the memory available
   */
  sync_scope sync = sync_scope::none;

  /**
   * @brief Enables deferred deallocation if the pool supports it (otherwise ignored)
   */
  bool enable_deferred_deallocation = false;

  /**
   * @brief Maximum number of outstanding deferred deallocations
   *
   * If there are more outstanding deferred deallocations than this number,
   * the subsequent allocation blocks.
   */
  int max_outstanding_deallocations = 16;

  size_t upstream_alignment = 256;
};

constexpr pool_options default_host_pool_opts() noexcept {
  return { (1 << 28), (1 << 12), 2.0f, true, true };
}

constexpr pool_options default_device_pool_opts() noexcept {
  return { (static_cast<size_t>(1) << 32), (1 << 20), 2.0f, true, true };
}

template <memory_kind kind>
constexpr sync_scope default_sync_scope() {
  return kind == memory_kind::device ? sync_scope::device
                                     : kind == memory_kind::host ? sync_scope::none
                                                                 : sync_scope::system;
}

template <memory_kind kind>
constexpr pool_options default_pool_opts() noexcept {
  if (kind == memory_kind::host) {
    return default_host_pool_opts();
  } else {
    auto opt = default_device_pool_opts();
    opt.sync = default_sync_scope<kind>();
    opt.enable_deferred_deallocation = true;
    return opt;
  }
}

struct dealloc_params {
  int sync_device = -1;  // -1 == current device
  void *ptr = nullptr;
  size_t bytes = 0, alignment = 0;
};

namespace detail {

inline void synchronize_all_devices() {
  int ndev;
  CUDA_CALL(cudaGetDeviceCount(&ndev));
  DeviceGuard dg;
  for (int i = 0; i < ndev; i++) {
    CUDA_CALL(cudaSetDevice(i));
    CUDA_CALL(cudaDeviceSynchronize());
  }
}

inline void synchronize(sync_scope scope) {
  if (scope == sync_scope::device) {
      CUDA_CALL(cudaDeviceSynchronize());
  } else if (scope == sync_scope::system) {
    synchronize_all_devices();
  }
}

}  // namespace detail

template <memory_kind kind, typename Context, class FreeList, class LockType>
class pool_resource_base : public memory_resource<kind, Context> {
 public:
  explicit pool_resource_base(memory_resource<kind, Context> *upstream = nullptr,
                              const pool_options opt = default_pool_opts<kind>())
  : upstream_(upstream), options_(opt) {
     next_block_size_ = opt.min_block_size;
  }

  pool_resource_base(const pool_resource_base &) = delete;
  pool_resource_base(pool_resource_base &&) = delete;

  ~pool_resource_base() {
    free_all();
  }

  void free_all() {
    upstream_lock_guard uguard(upstream_lock_);
    lock_guard guard(lock_);
    for (auto &block : blocks_) {
      upstream_->deallocate(block.ptr, block.bytes, block.alignment);
    }
    blocks_.clear();
    free_list_.clear();
  }

  /**
   * @brief Deallocates multiple blocks of memory, but synchronizes only once
   *
   * @remarks This function must not use do_deallocate virtual function.
   */
  void bulk_deallocate(span<const dealloc_params> params) {
    if (!params.empty()) {
      synchronize(params);
      lock_guard guard(lock_);
      for (const dealloc_params &par : params) {
        free_list_.put(par.ptr, par.bytes);
      }
    }
  }

  void synchronize(span<const dealloc_params> params) {
    if (options_.sync == sync_scope::device) {
      int prev = -1;
      const int kMaxDevices = 256;
      uint32_t dev_mask[kMaxDevices >> 5] = {};  // NOLINT - linter doesn't notice it's a constant expression
      for (const dealloc_params &par : params) {
        int dev = par.sync_device;
        if (dev < 0) {
          CUDA_CALL(cudaGetDevice(&dev));
        }
        if (dev < kMaxDevices) {  // that should do in all realistic cases
          int bin = dev >> 5;
          uint32_t mask = 1 << (dev & 31);
          if (dev_mask[bin] & mask)
            continue;  // already synchronized
          dev_mask[bin] |= mask;
        } else if (dev == prev) {  // if there's a highly unlikely system with >256 devices
          continue;                // we just check if the device is the same as previous or not
        }
        DeviceGuard dg(dev);
        CUDA_CALL(cudaDeviceSynchronize());
        prev = dev;
      }
    } else if (options_.sync == sync_scope::system) {
      detail::synchronize_all_devices();
    }
  }

  void synchronize() {
    detail::synchronize(options_.sync);
  }

  /**
   * @brief Flushes deferred deallocations, if supported
   *
   * This function is overriden by the deferred_deallocator modifier
   */
  virtual void flush_deferred() {}  /* no-op */


  /**
   * @brief Tries to obtain a block from the internal free list.
   *
   * Allocates `bytes` memory from the free list. If a block that satisifies
   * the size or alignment requirements is not found, the function returns
   * nullptr withoug allocating from upstream.
   */
  void *try_allocate_from_free(size_t bytes, size_t alignment) {
    if (!bytes)
      return nullptr;

    {
      lock_guard guard(lock_);
      return free_list_.get(bytes, alignment);
    }
  }

  /**
   * @brief Deallocates a block memory without synchronization
   *
   * Places a block of memory in the free list for immediate reuse.
   * The caller must guarantee, that the memory is available without
   * any additional synchronization in the execution context for this resource.
   */
  void deallocate_no_sync(void *ptr, size_t bytes, size_t alignment = alignof(std::max_align_t)) {
    lock_guard guard(lock_);
    free_list_.put(ptr, bytes);
  }

 protected:
  void *do_allocate(size_t bytes, size_t alignment) override {
    if (!bytes)
      return nullptr;

    {
      lock_guard guard(lock_);
      void *ptr = free_list_.get(bytes, alignment);
      if (ptr)
        return ptr;
    }
    alignment = std::max(alignment, options_.upstream_alignment);
    size_t blk_size = bytes;
    void *new_block = get_upstream_block(blk_size, bytes, alignment);
    assert(new_block);
    if (blk_size == bytes) {
      // we've allocated a block exactly of the required size - there's little
      // chance that it will be merged with anything in the pool, so we'll return it as-is
      return new_block;
    } else {
      // we've allocated an oversized block - put the remainder in the free list
      lock_guard guard(lock_);
      free_list_.put(static_cast<char *>(new_block) + bytes, blk_size - bytes);
      return new_block;
    }
  }

  void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
    (void)alignment;
    synchronize();
    deallocate_no_sync(ptr, bytes, alignment);
  }

  void *get_upstream_block(size_t &blk_size, size_t min_bytes, size_t alignment) {
    upstream_lock_guard uguard(upstream_lock_);
    blk_size = next_block_size(min_bytes);
    bool tried_return_to_upstream = false;
    void *new_block = nullptr;
    for (;;) {
      try {
        new_block = upstream_->allocate(blk_size, alignment);
        break;
      } catch (const std::bad_alloc &) {
        // If there are outstanding deallocations, wait for them to complete.
        flush_deferred();
        if (!options_.try_smaller_on_failure)
          throw;
        if (blk_size == min_bytes) {
          // We've reached the minimum size and still got no memory from upstream
          // - try to free something.
          if (tried_return_to_upstream || !options_.return_to_upstream_on_failure)
            throw;
          if (blocks_.empty())  // nothing to free -> fail
            throw;
          // If there are some upstream blocks which are completely free
          // (the free list covers them completely), we can try to return them
          // to the upstream, with the hope that it will reorganize and succeed in
          // the subsequent allocation attempt.
          int blocks_freed = 0;
          SmallVector<bool, 32> removed;
          removed.resize(blocks_.size(), false);
          {
            lock_guard guard(lock_);
            for (int i = 0; i < static_cast<int>(blocks_.size()); i++) {
              UpstreamBlock blk = blocks_[i];
              removed[i] = free_list_.remove_if_in_list(blk.ptr, blk.bytes);
              if (removed[i])
                blocks_freed++;
            }
          }

          if (!blocks_freed)
            throw;  // we freed nothing, so there's no point in retrying to allocate

          for (int i = blocks_.size() - 1; i >= 0; i--) {
            if (removed[i]) {
              UpstreamBlock blk = blocks_[i];
              upstream_->deallocate(blk.ptr, blk.bytes, blk.alignment);
              blocks_.erase_at(i);
            }
          }
          // mark that we've tried, so we can fail fast the next time
          tried_return_to_upstream = true;
        }
        blk_size = std::max(min_bytes, blk_size >> 1);

        // Shrink the next_block_size_, so that we don't try to allocate a big block
        // next time, because it would likely fail anyway.
        next_block_size_ = blk_size;
      }
    }
    try {
      blocks_.push_back({ new_block, blk_size, alignment });
    } catch (...) {
      upstream_->deallocate(new_block, blk_size, alignment);
      throw;
    }
    return new_block;
  }

  virtual Context do_get_context() const noexcept {
    return upstream_->get_context();
  }

  size_t next_block_size(size_t upcoming_allocation_size) {
    size_t actual_block_size = std::max<size_t>(upcoming_allocation_size,
                                                next_block_size_ * options_.growth_factor);
    // Align the upstream block to reduce fragmentation.
    // The upstream resource (e.g. OS routine) may return blocks that have
    // coarse size granularity. This may result in fragmentation - the next
    // large block will be overaligned and we'll never see the padding.
    // Even though we might have received contiguous memory, we're not aware of that.
    // To reduce the probability of this happening, we align the size to 1/1024th
    // of the allocation size or 4kB (typical page size), whichever is larger.
    // This makes (at least sometimes) the large blocks to be seen as adjacent
    // and therefore enables coalescing in the free list.
    size_t alignment = 1uL << std::max((ilog2(actual_block_size) - 10), 12);
    actual_block_size = align_up(actual_block_size, alignment);
    next_block_size_ = std::min<size_t>(actual_block_size, options_.max_block_size);
    return actual_block_size;
  }

  memory_resource<kind, Context> *upstream_;
  FreeList free_list_;

  // locking order: upstream_lock_, lock_
  std::mutex upstream_lock_;
  LockType lock_;
  pool_options options_;
  size_t next_block_size_ = 0;

  struct UpstreamBlock {
    void *ptr;
    size_t bytes, alignment;
  };

  SmallVector<UpstreamBlock, 16> blocks_;
  using lock_guard = std::lock_guard<LockType>;
  using unique_lock = std::unique_lock<LockType>;
  using upstream_lock_guard = std::lock_guard<std::mutex>;
};

template <memory_kind kind, typename Context, class FreeList, class LockType>
class deferred_dealloc_pool : public pool_resource_base<kind, Context, FreeList, LockType> {
 public:
  static_assert(!std::is_same<LockType, detail::dummy_lock>::value,
                "This resource is inherently multithreaded and requires a functioning lock.");

  explicit deferred_dealloc_pool(memory_resource<kind, Context> *upstream = nullptr,
                                 const pool_options opt = default_pool_opts<kind>())
  : base(upstream, opt) {}

  ~deferred_dealloc_pool() {
    if (worker_.joinable()) {
      stop();
      worker_.join();
    }
    this->bulk_deallocate(make_span(deallocs_[0]));
    this->bulk_deallocate(make_span(deallocs_[1]));
  }

  void deferred_deallocate(void *ptr,
                           size_t bytes,
                           size_t alignment = alignof(std::max_align_t),
                           int device_id = -1) {
    if (!ptr || !bytes)
      return;  // nothing to do
    if (device_id < 0) {
      CUDA_CALL(cudaGetDevice(&device_id));
    }

    {
      std::lock_guard<std::mutex> g(mtx_);
      deallocs_[queue_idx_].push_back({device_id, ptr, bytes, alignment});

      if (!started_)
        start_worker();
    }
    cv_.notify_one();
  }


  int outstanding_dealloc_count() const {
    return deallocs_[0].size() + deallocs_[1].size();
  }

  /**
   * @brief Waits until currently scheduled deallocations are flushed.
   *
   * This function waits until the worker notifies that it's completed flushing
   * current queue (there are two queues). It doesn't wait for the other queue
   * nor prevent new deallocations from being scheduled.
   *
   * This method overrides the default no-op function from pool resource.
   */
  void flush_deferred() override {
    if (!no_pending_deallocs()) {
      std::unique_lock<std::mutex> ulock(mtx_);
      if (!no_pending_deallocs())
        ready_.wait(ulock);
    }
  }

 protected:
  // exposed for testing
  bool no_pending_deallocs() const noexcept {
    return deallocs_[0].empty() && deallocs_[1].empty();
  }

 private:
  using base = pool_resource_base<kind, Context, FreeList, LockType>;
  using dealloc_queue = SmallVector<dealloc_params, 16>;

  void *do_allocate(size_t bytes, size_t alignment) override {
    if (this->options_.enable_deferred_deallocation) {
      if (this->outstanding_dealloc_count() > this->options_.max_outstanding_deallocations)
        this->flush_deferred();
    }
    return base::do_allocate(bytes, alignment);
  }

  void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
    if (this->options_.enable_deferred_deallocation)
      this->deferred_deallocate(ptr, bytes, alignment);
    else
      base::do_deallocate(ptr, bytes, alignment);
  }


  void start_worker() {
    worker_ = std::thread([this]() {
      run();
    });
    started_ = true;
  }

  void run() {
    std::unique_lock<std::mutex> ulock(mtx_);
    while (!is_stopped()) {
      cv_.wait(ulock, [&](){ return !stopped_ || deallocs_[queue_idx_].empty(); });
      if (is_stopped())
        break;
      auto &to_free = deallocs_[queue_idx_];
      queue_idx_ = 1 - queue_idx_;
      ulock.unlock();
      this->bulk_deallocate(make_span(to_free));
      to_free.clear();
      ready_.notify_one();
      ulock.lock();
    }
  }

  void stop() {
    stopped_ = true;
    cv_.notify_all();
  }

  bool is_stopped() const noexcept { return stopped_; }

  std::thread worker_;
  std::mutex mtx_;
  std::condition_variable cv_, ready_;
  dealloc_queue deallocs_[2];
  int queue_idx_ = 0;
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace mm
}  // namespace dali

#endif  // DALI_CORE_MM_POOL_RESOURCE_H_
