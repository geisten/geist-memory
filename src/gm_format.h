#ifndef GM_FORMAT_H
#define GM_FORMAT_H
#include "geist_memory.h"
/* Host values; never persist these C object representations. */
#define GM_V1_MAGIC 0x314D47EDu
#define GM_MAGIC 0x324D47EDu
#define GM_VERSION 2u
enum {
    GM_HEADER_BYTES = 64,
    GM_DOC_PAYLOAD = 256,
    GM_CHUNK_PAYLOAD = 16,
    GM_CHECKSUM_BYTES = 32,
    GM_RECORD_MAX = GM_DIM_MAX / 8 + GM_CHECKSUM_BYTES
};
enum gm_file_kind { GM_VECTORS, GM_CHUNKS, GM_DOCS };
struct gm_file_header {
    uint32_t magic, version, dim, rec_size;
    uint64_t count, model_fp;
};
struct gm_chunk_rec {
    uint32_t doc, chunk, generation;
};
struct gm_doc_rec {
    char path[GM_PATH_MAX];
    uint32_t generation;
};
size_t gm_payload_size(enum gm_file_kind kind, size_t dim);
size_t gm_memory_stride(enum gm_file_kind kind, size_t dim);
void gm_header_encode(const struct gm_file_header *h, uint8_t out[GM_HEADER_BYTES]);
bool gm_header_decode(const uint8_t in[GM_HEADER_BYTES], struct gm_file_header *h);
void gm_record_encode(enum gm_file_kind kind, size_t dim, uint64_t model_fp, uint64_t index,
                      const void *record, uint8_t *out);
bool gm_record_decode(enum gm_file_kind kind, size_t dim, uint64_t model_fp, uint64_t index,
                      const uint8_t *in, void *record);
/* Explicit decoding of the historical little-endian v1 layout, no checksums. */
bool gm_v1_header_decode(const uint8_t in[32], struct gm_file_header *h);
void gm_v1_record_decode(enum gm_file_kind kind, size_t dim, const uint8_t *in, void *record);
#endif
