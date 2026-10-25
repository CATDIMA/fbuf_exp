#include <iostream>
#include <array>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <poll.h>
#include <random>
#include <thread>
#include <chrono>
#include <fstream>
#include "functions.hpp"
#include <sys/ioctl.h>
#include <sys/mman.h>

std::random_device rdev;
std::mt19937 rng(rdev());

uint32_t get_random_uint32()
{
	std::uniform_int_distribution dist(0U, std::numeric_limits<uint32_t>::max());
	return dist(rng);
}

int main(int argc, char** argv)
{
	//получить карты
	std::vector<std::filesystem::path> cards_paths = getCardPaths();

	//перебор карт
	std::unique_ptr<ConnectedPlaneFD> connected_plane;
	for(const auto& card : cards_paths)
	{
		try
		{
			connected_plane = std::make_unique<ConnectedPlaneFD>(card);
			
			if(connected_plane.get())
				break;
		}
		catch(const ConnectedPlaneFDException& e)
		{
			std::cerr << e.what() << std::endl;
			continue;
		}
		catch(const std::exception& e)
		{
			std::cerr << "Error: " << e.what() << std::endl;
			return -1;
		}
		
	}
	if(!connected_plane.get())
	{
		std::cerr << "Connected plane is nullptr\n";
		return -1;
	}

	//настройка drm
	//1
	drm_mode_create_dumb dumb_buf1{};
	dumb_buf1.width = width;
	dumb_buf1.height = height;
	dumb_buf1.bpp = 32;
	//2
	drm_mode_create_dumb dumb_buf2{};
	dumb_buf2.width = width;
	dumb_buf2.height = height;
	dumb_buf2.bpp = 32;

	ioctl(connected_plane->getFD(), DRM_IOCTL_MODE_CREATE_DUMB, &dumb_buf1);
	ioctl(connected_plane->getFD(), DRM_IOCTL_MODE_CREATE_DUMB, &dumb_buf2);

	//1
	drm_mode_map_dumb map1{};
	map1.handle = dumb_buf1.handle;
	//2
	drm_mode_map_dumb map2{};
	map2.handle = dumb_buf2.handle;

	ioctl(connected_plane->getFD(), DRM_IOCTL_MODE_MAP_DUMB, &map1);
	ioctl(connected_plane->getFD(), DRM_IOCTL_MODE_MAP_DUMB, &map2);

	//1
	void* ptr1 = mmap(nullptr, dumb_buf1.size, PROT_READ | PROT_WRITE, MAP_SHARED, connected_plane->getFD(), map1.offset);
	
	//2
	void* ptr2 = mmap(nullptr, dumb_buf2.size, PROT_READ | PROT_WRITE, MAP_SHARED, connected_plane->getFD(), map2.offset);
	
	//1
	uint32_t fb1_id;
	uint32_t handles1[4] = {dumb_buf1.handle};
	uint32_t pitches1[4] = {dumb_buf1.pitch};
	uint32_t offsets1[4] = {0};
	//2
	uint32_t fb2_id;
	uint32_t handles2[4] = {dumb_buf2.handle};
	uint32_t pitches2[4] = {dumb_buf2.pitch};
	uint32_t offsets2[4] = {0};

	//1
	drmModeAddFB2(connected_plane->getFD(), width, height, DRM_FORMAT_XRGB8888, handles1, pitches1, offsets1, &fb1_id, 0);
	const uint32_t prop_fb_id = connected_plane->getProperty("FB_ID");
	const uint32_t prop_crtc_id = connected_plane->getProperty("CRTC_ID");
	const uint32_t prop_crtc_x = connected_plane->getProperty("CRTC_X");
	const uint32_t prop_crtc_y = connected_plane->getProperty("CRTC_Y");
	const uint32_t prop_crtc_w = connected_plane->getProperty("CRTC_W");
	const uint32_t prop_crtc_h = connected_plane->getProperty("CRTC_H");
	const uint32_t prop_src_x = connected_plane->getProperty("SRC_X");
	const uint32_t prop_src_y = connected_plane->getProperty("SRC_Y");
	const uint32_t prop_src_w = connected_plane->getProperty("SRC_W");
	const uint32_t prop_src_h = connected_plane->getProperty("SRC_H");

	connected_plane->setProperty(prop_fb_id, fb1_id);
	connected_plane->setProperty(prop_crtc_id, connected_plane->getCRTCID());
	connected_plane->setProperty(prop_crtc_x, 0);
	connected_plane->setProperty(prop_crtc_y, 0);
	connected_plane->setProperty(prop_crtc_w, width);
	connected_plane->setProperty(prop_crtc_h, height);
	connected_plane->setProperty(prop_src_x, 0);
	connected_plane->setProperty(prop_src_y, 0);
	connected_plane->setProperty(prop_src_w, width << 16);
	connected_plane->setProperty(prop_src_h, height << 16);
	
	if(int ret = drmModeAtomicCommit(connected_plane->getFD(), connected_plane->getReq(), 0, nullptr); ret < 0)
		throw std::runtime_error("commit failed " + std::string{strerror(-ret)});

	//2
	drmModeAddFB2(connected_plane->getFD(), width, height, DRM_FORMAT_XRGB8888, handles2, pitches2, offsets2, &fb2_id, 0);

	bool flip_done = false;
	drmEventContext ev{};
	ev.version = DRM_EVENT_CONTEXT_VERSION;

	ev.page_flip_handler = [](int fd, unsigned int sequnce, unsigned int tv_sec, unsigned int tv_usec, void* user_data)
	{
		*(reinterpret_cast<bool*>(user_data)) = true;
	};

	ev.sequence_handler = [](int fd, uint64_t, uint64_t, uint64_t user_data)
	{
		*(reinterpret_cast<bool*>(user_data)) = true;
	};

	uint32_t front = fb1_id;
	uint32_t back = fb2_id;

	constexpr unsigned int video_fps = 30;
	constexpr unsigned int monitor_fps = 144;
	uint64_t counter = 0;

	//видео
	std::ifstream video("../video/yamero.raw", std::ios::binary);
	uint32_t* pixels1 = reinterpret_cast<uint32_t*>(ptr1);
	uint32_t* pixels2 = reinterpret_cast<uint32_t*>(ptr2);

	std::array<uint8_t, arr_len> raw_frame;
	std::array<uint32_t, arr_len> frame;

	// основной цикл
	while(true)
	{
		bool sequence_done = false;
		uint64_t queued_sequence = 0;
		
		counter += monitor_fps;
		uint64_t vblanks = counter / video_fps;
		counter %= video_fps;

		int ret = drmCrtcQueueSequence(connected_plane->getFD(), connected_plane->getCRTCID(), DRM_CRTC_SEQUENCE_RELATIVE, vblanks - 1 , &queued_sequence, reinterpret_cast<uint64_t>(&sequence_done));
		if(ret < 0)
			throw std::runtime_error("seqence failed");
		
		if(!readFrame(video, raw_frame))
			break;
		
		void* back_ptr = (back == fb1_id) ? ptr1 : ptr2;
		uint32_t back_pitch = (back == fb1_id) ? dumb_buf1.pitch : dumb_buf2.pitch;
		
		for (size_t y = 0; y < height; ++y)
		{
			auto* dst = reinterpret_cast<uint32_t*>(
				static_cast<uint8_t*>(back_ptr) + y * back_pitch
			);
		
			for (size_t x = 0; x < width; ++x)
			{
				uint32_t gray = raw_frame[y * width + x];
				dst[x] = gray * 0x00010101u;
			}
		}

		while(!sequence_done)
		{
			pollfd pfd{connected_plane->getFD(), POLLIN, 0};

			if(poll(&pfd, 1, -1) < 0)
			{
				if(errno == EINTR)
					continue;

				throw std::runtime_error("poll failed");
			}
			if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				throw std::runtime_error("DRM poll error");

			if((pfd.revents & POLLIN) && drmHandleEvent(connected_plane->getFD(), &ev))
				throw std::runtime_error("drmHandleEvent failed");
		}	

		std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> req{drmModeAtomicAlloc(), drmModeAtomicFree};

		if(drmModeAtomicAddProperty(req.get(), connected_plane->getPrimaryPlaneID(), prop_fb_id, back) < 0)
			throw std::runtime_error("failed to add buffer");

		flip_done = false;

		if(drmModeAtomicCommit(connected_plane->getFD(), req.get(), DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &flip_done) != 0)
			throw std::runtime_error("atomic flip failed");

		while(!flip_done)
		{
			pollfd pfd{connected_plane->getFD(), POLLIN, 0};

			if(poll(&pfd, 1, -1) < 0)
			{
				if(errno == EINTR)
					continue;

				throw std::runtime_error("poll failed");
			}
			if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				throw std::runtime_error("DRM poll error");

			if((pfd.revents & POLLIN) && drmHandleEvent(connected_plane->getFD(), &ev))
				throw std::runtime_error("drmHandleEvent failed");
		}

		std::swap(front, back);
	}

	return 0;
}
