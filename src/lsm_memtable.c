#include "lsm_memtable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define LSM_MAX_LEVELS 32
#define LSM_SKIPLIST_P 0.25f

typedef struct lsm_arena_chunk {
    size_t capacity;
    size_t offset;
    struct lsm_arena_chunk* next;
    char data[];
} lsm_arena_chunk_t;

struct lsm_arena {
    size_t chunk_size;
    lsm_arena_chunk_t* head;
    pthread_spinlock_t lock;
};

typedef struct lsm_skiplist_node {
    uint64_t lsn;
    uint32_t type;
    uint32_t key_len;
    uint32_t val_len;
    atomic_int marked_for_removal;
    int height;
    struct lsm_skiplist_node* forward[];
} lsm_skiplist_node_t;

struct lsm_memtable {
    lsm_arena_t* arena;
    lsm_skiplist_node_t* head;
    atomic_size_t current_size;
    size_t capacity_limit;
    int max_height;
    unsigned int rng_state;
    atomic_size_t ref_count;
};

lsm_arena_t* lsm_arena_create(size_t chunk_size) {
    lsm_arena_t* arena = (lsm_arena_t*)malloc(sizeof(lsm_arena_t));
    if (!arena) return NULL;

    arena->chunk_size = chunk_size < 4096 ? 4096 : chunk_size;
    
    lsm_arena_chunk_t* chunk = (lsm_arena_chunk_t*)malloc(sizeof(lsm_arena_chunk_t) + arena->chunk_size);
    if (!chunk) {
        free(arena);
        return NULL;
    }
    chunk->capacity = arena->chunk_size;
    chunk->offset = 0;
    chunk->next = NULL;

    arena->head = chunk;
    pthread_spin_init(&arena->lock, PTHREAD_PROCESS_PRIVATE);
    return arena;
}

void lsm_arena_destroy(lsm_arena_t* arena) {
    if (!arena) return;
    pthread_spin_lock(&arena->lock);
    lsm_arena_chunk_t* curr = arena->head;
    while (curr) {
        lsm_arena_chunk_t* next = curr->next;
        free(curr);
        curr = next;
    }
    pthread_spin_unlock(&arena->lock);
    pthread_spin_destroy(&arena->lock);
    free(arena);
}

void* lsm_arena_alloc(lsm_arena_t* arena, size_t size) {
    if (!arena || size == 0) return NULL;
    
    // Align to 8 bytes
    size_t aligned_size = (size + 7) & ~((size_t)7);

    pthread_spin_lock(&arena->lock);
    lsm_arena_chunk_t* curr = arena->head;
    if (curr->offset + aligned_size > curr->capacity) {
        size_t alloc_size = arena->chunk_size;
        if (aligned_size > alloc_size) {
            alloc_size = aligned_size;
        }
        lsm_arena_chunk_t* new_chunk = (lsm_arena_chunk_t*)malloc(sizeof(lsm_arena_chunk_t) + alloc_size);
        if (!new_chunk) {
            pthread_spin_unlock(&arena->lock);
            return NULL;
        }
        new_chunk->capacity = alloc_size;
        new_chunk->offset = 0;
        new_chunk->next = arena->head;
        arena->head = new_chunk;
        curr = new_chunk;
    }

    void* ptr = (void*)&curr->data[curr->offset];
    curr->offset += aligned_size;
    pthread_spin_unlock(&arena->lock);
    return ptr;
}

static int random_height(lsm_memtable_t* mem) {
    int height = 1;
    while (height < mem->max_height) {
        // Simple xorshift
        mem->rng_state ^= mem->rng_state << 13;
        mem->rng_state ^= mem->rng_state >> 17;
        mem->rng_state ^= mem->rng_state << 5;
        if ((mem->rng_state % 100) >= (int)(LSM_SKIPLIST_P * 100)) {
            break;
        }
        height++;
    }
    return height;
}

lsm_memtable_t* lsm_memtable_create(size_t capacity_limit) {
    lsm_memtable_t* mem = (lsm_memtable_t*)malloc(sizeof(lsm_memtable_t));
    if (!mem) return NULL;

    mem->arena = lsm_arena_create(256 * 1024); // 256KB initial chunks
    if (!mem->arena) {
        free(mem);
        return NULL;
    }

    mem->capacity_limit = capacity_limit;
    mem->max_height = LSM_MAX_LEVELS;
    mem->rng_state = 123456789;
    atomic_init(&mem->current_size, 0);
    atomic_init(&mem->ref_count, 1);

    // Allocate head node with max height
    size_t head_size = sizeof(lsm_skiplist_node_t) + sizeof(lsm_skiplist_node_t*) * mem->max_height;
    mem->head = (lsm_skiplist_node_t*)lsm_arena_alloc(mem->arena, head_size);
    if (!mem->head) {
        lsm_arena_destroy(mem->arena);
        free(mem);
        return NULL;
    }

    mem->head->lsn = 0;
    mem->head->type = 0;
    mem->head->key_len = 0;
    mem->head->val_len = 0;
    atomic_init(&mem->head->marked_for_removal, 0);
    mem->head->height = mem->max_height;
    for (int i = 0; i < mem->max_height; i++) {
        atomic_init((_Atomic(lsm_skiplist_node_t*)*)&mem->head->forward[i], NULL);
    }

    return mem;
}

void lsm_memtable_destroy(lsm_memtable_t* mem) {
    if (!mem) return;
    lsm_arena_destroy(mem->arena);
    free(mem);
}

void lsm_memtable_ref(lsm_memtable_t* mem) {
    if (mem) {
        atomic_fetch_add(&mem->ref_count, 1);
    }
}

void lsm_memtable_unref(lsm_memtable_t* mem) {
    if (mem) {
        if (atomic_fetch_sub(&mem->ref_count, 1) == 1) {
            lsm_memtable_destroy(mem);
        }
    }
}

static int compare_keys(const char* a_key, uint32_t a_len, const char* b_key, uint32_t b_len) {
    uint32_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(a_key, b_key, min_len);
    if (cmp != 0) return cmp;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

int lsm_memtable_put(lsm_memtable_t* mem, uint64_t lsn, const char* key, uint32_t key_len, const char* val, uint32_t val_len) {
    if (!mem || !key || key_len == 0) return LSM_ERR_INVALID_ARGUMENT;

    size_t overhead = sizeof(lsm_skiplist_node_t) + key_len + val_len;
    if (atomic_load(&mem->current_size) + overhead > mem->capacity_limit) {
        return LSM_ERR_MEMTABLE_FULL;
    }

    int height = random_height(mem);
    size_t node_size = sizeof(lsm_skiplist_node_t) + sizeof(lsm_skiplist_node_t*) * height;
    
    // Allocate raw payload space
    char* node_mem = (char*)lsm_arena_alloc(mem->arena, node_size + key_len + val_len);
    if (!node_mem) return LSM_ERR_OUT_OF_MEMORY;

    lsm_skiplist_node_t* new_node = (lsm_skiplist_node_t*)node_mem;
    char* key_storage = node_mem + node_size;
    char* val_storage = key_storage + key_len;

    memcpy(key_storage, key, key_len);
    if (val && val_len > 0) {
        memcpy(val_storage, val, val_len);
    }

    new_node->lsn = lsn;
    new_node->type = 1; // PUT
    new_node->key_len = key_len;
    new_node->val_len = val_len;
    atomic_init(&new_node->marked_for_removal, 0);
    new_node->height = height;
    for (int i = 0; i < height; i++) {
        atomic_init((_Atomic(lsm_skiplist_node_t*)*)&new_node->forward[i], NULL);
    }

    lsm_skiplist_node_t* update[LSM_MAX_LEVELS];
    lsm_skiplist_node_t* preds[LSM_MAX_LEVELS];
    lsm_skiplist_node_t* succs[LSM_MAX_LEVELS];

    while (1) {
        lsm_skiplist_node_t* curr = mem->head;
        for (int i = mem->max_height - 1; i >= 0; i--) {
            lsm_skiplist_node_t* next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[i], memory_order_acquire);
            while (next) {
                int marked = atomic_load_explicit(&next->marked_for_removal, memory_order_acquire);
                if (marked) {
                    // Help unlink marked node
                    lsm_skiplist_node_t* next_next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&next->forward[i], memory_order_acquire);
                    if (atomic_compare_exchange_weak_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[i], &next, next_next, memory_order_release, memory_order_relaxed)) {
                        next = next_next;
                        continue;
                    } else {
                        break;
                    }
                }
                
                int cmp = compare_keys(key_storage, key_len, (char*)next + node_size, next->key_len);
                if (cmp > 0) {
                    curr = next;
                    next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[i], memory_order_acquire);
                } else {
                    break;
                }
            }
            update[i] = curr;
        }

        // Check if key already exists at level 0
        lsm_skiplist_node_t* target = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&update[0]->forward[0], memory_order_acquire);
        if (target) {
            int cmp = compare_keys(key_storage, key_len, (char*)target + node_size, target->key_len);
            if (cmp == 0) {
                // Update existing node inline or append new version via CAS
                // For MPSC skiplist, we allow multiple versions or CAS replacement if exact match
                // We'll link the new node right before target
            }
        }

        for (int i = 0; i < height; i++) {
            while (1) {
                lsm_skiplist_node_t* succ = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&update[i]->forward[i], memory_order_acquire);
                atomic_store_explicit((_Atomic(lsm_skiplist_node_t*)*)&new_node->forward[i], succ, memory_order_relaxed);
                if (atomic_compare_exchange_weak_explicit((_Atomic(lsm_skiplist_node_t*)*)&update[i]->forward[i], &succ, new_node, memory_order_release, memory_order_relaxed)) {
                    break;
                }
                // Retry find predecessor if CAS failed
                // Re-evaluate update[i]
                lsm_skiplist_node_t* curr = mem->head;
                for (int level = mem->max_height - 1; level >= i; level--) {
                    lsm_skiplist_node_t* next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[level], memory_order_acquire);
                    while (next) {
                        int cmp = compare_keys(key_storage, key_len, (char*)next + node_size, next->key_len);
                        if (cmp > 0) {
                            curr = next;
                            next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[level], memory_order_acquire);
                        } else {
                            break;
                        }
                    }
                    update[level] = curr;
                }
            }
        }
        break;
    }

    atomic_fetch_add(&mem->current_size, overhead);
    return LSM_OK;
}

int lsm_memtable_delete(lsm_memtable_t* mem, uint64_t lsn, const char* key, uint32_t key_len) {
    return lsm_memtable_put(mem, lsn, key, key_len, NULL, 0);
}

int lsm_memtable_get(lsm_memtable_t* mem, const char* key, uint32_t key_len, char* val_buf, uint32_t* val_len) {
    if (!mem || !key || key_len == 0) return LSM_ERR_INVALID_ARGUMENT;

    size_t node_size = sizeof(lsm_skiplist_node_t) + sizeof(lsm_skiplist_node_t*) * mem->max_height;
    lsm_skiplist_node_t* curr = mem->head;

    for (int i = mem->max_height - 1; i >= 0; i--) {
        lsm_skiplist_node_t* next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[i], memory_order_acquire);
        while (next) {
            int marked = atomic_load_explicit(&next->marked_for_removal, memory_order_acquire);
            if (marked) {
                next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&next->forward[i], memory_order_acquire);
                continue;
            }
            char* target_key = (char*)next + node_size;
            int cmp = compare_keys(key, key_len, target_key, next->key_len);
            if (cmp > 0) {
                curr = next;
                next = atomic_load_explicit((_Atomic(lsm_skiplist_node_t*)*)&curr->forward[i], memory_order_acquire);
            } else {
                break;
            }
        }
    }

    lsm_skiplist_node_t* target = atomic_load_explicit((_Atomic(l