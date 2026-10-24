#include "lsm_compaction.h"
#include "lsm_sstable.h"
#include "lsm_wal.h"
#include "lsm_memtable.h"
#include "lsm_engine.h"
#include "lsm_manifest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#define LSM_MAX_MERGE_WAY 64
#define LSM_COMPACTION_BUFFER_SIZE (64 * 1024)

typedef struct {
    lsm_sstable_reader_t *reader;
    lsm_sstable_iterator_t iter;
    lsm_entry_t current;
    bool valid;
} lsm_compaction_source_t;

typedef struct {
    lsm_compaction_source_t sources[LSM_MAX_MERGE_WAY];
    int count;
} lsm_compaction_merger_t;

static int lsm_compaction_source_compare(const void *a, const void *b) {
    const lsm_compaction_source_t *sa = (const lsm_compaction_source_t *)a;
    const lsm_compaction_source_t *sb = (const lsm_compaction_source_t *)b;
    
    if (!sa->valid) return 1;
    if (!sb->valid) return -1;

    int cmp = lsm_key_compare(&sa->current.key, &sb->current.key);
    if (cmp != 0) {
        return cmp;
    }

    if (sa->current.seq > sb->current.seq) {
        return -1;
    } else if (sa->current.seq < sb->current.seq) {
        return 1;
    }
    return 0;
}

static lsm_status_t lsm_compaction_source_init(lsm_env_t *env, lsm_sstable_reader_t *reader, lsm_compaction_source_t *source) {
    source->reader = reader;
    lsm_status_t status = lsm_sstable_iterator_init(env, reader, &source->iter);
    if (status != LSM_OK) {
        source->valid = false;
        return status;
    }
    source->valid = lsm_sstable_iterator_valid(&source->iter);
    if (source->valid) {
        source->current = lsm_sstable_iterator_key_value(&source->iter);
    }
    return LSM_OK;
}

static void lsm_compaction_source_advance(lsm_compaction_source_t *source) {
    if (!source->valid) return;
    lsm_status_t status = lsm_sstable_iterator_next(&source->iter);
    if (status != LSM_OK || !lsm_sstable_iterator_valid(&source->iter)) {
        source->valid = false;
    } else {
        source->current = lsm_sstable_iterator_key_value(&source->iter);
    }
}

static void lsm_compaction_source_close(lsm_compaction_source_t *source) {
    lsm_sstable_iterator_close(&source->iter);
}

static lsm_status_t lsm_compaction_execute_merge(lsm_env_t *env, lsm_manifest_t *manifest, int level, lsm_sstable_reader_t **inputs, int input_count, lsm_sstable_writer_t *writer, uint64_t *out_new_file_number) {
    lsm_compaction_merger_t merger;
    merger.count = 0;

    for (int i = 0; i < input_count && i < LSM_MAX_MERGE_WAY; i++) {
        lsm_status_t status = lsm_compaction_source_init(env, inputs[i], &merger.sources[merger.count]);
        if (status != LSM_OK) {
            for (int j = 0; j < merger.count; j++) {
                lsm_compaction_source_close(&merger.sources[j]);
            }
            return status;
        }
        if (merger.sources[merger.count].valid) {
            merger.count++;
        } else {
            lsm_compaction_source_close(&merger.sources[merger.count]);
        }
    }

    lsm_slice_t last_key;
    memset(&last_key, 0, sizeof(last_key));
    bool has_last_key = false;
    uint64_t current_seq = 0;

    while (merger.count > 0) {
        qsort(merger.sources, merger.count, sizeof(lsm_compaction_source_t), lsm_compaction_source_compare);

        lsm_compaction_source_t *top = &merger.sources[0];
        
        bool skip = false;
        if (has_last_key) {
            if (lsm_key_compare(&top->current.key, &last_key) == 0) {
                skip = true;
            }
        }

        if (!skip) {
            if (level + 1 < LSM_MAX_LEVELS || top->current.type != LSM_ENTRY_DELETE) {
                lsm_status_t status = lsm_sstable_writer_append(writer, &top->current.key, &top->current.value, top->current.seq, top->current.type);
                if (status != LSM_OK) {
                    for (int i = 0; i < merger.count; i++) {
                        lsm_compaction_source_close(&merger.sources[i]);
                    }
                    return status;
                }
                
                if (has_last_key) {
                    free(last_key.data);
                }
                last_key.data = malloc(top->current.key.size);
                if (!last_key.data) {
                    for (int i = 0; i < merger.count; i++) {
                        lsm_compaction_source_close(&merger.sources[i]);
                    }
                    return LSM_ERR_NOMEM;
                }
                memcpy(last_key.data, top->current.key.data, top->current.key.size);
                last_key.size = top->current.key.size;
                has_last_key = true;
            }
        }

        lsm_compaction_source_advance(top);
        if (!top->valid) {
            lsm_compaction_source_close(top);
            merger.sources[0] = merger.sources[merger.count - 1];
            merger.count--;
        }
    }

    if (has_last_key) {
        free(last_key.data);
    }

    return LSM_OK;
}

lsm_status_t lsm_compaction_run_manual(lsm_engine_t *engine, int level) {
    if (!engine) return LSM_ERR_INVALID_ARG;

    pthread_mutex_lock(&engine->compaction_mutex);

    lsm_manifest_t *manifest = engine->manifest;
    if (!manifest) {
        pthread_mutex_unlock(&engine->compaction_mutex);
        return LSM_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&manifest->mutex);
    int sst_count = manifest->levels[level].count;
    if (sst_count <= 0) {
        pthread_mutex_unlock(&manifest->mutex);
        pthread_mutex_unlock(&engine->compaction_mutex);
        return LSM_OK;
    }

    lsm_sstable_reader_t **readers = malloc(sizeof(lsm_sstable_reader_t *) * sst_count);
    if (!readers) {
        pthread_mutex_unlock(&manifest->mutex);
        pthread_mutex_unlock(&engine->compaction_mutex);
        return LSM_ERR_NOMEM;
    }

    for (int i = 0; i < sst_count; i++) {
        uint64_t file_num = manifest->levels[level].files[i];
        lsm_status_t status = lsm_sstable_reader_open(engine->env, file_num, &readers[i]);
        if (status != LSM_OK) {
            for (int j = 0; j < i; j++) {
                lsm_sstable_reader_close(readers[j]);
            }
            free(readers);
            pthread_mutex_unlock(&manifest->mutex);
            pthread_mutex_unlock(&engine->compaction_mutex);
            return status;
        }
    }
    pthread_mutex_unlock(&manifest->mutex);

    uint64_t new_file_num = lsm_manifest_next_file_number(manifest);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%06" PRIu64 ".sst", engine->env->base_dir, new_file_num);

    lsm_sstable_writer_t *writer;
    lsm_status_t status = lsm_sstable_writer_open(engine->env, path, &writer);
    if (status != LSM_OK) {
        for (int i = 0; i < sst_count; i++) {
            lsm_sstable_reader_close(readers[i]);
        }
        free(readers);
        pthread_mutex_unlock(&engine->compaction_mutex);
        return status;
    }

    status = lsm_compaction_execute_merge(engine->env, manifest, level, readers, sst_count, writer, &new_file_num);

    lsm_sstable_writer_close(writer);

    for (int i = 0; i < sst_count; i++) {
        lsm_sstable_reader_close(readers[i]);
    }
    free(readers);

    if (status == LSM_OK) {
        pthread_mutex_lock(&manifest->mutex);
        for (int i = 0; i < sst_count; i++) {
            lsm_manifest_remove_file(manifest, level, manifest->levels[level].files[i]);
        }
        lsm_manifest_add_file(manifest, level + 1, new_file_num);
        lsm_manifest_write(manifest, engine->env);
        pthread_mutex_unlock(&manifest->mutex);
    }

    pthread_mutex_unlock(&engine->compaction_mutex);
    return status;
}

void* lsm_compaction_worker_loop(void *arg) {
    lsm_engine_t *engine = (lsm_engine_t *)arg;
    if (!engine) return NULL;

    while (atomic_load(&engine->is_running)) {
        usleep(100000); 

        pthread_mutex_lock(&engine->compaction_mutex);
        lsm_manifest_t *manifest = engine->manifest;
        if (!manifest) {
            pthread_mutex_unlock(&engine->compaction_mutex);
            continue;
        }

        pthread_mutex_lock(&manifest->mutex);
        int target_level = -1;
        for (int i = 0; i < LSM_MAX_LEVELS - 1; i++) {
            if (manifest->levels[i].count > LSM_MAX_FILES_PER_LEVEL) {
                target_level = i;
                break;
            }
        }
        pthread_mutex_unlock(&manifest->mutex);
        pthread_mutex_unlock(&engine->compaction_mutex);

        if (target_level >= 0) {
            lsm_compaction_run_manual(engine, target_level);
        }
    }

    return NULL;
}

lsm_status_t lsm_compaction_worker_start(lsm_engine_t *engine) {
    if (!engine) return LSM_ERR_INVALID_ARG;
    
    atomic_store(&engine->is_running, true);
    int ret = pthread_create(&engine->compaction_thread, NULL, lsm_compaction_worker_loop, engine);
    if (ret != 0) {
        return LSM_ERR_IO;
    }
    return LSM_OK;
}

void lsm_compaction_worker_stop(lsm_engine_t *engine) {
    if (!engine) return;
    
    atomic_store(&engine->is_running, false);
    pthread_join(engine->compaction_thread, NULL);
}