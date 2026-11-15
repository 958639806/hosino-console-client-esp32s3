#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional DISPLAY COPY only. Never pass its output back as original message data.
 * Leaves ordinary UTF-8 (including combining marks) unchanged. Renders ZWJ as '+',
 * removes presentation selectors, renders skin modifiers/tags as readable ASCII.
 * Does not compose emoji. No heap allocation; source and destination must not overlap.
 * Returns required bytes excluding NUL, even on truncation. cap>0 always NUL-terminates
 * and never splits UTF-8/replacement tokens. dst=NULL is allowed only when cap==0.
 */
size_t hosino_chat_format_display(char *dst, size_t cap, const char *src);
#ifdef __cplusplus
}
#endif
