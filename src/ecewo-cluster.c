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

#ifndef __linux__
#error "ecewo-cluster is supported on Linux only."
#endif

#include "ecewo-cluster.h"
#include "ecewo.h"
#include <uv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <signal.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#define WORKER_STOP_SIGNAL SIGTERM

#define DEFAULT_SHUTDOWN_TIMEOUT_MS 15000
#define DEFAULT_RESPAWN_WINDOW_SEC 5
#define DEFAULT_RESPAWN_MAX_CRASHES 3
#define DEFAULT_WORKER_STARTUP_DELAY_MS 100
#define DEFAULT_WORKER_RESPAWN_DELAY_MS 100

#define CRASH_HISTORY_MAX 10
#define EXE_PATH_MAX 1024

#ifdef ECEWO_DEBUG
#define LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#define LOG_INFO(fmt, ...) ((void)0)
#endif

#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Configuration (opaque)
// ---------------------------------------------------------------------------

struct ecewo_cluster_config_s {
  uint8_t cpus;
  uint16_t port;
  bool respawn;
  uint32_t shutdown_timeout_ms;
  uint32_t respawn_window_sec;
  uint8_t respawn_max_crashes;
  uint32_t worker_startup_delay_ms;
  uint32_t worker_respawn_delay_ms;
  ecewo_cluster_on_start_t on_start;
  ecewo_cluster_on_exit_t on_exit;
};

struct ecewo_worker_stats_s {
  uint8_t worker_id;
  int pid;
  uint16_t port;
  ecewo_worker_status_t status;
  int64_t start_time;
  int64_t uptime_seconds;
  int exit_status;
  uint8_t crash_count;
  bool respawn_disabled;
};

struct ecewo_cluster_stats_s {
  uint8_t total_workers;
  uint8_t active_workers;
  uint8_t crashed_workers;
  uint8_t disabled_workers;
  bool shutdown_requested;
  bool restart_requested;
  uint64_t total_restarts;
};

ecewo_cluster_config_t *ecewo_cluster_config_new(void) {
  ecewo_cluster_config_t *cfg = calloc(1, sizeof(*cfg));
  if (!cfg)
    return NULL;

  cfg->cpus = 0; // auto-detect
  cfg->port = 0; // user must set
  cfg->respawn = false;
  cfg->shutdown_timeout_ms = DEFAULT_SHUTDOWN_TIMEOUT_MS;
  cfg->respawn_window_sec = DEFAULT_RESPAWN_WINDOW_SEC;
  cfg->respawn_max_crashes = DEFAULT_RESPAWN_MAX_CRASHES;
  cfg->worker_startup_delay_ms = DEFAULT_WORKER_STARTUP_DELAY_MS;
  cfg->worker_respawn_delay_ms = DEFAULT_WORKER_RESPAWN_DELAY_MS;
  cfg->on_start = NULL;
  cfg->on_exit = NULL;
  return cfg;
}

void ecewo_cluster_config_free(ecewo_cluster_config_t *cfg) {
  free(cfg);
}

void ecewo_cluster_config_set_cpus(ecewo_cluster_config_t *cfg, uint8_t cpus) {
  if (cfg) cfg->cpus = cpus;
}
void ecewo_cluster_config_set_port(ecewo_cluster_config_t *cfg, uint16_t port) {
  if (cfg) cfg->port = port;
}
void ecewo_cluster_config_set_respawn(ecewo_cluster_config_t *cfg, bool respawn) {
  if (cfg) cfg->respawn = respawn;
}
void ecewo_cluster_config_set_shutdown_timeout(ecewo_cluster_config_t *cfg, uint32_t ms) {
  if (cfg) cfg->shutdown_timeout_ms = ms;
}
void ecewo_cluster_config_set_respawn_window(ecewo_cluster_config_t *cfg, uint32_t seconds) {
  if (cfg) cfg->respawn_window_sec = seconds;
}
void ecewo_cluster_config_set_respawn_max_crashes(ecewo_cluster_config_t *cfg, uint8_t max_crashes) {
  if (cfg) cfg->respawn_max_crashes = max_crashes;
}
void ecewo_cluster_config_set_worker_startup_delay(ecewo_cluster_config_t *cfg, uint32_t ms) {
  if (cfg) cfg->worker_startup_delay_ms = ms;
}
void ecewo_cluster_config_set_worker_respawn_delay(ecewo_cluster_config_t *cfg, uint32_t ms) {
  if (cfg) cfg->worker_respawn_delay_ms = ms;
}
void ecewo_cluster_config_set_on_start(ecewo_cluster_config_t *cfg, ecewo_cluster_on_start_t cb) {
  if (cfg) cfg->on_start = cb;
}
void ecewo_cluster_config_set_on_exit(ecewo_cluster_config_t *cfg, ecewo_cluster_on_exit_t cb) {
  if (cfg) cfg->on_exit = cb;
}

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
  uv_process_t handle;
  uint8_t worker_id;
  uint16_t port;
  int pid;
  ecewo_worker_status_t status;

  time_t restart_times[CRASH_HISTORY_MAX];
  uint8_t restart_count;
  bool respawn_disabled;

  time_t start_time;
  int exit_status;
} worker_process_t;

typedef struct {
  bool is_master;
  uint8_t worker_id;
  uint8_t worker_count;
  uint16_t base_port;
  uint16_t worker_port;

  worker_process_t *workers;
  ecewo_cluster_config_t config; // resolved (defaults applied)

  uv_signal_t sigterm;
  uv_signal_t sigint;
  uv_signal_t sigusr2;

  int original_argc;
  char **original_argv;
  char exe_path[EXE_PATH_MAX];

  bool shutdown_requested;
  bool graceful_restart_requested;
  uint64_t total_restarts;

  bool initialized;
} cluster_state_t;

static cluster_state_t state = { 0 };

static void on_exit_cb(uv_process_t *handle, int64_t exit_status, int term_signal);
static int spawn_worker(uint8_t worker_id, uint16_t port);

// ---------------------------------------------------------------------------
// argv handling: master keeps a copy of its own argv to relaunch as worker
// ---------------------------------------------------------------------------

static void save_original_args(int argc, char **argv) {
  if (state.original_argv)
    return;

  state.original_argc = argc;
  state.original_argv = calloc((size_t)argc + 1, sizeof(char *));
  if (!state.original_argv) {
    LOG_ERROR("Failed to allocate memory for argv copy");
    return;
  }

  for (int i = 0; i < argc; i++) {
    if (!argv[i])
      continue;
    state.original_argv[i] = strdup(argv[i]);
    if (!state.original_argv[i]) {
      LOG_ERROR("Failed to duplicate argv[%d]", i);
      for (int j = 0; j < i; j++)
        free(state.original_argv[j]);
      free(state.original_argv);
      state.original_argv = NULL;
      return;
    }
  }
  state.original_argv[argc] = NULL;

  size_t size = sizeof(state.exe_path);
  if (uv_exepath(state.exe_path, &size) != 0) {
    LOG_DEBUG("uv_exepath failed; falling back to argv[0]");
    if (argv && argv[0]) {
      strncpy(state.exe_path, argv[0], sizeof(state.exe_path) - 1);
      state.exe_path[sizeof(state.exe_path) - 1] = '\0';
    }
  }
}

static void cleanup_original_args(void) {
  if (!state.original_argv)
    return;
  for (int i = 0; state.original_argv[i]; i++)
    free(state.original_argv[i]);
  free(state.original_argv);
  state.original_argv = NULL;
  state.original_argc = 0;
}

// Build the argv passed to a worker. Allocations live on the borrowed arena
// so the caller only needs to ecewo_arena_return() once we are done spawning.
static char **build_worker_args(ecewo_arena_t *arena, uint8_t worker_id, uint16_t port) {
  if (!state.original_argv || state.original_argc == 0) {
    LOG_ERROR("Original arguments not saved");
    return NULL;
  }

  // Filter out any pre-existing --cluster-worker triplet from argv.
  int filtered_count = 0;
  for (int i = 1; i < state.original_argc; i++) {
    if (strcmp(state.original_argv[i], "--cluster-worker") == 0) {
      i += 2;
      continue;
    }
    filtered_count++;
  }

  char *worker_id_str = ecewo_sprintf(arena, "%u", (unsigned)worker_id);
  char *port_str = ecewo_sprintf(arena, "%u", (unsigned)port);
  if (!worker_id_str || !port_str) {
    LOG_ERROR("Failed to allocate worker arg strings");
    return NULL;
  }

  // Layout: [exe, ...filtered argv (skipping argv[0]), --cluster-worker, id, port, NULL]
  size_t total = (size_t)(1 + filtered_count + 4);
  char **args = ecewo_alloc(arena, total * sizeof(char *));
  if (!args) {
    LOG_ERROR("Failed to allocate worker args");
    return NULL;
  }

  int idx = 0;
  args[idx++] = state.exe_path;

  for (int i = 1; i < state.original_argc; i++) {
    if (strcmp(state.original_argv[i], "--cluster-worker") == 0) {
      i += 2;
      continue;
    }
    args[idx++] = state.original_argv[i];
  }

  args[idx++] = (char *)"--cluster-worker";
  args[idx++] = worker_id_str;
  args[idx++] = port_str;
  args[idx] = NULL;

  return args;
}

// ---------------------------------------------------------------------------
// Config resolution
// ---------------------------------------------------------------------------

static void apply_config(const ecewo_cluster_config_t *user) {
  state.config = *user;

  if (state.config.cpus == 0)
    state.config.cpus = ecewo_cluster_cpus();
  if (state.config.shutdown_timeout_ms == 0)
    state.config.shutdown_timeout_ms = DEFAULT_SHUTDOWN_TIMEOUT_MS;
  if (state.config.respawn_window_sec == 0)
    state.config.respawn_window_sec = DEFAULT_RESPAWN_WINDOW_SEC;
  if (state.config.respawn_max_crashes == 0)
    state.config.respawn_max_crashes = DEFAULT_RESPAWN_MAX_CRASHES;
  if (state.config.worker_startup_delay_ms == 0)
    state.config.worker_startup_delay_ms = DEFAULT_WORKER_STARTUP_DELAY_MS;
  if (state.config.worker_respawn_delay_ms == 0)
    state.config.worker_respawn_delay_ms = DEFAULT_WORKER_RESPAWN_DELAY_MS;

  state.worker_count = state.config.cpus;
  if (state.worker_count < 1) {
    LOG_ERROR("Invalid worker count, using 1");
    state.worker_count = 1;
  }

  uint8_t cpu_count = ecewo_cluster_cpus();
  if (state.worker_count > cpu_count * 2) {
    LOG_INFO("Warning: %" PRIu8 " workers > 2x CPU count (%" PRIu8 ")",
             state.worker_count, cpu_count);
  }
}

// ---------------------------------------------------------------------------
// Crash-rate guard
// ---------------------------------------------------------------------------

static bool should_respawn_worker(worker_process_t *worker) {
  if (!state.config.respawn || worker->respawn_disabled)
    return false;

  time_t now = time(NULL);

  uint8_t max = state.config.respawn_max_crashes;
  if (max > CRASH_HISTORY_MAX)
    max = CRASH_HISTORY_MAX;

  if (worker->restart_count >= max) {
    for (int i = 0; i < max - 1; i++)
      worker->restart_times[i] = worker->restart_times[i + 1];
    worker->restart_count = (uint8_t)(max - 1);
  }

  worker->restart_times[worker->restart_count++] = now;

  if (worker->restart_count >= max) {
    time_t window = now - worker->restart_times[0];
    if (window < (time_t)state.config.respawn_window_sec) {
      LOG_ERROR("Worker %" PRIu8 " crashing too fast (%d times in %lds), disabling respawn",
                worker->worker_id, max, (long)window);
      worker->respawn_disabled = true;
      worker->status = ECEWO_WORKER_DISABLED;
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Spawn / respawn
// ---------------------------------------------------------------------------

static void setup_worker_stdio(uv_process_options_t *options) {
  static uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_INHERIT_FD;
  stdio[1].data.fd = 1;
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = 2;
  options->stdio_count = 3;
  options->stdio = stdio;
}

static int spawn_worker(uint8_t worker_id, uint16_t port) {
  if (worker_id >= state.worker_count) {
    LOG_ERROR("Invalid worker ID: %" PRIu8, worker_id);
    return -1;
  }
  if (!state.original_argv) {
    LOG_ERROR("Original arguments not saved");
    return -1;
  }

  worker_process_t *worker = &state.workers[worker_id];
  uint8_t prev_restart_count = worker->restart_count;
  time_t prev_restart_times[CRASH_HISTORY_MAX];
  memcpy(prev_restart_times, worker->restart_times, sizeof(prev_restart_times));
  bool prev_respawn_disabled = worker->respawn_disabled;

  memset(worker, 0, sizeof(*worker));
  worker->worker_id = worker_id;
  worker->port = port;
  worker->status = ECEWO_WORKER_STARTING;
  worker->respawn_disabled = prev_respawn_disabled;
  worker->restart_count = prev_restart_count;
  memcpy(worker->restart_times, prev_restart_times, sizeof(worker->restart_times));
  worker->start_time = time(NULL);

  // Borrow an arena for short-lived spawn allocations (argv + env). All
  // memory is released by ecewo_arena_return() after uv_spawn returns.
  ecewo_arena_t *arena = ecewo_arena_borrow();
  if (!arena) {
    LOG_ERROR("Failed to borrow arena for worker spawn");
    worker->status = ECEWO_WORKER_DISABLED;
    return -1;
  }

  char **args = build_worker_args(arena, worker_id, port);
  if (!args) {
    ecewo_arena_return(arena);
    worker->status = ECEWO_WORKER_DISABLED;
    return -1;
  }

  uv_process_options_t options = { 0 };
  options.file = state.exe_path;
  options.args = args;
  options.exit_cb = on_exit_cb;
  setup_worker_stdio(&options);

  extern char **environ;
  int env_count = 0;
  while (environ[env_count])
    env_count++;

  char **new_env = ecewo_alloc(arena, (size_t)(env_count + 2) * sizeof(char *));
  if (!new_env) {
    LOG_ERROR("Failed to allocate environment");
    ecewo_arena_return(arena);
    worker->status = ECEWO_WORKER_DISABLED;
    return -1;
  }
  for (int i = 0; i < env_count; i++)
    new_env[i] = environ[i];
  new_env[env_count] = (char *)"ECEWO_WORKER=1";
  new_env[env_count + 1] = NULL;
  options.env = new_env;
  options.flags = UV_PROCESS_DETACHED;

  uv_process_t *handle = &worker->handle;
  handle->data = worker;

  int result = uv_spawn(uv_default_loop(), handle, &options);

  ecewo_arena_return(arena);

  if (result != 0) {
    LOG_ERROR("Failed to spawn worker %" PRIu8 ": %s", worker_id, uv_strerror(result));
    worker->status = ECEWO_WORKER_DISABLED;
    return -1;
  }

  worker->status = ECEWO_WORKER_ACTIVE;
  worker->pid = handle->pid;

  if (state.config.on_start)
    state.config.on_start(worker_id);

  return 0;
}

static void respawn_timer_cb(uv_timer_t *t) {
  worker_process_t *w = (worker_process_t *)t->data;
  if (w && state.is_master) {
    if (spawn_worker(w->worker_id, w->port) != 0)
      LOG_ERROR("Failed to respawn worker %" PRIu8, w->worker_id);
  }
  uv_close((uv_handle_t *)t, (uv_close_cb)free);
}

static void on_exit_cb(uv_process_t *handle, int64_t exit_status, int term_signal) {
  worker_process_t *worker = (worker_process_t *)handle->data;
  if (!worker || !state.is_master)
    return;

  uint8_t worker_id = worker->worker_id;
  time_t uptime = time(NULL) - worker->start_time;

  worker->status = ECEWO_WORKER_CRASHED;
  worker->exit_status = (int)exit_status;
  worker->pid = 0;

  bool is_crash = !state.shutdown_requested && !state.graceful_restart_requested && exit_status != 0;
  if (term_signal == SIGTERM || term_signal == SIGINT)
    is_crash = false;

  if (is_crash) {
    LOG_ERROR("Worker %" PRIu8 " crashed after %lds (exit: %d, signal: %d)",
              worker_id, (long)uptime, (int)exit_status, term_signal);
  } else {
    LOG_DEBUG("Worker %" PRIu8 " exited after %lds (exit: %d)",
              worker_id, (long)uptime, (int)exit_status);
  }

  if (state.config.on_exit)
    state.config.on_exit(worker_id, (int)exit_status, is_crash);

  uv_close((uv_handle_t *)handle, NULL);

  bool should_respawn = false;
  if (state.graceful_restart_requested || (is_crash && should_respawn_worker(worker))) {
    should_respawn = true;
    worker->status = ECEWO_WORKER_RESPAWNING;
  } else {
    worker->status = worker->respawn_disabled ? ECEWO_WORKER_DISABLED : ECEWO_WORKER_CRASHED;
  }

  if (should_respawn) {
    state.total_restarts++;

    uint64_t delay = state.config.worker_respawn_delay_ms;
    uv_timer_t *timer = malloc(sizeof(uv_timer_t));
    if (timer) {
      uv_timer_init(uv_default_loop(), timer);
      timer->data = worker;
      uv_timer_start(timer, respawn_timer_cb, delay, 0);
    } else {
      if (spawn_worker(worker_id, worker->port) != 0)
        LOG_ERROR("Failed to respawn worker %" PRIu8, worker_id);
    }
  }

  if (state.graceful_restart_requested) {
    bool all_active = true;
    for (uint8_t i = 0; i < state.worker_count; i++) {
      if (state.workers[i].status != ECEWO_WORKER_ACTIVE) {
        all_active = false;
        break;
      }
    }
    if (all_active) {
      state.graceful_restart_requested = false;
      LOG_INFO("Graceful restart completed");
    }
  }
}

// ---------------------------------------------------------------------------
// Signal handling (master)
// ---------------------------------------------------------------------------

static void send_stop_to_workers(void) {
  for (uint8_t i = 0; i < state.worker_count; i++) {
    if (state.workers[i].status == ECEWO_WORKER_ACTIVE) {
      state.workers[i].status = ECEWO_WORKER_STOPPING;
      uv_process_kill(&state.workers[i].handle, WORKER_STOP_SIGNAL);
    }
  }
}

static void on_sigterm(uv_signal_t *handle, int signum) {
  (void)handle; (void)signum;
  if (!state.is_master || state.shutdown_requested) return;
  LOG_INFO("Shutdown requested (SIGTERM)");
  state.shutdown_requested = true;
  send_stop_to_workers();
}

static void on_sigint(uv_signal_t *handle, int signum) {
  (void)handle; (void)signum;
  if (!state.is_master || state.shutdown_requested) return;
  LOG_INFO("Shutdown requested (SIGINT)");
  state.shutdown_requested = true;
  send_stop_to_workers();
}

static void on_sigusr2(uv_signal_t *handle, int signum) {
  (void)handle; (void)signum;
  if (!state.is_master) return;
  if (state.graceful_restart_requested || state.shutdown_requested) {
    LOG_INFO("Graceful restart already in progress");
    return;
  }
  LOG_INFO("Graceful restart requested (SIGUSR2)");
  ecewo_cluster_graceful_restart();
}

static void setup_signal_handlers(void) {
  if (!state.is_master) return;

  uv_loop_t *loop = uv_default_loop();
  uv_signal_init(loop, &state.sigterm);
  uv_signal_start(&state.sigterm, on_sigterm, SIGTERM);
  uv_signal_init(loop, &state.sigint);
  uv_signal_start(&state.sigint, on_sigint, SIGINT);
  uv_signal_init(loop, &state.sigusr2);
  uv_signal_start(&state.sigusr2, on_sigusr2, SIGUSR2);
}

static void cleanup_signal_handlers(void) {
  if (!uv_is_closing((uv_handle_t *)&state.sigterm)) {
    uv_signal_stop(&state.sigterm);
    uv_close((uv_handle_t *)&state.sigterm, NULL);
  }
  if (!uv_is_closing((uv_handle_t *)&state.sigint)) {
    uv_signal_stop(&state.sigint);
    uv_close((uv_handle_t *)&state.sigint, NULL);
  }
  if (!uv_is_closing((uv_handle_t *)&state.sigusr2)) {
    uv_signal_stop(&state.sigusr2);
    uv_close((uv_handle_t *)&state.sigusr2, NULL);
  }
}

static void close_handle_cb(uv_handle_t *handle, void *arg) {
  (void)arg;
  if (uv_is_closing(handle)) return;
  if (handle->type == UV_SIGNAL) {
    uv_signal_stop((uv_signal_t *)handle);
  } else if (handle->type == UV_TIMER) {
    uv_timer_stop((uv_timer_t *)handle);
    uv_close(handle, (uv_close_cb)free);
    return;
  }
  uv_close(handle, NULL);
}

static void cluster_cleanup(void) {
  if (!state.initialized) return;

  if (state.workers) {
    free(state.workers);
    state.workers = NULL;
  }

  cleanup_original_args();

  uv_loop_t *loop = uv_default_loop();
  cleanup_signal_handlers();
  uv_walk(loop, close_handle_cb, NULL);

  int iterations = 0;
  while (uv_loop_alive(loop) && iterations < 50) {
    uv_run(loop, UV_RUN_NOWAIT);
    uv_sleep(10);
    iterations++;
  }

  int result = uv_loop_close(loop);
  if (result != 0) {
    uv_walk(loop, close_handle_cb, NULL);
    uv_run(loop, UV_RUN_NOWAIT);
    uv_loop_close(loop);
  }

  state.initialized = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ecewo_cluster_role_t ecewo_cluster_init(const ecewo_cluster_config_t *config, int argc, char **argv) {
  if (state.initialized) {
    LOG_ERROR("Cluster already initialized");
    return ECEWO_CLUSTER_ROLE_ERROR;
  }
  if (!config || config->port == 0 || !argv) {
    LOG_ERROR("Invalid cluster configuration (port required)");
    return ECEWO_CLUSTER_ROLE_ERROR;
  }

  save_original_args(argc, argv);
  apply_config(config);

  state.base_port = config->port;

  char **args = uv_setup_args(argc, argv);

  state.is_master = true;
  state.worker_id = 0;

  // Detect worker mode by scanning argv for --cluster-worker <id> <port>.
  for (int i = 1; args && i < argc - 2; i++) {
    if (strcmp(args[i], "--cluster-worker") == 0) {
      state.is_master = false;

      char *endptr;
      long worker_id_long = strtol(args[i + 1], &endptr, 10);
      long worker_port_long = strtol(args[i + 2], &endptr, 10);

      state.worker_id = (uint8_t)worker_id_long;
      state.worker_port = (uint16_t)worker_port_long;

      char title[64];
      snprintf(title, sizeof(title), "ecewo:worker-%" PRIu8, state.worker_id);
      uv_set_process_title(title);

      state.initialized = true;
      return ECEWO_CLUSTER_ROLE_WORKER;
    }
  }

  // Master.
  static bool cleanup_registered = false;
  if (!cleanup_registered) {
    atexit(cluster_cleanup);
    cleanup_registered = true;
  }

  uv_set_process_title("ecewo:master");
  setup_signal_handlers();

  state.workers = calloc(state.worker_count, sizeof(worker_process_t));
  if (!state.workers) {
    LOG_ERROR("Failed to allocate worker array");
    cleanup_original_args();
    return ECEWO_CLUSTER_ROLE_ERROR;
  }

  int failed = 0;
  for (uint8_t i = 0; i < state.worker_count; i++) {
    if (spawn_worker(i, state.base_port) != 0) {
      LOG_ERROR("Failed to spawn worker %" PRIu8, i);
      failed++;
      if (failed > state.worker_count / 2) {
        LOG_ERROR("Too many spawn failures, aborting");
        cleanup_original_args();
        return ECEWO_CLUSTER_ROLE_ERROR;
      }
    }
    if (i < state.worker_count - 1)
      uv_sleep(state.config.worker_startup_delay_ms);
  }

  uv_sleep(500);

  state.initialized = true;
  printf("Server listening on http://localhost:%" PRIu16 " (Cluster: %d CPUs)\n",
         state.base_port, state.worker_count);

  return ECEWO_CLUSTER_ROLE_MASTER;
}

uint16_t ecewo_cluster_port(void) {
  if (!state.initialized) return 0;
  return state.is_master ? state.base_port : state.worker_port;
}

bool ecewo_cluster_is_master(void) {
  return state.initialized && state.is_master;
}
bool ecewo_cluster_is_worker(void) {
  return state.initialized && !state.is_master;
}
uint8_t ecewo_cluster_worker_id(void) { return state.worker_id; }
uint8_t ecewo_cluster_worker_count(void) { return state.worker_count; }

void ecewo_cluster_signal_workers(int signum) {
  if (!state.is_master || !state.initialized) {
    LOG_ERROR("Only master can signal workers");
    return;
  }
  if (signum <= 0 || signum >= 32) {
    LOG_ERROR("Invalid signal number: %d", signum);
    return;
  }
  for (uint8_t i = 0; i < state.worker_count; i++) {
    if (state.workers[i].status == ECEWO_WORKER_ACTIVE)
      uv_process_kill(&state.workers[i].handle, signum);
  }
}

void ecewo_cluster_graceful_restart(void) {
  if (!state.is_master || !state.initialized) {
    LOG_ERROR("Only master can trigger graceful restart");
    return;
  }
  if (state.graceful_restart_requested || state.shutdown_requested) {
    LOG_INFO("Restart already in progress");
    return;
  }
  LOG_INFO("Starting graceful restart");
  state.graceful_restart_requested = true;
  for (uint8_t i = 0; i < state.worker_count; i++) {
    if (state.workers[i].status == ECEWO_WORKER_ACTIVE)
      uv_process_kill(&state.workers[i].handle, SIGTERM);
  }
}

void ecewo_cluster_wait(void) {
  if (!state.is_master || !state.initialized) {
    LOG_ERROR("Only master can wait for workers");
    return;
  }

  uv_loop_t *loop = uv_default_loop();
  uint64_t shutdown_started_at = 0;

  while (1) {
    bool any_active = false;
    for (uint8_t i = 0; i < state.worker_count; i++) {
      ecewo_worker_status_t s = state.workers[i].status;
      if (s == ECEWO_WORKER_ACTIVE || s == ECEWO_WORKER_STOPPING || s == ECEWO_WORKER_RESPAWNING) {
        any_active = true;
        break;
      }
    }
    if (!any_active) break;

    if (state.shutdown_requested) {
      if (shutdown_started_at == 0)
        shutdown_started_at = uv_now(loop);
      uint64_t elapsed = uv_now(loop) - shutdown_started_at;
      if (elapsed > state.config.shutdown_timeout_ms) {
        LOG_INFO("Shutdown timeout. Force killing remaining workers");
        for (uint8_t i = 0; i < state.worker_count; i++) {
          ecewo_worker_status_t s = state.workers[i].status;
          if (s == ECEWO_WORKER_ACTIVE || s == ECEWO_WORKER_STOPPING)
            uv_process_kill(&state.workers[i].handle, SIGKILL);
        }
        break;
      }
    }

    uv_run(loop, UV_RUN_ONCE);

    if (!uv_loop_alive(loop) && any_active)
      break;
  }

  cluster_cleanup();
}

// ---------------------------------------------------------------------------
// CPU detection
// ---------------------------------------------------------------------------

static long count_physical_cores(void) {
  int max_cpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (max_cpu <= 0 || max_cpu > 1024) return -1;

  bool core_seen[1024] = { 0 };
  int unique_cores = 0;

  for (int cpu = 0; cpu < max_cpu; cpu++) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);

    uv_fs_t open_req;
    int fd = uv_fs_open(NULL, &open_req, path, O_RDONLY, 0, NULL);
    uv_fs_req_cleanup(&open_req);
    if (fd < 0) continue;

    char buf[32];
    uv_buf_t uv_buf = uv_buf_init(buf, sizeof(buf) - 1);

    uv_fs_t read_req;
    int nread = uv_fs_read(NULL, &read_req, fd, &uv_buf, 1, 0, NULL);
    uv_fs_req_cleanup(&read_req);

    if (nread > 0) {
      buf[nread] = '\0';
      char *endptr;
      errno = 0;
      long core_id = strtol(buf, &endptr, 10);
      if (errno == 0 && endptr != buf && *endptr == '\0' &&
          core_id >= 0 && core_id < 1024 && !core_seen[core_id]) {
        core_seen[core_id] = true;
        unique_cores++;
      }
    }

    uv_fs_t close_req;
    uv_fs_close(NULL, &close_req, fd, NULL);
    uv_fs_req_cleanup(&close_req);
  }

  return unique_cores > 0 ? unique_cores : -1;
}

uint8_t ecewo_cluster_cpus_physical(void) {
  long logical_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (logical_cores > 255) logical_cores = 255;
  if (logical_cores < 1) return 1;

  long physical = count_physical_cores();
  if (physical > 0) return (uint8_t)physical;

  // Fallback: detect HT via thread_siblings_list.
  physical = logical_cores;

  uv_fs_t open_req;
  int fd = uv_fs_open(NULL, &open_req,
                      "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list",
                      O_RDONLY, 0, NULL);
  uv_fs_req_cleanup(&open_req);

  if (fd >= 0) {
    char line[256];
    uv_buf_t buf = uv_buf_init(line, sizeof(line) - 1);

    uv_fs_t read_req;
    int nread = uv_fs_read(NULL, &read_req, fd, &buf, 1, 0, NULL);
    uv_fs_req_cleanup(&read_req);

    if (nread > 0) {
      line[nread] = '\0';
      if (line[nread - 1] == '\n') line[nread - 1] = '\0';

      int sibling_count = 1;
      bool has_comma = false;
      bool has_dash = false;
      for (char *p = line; *p; p++) {
        if (*p == ',') { sibling_count++; has_comma = true; }
        else if (*p == '-') { has_dash = true; }
      }

      if (has_dash && !has_comma) {
        char *endptr;
        errno = 0;
        long start_long = strtol(line, &endptr, 10);
        if (errno == 0 && endptr != line && *endptr == '-') {
          errno = 0;
          long end_long = strtol(endptr + 1, &endptr, 10);
          if (errno == 0 && (*endptr == '\0' || *endptr == '\n')) {
            int start = (int)start_long;
            int end = (int)end_long;
            if (start >= 0 && end >= start)
              sibling_count = end - start + 1;
          }
        }
      }

      if (sibling_count > 1)
        physical = logical_cores / sibling_count;
    }

    uv_fs_t close_req;
    uv_fs_close(NULL, &close_req, fd, NULL);
    uv_fs_req_cleanup(&close_req);
  }

  if (physical < 1) physical = 1;
  return (uint8_t)physical;
}

uint8_t ecewo_cluster_cpus(void) {
  unsigned int parallelism = uv_available_parallelism();
  if (parallelism > 255) return 255;
  return (uint8_t)parallelism;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

ecewo_worker_stats_t *ecewo_worker_stats_new(void) {
  return calloc(1, sizeof(ecewo_worker_stats_t));
}

void ecewo_worker_stats_free(ecewo_worker_stats_t *stats) {
  free(stats);
}

int ecewo_cluster_get_worker_stats(uint8_t worker_id, ecewo_worker_stats_t *stats) {
  if (!state.is_master || !state.initialized) return -1;
  if (worker_id >= state.worker_count || !stats) return -1;

  worker_process_t *w = &state.workers[worker_id];
  stats->worker_id = w->worker_id;
  stats->pid = w->pid;
  stats->port = w->port;
  stats->status = w->status;
  stats->start_time = (int64_t)w->start_time;
  stats->uptime_seconds = w->status == ECEWO_WORKER_ACTIVE
                            ? (int64_t)(time(NULL) - w->start_time)
                            : 0;
  stats->exit_status = w->exit_status;
  stats->crash_count = w->restart_count;
  stats->respawn_disabled = w->respawn_disabled;
  return 0;
}

uint8_t ecewo_worker_stats_id(const ecewo_worker_stats_t *s) {
  return s ? s->worker_id : 0;
}
int ecewo_worker_stats_pid(const ecewo_worker_stats_t *s) {
  return s ? s->pid : 0;
}
uint16_t ecewo_worker_stats_port(const ecewo_worker_stats_t *s) {
  return s ? s->port : 0;
}
ecewo_worker_status_t ecewo_worker_stats_status(const ecewo_worker_stats_t *s) {
  return s ? s->status : ECEWO_WORKER_DISABLED;
}
int64_t ecewo_worker_stats_start_time(const ecewo_worker_stats_t *s) {
  return s ? s->start_time : 0;
}
int64_t ecewo_worker_stats_uptime_seconds(const ecewo_worker_stats_t *s) {
  return s ? s->uptime_seconds : 0;
}
int ecewo_worker_stats_exit_status(const ecewo_worker_stats_t *s) {
  return s ? s->exit_status : 0;
}
uint8_t ecewo_worker_stats_crash_count(const ecewo_worker_stats_t *s) {
  return s ? s->crash_count : 0;
}
bool ecewo_worker_stats_respawn_disabled(const ecewo_worker_stats_t *s) {
  return s ? s->respawn_disabled : false;
}

ecewo_cluster_stats_t *ecewo_cluster_stats_new(void) {
  return calloc(1, sizeof(ecewo_cluster_stats_t));
}

void ecewo_cluster_stats_free(ecewo_cluster_stats_t *stats) {
  free(stats);
}

int ecewo_cluster_get_stats(ecewo_cluster_stats_t *stats) {
  if (!state.is_master || !state.initialized) return -1;
  if (!stats) return -1;

  stats->total_workers = state.worker_count;
  stats->active_workers = 0;
  stats->crashed_workers = 0;
  stats->disabled_workers = 0;
  stats->shutdown_requested = state.shutdown_requested;
  stats->restart_requested = state.graceful_restart_requested;
  stats->total_restarts = state.total_restarts;

  for (uint8_t i = 0; i < state.worker_count; i++) {
    switch (state.workers[i].status) {
      case ECEWO_WORKER_ACTIVE:   stats->active_workers++; break;
      case ECEWO_WORKER_CRASHED:  stats->crashed_workers++; break;
      case ECEWO_WORKER_DISABLED: stats->disabled_workers++; break;
      default: break;
    }
  }
  return 0;
}

uint8_t ecewo_cluster_stats_total_workers(const ecewo_cluster_stats_t *s) {
  return s ? s->total_workers : 0;
}
uint8_t ecewo_cluster_stats_active_workers(const ecewo_cluster_stats_t *s) {
  return s ? s->active_workers : 0;
}
uint8_t ecewo_cluster_stats_crashed_workers(const ecewo_cluster_stats_t *s) {
  return s ? s->crashed_workers : 0;
}
uint8_t ecewo_cluster_stats_disabled_workers(const ecewo_cluster_stats_t *s) {
  return s ? s->disabled_workers : 0;
}
bool ecewo_cluster_stats_shutdown_requested(const ecewo_cluster_stats_t *s) {
  return s ? s->shutdown_requested : false;
}
bool ecewo_cluster_stats_restart_requested(const ecewo_cluster_stats_t *s) {
  return s ? s->restart_requested : false;
}
uint64_t ecewo_cluster_stats_total_restarts(const ecewo_cluster_stats_t *s) {
  return s ? s->total_restarts : 0;
}

int ecewo_cluster_get_all_workers(ecewo_worker_stats_t **stats_array, int array_size) {
  if (!state.is_master || !state.initialized) return 0;
  if (!stats_array || array_size <= 0) return 0;

  int count = state.worker_count < array_size ? state.worker_count : array_size;
  int populated = 0;
  for (int i = 0; i < count; i++) {
    if (!stats_array[i]) continue;
    if (ecewo_cluster_get_worker_stats((uint8_t)i, stats_array[i]) == 0)
      populated++;
  }
  return populated;
}
