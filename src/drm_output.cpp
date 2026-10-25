#include "drm_output.hpp"

bool readFrame(std::ifstream& video, std::array<uint8_t, FRAMEBUF_SIZE>& frame)
{
	video.read(reinterpret_cast<char*>(frame.data()), frame.size());

	return video.gcount() == FRAMEBUF_SIZE;
}

void processRawFrame(const std::array<uint8_t, FRAMEBUF_SIZE>& src, std::array<uint32_t, FRAMEBUF_SIZE>& dst)
{
	for (size_t i = 0; i < FRAMEBUF_SIZE; ++i)
	{	
		dst[i] = static_cast<uint32_t>(src[i] * 0x00010101u);
	}
}

std::vector<std::filesystem::path> getCardPaths()
{
    std::vector<std::filesystem::path> paths;

    const std::filesystem::path dev_dri_path("/dev/dri");
	for(const auto& card : std::filesystem::directory_iterator(dev_dri_path))
	{
		if(card.path().filename().generic_string().find("card") == 0)
            paths.push_back(card.path());
	}

    return paths;
}

ConnectedPlaneFD::ConnectedPlaneFD(const std::filesystem::path& path)
{
    fd = open(path.c_str(), O_RDWR | O_CLOEXEC);

    if(fd < 0)
	{
		throw std::runtime_error("Failed to open a card!\n");
	}

    if(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        throw std::runtime_error("drmSetClientCap failed");

	if(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
    	throw std::runtime_error("Atomic KMS not supported");

    std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources{drmModeGetResources(fd), drmModeFreeResources};
	uint32_t encoder_id = 0;
	for(int i = 0; i < resources->count_connectors; ++i)
	{
		uint32_t conncetor_id = resources->connectors[i];
		std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> conn{drmModeGetConnector(fd, conncetor_id), drmModeFreeConnector};
		
		if(conn->connection == DRM_MODE_CONNECTED)
		{
			if(conn->encoder_id != 0)
			{
				encoder_id = conn->encoder_id;
				break;
			}
		}
	}
	
	if(encoder_id == 0)
	{
        close(fd);
		throw ConnectedPlaneFDException("Encoder ID is 0. Cannot continue\n");
	}
	
	std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)> encoder(drmModeGetEncoder(fd, encoder_id), drmModeFreeEncoder);
	crtc_id = encoder->crtc_id;
	encoder.reset();

	int crtc_idx = -1;
	for(int i = 0; i < resources->count_crtcs; ++i)
	{
		if(resources->crtcs[i] == crtc_id)
		{
			crtc_idx = i;
			break;
		}
	}
	resources.reset();

	if(crtc_idx < 0)
	{
        close(fd);
		throw ConnectedPlaneFDException("CRTC index undefined. Abort\n");
	}

	std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)> planes{drmModeGetPlaneResources(fd), drmModeFreePlaneResources};
	for(int i = 0; i < planes->count_planes; ++i)
	{
		bool is_primary = false;
		uint32_t plane_id = planes->planes[i];
		std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)> plane{drmModeGetPlane(fd, plane_id), drmModeFreePlane};
		std::unique_ptr<drmModeObjectProperties, decltype(&drmModeFreeObjectProperties)> props{drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE), drmModeFreeObjectProperties};

		for(int p = 0; p < props->count_props; ++p)
		{
			std::unique_ptr<drmModePropertyRes, decltype(&drmModeFreeProperty)> prop{drmModeGetProperty(fd, props->props[p]), drmModeFreeProperty};

            if (std::string_view{prop->name} == "type")
            {
                is_primary = props->prop_values[p] == DRM_PLANE_TYPE_PRIMARY;
                break;
            }
		}

        uint32_t possible_crtc = 1 << crtc_idx;
		if(!is_primary || !(plane->possible_crtcs & possible_crtc))
			continue;
        else
        {
            primary_plane_id = plane->plane_id;
            break;
        }
	}
	planes.reset();

    if(primary_plane_id == 0)
    {
        close(fd);
        throw ConnectedPlaneFDException("No primary plane");
    }

	std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> request{drmModeAtomicAlloc(), drmModeAtomicFree};
	req = std::move(request);

	if(req.get() == nullptr)
	{
		close(fd);
		throw ConnectedPlaneFDException("Failed to create a request");
	}
}

ConnectedPlaneFD::~ConnectedPlaneFD()
{
    if(close(fd) < 0)
		std::cerr << "close failed in ~ConnectedPlaneFD: " << strerror(errno) << std::endl;
}

int ConnectedPlaneFD::getFD() const
{
    return fd;
}

uint32_t ConnectedPlaneFD::getPrimaryPlaneID()
{
    return primary_plane_id;
}

uint32_t ConnectedPlaneFD::getCRTCID()
{
    return crtc_id;
}

uint32_t ConnectedPlaneFD::getProperty(std::string_view prop)
{
	std::unique_ptr<drmModeObjectProperties, decltype(&drmModeFreeObjectProperties)> props{drmModeObjectGetProperties(fd, primary_plane_id, DRM_MODE_OBJECT_PLANE), drmModeFreeObjectProperties};

	if(props.get() == nullptr)
	{
		throw std::runtime_error("getPropertyID: cannot get properties");
	}

	for(int i = 0; i < props->count_props; ++i)
	{
		std::unique_ptr<drmModePropertyRes, decltype(&drmModeFreeProperty)> prop_res{drmModeGetProperty(fd, props->props[i]), drmModeFreeProperty};

		if(std::string_view{prop_res->name} == prop)
		{
			return prop_res->prop_id;
		}
	}

	throw std::runtime_error("getPropertyID: no such property" + std::string{prop.data()});
}

drmModeAtomicReqPtr ConnectedPlaneFD::getReq()
{
	return req.get();
}

void ConnectedPlaneFD::setProperty(uint32_t prop, uint64_t value)
{
	if(drmModeAtomicAddProperty(req.get(), primary_plane_id, prop, value) < 0)
		throw std::runtime_error("setProperty failed");
}

DumbBuffer::DumbBuffer(int plane_fd)
{
	fd = plane_fd;
	dumb_buf.width = WIDTH;
	dumb_buf.height = HEIGHT;
	dumb_buf.bpp = 32;
	dumb_buf.flags = 0;

	if(int ret = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb_buf); ret != 0)
		throw std::system_error(-ret, std::generic_category(), "DRM_IOCTL_MODE_CREATE_DUMB failed");
}

DumbBuffer::~DumbBuffer()
{
	if(dumb_buf.handle != 0)
		drmModeDestroyDumbBuffer(fd, dumb_buf.handle);
}

uint32_t DumbBuffer::getHandle() const
{
	return dumb_buf.handle;
}

uint32_t DumbBuffer::getPitch() const
{
	return dumb_buf.pitch;
}

uint64_t DumbBuffer::getSize() const
{
	return dumb_buf.size;
}

MappedMemory::MappedMemory(int fd) : buf(fd)
{
	map.handle = buf.getHandle();
	if(int ret = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map); ret != 0)
		throw std::system_error(-ret, std::generic_category(), "DRM_IOCTL_MODE_MAP_DUMB failed");

	mapped_mem = mmap(nullptr, buf.getSize(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if(mapped_mem == MAP_FAILED)
	{
		throw std::system_error(-errno, std::generic_category(), "mmap failed");
	}
}

MappedMemory::~MappedMemory()
{
	if(mapped_mem)
	{
		munmap(mapped_mem, buf.getSize());
	}
}

void* MappedMemory::data() const
{
	return mapped_mem;
}

uint32_t MappedMemory::getBufferHandle() const
{
	return buf.getHandle();
}

uint32_t MappedMemory::getBufferPitch() const
{
	return buf.getPitch();
}

uint64_t MappedMemory::getBufferSize() const
{
	return buf.getSize();
}

DoubleFramebuffer::DoubleFramebuffer(ConnectedPlaneFD& plane) : buf1(plane.getFD()), buf2(plane.getFD())
{
	handles1[0] = buf1.getBufferHandle();
	handles2[0] = buf2.getBufferHandle();
	pitches1[0] = buf1.getBufferPitch();
	pitches2[0] = buf2.getBufferPitch();
	fd = plane.getFD();
	crtc_id = plane.getCRTCID();
	primary_plane_id = plane.getPrimaryPlaneID();

	if(int ret = drmModeAddFB2(fd, WIDTH, HEIGHT, DRM_FORMAT_XRGB8888, handles1, pitches1, offsets1, &fb1_id, 0); ret != 0)
	{
		throw std::system_error(-ret, std::generic_category(), "drmModeAddFB2 for buf1 failed");
	}
	if(int ret = drmModeAddFB2(fd, WIDTH, HEIGHT, DRM_FORMAT_XRGB8888, handles2, pitches2, offsets2, &fb2_id, 0); ret != 0)
	{
		throw std::system_error(-ret, std::generic_category(), "drmModeAddFB2 for buf2 failed");
	}

	prop_fb_id = plane.getProperty("FB_ID");
	const uint32_t prop_crtc_id = plane.getProperty("CRTC_ID");
	const uint32_t prop_crtc_x = plane.getProperty("CRTC_X");
	const uint32_t prop_crtc_y = plane.getProperty("CRTC_Y");
	const uint32_t prop_crtc_w = plane.getProperty("CRTC_W");
	const uint32_t prop_crtc_h = plane.getProperty("CRTC_H");
	const uint32_t prop_src_x = plane.getProperty("SRC_X");
	const uint32_t prop_src_y = plane.getProperty("SRC_Y");
	const uint32_t prop_src_w = plane.getProperty("SRC_W");
	const uint32_t prop_src_h = plane.getProperty("SRC_H");

	try
	{
		plane.setProperty(prop_fb_id, fb1_id);
		plane.setProperty(prop_crtc_id, plane.getCRTCID());
		plane.setProperty(prop_crtc_x, 0);
		plane.setProperty(prop_crtc_y, 0);
		plane.setProperty(prop_crtc_w, WIDTH * 2);
		plane.setProperty(prop_crtc_h, HEIGHT * 2);
		plane.setProperty(prop_src_x, 0);
		plane.setProperty(prop_src_y, 0);
		plane.setProperty(prop_src_w, WIDTH << 16);
		plane.setProperty(prop_src_h, HEIGHT << 16);
	}
	catch(const std::exception& e)
	{
		drmModeRmFB(fd, fb1_id);
		drmModeRmFB(fd, fb2_id);
		throw e;
	}
	
	if(int ret = drmModeAtomicCommit(fd, plane.getReq(), 0, nullptr); ret < 0)
	{
		drmModeRmFB(fd, fb1_id);
		drmModeRmFB(fd, fb2_id);
		throw std::system_error(-ret, std::generic_category(), "drmModeAtomicCommit failed");
	}

	std::unique_ptr<drmModeCrtc, decltype(&drmModeFreeCrtc)> crtc(drmModeGetCrtc(fd, crtc_id), drmModeFreeCrtc);
	monitor_fps = crtc->mode.vrefresh;

	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = [](int fd, unsigned int sequnce, unsigned int tv_sec, unsigned int tv_usec, void* user_data)
	{
		*(reinterpret_cast<bool*>(user_data)) = true;
	};
	ev.sequence_handler = [](int fd, uint64_t, uint64_t, uint64_t user_data)
	{
		*(reinterpret_cast<bool*>(user_data)) = true;
	};

	front = fb1_id;
	back = fb2_id;
}

DoubleFramebuffer::~DoubleFramebuffer()
{
	drmModeRmFB(fd, fb1_id);
	drmModeRmFB(fd, fb2_id);
}

uint32_t DoubleFramebuffer::getBuffer1Id() const
{
	return fb1_id;
}

uint32_t DoubleFramebuffer::getBuffer2Id() const
{
	return fb2_id;
}

uint32_t DoubleFramebuffer::getPropId() const
{
	return prop_fb_id;
}

MappedMemory& DoubleFramebuffer::getBuffer1()
{
	return buf1;
}

MappedMemory& DoubleFramebuffer::getBuffer2()
{
	return buf2;
}

void DoubleFramebuffer::playVideo(const std::array<uint32_t, FRAMEBUF_SIZE>& frame, uint32_t src_fps)
{
	bool flip_done = false;
	sequence_done = false;
	
	if(src_fps > monitor_fps)
		throw std::runtime_error("src_framerate is higher than monitor framerate");
	uint64_t queued_sequence = 0;
	counter += monitor_fps;
	uint64_t vblanks = counter / src_fps;
	counter %= src_fps;

	int ret = drmCrtcQueueSequence(fd, crtc_id, DRM_CRTC_SEQUENCE_RELATIVE, vblanks - 1 , &queued_sequence, reinterpret_cast<uint64_t>(&sequence_done));
	if(ret < 0)
		throw std::runtime_error("sequence failed");
	
	while(!sequence_done)
	{
		pollfd pfd{fd, POLLIN, 0};

		if(poll(&pfd, 1, -1) < 0)
		{
			if (errno == EINTR)
				continue;
	
			throw std::system_error(errno, std::generic_category(), "poll failed");
		}
		if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			throw std::runtime_error("DRM poll error");

		if((pfd.revents & POLLIN) && drmHandleEvent(fd, &ev))
			throw std::runtime_error("drmHandleEvent failed");
	}	

	void* back_ptr = (back == fb1_id) ? buf1.data() : buf2.data();
	uint32_t back_pitch = (back == fb1_id) ? buf1.getBufferPitch() : buf2.getBufferPitch();
	std::byte* dst = static_cast<std::byte*>(back_ptr);
	for (uint32_t y = 0; y < HEIGHT; ++y)
	{
		std::memcpy(dst + y * back_pitch, frame.data() + y * WIDTH, WIDTH * sizeof(uint32_t));
	}

	std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> req{drmModeAtomicAlloc(), drmModeAtomicFree};
	if(req == nullptr)
		throw std::runtime_error("drmModeAtomicReq failed");

	if(drmModeAtomicAddProperty(req.get(), primary_plane_id, prop_fb_id, back) < 0)
		throw std::runtime_error("failed to add buffer");
	if(drmModeAtomicCommit(fd, req.get(), DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &flip_done) != 0)
		throw std::runtime_error("atomic flip failed");

	while(!flip_done)
	{
		pollfd pfd{fd, POLLIN, 0};

		if(poll(&pfd, 1, -1) < 0)
		{
			if (errno == EINTR)
				continue;
	
			throw std::system_error(errno, std::generic_category(), "poll failed");
		}
		if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			throw std::runtime_error("DRM poll error");

		if((pfd.revents & POLLIN) && drmHandleEvent(fd, &ev))
			throw std::runtime_error("drmHandleEvent failed");
	}

	std::swap(front, back);
}

void DoubleFramebuffer::showImage(const std::array<uint32_t, FRAMEBUF_SIZE>& image, std::chrono::seconds delay_sec)
{
	bool flip_done = false;

	void* back_ptr = (back == fb1_id) ? buf1.data() : buf2.data();
	uint32_t back_pitch = (back == fb1_id) ? buf1.getBufferPitch() : buf2.getBufferPitch();
	std::byte* dst = static_cast<std::byte*>(back_ptr);
	for (uint32_t y = 0; y < HEIGHT; ++y)
	{
		std::memcpy(dst + y * back_pitch, image.data() + y * WIDTH, WIDTH * sizeof(uint32_t));
	}

	std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> req{drmModeAtomicAlloc(), drmModeAtomicFree};
	if(req == nullptr)
		throw std::runtime_error("drmModeAtomicReq failed");

	if(drmModeAtomicAddProperty(req.get(), primary_plane_id, prop_fb_id, back) < 0)
		throw std::runtime_error("failed to add buffer");
	if(drmModeAtomicCommit(fd, req.get(), DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &flip_done) != 0)
		throw std::runtime_error("atomic flip failed");

	while(!flip_done)
	{
		pollfd pfd{fd, POLLIN, 0};

		if(poll(&pfd, 1, -1) < 0)
		{
			if (errno == EINTR)
				continue;
	
			throw std::system_error(errno, std::generic_category(), "poll failed");
		}
		if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			throw std::runtime_error("DRM poll error");

		if((pfd.revents & POLLIN) && drmHandleEvent(fd, &ev))
			throw std::runtime_error("drmHandleEvent failed");
	}
	std::swap(front, back);

	std::this_thread::sleep_for(std::chrono::duration{delay_sec});
}

void DoubleFramebuffer::resetCounter()
{
	counter = 0;
}

TTYguard::TTYguard()
{
	tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	
	if(tty_fd < 0)
		throw std::system_error(-errno, std::generic_category(), "open /dev/tty failed");

	if(ioctl(tty_fd, KDGETMODE, &old_state) < 0)
	{
		close(tty_fd);
		throw std::system_error(-errno, std::generic_category(), "KDGETMODE failed");
	}
		
	if(ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0)
	{
		close(tty_fd);
		throw std::system_error(-errno, std::generic_category(), "KDSETMODE failed");
	}
}

TTYguard::~TTYguard()
{
	if(ioctl(tty_fd, KDSETMODE, old_state) < 0)
		std::cerr << "ioctl failed in ~TTYguard: " << strerror(errno) << std::endl;
	
	if(close(tty_fd) < 0)
		std::cerr << "close failed in ~TTYguard: " << strerror(errno) << std::endl;
}