#include "plugin.hpp"

Plugin* pluginInstance = nullptr;


// This plugin deliberately adds no modules. Its only job is to register an
// audio driver, which Rack exposes to every module that owns an audio::Port.
void init(Plugin* p) {
	pluginInstance = p;

	pipewireAudioInit();
}
