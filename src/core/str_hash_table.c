#include "moar.h"

#define STR_LOAD_FACTOR 0.75
#define STR_INITIAL_SIZE 8
#define STR_INITIAL_KEY_RIGHT_SHIFT (8 * sizeof(MVMuint64) - 3)

MVM_STATIC_INLINE MVMuint32 hash_true_size(MVMStrHashTable *hashtable) {
    return MVM_str_hash_kompromat(hashtable);
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_str_hash_demolish(MVMThreadContext *tc, MVMStrHashTable *hashtable) {
    if (hashtable->metadata) {
        free(hashtable->entries);
        free(hashtable->metadata - 1);
    }
    /* We shouldn't need these, but make something foolproof and they invent a
     * better fool: */
    hashtable->cur_items = 0;
    hashtable->entries = NULL;
    hashtable->metadata = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE void hash_allocate_common(MVMStrHashTable *hashtable) {
    hashtable->max_items = hashtable->official_size * STR_LOAD_FACTOR;
    uint32_t overflow_size = hashtable->max_items - 1;
    /* -1 because...
     * probe distance of 1 is the correct bucket.
     * hence for a value whose ideal slot is the last bucket, it's *in* the
     * official allocation.
     * probe distance of 2 is the first extra bucket beyond the official
     * allocation
     * probe distance of 255 is the 254th beyond the official allocation.
     */
    if (MVM_HASH_MAX_PROBE_DISTANCE < overflow_size) {
        hashtable->probe_overflow_size = MVM_HASH_MAX_PROBE_DISTANCE - 1;
    } else {
        hashtable->probe_overflow_size = overflow_size;
    }
    size_t actual_items = hash_true_size(hashtable);
    hashtable->entries = malloc(hashtable->entry_size * actual_items);
    hashtable->metadata = calloc(1 + actual_items + 1, 1);
    /* A sentinel. This marks an occupied slot, at its ideal position. */
    *hashtable->metadata = 1;
    ++hashtable->metadata;
    /* A sentinel at the other end. Again, occupited, ideal position. */
    hashtable->metadata[actual_items] = 1;
}

MVM_STATIC_INLINE void hash_initial_allocate(MVMThreadContext *tc,
                                             MVMStrHashTable *hashtable) {
    hashtable->key_right_shift = STR_INITIAL_KEY_RIGHT_SHIFT;
    hashtable->official_size = STR_INITIAL_SIZE;
#if HASH_DEBUG_ITER
    hashtable->ht_id = MVM_proc_rand_i(tc);
#endif

    hash_allocate_common(hashtable);
}

/* make sure you still have your copies of entries and metadata before you
   call this. */
MVM_STATIC_INLINE void hash_grow(MVMStrHashTable *hashtable) {
    --hashtable->key_right_shift;
    hashtable->official_size *= 2;

    hash_allocate_common(hashtable);
}

MVM_STATIC_INLINE struct MVMStrHashHandle *hash_insert_internal(MVMThreadContext *tc,
                                                               MVMStrHashTable *hashtable,
                                                               MVMString *key) {
    if (MVM_UNLIKELY(hashtable->cur_items >= hashtable->max_items)) {
        MVM_oops(tc, "oops, attempt to recursively call grow when adding %p",
                 key);
    }

    unsigned int probe_distance = 1;
    MVMHashNumItems bucket = MVM_str_hash_code(tc, key) >> hashtable->key_right_shift;
    char *entry_raw = hashtable->entries + bucket * hashtable->entry_size;
    MVMuint8 *metadata = hashtable->metadata + bucket;
    while (1) {
        if (*metadata < probe_distance) {
            /* this is our slot. occupied or not, it is our rightful place. */

            if (*metadata == 0) {
                /* Open goal. Score! */
            } else {
                /* make room. */

                /* Optimisation first seen in Martin Ankerl's implementation -
                   we don't need actually implement the "stealing" by swapping
                   elements and carrying on with insert. The invariant of the
                   hash is that probe distances are never out of order, and as
                   all the following elements have probe distances in order, we
                   can maintain the invariant just as well by moving everything
                   along by one. */
                MVMuint8 *find_me_a_gap = metadata;
                MVMuint8 old_probe_distance = *metadata;
                do {
                    MVMuint8 new_probe_distance = 1 + old_probe_distance;
                    if (new_probe_distance == MVM_HASH_MAX_PROBE_DISTANCE) {
                        /* Optimisation from Martin Ankerl's implementation:
                           setting this to zero forces a resize on any insert,
                           *before* the actual insert, so that we never end up
                           having to handle overflow *during* this loop. This
                           loop can always complete. */
                        hashtable->max_items = 0;
                    }
                    /* a swap: */
                    old_probe_distance = *++find_me_a_gap;
                    *find_me_a_gap = new_probe_distance;
                } while (old_probe_distance);

                MVMuint32 entries_to_move = find_me_a_gap - metadata;
                memmove(entry_raw + hashtable->entry_size, entry_raw,
                        hashtable->entry_size * entries_to_move);
            }

            *metadata = probe_distance;
            struct MVMStrHashHandle *entry = (struct MVMStrHashHandle *) entry_raw;
            entry->key = NULL;
            return entry;
        }

        if (*metadata == probe_distance) {
            struct MVMStrHashHandle *entry = (struct MVMStrHashHandle *) entry_raw;
            if (entry->key == key
                || (MVM_string_graphs_nocheck(tc, key) == MVM_string_graphs_nocheck(tc, entry->key)
                    && MVM_string_substrings_equal_nocheck(tc, key, 0,
                                                           MVM_string_graphs_nocheck(tc, key),
                                                           entry->key, 0))) {
                return entry;
            }
        }
        ++probe_distance;
        ++metadata;
        entry_raw += hashtable->entry_size;
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < hashtable->metadata + hashtable->official_size + hashtable->max_items);
        assert(metadata < hashtable->metadata + hashtable->official_size + 256);
    }
}

void *MVM_str_hash_lvalue_fetch_nt(MVMThreadContext *tc,
                                   MVMStrHashTable *hashtable,
                                   MVMString *key) {
    if (MVM_UNLIKELY(hashtable->entries == NULL)) {
        hash_initial_allocate(tc, hashtable);
    }
    else if (MVM_UNLIKELY(hashtable->cur_items >= hashtable->max_items)) {
        /* We should avoid growing the hash if we don't need to.
         * It's expensive, and for hashes with iterators, growing the hash
         * invalidates iterators. Which is buggy behaviour if the fetch doesn't
         * need to create a key. */
        struct MVMStrHashHandle *entry = MVM_str_hash_fetch_nt(tc, hashtable, key);
        if (entry) {
            return entry;
        }

        MVMuint32 true_size =  hash_true_size(hashtable);
        char *entry_raw_orig = hashtable->entries;
        MVMuint8 *metadata_orig = hashtable->metadata;

        hash_grow(hashtable);

        char *entry_raw = entry_raw_orig;
        MVMuint8 *metadata = metadata_orig;
        MVMHashNumItems bucket = 0;
        while (bucket < true_size) {
            if (*metadata) {
                struct MVMStrHashHandle *old_entry = (struct MVMStrHashHandle *) entry_raw;
                void *new_entry_raw = hash_insert_internal(tc, hashtable, old_entry->key);
                struct MVMStrHashHandle *new_entry = (struct MVMStrHashHandle *) new_entry_raw;
                assert(new_entry->key == NULL);
                memcpy(new_entry, old_entry, hashtable->entry_size);
            }
            ++bucket;
            ++metadata;
            entry_raw += hashtable->entry_size;
        }
        free(entry_raw_orig);
        free(metadata_orig - 1);
    }
    struct MVMStrHashHandle *new_entry
        = hash_insert_internal(tc, hashtable, key);
    if (!new_entry->key) {
        ++hashtable->cur_items;
    }
    return new_entry;
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care.
 * (well that's the official line. As you can see, the XXX suggests we currently
 * don't exploit the documented freedom.) */
void *MVM_str_hash_insert_nt(MVMThreadContext *tc,
                             MVMStrHashTable *hashtable,
                             MVMString *key) {
    struct MVMStrHashHandle *new_entry = MVM_str_hash_lvalue_fetch(tc, hashtable, key);
    if (new_entry->key) {
        char *c_name = MVM_string_utf8_encode_C_string(tc, key);
        char *waste[] = { c_name, NULL };
        MVM_exception_throw_adhoc_free(tc, waste, "insert duplicate key %s",
                                       c_name);
    } else {
        new_entry->key = key;
    }
    return new_entry;
}


void MVM_str_hash_delete_nt(MVMThreadContext *tc,
                            MVMStrHashTable *hashtable,
                            MVMString *key) {
    if (MVM_UNLIKELY(hashtable->entries == NULL)) {
        /* Should this be an oops? */
        return;
    }
    unsigned int probe_distance = 1;
    MVMHashNumItems bucket = MVM_str_hash_code(tc, key) >> hashtable->key_right_shift;
    char *entry_raw = hashtable->entries + bucket * hashtable->entry_size;
    uint8_t *metadata = hashtable->metadata + bucket;
    while (1) {
        if (*metadata == probe_distance) {
            struct MVMStrHashHandle *entry = (struct MVMStrHashHandle *) entry_raw;
            if (entry->key == key
                || (MVM_string_graphs_nocheck(tc, key) == MVM_string_graphs_nocheck(tc, entry->key)
                    && MVM_string_substrings_equal_nocheck(tc, key, 0,
                                                           MVM_string_graphs_nocheck(tc, key),
                                                           entry->key, 0))) {
                /* Target acquired. */

                uint8_t *metadata_target = metadata;
                /* Look at the next slot */
                uint8_t old_probe_distance = metadata_target[1];
                while (old_probe_distance > 1) {
                    /* OK, we can move this one. */
                    *metadata_target = old_probe_distance - 1;
                    /* Try the next one, etc */
                    ++metadata_target;
                    old_probe_distance = metadata_target[1];
                }
                /* metadata_target now points to the metadata for the last thing
                   we did move. (possibly still our target). */

                uint32_t entries_to_move = metadata_target - metadata;
                if (entries_to_move) {
                    memmove(entry_raw, entry_raw + hashtable->entry_size,
                            hashtable->entry_size * entries_to_move);
                }
                /* and this slot is now emtpy. */
                *metadata_target = 0;
                --hashtable->cur_items;

                /* Job's a good 'un. */
                return;
            }
        }
        /* There's a sentinel at the end. This will terminate: */
        if (*metadata < probe_distance) {
            /* So, if we hit 0, the bucket is empty. "Not found".
               If we hit something with a lower probe distance then...
               consider what would have happened had this key been inserted into
               the hash table - it would have stolen this slot, and the key we
               find here now would have been displaced futher on. Hence, the key
               we seek can't be in the hash table. */
            /* Strange. Not in the hash. Should this be an oops? */
            return;
        }
        ++probe_distance;
        ++metadata;
        entry_raw += hashtable->entry_size;
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < hashtable->metadata + hashtable->official_size + hashtable->max_items);
        assert(metadata < hashtable->metadata + hashtable->official_size + 256);
    }
}


/* This is not part of the public API, and subject to change at any point.
   (possibly in ways that are actually incompatible but won't generate compiler
   warnings.) */
MVMuint64 MVM_str_hash_fsck(MVMThreadContext *tc, MVMStrHashTable *hashtable, MVMuint32 mode) {
    const char *prefix_hashes = mode & MVM_HASH_FSCK_PREFIX_HASHES ? "# " : "";
    MVMuint32 display = mode & 3;
    MVMuint64 errors = 0;
    MVMuint64 seen = 0;

    if (hashtable->entries == NULL) {
        if (display) {
            fprintf(stderr, "%s NULL %p (empty)\n", prefix_hashes, hashtable);
        }
        return 0;
    }

    MVMuint32 true_size = hash_true_size(hashtable);
    char *entry_raw = hashtable->entries;
    MVMuint8 *metadata = hashtable->metadata;
    MVMuint32 bucket = 0;
    MVMint64 prev_offset = 0;
    while (bucket < true_size) {
        if (!*metadata) {
            /* empty slot. */
            prev_offset = 0;
            if (display == 2) {
                fprintf(stderr, "%s%3X\n", prefix_hashes, bucket);
            }
        } else {
            ++seen;

            struct MVMStrHashHandle *entry = (struct MVMStrHashHandle *) entry_raw;
            char *problem = NULL;
            MVMString *key = NULL;
            if (!entry) {
                problem = "entry NULL";
            } else {
                key = entry->key;
                if (!key) {
                    problem = "key NULL";
                }
            }
            if (!problem) {
                if ((MVMObject *)key == tc->instance->VMNull) {
                    problem = "VMNull";
                } else {
                    if (mode & MVM_HASH_FSCK_CHECK_FROMSPACE) {
                        MVMThread *cur_thread = tc->instance->threads;
                        while (cur_thread) {
                            MVMThreadContext *thread_tc = cur_thread->body.tc;
                            if (thread_tc && thread_tc->nursery_fromspace &&
                            (char *)(key) >= (char *)thread_tc->nursery_fromspace &&
                            (char *)(key) < (char *)thread_tc->nursery_fromspace +
                                thread_tc->nursery_fromspace_size) {
                                problem = "fromspace";
                                break;
                            }
                            cur_thread = cur_thread->body.next;
                        }
                    }
                }
            }
            if (!problem) {
                if (((MVMCollectable *)key)->flags & MVM_CF_DEBUG_IN_GEN2_FREE_LIST) {
                    problem = "gen2 freelist";
                } else if (REPR(key)->ID != MVM_REPR_ID_MVMString) {
                    problem = "not a string";
                } else if (!IS_CONCRETE(key)) {
                    problem = "type object";
                }
            }

            if (problem) {
                ++errors;
                if (display) {
                    fprintf(stderr, "%s%3X! %s\n", prefix_hashes, bucket, problem);
                }
                /* We don't have any great choices here. The metadata signals a
                 * key, but as it is corrupt, we can't compute its ideal bucket,
                 * and hence its offset, so we can't remember that for the next
                 * bucket. */
                prev_offset = 0;
            } else {
                /* OK, it is a concrete string (still). */
                MVMuint64 hash_val = MVM_str_hash_code(tc, key);
                MVMuint32 ideal_bucket = hash_val >> hashtable->key_right_shift;
                MVMint64 offset = 1 + bucket - ideal_bucket;
                int wrong_bucket = offset != *metadata;
                int wrong_order = offset < 1 || offset > prev_offset + 1;

                if (display == 2
                    || (display == 1 && (wrong_bucket || wrong_order))) {
                    /* And if you think that these two are overkill, you haven't
                     * had to debug this stuff :-/ */
                    char open;
                    char close;
                    if (((MVMCollectable *)key)->flags & MVM_CF_SECOND_GEN) {
                        open = '{';
                        close = '}';
                    } else if (((MVMCollectable *)key)->flags & MVM_CF_NURSERY_SEEN) {
                        open = '[';
                        close = ']';
                    } else {
                        open = '(';
                        close = ')';
                    }

                    MVMuint64 len = MVM_string_graphs(tc, key);
                    if (mode & MVM_HASH_FSCK_KEY_VIA_API) {
                        char *c_key = MVM_string_utf8_encode_C_string(tc, key);
                        fprintf(stderr,
                                "%s%3X%c%3"PRIx64"%c%0"PRIx64" %c%2"PRIu64"%c %p %s\n",
                                prefix_hashes,
                                bucket, wrong_bucket ? '!' : ' ',
                                offset, wrong_order ? '!' : ' ', hash_val,
                                open, len, close, key, c_key);
                    } else {
                        /* Cheat. Don't use the API.
                         * (Doesn't allocate, so can't deadlock. */
                        if (key->body.storage_type == MVM_STRING_GRAPHEME_ASCII && len < 0xFFF) {
                            fprintf(stderr,
                                    "%s%3X%c%3"PRIx64"%c%0"PRIx64" %c%2"PRIu64"%c %p \"%*s\"\n",
                                    prefix_hashes, bucket,
                                    wrong_bucket ? '!' : ' ', offset,
                                    wrong_order ? '!' : ' ', hash_val,
                                    open, len, close, key,
                                    (int) len, key->body.storage.blob_ascii);
                        } else {
                            fprintf(stderr,
                                    "%s%3X%c%3"PRIx64"%c%0"PRIx64" %c%2"PRIu64"%c %p %u@%p\n",
                                    prefix_hashes,
                                    bucket, wrong_bucket ? '!' : ' ',
                                    offset, wrong_order ? '!' : ' ', hash_val,
                                    open, len, close, key,
                                    key->body.storage_type, key);
                        }
                    }
                }
                errors += wrong_bucket + wrong_order;
                prev_offset = offset;
            }
        }
        ++bucket;
        ++metadata;
        entry_raw += hashtable->entry_size;
    }
    if (*metadata != 1) {
        ++errors;
        if (display) {
            fprintf(stderr, "%s    %02x!\n", prefix_hashes, *metadata);
        }
    }
    if (seen != hashtable->cur_items) {
        ++errors;
        if (display) {
            fprintf(stderr, "%s %"PRIx64" != %"PRIx32"\n",
                    prefix_hashes, seen, hashtable->cur_items);
        }
    }

    return errors;
}
