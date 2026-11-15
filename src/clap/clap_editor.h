#ifndef SOLOUD_CLAP_EDITOR_H
#define SOLOUD_CLAP_EDITOR_H

// The host side of one CLAP instance's gui (plan Phase 4b): the embedded
// cocoa flow in a PluginHostWindow, or the plugin's own floating window.
//
// Main-thread only. The plugin's thread-safe gui callbacks never reach this
// class directly off main: ClapInstance posts them and the pump calls
// service() (see clap_instance.cpp).

#if defined(SOLOUD_ENABLE_CLAP) && defined(SOLOUD_ENABLE_PLUGIN_UI) && \
    defined(__APPLE__)

#include "../plugin_ui/plugin_host_window.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace soloud_clap
{
    class ClapInstance;

    class ClapEditor final : public SoLoud::PluginHostWindowListener
    {
    public:
        /// [gui] is the validated extension; [embedded] and [floating] say
        /// which cocoa modes the plugin supports.
        ClapEditor(ClapInstance &instance, const clap_plugin_gui_t *gui,
                   bool embedded, bool floating);
        ~ClapEditor() override;
        ClapEditor(const ClapEditor &) = delete;
        ClapEditor &operator=(const ClapEditor &) = delete;

        /// Creates and shows the gui, or shows a hidden one. The caller holds
        /// the instance's call guard.
        bool open(const std::string &title);
        /// Hides and destroys the gui and closes the window, without an
        /// editorClosed event. The caller holds the call guard (or tears
        /// the instance down).
        void close();
        /// Whether the gui exists (shown or hidden).
        bool created() const noexcept { return mCreated; }
        /// Whether the gui is on screen.
        bool visible() const noexcept { return mVisible; }
        bool floating() const noexcept { return mFloating; }

        /// A plugin request_resize on main, with its [serial]: applies it now
        /// when the editor is embedded and up. Returns whether the size was
        /// accepted.
        bool requestResizeOnMain(uint32_t width, uint32_t height,
                                 uint32_t serial);
        /// Applies posted requests ([bits] of GuiRequestBit, and the newest
        /// off-main request_resize as packGuiResize). The caller holds the
        /// call guard.
        void service(uint32_t bits, uint64_t resize);

        // PluginHostWindowListener (main, from AppKit).
        void onWindowClosing() override;
        void onApplicationWillTerminate() override;
        bool constrainUserResize(SoLoud::PluginWindowSize &size) override;
        void onUserResized(SoLoud::PluginWindowSize size) override;

    private:
        bool openEmbedded(const std::string &title);
        bool openFloating(const std::string &title);
        /// Destroys a created gui, hiding a shown one first when [hide] (no
        /// window work).
        void destroyGui(bool hide);
        /// Closes our window without listener calls.
        void closeWindow();
        /// Keeps the embedded parent view alive until destroyGui() is done.
        void retainParent();
        /// The window goes now and the gui on a later pump tick (a plugin
        /// call is in progress).
        void deferDestroy();
        void setVisible(bool visible) noexcept;
        /// Publishes mVisible and whether the gui can be shown.
        void publishState() noexcept;
        void applyResizeHints();
        bool acceptableSize(uint32_t width, uint32_t height) const noexcept;
        void applyPluginSize(uint32_t width, uint32_t height);
        void reportEditorClosed();

        ClapInstance &mInstance;
        const clap_plugin_t *mPlugin;
        const clap_plugin_gui_t *mGui;
        const bool mEmbeddedSupported;
        const bool mFloatingSupported;
        std::unique_ptr<SoLoud::PluginHostWindow> mWindow;

        bool mCreated = false;
        bool mFloating = false;
        bool mVisible = false;
        bool mResizable = false;
        /// The user closed the window during a plugin call: the pump
        /// destroys the gui.
        bool mDestroyPending = false;
        /// The content view the gui is parented to, kept alive while its
        /// window is gone and the destroy is pending.
        void *mRetainedParent = nullptr;
        /// The serial of the last request_resize applied or reverted; older
        /// queued ones are superseded.
        uint32_t mResizeSerial = 0;
        /// The last size both sides agreed on.
        SoLoud::PluginWindowSize mAccepted{0, 0};
        /// Bumped by every plugin-initiated resize, so a set_size the plugin
        /// answered with request_resize keeps the plugin's size.
        uint64_t mPluginResizeSerial = 0;
        clap_gui_resize_hints_t mHints{};
        bool mHaveHints = false;
    };
} // namespace soloud_clap

#endif
#endif // SOLOUD_CLAP_EDITOR_H
