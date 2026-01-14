# ecewo-cluster

ecewo is single-threaded by default, but can utilize all CPU cores using the cluster plugin for multi-process parallelism.

> [!WARNING]
> The cluster plugin is supported on **Linux only**.

## Table of Contents

1. [Core Concepts](#core-concepts)
2. [Installation](#installation)
3. [Quick Start](#quick-start)
4. [Configuration](#configuration)
5. [Advanced Features](#advanced-features)
6. [API Reference](#api-reference)
7. [Examples](#examples)

## Core Concepts

Cluster plugin provides multi-process clustering support for your application, enabling:

- Process isolation - Workers run in separate processes
- Auto-restart - Crashed workers are automatically respawned
- Load balancing - Distribute load across CPU cores
- Zero-downtime updates - Gracefully restart workers one by one

## Installation

Add to your `CMakeLists.txt`:

```sh
ecewo_plugin(cluster)

target_link_libraries(app PRIVATE
    ecewo::ecewo
    ecewo::cluster
)
```

## Quick Start
```c
#include "ecewo.h"
#include "ecewo-cluster.h"

void handle_index(Req *req, Res *res) {
    char *response = arena_sprintf(req->arena,
        "Hello from worker %u!", cluster_worker_id());
    send_text(res, OK, response);
}

int main(int argc, char *argv[]) {
    uint8_t cpus = cluster_cpus();

    // Configure cluster
    Cluster opts = {
        .cpus = cpus,
        .port = 3000,
        .respawn = true,
    };
    
    // Initialize cluster
    if (cluster_init(&opts, argc, argv)) {
        // MASTER PROCESS - wait for workers
        cluster_wait_workers();
        return 0;
    }
    
    // WORKER PROCESS - start server
    server_init();
    get("/", handle_index);
    server_listen(cluster_get_port());
    server_run();
    return 0;
}
```

## Configuration

### `Cluster` Structure
```c
typedef struct {
    uint8_t cpus;                      // Number of CPUs (0 = auto-detect)
    uint16_t port;                     // Port to bind (required)
    bool respawn;                      // Auto-respawn crashed workers
    
    // Timeouts (milliseconds)
    uint32_t shutdown_timeout_ms;      // Force-kill timeout (default: 15000)
    uint32_t worker_startup_delay_ms;  // Delay between spawns (default: 100)
    uint32_t worker_respawn_delay_ms;  // Delay before respawn (default: 100)
    
    // Crash protection
    uint32_t respawn_window_sec;       // Crash rate window (default: 5)
    uint8_t respawn_max_crashes;       // Max crashes in window (default: 3)
    
    // Callbacks
    void (*on_start)(uint8_t worker_id);
    void (*on_exit)(uint8_t worker_id, int exit_status, bool is_crash);
} Cluster;
```

### CPU Detection
```c
// Get logical cores (includes hyperthreading)
uint8_t logical = cluster_cpus();  // e.g., 8 on 4-core with HT

// Get physical cores only
uint8_t physical = cluster_cpus_physical();  // e.g., 4 on 4-core with HT
```

## Advanced Features

### Monitoring and Callbacks
```c
void on_worker_start(uint8_t worker_id) {
    printf("[%ld] Worker %u started (PID: %d)\n",
           time(NULL), worker_id, getpid());
}

void on_worker_exit(uint8_t worker_id, int exit_status, bool is_crash) {
    if (is_crash) {
        printf("[%ld] Worker %u crashed! (exit: %d)\n",
               time(NULL), worker_id, exit_status);
    } else {
        printf("[%ld] Worker %u exited gracefully (exit: %d)\n",
               time(NULL), worker_id, exit_status);
    }
}

int main(int argc, char *argv[]) {
    Cluster opts = {
        .cpus = cluster_cpus(),
        .port = 3000,
        .respawn = true,
        .on_start = on_worker_start,
        .on_exit = on_worker_exit
    };

    if (cluster_init(&opts, argc, argv)) {
        cluster_wait_workers();
        return 0;
    }
    
    // Worker code...
}
```

### Graceful Restart (Zero-Downtime Updates)
```c
// Method 1: Send SIGUSR2 to master process
// $ kill -USR2 <master_pid>

// Method 2: Call from master process
void on_sigusr1(int sig) {
    cluster_graceful_restart();
}

int main(int argc, char *argv[]) {
    Cluster opts = {
        .cpus = cluster_cpus(),
        .port = 3000,
        .respawn = true,
    };
    
    if (cluster_init(&opts, argc, argv)) {
        // Master: setup custom signal handler
        signal(SIGUSR1, on_sigusr1);
        cluster_wait_workers();
        return 0;
    }
    
    // Worker code...
}

// Usage:
// $ kill -USR1 <master_pid>  # Triggers graceful restart
```

### Worker Statistics
```c
void print_cluster_stats(void) {
    if (!cluster_is_master())
        return;
    
    cluster_stats_t stats;
    if (cluster_get_stats(&stats) != 0) {
        printf("Failed to get stats\n");
        return;
    }
    
    printf("=== Cluster Statistics ===\n");
    printf("Total workers: %u\n", stats.total_workers);
    printf("Active: %u\n", stats.active_workers);
    printf("Crashed: %u\n", stats.crashed_workers);
    printf("Disabled: %u\n", stats.disabled_workers);
    printf("Total restarts: %lu\n", stats.total_restarts);
    printf("Shutdown requested: %s\n", stats.shutdown_requested ? "yes" : "no");
    printf("Restart in progress: %s\n", stats.restart_requested ? "yes" : "no");
    
    // Get individual worker stats
    worker_stats_t worker_stats[stats.total_workers];
    int count = cluster_get_all_workers(worker_stats, stats.total_workers);
    
    printf("\n=== Worker Details ===\n");
    for (int i = 0; i < count; i++) {
        worker_stats_t *w = &worker_stats[i];
        printf("Worker %u: PID=%d, Port=%u, Status=%d, Uptime=%lds, Crashes=%u\n",
               w->worker_id, w->pid, w->port, w->status,
               w->uptime_seconds, w->crash_count);
    }
}
```

### Crash Protection

The cluster plugin automatically disables respawning for workers that crash too frequently:
```c
Cluster opts = {
    .cpus = cluster_cpus(),
    .respawn = true,
    .respawn_window_sec = 5, // 5-second window
    .respawn_max_crashes = 3, // Max 3 crashes
};

// If worker crashes 3 times within 5 seconds:
// [ERROR] Worker 2 crashing too fast (3 times in 4s), disabling respawn
// Worker status becomes WORKER_DISABLED and won't restart
```

### Custom Signals to Workers
```c
// Send custom signal to all workers (master only)
cluster_signal_workers(SIGUSR1);  // Custom reload signal
cluster_signal_workers(SIGTERM);  // Graceful shutdown
cluster_signal_workers(SIGKILL);  // Force kill

// Example: Custom reload handler in worker
void handle_reload(int sig) {
    printf("Worker %u: Reloading configuration...\n", cluster_worker_id());
    // Reload config, clear caches, etc.
}

int main(int argc, char *argv[]) {
    Cluster opts = {
        .cpus = cluster_cpus(),
        .port = 3000,
    };

    if (cluster_init(&opts, argc, argv)) {
        cluster_wait_workers();
        return 0;
    }
    
    // Worker: setup signal handler
    signal(SIGUSR1, handle_reload);
    
    server_init();
    server_listen(cluster_get_port());
    server_run();
    return 0;
}
```

## API Reference

### Initialization

#### `cluster_init()`

Initializes the cluster and spawns workers.
```c
bool cluster_init(const Cluster *options, int argc, char **argv);
```

**Parameters:**
- `options` - Cluster configuration (port required)
- `argc` - Argument count from `main()`
- `argv` - Argument vector from `main()`

**Returns:**
- `true` - Current process is **MASTER** (coordinate workers)
- `false` - Current process is **WORKER** (run server)

**Example:**
```c
int main(int argc, char *argv[]) {
    Cluster opts = {
        .cpus = cluster_cpus(),
        .port = 3000,
        .respawn = true,
    };
    
    if (cluster_init(&opts, argc, argv)) {
        // MASTER: Wait for workers
        cluster_wait_workers();
        return 0;
    }
    
    // WORKER: Start server
    server_init();
    server_listen(cluster_get_port());
    server_run();
    return 0;
}
```

---

### Worker Management

#### `cluster_wait_workers()`

Blocks master process until all workers exit.
```c
void cluster_wait_workers(void);
```

**Note:** Call only from master process after `cluster_init()`

**Features:**
- Handles worker crashes and respawns
- Processes shutdown signals (SIGTERM, SIGINT)
- Enforces shutdown timeout
- Cleans up resources on exit

**Example:**
```c
if (cluster_init(&opts, argc, argv)) {
    cluster_wait_workers();  // Blocks until shutdown
}
```

---

#### `cluster_signal_workers()`

Sends signal to all active workers.
```c
void cluster_signal_workers(int signal);
```

**Parameters:**
- `signal` - Signal number (SIGTERM, SIGUSR1, etc.)

**Note:** Master process only

**Example:**
```c
// Graceful shutdown
cluster_signal_workers(SIGTERM);

// Custom reload signal
cluster_signal_workers(SIGUSR1);
```

---

#### `cluster_graceful_restart()`

Triggers zero-downtime restart of all workers.
```c
void cluster_graceful_restart(void);
```

**Note:** Master process only

**How it works:**
1. Sends SIGTERM to all workers
2. Workers shut down gracefully
3. Master spawns replacement workers
4. Repeat for all workers

**Example:**
```c
// Programmatic restart
cluster_graceful_restart();

// Or via signal:
// $ kill -USR2 <master_pid>
```

---

### Information Functions

#### `cluster_get_port()`

Returns the port assigned to current worker.
```c
uint16_t cluster_get_port(void);
```

**Returns:** Port number, or `0` if not initialized

**Example:**
```c
server_listen(cluster_get_port());
```

---

#### `cluster_is_master()`

Checks if current process is the master.
```c
bool cluster_is_master(void);
```

**Returns:**
- `true` if master process
- `false` otherwise

**Example:**
```c
if (cluster_is_master()) {
    printf("I am the master\n");
}
```

---

#### `cluster_is_worker()`

Checks if current process is a worker.
```c
bool cluster_is_worker(void);
```

**Returns:**
- `true` if worker process
- `false` otherwise

---

#### `cluster_worker_id()`

Returns the ID of the current worker.
```c
uint8_t cluster_worker_id(void);
```

**Returns:** Worker ID (0-254), or `0` for master

**Example:**
```c
if (cluster_is_worker()) {
    printf("I am worker %u\n", cluster_worker_id());
}
```

---

#### `cluster_worker_count()`

Returns total number of configured workers.
```c
uint8_t cluster_worker_count(void);
```

**Returns:** Worker count

---

#### `cluster_cpus()`

Returns number of logical CPU cores (includes hyperthreading).
```c
uint8_t cluster_cpus(void);
```

**Returns:** Logical core count

**Example:**
```c
uint8_t cores = cluster_cpus();  // 8 on 4-core CPU with HT
printf("Logical cores: %u\n", cores);
```

---

#### `cluster_cpus_physical()`

Returns number of physical CPU cores (excludes hyperthreading).
```c
uint8_t cluster_cpus_physical(void);
```

**Returns:** Physical core count

**Example:**
```c
uint8_t cores = cluster_cpus_physical();  // 4 on 4-core CPU with HT
printf("Physical cores: %u\n", cores);
```

---

### Statistics API

#### `cluster_get_stats()`

Gets overall cluster statistics.
```c
int cluster_get_stats(cluster_stats_t *stats);
```

**Parameters:**
- `stats` - Output structure for cluster stats

**Returns:** `1` on success, `0` on error

**Structure:**
```c
typedef struct {
    uint8_t total_workers;
    uint8_t active_workers;
    uint8_t crashed_workers;
    uint8_t disabled_workers;
    bool shutdown_requested;
    bool restart_requested;
    uint64_t total_restarts;
} cluster_stats_t;
```

**Example:**
```c
cluster_stats_t stats;
if (cluster_get_stats(&stats)) {
    printf("Active: %u / %u\n", stats.active_workers, stats.total_workers);
}
```

---

#### `cluster_get_worker_stats()`

Gets statistics for a specific worker.
```c
int cluster_get_worker_stats(uint8_t worker_id, worker_stats_t *stats);
```

**Parameters:**
- `worker_id` - Worker ID to query
- `stats` - Output structure for worker stats

**Returns:** `1` on success, `0` on error

**Structure:**
```c
typedef struct {
    uint8_t worker_id;
    int pid;
    uint16_t port;
    worker_status_t status;  // ACTIVE, CRASHED, DISABLED, etc.
    time_t start_time;
    time_t uptime_seconds;
    int exit_status;
    uint8_t crash_count;
    bool respawn_disabled;
} worker_stats_t;
```

---

#### `cluster_get_all_workers()`

Gets statistics for all workers at once.
```c
int cluster_get_all_workers(worker_stats_t *stats_array, int array_size);
```

**Parameters:**
- `stats_array` - Array to fill with worker stats
- `array_size` - Size of array

**Returns:** Number of workers filled (may be less than array_size)

**Example:**
```c
worker_stats_t workers[16];
int count = cluster_get_all_workers(workers, 16);

for (int i = 0; i < count; i++) {
    printf("Worker %u: PID=%d, Status=%d\n",
           workers[i].worker_id, workers[i].pid, workers[i].status);
}
```

---

### Worker Status Enum
```c
typedef enum {
    WORKER_STARTING,    // Being spawned
    WORKER_ACTIVE,      // Running normally
    WORKER_STOPPING,    // Shutting down
    WORKER_CRASHED,     // Exited unexpectedly
    WORKER_RESPAWNING,  // Being restarted
    WORKER_DISABLED     // Too many crashes, won't restart
} worker_status_t;
```

## Examples

### Production Setup with Monitoring
```c
#include "ecewo.h"
#include "ecewo-cluster.h"
#include <signal.h>
#include <time.h>

void on_worker_start(uint8_t worker_id) {
    time_t now = time(NULL);
    printf("[%s] Worker %u started (PID: %d)\n",
           ctime(&now), worker_id, getpid());
}

void on_worker_exit(uint8_t worker_id, int exit_status, bool is_crash) {
    time_t now = time(NULL);
    
    if (is_crash) {
        printf("[%s] Worker %u CRASHED (exit: %d)\n",
               ctime(&now), worker_id, exit_status);
    } else {
        printf("[%s] Worker %u exited gracefully (exit: %d)\n",
               ctime(&now), worker_id, exit_status);
    }
}

void handle_index(Req *req, Res *res) {
    char *msg = arena_sprintf(req->arena,
        "Worker %u (PID: %d)", cluster_worker_id(), getpid());
    send_text(res, OK, msg);
}

void handle_stats(Req *req, Res *res) {
    // This endpoint works only from master or single worker
    // In real app, use IPC to query master from workers
    
    cluster_stats_t stats;
    if (cluster_get_stats(&stats) != 0) {
        send_text(res, INTERNAL_SERVER_ERROR, "Not master process");
        return;
    }
    
    char *response = arena_sprintf(req->arena,
        "Active: %u/%u | Crashed: %u | Disabled: %u | Restarts: %lu",
        stats.active_workers, stats.total_workers,
        stats.crashed_workers, stats.disabled_workers,
        stats.total_restarts);
    
    send_text(res, OK, response);
}

int main(int argc, char *argv[]) {
    Cluster opts = {
        .cpus = cluster_cpus_physical(),
        .port = 3000,
        .respawn = true,
        .shutdown_timeout_ms = 30000, // 30 second shutdown
        .respawn_window_sec = 10,     // 10 second crash window
        .respawn_max_crashes = 5,     // Max 5 crashes in window
        .on_start = on_worker_start,
        .on_exit = on_worker_exit
    };
    
    if (cluster_init(&opts, argc, argv)) {
        printf("Master process started (PID: %d)\n", getpid());
        printf("Press Ctrl+C to shutdown\n");
        printf("Send SIGUSR2 for graceful restart: kill -USR2 %d\n", getpid());
        
        cluster_wait_workers();
        
        printf("All workers exited. Shutting down.\n");
        return 0;
    }
    
    // Worker process
    server_init();
    
    get("/", handle_index);
    get("/stats", handle_stats);
    
    server_listen(cluster_get_port());
    server_run();
    
    return 0;
}
```
