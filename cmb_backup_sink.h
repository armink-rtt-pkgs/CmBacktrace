/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CMB_BACKUP_SINK_H
#define CMB_BACKUP_SINK_H

#include <stddef.h>
#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Context owned by one post-boot log export. */
typedef struct cmb_backup_sink
{
    int handle;              /**< Output handle; the DFS backend uses a file descriptor. */
    void *userdata;          /**< Optional application-defined output state. */
} cmb_backup_sink_t;

/** @brief Output interface, selected at build time; never called inside HardFault. */
typedef struct cmb_backup_sink_ops
{
    int (*open)(cmb_backup_sink_t *sink, const char *path);               /**< Return zero if ready. */
    int (*write)(cmb_backup_sink_t *sink, const void *data, size_t size); /**< Return bytes accepted. */
    int (*close)(cmb_backup_sink_t *sink);                                /**< Return zero if completed. */
} cmb_backup_sink_ops_t;

/** @brief FILE defaults to DFS; CUSTOM supplies this symbol externally. */
extern const cmb_backup_sink_ops_t cmb_backup_sink_ops;

#ifdef __cplusplus
}
#endif

#endif /* CMB_BACKUP_SINK_H */
