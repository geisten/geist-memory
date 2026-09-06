#include "gm_format.h"
#include "gm_hash.h"
#include "gm_internal.h"
#include <limits.h>
static_assert(CHAR_BIT == 8, "octet file format required");
size_t gm_payload_size(enum gm_file_kind kind, size_t dim) {
    return kind == GM_VECTORS ? dim / 8 : kind == GM_CHUNKS ? GM_CHUNK_PAYLOAD : GM_DOC_PAYLOAD;
}
size_t gm_memory_stride(enum gm_file_kind kind, size_t dim) {
    return kind == GM_VECTORS  ? dim / 8
           : kind == GM_CHUNKS ? sizeof(struct gm_chunk_rec)
                               : sizeof(struct gm_doc_rec);
}
static void decode_header_fields(const uint8_t *in, struct gm_file_header *h) {
    *h = (struct gm_file_header){.magic = gm_u32(in),
                                 .version = gm_u32(in + 4),
                                 .dim = gm_u32(in + 8),
                                 .rec_size = gm_u32(in + 12),
                                 .count = gm_u64(in + 16),
                                 .model_fp = gm_u64(in + 24)};
}
void gm_header_encode(const struct gm_file_header *h, uint8_t out[GM_HEADER_BYTES]) {
    gm_put32(out, h->magic);
    gm_put32(out + 4, h->version);
    gm_put32(out + 8, h->dim);
    gm_put32(out + 12, h->rec_size);
    gm_put64(out + 16, h->count);
    gm_put64(out + 24, h->model_fp);
    gm_digest(32, out, out + 32);
}
bool gm_header_decode(const uint8_t in[GM_HEADER_BYTES], struct gm_file_header *h) {
    uint8_t digest[32];
    gm_digest(32, in, digest);
    if (memcmp(digest, in + 32, 32))
        return false;
    decode_header_fields(in, h);
    return h->magic == GM_MAGIC && h->version == GM_VERSION;
}
bool gm_v1_header_decode(const uint8_t in[32], struct gm_file_header *h) {
    decode_header_fields(in, h);
    return h->magic == GM_V1_MAGIC && h->version == 1;
}
/* Bind every checksum to file kind, model, dimension and physical position.
 * Counts are excluded so appending does not invalidate existing records. */
static void record_digest(enum gm_file_kind kind, size_t dim, uint64_t fp, uint64_t index,
                          const uint8_t *payload, uint8_t out[32]) {
    uint8_t context[24];
    gm_put32(context, (uint32_t)kind);
    gm_put32(context + 4, (uint32_t)dim);
    gm_put64(context + 8, fp);
    gm_put64(context + 16, index);
    struct gm_hash h;
    gm_hash_init(&h);
    gm_hash_update(&h, sizeof context, context);
    gm_hash_update(&h, gm_payload_size(kind, dim), payload);
    gm_hash_final(&h, out);
}
void gm_record_encode(enum gm_file_kind kind, size_t dim, uint64_t fp, uint64_t index,
                      const void *record, uint8_t *out) {
    size_t payload = gm_payload_size(kind, dim);
    memset(out, 0, payload);
    if (kind == GM_VECTORS)
        memcpy(out, record, payload);
    else if (kind == GM_CHUNKS) {
        const struct gm_chunk_rec *r = record;
        gm_put32(out, r->doc);
        gm_put32(out + 4, r->chunk);
        gm_put32(out + 8, r->generation);
    } else {
        const struct gm_doc_rec *r = record;
        memcpy(out, r->path, strnlen(r->path, GM_PATH_MAX));
        gm_put32(out + 248, r->generation);
    }
    record_digest(kind, dim, fp, index, out, out + payload);
}
void gm_v1_record_decode(enum gm_file_kind kind, size_t dim, const uint8_t *in, void *record) {
    if (kind == GM_VECTORS)
        memcpy(record, in, dim / 8);
    else if (kind == GM_CHUNKS) {
        struct gm_chunk_rec *r = record;
        *r = (struct gm_chunk_rec){
            .doc = gm_u32(in), .chunk = gm_u32(in + 4), .generation = gm_u32(in + 8)};
    } else {
        struct gm_doc_rec *r = record;
        memset(r, 0, sizeof *r);
        memcpy(r->path, in, GM_PATH_MAX);
        r->generation = gm_u32(in + 248);
    }
}
bool gm_record_decode(enum gm_file_kind kind, size_t dim, uint64_t fp, uint64_t index,
                      const uint8_t *in, void *record) {
    size_t payload = gm_payload_size(kind, dim);
    uint8_t digest[32];
    record_digest(kind, dim, fp, index, in, digest);
    if (memcmp(digest, in + payload, 32))
        return false;
    if (kind == GM_CHUNKS && gm_u32(in + 12))
        return false;
    if (kind == GM_DOCS) {
        const uint8_t *end = memchr(in, 0, GM_PATH_MAX);
        if (!end || end == in || gm_u64(in + 232) || gm_u64(in + 240) || gm_u32(in + 252))
            return false;
        for (size_t i = (size_t)(end - in); i < GM_PATH_MAX; ++i)
            if (in[i])
                return false;
    }
    if (record)
        gm_v1_record_decode(kind, dim, in, record);
    return true;
}
