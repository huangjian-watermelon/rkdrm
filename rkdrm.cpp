#include "rkdrm.h"

#include "YInterfaceLogger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_time.h>
}
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <memory>
#include <vector>

#include "YInterfaceCircleBuf.h"

#define CODEC_ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))

using namespace std;

std::shared_ptr<int> RkDrmDisplay::m_devFd = std::make_shared<int>(0);
std::shared_ptr<RkDrmDisplay::connectorVec> RkDrmDisplay::m_connectors =
    std::make_shared<RkDrmDisplay::connectorVec>(RkDrmDisplay::connectorVec());
std::shared_ptr<RkDrmDisplay::encoderVec> RkDrmDisplay::m_encoders =
    std::make_shared<RkDrmDisplay::encoderVec>(RkDrmDisplay::encoderVec());
std::shared_ptr<RkDrmDisplay::crtcVec> RkDrmDisplay::m_crtcs =
    std::make_shared<RkDrmDisplay::crtcVec>(RkDrmDisplay::crtcVec());
std::shared_ptr<RkDrmDisplay::planeVec> RkDrmDisplay::m_planes =
    std::make_shared<RkDrmDisplay::planeVec>(RkDrmDisplay::planeVec());
RkDrmDisplay::planeVec RkDrmDisplay::m_hdmiPlanes = RkDrmDisplay::planeVec();

struct DrmDumbBufferCtx {
    uint32_t handle;
};

RkDrmDisplay::RkDrmDisplay()
    : m_crtcX(0),
      m_crtcY(0),
      m_crtcWidth(0),
      m_crtcHeight(0),
      m_planeIndex(0),
      m_crtcIndex(0),
      m_mapRes(nullptr),
      m_flipPending(false) {
    static bool init = RkDrmDisplay::InitDrm();
}

RkDrmDisplay::~RkDrmDisplay() {
    if (m_mapRes) {
        delete m_mapRes;
        m_mapRes = nullptr;
    }

    while (!m_frameQueue.empty()) {
        FrameResourse *res = m_frameQueue.front();
        m_frameQueue.pop_front();
        FreeFrameResInternal(res);
    }
}

bool RkDrmDisplay::FreeFrameResInternal(FrameResourse *&frameRes) {
    if (!frameRes)
        return true;

    if (frameRes->frame)
        av_frame_free(&frameRes->frame);
    if (frameRes->bufId)
        drmModeRmFB(*m_devFd, frameRes->bufId);
    drmIoctl(*m_devFd, DRM_IOCTL_GEM_CLOSE, &frameRes->gemClose);
    delete frameRes;
    frameRes = nullptr;
    return true;
}

void RkDrmDisplay::OnPageFlipEventInternal(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                                   void *userData) {
    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    RkDrmDisplay *display = reinterpret_cast<RkDrmDisplay *>(userData);
    if (!display)
        return;

    display->m_flipPending = false;
    if (display->m_frameQueue.size() > 1) {
        FrameResourse *oldRes = display->m_frameQueue.front();
        display->m_frameQueue.pop_front();
        display->FreeFrameResInternal(oldRes);
    }
}

bool RkDrmDisplay::PumpDrmEventsInternal(int timeoutMs) {
    if (!m_flipPending)
        return true;

    struct pollfd pfd = {};
    pfd.fd = *m_devFd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeoutMs);
    if (ret <= 0 || !(pfd.revents & POLLIN))
        return false;

    drmEventContext eventCtx = {};
    eventCtx.version = DRM_EVENT_CONTEXT_VERSION;
    eventCtx.page_flip_handler = RkDrmDisplay::OnPageFlipEventInternal;
    ret = drmHandleEvent(*m_devFd, &eventCtx);
    if (ret < 0)
        return false;

    return !m_flipPending;
}

bool RkDrmDisplay::GetObjectPropertyIdInternal(uint32_t objectId, uint32_t objectType, const char *propertyName,
                                       uint32_t &propertyId) {
    propertyId = 0;
    std::shared_ptr<drmModeObjectProperties> objectPtr(
        drmModeObjectGetProperties(*m_devFd, objectId, objectType),
        [](drmModeObjectPropertiesPtr ptr) { drmModeFreeObjectProperties(ptr); });
    if (!objectPtr)
        return false;

    for (uint32_t i = 0; i < objectPtr->count_props; i++) {
        std::shared_ptr<drmModePropertyRes> resPtr(
            drmModeGetProperty(*m_devFd, objectPtr->props[i]),
            [](drmModePropertyPtr ptr) { drmModeFreeProperty(ptr); });
        if (!resPtr)
            continue;
        if (!strcmp(resPtr->name, propertyName)) {
            propertyId = resPtr->prop_id;
            return true;
        }
    }

    return false;
}

bool RkDrmDisplay::AtomicSetPlaneInternal(uint32_t fbId, int32_t crtcX, int32_t crtcY, uint32_t crtcWidth,
                                  uint32_t crtcHeight, uint32_t srcWidth, uint32_t srcHeight) {
    PumpDrmEventsInternal(0);
    if (m_flipPending) {
        errno = EBUSY;
        return false;
    }

    const uint32_t planeId = m_planes->at(m_planeIndex)->plane_id;
    const uint32_t crtcId = m_crtcs->at(m_crtcIndex)->crtc_id;

    uint32_t propCrtcId = 0;
    uint32_t propFbId = 0;
    uint32_t propCrtcX = 0;
    uint32_t propCrtcY = 0;
    uint32_t propCrtcW = 0;
    uint32_t propCrtcH = 0;
    uint32_t propSrcX = 0;
    uint32_t propSrcY = 0;
    uint32_t propSrcW = 0;
    uint32_t propSrcH = 0;

    if (!GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_ID", propCrtcId) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "FB_ID", propFbId) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_X", propCrtcX) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_Y", propCrtcY) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_W", propCrtcW) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "CRTC_H", propCrtcH) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "SRC_X", propSrcX) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "SRC_Y", propSrcY) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "SRC_W", propSrcW) ||
        !GetObjectPropertyIdInternal(planeId, DRM_MODE_OBJECT_PLANE, "SRC_H", propSrcH)) {
        YLOG_ERROR("AtomicSetPlane: get plane property id failed.");
        return false;
    }

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) {
        YLOG_ERROR("AtomicSetPlane: drmModeAtomicAlloc failed.");
        return false;
    }

    int ret = 0;
    ret |= drmModeAtomicAddProperty(req, planeId, propCrtcId, fbId ? crtcId : 0);
    ret |= drmModeAtomicAddProperty(req, planeId, propFbId, fbId);
    ret |= drmModeAtomicAddProperty(req, planeId, propCrtcX, crtcX);
    ret |= drmModeAtomicAddProperty(req, planeId, propCrtcY, crtcY);
    ret |= drmModeAtomicAddProperty(req, planeId, propCrtcW, crtcWidth);
    ret |= drmModeAtomicAddProperty(req, planeId, propCrtcH, crtcHeight);
    ret |= drmModeAtomicAddProperty(req, planeId, propSrcX, 0);
    ret |= drmModeAtomicAddProperty(req, planeId, propSrcY, 0);
    ret |= drmModeAtomicAddProperty(req, planeId, propSrcW, static_cast<uint64_t>(srcWidth) << 16);
    ret |= drmModeAtomicAddProperty(req, planeId, propSrcH, static_cast<uint64_t>(srcHeight) << 16);
    if (ret < 0) {
        YLOG_ERROR("AtomicSetPlane: drmModeAtomicAddProperty failed.");
        drmModeAtomicFree(req);
        return false;
    }

    ret = drmModeAtomicCommit(*m_devFd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, this);
    drmModeAtomicFree(req);
    if (ret < 0) {
        YLOG_ERROR("AtomicSetPlane: drmModeAtomicCommit failed, ret=%d", ret);
        return false;
    }
    m_flipPending = true;
    return true;
}

void RkDrmDisplay::SetRotateAngleVideo(AVFrame *&frame, RotateAngle rotation) {
    if (!frame || frame->format != AV_PIX_FMT_DRM_PRIME || !rotation)
        return;

    AVDRMFrameDescriptor *srcDesc = (AVDRMFrameDescriptor *)frame->data[0];

    int srcFd = srcDesc->objects[0].fd;
    int srcWidth = frame->width;
    int srcHeight = CODEC_ALIGN(frame->height, 16);

    bool swapWH = (rotation == HAL_TRANSFORM_ROT_90 || rotation == HAL_TRANSFORM_ROT_270);

    int dstWidth = swapWH ? srcHeight : srcWidth;
    int dstHeight = swapWH ? srcWidth : srcHeight;

    // 分配新的 DRM PRIME AVFrame，用于存放旋转后的结果
    AVFrame *rotateframe = av_frame_alloc();
    if (!AllocDrmPrimeFrame(srcHeight, srcWidth, rotateframe)) {
        YLOG_ERROR("AllocDrmPrimeFrame is faile");
        av_frame_free(&rotateframe);
        return;
    }

    AVDRMFrameDescriptor *dstDesc = (AVDRMFrameDescriptor *)rotateframe->data[0];

    rga_info_t srcInfo{};
    rga_info_t dstInfo{};

    srcInfo.fd = srcFd;
    srcInfo.mmuFlag = 1;
    srcInfo.rotation = rotation;
    rga_set_rect(&srcInfo.rect, 0, 0, srcWidth, srcHeight, srcWidth, srcHeight, RK_FORMAT_YCbCr_420_SP);

    dstInfo.fd = dstDesc->objects[0].fd;
    dstInfo.mmuFlag = 1;
    rga_set_rect(&dstInfo.rect, 0, 0, dstWidth, dstHeight, dstWidth, dstHeight, RK_FORMAT_YCbCr_420_SP);

    // 调用 RGA 进行旋转 / 拷贝
    int ret = c_RkRgaBlit(&srcInfo, &dstInfo, NULL);
    if (ret != 0) {
        YLOG_ERROR("RotateAngleVideo:c_RkRgaBlit failed: %d", ret);
        av_frame_free(&rotateframe);
        return;
    }

    av_frame_free(&frame);
    frame = rotateframe;
    return;
}

void RkDrmDisplay::SetRotateAngleText(AVFrame *frame, RotateAngle rotation) {
    if (!frame || frame->format != AV_PIX_FMT_BGRA || !rotation)
        return;

    int srcWidth = frame->width;
    int srcHeight = frame->height;

    bool swapWH = (rotation == HAL_TRANSFORM_ROT_90 || rotation == HAL_TRANSFORM_ROT_270);

    int dstWidth = swapWH ? srcHeight : srcWidth;
    int dstHeight = swapWH ? srcWidth : srcHeight;

    size_t dstSize = dstWidth * dstHeight * 4;
    if (!m_dataText || m_dataTextSize < dstSize) {
        if (m_dataText)
            free(m_dataText);
        m_dataText = (char *)malloc(dstSize);
        m_dataTextSize = dstSize;
    }

    memset(m_dataText, 0, dstSize);

    rga_info_t srcInfo{};
    rga_info_t dstInfo{};

    srcInfo.virAddr = frame->data[0];
    srcInfo.mmuFlag = 1;
    srcInfo.rotation = rotation;
    rga_set_rect(&srcInfo.rect, 0, 0, srcWidth, srcHeight, srcWidth, srcHeight, RK_FORMAT_BGRA_8888);

    dstInfo.virAddr = (uint8_t *)m_dataText;
    dstInfo.mmuFlag = 1;
    rga_set_rect(&dstInfo.rect, 0, 0, dstWidth, dstHeight, dstWidth, dstHeight, RK_FORMAT_BGRA_8888);

    // 调用 RGA 进行旋转拷贝
    int ret = c_RkRgaBlit(&srcInfo, &dstInfo, nullptr);
    if (ret != 0) {
        YLOG_ERROR("RotateAngleText:c_RkRgaBlit failed: %d", ret);
        return;
    }

    // 解除 AVFrame 对原有数据的引用
    // 注意：这里只是 unref，不会释放 m_dataText
    av_frame_unref(frame);

    // 更新 AVFrame，指向旋转后的 BGRA 数据
    frame->data[0] = (uint8_t *)m_dataText;
    frame->linesize[0] = dstWidth * 4;
    frame->width = dstWidth;
    frame->height = dstHeight;
    frame->format = AV_PIX_FMT_BGRA;
    return;
}

void RkDrmDisplay::SetRotateAngle(uint64_t angle, DrmPlaneSequence planeIndex) {
    if (planeIndex >= m_planes->size())
        return;

    std::shared_ptr<drmModeObjectProperties> objectPtr(
                drmModeObjectGetProperties(*m_devFd, m_planes->at(planeIndex)->plane_id, DRM_MODE_OBJECT_PLANE),
                [](drmModeObjectPropertiesPtr ptr){drmModeFreeObjectProperties(ptr);});

    for (unsigned int i = 0; i < objectPtr->count_props; i++) {
        std::shared_ptr<drmModePropertyRes> resPtr(
                    drmModeGetProperty(*m_devFd, objectPtr->props[i]),
                    [](drmModePropertyPtr ptr){drmModeFreeProperty(ptr);});
        if (!strcmp(resPtr->name, "rotation")) {
            drmModeObjectSetProperty(*m_devFd, m_planes->at(planeIndex)->plane_id,
                                     DRM_MODE_OBJECT_PLANE, resPtr->prop_id, angle);
            break;
        }
    }
}

void RkDrmDisplay::SetZposValue(uint64_t zpos, DrmPlaneSequence planeIndex) {
    if (planeIndex >= m_planes->size())
        return;

    std::shared_ptr<drmModeObjectProperties> objectPtr(
                drmModeObjectGetProperties(*m_devFd, m_planes->at(planeIndex)->plane_id, DRM_MODE_OBJECT_PLANE),
                [](drmModeObjectPropertiesPtr ptr){drmModeFreeObjectProperties(ptr);});

    for (unsigned int i = 0; i < objectPtr->count_props; i++) {
        std::shared_ptr<drmModePropertyRes> resPtr(
                    drmModeGetProperty(*m_devFd, objectPtr->props[i]),
                    [](drmModePropertyPtr ptr){drmModeFreeProperty(ptr);});
        if (!strcmp(resPtr->name, "ZPOS")) {
            drmModeObjectSetProperty(*m_devFd, m_planes->at(planeIndex)->plane_id,
                                     DRM_MODE_OBJECT_PLANE, resPtr->prop_id, zpos);
            break;
        }
    }
}

void RkDrmDisplay::SetAlpha(uint64_t alpha, DrmPlaneSequence planeIndex)
{
    if (planeIndex >= m_planes->size())
        return;

    std::shared_ptr<drmModeObjectProperties> objectPtr(
                drmModeObjectGetProperties(*m_devFd, m_planes->at(planeIndex)->plane_id, DRM_MODE_OBJECT_PLANE),
                [](drmModeObjectPropertiesPtr ptr){drmModeFreeObjectProperties(ptr);});

    for (unsigned int i = 0; i < objectPtr->count_props; i++) {
        std::shared_ptr<drmModePropertyRes> resPtr(
                    drmModeGetProperty(*m_devFd, objectPtr->props[i]),
                    [](drmModePropertyPtr ptr){drmModeFreeProperty(ptr);});
        if (!strcmp(resPtr->name, "GLOBAL_ALPHA")) {
            drmModeObjectSetProperty(*m_devFd, m_planes->at(planeIndex)->plane_id,
                                     DRM_MODE_OBJECT_PLANE, resPtr->prop_id, alpha);
            break;
        }
    }
}

void RkDrmDisplay::SetDisplayRect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    m_crtcX = x;
    m_crtcY = y;
    m_crtcWidth = width;
    m_crtcHeight = height;
}

bool RkDrmDisplay::InitDrm() {
    shared_ptr<int> fd(new int(0), [](int *p) {
        if (*p > 0)
            close(*p);
        delete p;
    });
    if (!OpenDrmFd(fd))
        return false;

    if (!GetDrmInfo(fd))
        return false;
    m_devFd = fd;

    size_t crtcIndex = 0;
    for (size_t i = 0; i < m_planes->size(); i++) {
        drmModePlanePtr plane = m_planes->at(i);
        if (!(plane->possible_crtcs & 0x00000001 << crtcIndex))
            continue;
        m_hdmiPlanes.emplace_back(plane);
    }

    if (m_hdmiPlanes.size() != 4) {
        // TODO:
        return false;
    }

    YLOG_INFO("RkDrmDisplay Init Successful!");
    return true;
}

bool RkDrmDisplay::AllocDrmPrimeFrame(uint32_t width, uint32_t height, AVFrame *frame) {
    if (!frame || !width || !height)
        return false;

    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = width;
    frame->height = height;

    // 记录cd.handle的上下文，用于后面销毁dumb buffer
    DrmDumbBufferCtx *drmCtx = new DrmDumbBufferCtx;
    if(!drmCtx) {
        YLOG_ERROR("DrmDumbBufferCtx create is faile");
        return false;
    }

    // 分配 DRM Frame Descriptor 这个 desc 的生命周期必须交给 AVBufferRef 管理
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (!desc) {
        YLOG_ERROR("AVDRMFrameDescriptor create is faile");
        return false;
    }

    // 创建 dumb buffer
    drm_mode_create_dumb cd = {};
    cd.width = width;
    cd.height = height * 3 / 2;
    cd.bpp = 8;

    if (drmIoctl(*m_devFd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        YLOG_ERROR("cd create DUMB is faile");
        av_free(desc);
        return false;
    }

    // 将 dumb buffer 导出为 PRIME fd
    // prime_fd 会传给 FFmpeg / 下游组件
    int prime_fd = -1;
    if (drmPrimeHandleToFD(*m_devFd, cd.handle, DRM_CLOEXEC, &prime_fd) < 0) {
        YLOG_ERROR("drmPrimeHandleToFD is faile");
        drm_mode_destroy_dumb dd = { cd.handle };
        drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        av_free(desc);
        return false;
    }

    // 映射 dumb buffer 到 CPU 地址空间
    drm_mode_map_dumb md = {};
    md.handle = cd.handle;

    if (drmIoctl(*m_devFd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        YLOG_ERROR("MODE_MAP_DUMB is faile");
        close(prime_fd);

        drm_mode_destroy_dumb dd = {};
        dd.handle = cd.handle;
        drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);

        av_free(desc);
        return false;
    }

    void *cpu_ptr = mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, *m_devFd, md.offset);
    if (cpu_ptr == MAP_FAILED) {
        YLOG_ERROR("mmap is faile");
        close(prime_fd);

        drm_mode_destroy_dumb dd = {};
        dd.handle = cd.handle;
        drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);

        av_free(desc);
        return false;
    }

    // 记录cd.handle 用于后面销毁dumb
    drmCtx->handle = cd.handle;

    // 填充 AVDRMFrameDescriptor
    desc->nb_objects = 1;

    desc->objects[0].fd = prime_fd;
    desc->objects[0].size = cd.size;
    desc->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    desc->objects[0].ptr = cpu_ptr;

    desc->nb_layers = 1;
    desc->layers[0].format = DRM_FORMAT_NV12;
    desc->layers[0].nb_planes = 2;

    // Y plane
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = 0;
    desc->layers[0].planes[0].pitch = width;

    // UV plane
    desc->layers[0].planes[1].object_index = 0;
    desc->layers[0].planes[1].offset = width * height;
    desc->layers[0].planes[1].pitch = width;

    // 使用 av_buffer_create 管理 desc 生命周期 av_frame_free 只会 unref AVBufferRef 真正的资源释放必须放在 free 回调中
    AVBufferRef *buf = av_buffer_create(reinterpret_cast<uint8_t *>(desc), sizeof(AVDRMFrameDescriptor), [](void *opaque, uint8_t *data) {
        // free 回调：最后一个引用释放时调用
        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;
        DrmDumbBufferCtx *drmCtx = reinterpret_cast<DrmDumbBufferCtx *>(opaque);

        // 解除 CPU 映射
        if (desc->objects[0].ptr) {
            munmap(desc->objects[0].ptr, desc->objects[0].size);
            desc->objects[0].ptr = nullptr;
        }

        // 关闭 PRIME fd
        if (desc->objects[0].fd >= 0) {
            close(desc->objects[0].fd);
            desc->objects[0].fd = -1;
        }

        // 销毁 dumb buffer
        if (drmCtx->handle > 0) {
            drm_mode_destroy_dumb dd = {};
            dd.handle = desc->objects[1].fd;
            drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            desc->objects[1].fd = 0;
        }

        delete drmCtx;
        av_free(desc);
    },
    drmCtx, 0);

    // 创建 AVBufferRef 失败的兜底释放
    if (!buf) {
        YLOG_ERROR("AVBufferRef create is faile");
        munmap(desc->objects[0].ptr, desc->objects[0].size);
        close(desc->objects[0].fd);

        drm_mode_destroy_dumb dd = {(uint32_t)desc->objects[1].fd};
        drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);

        av_free(desc);
        return false;
    }

    frame->buf[0] = buf;
    frame->data[0] = buf->data;
    return true;
}

bool RkDrmDisplay::DisplayDrmFrame(AVFrame *frame) {
    if (!frame)
        return false;

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    FrameResourse *resourse = new FrameResourse();
    resourse->frame = frame;
    uint32_t pitches[AV_DRM_MAX_PLANES];
    uint32_t offsets[AV_DRM_MAX_PLANES];
    uint32_t bo_handles[AV_DRM_MAX_PLANES];
    for (int i = 0; i < AV_DRM_MAX_PLANES; i++) {
        pitches[i] = desc->layers[0].planes[i].pitch;
        offsets[i] = desc->layers[0].planes[i].offset;
    }

    int ret = drmPrimeFDToHandle(*m_devFd, desc->objects[0].fd, &resourse->gemClose.handle);
    bo_handles[0] = resourse->gemClose.handle;
    bo_handles[1] = resourse->gemClose.handle;
    bo_handles[2] = resourse->gemClose.handle;
    bo_handles[3] = resourse->gemClose.handle;

    uint32_t format = desc->layers[0].format;  // DRM_FORMAT_NV12
    ret = drmModeAddFB2(*m_devFd, frame->width, frame->height, format, bo_handles, pitches, offsets,
                        &resourse->bufId, 0);
    if (ret) {
        av_frame_free(&frame);
        drmModeRmFB(*m_devFd, resourse->bufId);
        drmIoctl(*m_devFd, DRM_IOCTL_GEM_CLOSE, &resourse->gemClose);
        return false;
    }

    if (!AtomicSetPlaneInternal(resourse->bufId, m_crtcX, m_crtcY, m_crtcWidth, m_crtcHeight, frame->width,
                        frame->height)) {
        if (errno != EBUSY)
            YLOG_ERROR("AtomicSetPlane failed.");
        av_frame_free(&frame);
        drmModeRmFB(*m_devFd, resourse->bufId);
        drmIoctl(*m_devFd, DRM_IOCTL_GEM_CLOSE, &resourse->gemClose);
        return false;
    }

    m_frameQueue.push_back(resourse);
    return true;
}

bool RkDrmDisplay::DisplayNormalFrame(AVFrame *frame) {
    if (!frame)
        return false;

    uint32_t drmFormat = 0;
    uint32_t bpp = 32;
    switch (frame->format) {
        case AV_PIX_FMT_YUV420P:
            drmFormat = DRM_FORMAT_NV12;
            break;
        case AV_PIX_FMT_NV12:
            drmFormat = DRM_FORMAT_NV12;
            bpp = 16;
            break;
        case AV_PIX_FMT_NV21:
            drmFormat = DRM_FORMAT_NV12;
            bpp = 16;
            break;
        case AV_PIX_FMT_ARGB:
            drmFormat = DRM_FORMAT_BGRA8888;
            break;
        case AV_PIX_FMT_RGBA:
            drmFormat = DRM_FORMAT_ABGR8888;
            break;
        case AV_PIX_FMT_ABGR:
            drmFormat = DRM_FORMAT_RGBA8888;
            break;
        case AV_PIX_FMT_BGRA:
            drmFormat = DRM_FORMAT_ARGB8888;
            break;
        case AV_PIX_FMT_0RGB:
            drmFormat = DRM_FORMAT_BGRX8888;
            break;
        case AV_PIX_FMT_RGB0:
            drmFormat = DRM_FORMAT_XBGR8888;
            break;
        case AV_PIX_FMT_0BGR:
            drmFormat = DRM_FORMAT_RGBX8888;
            break;
        case AV_PIX_FMT_BGR0:
            drmFormat = DRM_FORMAT_XRGB8888;
            break;
        default:
            return false;
    }
    if (m_mapRes && (m_mapRes->width != frame->width || m_mapRes->height != frame->height ||
                     m_mapRes->format != drmFormat)) {
        drm_mode_destroy_dumb dd;
        if (m_mapRes->mapping)
            munmap(m_mapRes->mapping, m_mapRes->size);
        if (m_mapRes->bufId)
            drmModeRmFB(*m_devFd, m_mapRes->bufId);
        if (m_mapRes->handle)
            drmIoctl(*m_devFd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        delete m_mapRes;
        m_mapRes = nullptr;
    }

    if (!m_mapRes) {
        auto errExit = [&frame, this]() -> bool {
            av_frame_free(&frame);
            delete m_mapRes;
            m_mapRes = nullptr;
            return false;
        };
        m_mapRes = new MapResourse();
        m_mapRes->width = frame->width;
        m_mapRes->height = frame->height;
        m_mapRes->format = drmFormat;

        drm_mode_create_dumb cd;
        cd.width = frame->width;
        cd.height = frame->height;
        cd.bpp = bpp;
        cd.flags = 0;
        int err = drmIoctl(*m_devFd, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
        if (err < 0) {
            return errExit();
        }

        m_mapRes->handle = cd.handle;
        m_mapRes->pitch = cd.pitch;
        m_mapRes->size = cd.size;

        uint32_t handles[4], pitches[4], offsets[4];
        memset(handles, 0, 4 * sizeof(uint32_t));
        memset(pitches, 0, 4 * sizeof(uint32_t));
        memset(offsets, 0, 4 * sizeof(uint32_t));

        if (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_NV21 || frame->format == AV_PIX_FMT_YUV420P) {
            handles[0] = cd.handle;
            pitches[0] = frame->width;
            offsets[0] = 0;
            handles[1] = cd.handle;
            pitches[1] = frame->width;
            offsets[1] = frame->width * frame->height;
            handles[2] = cd.handle;
            pitches[2] = frame->width;
            offsets[2] = frame->width * frame->height * 1.25;
        }
        else {
            handles[0] = cd.handle;
            pitches[0] = cd.pitch;
            offsets[0] = 0;
        }

        int ret = drmModeAddFB2(*m_devFd, frame->width, frame->height, drmFormat, handles, pitches,
                                offsets, &m_mapRes->bufId, 0);
        if (ret) {
            return errExit();
        }

        drm_mode_map_dumb md;
        memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        err = drmIoctl(*m_devFd, DRM_IOCTL_MODE_MAP_DUMB, &md);
        if (err < 0) {
            return errExit();
        }
        m_mapRes->mapping = (uint8_t *)mmap(nullptr, m_mapRes->size, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, *m_devFd, md.offset);
        if (!m_mapRes->mapping || m_mapRes->mapping == MAP_FAILED) {
            return errExit();
        }
    }

    int64_t frameSize = frame->width * frame->height;

    if (frame->format == AV_PIX_FMT_NV12) {
        memcpy(m_mapRes->mapping, frame->data[0], frame->width * frame->height);
        memcpy(m_mapRes->mapping + frameSize, frame->data[1], frameSize / 2);
    }
    else if (frame->format == AV_PIX_FMT_NV21) {
        memcpy(m_mapRes->mapping, frame->data[0], frame->width * frame->height);
        for (int i = 0; i < frameSize / 2; i += 2)
        {
            memcpy(m_mapRes->mapping + frameSize + i + 1, frame->data[1] + i, 1);
            memcpy(m_mapRes->mapping + frameSize + i, frame->data[1] + i + 1, 1);
        }
    }
    else if (frame->format == AV_PIX_FMT_YUV420P)
    {
        memcpy(m_mapRes->mapping, frame->data[0], frameSize);
        for (int i = 0; i < frameSize / 4; i++)
        {
            memcpy(m_mapRes->mapping + frameSize + 2 * i, frame->data[1] + i, 1);
            memcpy(m_mapRes->mapping + frameSize + 2 * i + 1, frame->data[2] + i, 1);
        }
    }
    else
    {
        for (int i = 0; i < frame->height; i++) {
            memcpy(m_mapRes->mapping + i * m_mapRes->pitch, frame->data[0] + i * frame->width * 4,
                   frame->width * 4);
        }
    }

    if (!AtomicSetPlaneInternal(m_mapRes->bufId, m_crtcX, m_crtcY, m_crtcWidth, m_crtcHeight,
                        m_mapRes->width, m_mapRes->height)) {
        if (errno != EBUSY)
            YLOG_ERROR("AtomicSetPlane failed.");
    }

//    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
//        if (frame->data[i])
//            av_freep(&frame->data[i]);
//    }
    av_frame_free(&frame);

    return true;
}

bool RkDrmDisplay::OpenDrmFd(std::shared_ptr<int> fd) {
    *fd = open("/dev/dri/card0", O_RDWR);
    if (*fd < 0) {
        YLOG_ERROR("drm fd open failed.");
        return false;
    }

    /* set FD_CLOEXEC flag */
    int flags = fcntl(*fd, F_GETFD);
    if (flags < 0 || fcntl(*fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        YLOG_ERROR("fcntl FD_CLOEXEC failed.");
        return false;
    }

    /* check capability */
    uint64_t has_dumb;
    int ret = drmGetCap(*fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
    if (ret < 0 || has_dumb == 0) {
        YLOG_ERROR("drmGetCap DRM_CAP_DUMB_BUFFER failed or doesn't have dumb buffer.");
        return false;
    }

    ret = drmSetClientCap(*fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        YLOG_ERROR("drm set client cap atomic failed.");
        return false;
    }

    ret = drmSetClientCap(*fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
        YLOG_ERROR("drm set client cap universal planes failed.");
        return false;
    }
    return true;
}

bool RkDrmDisplay::GetDrmInfo(std::shared_ptr<int> fd) {
    shared_ptr<drmModeRes> resourses(drmModeGetResources(*fd),
                                     [](drmModeResPtr ptr) { drmModeFreeResources(ptr); });
    if (!resourses) {
        YLOG_ERROR("drm get resources failed.");
        return false;
    }

    shared_ptr<connectorVec> connectors(new connectorVec(), [](connectorVec *vec) {
        for (auto &ptr : *vec) {
            if (ptr)
                drmModeFreeConnector(ptr);
        }
        delete vec;
    });
    connectors->resize(resourses->count_connectors, nullptr);
    for (int i = 0; i < resourses->count_connectors; i++) {
        connectors->at(i) = drmModeGetConnector(*fd, resourses->connectors[i]);
        if (!connectors->at(i)) {
            YLOG_ERROR("drm get connector failed.");
            return false;
        }
    }

    shared_ptr<encoderVec> encoders(new encoderVec(), [](encoderVec *vec) {
        for (auto &ptr : *vec) {
            if (ptr)
                drmModeFreeEncoder(ptr);
        }
        delete vec;
    });
    encoders->resize(resourses->count_encoders, nullptr);
    for (int i = 0; i < resourses->count_encoders; i++) {
        encoders->at(i) = drmModeGetEncoder(*fd, resourses->encoders[i]);
        if (!encoders->at(i)) {
            YLOG_ERROR("drm get encoder failed.");
            return false;
        }
    }

    shared_ptr<crtcVec> crtcs(new crtcVec(), [](crtcVec *vec) {
        for (auto &ptr : *vec) {
            if (ptr)
                drmModeFreeCrtc(ptr);
        }
        delete vec;
    });
    crtcs->resize(resourses->count_crtcs, nullptr);
    for (int i = 0; i < resourses->count_crtcs; i++) {
        crtcs->at(i) = drmModeGetCrtc(*fd, resourses->crtcs[i]);
        if (!crtcs->at(i)) {
            YLOG_ERROR("drm get crtc failed.");
            return false;
        }
    }

    shared_ptr<drmModePlaneRes> planeRes(drmModeGetPlaneResources(*fd), [](drmModePlaneResPtr ptr) {
        drmModeFreePlaneResources(ptr);
    });
    if (!planeRes) {
        YLOG_ERROR("drm get plane resources failed.");
        return false;
    }

    shared_ptr<planeVec> planes(new planeVec(), [](planeVec *vec) {
        for (auto &ptr : *vec) {
            if (ptr)
                drmModeFreePlane(ptr);
        }
        delete vec;
    });
    planes->resize(planeRes->count_planes, nullptr);
    for (int i = 0; i < planeRes->count_planes; i++) {
        planes->at(i) = drmModeGetPlane(*fd, planeRes->planes[i]);
        if (!planes->at(i)) {
            YLOG_ERROR("drm get plane failed.");
            return false;
        }
    }

    // rk3399目前有两种图层的板子，它们的图层获取顺序是不一致的
    // 这里手动调换一下一种板子图层获取的顺序，使两种板子保持一致
    if (planes->at(0)->count_formats == 14) {
        std::swap(planes->at(3), planes->at(2));
    }

    m_connectors = connectors;
    m_encoders = encoders;
    m_crtcs = crtcs;
    m_planes = planes;
    return true;
}

bool RkDrmDisplay::CheckPlaneFormat(drmModePlanePtr plane, uint32_t format) {
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == format)
            return true;
    }
    return false;
}

void RkDrmDisplay::Display(AVFrame *frame) {
    switch (frame->format) {
        case AV_PIX_FMT_DRM_PRIME:
            DisplayDrmFrame(frame);
            break;
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_RGB0:
        case AV_PIX_FMT_0BGR:
        case AV_PIX_FMT_BGR0:
        case AV_PIX_FMT_NV12:
            DisplayNormalFrame(frame);
            break;
        case AV_PIX_FMT_NV21:
            DisplayNormalFrame(frame);
            break;
        case AV_PIX_FMT_YUV420P:
            DisplayNormalFrame(frame);
            break;
        default:
            av_frame_free(&frame);
            break;
    }
}

void RkDrmDisplay::ClearDisplay() {
    PumpDrmEventsInternal(0);
    AtomicSetPlaneInternal(0, 0, 0, 0, 0, 0, 0);
}
