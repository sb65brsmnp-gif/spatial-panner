// Pressing a key in the host's own window (macOS). Logic runs its key
// commands only for keys that reach its project window; a key the web view
// leaves alone dies in the plug-in window. So the editor hands the keys
// Logic should get (Space, Return, comma, period) to this, which makes
// Logic's window key for a moment, sends the key there, and hands key status
// back to the plug-in window.
#pragma once

#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

namespace spplug {

struct HostKey {
    std::string key;   // KeyboardEvent.key
    std::string code;  // KeyboardEvent.code
    bool shift = false, alt = false, ctrl = false, meta = false;
};

// `c` is any component in the plug-in window. False with `error` set when no
// host window could be found (the plug-in runs in its own process, or there
// is no other window).
bool sendKeyToHost(juce::Component& c, const HostKey& k, std::string& error);

}  // namespace spplug
