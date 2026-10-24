#ifndef LSM_TYPES_H
#define LSM_TYPES_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * MACROS & CONFIGURATION CONSTANTS
 * ========================================================================= */

#define LSM_MAX_KEY_LEN         65536
#define LSM_MAX_VAL_LEN         (1024 * 1024 * 64)
#define LSM_BLOCK_SIZE          4096
#define LSM_MAGIC               0x4C534D4558504E47ULL /* "LSMEXPNG" */
#define LSM_VERSION             1
#define LSM_MAX_LEVELS          7
#define LSM_SKIPLIST_MAX_HEIGHT 32
#define LSM_SKIPLIST_P          0.25f

/* =========================================================================
 * ERROR CODES
 * ========================================================================= */

typedef enum {
    LSM_OK                  =  0,
    LSM_ERR_IO              = -1,
    LSM_ERR_NOMEM           = -2,
    LSM_ERR_CORRUPT         = -3,
    LSM_ERR_NOT_FOUND       = -4,
    LSM_ERR_KEY_TOO_LARGE   = -5,
    LSM_ERR_VAL_TOO_LARGE   = -6,
    LSM_ERR_INVALID_ARG     = -7,
    LSM_ERR_BUSY            = -8,
    LSM_ERR_FULL            = -9,
    LSM_ERR_DEADLOCK        = -10,
    LSM_ERR_STATE           = -11
} lsm_status_t;

/* =========================================================================
 * RECORD & MUTATION TYPES
 * ========================================================================= */

typedef enum {
    LSM_VAL_PUT     = 0x01,
    LSM_VAL_DELETE  = 0x02
} lsm_val_type_t;

typedef uint64_t lsn_t;
typedef uint64_t lsm_timestamp_t;

typedef struct {
    const uint8_t *data;
    size_t         size;
} lsm_slice_t;

static inline lsm_slice_t lsm_slice_init(const uint8_t *data, size_t size) {
    lsm_slice_t s;
    s.data = data;
    s.size = size;
    return s;
}

static inline lsm_slice_t lsm_slice_empty(void) {
    lsm_slice_t s;
    s.data = NULL;
    s.size = 0;
    return s;
}

/* =========================================================================
 * ARENA ALLOCATOR TYPES
 * ========================================================================= */

typedef struct lsm_arena_block lsm_arena_block_t;

struct lsm_arena_block {
    uint8_t             *memory;
    size_t               capacity;
    size_t               offset;
    lsm_arena_block_t   *next;
};

typedef struct {
    lsm_arena_block_t   *head;
    lsm_arena_block_t   *current;
    size_t               block_size;
    size_t               total_allocated;
    pthread_spinlock_t   lock;
} lsm_arena_t;

/* =========================================================================
 * HAZARD POINTER TYPES
 * ========================================================================= */

#define LSM_MAX_THREADS_DEFAULT 128
#define LSM_HPT_MAX_RETIRED     1024

typedef struct lsm_hp_record lsm_hp_record_t;

struct lsm_hp_record {
    _Atomic(lsm_hp_record_t*) next;
    _Atomic(bool)            active;
    _Atomic(uint64_t)        tid;
    _Atomic(const void*)     hazard[2];
};

typedef struct {
    void    *ptr;
    void    (*deleter)(void*);
} lsm_retired_node_t;

typedef struct {
    lsm_retired_node_t  nodes[LSM_HPT_MAX_RETIRED];
    size_t              count;
} lsm_hazard_local_t;

typedef struct {
    _Atomic(lsm_hp_record_t*) head;
    size_t                    max_threads;
} lsm_hazard_domain_t;

/* =========================================================================
 * SKIP-LIST & MEMTABLE TYPES
 * ========================================================================= */

typedef struct lsm_skip_node lsm_skip_node_t;

struct lsm_skip_node {
    lsm_slice_t           key;
    lsm_slice_t           value;
    lsn_t                 lsn;
    lsm_val_type_t        type;
    uint32_t              height;
    _Atomic(lsm_skip_node_t*) forward[1];
};

typedef struct {
    lsm_skip_node_t      *header;
    lsm_arena_t          *arena;
    _Atomic(uint32_t)     height;
    _Atomic(uint64_t)     count;
    _Atomic(uint64_t)     size_bytes;
} lsm_skiplist_t;

typedef struct {
    lsm_skiplist_t       *skiplist;
    lsm_arena_t          *arena;
    lsn_t                 min_lsn;
    lsn_t                 max_lsn;
    _Atomic(uint64_t)     approx_bytes;
    uint32_t              ref_count;
    bool                  is_immutable;
} lsm_memtable_t;

/* =========================================================================
 * WAL (WRITE-AHEAD LOG) TYPES
 * ========================================================================= */

typedef struct {
    uint32_t              crc32;
    uint32_t              length;
    lsn_t                 lsn;
    uint8_t               type;
    uint8_t               pad[7];
} __attribute__((packed)) lsm_wal_header_t;

typedef struct {
    int                   fd;
    char                 *path;
    lsn_t                 current_lsn;
    uint64_t              file_offset;
    pthread_mutex_t       mutex;
    bool                  sync_on_write;
} lsm_wal_t;

/* =========================================================================
 * SSTABLE & BLOCK TYPES
 * ========================================================================= */

typedef enum {
    LSM_COMPRESSION_NONE   = 0,
    LSM_COMPRESSION_SNAPPY = 1,
    LSM_COMPRESSION_ZSTD   = 2
} lsm_compression_type_t;

typedef struct {
    uint64_t              data_offset;
    uint64_t              data_size;
    uint64_t              meta_index_offset;
    uint64_t              meta_index_size;
    uint64_t              bloom_offset;
    uint64_t              bloom_size;
    uint64_t              sparse_index_offset;
    uint64_t              sparse_index_size;
    uint64_t              min_lsn;
    uint64_t              max_lsn;
    uint32_t              compression;
    uint32_t              num_entries;
} __attribute__((packed)) lsm_sst_footer_t;

typedef struct {
    lsm_slice_t           last_key;
    uint64_t              block_offset;
    uint32_t              block_size;
} __attribute__((packed)) lsm_sparse_index_entry_t;

typedef struct {
    int                   fd;
    char                 *path;
    const uint8_t        *mmap_base;
    size_t                mmap_size;
    lsm_sst_footer_t      footer;
    lsm_sparse_index_entry_t *sparse_index;
    uint32_t              sparse_index_count;
    uint8_t              *bloom_data;
    size_t                bloom_size;
    uint32_t              bloom_hash_funcs;
} lsm_sstable_t;

/* =========================================================================
 * MANIFEST & LEVEL TYPES
 * ========================================================================= */

typedef struct {
    uint64_t              file_id;
    uint32_t              level;
    lsm_slice_t           min_key;
    lsm_slice_t           max_key;
    uint64_t              file_size;
} lsm_manifest_entry_t;

typedef struct {
    lsm_manifest_entry_t *files;
    size_t                count;
    size_t                capacity;
    pthread_rwlock_t      lock;
} lsm_level_meta_t;

typedef struct {
    uint64_t              next_file_id;
    lsn_t                 last_lsn;
    lsm_level_meta_t      levels[LSM_MAX_LEVELS];
    char                 *manifest_path;
    int                   manifest_fd;
    pthread_rwlock_t      lock;
} lsm_manifest_t;

/* =========================================================================
 * THREAD POOL & RING BUFFER TYPES
 * ========================================================================= */

typedef struct {
    void                **buffer;
    size_t                capacity;
    _Atomic(size_t)       head;
    _Atomic(size_t)       tail;
    pthread_mutex_t       lock;
    pthread_cond_t        not_empty;
    pthread_cond_t        not_full;
    bool                  shutdown;
} lsm_ring_buffer_t;

typedef struct lsm_worker_pool lsm_worker_pool_t;

typedef void (*lsm_task_func_t)(void *arg);

typedef struct {
    lsm_task_func_t       func;
    void                 *arg;
} lsm_task_t;

struct lsm_worker_pool {
    pthread_t            *threads;
    size_t                num_threads;
    lsm_ring_buffer_t     queue;
    bool                  stop;
};

/* =========================================================================
 * OPTIONS & ENGINE HANDLE TYPES
 * ========================================================================= */

typedef struct {
    char                 *db_path;
    size_t                arena_block_size;
    size_t                memtable_budget_bytes;
    size_t                max_open_files;
    size_t                block_cache_size;
    size_t                worker_threads;
    bool                  sync_wal;
    lsm_compression_type_t compression;
} lsm_options_t;

typedef struct {
    lsm_options_t         options;
    lsm_manifest_t        manifest;
    lsm_wal_t             wal;
    lsm_memtable_t       *active_memtable;
    lsm_memtable_t       *immutable_memtable;
    pthread_rwlock_t      memtable_lock;
    lsm_hazard_domain_t   hp_domain;
    lsm_worker_pool_t     bg_pool;
    lsm_ring_buffer_t     flush_queue;
    lsm_ring_buffer_t     compaction_queue;
    _Atomic(lsn_t)        global_lsn;
    _Atomic(bool)         running;
    pthread_t             flush_thread;
    pthread_t             compaction_thread;
} lsm_engine_t;

/* =========================================================================
 * ITERATOR TYPES
 * ========================================================================= */

typedef struct {
    lsm_engine_t         *engine;
    lsm_memtable_t       *active_mt;
    lsm_memtable_t       *imm_mt;
    lsm_sstable_t       **sstables;
    size_t                num_sstables;
    lsm_slice_t           current_key;
    lsm_slice_t           current_val;
    lsn_t                 current_lsn;
    lsm_val_type_t        current_type;
    bool                  valid;
    bool                  reversed;
    lsm_status_t          status;
} lsm_iterator_t;

#ifdef __cplusplus
}
#endif

#endif /* LSM_TYPES_H */