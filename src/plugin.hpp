#pragma once
#include <rack.hpp>


using namespace rack;

// Declare the Plugin, defined in plugin.cpp
extern Plugin* pluginInstance;

// Registers the native PipeWire audio driver (Linux only, no-op elsewhere).
// Defined in PipeWireAudio.cpp.
void pipewireAudioInit();
