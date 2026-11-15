#if defined(SOLOUD_ENABLE_VST3) && defined(__APPLE__)

#include <TargetConditionals.h>

#if !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE

#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "public.sdk/source/common/memorystream.h"
#include "vst3_editor_shm.h"
#include "vst3_editor_window.h"
#include "vst3_host.h"
#include "../plugin_ui/plugin_host_window.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>


namespace SoLoud {

namespace {
// Bridge for VST3 plugins (such as flutter_vst3) that embed their own
// precompiled App.framework and create a secondary FlutterEngine inside a
// Flutter macOS host application.
//
// 1. On macOS, FlutterDartProject has no aotLibraryPath property, so a secondary
//    FlutterEngine falls back to dlsym(RTLD_DEFAULT, "_kDartSnapshotData"),
//    which resolves to the host app's App.framework instead of the plugin's
//    App.framework. We intercept FlutterEngine initialization for precompiled
//    plugin bundles and append --aot-shared-library-name=<plugin App binary>.
// 2. When the host app runs in Debug (JIT) mode, the loaded FlutterMacOS
//    framework cannot execute a precompiled AOT plugin bundle. We detect this
//    before FlutterEngineInitialize is called, unregister the temporary engine
//    from FlutterAppDelegate (preventing 'flutter/lifecycle' platform-message
//    errors), and fail fast so the plugin cleanly falls back to its Cocoa UI.
const char kVst3PluginBundleKey = 0;

using Vst3EmbedderInitFn = int (*)(size_t, const void *, const void *, void *,
                                   void **);
Vst3EmbedderInitFn sOrigEmbedderInit = nullptr;
std::string sPendingAotSwitch;
std::atomic<bool> sAppTerminating{false};

id (*sOrigInitWithBundle)(id, SEL, NSBundle *) = nullptr;
id (*sOrigInitWithNameProject)(id, SEL, NSString *, id, BOOL) = nullptr;
BOOL (*sOrigRunWithEntrypoint)(id, SEL, NSString *) = nullptr;
void (*sOrigShutDownEngine)(id, SEL) = nullptr;

int vst3EmbedderInitWithAotLibrary(size_t version, const void *config,
                                   const void *args, void *userData,
                                   void **engineOut) {
  if (args == nullptr || sPendingAotSwitch.empty()) {
    return sOrigEmbedderInit(version, config, args, userData, engineOut);
  }
  const size_t structSize = *static_cast<const size_t *>(args);
  std::vector<char> copy(static_cast<const char *>(args),
                         static_cast<const char *>(args) + structSize);
  // In FlutterProjectArgs (64-bit), command_line_argc is at +0x28 and
  // command_line_argv is at +0x30.
  const int argc = *reinterpret_cast<const int *>(copy.data() + 0x28);
  const auto *argv =
      *reinterpret_cast<const char *const *const *>(copy.data() + 0x30);
  std::vector<const char *> vec(argv, argv + (argv && argc > 0 ? argc : 0));
  vec.push_back(sPendingAotSwitch.c_str());
  *reinterpret_cast<int *>(copy.data() + 0x28) = static_cast<int>(vec.size());
  *reinterpret_cast<const char ***>(copy.data() + 0x30) = vec.data();
  return sOrigEmbedderInit(version, config, copy.data(), userData, engineOut);
}

void unregisterPluginEngineLifecycle(id engine) {
  if (engine == nil) {
    return;
  }
  id appDel = [[NSApplication sharedApplication] delegate];
  SEL removeSel = sel_registerName("removeApplicationLifecycleDelegate:");
  if (appDel != nil && [appDel respondsToSelector:removeSel]) {
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(appDel, removeSel,
                                                          engine);
  }
}

id vst3InitWithPrecompiledDartBundle(id self, SEL _cmd, NSBundle *bundle) {
  id proj = sOrigInitWithBundle(self, _cmd, bundle);
  if (proj != nil && bundle != nil) {
    objc_setAssociatedObject(proj, &kVst3PluginBundleKey, bundle,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return proj;
}

id vst3InitWithNameProject(id self, SEL _cmd, NSString *name, id project,
                           BOOL allowHeadless) {
  id eng = sOrigInitWithNameProject(self, _cmd, name, project, allowHeadless);
  if (eng != nil && project != nil) {
    NSBundle *bundle = static_cast<NSBundle *>(
        objc_getAssociatedObject(project, &kVst3PluginBundleKey));
    if (bundle != nil) {
      objc_setAssociatedObject(eng, &kVst3PluginBundleKey, bundle,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
  }
  return eng;
}

BOOL vst3RunWithEntrypoint(id self, SEL _cmd, NSString *entrypoint) {
  NSBundle *appBundle = static_cast<NSBundle *>(
      objc_getAssociatedObject(self, &kVst3PluginBundleKey));
  if (appBundle == nil) {
    return sOrigRunWithEntrypoint(self, _cmd, entrypoint);
  }

  SEL apiSel = sel_registerName("embedderAPI");
  void **api = [self respondsToSelector:apiSel]
                   ? reinterpret_cast<void **(*)(id, SEL)>(objc_msgSend)(
                         self, apiSel)
                   : nullptr;
  // FlutterEngineProcTable::RunsAOTCompiledDartCode is at index 30 (+0xf0).
  auto runsAot = reinterpret_cast<bool (*)(void)>(api ? api[30] : nullptr);
  if (!runsAot || !runsAot()) {
    NSString *kernelPath = [appBundle pathForResource:@"kernel_blob"
                                               ofType:@"bin"
                                          inDirectory:@"flutter_assets"];
    if (kernelPath == nil) {
      unregisterPluginEngineLifecycle(self);
      return NO;
    }
  } else if (appBundle.executablePath != nil) {
    // FlutterEngineProcTable::Initialize is at index 5 (+0x28).
    sPendingAotSwitch = std::string("--aot-shared-library-name=") +
                        appBundle.executablePath.fileSystemRepresentation;
    if (reinterpret_cast<Vst3EmbedderInitFn>(api[5]) !=
        &vst3EmbedderInitWithAotLibrary) {
      sOrigEmbedderInit = reinterpret_cast<Vst3EmbedderInitFn>(api[5]);
      api[5] = reinterpret_cast<void *>(&vst3EmbedderInitWithAotLibrary);
    }
  }

  const BOOL ok = sOrigRunWithEntrypoint(self, _cmd, entrypoint);
  sPendingAotSwitch.clear();
  if (!ok) {
    unregisterPluginEngineLifecycle(self);
  }
  return ok;
}

void vst3ShutDownEngine(id self, SEL _cmd) {
  NSBundle *appBundle = static_cast<NSBundle *>(
      objc_getAssociatedObject(self, &kVst3PluginBundleKey));
  if (appBundle != nil) {
    objc_setAssociatedObject(self, &kVst3PluginBundleKey, nil,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    unregisterPluginEngineLifecycle(self);
    if (!sAppTerminating.load(std::memory_order_acquire)) {
      // When closeEditor() is invoked from Dart FFI on the main thread, the
      // host app's Dart isolate is still bound to Thread 0. Calling
      // FlutterEngineDeinitialize synchronously would invoke
      // DartIsolate::Shutdown() -> Dart_EnterIsolate() on the same thread and
      // abort with "Dart_EnterIsolate expects there to be no current isolate".
      // Deferring shutDownEngine to the next main-queue turn runs it after the
      // FFI call returns and the host Dart isolate has exited.
#if !__has_feature(objc_arc)
      [self retain];
      [appBundle retain];
#endif
      dispatch_async(dispatch_get_main_queue(), ^{
        (void)appBundle;
        sOrigShutDownEngine(self, _cmd);
#if !__has_feature(objc_arc)
        [self release];
        [appBundle release];
#endif
      });
      return;
    }
  }
  sOrigShutDownEngine(self, _cmd);
}

void installFlutterVst3HostBridgeIfNeeded() {
  static bool sInstalled = false;
  if (sInstalled) {
    return;
  }
  Class projCls = NSClassFromString(@"FlutterDartProject");
  Class engCls = NSClassFromString(@"FlutterEngine");
  if (projCls == nil || engCls == nil) {
    return;
  }
  Method mBundle = class_getInstanceMethod(
      projCls, sel_registerName("initWithPrecompiledDartBundle:"));
  Method mInit = class_getInstanceMethod(
      engCls, sel_registerName("initWithName:project:allowHeadlessExecution:"));
  Method mRun =
      class_getInstanceMethod(engCls, sel_registerName("runWithEntrypoint:"));
  Method mShut =
      class_getInstanceMethod(engCls, sel_registerName("shutDownEngine"));
  if (mBundle == nullptr || mInit == nullptr || mRun == nullptr ||
      mShut == nullptr) {
    return;
  }
  sOrigInitWithBundle = reinterpret_cast<decltype(sOrigInitWithBundle)>(
      method_setImplementation(
          mBundle, reinterpret_cast<IMP>(&vst3InitWithPrecompiledDartBundle)));
  sOrigInitWithNameProject =
      reinterpret_cast<decltype(sOrigInitWithNameProject)>(
          method_setImplementation(
              mInit, reinterpret_cast<IMP>(&vst3InitWithNameProject)));
  sOrigRunWithEntrypoint = reinterpret_cast<decltype(sOrigRunWithEntrypoint)>(
      method_setImplementation(mRun,
                               reinterpret_cast<IMP>(&vst3RunWithEntrypoint)));
  sOrigShutDownEngine = reinterpret_cast<decltype(sOrigShutDownEngine)>(
      method_setImplementation(mShut,
                               reinterpret_cast<IMP>(&vst3ShutDownEngine)));
  sInstalled = true;
}
} // namespace

static inline bool rectEqual(const Steinberg::ViewRect &a,
                             const Steinberg::ViewRect &b) noexcept {
  return a.left == b.left && a.top == b.top &&
         a.right == b.right && a.bottom == b.bottom;
}

static NSView *borrowedView(void *view) {
#if __has_feature(objc_arc)
  return (__bridge NSView *)view;
#else
  return reinterpret_cast<NSView *>(view);
#endif
}

// The VST3 IPlugFrame side of an editor: the native window itself is the
// shared PluginHostWindow (src/plugin_ui), and the out-of-process editor
// helper lives here.
class Vst3EditorWindowMacOS final : public Vst3EditorWindow,
                                    public PluginHostWindowListener {
public:
  Vst3EditorWindowMacOS() = default;
  ~Vst3EditorWindowMacOS() override { close(); }

  bool supportsRemoteEditor(const std::string &bundlePath) const override {
#if defined(SOLOUD_VST3_HELPER_MAIN) || !defined(SOLOUD_VST3_HELPER_PATH)
    (void)bundlePath;
    return false;
#else
    if (bundlePath.empty()) {
      return false;
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *helperPath =
        [NSString stringWithUTF8String:SOLOUD_VST3_HELPER_PATH];
    if (helperPath == nil || ![fm isExecutableFileAtPath:helperPath]) {
      return false;
    }
    NSString *nsBundle = [NSString stringWithUTF8String:bundlePath.c_str()];
    if (nsBundle == nil) {
      return false;
    }
    NSString *appFw = [nsBundle
        stringByAppendingPathComponent:@"Contents/Frameworks/App.framework"];
    NSString *flFw = [nsBundle stringByAppendingPathComponent:
                                   @"Contents/Frameworks/FlutterMacOS.framework"];
    return [fm fileExistsAtPath:appFw] || [fm fileExistsAtPath:flFw];
#endif
  }

  bool openRemote(const Vst3RemoteEditorContext &ctx) override {
#if defined(SOLOUD_VST3_HELPER_MAIN) || !defined(SOLOUD_VST3_HELPER_PATH)
    (void)ctx;
    return false;
#else
    close();
    if (!supportsRemoteEditor(ctx.bundlePath)) {
      return false;
    }

    static std::atomic<uint32_t> sShmCounter{0};
    NSString *shmFile = [NSTemporaryDirectory()
        stringByAppendingPathComponent:
            [NSString stringWithFormat:@"soloud_vst3_shm_%d_%u.bin", getpid(),
                                       ++sShmCounter]];
    const char *shmPath = [shmFile fileSystemRepresentation];
    const int fd = ::open(shmPath, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
      return false;
    }
    if (ftruncate(fd, sizeof(SoLoudVst3EditorShm)) != 0) {
      ::close(fd);
      unlink(shmPath);
      return false;
    }
    void *mapped =
        mmap(nullptr, sizeof(SoLoudVst3EditorShm), PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED || mapped == nullptr) {
      unlink(shmPath);
      return false;
    }

    mShm = static_cast<SoLoudVst3EditorShm *>(mapped);
    initVst3EditorShmHeader(mShm);

    if (ctx.component != nullptr) {
      Steinberg::MemoryStream stream;
      if (ctx.component->getState(&stream) == Steinberg::kResultTrue &&
          stream.getSize() > 0 &&
          static_cast<size_t>(stream.getSize()) <= kVst3EditorMaxStateBytes &&
          stream.getData() != nullptr) {
        mShm->initialStateSize = static_cast<uint32_t>(stream.getSize());
        std::memcpy(mShm->initialState, stream.getData(),
                    mShm->initialStateSize);
      }
    }

    if (ctx.controller != nullptr) {
      const int32_t count = ctx.controller->getParameterCount();
      uint32_t written = 0;
      for (int32_t i = 0;
           i < count && written < kVst3EditorMaxInitialParams; ++i) {
        Steinberg::Vst::ParameterInfo info{};
        if (ctx.controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
          mShm->initialParams[written].paramId = info.id;
          mShm->initialParams[written].reserved = 0;
          mShm->initialParams[written].value =
              ctx.controller->getParamNormalized(info.id);
          ++written;
        }
      }
      mShm->initialParamCount = written;
    }

    mOnGuiParamEdit = ctx.onGuiParamEdit;

    const std::string parentPidStr = std::to_string(getpid());
    const std::string titleStr =
        ctx.title.empty() ? std::string("VST3 Plugin") : ctx.title;
    const std::string shmPathStr(shmPath);

    const pid_t pid = fork();
    if (pid < 0) {
      unlink(shmPath);
      munmap(mShm, sizeof(SoLoudVst3EditorShm));
      mShm = nullptr;
      return false;
    }
    if (pid == 0) {
      const int maxFd = getdtablesize();
      for (int d = 3; d < maxFd; ++d) {
        ::close(d);
      }
      execl(SOLOUD_VST3_HELPER_PATH, SOLOUD_VST3_HELPER_PATH,
            ctx.bundlePath.c_str(), ctx.classUid.c_str(), titleStr.c_str(),
            shmPathStr.c_str(), parentPidStr.c_str(),
            static_cast<char *>(nullptr));
      _exit(127);
    }

    mHelperPid = pid;
    mShmPath = shmPathStr;

    // Wait up to 3 seconds for the helper window to finish mounting.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    uint32_t ready = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      ready = mShm->helperReady.load(std::memory_order_acquire);
      if (ready != 0) {
        break;
      }
      int status = 0;
      if (waitpid(mHelperPid, &status, WNOHANG) != 0) {
        mHelperPid = 0;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (ready != 1) {
      close();
      return false;
    }

    mIsOpen.store(true, std::memory_order_release);
    publishRemoteShm(mShm);

    void (^startTimerBlock)(void) = ^{
      NSTimer *timer = [NSTimer
          timerWithTimeInterval:0.008
                        repeats:YES
                          block:^(NSTimer *t) {
                            (void)t;
                            pollRemoteHelper();
                          }];
      [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
#if !__has_feature(objc_arc)
      [timer retain];
#endif
      mPollTimer = timer;
    };
    if ([NSThread isMainThread]) {
      startTimerBlock();
    } else {
      dispatch_sync(dispatch_get_main_queue(), startTimerBlock);
    }

    return true;
#endif
  }

  bool open(Steinberg::IPlugView *plugView, const char *title) override {
    if (plugView == nullptr) {
      return false;
    }
    if (plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeNSView) !=
        Steinberg::kResultTrue) {
      return false;
    }

    close();

    bool success = false;
    std::string titleStr = (title != nullptr && title[0] != '\0')
                               ? std::string(title)
                               : std::string("VST3 Plugin");

    runOnMainThreadSync([&] {
      Steinberg::ViewRect rect{0, 0, 800, 600};
      plugView->getSize(&rect);
      const int32_t w = rect.getWidth() > 64 ? rect.getWidth() : 800;
      const int32_t h = rect.getHeight() > 64 ? rect.getHeight() : 600;

      bool nativeResize = (plugView->canResize() == Steinberg::kResultTrue);
      if (!nativeResize) {
        Steinberg::ViewRect testRect{0, 0, static_cast<Steinberg::int32>(w * 1.5),
                                     static_cast<Steinberg::int32>(h * 1.5)};
        if (plugView->checkSizeConstraint(&testRect) == Steinberg::kResultTrue &&
            (testRect.getWidth() != w || testRect.getHeight() != h)) {
          nativeResize = true;
        }
      }

      if (mHostWindow == nullptr) {
        mHostWindow = PluginHostWindow::create();
      }
      if (!mHostWindow->open(titleStr, {w, h},
                             nativeResize ? PluginWindowResize::Native
                                          : PluginWindowResize::Fixed,
                             this)) {
        return;
      }

      mPlugView = plugView;
      mPlugView->setFrame(this);
      installFlutterVst3HostBridgeIfNeeded();

      if (mPlugView->attached(mHostWindow->contentView(),
                              Steinberg::kPlatformTypeNSView) !=
          Steinberg::kResultTrue) {
        mPlugView->setFrame(nullptr);
        mPlugView = nullptr;
        mHostWindow->close();
        return;
      }

      NSView *pluginView = borrowedView(mHostWindow->firstPluginSubview());
      if (nativeResize) {
        mHostWindow->setResizeMode(PluginWindowResize::Native);
      } else {
        // Check if the attached plugin subview is JUCE-based (or supports Cocoa view
        // coordinate transforms). JUCE plugins return canResize() == kResultFalse
        // but safely scale via host contentView bounds scaling. Non-JUCE plugins
        // (such as Voxengo's CWindowCocoaContent) bypass Cocoa coordinate transformations
        // and directly query hardware display mouse positions via CGEventGetLocation,
        // breaking mouse hit-testing when host scaling is applied.
        bool canHostScale = false;
        if (pluginView != nil) {
          NSString *className = NSStringFromClass([pluginView class]);
          if ([className hasPrefix:@"JUCEView"]) {
            canHostScale = true;
          }
        }
        mHostWindow->setResizeMode(canHostScale ? PluginWindowResize::HostScaled
                                                : PluginWindowResize::Fixed);
      }
      mHostWindow->show();
      if (pluginView == nil) {
        // Some VST3 plugins (such as flutter_vst3 EmbeddedFlutterView) mount
        // their NSView asynchronously on the next main-queue turn.
        const uint64_t generation = mHostWindow->generation();
        const std::weak_ptr<int> alive = mAlive;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (alive.expired() || !mIsOpen.load(std::memory_order_acquire) ||
              mHostWindow == nullptr || !mHostWindow->isOpen() ||
              mHostWindow->generation() != generation) {
            return;
          }
          if (mPlugView != nullptr) {
            const PluginWindowSize base = mHostWindow->baseSize();
            Steinberg::ViewRect asyncRect{0, 0, 0, 0};
            if (mPlugView->getSize(&asyncRect) == Steinberg::kResultTrue &&
                asyncRect.getWidth() > 64 && asyncRect.getHeight() > 64 &&
                (asyncRect.getWidth() != base.width ||
                 asyncRect.getHeight() != base.height)) {
              resizeView(mPlugView, &asyncRect);
            }
          }
          if (mHostWindow->isOpen() &&
              mHostWindow->firstPluginSubview() != nullptr) {
            mHostWindow->focusPluginSubviewIfUnfocused();
            mHostWindow->refreshContent();
          }
        });
      }
      mIsOpen.store(true, std::memory_order_release);
      success = true;
    });
    return success;
  }

  void close() override {
    const bool windowOpen = mHostWindow != nullptr && mHostWindow->isOpen();
    if (!mIsOpen.load(std::memory_order_acquire) && !windowOpen &&
        mPlugView == nullptr && mHelperPid == 0 && mShm == nullptr) {
      return;
    }

    runOnMainThreadSync([&] {
      mIsOpen.store(false, std::memory_order_release);
      if (mPollTimer != nil) {
        [mPollTimer invalidate];
#if !__has_feature(objc_arc)
        [mPollTimer release];
#endif
        mPollTimer = nil;
      }
      if (mShm != nullptr) {
        mShm->hostClosed.store(1, std::memory_order_release);
      }
      if (mHelperPid > 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        int status = 0;
        bool exited = false;
        while (std::chrono::steady_clock::now() < deadline) {
          if (waitpid(mHelperPid, &status, WNOHANG) != 0) {
            exited = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!exited) {
          kill(mHelperPid, SIGKILL);
          waitpid(mHelperPid, &status, 0);
        }
        mHelperPid = 0;
      }
      if (mShm != nullptr) {
        retireRemoteShm();
        munmap(mShm, sizeof(SoLoudVst3EditorShm));
        mShm = nullptr;
      }
      if (!mShmPath.empty()) {
        unlink(mShmPath.c_str());
        mShmPath.clear();
      }
      mOnGuiParamEdit = nullptr;
      if (mHostWindow != nullptr) {
        mHostWindow->detachListener();
      }
      if (mPlugView != nullptr) {
        mPlugView->removed();
        mPlugView->setFrame(nullptr);
        mPlugView = nullptr;
      }
      if (mHostWindow != nullptr) {
        mHostWindow->close();
      }
    });
  }

  bool isOpen() const noexcept override {
    return mIsOpen.load(std::memory_order_acquire);
  }

  Steinberg::tresult PLUGIN_API
  resizeView(Steinberg::IPlugView *view,
             Steinberg::ViewRect *newSize) override {
    if (view == nullptr || newSize == nullptr || view != mPlugView) {
      return Steinberg::kInvalidArgument;
    }
    if (mHostWindow == nullptr || !mHostWindow->isOpen()) {
      return Steinberg::kInternalError;
    }
    if (mInResize.load(std::memory_order_acquire)) {
      return Steinberg::kResultFalse;
    }

    Steinberg::ViewRect r{0, 0, 0, 0};
    if (view->getSize(&r) != Steinberg::kResultTrue) {
      return Steinberg::kInternalError;
    }
    const PluginWindowSize base = mHostWindow->baseSize();
    if (rectEqual(r, *newSize) && base.width == newSize->getWidth() &&
        base.height == newSize->getHeight()) {
      return Steinberg::kResultTrue;
    }

    mInResize.store(true, std::memory_order_release);
    runOnMainThreadSync([&] {
      if (mHostWindow != nullptr && mHostWindow->isOpen()) {
        mHostWindow->setContentSize({newSize->getWidth(), newSize->getHeight()});
      }
    });
    mInResize.store(false, std::memory_order_release);

    view->onSize(newSize);

    runOnMainThreadSync([&] {
      if (mHostWindow != nullptr && mHostWindow->isOpen()) {
        mHostWindow->refreshContent();
      }
    });
    return Steinberg::kResultTrue;
  }

  // PluginHostWindowListener

  void onWindowClosing() override {
    mIsOpen.store(false, std::memory_order_release);
    if (mPlugView != nullptr) {
      mPlugView->removed();
      mPlugView->setFrame(nullptr);
      mPlugView = nullptr;
    }
  }

  void onApplicationWillTerminate() override {
    sAppTerminating.store(true, std::memory_order_release);
    close();
  }

  bool constrainUserResize(PluginWindowSize &size) override {
    if (mPlugView == nullptr) {
      return false;
    }
    Steinberg::ViewRect r{0, 0, size.width, size.height};
    if (mPlugView->checkSizeConstraint(&r) != Steinberg::kResultTrue) {
      return false;
    }
    size = {r.getWidth(), r.getHeight()};
    return true;
  }

  void onUserResized(PluginWindowSize size) override {
    if (mInResize.load(std::memory_order_acquire) || mPlugView == nullptr) {
      return;
    }
    Steinberg::ViewRect r{0, 0, size.width, size.height};
    Steinberg::ViewRect current{0, 0, 0, 0};
    if (mPlugView->getSize(&current) == Steinberg::kResultTrue &&
        !rectEqual(r, current)) {
      mPlugView->onSize(&r);
    }
  }


private:
  void pollRemoteHelper() {
    if (mShm == nullptr) {
      return;
    }
    uint32_t r = mShm->guiToHostRead.load(std::memory_order_relaxed);
    const uint32_t w = mShm->guiToHostWrite.load(std::memory_order_acquire);
    while (r != w) {
      const auto msg = mShm->guiToHostQueue[r % kVst3EditorParamQueueSize];
      if (mOnGuiParamEdit) {
        mOnGuiParamEdit(msg.paramId, msg.value);
      }
      ++r;
    }
    mShm->guiToHostRead.store(r, std::memory_order_release);

    bool helperExited =
        (mShm->helperClosed.load(std::memory_order_acquire) != 0);
    if (!helperExited && mHelperPid > 0) {
      int status = 0;
      if (waitpid(mHelperPid, &status, WNOHANG) != 0) {
        mHelperPid = 0;
        helperExited = true;
      }
    }
    if (helperExited) {
      close();
    }
  }

  Steinberg::IPtr<Steinberg::IPlugView> mPlugView;
  std::unique_ptr<PluginHostWindow> mHostWindow;
  SoLoudVst3EditorShm *mShm = nullptr;
  std::string mShmPath;
  pid_t mHelperPid = 0;
  NSTimer *mPollTimer = nil;
  std::function<void(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue)>
      mOnGuiParamEdit;
  // Expires with this object, for blocks queued by open().
  std::shared_ptr<int> mAlive = std::make_shared<int>(0);
  std::atomic<bool> mIsOpen{false};
  std::atomic<bool> mInResize{false};
};

std::unique_ptr<Vst3EditorWindow> Vst3EditorWindow::create() {
  return std::make_unique<Vst3EditorWindowMacOS>();
}

} // namespace SoLoud

@interface SoLoudVst3AppTerminateObserver : NSObject
@end

@implementation SoLoudVst3AppTerminateObserver
- (void)applicationWillTerminate:(NSNotification *)notification {
  (void)notification;
  SoLoud::sAppTerminating.store(true, std::memory_order_release);
  // The engine may still be mixing.
  SoLoud::Vst3HostManager::instance().unloadAllAtExit();
}
@end

static SoLoudVst3AppTerminateObserver *gVst3AppTerminateObserver = nil;

namespace SoLoud {

void installVst3TerminationObserver() {
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    void (^installBlock)(void) = ^{
      gVst3AppTerminateObserver = [[SoLoudVst3AppTerminateObserver alloc] init];
      [[NSNotificationCenter defaultCenter]
          addObserver:gVst3AppTerminateObserver
             selector:@selector(applicationWillTerminate:)
                 name:NSApplicationWillTerminateNotification
               object:nil];
    };
    if ([NSThread isMainThread]) {
      installBlock();
    } else {
      dispatch_async(dispatch_get_main_queue(), installBlock);
    }
  });
}

} // namespace SoLoud

#endif // !TARGET_OS_IPHONE
#endif // SOLOUD_ENABLE_VST3 && __APPLE__
