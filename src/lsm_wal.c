#include "lsm_wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#define LSM_WAL_MAGIC 0x4C534D5731303030ULL /* "LSMW1000" */

typedef struct lsm_wal_header {
    uint64_t magic;
    uint64_t reserved;
} lsm_wal_header_t;

static uint32_t lsm_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

lsm_status_t lsm_wal_open(const char *filepath, uint64_t initial_lsn, lsm_wal_t **out_wal) {
    if (!filepath || !out_wal) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    lsm_wal_t *wal = (lsm_wal_t *)malloc(sizeof(lsm_wal_t));
    if (!wal) {
        return LSM_ERR_OUT_OF_MEMORY;
    }

    if (pthread_mutex_init(&wal->mutex, NULL) != 0) {
        free(wal);
        return LSM_ERR_IO;
    }

    wal->fd = open(filepath, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (wal->fd < 0) {
        pthread_mutex_destroy(&wal->mutex);
        free(wal);
        return LSM_ERR_IO;
    }

    wal->filepath = strdup(filepath);
    if (!wal->filepath) {
        close(wal->fd);
        pthread_mutex_destroy(&wal->mutex);
        free(wal);
        return LSM_ERR_OUT_OF_MEMORY;
    }

    wal->current_lsn = initial_lsn;
    wal->block_offset = 0;

    struct stat st;
    if (fstat(wal->fd, &st) == 0 && st.st_size == 0) {
        lsm_wal_header_t header;
        header.magic = LSM_WAL_MAGIC;
        header.reserved = 0;
        ssize_t written = write(wal->fd, &header, sizeof(header));
        if (written != (ssize_t)sizeof(header)) {
            free(wal->filepath);
            close(wal->fd);
            pthread_mutex_destroy(&wal->mutex);
            free(wal);
            return LSM_ERR_IO;
        }
        wal->block_offset = sizeof(header);
    } else {
        wal->block_offset = st.st_size % LSM_WAL_BLOCK_SIZE;
    }

    *out_wal = wal;
    return LSM_OK;
}

lsm_status_t lsm_wal_append(lsm_wal_t *wal, uint8_t type, const uint8_t *key, size_t key_len, const uint8_t *value, size_t val_len, uint64_t *out_lsn) {
    if (!wal || !key || key_len == 0 || !out_lsn) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&wal->mutex);

    uint64_t lsn = ++(wal->current_lsn);
    uint32_t payload_len = (uint32_t)(sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) + key_len + val_len);
    
    size_t total_record_size = sizeof(lsm_wal_record_t) + key_len + val_len;
    uint8_t *buffer = (uint8_t *)malloc(total_record_size);
    if (!buffer) {
        pthread_mutex_unlock(&wal->mutex);
        return LSM_ERR_OUT_OF_MEMORY;
    }

    lsm_wal_record_t *record = (lsm_wal_record_t *)buffer;
    record->sequence_number = lsn;
    record->type = type;
    record->key_length = (uint32_t)key_len;
    record->value_length = (uint32_t)val_len;

    uint8_t *ptr = buffer + sizeof(lsm_wal_record_t);
    memcpy(ptr, key, key_len);
    if (val_len > 0 && value) {
        memcpy(ptr + key_len, value, val_len);
    }

    record->crc32 = lsm_crc32(ptr, key_len + val_len);

    size_t remaining_in_block = LSM_WAL_BLOCK_SIZE - (wal->block_offset % LSM_WAL_BLOCK_SIZE);
    if (remaining_in_block < total_record_size && remaining_in_block < LSM_WAL_BLOCK_SIZE) {
        if (remaining_in_block > 0 && remaining_in_block < sizeof(lsm_wal_record_t)) {
            uint8_t *padding = (uint8_t *)calloc(1, remaining_in_block);
            if (padding) {
                (void)write(wal->fd, padding, remaining_in_block);
                free(padding);
            }
        }
        wal->block_offset = 0;
    }

    ssize_t written = write(wal->fd, buffer, total_record_size);
    free(buffer);

    if (written != (ssize_t)total_record_size) {
        pthread_mutex_unlock(&wal->mutex);
        return LSM_ERR_IO;
    }

    wal->block_offset = (wal->block_offset + total_record_size) % LSM_WAL_BLOCK_SIZE;

    *out_lsn = lsn;
    pthread_mutex_unlock(&wal->mutex);
    return LSM_OK;
}

lsm_status_t lsm_wal_sync(lsm_wal_t *wal) {
    if (!wal) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&wal->mutex);
    int res = fsync(wal->fd);
    pthread_mutex_unlock(&wal->mutex);

    return res == 0 ? LSM_OK : LSM_ERR_IO;
}

lsm_status_t lsm_wal_close(lsm_wal_t *wal) {
    if (!wal) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&wal->mutex);
    close(wal->fd);
    if (wal->filepath) {
        free(wal->filepath);
    }
    pthread_mutex_unlock(&wal->mutex);
    pthread_mutex_destroy(&wal->mutex);
    free(wal);

    return LSM_OK;
}

lsm_status_t lsm_wal_recover(const char *filepath, lsm_wal_recovery_cb callback, void *user_data) {
    if (!filepath || !callback) {
        return LSM_ERR_INVALID_ARGUMENT;
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return LSM_OK; 
        }
        return LSM_ERR_IO;
    }

    lsm_wal_header_t header;
    ssize_t bytes_read = read(fd, &header, sizeof(header));
    if (bytes_read != (ssize_t)sizeof(header) || header.magic != LSM_WAL_MAGIC) {
        close(fd);
        return LSM_ERR_CORRUPTION;
    }

    while (1) {
        lsm_wal_record_t record;
        bytes_read = read(fd, &record, sizeof(lsm_wal_record_t));
        if (bytes_read == 0) {
            break; 
        }
        if (bytes_read < (ssize_t)sizeof(lsm_wal_record_t)) {
            close(fd);
            return LSM_ERR_CORRUPTION;
        }

        size_t data_len = (size_t)record.key_length + (size_t)record.value_length;
        uint8_t *data_buf = (uint8_t *)malloc(data_len);
        if (!data_buf) {
            close(fd);
            return LSM_ERR_OUT_OF_MEMORY;
        }

        bytes_read = read(fd, data_buf, data_len);
        if (bytes_read != (ssize_t)data_len) {
            free(data_buf);
            close(fd);
            return LSM_ERR_CORRUPTION;
        }

        uint32_t calculated_crc = lsm_crc32(data_buf, data_len);
        if (calculated_crc != record.crc32) {
            free(data_buf);
            close(fd);
            return LSM_ERR_CORRUPTION;
        }

        uint8_t *key = data_buf;
        uint8_t *value = data_buf + record.key_length;

        lsm_status_t status = callback(record.sequence_number, record.type, key, record.key_length, value, record.value_length, user_data);
        free(data_buf);

        if (status != LSM_OK) {
            close(fd);
            return status;
        }
    }

    close(fd);
    return LSM_OK;
}