#include "ecewo-cluster.h"
#include "ecewo.h"
#include "tester.h"
#include <string.h>
#include <stdio.h>

static bool s_started;
static bool s_exited;
static uint8_t s_last_worker_id;
static int s_last_exit_status;
static bool s_last_is_crash;

static void on_start(uint8_t worker_id) {
  s_started = true;
  s_last_worker_id = worker_id;
}

static void on_exit_cb(uint8_t worker_id, int status, bool is_crash) {
  s_exited = true;
  s_last_worker_id = worker_id;
  s_last_exit_status = status;
  s_last_is_crash = is_crash;
}

int test_cluster_cpu_count(void) {
  uint8_t cpus = ecewo_cluster_cpus();
  ASSERT_GT(cpus, 0);
  ASSERT_LE(cpus, 255);

  uint8_t physical = ecewo_cluster_cpus_physical();
  ASSERT_GT(physical, 0);
  ASSERT_LE(physical, cpus);

  RETURN_OK();
}

int test_cluster_config_lifecycle(void) {
  ecewo_cluster_config_t *cfg = ecewo_cluster_config_new();
  ASSERT_NOT_NULL(cfg);

  ecewo_cluster_config_set_cpus(cfg, ecewo_cluster_cpus());
  ecewo_cluster_config_set_port(cfg, 3000);
  ecewo_cluster_config_set_respawn(cfg, true);
  ecewo_cluster_config_set_shutdown_timeout(cfg, 5000);
  ecewo_cluster_config_set_respawn_window(cfg, 10);
  ecewo_cluster_config_set_respawn_max_crashes(cfg, 5);
  ecewo_cluster_config_set_worker_startup_delay(cfg, 50);
  ecewo_cluster_config_set_worker_respawn_delay(cfg, 50);
  ecewo_cluster_config_set_on_start(cfg, on_start);
  ecewo_cluster_config_set_on_exit(cfg, on_exit_cb);

  // Setters should tolerate NULL.
  ecewo_cluster_config_set_port(NULL, 1234);
  ecewo_cluster_config_set_on_start(NULL, on_start);

  ecewo_cluster_config_free(cfg);
  ecewo_cluster_config_free(NULL);

  RETURN_OK();
}

int test_cluster_invalid_init(void) {
  // NULL config.
  ASSERT_EQ(ecewo_cluster_init(NULL, 0, NULL), ECEWO_CLUSTER_ROLE_ERROR);

  // Missing port.
  ecewo_cluster_config_t *cfg = ecewo_cluster_config_new();
  ASSERT_NOT_NULL(cfg);
  ecewo_cluster_config_set_cpus(cfg, 2);
  char *fake_argv[] = { (char *)"prog", NULL };
  ASSERT_EQ(ecewo_cluster_init(cfg, 1, fake_argv), ECEWO_CLUSTER_ROLE_ERROR);

  // NULL argv.
  ecewo_cluster_config_set_port(cfg, 3000);
  ASSERT_EQ(ecewo_cluster_init(cfg, 0, NULL), ECEWO_CLUSTER_ROLE_ERROR);

  ecewo_cluster_config_free(cfg);
  RETURN_OK();
}

int test_cluster_introspection_before_init(void) {
  ASSERT_FALSE(ecewo_cluster_is_master());
  ASSERT_FALSE(ecewo_cluster_is_worker());
  ASSERT_EQ(ecewo_cluster_port(), 0);
  RETURN_OK();
}

int test_cluster_stats_handles(void) {
  ecewo_worker_stats_t *ws = ecewo_worker_stats_new();
  ASSERT_NOT_NULL(ws);

  ASSERT_EQ(ecewo_worker_stats_id(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_pid(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_port(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_status(ws), ECEWO_WORKER_STARTING);
  ASSERT_EQ(ecewo_worker_stats_start_time(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_uptime_seconds(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_exit_status(ws), 0);
  ASSERT_EQ(ecewo_worker_stats_crash_count(ws), 0);
  ASSERT_FALSE(ecewo_worker_stats_respawn_disabled(ws));

  ASSERT_EQ(ecewo_worker_stats_id(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_pid(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_port(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_status(NULL), ECEWO_WORKER_DISABLED);
  ASSERT_EQ(ecewo_worker_stats_start_time(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_uptime_seconds(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_exit_status(NULL), 0);
  ASSERT_EQ(ecewo_worker_stats_crash_count(NULL), 0);
  ASSERT_FALSE(ecewo_worker_stats_respawn_disabled(NULL));

  ecewo_worker_stats_free(ws);
  ecewo_worker_stats_free(NULL);

  ecewo_cluster_stats_t *cs = ecewo_cluster_stats_new();
  ASSERT_NOT_NULL(cs);

  ASSERT_EQ(ecewo_cluster_stats_total_workers(cs), 0);
  ASSERT_EQ(ecewo_cluster_stats_active_workers(cs), 0);
  ASSERT_EQ(ecewo_cluster_stats_crashed_workers(cs), 0);
  ASSERT_EQ(ecewo_cluster_stats_disabled_workers(cs), 0);
  ASSERT_FALSE(ecewo_cluster_stats_shutdown_requested(cs));
  ASSERT_FALSE(ecewo_cluster_stats_restart_requested(cs));
  ASSERT_EQ(ecewo_cluster_stats_total_restarts(cs), 0);

  ASSERT_EQ(ecewo_cluster_stats_total_workers(NULL), 0);
  ASSERT_EQ(ecewo_cluster_stats_active_workers(NULL), 0);
  ASSERT_EQ(ecewo_cluster_stats_crashed_workers(NULL), 0);
  ASSERT_EQ(ecewo_cluster_stats_disabled_workers(NULL), 0);
  ASSERT_FALSE(ecewo_cluster_stats_shutdown_requested(NULL));
  ASSERT_FALSE(ecewo_cluster_stats_restart_requested(NULL));
  ASSERT_EQ(ecewo_cluster_stats_total_restarts(NULL), 0);

  ecewo_cluster_stats_free(cs);
  ecewo_cluster_stats_free(NULL);

  RETURN_OK();
}

int test_cluster_stats_error_before_init(void) {
  ASSERT_EQ(ecewo_cluster_worker_id(), 0);
  ASSERT_EQ(ecewo_cluster_worker_count(), 0);

  ecewo_worker_stats_t *ws = ecewo_worker_stats_new();
  ASSERT_NOT_NULL(ws);
  ASSERT_EQ(ecewo_cluster_get_worker_stats(0, ws), -1);
  ASSERT_EQ(ecewo_cluster_get_worker_stats(0, NULL), -1);
  ecewo_worker_stats_free(ws);

  ecewo_cluster_stats_t *cs = ecewo_cluster_stats_new();
  ASSERT_NOT_NULL(cs);
  ASSERT_EQ(ecewo_cluster_get_stats(cs), -1);
  ASSERT_EQ(ecewo_cluster_get_stats(NULL), -1);
  ecewo_cluster_stats_free(cs);

  ecewo_worker_stats_t *ws2 = ecewo_worker_stats_new();
  ASSERT_NOT_NULL(ws2);
  ecewo_worker_stats_t *arr[1] = { ws2 };
  ASSERT_EQ(ecewo_cluster_get_all_workers(arr, 1), 0);
  ASSERT_EQ(ecewo_cluster_get_all_workers(NULL, 0), 0);
  ecewo_worker_stats_free(ws2);

  RETURN_OK();
}

// Must run last: sets state.initialized = true (worker mode).
int test_cluster_worker_mode_init(void) {
  ecewo_cluster_config_t *cfg = ecewo_cluster_config_new();
  ASSERT_NOT_NULL(cfg);
  ecewo_cluster_config_set_port(cfg, 3000);

  // All argv entries must live in one contiguous buffer. uv_setup_args computes
  // the process-title capacity as argv[last] + last_len - argv[0]; when argv
  // pointers span separate allocations the arithmetic overflows, and the
  // subsequent memset in uv_set_process_title crashes.
  // Layout: "prog\0--cluster-worker\02\03000\0" (29 bytes used, 64 allocated)
  char fake_args[64] = "prog\0--cluster-worker\0" "2\0" "3000";
  char *argv[] = { fake_args, fake_args + 5, fake_args + 22, fake_args + 24, NULL };
  ecewo_cluster_role_t role = ecewo_cluster_init(cfg, 4, argv);
  ecewo_cluster_config_free(cfg);

  ASSERT_EQ(role, ECEWO_CLUSTER_ROLE_WORKER);
  ASSERT_TRUE(ecewo_cluster_is_worker());
  ASSERT_FALSE(ecewo_cluster_is_master());
  ASSERT_EQ(ecewo_cluster_worker_id(), 2);
  ASSERT_EQ(ecewo_cluster_port(), 3000);

  ecewo_worker_stats_t *ws = ecewo_worker_stats_new();
  ASSERT_NOT_NULL(ws);
  ASSERT_EQ(ecewo_cluster_get_worker_stats(0, ws), -1);
  ecewo_worker_stats_free(ws);

  ecewo_cluster_stats_t *cs = ecewo_cluster_stats_new();
  ASSERT_NOT_NULL(cs);
  ASSERT_EQ(ecewo_cluster_get_stats(cs), -1);
  ecewo_cluster_stats_free(cs);

  RETURN_OK();
}
