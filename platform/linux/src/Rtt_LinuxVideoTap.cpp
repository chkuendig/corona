//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#ifdef Rtt_SIMULATOR

#include "Rtt_LinuxVideoTap.h"

#include "Core/Rtt_Config.h"  // defines GL_GLEXT_PROTOTYPES; must precede the GL header

#include <SDL_opengl.h>

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace Rtt
{

	static const uint32_t kHeaderSize = 64;
	static const char kMagic[4] = { 'S', '2', 'V', 'T' };
	static const size_t kMaxQueuedFrames = 2;

	static void TapLog(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		fprintf(stderr, "[VIDEOTAP] ");
		vfprintf(stderr, fmt, args);
		fprintf(stderr, "\n");
		va_end(args);
	}

	static int64_t MonotonicNs()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
	}

	// The EGL surface is the authority on readable bounds — the SDL window
	// size can disagree under drivers whose surface is fixed at creation
	// (offscreen). Resolved lazily through dlopen so nothing extra is linked.
	// Constants per EGL/egl.h: EGL_DRAW 0x3059, EGL_WIDTH 0x3057,
	// EGL_HEIGHT 0x3056.
	static bool QuerySurfaceSize(int* width, int* height)
	{
		static void* egl = nullptr;
		static void (*query)(void*, void*, unsigned int, int*) = nullptr;
		static void* (*current_display)() = nullptr;
		static void* (*current_surface)(unsigned int) = nullptr;
		static bool resolved = false;

		if (!resolved)
		{
			egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
			if (egl)
			{
				// dlsym returns void*, which C++ refuses to assign to a
				// function pointer directly.
				*reinterpret_cast<void**>(&current_display) = dlsym(egl, "eglGetCurrentDisplay");
				*reinterpret_cast<void**>(&current_surface) = dlsym(egl, "eglGetCurrentSurface");
				*reinterpret_cast<void**>(&query) = dlsym(egl, "eglQuerySurface");
			}
			resolved = true;
		}

		if (egl && query && current_display && current_surface)
		{
			void* display = current_display();
			void* surface = current_surface(0x3059 /* EGL_DRAW */);
			if (display && surface)
			{
				int w = 0, h = 0;
				query(display, surface, 0x3057 /* EGL_WIDTH */, &w);
				query(display, surface, 0x3056 /* EGL_HEIGHT */, &h);
				if (w > 0 && h > 0)
				{
					*width = w;
					*height = h;
					return true;
				}
			}
		}
		return false;
	}

	LinuxVideoTap* LinuxVideoTap::Create()
	{
		const char* path = getenv("SOLAR2D_VIDEO_PIPE");
		if (!path || !*path)
		{
			return NULL;
		}
		// Consume both variables on every path: children started through
		// system() must not inherit a frame stream they never asked for.
		const char* fpsEnv = getenv("SOLAR2D_VIDEO_FPS");
		double fps = 30.0;
		if (fpsEnv && *fpsEnv)
		{
			char* end = NULL;
			double parsed = strtod(fpsEnv, &end);
			if (end == fpsEnv || parsed <= 0.0 || parsed > 240.0)
			{
				TapLog("SOLAR2D_VIDEO_FPS '%s' is not a usable rate; tap disabled", fpsEnv);
				unsetenv("SOLAR2D_VIDEO_PIPE");
				unsetenv("SOLAR2D_VIDEO_FPS");
				return NULL;
			}
			fps = parsed;
		}
		if (path[0] != '/')
		{
			TapLog("SOLAR2D_VIDEO_PIPE must be an absolute path; tap disabled");
			unsetenv("SOLAR2D_VIDEO_PIPE");
			unsetenv("SOLAR2D_VIDEO_FPS");
			return NULL;
		}
		unsetenv("SOLAR2D_VIDEO_FPS");

		LinuxVideoTap* tap = new LinuxVideoTap(path, fps);
		if (!tap->Start())
		{
			delete tap;
			return NULL;
		}
		TapLog("streaming %s (fps cap %.1f)", path, fps);
		return tap;
	}

	LinuxVideoTap::LinuxVideoTap(const std::string& fifoPath, double fpsCap)
		: fFifoPath(fifoPath), fFpsCap(fpsCap), fEventFd(-1), fLockFd(-1),
		  fStagedSeq(0), fNextEmitNs(0), fRunning(false), fSequence(0),
		  fDropped(0), fEmitted(0), fLoggedWidthClamp(false)
	{
	}

	LinuxVideoTap::~LinuxVideoTap()
	{
		fRunning.store(false, std::memory_order_release);
		WakeWriter();
		{
			// Wake a writer parked in fCond.wait_for as well as the eventfd
			// poll, so the join is prompt rather than up to 200ms away.
			std::lock_guard<std::mutex> guard(fMutex);
			fCond.notify_all();
		}
		if (fWriter.joinable())
		{
			fWriter.join();
		}
		if (fEventFd >= 0)
		{
			close(fEventFd);
		}
		// The marker and lock belong to whoever holds the flock — a tap whose
		// Start() failed (another instance won the lock) must not delete the
		// live tap's files.
		if (fLockFd >= 0)
		{
			flock(fLockFd, LOCK_UN);
			close(fLockFd);
			std::string ready = fFifoPath + ".ready";
			unlink(ready.c_str());
			std::string lock = fFifoPath + ".lock";
			unlink(lock.c_str());
		}
	}

	bool LinuxVideoTap::Start()
	{
		struct stat st;
		if (lstat(fFifoPath.c_str(), &st) == 0)
		{
			// Whatever is there must be a FIFO we own. A regular file would
			// be written at full frame rate until the disk fills; a symlink
			// would clobber its target.
			if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid())
			{
				TapLog("'%s' exists but is not a FIFO owned by uid %d; tap disabled",
					   fFifoPath.c_str(), geteuid());
				return false;
			}
		}
		else if (mkfifo(fFifoPath.c_str(), 0600) != 0 && errno != EEXIST)
		{
			TapLog("mkfifo('%s') failed: %s; tap disabled", fFifoPath.c_str(), strerror(errno));
			return false;
		}

		// One writer per path. Interleaved writes of frames far larger than
		// PIPE_BUF would corrupt every consumer.
		std::string lockPath = fFifoPath + ".lock";
		fLockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fLockFd < 0 || flock(fLockFd, LOCK_EX | LOCK_NB) != 0)
		{
			TapLog("another tap holds '%s'; tap disabled", lockPath.c_str());
			if (fLockFd >= 0)
			{
				close(fLockFd);
				fLockFd = -1;
			}
			return false;
		}

		// Capability + liveness marker. Consumers must probe this file (never
		// the FIFO: opening the FIFO connects a reader and starts the stream).
		// Written atomically so a consumer never reads a partial pid.
		std::string readyPath = fFifoPath + ".ready";
		std::string tmpPath = readyPath + ".tmp";
		FILE* f = fopen(tmpPath.c_str(), "w");
		if (!f)
		{
			TapLog("cannot write '%s': %s; tap disabled", tmpPath.c_str(), strerror(errno));
			return false;
		}
		fprintf(f, "pid %d version 1\n", (int)getpid());
		fclose(f);
		rename(tmpPath.c_str(), readyPath.c_str());

		fEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (fEventFd < 0)
		{
			TapLog("eventfd failed: %s; tap disabled", strerror(errno));
			return false;
		}

		fRunning.store(true, std::memory_order_release);
		fWriter = std::thread(&LinuxVideoTap::WriterLoop, this);
		return true;
	}

	void LinuxVideoTap::WakeWriter()
	{
		if (fEventFd >= 0)
		{
			uint64_t one = 1;
			ssize_t ignored = write(fEventFd, &one, sizeof(one));
			(void)ignored;
		}
	}

	void LinuxVideoTap::WriteHeader(std::vector<uint8_t>& frame, uint32_t w, uint32_t h)
	{
		if (frame.size() < kHeaderSize)
		{
			frame.resize(kHeaderSize);
		}
		uint8_t* p = frame.data();
		memset(p, 0, kHeaderSize);
		memcpy(p + 0, kMagic, 4);
		*(uint16_t*)(p + 4) = 1;                 // version
		*(uint16_t*)(p + 6) = (uint16_t)kHeaderSize;
		*(uint32_t*)(p + 8) = w;
		*(uint32_t*)(p + 12) = h;
		*(uint32_t*)(p + 16) = w * 4;            // stride
		*(uint32_t*)(p + 20) = 0x41524742;       // 'BGRA' little-endian
		p[24] = 1;                               // bottom-up rows
		*(uint64_t*)(p + 32) = fSequence.load(std::memory_order_relaxed);
		*(uint64_t*)(p + 40) = (uint64_t)MonotonicNs();
	}

	void LinuxVideoTap::StageFrame(int drawableWidth, int drawableHeight)
	{
		int w = drawableWidth;
		int h = drawableHeight;
		int surfaceW = 0, surfaceH = 0;
		if (QuerySurfaceSize(&surfaceW, &surfaceH) && (surfaceW < w || surfaceH < h))
		{
			w = surfaceW;
			h = surfaceH;
			if (!fLoggedWidthClamp)
			{
				fLoggedWidthClamp = true;
				TapLog("surface is %dx%d, smaller than the %dx%d window; reading the surface",
					   surfaceW, surfaceH, drawableWidth, drawableHeight);
			}
		}
		if (w <= 0 || h <= 0)
		{
			return;
		}

		// Peek the pacing accumulator: when no frame is due under the fps
		// cap, the readback is skipped entirely — glReadPixels is a sync
		// point for llvmpipe's threaded rasterizer, not just a memcpy, so a
		// 60 fps app recorded at 30 should not pay 60 readbacks a second.
		// (CommitFrame owns the accumulator; this check must not advance it.)
		const int64_t interval = (int64_t)(1000000000.0 / fFpsCap);
		const int64_t now = MonotonicNs();
		if (fNextEmitNs != 0 && now < fNextEmitNs - interval / 2)
		{
			return;
		}

		{
			std::lock_guard<std::mutex> guard(fMutex);
			if (fQueue.size() >= kMaxQueuedFrames)
			{
				// Full queue with a frame due: CommitFrame will count this
				// drop (and consume its sequence number, so consumers see the
				// gap). Staging reports nothing.
				fStagedSeq = 0;
				return;
			}
		}

		const size_t payload = (size_t)w * h * 4;
		fFill.data.resize(kHeaderSize + payload);

		GLint readFbo = 0, packBuffer = 0, readBuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
		glGetIntegerv(GL_READ_BUFFER, &readBuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		// Double-buffered contexts read GL_BACK; the offscreen driver's
		// pbuffer context is single-buffered, where GL_BACK is invalid and
		// the read would come back black. Probe once and remember.
		static int sReadBufferMode = -1;
		if (sReadBufferMode < 0)
		{
			// Draining pending GL errors is unavoidable for a clean probe;
			// this runs once, before the app has usually raised any.
			while (glGetError() != GL_NO_ERROR)
			{
			}
			glReadBuffer(GL_BACK);
			sReadBufferMode = (glGetError() == GL_NO_ERROR) ? GL_BACK : GL_FRONT;
			if (sReadBufferMode == GL_FRONT)
			{
				TapLog("context is single-buffered; reading GL_FRONT");
			}
		}
		glReadBuffer((GLenum)sReadBufferMode);
		glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, fFill.data.data() + kHeaderSize);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
		glReadBuffer((GLenum)readBuffer);

		fStagedSeq = 1;  // marker: a staged frame exists (seq assigned at commit)
		WriteHeader(fFill.data, (uint32_t)w, (uint32_t)h);
	}

	void LinuxVideoTap::CommitFrame()
	{
		if (fStagedSeq == 0)
		{
			return;
		}

		// Accumulator pacing. "Emit if now >= next - half interval" instead
		// of "if now - last >= interval": the main loop sleeps in whole
		// milliseconds, and the difference rule aliases against that jitter
		// to roughly half the requested rate.
		const int64_t interval = (int64_t)(1000000000.0 / fFpsCap);
		const int64_t now = MonotonicNs();
		if (fNextEmitNs == 0)
		{
			fNextEmitNs = now;
		}
		if (now < fNextEmitNs - interval / 2)
		{
			fStagedSeq = 0;
			return;
		}
		fNextEmitNs += interval;
		if (fNextEmitNs < now - 2 * interval)
		{
			fNextEmitNs = now;  // fell far behind; resynchronize
		}

		{
			std::lock_guard<std::mutex> guard(fMutex);
			if (fQueue.size() >= kMaxQueuedFrames)
			{
				// A drop consumes a sequence number so the consumer sees the
				// gap — the ONLY thing that creates one. Staging skips and
				// fps-cap skips never do.
				fSequence.fetch_add(1, std::memory_order_relaxed);
				fDropped.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				*(uint64_t*)(fFill.data.data() + 32) =
					fSequence.fetch_add(1, std::memory_order_relaxed) + 1;
				fQueue.push_back(std::move(fFill));
				fFill = Frame{};
				fEmitted.fetch_add(1, std::memory_order_relaxed);
				fCond.notify_one();
			}
		}
		fStagedSeq = 0;
	}

	int LinuxVideoTap::OpenFifo()
	{
		while (fRunning.load(std::memory_order_acquire))
		{
			int fd = open(fFifoPath.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			if (fd >= 0)
			{
				// Still a FIFO we own? The path may have been removed and
				// recreated as a regular file since startup — writing frames
				// into that would fill the disk at full frame rate.
				struct stat st;
				if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode) || st.st_uid != geteuid())
				{
					TapLog("'%s' is no longer our FIFO; tap stopping", fFifoPath.c_str());
					close(fd);
					return -1;
				}
				// A reader just connected: anything staged while it was gone
				// is stale by definition. Start the segment from now.
				{
					std::lock_guard<std::mutex> guard(fMutex);
					fQueue.clear();
				}
				return fd;
			}
			if (errno != ENXIO && errno != ENOENT)
			{
				TapLog("open('%s') failed: %s; tap stopping", fFifoPath.c_str(), strerror(errno));
				return -1;
			}

			// No reader yet. Park for 200ms or until shutdown; zero cost.
			struct pollfd pfd;
			pfd.fd = fEventFd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			int rc = poll(&pfd, 1, 200);
			if (rc < 0 && errno != EINTR)
			{
				return -1;
			}
		}
		return -1;
	}

	bool LinuxVideoTap::WriteAll(int fd, const uint8_t* buf, size_t len)
	{
		size_t offset = 0;
		while (offset < len)
		{
			struct pollfd pfd[2];
			pfd[0].fd = fd;
			pfd[0].events = POLLOUT;
			pfd[0].revents = 0;
			pfd[1].fd = fEventFd;
			pfd[1].events = POLLIN;
			pfd[1].revents = 0;
			int rc = poll(pfd, 2, -1);
			if (rc < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				return false;
			}
			if (pfd[1].revents & POLLIN)
			{
				return false;  // shutdown
			}
			if (pfd[0].revents & (POLLERR | POLLHUP))
			{
				return false;  // reader went away
			}
			if (pfd[0].revents & POLLOUT)
			{
				ssize_t n = write(fd, buf + offset, len - offset);
				if (n < 0)
				{
					if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
					{
						continue;
					}
					return false;  // EPIPE among others
				}
				offset += (size_t)n;
			}
		}
		return true;
	}

	void LinuxVideoTap::WriterLoop()
	{
		// SIGPIPE must not kill the simulator when a reader disappears.
		// Blocking it in this thread turns the signal into an EPIPE return
		// from write(); the process-wide disposition is never touched.
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGPIPE);
		pthread_sigmask(SIG_BLOCK, &set, NULL);

		uint64_t segmentFrames = 0;
		uint64_t segmentDrops = 0;

		while (fRunning.load(std::memory_order_acquire))
		{
			int fd = OpenFifo();
			if (fd < 0)
			{
				break;
			}

			segmentFrames = 0;
			segmentDrops = fDropped.load(std::memory_order_relaxed);

			bool connected = true;
			while (connected && fRunning.load(std::memory_order_acquire))
			{
				Frame frame;
				bool timedOut = false;
				{
					std::unique_lock<std::mutex> lock(fMutex);
					fCond.wait_for(lock, std::chrono::milliseconds(200),
								   [this] { return !fRunning.load(std::memory_order_acquire) || !fQueue.empty(); });
					if (!fRunning.load(std::memory_order_acquire))
					{
						break;
					}
					if (fQueue.empty())
					{
						timedOut = true;  // idle: check the reader is still there
					}
					else
					{
						frame = std::move(fQueue.front());
						fQueue.pop_front();
					}
				}

				if (timedOut)
				{
					// A reader that left while we were idle: POLLERR now, so
					// the next reader cannot inherit a stale partial frame.
					struct pollfd pfd;
					pfd.fd = fd;
					pfd.events = POLLOUT;
					pfd.revents = 0;
					if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLERR | POLLHUP)))
					{
						connected = false;
					}
					continue;
				}

				if (!WriteAll(fd, frame.data.data(), frame.data.size()))
				{
					connected = false;
					continue;
				}
				++segmentFrames;
			}

			close(fd);
			uint64_t drops = fDropped.load(std::memory_order_relaxed) - segmentDrops;
			TapLog("segment: %llu frames, %llu dropped",
				   (unsigned long long)segmentFrames, (unsigned long long)drops);
		}
	}

} // namespace Rtt

#endif // Rtt_SIMULATOR
