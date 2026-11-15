#include "clap_editor.h"

#if defined(SOLOUD_ENABLE_CLAP) && defined(SOLOUD_ENABLE_PLUGIN_UI) && \
    defined(__APPLE__)

#include "clap_instance.h"

namespace soloud_clap
{
    namespace
    {
        constexpr uint32_t kMaxEdge = 16384;
        /// The smallest a user resize makes the window, unless the plugin's
        /// own size is smaller.
        constexpr int32_t kMinUserEdge = 100;

        bool sameSize(SoLoud::PluginWindowSize a, SoLoud::PluginWindowSize b)
        {
            return a.width == b.width && a.height == b.height;
        }
    } // namespace

    ClapEditor::ClapEditor(ClapInstance &instance, const clap_plugin_gui_t *gui,
                           bool embedded, bool floating)
        : mInstance(instance), mPlugin(instance.plugin()), mGui(gui),
          mEmbeddedSupported(embedded), mFloatingSupported(floating)
    {
    }

    // The owner closes the editor (with the plugin alive) first; this only
    // makes sure the window never calls back into a freed listener.
    ClapEditor::~ClapEditor()
    {
        closeWindow();
        SoLoud::releaseNativeView(mRetainedParent);
    }

    bool ClapEditor::open(const std::string &title)
    {
        if (mDestroyPending)
            destroyGui(false);
        if (mCreated)
        {
            if (!mVisible)
            {
                if (!mGui->show(mPlugin))
                    return false;
                if (mDestroyPending)
                    return false; // Closed meanwhile (see openEmbedded()).
                if (!mFloating)
                    mWindow->show();
                setVisible(true);
            }
            else if (!mFloating)
            {
                mWindow->show(); // Bring it to the front.
            }
            return true;
        }
        // Requests posted for an earlier gui no longer apply.
        mInstance.mGuiRequests.store(0, std::memory_order_release);
        mResizeSerial =
            mInstance.mGuiResizeSerial.load(std::memory_order_acquire);
        if (mEmbeddedSupported && openEmbedded(title))
            return true;
        // Plan 4b: the plugin's own window when embedding is not offered or
        // failed.
        return mFloatingSupported && openFloating(title);
    }

    bool ClapEditor::openEmbedded(const std::string &title)
    {
        if (!mGui->create(mPlugin, CLAP_WINDOW_API_COCOA, false))
            return false;
        mCreated = true;
        mFloating = false;
        mHaveHints = false;
        // Cocoa sizes are logical points: no set_scale (gui.h).
        mResizable = mGui->can_resize(mPlugin);
        uint32_t width = 0;
        uint32_t height = 0;
        if (!mGui->get_size(mPlugin, &width, &height) ||
            !acceptableSize(width, height))
        {
            mInstance.logMisbehaving("gui get_size failed or out of range");
            destroyGui(false);
            return false;
        }
        if (!mWindow)
            mWindow = SoLoud::PluginHostWindow::create();
        const SoLoud::PluginWindowSize size{static_cast<int32_t>(width),
                                            static_cast<int32_t>(height)};
        if (!mWindow ||
            !mWindow->open(title, size,
                           mResizable ? SoLoud::PluginWindowResize::Native
                                      : SoLoud::PluginWindowResize::Fixed,
                           this))
        {
            destroyGui(false);
            return false;
        }
        mAccepted = size;
        if (mResizable)
            applyResizeHints();
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_COCOA;
        window.cocoa = mDestroyPending ? nullptr : mWindow->contentView();
        const bool parented =
            window.cocoa != nullptr && mGui->set_parent(mPlugin, &window);
        const bool shown = parented && !mDestroyPending && mGui->show(mPlugin);
        // The user closed the window from a run loop one of these calls
        // spun: it is gone, editorClosed is reported and the pump destroys
        // the gui.
        if (mDestroyPending)
            return false;
        if (!shown)
        {
            // The gui goes before the view it was parented to.
            destroyGui(false);
            closeWindow();
            return false;
        }
        mWindow->show();
        setVisible(true);
        return true;
    }

    bool ClapEditor::openFloating(const std::string &title)
    {
        if (!mGui->create(mPlugin, CLAP_WINDOW_API_COCOA, true))
            return false;
        mCreated = true;
        mFloating = true;
        mResizable = false;
        mGui->suggest_title(mPlugin, title.c_str());
        if (void *view = SoLoud::applicationWindowView())
        {
            clap_window_t window{};
            window.api = CLAP_WINDOW_API_COCOA;
            window.cocoa = view;
            // Optional: the window still works without a transient parent.
            (void)mGui->set_transient(mPlugin, &window);
        }
        if (!mGui->show(mPlugin))
        {
            destroyGui(false);
            return false;
        }
        setVisible(true);
        return true;
    }

    void ClapEditor::close()
    {
        destroyGui(true);
        closeWindow();
    }

    void ClapEditor::destroyGui(bool hide)
    {
        mDestroyPending = false;
        if (!mCreated)
            return;
        // The parent outlives hide() and destroy(), even when the user
        // closes the window from a run loop either call spins
        // (onWindowClosing() ignores it then: mCreated is already false).
        retainParent();
        // Cleared first: a request the plugin makes from these calls is
        // ignored.
        mCreated = false;
        const bool shown = mVisible;
        setVisible(false);
        if (hide && shown)
            (void)mGui->hide(mPlugin);
        mGui->destroy(mPlugin);
        mFloating = false;
        // The parent may go only now.
        SoLoud::releaseNativeView(mRetainedParent);
        mRetainedParent = nullptr;
    }

    void ClapEditor::closeWindow()
    {
        if (mWindow && mWindow->isOpen())
        {
            mWindow->detachListener();
            mWindow->close();
        }
    }

    void ClapEditor::retainParent()
    {
        if (!mFloating && mRetainedParent == nullptr && mWindow &&
            mWindow->isOpen())
            mRetainedParent = SoLoud::retainNativeView(mWindow->contentView());
    }

    void ClapEditor::deferDestroy()
    {
        retainParent();
        mDestroyPending = true;
        setVisible(false);
        mInstance.postGuiRequest(kGuiRequestDeferredDestroy);
    }

    void ClapEditor::setVisible(bool visible) noexcept
    {
        mVisible = visible;
        publishState();
    }

    void ClapEditor::publishState() noexcept
    {
        mInstance.mEditorVisible.store(mVisible, std::memory_order_release);
        mInstance.mEditorCreated.store(mCreated && !mDestroyPending,
                                       std::memory_order_release);
    }

    bool ClapEditor::acceptableSize(uint32_t width,
                                    uint32_t height) const noexcept
    {
        return width >= 1 && height >= 1 && width <= kMaxEdge &&
               height <= kMaxEdge;
    }

    void ClapEditor::applyResizeHints()
    {
        if (mFloating || !mResizable || !mWindow || !mWindow->isOpen())
            return;
        clap_gui_resize_hints_t hints{};
        mHaveHints = mGui->get_resize_hints != nullptr &&
                     mGui->get_resize_hints(mPlugin, &hints);
        mHints = mHaveHints ? hints : clap_gui_resize_hints_t{};
        SoLoud::PluginWindowNativeLimits limits;
        limits.minSize = {kMinUserEdge, kMinUserEdge};
        limits.maxSize = {static_cast<int32_t>(kMaxEdge),
                          static_cast<int32_t>(kMaxEdge)};
        if (mHaveHints)
        {
            limits.resizeHorizontally = mHints.can_resize_horizontally;
            limits.resizeVertically = mHints.can_resize_vertically;
            if (mHints.preserve_aspect_ratio)
            {
                limits.aspectWidth = mHints.aspect_ratio_width;
                limits.aspectHeight = mHints.aspect_ratio_height;
            }
        }
        mWindow->setNativeLimits(limits);
    }

    void ClapEditor::applyPluginSize(uint32_t width, uint32_t height)
    {
        const SoLoud::PluginWindowSize size{static_cast<int32_t>(width),
                                            static_cast<int32_t>(height)};
        // A plugin-initiated size: the window makes no onUserResized call,
        // so no set_size goes back to the plugin (gui.h request_resize).
        mWindow->setContentSize(size);
        mAccepted = size;
        ++mPluginResizeSerial;
    }

    bool ClapEditor::requestResizeOnMain(uint32_t width, uint32_t height,
                                         uint32_t serial)
    {
        if (!mCreated || mFloating || !mWindow || !mWindow->isOpen() ||
            !acceptableSize(width, height))
            return false;
        applyPluginSize(width, height);
        mResizeSerial = serial;
        return true;
    }

    void ClapEditor::service(uint32_t bits, uint64_t resize)
    {
        // The window is gone: whatever else was posted no longer applies.
        if (mDestroyPending)
            destroyGui(false);
        if ((bits & (kGuiRequestClosed | kGuiRequestDestroyed)) != 0)
        {
            if (!mCreated)
                return;
            // The plugin's window is gone, or the gui lost its connection
            // (was_destroyed): either way the editor is closed. destroy() is
            // required after was_destroyed and harmless otherwise.
            destroyGui(false);
            closeWindow();
            reportEditorClosed();
            return;
        }
        if (!mCreated)
            return;
        if ((bits & kGuiRequestHints) != 0)
            applyResizeHints();
        const uint32_t serial = guiResizeSerial(resize);
        // Newer than the last applied one (serials wrap).
        if ((bits & kGuiRequestResize) != 0 && !mFloating &&
            guiResizeNewer(serial, mResizeSerial))
        {
            mResizeSerial = serial;
            // Acknowledged off main: apply it, or revert the plugin to the
            // size the window has (gui.h request_resize).
            const uint32_t width = guiResizeWidth(resize);
            const uint32_t height = guiResizeHeight(resize);
            if (mWindow && mWindow->isOpen() && acceptableSize(width, height))
                applyPluginSize(width, height);
            else
                (void)mGui->set_size(mPlugin,
                                     static_cast<uint32_t>(mAccepted.width),
                                     static_cast<uint32_t>(mAccepted.height));
        }
        if ((bits & kGuiRequestHide) != 0 && mVisible)
        {
            (void)mGui->hide(mPlugin);
            if (!mFloating)
                mWindow->hide();
            setVisible(false);
        }
        if ((bits & kGuiRequestShow) != 0 && !mVisible && mCreated)
        {
            if (mGui->show(mPlugin))
            {
                if (!mFloating)
                    mWindow->show();
                setVisible(true);
            }
        }
    }

    void ClapEditor::reportEditorClosed()
    {
        mInstance.notify(kNotifyEditorClosed);
    }

    // --- window listener (main, from AppKit) ---------------------------------

    void ClapEditor::onWindowClosing()
    {
        if (!mCreated)
            return;
        InstanceCallGuard guard(mInstance);
        if (guard.acquired() && mInstance.guiCallable())
        {
            destroyGui(true);
            mInstance.afterPluginCall();
        }
        else
        {
            // Inside a plugin call (a nested run loop): destroy on a later
            // pump tick. The window still goes now, but the view the gui is
            // parented to stays until the destroy (gui.h set_parent).
            deferDestroy();
        }
        reportEditorClosed();
    }

    void ClapEditor::onApplicationWillTerminate()
    {
        if (mCreated && !mDestroyPending)
        {
            InstanceCallGuard guard(mInstance);
            if (guard.acquired() && mInstance.guiCallable())
                destroyGui(true);
            else
                deferDestroy(); // As for a close inside a plugin call.
        }
        closeWindow();
    }

    bool ClapEditor::constrainUserResize(SoLoud::PluginWindowSize &size)
    {
        if (!mCreated || mFloating || !mResizable)
            return false;
        InstanceCallGuard guard(mInstance);
        if (!guard.acquired() || !mInstance.guiCallable())
            return false;
        uint32_t width = static_cast<uint32_t>(std::max<int32_t>(1, size.width));
        uint32_t height =
            static_cast<uint32_t>(std::max<int32_t>(1, size.height));
        if (mHaveHints && !mHints.can_resize_horizontally)
            width = static_cast<uint32_t>(mAccepted.width);
        if (mHaveHints && !mHints.can_resize_vertically)
            height = static_cast<uint32_t>(mAccepted.height);
        uint32_t adjustedWidth = width;
        uint32_t adjustedHeight = height;
        if (mGui->adjust_size != nullptr &&
            mGui->adjust_size(mPlugin, &adjustedWidth, &adjustedHeight) &&
            acceptableSize(adjustedWidth, adjustedHeight))
        {
            width = adjustedWidth;
            height = adjustedHeight;
        }
        size = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        return true;
    }

    void ClapEditor::onUserResized(SoLoud::PluginWindowSize size)
    {
        if (!mCreated || mFloating || sameSize(size, mAccepted))
            return;
        InstanceCallGuard guard(mInstance);
        if (!guard.acquired() || !mInstance.guiCallable() ||
            !acceptableSize(static_cast<uint32_t>(std::max<int32_t>(0, size.width)),
                            static_cast<uint32_t>(std::max<int32_t>(0, size.height))))
        {
            mWindow->setContentSize(mAccepted);
            return;
        }
        const uint64_t serial = mPluginResizeSerial;
        const bool accepted =
            mGui->set_size(mPlugin, static_cast<uint32_t>(size.width),
                           static_cast<uint32_t>(size.height));
        mInstance.afterPluginCall();
        if (!mCreated || serial != mPluginResizeSerial)
            return; // The plugin resized itself meanwhile: its size stands.
        if (accepted)
        {
            mAccepted = size;
            // Limits on a locked axis follow the size the plugin now has.
            mWindow->setAcceptedUserSize(size);
        }
        else
            mWindow->setContentSize(mAccepted); // No listener call: no loop.
    }
} // namespace soloud_clap

#endif
