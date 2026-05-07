#include "ecewo.h"
#include "tester.h"
#include "tests.h"

int main(void) {
#ifdef __linux__
  RUN_TEST(test_cluster_cpu_count);
  RUN_TEST(test_cluster_config_lifecycle);
  RUN_TEST(test_cluster_invalid_init);
  RUN_TEST(test_cluster_introspection_before_init);
  RUN_TEST(test_cluster_stats_handles);
  RUN_TEST(test_cluster_stats_error_before_init);
  RUN_TEST(test_cluster_worker_mode_init); // must run last
#endif
  return 0;
}
