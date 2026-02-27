/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

/* XShm stub library for Android - provides stub implementations
 * of MIT-SHM extension functions since android-xserver doesn't
 * support this extension.
 * 
 * This file provides weak stub implementations that will be used
 * if the real XShm functions are not available.
 */
#include <stdlib.h>

// Opaque X11 types (we only need pointers)
typedef void* Display;
typedef unsigned long Drawable;
typedef void* GC;
typedef void* XImage;
typedef void* Visual;
typedef void* Pixmap;
typedef unsigned long Bool;
typedef int Status;

// XShmSegmentInfo structure (minimal)
typedef struct {
    unsigned long shmseg;
    int shmid;
    char *shmaddr;
    int readOnly;
} XShmSegmentInfo;

// Weak symbol stubs - will be overridden if real XShm is available
__attribute__((weak)) int XShmQueryExtension(void *dpy) {
    (void)dpy;
    return 0; // Extension not available
}

__attribute__((weak)) int XShmGetEventBase(void *dpy) {
    (void)dpy;
    return -1;
}

__attribute__((weak)) Bool XShmQueryVersion(void *dpy, int *major, int *minor, Bool *pixmaps) {
    (void)dpy;
    (void)major;
    (void)minor;
    (void)pixmaps;
    return 0; // Extension not available
}

__attribute__((weak)) int XShmPixmapFormat(void *dpy) {
    (void)dpy;
    return -1;
}

__attribute__((weak)) Status XShmAttach(void *dpy, XShmSegmentInfo *shminfo) {
    (void)dpy;
    (void)shminfo;
    return 0; // Failed
}

__attribute__((weak)) Status XShmDetach(void *dpy, XShmSegmentInfo *shminfo) {
    (void)dpy;
    (void)shminfo;
    return 0;
}

// External XPutImage function (provided by android-xserver)
extern int XPutImage(void *dpy, unsigned long d, void *gc, void *image,
                     int src_x, int src_y, int dest_x, int dest_y,
                     unsigned int width, unsigned int height);

__attribute__((weak)) Status XShmPutImage(void *dpy, unsigned long d, void *gc, void *image,
                    int src_x, int src_y, int dest_x, int dest_y,
                    unsigned int width, unsigned int height, Bool send_event) {
    (void)send_event;
    // Fall back to regular XPutImage
    XPutImage(dpy, d, gc, image, src_x, src_y, dest_x, dest_y, width, height);
    return 1;
}

__attribute__((weak)) Status XShmGetImage(void *dpy, unsigned long d, void *image,
                    int x, int y, unsigned long plane_mask) {
    (void)dpy;
    (void)d;
    (void)image;
    (void)x;
    (void)y;
    (void)plane_mask;
    return 0; // Failed
}

// External XCreateImage function (provided by android-xserver)
extern void* XCreateImage(void *dpy, void *visual, unsigned int depth,
                         int format, int offset, char *data,
                         unsigned int width, unsigned int height,
                         int bitmap_pad, int bytes_per_line);

__attribute__((weak)) XImage *XShmCreateImage(void *dpy, void *visual, unsigned int depth,
                        int format, char *data, XShmSegmentInfo *shminfo,
                        unsigned int width, unsigned int height) {
    (void)shminfo;
    // Just create a regular XImage, ignore shared memory
    return XCreateImage(dpy, visual, depth, format, 0, data, width, height, 32, 0);
}

__attribute__((weak)) unsigned long XShmCreatePixmap(void *dpy, unsigned long d, char *data,
                     XShmSegmentInfo *shminfo, unsigned int width, unsigned int height,
                     unsigned int depth) {
    (void)dpy;
    (void)d;
    (void)data;
    (void)shminfo;
    (void)width;
    (void)height;
    (void)depth;
    return 0; // Failed
}
