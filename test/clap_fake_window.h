// A fake SoLoud::PluginHostWindow for the native CLAP host harness
// (test/clap_host_test.cpp): records what the CLAP editor asks of its window
// and lets a test act as the user. Main thread only, like the real window.
#pragma once

#include "../src/plugin_ui/plugin_host_window.h"

#include <cstdint>
#include <string>

struct FakeWindowState
{
    // --- what the editor did (the most recently created window) ----------
    int creates = 0;
    int opens = 0;
    int closes = 0;
    int shows = 0;
    int hides = 0;
    int setContentSizes = 0;
    int setNativeLimitsCalls = 0;
    int acceptedUserSizes = 0;
    /// Net retainNativeView() calls on the content view: it outlives a
    /// closed window while above 0.
    int contentViewRetains = 0;
    bool open = false;
    bool visible = false;
    bool listenerDetached = false;
    std::string title;
    SoLoud::PluginWindowResize mode = SoLoud::PluginWindowResize::Fixed;
    /// The base size (open() or setContentSize()).
    SoLoud::PluginWindowSize base{0, 0};
    /// The content size on screen (base, or the last user resize).
    SoLoud::PluginWindowSize current{0, 0};
    SoLoud::PluginWindowNativeLimits limits;
    bool hasLimits = false;
    SoLoud::PluginHostWindowListener *listener = nullptr;

    // --- knobs -----------------------------------------------------------
    /// The next open() fails.
    bool failOpen = false;
    /// contentView() returns null.
    bool nullContentView = false;
    /// applicationWindowView() returns null.
    bool noApplicationWindow = false;
};

/// The shared state (reset with resetFakeWindow()).
FakeWindowState &fakeWindow();
void resetFakeWindow();

/// The user closes the window: the listener's onWindowClosing, then the
/// window tears itself down. False when no window is open.
bool fakeUserClose();

/// The user drags the open Native window to [size]: constrainUserResize,
/// then onUserResized with the result. False when not open or not Native.
bool fakeUserResize(SoLoud::PluginWindowSize size);

/// The application is terminating: the listener's
/// onApplicationWillTerminate. False when no window is open.
bool fakeAppTerminate();

/// The view applicationWindowView() hands out.
void *fakeApplicationView();

/// Whether the content view the editor parented the gui to still exists:
/// its window is open, or it was retained.
bool fakeContentViewAlive();

/// The content min and max sizes the real window would give AppKit for the
/// current base size and native limits (plugin_host_window_macos.mm).
struct FakeContentLimits
{
    SoLoud::PluginWindowSize minSize;
    SoLoud::PluginWindowSize maxSize;
};
FakeContentLimits fakeContentLimits();
