#define _GNU_SOURCE
#include "lsm_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

typedef struct lsm_block {
    size_t capacity;
    size_t used;
    struct lsm_block* next;
    char data[];
} lsm_block_t;

struct lsm_arena {
    size_t block_size;
    size_t max_capacity;
    atomic_size_t total_allocated;
    pthread_mutex_t lock;
    lsm_block_t* current;
    lsm_block_t* head;
};

lsm_arena_t* lsm_arena_create(size_t block_size, size_t max_capacity) {
    if (block_size == 0) {
        block_size = 4096;
    }
    
    // Align block_size to system page size (typically 4096)
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        size_t ps = (size_t)page_size;
        block_size = (block_size + ps - 1) & ~(ps - 1);
    }

    lsm_arena_t* arena = (lsm_arena_t*)malloc(sizeof(lsm_arena_t));
    if (!arena) {
        return NULL;
    }

    arena->block_size = block_size;
    arena->max_capacity = max_capacity;
    atomic_init(&arena->total_allocated, 0);
    
    if (pthread_mutex_init(&arena->lock, NULL) != 0) {
        free(arena);
        return NULL;
    }

    size_t initial_alloc_size = sizeof(lsm_block_t) + block_size;
    lsm_block_t* initial_block = (lsm_block_t*)mmap(
        NULL, 
        initial_alloc_size, 
        PROT_READ | PROT_WRITE, 
        MAP_PRIVATE | MAP_ANONYMOUS, 
        -1, 
        0
    );

    if (initial_block == MAP_FAILED) {
        pthread_mutex_destroy(&arena->lock);
        free(arena);
        return NULL;
    }

    initial_block->capacity = block_size;
    initial_block->used = 0;
    initial_block->next = NULL;

    arena->head = initial_block;
    arena->current = initial_block;
    atomic_store(&arena->total_allocated, initial_alloc_size);

    return arena;
}

void* lsm_arena_alloc(lsm_arena_t* arena, size_t size) {
    if (!arena || size == 0) {
        return NULL;
    }

    // Ensure 8-byte alignment for all allocations
    size_t aligned_size = (size + 7) & ~7;

    pthread_mutex_lock(&arena->lock);

    lsm_block_t* curr = arena->current;
    
    // Check if the current block has enough space
    if (curr->used + aligned_size <= curr->capacity) {
        void* ptr = (void*)&curr->data[curr->used];
        curr->used += aligned_size;
        pthread_mutex_unlock(&arena->lock);
        return ptr;
    }

    // Determine required block size for the new allocation
    size_t alloc_payload_size = arena->block_size;
    if (aligned_size > alloc_payload_size) {
        alloc_payload_size = aligned_size;
    }

    size_t current_total = atomic_load(&arena->total_allocated);
    size_t new_block_total_size = sizeof(lsm_block_t) + alloc_payload_size;

    if (arena->max_capacity > 0 && (current_total + new_block_total_size > arena->max_capacity)) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }

    lsm_block_t* new_block = (lsm_block_t*)mmap(
        NULL,
        new_block_total_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (new_block == MAP_FAILED) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }

    new_block->capacity = alloc_payload_size;
    new_block->used = aligned_size;
    new_block->next = NULL;

    // Link the new block
    arena->current->next = new_block;
    arena->current = new_block;

    atomic_fetch_add(&arena->total_allocated, new_block_total_size);

    void* ptr = (void*)&new_block->data[0];
    pthread_mutex_unlock(&arena->lock);
    return ptr;
}

void* lsm_arena_realloc(lsm_arena_t* arena, void* ptr, size_t old_size, size_t new_size) {
    if (!arena) {
        return NULL;
    }
    if (new_size == 0) {
        return NULL;
    }
    if (!ptr) {
        return lsm_arena_alloc(arena, new_size);
    }
    if (new_size <= old_size) {
        return ptr;
    }

    pthread_mutex_lock(&arena->lock);

    // Check if ptr is positioned at the very end of the current block
    lsm_block_t* curr = arena->current;
    size_t aligned_old = (old_size + 7) & ~7;
    size_t aligned_new = (new_size + 7) & ~7;
    size_t diff = aligned_new - aligned_old;

    char* block_start = (char*)&curr->data[0];
    char* block_end = block_start + curr->used;
    char* target_ptr = (char*)ptr;

    // Verify if target_ptr + aligned_old equals block_end
    if (target_ptr + aligned_old == block_end) {
        if (curr->used + diff <= curr->capacity) {
            curr->used += diff;
            pthread_mutex_unlock(&arena->lock);
            return ptr;
        }
    }

    pthread_mutex_unlock(&arena->lock);

    // Fallback: allocate new memory and copy data
    void* new_ptr = lsm_arena_alloc(arena, new_size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    return new_ptr;
}

void lsm_arena_reset(lsm_arena_t* arena) {
    if (!arena) {
        return;
    }

    pthread_mutex_lock(&arena->lock);

    lsm_block_t* curr = arena->head->next;
    // Unmap all blocks except the initial head block to retain baseline memory
    while (curr != NULL) {
        lsm_block_t* next = curr->next;
        size_t block_total_size = sizeof(lsm_block_t) + curr->capacity;
        munmap(curr, block_total_size);
        curr = next;
    }

    arena->head->next = NULL;
    arena->head->used = 0;
    arena->current = arena->head;

    atomic_store(&arena->total_allocated, sizeof(lsm_block_t) + arena->head->capacity);

    pthread_mutex_unlock(&arena->lock);
}

void lsm_arena_destroy(lsm_arena_t* arena) {
    if (!arena) {
        return;
    }

    pthread_mutex_lock(&arena->lock);

    lsm_block_t* curr = arena->head;
    while (curr != NULL) {
        lsm_block_t* next = curr->next;
        size_t block_total_size = sizeof(lsm_block_t) + curr->capacity;
        munmap(curr, block_total_size);
        curr = next;
    }

    pthread_mutex_unlock(&arena->lock);
    pthread_mutex_destroy(&arena->lock);
    free(arena);
}

size_t lsm_arena_used_bytes(const lsm_arena_t* arena) {
    if (!arena) {
        return 0;
    }

    // Cast away const safely for mutex locking
    lsm_arena_t* mutable_arena = (lsm_arena_t*)arena;
    pthread_mutex_lock(&mutable_arena->lock);

    size_t total_used = 0;
    lsm_block_t* curr = arena->head;
    while (curr != NULL) {
        total_used += curr->used;
        curr = curr->next;
    }

    pthread_mutex_unlock(&mutable_arena->lock);
    return total_used;
}

size_t lsm_arena_allocated_bytes(const lsm_arena_t* arena) {
    if (!arena) {
        return 0;
    }
    return atomic_load(&arena->total_allocated);
}