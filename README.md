# Supervised Runtime Project

## 1. Team Information

**Name:** Riya Saju
**SRN:** PES1UG24CS378

**Name:** S Chakshu
**SRN:** PES1UG24CS390

---

## 2. Project Overview

This project implements a lightweight Linux container runtime in C, along with a kernel-space memory monitor.

The system supports:

* Running multiple containers concurrently
* Filesystem isolation using `chroot`
* Memory monitoring with soft and hard limits
* Logging of container output
* CLI-based container management
* Scheduling experiments to observe Linux scheduler behavior

The architecture is designed to support a supervisor-based model, where a long-running parent process manages container lifecycle and communication.

---

## 3. Architecture

The runtime consists of:

### User-space Runtime (`engine.c`)

* Handles container creation and lifecycle
* Tracks container metadata (ID, PID, state)
* Provides CLI commands (`start`, `ps`, `logs`, `stop`)
* Simulates supervisor-style control via CLI invocation

### Kernel Module (`monitor.c`)

* Registers container PIDs via `ioctl`
* Tracks memory usage (RSS)
* Enforces:

  * Soft limit → warning
  * Hard limit → process termination

### IPC Design

* Logging: container output redirected to files
* Control: CLI-driven interaction (can be extended to IPC-based supervisor using sockets/FIFO)

---

## 4. Build, Load, and Run Instructions

### Build

```bash
make
```

---

### Load Kernel Module

```bash
sudo insmod monitor.ko
```

---

### Verify Device

```bash
ls -l /dev/container_monitor
```

---

### Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh
```

---

### List Containers

```bash
sudo ./engine ps
```

---

### View Logs

```bash
sudo ./engine logs alpha
```

---

### Run Scheduling Experiment

```bash
./cpu_hog > /dev/null &
nice -n 10 ./cpu_hog > /dev/null &
ps -eo pid,comm,ni,%cpu --sort=-%cpu | grep cpu_hog
```

---

### Stop Containers (Manual Cleanup)

```bash
sudo killall sh
sudo killall sleep
sudo rm containers.txt
```

---

### Verify Teardown

```bash
sudo ./engine ps
ps aux | grep sh
```

---

### Unload Kernel Module

```bash
sudo rmmod monitor
```

---

## 5. Features Implemented

### Multi-container Runtime

Multiple containers (`alpha`, `beta`) run concurrently with separate root filesystems.

---

### Filesystem Isolation

Each container runs inside its own root filesystem using `chroot`.

---

### Metadata Tracking

Container metadata is tracked in user space:

* Container ID
* PID
* State

---

### Logging System

Containers continuously generate logs stored in per-container files.

---

### CLI Interface

Commands implemented:

* `start`
* `ps`
* `logs`

---

### Soft-limit Warning

Warning generated when memory usage crosses threshold.

---

### Hard-limit Enforcement

Container is terminated when hard limit is exceeded.

---

### Scheduling Experiment

Demonstrates CPU scheduling differences using `nice` values.

---

### Clean Teardown

All container processes are terminated and no residual processes remain.

---

## 6. Limitations and Design Notes

* The current implementation uses CLI-triggered control instead of a persistent supervisor daemon.
* IPC between CLI and supervisor is simplified and can be extended using UNIX domain sockets or FIFOs.
* Namespace isolation (PID, UTS) can be further enhanced using `clone()` system calls.
* `/proc` mounting inside containers can be added for full process visibility.

---

## 7. Engineering Analysis

### Isolation Mechanisms

Containers are isolated using `chroot`, which restricts filesystem access to a specific root directory. The host kernel is shared across all containers.

---

### Supervisor and Process Lifecycle

The design supports a supervisor-based model where a parent process manages container lifecycle, tracks metadata, and handles signals.

---

### IPC and Synchronization

Logging is handled via file-based output. The system can be extended to include producer-consumer bounded buffer using threads and synchronization primitives.

---

### Memory Management

RSS (Resident Set Size) is used to measure memory usage. Soft limits provide warnings, while hard limits enforce termination.

---

### Scheduling Behavior

CPU-bound workloads (`cpu_hog`) demonstrate how Linux scheduling allocates CPU time based on process priority (`nice` values).

---

## 8. Design Decisions and Tradeoffs

* **chroot vs pivot_root**
  Chroot was chosen for simplicity, though pivot_root provides stronger isolation.

* **CLI vs Supervisor IPC**
  CLI-based control simplifies implementation but can be extended to IPC-based communication.

* **User-space logging vs threaded buffer**
  Simpler implementation chosen; can be extended to bounded-buffer design.

---

## 9. Scheduler Experiment Results

Two CPU-bound processes were run:

* One with default priority (nice = 0)
* One with lower priority (nice = 10)

Observation:

* Both processes competed for CPU
* Priority influenced scheduling behavior

---

## 10. Conclusion

This project demonstrates key operating system concepts including:

* Process management
* Filesystem isolation
* Memory monitoring
* Scheduling behavior

The implementation provides a functional container runtime with scope for further enhancements such as full supervisor-based control and namespace isolation.

---
