/* PicoView plugin interface, version 1.
 *
 * A plugin is an ordinary DLL with one exported function. PicoView calls it
 * once, the plugin fills in a table of function pointers, and from then on the
 * two speak only through this header - no C++ classes, no allocators in
 * common, nothing that ties a plugin to the compiler that built the viewer.
 * That is the whole reason this file is plain C: a plugin built with MinGW,
 * clang or a different version of MSVC has to keep working.
 *
 * Put the DLL in  plugins\  next to PicoView.exe, or in
 * %APPDATA%\PicoView\plugins\ . PicoView looks for that folder at startup and
 * skips the whole subsystem when it is not there, so a machine with no plugins
 * pays nothing for this.
 *
 * Threads: decode() is called from PicoView's decoding workers and may be
 * entered concurrently unless the plugin says otherwise; enhance() is called
 * from the interface thread. Keep them re-entrant or take your own lock.
 *
 * Pixels: PV_BGRA8 is 8 bits per channel in B, G, R, A order, alpha
 * premultiplied, rows `stride` bytes apart, top row first. That is what
 * Direct2D wants and what PicoView holds internally, so nothing is converted
 * on the way in or out.
 */
#ifndef PICOVIEW_PLUGIN_H
#define PICOVIEW_PLUGIN_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PV_ABI_VERSION 1

/* Pixel formats. Only one so far; the field exists so adding another does not
   need a new ABI. */
#define PV_BGRA8 1u

/* What a plugin is offering to do. */
#define PV_CAP_DECODE  (1u << 0)   /* turn a file PicoView cannot read into pixels */
#define PV_CAP_ENHANCE (1u << 1)   /* sharpen or enlarge an image already open */

/* Return values. Anything non-zero is a failure and PicoView carries on as if
   the plugin had not been there. */
#define PV_OK             0
#define PV_ERR_FORMAT     1   /* not a file this plugin handles - not an error */
#define PV_ERR_MEMORY     2
#define PV_ERR_CORRUPT    3
#define PV_ERR_UNSUPPORTED 4

typedef struct PvImage {
    uint32_t format;      /* PV_BGRA8 */
    int32_t  width;
    int32_t  height;
    int32_t  stride;      /* bytes between the starts of two rows */
    void*    pixels;
    void*    owner;       /* the plugin's business; PicoView never touches it */
} PvImage;

/* What PicoView lends the plugin. Check `size` before reading a field that was
   added after the version you were built against. */
typedef struct PvHost {
    uint32_t size;
    uint32_t abi;                              /* PV_ABI_VERSION of the host */
    void (*log)(const wchar_t* message);       /* goes to picoview.log under PG_DEBUG */
    void (*toast)(const wchar_t* message);     /* a line on screen, for real news only */
    void* (*alloc)(size_t bytes);              /* zeroed; free with host->free */
    void (*release)(void* block);
} PvHost;

/* What the plugin hands back. Everything except `size`, `caps` and `name` may
   be left at zero. */
typedef struct PvPlugin {
    uint32_t size;
    uint32_t caps;
    const wchar_t* name;
    const wchar_t* version;
    const wchar_t* author;
    const wchar_t* description;

    /* PV_CAP_DECODE: lower case, dot included, comma separated - ".pcx,.ras".
       PicoView adds these to the formats it will open and list in folders. */
    const wchar_t* extensions;

    /* PV_CAP_DECODE. Fill `out` and return PV_OK. Return PV_ERR_FORMAT for a
       file that is simply not yours; PicoView then tries the next plugin. */
    int (*decode)(const wchar_t* path, PvImage* out);

    /* PV_CAP_ENHANCE. `scale` is 1, 2 or 4. A plugin that only sharpens should
       accept 1 and refuse the rest with PV_ERR_UNSUPPORTED. */
    int (*enhance)(const PvImage* in, int scale, PvImage* out);

    /* Called for every image the plugin produced, once PicoView is done with
       it. Required if decode or enhance is implemented. */
    void (*free_image)(PvImage* image);

    /* Called before the DLL is unloaded. */
    void (*shutdown)(void);
} PvPlugin;

/* The one export. Must be named exactly this.
 *
 *   __declspec(dllexport) int PicoViewPluginInit(const PvHost* host, PvPlugin* out)
 *
 * Return PV_OK after filling `out`. Anything else and the DLL is unloaded
 * again. Do not keep a copy of `host` past shutdown().
 */
typedef int (*PvPluginInit)(const PvHost* host, PvPlugin* out);

#ifdef __cplusplus
}
#endif
#endif /* PICOVIEW_PLUGIN_H */
