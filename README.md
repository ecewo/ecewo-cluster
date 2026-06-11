# ecewo-cluster

ecewo is single-threaded by default. The cluster plugin runs your ecewo app
across all CPU cores using multi-process parallelism (one process per worker,
sharing a single port via `SO_REUSEPORT`).

> [!WARNING]
> Linux only.

## Table of Contents

1. [How it works](#how-it-works)
2. [Installation](#installation)
3. [Quick start](#quick-start)
4. [Configuration](#configuration)
5. [Lifecycle and signals](#lifecycle-and-signals)
6. [Statistics](#statistics)
7. [API reference](#api-reference)

## How it works

- The first invocation of your binary becomes the **master**. It spawns one
  worker per configured CPU and supervises them.
- Each worker is a re-exec of your binary with `--cluster-worker <id> <port>`
  appended to argv. The cluster plugin detects this on startup, marks the
  process as a worker, and returns control to your `main()`.
- All workers bind the same port via `SO_REUSEPORT`, so the kernel
  load-balances connections.
- The master forwards `SIGTERM`/`SIGINT` to workers (graceful shutdown) and
  treats `SIGUSR2` as a rolling restart trigger.
- Workers that crash are respawned automatically when `respawn` is enabled,
  with a configurable rate-limit to break crash loops.

## Installation

```cmake
ecewo_add(cluster@v0.2.0)

target_link_libraries(app PRIVATE
  ecewo::ecewo
  ecewo::cluster
)
```

## Quick start

```c
#include "ecewo.h"
#include "ecewo-cluster.h"

static void handle_index(ecewo_request_t *req, ecewo_response_t *res) {
  char *msg = ecewo_sprintf(ecewo_req_arena(req),
                            "Hello from worker %u!", ecewo_cluster_worker_id());
  ecewo_send_text(res, ECEWO_OK, msg);
}

int main(int argc, char *argv[]) {
  ecewo_cluster_config_t *cfg = ecewo_cluster_config_new();
  ecewo_cluster_config_set_port(cfg, 3000);
  ecewo_cluster_config_set_respawn(cfg, true);

  ecewo_cluster_role_t role = ecewo_cluster_init(cfg, argc, argv);
  ecewo_cluster_config_free(cfg);

  switch (role) {
    case ECEWO_CLUSTER_ROLE_MASTER:
      ecewo_cluster_wait();
      return 0;

    case ECEWO_CLUSTER_ROLE_WORKER: {
      ecewo_app_t *app = ecewo_create();
      ECEWO_GET(app, "/", handle_index);
      ecewo_listen(app, ecewo_cluster_port());
      return 0;
    }

    case ECEWO_CLUSTER_ROLE_ERROR:
    default:
      return 1;
  }
}
```

## Configuration

The configuration is an opaque builder. Allocate one with
`ecewo_cluster_config_new()`, populate it with the setters, pass it to
`ecewo_cluster_init()`, then free it.

| Setter | Default | Description |
|---|---|---|
| `ecewo_cluster_config_set_port` | required | TCP port shared by all workers |
| `ecewo_cluster_config_set_cpus` | `0` (auto) | Worker count. `0` uses logical CPU count |
| `ecewo_cluster_config_set_respawn` | `false` | Restart crashed workers |
| `ecewo_cluster_config_set_shutdown_timeout` | `15000` | Force-kill timeout (ms) after SIGTERM |
| `ecewo_cluster_config_set_respawn_window` | `5` | Crash-rate window (seconds) |
| `ecewo_cluster_config_set_respawn_max_crashes` | `3` | Max crashes within window before respawn is disabled |
| `ecewo_cluster_config_set_worker_startup_delay` | `100` | Stagger between initial spawns (ms) |
| `ecewo_cluster_config_set_worker_respawn_delay` | `100` | Delay before respawning a crashed worker (ms) |
| `ecewo_cluster_config_set_on_start` | `NULL` | Called in master after each worker spawn |
| `ecewo_cluster_config_set_on_exit` | `NULL` | Called in master after each worker exit |

```c
static void on_worker_start(uint8_t worker_id) {
  printf("Worker %u started\n", worker_id);
}

static void on_worker_exit(uint8_t worker_id, int exit_status, bool is_crash) {
  printf("Worker %u exited (status=%d, crash=%s)\n",
         worker_id, exit_status, is_crash ? "yes" : "no");
}

ecewo_cluster_config_t *cfg = ecewo_cluster_config_new();
ecewo_cluster_config_set_cpus(cfg, ecewo_cluster_cpus_physical()); // physical cores only
ecewo_cluster_config_set_port(cfg, 3000);
ecewo_cluster_config_set_respawn(cfg, true);
ecewo_cluster_config_set_respawn_window(cfg, 10);
ecewo_cluster_config_set_respawn_max_crashes(cfg, 5);
ecewo_cluster_config_set_on_start(cfg, on_worker_start);
ecewo_cluster_config_set_on_exit(cfg, on_worker_exit);
```

## Lifecycle and signals

`ecewo_cluster_init()` returns one of three roles:

| Role | Meaning |
|---|---|
| `ECEWO_CLUSTER_ROLE_MASTER` | Workers have been spawned. Call `ecewo_cluster_wait()` |
| `ECEWO_CLUSTER_ROLE_WORKER` | Run the ecewo app on `ecewo_cluster_port()` |
| `ECEWO_CLUSTER_ROLE_ERROR` | Initialization failed |

The master responds to:

- `SIGTERM` / `SIGINT` - forward `SIGTERM` to every worker, wait
  `shutdown_timeout` ms, then `SIGKILL` any holdouts.
- `SIGUSR2` - rolling restart of every worker. Same as calling
  `ecewo_cluster_graceful_restart()`.

```c
// Trigger a zero-downtime restart from outside:
//   $ kill -USR2 <master_pid>
//
// Or from inside the master:
ecewo_cluster_graceful_restart();
```

You can also forward arbitrary signals to every worker:

```c
ecewo_cluster_signal_workers(SIGUSR1); // e.g. config reload
```

## Statistics

The master can introspect cluster state. Both stats types are opaque; allocate
them with their `_new()` functions, read fields via accessors, then free.

```c
// Cluster-wide stats
ecewo_cluster_stats_t *stats = ecewo_cluster_stats_new();
if (ecewo_cluster_get_stats(stats) == 0) {
  printf("active=%u/%u crashed=%u disabled=%u restarts=%lu\n",
         ecewo_cluster_stats_active_workers(stats),
         ecewo_cluster_stats_total_workers(stats),
         ecewo_cluster_stats_crashed_workers(stats),
         ecewo_cluster_stats_disabled_workers(stats),
         (unsigned long)ecewo_cluster_stats_total_restarts(stats));
}
ecewo_cluster_stats_free(stats);

// Per-worker snapshot
uint8_t n = ecewo_cluster_worker_count();
ecewo_worker_stats_t **workers = malloc(n * sizeof(*workers));
for (uint8_t i = 0; i < n; i++)
  workers[i] = ecewo_worker_stats_new();

int count = ecewo_cluster_get_all_workers(workers, n);
for (int i = 0; i < count; i++) {
  printf("worker %u: pid=%d uptime=%lds\n",
         ecewo_worker_stats_id(workers[i]),
         ecewo_worker_stats_pid(workers[i]),
         (long)ecewo_worker_stats_uptime_seconds(workers[i]));
}

for (uint8_t i = 0; i < n; i++)
  ecewo_worker_stats_free(workers[i]);
free(workers);
```

## API reference

### Configuration

```c
ecewo_cluster_config_t *ecewo_cluster_config_new(void);
void ecewo_cluster_config_free(ecewo_cluster_config_t *config);

void ecewo_cluster_config_set_cpus(ecewo_cluster_config_t *, uint8_t);
void ecewo_cluster_config_set_port(ecewo_cluster_config_t *, uint16_t);
void ecewo_cluster_config_set_respawn(ecewo_cluster_config_t *, bool);
void ecewo_cluster_config_set_shutdown_timeout(ecewo_cluster_config_t *, uint32_t ms);
void ecewo_cluster_config_set_respawn_window(ecewo_cluster_config_t *, uint32_t seconds);
void ecewo_cluster_config_set_respawn_max_crashes(ecewo_cluster_config_t *, uint8_t);
void ecewo_cluster_config_set_worker_startup_delay(ecewo_cluster_config_t *, uint32_t ms);
void ecewo_cluster_config_set_worker_respawn_delay(ecewo_cluster_config_t *, uint32_t ms);
void ecewo_cluster_config_set_on_start(ecewo_cluster_config_t *, ecewo_cluster_on_start_t);
void ecewo_cluster_config_set_on_exit(ecewo_cluster_config_t *, ecewo_cluster_on_exit_t);
```

### Lifecycle

```c
ecewo_cluster_role_t ecewo_cluster_init(const ecewo_cluster_config_t *config,
                                        int argc, char **argv);
void ecewo_cluster_wait(void);                    // master: blocks until all workers exit
void ecewo_cluster_signal_workers(int signum);    // master: forwards signal to workers
void ecewo_cluster_graceful_restart(void);        // master: rolling restart
```

### Introspection

```c
uint16_t ecewo_cluster_port(void);
bool     ecewo_cluster_is_master(void);
bool     ecewo_cluster_is_worker(void);
uint8_t  ecewo_cluster_worker_id(void);
uint8_t  ecewo_cluster_worker_count(void);
uint8_t  ecewo_cluster_cpus(void);
uint8_t  ecewo_cluster_cpus_physical(void);
```

### Callbacks

```c
typedef void (*ecewo_cluster_on_start_t)(uint8_t worker_id);
typedef void (*ecewo_cluster_on_exit_t)(uint8_t worker_id, int exit_status, bool is_crash);
```

### Statistics (master only)

```c
// Cluster stats
ecewo_cluster_stats_t *ecewo_cluster_stats_new(void);
void                   ecewo_cluster_stats_free(ecewo_cluster_stats_t *stats);
int                    ecewo_cluster_get_stats(ecewo_cluster_stats_t *stats);

uint8_t  ecewo_cluster_stats_total_workers(const ecewo_cluster_stats_t *);
uint8_t  ecewo_cluster_stats_active_workers(const ecewo_cluster_stats_t *);
uint8_t  ecewo_cluster_stats_crashed_workers(const ecewo_cluster_stats_t *);
uint8_t  ecewo_cluster_stats_disabled_workers(const ecewo_cluster_stats_t *);
bool     ecewo_cluster_stats_shutdown_requested(const ecewo_cluster_stats_t *);
bool     ecewo_cluster_stats_restart_requested(const ecewo_cluster_stats_t *);
uint64_t ecewo_cluster_stats_total_restarts(const ecewo_cluster_stats_t *);

// Worker stats
ecewo_worker_stats_t *ecewo_worker_stats_new(void);
void                  ecewo_worker_stats_free(ecewo_worker_stats_t *stats);
int                   ecewo_cluster_get_worker_stats(uint8_t worker_id, ecewo_worker_stats_t *stats);
int                   ecewo_cluster_get_all_workers(ecewo_worker_stats_t **stats_array, int array_size);

uint8_t              ecewo_worker_stats_id(const ecewo_worker_stats_t *);
int                  ecewo_worker_stats_pid(const ecewo_worker_stats_t *);
uint16_t             ecewo_worker_stats_port(const ecewo_worker_stats_t *);
ecewo_worker_status_t ecewo_worker_stats_status(const ecewo_worker_stats_t *);
int64_t              ecewo_worker_stats_start_time(const ecewo_worker_stats_t *);
int64_t              ecewo_worker_stats_uptime_seconds(const ecewo_worker_stats_t *);
int                  ecewo_worker_stats_exit_status(const ecewo_worker_stats_t *);
uint8_t              ecewo_worker_stats_crash_count(const ecewo_worker_stats_t *);
bool                 ecewo_worker_stats_respawn_disabled(const ecewo_worker_stats_t *);
```

### Worker status

```c
typedef enum {
  ECEWO_WORKER_STARTING,
  ECEWO_WORKER_ACTIVE,
  ECEWO_WORKER_STOPPING,
  ECEWO_WORKER_CRASHED,
  ECEWO_WORKER_RESPAWNING,
  ECEWO_WORKER_DISABLED
} ecewo_worker_status_t;
```
