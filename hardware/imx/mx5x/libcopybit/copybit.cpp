 /*
 * Copyright 2009-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#define LOG_TAG "copybit"

#include <cutils/log.h>
 
#include <c2d_api.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <hardware/copybit.h>

#include "gralloc_priv.h"


/******************************************************************************/
#define MAX_SCALE_FACTOR    (8)
#define MDP_ALPHA_NOP 0xff
/******************************************************************************/

/* mFlags bit define */
enum {
    /* flip source image horizontally */
    C2D_FLIP_H    = HAL_TRANSFORM_FLIP_H,
    /* flip source image vertically */
    C2D_FLIP_V    = HAL_TRANSFORM_FLIP_V,
    /* enable or disable dithering */
    C2D_DITHER = 0x4,
    /* enable or disable alpha blend */
    C2D_ALPHA_BLEND = 0x8,
};

/** State information for each device instance */
struct copybit_context_t {
    struct copybit_device_t device;
    C2D_CONTEXT c2dctx;
    int     mCache;
    uint32_t mAlpha;
    uint32_t mRotate;    
    uint32_t mFlags;
};


/**
 * Common hardware methods
 */

static int open_copybit(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t copybit_module_methods = {
    open:  open_copybit
};

/*
 * The COPYBIT Module
 */
struct copybit_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: COPYBIT_HARDWARE_MODULE_ID,
        name: "C2D Z160 COPYBIT Module",
        author: "Freescale, Inc.",
        methods: &copybit_module_methods
    }
};

/*****************************************************************************/
/** check pixel alpha */
static bool hasAlpha(int format) {
    switch (format) {
    case COPYBIT_FORMAT_RGBA_8888:
    case COPYBIT_FORMAT_BGRA_8888:
    case COPYBIT_FORMAT_RGBA_5551:
    case COPYBIT_FORMAT_RGBA_4444:
        return true;
    default:
        return false;
    }
}

/** min of int a, b */
static inline int min(int a, int b) {
    return (a<b) ? a : b;
}

/** max of int a, b */
static inline int max(int a, int b) {
    return (a>b) ? a : b;
}

/** scale each parameter by mul/div. Assume div isn't 0 */
static inline void MULDIV(int *a, int *b, int mul, int div) {
    if (mul != div) {
        *a = (mul * *a) / div;
        *b = (mul * *b) / div;
    }
}

/** Determine the intersection of lhs & rhs store in out */
static void intersect(struct copybit_rect_t *out,
                      const struct copybit_rect_t *lhs,
                      const struct copybit_rect_t *rhs) {
    out->l = max(lhs->l, rhs->l);
    out->t = max(lhs->t, rhs->t);
    out->r = min(lhs->r, rhs->r);
    out->b = min(lhs->b, rhs->b);
}

/** Set a parameter to value */
static int set_parameter_copybit(
        struct copybit_device_t *dev,
        int name,
        int value) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    int status = 0;
    if (ctx) {
        switch(name) {
        case COPYBIT_ROTATION_DEG:
            switch (value) {
            case 0:
            case 90:
            case 180:
            case 270:
                ctx->mRotate = value;
                break;
            default:
                LOGE("Invalid value for COPYBIT_ROTATION");
                status = -EINVAL;
                break;
            }
            break;
        case COPYBIT_PLANE_ALPHA:
            if (value < 0)      value = 0;
            if (value >= 256)   value = 255;
                ctx->mAlpha = value;
            break;
        case COPYBIT_DITHER:
            if (value == COPYBIT_ENABLE) {
                ctx->mFlags |= C2D_DITHER;
            } else if (value == COPYBIT_DISABLE) {
                ctx->mFlags &= ~C2D_DITHER;
            }
            break;
        case COPYBIT_TRANSFORM:
            switch (value) {
            case 0:
                ctx->mRotate = 0;
                break;            
            case COPYBIT_TRANSFORM_ROT_90:
                ctx->mRotate = 90;
                break;
            case COPYBIT_TRANSFORM_ROT_180:
                ctx->mRotate = 180;
                break;
            case COPYBIT_TRANSFORM_ROT_270:
                ctx->mRotate = 270;
                break;
            case COPYBIT_TRANSFORM_FLIP_H:
                ctx->mFlags &= ~(C2D_FLIP_H | C2D_FLIP_V);
                ctx->mFlags |= C2D_FLIP_H;
                break;
            case COPYBIT_TRANSFORM_FLIP_V:
                ctx->mFlags &= ~(C2D_FLIP_H | C2D_FLIP_V);
                ctx->mFlags |= C2D_FLIP_V;
                break;
            default:
                LOGE("Invalid value for COPYBIT_ROTATION");
                status = -EINVAL;
                break;
            }
            break;        
        case COPYBIT_BLUR:
            LOGE("Not support for COPYBIT_BLUR");
            status = -EINVAL;
            break;
        default:
            status = -EINVAL;
            break;
        }
    } else {
        status = -EINVAL;
    }
    return status;
}

/** Get a static info value */
static int get(struct copybit_device_t *dev, int name) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    int value;
    if (ctx) {
        switch(name) {
        case COPYBIT_MINIFICATION_LIMIT:
            value = MAX_SCALE_FACTOR;
            break;
        case COPYBIT_MAGNIFICATION_LIMIT:
            value = MAX_SCALE_FACTOR;
            break;
        case COPYBIT_SCALING_FRAC_BITS:
            value = 32;
            break;
        case COPYBIT_ROTATION_STEP_DEG:
            value = 90;
            break;
        default:
            value = -EINVAL;
        }
    } else {
        value = -EINVAL;
    }
    return value;
}

/** convert COPYBIT_FORMAT to C2D format */
static C2D_COLORFORMAT get_format(int format) {
    switch (format) {
	case COPYBIT_FORMAT_RGBA_8888:     return C2D_COLOR_8888_ABGR;    
    case COPYBIT_FORMAT_RGB_565:       return C2D_COLOR_0565;
    case COPYBIT_FORMAT_RGBA_5551:     return C2D_COLOR_5551_RGBA;
    case COPYBIT_FORMAT_RGBA_4444:     return C2D_COLOR_4444_RGBA;
    case COPYBIT_FORMAT_RGBX_8888:	   return C2D_COLOR_8888_ABGR; //work-around, C2D does not support RGBX
    case COPYBIT_FORMAT_RGB_888:
	   return C2D_COLOR_888; //work-around, C2D supports BGR not RGB in this case
    case COPYBIT_FORMAT_BGRA_8888:	   return C2D_COLOR_8888;//work-around, C2D supports ARGB not BGRA in this case
    case COPYBIT_FORMAT_YCbCr_422_SP:  
    case COPYBIT_FORMAT_YCbCr_420_SP:
 
    default:                           return C2D_COLOR_0565;//work-around, C2D does not support YCbCr   
    }
}

/** get  pixelbit from COPYBIT_FORMAT format */
static int get_pixelbit(int format) {
    switch (format) {
    case COPYBIT_FORMAT_RGBA_8888:
    case COPYBIT_FORMAT_RGBX_8888:
    case COPYBIT_FORMAT_BGRA_8888:     return 32;
    case COPYBIT_FORMAT_RGB_888:       return 24;
    case COPYBIT_FORMAT_RGB_565:
    case COPYBIT_FORMAT_RGBA_5551:
    case COPYBIT_FORMAT_RGBA_4444:     return 16;
    case COPYBIT_FORMAT_YCbCr_422_SP:
    case COPYBIT_FORMAT_YCbCr_420_SP:
    default:                           return 8;
    }
}

/** do convert of image to c2d surface **/
static void image_to_surface(copybit_image_t const *img, C2D_SURFACE_DEF *surfaceDef) 
{
    private_handle_t* hnd = (private_handle_t*)img->handle;
    surfaceDef->format = get_format(img->format);
    surfaceDef->width =  img->w;
    surfaceDef->height = img->h;

	//make sure stride is 32 pixel aligned
    surfaceDef->stride = ((img->w + 31) & ~31)*get_pixelbit(img->format)>>3;

    surfaceDef->buffer = (void *)hnd->phys;
    surfaceDef->host = (void *)hnd->base;
    surfaceDef->flags = C2D_SURFACE_NO_BUFFER_ALLOC;
}

/** setup rectangles */
static void set_rects(struct copybit_context_t *dev,
                      C2D_RECT *srcRect,
                      C2D_RECT *dstRect,
                      const struct copybit_rect_t *dst,
                      const struct copybit_rect_t *src,
                      const struct copybit_rect_t *scissor,
                      copybit_image_t const *img) {
    struct copybit_rect_t clip;
    intersect(&clip, scissor, dst);

    dstRect->x  = clip.l;
    dstRect->y  = clip.t;
    dstRect->width  = clip.r - clip.l;
    dstRect->height  = clip.b - clip.t;

    uint32_t W, H;
    if ((dev->mRotate == 90) || ((dev->mRotate == 270))) {
        srcRect->x = (clip.t - dst->t) + src->t;
        srcRect->y = (dst->r - clip.r) + src->l;
        srcRect->width = (clip.b - clip.t);
        srcRect->height = (clip.r - clip.l);
        W = dst->b - dst->t;
        H = dst->r - dst->l;
    } else {
        srcRect->x  = (clip.l - dst->l) + src->l;
        srcRect->y  = (clip.t - dst->t) + src->t;
        srcRect->width  = (clip.r - clip.l);
        srcRect->height  = (clip.b - clip.t);
        W = dst->r - dst->l;
        H = dst->b - dst->t;
    }
    MULDIV(&srcRect->x, &srcRect->width, src->r - src->l, W);
    MULDIV(&srcRect->y, &srcRect->height, src->b - src->t, H);

    if ((dev->mRotate == 180) || ((dev->mRotate == 270))) {
        srcRect->y = src->b - src->t - (srcRect->y + srcRect->height);
        srcRect->x = src->r - src->l - (srcRect->x + srcRect->width);
    }
}

/** do a stretch blit type operation */
static int stretch_copybit(
        struct copybit_device_t *dev,
        struct copybit_image_t const *dst,
        struct copybit_image_t const *src,
        struct copybit_rect_t const *dst_rect,
        struct copybit_rect_t const *src_rect,
        struct copybit_region_t const *region) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    C2D_SURFACE_DEF srcSurfaceDef;
    C2D_SURFACE_DEF dstSurfaceDef;
    C2D_SURFACE srcSurface;
    C2D_SURFACE dstSurface;    
    C2D_RECT srcRect;
    C2D_RECT dstRect;
    int status = 0;

    if (ctx) {
        if (ctx->mAlpha < 255) {
            switch (src->format) {
                // we don't support plane alpha with below formats
                case COPYBIT_FORMAT_RGBX_8888:
                case COPYBIT_FORMAT_RGB_888:
                case COPYBIT_FORMAT_BGRA_8888:
                case COPYBIT_FORMAT_YCbCr_422_SP:
                case COPYBIT_FORMAT_YCbCr_420_SP:
                    return -EINVAL;
            }
        }

        if (src_rect->l < 0 || src_rect->r > src->w ||
            src_rect->t < 0 || src_rect->b > src->h) {
            // this is always invalid
            LOGE("src_rect invalid");
            return -EINVAL;
        }

        const struct copybit_rect_t bounds = { 0, 0, dst->w, dst->h };
        struct copybit_rect_t clip;
        status = 0;


        image_to_surface(src, &srcSurfaceDef);
        if (c2dSurfAlloc(ctx->c2dctx, &srcSurface, &srcSurfaceDef) != C2D_STATUS_OK)
        {
            LOGE("srcSurface c2dSurfAlloc fail");
            return -EINVAL;
        }
                
        image_to_surface(dst, &dstSurfaceDef);
        if (c2dSurfAlloc(ctx->c2dctx, &dstSurface, &dstSurfaceDef) != C2D_STATUS_OK)
        {
            LOGE("dstSurface c2dSurfAlloc fail");
            c2dSurfFree(ctx->c2dctx, srcSurface);
            return -EINVAL;
        }


        c2dSetSrcSurface(ctx->c2dctx, srcSurface);
        c2dSetDstSurface(ctx->c2dctx, dstSurface); 
        c2dSetSrcRotate(ctx->c2dctx, ctx->mRotate);
        

        //if (hasAlpha(src->format) || hasAlpha(dst->format))
        if (hasAlpha(src->format) && (ctx->mFlags & C2D_ALPHA_BLEND))
                c2dSetBlendMode(ctx->c2dctx, C2D_ALPHA_BLEND_SRCOVER);
        else
                c2dSetBlendMode(ctx->c2dctx, C2D_ALPHA_BLEND_NONE);
                   
        c2dSetGlobalAlpha(ctx->c2dctx, ctx->mAlpha);  
        c2dSetDither(ctx->c2dctx, (ctx->mFlags & C2D_DITHER) > 0 ? 1:0); 

        while ((status == 0) && region->next(region, &clip)) {          
                intersect(&clip, &bounds, &clip);
                set_rects(ctx, &srcRect, &dstRect, dst_rect, src_rect, &clip, src);
                if (srcRect.width<=0 || srcRect.height<=0)
                {
                        LOGE("srcRect invalid");
                        continue;
                }
                if (dstRect.width<=0 || dstRect.height<=0)
                {
                        LOGE("dstRect invalid");
                        continue;
                }

                c2dSetSrcRectangle(ctx->c2dctx, &srcRect);
                c2dSetDstRectangle(ctx->c2dctx, &dstRect);      
                c2dDrawBlit(ctx->c2dctx); 
        }

		c2dFinish(ctx->c2dctx);
		c2dSurfFree(ctx->c2dctx, srcSurface);
		c2dSurfFree(ctx->c2dctx, dstSurface);

    } 
    else {
        status = -EINVAL;
    }


    return status;
}

/** Perform a blit type operation */
/* Pay attention, from now on blit_copybit will only work on C2D_ALPHA_BLEND_NONE mode,
 if need C2D_ALPHA_BLEND_SRCOVER mode pls use stretch_copybit */
static int blit_copybit(
        struct copybit_device_t *dev,
        struct copybit_image_t const *dst,
        struct copybit_image_t const *src,
        struct copybit_region_t const *region) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    struct copybit_rect_t dr = { 0, 0, dst->w, dst->h };
    struct copybit_rect_t sr = { 0, 0, src->w, src->h };
    if (ctx->mFlags & C2D_ALPHA_BLEND)
    {
        int status = 0;
        ctx->mFlags &= ~C2D_ALPHA_BLEND;
        status = stretch_copybit(dev, dst, src, &dr, &sr, region);
        ctx->mFlags |= C2D_ALPHA_BLEND;
        return status;
    }
    else
        return stretch_copybit(dev, dst, src, &dr, &sr, region);
}

/*****************************************************************************/

/** Close the copybit device */
static int close_copybit(struct hw_device_t *dev) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    if (ctx) {
        C2D_STATUS c2dstatus;
        if (ctx->c2dctx != NULL)
        	c2dstatus = c2dDestroyContext(ctx->c2dctx);
        free(ctx);
    }
    return 0;
}

/** Open a new instance of a copybit device using name */
static int open_copybit(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
       
    copybit_context_t *ctx;
    ctx = (copybit_context_t *)malloc(sizeof(copybit_context_t));
    memset(ctx, 0, sizeof(*ctx));

    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = 1;
    ctx->device.common.module = const_cast<hw_module_t*>(module);
    ctx->device.common.close = close_copybit;
    ctx->device.set_parameter = set_parameter_copybit;
    ctx->device.get = get;
    ctx->device.blit = blit_copybit;
    ctx->device.stretch = stretch_copybit;
    ctx->mAlpha = MDP_ALPHA_NOP;
    ctx->mFlags |= C2D_ALPHA_BLEND;
     
    C2D_STATUS c2dstatus;
    c2dstatus = c2dCreateContext(&ctx->c2dctx);
    if (c2dstatus != C2D_STATUS_OK)
        close_copybit(&ctx->device.common);
    else
    {
        *device = &ctx->device.common;
        status = 0;
    }

    return status;
}
