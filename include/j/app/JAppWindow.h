#pragma once

// ============================================================================
// jf::JAppWindow — a top-level application window that owns the native window,
// the Vulkan GPU HAL, the font atlas, AND the framework's own window chrome
// (frame + title bar + min/max/close + drag + edge-resize), then runs the
// standard render/present/resize loop.
//
// The window is opened BORDERLESS so the OS chrome is gone and the framework
// draws its own frame (JWindowSkin) — consistent custom look across platforms.
// The application supplies only its content: onRender(buffer) draws into the
// content area (origin at contentTop()), and onInput(mx,my,pressed,released)
// receives clicks the chrome did not consume.
//
//   JGuiApplication app;
//   JAppWindow win("My App", 1280, 800);
//   win.onRender = [&](JPrimitiveBuffer& buf){ ...draw content below win.contentTop()... };
//   win.onInput  = [&](float x,float y,bool p,bool r){ ...dispatch... };
//   return win.run();
// ============================================================================

#include <j/core/ApplicationCore.h>      // JPlatformWindow
#include <j/core/PlatformCommon.h>       // JPlatformWindowStyle
#include <j/core/GenesisComponents.h>    // JGuiApplication
#include <j/core/JComboBox.h>            // JComboBox (managed combo-dropdown popup)
#include <j/core/JColorButton.h>         // JColorButton (opens the colour picker dialog)
#include <j/core/JFontButton.h>          // JFontButton (opens the font picker dialog)
#include <j/core/JTitleBar.h>            // JTitleBar::draw (window title bar)
#include <j/core/JCloseButton.h>         // JCloseButton::draw/rectFor (window close control)
#include <j/core/DragDrop.h>             // JDragDrop — drag ghost overlay + drop-end repaint
#include <j/core/DockManager.h>          // JDockHost (auto-managed dock layout)
#include <j/core/DockSpace.h>            // JDockSpace (centre + 4 dock areas)
#include <j/core/DockRegistry.h>         // host registration for floating re-dock
#include <j/core/MenuSystem.h>           // JMenuBar, JMenuManager (accelerators)
#include <j/core/ShortcutMap.h>          // jShortcuts() — JAction / JKeySequence global accelerators
#include <j/core/MenuRuntime.h>          // JMenuRuntime (popup menu engine)
#include <j/core/FocusManager.h>         // JFocusManager (keyboard focus + Tab cycling)
#include <j/core/MainThreadDispatcher.h> // drain UI-thread callbacks (JTimer / JSerialPort / posts)
#include <j/core/Animation.h>            // jAnimator() — per-frame tween/transition registry
#include <j/core/FrameTimer.h>           // jTimers() — per-frame timer / deferred-call registry
#include <j/core/ToolBar.h>              // JToolBar
#include <j/core/StatusBar.h>            // JStatusBar (text + transient messages + widgets)
#include <j/core/Dialog.h>               // JDialogManager (native dialog request queue)
#include <j/platforms/PlatformWindow.h>  // createPlatformWindow
#include <j/platforms/PopupWindow.h>     // JPopupWindow (combo dropdown)
#include <j/platforms/ColorPickerDialog.h> // JColorPickerDialog (colour-button modal dialog)
#include <j/platforms/FontPickerDialog.h>  // JFontPickerDialog (font-button modal dialog)
#include <j/platforms/NativeDialogWindow.h>  // JNativeDialogWindow
#include <j/platforms/FileDialogWindow.h>    // JFileDialogWindow (in-app file/folder picker)
#include <j/core/JAiBus.h>                    // opt-in AI bus (introspect/drive over shared memory)
#include <cstdlib>                            // getenv (JF_AI_BUS opt-in)
#if defined(__linux__)
#  include <j/platforms/linux/FloatingDockWindow.h>   // tear-out / floating docks
#endif
#include <j/graphics/GpuHal.h>
#include <j/graphics/FontEngine.h>
#include <j/graphics/RenderPrimitive.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <thread>
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <map>

inline namespace jf {

class JAppWindow {
public:
    JAppWindow(const std::string& title, uint32_t width, uint32_t height)
        : m_title(title), m_w(width), m_h(height) {
        // Borderless: the OS draws no chrome — the framework draws its own frame.
        m_window = createPlatformWindow(title, width, height, 100, 100,
                                        JPlatformWindowStyle::Borderless);
        if (!m_window) return;
        m_window->setMinSize(200, 150);   // declare a sane minimum -> WM treats it resizable
        m_hal = JGpuHal::create(JGpuApiType::Vulkan, m_window->nativeHandle());
        if (m_hal) m_hal->resizeSwapchain(width, height);
        if (m_font.loadSystemFont() && m_hal) {
            auto atlas = m_font.buildAtlas(14.0f * m_window->dpiScale());
            JTextHelper::setAtlas(atlas);
            m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        }
        _wireSizedGlyphCache();

        // Route the OS clipboard through this window's native selection. JClipboard
        // is the single funnel: text widgets go via JWidget::clipboardGet/Set →
        // JClipboard, and direct JClipboard callers land here too.
        JPlatformWindow* w = m_window.get();
        JClipboard::s_setHook = [w](const std::string& s){ w->setClipboardText(s); };
        JClipboard::s_getHook = [w]{ return w->getClipboardText(); };
        JWidget::s_clipboardSet = [](const std::string& s){ JClipboard::setText(s); };
        JWidget::s_clipboardGet = []{ return JClipboard::getText(); };

        // AI bus: opt-in via env (JF_AI_BUS=1). Off by default → zero overhead. When on, the frame loop
        // publishes the widget snapshot + services actions over shared memory (see tick() below).
        if (const char* e = std::getenv("JF_AI_BUS"); e && *e && *e != '0')
            JAiBus::instance().enable();
    }

    bool valid() const { return m_window && m_hal; }

    JPlatformWindow& window() { return *m_window; }
    JGpuHal&         hal()    { return *m_hal; }
    void             requestClose() { m_window->requestClose(); }   // e.g. File▸Quit

    // Fired when a dock is dismissed via its title-bar close button (not the menu). The app uses this
    // to keep a View-menu visibility toggle in sync with a dock the user closed directly.
    std::function<void(JDockWidget*)> onDockClosed;

    // Interpose on window close (✕ / WM-close / File▸Quit → requestClose). Return true to let the window
    // close; return false to VETO it (the app is handling it — e.g. showing a Save-changes prompt, then
    // calling requestClose() again once the user chose). Unset → always close.
    std::function<bool()> onCloseRequest;

    // Change the whole-UI (application) font at runtime: reload the font file, rebuild the glyph atlas at the
    // given base size (scaled by DPI), repoint the text helper's metrics, and re-upload the atlas to the GPU.
    // A single global font (not simultaneous mixed fonts), so the one atlas is simply rebuilt. false on failure.
    bool setAppFont(const std::string& fontPath, float px = 14.0f) {
        if (!m_hal || fontPath.empty() || !m_font.loadFromFile(fontPath)) return false;
        auto atlas = m_font.buildAtlas(px * m_window->dpiScale());
        JTextHelper::setAtlas(atlas);
        m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        JTextHelper::invalidateSized();   // new face → stale size-specific glyph atlases
        m_appFontPx = px;
        return true;
    }
    // Reload the framework's built-in system face at px and rebuild the atlas — the "Default" choice in the
    // font picker. Same atlas-rebuild path as setAppFont, just from loadSystemFont() rather than a chosen file.
    bool setAppFontDefault(float px) {
        if (!m_hal || !m_font.loadSystemFont()) return false;
        auto atlas = m_font.buildAtlas(px * m_window->dpiScale());
        JTextHelper::setAtlas(atlas);
        m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        JTextHelper::invalidateSized();   // new size → stale size-specific glyph atlases
        m_appFontPx = px;
        return true;
    }
    // Rebuild the atlas from the ALREADY-loaded font at a new base size (px, DPI-scaled) — set the global
    // text scale without swapping the font file (the ctor's loadSystemFont() font stays). Avoids reloading
    // a file the loader might choke on; use when only the size should change. false if the HAL isn't ready.
    bool setAppFontSize(float px) {
        if (!m_hal) return false;
        auto atlas = m_font.buildAtlas(px * m_window->dpiScale());
        JTextHelper::setAtlas(atlas);
        m_hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        JTextHelper::invalidateSized();   // base font changed → drop stale size-specific glyph atlases
        m_appFontPx = px;
        return true;
    }

    // Wire JTextHelper's size-aware glyph cache to this window's font engine + GPU: rasterise a fresh
    // atlas at any requested pixel size, upload it as a resident glyph atlas, and free them all when the
    // base font changes. This is what makes pushTextScaled draw large text CRISP (a real glyph at that
    // size, Qt-style) instead of stretching the 14px base bitmap into lego bricks.
    void _wireSizedGlyphCache() {
        JTextHelper::s_buildSized = [this](const std::string& facePath, float px) -> JFontAtlas {
            uint32_t dim = static_cast<uint32_t>(std::ceil(px * 13.0f));   // room for the full glyph set at px
            dim = std::clamp(dim, 512u, 2048u);
            if (facePath.empty())
                return m_font.buildAtlas(px, dim, dim);                    // the app's default face
            // A specific family/style face: load the file once per path (JFontEngine is non-copyable, so it
            // lives in the node-based map), then bake at the requested size. Empty atlas on load failure →
            // JTextHelper falls back to the base atlas scaled.
            auto it = m_faceCache.find(facePath);
            if (it == m_faceCache.end()) {
                it = m_faceCache.try_emplace(facePath).first;
                if (!it->second.loadFromFile(facePath)) { m_faceCache.erase(it); return {}; }
            }
            return it->second.buildAtlas(px, dim, dim);
        };
        JTextHelper::s_uploadSized = [this](const JFontAtlas& a) -> uint32_t {
            return m_hal ? m_hal->createFontAtlas(a.bitmap.data(), a.width, a.height) : 0u;
        };
        JTextHelper::s_freeSized = [this]() { if (m_hal) m_hal->freeFontAtlases(); };
    }

    // Open the in-app font picker for the WHOLE-APP font (no external chooser). Lists the real installed
    // fonts; selecting one live-applies it (whole app re-renders); Cancel reverts to the font in effect when
    // it opened; Select commits and calls onCommit(path) so the caller can persist it (empty path = built-in
    // "Default"). Applies at the current app-font px, so the calibrated size is preserved.
    void openAppFontPicker(std::function<void(std::string,int)> onCommit = {}) {
        const std::string orig = m_font.path();          // revert target on Cancel
        const std::string startFam = jReadFontFamilyName(orig);   // seed the list to the active family (best-effort)
        const std::string startSpec = (startFam.empty() ? std::string("Default") : startFam)
                                    + "|" + std::to_string(static_cast<int>(m_appFontPx)) + "|0|0";   // seed size = current app size
        const int cx = m_window->screenX() + (static_cast<int>(m_w) - static_cast<int>(JFontPickerDialog::kW)) / 2;
        const int cy = m_window->screenY() + (static_cast<int>(m_h) - static_cast<int>(JFontPickerDialog::kH)) / 2;
        // Parent to the opener (e.g. the Preferences modal), not the main window, so the picker stacks
        // ABOVE the dialog that opened it rather than as an arbitrarily-ordered sibling of it.
        const auto parent = (JFontPickerDialog::NativeWinHandleType)(_parentForChildModal());
        auto dlg = std::make_shared<JFontPickerDialog>(
            startSpec, *m_hal, cx, cy, parent,
            std::function<void(std::string)>{});          // spec-accept unused in app-font mode
        const uintptr_t handle = _lastCreatedNativeId();   // the picker's own window id
        // Browsing no longer touches the app font (the picker previews the highlighted face only in its own
        // sample line). Select is the single commit point: apply the chosen face to the whole app, then persist.
        dlg->setAppFontMode([this, onCommit](std::string p, int sizePx) {
            const float fpx = static_cast<float>(sizePx);
            if (p.empty()) setAppFontDefault(fpx); else setAppFont(p, fpx);   // apply the chosen SIZE, not a fixed one
            if (onCommit) onCommit(p, sizePx);
        });
        setModalDialog([dlg](JGpuHal& h, JPrimitiveBuffer& bf) { return dlg->pollAndRender(h, bf); },
                       [dlg](JGpuHal& h) { dlg->destroySurface(h); }, handle);
    }
    int              windowX() const { return m_window->screenX(); }
    int              windowY() const { return m_window->screenY(); }
    void             setWindowPos(int x, int y) { m_window->setPosition(x, y); }
    uint32_t width()  const { return m_w; }
    uint32_t height() const { return m_h; }
    // Content y-origin: below the title bar + menu bar + toolbar.
    float    contentTop() const { return m_titleH + m_menuH + m_toolbarH; }

    // The window's dock host — build the layout on this; the runner auto-lays-it-out,
    // renders it, and routes input to it (no per-app plumbing). Empty = no docks.
    // The window's dock space: a protected centre framed by Left/Right/Top/Bottom areas,
    // each a full dock host. Build the layout on the areas; the runner lays it out, renders
    // it, routes input, and handles tear-out / re-dock across areas.
    JDockSpace& dockSpace() { return m_space; }

    // The window's central content — the widget that fills the protected centre (the four dock areas
    // frame it). Any JWidget; opt into a dock host here only if you actually want docks in the centre.
    void setCentralWidget(JWidget* w) { m_space.setCentralWidget(w); }

    // Install a generic app-owned modal dialog: `poll` is pumped each frame (renders to its own
    // surface) and returns false once the dialog has closed, at which point `destroy` is called.
    // The app owns the dialog object/lifetime; this only drives it. Used for Preferences, etc.
    void setModalDialog(std::function<bool(JGpuHal&, JPrimitiveBuffer&)> poll, std::function<void(JGpuHal&)> destroy,
                        uintptr_t nativeHandle = 0) {
        // The opener (main window, or a modal already on the stack) grabbed the pointer on the very
        // button-press that is opening this dialog, and — because it is about to be frozen out of the
        // pump (only the TOP of the stack polls) — will never see its own release to drop that grab.
        // A stale grab steals every mouse event this new modal should get (keyboard is unaffected, so
        // the modal would take keys but not the mouse). Release it here, at the single push choke point,
        // so modality is "passed along" by the framework rather than patched per-dialog.
#if defined(__linux__)
        JLinuxPlatformWindow::releaseActivePointerGrab();
#elif defined(_WIN32)
        JWindowsPlatformWindow::releaseActivePointerGrab();
#endif
        // nativeHandle = this modal's OWN window id; a nested child pushed later parents to it (see
        // _parentForChildModal) so the WM stacks the child above its opener rather than the root window.
        m_modalStack.push_back({ std::move(poll), std::move(destroy), nativeHandle });   // push: modals STACK, so a modal can open another (e.g. Axis Setup -> channel picker) without destroying its parent
    }

    // The parent window for a NEW modal: the modal currently on top of the stack (its opener) so the WM
    // stacks the child above it; or the main window when the stack is empty (a top-level modal). This is
    // how the framework "passes modality along" — a nested dialog is a child of whoever opened it.
    uintptr_t _parentForChildModal() const {
        return m_modalStack.empty() ? m_window->rawWindowId() : m_modalStack.back().handle;
    }
    // Native id of the window just constructed (a dialog creates its window in its ctor), so the modal
    // stack can remember each modal's own window for the parenting above. Platform-dispatched.
    static uintptr_t _lastCreatedNativeId() {
#if defined(__linux__)
        return JLinuxPlatformWindow::lastCreatedRawWindowId();
#elif defined(_WIN32)
        return JWindowsPlatformWindow::lastCreatedRawWindowId();
#else
        return 0;
#endif
    }

    // Construct an app modal dialog of type T, centred over this window, and drive it via the modal hook.
    // T's ctor is (args..., JGpuHal&, screenX, screenY, T::NativeWinHandleType) — any leading args (a data
    // list, a seed value, an on-accept callback) are forwarded before the fixed hal/pos/handle tail. T needs
    // static kW/kH + pollAndRender + destroySurface. The dialog lives for as long as it's open.
    template <typename T, typename... Args>
    void openModal(Args&&... args) {
        const int cx = m_window->screenX() + (static_cast<int>(m_w) - static_cast<int>(T::kW)) / 2;
        const int cy = m_window->screenY() + (static_cast<int>(m_h) - static_cast<int>(T::kH)) / 2;
        // Parent to the opener (top-of-stack modal, or the main window) so a nested modal stacks above it.
        auto dlg = std::make_shared<T>(std::forward<Args>(args)..., *m_hal, cx, cy,
                                       (typename T::NativeWinHandleType)(_parentForChildModal()));
        const uintptr_t handle = _lastCreatedNativeId();   // dlg just created its window -> its own id
        setModalDialog([dlg](JGpuHal& h, JPrimitiveBuffer& b) { return dlg->pollAndRender(h, b); },
                       [dlg](JGpuHal& h) { dlg->destroySurface(h); }, handle);
    }

    // The window's menu bar — built lazily on first call (reserves a 28px strip below the
    // title bar and wires the popup engine). Add JMenu* with addMenu(); the app owns the
    // JMenu objects. Returns the bar so the app just declares menus.
    JMenuBar& menuBar() {
        if (!m_menuBar) {
            auto& g = JGuiApplication::instance()->sceneGraph();
            m_menuBar = std::make_unique<JMenuBar>(g);
            m_menuBar->onQueryScreenPos = [this](float lx, float ly) -> std::pair<int,int> {
                return { m_window->screenX() + static_cast<int>(lx),
                         m_window->screenY() + static_cast<int>(ly) };
            };
            m_menuRuntime.wire(m_hal.get(),
                               (JPopupWindow::NativeWinHandleType)(m_window->rawWindowId()),
                               m_menuBar.get());
            m_menuH = JStyle::current().menuItemHeight + 4.f;   // menu bar scales with the scheme row height
            layoutDocks();
        }
        return *m_menuBar;
    }

    // Status bar (reserves a strip at the bottom). setStatusText sets the PERMANENT text; showStatus
    // shows a transient message that auto-reverts after `ms` (0 = until replaced). statusBar() exposes
    // the bar itself (e.g. to host a right-aligned widget). Any of these lazily reserves the strip.
    void setStatusText(std::string s) { ensureStatusBar(); m_statusBar.setText(std::move(s)); }
    void showStatus(std::string s, int ms = 0) { ensureStatusBar(); m_statusBar.showMessage(std::move(s), ms); }
    // A right-aligned live readout the bar refreshes itself: `provider` returns the current text
    // (return "" to show nothing), re-asked every `ms`. The bar owns the timer — the app just
    // supplies the string. See JStatusBar::setLiveText.
    void setLiveStatus(std::function<std::string()> provider, int ms = 250) {
        ensureStatusBar(); m_statusBar.setLiveText(std::move(provider), ms);
    }
    JStatusBar& statusBar() { ensureStatusBar(); return m_statusBar; }

    // The window's toolbar — built lazily (reserves a 40px strip below the menu bar). Add
    // buttons with addButton(label, onClick); the framework lays it out, renders it, and
    // routes input. Returns the bar so the app just declares buttons.
    JToolBar& toolBar() {
        if (!m_toolBar) { m_toolBar = std::make_unique<JToolBar>(); m_toolbarH = JStyle::current().buttonHeight + 8.f; layoutDocks(); }   // toolbar scales with the scheme button height
        return *m_toolBar;
    }
    // Override the toolbar height (e.g. to host a tall icon widget). Call after toolBar().
    void setToolbarHeight(float h) { if (m_toolBar) { m_toolbarH = h; layoutDocks(); } }

    // The window's keyboard-focus manager. The runner owns focus: it rebuilds the tab order
    // from the live widget set each frame, focuses the widget under a click, cycles focus on
    // Tab/Shift-Tab, and routes key events to the focused widget — the app registers nothing.
    // Exposed so the app can query/set focus (e.g. focus a field programmatically).
    JFocusManager& focus() { return m_focus; }

    // App hooks.
    std::function<void(JPrimitiveBuffer&)>              onRender;  // draw content (below contentTop())
    std::function<void(float,float,bool,bool)>          onInput;   // mx,my,pressed,released (chrome-filtered)
    std::function<void(uint32_t,uint32_t)>              onResize;  // new client size (px)
    std::function<void(const JKeyEvent&)>               onKey;     // key events not consumed by menu/focus/Tab

    void requestRedraw() { m_needRedraw = true; }   // app asks for a frame (e.g. animation)

    // Off-screen capture: a tool (e.g. a SIGUSR1 handler) sets s_captureRequest; the run loop grabs the next
    // frame's swapchain to s_capturePath (a PPM). Lets the Vulkan-rendered UI be inspected headlessly.
    static inline std::atomic<bool> s_captureRequest{false};
    static inline const char*       s_capturePath = "/tmp/studio_shot";   // prefix → <prefix>_<surfaceId>.ppm per surface

    int run() {
        if (!valid()) return -1;
        m_window->setVSync(true);
        layoutDocks();
        // The runner owns combo dropdowns: any Popup-mode combo opens/closes through us.
        JComboBox::onOpenPopupHook = [this](JComboBox* cb){ openComboDropdown(cb); };
        JColorButton::onOpenPickerHook = [this](JColorButton* b){ openColorPicker(b); };
        JFontButton::onOpenPickerHook  = [this](JFontButton* b){ openFontPicker(b); };
        int   redraw = 4;                  // frames left to render (armed by activity)
        float lastMx = -1.f, lastMy = -1.f;
        auto  lastFrameTime = std::chrono::steady_clock::now();   // for per-frame animation delta
        while (true) {
            m_window->pollNativeEvents();
            // Close interposition: if a close is pending and the app vetoes (e.g. it's showing a Save
            // prompt), clear the request and keep running so the prompt can render + resolve.
            if (m_window->shouldClose()) {
                if (!onCloseRequest || onCloseRequest()) break;
                m_window->clearCloseRequest();
            }
            bool activity = false;

            // Off-screen capture request (set by a signal handler / tool): grab THIS frame's swapchain to a
            // PPM so the rendered output can be inspected even though the Vulkan surface isn't X-readable.
            if (s_captureRequest.exchange(false)) { m_hal->captureAllNextFrame(s_capturePath); activity = true; m_needRedraw = true; }

            // Drain callbacks posted to the UI thread (JTimer ticks, JSerialPort data, async
            // posts) so framework primitives that marshal to the main thread actually fire under
            // the runner. Any work done arms a redraw so the UI reflects it this frame.
            if (JMainThreadDispatcher::instance().drain() > 0) activity = true;

            // Animation tick: advance every registered tween by the real wall-clock delta
            // since the previous frame, then reap finished ones. While anything is active we
            // arm a redraw so the animation keeps producing frames (event-driven presenter).
            {
                const auto now = std::chrono::steady_clock::now();
                float dtMs = std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
                lastFrameTime = now;
                if (dtMs > 100.0f) dtMs = 100.0f;   // clamp long idles so tweens don't jump
                jAnimator().tick(dtMs);
                if (jAnimator().hasActive()) activity = true;
                // Frame-ticked timers ride the SAME dt: fire due timeouts / deferred calls
                // and arm a repaint when anything fired so timer-driven UI updates present.
                if (jTimers().tick(dtMs)) activity = true;
            }

            // Sync the swapchain to the window's CURRENT size every iteration, by direct
            // comparison rather than the resize flag (which can lag/coalesce). If the
            // swapchain ever trails the window by a frame, the newly exposed edge flashes
            // — the main source of resize flicker. width()/height() keep them in lockstep.
            m_window->consumeWasResized();   // drain the flag; we resize by size-compare
            const uint32_t curW = m_window->width(), curH = m_window->height();
            if (curW > 0 && curH > 0 && (curW != m_w || curH != m_h)) {
                m_w = curW; m_h = curH;
                m_hal->resizeSwapchain(m_w, m_h);
                layoutDocks();
                if (onResize) onResize(m_w, m_h);
                activity = true;
            }

            float mx = m_window->mouseX(), my = m_window->mouseY();
            // While the left button is held, the pointer is grabbed (so drags can leave the
            // window). On some WMs/compositors MotionNotify stops updating during that grab,
            // which freezes mx/my mid-drag and makes tear-out/threshold detection erratic
            // (drags that never initiate, "invisible" drags). Poll the global cursor instead —
            // XQueryPointer works under a grab — so the main window tracks drags reliably,
            // matching how floating windows already track. event_x is window-relative, and so
            // is (global - windowOrigin), so the two coordinate spaces agree.
            if (m_leftHeld || JDragDrop::isDragging()) {   // also track a drag begun in a floating dock
                auto [gx, gy] = m_window->globalCursorPos();
                mx = static_cast<float>(gx - m_window->screenX());
                my = static_cast<float>(gy - m_window->screenY());
            }
            m_mx = mx; m_my = my;   // remembered for chrome hover in drawChrome()
            // Resize-cursor feedback — window edges first, then dock splitters. Both are
            // framework-managed: the app never sets a resize cursor itself.
            // Window-edge resize cursor only when not maximized (no edges to drag then),
            // but dock-splitter cursors apply either way.
            JPlatformCursor c = m_window->isMaximized()
                                    ? JPlatformCursor::Default
                                    : cursorForDir(resizeDirAt(mx, my));
            if (c == JPlatformCursor::Default) {
                switch (m_space.getHoverCursor(mx, my)) {
                    case JDockHost::JHoverCursor::Horiz: c = JPlatformCursor::ResizeLeftRight; break;
                    case JDockHost::JHoverCursor::Vert:  c = JPlatformCursor::ResizeUpDown;    break;
                    default: break;
                }
            }
            m_window->setCursor(c);
            const bool pressed     = m_window->consumePress();
            const bool releasedRaw = m_window->consumeRelease();
            // Fundamental drag-release routing. While a JDragDrop is active, the button-release belongs to
            // drop resolution ALONE. Resolve it here — deliver to the drop target under the cursor (the
            // central surface), else cancel — and then WITHHOLD the release from every downstream handler
            // below (chrome, menu bar, toolbar, status bar, dock splitters, dock content, central routing).
            // One choke point, so no chrome element can steal the drop; replaces the per-consumer patches.
            const bool dragActive = JDragDrop::isDragging();
            if (releasedRaw && dragActive) {
                if (!m_space.isResizing())
                    if (JWidget* cw = m_space.centralWidget()) {
                        const JRect& cr = m_space.centerRect();
                        if (mx >= cr.x && mx < cr.x + cr.width && my >= cr.y && my < cr.y + cr.height) cw->handleMouseRelease(mx, my);
                    }
                // Cancel unless a drop target actually CONSUMED the payload (accept() clears the session).
                // Being merely over the centre is not enough — a release on the tab strip, in the wrong
                // mode, or in the abyss must never leave the drag ghost stuck to the cursor.
                if (JDragDrop::isDragging()) JDragDrop::cancel();
                m_needRedraw = true;
            }
            // Downstream widget routing never sees a release that a drag has claimed (physical-state
            // tracking below still uses releasedRaw so the button state stays honest).
            const bool released = releasedRaw && !dragActive;
            JWidget::s_ctrlDown  = m_window->isCtrlDown();    // publish modifier state for handleMousePress
            JWidget::s_shiftDown = m_window->isShiftDown();
            // App-tracked button state: more reliable than querying the server's button mask
            // (which some compositors / synthetic-input paths don't report). Drives the
            // poll-the-global-cursor branch above so drags track even while the pointer is
            // grabbed and MotionNotify goes quiet.
            m_leftHeld = (m_leftHeld || pressed) && !releasedRaw;
            if (pressed || releasedRaw || mx != lastMx || my != lastMy) activity = true;
            // While a button is held (drag / WM resize), keep the loop live every frame —
            // never sleep through a drag. This matters because we poll the global cursor (above)
            // rather than relying on MotionNotify (which can go quiet under the press grab); if
            // the loop slept, the poll would miss the motion and only see the cursor at release,
            // turning every drag into a click. m_leftHeld is app-tracked so it's reliable even
            // where the server's button mask / motion events aren't.
            if (m_leftHeld || m_window->isLeftButtonDown()) activity = true;
            lastMx = mx; lastMy = my;

            // Consume the frame's key events once — shared by drag-abort (Escape) and the
            // keyboard-focus routing below. Consuming twice would starve one of the two.
            auto frameKeys = m_window->consumeAllKeys();
            const float wheel = m_window->consumeWheel();
            if (wheel != 0.f) activity = true;

            const bool chromeAte = handleChrome(mx, my, pressed);
            const bool menusOpen = m_menuRuntime.hasOpenMenus();
            // While a menu popup is open it owns input modally (its own grab); the main UI is
            // frozen out so it can't fight the popup. The menu bar still gets hover so the
            // active title stays lit.
            bool menuAte = menusOpen;
            if (!chromeAte && m_menuBar) {
                // While a menu is open the popup has grabbed the pointer, so the main
                // window's mouse is frozen. Query the GLOBAL cursor instead (works under a
                // grab) so the menu bar can detect hover over a sibling title and switch to
                // it — the open menu follows the mouse along the bar.
                float bx = mx, by = my;
                if (menusOpen) {
                    auto [gx, gy] = m_window->globalCursorPos();
                    bx = static_cast<float>(gx - m_window->screenX());
                    by = static_cast<float>(gy - m_window->screenY());
                }
                m_menuBar->handleMouseMove(bx, by);
                if (!menusOpen && my >= m_titleH && my < m_titleH + m_menuH) {
                    if (pressed)  { m_menuBar->handleMousePress(mx, my); menuAte = true; }
                    if (released)   m_menuBar->handleMouseRelease(mx, my);
                }
            }
            if (m_toolBar && m_toolbarH > 0.f) {
                const float ty = m_titleH + m_menuH;
                m_toolBar->setRect(JRect{0.f, ty, static_cast<float>(m_w), m_toolbarH});
                const bool act = !chromeAte && !menusOpen;
                if (m_toolBar->handleMouse(mx, my, act && pressed, act && released) && !menusOpen)
                    menuAte = true;   // swallow input over the toolbar strip
            }
            if (m_statusH > 0.f) {
                const float sy = static_cast<float>(m_h) - m_statusH;
                m_statusBar.setRect(JRect{0.f, sy, static_cast<float>(m_w), m_statusH});
                const bool act = !chromeAte && !menusOpen;
                if (m_statusBar.handleMouse(mx, my, act && pressed, act && released) && !menusOpen)
                    menuAte = true;   // swallow input over the status strip (its hosted widgets)
            }
            // Right-click → open the context menu of the top-most widget under the cursor that has
            // one, reusing the floating-menu popup path (JMenuRuntime drives it modally). Consume
            // the flag every frame; only act when no menu is already open.
            const bool rightPressed = m_window->consumeRightPress();
            if (rightPressed && !menusOpen && !chromeAte) {
                for (auto it = JWidget::s_activeWidgets.rbegin(); it != JWidget::s_activeWidgets.rend(); ++it) {
                    JWidget* w = *it;
                    // prepareContextMenu FIRST so a widget can pick its menu from the click position (a surface
                    // sets the table/curve menu for whatever control is under the cursor), THEN check contextMenu().
                    if (w && w->isVisible() && w->hitTest(mx, my)) w->prepareContextMenu(mx, my);
                    if (w && w->isVisible() && w->contextMenu() && w->hitTest(mx, my)) {
                        if (JMenuManager::instance().onOpenMenu)
                            JMenuManager::instance().onOpenMenu(
                                w->contextMenu(),
                                m_window->screenX() + static_cast<int>(mx),
                                m_window->screenY() + static_cast<int>(my), false);
                        menuAte = true;
                        break;
                    }
                }
            }
            if (!chromeAte && !menuAte) {
                auto res = m_space.handleMouse(mx, my, pressed, released);
                if (res.ev && res.host) {
                    if (res.ev->type == JDockHost::JDockEvent::JType::WantsFloat)
                        spawnFloat(res.host, res.ev->dock);   // framework owns tear-out
                    else if (res.ev->type == JDockHost::JDockEvent::JType::CloseRequested) {
                        JDockWidget* closed = res.ev->dock;
                        if (closed == m_contentCapture) m_contentCapture = nullptr;   // don't keep capturing a gone dock
                        res.host->removeDock(closed);
                        if (onDockClosed) onDockClosed(closed);   // let the app sync its View-menu toggle, etc.
                    }
                }
            }
            // Focus-on-click: pressing a focusable widget focuses it (keyboard then routes
            // there); pressing empty space / non-focusable chrome clears focus. The app
            // registers nothing.
            if (pressed && !chromeAte && !menuAte)
                m_focus.focusAt(JWidget::s_activeWidgets, mx, my);
            if (onInput) onInput(mx, my, (chromeAte || menuAte) ? false : pressed, released);

            // Route content input (clicks / wheel / hover) to the dock under the cursor. The
            // framework hosts dock content, so a dock's onInputContent hook fires whether the
            // dock is inline here or torn into a float (spawnFloat bridges to the same hook).
            // Content input routes to the dock under the cursor — but a content press CAPTURES the stream to
            // that dock until the physical release, so a drag that leaves the dock (dragging a tree node onto a
            // surface) keeps feeding the source its motion. Without the capture the source stops getting moves
            // the instant the cursor exits its bounds, so it can never detect the leave to arm an external drag.
            if (!chromeAte && !menuAte) {
                JDockWidget* cd = m_contentCapture ? m_contentCapture : m_space.contentDockAt(mx, my);
                if (pressed && !m_contentCapture && cd && !m_space.isResizing()) m_contentCapture = cd;   // arm on the press's dock
                if (cd) cd->dispatchContentInput(mx, my, pressed, released, wheel);
            }
            if (releasedRaw) m_contentCapture = nullptr;   // physical release ends the content gesture

            // The centre is a plain widget (not a dock host), so route its input directly — but not
            // while a dock splitter is being dragged (its grab sits on the centre boundary, and would
            // otherwise also start a surface marquee/selection).
            if (!chromeAte && !menuAte && !m_space.isResizing())
                if (JWidget* cw = m_space.centralWidget()) {
                    const JRect& cr = m_space.centerRect();
                    if (mx >= cr.x && mx < cr.x + cr.width && my >= cr.y && my < cr.y + cr.height) {
                        cw->handleMouseMove(mx, my);
                        if (pressed)      cw->handleMousePress(mx, my);
                        if (released)     cw->handleMouseRelease(mx, my);
                        if (wheel != 0.f) cw->handleScroll(mx, my, wheel);
                    }
                }

            // General widget-to-widget drag (DragDrop.h / JMimeData). Runs AFTER the mouse
            // press/move/release routing above (which is where a source's startDrag() opens the
            // session) so an in-flight session routes enter/move/leave to the top-most accepting
            // widget under the cursor, and delivers onDrop on release. Uses its own jCurrentDrag()
            // state — independent of the JDragDrop typed-payload/ghost path handled elsewhere — and
            // consumes nothing from the runner, so it does not disturb focus/click routing.
            jDragTick(mx, my, pressed, released);

            // Keyboard: the runner owns focus routing. Re-sync the tab order from the live
            // widget set (so newly created / destroyed widgets are tracked without app
            // registration), then for each pressed key: menu accelerators first, then the
            // focused widget, then Tab/Shift-Tab focus cycling, then the app's onKey hook.
            // An open menu popup grabs the keyboard at the OS level, so its keys never arrive
            // here — no special-casing needed.
            m_focus.syncOrder(JWidget::s_activeWidgets);
            for (const auto& ke : frameKeys) {
                if (!ke.pressed) continue;
                activity = true;
                if (JMenuManager::instance().processAccelerator(ke)) continue;
                if (JWidget* f = m_focus.focused(); f && f->handleKeyEvent(ke)) continue;
                // The centre is a plain central widget (not in the focus order), so give it a shot at
                // keys nothing else consumed — e.g. the surface editor's Delete / arrows / Ctrl+Z. But
                // ONLY when nothing focusable holds focus (i.e. focus is on the canvas itself): a focused
                // dock field ignores keys it doesn't use (a line-edit drops Up/Down, a spin box Left/Right),
                // and those must NOT bleed onto the canvas and nudge/delete the selection behind the user.
                if (!m_focus.focused())
                    if (JWidget* cw = m_space.centralWidget(); cw && cw->handleKeyEvent(ke)) continue;
                // Global action/shortcut accelerators: a focused widget (e.g. a text field) has
                // already had first refusal above, so typing still works; anything it didn't
                // consume is offered to registered JActions / standalone chords (like the menu
                // accelerator pass, but for the shared command registry). Consumed = stop routing.
                if (jShortcuts().dispatch(ke)) continue;
                if (ke.key == JKeyEvent::JKey::Tab)     { m_focus.nextFocus(); continue; }
                if (ke.key == JKeyEvent::JKey::BackTab) { m_focus.prevFocus(); continue; }
                if (onKey) onKey(ke);
            }

            if (auto* app = JGuiApplication::instance()) app->serviceFrame();

            // Floating docks (tear-out): keep the host's screen rect current for re-dock
            // hit-testing, then poll/render/re-dock/close each floating window.
            m_space.registerAll(m_window->screenX(), m_window->screenY());
#if defined(__linux__)
            // Escape reverts an in-progress dock drag to its exact pre-drag state. A float
            // may hold keyboard focus (it flags consumeAbortRequest) or the main window may
            // (poll its keys while dragging) — either way the host owns the revert.
            bool escAbort = false, anyDragging = false;
            for (auto& fd : m_floating) {
                if (fd.isDragging()) anyDragging = true;
                if (fd.consumeAbortRequest()) escAbort = true;
            }
            if (anyDragging)
                for (const auto& k : frameKeys)
                    if (k.key == JKeyEvent::JKey::Escape) escAbort = true;
            if (escAbort) revertActiveDrag();
#endif
            serviceFloats();
#if defined(__linux__)
            if (!m_floating.empty()) activity = true;   // keep polling while docks float
#endif
            // Re-derive the area layout each frame so tear-out/re-dock changes take effect —
            // an emptied area collapses and the centre reclaims its space.
            m_space.computeLayout({0.f, contentTop(), static_cast<float>(m_w),
                                   static_cast<float>(m_h) - contentTop() - m_statusH});

            // AI bus (opt-in): one main-thread call — service a pending action, publish the live snapshot.
            // No-op unless JF_AI_BUS enabled it. A serviced action forces a repaint so its effect shows.
            if (JAiBus::instance().tick(JWidget::s_activeWidgets, &m_focus)) activity = true;

            // Menu popups: poll (modal grab / dismiss-outside) + render their own surfaces.
            if (m_menuBar) {
                m_menuRuntime.updateAndRender(*m_hal);
                if (m_menuRuntime.hasOpenMenus()) activity = true;
            }
            // Combo dropdown + native dialogs — their own surfaces, outside the main frame.
            if (serviceComboAndDialogs()) activity = true;

            // Active drag: track the cursor for the ghost, keep the frame alive while held, and resolve the
            // drop on button-release. A drag begun in a FLOATING dock grabs its release in the float, so the
            // main window's press/release never fire — poll the real button state and, when it goes up with
            // a drag still active, deliver the release to the surface (if the cursor is over the centre) or
            // cancel (so the ghost can't stay stuck to the cursor until a click). Also force one more frame
            // when the drag ends so the drop's result repaints immediately.
            // Trail the drag ghost every frame. The drop itself is resolved authoritatively on release —
            // by the choke point above for an inline drag, or by _resolveFloatDrop for a floating-dock
            // drag (a genuinely separate window) — so there is no button-polling drop path here.
            const bool dragging = JDragDrop::isDragging();
            if (dragging) { JDragDrop::update(mx, my); activity = true; }
            if (m_wasDragging && !JDragDrop::isDragging()) activity = true;   // one more frame so the drop's result paints
            m_wasDragging = JDragDrop::isDragging();

            if (m_needRedraw) { activity = true; m_needRedraw = false; }
            if (activity) redraw = 4;

            // Event-driven: present only while armed, idle otherwise. Continuously
            // presenting static content flickered under the compositor — the toolkit's
            // own loop renders in bursts and stays solid, so we do the same.
            if (redraw > 0) {
                --redraw;
                JPrimitiveBuffer buffer;
                auto frame = m_hal->beginFrame();
                drawChrome(buffer);
                drawBars(buffer);                           // menu bar / toolbar / status bar
                m_space.render(buffer);                     // all areas: content + overlays
                if (dragging) _drawDragGhost(buffer);       // floating "what you're holding" label
                if (onRender) onRender(buffer);
                m_hal->drawPrimitives(buffer);
                m_hal->submitAndPresentFrame(frame);
                m_window->swapBuffers();   // frame presented -> echo _NET_WM_SYNC_REQUEST
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
        m_hal->waitIdle();
        return 0;
    }

private:
    static constexpr float kBtnW = 28.0f;   // width of each window-control button (toolkit value)

    // Resize direction under (mx,my): _NET_WM_MOVERESIZE dirs 3=R,4=BR,5=B,6=BL,7=L, or
    // -1. Top edge is intentionally skipped — it conflicts with the title bar (catalog).
    int resizeDirAt(float mx, float my) const {
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h);
        constexpr float kEdge = 6.0f, kCorn = 14.0f;
        const bool onLeft   = mx < kEdge   && my >= m_titleH;
        const bool onRight  = mx >= W-kEdge && my >= m_titleH;
        const bool onBottom = my >= H-kEdge;
        const bool onBL     = mx < kCorn   && my >= H-kCorn;
        const bool onBR     = mx >= W-kCorn && my >= H-kCorn;
        if (onBL)     return 6;
        if (onBR)     return 4;
        if (onBottom) return 5;
        if (onLeft)   return 7;
        if (onRight)  return 3;
        return -1;
    }

    void ensureStatusBar() { if (m_statusH == 0.f) { m_statusH = 24.f; layoutDocks(); } }

    void layoutDocks() {
        const float top = contentTop();
        m_space.computeLayout({0.f, top, static_cast<float>(m_w),
                               static_cast<float>(m_h) - top - m_statusH});
        if (m_menuBar) {
            auto& g = JGuiApplication::instance()->sceneGraph();
            g.getLayout(m_menuBar->getNodeId()).boundingBox = {0.f, m_titleH, static_cast<float>(m_w), m_menuH};
            g.invalidateNode(m_menuBar->getNodeId(), DirtySelf);
            g.computeLayout(m_menuBar->getNodeId(),
                            {static_cast<float>(m_w), static_cast<float>(m_w), m_menuH, m_menuH});
        }
    }

    // Draw the menu bar / toolbar / status bar strips into the main frame.
    void drawBars(JPrimitiveBuffer& buf) {
        const float W = static_cast<float>(m_w);
        uint8_t bg[4]  = {Colors::Surface1[0], Colors::Surface1[1], Colors::Surface1[2], 255};
        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], Colors::Border[3]};
        if (m_menuBar) {
            buf.pushRectangle(0.f, m_titleH, W, m_menuH, bg, 0.f);
            buf.pushRectangle(0.f, m_titleH + m_menuH - 1.f, W, 1.f, sep, 0.f);
            m_menuBar->populateRenderPrimitives(buf);
        }
        if (m_toolBar && m_toolbarH > 0.f) {
            const float y = m_titleH + m_menuH;
            buf.pushRectangle(0.f, y, W, m_toolbarH, bg, 0.f);
            buf.pushRectangle(0.f, y + m_toolbarH - 1.f, W, 1.f, sep, 0.f);
            m_toolBar->setRect(JRect{0.f, y, W, m_toolbarH});
            m_toolBar->render(buf);
        }
        if (m_statusH > 0.f) {
            const float y = static_cast<float>(m_h) - m_statusH;
            buf.pushRectangle(0.f, y, W, m_statusH, bg, 0.f);
            buf.pushRectangle(0.f, y, W, 1.f, sep, 0.f);   // separator at the top edge
            m_statusBar.setRect(JRect{0.f, y, W, m_statusH});
            m_statusBar.render(buf);
        }
    }

    // Tear a dock out of the host into its own floating window (the host only emits
    // WantsFloat for docks the app declared floatable).
    void spawnFloat(JDockHost* host, JDockWidget* dw) {
#if defined(__linux__)
        if (!host || !dw) return;
        const JDockNode* n = host->node(host->findDock(dw));
        if (!n) return;
        const JRect r = n->rect;
        // Tear out at the dock's CURRENT docked size (its leaf rect) — so a panel resized
        // while docked floats at that size. (Re-docking then uses the float's own size,
        // which the float keeps in sync as it's resized.)
        const float fw = std::max(r.width,  dw->minW());
        const float fh = std::max(r.height, dw->minH());
        const int sx = m_window->screenX() + static_cast<int>(r.x);
        const int sy = m_window->screenY() + static_cast<int>(r.y);
        auto [gx, gy] = m_window->globalCursorPos();
        const int offX = std::clamp(gx - sx, 0, static_cast<int>(fw) - 1);
        const int offY = std::clamp(gy - sy, 0, static_cast<int>(fh) - 1);

        // Never tear out unless the button is actually held: if the drag-gesture already ended
        // (button up), spawning a float would remove the dock into a window that can't be
        // dragged and instantly commits/closes — the dock just vanishes. Leave it docked.
        if (!m_window->isLeftButtonDown()) return;
        m_revert = { true, host->saveTree(), host };   // enable Escape-to-revert this drag
        host->removeDock(dw);
        // BORROW the app-owned dock into the float — the object is NOT moved, so &dw stays the one true
        // dock. The saved revert tree references &dw and remains valid; no husk, no retarget.
        dw->setPosition(0.f, 0.f);
        dw->setSize(fw, fh);
        m_floating.emplace_back(dw, sx, sy,
                                static_cast<uint32_t>(fw), static_cast<uint32_t>(fh),
                                offX, offY, *m_hal, /*initialDrag=*/true,
                                JFloatingDockOptions{},
                                (JFloatingDockWindow::NativeWinHandleType)(m_window->rawWindowId()));
        // Bridge the float's content input (which carries the wheel) to the torn dock's
        // onInputContent hook. Content RENDER needs no bridge: the float's internal host
        // renders through the same _renderLeaf path, invoking the dock's onRenderContent.
        {
            JDockHost* fhost = &m_floating.back().dockHost();
            m_floating.back().setContentInputHost(
                [this, fhost](float x, float y, bool p, bool r, float w) {
                    if (JDockWidget* d = fhost->contentDockAt(x, y))
                        d->dispatchContentInput(x, y, p, r, w);
                    // A content drag (Dictionary binding / palette control) whose button releases inside a
                    // FLOATING dock: the main window never sees this gesture's release, so resolve the drop
                    // on the main surface here — at the global cursor — instead of leaving it stuck to the
                    // cursor until a click.
                    if (r && JDragDrop::isDragging()) _resolveFloatDrop();
                });
        }
        // The drag now lives in the floating window; the main window won't see this gesture's
        // button-release, so drop the capture and held-state here rather than leaving them stuck.
        m_space.releaseMouseCapture();
        m_leftHeld = false;
#else
        (void)host; (void)dw;
#endif
    }

    // Escape-to-revert: restore the host to its exact pre-drag tree and re-home the dock,
    // undoing the tear-out (instead of leaving it floating mid-flight).
    void revertActiveDrag() {
#if defined(__linux__)
        if (!m_revert.active) return;
        for (auto it = m_floating.begin(); it != m_floating.end(); ++it) {
            if (!it->isInInitialDrag()) continue;
            // The float only BORROWED the dock — the object never moved — so the saved tree still points
            // at the live dock. Relinquish the float's hold (keeping any framework-owned dock alive at its
            // unchanged address), drop the float, then restore the source host's pre-drag tree verbatim.
            // No retarget: nothing moved.
            std::unique_ptr<JDockWidget> owned = it->releasePrimary();
            it->destroySurface(*m_hal);
            m_floating.erase(it);
            if (m_revert.host) m_revert.host->restoreTree(m_revert.tree);
            if (owned) m_ownedDocks.push_back(std::move(owned));
            layoutDocks();
            break;
        }
        m_revert.active = false;
#endif
    }

    // Resolve an active content-drag (Dictionary binding / palette control) when its button releases
    // inside a floating dock: deliver the release to the main central widget at the global cursor (if the
    // cursor is over the centre) so it drops there; else cancel so the ghost can't hang until a click.
    void _resolveFloatDrop() {
        if (!JDragDrop::isDragging()) return;
        auto [gx, gy] = m_window->globalCursorPos();
        const float mx = static_cast<float>(gx - m_window->screenX());
        const float my = static_cast<float>(gy - m_window->screenY());
        if (!m_space.isResizing())
            if (JWidget* cw = m_space.centralWidget()) {
                const JRect& cr = m_space.centerRect();
                if (mx >= cr.x && mx < cr.x + cr.width && my >= cr.y && my < cr.y + cr.height) cw->handleMouseRelease(mx, my);
            }
        if (JDragDrop::isDragging()) JDragDrop::cancel();   // consumed → session cleared; else never leave it stuck
        m_needRedraw = true;
    }

    // Per-frame: drive each floating window — move/drag, re-dock to the host on drop,
    // close, and render to its own surface.
    void serviceFloats() {
#if defined(__linux__)
        // A settled (released/committed) tear-out is no longer revertible.
        bool anyInitial = false;
        for (auto& fd : m_floating) if (fd.isInInitialDrag()) { anyInitial = true; break; }
        if (!anyInitial) m_revert.active = false;

        for (auto it = m_floating.begin(); it != m_floating.end(); ) {
            auto pr = it->pollAndMove();
            if (pr.type == JFloatingDockWindow::JPollResult::JType::CommitDrop &&
                pr.dropHost && pr.dropHost->tryCommitDrop()) {
                // tryCommitDrop already inserted the dock's stable pointer into the drop host; the object
                // never moved, so that pointer is correct — no retarget. Relinquish the float's hold: keep a
                // framework-owned dock alive in our pool at its unchanged address; a borrowed app dock needs
                // nothing (its owner still holds it).
                it->destroySurface(*m_hal);
                if (std::unique_ptr<JDockWidget> owned = it->releasePrimary())
                    m_ownedDocks.push_back(std::move(owned));
                it = m_floating.erase(it);
                continue;
            }
            if (it->shouldClose()) { it->destroySurface(*m_hal); it = m_floating.erase(it); continue; }
            JPrimitiveBuffer fbuf;
            it->render(*m_hal, fbuf);
            ++it;
        }
#endif
    }

    // Open (or toggle/replace) a Popup-mode combo's dropdown below the combo. Installed as
    // JComboBox::onOpenPopupHook so any combo just works — the app wires nothing.
    void openComboDropdown(JComboBox* cb) {
        if (m_comboPopup && m_comboOwner == cb) {   // click same combo again → close
            m_comboPopup->destroySurface(*m_hal); m_comboPopup.reset(); m_comboOwner = nullptr; return;
        }
        if (m_comboPopup) { m_comboPopup->destroySurface(*m_hal); m_comboPopup.reset(); }

        const auto bb = cb->getBoundingBox();
        // Anchor to the combo's OWN window: a control in a modal dialog lives in that dialog's scene graph,
        // which declares its live screen origin + native handle. Fall back to this (main) window when unset.
        const auto& host = cb->sceneGraph().hostWindow();
        const int wsx = host.set ? host.screenX : m_window->screenX();
        const int wsy = host.set ? host.screenY : m_window->screenY();
        const auto parent = host.set ? (JPopupWindow::NativeWinHandleType)(host.nativeHandle)
                                     : (JPopupWindow::NativeWinHandleType)(m_window->rawWindowId());
        const int sx = wsx + static_cast<int>(bb.x);
        const int sy = wsy + static_cast<int>(bb.y + bb.height);
        const auto popupW = static_cast<uint32_t>(bb.width);
        auto popup = std::make_unique<JPopupWindow>(sx, sy, popupW, 8, *m_hal,
                        JPopupWindow::JStyle::Borderless, parent);
        const auto& items = cb->items();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            auto* pi = popup->add<JPopupItem>(items[i], static_cast<float>(popupW), 28.f);
            pi->onActivated.connect([this, cb, i]() {
                cb->setCurrentIndex(i);
                m_comboCloseReq = m_comboPopup.get();   // defer close until after pollEvents returns
                m_comboOwner = nullptr;
            });
        }
        popup->computeNaturalHeight();
        m_comboPopup = std::move(popup);
        m_comboOwner = cb;
    }

    // JColorButton picker: open a JColorPickerDialog — a proper modal dialog window (title bar,
    // OK/Cancel, draggable), seeded with the button's current colour. The colour is applied to the
    // button only on OK; Cancel/close/Escape discard the edit. Installed as JColorButton::onOpenPickerHook.
    void openColorPicker(JColorButton* b) {
        if (m_colorDialog) { m_colorDialog->destroySurface(*m_hal); m_colorDialog.reset(); }

        const auto bb = b->getBoundingBox();
        // Anchor to the button's OWN window (a button in a modal dialog lives in that dialog's graph, which
        // declares its screen origin + handle) — fall back to this window when unset.
        const auto& host = b->sceneGraph().hostWindow();
        const int wsx = host.set ? host.screenX : m_window->screenX();
        const int wsy = host.set ? host.screenY : m_window->screenY();
        const auto parent = host.set ? (JColorPickerDialog::NativeWinHandleType)(host.nativeHandle)
                                     : (JColorPickerDialog::NativeWinHandleType)(m_window->rawWindowId());
        // The dialog opens on its palette page; anchor against that height (it grows for the editor).
        const int pw = static_cast<int>(JColorPickerDialog::kW), ph = static_cast<int>(JColorPickerDialog::paletteH());
        const auto [sw, sh] = m_window->virtualDesktopSize();
        // Anchor below the button, flipping above if it would run off the bottom; clamp on-screen.
        const int btnBot = wsy + static_cast<int>(bb.y + bb.height);
        const int btnTop = wsy + static_cast<int>(bb.y);
        int sx = wsx + static_cast<int>(bb.x);
        int sy = (btnBot + ph <= static_cast<int>(sh)) ? btnBot : btnTop - ph;
        sx = std::clamp(sx, 0, std::max(0, static_cast<int>(sw) - pw));
        sy = std::clamp(sy, 0, std::max(0, static_cast<int>(sh) - ph));

        JColorButton* target = b;
        m_colorDialog = std::make_unique<JColorPickerDialog>(b->colorHex(), *m_hal, sx, sy, parent,
            [target](const std::string& hex) { target->pick(hex); });   // apply on OK only
    }

    // JFontButton picker: open a font dialog centred over the window, seeded with the button's spec;
    // applies to the button only on OK. Driven via the generic modal hook (setModalDialog).
    void openFontPicker(JFontButton* b) {
        // Position + parent against the button's OWN window (a button in a modal dialog declares its host in
        // its scene graph); fall back to centring over this window.
        const auto& host = b->sceneGraph().hostWindow();
        int cx, cy;
        typename JFontPickerDialog::NativeWinHandleType parent;
        if (host.set) {
            cx = host.screenX + 40; cy = host.screenY + 40;
            parent = (JFontPickerDialog::NativeWinHandleType)(host.nativeHandle);
        } else {
            cx = m_window->screenX() + (static_cast<int>(m_w) - static_cast<int>(JFontPickerDialog::kW)) / 2;
            cy = m_window->screenY() + (static_cast<int>(m_h) - static_cast<int>(JFontPickerDialog::kH)) / 2;
            parent = (JFontPickerDialog::NativeWinHandleType)(m_window->rawWindowId());
        }
        JFontButton* target = b;
        auto dlg = std::make_shared<JFontPickerDialog>(b->fontSpec(), *m_hal, cx, cy, parent,
            [target](std::string spec) { target->pick(spec); });
        const uintptr_t handle = _lastCreatedNativeId();   // the picker's own window id (for nested-modal parenting)
        setModalDialog([dlg](JGpuHal& h, JPrimitiveBuffer& bf) { return dlg->pollAndRender(h, bf); },
                       [dlg](JGpuHal& h) { dlg->destroySurface(h); }, handle);
    }

    // Per-frame: poll/render the open combo dropdown, and drain the JDialogManager queue into
    // native dialog windows then poll/render them. Overlays own their surfaces (own beginFrame),
    // so this runs OUTSIDE the main window's frame. Returns true while any overlay is active.
    bool serviceComboAndDialogs() {
        JPrimitiveBuffer scratch;   // overlays render to their OWN surfaces; this is scratch
        if (m_comboPopup) {
            auto res = m_comboPopup->pollEvents(*m_hal);
            const bool close = res.type == JPopupWindow::JPollResult::JType::Dismissed
                               || m_comboCloseReq == m_comboPopup.get();
            m_comboCloseReq = nullptr;
            if (close) { m_comboPopup->destroySurface(*m_hal); m_comboPopup.reset(); m_comboOwner = nullptr; }
            else if (m_comboPopup->isViewable()) m_comboPopup->render(*m_hal, scratch);
        }

        if (m_colorDialog) {
            if (!m_colorDialog->pollAndRender(*m_hal, scratch)) { m_colorDialog->destroySurface(*m_hal); m_colorDialog.reset(); }
        }

        // Generic app modals (Preferences, pickers, Axis Setup…) form a STACK: only the TOP is pumped each
        // frame. poll() returns false when it closed; then run its destroy hook and pop, resuming its parent.
        // Copy the top's poll out first — the call may push a child modal (realloc), which would otherwise
        // free the std::function mid-invocation; guard the pop on the stack not having grown during the poll.
        if (!m_modalStack.empty()) {
            const size_t idx = m_modalStack.size() - 1;   // the top being pumped
            auto poll = m_modalStack[idx].poll;           // copy: poll() may push a child (realloc) and would
                                                          // otherwise free this std::function mid-invocation
            if (!poll(*m_hal, scratch)) {
                // This modal is done — remove IT (idx), even if its poll opened a CHILD (sequential modals like
                // Open-ECU → Choose-a-tune: the parent returns false AND pushes; the child now sits above at
                // idx+1). Popping back() here would wrongly drop the child and leave the DONE parent lingering,
                // re-polled next frame against a torn-down state (the intermittent Open-ECU crash).
                if (idx < m_modalStack.size()) {
                    if (m_modalStack[idx].destroy) m_modalStack[idx].destroy(*m_hal);
                    m_modalStack.erase(m_modalStack.begin() + idx);
                }
            }
        }

        while (JDialogManager::instance().hasPending()) {
            const auto* req  = JDialogManager::instance().front();
            const auto& opts = req->options;
            const bool isFile = (req->kind == JDialogRequest::JKind::OpenFile ||
                                 req->kind == JDialogRequest::JKind::SaveFile ||
                                 req->kind == JDialogRequest::JKind::OpenFolder);
            const int dlgW = static_cast<int>(isFile ? JFileDialogWindow::kW : JNativeDialogWindow::kW);
            const int dlgH = static_cast<int>(isFile ? JFileDialogWindow::calcHeight()
                                                     : JNativeDialogWindow::calcHeight(req->kind, opts));
            const int cW = static_cast<int>(m_w), cH = static_cast<int>(m_h);
            const int wx = m_window->screenX(), wy = m_window->screenY();
            int dlgX, dlgY;
            using Pos = JDialogOptions::JPosition;
            switch (opts.position) {
            case Pos::Fixed:          dlgX = opts.x; dlgY = opts.y; break;
            case Pos::CenterOnScreen: { auto [sw, sh] = m_window->virtualDesktopSize(); dlgX = (sw - dlgW) / 2; dlgY = (sh - dlgH) / 2; break; }
            case Pos::AtCursor:       { auto [gx, gy] = m_window->globalCursorPos();     dlgX = gx; dlgY = gy; break; }
            case Pos::TopLeft:        dlgX = wx + 16;                 dlgY = wy + 16; break;
            case Pos::TopRight:       dlgX = wx + cW - dlgW - 16;     dlgY = wy + 16; break;
            case Pos::TopCenter:      dlgX = wx + (cW - dlgW) / 2;    dlgY = wy + 16; break;
            case Pos::BottomLeft:     dlgX = wx + 16;                 dlgY = wy + cH - dlgH - 16; break;
            case Pos::BottomRight:    dlgX = wx + cW - dlgW - 16;     dlgY = wy + cH - dlgH - 16; break;
            case Pos::BottomCenter:   dlgX = wx + (cW - dlgW) / 2;    dlgY = wy + cH - dlgH - 16; break;
            case Pos::CenterOnParent:
            default:                  dlgX = wx + (cW - dlgW) / 2;    dlgY = wy + (cH - dlgH) / 2; break;
            }
            if (isFile)
                m_fileDialogs.emplace_back(*req, *m_hal, dlgX, dlgY,
                    (JFileDialogWindow::NativeWinHandleType)(m_window->rawWindowId()));
            else
                m_dialogs.emplace_back(*req, *m_hal, dlgX, dlgY,
                    (JNativeDialogWindow::NativeWinHandleType)(m_window->rawWindowId()));
            JDialogManager::instance().pop();
        }
        for (auto it = m_dialogs.begin(); it != m_dialogs.end();) {
            if (!it->pollAndRender(*m_hal, scratch)) { it->destroySurface(*m_hal); it = m_dialogs.erase(it); }
            else ++it;
        }
        for (auto it = m_fileDialogs.begin(); it != m_fileDialogs.end();) {
            if (!it->pollAndRender(*m_hal, scratch)) { it->destroySurface(*m_hal); it = m_fileDialogs.erase(it); }
            else ++it;
        }
        return m_comboPopup != nullptr || m_colorDialog != nullptr || !m_modalStack.empty()
            || !m_dialogs.empty() || !m_fileDialogs.empty();
    }

    static JPlatformCursor cursorForDir(int d) {
        switch (d) {
            case 4: return JPlatformCursor::ResizeBottomRight;
            case 6: return JPlatformCursor::ResizeBottomLeft;
            case 5: return JPlatformCursor::ResizeUpDown;
            case 3: case 7: return JPlatformCursor::ResizeLeftRight;
            default: return JPlatformCursor::Default;
        }
    }

    // Hit-test the chrome on a fresh press; act on it. Returns true if it consumed the press.
    bool handleChrome(float mx, float my, bool pressed) {
        if (!pressed) return false;
        const float W = static_cast<float>(m_w);

        if (!m_window->isMaximized()) {
            const int dir = resizeDirAt(mx, my);
            if (dir >= 0) { m_window->startWindowResize(static_cast<uint32_t>(dir)); return true; }
        }

        // Title bar: window-control buttons (right-aligned), else drag.
        if (my < m_titleH) {
            if (mx >= W - kBtnW)     { m_window->requestClose(); return true; }
            if (mx >= W - 2 * kBtnW) { m_window->setMaximized(!m_window->isMaximized()); return true; }
            if (mx >= W - 3 * kBtnW) { m_window->minimize(); return true; }
            // Double-click the title bar → expand / restore (before starting a move-drag).
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - m_lastTitleMs < 400) { m_window->setMaximized(!m_window->isMaximized()); m_lastTitleMs = 0; return true; }
            m_lastTitleMs = nowMs;
            m_window->startWindowMove();
            return true;
        }
        return false;
    }

    // The toolkit's own title bar — matches controls_catalog exactly (flat 28px strip,
    // Colors palette, a 1px separator, and vector-drawn min/max/close with hover).
    void drawChrome(JPrimitiveBuffer& buf) {
        const float W  = static_cast<float>(m_w), H = static_cast<float>(m_h);

        // Opaque full-window background FIRST — every pixel is written each frame, so the
        // swapchain's images can't cycle stale content (that was the resize flicker).
        uint8_t bg[4] = {Colors::Surface0[0], Colors::Surface0[1], Colors::Surface0[2], 255};
        buf.pushRectangle(0.f, 0.f, W, H, bg, 0.f);

        // Title bar — the ONE shared primitive (square top for the app frame), reserving the 3 window buttons.
        JTitleBar::draw(buf, 0.f, 0.f, W, m_titleH, m_title, 0.f, 0, 10.f, kBtnW * 3.f + 20.f);

        uint8_t sep[4] = {Colors::Border[0], Colors::Border[1], Colors::Border[2], Colors::Border[3]};
        buf.pushRectangle(0.f, m_titleH - 1.f, W, 1.f, sep, 0.f);

        const float closeX = W - kBtnW, maxX = W - kBtnW * 2.f, minX = W - kBtnW * 3.f;
        auto hov = [&](float bx) {
            return m_my >= 0.f && m_my < m_titleH && m_mx >= bx && m_mx < bx + kBtnW;
        };

        // Minimize  −
        {
            if (hov(minX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(minX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = minX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 1.f;
            uint8_t ic[4] = {190,190,200,220};
            buf.pushRectangle(cx, cy, 9.f, 2.f, ic, 0.f);
        }
        // Maximize □ / restore ⊡
        {
            if (hov(maxX)) { uint8_t hb[4]={60,60,70,200}; buf.pushRectangle(maxX,0.f,kBtnW,m_titleH,hb,0.f); }
            float cx = maxX + kBtnW*0.5f - 4.f, cy = m_titleH*0.5f - 4.f;
            uint8_t ic[4]={190,190,200,220}, nf[4]={0,0,0,0}, wbg[4]={22,22,28,255};
            if (!m_window->isMaximized()) {
                buf.pushRectangle(cx, cy, 9.f, 9.f, nf, 1.f, 1.5f, ic);
            } else {
                buf.pushRectangle(cx+2.f, cy,     7.f, 7.f, nf,  0.f, 1.f, ic);
                buf.pushRectangle(cx,     cy+2.f,  7.f, 7.f, wbg, 0.f, 1.f, ic);
            }
        }
        // Close  × — the framework close-button primitive (rect derived from the title bar, no magic size)
        {
            const JRect cr = JCloseButton::rectFor({0.f, 0.f, W, m_titleH});
            JCloseButton::draw(buf, cr, hov(closeX));
        }
    }

    std::unique_ptr<JPlatformWindow> m_window;
    std::unique_ptr<JGpuHal>         m_hal;
    JDockSpace                       m_space;
    std::vector<std::unique_ptr<JDockWidget>> m_ownedDocks;   // docks adopted back from floats
#if defined(__linux__)
    std::vector<JFloatingDockWindow> m_floating;
#endif
    // Pre-drag tree snapshot for Escape-to-revert (active only during a tear-out drag).
    struct { bool active{false}; JDockHost::JSavedTree tree;
             JDockHost* host{nullptr}; } m_revert;

    std::unique_ptr<JMenuBar> m_menuBar;     // lazily created on menuBar()
    JMenuRuntime              m_menuRuntime; // popup menu engine
    JFocusManager             m_focus;       // keyboard focus + Tab order (auto-synced each frame)
    // Combo dropdown + native dialogs — overlay OS-windows the runner owns and services each
    // frame (like menu popups), so apps don't hand-roll JPopupWindow / JNativeDialogWindow.
    std::unique_ptr<JPopupWindow>    m_comboPopup;
    JComboBox*                       m_comboOwner{nullptr};
    std::unique_ptr<JColorPickerDialog> m_colorDialog;          // open colour dialog (own surface)
    // Generic app-modal STACK (Preferences, pickers, Axis Setup…): only the top is pumped, so one modal
    // can open another. Pushed by setModalDialog, popped when its poll returns false. Each entry also
    // carries the modal's own native window id so a nested child parents to its opener (_parentForChildModal).
    struct JModalEntry {
        std::function<bool(JGpuHal&, JPrimitiveBuffer&)> poll;
        std::function<void(JGpuHal&)>                    destroy;
        uintptr_t                                        handle{0};   // this modal's own native window id
    };
    std::vector<JModalEntry> m_modalStack;
    JPopupWindow*                    m_comboCloseReq{nullptr};
    std::vector<JNativeDialogWindow> m_dialogs;
    std::vector<JFileDialogWindow>   m_fileDialogs;
    std::unique_ptr<JToolBar> m_toolBar;     // lazily created on toolBar()
    float                     m_menuH{0.f}, m_toolbarH{0.f}, m_statusH{0.f};
    JStatusBar                m_statusBar;
    JFontEngine                      m_font;
    std::map<std::string, JFontEngine> m_faceCache;        // facePath → loaded engine for sized-face atlas bakes
    float                            m_appFontPx{14.0f};   // current app-font size — preview/revert reuse it
    std::string                      m_title;
    uint32_t                         m_w{0}, m_h{0};
    float                            m_mx{-1.0f}, m_my{-1.0f};
    int64_t                          m_lastTitleMs{0};   // last title-bar press (double-click → maximize)
    bool                             m_leftHeld{false};   // app-tracked button state (press→release)
    JDockWidget*                     m_contentCapture{nullptr};   // dock that owns the in-progress content press gesture
    float                            m_titleH{JStyle::current().titleBarHeight};   // one canonical title-bar height
    bool                             m_needRedraw{false};
    bool                             m_wasDragging{false};   // drag active last frame (repaint on drop)
    bool                             m_dragBtnWasDown{false};// button state last frame during a drag (release edge)

    // The floating "what you're holding" ghost during a JDragDrop: a small labelled chip trailing the
    // cursor, so a drag reads as a real object in hand rather than blind faith.
    void _drawDragGhost(JPrimitiveBuffer& buf) {
        const std::string lbl = JDragDrop::label();
        const float cx = JDragDrop::cursorX() + 12.f, cy = JDragDrop::cursorY() + 6.f;
        const float tw = JTextHelper::hasAtlas() ? JTextHelper::measureWidth(lbl) : lbl.size() * 7.f;
        const float w = tw + 16.f, h = JTextHelper::lineHeight() + 8.f;
        uint8_t bg[4] = {44, 48, 60, 235};
        buf.pushRectangle(cx, cy, w, h, bg, 5.f, 1.5f, Colors::Accent);
        if (JTextHelper::hasAtlas())
            JTextHelper::pushText(buf, cx + 8.f, cy + 4.f, lbl, Colors::TextPrimary, w - 14.f);
    }
};

}  // namespace jf
