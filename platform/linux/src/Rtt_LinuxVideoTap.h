//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://coronalabs.com
//
// Opt-in frame tap for headless video capture. Compiled into the simulator
// target only. Nothing runs unless SOLAR2D_VIDEO_PIPE names an absolute FIFO
// path at startup; every hook site is a NULL pointer check in that case.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef _Rtt_LinuxVideoTap_H__
#define _Rtt_LinuxVideoTap_H__

#ifdef Rtt_SIMULATOR

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Rtt
{
	class LinuxVideoTap
	{
	public:
		// Returns NULL unless the environment asks for a tap and every path
		// check passes. The env var is consumed (unset) so child processes
		// started via system() cannot become second writers.
		static LinuxVideoTap* Create();

		~LinuxVideoTap();

		// Called from SolarAppContext::Flush() with the GL context current,
		// before SDL_GL_SwapWindow. Reads the back buffer into the fill
		// buffer. Skips the readback entirely when the writer queue is full.
		void StageFrame(int drawableWidth, int drawableHeight);

		// Called once at the end of SolarAppContext::advance(): the last
		// staged frame of the tick is handed to the writer. No GL use.
		void CommitFrame();

	private:
		LinuxVideoTap(const std::string& fifoPath, double fpsCap);

		bool Start();          // mkfifo, lock, ready marker, writer thread
		void WriterLoop();
		int OpenFifo();        // -1 = shutdown, otherwise a connected fd
		bool WriteAll(int fd, const uint8_t* buf, size_t len);  // false = reader gone
		void WakeWriter();
		void WriteHeader(std::vector<uint8_t>& frame, uint32_t w, uint32_t h);

		struct Frame		{
			std::vector<uint8_t> data;  // 64-byte header + payload
		};

		const std::string fFifoPath;
		const double fFpsCap;

		int fEventFd;
		int fLockFd;

		// Render thread only
		Frame fFill;
		uint64_t fStagedSeq;
		int64_t fNextEmitNs;

		// Shared with the writer thread
		std::mutex fMutex;
		std::condition_variable fCond;
		std::deque<Frame> fQueue;  // bounded to 2

		std::thread fWriter;
		std::atomic<bool> fRunning;
		std::atomic<uint64_t> fSequence;
		std::atomic<uint64_t> fDropped;
		std::atomic<uint64_t> fEmitted;
		bool fLoggedWidthClamp;
	};

	// Wire format (little-endian), version 1. One 64-byte header per frame,
	// then w*h*4 bytes of BGRA (GL bottom-up row order; alpha is undefined —
	// consumers may read the stream as bgr0).
	//
	//   offset size  field
	//        0    4  magic "S2VT"
	//        4    2  version (1)
	//        6    2  header length (64)
	//        8    4  width
	//       12    4  height
	//       16    4  stride (width * 4)
	//       20    4  fourcc "BGRA"
	//       24    1  bottom_up (1)
	//       25    7  reserved (zero)
	//       32    8  sequence number (per process, monotonic)
	//       40    8  capture time (CLOCK_MONOTONIC ns)
	//       48   16  reserved (zero)
}

#endif // Rtt_SIMULATOR

#endif // _Rtt_LinuxVideoTap_H__
