/**
 * @file t9_dict.c
 * @brief Innovation I10: ~80 common English words for Note prefix suggest.
 */
#include "t9_dict.h"
#include <string.h>

static const char *const s_dict[] = {
  "a", "about", "after", "again", "all", "also", "and", "any", "as", "at",
  "back", "be", "because", "been", "before", "but", "by",
  "call", "can", "come", "could",
  "day", "do", "down",
  "each", "even", "every",
  "find", "first", "for", "from",
  "get", "give", "go", "good", "great",
  "had", "has", "have", "he", "her", "here", "him", "his", "how",
  "i", "if", "in", "into", "is", "it", "its",
  "just",
  "know",
  "like", "look",
  "make", "me", "more", "most", "my",
  "new", "no", "not", "now",
  "of", "on", "one", "only", "or", "other", "our", "out", "over",
  "people", "please",
  "said", "same", "see", "she", "should", "so", "some", "such",
  "take", "than", "that", "the", "their", "them", "then", "there",
  "these", "they", "this", "time", "to", "two",
  "up", "us", "use",
  "very",
  "want", "was", "way", "we", "well", "what", "when", "where",
  "which", "who", "will", "with", "would",
  "you", "your",
};

#define T9_DICT_COUNT ((uint8_t)(sizeof(s_dict) / sizeof(s_dict[0])))

uint8_t T9_Suggest(const char *prefix, char *out, uint8_t out_cap)
{
  if (prefix == NULL || out == NULL || out_cap < 2u) {
    return 0u;
  }
  out[0] = '\0';
  size_t plen = strlen(prefix);
  if (plen == 0u) {
    return 0u;
  }

  for (uint8_t i = 0u; i < T9_DICT_COUNT; i++) {
    const char *w = s_dict[i];
    if (strncmp(w, prefix, plen) == 0 && strlen(w) > plen) {
      size_t wlen = strlen(w);
      if (wlen >= (size_t)out_cap) {
        wlen = (size_t)out_cap - 1u;
      }
      memcpy(out, w, wlen);
      out[wlen] = '\0';
      return 1u;
    }
  }
  return 0u;
}
