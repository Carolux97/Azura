// A controller that presses buttons on a fixed schedule, for unattended test runs (the HLE graphics
// comparisons of docs/hle-graphics/ reach the main menu with it). Only installed when the
// environment variable AZURA_SCRIPTED_PAD is set, to a comma-separated list of
// "seconds:button" presses counted from start, for example "45:start,50:a,55:a". Buttons: a, b, x,
// y, start, back, up, down, left, right. Each press is held for a fifth of a second.
//
// AZURA_RECORD_PAD, set to a file path, records the presses of player 1 (whatever controller or
// keyboard drives it) to that file in the same format, timed from the same moment, so a run played
// by hand can be replayed with AZURA_SCRIPTED_PAD.

#include "input/scripted_pad.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

namespace azura::input {

namespace {

using Clock = std::chrono::steady_clock;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr DeviceId kScriptedPad = static_cast<DeviceId>(0x53435250);  // "SCRP"
constexpr auto kHold = std::chrono::milliseconds(200);

struct Press {
  Clock::duration at;
  uint16_t buttons;
};

uint16_t ButtonFromName(const std::string& name) {
  if (name == "a") return rex::input::X_INPUT_GAMEPAD_A;
  if (name == "b") return rex::input::X_INPUT_GAMEPAD_B;
  if (name == "x") return rex::input::X_INPUT_GAMEPAD_X;
  if (name == "y") return rex::input::X_INPUT_GAMEPAD_Y;
  if (name == "start") return rex::input::X_INPUT_GAMEPAD_START;
  if (name == "back") return rex::input::X_INPUT_GAMEPAD_BACK;
  if (name == "up") return rex::input::X_INPUT_GAMEPAD_DPAD_UP;
  if (name == "down") return rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
  if (name == "left") return rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
  if (name == "right") return rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
  return 0;
}

class ScriptedPadDriver final : public rex::input::InputDriver {
 public:
  ScriptedPadDriver(std::vector<Press> presses, rex::ui::Window* window)
      : InputDriver(window, 0), presses_(std::move(presses)), start_(Clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kScriptedPad;
    info.name = "Scripted pad";
    info.controller_subtype = rex::input::ControllerSubtype::kXbox;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
    if (id != kScriptedPad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      const Clock::duration now = Clock::now() - start_;
      uint16_t buttons = 0;
      for (const Press& press : presses_) {
        if (now >= press.at && now < press.at + kHold) {
          buttons |= press.buttons;
        }
      }
      if (buttons != last_buttons_) {
        last_buttons_ = buttons;
        ++packet_number_;
      }
      out_state->packet_number = packet_number_;
      out_state->gamepad.buttons = buttons;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kScriptedPad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // gamepad
      out_caps->sub_type = 0x01;  // gamepad
      out_caps->gamepad.buttons = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kScriptedPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kScriptedPad ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::vector<Press> presses_;
  Clock::time_point start_;
  uint16_t last_buttons_ = 0;
  uint32_t packet_number_ = 0;
};

constexpr std::pair<const char*, uint16_t> kButtonNames[] = {
    {"a", rex::input::X_INPUT_GAMEPAD_A},          {"b", rex::input::X_INPUT_GAMEPAD_B},
    {"x", rex::input::X_INPUT_GAMEPAD_X},          {"y", rex::input::X_INPUT_GAMEPAD_Y},
    {"start", rex::input::X_INPUT_GAMEPAD_START},  {"back", rex::input::X_INPUT_GAMEPAD_BACK},
    {"up", rex::input::X_INPUT_GAMEPAD_DPAD_UP},   {"down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {"left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {"right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
};

// Polls player 1's state every 10 ms and appends each new press to the file as "seconds:button,".
void StartPadRecorder(rex::input::InputSystem* input, const char* path) {
  auto file = std::make_shared<std::ofstream>(path, std::ios::trunc);
  if (!*file) {
    REXLOG_ERROR("[Azura input] Cannot write the pad recording to {}", path);
    return;
  }
  REXLOG_INFO("[Azura input] Recording player 1's presses to {}", path);
  std::thread([input, file, start = Clock::now()] {
    uint16_t last_buttons = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      rex::input::X_INPUT_STATE state = {};
      uint16_t buttons =
          input->GetState(0, &state) == X_ERROR_SUCCESS ? uint16_t(state.gamepad.buttons) : 0;
      uint16_t pressed = buttons & ~last_buttons;
      last_buttons = buttons;
      if (!pressed) {
        continue;
      }
      double seconds = std::chrono::duration<double>(Clock::now() - start).count();
      for (const auto& [name, button] : kButtonNames) {
        if (pressed & button) {
          *file << fmt::format("{:.1f}:{},", seconds, name);
        }
      }
      file->flush();
    }
  }).detach();
}

}  // namespace

void InstallScriptedPad(rex::input::InputSystem* input) {
  const char* record_path = std::getenv("AZURA_RECORD_PAD");
  if (input && record_path && *record_path) {
    StartPadRecorder(input, record_path);
  }
  const char* script = std::getenv("AZURA_SCRIPTED_PAD");
  if (!input || !script || !*script) {
    return;
  }
  std::vector<Press> presses;
  std::stringstream stream(script);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const uint16_t buttons = ButtonFromName(item.substr(colon + 1));
    if (buttons) {
      presses.push_back({std::chrono::duration_cast<Clock::duration>(
                             std::chrono::duration<double>(std::atof(item.substr(0, colon).c_str()))),
                         buttons});
    }
  }
  REXLOG_INFO("[Azura input] Scripted pad with {} presses", presses.size());
  input->AddDriver(std::make_unique<ScriptedPadDriver>(std::move(presses), input->window()));
}

}  // namespace azura::input
