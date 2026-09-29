//////////////////////////////////////////////////////////////////////////////
//
// This file is part of the Corona game engine.
// For overview and more information on licensing please refer to README.md
// Home page: https://github.com/coronalabs/corona
// Contact: support@coronalabs.com
//
//////////////////////////////////////////////////////////////////////////////

#define _chdir chdir
#include <string.h>
#include <fstream>
#include "Core/Rtt_Build.h"
#include "Core/Rtt_Time.h"
#include "Rtt_Runtime.h"
#include "Rtt_LuaContext.h"
#include "Core/Rtt_Types.h"
#include "Rtt_LinuxContext.h"
#include "Rtt_LinuxPlatform.h"
#include "Rtt_LinuxRuntimeDelegate.h"
#include "Rtt_LuaFile.h"
#include "Core/Rtt_FileSystem.h"
#include "Rtt_Archive.h"
#include "Display/Rtt_Display.h"
#include "Display/Rtt_DisplayDefaults.h"
#include "Rtt_Freetype.h"
#include "Rtt_LuaLibSimulator.h"
#include "Rtt_LinuxSimulatorView.h"
#include "Rtt_LinuxUtils.h"
#include "Rtt_LinuxUtils.h"
#include "Rtt_MPlatformServices.h"
#include "Rtt_LinuxApp.h"

#ifdef Rtt_SIMULATOR
#include "Rtt_LinuxVideoTap.h"
#endif
#include "Rtt_HTTPClient.h"
#include "Rtt_LinuxCEF.h"
#include <curl/curl.h>
#include <utility>		// for pairs
#include "lua.h"
#include "lauxlib.h"

using namespace Rtt;
using namespace std;

static bool IsHomeScreen(string appName)
{
	return appName.compare(HOMESCREEN_ID) == 0;
}

// for redirecting output to Solar2DConsole
extern "C"
{
	static int print2console(lua_State* L)
	{
		return SolarAppContext::Print(L);
	}
}

namespace Rtt
{

	SolarAppContext::SolarAppContext(SDL_Window* window)
		: fRuntime(NULL)
		, fRuntimeDelegate(new LinuxRuntimeDelegate())
		, fPlatform(NULL)
		, fTouchDeviceExist(false)
		, fMode("normal")
		, fIsDebApp(false)
		, fLinuxSimulatorServices(NULL)
		, fProjectSettings(new ProjectSettings())
		, fWindow(window)
		, fBeginRunLoop(true)
	{
	}

	SolarAppContext::~SolarAppContext()
	{
		int w, h, x, y;
		SDL_GetWindowPosition(fWindow, &x, &y);
		SDL_GetWindowSize(fWindow, &w, &h);
		h -= app->GetMenuHeight();
		fConfig["x"] = x;
		fConfig["y"] = y;
		fConfig["w"] = w;
		fConfig["h"] = h;

		delete fRuntime;
		delete fRuntimeDelegate;
		delete fPlatform;
		delete fLinuxSimulatorServices;
		delete fProjectSettings;

		setGlyphProvider(NULL);
	}

	void SolarAppContext::Init()
	{
		string appDir = GetSandboxPath(GetAppName());
		if (!Rtt_IsDirectory(appDir.c_str()))
		{
			Rtt_MakeDirectory(appDir.c_str());
		}

		string documentsDir(appDir);
		string temporaryDir(appDir);
		string cachesDir(appDir);
		string systemCachesDir(appDir);
		string skinDir(GetStartupPath(NULL));

		// setup directory paths
		documentsDir.append("/Documents");
		temporaryDir.append("/TemporaryFiles");
		cachesDir.append("/CachedFiles");
		systemCachesDir.append("/.system");
		skinDir.append("/Resources/Skins");

		if (!Rtt_IsDirectory(documentsDir.c_str()))
		{
			Rtt_MakeDirectory(documentsDir.c_str());
		}

		if (!Rtt_IsDirectory(systemCachesDir.c_str()))
		{
			Rtt_MakeDirectory(systemCachesDir.c_str());
		}

		if (!Rtt_IsDirectory(temporaryDir.c_str()))
		{
			Rtt_MakeDirectory(temporaryDir.c_str());
		}

		if (!Rtt_IsDirectory(cachesDir.c_str()))
		{
			Rtt_MakeDirectory(cachesDir.c_str());
		}

		setGlyphProvider(new glyph_freetype_provider(fPathToApp.c_str()));
		fPlatform = new LinuxPlatform(fPathToApp.c_str(), documentsDir.c_str(), temporaryDir.c_str(), cachesDir.c_str(), systemCachesDir.c_str(), skinDir.c_str(), GetStartupPath(NULL));
		fRuntime = new LinuxRuntime(*fPlatform, NULL);
		fRuntime->SetDelegate(fRuntimeDelegate);

		if (app->IsRunningOnSimulator())
		{
			// it can launch main.lua
			fRuntime->SetProperty(Runtime::kLinuxMaskSet | Runtime::kIsApplicationNotArchived, true);
		}
		else
		{
			fRuntime->SetProperty(Runtime::kLinuxMaskSet, true);
		}

		bool fullScreen = false;
		int width = 320;
		int height = 480;

		fProjectSettings->ResetBuildSettings();
		fProjectSettings->ResetConfigLuaSettings();
		fProjectSettings->LoadFromDirectory(fPathToApp.c_str());

		// read config.lua
		if (fProjectSettings->HasConfigLua())
		{
			if (width <= 0 || height <= 0)
			{
				width = fProjectSettings->GetContentWidth();
				height = fProjectSettings->GetContentHeight();
			}
		}
		else
		{
			// no config.lua, set width & height to default values
			fRuntimeDelegate->SetWidth(width);
			fRuntimeDelegate->SetHeight(height);
		}

		// read build.settings
		if (fProjectSettings->HasBuildSettings())
		{
			int minWidth = fProjectSettings->GetMinWindowViewWidth();
			int minHeight = fProjectSettings->GetMinWindowViewHeight();
			const Rtt::NativeWindowMode* nativeWindowMode = fProjectSettings->GetDefaultWindowMode();
			DeviceOrientation::Type orientation = fProjectSettings->GetDefaultOrientation();

			if (*nativeWindowMode == Rtt::NativeWindowMode::kFullscreen)
			{
				fMode = NativeWindowMode::kFullscreen.GetStringId();
			}

			fullScreen = strcmp(fMode, NativeWindowMode::kFullscreen.GetStringId()) == 0;

			if (fullScreen)
			{
				SDL_DisplayMode DM;
				SDL_GetCurrentDisplayMode(0, &DM);
				width = DM.w;
				height = DM.h;
			}
			else
			{
				width = fProjectSettings->GetDefaultWindowViewWidth();
				height = fProjectSettings->GetDefaultWindowViewHeight();
			}

			switch (orientation)
			{
			case DeviceOrientation::kSidewaysRight:
				fRuntimeDelegate->fOrientation = DeviceOrientation::kSidewaysRight; // bottom of device is to the right

				if (width > 0 && height > 0)
				{
					fRuntimeDelegate->SetWidth(width);
					fRuntimeDelegate->SetHeight(height);
				}
				else
				{
					// no valid defaultViewWidth & defaultViewHeight in 'build.settings', default values of fWidth & fHeight for Portrait
					// use swapped default settings
					int w = fRuntimeDelegate->GetWidth();
					int h = fRuntimeDelegate->GetHeight();
					fRuntimeDelegate->SetWidth(480);
					fRuntimeDelegate->SetHeight(320);
				}
				break;

			case DeviceOrientation::kSidewaysLeft:
				fRuntimeDelegate->fOrientation = DeviceOrientation::kSidewaysLeft; // bottom of device is to the left

				if (width > 0 && height > 0)
				{
					fRuntimeDelegate->SetWidth(width);
					fRuntimeDelegate->SetHeight(height);
				}
				else
				{
					// no valid defaultViewWidth & defaultViewHeight in 'build.settings', default values of fWidth & fHeight for Portrait
					// use swapped default settings
					int w = fRuntimeDelegate->GetWidth();
					int h = fRuntimeDelegate->GetHeight();
					fRuntimeDelegate->SetWidth(h);
					fRuntimeDelegate->SetHeight(w);
				}
				break;

			case DeviceOrientation::kUpright:
				fRuntimeDelegate->fOrientation = DeviceOrientation::kUpright; // bottom of device is at the bottom

				if (width > 0 && height > 0)
				{
					fRuntimeDelegate->SetWidth(width);
					fRuntimeDelegate->SetHeight(height);
				}
				else
				{
					// no valid defaultViewWidth & defaultViewHeight in 'build.settings', default values of fWidth & fHeight for Portrait
					// use default settings
				}
				break;

			case DeviceOrientation::kUpsideDown:
				fRuntimeDelegate->fOrientation = DeviceOrientation::kUpsideDown; // bottom of device is at the top

				if (width > 0 && height > 0)
				{
					fRuntimeDelegate->SetWidth(width);
					fRuntimeDelegate->SetHeight(height);
				}
				else
				{
					// no valid defaultViewWidth & defaultViewHeight in 'build.settings', default values of fWidth & fHeight for Portrait
					// use default settings
				}
				break;
			}
		}

		fPlatform->setWindow(this);

		// Initialize Joystick Support:
		LinuxInputDeviceManager& deviceManager = (LinuxInputDeviceManager&)fPlatform->GetDevice().GetInputDeviceManager();
		deviceManager.init();
	}

	// global.print()
	int SolarAppContext::Print(lua_State* L)
	{
		membuf mb;
		int n = lua_gettop(L);  // number of arguments
		lua_getglobal(L, "tostring");
		for (int i = 1; i <= n; i++)
		{
			const char* s;
			lua_pushvalue(L, -1);  // function to be called 
			lua_pushvalue(L, i);   // value to print 
			lua_call(L, 1, 1);
			s = lua_tostring(L, -1);  // get result 
			if (s == NULL)
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

			if (i > 1)
			{
				mb.append('\t');
			}
			mb.append(s);

			lua_pop(L, 1);  // pop result 
		}

		puts(mb.c_str());
		mb.append('\n');
		//vv	ConsoleApp::Log(mb.c_str());
		return 0;
	}

	bool SolarAppContext::LoadApp(const string& appPath)
	{
		fPathToApp = appPath;

		// appName

		const char* homeDir = GetHomePath();
		fSaveFolder.append(homeDir);
		fSaveFolder.append("/Documents/Solar2D Built Apps");

		vector<string> tokens;
		splitString(tokens, appPath, "/");
		if (tokens.size() >= 2)
		{
			fAppName = tokens[tokens.size() - 1];

			// appName = exeFileName for standalone apps
			if (fAppName == "Resources")
			{
				GetStartupPath(&fAppName);
			}
		}

		fConfig.Load(GetConfigPath(fAppName));

		// set workdir

		if (Rtt_FileExists((fPathToApp + "/resource.car").c_str()))
		{
			chdir(GetAppPath().c_str());
		}
		else
			if (Rtt_FileExists((fPathToApp + "/main.lua").c_str()))
			{
				chdir(GetAppPath().c_str());
			}
			else
			{
				Rtt_ASSERT_NOT_REACHED();
			}

		// load app

		Init();

		if (Runtime::kSuccess != fRuntime->LoadApplication(Runtime::kLinuxLaunchOption, fRuntimeDelegate->fOrientation))
		{
			delete fRuntime;
			delete fPlatform;
			return false;
		}

		// add Resources to LUA_PATH
		string luapath(getenv("LUA_PATH"));
		luapath.append(GetStartupPath(NULL));
		luapath.append("/Resources/?.lua;");

		setenv("LUA_PATH", luapath.c_str(), true);

		if (fRuntimeDelegate->fOrientation == DeviceOrientation::kSidewaysRight || fRuntimeDelegate->fOrientation == DeviceOrientation::kSidewaysLeft)
		{
			// Swap(fRuntimeDelegate->fContentWidth, fRuntimeDelegate->fContentHeight);
		}

		if (app->IsRunningOnSimulator())
		{
			fLinuxSimulatorServices = new LinuxSimulatorServices();
			lua_State* luaStatePointer = fRuntime->VMContext().L();
			lua_pushlightuserdata(luaStatePointer, fLinuxSimulatorServices);
			Rtt::LuaContext::RegisterModuleLoader(luaStatePointer, Rtt::LuaLibSimulator::kName, Rtt::LuaLibSimulator::Open, 1);
		}

		// re-define
		lua_State* L = fRuntime->VMContext().L();
		lua_pushcfunction(L, print2console);
		lua_setglobal(L, "print");

		int w = fConfig["w"].to_int() > 0 ? fConfig["w"].to_int() : GetWidth();
		int h = fConfig["h"].to_int() > 0 ? fConfig["h"].to_int() : GetHeight();

		SDL_DisplayMode dm;
		SDL_GetCurrentDisplayMode(0, &dm);
		int x = fConfig["x"].to_int() > 0 ? fConfig["x"].to_int() : (dm.w - w) / 2;
		int y = fConfig["y"].to_int() > 0 ? fConfig["y"].to_int() : (dm.h - h) / 2;

		SetSize(w, h);
		SDL_SetWindowPosition(fWindow, x, y);

		string title = fConfig["title"].to_string();
		if (title.empty())
		{
			string localeName = "en_US"; // fixme
			string langCode = localeName.substr(0, 2);
			string countryCode = localeName.substr(3, 2);
			const char* projectTitle = fProjectSettings->GetWindowTitleTextForLocale(langCode.c_str(), countryCode.c_str());
			if (projectTitle)
			{
				title = projectTitle;
			}
		}

		SetTitle(title.empty() ? fAppName : title);
		return true;
	}

	// timer callback
	void SolarAppContext::advance()
	{
		if (fBeginRunLoop)
		{
			fBeginRunLoop = false;
			fRuntime->BeginRunLoop();
		}

		if (fRuntime->IsSuspended())
		{
			// render only GUI
			Flush();
#ifdef Rtt_SIMULATOR
			if (LinuxVideoTap* tap = app->GetVideoTap())
			{
				tap->CommitFrame();
			}
#endif
			return;
		}

		advanceCEF();

		LinuxInputDeviceManager& deviceManager = (LinuxInputDeviceManager&)GetPlatform()->GetDevice().GetInputDeviceManager();
		deviceManager.dispatchEvents(fRuntime);

		// advance engine
		(*fRuntime)();
#ifdef Rtt_SIMULATOR
		// One tap commit per tick, after every Flush (the runtime's render,
		// and any stills' re-renders) has run: the last staged frame wins.
		if (LinuxVideoTap* tap = app->GetVideoTap())
		{
			tap->CommitFrame();
		}
#endif
	}

	void SolarAppContext::Flush()
	{
		app->RenderGUI();
		fRuntime->GetDisplay().Invalidate();
#ifdef Rtt_SIMULATOR
		// Stage the frame tap before the swap, while the back buffer is still
		// current. Extra Flush() calls (Display::Capture re-renders through
		// here too) only overwrite the staging buffer; the commit happens
		// once per tick at the end of advance(), so stills taken mid-recording
		// insert no extra frames.
		if (LinuxVideoTap* tap = app->GetVideoTap())
		{
			int w = 0, h = 0;
			SDL_GL_GetDrawableSize(fWindow, &w, &h);
			tap->StageFrame(w, h);
		}
#endif
		SDL_GL_SwapWindow(fWindow);
	}

	void SolarAppContext::Pause()
	{
		if (!fRuntime->IsSuspended())
		{
			fRuntime->Suspend();
		}
	}

	void SolarAppContext::Resume()
	{
		if (GetRuntime()->IsSuspended())
		{
			fRuntime->DispatchEvent(ResizeEvent());
			fRuntime->Resume();
		}
	}

	void SolarAppContext::RestartRenderer()
	{
		fRuntime->GetDisplay().WindowSizeChanged();
		fRuntime->RestartRenderer(fRuntimeDelegate->fOrientation);
		fRuntime->GetDisplay().Invalidate();
		fRuntime->DispatchEvent(ResizeEvent());
	}

	int SolarAppContext::GetWidth() const
	{
		return fRuntimeDelegate->GetWidth();
	}

	void SolarAppContext::SetWidth(int val)
	{
		fRuntimeDelegate->SetWidth(val);
	}

	int SolarAppContext::GetHeight() const
	{
		return fRuntimeDelegate->GetHeight();
	}

	void SolarAppContext::SetHeight(int val)
	{
		fRuntimeDelegate->SetHeight(val);
	}

	void SolarAppContext::SetSize(int w, int h)
	{
		SetWidth(w);
		SetHeight(h);
#ifdef Rtt_SIMULATOR
		// The offscreen driver cannot resize its EGL pbuffer, so the window
		// (and with it the GL context) is recreated at the target size; on
		// every other driver RecreateWindowForOffscreen() is a no-op false
		// and the ordinary resize path runs.
		if (!app->RecreateWindowForOffscreen(w, h + app->GetMenuHeight()))
#endif
		{
			SDL_SetWindowSize(fWindow, w, h + app->GetMenuHeight());
		}
		RestartRenderer();

		fRuntime->DispatchEvent(ResizeEvent());

		// todo: refresh native elements
	}

	string SolarAppContext::GetTitle() const
	{
		return SDL_GetWindowTitle(fWindow);
	}

	void SolarAppContext::SetTitle(const string& xtitle)
	{
		string title = xtitle == HOMESCREEN_ID ? "Solar2D Simulator" : xtitle;
		SDL_SetWindowTitle(fWindow, title.c_str());
		SDL_ShowWindow(fWindow);
		fConfig["title"] = title;
	}

} // namespace Rtt

