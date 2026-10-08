//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://coronalabs.com
//
// Opt-in input injection for headless runs. Compiled into the simulator
// target only. Nothing runs unless SOLAR2D_INPUT_PIPE names an absolute FIFO
// path at startup.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef _Rtt_LinuxInputTap_H__
#define _Rtt_LinuxInputTap_H__

#ifdef Rtt_SIMULATOR

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Rtt
{
	class Display;

	class LinuxInputTap
	{
	public:
		// NULL unless the environment asks for the tap and every path check
		// passes. The env var is consumed (unset) so child processes cannot
		// inherit an input channel.
		static LinuxInputTap* Create();

		~LinuxInputTap();

		// Command grammar, one command per line written to the FIFO:
		//
		//   tap <x> <y>                 tap at content coordinates
		//   wtap <x> <y>                tap at raw window pixels (y includes
		//                               the menu bar)
		//   drag <x1> <y1> <x2> <y2> [ms]   press, move to (x2,y2) over ms
		//                               (default 300, frame-paced), release
		//   key <name>                  SDL key name: return, escape, a, ...
		//   text <string>               SDL text input (needs a focused field)
		//
		// text takes up to 256 bytes of valid UTF-8 and delivers it as one
		// insertion: SDL carries at most 31 bytes per SDL_TEXTINPUT, so the
		// tap splits at character boundaries and pushes every piece in the
		// same tick. Invalid UTF-8 is rejected, not repaired. Longer text is
		// several text commands. The field's own capacity and input filters
		// are outside what the ack can know.
		//
		// Coordinates in tap/drag are content units — the space the project
		// itself thinks in. Unknown or malformed lines are logged to stdout
		// and skipped, never fatal.
		//
		// Ack semantics: commands are fire-and-forget from the writer's
		// perspective (writing only queues them). Each dispatched command
		// prints one "[INPUT] ..." line to stdout, at most one frame later;
		// a tap or drag moves the pointer on that frame and presses on the
		// next, so the press reaches the app at most two frames after;
		// application-level waits stay with the project's own markers.
		//
		// Faithfulness: injected touch events are indistinguishable from real
		// ones. SDL_PushEvent does not update SDL's global button state, so
		// injected mouse events carry kInjectedMouseId and the mouse listener
		// keeps their button state itself; a Corona "mouse" listener sees
		// isPrimaryButtonDown true on the press and through a drag, as with
		// a real mouse. Only the left button is ever injected.
		//
		// Called once per tick from the main loop. Dispatches queued commands
		// as real SDL events through the genuine input pipeline (listener,
		// hit-testing, native focus), which is what makes this the headless
		// equivalent of an OS-level tap.
		void DispatchPending(Display* display, int menuHeight, unsigned long windowID);

	private:
		LinuxInputTap(const std::string& fifoPath);

		bool Start();
		void ReaderLoop();
		void PushHover(int windowX, int windowY, unsigned long windowID);
		void PushTap(int windowX, int windowY, unsigned long windowID);
		void PushMoveTo(int windowX, int windowY, bool isDown, bool isUp, unsigned long windowID);
		void PushKey(const std::string& name, unsigned long windowID);
		// Returns how many SDL_TEXTINPUT events went out; *total is how many
		// the text needed.
		int PushText(const std::string& text, unsigned long windowID, int* total);
		void Ack(const char* fmt, ...);

		struct Command
		{
			enum Type
			{
				kTap,        // content coords
				kWindowTap,  // window coords
				kDrag,       // content coords
				kKey,
				kText
			};
			Type type;
			float x1, y1, x2, y2;
			int ms;
			std::string arg;
			size_t count;  // kText: UTF-8 codepoints in arg
		};

		struct DragState
		{
			bool active;
			float fromX, fromY, toX, toY;
			int64_t startNs, endNs;
			float lastX, lastY;
			bool downSent;
		};

		void Requeue(std::deque<Command>& commands, size_t from);

		const std::string fFifoPath;
		int fLockFd;

		std::thread fReader;
		std::atomic<bool> fRunning;

		std::mutex fMutex;
		std::deque<Command> fQueue;  // reader -> main thread, bounded

		// A tap whose pointer has moved and whose press goes out next tick.
		struct PressState
		{
			bool active;
			int x, y;
		};

		DragState fDrag;    // main thread only
		PressState fPress;  // main thread only
		bool fLeftHeld;     // main thread only: an injected press is down
	};
}

#endif // Rtt_SIMULATOR

#endif // _Rtt_LinuxInputTap_H__
