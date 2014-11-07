// Copyright (c) 2012, Cloudera, inc
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_UTIL_STOPWATCH_H
#define KUDU_UTIL_STOPWATCH_H

#include <glog/logging.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <string>
#if defined(__APPLE__)
#include <mach/clock.h>
#include <mach/mach.h>
#endif  // defined(__APPLE__)

#include "kudu/gutil/macros.h"
#include "kudu/gutil/stringprintf.h"

namespace kudu {

// Macro for logging timing of a block. Usage:
//   LOG_TIMING_IF(INFO, FLAGS_should_record_time, "doing some task") {
//     ... some task which takes some time
//   }
// If FLAGS_should_record_time is true, yields a log like:
// I1102 14:35:51.726186 23082 file.cc:167] Time spent doing some task:
//   real 3.729s user 3.570s sys 0.150s
// The task will always execute regardless of whether the timing information is
// printed.
#define LOG_TIMING_IF(severity, condition, description) \
  for (kudu::sw_internal::LogTiming _l(__FILE__, __LINE__, google::severity, description, \
          -1, (condition)); !_l.HasRun(); _l.MarkHasRun())

// Alias for LOG_TIMING_IF(severity, true, description)
#define LOG_TIMING(severity, description) \
  LOG_TIMING_IF(severity, true, (description))

// Macro to log the time spent in the rest of the block.
#define SCOPED_LOG_TIMING(severity, description) \
  kudu::sw_internal::LogTiming VARNAME_LINENUM(_log_timing)(__FILE__, __LINE__, \
      google::severity, description, -1, true);

// Macro for logging timing of a block. Usage:
//   LOG_SLOW_EXECUTION(INFO, 5, "doing some task") {
//     ... some task which takes some time
//   }
// when slower than 5 milliseconds, yields a log like:
// I1102 14:35:51.726186 23082 file.cc:167] Time spent doing some task:
//   real 3.729s user 3.570s sys 0.150s
#define LOG_SLOW_EXECUTION(severity, max_expected_millis, description) \
  for (kudu::sw_internal::LogTiming _l(__FILE__, __LINE__, google::severity, description, \
          max_expected_millis, true); !_l.HasRun(); _l.MarkHasRun())

// Macro for vlogging timing of a block. The execution happens regardless of the vlog_level,
// it's only the logging that's affected.
// Usage:
//   VLOG_TIMING(1, "doing some task") {
//     ... some task which takes some time
//   }
// Yields a log just like LOG_TIMING's.
#define VLOG_TIMING(vlog_level, description) \
  for (kudu::sw_internal::LogTiming _l(__FILE__, __LINE__, google::INFO, description, \
          -1, VLOG_IS_ON(vlog_level)); !_l.HasRun(); _l.MarkHasRun())

// Macro to log the time spent in the rest of the block.
#define SCOPED_VLOG_TIMING(vlog_level, description) \
  kudu::sw_internal::LogTiming VARNAME_LINENUM(_log_timing)(__FILE__, __LINE__, \
      google::INFO, description, -1, VLOG_IS_ON(vlog_level));

#define NANOS_PER_SECOND 1000000000.0
#define NANOS_PER_MILLISECOND 1000000.0

class Stopwatch;

typedef uint64_t nanosecond_type;

// Structure which contains an elapsed amount of wall/user/sys time.
struct CpuTimes {
  nanosecond_type wall;
  nanosecond_type user;
  nanosecond_type system;

  void clear() { wall = user = system = 0LL; }

  // Return a string formatted similar to the output of the "time" shell command.
  std::string ToString() const {
    return StringPrintf(
      "real %.3fs\tuser %.3fs\tsys %.3fs",
      wall_seconds(), user_cpu_seconds(), system_cpu_seconds());
  }

  double wall_millis() const {
    return static_cast<double>(wall) / NANOS_PER_MILLISECOND;
  }

  double wall_seconds() const {
    return static_cast<double>(wall) / NANOS_PER_SECOND;
  }

  double user_cpu_seconds() const {
    return static_cast<double>(user) / NANOS_PER_SECOND;
  }

  double system_cpu_seconds() const {
    return static_cast<double>(system) / NANOS_PER_SECOND;
  }
};

// A Stopwatch is a convenient way of timing a given operation.
//
// Wall clock time is based on a monotonic timer, so can be reliably used for
// determining durations.
// CPU time is based on the current thread's usage (not the whole process).
//
// The implementation relies on several syscalls, so should not be used for
// hot paths, but is useful for timing anything on the granularity of seconds
// or more.
class Stopwatch {
 public:

  enum Mode {
    // Collect usage only about the calling thread.
    // This may not be supported on older versions of Linux.
    THIS_THREAD,
    // Collect usage of all threads.
    ALL_THREADS
  };

  // Construct a new stopwatch. The stopwatch is initially stopped.
  explicit Stopwatch(Mode mode = THIS_THREAD)
    : stopped_(true),
      mode_(mode) {
    times_.clear();
  }

  // Start counting. If the stopwatch is already counting, then resets the
  // start point at the current time.
  void start() {
    stopped_ = false;
    GetTimes(&times_);
  }

  // Stop counting. If the stopwatch is already stopped, has no effect.
  void stop() {
    if (stopped_) return;
    stopped_ = true;

    CpuTimes current;
    GetTimes(&current);
    times_.wall = current.wall - times_.wall;
    times_.user = current.user - times_.user;
    times_.system = current.system - times_.system;
  }

  // Return the elapsed amount of time. If the stopwatch is running, then returns
  // the amount of time since it was started. If it is stopped, returns the amount
  // of time between the most recent start/stop pair. If the stopwatch has never been
  // started, the elapsed time is considered to be zero.
  CpuTimes elapsed() const {
    if (stopped_) return times_;

    CpuTimes current;
    GetTimes(&current);
    current.wall -= times_.wall;
    current.user -= times_.user;
    current.system -= times_.system;
    return current;
  }

  // Resume a stopped stopwatch, such that the elapsed time continues to grow from
  // the point where it was last stopped.
  // For example:
  //   Stopwatch s;
  //   s.start();
  //   sleep(1); // elapsed() is now ~1sec
  //   s.stop();
  //   sleep(1);
  //   s.resume();
  //   sleep(1); // elapsed() is now ~2sec
  void resume() {
    if (!stopped_) return;

    CpuTimes current(times_);
    start();
    times_.wall   -= current.wall;
    times_.user   -= current.user;
    times_.system -= current.system;
  }

  bool is_stopped() const {
    return stopped_;
  }

 private:
  void GetTimes(CpuTimes *times) const {
    struct rusage usage;
    CHECK_EQ(0, getrusage((mode_ == THIS_THREAD) ? RUSAGE_THREAD : RUSAGE_SELF, &usage));
    struct timespec wall;

#if defined(linux)
    CHECK_EQ(0, clock_gettime(CLOCK_MONOTONIC, &wall));
#elif defined(__APPLE__)
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    ts.tv_sec = mts.tv_sec;
    ts.tv_nsec = mts.tv_nsec;
    ts.tv_usec = mts.tv_nsec * 1000;
#endif  // defined(linux)
    times->wall   = wall.tv_sec * 1000000000L + wall.tv_nsec;
    times->user   = usage.ru_utime.tv_sec * 1000000000L + usage.ru_utime.tv_usec * 1000;
    times->system = usage.ru_stime.tv_sec * 1000000000L + usage.ru_stime.tv_usec * 1000;
  }

  bool stopped_;

  CpuTimes times_;
  Mode mode_;
};


namespace sw_internal {

// Internal class used by the LOG_TIMING macro.
class LogTiming {
 public:
  LogTiming(const char *file, int line, google::LogSeverity severity,
            const std::string &description, int64_t max_expected_millis,
            bool should_print)
    : file_(file),
      line_(line),
      severity_(severity),
      description_(description),
      max_expected_millis_(max_expected_millis),
      should_print_(should_print),
      has_run_(false) {
    stopwatch_.start();
  }

  ~LogTiming() {
    if (should_print_) {
      Print(max_expected_millis_);
    }
  }

  // Allows this object to be used as the loop variable in for-loop macros.
  // Call HasRun() in the conditional check in the for-loop.
  bool HasRun() {
    return has_run_;
  }

  // Allows this object to be used as the loop variable in for-loop macros.
  // Call MarkHasRun() in the "increment" section of the for-loop.
  void MarkHasRun() {
    has_run_ = true;
  }

 private:
  Stopwatch stopwatch_;
  const char *file_;
  const int line_;
  const google::LogSeverity severity_;
  const std::string description_;
  const int64_t max_expected_millis_;
  const bool should_print_;
  bool has_run_;

  // Print if the number of expected millis exceeds the max.
  // Passing a negative number implies "always print".
  void Print(int64_t max_expected_millis) {
    stopwatch_.stop();
    CpuTimes times = stopwatch_.elapsed();
    if (times.wall_millis() > max_expected_millis) {
      google::LogMessage(file_, line_, severity_).stream()
        << "Time spent " << description_ << ": "
        << times.ToString();
    }
  }

};

} // namespace sw_internal
} // namespace kudu

#endif
