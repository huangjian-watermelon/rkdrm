#ifndef RKDRM_H
#define RKDRM_H

#include <memory>
#include <vector>
#include <deque>
#include "xf86drmMode.h"
#include <rockchip/mpp_frame.h>
extern "C"
{
#include "libavcodec/avcodec.h"
}
#include <rga/rga.h>
#include <rga/drmrga.h>
#include <rga/RgaApi.h>
#include <rga/RockchipRga.h>
#include <rga/RockchipRgaMacro.h>

enum DrmPlaneSequence{
    DESKTOP_PLANE,
    CURSOUR_PLANE,
    NON_PREFERRED_PLANE,
    VIDEO_PLANE,
    TOTAL_PLANE
};

struct FrameResourse
{
    AVFrame *frame = nullptr;
    uint32_t bufId = 0;
    drm_gem_close gemClose;
};

enum RotateAngle
{
    Rotate90 = HAL_TRANSFORM_ROT_90,
    Rotate180 = HAL_TRANSFORM_ROT_180,
    Rotate270 = HAL_TRANSFORM_ROT_270,
    MirrorHorizontal = HAL_TRANSFORM_FLIP_H,
    MirrorVertical = HAL_TRANSFORM_FLIP_V,
};

class RkDrmDisplay
{
public:
    RkDrmDisplay();
    ~RkDrmDisplay();

public:
    void SetRotateAngle(uint64_t angle, DrmPlaneSequence planeIndex);

    void SetRotateAngleText(AVFrame *frame, RotateAngle rotation);

    void SetRotateAngleVideo(AVFrame *&frame, RotateAngle rotation);

    void SetZposValue(uint64_t zpos, DrmPlaneSequence planeIndex);

    void SetAlpha(uint64_t alpha, DrmPlaneSequence planeIndex);

    void SetRenderPlane(DrmPlaneSequence plane) { m_planeIndex = plane; }

    void SetDisplayRect(int32_t x, int32_t y, uint32_t width, uint32_t height);

    void Display(AVFrame *frame);

    void ClearDisplay();

    static bool InitDrm();

private:
    bool AtomicSetPlane(uint32_t fbId, int32_t crtcX, int32_t crtcY, uint32_t crtcWidth,
                        uint32_t crtcHeight, uint32_t srcWidth, uint32_t srcHeight);
    bool GetObjectPropertyId(uint32_t objectId, uint32_t objectType, const char *propertyName,
                             uint32_t &propertyId);
    bool FreeFrameRes(FrameResourse *&frameRes);
    bool PumpDrmEvents(int timeoutMs);
    static void OnPageFlipEvent(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                                void *userData);

    bool DisplayDrmFrame(AVFrame *frame);
    bool DisplayNormalFrame(AVFrame *frame);

    bool AllocDrmPrimeFrame(uint32_t width, uint32_t height, AVFrame *frame);

    static bool OpenDrmFd(std::shared_ptr<int> fd);

    static bool GetDrmInfo(std::shared_ptr<int> fd);

    static bool CheckPlaneFormat(drmModePlanePtr plane, uint32_t format);
private:
    int32_t m_crtcX;
    int32_t m_crtcY;
    uint32_t m_crtcWidth;
    uint32_t m_crtcHeight;

    using connectorVec = std::vector<drmModeConnectorPtr>;
    using crtcVec = std::vector<drmModeCrtcPtr>;
    using encoderVec = std::vector<drmModeEncoderPtr>;
    using planeVec = std::vector<drmModePlanePtr>;

    static std::shared_ptr<int> m_devFd;
    static std::shared_ptr<connectorVec> m_connectors;
    static std::shared_ptr<encoderVec> m_encoders;
    static std::shared_ptr<crtcVec> m_crtcs;
    static std::shared_ptr<planeVec> m_planes;
    static planeVec m_hdmiPlanes;

    int m_planeIndex;
    int m_crtcIndex;

    struct MapResourse{
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t handle;
        uint32_t pitch;
        uint32_t bufId;
        uint64_t size;
        uint8_t *mapping = nullptr;
    } *m_mapRes;
    std::deque<FrameResourse *> m_frameQueue;
    bool m_flipPending;
    char* m_dataText = nullptr;
    size_t m_dataTextSize = 0;
};
#endif // RKDRM_H
