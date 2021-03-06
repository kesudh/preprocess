#ifndef UTIL_PCQUEUE_H
#define UTIL_PCQUEUE_H

#include "util/exception.hh"

#include <algorithm>
#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>

#ifdef __APPLE__
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/mach_traps.h>
#include <mach/mach.h>
#elif defined(__linux)
#include <semaphore.h>
#else
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#endif

namespace util {

/* OS X Maverick and Boost interprocess were doing "Function not implemented."
 * So this is my own wrapper around the mach kernel APIs.
 */
#ifdef __APPLE__

#define MACH_CALL(call) UTIL_THROW_IF(KERN_SUCCESS != (call), Exception, "Mach call failure")

class Semaphore {
  public:
    explicit Semaphore(int value) : task_(mach_task_self()) {
      MACH_CALL(semaphore_create(task_, &back_, SYNC_POLICY_FIFO, value));
    }

    ~Semaphore() {
      MACH_CALL(semaphore_destroy(task_, back_));
    }

    void wait() {
      MACH_CALL(semaphore_wait(back_));
    }

    void post() {
      MACH_CALL(semaphore_signal(back_));
    }

  private:
    semaphore_t back_;
    task_t task_;
};

inline void WaitSemaphore(Semaphore &semaphore) {
  semaphore.wait();
}

#elif defined(__linux)

class Semaphore {
  public:
    explicit Semaphore(unsigned int value) {
      UTIL_THROW_IF(sem_init(&sem_, 0, value), ErrnoException, "Could not create semaphore");
    }

    ~Semaphore() {
      if (-1 == sem_destroy(&sem_)) {
        std::cerr << "Could not destroy semaphore " << ErrnoException().what() << std::endl;
        abort();
      }
    }

    void wait() {
      while (UTIL_UNLIKELY(-1 == sem_wait(&sem_))) {
        UTIL_THROW_IF(errno != EINTR, ErrnoException, "Wait for semaphore failed");
      }
    }

    void post() {
      UTIL_THROW_IF(-1 == sem_post(&sem_), ErrnoException, "Could not post to semaphore");
    }

  private:
    sem_t sem_;
};

inline void WaitSemaphore(Semaphore &semaphore) {
  semaphore.wait();
}

#else
typedef boost::interprocess::interprocess_semaphore Semaphore;

inline void WaitSemaphore (Semaphore &on) {
  while (1) {
    try {
      on.wait();
      break;
    }
    catch (boost::interprocess::interprocess_exception &e) {
      if (e.get_native_error() != EINTR) {
        throw;
      }
    }
  }
}

#endif // Apple

/**
 * Producer consumer queue safe for multiple producers and multiple consumers.
 * T must be default constructable and have operator=.
 * The value is copied twice for Consume(T &out) or three times for Consume(),
 * so larger objects should be passed via pointer.
 * Strong exception guarantee if operator= throws.  Undefined if semaphores throw.
 */
template <class T> class PCQueue {
 public:
  explicit PCQueue(size_t size)
   : empty_(size), used_(0),
     storage_(new T[size]),
     end_(storage_.get() + size),
     produce_at_(storage_.get()),
     consume_at_(storage_.get()) {}

  // Add a value to the queue.
  void Produce(const T &val) {
    WaitSemaphore(empty_);
    {
      std::lock_guard<std::mutex> produce_lock(produce_at_mutex_);
      try {
        *produce_at_ = val;
      } catch (...) {
        empty_.post();
        throw;
      }
      if (++produce_at_ == end_) produce_at_ = storage_.get();
    }
    used_.post();
  }

  // Add a value to the queue, but swap it into place.
  void ProduceSwap(T &val) {
    WaitSemaphore(empty_);
    {
      std::lock_guard<std::mutex> produce_lock(produce_at_mutex_);
      try {
        std::swap(*produce_at_, val);
      } catch (...) {
        empty_.post();
        throw;
      }
      if (++produce_at_ == end_) produce_at_ = storage_.get();
    }
    used_.post();
  }


  // Consume a value, assigning it to out.
  T& Consume(T &out) {
    WaitSemaphore(used_);
    {
      std::lock_guard<std::mutex> consume_lock(consume_at_mutex_);
      try {
        out = *consume_at_;
      } catch (...) {
        used_.post();
        throw;
      }
      if (++consume_at_ == end_) consume_at_ = storage_.get();
    }
    empty_.post();
    return out;
  }

  // Consume a value, swapping it to out.
  T& ConsumeSwap(T &out) {
    WaitSemaphore(used_);
    {
      std::lock_guard<std::mutex> consume_lock(consume_at_mutex_);
      try {
        std::swap(out, *consume_at_);
      } catch (...) {
        used_.post();
        throw;
      }
      if (++consume_at_ == end_) consume_at_ = storage_.get();
    }
    empty_.post();
    return out;
  }


  // Convenience version of Consume that copies the value to return.
  // The other version is faster.
  T Consume() {
    T ret;
    Consume(ret);
    return ret;
  }

 private:
  // Number of empty spaces in storage_.
  Semaphore empty_;
  // Number of occupied spaces in storage_.
  Semaphore used_;

  std::unique_ptr<T[]> storage_;

  T *const end_;

  // Index for next write in storage_.
  T *produce_at_;
  std::mutex produce_at_mutex_;

  // Index for next read from storage_.
  T *consume_at_;
  std::mutex consume_at_mutex_;
};

template <class T> struct UnboundedPage {
  UnboundedPage() : next(nullptr) {}
  UnboundedPage *next;
  T entries[1023];
};

template <class T> class UnboundedSingleQueue {
  public:
    UnboundedSingleQueue() : valid_(0) {
      SetFilling(new UnboundedPage<T>());
      SetReading(filling_);
    }

    void Produce(const T &val) {
      if (filling_current_ == filling_end_) {
        UnboundedPage<T> *next = new UnboundedPage<T>();
        filling_->next = next;
        SetFilling(next);
      }
      *(filling_current_++) = val;
      valid_.post();
    }

    T& Consume(T &out) {
      WaitSemaphore(valid_);
      if (reading_current_ == reading_end_) {
        SetReading(reading_->next);
      }
      out = *(reading_current_++);
      return out;
    }

  private:
    void SetFilling(UnboundedPage<T> *to) {
      filling_ = to;
      filling_current_ = to->entries;
      filling_end_ = filling_current_ + sizeof(to->entries) / sizeof(T);
    }
    void SetReading(UnboundedPage<T> *to) {
      reading_.reset(to);
      reading_current_ = to->entries;
      reading_end_ = reading_current_ + sizeof(to->entries) / sizeof(T);
    }

    Semaphore valid_;

    UnboundedPage<T> *filling_;

    std::unique_ptr<UnboundedPage<T> > reading_;

    T *filling_current_;
    T *filling_end_;
    T *reading_current_;
    T *reading_end_;

    UnboundedSingleQueue(const UnboundedSingleQueue &) = delete;
    UnboundedSingleQueue &operator=(const UnboundedSingleQueue &) = delete;
};

} // namespace util

#endif // UTIL_PCQUEUE_H
