#include "plugin.hpp"

// ============================================================================
// Native PipeWire audio driver
// ============================================================================
//
// Registers a "PipeWire" entry in the driver dropdown of every module that
// owns an audio::Port (VCV Audio 2/8/16 and friends). Rack's stock Linux
// drivers all go through RtAudio, which only exposes PipeWire indirectly:
// the ALSA and PulseAudio backends lose the graph, and the JACK backend
// invents "devices" out of the physical clients instead of giving Rack a node
// you can patch freely.
//
// Two kinds of device are offered:
//   - "Manual routing (N ch)": a bare node with named DSP ports, left
//     unconnected for you to wire up in qpwgraph / Helvum / pw-link.
//   - One entry per audio sink in the graph (sound cards, null sinks, ...),
//     which additionally links our ports to that card on open. The capture
//     side of the same card is paired automatically via its device.id, so
//     picking a card gives a real duplex device.
//
// libpipewire is loaded with dlopen() and never linked, so a machine without
// PipeWire (or a Windows/macOS build) simply doesn't get the driver rather
// than failing to load the whole plugin.
// ============================================================================

// HAVE_PIPEWIRE is defined by the Makefile when the PipeWire headers are
// present on the build host.
#if defined(ARCH_LIN) && defined(HAVE_PIPEWIRE)

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>


namespace uzz {
namespace pipewire {


// Stored in patches, so it must never change once released. Rack's own
// RtAudio drivers use the small RtAudio::Api enum values, so anything large
// is safe from collisions.
static const int DRIVER_ID = 0x555A5A01;

// PipeWire's default max-quantum is 2048; leave generous headroom so a server
// configured for large buffers doesn't force us to drop cycles.
static const int MAX_FRAMES = 16384;

// Unrouted nodes for wiring by hand. Rack lists one menu entry per block of
// channels the module can handle, so a 2-channel module turns the 16 ch device
// into eight entries — keep this list short. Rack resolves devices by name
// when loading a patch, so these strings are part of the saved-patch contract
// just like the driver ID.
static const struct {
    const char* name;
    int numChannels;
} MANUAL_DEVICES[] = {
    {"Manual routing (2 ch)", 2},
    {"Manual routing (16 ch)", 16},
};
static const int NUM_MANUAL_DEVICES = (int) (sizeof(MANUAL_DEVICES) / sizeof(MANUAL_DEVICES[0]));


// ----------------------------------------------------------------------------
// Dynamically loaded libpipewire entry points
// ----------------------------------------------------------------------------

struct Lib {
    void* handle = NULL;

    void (*init)(int*, char***);
    const char* (*get_library_version)(void);

    struct pw_thread_loop* (*thread_loop_new)(const char*, const struct spa_dict*);
    void (*thread_loop_destroy)(struct pw_thread_loop*);
    struct pw_loop* (*thread_loop_get_loop)(struct pw_thread_loop*);
    int (*thread_loop_start)(struct pw_thread_loop*);
    void (*thread_loop_stop)(struct pw_thread_loop*);
    void (*thread_loop_lock)(struct pw_thread_loop*);
    void (*thread_loop_unlock)(struct pw_thread_loop*);

    struct pw_context* (*context_new)(struct pw_loop*, struct pw_properties*, size_t);
    void (*context_destroy)(struct pw_context*);
    struct pw_core* (*context_connect)(struct pw_context*, struct pw_properties*, size_t);
    int (*core_disconnect)(struct pw_core*);
    void (*proxy_destroy)(struct pw_proxy*);

    struct pw_properties* (*properties_new)(const char*, ...);
    void (*properties_free)(struct pw_properties*);

    struct pw_filter* (*filter_new_simple)(struct pw_loop*, const char*,
                                           struct pw_properties*,
                                           const struct pw_filter_events*, void*);
    void (*filter_destroy)(struct pw_filter*);
    int (*filter_connect)(struct pw_filter*, enum pw_filter_flags,
                          const struct spa_pod**, uint32_t);
    void* (*filter_add_port)(struct pw_filter*, enum pw_direction,
                             enum pw_filter_port_flags, size_t,
                             struct pw_properties*, const struct spa_pod**, uint32_t);
    void* (*filter_get_dsp_buffer)(void*, uint32_t);
    uint32_t (*filter_get_node_id)(struct pw_filter*);
};

static Lib lib;


template <typename T>
static bool loadSym(T& fn, const char* name) {
    void* sym = dlsym(lib.handle, name);
    if (!sym) {
        WARN("PipeWire: missing symbol %s", name);
        return false;
    }
    // The usual POSIX object-pointer-to-function-pointer dance.
    fn = reinterpret_cast<T>(reinterpret_cast<uintptr_t>(sym));
    return true;
}

static bool loadLibrary() {
    lib.handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle) {
        INFO("PipeWire: libpipewire-0.3.so.0 not available (%s), driver disabled", dlerror());
        return false;
    }

    bool ok = true;
    ok &= loadSym(lib.init, "pw_init");
    ok &= loadSym(lib.get_library_version, "pw_get_library_version");
    ok &= loadSym(lib.thread_loop_new, "pw_thread_loop_new");
    ok &= loadSym(lib.thread_loop_destroy, "pw_thread_loop_destroy");
    ok &= loadSym(lib.thread_loop_get_loop, "pw_thread_loop_get_loop");
    ok &= loadSym(lib.thread_loop_start, "pw_thread_loop_start");
    ok &= loadSym(lib.thread_loop_stop, "pw_thread_loop_stop");
    ok &= loadSym(lib.thread_loop_lock, "pw_thread_loop_lock");
    ok &= loadSym(lib.thread_loop_unlock, "pw_thread_loop_unlock");
    ok &= loadSym(lib.context_new, "pw_context_new");
    ok &= loadSym(lib.context_destroy, "pw_context_destroy");
    ok &= loadSym(lib.context_connect, "pw_context_connect");
    ok &= loadSym(lib.core_disconnect, "pw_core_disconnect");
    ok &= loadSym(lib.proxy_destroy, "pw_proxy_destroy");
    ok &= loadSym(lib.properties_new, "pw_properties_new");
    ok &= loadSym(lib.properties_free, "pw_properties_free");
    ok &= loadSym(lib.filter_new_simple, "pw_filter_new_simple");
    ok &= loadSym(lib.filter_destroy, "pw_filter_destroy");
    ok &= loadSym(lib.filter_connect, "pw_filter_connect");
    ok &= loadSym(lib.filter_add_port, "pw_filter_add_port");
    ok &= loadSym(lib.filter_get_dsp_buffer, "pw_filter_get_dsp_buffer");
    ok &= loadSym(lib.filter_get_node_id, "pw_filter_get_node_id");

    if (!ok) {
        dlclose(lib.handle);
        lib.handle = NULL;
        return false;
    }
    return true;
}


static std::string dictGet(const struct spa_dict* props, const char* key) {
    const char* value = props ? spa_dict_lookup(props, key) : NULL;
    return value ? std::string(value) : std::string();
}


// ----------------------------------------------------------------------------
// Graph monitor
// ----------------------------------------------------------------------------
//
// A second, long-lived PipeWire connection that mirrors the registry. It
// answers two questions the audio Device can't answer on its own: which sinks
// exist (so they can be listed as Rack devices), and which global port IDs to
// hand to the link factory.

struct NodeInfo {
    std::string name;
    std::string description;
    std::string mediaClass;
    std::string deviceId;
};

struct PortInfo {
    uint32_t nodeId = 0;
    uint32_t portIndex = 0;
    bool isInput = false;
    std::string name;
};

/** A sink (plus the capture side of the same card, when there is one). */
struct Target {
    std::string key;        // sink node.name, stable across sessions
    std::string label;      // what Rack shows
    uint32_t sinkNodeId = 0;
    uint32_t sourceNodeId = 0;
    int numOutputs = 0;
    int numInputs = 0;
};


struct Graph {
    struct pw_thread_loop* loop = NULL;
    struct pw_context* context = NULL;
    struct pw_core* core = NULL;
    struct pw_registry* registry = NULL;
    struct spa_hook coreHook;
    struct spa_hook registryHook;

    std::mutex mutex;
    std::map<uint32_t, NodeInfo> nodes;
    std::map<uint32_t, PortInfo> ports;

    std::atomic<bool> syncDone;
    int syncSeq = 0;

    Graph() {
        syncDone.store(false);
        memset(&coreHook, 0, sizeof(coreHook));
        memset(&registryHook, 0, sizeof(registryHook));
    }

    bool start() {
        loop = lib.thread_loop_new("VCV Rack graph", NULL);
        if (!loop)
            return false;

        context = lib.context_new(lib.thread_loop_get_loop(loop), NULL, 0);
        if (!context) {
            lib.thread_loop_destroy(loop);
            loop = NULL;
            return false;
        }

        if (lib.thread_loop_start(loop) < 0) {
            lib.context_destroy(context);
            context = NULL;
            lib.thread_loop_destroy(loop);
            loop = NULL;
            return false;
        }

        lib.thread_loop_lock(loop);
        core = lib.context_connect(context, NULL, 0);
        if (core) {
            pw_core_add_listener(core, &coreHook, &coreEvents(), this);
            registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
            pw_registry_add_listener(registry, &registryHook, &registryEvents(), this);
        }
        lib.thread_loop_unlock(loop);

        if (!core) {
            stop();
            return false;
        }

        // Let the initial burst of registry globals arrive before anybody asks
        // for the device list.
        roundtrip();
        return true;
    }

    void stop() {
        if (!loop)
            return;
        lib.thread_loop_stop(loop);
        if (registry) {
            lib.proxy_destroy((struct pw_proxy*) registry);
            registry = NULL;
        }
        if (core) {
            lib.core_disconnect(core);
            core = NULL;
        }
        if (context) {
            lib.context_destroy(context);
            context = NULL;
        }
        lib.thread_loop_destroy(loop);
        loop = NULL;
    }

    /** Blocks until the server has processed everything sent so far, so the
    registry mirror is up to date. */
    void roundtrip() {
        if (!core)
            return;
        syncDone.store(false);
        lib.thread_loop_lock(loop);
        syncSeq = pw_core_sync(core, PW_ID_CORE, 0);
        lib.thread_loop_unlock(loop);
        for (int i = 0; i < 300 && !syncDone.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // -- Registry mirror ------------------------------------------------------

    void onGlobal(uint32_t id, const char* type, const struct spa_dict* props) {
        if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
            NodeInfo node;
            node.name = dictGet(props, PW_KEY_NODE_NAME);
            node.description = dictGet(props, PW_KEY_NODE_DESCRIPTION);
            node.mediaClass = dictGet(props, PW_KEY_MEDIA_CLASS);
            node.deviceId = dictGet(props, PW_KEY_DEVICE_ID);
            std::lock_guard<std::mutex> lock(mutex);
            nodes[id] = node;
        }
        else if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
            PortInfo port;
            port.nodeId = (uint32_t) strtoul(dictGet(props, PW_KEY_NODE_ID).c_str(), NULL, 10);
            port.portIndex = (uint32_t) strtoul(dictGet(props, PW_KEY_PORT_ID).c_str(), NULL, 10);
            port.isInput = (dictGet(props, PW_KEY_PORT_DIRECTION) == "in");
            port.name = dictGet(props, PW_KEY_PORT_NAME);
            std::lock_guard<std::mutex> lock(mutex);
            ports[id] = port;
        }
    }

    void onGlobalRemove(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        nodes.erase(id);
        ports.erase(id);
    }

    // -- Queries --------------------------------------------------------------

    /** Global port IDs of one node in one direction, ordered by port index.
    Caller must hold the mutex. */
    std::vector<uint32_t> portIdsLocked(uint32_t nodeId, bool isInput) {
        std::vector<std::pair<uint32_t, uint32_t> > found;  // (portIndex, globalId)
        for (std::map<uint32_t, PortInfo>::const_iterator it = ports.begin();
             it != ports.end(); ++it) {
            if (it->second.nodeId == nodeId && it->second.isInput == isInput)
                found.push_back(std::make_pair(it->second.portIndex, it->first));
        }
        std::sort(found.begin(), found.end());
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < found.size(); i++)
            ids.push_back(found[i].second);
        return ids;
    }

    std::vector<uint32_t> portIds(uint32_t nodeId, bool isInput) {
        std::lock_guard<std::mutex> lock(mutex);
        return portIdsLocked(nodeId, isInput);
    }

    /** Global port ID of one of our own filter ports, matched by name. */
    uint32_t portIdByName(uint32_t nodeId, const std::string& portName) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::map<uint32_t, PortInfo>::const_iterator it = ports.begin();
             it != ports.end(); ++it) {
            if (it->second.nodeId == nodeId && it->second.name == portName)
                return it->first;
        }
        return SPA_ID_INVALID;
    }

    /** Every audio sink in the graph, paired with the capture side of the same
    card when there is one. Sorted by node name so Rack's device IDs stay put
    for as long as the graph does. */
    std::vector<Target> getTargets() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Target> targets;

        for (std::map<uint32_t, NodeInfo>::const_iterator it = nodes.begin();
             it != nodes.end(); ++it) {
            const NodeInfo& sink = it->second;
            if (sink.mediaClass != "Audio/Sink")
                continue;

            Target target;
            target.key = sink.name;
            target.label = sink.description.empty() ? sink.name : sink.description;
            target.sinkNodeId = it->first;
            target.numOutputs = (int) portIdsLocked(it->first, true).size();

            // A card shows up as a separate sink node and source node sharing
            // one device.id. Pairing them turns "pick a card" into a duplex
            // device instead of an output-only one.
            if (!sink.deviceId.empty()) {
                for (std::map<uint32_t, NodeInfo>::const_iterator jt = nodes.begin();
                     jt != nodes.end(); ++jt) {
                    const NodeInfo& source = jt->second;
                    if (source.deviceId != sink.deviceId)
                        continue;
                    if (source.mediaClass != "Audio/Source" &&
                        source.mediaClass != "Audio/Source/Virtual")
                        continue;
                    target.sourceNodeId = jt->first;
                    target.numInputs = (int) portIdsLocked(jt->first, false).size();
                    break;
                }
            }

            // A suspended sink can momentarily report no ports; a stereo guess
            // beats offering a device with no channels at all.
            if (target.numOutputs == 0)
                target.numOutputs = 2;

            targets.push_back(target);
        }

        std::sort(targets.begin(), targets.end(), byKey);
        return targets;
    }

    static bool byKey(const Target& a, const Target& b) {
        return a.key < b.key;
    }

    /** Creates a link and returns its proxy, or NULL. Takes the loop lock. */
    struct pw_proxy* createLink(uint32_t outNode, uint32_t outPort,
                                uint32_t inNode, uint32_t inPort) {
        char outNodeStr[16], outPortStr[16], inNodeStr[16], inPortStr[16];
        snprintf(outNodeStr, sizeof(outNodeStr), "%u", outNode);
        snprintf(outPortStr, sizeof(outPortStr), "%u", outPort);
        snprintf(inNodeStr, sizeof(inNodeStr), "%u", inNode);
        snprintf(inPortStr, sizeof(inPortStr), "%u", inPort);

        struct pw_properties* props = lib.properties_new(
            PW_KEY_LINK_OUTPUT_NODE, outNodeStr,
            PW_KEY_LINK_OUTPUT_PORT, outPortStr,
            PW_KEY_LINK_INPUT_NODE, inNodeStr,
            PW_KEY_LINK_INPUT_PORT, inPortStr,
            (const char*) NULL);
        if (!props)
            return NULL;

        lib.thread_loop_lock(loop);
        struct pw_proxy* proxy = (struct pw_proxy*) pw_core_create_object(
            core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
            &props->dict, 0);
        lib.thread_loop_unlock(loop);

        lib.properties_free(props);
        return proxy;
    }

    void destroyLinks(std::vector<struct pw_proxy*>& links) {
        if (links.empty())
            return;
        lib.thread_loop_lock(loop);
        for (size_t i = 0; i < links.size(); i++)
            lib.proxy_destroy(links[i]);
        lib.thread_loop_unlock(loop);
        links.clear();
    }

    // -- Event tables (no designated initializers in C++11) -------------------

    static void onCoreDoneCb(void* data, uint32_t id, int seq) {
        Graph* self = (Graph*) data;
        if (id == PW_ID_CORE && seq == self->syncSeq)
            self->syncDone.store(true);
    }

    static void onGlobalCb(void* data, uint32_t id, uint32_t permissions,
                           const char* type, uint32_t version,
                           const struct spa_dict* props) {
        ((Graph*) data)->onGlobal(id, type, props);
    }

    static void onGlobalRemoveCb(void* data, uint32_t id) {
        ((Graph*) data)->onGlobalRemove(id);
    }

    static const struct pw_core_events& coreEvents() {
        static struct pw_core_events events;
        static bool init = false;
        if (!init) {
            memset(&events, 0, sizeof(events));
            events.version = PW_VERSION_CORE_EVENTS;
            events.done = onCoreDoneCb;
            init = true;
        }
        return events;
    }

    static const struct pw_registry_events& registryEvents() {
        static struct pw_registry_events events;
        static bool init = false;
        if (!init) {
            memset(&events, 0, sizeof(events));
            events.version = PW_VERSION_REGISTRY_EVENTS;
            events.global = onGlobalCb;
            events.global_remove = onGlobalRemoveCb;
            init = true;
        }
        return events;
    }
};

static Graph graph;


// ----------------------------------------------------------------------------
// Device
// ----------------------------------------------------------------------------

struct Device : rack::audio::Device {
    std::string name;
    int numInputs;
    int numOutputs;
    Target target;          // sinkNodeId == 0 means "leave it unrouted"

    // What we ask PipeWire for. The server is free to refuse either one.
    float reqSampleRate = 48000.f;
    int reqBlockSize = 256;
    // What the graph actually granted, observed in the process() callback.
    std::atomic<float> actualSampleRate;
    std::atomic<int> actualBlockSize;
    std::atomic<bool> cycleSeen;

    struct pw_thread_loop* loop = NULL;
    struct pw_filter* filter = NULL;
    std::vector<void*> inPorts;
    std::vector<void*> outPorts;
    std::vector<struct pw_proxy*> links;

    // Interleaved scratch buffers handed to Rack. PipeWire gives us planar
    // mono buffers, one per port.
    std::vector<float> inBuf;
    std::vector<float> outBuf;

    bool overrunLogged = false;

    Device(const std::string& name, int numInputs, int numOutputs, const Target& target)
        : name(name), numInputs(numInputs), numOutputs(numOutputs), target(target) {
        actualSampleRate.store(reqSampleRate);
        actualBlockSize.store(reqBlockSize);
        cycleSeen.store(false);
        inBuf.resize((size_t) MAX_FRAMES * std::max(numInputs, 1));
        outBuf.resize((size_t) MAX_FRAMES * std::max(numOutputs, 1));
        open();
    }

    ~Device() override {
        close();
    }

    // -- audio::Device interface ---------------------------------------------

    std::string getName() override {
        return name;
    }
    int getNumInputs() override {
        return numInputs;
    }
    int getNumOutputs() override {
        return numOutputs;
    }

    std::set<float> getSampleRates() override {
        return {44100.f, 48000.f, 88200.f, 96000.f, 176400.f, 192000.f};
    }
    float getSampleRate() override {
        return actualSampleRate.load();
    }
    void setSampleRate(float sampleRate) override {
        if (sampleRate == reqSampleRate)
            return;
        reqSampleRate = sampleRate;
        reopen();
    }

    std::set<int> getBlockSizes() override {
        return {32, 64, 128, 256, 512, 1024, 2048};
    }
    int getBlockSize() override {
        return actualBlockSize.load();
    }
    void setBlockSize(int blockSize) override {
        if (blockSize == reqBlockSize)
            return;
        reqBlockSize = blockSize;
        reopen();
    }

    // -- Stream lifecycle -----------------------------------------------------

    void reopen() {
        close();
        open();
    }

    void open() {
        char rateStr[32];
        char latencyStr[32];
        snprintf(rateStr, sizeof(rateStr), "1/%d", (int) reqSampleRate);
        snprintf(latencyStr, sizeof(latencyStr), "%d/%d", reqBlockSize, (int) reqSampleRate);

        loop = lib.thread_loop_new("VCV Rack", NULL);
        if (!loop)
            throw rack::Exception("Could not create PipeWire thread loop");

        struct pw_properties* props = lib.properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Duplex",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_APP_NAME, "VCV Rack",
            PW_KEY_NODE_NAME, "VCV Rack",
            PW_KEY_NODE_DESCRIPTION, name.c_str(),
            PW_KEY_NODE_RATE, rateStr,
            PW_KEY_NODE_LATENCY, latencyStr,
            // Rack's engine is clocked by our process() callback when this is
            // the primary device, so the node must keep cycling even with
            // nothing patched into it. Without these the engine stalls as soon
            // as the session manager decides we're idle.
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            PW_KEY_NODE_PAUSE_ON_IDLE, "false",
            PW_KEY_NODE_SUSPEND_ON_IDLE, "false",
            (const char*) NULL);

        filter = lib.filter_new_simple(lib.thread_loop_get_loop(loop), "VCV Rack",
                                       props, &filterEvents(), this);
        if (!filter) {
            lib.thread_loop_destroy(loop);
            loop = NULL;
            throw rack::Exception("Could not create PipeWire filter");
        }

        inPorts.resize(numInputs, NULL);
        outPorts.resize(numOutputs, NULL);
        for (int i = 0; i < numInputs; i++)
            inPorts[i] = addPort(PW_DIRECTION_INPUT, "in_%d", i);
        for (int i = 0; i < numOutputs; i++)
            outPorts[i] = addPort(PW_DIRECTION_OUTPUT, "out_%d", i);

        int err = lib.filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
        if (err < 0) {
            lib.filter_destroy(filter);
            filter = NULL;
            lib.thread_loop_destroy(loop);
            loop = NULL;
            throw rack::Exception("Could not connect PipeWire filter: %s", strerror(-err));
        }

        cycleSeen.store(false);
        actualSampleRate.store(reqSampleRate);
        actualBlockSize.store(reqBlockSize);
        overrunLogged = false;

        if (lib.thread_loop_start(loop) < 0) {
            close();
            throw rack::Exception("Could not start PipeWire thread loop");
        }

        waitForCycle();
        connectToTarget();

        // Linking to a card can hand the node a different driver, and with it
        // a different rate or quantum, so re-read after wiring rather than
        // before. Rack asks for these right after open() to set the engine
        // sample rate, and PipeWire silently ignores a rate that isn't in
        // clock.allowed-rates.
        float gotRate = actualSampleRate.load();
        int gotBlock = actualBlockSize.load();
        INFO("PipeWire: opened \"%s\" (%d in, %d out, %g sample rate, %d block size)",
             name.c_str(), numInputs, numOutputs, gotRate, gotBlock);
        if (gotRate != reqSampleRate) {
            WARN("PipeWire: requested %g Hz but the graph runs at %g Hz. "
                 "Add the rate to clock.allowed-rates in pipewire.conf to change it.",
                 reqSampleRate, gotRate);
        }
        if (gotBlock != reqBlockSize) {
            WARN("PipeWire: requested a block size of %d but the graph quantum is %d. "
                 "Another client may be forcing it.", reqBlockSize, gotBlock);
        }

        onStartStream();
    }

    void close() {
        if (!loop)
            return;
        onStopStream();
        graph.destroyLinks(links);
        // Must be called without the loop lock held; the loop thread is joined
        // here, so the filter can be torn down safely afterwards.
        lib.thread_loop_stop(loop);
        if (filter) {
            lib.filter_destroy(filter);
            filter = NULL;
        }
        lib.thread_loop_destroy(loop);
        loop = NULL;
        inPorts.clear();
        outPorts.clear();
        INFO("PipeWire: closed \"%s\"", name.c_str());
    }

    void* addPort(enum pw_direction dir, const char* fmt, int index) {
        char portName[32];
        snprintf(portName, sizeof(portName), fmt, index + 1);
        struct pw_properties* props = lib.properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, portName,
            (const char*) NULL);
        return lib.filter_add_port(filter, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                   0, props, NULL, 0);
    }

    /** Waits for the graph to schedule us at least once, so the reported rate
    and quantum are the real ones. */
    void waitForCycle() {
        for (int i = 0; i < 500 && !cycleSeen.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /** Wires our ports to the selected card. No-op for the manual devices. */
    void connectToTarget() {
        if (target.sinkNodeId == 0)
            return;

        uint32_t nodeId = lib.filter_get_node_id(filter);
        if (nodeId == SPA_ID_INVALID) {
            WARN("PipeWire: node for \"%s\" has no ID yet, leaving it unrouted", name.c_str());
            return;
        }

        // Our own ports have to reach the registry before they can be linked.
        graph.roundtrip();

        std::vector<uint32_t> sinkPorts = graph.portIds(target.sinkNodeId, true);
        std::vector<uint32_t> sourcePorts = graph.portIds(target.sourceNodeId, false);

        int outLinks = 0;
        for (int c = 0; c < numOutputs && c < (int) sinkPorts.size(); c++) {
            char portName[32];
            snprintf(portName, sizeof(portName), "out_%d", c + 1);
            uint32_t ourPort = graph.portIdByName(nodeId, portName);
            if (ourPort == SPA_ID_INVALID)
                continue;
            struct pw_proxy* link = graph.createLink(nodeId, ourPort, target.sinkNodeId, sinkPorts[c]);
            if (link) {
                links.push_back(link);
                outLinks++;
            }
        }

        int inLinks = 0;
        for (int c = 0; c < numInputs && c < (int) sourcePorts.size(); c++) {
            char portName[32];
            snprintf(portName, sizeof(portName), "in_%d", c + 1);
            uint32_t ourPort = graph.portIdByName(nodeId, portName);
            if (ourPort == SPA_ID_INVALID)
                continue;
            struct pw_proxy* link = graph.createLink(target.sourceNodeId, sourcePorts[c], nodeId, ourPort);
            if (link) {
                links.push_back(link);
                inLinks++;
            }
        }

        // Settle the new links, then let a few cycles run under the card's
        // clock so the rate and quantum we report are post-relink.
        graph.roundtrip();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        INFO("PipeWire: linked \"%s\" to \"%s\" (%d out, %d in)",
             name.c_str(), target.label.c_str(), outLinks, inLinks);
        if (outLinks == 0 && numOutputs > 0)
            WARN("PipeWire: could not link any output to \"%s\"", target.label.c_str());
    }

    // -- Realtime callback ----------------------------------------------------

    void process(struct spa_io_position* position) {
        uint32_t frames = position->clock.duration;
        uint32_t rate = position->clock.rate.denom;

        if (rate > 0)
            actualSampleRate.store((float) rate);
        actualBlockSize.store((int) frames);
        cycleSeen.store(true);

        if (frames > (uint32_t) MAX_FRAMES) {
            // Can't service a quantum this large. Silence rather than smear.
            for (int c = 0; c < numOutputs; c++) {
                float* dst = (float*) lib.filter_get_dsp_buffer(outPorts[c], frames);
                if (dst)
                    memset(dst, 0, frames * sizeof(float));
            }
            if (!overrunLogged) {
                overrunLogged = true;
                WARN("PipeWire: quantum of %u frames exceeds the %d frame limit", frames, MAX_FRAMES);
            }
            return;
        }

        // Planar (one buffer per port) -> interleaved, which is what Rack wants.
        for (int c = 0; c < numInputs; c++) {
            const float* src = (const float*) lib.filter_get_dsp_buffer(inPorts[c], frames);
            if (src) {
                for (uint32_t i = 0; i < frames; i++)
                    inBuf[i * numInputs + c] = src[i];
            }
            else {
                for (uint32_t i = 0; i < frames; i++)
                    inBuf[i * numInputs + c] = 0.f;
            }
        }

        processBuffer(inBuf.data(), numInputs, outBuf.data(), numOutputs, (int) frames);

        for (int c = 0; c < numOutputs; c++) {
            float* dst = (float*) lib.filter_get_dsp_buffer(outPorts[c], frames);
            if (!dst)
                continue;
            for (uint32_t i = 0; i < frames; i++)
                dst[i] = outBuf[i * numOutputs + c];
        }
    }

    static void onProcess(void* data, struct spa_io_position* position) {
        ((Device*) data)->process(position);
    }

    static const struct pw_filter_events& filterEvents() {
        static struct pw_filter_events events;
        static bool init = false;
        if (!init) {
            memset(&events, 0, sizeof(events));
            events.version = PW_VERSION_FILTER_EVENTS;
            events.process = onProcess;
            init = true;
        }
        return events;
    }
};


// ----------------------------------------------------------------------------
// Driver
// ----------------------------------------------------------------------------

struct Driver : rack::audio::Driver {
    std::map<int, Device*> devices;

    std::string getName() override {
        return "PipeWire";
    }

    /** Device IDs 0..2 are the manual-routing nodes; everything above is an
    index into the current sink list. */
    bool getTarget(int deviceId, Target& target) {
        if (deviceId < NUM_MANUAL_DEVICES)
            return false;
        std::vector<Target> targets = graph.getTargets();
        int index = deviceId - NUM_MANUAL_DEVICES;
        if (index < 0 || index >= (int) targets.size())
            return false;
        target = targets[index];
        return true;
    }

    std::vector<int> getDeviceIds() override {
        std::vector<int> ids;
        int count = NUM_MANUAL_DEVICES + (int) graph.getTargets().size();
        for (int i = 0; i < count; i++)
            ids.push_back(i);
        return ids;
    }

    int getDefaultDeviceId() override {
        return 0;
    }

    std::string getDeviceName(int deviceId) override {
        if (deviceId >= 0 && deviceId < NUM_MANUAL_DEVICES)
            return MANUAL_DEVICES[deviceId].name;
        Target target;
        if (getTarget(deviceId, target))
            return target.label;
        return "";
    }

    int getDeviceNumInputs(int deviceId) override {
        if (deviceId >= 0 && deviceId < NUM_MANUAL_DEVICES)
            return MANUAL_DEVICES[deviceId].numChannels;
        Target target;
        if (getTarget(deviceId, target))
            return target.numInputs;
        return 0;
    }

    int getDeviceNumOutputs(int deviceId) override {
        if (deviceId >= 0 && deviceId < NUM_MANUAL_DEVICES)
            return MANUAL_DEVICES[deviceId].numChannels;
        Target target;
        if (getTarget(deviceId, target))
            return target.numOutputs;
        return 0;
    }

    rack::audio::Device* subscribe(int deviceId, rack::audio::Port* port) override {
        std::map<int, Device*>::iterator it = devices.find(deviceId);
        if (it != devices.end()) {
            it->second->subscribe(port);
            return it->second;
        }

        Device* device;
        if (deviceId >= 0 && deviceId < NUM_MANUAL_DEVICES) {
            int channels = MANUAL_DEVICES[deviceId].numChannels;
            // Throws on failure, before the device is registered.
            device = new Device(MANUAL_DEVICES[deviceId].name, channels, channels, Target());
        }
        else {
            Target target;
            if (!getTarget(deviceId, target))
                throw rack::Exception("No PipeWire device with ID %d", deviceId);
            device = new Device(target.label, target.numInputs, target.numOutputs, target);
        }

        devices[deviceId] = device;
        device->subscribe(port);
        return device;
    }

    void unsubscribe(int deviceId, rack::audio::Port* port) override {
        std::map<int, Device*>::iterator it = devices.find(deviceId);
        if (it == devices.end())
            return;
        Device* device = it->second;
        device->unsubscribe(port);

        if (device->subscribed.empty()) {
            devices.erase(it);
            delete device;
        }
    }
};


} // namespace pipewire
} // namespace uzz

#endif // ARCH_LIN && HAVE_PIPEWIRE


void pipewireAudioInit() {
#if defined(ARCH_LIN) && defined(HAVE_PIPEWIRE)
    using namespace uzz::pipewire;

    if (!loadLibrary())
        return;

    lib.init(NULL, NULL);

    // No point offering the driver if there is no server to talk to.
    if (!graph.start()) {
        INFO("PipeWire: could not connect to a PipeWire server, driver disabled");
        return;
    }

    INFO("PipeWire: using libpipewire %s, registering audio driver", lib.get_library_version());
    rack::audio::addDriver(DRIVER_ID, new Driver);
#endif
}
