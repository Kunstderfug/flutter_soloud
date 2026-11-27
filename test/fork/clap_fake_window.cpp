#include "clap_fake_window.h"

#include <algorithm>

namespace
{
    FakeWindowState gState;
    int gContentView = 0;
    int gApplicationView = 0;

    class FakeWindow final : public SoLoud::PluginHostWindow
    {
    public:
        FakeWindow() { ++gState.creates; }
        ~FakeWindow() override
        {
            if (gState.open)
                close();
        }

        bool open(const std::string &title, SoLoud::PluginWindowSize size,
                  SoLoud::PluginWindowResize mode,
                  SoLoud::PluginHostWindowListener *listener) override
        {
            if (gState.open)
                close();
            if (gState.failOpen)
            {
                gState.failOpen = false;
                return false;
            }
            ++gState.opens;
            ++mGeneration;
            gState.open = true;
            gState.visible = false;
            gState.listenerDetached = false;
            gState.title = title;
            gState.mode = mode;
            gState.base = size;
            gState.current = size;
            gState.hasLimits = false;
            gState.limits = SoLoud::PluginWindowNativeLimits();
            gState.listener = listener;
            return true;
        }

        void *contentView() const override
        {
            return gState.open && !gState.nullContentView ? &gContentView
                                                          : nullptr;
        }

        void *firstPluginSubview() const override { return nullptr; }

        void setResizeMode(SoLoud::PluginWindowResize mode) override
        {
            gState.mode = mode;
        }

        SoLoud::PluginWindowResize resizeMode() const override
        {
            return gState.mode;
        }

        void setContentSize(SoLoud::PluginWindowSize size) override
        {
            ++gState.setContentSizes;
            gState.base = size;
            gState.current = size;
        }

        SoLoud::PluginWindowSize baseSize() const override { return gState.base; }

        void setAcceptedUserSize(SoLoud::PluginWindowSize size) override
        {
            if (!gState.open || gState.mode != SoLoud::PluginWindowResize::Native)
                return;
            ++gState.acceptedUserSizes;
            gState.base = size;
        }

        void setNativeLimits(const SoLoud::PluginWindowNativeLimits &limits) override
        {
            ++gState.setNativeLimitsCalls;
            gState.limits = limits;
            gState.hasLimits = true;
        }

        void show() override
        {
            ++gState.shows;
            gState.visible = true;
        }

        void hide() override
        {
            ++gState.hides;
            gState.visible = false;
        }

        bool isVisible() const override { return gState.open && gState.visible; }
        void focusPluginSubviewIfUnfocused() override {}
        void refreshContent() override {}

        void detachListener() override
        {
            gState.listenerDetached = true;
            gState.listener = nullptr;
        }

        void close() override
        {
            if (!gState.open)
                return;
            ++gState.closes;
            gState.open = false;
            gState.visible = false;
            gState.listener = nullptr;
        }

        bool isOpen() const override { return gState.open; }
        uint64_t generation() const override { return mGeneration; }

    private:
        uint64_t mGeneration = 0;
    };
} // namespace

FakeWindowState &fakeWindow() { return gState; }

void resetFakeWindow() { gState = FakeWindowState(); }

bool fakeUserClose()
{
    if (!gState.open)
        return false;
    if (SoLoud::PluginHostWindowListener *listener = gState.listener)
        listener->onWindowClosing();
    // The window then tears itself down (no further listener calls).
    if (gState.open)
    {
        ++gState.closes;
        gState.open = false;
        gState.visible = false;
        gState.listener = nullptr;
    }
    return true;
}

bool fakeAppTerminate()
{
    if (!gState.open)
        return false;
    if (SoLoud::PluginHostWindowListener *listener = gState.listener)
        listener->onApplicationWillTerminate();
    return true;
}

bool fakeUserResize(SoLoud::PluginWindowSize size)
{
    if (!gState.open || gState.mode != SoLoud::PluginWindowResize::Native)
        return false;
    SoLoud::PluginHostWindowListener *listener = gState.listener;
    if (listener != nullptr)
        (void)listener->constrainUserResize(size);
    gState.current = size;
    if (listener != nullptr && gState.listener == listener)
        listener->onUserResized(size);
    return true;
}

void *fakeApplicationView() { return &gApplicationView; }

bool fakeContentViewAlive()
{
    return gState.open || gState.contentViewRetains > 0;
}

FakeContentLimits fakeContentLimits()
{
    const SoLoud::PluginWindowNativeLimits &l = gState.limits;
    const SoLoud::PluginWindowSize b = gState.base;
    FakeContentLimits out;
    out.minSize = {std::min(l.minSize.width, b.width),
                   std::min(l.minSize.height, b.height)};
    out.maxSize = {std::max(l.maxSize.width, b.width),
                   std::max(l.maxSize.height, b.height)};
    if (!l.resizeHorizontally)
        out.minSize.width = out.maxSize.width = b.width;
    if (!l.resizeVertically)
        out.minSize.height = out.maxSize.height = b.height;
    return out;
}

namespace SoLoud
{
    std::unique_ptr<PluginHostWindow> PluginHostWindow::create()
    {
        return std::unique_ptr<PluginHostWindow>(new FakeWindow());
    }

    void runOnMainThreadSync(const std::function<void()> &fn) { fn(); }

    void *retainNativeView(void *view)
    {
        if (view == &gContentView)
            ++gState.contentViewRetains;
        return view;
    }

    void releaseNativeView(void *view)
    {
        if (view == &gContentView)
            --gState.contentViewRetains;
    }

    void *applicationWindowView()
    {
        return gState.noApplicationWindow ? nullptr : &gApplicationView;
    }
} // namespace SoLoud
