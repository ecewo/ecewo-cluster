// Copyright 2025-2026 Savas Sahin <savashn@proton.me>

// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef ECEWO_CLUSTER_H
#define ECEWO_CLUSTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "ecewo-cluster-export.h"

/**
 * Opaque cluster configuration. Build with ecewo_cluster_config_new(),
 * populate via ecewo_cluster_config_set_*(), pass to ecewo_cluster_init(),
 * then free with ecewo_cluster_config_free().
 */
typedef struct ecewo_cluster_config_s ecewo_cluster_config_t;

/**
 * Opaque snapshot of a single worker's metrics. Allocate with
 * ecewo_worker_stats_new(), populate via ecewo_cluster_get_worker_stats(),
 * read fields via ecewo_worker_stats_*() accessors, free with
 * ecewo_worker_stats_free().
 */
typedef struct ecewo_worker_stats_s ecewo_worker_stats_t;

/**
 * Opaque snapshot of cluster-wide metrics. Allocate with
 * ecewo_cluster_stats_new(), populate via ecewo_cluster_get_stats(),
 * read fields via ecewo_cluster_stats_*() accessors, free with
 * ecewo_cluster_stats_free().
 */
typedef struct ecewo_cluster_stats_s ecewo_cluster_stats_t;

/** Callback fired in the master after a worker process has been spawned. */
typedef void (*ecewo_cluster_on_start_t)(uint8_t worker_id);

/** Callback fired in the master after a worker process has exited. */
typedef void (*ecewo_cluster_on_exit_t)(uint8_t worker_id, int exit_status, bool is_crash);

/** Role of the current process after ecewo_cluster_init() returns. */
typedef enum {
  ECEWO_CLUSTER_ROLE_ERROR = -1, // Initialization failed
  ECEWO_CLUSTER_ROLE_MASTER = 0, // This process should supervise workers
  ECEWO_CLUSTER_ROLE_WORKER = 1 // This process should run the ecewo app
} ecewo_cluster_role_t;

/** Lifecycle state of a worker process tracked by the master. */
typedef enum {
  ECEWO_WORKER_STARTING,
  ECEWO_WORKER_ACTIVE,
  ECEWO_WORKER_STOPPING,
  ECEWO_WORKER_CRASHED,
  ECEWO_WORKER_RESPAWNING,
  ECEWO_WORKER_DISABLED
} ecewo_worker_status_t;

// ---------------------------------------------------------------------------
// CONFIGURATION
// ---------------------------------------------------------------------------

/** Allocate a config initialized with default values. Returns NULL on OOM. */
ECEWO_CLUSTER_EXPORT ecewo_cluster_config_t *ecewo_cluster_config_new(void);

/** Free a config previously returned by ecewo_cluster_config_new(). */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_free(ecewo_cluster_config_t *config);

/** Number of worker processes to spawn. 0 means "auto-detect logical CPUs". */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_cpus(ecewo_cluster_config_t *config, uint8_t cpus);

/** Port to bind in every worker (required, must be non-zero). */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_port(ecewo_cluster_config_t *config, uint16_t port);

/** Auto-respawn workers that crash. Default: false. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_respawn(ecewo_cluster_config_t *config, bool respawn);

/** Force-kill timeout after SIGTERM, in milliseconds. Default: 15000. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_shutdown_timeout(ecewo_cluster_config_t *config, uint32_t ms);

/** Sliding window for crash-rate detection, in seconds. Default: 5. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_respawn_window(ecewo_cluster_config_t *config, uint32_t seconds);

/** Maximum crashes within the respawn window before respawn is disabled. Default: 3. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_respawn_max_crashes(ecewo_cluster_config_t *config, uint8_t max_crashes);

/** Delay between successive worker spawns at startup, in milliseconds. Default: 100. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_worker_startup_delay(ecewo_cluster_config_t *config, uint32_t ms);

/** Delay before respawning a crashed worker, in milliseconds. Default: 100. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_worker_respawn_delay(ecewo_cluster_config_t *config, uint32_t ms);

/** Callback fired in the master immediately after a worker is spawned. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_on_start(ecewo_cluster_config_t *config, ecewo_cluster_on_start_t cb);

/** Callback fired in the master after a worker exits (graceful or crash). */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_config_set_on_exit(ecewo_cluster_config_t *config, ecewo_cluster_on_exit_t cb);

// ---------------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------------

/**
 * Initialize the cluster.
 *
 * In the master process this spawns workers and registers signal handlers.
 * In a worker process this just records its identity and returns.
 *
 * Returns:
 *   ECEWO_CLUSTER_ROLE_MASTER - call ecewo_cluster_wait() to supervise workers.
 *   ECEWO_CLUSTER_ROLE_WORKER - bring up the ecewo app and listen on
 *                               ecewo_cluster_port().
 *   ECEWO_CLUSTER_ROLE_ERROR  - initialization failed; do not call other
 *                               cluster APIs.
 */
ECEWO_CLUSTER_EXPORT ecewo_cluster_role_t ecewo_cluster_init(const ecewo_cluster_config_t *config, int argc, char **argv);

/**
 * Master only. Run the master event loop until every worker exits, then clean
 * up. Returns once the cluster has fully shut down.
 */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_wait(void);

/** Master only. Forward a signal to every active worker (e.g. SIGUSR1). */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_signal_workers(int signum);

/** Master only. Trigger a rolling SIGTERM + respawn of all workers. */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_graceful_restart(void);

// ---------------------------------------------------------------------------
// INTROSPECTION
// ---------------------------------------------------------------------------

/** Port the current process is configured to bind. */
ECEWO_CLUSTER_EXPORT uint16_t ecewo_cluster_port(void);

/** True iff this process is the master. */
ECEWO_CLUSTER_EXPORT bool ecewo_cluster_is_master(void);

/** True iff this process is a worker. */
ECEWO_CLUSTER_EXPORT bool ecewo_cluster_is_worker(void);

/** Worker ID (0..N-1) of the current process. Returns 0 in the master. */
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_worker_id(void);

/** Total number of workers configured for the cluster. */
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_worker_count(void);

/** Number of logical CPUs (counts hyperthreads). Capped at 255. */
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_cpus(void);

/** Number of physical CPU cores (excludes hyperthreads). Capped at 255. */
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_cpus_physical(void);

// ---------------------------------------------------------------------------
// WORKER STATS (master only)
// ---------------------------------------------------------------------------

/** Allocate a zero-initialized worker stats handle. Returns NULL on OOM. */
ECEWO_CLUSTER_EXPORT ecewo_worker_stats_t *ecewo_worker_stats_new(void);

/** Free a stats handle returned by ecewo_worker_stats_new(). */
ECEWO_CLUSTER_EXPORT void ecewo_worker_stats_free(ecewo_worker_stats_t *stats);

/** Populate `stats` for `worker_id`. Returns 0 on success, -1 on error. */
ECEWO_CLUSTER_EXPORT int ecewo_cluster_get_worker_stats(uint8_t worker_id, ecewo_worker_stats_t *stats);

ECEWO_CLUSTER_EXPORT uint8_t ecewo_worker_stats_id(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT int ecewo_worker_stats_pid(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint16_t ecewo_worker_stats_port(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT ecewo_worker_status_t ecewo_worker_stats_status(const ecewo_worker_stats_t *stats);
/** Worker start time as Unix epoch seconds. */
ECEWO_CLUSTER_EXPORT int64_t ecewo_worker_stats_start_time(const ecewo_worker_stats_t *stats);
/** Seconds the worker has been active. 0 if not currently ACTIVE. */
ECEWO_CLUSTER_EXPORT int64_t ecewo_worker_stats_uptime_seconds(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT int ecewo_worker_stats_exit_status(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint8_t ecewo_worker_stats_crash_count(const ecewo_worker_stats_t *stats);
ECEWO_CLUSTER_EXPORT bool ecewo_worker_stats_respawn_disabled(const ecewo_worker_stats_t *stats);

/**
 * Populate up to `array_size` stats handles with per-worker snapshots. Each
 * entry in `stats_array` must be a non-NULL handle from ecewo_worker_stats_new().
 * Returns the number of slots populated, or 0 on error.
 */
ECEWO_CLUSTER_EXPORT int ecewo_cluster_get_all_workers(ecewo_worker_stats_t **stats_array, int array_size);

// ---------------------------------------------------------------------------
// CLUSTER STATS (master only)
// ---------------------------------------------------------------------------

/** Allocate a zero-initialized cluster stats handle. Returns NULL on OOM. */
ECEWO_CLUSTER_EXPORT ecewo_cluster_stats_t *ecewo_cluster_stats_new(void);

/** Free a stats handle returned by ecewo_cluster_stats_new(). */
ECEWO_CLUSTER_EXPORT void ecewo_cluster_stats_free(ecewo_cluster_stats_t *stats);

/** Populate `stats` with cluster-wide metrics. Returns 0 on success, -1 on error. */
ECEWO_CLUSTER_EXPORT int ecewo_cluster_get_stats(ecewo_cluster_stats_t *stats);

ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_stats_total_workers(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_stats_active_workers(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_stats_crashed_workers(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint8_t ecewo_cluster_stats_disabled_workers(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT bool ecewo_cluster_stats_shutdown_requested(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT bool ecewo_cluster_stats_restart_requested(const ecewo_cluster_stats_t *stats);
ECEWO_CLUSTER_EXPORT uint64_t ecewo_cluster_stats_total_restarts(const ecewo_cluster_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
