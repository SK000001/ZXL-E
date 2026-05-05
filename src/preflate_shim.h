#ifndef ZXLE_PREFLATE_SHIM_H
#define ZXLE_PREFLATE_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in preflate_shim.cpp; both return 1 on success, 0 on failure.
 * Out buffers are malloc'd; release with zxle_preflate_free. */
int  zxle_preflate_split(const uint8_t *deflate, size_t n,
                         uint8_t **out_unp, size_t *out_unp_n,
                         uint8_t **out_diff, size_t *out_diff_n);
int  zxle_preflate_join (const uint8_t *unp, size_t unp_n,
                         const uint8_t *diff, size_t diff_n,
                         uint8_t **out_def, size_t *out_def_n);
void zxle_preflate_free (void *p);

#ifdef __cplusplus
}
#endif

#endif
