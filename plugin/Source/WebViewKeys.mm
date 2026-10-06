#include "WebViewKeys.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <initializer_list>

namespace spplug {

namespace {

std::atomic<bool> textEditing{false};

// macOS virtual key codes (HIToolbox Events.h): Space, Return, keypad Enter,
// comma, period, Home, End.
bool isTransportKey(NSEvent* e) {
    switch ([e keyCode]) {
        case 49: case 36: case 76: case 43: case 47: case 115: case 119: return true;
        default: return false;
    }
}

bool passToHost(NSEvent* e) { return isTransportKey(e) && !textEditing.load(); }

// ~/Library/Logs/Spatial Panner/keys.log: every key the web view receives
// and where it went, for bug reports (Logic cannot be run where the plug-in
// is developed). One line per key event, with the wall clock and, for keys
// handed to Logic, the call path that delivered the event.
void logKey(const char* what, NSEvent* e, bool withStack = false) {
    static FILE* f = [] {
        NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Spatial Panner"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        return fopen([[dir stringByAppendingPathComponent:@"keys.log"] fileSystemRepresentation], "a");
    }();
    if (!f) return;
    fprintf(f, "%.3f at=%.3f %s type=%s code=%d repeat=%d editing=%d\n", [e timestamp], CACurrentMediaTime(), what,
            [e type] == NSEventTypeKeyDown ? "down" : "up", (int) [e keyCode], (int) [e isARepeat], (int) textEditing.load());
    if (withStack) {
        NSArray<NSString*>* frames = [NSThread callStackSymbols];
        const NSUInteger n = std::min<NSUInteger>([frames count], 18);
        for (NSUInteger i = 2; i < n; ++i) fprintf(f, "    %s\n", [frames[i] UTF8String]);
    }
    fflush(f);
}

// The same key event is handed to keyDown: twice on the way from Logic's
// window to this view (keys.log showed every Space arriving twice, Return
// once), and Logic toggles play/stop on each copy it gets back. Only the
// first copy of an event goes up; the second is swallowed.
bool seenBefore(NSEvent* e) {
    static NSTimeInterval lastTime = -1;
    static NSEventType lastType = NSEventTypeKeyDown;
    static unsigned short lastCode = 0;
    const bool same = [e timestamp] == lastTime && [e type] == lastType && [e keyCode] == lastCode;
    lastTime = [e timestamp];
    lastType = [e type];
    lastCode = [e keyCode];
    return same;
}

using KeyFn = void (*)(struct objc_super*, SEL, NSEvent*);

// NSView's own keyDown:/keyUp: (NSResponder's: hand the event to the next
// responder), skipping WKWebView's, which would take the key.
void callNSView(id self, SEL sel, NSEvent* e) {
    struct objc_super sup { self, [NSView class] };
    reinterpret_cast<KeyFn>(objc_msgSendSuper)(&sup, sel, e);
}

// WKWebView's version, for every other key.
void callWebView(Class cls, id self, SEL sel, NSEvent* e) {
    struct objc_super sup { self, class_getSuperclass(cls) };
    reinterpret_cast<KeyFn>(objc_msgSendSuper)(&sup, sel, e);
}

void patchClass(Class cls) {
    static NSMutableSet<Class>* patched = [NSMutableSet new];
    if ([patched containsObject:cls]) return;
    [patched addObject:cls];
    for (SEL sel : {@selector(keyDown:), @selector(keyUp:)}) {
        id block = ^void (id self, NSEvent* e) {
            if (passToHost(e)) {
                // A held key repeats after macOS's repeat delay. Logic toggles
                // play/stop on every Space it is handed, and a key handed back
                // from a plug-in window skips its own repeat filter, so a
                // slightly long press played and then stopped. Repeats are
                // dropped here.
                if ([e isARepeat]) { logKey("dropped repeat", e); return; }
                if (seenBefore(e)) { logKey("dropped duplicate", e, true); return; }
                logKey("to Logic", e, true);
                callNSView(self, sel, e);
            } else {
                logKey("to page", e);
                callWebView(cls, self, sel, e);
            }
        };
        class_addMethod(cls, sel, imp_implementationWithBlock(block), "v@:@");
    }
}

void patchWebViews(NSView* v) {
    if ([v isKindOfClass:[WKWebView class]]) {
        // JUCE makes its own subclass of WKWebView for its web view; the
        // methods go on that class, so WKWebView itself is untouched.
        Class cls = object_getClass(v);
        if (cls != [WKWebView class]) patchClass(cls);
    }
    for (NSView* s in [v subviews]) patchWebViews(s);
}

}  // namespace

void passTransportKeysToHost(juce::Component& c) {
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle()) patchWebViews(view);
}

void setTextEditing(bool editing) { textEditing = editing; }

}  // namespace spplug
