#ifndef SOLOUD_PLUGIN_HOST_WINDOW_H
#define SOLOUD_PLUGIN_HOST_WINDOW_H

// Format-agnostic native window that hosts a plugin editor view (VST3
// IPlugView, CLAP gui) on macOS. It owns the NSWindow, its content view,
// sizing and the close notification; attaching the plugin's view and talking
// to the plugin stay in the format adapter (see vst3_editor_macos.mm).
//
// Plain C++ so format code compiled as C++ can include it. Every method is
// main-thread only; use runOnMainThreadSync() from other threads.

#if defined(SOLOUD_ENABLE_PLUGIN_UI) && defined(__APPLE__)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace SoLoud {

/// How the window treats size changes.
enum class PluginWindowResize {
  /// The content size is locked to the plugin's size.
  Fixed,
  /// The user can resize; the plugin constrains and follows the size
  /// (min max(100, size/2), max 4x the size at open, unless
  /// setNativeLimits() replaced the limits).
  Native,
  /// The plugin view keeps its size and the host scales the content view's
  /// bounds, keeping the aspect ratio (Cmd +/-/0 zoom, 0.5x-4x).
  HostScaled,
};

struct PluginWindowSize {
  int32_t width = 0;
  int32_t height = 0;
};

/// Native-mode limits set with PluginHostWindow::setNativeLimits().
struct PluginWindowNativeLimits {
  PluginWindowSize minSize{1, 1};
  PluginWindowSize maxSize{16384, 16384};
  /// A locked axis keeps the base size's extent on that axis.
  bool resizeHorizontally = true;
  bool resizeVertically = true;
  /// A width:height ratio a user resize keeps; 0 for none.
  uint32_t aspectWidth = 0;
  uint32_t aspectHeight = 0;
};

/// Callbacks from the window to the format adapter, on the main thread.
class PluginHostWindowListener {
public:
  virtual ~PluginHostWindowListener() = default;

  /// The user closed the window. Detach the plugin view here; the content
  /// view stays alive until this returns. The window then tears itself down
  /// and makes no further calls for this open. A nested close() is a no-op.
  virtual void onWindowClosing() = 0;

  /// NSApplicationWillTerminate arrived while the window was open.
  virtual void onApplicationWillTerminate() = 0;

  /// Native mode: the user is dragging to content [size]. Returns true with
  /// [size] changed to the size the plugin accepts, or false to leave the
  /// drag as it is.
  virtual bool constrainUserResize(PluginWindowSize &size) {
    (void)size;
    return false;
  }

  /// Native mode: the user resized the content to [size]. Not called for
  /// setContentSize().
  virtual void onUserResized(PluginWindowSize size) { (void)size; }
};

class PluginHostWindow {
public:
  /// Creates the platform implementation. The object lives as long as its
  /// owner; each open() creates a new native window.
  static std::unique_ptr<PluginHostWindow> create();

  virtual ~PluginHostWindow() = default;

  /// Creates and centres a hidden window with content [size] (the plugin's
  /// base size). [listener] must outlive the open window.
  virtual bool open(const std::string &title, PluginWindowSize size,
                    PluginWindowResize mode,
                    PluginHostWindowListener *listener) = 0;

  /// The content view (a borrowed NSView *) to attach the plugin view to,
  /// or null when closed.
  virtual void *contentView() const = 0;

  /// The first subview of the content view (the plugin's view, a borrowed
  /// NSView *), or null.
  virtual void *firstPluginSubview() const = 0;

  /// Changes the resize mode of the open window, keeping the base size.
  virtual void setResizeMode(PluginWindowResize mode) = 0;

  virtual PluginWindowResize resizeMode() const = 0;

  /// Applies a plugin-initiated content size: the window keeps its top edge,
  /// [size] becomes the base size, and Fixed/HostScaled limits follow it.
  virtual void setContentSize(PluginWindowSize size) = 0;

  /// The plugin's base content size.
  virtual PluginWindowSize baseSize() const = 0;

  /// Native mode: replaces the default limits for the rest of this open.
  /// Locked axes follow the base size from then on.
  virtual void setNativeLimits(const PluginWindowNativeLimits &limits) = 0;

  /// Native mode: a user resize the plugin accepted. [size] becomes the base
  /// size without resizing the window, so limits set with setNativeLimits()
  /// follow it.
  virtual void setAcceptedUserSize(PluginWindowSize size) = 0;

  /// Shows the window, activates the app and focuses the plugin view. Also
  /// reveals a window hidden with hide().
  virtual void show() = 0;

  /// Orders the window out, keeping it, its content view and the plugin
  /// view, with no listener call. show() brings it back.
  virtual void hide() = 0;

  /// Whether the open window is on screen (not hidden or not yet shown).
  virtual bool isVisible() const = 0;

  /// Focuses the plugin view if nothing in the window has focus yet (for
  /// plugin views that mount after show()).
  virtual void focusPluginSubviewIfUnfocused() = 0;

  /// Marks the plugin's views as visible and redraws them.
  virtual void refreshContent() = 0;

  /// Stops listener calls for this open, keeping the window and its content
  /// view up until close(), so the adapter can detach the plugin view first.
  virtual void detachListener() = 0;

  /// Closes the window without listener callbacks.
  virtual void close() = 0;

  virtual bool isOpen() const = 0;

  /// Changes on every open(), so deferred work can check that the window it
  /// was scheduled for is still the open one.
  virtual uint64_t generation() const = 0;
};

/// Runs [fn] on the main thread and waits for it.
void runOnMainThreadSync(const std::function<void()> &fn);

/// Keeps [view] (an NSView *, or null) alive past its window until
/// releaseNativeView(): a plugin's gui can outlive the window it was
/// parented to for a while. Returns [view].
void *retainNativeView(void *view);
void releaseNativeView(void *view);

/// Main thread: the content view (a borrowed NSView *) of the application's
/// main window, or its key window, or null. A floating plugin window stays
/// above that window.
void *applicationWindowView();

} // namespace SoLoud

#endif // SOLOUD_ENABLE_PLUGIN_UI && __APPLE__
#endif // SOLOUD_PLUGIN_HOST_WINDOW_H
