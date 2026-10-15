/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * System V shared memory, message queues and semaphores shared by all processes of one guest
 * (x86_64 VM PAL with `sys.experimental__multiprocess`), see libos_sysv_shared.c.
 *
 * Interface for the process-local implementations (libos_sysv_shm.c, libos_sysv_msg.c,
 * libos_sem.c): each of their entry points starts with one block
 *
 *     if (sysv_shared_enabled())
 *         return sysv_shared_<call>(<the same arguments>);
 *
 * so that their tables stay untouched when the shared tables are in use. The fork, exec and exit
 * hooks are called from the process code (libos_checkpoint.c, libos_exec.c, libos_exit.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libos_types.h"

struct linux_sembuf;
struct __kernel_timespec;

/* True in the VM PAL with sys.experimental__multiprocess (decided once, at LibOS start). */
bool sysv_shared_enabled(void);
/* The same for semaphores, unless sys.experimental__enable_host_sysv_semaphores forwards them to
 * the host (that option keeps its priority). */
bool sysv_shared_sem_enabled(void);
int init_sysv_shared(void);

long sysv_shared_shmget(int key, size_t size, int shmflg);
long sysv_shared_shmat(int shmid, const void* shmaddr, int shmflg);
long sysv_shared_shmdt(const void* shmaddr);
long sysv_shared_shmctl(int shmid, int cmd, void* buf);
int sysv_shared_shm_proc_load(char** out_data, size_t* out_size);

long sysv_shared_msgget(int key, int msgflg);
long sysv_shared_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg);
long sysv_shared_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);
long sysv_shared_msgctl(int msqid, int cmd, void* buf);
int sysv_shared_msg_proc_load(char** out_data, size_t* out_size);

long sysv_shared_semget(int key, int nsems, int semflg);
long sysv_shared_semctl(int semid, int semnum, int cmd, unsigned long arg);
long sysv_shared_semtimedop(int semid, const struct linux_sembuf* sops, size_t nsops,
                            const struct __kernel_timespec* timeout);

/* Process events: a child created by fork (inherits the parent's attachments), an execve (the
 * attachments go with the old image), and the exit of the process (attachments and SEM_UNDO). */
void sysv_shared_fork(IDTYPE child_pid);
void sysv_shared_exec(void);
void sysv_shared_exit(void);
