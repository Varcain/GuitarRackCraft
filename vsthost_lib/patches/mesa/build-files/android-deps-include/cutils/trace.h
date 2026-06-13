/* vstpoc stub: Android libcutils ATRACE header (private, not in the NDK). Mesa's
 * util/perf/cpu_trace.h uses atrace_begin/end for systrace; we don't need it. */
#ifndef _VSTPOC_CUTILS_TRACE_H
#define _VSTPOC_CUTILS_TRACE_H
#define ATRACE_TAG_GRAPHICS 0
#define ATRACE_TAG 0
static inline void atrace_begin(long tag, const char *name) { (void)tag; (void)name; }
static inline void atrace_end(long tag) { (void)tag; }
static inline void atrace_init(void) {}
static inline unsigned long atrace_get_enabled_tags(void) { return 0; }
static inline int atrace_is_tag_enabled(long tag) { (void)tag; return 0; }
#endif
