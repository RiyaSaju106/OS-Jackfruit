# Supervised Runtime Project

## Team Information

**Name:** Riya Saju
**SRN:** PES1UG24CS378

**Name:** S Chakshu
**SRN:** PES1UG24CS390

---

## Getting Started

### What You Need

Before building, make sure your system has the following:

- Ubuntu 22.04 or 24.04
- `gcc` and `make` installed
- Linux kernel headers for your running kernel

### Building the Project

From the project root, run:

```bash
make
```

### Loading the Kernel Module

Insert the module and verify the device node exists:

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Starting the Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Setting Up Root Filesystems

Create separate filesystem roots for each container:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Launching Containers

Spin up both containers with their respective roots and workloads:

```bash
sudo ./engine start alpha ./rootfs-alpha "/io_pulse"
sudo ./engine start beta  ./rootfs-beta  "/io_pulse"
```

### Day-to-Day Commands

```bash
sudo ./engine ps           # List running containers and their state
sudo ./engine logs alpha   # Stream logs from a specific container
sudo ./engine stop alpha   # Gracefully stop a container
```

### Unloading the Module

```bash
sudo rmmod monitor
```

---

## Feature Demonstrations

**Concurrent Container Execution**
Both containers run simultaneously under a single supervisor, each with its own isolated filesystem and process namespace — confirming that parallel execution is fully supported.

**Live Container Metadata**
`engine ps` surfaces per-container details: PID, current state, memory limits, exit codes, and recent log lines. This validates real-time status tracking.

**Log Capture**
Output from each container is captured over pipes and written into a bounded ring buffer. Logs remain accessible through the CLI or on-disk log files, with no data dropped.

**CLI over UNIX Socket**
The command-line tool communicates with the supervisor through a UNIX domain socket at `/tmp/engine_supervisor.sock`, exchanging commands and responses over that channel.

**Soft Memory Limit Behavior**
Once a container's memory crosses the soft threshold, a warning is emitted to `dmesg` but the process is allowed to keep running — confirming that soft limits are advisory only.

**Hard Memory Limit Enforcement**
Exceeding the hard limit triggers a `SIGKILL`. The container's recorded state updates to reflect the termination, confirming that enforcement is working correctly.

**Scheduling Under Priority**
Containers assigned different nice values exhibit measurably different CPU consumption under contention — higher-priority workloads receive a larger share of CPU time.

**Clean Shutdown**
All containers and supervisor processes exit cleanly, with no zombie processes or stale sockets left behind.

---

## Technical Overview

### Process Isolation

Each container gets its own hostname and process namespace via Linux namespaces, combined with `chroot()` to restrict filesystem visibility.

### Supervisor Responsibilities

The supervisor owns the full container lifecycle — spawning, tracking metadata, and reaping finished processes through `waitpid()` to prevent resource leaks.

### IPC and Logging Stack

| Channel | Purpose |
|---|---|
| UNIX sockets | CLI ↔ supervisor communication |
| Pipes | Container stdout/stderr → log buffer |
| Threads + mutex | Synchronized, race-free log writes |

### Kernel Memory Monitor

The loadable kernel module continuously checks each container's memory footprint and enforces two-tier limits:

- **Soft limit** — logs a warning, execution continues
- **Hard limit** — sends `SIGKILL` and terminates the container

### Scheduling Experiments

Different workload types and nice-value assignments are used to observe and document Linux scheduler behavior across CPU-bound and I/O-bound scenarios.

---

## Design Decisions

| Component | Approach | Notable Tradeoff |
|---|---|---|
| Isolation | Namespaces + chroot | Lightweight, but still shares the host kernel |
| Supervisor | Single central process | Simpler coordination; single point of failure |
| IPC | Socket + pipe combination | Flexible but adds implementation complexity |
| Memory Control | Kernel module | Powerful enforcement; harder to debug |
| Scheduling | Synthetic test workloads | Results may need tuning for real workloads |

---

## Scheduler Observations

**CPU-bound vs CPU-bound**
Nice values only meaningfully affect CPU share when available cores are saturated. On underloaded systems the difference is negligible.

**CPU-bound vs I/O-bound**
I/O-heavy tasks respond quickly to their data arriving, while CPU-heavy tasks steadily consume their allocated core time. The two coexist without starving each other.

**Takeaway**
The Linux scheduler distributes load across available cores and adjusts dynamically based on task priority and workload character.

---

## Demo with Screenshots
### 1. Multi-container Supervision
Shows two containers running under a single supervisor process.

### 2. Metadata Tracking
Displays container metadata using engine ps.

### 3. Bounded-buffer Logging
Shows logs captured through producer-consumer pipeline.

### 4. CLI and IPC
Demonstrates CLI communicating with supervisor via UNIX socket.

### 5. Soft-limit Warning
Kernel logs showing memory soft limit exceeded.

### 6. Hard-limit Enforcement
Kernel kills container when hard limit is exceeded.

### 7. Scheduling Experiment
Comparison of containers with different nice values.

### 8. Clean Teardown
Shows no zombie processes after shutdown.

---

## Summary

This project delivers a working demonstration of core operating system concepts through a purpose-built container runtime:

- Fully isolated multi-container execution
- CLI tooling connected via IPC
- Reliable, bounded logging infrastructure
- Kernel-level memory monitoring with tiered enforcement
- Reproducible scheduling experiments
- Deterministic, leak-free cleanup

Together these components illustrate process lifecycle management, inter-process communication, memory control, and CPU scheduling in a concrete, runnable system.
