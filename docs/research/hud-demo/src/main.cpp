// VaCuus HUD demo host: RmlUi (SDL2 + OpenGL3 backend) + QuickJS-ng.
#include "VacuusJs.h"

#include <RmlUi/Core.h>
#include <RmlUi_Backend.h>

#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start)
{
	return std::chrono::duration<double>(Clock::now() - start).count();
}

// Map the few keys the demo cares about to JS-friendly names.
const char* KeyName(Rml::Input::KeyIdentifier key)
{
	switch (key)
	{
	case Rml::Input::KI_1: return "1";
	case Rml::Input::KI_2: return "2";
	case Rml::Input::KI_3: return "3";
	case Rml::Input::KI_4: return "4";
	case Rml::Input::KI_ESCAPE: return "Escape";
	case Rml::Input::KI_SPACE: return "Space";
	default: return nullptr;
	}
}

bool HudKeyCallback(Rml::Context* /*context*/, Rml::Input::KeyIdentifier key, int /*key_modifier*/, float /*dp_ratio*/, bool priority)
{
	if (priority)
	{
		if (const char* name = KeyName(key))
			VacuusJs::OnKey(name);
	}
	return true; // never consume; let the context see the event too
}

// Debug helper: dump the presented frame to a BMP (used by the smoke test).
void SaveScreenshot(const std::string& path, int w, int h)
{
	using ReadPixelsFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
	using ReadBufferFn = void (*)(unsigned);
	auto read_pixels = reinterpret_cast<ReadPixelsFn>(SDL_GL_GetProcAddress("glReadPixels"));
	auto read_buffer = reinterpret_cast<ReadBufferFn>(SDL_GL_GetProcAddress("glReadBuffer"));
	if (!read_pixels)
		return;
	constexpr unsigned GL_FRONT_ = 0x0404, GL_BACK_ = 0x0405, GL_RGBA_ = 0x1908, GL_UBYTE_ = 0x1401;
	std::vector<unsigned char> px(size_t(w) * h * 4, 0);
	for (unsigned buffer : {GL_FRONT_, GL_BACK_})
	{
		if (read_buffer)
			read_buffer(buffer);
		read_pixels(0, 0, w, h, GL_RGBA_, GL_UBYTE_, px.data());
		bool non_black = false;
		for (size_t i = 0; i < px.size() && !non_black; i += 4)
			non_black = px[i] | px[i + 1] | px[i + 2];
		if (non_black)
			break;
	}
	std::vector<unsigned char> flipped(px.size());
	for (int y = 0; y < h; y++)
		std::memcpy(&flipped[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
	if (SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32))
	{
		SDL_SaveBMP(surf, path.c_str());
		SDL_FreeSurface(surf);
		std::printf("[vacuus] screenshot saved: %s\n", path.c_str());
	}
}

} // namespace

int main(int argc, char** argv)
{
	const int width = 1600;
	const int height = 900;

	double auto_exit_seconds = 0.0;
	std::string data_dir = "data";
	std::string shot_prefix; // save screenshots at 2.7s and 3.7s if set
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			auto_exit_seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--data") && i + 1 < argc)
			data_dir = argv[++i];
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc)
			shot_prefix = argv[++i];
	}

	if (!Backend::Initialize("VaCuus HUD Demo (RmlUi + QuickJS-ng)", width, height, true))
	{
		std::fprintf(stderr, "Failed to initialize SDL/GL backend\n");
		return 1;
	}

	Rml::SetSystemInterface(Backend::GetSystemInterface());
	Rml::SetRenderInterface(Backend::GetRenderInterface());
	Rml::Initialise();

	Rml::Context* context = Rml::CreateContext("main", Rml::Vector2i(width, height));
	if (!context)
	{
		Rml::Shutdown();
		Backend::Shutdown();
		return 1;
	}

	for (const char* font : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf", "RobotoMono-Regular.ttf", "RobotoMono-Bold.ttf"})
	{
		if (!Rml::LoadFontFace(data_dir + "/" + font))
			std::fprintf(stderr, "Failed to load font %s/%s\n", data_dir.c_str(), font);
	}

	Rml::ElementDocument* document = context->LoadDocument(data_dir + "/hud.rml");
	if (!document)
	{
		std::fprintf(stderr, "Failed to load %s/hud.rml\n", data_dir.c_str());
		Rml::Shutdown();
		Backend::Shutdown();
		return 1;
	}
	document->Show();

	if (!VacuusJs::Initialize(context, document, auto_exit_seconds))
	{
		std::fprintf(stderr, "Failed to initialize QuickJS\n");
		Rml::Shutdown();
		Backend::Shutdown();
		return 1;
	}
	VacuusJs::EvalFile((data_dir + "/hud.js").c_str());

	const Clock::time_point start = Clock::now();
	double ema_update_ms = 0.0, ema_render_ms = 0.0, ema_frame_s = 1.0 / 60.0;
	Clock::time_point last_frame = Clock::now();

	bool running = true;
	while (running)
	{
		running = Backend::ProcessEvents(context, &HudKeyCallback, false);
		if (VacuusJs::ExitRequested())
			running = false;

		const double now = SecondsSince(start);
		if (auto_exit_seconds > 0.0 && now >= auto_exit_seconds)
			running = false;

		VacuusJs::SetStats(ema_update_ms, ema_render_ms, 1.0 / ema_frame_s);
		VacuusJs::OnFrame(now);

		const Clock::time_point t0 = Clock::now();
		context->Update();
		const Clock::time_point t1 = Clock::now();

		Backend::BeginFrame();
		context->Render();
		const Clock::time_point t2 = Clock::now();
		Backend::PresentFrame();

		if (!shot_prefix.empty())
		{
			static bool shot1 = false, shot2 = false, shot3 = false;
			if (!shot1 && now >= 2.7) { shot1 = true; SaveScreenshot(shot_prefix + "_1.bmp", width, height); }
			if (!shot2 && now >= 3.7) { shot2 = true; SaveScreenshot(shot_prefix + "_2.bmp", width, height); }
			if (!shot3 && now >= 5.5) { shot3 = true; SaveScreenshot(shot_prefix + "_3.bmp", width, height); }
		}

		const Clock::time_point t3 = Clock::now();
		const double update_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		const double render_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
		const double frame_s = std::chrono::duration<double>(t3 - last_frame).count();
		last_frame = t3;
		ema_update_ms = ema_update_ms * 0.9 + update_ms * 0.1;
		ema_render_ms = ema_render_ms * 0.9 + render_ms * 0.1;
		ema_frame_s = ema_frame_s * 0.9 + frame_s * 0.1;
	}

	const int js_errors = VacuusJs::ErrorCount();

	// Destroy the document tree while JS is still alive (event listeners
	// self-release their JS callbacks on detach), then tear down JS, then Rml.
	document->Close();
	context->Update();
	VacuusJs::Shutdown();
	Rml::Shutdown();
	Backend::Shutdown();

	std::printf("[vacuus] clean exit, js_errors=%d\n", js_errors);
	return js_errors == 0 ? 0 : 2;
}
