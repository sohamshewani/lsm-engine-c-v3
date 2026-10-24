#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>

#define LSM_OK 0
#define LSM_ERR_IO -1
#define LSM_ERR_CORRUPT -2
#define LSM_ERR_NOMEM -3
#define LSM_ERR_NOT_FOUND -4
#define LSM_ERR_INVALID -5

#define LSM_SST_MAGIC 0x4C534D535354544CULL /* "LSMSSTTL" */
#define LSM_BLOOM_FP_RATE 0.01

typedef uint64_t lsn_t;

typedef enum {
    LSM_VAL_PUT = 1,
    LSM_VAL_DELETE = 2
} lsm_value_type_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} lsm_slice_t;

typedef struct {
    uint32_t crc32;
    uint32_t length;
    lsn_t lsn;
    uint8_t type;
    lsm_slice_t key;
    lsm_slice_t value;
} lsm_record_t;

typedef struct {
    uint32_t num_hashes;
    uint32_t bit_count;
    uint8_t *bits;
} lsm_bloom_filter_t;

typedef struct {
    lsm_slice_t key;
    uint64_t block_offset;
    uint32_t block_size;
} lsm_index_entry_t;

typedef struct {
    uint32_t count;
    lsm_index_entry_t *entries;
} lsm_index_block_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t flags;
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t bloom_offset;
    uint64_t bloom_size;
    uint64_t min_lsn;
    uint64_t max_lsn;
    uint64_t key_count;
} lsm_sst_footer_t;

typedef struct {
    int fd;
    uint64_t file_size;
    const uint8_t *mmap_region;
    lsm_sst_footer_t footer;
    lsm_bloom_filter_t bloom;
    lsm_index_block_t index;
    char path[1024];
} lsm_sstable_t;

typedef struct {
    lsm_slice_t key;
    lsm_slice_t value;
    lsn_t lsn;
    lsm_value_type_t type;
} lsm_kv_t;

typedef struct {
    int fd;
    uint64_t current_offset;
    uint32_t block_count;
    uint64_t *block_offsets;
    uint64_t *block_sizes;
    lsm_index_entry_t *index_entries;
    size_t index_capacity;
    size_t index_count;
    lsm_bloom_filter_t bloom;
    size_t total_keys;
} lsm_sst_builder_t;

static uint32_t lsm_crc32(uint32_t crc, const void *buf, size_t size) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (-(int)(crc & 1) & 0xEDB88320U);
        }
    }
    return ~crc;
}

static uint32_t lsm_hash_murmur2(const void *key, int len, uint32_t seed) {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = seed ^ len;
    const unsigned char *data = (const unsigned char *)key;

    while (len >= 4) {
        uint32_t k = *(uint32_t *)data;
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }

    switch (len) {
        case 3: h ^= data[2] << 16;
        case 2: h ^= data[1] << 8;
        case 1: h ^= data[0];
            h *= m;
    };

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

static void lsm_bloom_init(lsm_bloom_filter_t *bloom, size_t expected_keys) {
    if (expected_keys == 0) expected_keys = 1;
    double m_prime = -((double)expected_keys * log(LSM_BLOOM_FP_RATE)) / (log(2.0) * log(2.0));
    uint32_t bit_count = (uint32_t)m_prime;
    if (bit_count < 64) bit_count = 64;
    
    uint32_t num_hashes = (uint32_t)((double)bit_count / (double)expected_keys * log(2.0));
    if (num_hashes < 1) num_hashes = 1;
    if (num_hashes > 30) num_hashes = 30;

    size_t byte_count = (bit_count + 7) / 8;
    bloom->bits = (uint8_t *)calloc(1, byte_count);
    bloom->bit_count = byte_count * 8;
    bloom->num_hashes = num_hashes;
}

static void lsm_bloom_free(lsm_bloom_filter_t *bloom) {
    if (bloom->bits) {
        free(bloom->bits);
        bloom->bits = NULL;
    }
}

static void lsm_bloom_add(lsm_bloom_filter_t *bloom, const void *key, size_t len) {
    if (!bloom->bits) return;
    uint32_t h1 = lsm_hash_murmur2(key, (int)len, 0xbc9f1d34);
    uint32_t h2 = lsm_hash_murmur2(key, (int)len, h1);
    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint32_t combined = h1 + i * h2;
        uint32_t bit_pos = combined % bloom->bit_count;
        bloom->bits[bit_pos / 8] |= (1 << (bit_pos % 8));
    }
}

static bool lsm_bloom_check(const lsm_bloom_filter_t *bloom, const void *key, size_t len) {
    if (!bloom->bits) return true;
    uint32_t h1 = lsm_hash_murmur2(key, (int)len, 0xbc9f1d34);
    uint32_t h2 = lsm_hash_murmur2(key, (int)len, h1);
    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint32_t combined = h1 + i * h2;
        uint32_t bit_pos = combined % bloom->bit_count;
        if (!(bloom->bits[bit_pos / 8] & (1 << (bit_pos % 8)))) {
            return false;
        }
    }
    return true;
}

int lsm_sst_builder_init(lsm_sst_builder_t *builder, const char *path, size_t expected_keys) {
    memset(builder, 0, sizeof(*builder));
    builder->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (builder->fd < 0) {
        return LSM_ERR_IO;
    }
    builder->index_capacity = 1024;
    builder->index_entries = (lsm_index_entry_t *)malloc(builder->index_capacity * sizeof(lsm_index_entry_t));
    if (!builder->index_entries) {
        close(builder->fd);
        return LSM_ERR_NOMEM;
    }
    lsm_bloom_init(&builder->bloom, expected_keys);
    return LSM_OK;
}

int lsm_sst_builder_add(lsm_sst_builder_t *builder, const lsm_kv_t *kv) {
    lsm_bloom_add(&builder->bloom, kv->key.data, kv->key.size);

    uint32_t payload_len = sizeof(lsn_t) + sizeof(uint8_t) + sizeof(uint32_t) + kv->key.size + sizeof(uint32_t) + kv->value.size;
    uint32_t total_len = sizeof(uint32_t) + payload_len;
    
    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf) return LSM_ERR_NOMEM;

    uint8_t *p = buf + sizeof(uint32_t);
    memcpy(p, &kv->lsn, sizeof(lsn_t)); p += sizeof(lsn_t);
    uint8_t type_val = (uint8_t)kv->type;
    *p = type_val; p += sizeof(uint8_t);
    
    uint32_t ksz = (uint32_t)kv->key.size;
    memcpy(p, &ksz, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, kv->key.data, ksz); p += ksz;

    uint32_t vsz = (uint32_t)kv->value.size;
    memcpy(p, &vsz, sizeof(uint32_t)); p += sizeof(uint32_t);
    memcpy(p, kv->value.data, vsz); p += vsz;

    uint32_t crc = lsm_crc32(0xFFFFFFFF, buf + sizeof(uint32_t), payload_len);
    memcpy(buf, &crc, sizeof(uint32_t));

    ssize_t written = write(builder->fd, buf, total_len);
    free(buf);

    if (written != (ssize_t)total_len) {
        return LSM_ERR_IO;
    }

    if (builder->index_count >= builder->index_capacity) {
        builder->index_capacity *= 2;
        lsm_index_entry_t *new_entries = (lsm_index_entry_t *)realloc(builder->index_entries, builder->index_capacity * sizeof(lsm_index_entry_t));
        if (!new_entries) return LSM_ERR_NOMEM;
        builder->index_entries = new_entries;
    }

    lsm_index_entry_t *entry = &builder->index_entries[builder->index_count++];
    entry->key.data = (const uint8_t *)malloc(kv->key.size);
    if (!entry->key.data) return LSM_ERR_NOMEM;
    memcpy((void *)entry->key.data, kv->key.data, kv->key.size);
    entry->key.size = kv->key.size;
    entry->block_offset = builder->current_offset;
    entry->block_size = total_len;

    builder->current_offset += total_len;
    builder->total_keys++;

    return LSM_OK;
}

int lsm_sst_builder_finish(lsm_sst_builder_t *builder, lsn_t min_lsn, lsn_t max_lsn) {
    uint64_t index_offset = builder->current_offset;
    
    uint32_t count = (uint32_t)builder->index_count;
    if (write(builder->fd, &count, sizeof(uint32_t)) != sizeof(uint32_t)) return LSM_ERR_IO;

    for (size_t i = 0; i < builder->index_count; i++) {
        lsm_index_entry_t *entry = &builder->index_entries[i];
        uint32_t ksz = (uint32_t)entry->key.size;
        if (write(builder->fd, &ksz, sizeof(uint32_t)) != sizeof(uint32_t)) return LSM_ERR_IO;
        if (write(builder->fd, entry->key.data, ksz) != (ssize_t)ksz) return LSM_ERR_IO;
        if (write(builder->fd, &entry->block_offset, sizeof(uint64_t)) != sizeof(uint64_t)) return LSM_ERR_IO;
        if (write(builder->fd, &entry->block_size, sizeof(uint32_t)) != sizeof(uint32_t)) return LSM_ERR_IO;
    }
    
    uint64_t index_size = (uint64_t)lseek(builder->fd, 0, SEEK_CUR) - index_offset;

    uint64_t bloom_offset = (uint64_t)lseek(builder->fd, 0, SEEK_CUR);
    uint32_t bit_count = builder->bloom.bit_count;
    uint32_t num_hashes = builder->bloom.num_hashes;
    size_t byte_count = bit_count / 8;

    if (write(builder->fd, &bit_count, sizeof(uint32_t)) != sizeof(uint32_t)) return LSM_ERR_IO;
    if (write(builder->fd, &num_hashes, sizeof(uint32_t)) != sizeof(uint32_t)) return LSM_ERR_IO;
    if (write(builder->fd, builder->bloom.bits, byte_count) != (ssize_t)byte_count) return LSM_ERR_IO;

    uint64_t bloom_size = (uint64_t)lseek(builder->fd, 0, SEEK_CUR) - bloom_offset;

    lsm_sst_footer_t footer;
    footer.magic = LSM_SST_MAGIC;
    footer.version = 1;
    footer.flags = 0;
    footer.index_offset = index_offset;
    footer.index_size = index_size;
    footer.bloom_offset = bloom_offset;
    footer.bloom_size = bloom_size;
    footer.min_lsn = min_lsn;
    footer.max_lsn = max_lsn;
    footer.key_count = builder->total_keys;

    if (write(builder->fd, &footer, sizeof(footer)) != sizeof(footer)) return LSM_ERR_IO;

    if (fsync(builder->fd) < 0) return LSM_ERR_IO;

    for (size_t i = 0; i < builder->index_count; i++) {
        free((void *)builder->index_entries[i].key.data);
    }
    free(builder->index_entries);
    lsm_bloom_free(&builder->bloom);
    close(builder->fd);

    return LSM_OK;
}

void lsm_sst_builder_destroy(lsm_sst_builder_t *builder) {
    if (builder->fd >= 0) {
        close(builder->fd);
        builder->fd = -1;
    }
    for (size_t i = 0; i < builder->index_count; i++) {
        free((void *)builder->index_entries[i].key.data);