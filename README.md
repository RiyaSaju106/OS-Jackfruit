# Supervised Runtime Project

## 1. Team Information

**Name:** Riya Saju
**SRN:** PES1UG24CS378

**Name:** S Chakshu
**SRN:** PES1UG24CS389

---

## 2. Project Overview

This project implements a **supervised container runtime** consisting of:

* **User-space supervisor (`engine.c`)**
* **Kernel module (`monitor.c`)** for memory tracking
* **Shared interface (`monitor_ioctl.h`)**

The system supports:

* Container lifecycle management
* Memory monitoring (soft + hard limits)
* Logging pipeline
* CLI-based interaction
* Scheduling experiments

---

## 3. Build, Load, and Run Instructions

### 🔧 Build

```bash
make
```

---

### 🔌 Load Kernel Module

```bash
sudo insmod monitor.ko
```

---

### 🔍 Verify Device

```bash
ls -l /dev/container_monitor
```

---

### 📁 Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### 🚀 Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh
```

---

### 📊 List Containers

```bash
sudo ./engine ps
```

---

### 📜 View Logs

```bash
sudo ./engine logs alpha
```

---

### 🧪 Run Workloads

Example workload programs:

```bash
./cpu_hog
./memory_hog
```

---

### 🛑 Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

### 🧹 Cleanup

```bash
sudo killall sh
sudo killall sleep
sudo rm containers.txt
sudo rmmod monitor
```

---

## 4. Features Implemented

### ✅ Multi-container Supervision

Multiple containers can run simultaneously with independent root filesystems.

---

### ✅ Metadata Tracking

Container information (ID, PID, state) is tracked and displayed using:

```bash
sudo ./engine ps
```

---

### ✅ Bounded-buffer Logging

Containers continuously log output to files inside their root filesystem.

---

### ✅ CLI and IPC

Commands such as:

* `start`
* `ps`
* `logs`
* `stop`

are handled via CLI interface.

---

### ✅ Soft-limit Warning

When memory threshold is exceeded:

* Warning printed to terminal
* Logged to file

---

### ✅ Hard-limit Enforcement

Containers exceeding hard limit:

* Are terminated using `SIGKILL`
* Logged appropriately

---

### ✅ Scheduling Experiment

CPU scheduling differences demonstrated using:

```bash
./cpu_hog &
nice -n 10 ./cpu_hog &
```

Shows impact of process priority on CPU allocation.

---

### ✅ Clean Teardown

All containers are:

* Properly terminated
* Metadata cleared
* No zombie processes remain

---

## 5. Demo Screenshots

The following demonstrations are included:

1. Multi-container supervision
2. Metadata tracking (`engine ps`)
3. Logging pipeline
4. CLI interaction
5. Soft-limit warning
6. Hard-limit enforcement
7. Scheduling experiment
8. Clean teardown

Each screenshot is annotated with a brief caption.

---

## 6. File Structure

```
.
├── engine.c
├── monitor.c
├── monitor_ioctl.h
├── cpu_hog.c
├── memory_hog.c
├── Makefile
├── README.md
```

---

## 7. Notes

* Root privileges (`sudo`) are required for most operations
* Root filesystem directories (`rootfs-*`) are not committed to the repository
* This implementation focuses on demonstrating core OS concepts:

  * Process management
  * Memory control
  * Scheduling
  * Kernel-user interaction

---

## 8. Conclusion

This project demonstrates a simplified container runtime with supervision, memory monitoring, and scheduling control, providing practical insight into operating system internals.

---
![Uploading image.png…]()




