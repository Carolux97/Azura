#include "alboria_sounds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "menu/pc_options.h"
#include "script_hooks.h"

REXCVAR_DECLARE(bool, audio_mute);
REXCVAR_DEFINE_INT32(alboria_menu_volume, 10, "Audio",
                     "Volume of Alboria's menu sounds over the game (moving, choosing, the overlay opening "
                     "and closing), 0 to 10 of the game's effects volume")
    .range(0, 10);
REXCVAR_DEFINE_INT32(alboria_notice_volume, 10, "Audio",
                     "Volume of Alboria's notices over the game (an invitation, a friend request), 0 to 10 "
                     "of the game's effects volume")
    .range(0, 10);

namespace azura::alboria_sounds {

namespace {

using alboria_sound_synth::Buffer;
constexpr size_t kSoundCount = static_cast<size_t>(Sound::kCount);

// The Options' effects volume (a slider from 0 to 1, as a float's bits) in player 1's profile.
constexpr int kEffectsVolumeSetting = 42;
// How loud Alboria's sounds are at full effects volume: under the game's own, as a service's
// sounds over a game should be.
constexpr float kLoudness = 0.55f;

// The sounds, made by Prepare's thread; read only once g_made is true.
std::array<Buffer, kSoundCount> g_sounds;
std::atomic<bool> g_made{false};
std::atomic<bool> g_preparing{false};

// The sounds playing: which one, how far it has gone and how loud.
struct Voice {
  const Buffer* buffer;
  size_t frame;
  float gain;
};

// The device and the voices; the audio callback takes the stream's lock, then g_mutex.
std::mutex g_mutex;
bool g_open = false;
bool g_failed = false;
SDL_AudioStream* g_stream = nullptr;
std::vector<Voice> g_voices;
std::vector<float> g_mix;

// SDL asks for more audio: mixes the voices playing into it.
void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional_amount, int) {
  if (additional_amount <= 0) return;
  const size_t frames = static_cast<size_t>(additional_amount) / (2 * sizeof(float));
  std::lock_guard lock(g_mutex);
  g_mix.assign(frames * 2, 0.0f);
  for (Voice& voice : g_voices) {
    const Buffer& buffer = *voice.buffer;
    const size_t count = std::min(frames, buffer.size() / 2 - voice.frame);
    for (size_t i = 0; i < count * 2; ++i) g_mix[i] += buffer[voice.frame * 2 + i] * voice.gain;
    voice.frame += count;
  }
  std::erase_if(g_voices, [](const Voice& voice) { return voice.frame * 2 >= voice.buffer->size(); });
  for (float& sample : g_mix) sample = std::clamp(sample, -1.0f, 1.0f);
  SDL_PutAudioStreamData(stream, g_mix.data(), static_cast<int>(g_mix.size() * sizeof(float)));
}

// Opens the audio device, once. With g_mutex held.
bool Open() {
  if (g_open || g_failed) return g_open;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_WARN("[Azura] Alboria's sounds are off: SDL audio did not start ({})", SDL_GetError());
    g_failed = true;
    return false;
  }
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = alboria_sound_synth::kSampleRate;
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, nullptr);
  if (!g_stream) {
    REXLOG_WARN("[Azura] Alboria's sounds are off: no audio device ({})", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_failed = true;
    return false;
  }
  SDL_ResumeAudioStreamDevice(g_stream);
  g_open = true;
  return true;
}

// The game's effects volume, from 0 to 1 (1 until player 1's profile can be read).
float EffectsVolume() {
  uint32_t bits = 0;
  if (!azura::script::ReadProfileSetting(0, kEffectsVolumeSetting, bits)) return 1.0f;
  float volume;
  std::memcpy(&volume, &bits, sizeof(volume));
  return std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f;
}

}  // namespace

Group GroupOf(Sound sound) {
  return sound == Sound::kInvitation || sound == Sound::kNotice ? Group::kNotices : Group::kMenu;
}

int Volume(Group group) {
  const int volume = group == Group::kMenu ? REXCVAR_GET(alboria_menu_volume) : REXCVAR_GET(alboria_notice_volume);
  return std::clamp(volume, 0, kMaxVolume);
}

void SetVolume(Group group, int volume) {
  volume = std::clamp(volume, 0, kMaxVolume);
  azura::menu::StoreControlsSetting(group == Group::kMenu ? "alboria_menu_volume" : "alboria_notice_volume",
                                    std::to_string(volume), false);
}

void Prepare() {
  if (g_preparing.exchange(true)) return;
  std::thread([] {
    for (size_t i = 0; i < kSoundCount; ++i) g_sounds[i] = alboria_sound_synth::Make(static_cast<Sound>(i));
    g_made.store(true);
  }).detach();
}

void Play(Sound sound) {
  if (REXCVAR_GET(audio_mute) || !g_made.load() || sound == Sound::kCount) return;
  const float gain = kLoudness * EffectsVolume() * static_cast<float>(Volume(GroupOf(sound))) / kMaxVolume;
  if (gain <= 0.0f) return;
  if (g_stream) SDL_LockAudioStream(g_stream);
  {
    std::lock_guard lock(g_mutex);
    if (Open()) {
      // A tick replaces the one still ringing, so holding a direction does not pile them up.
      const Buffer* buffer = &g_sounds[static_cast<size_t>(sound)];
      if (sound == Sound::kMove || sound == Sound::kType) {
        std::erase_if(g_voices, [&](const Voice& voice) { return voice.buffer == buffer; });
      }
      g_voices.push_back({buffer, 0, gain});
    }
  }
  if (g_stream) SDL_UnlockAudioStream(g_stream);
}

void Shutdown() {
  SDL_AudioStream* stream = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (!g_open) return;
    stream = std::exchange(g_stream, nullptr);
    g_open = false;
  }
  // Outside the lock: destroying the stream waits for a callback that may be waiting for it.
  SDL_DestroyAudioStream(stream);
  std::lock_guard lock(g_mutex);
  g_voices.clear();
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace azura::alboria_sounds
