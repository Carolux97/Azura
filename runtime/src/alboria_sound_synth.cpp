#include "alboria_sound_synth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace azura::alboria_sound_synth {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRate = static_cast<float>(kSampleRate);

// The call (D4, A4, E5), the F#5 that answers it, and the other notes the sounds use.
constexpr float kD4 = 293.66f;
constexpr float kGs4 = 415.30f;
constexpr float kA4 = 440.00f;
constexpr float kD5 = 587.33f;
constexpr float kE5 = 659.26f;
constexpr float kFs5 = 739.99f;

// The room: how much of it is heard, how long it rings, and how late it answers (the distance).
constexpr float kRoomWet = 0.55f;
constexpr float kRoomFeedback = 0.86f;
constexpr float kRoomPredelay = 0.03f;

// One note: its pitch, when it starts, how long it rings (the time its loudness takes to fall to
// about a third), how loud, and where in the stereo field (-1 left, 1 right).
struct Note {
  float frequency;
  float start;
  float ring;
  float gain;
  float pan;
};

// The instruments.
enum class Timbre { kStrike, kHalo, kStrings, kChant, kBass };

// A group of notes played by one instrument.
struct Layer {
  Timbre timbre;
  std::vector<Note> notes;
};

float Cents(float cents) {
  return std::pow(2.0f, cents / 1200.0f);
}

// The phase of a tone of `hz`, detuned by `detune` cents and with a vibrato of `depth` cents at
// `rate` Hz starting at phase `start`: the integral of its frequency, so the pitch only wobbles by
// the vibrato and never drifts as the note goes on.
float VibratoPhase(float hz, float t, float detune, float depth, float rate, float start) {
  const float base = Cents(detune);
  const float swing = base * depth * 0.000577623f;  // ln 2 / 1200
  const float w = 2 * kPi * rate;
  return 2 * kPi * hz * (base * t + swing * (std::cos(start) - std::cos(w * t + start)) / w);
}

float Peak(float x, float centre, float width) {
  return std::exp(-((x - centre) * (x - centre)) / (2 * width * width));
}

// Note `n` of instrument `timbre`, `t` seconds after it starts.
float Sample(Timbre timbre, const Note& n, float t) {
  const float f = n.frequency;
  const float ring = n.ring;
  switch (timbre) {
    case Timbre::kStrike: {
      // A warm electric piano: an FM strike whose brightness fades fast, a twin 4 cents above it,
      // and a sub an octave down, with a felt-soft attack.
      const float attack = t < 0.012f ? t / 0.012f : 1.0f;
      const float index = 1.1f * std::exp(-t / (ring * 0.35f));
      const float strike = std::sin(2 * kPi * f * t + index * std::sin(2 * kPi * f * t));
      const float twin = 0.35f * std::sin(2 * kPi * f * Cents(4) * t + index * 0.6f * std::sin(2 * kPi * f * t));
      const float sub = 0.25f * std::sin(2 * kPi * f * 0.5f * t);
      return attack * std::exp(-t / ring) * (strike + twin + sub) * n.gain;
    }
    case Timbre::kHalo: {
      // Pure sines above the strike, swelling in slowly and breathing with a slow tremolo, as a
      // shimmer reverb leaves behind.
      const float attack = t < 0.18f ? std::pow(t / 0.18f, 2.0f) : 1.0f;
      const float breath = 0.85f + 0.15f * std::sin(2 * kPi * 3.1f * t);
      const float tone = std::sin(2 * kPi * f * t) + 0.6f * std::sin(2 * kPi * f * Cents(6) * t);
      return attack * breath * std::exp(-t / ring) * tone * n.gain;
    }
    case Timbre::kStrings: {
      // A muted string section: six bowed voices a few cents apart (soft saws whose upper harmonics
      // roll off), a slow bow in and a gentle vibrato.
      const float attack = t < 0.7f ? std::pow(t / 0.7f, 1.6f) : 1.0f;
      constexpr float kDetune[6] = {-11.0f, -6.0f, -2.0f, 3.0f, 7.0f, 12.0f};
      float sum = 0;
      for (int v = 0; v < 6; ++v) {
        for (int h = 1; h <= 30; ++h) {
          const float hz = f * h;
          if (hz > 7000) break;
          sum += std::sin(VibratoPhase(hz, t, kDetune[v], 5.0f, 5.0f + 0.3f * v, static_cast<float>(v)) + h * v * 0.7f) /
                 h * std::exp(-hz / 1800.0f);
        }
      }
      return attack * std::exp(-t / ring) * sum * 0.22f * n.gain;
    }
    case Timbre::kChant: {
      // Monks far away holding a note on "oh": three voices in near unison with a straight tone and
      // the slightest slow waver, few harmonics shaped by a round, dark vowel (formants about 450
      // and 800 Hz), breathing in slowly.
      const float attack = t < 0.9f ? std::pow(t / 0.9f, 2.0f) : 1.0f;
      constexpr float kDetune[3] = {-2.0f, 0.0f, 2.5f};
      float sum = 0;
      for (int v = 0; v < 3; ++v) {
        for (int h = 1; h <= 8; ++h) {
          const float hz = f * h;
          const float vowel = Peak(hz, 450, 160) + 0.45f * Peak(hz, 800, 180) + 0.03f;
          sum += vowel * std::sin(VibratoPhase(hz, t, kDetune[v], 1.5f, 3.7f + 0.4f * v, 1.3f * v)) / h;
        }
      }
      return attack * std::exp(-t / ring) * sum * 0.45f * n.gain;
    }
    case Timbre::kBass: {
      // Double basses bowing the root: three players a few cents apart, a dark bowed tone, a slow
      // bow in and a slight vibrato.
      const float attack = t < 0.4f ? std::pow(t / 0.4f, 1.4f) : 1.0f;
      constexpr float kDetune[3] = {-4.0f, 0.0f, 4.5f};
      float sum = 0;
      for (int v = 0; v < 3; ++v) {
        for (int h = 1; h <= 14; ++h) {
          const float hz = f * h;
          sum += std::sin(VibratoPhase(hz, t, kDetune[v], 4.0f, 4.4f + 0.3f * v, 0.8f * v) + h * v) / h *
                 std::exp(-hz / 650.0f);
        }
      }
      return attack * std::exp(-t / ring) * sum * 0.5f * n.gain;
    }
  }
  return 0.0f;
}

// The room: four comb filters and two all-pass filters (Schroeder's reverberator), each side's
// delays a few samples apart so the tail is wide.
class Room {
 public:
  explicit Room(size_t spread) {
    constexpr float kCombs[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    constexpr float kAllPasses[2] = {0.0050f, 0.0017f};
    for (size_t i = 0; i < 4; ++i) combs_[i].assign(static_cast<size_t>(kCombs[i] * kRate) + spread, 0.0f);
    for (size_t i = 0; i < 2; ++i) passes_[i].assign(static_cast<size_t>(kAllPasses[i] * kRate) + spread / 2, 0.0f);
  }

  float Step(float in) {
    constexpr float kDamping = 0.3f;
    float out = 0.0f;
    for (size_t i = 0; i < 4; ++i) {
      auto& line = combs_[i];
      const float delayed = line[comb_at_[i]];
      low_[i] = delayed * (1.0f - kDamping) + low_[i] * kDamping;
      line[comb_at_[i]] = in + low_[i] * kRoomFeedback;
      comb_at_[i] = (comb_at_[i] + 1) % line.size();
      out += delayed;
    }
    out *= 0.25f;
    for (size_t i = 0; i < 2; ++i) {
      auto& line = passes_[i];
      const float delayed = line[pass_at_[i]];
      line[pass_at_[i]] = out + delayed * 0.6f;
      out = delayed - out * 0.6f;
      pass_at_[i] = (pass_at_[i] + 1) % line.size();
    }
    return out;
  }

 private:
  std::array<std::vector<float>, 4> combs_;
  std::array<size_t, 4> comb_at_ = {};
  std::array<float, 4> low_ = {};
  std::array<std::vector<float>, 2> passes_;
  std::array<size_t, 2> pass_at_ = {};
};

// Renders `layers` in the room (`wet` of it heard), its loudest point at `peak`.
Buffer Render(const std::vector<Layer>& layers, float wet, float peak) {
  float end = 0.0f;
  for (const Layer& layer : layers) {
    for (const Note& n : layer.notes) end = std::max(end, n.start + n.ring * 5.0f);
  }
  const size_t frames = static_cast<size_t>((end + 1.6f) * kRate);
  Buffer buffer(frames * 2, 0.0f);
  for (const Layer& layer : layers) {
    for (const Note& n : layer.notes) {
      // Equal-power panning.
      const float angle = (n.pan + 1.0f) * kPi * 0.25f;
      const float left = std::cos(angle);
      const float right = std::sin(angle);
      const size_t first = static_cast<size_t>(n.start * kRate);
      const size_t length = std::min(frames - first, static_cast<size_t>(n.ring * 5.0f * kRate));
      for (size_t i = 0; i < length; ++i) {
        const float sample = Sample(layer.timbre, n, static_cast<float>(i) / kRate);
        buffer[(first + i) * 2] += sample * left;
        buffer[(first + i) * 2 + 1] += sample * right;
      }
    }
  }
  Room room_left(0);
  Room room_right(23);
  std::vector<float> predelay(static_cast<size_t>(kRoomPredelay * kRate) + 1, 0.0f);
  size_t at = 0;
  for (size_t i = 0; i < frames; ++i) {
    predelay[at] = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
    at = (at + 1) % predelay.size();
    const float late = predelay[at];
    buffer[i * 2] += room_left.Step(late) * wet;
    buffer[i * 2 + 1] += room_right.Step(late) * wet;
  }
  float loudest = 0.0f;
  for (float sample : buffer) loudest = std::max(loudest, std::fabs(sample));
  if (loudest > 0.0f) {
    for (float& sample : buffer) sample *= peak / loudest;
  }
  // The room's tail ends in silence long before the frames made for it: cut it where nothing is
  // heard any more (under about -80 dB).
  size_t heard = buffer.size();
  while (heard >= 2 && std::fabs(buffer[heard - 1]) < 1e-4f && std::fabs(buffer[heard - 2]) < 1e-4f) heard -= 2;
  buffer.resize(heard);
  return buffer;
}

// `notes` with their pitch, loudness and ring scaled and their start delayed.
std::vector<Note> Scaled(const std::vector<Note>& notes, float pitch, float gain, float ring, float delay = 0.0f) {
  std::vector<Note> out;
  for (Note n : notes) {
    n.frequency *= pitch;
    n.gain *= gain;
    n.ring *= ring;
    n.start += delay;
    out.push_back(n);
  }
  return out;
}

// A sound that matters: the strike and its halo, over a bed holding its chord (`bed` sets how
// present it is): the strings an octave down, the monks on its first note and the fifth above, and
// the basses on its first note two octaves down, which fade first.
Buffer Large(const std::vector<Note>& strike, float peak, float bed) {
  const Note root = strike.front();
  std::vector<Note> chord;
  for (Note n : strike) {
    n.start = 0.0f;
    chord.push_back(n);
  }
  const float chant_ring = root.ring * 1.5f * 2.6f;
  return Render({{Timbre::kStrike, Scaled(strike, 1.0f, 1.0f, 1.5f)},
                 {Timbre::kHalo, Scaled(strike, 3.0f, 0.16f, 2.2f, 0.04f)},
                 {Timbre::kStrings, Scaled(chord, 0.5f, bed * 0.9f, 3.0f)},
                 {Timbre::kChant,
                  {{root.frequency * 0.5f, 0.0f, chant_ring, bed * 0.55f, -0.2f},
                   {root.frequency * 0.75f, 0.05f, chant_ring, bed * 0.45f, 0.2f}}},
                 {Timbre::kBass, {{root.frequency * 0.25f, 0.0f, root.ring * 1.5f * 1.4f, bed * 0.35f, 0.0f}}}},
                kRoomWet, peak);
}

// A short sound: the strike with a touch of halo, in less of the room.
Buffer Small(const std::vector<Note>& strike, float peak) {
  return Render({{Timbre::kStrike, strike}, {Timbre::kHalo, Scaled(strike, 2.0f, 0.08f, 3.0f, 0.02f)}},
                kRoomWet * 0.45f, peak);
}

}  // namespace

Buffer Make(Sound sound) {
  switch (sound) {
    case Sound::kMove:
      return Small({{kE5 * 2.0f, 0.0f, 0.05f, 1.0f, 0.1f}}, 0.2f);
    case Sound::kPage:
      return Small({{kA4 * 2.0f, 0.0f, 0.05f, 1.0f, -0.2f}, {kE5 * 2.0f, 0.035f, 0.06f, 0.8f, 0.2f}}, 0.24f);
    case Sound::kType:
      return Small({{kD5 * 2.0f, 0.0f, 0.035f, 1.0f, 0.0f}}, 0.13f);
    case Sound::kAccept:
      // Rising a fourth.
      return Small({{kA4, 0.0f, 0.14f, 0.9f, -0.15f}, {kD5, 0.08f, 0.32f, 1.0f, 0.15f}}, 0.42f);
    case Sound::kBack:
      return Small({{kA4, 0.0f, 0.16f, 1.0f, -0.1f}}, 0.28f);
    case Sound::kOpen:
      // The call's last two notes, rising.
      return Large({{kA4, 0.0f, 0.25f, 0.85f, -0.25f}, {kE5, 0.1f, 0.45f, 1.0f, 0.25f}}, 0.45f, 0.8f);
    case Sound::kClose:
      // And falling.
      return Large({{kE5, 0.0f, 0.18f, 0.8f, 0.25f}, {kA4, 0.09f, 0.35f, 0.9f, -0.25f}}, 0.38f, 0.7f);
    case Sound::kInvitation:
      // The whole call rising across the field, answered by the F# above it.
      return Large({{kD4, 0.0f, 0.5f, 0.9f, -0.45f},
                    {kA4, 0.16f, 0.5f, 0.85f, -0.1f},
                    {kE5, 0.32f, 0.6f, 0.8f, 0.25f},
                    {kFs5, 0.6f, 0.9f, 0.7f, 0.45f}},
                   0.6f, 1.0f);
    case Sound::kNotice:
      // The call's first two notes.
      return Large({{kD4, 0.0f, 0.45f, 0.9f, -0.3f}, {kA4, 0.16f, 0.55f, 0.85f, 0.2f}}, 0.5f, 0.8f);
    case Sound::kError:
      // A semitone falling, softly.
      return Small({{kA4, 0.0f, 0.14f, 0.9f, 0.0f}, {kGs4, 0.11f, 0.28f, 0.9f, 0.0f}}, 0.34f);
    case Sound::kCount:
      break;
  }
  return {};
}

}  // namespace azura::alboria_sound_synth
