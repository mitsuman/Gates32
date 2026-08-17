#pragma once

// Minimal SDL surface used by the native solver build.  The solver executes
// only the deterministic game logic; video, audio and host input are never
// initialized.  Keeping these declarations here lets the browser and solver
// share the exact same Gates32 implementation without a native SDL runtime.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Uint8 = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using SDL_AudioDeviceID = Uint32;
using SDL_Keycode = std::int32_t;

struct SDL_Window {};
struct SDL_Renderer {};
struct SDL_Texture {};
struct SDL_PixelFormat { Uint8 BytesPerPixel = 4; };
struct SDL_Surface { void* pixels = nullptr; SDL_PixelFormat* format = nullptr; };
struct SDL_Rect { int x = 0, y = 0, w = 0, h = 0; };
struct SDL_AudioSpec {
  int freq = 0;
  Uint16 format = 0;
  Uint8 channels = 0;
  Uint16 samples = 0;
  void (*callback)(void*, Uint8*, int) = nullptr;
  void* userdata = nullptr;
};
struct SDL_AudioCVT {
  Uint8* buf = nullptr;
  int len = 0;
  int len_cvt = 0;
  int len_mult = 1;
};
struct SDL_Keysym { SDL_Keycode sym = 0; };
struct SDL_KeyboardEvent { Uint8 repeat = 0; SDL_Keysym keysym{}; };
struct SDL_Event { Uint32 type = 0; SDL_KeyboardEvent key{}; };

constexpr int SDL_FALSE = 0;
constexpr int SDL_TRUE = 1;
constexpr Uint32 SDL_INIT_VIDEO = 1u;
constexpr Uint32 SDL_INIT_AUDIO = 2u;
constexpr Uint32 SDL_INIT_EVENTS = 4u;
constexpr int SDL_WINDOWPOS_CENTERED = 0;
constexpr Uint32 SDL_WINDOW_RESIZABLE = 1u;
constexpr Uint32 SDL_WINDOW_ALLOW_HIGHDPI = 2u;
constexpr Uint32 SDL_RENDERER_SOFTWARE = 1u;
constexpr Uint32 SDL_RENDERER_ACCELERATED = 2u;
constexpr Uint32 SDL_PIXELFORMAT_RGBA8888 = 0;
constexpr int SDL_TEXTUREACCESS_TARGET = 0;
constexpr int SDL_BLENDMODE_NONE = 0;
constexpr int SDL_BLENDMODE_BLEND = 1;
constexpr Uint16 AUDIO_F32SYS = 0;
constexpr int SDL_AUDIO_ALLOW_FREQUENCY_CHANGE = 1;
constexpr const char* SDL_HINT_RENDER_SCALE_QUALITY = "SDL_RENDER_SCALE_QUALITY";

constexpr Uint32 SDL_QUIT = 0x100;
constexpr Uint32 SDL_KEYDOWN = 0x300;
constexpr Uint32 SDL_KEYUP = 0x301;
constexpr Uint32 SDL_MOUSEBUTTONDOWN = 0x401;
constexpr Uint32 SDL_MOUSEBUTTONUP = 0x402;

constexpr int SDL_SCANCODE_DOWN = 0;
constexpr int SDL_SCANCODE_LEFT = 1;
constexpr int SDL_SCANCODE_RIGHT = 2;
constexpr int SDL_SCANCODE_UP = 3;
constexpr int SDL_SCANCODE_SPACE = 4;
constexpr int SDL_SCANCODE_KP_0 = 5;
constexpr int SDL_SCANCODE_Z = 6;

constexpr SDL_Keycode SDLK_ESCAPE = 27;
constexpr SDL_Keycode SDLK_RETURN = 13;
constexpr SDL_Keycode SDLK_KP_ENTER = 271;
constexpr SDL_Keycode SDLK_SPACE = 32;
constexpr SDL_Keycode SDLK_KP_0 = 256;
constexpr SDL_Keycode SDLK_z = 'z';
constexpr SDL_Keycode SDLK_0 = '0';
constexpr SDL_Keycode SDLK_1 = '1';
constexpr SDL_Keycode SDLK_9 = '9';
constexpr SDL_Keycode SDLK_F2 = 1002;
constexpr SDL_Keycode SDLK_F3 = 1003;
constexpr SDL_Keycode SDLK_F4 = 1004;
constexpr SDL_Keycode SDLK_F6 = 1006;
constexpr SDL_Keycode SDLK_F7 = 1007;
constexpr SDL_Keycode SDLK_F8 = 1008;
constexpr SDL_Keycode SDLK_F9 = 1009;
constexpr SDL_Keycode SDLK_F10 = 1010;
constexpr SDL_Keycode SDLK_F11 = 1011;
constexpr SDL_Keycode SDLK_F12 = 1012;

inline int SDL_Init(Uint32) { return 0; }
inline void SDL_Quit() {}
inline int SDL_SetHint(const char*, const char*) { return SDL_TRUE; }
inline const char* SDL_GetError() { return "headless SDL stub"; }
inline void SDL_Log(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  std::fputc('\n', stderr);
  va_end(arguments);
}
inline Uint32 SDL_GetTicks() { return 1; }
inline void SDL_Delay(Uint32) {}
inline SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, Uint32) { return nullptr; }
inline SDL_Renderer* SDL_CreateRenderer(SDL_Window*, int, Uint32) { return nullptr; }
inline SDL_Texture* SDL_CreateTexture(SDL_Renderer*, Uint32, int, int, int) { return nullptr; }
inline SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer*, SDL_Surface*) { return nullptr; }
inline SDL_Surface* SDL_LoadBMP(const char*) { return nullptr; }
inline void SDL_FreeSurface(SDL_Surface*) {}
inline void SDL_DestroyTexture(SDL_Texture*) {}
inline void SDL_DestroyRenderer(SDL_Renderer*) {}
inline void SDL_DestroyWindow(SDL_Window*) {}
inline int SDL_SetColorKey(SDL_Surface*, int, Uint32) { return 0; }
inline int SDL_SetTextureBlendMode(SDL_Texture*, int) { return 0; }
inline int SDL_RenderSetLogicalSize(SDL_Renderer*, int, int) { return 0; }
inline int SDL_RenderSetIntegerScale(SDL_Renderer*, int) { return 0; }
inline int SDL_SetRenderTarget(SDL_Renderer*, SDL_Texture*) { return 0; }
inline int SDL_SetRenderDrawColor(SDL_Renderer*, Uint8, Uint8, Uint8, Uint8) { return 0; }
inline int SDL_RenderClear(SDL_Renderer*) { return 0; }
inline int SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) { return 0; }
inline int SDL_RenderDrawPoint(SDL_Renderer*, int, int) { return 0; }
inline void SDL_RenderPresent(SDL_Renderer*) {}
inline int SDL_PollEvent(SDL_Event*) { return 0; }
inline const Uint8* SDL_GetKeyboardState(int*) { static Uint8 keys[512]{}; return keys; }

inline SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int) { return 0; }
inline void SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
inline void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
inline int SDL_GetAudioDeviceStatus(SDL_AudioDeviceID) { return 0; }
inline void SDL_LockAudioDevice(SDL_AudioDeviceID) {}
inline void SDL_UnlockAudioDevice(SDL_AudioDeviceID) {}
inline Uint8* SDL_LoadWAV(const char*, SDL_AudioSpec*, Uint8**, Uint32*) { return nullptr; }
inline void SDL_FreeWAV(Uint8*) {}
inline int SDL_BuildAudioCVT(SDL_AudioCVT*, Uint16, Uint8, int, Uint16, Uint8, int) { return -1; }
inline int SDL_ConvertAudio(SDL_AudioCVT*) { return -1; }
inline void* SDL_malloc(std::size_t size) { return std::malloc(size); }
inline void SDL_free(void* memory) { std::free(memory); }
