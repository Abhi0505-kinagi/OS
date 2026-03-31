

---

# Multi-Container Memory Monitor (Linux Kernel Module)

## Overview

`container_monitor` is a Linux kernel module designed to **supervise multiple container processes** and enforce memory usage policies. It monitors the **Resident Set Size (RSS)** of registered processes and issues warnings or kills processes based on soft and hard memory limits.

---

## Features

* Register/unregister processes associated with containers via **ioctl**.
* Periodically monitors memory usage (RSS) of each process.
* Emits **soft-limit warnings** when a process exceeds a soft memory threshold.
* Kills processes exceeding the **hard memory limit**.
* Safe memory management and concurrency using **kernel linked lists and mutexes**.

---

## Data Structures

### `struct monitored_entry`

Each monitored process is tracked using a kernel linked-list node:

| Field              | Description                                                   |
| ------------------ | ------------------------------------------------------------- |
| `pid`              | Process ID to monitor                                         |
| `container_id`     | Unique container identifier (name/ID)                         |
| `soft_limit_bytes` | Memory soft limit in bytes                                    |
| `hard_limit_bytes` | Memory hard limit in bytes                                    |
| `soft_warned`      | Flag indicating whether a soft-limit warning has been emitted |
| `list`             | Kernel `struct list_head` for linked list linkage             |

**Global List and Lock:**

* `monitored_list` — head of monitored entries.
* `monitored_lock` — `mutex` to protect insert, remove, and iteration.

**Justification:**
A **mutex** is used because both ioctl calls (can sleep) and timer callbacks require safe access to shared memory. A spinlock is avoided as sleeping operations occur inside the critical section.

---

## Kernel Timer

* Fires every **1 second** (`CHECK_INTERVAL_SEC`).
* Iterates over all monitored entries:

  * Checks process RSS using `get_rss_bytes()`.
  * Logs soft-limit warnings (once per entry).
  * Kills process if hard limit exceeded.
  * Removes entries if the process exits or is killed.

---

## IOCTL Interface

### Commands

| Command              | Description                                         |
| -------------------- | --------------------------------------------------- |
| `MONITOR_REGISTER`   | Registers a PID with container ID and memory limits |
| `MONITOR_UNREGISTER` | Removes a PID from monitoring                       |

**Request structure:**

```c
struct monitor_request {
    pid_t pid;
    char container_id[64];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};
```

---

## Module Initialization & Exit

* **Init (`monitor_init`)**:

  * Allocates character device `/dev/container_monitor`.
  * Sets up timer.
* **Exit (`monitor_exit`)**:

  * Stops timer.
  * Frees all monitored entries.
  * Cleans up device and class.
* Ensures **no memory leaks** and **safe teardown**.

---

## Usage Example

```bash
# Register a process (pseudo-code)
ioctl(fd, MONITOR_REGISTER, &req);

# Unregister a process
ioctl(fd, MONITOR_UNREGISTER, &req);
```

---

## Implementation Notes

* RSS is obtained safely via `get_task_mm()` and `get_mm_rss()`.
* String safety is ensured using `strncpy`.
* Duplicate registrations are **allowed**; you may add a check to prevent this.
* Concurrency is managed using a **mutex**, suitable for timer and ioctl contexts.

---

## References

* Linux Kernel Linked List: `linux/list.h`
* Kernel Timers: `linux/timer.h`
* Task and Memory APIs: `linux/sched/signal.h`, `linux/mm.h`

---
