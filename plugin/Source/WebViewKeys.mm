#include "WebViewKeys.h"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <atomic>
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
            if (passToHost(e)) callNSView(self, sel, e);
            else callWebView(cls, self, sel, e);
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
