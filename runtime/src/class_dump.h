// Logs the layout of UnrealScript classes (super class, functions with parameters, properties),
// for reverse engineering the game's script code. See class_dump.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace azura::script {

// Starts, once, a background thread that waits until the classes in the comma-separated
// `class_names` are loaded and logs them, with a rough listing of each function's bytecode when
// `with_code` is set. `base` is the start of guest memory.
void StartClassDump(const uint8_t* base, std::string class_names, bool with_code);

// Reports a class the game is using (the class of a running script function or object). Cheap
// when no dump is running.
void NoteClass(uint32_t cls);

// Research (host migration): the connections net driver `driver` holds (in its fields or in arrays
// among them, read without faulting), logged, and their words that hold `timeout` (connection,
// address): where a connection may keep its own copy of the driver's connection timeout.
std::vector<std::pair<uint32_t, uint32_t>> FindConnectionTimeouts(const uint8_t* base, uint32_t driver, float timeout);

// Research (host migration): in the connections of net driver `driver`, the doubles that hold a
// time `min_age` to `max_age` seconds before `clock` (the driver's own time): candidates for a
// connection's LastReceiveTime. Logged, with the floats in that range, and returned (address, value).
std::vector<std::pair<uint32_t, double>> StaleTimes(const uint8_t* base, uint32_t driver, double clock, double min_age,
                                                    double max_age);

// Research (host migration): looks for the game's net drivers in the object table on a thread of
// its own (it takes a while), and logs them; NetDrivers answers those of the last scan still alive.
void StartNetDriverScan(const uint8_t* base);
std::vector<uint32_t> NetDrivers(const uint8_t* base);
// Whether `object` (read without faulting) is still an object whose class name has `class_suffix` in it.
bool IsLiveObjectOf(const uint8_t* base, uint32_t object, const char* class_suffix);

}  // namespace azura::script
