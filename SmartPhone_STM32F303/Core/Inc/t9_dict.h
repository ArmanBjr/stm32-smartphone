/**
 * @file t9_dict.h
 * @brief Innovation I10: tiny English prefix dictionary for Note T9-lite.
 */
#ifndef T9_DICT_H
#define T9_DICT_H

#include <stdint.h>

/** Copies the first dictionary word that starts with `prefix` into `out`
 *  (NUL-terminated, truncated to out_cap-1). Returns 1 if a suggestion was
 *  found, 0 otherwise (empty prefix, no match, or bad args). */
uint8_t T9_Suggest(const char *prefix, char *out, uint8_t out_cap);

#endif /* T9_DICT_H */
