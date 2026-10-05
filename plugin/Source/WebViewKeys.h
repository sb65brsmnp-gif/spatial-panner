// Logic's transport keys, pressed in the plug-in window (macOS).
//
// Logic runs its key commands for a key that the plug-in's view leaves
// unhandled: the key travels up the view's responder chain, out of the
// plug-in's view (and, when Logic hosts the plug-in in its own process, back
// across to Logic). That is how every plug-in whose window is in front still
// lets Space play and stop. The editor's web view (WKWebView) takes every
// key itself, so Space died in the plug-in window. These overrides, added to
// the web view's class at run time, send the transport keys (Space, Return,
// Enter, comma, period, Home, End) straight up the chain instead, unless the
// page is editing text (setTextEditing), when the key is typed as usual.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spplug {

// `c` is any component in the plug-in window. Call it once the web view
// exists; calling again is free, so the editor's timer repeats it.
void passTransportKeysToHost(juce::Component& c);

// The page has (or no longer has) a text field focused: keys stay in the web view.
void setTextEditing(bool editing);

}  // namespace spplug
