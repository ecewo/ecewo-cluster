#include "ecewo.h"
#include "tester.h"
#include "tests.h"

int main(void) {
#ifdef __linux__
  RUN_TEST(test_cluster_cpu_count);
  RUN_TEST(test_cluster_callbacks);
  RUN_TEST(test_cluster_invalid_config);
  RUN_TEST(test_cluster_port_strategy);
#endif

  return 0;
}
