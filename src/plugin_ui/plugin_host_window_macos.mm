#if defined(SOLOUD_ENABLE_PLUGIN_UI) && defined(__APPLE__)

#include <TargetConditionals.h>

#if !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "plugin_host_window.h"

#include <algorithm>
#include <cmath>

// Builds with and without ARC: the plugin library is not ARC, the VST3
// out-of-process helper is.

namespace SoLoud {
class PluginHostWindowMacOS;
}

@interface SoLoudPluginWindowDelegate : NSObject <NSWindowDelegate> {
  SoLoud::PluginHostWindowMacOS *_owner;
}
- (instancetype)initWithOwner:(SoLoud::PluginHostWindowMacOS *)owner;
- (void)clearOwner;
@end

// Optional selectors of Steinberg's and similar Cocoa plugin views. Sent only
// when the view responds to them.
@interface NSObject (SoLoudPluginCocoaViewSupport)
- (BOOL)isPathBrowserViewRenderView;
- (void)setIsPathBrowserViewRenderView:(BOOL)val;
- (void)windowOcclusionStateChanged:(BOOL)occluded;
@end

@interface SoLoudPluginWindow : NSWindow
@property(nonatomic, assign) NSSize baseContentSize;
@property(nonatomic, assign) BOOL supportsHostScaling;
- (void)zoomByFactor:(CGFloat)factor;
- (void)resetZoom;
@end

@implementation SoLoudPluginWindow
- (BOOL)performKeyEquivalent:(NSEvent *)event {
  if (self.supportsHostScaling &&
      ([event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask) ==
      NSEventModifierFlagCommand) {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars isEqualToString:@"+"] || [chars isEqualToString:@"="]) {
      [self zoomByFactor:1.25];
      return YES;
    } else if ([chars isEqualToString:@"-"] || [chars isEqualToString:@"_"]) {
      [self zoomByFactor:0.8];
      return YES;
    } else if ([chars isEqualToString:@"0"]) {
      [self resetZoom];
      return YES;
    }
  }
  return [super performKeyEquivalent:event];
}

- (void)zoomByFactor:(CGFloat)factor {
  NSRect curContentRect = [self contentRectForFrameRect:[self frame]];
  CGFloat newW = std::round(curContentRect.size.width * factor);
  CGFloat newH = std::round(curContentRect.size.height * factor);
  if (self.baseContentSize.width > 0 && self.baseContentSize.height > 0) {
    newH = std::round(newW * (self.baseContentSize.height / self.baseContentSize.width));
  }

  NSSize minSize = [self contentMinSize];
  NSSize maxSize = [self contentMaxSize];
  if (minSize.width > 0 && newW < minSize.width) {
    newW = minSize.width;
    if (self.baseContentSize.width > 0) {
      newH = std::round(newW * (self.baseContentSize.height / self.baseContentSize.width));
    }
  }
  if (minSize.height > 0 && newH < minSize.height) {
    newH = minSize.height;
    if (self.baseContentSize.height > 0) {
      newW = std::round(newH * (self.baseContentSize.width / self.baseContentSize.height));
    }
  }
  if (maxSize.width > 0 && newW > maxSize.width) {
    newW = maxSize.width;
    if (self.baseContentSize.width > 0) {
      newH = std::round(newW * (self.baseContentSize.height / self.baseContentSize.width));
    }
  }
  if (maxSize.height > 0 && newH > maxSize.height) {
    newH = maxSize.height;
    if (self.baseContentSize.height > 0) {
      newW = std::round(newH * (self.baseContentSize.width / self.baseContentSize.height));
    }
  }

  if (std::abs(newW - curContentRect.size.width) < 1.0) return;

  [self applyContentSize:NSMakeSize(newW, newH) animate:YES];
}

- (void)applyContentSize:(NSSize)newSize animate:(BOOL)animate {
  NSRect curContentRect = [self contentRectForFrameRect:[self frame]];
  NSRect newContentRect = NSMakeRect(curContentRect.origin.x,
                                     curContentRect.origin.y - (newSize.height - curContentRect.size.height),
                                     newSize.width, newSize.height);
  NSRect newFrame = [self frameRectForContentRect:newContentRect];
  NSScreen *screen = [self screen];
  if (screen != nil) {
    NSRect screenRect = [screen visibleFrame];
    CGFloat top = [self frame].origin.y + [self frame].size.height;
    newFrame.origin.y = top - newFrame.size.height;
    if (newFrame.origin.y < screenRect.origin.y) {
      newFrame.origin.y = screenRect.origin.y;
    }
    if (NSMaxX(newFrame) > NSMaxX(screenRect)) {
      newFrame.origin.x = NSMaxX(screenRect) - newFrame.size.width;
    }
    if (newFrame.origin.x < screenRect.origin.x) {
      newFrame.origin.x = screenRect.origin.x;
    }
  }
  [self setFrame:newFrame display:YES animate:animate];
}

- (void)resetZoom {
  if (self.baseContentSize.width <= 0 || self.baseContentSize.height <= 0) return;
  [self applyContentSize:self.baseContentSize animate:YES];
}
@end

namespace SoLoud {

namespace {

void ensurePluginSubviewsVisible(NSView *view) {
  if (view == nil) {
    return;
  }
  SEL setPathBrowserSel = @selector(setIsPathBrowserViewRenderView:);
  if ([view respondsToSelector:setPathBrowserSel]) {
    [view setIsPathBrowserViewRenderView:YES];
  }
  SEL occlusionSel = @selector(windowOcclusionStateChanged:);
  if ([view respondsToSelector:occlusionSel]) {
    [view windowOcclusionStateChanged:NO];
  }
  [view setNeedsDisplay:YES];
  CALayer *layer = [view layer];
  if (layer != nil) {
    [layer setNeedsDisplay];
  }
  for (NSView *subview in [view subviews]) {
    ensurePluginSubviewsVisible(subview);
  }
}

NSView *firstSubviewOf(NSView *view) {
  return (view != nil && [[view subviews] count] > 0)
             ? [[view subviews] firstObject]
             : nil;
}

// Drops the caller's reference on the next main-queue turn rather than now,
// so an object whose method is still on the stack (a delegate tearing down
// its own window, a notification being delivered) outlives that call.
void releaseOnNextMainTurn(id obj) {
  if (obj == nil) {
    return;
  }
#if __has_feature(objc_arc)
  dispatch_async(dispatch_get_main_queue(), ^{
    (void)obj;
  });
#else
  dispatch_async(dispatch_get_main_queue(), ^{
    [obj release];
  });
#endif
}

} // namespace

void runOnMainThreadSync(const std::function<void()> &fn) {
  if ([NSThread isMainThread]) {
    fn();
  } else {
    dispatch_sync(dispatch_get_main_queue(), ^{
      fn();
    });
  }
}

class PluginHostWindowMacOS final : public PluginHostWindow {
public:
  PluginHostWindowMacOS() = default;
  ~PluginHostWindowMacOS() override { close(); }

  bool open(const std::string &title, PluginWindowSize size,
            PluginWindowResize mode,
            PluginHostWindowListener *listener) override {
    close();
    ++mGeneration;
    mBase = size;
    mMode = mode;

    const bool resizable = mode != PluginWindowResize::Fixed;
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable;
    if (resizable) {
      style |= NSWindowStyleMaskResizable;
    }
    mWindow = [[SoLoudPluginWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                  styleMask:style
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (mWindow == nil) {
      return false;
    }
    mWindow.baseContentSize = NSMakeSize(size.width, size.height);
    mWindow.supportsHostScaling = NO;
    [mWindow setReleasedWhenClosed:NO];
    [mWindow setHidesOnDeactivate:NO];
    if (resizable) {
      setScalableLimits();
    } else {
      setLockedLimits();
    }
    NSString *nsTitle = [NSString stringWithUTF8String:title.c_str()];
    [mWindow setTitle:(nsTitle != nil ? nsTitle : @"Plugin")];
    [mWindow center];

    mListener = listener;
    mDelegate = [[SoLoudPluginWindowDelegate alloc] initWithOwner:this];
    [mWindow setDelegate:mDelegate];
    [[NSNotificationCenter defaultCenter]
        addObserver:mDelegate
           selector:@selector(applicationWillTerminate:)
               name:NSApplicationWillTerminateNotification
             object:nil];
    return true;
  }

  void *contentView() const override {
    if (mWindow == nil) {
      return nullptr;
    }
#if __has_feature(objc_arc)
    return (__bridge void *)[mWindow contentView];
#else
    return reinterpret_cast<void *>([mWindow contentView]);
#endif
  }

  void *firstPluginSubview() const override {
    if (mWindow == nil) {
      return nullptr;
    }
    NSView *view = firstSubviewOf([mWindow contentView]);
#if __has_feature(objc_arc)
    return (__bridge void *)view;
#else
    return reinterpret_cast<void *>(view);
#endif
  }

  void setResizeMode(PluginWindowResize mode) override {
    if (mWindow == nil) {
      return;
    }
    mMode = mode;
    NSView *contentView = [mWindow contentView];
    NSView *pluginView = firstSubviewOf(contentView);
    const CGFloat w = mBase.width;
    const CGFloat h = mBase.height;
    switch (mode) {
    case PluginWindowResize::HostScaled:
      mWindow.supportsHostScaling = YES;
      [mWindow setStyleMask:([mWindow styleMask] | NSWindowStyleMaskResizable)];
      [mWindow setContentAspectRatio:NSMakeSize(w, h)];
      setScalableLimits();
      [contentView setFrameSize:NSMakeSize(w, h)];
      [contentView setBoundsSize:NSMakeSize(w, h)];
      break;
    case PluginWindowResize::Fixed:
      mWindow.supportsHostScaling = NO;
      [mWindow setStyleMask:([mWindow styleMask] & ~NSWindowStyleMaskResizable)];
      [mWindow setContentAspectRatio:NSMakeSize(0, 0)];
      setLockedLimits();
      [contentView setFrame:NSMakeRect(0, 0, w, h)];
      [contentView setBounds:NSMakeRect(0, 0, w, h)];
      break;
    case PluginWindowResize::Native:
      mWindow.supportsHostScaling = NO;
      [contentView setFrame:NSMakeRect(0, 0, w, h)];
      [contentView setBounds:NSMakeRect(0, 0, w, h)];
      break;
    }
    if (pluginView != nil) {
      [pluginView setFrame:NSMakeRect(0, 0, w, h)];
    }
  }

  PluginWindowResize resizeMode() const override { return mMode; }

  void setContentSize(PluginWindowSize size) override {
    if (mWindow == nil) {
      return;
    }
    mBase = size;
    const CGFloat w = size.width;
    const CGFloat h = size.height;
    mWindow.baseContentSize = NSMakeSize(w, h);
    if (mMode == PluginWindowResize::HostScaled) {
      [mWindow setContentAspectRatio:NSMakeSize(w, h)];
      setScalableLimits();
    } else if (mMode == PluginWindowResize::Fixed) {
      setLockedLimits();
    }
    NSRect contentRect = [mWindow contentRectForFrameRect:mWindow.frame];
    const CGFloat diffY = h - contentRect.size.height;
    contentRect.size.width = w;
    contentRect.size.height = h;
    contentRect.origin.y -= diffY;
    NSRect frameRect = [mWindow frameRectForContentRect:contentRect];
    mApplyingContentSize = true;
    [mWindow setFrame:frameRect display:[mWindow isVisible] animate:NO];
    mApplyingContentSize = false;
    NSView *contentView = [mWindow contentView];
    if (contentView != nil) {
      if (mMode == PluginWindowResize::HostScaled) {
        [contentView setBoundsSize:NSMakeSize(w, h)];
      } else {
        [contentView setFrame:NSMakeRect(0, 0, w, h)];
        [contentView setBounds:NSMakeRect(0, 0, w, h)];
      }
      NSView *pluginView = firstSubviewOf(contentView);
      if (pluginView != nil) {
        [pluginView setFrame:NSMakeRect(0, 0, w, h)];
      }
    }
  }

  PluginWindowSize baseSize() const override { return mBase; }

  void show() override {
    if (mWindow == nil) {
      return;
    }
    NSView *contentView = [mWindow contentView];
    NSView *pluginView = firstSubviewOf(contentView);
    ensurePluginSubviewsVisible(contentView);
    [mWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (pluginView != nil) {
      [mWindow makeFirstResponder:pluginView];
    }
    [[NSNotificationCenter defaultCenter]
        postNotificationName:NSWindowDidBecomeKeyNotification
                      object:mWindow];
    [[NSNotificationCenter defaultCenter]
        postNotificationName:NSWindowDidBecomeMainNotification
                      object:mWindow];
    [mWindow displayIfNeeded];
  }

  void focusPluginSubviewIfUnfocused() override {
    if (mWindow == nil) {
      return;
    }
    NSView *pluginView = firstSubviewOf([mWindow contentView]);
    if (pluginView != nil && ([mWindow firstResponder] == nil ||
                              [mWindow firstResponder] == mWindow)) {
      [mWindow makeFirstResponder:pluginView];
    }
  }

  void refreshContent() override {
    if (mWindow == nil) {
      return;
    }
    ensurePluginSubviewsVisible([mWindow contentView]);
    [mWindow displayIfNeeded];
  }

  void detachListener() override { stopCallbacks(); }

  void close() override {
    if (mTearingDown) {
      return;
    }
    stopCallbacks();
    if (mWindow != nil) {
      SoLoudPluginWindow *window = mWindow;
      mWindow = nil;
      [window setDelegate:nil];
      [window close];
      releaseOnNextMainTurn(window);
    }
    releaseDelegate();
  }

  bool isOpen() const override { return mWindow != nil; }

  uint64_t generation() const override { return mGeneration; }

  // Delegate callbacks.

  bool onShouldClose() {
    if (mTearingDown || mWindow == nil) {
      return true;
    }
    mTearingDown = true;
    PluginHostWindowListener *listener = mListener;
    stopCallbacks();
    if (listener != nullptr) {
      listener->onWindowClosing();
    }
    // AppKit closes the window once this returns YES.
    SoLoudPluginWindow *window = mWindow;
    mWindow = nil;
    [window setDelegate:nil];
    releaseOnNextMainTurn(window);
    releaseDelegate();
    mTearingDown = false;
    return true;
  }

  void onApplicationWillTerminate() {
    if (mListener != nullptr && !mTearingDown) {
      mListener->onApplicationWillTerminate();
    }
  }

  NSSize onWillResize(NSSize frameSize) {
    if (mListener == nullptr || mWindow == nil) {
      return frameSize;
    }
    switch (mMode) {
    case PluginWindowResize::Fixed: {
      NSRect contentRect = NSMakeRect(0, 0, mBase.width, mBase.height);
      return [mWindow frameRectForContentRect:contentRect].size;
    }
    case PluginWindowResize::HostScaled:
      return frameSize;
    case PluginWindowResize::Native:
      break;
    }
    NSRect frameRect = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    NSRect contentRect = [mWindow contentRectForFrameRect:frameRect];
    PluginWindowSize size{static_cast<int32_t>(contentRect.size.width),
                          static_cast<int32_t>(contentRect.size.height)};
    if (!mListener->constrainUserResize(size)) {
      return frameSize;
    }
    contentRect.size.width = size.width;
    contentRect.size.height = size.height;
    return [mWindow frameRectForContentRect:contentRect].size;
  }

  void onDidResize() {
    if (mApplyingContentSize || mListener == nullptr || mWindow == nil) {
      return;
    }
    NSView *contentView = [mWindow contentView];
    if (contentView == nil) {
      return;
    }
    if (mMode == PluginWindowResize::HostScaled) {
      [contentView setBoundsSize:NSMakeSize(mBase.width, mBase.height)];
      NSView *pluginView = firstSubviewOf(contentView);
      if (pluginView != nil) {
        [pluginView setFrame:NSMakeRect(0, 0, mBase.width, mBase.height)];
      }
    } else if (mMode == PluginWindowResize::Native) {
      const NSRect bounds = [contentView bounds];
      mListener->onUserResized({static_cast<int32_t>(bounds.size.width),
                                static_cast<int32_t>(bounds.size.height)});
    }
    ensurePluginSubviewsVisible(contentView);
    [mWindow displayIfNeeded];
  }

  void onOcclusionChanged() {
    if (mWindow == nil) {
      return;
    }
    if (([mWindow occlusionState] & NSWindowOcclusionStateVisible) != 0) {
      ensurePluginSubviewsVisible([mWindow contentView]);
    }
  }

  void onDidBecomeKey() {
    if (mWindow == nil) {
      return;
    }
    NSView *contentView = [mWindow contentView];
    if (contentView != nil) {
      NSView *pluginView = firstSubviewOf(contentView);
      if (pluginView != nil && [mWindow firstResponder] != pluginView) {
        [mWindow makeFirstResponder:pluginView];
      }
      ensurePluginSubviewsVisible(contentView);
      [mWindow displayIfNeeded];
    }
  }

private:
  // Native mode at open, and HostScaled: half to four times the base size.
  void setScalableLimits() {
    [mWindow setContentMinSize:NSMakeSize(std::max<int>(100, mBase.width / 2),
                                          std::max<int>(100, mBase.height / 2))];
    [mWindow setContentMaxSize:NSMakeSize(mBase.width * 4, mBase.height * 4)];
  }

  void setLockedLimits() {
    [mWindow setContentMinSize:NSMakeSize(mBase.width, mBase.height)];
    [mWindow setContentMaxSize:NSMakeSize(mBase.width, mBase.height)];
  }

  // Stops listener calls for this open: unregisters the termination observer
  // and detaches the delegate from this object.
  void stopCallbacks() {
    mListener = nullptr;
    if (mDelegate != nil) {
      [[NSNotificationCenter defaultCenter] removeObserver:mDelegate];
      [mDelegate clearOwner];
    }
  }

  void releaseDelegate() {
    if (mDelegate != nil) {
      SoLoudPluginWindowDelegate *delegate = mDelegate;
      mDelegate = nil;
      releaseOnNextMainTurn(delegate);
    }
  }

  SoLoudPluginWindow *mWindow = nil;
  SoLoudPluginWindowDelegate *mDelegate = nil;
  PluginHostWindowListener *mListener = nullptr;
  PluginWindowSize mBase{800, 600};
  PluginWindowResize mMode = PluginWindowResize::Fixed;
  uint64_t mGeneration = 0;
  bool mApplyingContentSize = false;
  bool mTearingDown = false;
};

std::unique_ptr<PluginHostWindow> PluginHostWindow::create() {
  return std::make_unique<PluginHostWindowMacOS>();
}

} // namespace SoLoud

@implementation SoLoudPluginWindowDelegate
- (instancetype)initWithOwner:(SoLoud::PluginHostWindowMacOS *)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}

- (void)clearOwner {
  _owner = nullptr;
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
  (void)notification;
  if (_owner != nullptr) {
    _owner->onDidBecomeKey();
  }
}

- (void)windowDidBecomeMain:(NSNotification *)notification {
  (void)notification;
  if (_owner != nullptr) {
    _owner->onDidBecomeKey();
  }
}

- (void)applicationWillTerminate:(NSNotification *)notification {
  (void)notification;
  if (_owner != nullptr) {
    _owner->onApplicationWillTerminate();
  }
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
  (void)sender;
  if (_owner != nullptr) {
    return _owner->onShouldClose() ? YES : NO;
  }
  return YES;
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
  (void)sender;
  if (_owner != nullptr) {
    return _owner->onWillResize(frameSize);
  }
  return frameSize;
}

- (void)windowDidResize:(NSNotification *)notification {
  (void)notification;
  if (_owner != nullptr) {
    _owner->onDidResize();
  }
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification {
  (void)notification;
  if (_owner != nullptr) {
    _owner->onOcclusionChanged();
  }
}
@end

#endif // !TARGET_OS_IPHONE
#endif // SOLOUD_ENABLE_PLUGIN_UI && __APPLE__
