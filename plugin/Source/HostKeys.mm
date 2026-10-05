#include "HostKeys.h"

#import <AppKit/AppKit.h>

#include <utility>

namespace spplug {

namespace {

// macOS virtual key codes (HIToolbox Events.h) for the keys the editor passes on.
unsigned short keyCodeFor(const std::string& code) {
    static const std::pair<const char*, unsigned short> table[] = {
        {"Space", 49}, {"Enter", 36}, {"NumpadEnter", 76}, {"Comma", 43}, {"Period", 47},
        {"Home", 115}, {"End", 119}, {"Escape", 53}, {"Backspace", 51}, {"Delete", 117},
    };
    for (const auto& [name, kc] : table)
        if (code == name) return kc;
    return 0;
}

std::string charsFor(const HostKey& k) {
    if (k.key == "Enter") return "\r";
    if (k.key == " " || k.key == "," || k.key == "." || k.key == "<" || k.key == ">") return k.key;
    if (k.key.size() == 1) return k.key;
    return {};
}

NSWindow* hostWindow(NSWindow* own) {
    NSWindow* main = [NSApp mainWindow];
    if (main != nil && main != own) return main;
    for (NSWindow* w in [NSApp orderedWindows])
        if (w != own && [w isVisible] && [w canBecomeMainWindow] && ![w isKindOfClass:[NSPanel class]]) return w;
    return nil;
}

}  // namespace

bool sendKeyToHost(juce::Component& c, const HostKey& k, std::string& error) {
    auto* peer = c.getPeer();
    NSView* view = peer ? (NSView*) peer->getNativeHandle() : nil;
    NSWindow* own = [view window];
    NSWindow* target = hostWindow(own);
    if (target == nil) {
        error = "Logic's window is out of reach from the plug-in window (it may be running the plug-in in a separate process).";
        return false;
    }
    NSEventModifierFlags flags = 0;
    if (k.shift) flags |= NSEventModifierFlagShift;
    if (k.alt) flags |= NSEventModifierFlagOption;
    if (k.ctrl) flags |= NSEventModifierFlagControl;
    if (k.meta) flags |= NSEventModifierFlagCommand;
    const std::string chars = charsFor(k);
    NSString* s = [NSString stringWithUTF8String:chars.c_str()];
    const NSTimeInterval ts = [[NSProcessInfo processInfo] systemUptime];
    const unsigned short kc = keyCodeFor(k.code);
    NSEvent* down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:ts
                                 windowNumber:[target windowNumber] context:nil characters:s charactersIgnoringModifiers:s
                                    isARepeat:NO keyCode:kc];
    NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:flags timestamp:ts + 0.01
                               windowNumber:[target windowNumber] context:nil characters:s charactersIgnoringModifiers:s
                                  isARepeat:NO keyCode:kc];
    if (down == nil || up == nil) { error = "Could not make the key event."; return false; }
    // As if the user had clicked into Logic's window and pressed the key:
    // Logic's project window takes the key, then the plug-in window gets key
    // status back so the editor keeps the keyboard.
    const bool wasKey = own != nil && [own isKeyWindow];
    [target makeKeyWindow];
    [NSApp sendEvent:down];
    [NSApp sendEvent:up];
    if (wasKey) [own makeKeyWindow];
    return true;
}

}  // namespace spplug
