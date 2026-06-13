/* vstpoc stub: Android libcutils properties (private; apps can't read sysprops
 * anyway). property_get returns the default. */
#ifndef _VSTPOC_CUTILS_PROPERTIES_H
#define _VSTPOC_CUTILS_PROPERTIES_H
#include <string.h>
#define PROPERTY_VALUE_MAX 92
#define PROPERTY_KEY_MAX 32
static inline int property_get(const char *key, char *value, const char *default_value) {
    (void)key;
    if (default_value) { size_t n = strlen(default_value); if (n > PROPERTY_VALUE_MAX-1) n = PROPERTY_VALUE_MAX-1; memcpy(value, default_value, n); value[n] = 0; return (int)n; }
    value[0] = 0; return 0;
}
static inline int property_set(const char *key, const char *value) { (void)key; (void)value; return 0; }
static inline int property_get_bool(const char *key, int default_value) { (void)key; return default_value; }
static inline int property_get_int32(const char *key, int default_value) { (void)key; return default_value; }
#endif
