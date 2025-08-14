# Kernel-Free Concurrency (KFC)

**Kernel-Free Concurrency (KFC)** is a user-space threading library in Linux designed to provide lightweight thread creation, scheduling, and management without relying on the operating system's kernel threads.  
This project implements core concurrency primitives, context switching, and synchronization mechanisms entirely in user space.

## Overview

The KFC library allows developers to:

- Create, run, and manage lightweight threads (KFC threads)  
- Switch contexts without kernel involvement  
- Implement custom scheduling and concurrency logic  
- Experiment with cooperative and preemptive multitasking  

## My Contribution

This work primarily focuses on **`kfc.c`** and **`kfc.h`**, defining and implementing the core data structures and logic that form the foundation of the threading system.

Key contributions include:

- **Thread Control Block (TCB)** structures for managing thread metadata  
- Functions for:
  - Thread creation and initialization  
  - Context management with `ucontext_t`  
  - Thread state tracking (ready, running, blocked, terminated)  
- Internal data structures for queues and scheduling
  - Created scheduler data type that provides an abstraction, orchastrating which threads are running.

## Key Files

| File         | Purpose |
|--------------|---------|
| `kfc.c`      | Implementation of KFC threading functions — thread creation, scheduling, and context switching logic. |
| `kfc.h`      | Definitions of thread data structures, constants, and function prototypes. |
| `queue.c/h`  | Generic queue implementation for thread scheduling. |
| `kthread.c/h`| Thread abstraction and OS-level interfacing (if needed). |
| `Makefile`   | Build instructions for the library and test programs. |
| `test-*`     | Example programs to validate KFC functionality. |

## Skills Used
- C
- TCP/IP
- Linux
- POSIX
- Bash
- Vim


