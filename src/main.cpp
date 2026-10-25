#include <thread>
#include <chrono>
#include <fstream>
#include <csignal>
#include "drm_output.hpp"
#include "images.hpp"
#include "libbmp.h"

volatile std::sig_atomic_t isRunning = 1;

extern "C" void signal_handler(int)
{
	isRunning = 0;
}

int main(int argc, char** argv)
{
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

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

	//сохранение текущего состояния и переключение режима
	TTYguard guard;

	//настройка drm
	std::unique_ptr<DoubleFramebuffer> framebuf;
	try
	{
		framebuf = std::make_unique<DoubleFramebuffer>(*(connected_plane.get()));
	}
	catch(const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}

	//фпс
	constexpr unsigned int video_fps = 30;

	//видео
	std::ifstream video("../video/yamero.raw", std::ios::binary);
	std::array<uint8_t, FRAMEBUF_SIZE> raw_frame;
	std::array<uint32_t, FRAMEBUF_SIZE> frame;

	//картинка
	BmpImg img;
	img.read("../image/clear.bmp");
	std::array<uint32_t, FRAMEBUF_SIZE> image;

	for(int i = 0; i < FRAMEBUF_SIZE; ++i)
	{
		image[i] = static_cast<uint32_t>(img.green_at(i % WIDTH, i / WIDTH) * 0x00010101u);
	}

	bool showVideo = true;
	while(isRunning)
	{
		if(showVideo)
		{
			if(!readFrame(video, raw_frame))
			{
				showVideo = false;
				continue;
			}
			processRawFrame(raw_frame, frame);

			try
			{
				framebuf->playVideo(frame, video_fps);
			}
			catch(const std::exception& e)
			{
				std::cerr << e.what() << '\n';
				isRunning = false;
			}
		}
		else
		{
			try
			{
				framebuf->showImage(image, std::chrono::seconds{3});
			}
			catch(const std::exception& e)
			{
				std::cerr << e.what() << '\n';
				isRunning = false;
			}

			showVideo = true;
			video.clear();
			video.seekg(0, std::ios::beg);
			framebuf->resetCounter();

			if (!video)
			{
				std::cerr << "rewind failed\n";
				isRunning = false;
			}
		}
	}
	
	video.close();
	framebuf.release();
	return 0;
}
