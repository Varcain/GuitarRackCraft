/* Desktop stub for <jni.h>
 * Only the opaque types appearing in X11NativeDisplay signatures are needed;
 * attachSurface(JNIEnv*, jobject, ...) is never called by the harness. */
#ifndef XTEST_STUB_JNI_H
#define XTEST_STUB_JNI_H

typedef void* jobject;
typedef void* jclass;
typedef void* jstring;
typedef int jint;
typedef long jlong;
typedef unsigned char jboolean;
typedef signed char jbyte;
typedef short jshort;
typedef float jfloat;
typedef double jdouble;

struct _JNIEnv;
typedef struct _JNIEnv JNIEnv;

#endif /* XTEST_STUB_JNI_H */
