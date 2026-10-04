#include "MacSupport.h"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <cstdio>

namespace spapp {

namespace {

bool dragHasFiles(id<NSDraggingInfo> sender) {
    return [[sender draggingPasteboard] availableTypeFromArray:@[NSPasteboardTypeFileURL]] != nil;
}

// The JUCE window's own NSView (the peer), which handles file drags for the
// components inside it: the first ancestor that takes drops.
NSView* dropView(NSView* v) {
    for (NSView* s = [v superview]; s != nil; s = [s superview])
        if ([s respondsToSelector:@selector(performDragOperation:)]) return s;
    return nil;
}

template <typename R, typename... Args>
R callSuper(Class cls, id self, SEL sel, Args... args) {
    struct objc_super sup { self, class_getSuperclass(cls) };
    using Fn = R (*)(struct objc_super*, SEL, Args...);
    return reinterpret_cast<Fn>(objc_msgSendSuper)(&sup, sel, args...);
}

// WKWebView takes every drag that lands on it, whatever types it registered
// (its drag hit test answers for any point inside its frame), and the web
// page then sees files without their paths. These overrides, added to the
// web view's class at run time, hand file drags to the window instead: the
// drag hit test declines them, and should one still arrive, each step is
// passed to the peer view. Other drags (text, links) keep WebKit's handling.
void patchClass(Class cls) {
    static NSMutableSet<Class>* patched = [NSMutableSet new];
    if ([patched containsObject:cls]) return;
    [patched addObject:cls];

    // - (NSView*)_hitTest:(NSPoint*)point dragTypes:(NSSet<NSString*>*)types
    SEL hitTest = NSSelectorFromString(@"_hitTest:dragTypes:");
    if ([cls instancesRespondToSelector:hitTest]) {
        id block = ^NSView* (id self, NSPoint* point, NSSet<NSString*>* types) {
            if ([types containsObject:NSPasteboardTypeFileURL]) return nil;
            return callSuper<NSView*>(cls, self, hitTest, point, types);
        };
        class_addMethod(cls, hitTest, imp_implementationWithBlock(block), "@@:^{CGPoint=dd}@");
    }

    // The overrides call WKWebView's own version for drags without files.
    Class superCls = class_getSuperclass(cls);
    auto superHas = [superCls](SEL sel) { return [superCls instancesRespondToSelector:sel]; };
    char opEnc[16], boolEnc[16];
    snprintf(opEnc, sizeof(opEnc), "%s@:@", @encode(NSDragOperation));
    snprintf(boolEnc, sizeof(boolEnc), "%s@:@", @encode(BOOL));

    SEL entered = @selector(draggingEntered:), updated = @selector(draggingUpdated:), exited = @selector(draggingExited:);
    SEL prepare = @selector(prepareForDragOperation:), perform = @selector(performDragOperation:), conclude = @selector(concludeDragOperation:);

    auto forwardOp = [cls, superHas, &opEnc](SEL sel) {
        id block = ^NSDragOperation (id self, id<NSDraggingInfo> sender) {
            NSView* target = dragHasFiles(sender) ? dropView(self) : nil;
            if (target && [target respondsToSelector:sel])
                return reinterpret_cast<NSDragOperation (*)(id, SEL, id)>(objc_msgSend)(target, sel, sender);
            return superHas(sel) ? callSuper<NSDragOperation>(cls, self, sel, sender) : NSDragOperationNone;
        };
        class_addMethod(cls, sel, imp_implementationWithBlock(block), opEnc);
    };
    forwardOp(entered);
    forwardOp(updated);

    auto forwardBool = [cls, superHas, &boolEnc](SEL sel) {
        id block = ^BOOL (id self, id<NSDraggingInfo> sender) {
            NSView* target = dragHasFiles(sender) ? dropView(self) : nil;
            if (target && [target respondsToSelector:sel])
                return reinterpret_cast<BOOL (*)(id, SEL, id)>(objc_msgSend)(target, sel, sender);
            return superHas(sel) ? callSuper<BOOL>(cls, self, sel, sender) : NO;
        };
        class_addMethod(cls, sel, imp_implementationWithBlock(block), boolEnc);
    };
    forwardBool(prepare);
    forwardBool(perform);

    auto forwardVoid = [cls, superHas](SEL sel) {
        id block = ^void (id self, id<NSDraggingInfo> sender) {
            NSView* target = dragHasFiles(sender) ? dropView(self) : nil;
            if (target && [target respondsToSelector:sel]) {
                reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(target, sel, sender);
                return;
            }
            if (superHas(sel)) callSuper<void>(cls, self, sel, sender);
        };
        class_addMethod(cls, sel, imp_implementationWithBlock(block), "v@:@");
    };
    forwardVoid(exited);
    forwardVoid(conclude);
}

void patchWebViews(NSView* v) {
    if ([v isKindOfClass:[WKWebView class]]) {
        // JUCE makes its own subclass of WKWebView for its web view; the
        // methods go on that class, so WKWebView itself is untouched.
        Class cls = object_getClass(v);
        if (cls != [WKWebView class]) patchClass(cls);
        if ([[v registeredDraggedTypes] count] > 0) [v unregisterDraggedTypes];
    }
    for (NSView* s in [v subviews]) patchWebViews(s);
}

}  // namespace

void passFileDragsToWindow(juce::Component& c) {
    if (auto* peer = c.getPeer())
        if (auto* view = (NSView*) peer->getNativeHandle()) patchWebViews(view);
}

}  // namespace spapp
