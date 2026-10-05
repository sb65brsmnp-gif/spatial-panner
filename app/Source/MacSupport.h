// macOS-only glue for the app window (no-ops elsewhere).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spapp {

// The editor's WKWebView takes every drag that lands on it (its drag hit
// test answers for any point in its frame, whatever types it registered),
// and the page would only see dropped files without their paths. This hands
// file drags to the JUCE window around it, where MainComponent receives them
// with their full paths. Call it once the web view exists; calling again is
// free, so the window's timer repeats it in case the view is recreated.
void passFileDragsToWindow(juce::Component& windowContent);

}  // namespace spapp
