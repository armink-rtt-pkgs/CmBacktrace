/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-12-14     armink       the first version
 * 2026-10-25     wdfk-prog    add optional retained flash fault log replay
 */

#define DBG_TAG "cmb_log"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#include <rtthread.h>
#include <stdio.h>
#include <cm_backtrace.h>

#if defined(CMB_USING_FAL_FLASH_LOG)

#include <fal.h>
#include "cmb_backup_sink.h"

#if defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FLASH)
#error "CMB FAL backup modes must be mutually exclusive"
#endif

#if defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE) && !defined(CMB_BACKUP_SINK_CUSTOM)
#if RT_VER_NUM < 0x40100
#include <dfs_posix.h>
#else
#include <dfs_file.h>
#include <unistd.h>
#endif /* RT_VER_NUM < 0x40100 */
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FILE && !CMB_BACKUP_SINK_CUSTOM */

#ifndef CMB_FAL_FLASH_LOG_PART
#define CMB_FAL_FLASH_LOG_PART         "cmb_log"
#endif

#ifndef CMB_LOG_FILE_PATH
#define CMB_LOG_FILE_PATH              "/log/cmb.log"
#endif

/* cmb flash log partition write granularity, default: 8 bytes */
#ifndef CMB_FLASH_LOG_PART_WG
#define CMB_FLASH_LOG_PART_WG          8
#endif

/* the log length's size which saved in flash */
#define CMB_LOG_LEN_SIZE               MAX(sizeof(size_t), CMB_FLASH_LOG_PART_WG)

#ifndef MIN
#define MIN(a, b) (a < b ? a : b)
#endif
#ifndef MAX
#define MAX(a, b) (a > b ? a : b)
#endif

static const struct fal_partition *cmb_log_part = NULL;

/**
 * @brief Reserve the final program word for the boot-replay marker in FLASH mode.
 * @return Number of bytes available to length-prefixed fault records.
 */
static size_t cmb_flash_log_data_size(void)
{
    if (cmb_log_part == NULL)
        return 0;
#ifdef CMB_USING_FAL_BACKUP_LOG_TO_FLASH
    return cmb_log_part->len >= sizeof(rt_uint32_t) ?
            cmb_log_part->len - sizeof(rt_uint32_t) : 0;
#else
    return cmb_log_part->len;
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FLASH */
}

/**
 * write cmb log to flash partition @see CMB_FLASH_LOG_PART
 *
 * @param log log buffer
 * @param len log length
 */
void cmb_flash_log_write(const char *log, size_t len)
{

    static uint32_t addr = 0;
    static rt_bool_t write_failed = RT_FALSE;
    uint32_t next_addr;

    /* A failed Flash write may leave a partially programmed record. */
    if (cmb_log_part == NULL || write_failed)
        return;
#ifdef CMB_USING_FAL_BACKUP_LOG_TO_FLASH
    /* FLASH mode reserves the last program word for the boot replay marker. */
    size_t limit = cmb_flash_log_data_size();
    if (log == NULL || addr >= limit || CMB_LOG_LEN_SIZE > limit - addr ||
            RT_ALIGN(len, CMB_FLASH_LOG_PART_WG) > limit - addr - CMB_LOG_LEN_SIZE)
        return;
#else
    /* Preserve the original FILE mode write boundary. */
    if(addr >= cmb_log_part->len)
        return;
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FLASH */

    uint8_t len_buf[CMB_LOG_LEN_SIZE] = { 0 };
    static rt_bool_t first_write = RT_TRUE;

    if (first_write)
    {
        if (fal_partition_erase_all(cmb_log_part) != (int)cmb_log_part->len)
            return;
        first_write = RT_FALSE;
    }

    /* write log length */
    rt_memcpy(len_buf, (uint8_t *)&len, sizeof(size_t));
    if (fal_partition_write(cmb_log_part, addr, len_buf, sizeof(len_buf)) != (int)sizeof(len_buf))
    {
        write_failed = RT_TRUE;
        return;
    }
    next_addr = addr + CMB_LOG_LEN_SIZE;
    /* write log content */
    if (fal_partition_write(cmb_log_part, next_addr, (uint8_t *)log, len) != (int)len)
    {
        write_failed = RT_TRUE;
        return;
    }
    addr = next_addr + RT_ALIGN(len, CMB_FLASH_LOG_PART_WG);
}

#if defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE)

static void ulog_cmb_flash_log_backend_output(struct ulog_backend *backend, rt_uint32_t level, const char *tag, rt_bool_t is_raw,
        const char *log, size_t len)
{
    if (!rt_strcmp(tag, CMB_LOG_TAG))
    {
        cmb_flash_log_write(log, len);
    }
}

#else
void cmb_flash_log_println(const char *fmt, ...)
{
    va_list args;
    rt_size_t length;
    static char rt_log_buf[RT_CONSOLEBUF_SIZE];

    va_start(args, fmt);

    length = rt_vsnprintf(rt_log_buf, sizeof(rt_log_buf) - 1, fmt, args);
    if (length > RT_CONSOLEBUF_SIZE - 1 - 2)
        length = RT_CONSOLEBUF_SIZE - 3;

    /* add CRLF */
    rt_log_buf[length++] = '\r';
    rt_log_buf[length++] = '\n';

    cmb_flash_log_write(rt_log_buf, length);

    va_end(args);
}
#endif /* defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE) */

/**
 * @brief Initialize FAL and cache the CMB partition during component init.
 * @return Zero on success, negative on failure. Never erases or writes flash.
 */
static int cmb_flash_log_init(void)
{
#if defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE)
    /* The ULOG backend must outlive this initialization function. */
    static struct ulog_backend backend;
#endif /* defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE) */

    if (fal_init() <= 0)
        return -1;
    cmb_log_part = fal_partition_find(CMB_FAL_FLASH_LOG_PART);
    if (cmb_log_part == NULL)
        return -1;

#if defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE)
    backend.init = RT_NULL;
    backend.output = ulog_cmb_flash_log_backend_output;
    ulog_backend_register(&backend, "cmb_flash_log", RT_FALSE);
#endif /* defined(RT_USING_ULOG) && defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE) */

    return 0;
}
INIT_COMPONENT_EXPORT(cmb_flash_log_init);

/**
 * @brief Read a single length-prefixed log entry, shared by FILE and FLASH.
 * @param addr In/out partition cursor.
 * @param buf Destination for exported text.
 * @param cap Buffer capacity in bytes.
 * @param size Complete record length on success.
 * @return 1 for one record, 0 for end, -1 for corrupt record,
 *         -2 for read error or insufficient buffer capacity.
 */
static int cmb_flash_log_read_one(uint32_t *addr, uint8_t *buf, size_t cap, size_t *size)
{
    size_t len;
    size_t aligned_len;
    size_t limit = cmb_flash_log_data_size();

    if (cmb_log_part == NULL)
        return -2;
    if (*addr >= limit)
        return 0;
    if (sizeof(len) > limit - *addr)
        return -1;
    if (fal_partition_read(cmb_log_part, *addr, (uint8_t *)&len, sizeof(len)) != (int)sizeof(len))
        return -2;
    if (len == (size_t)-1)
        return 0;
    if (len == 0 || len > limit || CMB_LOG_LEN_SIZE > limit - *addr)
        return -1;
    aligned_len = RT_ALIGN(len, CMB_FLASH_LOG_PART_WG);
    if (aligned_len < len || aligned_len > limit - *addr - CMB_LOG_LEN_SIZE)
        return -1;

    /* Reject records that cannot fit, rather than silently exporting partial logs. */
    if (len > cap)
        return -2;
    *size = len;
    if (fal_partition_read(cmb_log_part, *addr + CMB_LOG_LEN_SIZE, buf, *size) != (int)*size)
        return -2;
    *addr += CMB_LOG_LEN_SIZE + aligned_len;
    return 1;
}

#ifdef CMB_USING_FAL_BACKUP_LOG_TO_FILE
int cmb_backup_flash_log_to_file(void)
{
    size_t len;
    int result = 0;
    uint32_t addr = 0;
    rt_bool_t has_read_log = RT_FALSE;
    rt_bool_t corrupt = RT_FALSE;
    cmb_backup_sink_t sink = { -1, RT_NULL };

    if (cmb_log_part == NULL)
        return -1;

    while (1)
    {
        char log_buf[RT_CONSOLEBUF_SIZE];
        int read_result = cmb_flash_log_read_one(&addr, (uint8_t *)log_buf, sizeof(log_buf), &len);
        if (read_result > 0)
        {
            if (!has_read_log)
            {
                LOG_I("An CmBacktrace log was found on flash. Now will backup it to file ("CMB_LOG_FILE_PATH").");
                /*TODO check the folder*/
                if (cmb_backup_sink_ops.open(&sink, CMB_LOG_FILE_PATH) < 0)
                {
                    LOG_E("Open file ("CMB_LOG_FILE_PATH") failed.");
                    result = -1;
                    break;
                }
                has_read_log = RT_TRUE;
            }
            /* backup log to selected file sink */
            if (cmb_backup_sink_ops.write(&sink, log_buf, len) != (int)len)
            {
                result = -1;
                break;
            }
        }
        else
        {
            if (read_result < 0)
            {
                corrupt = (read_result == -1);
                result = -1;
            }
            break;
        }
    }

    if (has_read_log)
    {
        if (cmb_backup_sink_ops.close(&sink) < 0)
            result = -1;
    }
    if (corrupt)
    {
        LOG_E("The partition is corrupted. Erasing it to fix the issue.");
    }
    else if (has_read_log && result == 0)
    {
        LOG_I("Backup the CmBacktrace flash log to file ("CMB_LOG_FILE_PATH") successful.");
    }
    else
    {
        return result;
    }

    return fal_partition_erase_all(cmb_log_part) == (int)cmb_log_part->len ? 0 : -1;
}
#if defined(INIT_FS_EXPORT)
INIT_FS_EXPORT(cmb_backup_flash_log_to_file);
#else
INIT_APP_EXPORT(cmb_backup_flash_log_to_file);
#endif /* INIT_FS_EXPORT */
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FILE */

#ifndef CMB_BACKUP_SINK_CUSTOM
#if defined(CMB_USING_FAL_BACKUP_LOG_TO_FILE)
/**
 * @brief Open the default DFS backup file.
 * @param sink Output context holding the file handle.
 * @param path File path.
 * @return Zero on success, negative on failure.
 */
static int cmb_sink_open(cmb_backup_sink_t *sink, const char *path)
{
    sink->handle = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    return sink->handle < 0 ? -1 : 0;
}
/**
 * @brief Append one text fragment to the DFS backup file.
 * @param sink Output context.
 * @param data Source text.
 * @param size Number of bytes.
 * @return Number of bytes written, or negative on error.
 */
static int cmb_sink_write(cmb_backup_sink_t *sink, const void *data, size_t size)
{
    return write(sink->handle, data, size);
}
/**
 * @brief Close the DFS backup file.
 * @param sink Output context.
 * @return Zero on success, negative on failure.
 */
static int cmb_sink_close(cmb_backup_sink_t *sink)
{
    return close(sink->handle);
}
#else
/**
 * @brief Prepare normal-context console output in FLASH mode.
 * @param sink Unused context.
 * @param path Unused path.
 * @return Zero.
 */
static int cmb_sink_open(cmb_backup_sink_t *sink, const char *path)
{
    (void)sink;
    (void)path;
    return 0;
}
/**
 * @brief Emit a saved fault line directly, without the asynchronous ULOG path.
 * @param sink Unused context.
 * @param data Saved text.
 * @param size Number of bytes.
 * @return Number of bytes accepted for output.
 */
static int cmb_sink_write(cmb_backup_sink_t *sink, const void *data, size_t size)
{
    (void)sink;
    rt_kprintf("%.*s", (int)size, (const char *)data);
    return (int)size;
}
/**
 * @brief Finish normal-context console replay.
 * @param sink Unused context.
 * @return Zero.
 */
static int cmb_sink_close(cmb_backup_sink_t *sink)
{
    (void)sink;
    return 0;
}
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FILE */
/** @brief Selected output sink; CUSTOM mode supplies this object in application code. */
const cmb_backup_sink_ops_t cmb_backup_sink_ops =
{
    cmb_sink_open, cmb_sink_write, cmb_sink_close
};
#endif /* !CMB_BACKUP_SINK_CUSTOM */

#ifdef CMB_USING_FAL_BACKUP_LOG_TO_FLASH
/** @brief Completed once-only boot log replay, stored in the final four bytes. */
#define CMB_FLASH_LOG_BOOT_REPLAYED     0x5A5A5A5AU
/** @brief Erased replay marker: saved fault has not yet been replayed. */
#define CMB_FLASH_LOG_BOOT_UNREAD       0xFFFFFFFFU

/**
 * @brief Export existing FLASH-mode records to the selected sink without erasing.
 * @param has_log Set to RT_TRUE if at least one saved record was exported.
 * @return Zero on successful export or an empty partition, negative on failure.
 */
static int cmb_flash_log_export(rt_bool_t *has_log)
{
    uint32_t addr = 0;
    uint8_t log_buf[RT_CONSOLEBUF_SIZE];
    cmb_backup_sink_t sink = { -1, RT_NULL };
    int result = 0;
    int read_result;
    size_t size;

    *has_log = RT_FALSE;
    while ((read_result = cmb_flash_log_read_one(&addr, log_buf, sizeof(log_buf), &size)) > 0)
    {
        if (!*has_log)
        {
            if (cmb_backup_sink_ops.open(&sink, CMB_LOG_FILE_PATH) < 0)
                return -1;
            *has_log = RT_TRUE;
        }
        if (cmb_backup_sink_ops.write(&sink, log_buf, size) != (int)size)
        {
            result = -1;
            break;
        }
    }
    if (read_result < 0)
        result = -1;
    if (*has_log && cmb_backup_sink_ops.close(&sink) < 0)
        result = -1;
    return result;
}

/**
 * @brief Replay a saved crash once and persist the replay-completed marker.
 * @return Zero on normal/no-log handling, negative on FAL or sink failure.
 */
static int cmb_flash_log_boot_replay(void)
{
    rt_uint32_t marker;
    const rt_uint32_t replayed = CMB_FLASH_LOG_BOOT_REPLAYED;
    rt_bool_t has_log;
    size_t offset;
    int result;

    if (cmb_log_part == NULL)
        return -1;
    offset = cmb_flash_log_data_size();
    if (fal_partition_read(cmb_log_part, offset, (rt_uint8_t *)&marker, sizeof(marker)) != (int)sizeof(marker))
        return -1;
    if (marker == CMB_FLASH_LOG_BOOT_REPLAYED)
    {
        rt_kprintf("Previous CMB fault log retained; use 'cmb_log read' or 'cmb_log clear'.\r\n");
        return 0;
    }
    if (marker != CMB_FLASH_LOG_BOOT_UNREAD)
        return -1;
    result = cmb_flash_log_export(&has_log);
    if (result != 0)
        return result;
    if (!has_log)
        return 0;
    return fal_partition_write(cmb_log_part, offset, (const rt_uint8_t *)&replayed,
                               sizeof(replayed)) == (int)sizeof(replayed) ? 0 : -1;
}
INIT_APP_EXPORT(cmb_flash_log_boot_replay);

/**
 * @brief Read or explicitly erase the retained CMB fault log using MSH.
 * @param argc Number of command arguments.
 * @param argv Command argument vector.
 * @return Zero on success, negative on bad arguments or I/O failure.
 */
static int cmb_log(int argc, char **argv)
{
    rt_bool_t has_log;

    if (cmb_log_part == NULL)
        return -1;
    if (argc == 2 && !rt_strcmp(argv[1], "read"))
        return cmb_flash_log_export(&has_log);
    if (argc == 2 && !rt_strcmp(argv[1], "clear"))
        return fal_partition_erase_all(cmb_log_part) == (int)cmb_log_part->len ? 0 : -1;
    rt_kprintf("Usage: cmb_log read|clear\r\n");
    return -1;
}
MSH_CMD_EXPORT(cmb_log, CmBacktrace retained flash log read/clear);
#endif /* CMB_USING_FAL_BACKUP_LOG_TO_FLASH */

#endif /* CMB_USING_FAL_FLASH_LOG */
