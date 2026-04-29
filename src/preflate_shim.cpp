/* C ABI shim around libpreflate. */
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "preflate.h"

extern "C" {

int zxle_preflate_split(const uint8_t *deflate, size_t n,
                        uint8_t **out_unp, size_t *out_unp_n,
                        uint8_t **out_diff, size_t *out_diff_n) {
    std::vector<unsigned char> in(deflate, deflate + n);
    std::vector<unsigned char> unp;
    std::vector<unsigned char> diff;
    if (!preflate_decode(unp, diff, in)) return 0;

    *out_unp = (uint8_t*)malloc(unp.size() ? unp.size() : 1);
    *out_diff = (uint8_t*)malloc(diff.size() ? diff.size() : 1);
    if (!*out_unp || !*out_diff) { free(*out_unp); free(*out_diff); return 0; }
    if (!unp.empty())  memcpy(*out_unp,  unp.data(),  unp.size());
    if (!diff.empty()) memcpy(*out_diff, diff.data(), diff.size());
    *out_unp_n  = unp.size();
    *out_diff_n = diff.size();
    return 1;
}

int zxle_preflate_join(const uint8_t *unp, size_t unp_n,
                       const uint8_t *diff, size_t diff_n,
                       uint8_t **out_def, size_t *out_def_n) {
    std::vector<unsigned char> u(unp, unp + unp_n);
    std::vector<unsigned char> d(diff, diff + diff_n);
    std::vector<unsigned char> def;
    if (!preflate_reencode(def, d, u)) return 0;

    *out_def = (uint8_t*)malloc(def.size() ? def.size() : 1);
    if (!*out_def) return 0;
    if (!def.empty()) memcpy(*out_def, def.data(), def.size());
    *out_def_n = def.size();
    return 1;
}

void zxle_preflate_free(void *p) { free(p); }

} /* extern "C" */
