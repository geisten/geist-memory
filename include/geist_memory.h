/*
 * geist_memory.h — a local semantic memory over the geist engine.
 *
 * The whole surface: point it at a directory and a GGUF embedding model,
 * feed it files or strings, ask it questions in natural language.
 *
 *     struct gm *mem;
 *     gm_open("~/.geist/memory", "bitnet-embeddings-0.6b.gguf", nullptr, &mem);
 *     gm_remember_file(mem, "notes/arm-simd.md");
 *     struct gm_hit hits[5];
 *     size_t n;
 *     gm_recall(mem, 5, "what did we decide about ARM SIMD?", hits, &n);
 *
 * There is no daemon, no server, no database. The store is three
 * fixed-stride files and the search is a popcount loop — see README.md.
 *
 * A store belongs to ONE model. Vectors from two models are not comparable,
 * so gm_open records the model's fingerprint and refuses a different one
 * (GM_E_MODEL) rather than silently mixing spaces. Changing models means
 * re-indexing.
 *
 * Not thread-safe: one gm handle, one thread. Two handles on one directory
 * is likewise unsupported — the writer's appends are not visible to the
 * reader, and both would write the same file.
 */
#ifndef GEIST_MEMORY_H
#define GEIST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gm_status {
    GM_OK = 0,
    GM_E_INVALID_ARG,
    GM_E_IO,
    GM_E_OOM,
    GM_E_FORMAT,  /* the store on disk is not one of ours, or is truncated */
    GM_E_MODEL,   /* the store was built with a different embedding model */
    GM_E_TOO_LONG, /* a path or document exceeds a hard limit — never truncated */
    GM_E_ENGINE,  /* the geist engine refused: load, session or embed failed */
};

const char *gm_status_str(enum gm_status s);

/* Longest path the store can hold, including the terminator. A longer one
 * is rejected at gm_remember_file, not shortened. */
#define GM_PATH_MAX 232u

struct gm;

struct gm_opts {
    /* Instruction the model wants on the QUERY side only. The
     * bitnet-embedding checkpoints are trained with one and lose accuracy
     * without it; their card's own example uses "query: ". Documents never
     * get a prefix. nullptr = none. */
    const char *query_prefix;
};

/* Open or create the store in `dir` (created if absent) against the GGUF at
 * `model_path`. `opts` may be nullptr for defaults. */
[[nodiscard]] enum gm_status gm_open(const char           *dir,
                                     const char           *model_path,
                                     const struct gm_opts *opts,
                                     struct gm           **out);

void gm_close(struct gm *m);

/* Index one file. A path already indexed with the same mtime and size is a
 * no-op; a changed one is re-indexed and its old chunks stop matching. */
[[nodiscard]] enum gm_status gm_remember_file(struct gm *m, const char *path);

/* Index a string under a caller-chosen id (any short label — a URL, a
 * message id, "shell-history"). Same re-index rule, keyed on the id: the
 * same id with the same text is a no-op. */
[[nodiscard]] enum gm_status gm_remember_text(struct gm *m, const char *id, const char *text);

struct gm_hit {
    uint32_t doc;      /* index for gm_doc_path */
    uint32_t chunk;    /* which chunk of that document */
    uint32_t distance; /* Hamming distance, 0 = identical */
};

/* Nearest `k` chunks to `query`, best first. Writes the count to *n_out,
 * which is less than k when the store holds fewer chunks. */
[[nodiscard]] enum gm_status gm_recall(struct gm    *m,
                                       size_t        k,
                                       const char   *query,
                                       struct gm_hit out[static k],
                                       size_t       *n_out);

/* The path or id a hit's `doc` refers to. Owned by the store; valid until
 * gm_close. nullptr for an out-of-range index. */
const char *gm_doc_path(const struct gm *m, uint32_t doc);

/* Chunks a recall would consider: those whose document has not been
 * re-indexed under them. Superseded chunks are excluded — they stay on disk
 * until a compaction that does not exist yet, but they never match. */
size_t gm_chunk_count(const struct gm *m);

/* Embedding width in bits, i.e. the model's d_model. */
size_t gm_dim(const struct gm *m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GEIST_MEMORY_H */
