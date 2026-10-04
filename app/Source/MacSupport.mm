#include "MacSupport.h"

#import <AppKit/AppKit.h>

namespace spapp {

static void unregisterWebViews(NSView* v) {
    static Class webView = NSClassFromString(@"WKWebView");
    if (webView != nil && [v isKindOfClass:webView] && [[v registeredDraggedTypes] count] > 0) [v unregisterDraggedTypes];
    for (NSView* s in [v subviews]) unregisterWebViews(s);
}

void passFileDragsToWindow(juce::Component& c) {
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle()) unregisterWebViews(view);
}

}  // namespace spapp
