# 🧠 Multi-Container Runtime with Kernel Memory Monitor

## 👥 Team Information

| Name                      | SRN           |
| ------------------------- | ------------- |
| **Chirra Yuktha Praneel** | PES2UG24AM049 |
| **Archita**               | PES2UG24AM024 |

---

## 📌 Overview

This project implements a **lightweight container runtime** with:

* Process isolation using Linux namespaces
* A supervisor for lifecycle management
* Logging and IPC mechanisms
* A **Linux Kernel Module** for memory monitoring with soft and hard limits

---

## 🏗️ System Architecture

```
+------------------------------+
|        User Commands         |
| (engine start / logs / ps)   |
+--------------+---------------+
               |
               v
+------------------------------+
|        Supervisor            |
|  - Manages containers        |
|  - Handles IPC (UNIX socket) |
|  - Tracks metadata           |
+--------------+---------------+
               |
               v
+------------------------------+
|        Containers            |
|  (clone + namespaces)        |
|  - PID Namespace             |
|  - Mount Namespace           |
|  - UTS Namespace             |
|  - chroot FS                 |
+--------------+---------------+
               |
               v
+------------------------------+
|    Kernel Module (monitor)   |
|  - Tracks RSS usage          |
|  - Soft limit → warning      |
|  - Hard limit → SIGKILL      |
+------------------------------+
```

---

## 🔄 Execution Flow (Clone → Monitor → Kill)

```
[ User runs engine start ]
            |
            v
[ Supervisor creates container ]
            |
            v
[ clone() with namespaces ]
            |
            v
[ Process starts (memory_hog / cpu_hog / io_pulse) ]
            |
            v
[ Kernel Timer triggers every 1s ]
            |
            v
[ monitor.c checks RSS ]
            |
     +------+------+
     |             |
     v             v
[ RSS > Soft ]   [ RSS > Hard ]
     |             |
     v             v
 Log warning     Send SIGKILL
                     |
                     v
              Remove from list
```

---

## ⚙️ Build and Setup

### Compile everything

```
make
```

### Load kernel module

```
sudo insmod monitor.ko
```

---

## 🧪 Experiments & Results

### 🔹 1. Memory Limit Enforcement

```
sudo ./engine start gamma ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
```

**Observed:**

* Soft limit triggered (~20MB)
* Hard limit triggered (~40MB)
* Process killed successfully

---

### 🔹 2. Kernel Logs

```
sudo dmesg | tail -n 20
```

**Observed:**

* Module loaded
* Soft limit warning printed
* Hard limit kill executed

---

### 🔹 3. Container Status

```
sudo ./engine ps
```

**Observed:**

* gamma → `hard_limit_killed`
* beta → `exited`
* alpha → `running`

---

### 🔹 4. Memory Growth Behavior

```
sudo ./engine logs alpha
```

**Observed:**

* Memory increases in chunks (8MB)
* Confirms controlled allocation

---

### 🔹 5. CPU vs I/O Scheduling

```
sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice 10
sudo ./engine start io1 ./rootfs-beta /io_pulse --nice -10
```

```
sudo ./engine logs cpu1
sudo ./engine logs io1
```

**Observed:**

* CPU-bound task runs continuously
* IO-bound task executes intermittently
* Lower nice value → higher priority

---

### 🔹 6. Zombie Process Check

```
ps aux | grep defunct
```

**Observed:**

* No zombie processes ✅

---

## 🧠 Design Details

### 🔸 Isolation Mechanisms

* **PID namespace** → process isolation
* **Mount namespace** → filesystem isolation
* **UTS namespace** → hostname isolation
* **chroot** → container filesystem

---

### 🔸 Supervisor Design

* Uses `clone()` for container creation
* Tracks containers via metadata
* Handles termination via signals

---

### 🔸 IPC & Logging

* UNIX domain socket → communication
* Pipes → log streaming
* Bounded buffer → prevents overflow
* Mutex + condition variables → synchronization

---

### 🔸 Kernel Module Logic

* Periodic timer (`1 sec`)
* Fetch RSS using `get_mm_rss()`
* Compare against limits:

  * Soft → log warning
  * Hard → kill process + remove entry

---

## ⚖️ Design Decisions

| Component  | Choice              | Reason           |
| ---------- | ------------------- | ---------------- |
| Isolation  | chroot + namespaces | Lightweight      |
| Monitoring | Timer-based         | Efficient        |
| IPC        | UNIX socket         | Fast             |
| Logging    | Bounded buffer      | Prevent overflow |

---

## 📊 Results Summary

| Feature            | Status |
| ------------------ | ------ |
| Container creation | ✅      |
| Logging system     | ✅      |
| Memory monitoring  | ✅      |
| Soft limit         | ✅      |
| Hard limit         | ✅      |
| Scheduler behavior | ✅      |
| Zombie handling    | ✅      |

---

## 🚀 Conclusion

This project successfully demonstrates:

* A fully functional container runtime
* Kernel-level memory monitoring
* Process scheduling differences
* Robust IPC and logging system

All required objectives were implemented and validated.

---

