//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#ifdef Rtt_SIMULATOR

#include "Rtt_LinuxInputTap.h"

#include "Core/Rtt_Config.h"
#include "Display/Rtt_Display.h"

#include <SDL.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace Rtt
{

	static const size_t kMaxQueuedCommands = 256;

	static int64_t MonotonicNs()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
	}

	LinuxInputTap* LinuxInputTap::Create()
	{
		const char* path = getenv("SOLAR2D_INPUT_PIPE");
		if (!path || !*path)
		{
			return NULL;
		}
		if (path[0] != '/')
		{
			fprintf(stdout, "[INPUT] SOLAR2D_INPUT_PIPE must be an absolute path; input tap disabled\n");
			return NULL;
		}

		// Consume the variable: children started through system() must not
		// inherit an input channel.
		unsetenv("SOLAR2D_INPUT_PIPE");

		LinuxInputTap* tap = new LinuxInputTap(path);
		if (!tap->Start())
		{
			delete tap;
			return NULL;
		}
		return tap;
	}

	LinuxInputTap::LinuxInputTap(const std::string& fifoPath)
		: fFifoPath(fifoPath), fLockFd(-1), fRunning(false)
	{
		memset(&fDrag, 0, sizeof(fDrag));
	}

	LinuxInputTap::~LinuxInputTap()
	{
		fRunning.store(false, std::memory_order_release);
		if (fReader.joinable())
		{
			fReader.join();
		}
		if (fLockFd >= 0)
		{
			flock(fLockFd, LOCK_UN);
			close(fLockFd);
		}
		std::string ready = fFifoPath + ".ready";
		unlink(ready.c_str());
		std::string lock = fFifoPath + ".lock";
		unlink(lock.c_str());
	}

	bool LinuxInputTap::Start()
	{
		struct stat st;
		if (lstat(fFifoPath.c_str(), &st) == 0)
		{
			if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid())
			{
				fprintf(stdout, "[INPUT] '%s' exists but is not a FIFO owned by this uid; input tap disabled\n",
						fFifoPath.c_str());
				return false;
			}
		}
		else if (mkfifo(fFifoPath.c_str(), 0600) != 0 && errno != EEXIST)
		{
			fprintf(stdout, "[INPUT] mkfifo('%s') failed: %s; input tap disabled\n",
					fFifoPath.c_str(), strerror(errno));
			return false;
		}

		std::string lockPath = fFifoPath + ".lock";
		fLockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fLockFd < 0 || flock(fLockFd, LOCK_EX | LOCK_NB) != 0)
		{
			fprintf(stdout, "[INPUT] another tap holds '%s'; input tap disabled\n", lockPath.c_str());
			if (fLockFd >= 0)
			{
				close(fLockFd);
				fLockFd = -1;
			}
			return false;
		}

		std::string readyPath = fFifoPath + ".ready";
		std::string tmpPath = readyPath + ".tmp";
		FILE* f = fopen(tmpPath.c_str(), "w");
		if (!f)
		{
			fprintf(stdout, "[INPUT] cannot write '%s': %s; input tap disabled\n",
					tmpPath.c_str(), strerror(errno));
			return false;
		}
		fprintf(f, "pid %d version 1\n", (int)getpid());
		fclose(f);
		rename(tmpPath.c_str(), readyPath.c_str());

		fRunning.store(true, std::memory_order_release);
		fReader = std::thread(&LinuxInputTap::ReaderLoop, this);
		return true;
	}

	void LinuxInputTap::ReaderLoop()
	{
		std::string pending;
		char buf[512];

		while (fRunning.load(std::memory_order_acquire))
		{
			int fd = open(fFifoPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0)
			{
				if (errno != ENOENT && errno != EINTR)
				{
					fprintf(stdout, "[INPUT] open('%s') failed: %s; input tap stopping\n",
							fFifoPath.c_str(), strerror(errno));
					return;
				}
				struct timespec ts = { 0, 200 * 1000 * 1000 };
				nanosleep(&ts, NULL);
				continue;
			}

			// Park until a writer appears, then drain lines until it leaves
			// (read == 0). EOF is normal here — every `echo ... > fifo` opens
			// and closes the pipe — so reopen and keep serving.
			while (fRunning.load(std::memory_order_acquire))
			{
				struct pollfd pfd;
				pfd.fd = fd;
				pfd.events = POLLIN;
				int rc = poll(&pfd, 1, 200);
				if (rc < 0 && errno != EINTR)
				{
					break;
				}
				if (rc == 0 || !(pfd.revents & (POLLIN | POLLHUP)))
				{
					continue;
				}
				ssize_t n = read(fd, buf, sizeof(buf));
				if (n < 0)
				{
					if (errno == EAGAIN || errno == EINTR)
					{
						continue;
					}
					break;
				}
				if (n == 0)
				{
					break;  // writer left
				}
				pending.append(buf, (size_t)n);

				size_t start = 0;
				while (true)
				{
					size_t end = pending.find('\n', start);
					if (end == std::string::npos)
					{
						break;
					}
					std::string line = pending.substr(start, end - start);
					start = end + 1;

					// Parse on the reader thread (string work only — no
					// engine state); dispatch happens on the main thread.
					Command cmd;
					memset(&cmd, 0, sizeof(cmd));
					cmd.ms = 300;

					int consumed = 0;
					float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
					int ms = 300;
					char key[64] = { 0 };
					char text[257] = { 0 };
					if (sscanf(line.c_str(), " tap %f %f%n", &x1, &y1, &consumed) == 2 && consumed == (int)line.size())
					{
						cmd.type = Command::kTap;
						cmd.x1 = x1; cmd.y1 = y1;
					}
					else if (sscanf(line.c_str(), " wtap %f %f%n", &x1, &y1, &consumed) == 2 && consumed == (int)line.size())
					{
						cmd.type = Command::kWindowTap;
						cmd.x1 = x1; cmd.y1 = y1;
					}
					else if (sscanf(line.c_str(), " drag %f %f %f %f %d%n", &x1, &y1, &x2, &y2, &ms, &consumed) == 5 && consumed == (int)line.size())
					{
						cmd.type = Command::kDrag;
						cmd.x1 = x1; cmd.y1 = y1; cmd.x2 = x2; cmd.y2 = y2;
						cmd.ms = ms < 0 ? 0 : ms;
					}
					else if (sscanf(line.c_str(), " drag %f %f %f %f%n", &x1, &y1, &x2, &y2, &consumed) == 4 && consumed == (int)line.size())
					{
						cmd.type = Command::kDrag;
						cmd.x1 = x1; cmd.y1 = y1; cmd.x2 = x2; cmd.y2 = y2;
					}
					else if (sscanf(line.c_str(), " key %63s%n", key, &consumed) == 1 && consumed == (int)line.size())
					{
						cmd.type = Command::kKey;
						cmd.arg = key;
					}
					else if (line.rfind("text ", 0) == 0 && line.size() > 5 && line.size() < 5 + sizeof(text))
					{
						cmd.type = Command::kText;
						cmd.arg = line.substr(5);
					}
					else
					{
						fprintf(stdout, "[INPUT] ignored: %s\n", line.c_str());
						continue;
					}

					std::lock_guard<std::mutex> guard(fMutex);
					if (fQueue.size() >= kMaxQueuedCommands)
					{
						fprintf(stdout, "[INPUT] command queue full; dropped: %s\n", line.c_str());
					}
					else
					{
						fQueue.push_back(cmd);
					}
				}
				pending.erase(0, start);
				if (pending.size() > 512)
				{
					pending.clear();  // a line that cannot end; drop it
				}
			}
			close(fd);
		}
	}

	void LinuxInputTap::Ack(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		fputs("[INPUT] ", stdout);
		vfprintf(stdout, fmt, args);
		fputc('\n', stdout);
		fflush(stdout);
		va_end(args);
	}

	void LinuxInputTap::PushMoveTo(int windowX, int windowY, bool isDown, bool isUp, unsigned long windowID)
	{
		if (isDown)
		{
			SDL_Event e;
			memset(&e, 0, sizeof(e));
			e.type = SDL_MOUSEBUTTONDOWN;
			e.button.windowID = (Uint32)windowID;
			e.button.button = SDL_BUTTON_LEFT;
			e.button.state = SDL_PRESSED;
			e.button.clicks = 1;
			e.button.x = windowX;
			e.button.y = windowY;
			SDL_PushEvent(&e);
		}

		SDL_Event m;
		memset(&m, 0, sizeof(m));
		m.type = SDL_MOUSEMOTION;
		m.motion.windowID = (Uint32)windowID;
		m.motion.x = windowX;
		m.motion.y = windowY;
		SDL_PushEvent(&m);

		if (isUp)
		{
			SDL_Event u;
			memset(&u, 0, sizeof(u));
			u.type = SDL_MOUSEBUTTONUP;
			u.button.windowID = (Uint32)windowID;
			u.button.button = SDL_BUTTON_LEFT;
			u.button.state = SDL_RELEASED;
			u.button.clicks = 1;
			u.button.x = windowX;
			u.button.y = windowY;
			SDL_PushEvent(&u);
		}
	}

	void LinuxInputTap::PushTap(int windowX, int windowY, unsigned long windowID)
	{
		PushMoveTo(windowX, windowY, true, true, windowID);
	}

	void LinuxInputTap::PushKey(const std::string& name, unsigned long windowID)
	{
		SDL_Keycode keycode = SDL_GetKeyFromName(name.c_str());
		if (keycode == SDLK_UNKNOWN)
		{
			Ack("ignored: unknown key '%s'", name.c_str());
			return;
		}
		SDL_Event down;
		memset(&down, 0, sizeof(down));
		down.type = SDL_KEYDOWN;
		down.key.windowID = (Uint32)windowID;
		down.key.keysym.sym = keycode;
		SDL_PushEvent(&down);

		SDL_Event up;
		memset(&up, 0, sizeof(up));
		up.type = SDL_KEYUP;
		up.key.windowID = (Uint32)windowID;
		up.key.keysym.sym = keycode;
		SDL_PushEvent(&up);
	}

	void LinuxInputTap::PushText(const std::string& text, unsigned long windowID)
	{
		SDL_Event e;
		memset(&e, 0, sizeof(e));
		e.type = SDL_TEXTINPUT;
		e.text.windowID = (Uint32)windowID;
		snprintf(e.text.text, sizeof(e.text.text), "%s", text.c_str());
		SDL_PushEvent(&e);
	}

	void LinuxInputTap::DispatchPending(Display* display, int menuHeight, unsigned long windowID)
	{
		// Content -> window: the display's own transform gives content-area
		// ("screen") pixels; the mouse listener subtracts the menu height on
		// the way in, so adding it back yields the SDL window coordinates a
		// real mouse event would carry.
		std::deque<Command> commands;
		{
			std::lock_guard<std::mutex> guard(fMutex);
			commands.swap(fQueue);
		}

		for (const Command& cmd : commands)
		{
			switch (cmd.type)
			{
				case Command::kTap:
				{
					if (display)
					{
						S32 sx = (S32)cmd.x1, sy = (S32)cmd.y1;
						display->ContentToScreen(sx, sy);
						PushTap(sx, sy + menuHeight, windowID);
						Ack("dispatched tap (%g,%g) -> window (%d,%d)", cmd.x1, cmd.y1, sx, sy + menuHeight);
					}
					break;
				}
				case Command::kWindowTap:
				{
					PushTap((int)cmd.x1, (int)cmd.y1, windowID);
					Ack("dispatched wtap at window (%g,%g)", cmd.x1, cmd.y1);
					break;
				}
				case Command::kDrag:
				{
					if (display && !fDrag.active)
					{
						S32 sx = (S32)cmd.x1, sy = (S32)cmd.y1;
						display->ContentToScreen(sx, sy);
						S32 ex = (S32)cmd.x2, ey = (S32)cmd.y2;
						display->ContentToScreen(ex, ey);
						fDrag.active = true;
						fDrag.downSent = false;
						fDrag.fromX = (float)sx;
						fDrag.fromY = (float)sy + menuHeight;
						fDrag.toX = (float)ex;
						fDrag.toY = (float)ey + menuHeight;
						fDrag.startNs = MonotonicNs();
						fDrag.endNs = fDrag.startNs + (int64_t)cmd.ms * 1000000ll;
						fDrag.lastX = fDrag.fromX;
						fDrag.lastY = fDrag.fromY;
						Ack("dispatched drag (%g,%g)->(%g,%g) over %dms", cmd.x1, cmd.y1, cmd.x2, cmd.y2, cmd.ms);
					}
					else if (fDrag.active)
					{
						Ack("ignored: drag already in progress");
					}
					break;
				}
				case Command::kKey:
				{
					PushKey(cmd.arg, windowID);
					Ack("dispatched key '%s'", cmd.arg.c_str());
					break;
				}
				case Command::kText:
				{
					PushText(cmd.arg, windowID);
					Ack("dispatched text (%d chars)", (int)cmd.arg.size());
					break;
				}
			}
		}

		// Frame-paced drag: emit the interpolated move for "now", so the app
		// sees began/moved/.../ended across frames like a real finger.
		if (fDrag.active)
		{
			const int64_t now = MonotonicNs();
			float t;
			if (fDrag.endNs <= fDrag.startNs || now >= fDrag.endNs)
			{
				t = 1.0f;
			}
			else
			{
				t = (float)(double)(now - fDrag.startNs) / (float)(double)(fDrag.endNs - fDrag.startNs);
			}
			float x = fDrag.fromX + (fDrag.toX - fDrag.fromX) * t;
			float y = fDrag.fromY + (fDrag.toY - fDrag.fromY) * t;
			PushMoveTo((int)x, (int)y, !fDrag.downSent, t >= 1.0f, windowID);
			fDrag.downSent = true;
			fDrag.lastX = x;
			fDrag.lastY = y;
			if (t >= 1.0f)
			{
				fDrag.active = false;
			}
		}
	}

} // namespace Rtt

#endif // Rtt_SIMULATOR
