#define COBJMACROS
#ifdef BRIDGE_NATIVE_PE
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BRIDGE_NATIVE_PE
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#endif

#include "core/Factory.h"
#include "components/VideoEncoderVCE.h"
#include "dxva_bridge.h"
#include "helper_protocol.h"

#define BRIDGE_MAX_PROPERTIES 64
#define BRIDGE_MAX_OUTPUTS 8
#define AMF_TIME_BASE 10000000LL

typedef struct BridgeProperty {
    wchar_t *name;
    AMFVariantStruct value;
} BridgeProperty;

typedef struct BridgePropertyStore {
    BridgeProperty entries[BRIDGE_MAX_PROPERTIES];
    size_t count;
} BridgePropertyStore;

typedef struct BridgeContext BridgeContext;
typedef struct BridgeSurface BridgeSurface;
typedef struct BridgeBuffer BridgeBuffer;
typedef struct BridgeComponent BridgeComponent;

struct BridgeContext {
    AMFContext iface;
    LONG refs;
    BridgePropertyStore properties;
    ID3D11Device *device;
    ID3D11DeviceContext *immediate;
};

struct BridgeSurface {
    AMFSurface iface;
    AMFPlane plane;
    LONG refs;
    BridgePropertyStore properties;
    ID3D11Texture2D *texture;
    AMF_SURFACE_FORMAT format;
    amf_int32 width;
    amf_int32 height;
    amf_pts pts;
    amf_pts duration;
    AMF_FRAME_TYPE frame_type;
};

struct BridgeBuffer {
    AMFBuffer iface;
    LONG refs;
    BridgePropertyStore properties;
    uint8_t *data;
    size_t size;
    amf_pts pts;
    amf_pts duration;
};

struct BridgeComponent {
    AMFComponent iface;
    LONG refs;
    BridgePropertyStore properties;
    BridgeContext *context;
#ifndef BRIDGE_NATIVE_PE
    AVCodecContext *codec;
    AVBufferRef *hw_device;
    AVBufferRef *hw_frames;
#else
    SOCKET helper;
#endif
    BridgeBuffer *outputs[BRIDGE_MAX_OUTPUTS];
    size_t output_head;
    size_t output_count;
    amf_int32 width;
    amf_int32 height;
    AMF_SURFACE_FORMAT format;
    int64_t next_pts;
    int initialized;
    int draining;
};

static FILE *bridge_log_file;

static void bridge_log(const char *level, const char *message)
{
    if (!bridge_log_file) {
        const char *path = getenv("UU_AMF_BRIDGE_LOG");
        if (path && *path) {
            bridge_log_file = fopen(path, "a");
        }
    }
    if (bridge_log_file) {
        fprintf(bridge_log_file, "[%s] %s\n", level, message);
        fflush(bridge_log_file);
    }
}

static int guid_equal(const AMFGuid *left, const AMFGuid *right)
{
    return left && right && memcmp(left, right, sizeof(*left)) == 0;
}

static void property_store_clear(BridgePropertyStore *store)
{
    size_t i;
    for (i = 0; i < store->count; ++i) {
        AMFVariantClear(&store->entries[i].value);
        free(store->entries[i].name);
    }
    memset(store, 0, sizeof(*store));
}

static BridgeProperty *property_find(BridgePropertyStore *store, const wchar_t *name)
{
    size_t i;
    if (!name) {
        return NULL;
    }
    for (i = 0; i < store->count; ++i) {
        if (wcscmp(store->entries[i].name, name) == 0) {
            return &store->entries[i];
        }
    }
    return NULL;
}

static AMF_RESULT property_set(BridgePropertyStore *store, const wchar_t *name, AMFVariantStruct value)
{
    BridgeProperty *entry;
    size_t length;
    if (!store || !name) {
        return AMF_INVALID_ARG;
    }
    entry = property_find(store, name);
    if (!entry) {
        if (store->count == BRIDGE_MAX_PROPERTIES) {
            return AMF_OUT_OF_MEMORY;
        }
        entry = &store->entries[store->count++];
        length = wcslen(name) + 1;
        entry->name = malloc(length * sizeof(*entry->name));
        if (!entry->name) {
            --store->count;
            return AMF_OUT_OF_MEMORY;
        }
        memcpy(entry->name, name, length * sizeof(*entry->name));
        AMFVariantInit(&entry->value);
    } else {
        AMFVariantClear(&entry->value);
    }
    return AMFVariantCopy(&entry->value, &value);
}

static AMF_RESULT property_get(BridgePropertyStore *store, const wchar_t *name, AMFVariantStruct *value)
{
    BridgeProperty *entry;
    if (!store || !name || !value) {
        return AMF_INVALID_ARG;
    }
    entry = property_find(store, name);
    if (!entry) {
        AMFVariantInit(value);
        return AMF_NOT_FOUND;
    }
    AMFVariantInit(value);
    return AMFVariantCopy(value, &entry->value);
}

static amf_int64 property_get_i64(BridgePropertyStore *store, const wchar_t *name, amf_int64 fallback)
{
    BridgeProperty *entry = property_find(store, name);
    return entry && entry->value.type == AMF_VARIANT_INT64 ? entry->value.int64Value : fallback;
}

static AMFRate property_get_rate(BridgePropertyStore *store, const wchar_t *name, AMFRate fallback)
{
    BridgeProperty *entry = property_find(store, name);
    return entry && entry->value.type == AMF_VARIANT_RATE ? entry->value.rateValue : fallback;
}

#ifdef BRIDGE_NATIVE_PE
static unsigned long parse_port(const char *text, unsigned long fallback)
{
    unsigned long value = 0;
    if (!text || !*text) return fallback;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (unsigned long)(*text - '0');
        if (value > 65535) return fallback;
        ++text;
    }
    return *text == 0 && value != 0 ? value : fallback;
}

static int socket_send_all(SOCKET socket_handle, const void *data, size_t size)
{
    const char *cursor = data;
    while (size) {
        int sent = send(socket_handle, cursor, size > INT_MAX ? INT_MAX : (int)size, 0);
        if (sent <= 0) return 0;
        cursor += sent;
        size -= sent;
    }
    return 1;
}

static int socket_recv_all(SOCKET socket_handle, void *data, size_t size)
{
    char *cursor = data;
    while (size) {
        int received = recv(socket_handle, cursor, size > INT_MAX ? INT_MAX : (int)size, 0);
        if (received <= 0) return 0;
        cursor += received;
        size -= received;
    }
    return 1;
}
#endif

#define DEFINE_PROPERTY_METHODS(prefix, object_type, interface_type, member) \
static AMF_RESULT AMF_STD_CALL prefix##_set(interface_type *iface, const wchar_t *name, AMFVariantStruct value) \
{ return property_set(&((object_type *)iface)->member, name, value); } \
static AMF_RESULT AMF_STD_CALL prefix##_get(interface_type *iface, const wchar_t *name, AMFVariantStruct *value) \
{ return property_get(&((object_type *)iface)->member, name, value); } \
static amf_bool AMF_STD_CALL prefix##_has(interface_type *iface, const wchar_t *name) \
{ return property_find(&((object_type *)iface)->member, name) != NULL; } \
static amf_size AMF_STD_CALL prefix##_count(interface_type *iface) \
{ return ((object_type *)iface)->member.count; } \
static AMF_RESULT AMF_STD_CALL prefix##_at(interface_type *iface, amf_size index, wchar_t *name, amf_size name_size, AMFVariantStruct *value) \
{ \
    BridgePropertyStore *store = &((object_type *)iface)->member; \
    size_t needed; \
    if (index >= store->count || !value) return AMF_OUT_OF_RANGE; \
    needed = wcslen(store->entries[index].name) + 1; \
    if (name && name_size) { wcsncpy(name, store->entries[index].name, name_size - 1); name[name_size - 1] = 0; } \
    AMFVariantInit(value); \
    (void)needed; \
    return AMFVariantCopy(value, &store->entries[index].value); \
} \
static AMF_RESULT AMF_STD_CALL prefix##_clear(interface_type *iface) \
{ property_store_clear(&((object_type *)iface)->member); return AMF_OK; } \
static AMF_RESULT AMF_STD_CALL prefix##_add_to(interface_type *iface, AMFPropertyStorage *dest, amf_bool overwrite, amf_bool deep) \
{ (void)iface; (void)dest; (void)overwrite; (void)deep; return AMF_NOT_IMPLEMENTED; } \
static AMF_RESULT AMF_STD_CALL prefix##_copy_to(interface_type *iface, AMFPropertyStorage *dest, amf_bool deep) \
{ (void)iface; (void)dest; (void)deep; return AMF_NOT_IMPLEMENTED; } \
static void AMF_STD_CALL prefix##_add_observer(interface_type *iface, AMFPropertyStorageObserver *observer) \
{ (void)iface; (void)observer; } \
static void AMF_STD_CALL prefix##_remove_observer(interface_type *iface, AMFPropertyStorageObserver *observer) \
{ (void)iface; (void)observer; }

DEFINE_PROPERTY_METHODS(context_prop, BridgeContext, AMFContext, properties)
DEFINE_PROPERTY_METHODS(surface_prop, BridgeSurface, AMFSurface, properties)
DEFINE_PROPERTY_METHODS(buffer_prop, BridgeBuffer, AMFBuffer, properties)
DEFINE_PROPERTY_METHODS(component_prop, BridgeComponent, AMFComponent, properties)

/* Buffer implementation */
static amf_long AMF_STD_CALL buffer_acquire(AMFBuffer *iface)
{
    return InterlockedIncrement(&((BridgeBuffer *)iface)->refs);
}

static amf_long AMF_STD_CALL buffer_release(AMFBuffer *iface)
{
    BridgeBuffer *buffer = (BridgeBuffer *)iface;
    LONG refs = InterlockedDecrement(&buffer->refs);
    if (!refs) {
        property_store_clear(&buffer->properties);
        free(buffer->data);
        free(buffer);
    }
    return refs;
}

static AMF_RESULT AMF_STD_CALL buffer_query_interface(AMFBuffer *iface, const AMFGuid *iid, void **out)
{
    AMFGuid buffer_iid = IID_AMFBuffer();
    AMFGuid data_iid = IID_AMFData();
    AMFGuid storage_iid = IID_AMFPropertyStorage();
    AMFGuid interface_iid = IID_AMFInterface();
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (guid_equal(iid, &buffer_iid) || guid_equal(iid, &data_iid) ||
        guid_equal(iid, &storage_iid) || guid_equal(iid, &interface_iid)) {
        *out = iface;
        buffer_acquire(iface);
        return AMF_OK;
    }
    return AMF_NO_INTERFACE;
}

static AMF_MEMORY_TYPE AMF_STD_CALL buffer_memory_type(AMFBuffer *iface) { return AMF_MEMORY_HOST; }
static AMF_RESULT AMF_STD_CALL buffer_duplicate(AMFBuffer *iface, AMF_MEMORY_TYPE type, AMFData **out) { (void)iface; (void)type; (void)out; return AMF_NOT_IMPLEMENTED; }
static AMF_RESULT AMF_STD_CALL buffer_convert(AMFBuffer *iface, AMF_MEMORY_TYPE type) { (void)iface; return type == AMF_MEMORY_HOST ? AMF_OK : AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL buffer_interop(AMFBuffer *iface, AMF_MEMORY_TYPE type) { return buffer_convert(iface, type); }
static AMF_DATA_TYPE AMF_STD_CALL buffer_data_type(AMFBuffer *iface) { (void)iface; return AMF_DATA_BUFFER; }
static amf_bool AMF_STD_CALL buffer_reusable(AMFBuffer *iface) { (void)iface; return 0; }
static void AMF_STD_CALL buffer_set_pts(AMFBuffer *iface, amf_pts pts) { ((BridgeBuffer *)iface)->pts = pts; }
static amf_pts AMF_STD_CALL buffer_get_pts(AMFBuffer *iface) { return ((BridgeBuffer *)iface)->pts; }
static void AMF_STD_CALL buffer_set_duration(AMFBuffer *iface, amf_pts duration) { ((BridgeBuffer *)iface)->duration = duration; }
static amf_pts AMF_STD_CALL buffer_get_duration(AMFBuffer *iface) { return ((BridgeBuffer *)iface)->duration; }
static AMF_RESULT AMF_STD_CALL buffer_set_size(AMFBuffer *iface, amf_size size)
{
    BridgeBuffer *buffer = (BridgeBuffer *)iface;
    uint8_t *data = realloc(buffer->data, size);
    if (size && !data) return AMF_OUT_OF_MEMORY;
    buffer->data = data;
    buffer->size = size;
    return AMF_OK;
}
static amf_size AMF_STD_CALL buffer_get_size(AMFBuffer *iface) { return ((BridgeBuffer *)iface)->size; }
static void *AMF_STD_CALL buffer_get_native(AMFBuffer *iface) { return ((BridgeBuffer *)iface)->data; }
static void AMF_STD_CALL buffer_add_observer(AMFBuffer *iface, AMFBufferObserver *observer) { (void)iface; (void)observer; }
static void AMF_STD_CALL buffer_remove_observer(AMFBuffer *iface, AMFBufferObserver *observer) { (void)iface; (void)observer; }

static const AMFBufferVtbl buffer_vtbl = {
    buffer_acquire, buffer_release, buffer_query_interface,
    buffer_prop_set, buffer_prop_get, buffer_prop_has, buffer_prop_count, buffer_prop_at,
    buffer_prop_clear, buffer_prop_add_to, buffer_prop_copy_to,
    buffer_prop_add_observer, buffer_prop_remove_observer,
    buffer_memory_type, buffer_duplicate, buffer_convert, buffer_interop,
    buffer_data_type, buffer_reusable,
    buffer_set_pts, buffer_get_pts, buffer_set_duration, buffer_get_duration,
    buffer_set_size, buffer_get_size, buffer_get_native,
    buffer_add_observer, buffer_remove_observer
};

static BridgeBuffer *buffer_create(const uint8_t *data, size_t size, amf_pts pts, amf_pts duration)
{
    BridgeBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->iface.pVtbl = &buffer_vtbl;
    buffer->refs = 1;
    buffer->pts = pts;
    buffer->duration = duration;
    if (size) {
        buffer->data = malloc(size);
        if (!buffer->data) { free(buffer); return NULL; }
        memcpy(buffer->data, data, size);
        buffer->size = size;
    }
    return buffer;
}

/* Surface implementation */
static amf_long AMF_STD_CALL surface_acquire(AMFSurface *iface)
{
    return InterlockedIncrement(&((BridgeSurface *)iface)->refs);
}

static amf_long AMF_STD_CALL surface_release(AMFSurface *iface)
{
    BridgeSurface *surface = (BridgeSurface *)iface;
    LONG refs = InterlockedDecrement(&surface->refs);
    if (!refs) {
        property_store_clear(&surface->properties);
        if (surface->texture) ID3D11Texture2D_Release(surface->texture);
        free(surface);
    }
    return refs;
}

static AMF_RESULT AMF_STD_CALL surface_query_interface(AMFSurface *iface, const AMFGuid *iid, void **out)
{
    AMFGuid surface_iid = IID_AMFSurface();
    AMFGuid data_iid = IID_AMFData();
    AMFGuid storage_iid = IID_AMFPropertyStorage();
    AMFGuid interface_iid = IID_AMFInterface();
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (guid_equal(iid, &surface_iid) || guid_equal(iid, &data_iid) ||
        guid_equal(iid, &storage_iid) || guid_equal(iid, &interface_iid)) {
        *out = iface;
        surface_acquire(iface);
        return AMF_OK;
    }
    return AMF_NO_INTERFACE;
}

static AMF_MEMORY_TYPE AMF_STD_CALL surface_memory_type(AMFSurface *iface) { (void)iface; return AMF_MEMORY_DX11; }
static AMF_RESULT AMF_STD_CALL surface_duplicate(AMFSurface *iface, AMF_MEMORY_TYPE type, AMFData **out) { (void)iface; (void)type; (void)out; return AMF_NOT_IMPLEMENTED; }
static AMF_RESULT AMF_STD_CALL surface_convert(AMFSurface *iface, AMF_MEMORY_TYPE type) { (void)iface; return type == AMF_MEMORY_DX11 ? AMF_OK : AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL surface_interop(AMFSurface *iface, AMF_MEMORY_TYPE type) { return surface_convert(iface, type); }
static AMF_DATA_TYPE AMF_STD_CALL surface_data_type(AMFSurface *iface) { (void)iface; return AMF_DATA_SURFACE; }
static amf_bool AMF_STD_CALL surface_reusable(AMFSurface *iface) { (void)iface; return 0; }
static void AMF_STD_CALL surface_set_pts(AMFSurface *iface, amf_pts pts) { ((BridgeSurface *)iface)->pts = pts; }
static amf_pts AMF_STD_CALL surface_get_pts(AMFSurface *iface) { return ((BridgeSurface *)iface)->pts; }
static void AMF_STD_CALL surface_set_duration(AMFSurface *iface, amf_pts duration) { ((BridgeSurface *)iface)->duration = duration; }
static amf_pts AMF_STD_CALL surface_get_duration(AMFSurface *iface) { return ((BridgeSurface *)iface)->duration; }
static AMF_SURFACE_FORMAT AMF_STD_CALL surface_get_format(AMFSurface *iface) { return ((BridgeSurface *)iface)->format; }
static amf_size AMF_STD_CALL surface_plane_count(AMFSurface *iface) { (void)iface; return 1; }
static AMFPlane *AMF_STD_CALL surface_get_plane_at(AMFSurface *iface, amf_size index) { return index == 0 ? &((BridgeSurface *)iface)->plane : NULL; }
static AMFPlane *AMF_STD_CALL surface_get_plane(AMFSurface *iface, AMF_PLANE_TYPE type) { return type == AMF_PLANE_PACKED || type == AMF_PLANE_Y ? &((BridgeSurface *)iface)->plane : NULL; }
static AMF_FRAME_TYPE AMF_STD_CALL surface_get_frame_type(AMFSurface *iface) { return ((BridgeSurface *)iface)->frame_type; }
static void AMF_STD_CALL surface_set_frame_type(AMFSurface *iface, AMF_FRAME_TYPE type) { ((BridgeSurface *)iface)->frame_type = type; }
static AMF_RESULT AMF_STD_CALL surface_set_crop(AMFSurface *iface, amf_int32 x, amf_int32 y, amf_int32 width, amf_int32 height) { (void)iface; (void)x; (void)y; (void)width; (void)height; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL surface_copy_region(AMFSurface *iface, AMFSurface *dest, amf_int32 dx, amf_int32 dy, amf_int32 sx, amf_int32 sy, amf_int32 width, amf_int32 height) { (void)iface; (void)dest; (void)dx; (void)dy; (void)sx; (void)sy; (void)width; (void)height; return AMF_NOT_IMPLEMENTED; }
static void AMF_STD_CALL surface_add_observer(AMFSurface *iface, AMFSurfaceObserver *observer) { (void)iface; (void)observer; }
static void AMF_STD_CALL surface_remove_observer(AMFSurface *iface, AMFSurfaceObserver *observer) { (void)iface; (void)observer; }

static const AMFSurfaceVtbl surface_vtbl = {
    surface_acquire, surface_release, surface_query_interface,
    surface_prop_set, surface_prop_get, surface_prop_has, surface_prop_count, surface_prop_at,
    surface_prop_clear, surface_prop_add_to, surface_prop_copy_to,
    surface_prop_add_observer, surface_prop_remove_observer,
    surface_memory_type, surface_duplicate, surface_convert, surface_interop,
    surface_data_type, surface_reusable,
    surface_set_pts, surface_get_pts, surface_set_duration, surface_get_duration,
    surface_get_format, surface_plane_count, surface_get_plane_at, surface_get_plane,
    surface_get_frame_type, surface_set_frame_type, surface_set_crop, surface_copy_region,
    surface_add_observer, surface_remove_observer
};

static BridgeSurface *surface_from_plane(AMFPlane *iface)
{
    return (BridgeSurface *)((char *)iface - offsetof(BridgeSurface, plane));
}

static amf_long AMF_STD_CALL plane_acquire(AMFPlane *iface) { return InterlockedIncrement(&surface_from_plane(iface)->refs); }
static amf_long AMF_STD_CALL plane_release(AMFPlane *iface) { return surface_release(&surface_from_plane(iface)->iface); }
static AMF_RESULT AMF_STD_CALL plane_query_interface(AMFPlane *iface, const AMFGuid *iid, void **out)
{
    AMFGuid plane_iid = IID_AMFPlane();
    AMFGuid interface_iid = IID_AMFInterface();
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!guid_equal(iid, &plane_iid) && !guid_equal(iid, &interface_iid)) return AMF_NO_INTERFACE;
    *out = iface;
    plane_acquire(iface);
    return AMF_OK;
}
static AMF_PLANE_TYPE AMF_STD_CALL plane_get_type(AMFPlane *iface) { (void)iface; return AMF_PLANE_PACKED; }
static void *AMF_STD_CALL plane_get_native(AMFPlane *iface) { return surface_from_plane(iface)->texture; }
static amf_int32 AMF_STD_CALL plane_pixel_size(AMFPlane *iface) { (void)iface; return 1; }
static amf_int32 AMF_STD_CALL plane_zero(AMFPlane *iface) { (void)iface; return 0; }
static amf_int32 AMF_STD_CALL plane_width(AMFPlane *iface) { return surface_from_plane(iface)->width; }
static amf_int32 AMF_STD_CALL plane_height(AMFPlane *iface) { return surface_from_plane(iface)->height; }
static amf_int32 AMF_STD_CALL plane_hpitch(AMFPlane *iface) { return surface_from_plane(iface)->width; }
static amf_int32 AMF_STD_CALL plane_vpitch(AMFPlane *iface) { return surface_from_plane(iface)->height * 3 / 2; }
static amf_bool AMF_STD_CALL plane_tiled(AMFPlane *iface) { (void)iface; return 0; }

static const AMFPlaneVtbl plane_vtbl = {
    plane_acquire, plane_release, plane_query_interface,
    plane_get_type, plane_get_native, plane_pixel_size,
    plane_zero, plane_zero, plane_width, plane_height,
    plane_hpitch, plane_vpitch, plane_tiled
};

static AMF_SURFACE_FORMAT format_from_dxgi(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_NV12: return AMF_SURFACE_NV12;
    case DXGI_FORMAT_P010: return AMF_SURFACE_P010;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return AMF_SURFACE_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return AMF_SURFACE_RGBA;
    default: return AMF_SURFACE_UNKNOWN;
    }
}

static BridgeSurface *surface_wrap(ID3D11Texture2D *texture, AMF_SURFACE_FORMAT forced_format)
{
    BridgeSurface *surface;
    D3D11_TEXTURE2D_DESC desc;
    if (!texture) return NULL;
    surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->iface.pVtbl = &surface_vtbl;
    surface->plane.pVtbl = &plane_vtbl;
    surface->refs = 1;
    surface->texture = texture;
    ID3D11Texture2D_AddRef(texture);
    ID3D11Texture2D_GetDesc(texture, &desc);
    surface->width = desc.Width;
    surface->height = desc.Height;
    surface->format = forced_format == AMF_SURFACE_UNKNOWN ? format_from_dxgi(desc.Format) : forced_format;
    surface->frame_type = AMF_FRAME_PROGRESSIVE;
    return surface;
}

/* Context implementation */
static amf_long AMF_STD_CALL context_acquire(AMFContext *iface)
{
    return InterlockedIncrement(&((BridgeContext *)iface)->refs);
}

static amf_long AMF_STD_CALL context_release(AMFContext *iface)
{
    BridgeContext *context = (BridgeContext *)iface;
    LONG refs = InterlockedDecrement(&context->refs);
    if (!refs) {
        property_store_clear(&context->properties);
        if (context->immediate) ID3D11DeviceContext_Release(context->immediate);
        if (context->device) ID3D11Device_Release(context->device);
        free(context);
    }
    return refs;
}

static AMF_RESULT AMF_STD_CALL context_query_interface(AMFContext *iface, const AMFGuid *iid, void **out)
{
    AMFGuid context_iid = IID_AMFContext();
    AMFGuid storage_iid = IID_AMFPropertyStorage();
    AMFGuid interface_iid = IID_AMFInterface();
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (guid_equal(iid, &context_iid) || guid_equal(iid, &storage_iid) || guid_equal(iid, &interface_iid)) {
        *out = iface;
        context_acquire(iface);
        return AMF_OK;
    }
    return AMF_NO_INTERFACE;
}

static AMF_RESULT AMF_STD_CALL context_terminate(AMFContext *iface) { (void)iface; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL context_unsupported(AMFContext *iface) { (void)iface; return AMF_NOT_SUPPORTED; }
static void *AMF_STD_CALL context_null_device(AMFContext *iface, AMF_DX_VERSION version) { (void)iface; (void)version; return NULL; }
static void *AMF_STD_CALL context_null_ptr(AMFContext *iface) { (void)iface; return NULL; }
static AMF_RESULT AMF_STD_CALL context_init_dx9(AMFContext *iface, void *device) { (void)iface; (void)device; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_init_dx11(AMFContext *iface, void *native_device, AMF_DX_VERSION version)
{
    BridgeContext *context = (BridgeContext *)iface;
    ID3D11Device *device = native_device;
    (void)version;
    if (!device) return AMF_INVALID_ARG;
    if (context->immediate) { ID3D11DeviceContext_Release(context->immediate); context->immediate = NULL; }
    if (context->device) {
        ID3D11Device_Release(context->device);
    }
    context->device = device;
    ID3D11Device_AddRef(device);
    ID3D11Device_GetImmediateContext(device, &context->immediate);
    if (FAILED(dxva_bridge_install(device, context->immediate))) {
        bridge_log("error", "DXVA11 bridge installation failed");
    } else {
        bridge_log("info", "DXVA11 bridge installed");
    }
    bridge_log("info", "AMF context initialized with D3D11 device");
    return AMF_OK;
}
static void *AMF_STD_CALL context_get_dx11(AMFContext *iface, AMF_DX_VERSION version) { (void)version; return ((BridgeContext *)iface)->device; }
static AMF_RESULT AMF_STD_CALL context_ok(AMFContext *iface) { (void)iface; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL context_init_ptr(AMFContext *iface, void *value) { (void)iface; (void)value; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_init_opencl_ex(AMFContext *iface, AMFComputeDevice *device) { (void)iface; (void)device; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_compute_factory(AMFContext *iface, AMFComputeFactory **out) { (void)iface; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_init_gl(AMFContext *iface, amf_handle context, amf_handle window, amf_handle dc) { (void)iface; (void)context; (void)window; (void)dc; return AMF_NOT_SUPPORTED; }
static amf_handle AMF_STD_CALL context_null_handle(AMFContext *iface) { (void)iface; return NULL; }
static AMF_RESULT AMF_STD_CALL context_alloc_buffer(AMFContext *iface, AMF_MEMORY_TYPE type, amf_size size, AMFBuffer **out)
{
    BridgeBuffer *buffer;
    (void)iface;
    if (!out || type != AMF_MEMORY_HOST) return AMF_INVALID_ARG;
    buffer = buffer_create(NULL, size, 0, 0);
    if (!buffer) return AMF_OUT_OF_MEMORY;
    *out = &buffer->iface;
    return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL context_create_surface_dx11(AMFContext *iface, void *native, AMFSurface **out, AMFSurfaceObserver *observer)
{
    BridgeSurface *surface;
    (void)iface; (void)observer;
    if (!native || !out) return AMF_INVALID_ARG;
    surface = surface_wrap((ID3D11Texture2D *)native, AMF_SURFACE_UNKNOWN);
    if (!surface) return AMF_OUT_OF_MEMORY;
    *out = &surface->iface;
    if (surface->format == AMF_SURFACE_UNKNOWN) {
        bridge_log("error", "unsupported wrapped D3D11 surface format");
        return AMF_INVALID_FORMAT;
    }
    bridge_log("info", "wrapped D3D11 surface");
    return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL context_alloc_surface(AMFContext *iface, AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, AMFSurface **out)
{
    BridgeContext *context = (BridgeContext *)iface;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *texture = NULL;
    BridgeSurface *surface;
    HRESULT hr;
    if (!out || type != AMF_MEMORY_DX11 || !context->device) return AMF_INVALID_ARG;
    if (format != AMF_SURFACE_NV12) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    memset(&desc, 0, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    hr = ID3D11Device_CreateTexture2D(context->device, &desc, NULL, &texture);
    if (FAILED(hr)) {
        bridge_log("error", "D3D11 NV12 surface allocation failed");
        return AMF_DIRECTX_FAILED;
    }
    surface = surface_wrap(texture, format);
    ID3D11Texture2D_Release(texture);
    if (!surface) return AMF_OUT_OF_MEMORY;
    *out = &surface->iface;
    bridge_log("info", "allocated D3D11 NV12 surface");
    return AMF_OK;
}
static AMF_RESULT AMF_STD_CALL context_alloc_audio(AMFContext *iface, AMF_MEMORY_TYPE type, AMF_AUDIO_FORMAT format, amf_int32 samples, amf_int32 rate, amf_int32 channels, AMFAudioBuffer **out) { (void)iface; (void)type; (void)format; (void)samples; (void)rate; (void)channels; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_buffer_host(AMFContext *iface, void *data, amf_size size, AMFBuffer **out, AMFBufferObserver *observer) { (void)iface; (void)observer; BridgeBuffer *buffer = buffer_create(data, size, 0, 0); if (!buffer) return AMF_OUT_OF_MEMORY; *out = &buffer->iface; return AMF_OK; }
static AMF_RESULT AMF_STD_CALL context_surface_host(AMFContext *iface, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf_int32 hpitch, amf_int32 vpitch, void *data, AMFSurface **out, AMFSurfaceObserver *observer) { (void)iface; (void)format; (void)width; (void)height; (void)hpitch; (void)vpitch; (void)data; (void)out; (void)observer; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_surface_dx9(AMFContext *iface, void *native, AMFSurface **out, AMFSurfaceObserver *observer) { (void)iface; (void)native; (void)out; (void)observer; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_surface_gl(AMFContext *iface, AMF_SURFACE_FORMAT format, amf_handle texture, AMFSurface **out, AMFSurfaceObserver *observer) { (void)iface; (void)format; (void)texture; (void)out; (void)observer; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_surface_handle(AMFContext *iface, amf_handle native, AMFSurface **out, AMFSurfaceObserver *observer) { (void)iface; (void)native; (void)out; (void)observer; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_surface_opencl(AMFContext *iface, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, void **planes, AMFSurface **out, AMFSurfaceObserver *observer) { (void)iface; (void)format; (void)width; (void)height; (void)planes; (void)out; (void)observer; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_buffer_opencl(AMFContext *iface, void *native, amf_size size, AMFBuffer **out) { (void)iface; (void)native; (void)size; (void)out; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL context_get_compute(AMFContext *iface, AMF_MEMORY_TYPE type, AMFCompute **out) { (void)iface; (void)type; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }

static const AMFContextVtbl context_vtbl = {
    context_acquire, context_release, context_query_interface,
    context_prop_set, context_prop_get, context_prop_has, context_prop_count, context_prop_at,
    context_prop_clear, context_prop_add_to, context_prop_copy_to,
    context_prop_add_observer, context_prop_remove_observer,
    context_terminate,
    context_init_dx9, context_null_device, context_ok, context_ok,
    context_init_dx11, context_get_dx11, context_ok, context_ok,
    context_init_ptr, context_null_ptr, context_null_ptr, context_null_ptr,
    context_compute_factory, context_init_opencl_ex, context_unsupported, context_unsupported,
    context_init_gl, context_null_handle, context_null_handle, context_unsupported, context_unsupported,
    context_init_ptr, context_null_ptr, context_unsupported, context_unsupported,
    context_init_ptr, context_null_ptr, context_unsupported, context_unsupported,
    context_alloc_buffer, context_alloc_surface, context_alloc_audio,
    context_buffer_host, context_surface_host, context_surface_dx9, context_create_surface_dx11,
    context_surface_gl, context_surface_handle, context_surface_opencl, context_buffer_opencl,
    context_get_compute
};

static BridgeContext *context_create(void)
{
    BridgeContext *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->iface.pVtbl = &context_vtbl;
    context->refs = 1;
    return context;
}

/* Encoder component implementation */
static int component_queue_output(BridgeComponent *component, BridgeBuffer *buffer)
{
    size_t index;
    if (component->output_count == BRIDGE_MAX_OUTPUTS) return -1;
    index = (component->output_head + component->output_count) % BRIDGE_MAX_OUTPUTS;
    component->outputs[index] = buffer;
    ++component->output_count;
    return 0;
}

static void component_close_codec(BridgeComponent *component)
{
#ifdef BRIDGE_NATIVE_PE
    if (component->helper != INVALID_SOCKET) {
        closesocket(component->helper);
        component->helper = INVALID_SOCKET;
    }
#else
    size_t i;
    avcodec_free_context(&component->codec);
    av_buffer_unref(&component->hw_frames);
    av_buffer_unref(&component->hw_device);
    for (i = 0; i < component->output_count; ++i) {
        size_t index = (component->output_head + i) % BRIDGE_MAX_OUTPUTS;
        if (component->outputs[index]) buffer_release(&component->outputs[index]->iface);
    }
    memset(component->outputs, 0, sizeof(component->outputs));
    component->output_head = 0;
    component->output_count = 0;
#endif
    component->initialized = 0;
}

static amf_long AMF_STD_CALL component_acquire(AMFComponent *iface)
{
    return InterlockedIncrement(&((BridgeComponent *)iface)->refs);
}

static amf_long AMF_STD_CALL component_release(AMFComponent *iface)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    LONG refs = InterlockedDecrement(&component->refs);
    if (!refs) {
        component_close_codec(component);
        property_store_clear(&component->properties);
        if (component->context) context_release(&component->context->iface);
        free(component);
    }
    return refs;
}

static AMF_RESULT AMF_STD_CALL component_query_interface(AMFComponent *iface, const AMFGuid *iid, void **out)
{
    AMFGuid component_iid = IID_AMFComponent();
    AMFGuid storage_iid = IID_AMFPropertyStorage();
    AMFGuid storage_ex_iid = IID_AMFPropertyStorageEx();
    AMFGuid interface_iid = IID_AMFInterface();
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (guid_equal(iid, &component_iid) || guid_equal(iid, &storage_iid) ||
        guid_equal(iid, &storage_ex_iid) || guid_equal(iid, &interface_iid)) {
        *out = iface;
        component_acquire(iface);
        return AMF_OK;
    }
    return AMF_NO_INTERFACE;
}

static amf_size AMF_STD_CALL component_info_count(AMFComponent *iface) { (void)iface; return 0; }
static AMF_RESULT AMF_STD_CALL component_info_at(AMFComponent *iface, amf_size index, const AMFPropertyInfo **out) { (void)iface; (void)index; if (out) *out = NULL; return AMF_OUT_OF_RANGE; }
static AMF_RESULT AMF_STD_CALL component_info(AMFComponent *iface, const wchar_t *name, const AMFPropertyInfo **out) { (void)iface; (void)name; if (out) *out = NULL; return AMF_NOT_FOUND; }
static AMF_RESULT AMF_STD_CALL component_validate(AMFComponent *iface, const wchar_t *name, AMFVariantStruct value, AMFVariantStruct *out) { (void)iface; (void)name; if (!out) return AMF_INVALID_POINTER; AMFVariantInit(out); return AMFVariantCopy(out, &value); }

static AMF_RESULT AMF_STD_CALL component_init(AMFComponent *iface, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height)
{
    BridgeComponent *component = (BridgeComponent *)iface;
#ifdef BRIDGE_NATIVE_PE
    struct sockaddr_in address;
    const char *host = getenv("UU_AMF_HELPER_HOST");
    const char *port_text = getenv("UU_AMF_HELPER_PORT");
    unsigned long port = parse_port(port_text, HELPER_DEFAULT_PORT);
    HelperInitMessage init_message;
    AMFRate default_rate = {30, 1};
    AMFRate rate;
    amf_int64 bitrate;
    int enable = 1;
    if (component->initialized) return AMF_ALREADY_INITIALIZED;
    if (format != AMF_SURFACE_NV12 || width < 128 || height < 128) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    if (component->helper == INVALID_SOCKET) {
        component->helper = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (component->helper == INVALID_SOCKET) return AMF_ENCODER_NOT_PRESENT;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons((u_short)port);
        if (!host || !*host) host = "127.0.0.1";
        if (InetPtonA(AF_INET, host, &address.sin_addr) != 1 || connect(component->helper, (struct sockaddr *)&address, sizeof(address)) != 0) {
            closesocket(component->helper);
            component->helper = INVALID_SOCKET;
            return AMF_ENCODER_NOT_PRESENT;
        }
    }
    rate = property_get_rate(&component->properties, AMF_VIDEO_ENCODER_FRAMERATE, default_rate);
    bitrate = property_get_i64(&component->properties, AMF_VIDEO_ENCODER_TARGET_BITRATE, 8000000);
    memset(&init_message, 0, sizeof(init_message));
    init_message.magic = HELPER_MAGIC;
    init_message.type = HELPER_INIT;
    init_message.width = width;
    init_message.height = height;
    init_message.format = AMF_SURFACE_NV12;
    init_message.bitrate = bitrate;
    init_message.fps_num = rate.num > 0 ? (uint32_t)rate.num : 30;
    init_message.fps_den = rate.den > 0 ? (uint32_t)rate.den : 1;
    if (!socket_send_all(component->helper, &init_message, sizeof(init_message)) || !socket_recv_all(component->helper, &enable, sizeof(enable)) || !enable) {
        component_close_codec(component);
        return AMF_ENCODER_NOT_PRESENT;
    }
    component->width = width;
    component->height = height;
    component->format = format;
    component->next_pts = 0;
    component->initialized = 1;
    bridge_log("info", "connected to Vulkan Video helper");
    return AMF_OK;
#else
    const AVCodec *codec;
    AVBufferRef *frames_ref;
    AVHWFramesContext *frames;
    AMFRate default_rate = {30, 1};
    AMFRate rate;
    int error;
    char error_text[AV_ERROR_MAX_STRING_SIZE];
    if (component->initialized) return AMF_ALREADY_INITIALIZED;
    if (format != AMF_SURFACE_NV12 || width < 128 || height < 128) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    codec = avcodec_find_encoder_by_name("h264_vulkan");
    if (!codec) return AMF_ENCODER_NOT_PRESENT;
    error = av_hwdevice_ctx_create(&component->hw_device, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0);
    if (error < 0) goto fail;
    frames_ref = av_hwframe_ctx_alloc(component->hw_device);
    if (!frames_ref) { error = AVERROR(ENOMEM); goto fail; }
    frames = (AVHWFramesContext *)frames_ref->data;
    frames->format = AV_PIX_FMT_VULKAN;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = 8;
    error = av_hwframe_ctx_init(frames_ref);
    if (error < 0) { av_buffer_unref(&frames_ref); goto fail; }
    component->hw_frames = frames_ref;
    component->codec = avcodec_alloc_context3(codec);
    if (!component->codec) { error = AVERROR(ENOMEM); goto fail; }
    rate = property_get_rate(&component->properties, AMF_VIDEO_ENCODER_FRAMERATE, default_rate);
    if (rate.num <= 0 || rate.den <= 0) rate = default_rate;
    component->codec->width = width;
    component->codec->height = height;
    component->codec->pix_fmt = AV_PIX_FMT_VULKAN;
    component->codec->hw_frames_ctx = av_buffer_ref(component->hw_frames);
    component->codec->time_base = (AVRational){rate.den, rate.num};
    component->codec->framerate = (AVRational){rate.num, rate.den};
    component->codec->bit_rate = property_get_i64(&component->properties, AMF_VIDEO_ENCODER_TARGET_BITRATE, 8000000);
    component->codec->rc_max_rate = property_get_i64(&component->properties, AMF_VIDEO_ENCODER_PEAK_BITRATE, component->codec->bit_rate);
    component->codec->rc_buffer_size = property_get_i64(&component->properties, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, component->codec->bit_rate);
    component->codec->gop_size = 60;
    component->codec->max_b_frames = 0;
    component->codec->profile = AV_PROFILE_H264_HIGH;
    av_opt_set_int(component->codec->priv_data, "quality", 0, 0);
    error = avcodec_open2(component->codec, codec, NULL);
    if (error < 0) goto fail;
    component->width = width;
    component->height = height;
    component->format = format;
    component->next_pts = 0;
    component->initialized = 1;
    bridge_log("info", "h264_vulkan encoder initialized");
    return AMF_OK;
fail:
    av_strerror(error, error_text, sizeof(error_text));
    bridge_log("error", error_text);
    component_close_codec(component);
    return AMF_ENCODER_NOT_PRESENT;
#endif
}

static AMF_RESULT AMF_STD_CALL component_reinit(AMFComponent *iface, amf_int32 width, amf_int32 height)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    AMF_SURFACE_FORMAT format = component->format;
    component_close_codec(component);
    return component_init(iface, format, width, height);
}

static AMF_RESULT AMF_STD_CALL component_terminate(AMFComponent *iface) { component_close_codec((BridgeComponent *)iface); return AMF_OK; }

#ifndef BRIDGE_NATIVE_PE
static AMF_RESULT component_receive_packets(BridgeComponent *component, amf_pts pts, amf_pts duration)
{
    AVPacket *packet = av_packet_alloc();
    int error;
    if (!packet) return AMF_OUT_OF_MEMORY;
    for (;;) {
        BridgeBuffer *buffer;
        error = avcodec_receive_packet(component->codec, packet);
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) break;
        if (error < 0) { av_packet_free(&packet); return AMF_FAIL; }
        buffer = buffer_create(packet->data, packet->size, pts, duration);
        av_packet_unref(packet);
        if (!buffer) { av_packet_free(&packet); return AMF_OUT_OF_MEMORY; }
        if (component_queue_output(component, buffer) < 0) {
            buffer_release(&buffer->iface);
            av_packet_free(&packet);
            return AMF_INPUT_FULL;
        }
    }
    av_packet_free(&packet);
    return AMF_OK;
}
#endif

static AMF_RESULT AMF_STD_CALL component_drain(AMFComponent *iface)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    if (!component->initialized) return AMF_NOT_INITIALIZED;
#ifdef BRIDGE_NATIVE_PE
    component->draining = 1;
    return AMF_OK;
#else
    component->draining = 1;
    avcodec_send_frame(component->codec, NULL);
    return component_receive_packets(component, 0, 0);
#endif
}

static AMF_RESULT AMF_STD_CALL component_flush(AMFComponent *iface)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    if (!component->initialized) return AMF_NOT_INITIALIZED;
#ifdef BRIDGE_NATIVE_PE
    component->draining = 0;
    return AMF_OK;
#else
    avcodec_flush_buffers(component->codec);
    component->draining = 0;
    return AMF_OK;
#endif
}

#ifndef BRIDGE_NATIVE_PE
static AMF_RESULT map_surface_nv12(BridgeComponent *component, BridgeSurface *surface, AVFrame *frame)
{
    D3D11_TEXTURE2D_DESC source_desc;
    D3D11_TEXTURE2D_DESC staging_desc;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    int y;
    ID3D11Texture2D_GetDesc(surface->texture, &source_desc);
    if (source_desc.Format != DXGI_FORMAT_NV12 || source_desc.Width != (UINT)component->width || source_desc.Height != (UINT)component->height) {
        return AMF_INVALID_FORMAT;
    }
    staging_desc = source_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(component->context->device, &staging_desc, NULL, &staging);
    if (FAILED(hr)) return AMF_DIRECTX_FAILED;
    ID3D11DeviceContext_CopyResource(component->context->immediate, (ID3D11Resource *)staging, (ID3D11Resource *)surface->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(component->context->immediate, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { ID3D11Texture2D_Release(staging); return AMF_DIRECTX_FAILED; }
    for (y = 0; y < component->height; ++y) {
        memcpy(frame->data[0] + y * frame->linesize[0], (uint8_t *)mapped.pData + y * mapped.RowPitch, component->width);
    }
    for (y = 0; y < component->height / 2; ++y) {
        memcpy(frame->data[1] + y * frame->linesize[1], (uint8_t *)mapped.pData + (component->height + y) * mapped.RowPitch, component->width);
    }
    ID3D11DeviceContext_Unmap(component->context->immediate, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    return AMF_OK;
}
#else
static AMF_RESULT map_surface_bytes(BridgeComponent *component, BridgeSurface *surface, uint8_t **out_data, uint32_t *out_size)
{
    D3D11_TEXTURE2D_DESC source_desc;
    D3D11_TEXTURE2D_DESC staging_desc;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint8_t *data;
    HRESULT hr;
    int y;
    ID3D11Texture2D_GetDesc(surface->texture, &source_desc);
    if (source_desc.Format != DXGI_FORMAT_NV12 || source_desc.Width != (UINT)component->width || source_desc.Height != (UINT)component->height) return AMF_INVALID_FORMAT;
    staging_desc = source_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(component->context->device, &staging_desc, NULL, &staging);
    if (FAILED(hr)) return AMF_DIRECTX_FAILED;
    ID3D11DeviceContext_CopyResource(component->context->immediate, (ID3D11Resource *)staging, (ID3D11Resource *)surface->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(component->context->immediate, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { ID3D11Texture2D_Release(staging); return AMF_DIRECTX_FAILED; }
    *out_size = component->width * component->height * 3 / 2;
    data = malloc(*out_size);
    if (!data) { ID3D11DeviceContext_Unmap(component->context->immediate, (ID3D11Resource *)staging, 0); ID3D11Texture2D_Release(staging); return AMF_OUT_OF_MEMORY; }
    for (y = 0; y < component->height; ++y) memcpy(data + y * component->width, (uint8_t *)mapped.pData + y * mapped.RowPitch, component->width);
    for (y = 0; y < component->height / 2; ++y) memcpy(data + component->width * component->height + y * component->width, (uint8_t *)mapped.pData + (component->height + y) * mapped.RowPitch, component->width);
    ID3D11DeviceContext_Unmap(component->context->immediate, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    *out_data = data;
    return AMF_OK;
}
#endif

static AMF_RESULT AMF_STD_CALL component_submit(AMFComponent *iface, AMFData *data)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    BridgeSurface *surface = (BridgeSurface *)data;
#ifdef BRIDGE_NATIVE_PE
    HelperFrameMessage header;
    HelperReply reply;
    uint8_t *frame_data = NULL;
    uint32_t frame_size = 0;
    uint8_t *packet_data = NULL;
    AMF_RESULT result;
    if (!component->initialized) return AMF_NOT_INITIALIZED;
    if (!data || data->pVtbl != (const AMFDataVtbl *)&surface_vtbl) return AMF_INVALID_DATA_TYPE;
    result = map_surface_bytes(component, surface, &frame_data, &frame_size);
    if (result != AMF_OK) {
        bridge_log("error", "failed to map D3D11 NV12 surface");
        return result;
    }
    bridge_log("info", "submitting NV12 frame to helper");
    memset(&header, 0, sizeof(header));
    header.magic = HELPER_MAGIC;
    header.type = HELPER_FRAME;
    header.payload_size = frame_size;
    header.pts = surface->pts;
    header.duration = surface->duration;
    if (!socket_send_all(component->helper, &header, sizeof(header)) || !socket_send_all(component->helper, frame_data, frame_size) || !socket_recv_all(component->helper, &reply, sizeof(reply)) || reply.magic != HELPER_MAGIC || reply.type != HELPER_REPLY || reply.status != 0) {
        free(frame_data);
        bridge_log("error", "helper rejected NV12 frame");
        return AMF_FAIL;
    }
    free(frame_data);
    if (reply.payload_size) {
        packet_data = malloc(reply.payload_size);
        if (!packet_data || !socket_recv_all(component->helper, packet_data, reply.payload_size)) { free(packet_data); return AMF_FAIL; }
        {
            BridgeBuffer *buffer = buffer_create(packet_data, reply.payload_size, surface->pts, surface->duration);
            free(packet_data);
            if (!buffer) return AMF_OUT_OF_MEMORY;
            if (component_queue_output(component, buffer) < 0) { buffer_release(&buffer->iface); return AMF_INPUT_FULL; }
        }
        bridge_log("info", "queued encoded H.264 packet");
    } else {
        bridge_log("info", "encoder accepted frame without immediate packet");
    }
    return AMF_OK;
#else
    AVFrame *host = NULL;
    AVFrame *gpu = NULL;
    AMF_RESULT result;
    int error;
    if (!component->initialized) return AMF_NOT_INITIALIZED;
    if (!data || data->pVtbl != (const AMFDataVtbl *)&surface_vtbl) return AMF_INVALID_DATA_TYPE;
    if (component->output_count == BRIDGE_MAX_OUTPUTS) return AMF_INPUT_FULL;
    host = av_frame_alloc();
    gpu = av_frame_alloc();
    if (!host || !gpu) { result = AMF_OUT_OF_MEMORY; goto done; }
    host->format = AV_PIX_FMT_NV12;
    host->width = component->width;
    host->height = component->height;
    error = av_frame_get_buffer(host, 32);
    if (error < 0) { result = AMF_OUT_OF_MEMORY; goto done; }
    result = map_surface_nv12(component, surface, host);
    if (result != AMF_OK) goto done;
    error = av_hwframe_get_buffer(component->hw_frames, gpu, 0);
    if (error < 0) { result = AMF_VULKAN_FAILED; goto done; }
    error = av_hwframe_transfer_data(gpu, host, 0);
    if (error < 0) { result = AMF_VULKAN_FAILED; goto done; }
    gpu->pts = surface->pts ? av_rescale_q(surface->pts, (AVRational){1, AMF_TIME_BASE}, component->codec->time_base) : component->next_pts;
    component->next_pts = gpu->pts + 1;
    error = avcodec_send_frame(component->codec, gpu);
    if (error == AVERROR(EAGAIN)) { result = AMF_INPUT_FULL; goto done; }
    if (error < 0) { result = AMF_FAIL; goto done; }
    result = component_receive_packets(component, surface->pts, surface->duration);
done:
    av_frame_free(&gpu);
    av_frame_free(&host);
    return result;
#endif
}

static AMF_RESULT AMF_STD_CALL component_query_output(AMFComponent *iface, AMFData **out)
{
    BridgeComponent *component = (BridgeComponent *)iface;
    BridgeBuffer *buffer;
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!component->output_count) return component->draining ? AMF_EOF : AMF_REPEAT;
    buffer = component->outputs[component->output_head];
    component->outputs[component->output_head] = NULL;
    component->output_head = (component->output_head + 1) % BRIDGE_MAX_OUTPUTS;
    --component->output_count;
    *out = (AMFData *)&buffer->iface;
    return AMF_OK;
}

static AMFContext *AMF_STD_CALL component_get_context(AMFComponent *iface) { return &((BridgeComponent *)iface)->context->iface; }
static AMF_RESULT AMF_STD_CALL component_allocator(AMFComponent *iface, AMFDataAllocatorCB *callback) { (void)iface; (void)callback; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL component_caps(AMFComponent *iface, AMFCaps **out) { (void)iface; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL component_optimize(AMFComponent *iface, AMFComponentOptimizationCallback *callback) { (void)iface; (void)callback; return AMF_OK; }

static const AMFComponentVtbl component_vtbl = {
    component_acquire, component_release, component_query_interface,
    component_prop_set, component_prop_get, component_prop_has, component_prop_count, component_prop_at,
    component_prop_clear, component_prop_add_to, component_prop_copy_to,
    component_prop_add_observer, component_prop_remove_observer,
    component_info_count, component_info_at, component_info, component_validate,
    component_init, component_reinit, component_terminate, component_drain, component_flush,
    component_submit, component_query_output, component_get_context, component_allocator,
    component_caps, component_optimize
};

static BridgeComponent *component_create(BridgeContext *context)
{
    BridgeComponent *component = calloc(1, sizeof(*component));
    if (!component) return NULL;
    component->iface.pVtbl = &component_vtbl;
    component->refs = 1;
    component->context = context;
#ifdef BRIDGE_NATIVE_PE
    component->helper = INVALID_SOCKET;
#endif
    context_acquire(&context->iface);
    return component;
}

/* Factory and exported AMF runtime entry points */
static AMF_RESULT AMF_STD_CALL factory_create_context(AMFFactory *factory, AMFContext **out)
{
    BridgeContext *context;
    (void)factory;
    if (!out) return AMF_INVALID_POINTER;
    context = context_create();
    if (!context) return AMF_OUT_OF_MEMORY;
    *out = &context->iface;
    return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL factory_create_component(AMFFactory *factory, AMFContext *context_iface, const wchar_t *id, AMFComponent **out)
{
    BridgeComponent *component;
    (void)factory;
    if (!context_iface || !id || !out) return AMF_INVALID_ARG;
    *out = NULL;
    if (wcscmp(id, AMFVideoEncoderVCE_AVC) != 0) return AMF_NOT_SUPPORTED;
    component = component_create((BridgeContext *)context_iface);
    if (!component) return AMF_OUT_OF_MEMORY;
    *out = &component->iface;
    return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL factory_set_cache(AMFFactory *factory, const wchar_t *path) { (void)factory; (void)path; return AMF_OK; }
static const wchar_t *AMF_STD_CALL factory_get_cache(AMFFactory *factory) { (void)factory; return L""; }
static AMF_RESULT AMF_STD_CALL factory_get_debug(AMFFactory *factory, AMFDebug **out) { (void)factory; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL factory_get_trace(AMFFactory *factory, AMFTrace **out) { (void)factory; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }
static AMF_RESULT AMF_STD_CALL factory_get_programs(AMFFactory *factory, AMFPrograms **out) { (void)factory; if (out) *out = NULL; return AMF_NOT_SUPPORTED; }

static const AMFFactoryVtbl factory_vtbl = {
    factory_create_context, factory_create_component,
    factory_set_cache, factory_get_cache,
    factory_get_debug, factory_get_trace, factory_get_programs
};

static AMFFactory bridge_factory = { &factory_vtbl };

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFQueryVersion(amf_uint64 *version)
{
    if (!version) return AMF_INVALID_POINTER;
    *version = AMF_FULL_VERSION;
    return AMF_OK;
}

__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFInit(amf_uint64 version, AMFFactory **factory)
{
#ifdef BRIDGE_NATIVE_PE
    WSADATA winsock_data;
#endif
    if (!factory) return AMF_INVALID_POINTER;
    if (version > AMF_FULL_VERSION) return AMF_NOT_SUPPORTED;
#ifdef BRIDGE_NATIVE_PE
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) return AMF_FAIL;
#endif
    *factory = &bridge_factory;
    bridge_log("info", "UU AMF bridge loaded");
    return AMF_OK;
}
