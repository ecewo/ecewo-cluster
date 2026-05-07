#ifndef TESTS_H
#define TESTS_H

int test_cluster_cpu_count(void);
int test_cluster_config_lifecycle(void);
int test_cluster_invalid_init(void);
int test_cluster_introspection_before_init(void);
int test_cluster_stats_handles(void);
int test_cluster_stats_error_before_init(void);
int test_cluster_worker_mode_init(void);

#endif
