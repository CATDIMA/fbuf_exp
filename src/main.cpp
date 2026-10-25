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
#include "drm_output.hpp"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kd.h>

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

	//сохранение текущего состояния и переклчение режима
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
	uint32_t monitor_fps = getMonitorFrequency(connected_plane->getFD(), connected_plane->getCRTCID());

	//видео
	std::ifstream video("../video/yamero.raw", std::ios::binary);

	bool success = false;
	try
	{
		success = framebuf->playVideo(video, video_fps, monitor_fps);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
	}
	
	success ? std::cout << "Video ended\n" : std::cerr << "Video play failed\n";
	
	framebuf.release();
	return 0;
}
