// macOS-only glue for the app window (no-ops elsewhere).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spapp {

// The editor's WKWebView claims file drags for itself (and would navigate to
// a dropped file). Unregistering it hands Finder drags to the JUCE window
// around it, where MainComponent receives them with their full paths.
// WebKit can register again (e.g. after a reload), so call this regularly.
void passFileDragsToWindow(juce::Component& windowContent);

}  // namespace spapp
