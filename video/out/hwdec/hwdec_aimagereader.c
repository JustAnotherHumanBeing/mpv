/*
 * Copyright (c) 2021 sfan5 <sfan5@live.de>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <EGL/egl.h>
#include <media/NdkImageReader.h>
#include <android/native_window_jni.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/opengl/ra_gl.h"

typedef void *GLeglImageOES;
typedef void *EGLImageKHR;
#define EGL_NATIVE_BUFFER_ANDROID 0x3140

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    AImageReader *reader;
    jobject surface;
    void *lib_handle;

    media_status_t (*AImageReader_newWithUsage)(
        int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    media_status_t (*AImageReader_getWindow)(
        AImageReader *, ANativeWindow **);
    media_status_t (*AImageReader_setImageListener)(
        AImageReader *, AImageReader_ImageListener *);
    media_status_t (*AImageReader_acquireLatestImage)(AImageReader *, AImage **);
    void (*AImageReader_delete)(AImageReader *);
    media_status_t (*AImage_getHardwareBuffer)(const AImage *, AHardwareBuffer **);
    void (*AImage_delete)(AImage *);
    void (*AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    jobject (*ANativeWindow_toSurface)(JNIEnv *, ANativeWindow *);

    media_status_t (*AImage_getWidth)(const AImage *, int32_t *);
    media_status_t (*AImage_getHeight)(const AImage *, int32_t *);
    media_status_t (*AImage_getFormat)(const AImage *, int32_t *);
    media_status_t (*AImage_getTimestamp)(const AImage *, int64_t *);
    media_status_t (*AImage_getDataSpace)(const AImage *, int32_t *);
    media_status_t (*AImage_getNumberOfPlanes)(const AImage *, int32_t *);
    media_status_t (*AImage_getPlanePixelStride)(const AImage *, int,
                                                 int32_t *);
    media_status_t (*AImage_getPlaneRowStride)(const AImage *, int, int32_t *);
    media_status_t (*AImage_getCropRect)(const AImage *, AImageCropRect *);
};

struct priv {
    struct mp_log *log;

    GLuint gl_texture;
    AImage *image;
    EGLImageKHR egl_image;

    mp_mutex lock;
    mp_cond cond;
    bool image_available;
    uint64_t image_serial;

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(
        EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLClientBuffer (EGLAPIENTRY *GetNativeClientBufferANDROID)(
        const struct AHardwareBuffer *);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static const struct {
    const char *symbol;
    int offset;
    bool required;
} lib_functions[] = {
    { "AImageReader_newWithUsage", offsetof(struct priv_owner, AImageReader_newWithUsage), true },
    { "AImageReader_getWindow", offsetof(struct priv_owner, AImageReader_getWindow), true },
    { "AImageReader_setImageListener", offsetof(struct priv_owner, AImageReader_setImageListener), true },
    { "AImageReader_acquireLatestImage", offsetof(struct priv_owner, AImageReader_acquireLatestImage), true },
    { "AImageReader_delete", offsetof(struct priv_owner, AImageReader_delete), true },
    { "AImage_getHardwareBuffer", offsetof(struct priv_owner, AImage_getHardwareBuffer), true },
    { "AImage_delete", offsetof(struct priv_owner, AImage_delete), true },
    { "AHardwareBuffer_describe", offsetof(struct priv_owner, AHardwareBuffer_describe), true },
    { "ANativeWindow_toSurface", offsetof(struct priv_owner, ANativeWindow_toSurface), true },
    { "AImage_getWidth", offsetof(struct priv_owner, AImage_getWidth), false },
    { "AImage_getHeight", offsetof(struct priv_owner, AImage_getHeight), false },
    { "AImage_getFormat", offsetof(struct priv_owner, AImage_getFormat), false },
    { "AImage_getTimestamp", offsetof(struct priv_owner, AImage_getTimestamp), false },
    { "AImage_getDataSpace", offsetof(struct priv_owner, AImage_getDataSpace), false },
    { "AImage_getNumberOfPlanes", offsetof(struct priv_owner, AImage_getNumberOfPlanes), false },
    { "AImage_getPlanePixelStride", offsetof(struct priv_owner, AImage_getPlanePixelStride), false },
    { "AImage_getPlaneRowStride", offsetof(struct priv_owner, AImage_getPlaneRowStride), false },
    { "AImage_getCropRect", offsetof(struct priv_owner, AImage_getCropRect), false },
    { NULL, 0, false },
};


static AVBufferRef *create_mediacodec_device_ref(jobject surface)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    hwctx->surface = surface;

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static bool load_lib_functions(struct priv_owner *p, struct mp_log *log)
{
    p->lib_handle = dlopen("libmediandk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!p->lib_handle)
        return false;
    for (int i = 0; lib_functions[i].symbol; i++) {
        const char *sym = lib_functions[i].symbol;
        void *fun = dlsym(p->lib_handle, sym);
        if (!fun)
            fun = dlsym(RTLD_DEFAULT, sym);
        if (!fun && lib_functions[i].required) {
            mp_warn(log, "Could not resolve symbol %s\n", sym);
            return false;
        }

        if (fun)
            *(void **) ((uint8_t*)p + lib_functions[i].offset) = fun;
    }
    return true;
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (!ra_is_gl(hw->ra_ctx->ra))
        return -1;
    if (!eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!gl_check_extension(exts, "EGL_ANDROID_image_native_buffer"))
        return -1;

    JNIEnv *env = MP_JNI_GET_ENV(hw);
    if (!env)
        return -1;

    if (!load_lib_functions(p, hw->log))
        return -1;

    static const char *es2_exts[] = {"GL_OES_EGL_image_external", 0};
    static const char *es3_exts[] = {"GL_OES_EGL_image_external_essl3", 0};
    GL *gl = ra_gl_get(hw->ra_ctx->ra);
    if (gl_check_extension(gl->extensions, es3_exts[0]))
        hw->glsl_extensions = es3_exts;
    else
        hw->glsl_extensions = es2_exts;

    // Dummy dimensions: AImageReader only transports hardware buffers.
    const int32_t reader_width = 16;
    const int32_t reader_height = 16;
    const int32_t reader_format = AIMAGE_FORMAT_PRIVATE;
    const uint64_t reader_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    const int32_t reader_max_images = 5;
    MP_TRACE(hw,
             "[dovi-diag] AImageReader config size=%dx%d format=%#x "
             "usage=%#"PRIx64" max-images=%d\n",
             reader_width, reader_height, reader_format, reader_usage,
             reader_max_images);
    media_status_t ret = p->AImageReader_newWithUsage(
        reader_width, reader_height, reader_format, reader_usage,
        reader_max_images, &p->reader);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "newWithUsage failed: %d\n", ret);
        return -1;
    }
    mp_assert(p->reader);

    ANativeWindow *window;
    ret = p->AImageReader_getWindow(p->reader, &window);
    if (ret != AMEDIA_OK) {
        MP_ERR(hw, "getWindow failed: %d\n", ret);
        return -1;
    }
    mp_assert(window);

    jobject surface = p->ANativeWindow_toSurface(env, window);
    p->surface = (*env)->NewGlobalRef(env, surface);
    (*env)->DeleteLocalRef(env, surface);

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = create_mediacodec_device_ref(p->surface),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(hw->devs, &p->hwctx);

    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;

    if (p->surface) {
        JNIEnv *env = MP_JNI_GET_ENV(hw);
        mp_assert(env);
        (*env)->DeleteGlobalRef(env, p->surface);
        p->surface = NULL;
    }

    if (p->reader) {
        p->AImageReader_delete(p->reader);
        p->reader = NULL;
    }

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    if (p->lib_handle) {
        dlclose(p->lib_handle);
        p->lib_handle = NULL;
    }
}

static void image_callback(void *context, AImageReader *reader)
{
    struct priv *p = context;

    mp_mutex_lock(&p->lock);
    p->image_available = true;
    mp_cond_signal(&p->cond);
    mp_mutex_unlock(&p->lock);
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    p->log = mapper->log;
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->cond);

    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    p->GetNativeClientBufferANDROID =
        (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->GetNativeClientBufferANDROID || !p->EGLImageTargetTexture2DOES)
        return -1;

    AImageReader_ImageListener listener = {
        .context = p,
        .onImageAvailable = image_callback,
    };
    o->AImageReader_setImageListener(o->reader, &listener);

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = IMGFMT_RGB0;
    mapper->dst_params.hw_subfmt = 0;

    // texture creation
    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = ra_find_unorm_format(mapper->ra, 1, 4),
        .render_src = true,
        .src_linear = true,
        .external_oes = true,
    };

    if (params.format->ctype != RA_CTYPE_UNORM)
        return -1;

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    if (!mapper->tex[0])
        return -1;

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    o->AImageReader_setImageListener(o->reader, NULL);

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;

    ra_tex_free(mapper->ra, &mapper->tex[0]);

    mp_mutex_destroy(&p->lock);
    mp_cond_destroy(&p->cond);
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;

    if (p->egl_image) {
        p->DestroyImageKHR(eglGetCurrentDisplay(), p->egl_image);
        p->egl_image = 0;
    }

    if (p->image) {
        o->AImage_delete(p->image);
        p->image = NULL;
    }
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    struct priv_owner *o = mapper->owner->priv;
    GL *gl = ra_gl_get(mapper->ra);

    {
        if (mapper->src->imgfmt != IMGFMT_MEDIACODEC)
            return -1;
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)mapper->src->planes[3];
        av_mediacodec_release_buffer(buffer, 1);
    }

    bool image_available = false;
    mp_mutex_lock(&p->lock);
    if (!p->image_available) {
        mp_cond_timedwait(&p->cond, &p->lock, MP_TIME_MS_TO_NS(100));
        if (!p->image_available)
            MP_WARN(mapper, "Waiting for frame timed out!\n");
    }
    image_available = p->image_available;
    p->image_available = false;
    mp_mutex_unlock(&p->lock);

    media_status_t ret = o->AImageReader_acquireLatestImage(o->reader, &p->image);
    if (ret != AMEDIA_OK) {
        MP_ERR(mapper, "acquireLatestImage failed: %d\n", ret);
        // If we merely timed out waiting return success anyway to avoid
        // flashing frames of render errors.
        return image_available ? -1 : 0;
    }
    mp_assert(p->image);

    AHardwareBuffer *hwbuf = NULL;
    ret = o->AImage_getHardwareBuffer(p->image, &hwbuf);
    if (ret != AMEDIA_OK) {
        MP_ERR(mapper, "getHardwareBuffer failed: %d\n", ret);
        return -1;
    }
    mp_assert(hwbuf);

    // Update texture size since it may differ
    AHardwareBuffer_Desc d = {0};
    o->AHardwareBuffer_describe(hwbuf, &d);
    p->image_serial++;

    int32_t image_width = -1;
    int32_t image_height = -1;
    int32_t image_format = -1;
    int32_t image_dataspace = -1;
    int32_t plane_count = -1;
    int64_t timestamp = -1;
    AImageCropRect crop = {
        .left = -1,
        .top = -1,
        .right = -1,
        .bottom = -1,
    };
    if (o->AImage_getWidth)
        o->AImage_getWidth(p->image, &image_width);
    if (o->AImage_getHeight)
        o->AImage_getHeight(p->image, &image_height);
    if (o->AImage_getFormat)
        o->AImage_getFormat(p->image, &image_format);
    if (o->AImage_getTimestamp)
        o->AImage_getTimestamp(p->image, &timestamp);
    if (o->AImage_getDataSpace)
        o->AImage_getDataSpace(p->image, &image_dataspace);
    if (o->AImage_getNumberOfPlanes)
        o->AImage_getNumberOfPlanes(p->image, &plane_count);
    if (o->AImage_getCropRect)
        o->AImage_getCropRect(p->image, &crop);

    int planes_to_log = plane_count;
    if (planes_to_log < 0 || planes_to_log > 4) {
        if (planes_to_log > 4)
            MP_WARN(mapper, "[dovi-diag] invalid AImage plane count: %d\n",
                    planes_to_log);
        planes_to_log = 0;
    }
    if (planes_to_log > 0) {
        for (int i = 0; i < planes_to_log; i++) {
            int32_t pixel_stride = -1;
            int32_t row_stride = -1;
            if (o->AImage_getPlanePixelStride)
                o->AImage_getPlanePixelStride(p->image, i, &pixel_stride);
            if (o->AImage_getPlaneRowStride)
                o->AImage_getPlaneRowStride(p->image, i, &row_stride);
            MP_TRACE(mapper,
                     "[dovi-diag] AImage=%"PRIu64
                     " plane=%d pixel-stride=%d row-stride=%d\n",
                     p->image_serial, i, pixel_stride, row_stride);
        }
    }
    if (mapper->tex[0]->params.w != d.width || mapper->tex[0]->params.h != d.height) {
        MP_VERBOSE(p, "Texture dimensions changed to %dx%d\n", d.width, d.height);
        mapper->tex[0]->params.w = d.width;
        mapper->tex[0]->params.h = d.height;
    }

    MP_TRACE(mapper,
             "[dovi-diag] AImage=%"PRIu64" ptr=%p timestamp=%"PRId64
             " image=%dx%d format=%#x dataspace=%#x planes=%d "
             "crop=%d,%d-%d,%d "
             "AHB=%ux%u layers=%u format=%#x usage=%#"PRIx64
             " stride=%u texture=%dx%d "
             "transform=[1,0,0,0;0,1,0,0;0,0,1,0;0,0,0,1]\n",
             p->image_serial, p->image, timestamp, image_width, image_height,
             image_format, image_dataspace, plane_count, crop.left, crop.top,
             crop.right, crop.bottom, d.width, d.height, d.layers, d.format,
             d.usage, d.stride, mapper->tex[0]->params.w,
             mapper->tex[0]->params.h);

    EGLClientBuffer buf = p->GetNativeClientBufferANDROID(hwbuf);
    if (!buf)
        return -1;

    const int attribs[] = {EGL_NONE};
    p->egl_image = p->CreateImageKHR(eglGetCurrentDisplay(),
        EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, attribs);
    if (!p->egl_image) {
        MP_ERR(mapper, "[dovi-diag] eglCreateImageKHR failed: %#x\n",
               eglGetError());
        return -1;
    }

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->gl_texture);
    p->EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->egl_image);
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (mp_msg_test(mapper->log, MSGL_TRACE)) {
        GLenum error = gl->GetError();
        MP_TRACE(mapper,
                 "[dovi-diag] AImage=%"PRIu64
                 " EGLImage=%p GL texture=%u target=%#x error=%#x\n",
                 p->image_serial, p->egl_image, p->gl_texture,
                 GL_TEXTURE_EXTERNAL_OES, error);
    }

    return 0;
}


const struct ra_hwdec_driver ra_hwdec_aimagereader = {
    .name = "aimagereader",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_MEDIACODEC, 0},
    .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
