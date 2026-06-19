#include <genesis/core/ApplicationCore.h>
#include <genesis/core/GenesisComponents.h>
#include <genesis/core/BaseWidgets.h>
#include <genesis/core/DockWidget.h>
#include <genesis/core/DockManager.h>
#include <genesis/core/DockRegistry.h>
#include <genesis/core/TranslationEngine.h>
#include <genesis/core/FocusManager.h>
#include <genesis/core/AccessibilityBridge.h>
#include <genesis/graphics/GpuHal.h>
#include <genesis/graphics/RenderPrimitive.h>
#include <genesis/graphics/FontEngine.h>
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#include <genesis/platforms/linux/FloatingDockWindow.h>

#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>

using namespace Genesis;

// ============================================================================
// Controls Catalog — showcases every Genesis widget with live rendering
// ============================================================================

class ControlsCatalog {
public:
    // AI node ids at/above this base are docked panels (offset past scene-graph node ids).
    static constexpr uint32_t kDockAiIdBase = 0x40000000u;

    explicit ControlsCatalog(SceneGraph& graph, uint32_t winW, uint32_t winH)
        : m_graph(graph), m_winW(winW), m_winH(winH) { buildUI(); }

    void update(float dt) {
        if (m_animPaused) return;          // frozen → nothing changes → no redraw needed
        m_elapsed += dt;
        if (m_progressBar)
            m_progressBar->setProgress(0.5f + 0.5f * std::sin(m_elapsed * 0.8f));
    }

    // True while something is visually animating and the frame must be redrawn
    // continuously.  Here only the demo progress bar animates; a real app would OR
    // together all active animators / transitions.
    bool isAnimating() const { return !m_animPaused && m_progressBar != nullptr; }
    void toggleAnimation()    { m_animPaused = !m_animPaused; }
    bool animationPaused() const { return m_animPaused; }

    void render(PrimitiveBuffer& buf) {
        // 1. Dock chrome (panel backgrounds, tab bars, borders) as the base layer.
        if (m_dockHost) m_dockHost->populateRenderPrimitives(buf);

        // 2. Each visible panel's content, clipped + scrolled within its viewport.
        for (auto& p : m_panels) {
            if (!p.visible) continue;
            buf.pushClip(p.viewport.x, p.viewport.y, p.viewport.width, p.viewport.height);
            for (Widget* w : p.widgets)
                if (w->isVisible()) w->populateRenderPrimitives(buf);
            buf.popClip();
        }

        // 3. Drag ghost + dock drop overlay on top of everything.
        if (m_tearableTab) m_tearableTab->populateDragGhost(buf);
        if (m_dockHost)    m_dockHost->populateOverlay(buf);
    }

    // Per-frame: place each docked panel's content at its content area, lay it out at the
    // panel width (controls stretch), measure height, clamp scroll, and apply the wheel.
    void updateDockContent(float wheelDelta, float mouseX, float mouseY) {
        for (auto& p : m_panels) p.visible = false;
        if (!m_dockHost) return;
        const float TB = DockHost::TAB_BAR_SZ;
        m_dockHost->forEachDockPanel(
            [&](const DockWidget* dock, const Rect& rect, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;          // only the active tab shows
                Panel* p = panelByTitle(dock->title());
                if (!p) return;
                Rect content{ rect.x, rect.y + TB, rect.width, std::max(0.0f, rect.height - TB) };
                p->viewport = content;
                p->visible  = true;

                if (wheelDelta != 0.0f &&
                    mouseX >= content.x && mouseX < content.x + content.width &&
                    mouseY >= content.y && mouseY < content.y + content.height) {
                    p->scrollY -= wheelDelta * 40.0f;
                }

                auto& L = m_graph.getLayout(p->root);
                Constraints cc{ content.width, content.width, 0.0f, 100000.0f };
                m_graph.invalidateNode(p->root, DirtySelf);
                L.boundingBox.x = content.x;
                L.boundingBox.y = content.y;
                m_graph.computeLayout(p->root, cc);
                p->contentH = m_graph.getLayoutConst(p->root).boundingBox.height;

                float maxScroll = std::max(0.0f, p->contentH - content.height);
                p->scrollY = std::clamp(p->scrollY, 0.0f, maxScroll);
                if (p->scrollY != 0.0f) {                      // re-place with scroll applied
                    m_graph.invalidateNode(p->root, DirtySelf);
                    L.boundingBox.x = content.x;
                    L.boundingBox.y = content.y - p->scrollY;
                    m_graph.computeLayout(p->root, cc);
                }
            });
    }

    // Build the semantic snapshot the AI side sees: per widget its role, label,
    // current value, live state flags, and geometry — addressed by node id, not pixels.
    void collectAiNodes(std::vector<AiNodeDescriptor>& out) const {
        out.clear();
        out.reserve(m_widgets.size());
        for (const auto& w : m_widgets) {
            AiNodeDescriptor d{};
            NodeId nid = w->getNodeId();
            d.id = nid;
            const auto& bb = m_graph.getLayoutConst(nid).boundingBox;
            d.x = bb.x; d.y = bb.y; d.width = bb.width; d.height = bb.height;

            AISemanticNode sn = w->getSemanticNode();
            aiSetField(d.role,  sizeof(d.role),  sn.role);
            aiSetField(d.name,  sizeof(d.name),  sn.label);
            aiSetField(d.value, sizeof(d.value), sn.value);

            uint32_t f = 0;
            if (w->isEnabled())  f |= AiEnabled;
            if (w->isVisible())  f |= AiVisible;
            if (sn.interactable) f |= AiInteractable;
            if (w->getState() == WidgetState::Pressed) f |= AiPressed;
            if (w->getState() == WidgetState::Focused) f |= AiFocused;
            d.stateFlags = f;
            out.push_back(d);
        }

        // Docked panels (containers).  Synthetic ids offset past widget node ids so the
        // AI can see & address the docking layout too.  Floating panels are separate OS
        // windows and not included here.
        if (m_dockHost) {
            uint32_t dockId = kDockAiIdBase;
            m_dockHost->forEachDockPanel(
                [&](const DockWidget* dock, const Rect& r, bool active, int tabCount) {
                    AiNodeDescriptor d{};
                    d.id = dockId++;
                    d.x = r.x; d.y = r.y; d.width = r.width; d.height = r.height;
                    aiSetField(d.role,  sizeof(d.role),  "DockPanel");
                    aiSetField(d.name,  sizeof(d.name),  dock->title());
                    aiSetField(d.value, sizeof(d.value),
                               tabCount > 1 ? (active ? "active" : "background") : "docked");
                    uint32_t f = AiVisible | AiInteractable;
                    if (active) f |= AiFocused;
                    d.stateFlags = f;
                    out.push_back(d);
                });
        }
    }

    // Dispatch a semantic action from the AI bus to a target by id, on the UI thread.
    // Returns 1 = handled, 0 = action not understood, -1 = no such target.
    int dispatchAiAction(uint32_t targetId, const std::string& action) {
        if (targetId >= kDockAiIdBase) {                 // a docked panel
            uint32_t want = targetId - kDockAiIdBase, idx = 0;
            std::string title;
            m_dockHost->forEachDockPanel(
                [&](const DockWidget* d, const Rect&, bool, int) {
                    if (idx++ == want) title = d->title();
                });
            if (title.empty()) return -1;
            if (action == "activate") return m_dockHost->activatePanelByTitle(title) ? 1 : 0;
            return 0;
        }
        for (auto& w : m_widgets)
            if (w->getNodeId() == targetId)
                return w->executeSemanticAction(action) ? 1 : 0;
        return -1;
    }

    void handleMouse(float x, float y, bool pressed, bool released) {
        // Route to the controls of currently-visible docked panels only.  Floated panels
        // receive input through handleFloatingPanelInput (in their own window's coords).
        for (auto& p : m_panels) {
            if (!p.visible) continue;
            for (Widget* w : p.widgets) {
                w->handleMouseMove(x, y);
                if (pressed)  w->handleMousePress(x, y);
                if (released) w->handleMouseRelease(x, y);
            }
        }
    }

    // Render a floated panel's content into a floating window (window-local coords).
    void renderFloatingPanel(const std::string& title, PrimitiveBuffer& buf, float w, float h) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        const float TB = DockHost::TAB_BAR_SZ;
        Rect content{ 0.0f, TB, w, std::max(0.0f, h - TB) };
        auto& L = m_graph.getLayout(p->root);
        m_graph.invalidateNode(p->root, DirtySelf);
        L.boundingBox.x = content.x;
        L.boundingBox.y = content.y;
        m_graph.computeLayout(p->root, Constraints{content.width, content.width, 0.0f, 100000.0f});
        buf.pushClip(content.x, content.y, content.width, content.height);
        for (Widget* wt : p->widgets) if (wt->isVisible()) wt->populateRenderPrimitives(buf);
        buf.popClip();
    }

    // Drive a floated panel's content (window-local coords).
    void handleFloatingPanelInput(const std::string& title, float x, float y, bool press, bool release) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        for (Widget* wt : p->widgets) {
            wt->handleMouseMove(x, y);
            if (press)   wt->handleMousePress(x, y);
            if (release) wt->handleMouseRelease(x, y);
        }
    }

    void forceTear(int idx) {
        if (m_tearableTab) m_tearableTab->forceTear(idx);
    }

    std::optional<std::pair<TornTabState, std::pair<float,float>>> consumeNewFloat() {
        if (!m_tearableTab || !m_tearableTab->hasTornTab()) return std::nullopt;
        auto state = m_tearableTab->consumeTornTab();
        float dx = m_tearableTab->lastDragX();
        float dy = m_tearableTab->lastDragY();
        return std::make_pair(std::move(state), std::make_pair(dx, dy));
    }

    DockHost& dockHost() { return *m_dockHost; }

    void removeInlineDock(DockWidget* ptr) {
        m_inlineDocks.erase(
            std::remove_if(m_inlineDocks.begin(), m_inlineDocks.end(),
                [ptr](const auto& u){ return u.get() == ptr; }),
            m_inlineDocks.end());
    }

    // Transfer a dock from a FloatingDockWindow into this catalog's ownership
    // after a successful re-dock.  oldPtr is the address tryCommitDrop already
    // stored in the DockHost tree; we fix it up to point at the new allocation.
    void adoptInlineDock(std::unique_ptr<DockWidget> d, DockWidget* oldPtr) {
        DockWidget* raw = d.get();
        m_inlineDocks.push_back(std::move(d));
        if (m_dockHost) m_dockHost->retargetDock(oldPtr, raw);
    }

private:
    void buildUI() {
        // ---- Content, grouped into dock panels (each is a scrollable flex column) ----
        beginPanel("Navigator");
        section("Navigation");
        add<TabBar>(m_graph, std::vector<std::string>{"Overview", "Controls", "Themes", "Settings"}, 200.0f, 34.0f);
        section("Actions");
        add<Button>(m_graph, "Primary Action", 200.0f, 36.0f);
        add<Button>(m_graph, "Secondary",      160.0f, 36.0f);
        add<ToggleButton>(m_graph, "Dark Mode", 180.0f, 34.0f);
        add<ToggleButton>(m_graph, "Auto-save", 180.0f, 34.0f)->setToggled(true);

        beginPanel("Properties");
        section("Toggles");
        add<CheckBox>(m_graph, "Enable hardware acceleration", 320.0f, 22.0f)->setChecked(true);
        add<CheckBox>(m_graph, "Show tooltips",                280.0f, 22.0f);
        separator();
        section("Backend");
        add<RadioButton>(m_graph, "Vulkan backend",  260.0f, 22.0f)->setSelected(true);
        add<RadioButton>(m_graph, "Metal backend",   260.0f, 22.0f);
        add<RadioButton>(m_graph, "Software (lvp)",  260.0f, 22.0f);

        beginPanel("Inspector");
        section("Range");
        add<Slider>(m_graph, 340.0f, 24.0f)->setValue(0.65f);
        add<Slider>(m_graph, 340.0f, 24.0f)->setValue(0.30f);
        m_progressBar = add<ProgressBar>(m_graph, 340.0f, 14.0f);
        add<ProgressBar>(m_graph, 340.0f, 14.0f)->setProgress(1.0f);
        add<ScrollBar>(m_graph, 340.0f, 14.0f, 0.25f)->setScrollPosition(0.35f);
        section("Steppers");
        add<SpinBox>(m_graph, 0, 255, 160.0f, 32.0f)->setValue(42);
        add<SpinBox>(m_graph, 0, 100, 160.0f, 32.0f)->setValue(75);

        beginPanel("Console");
        section("Filters");
        add<LineEdit>(m_graph, "Search widgets...", 340.0f, 32.0f);
        add<LineEdit>(m_graph, "API endpoint URL", 340.0f, 32.0f);
        add<Label>(m_graph, "Genesis UI Toolkit — Zero-dependency, AI-native", 460.0f, 20.0f);

        beginPanel("Output");
        section("Display");
        add<ComboBox>(m_graph, std::vector<std::string>{"1080p", "1440p", "4K", "8K"}, 200.0f, 34.0f)->setCurrentIndex(1);
        add<ComboBox>(m_graph, std::vector<std::string>{"60 Hz", "120 Hz", "144 Hz", "240 Hz"}, 200.0f, 34.0f)->setCurrentIndex(2);
        add<GroupBox>(m_graph, "Render Settings", 340.0f, 90.0f);

        beginPanel("Assets");
        section("Tear-off Tabs — drag a tab down to detach");
        {
            auto tb = std::make_unique<TabBar>(m_graph,
                std::vector<std::string>{"Properties", "Console", "Assets"}, 300.0f, 34.0f);
            tb->setTearable(true);
            m_tearableTab = tb.get();
            m_graph.addChild(m_curContainer, tb->getNodeId());
            m_curPanel->widgets.push_back(tb.get());
            m_widgets.push_back(std::move(tb));
        }
        add<GroupBox>(m_graph, "Asset Browser", 340.0f, 120.0f);

        // ====================================================================
        // Dock zone management — 3-column layout exercising every constraint.
        //
        // Layout (H = horizontal split, V = vertical split):
        //
        //  Root H-split
        //  ├── Left  V-split  (22%)
        //  │   ├── "nav-zone"     leaf  (40%) ← Navigator
        //  │   └── "asset-zone"   leaf  (60%) ← Assets
        //  └── Right V-split  (78%)
        //      ├── TopRight H-split (42%)
        //      │   ├── "props-zone"   leaf (50%) ← Properties
        //      │   └── "inspect-zone" leaf (50%) ← Inspector (solo, tag-locked)
        //      └── "output-group"     leaf (58%) ← Console + Output (tabbed)
        //
        // Dock constraints demonstrated:
        //   Properties — default: all drops, tabifiable, floatable, minSize
        //   Inspector  — NOT tabifiable (solo leaf), floatable, tag-locked leaf
        //   Console    — tabifiable, floatable, rejects "inspect-zone"
        //   Output     — NOT floatable (anchored), tabifiable, "output-group" only
        //   Navigator  — Left/Right/Center only (no Top/Bottom splits), maxW cap
        //   Assets     — all drops, tabifiable, floatable, minSize
        // ====================================================================

        m_dockHost = std::make_unique<DockHost>();
        m_dockHost->setRootSplit(SplitDir::Horizontal);

        // ---- Left column (Navigator + Assets) ----
        DockNodeId leftPanel  = m_dockHost->addSplit(m_dockHost->rootId(), SplitDir::Vertical, 0.22f);
        DockNodeId navLeaf    = m_dockHost->addLeaf(leftPanel,  "nav-zone",   0.40f);
        DockNodeId assetLeaf  = m_dockHost->addLeaf(leftPanel,  "asset-zone", 0.60f);

        // ---- Right column (Properties, Inspector, output group) ----
        DockNodeId rightPanel = m_dockHost->addSplit(m_dockHost->rootId(), SplitDir::Vertical, 0.78f);
        DockNodeId topRight   = m_dockHost->addSplit(rightPanel, SplitDir::Horizontal, 0.42f);

        DockNodeId propsLeaf   = m_dockHost->addLeaf(topRight,   "props-zone",   0.50f);
        DockNodeId inspectLeaf = m_dockHost->addLeaf(topRight,   "inspect-zone", 0.50f);
        DockNodeId outputLeaf  = m_dockHost->addLeaf(rightPanel, "output-group", 0.58f);

        // All panels are floatable, tabifiable, and accept drops everywhere, so any panel
        // can be re-docked anywhere after floating.  (The constraint API — affinity,
        // allowedDrops, setFloatable/Tabifiable, min/max size — still exists; this demo
        // just keeps it permissive for free-form docking.)
        auto makeDock = [&](const char* title, float w, float h, DockNodeId leaf) {
            auto d = std::make_unique<DockWidget>(title, 0.f, 0.f, w, h);
            d->setMinSize(120.f, 80.f);
            m_dockHost->insertDock(d.get(), leaf);
            m_inlineDocks.push_back(std::move(d));
        };
        makeDock("Properties", 260.f, 200.f, propsLeaf);
        makeDock("Inspector",  260.f, 200.f, inspectLeaf);
        makeDock("Console",    260.f, 160.f, outputLeaf);
        makeDock("Output",     260.f, 160.f, outputLeaf);
        makeDock("Navigator",  200.f, 300.f, navLeaf);
        makeDock("Assets",     200.f, 280.f, assetLeaf);

        m_dockHost->computeLayout({0.f, 0.f,
            static_cast<float>(m_winW), static_cast<float>(m_winH)});
    }

    // A dock panel's scrollable content: a flex-column root + the widgets it owns-by-ref,
    // its scroll offset, measured content height, and on-screen viewport this frame.
    struct Panel {
        std::string          title;
        NodeId               root{InvalidNodeId};
        std::vector<Widget*> widgets;        // non-owning; rendered clipped to viewport
        float                scrollY{0.0f};
        float                contentH{0.0f};
        Rect                 viewport{};     // dock content area (set each frame)
        bool                 visible{false};
    };

    // Begin filling a dock panel by title.  Subsequent add()/section()/spacer() go here.
    void beginPanel(const std::string& title) {
        NodeId root = m_graph.createNode("PanelContent:" + title);
        auto& L = m_graph.getLayout(root);
        L.direction  = FlexDirection::Column;
        L.padding    = 14.0f;
        L.gap        = 8.0f;
        L.alignItems = AlignItems::Stretch;   // controls fill the panel width
        m_panels.push_back(Panel{title, root, {}, 0.f, 0.f, {}, false});
        m_curPanel     = &m_panels.back();
        m_curContainer = root;
    }

    void section(const std::string& name) {
        add<Label>(m_graph, name, 300.0f, 18.0f);
    }

    void separator() {
        spacer(2.0f);
        add<Separator>(m_graph, Separator::Orientation::Horizontal, 300.0f);
        spacer(2.0f);
    }

    void spacer(float h) {
        NodeId id = m_graph.createNode("Spacer");
        auto& l = m_graph.getLayout(id);
        l.boundingBox.width = 1.0f; l.boundingBox.height = h;
        m_graph.addChild(m_curContainer, id);
    }

    template<typename T, typename... Args>
    T* add(Args&&... args) {
        auto w = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = w.get();
        m_graph.addChild(m_curContainer, ptr->getNodeId());
        if (m_curPanel) m_curPanel->widgets.push_back(ptr);
        m_widgets.push_back(std::move(w));
        return ptr;
    }

    Panel* panelByTitle(const std::string& t) {
        for (auto& p : m_panels) if (p.title == t) return &p;
        return nullptr;
    }

    SceneGraph& m_graph;
    uint32_t    m_winW, m_winH;
    NodeId      m_rootId{InvalidNodeId};
    std::vector<std::unique_ptr<Widget>> m_widgets;
    std::vector<Panel> m_panels;
    NodeId      m_curContainer{InvalidNodeId};
    Panel*      m_curPanel{nullptr};
    ProgressBar* m_progressBar{nullptr};
    TabBar*      m_tearableTab{nullptr};

    std::unique_ptr<DockHost>                m_dockHost;
    std::vector<std::unique_ptr<DockWidget>> m_inlineDocks;
    float m_elapsed{0.0f};
    bool  m_animPaused{false};
};

// ============================================================================
// Entry point
// ============================================================================

int main() {
    std::cout << "[GENESIS] Controls Catalog starting...\n";

    constexpr uint32_t W = 760, H = 860;
    uint32_t curW = W, curH = H;   // swapchain size, kept locked to the window size

    Genesis::TranslationEngine::instance().setSearchPath("./translations");
    Genesis::TranslationEngine::instance().setLocale("en");

    auto window = std::make_unique<LinuxPlatformWindow>("Genesis Controls Catalog", W, H);

    NativeWindowHandle handle{};
    handle.apiTarget         = GpuApiType::Vulkan;
    handle.connectionPointer = window->nativeConnection();
    handle.windowPointer     = reinterpret_cast<void*>(
        static_cast<uintptr_t>(window->nativeWindow()));

    auto hal = GpuHal::create(GpuApiType::Vulkan, handle);
    if (!hal) { std::cerr << "[GENESIS] Failed to create Vulkan HAL\n"; return -1; }
    hal->resizeSwapchain(W, H);

    Genesis::FontEngine fontEngine;
    if (fontEngine.loadSystemFont()) {
        auto atlas = fontEngine.buildAtlas(14.0f);
        Genesis::TextHelper::setAtlas(atlas);
        hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        std::cout << "[GENESIS] Font atlas ready.\n";
    } else {
        std::cout << "[GENESIS] No system font found — text will use placeholder rendering.\n";
    }

    Genesis::AccessibilityBridge a11y;
    a11y.start("GenesisControlsCatalog");

    GApplication app;
    Genesis::FocusManager focus;
    auto catalog = std::make_unique<ControlsCatalog>(app.sceneGraph(), W, H);

    // Register the main dock host so FloatingDockWindows can find it by cursor pos.
    DockRegistry::instance().registerHost(
        catalog->dockHost(), window->screenX(), window->screenY(), W, H);

    PrimitiveBuffer buffer;
    std::vector<AiNodeDescriptor> aiNodes;   // reused per-frame semantic snapshot buffer
    auto lastTime = std::chrono::steady_clock::now();
    int  frame60  = 0;

    // ---- Event-driven render state ----
    // We only render a frame when something actually changed (input, resize, an active
    // drag, a running animation, or an AI action).  `redrawFrames` is re-armed to a few
    // frames on any such activity and counts down; while it is >0 we render, otherwise we
    // idle.  This takes the toolkit from "60 full redraws/sec forever" to "0 when nothing
    // changes" — see isAnimating()/the activity check below.
    int   redrawFrames = 4;            // render the first few frames to settle the UI
    float lastMouseX = -1.f, lastMouseY = -1.f;

    std::vector<FloatingDockWindow> floatingDocks;

    std::cout << "[GENESIS] Catalog running. Tab/Shift-Tab cycles focus. Close window to exit.\n";

    while (!window->shouldClose()) {
        auto now = std::chrono::steady_clock::now();
        float dt  = std::chrono::duration<float>(now - lastTime).count();
        lastTime  = now;

        window->pollNativeEvents();

        // Resize handling — keep the swapchain size LOCKED to the window size every
        // frame.  Deferring the rebuild only desyncs the layout (sized to the new
        // window) from the swapchain (still old), so the frame is clipped/stretched and
        // the newly exposed edge flashes — the main source of resize flicker.  The HAL
        // applies resizeSwapchain at the next beginFrame, before this frame renders, so
        // layout and swapchain always agree.  (The OUT_OF_DATE path already rebuilds with
        // waitIdle during a drag, so prompt rebuilds add no extra stalls.)
        bool resized = false;
        {
            uint32_t newW = window->width();
            uint32_t newH = window->height();
            if (newW > 0 && newH > 0 && (newW != curW || newH != curH)) {
                curW = newW;
                curH = newH;
                hal->resizeSwapchain(curW, curH);
                resized = true;
            }
            catalog->dockHost().computeLayout(
                {0.f, 0.f, static_cast<float>(curW), static_cast<float>(curH)});
        }

        // Keep the registry's bounds current (WM may have moved or resized our window).
        DockRegistry::instance().updateBounds(
            catalog->dockHost(), window->screenX(), window->screenY(), curW, curH);

        // ---- Place dock-panel content at its area (handles scroll wheel) ----
        float wheel = window->consumeWheel();
        catalog->updateDockContent(wheel, window->mouseX(), window->mouseY());

        // ---- Keyboard ----
        bool wantScreenshot = false;
        bool keyActivity    = false;
        for (auto& ke : window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            keyActivity = true;
            using K = Genesis::KeyEvent::Key;
            if (ke.key == K::Tab)     focus.nextFocus();
            if (ke.key == K::BackTab) focus.prevFocus();
            if (ke.utf8[0] == 'p' || ke.utf8[0] == 'P') wantScreenshot = true;
            // 'a' pauses/resumes animations (the UI then idles to 0 renders until input).
            if (ke.utf8[0] == 'a' || ke.utf8[0] == 'A') catalog->toggleAnimation();
        }

        bool pressed  = window->consumePress();
        bool released = window->consumeRelease();

        catalog->handleMouse(window->mouseX(), window->mouseY(), pressed, released);
        catalog->update(dt);

        // ---- DockHost mouse routing (inline docks) ----
        if (auto ev = catalog->dockHost().handleMouse(
                window->mouseX(), window->mouseY(), pressed, released))
        {
            if (ev->type == DockHost::DockEvent::Type::WantsFloat) {
                DockWidget* dw  = ev->dock;
                DockNodeId  loc = catalog->dockHost().findDock(dw);
                Rect        r   = catalog->dockHost().node(loc)->rect;

                // Screen position of the dock's top-left corner.
                int sx = window->screenX() + static_cast<int>(r.x);
                int sy = window->screenY() + static_cast<int>(r.y);

                // Cursor offset within the new floating window.
                auto [gx, gy] = window->globalCursorPos();
                int offX = gx - sx;
                int offY = gy - sy;

                catalog->dockHost().removeDock(dw);

                DockWidget moved = std::move(*dw);
                moved.setPosition(0.f, 0.f);
                moved.setSize(r.width, r.height);

                floatingDocks.emplace_back(
                    std::move(moved),
                    sx, sy,
                    static_cast<uint32_t>(r.width), static_cast<uint32_t>(r.height),
                    offX, offY,
                    *hal, /*initialDrag=*/true);

                // The floated panel carries its catalog content: render & drive it in the
                // floating window so it stays fully functional while detached.
                {
                    ControlsCatalog* cat = catalog.get();
                    std::string title = floatingDocks.back().dock().title();
                    floatingDocks.back().setContentRender(
                        [cat, title](PrimitiveBuffer& b, float w, float h) {
                            cat->renderFloatingPanel(title, b, w, h);
                        });
                    floatingDocks.back().setContentInput(
                        [cat, title](float x, float y, bool pr, bool rl) {
                            cat->handleFloatingPanelInput(title, x, y, pr, rl);
                        });
                }

                catalog->removeInlineDock(dw);
            }
            if (ev->type == DockHost::DockEvent::Type::CloseRequested) {
                catalog->dockHost().removeDock(ev->dock);
                catalog->removeInlineDock(ev->dock);
            }
        }

        // ---- Floating dock update: drag / re-dock / close / render ----
        for (auto it = floatingDocks.begin(); it != floatingDocks.end(); ) {
            auto& fd = *it;

            // pollAndMove returns a DockHost* the moment a drag ends over it.
            DockHost* dropHost = fd.pollAndMove();

            if (dropHost) {
                if (auto result = dropHost->tryCommitDrop()) {
                    (void)result;
                    fd.destroySurface(*hal);
                    DockWidget* oldPtr = &fd.dock();
                    catalog->adoptInlineDock(
                        std::make_unique<DockWidget>(fd.takeDock()), oldPtr);
                    it = floatingDocks.erase(it);
                    continue;
                }
                // tryCommitDrop failed (no highlighted drop zone) — dock stays floating.
            }

            if (fd.shouldClose()) {
                fd.destroySurface(*hal);
                it = floatingDocks.erase(it);
                continue;
            }

            // Always render — the OS window follows the cursor from the first
            // frame so there is no ghost phase inside the main window.
            fd.render(*hal, buffer);
            ++it;
        }

        // ---- Inline-only drop (no floating docks in flight) ----
        // Handles the case where DockHost itself initiated a tab-reorder drag
        // within the host and the user released the button.
        if (released && floatingDocks.empty())
            catalog->dockHost().tryCommitDrop();

        // ---- Torn tab → new FloatingDockWindow ----
        if (auto newFloat = catalog->consumeNewFloat()) {
            auto& [state, pos] = *newFloat;
            auto [gx, gy] = window->globalCursorPos();
            constexpr int kTabH = static_cast<int>(DockHost::TAB_BAR_SZ) / 2;
            int winX   = gx - static_cast<int>(FloatingDockWindow::kDefaultW) / 2;
            int winY   = gy - kTabH;
            int dragOX = static_cast<int>(FloatingDockWindow::kDefaultW) / 2;
            int dragOY = kTabH;
            floatingDocks.emplace_back(std::move(state), winX, winY, dragOX, dragOY, *hal);
        }

        // ---- AI semantic command channel (act-by-id, dispatched on the UI thread) ----
        bool aiActed = false;
        {
            uint32_t aiTarget = 0; char aiAction[64] = {0}; uint32_t aiSeq = 0;
            if (app.aiBus().pollAction(aiTarget, aiAction, sizeof(aiAction), aiSeq)) {
                int result = catalog->dispatchAiAction(aiTarget, aiAction);
                app.aiBus().ackAction(aiSeq, result);
                aiActed = true;   // force a redraw + telemetry republish so the agent sees it
            }
        }

        // ---- Decide if this frame needs rendering (event-driven damage) ----
        float mx = window->mouseX(), my = window->mouseY();
        bool mouseMoved = (mx != lastMouseX || my != lastMouseY);
        lastMouseX = mx; lastMouseY = my;

        bool activity = keyActivity || pressed || released || mouseMoved || resized
                        || wantScreenshot || aiActed || wheel != 0.0f || !floatingDocks.empty()
                        || catalog->isAnimating();
        if (activity) redrawFrames = 4;        // render now + a short settle tail

        bool doRender = (redrawFrames > 0);
        if (doRender) --redrawFrames;

        // ---- Render main window (event-driven: only when something changed) ----
        if (doRender) {
            auto frame = hal->beginFrame();
            buffer.clear();
            catalog->render(buffer);
            hal->drawPrimitives(buffer);
            hal->submitAndPresentFrame(frame);
            if (wantScreenshot) hal->captureNextFrame("/tmp/genesis_screenshot.ppm");
        }

        // ---- AI semantic telemetry + accessibility ----
        // Publish the full semantic snapshot to the shared bus whenever the UI changed
        // (we rendered) plus a ~1s idle heartbeat, so an external agent always has a
        // fresh, consistent, lock-free view of every control by identity & meaning.
        ++frame60;
        if (doRender || frame60 >= 60) {
            frame60 = 0;
            catalog->collectAiNodes(aiNodes);
            app.aiBus().publishNodes(aiNodes.data(), static_cast<uint32_t>(aiNodes.size()));
            a11y.update(aiNodes);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    DockRegistry::instance().unregisterHost(catalog->dockHost());

    // Destroy floating surfaces before HAL teardown.
    for (auto& fd : floatingDocks)
        fd.destroySurface(*hal);
    floatingDocks.clear();

    hal->waitIdle();
    a11y.stop();
    return 0;
}
