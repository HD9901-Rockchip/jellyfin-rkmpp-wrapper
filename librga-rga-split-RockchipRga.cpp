/*
 * Copyright (C) 2016 Rockchip Electronics Co., Ltd.
 * Authors:
 *  Zhiqin Wei <wzq@rock-chips.com>
 *
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

#define LOG_NDEBUG 0
#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG "rockchiprga"
#endif

#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <mutex>

#ifdef ANDROID
#include <utils/misc.h>
#include <cutils/properties.h>
#include "core/NormalRga.h"

#ifndef ANDROID_8
#include <gui/Surface.h>
#include <binder/IPCThreadState.h>
#include <gui/SurfaceComposerClient.h>
#endif

#include <android/log.h>
#include <log/log_main.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Singleton.h>
#include <ui/PixelFormat.h>
#include <ui/GraphicBufferMapper.h>
#endif

#include "RockchipRga.h"

#ifdef LINUX
#include "NormalRga.h"
#include "drm.h"
#include "drm_mode.h"
#endif

#include "im2d_api/im2d.h"

#ifdef LINUX
static int local_drmIoctl(int fd, unsigned long request, void *arg) {
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

static int local_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    struct drm_prime_handle args;
    int ret;

    memset(&args, 0, sizeof(args));
    args.fd = -1;
    args.handle = handle;
    args.flags = flags;
    ret = local_drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
    if (ret)
        return ret;

    *prime_fd = args.fd;
    return 0;
}
#endif

#ifdef ANDROID
namespace android {
// ---------------------------------------------------------------------------
    ANDROID_SINGLETON_STATIC_INSTANCE(RockchipRga)
#else
RGA_SINGLETON_STATIC_INSTANCE(RockchipRga)
#endif

    RockchipRga::RockchipRga():
        mSupportRga(false),
        mLogOnce(0),
        mLogAlways(0),
        mContext(NULL) {
        RkRgaInit();

#ifdef ANDROID
        property_set("vendor.rga_api.version", RGA_API_VERSION);
#endif
        ALOGI("%s", RGA_API_FULL_VERSION);
    }

    RockchipRga::~RockchipRga() {
        RgaDeInit(&mContext);
    }

    int RockchipRga::RkRgaInit() {
        int ret = 0;

        if (mSupportRga)
            return 0;

        ret = RgaInit(&mContext);
        if(ret >= 0)
            mSupportRga = true;
        else
            mSupportRga = false;

        return ret;
    }

    void RockchipRga::RkRgaDeInit() {
        if (mSupportRga)
            RgaDeInit(&mContext);

        mSupportRga = false;
    }

    void RockchipRga::RkRgaGetContext(void **ctx) {
        *ctx = mContext;
    }

#ifdef LINUX
    int RockchipRga::RkRgaAllocBuffer(int drm_fd, bo_t *bo_info, int width,
                                      int height, int bpp, int flags) {
        struct drm_mode_create_dumb arg;
        int ret;

        memset(&arg, 0, sizeof(arg));
        arg.bpp = bpp;
        arg.width = width;
        arg.height = height;
        arg.flags = flags;

        ret = local_drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
        if (ret) {
            fprintf(stderr, "failed to create dumb buffer: %s\n", strerror(errno));
            return ret;
        }

        bo_info->handle = arg.handle;
        bo_info->size = arg.size;
        bo_info->pitch = arg.pitch;

        return 0;
    }

    int RockchipRga::RkRgaFreeBuffer(int drm_fd, bo_t *bo_info) {
        struct drm_mode_destroy_dumb arg;
        int ret;

        if (bo_info->handle <= 0)
            return -EINVAL;
        memset(&arg, 0, sizeof(arg));
        arg.handle = bo_info->handle;
        ret = local_drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
        if (ret) {
            fprintf(stderr, "failed to destroy dumb buffer: %s\n", strerror(errno));
            return -errno;
        }
        bo_info->handle = 0;

        return 0;

    }

    int RockchipRga::RkRgaGetAllocBufferExt(bo_t *bo_info, int width, int height, int bpp, int flags) {
        static const char* card = "/dev/dri/card0";
        int ret;
        int drm_fd;
        int flag = O_RDWR;
#ifdef O_CLOEXEC
        flag |= O_CLOEXEC;
#endif
        bo_info->fd = -1;
        bo_info->handle = 0;
        drm_fd = open(card, flag);
        if (drm_fd < 0) {
            fprintf(stderr, "Fail to open %s: %m\n", card);
            return -errno;
        }
        ret = RkRgaAllocBuffer(drm_fd, bo_info, width, height, bpp, flags);
        if (ret) {
            close(drm_fd);
            return ret;
        }
        bo_info->fd = drm_fd;
        return 0;
    }

    int RockchipRga::RkRgaGetAllocBuffer(bo_t *bo_info, int width, int height, int bpp) {
        return RkRgaGetAllocBufferExt(bo_info, width, height, bpp, 0);
    }

    int RockchipRga::RkRgaGetAllocBufferCache(bo_t *bo_info, int width, int height, int bpp) {
        return RkRgaGetAllocBufferExt(bo_info, width, height, bpp, ROCKCHIP_BO_CACHABLE);
    }

    int RockchipRga::RkRgaGetMmap(bo_t *bo_info) {
        struct drm_mode_map_dumb arg;
        void *map;
        int ret;

        memset(&arg, 0, sizeof(arg));
        arg.handle = bo_info->handle;
        ret = local_drmIoctl(bo_info->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
        if (ret)
            return ret;
        map = mmap64(0, bo_info->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo_info->fd, arg.offset);
        if (map == MAP_FAILED)
            return -EINVAL;
        bo_info->ptr = map;
        return 0;
    }

    int RockchipRga::RkRgaUnmap(bo_t *bo_info) {
        munmap(bo_info->ptr, bo_info->size);
        bo_info->ptr = NULL;
        return 0;
    }

    int RockchipRga::RkRgaFree(bo_t *bo_info) {
        int ret;
        if (bo_info->fd < 0)
            return -EINVAL;
        ret = RkRgaFreeBuffer(bo_info->fd, bo_info);
        close(bo_info->fd);
        bo_info->fd = -1;
        return ret;
    }

    int RockchipRga::RkRgaGetBufferFd(bo_t *bo_info, int *fd) {
        int ret = 0;
        ret = local_drmPrimeHandleToFD(bo_info->fd, bo_info->handle, DRM_CLOEXEC | DRM_RDWR, fd);
        return ret;
    }
#endif

#ifdef ANDROID
    int RockchipRga::RkRgaGetBufferFd(buffer_handle_t handle, int *fd) {
        int ret = 0;
        ret = RkRgaGetHandleFd(handle, fd);
        return ret;
    }

    int RockchipRga::RkRgaGetHandleMapCpuAddress(buffer_handle_t handle, void **buf) {
        int ret = 0;
        ret = RkRgaGetHandleMapAddress(handle, buf);
        return ret;
    }
#endif

    /* ------------------------------------------------------------------
     * RK3399 RGA2 overlay color-space workaround
     *
     * The RGA2 in RK3399 bypasses the destination R2Y color-space
     * conversion whenever src0 Y2R and blending are active in the same
     * pass.  A "YUV main + RGB overlay -> YUV out" job (ffmpeg's
     * overlay_rkrga filter) therefore writes post-blend RGB data straight
     * into the YUV planes and the output turns green/corrupt.
     *
     * Empirical capability matrix on RK3399 RGA2 (v3.2):
     *   YUV -> YUV / RGB -> RGB passthrough + blend  : ok
     *   RGB -> YUV (dst R2Y, no blend)               : ok
     *   YUV -> RGB (src0 Y2R, no blend)              : ok
     *   YUV + RGB -> RGB (blend, Y2R only)           : ok
     *   RGB + RGB -> YUV (blend, dst R2Y only)       : ok
     *   YUV + RGB -> YUV (blend, Y2R + R2Y)          : BROKEN (dst R2Y bypassed)
     *   full_csc coefficient path / src1 CSC         : inert
     *
     * dst R2Y simple-mode selection (empirical):
     *   mode 2          -> BT.601 limited
     *   mode 3          -> BT.709 full
     *   mode 3 + clip   -> BT.709 limited
     *
     * Workaround: split the job into two hardware passes
     *   pass1: YUV main -> RGB scratch        (src0 Y2R only)
     *   pass2: RGB scratch + RGB overlay -> YUV dst (blend + dst R2Y only)
     * The scratch buffer is a cached dma-heap allocation.
     * ------------------------------------------------------------------ */

#define RGA_SPLIT_HEAP_IOCTL_ALLOC  _IOWR('H', 0x0, struct rga_split_heap_alloc_data)
struct rga_split_heap_alloc_data {
    unsigned long long len;
    unsigned int fd;
    unsigned int fd_flags;
    unsigned long long heap_flags;
};

static int rga_split_is_yuv_format(int fmt) {
    switch (fmt >> 8) {
    case 0x8: case 0x9: case 0xa: case 0xb:
    case 0xc: case 0xd: case 0xe: case 0xf:
    case 0x15:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
    case 0x20: case 0x21: case 0x22: case 0x23:
        return 1;
    default:
        return 0;
    }
}

static int rga_split_is_rgb_format(int fmt) {
    switch (fmt >> 8) {
    case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
    case 0x6: case 0x7: case 0x16:
    case 0x24: case 0x25: case 0x26: case 0x28: case 0x29:
        return 1;
    default:
        return 0;
    }
}

struct rga_split_scratch_t {
    int fd;
    int wstride;
    int hstride;
    size_t size;
};
static rga_split_scratch_t g_rga_split_scratch = { -1, 0, 0, 0 };
static std::mutex g_rga_split_mutex;

static int rga_split_get_scratch(int w, int h) {
    int wstride = (w + 3) & ~3;
    size_t need = (size_t)wstride * h * 3;

    if (g_rga_split_scratch.fd >= 0 && g_rga_split_scratch.size >= need)
        return g_rga_split_scratch.fd;
    if (g_rga_split_scratch.fd >= 0) {
        close(g_rga_split_scratch.fd);
        g_rga_split_scratch.fd = -1;
    }

    int heap = open("/dev/dma_heap/system", O_RDWR);
    if (heap < 0)
        return -1;

    struct rga_split_heap_alloc_data d;
    memset(&d, 0, sizeof(d));
    d.len = need;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, RGA_SPLIT_HEAP_IOCTL_ALLOC, &d) < 0) {
        close(heap);
        return -1;
    }
    close(heap);

    g_rga_split_scratch.fd = (int)d.fd;
    g_rga_split_scratch.wstride = wstride;
    g_rga_split_scratch.hstride = h;
    g_rga_split_scratch.size = need;
    ALOGD("rga split overlay: scratch fd=%d wstride=%d size=%zu\n",
          g_rga_split_scratch.fd, g_rga_split_scratch.wstride, need);
    return g_rga_split_scratch.fd;
}

    int RockchipRga::RkRgaBlit(rga_info *src, rga_info *dst, rga_info *src1) {
        int ret = 0;

        /* YUV main + RGB overlay -> YUV out with blending is not supported
         * by the RK3399 RGA2 in a single pass; run it as two passes. */
        if (src && dst && src1 &&
            (src1->fd >= 0 || src1->virAddr) &&
            rga_split_is_yuv_format(src->rect.format) &&
            rga_split_is_rgb_format(src1->rect.format) &&
            rga_split_is_yuv_format(dst->rect.format) &&
            (src->blend & 0xfff)) {
            std::lock_guard<std::mutex> lock(g_rga_split_mutex);

            int scratch_fd = rga_split_get_scratch(dst->rect.width, dst->rect.height);
            if (scratch_fd >= 0) {
                rga_info pass_src;
                rga_info pass_dst;

                /* pass1: YUV main -> RGB scratch (src0 Y2R only) */
                pass_src = *src;
                pass_src.blend = 0;
                pass_src.color_space_mode = 0;
                pass_src.sync_mode = RGA_BLIT_SYNC;
                pass_src.out_fence_fd = -1;

                memset(&pass_dst, 0, sizeof(pass_dst));
                pass_dst.mmuFlag = 1;
                pass_dst.fd = scratch_fd;
                rga_set_rect(&pass_dst.rect, 0, 0,
                             dst->rect.width, dst->rect.height,
                             g_rga_split_scratch.wstride, dst->rect.height,
                             RK_FORMAT_RGB_888);
                /*
                 * Use the BT.601 pair for the round trip: it is the only
                 * matrix family whose Y2R and R2Y are near-exact inverses
                 * on this hardware (~53dB round-trip PSNR).  The BT.709
                 * dst-R2Y simple mode ("mode3 + clip") is not the exact
                 * inverse of its Y2R counterpart (~32dB) and the full_csc
                 * coefficient path is inert, so honoring the source matrix
                 * here would trade a small subtitle-color deviation for a
                 * visible whole-frame luma error.  Main pixels round-trip
                 * near-losslessly; overlay colors use the BT.601 matrix.
                 * 601F (JPEG/full-range) sources keep the 601F pair.
                 */
                pass_dst.color_space_mode =
                    ((dst->color_space_mode & 0x3) == 2) ? 2 : 1;

                ret = RgaBlit(&pass_src, &pass_dst, NULL);
                if (ret == 0) {
                    /* pass2: RGB scratch + RGB overlay -> YUV dst
                     * (blend + dst R2Y only) */
                    memset(&pass_src, 0, sizeof(pass_src));
                    pass_src.mmuFlag = 1;
                    pass_src.fd = scratch_fd;
                    rga_set_rect(&pass_src.rect, 0, 0,
                                 dst->rect.width, dst->rect.height,
                                 g_rga_split_scratch.wstride, dst->rect.height,
                                 RK_FORMAT_RGB_888);
                    pass_src.blend = src->blend;
                    pass_src.sync_mode = dst->sync_mode;
                    pass_src.in_fence_fd = -1;

                    pass_dst = *dst;
                    pass_dst.in_fence_fd = -1;
                    {
                        /* dst R2Y simple mode:
                         *   mode 1        -> BT.601 full
                         *   mode 2        -> BT.601 limited
                         * (see the round-trip note above) */
                        bool full_range = ((dst->color_space_mode & 0x3) == 2);
                        pass_dst.color_space_mode = full_range ? 0x4 : 0x8;
                    }

                    ret = RgaBlit(&pass_src, &pass_dst, src1);
                    dst->out_fence_fd = pass_dst.out_fence_fd;
                }

                if (ret == 0)
                    return 0;

                ALOGE("rga split overlay failed (%d), fallback to single pass", ret);
            } else {
                ALOGE("rga split overlay: scratch alloc failed, fallback to single pass");
            }
        }

        ret = RgaBlit(src, dst, src1);
        if (ret) {
            RkRgaLogOutUserPara(src);
            RkRgaLogOutUserPara(dst);
            RkRgaLogOutUserPara(src1);
        }
        return ret;
    }

    int RockchipRga::RkRgaFlush() {
        int ret = 0;
        ret = RgaFlush();
        if (ret) {
            ALOGE("RgaFlush Failed");
        }
        return ret;
    }

    int RockchipRga::RkRgaCollorFill(rga_info *dst) {
        int ret = 0;
        ret = RgaCollorFill(dst);
        return ret;
    }

    int RockchipRga::RkRgaCollorPalette(rga_info *src, rga_info *dst, rga_info *lut) {
        int ret = 0;
        ret = RgaCollorPalette(src, dst, lut);
        if (ret) {
            RkRgaLogOutUserPara(src);
            RkRgaLogOutUserPara(dst);
        }
        return ret;
    }

    int RockchipRga::RkRgaLogOutUserPara(rga_info *rgaInfo) {
        if (!rgaInfo)
            return -EINVAL;

        ALOGE("handl-fd-vir-phy-hnd-format[%d, %d, %p, %p, %lx, %d]",
              rgaInfo->handle, rgaInfo->fd, rgaInfo->virAddr, rgaInfo->phyAddr,
              (unsigned long)rgaInfo->hnd, rgaInfo->format);
        ALOGE("rect[%d, %d, %d, %d, %d, %d, %d, %d]",
              rgaInfo->rect.xoffset, rgaInfo->rect.yoffset,
              rgaInfo->rect.width,   rgaInfo->rect.height, rgaInfo->rect.wstride,
              rgaInfo->rect.hstride, rgaInfo->rect.format, rgaInfo->rect.size);
        ALOGE("f-blend-size-rotation-col-log-mmu[%d, %x, %d, %d, %d, %d, %d]",
              rgaInfo->format, rgaInfo->blend, rgaInfo->bufferSize,
              rgaInfo->rotation, rgaInfo->color, rgaInfo->testLog, rgaInfo->mmuFlag);
        return 0;
    }

// ---------------------------------------------------------------------------
#ifdef ANDROID
}; // namespace android
#endif

