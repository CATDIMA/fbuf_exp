#ifndef _FBBUF_FUNCTIONS_H
#define _FBBUF_FUNCTIONS_H

#include <vector>
#include <filesystem>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <bitset>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <array>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kd.h>
#include <poll.h>
#include <string.h>
#include <iostream>

constexpr unsigned short WIDTH = 256;
constexpr unsigned short HEIGHT = 64;
constexpr unsigned short FRAMEBUF_SIZE = WIDTH * HEIGHT;

std::vector<std::filesystem::path> getCardPaths();
uint32_t getMonitorFrequency(int fd, uint32_t crtc_id);
bool readFrame(std::ifstream& video, std::array<uint8_t, FRAMEBUF_SIZE>& frame);

class ConnectedPlaneFDException : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class ConnectedPlaneFD
{
public:

    ConnectedPlaneFD(const std::filesystem::path&);
    //copy and move constructors are deleted
    ~ConnectedPlaneFD();

    int getFD() const;
    uint32_t getPrimaryPlaneID();
    uint32_t getCRTCID();
    uint32_t getProperty(std::string_view);
    const drmModeAtomicReqPtr getReq();
    void setProperty(uint32_t, uint64_t);
private:
    int fd = -1;
    uint32_t primary_plane_id = 0;
    uint32_t crtc_id = 0;
    std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> req{nullptr, drmModeAtomicFree};
};

class DumbBuffer
{
public:
	DumbBuffer(int);
	~DumbBuffer();

	uint32_t getHandle() const;
	uint32_t getPitch() const;
	uint64_t getSize() const;

private:
	int fd = 0;
	drm_mode_create_dumb dumb_buf;
};

class MappedMemory
{
public:
    MappedMemory(int);
    ~MappedMemory();

    void* data() const;
    uint32_t getBufferHandle() const;
    uint32_t getBufferPitch() const;
    uint64_t getBufferSize() const;
private:
    DumbBuffer buf;
    void* mapped_mem = nullptr;
    drm_mode_map_dumb map;
};

class DoubleFramebuffer
{
public:
    DoubleFramebuffer(ConnectedPlaneFD&);
    ~DoubleFramebuffer();
    uint32_t getBuffer1Id() const;
    uint32_t getBuffer2Id() const;
    uint32_t getPropId() const;
    MappedMemory& getBuffer1();
    MappedMemory& getBuffer2();

    bool playVideo(std::ifstream&, uint32_t src_fps, uint32_t dst_fps);

private:
    uint32_t fb1_id, fb2_id;
    uint32_t prop_fb_id;
    uint32_t handles1[4] = {0};
    uint32_t handles2[4] = {0};
    uint32_t pitches1[4] = {0};
    uint32_t pitches2[4] = {0};
    uint32_t offsets1[4] = {0};
    uint32_t offsets2[4] = {0};
    MappedMemory buf1, buf2;
    int fd = 0;
    uint32_t crtc_id = 0;
    uint32_t primary_plane_id = 0;

    std::array<uint8_t, FRAMEBUF_SIZE> raw_frame;
	std::array<uint32_t, FRAMEBUF_SIZE> frame;
};

class TTYguard
{
public:
    TTYguard();
    ~TTYguard();
private:
    int tty_fd = 0;
    int old_state = 0;
};

#endif