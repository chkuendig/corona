#ifndef Rtt_Linux_Mouse_Listener
#define Rtt_Linux_Mouse_Listener

#include "Rtt_Event.h"
#include "Core/Rtt_Types.h"
#include "Rtt_Runtime.h"
#include "Rtt_LinuxContainer.h"
#include <SDL.h>

namespace Rtt
{
	// Mouse id carried by events that LinuxInputTap pushes with
	// SDL_PushEvent. SDL's global button state (SDL_GetMouseState) only
	// follows events SDL generated itself, so for these the listener keeps
	// the button state from the events instead.
	static const Uint32 kInjectedMouseId = 0x54415031; // "TAP1"

	struct LinuxMouseListener : public ref_counted
	{
		float fScaleX, fScaleY;

		LinuxMouseListener();

		void TouchDown(int x, int y, int id);
		void TouchMoved(int x, int y, int id);
		void TouchUp(int x, int y, int id);
		void DispatchEvent(const MEvent& e) const;
		void OnEvent(const SDL_Event& evt, SDL_Window* window);

	private:
		struct pt
		{
			pt() : x(0), y(0) {}
			pt(int xx, int yy) : x(xx), y(yy) {}
			int x;
			int y;
		};

		// SDL_BUTTON() mask of the buttons the current event leaves pressed.
		Uint32 ButtonState(const SDL_Event& evt);

		std::map<int, pt> fStartPoint; // finger id ==> point
		Uint32 fInjectedButtons; // button mask built from injected events
	};
};

#endif // Rtt_Linux_Mouse_Listener
