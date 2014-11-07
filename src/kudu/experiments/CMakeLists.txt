# Copyright (c) 2014, Cloudera, inc.
# Confidential Cloudera Information: Covered by NDA.

# rwlock-perf
# No point in running on Mac OS, where sched_getcpu() doesn't exist.
if (NOT APPLE)
  add_executable(rwlock-perf rwlock-perf.cc)
  target_link_libraries(rwlock-perf
    kudu_util
    ${KUDU_MIN_TEST_LIBS})
endif()

add_executable(merge-test merge-test.cc)
target_link_libraries(merge-test
  kudu_util
  ${KUDU_MIN_TEST_LIBS})
