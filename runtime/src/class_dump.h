// Logs the layout of UnrealScript classes (super class, functions with parameters, properties),
// for reverse engineering the game's script code. See class_dump.cpp.

#pragma once

#include <cstdint>
#include <string>

namespace azura::script {

// Starts, once, a background thread that waits until the classes in the comma-separated
// `class_names` are loaded and logs them, with a rough listing of each function's bytecode when
// `with_code` is set. `base` is the start of guest memory.
void StartClassDump(const uint8_t* base, std::string class_names, bool with_code);

// Reports a class the game is using (the class of a running script function or object). Cheap
// when no dump is running.
void NoteClass(uint32_t cls);

}  // namespace azura::script
