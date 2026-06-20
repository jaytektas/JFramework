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
#include <genesis/platforms/linux/PopupWindow.h>

#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <type_traits>
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

    std::function<void(ComboBox*)> onComboBoxPopupRequested;
    std::function<void(const std::string&)> onFloatPanelRequested;

    explicit ControlsCatalog(SceneGraph& graph, FocusManager& focus, uint32_t winW, uint32_t winH)
        : m_graph(graph), m_focus(focus), m_winW(winW), m_winH(winH) { buildUI(); }

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
        if (m_dockHost) {
            m_dockHost->forEachDockPanel(
                [&](const DockWidget* dock, const Rect&, bool active, int tabCount) {
                    if (tabCount > 1 && !active) return;
                    Panel* p = panelByTitle(dock->title());
                    if (!p) return;
                    Rect content = m_dockHost->contentArea(m_dockHost->findDock(dock));
                    buf.pushClip(content.x, content.y, content.width, content.height);
                    for (Widget* w : p->widgets)
                        if (w->isVisible()) w->populateRenderPrimitives(buf);
                    buf.popClip();
                });
        }

        // 3. Drag ghost + dock drop overlay on top of everything.
        if (m_tearableTab) m_tearableTab->populateDragGhost(buf);
        if (m_dockHost)    m_dockHost->populateOverlay(buf);
    }

    // Per-frame: place each docked panel's content at its content area, lay it out at the
    // panel width (controls stretch), measure height, clamp scroll, and apply the wheel.
    void clearPanelVisibility() {
        for (auto& p : m_panels) p.visible = false;
    }

    // Per-frame: place each docked panel's content at its content area, lay it out at the
    // panel width (controls stretch), measure height, clamp scroll, and apply the wheel.
    void updateHostDockContent(DockHost& host, float wheelDelta, float mouseX, float mouseY) {
        host.forEachDockPanel(
            [&](const DockWidget* dock, const Rect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;          // only the active tab shows
                Panel* p = panelByTitle(dock->title());
                if (!p) return;
                Rect content = host.contentArea(host.findDock(dock));
                p->viewport = content;
                p->visible  = true;

                if (wheelDelta != 0.0f &&
                    mouseX >= content.x && mouseX < content.x + content.width &&
                    mouseY >= content.y && mouseY < content.y + content.height) {
                    p->scrollY -= wheelDelta * 40.0f;
                }

                auto& L = m_graph.getLayout(p->root);
                m_graph.computeMinSize(p->root);
                float panelMinW = m_graph.getLayoutConst(p->root).minWidth;
                float panelMinH = m_graph.getLayoutConst(p->root).minHeight;
                const_cast<DockWidget*>(dock)->setMinSize(panelMinW, panelMinH);

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

    bool adjustNodeDimension(DockNodeId leafId, bool horizontal, float desiredPixels) {
        if (!m_dockHost) return false;
        DockNodeId curr = leafId;
        while (curr.valid()) {
            DockNode* cNode = m_dockHost->node(curr);
            if (!cNode) break;
            DockNodeId parentId = cNode->parent;
            if (!parentId.valid()) break;
            DockNode* parent = m_dockHost->node(parentId);
            SplitDir targetDir = horizontal ? SplitDir::Horizontal : SplitDir::Vertical;
            if (parent->splitDir == targetDir) {
                int idx = -1;
                for (int i = 0; i < static_cast<int>(parent->children.size()); ++i) {
                    if (parent->children[i] == curr) { idx = i; break; }
                }
                if (idx == -1) return false;
                
                float totalDim = horizontal ? parent->rect.width : parent->rect.height;
                float handleSpace = DockHost::HANDLE_HALF * 2.0f;
                float usable = std::max(0.f, totalDim - handleSpace * static_cast<float>(parent->children.size() - 1));
                if (usable <= 0.f) return false;
                
                float w_new = desiredPixels / usable;
                w_new = std::clamp(w_new, 0.01f, 0.99f);
                float w_old = parent->weights[idx];
                
                parent->weights[idx] = w_new;
                float remaining_new = 1.0f - w_new;
                float remaining_old = 1.0f - w_old;
                if (remaining_old > 1e-4f) {
                    float scale = remaining_new / remaining_old;
                    for (int i = 0; i < static_cast<int>(parent->weights.size()); ++i) {
                        if (i != idx) parent->weights[i] *= scale;
                    }
                } else {
                    float each = remaining_new / static_cast<float>(parent->weights.size() - 1);
                    for (int i = 0; i < static_cast<int>(parent->weights.size()); ++i) {
                        if (i != idx) parent->weights[i] = each;
                    }
                }
                float sum = 0.f;
                for (float w : parent->weights) sum += w;
                if (sum > 1e-6f) {
                    for (float& w : parent->weights) w /= sum;
                }
                m_dockHost->computeLayout(m_dockHost->hostRect());
                return true;
            }
            curr = parentId;
        }
        return false;
    }

    // Dispatch a semantic action from the AI bus to a target by id, on the UI thread.
    // Returns 1 = handled, 0 = action not understood, -1 = no such target.
    int dispatchAiAction(uint32_t targetId, const std::string& action) {
        std::printf("[dispatchAiAction] targetId=%u, action=%s\n", targetId, action.c_str());
        std::fflush(stdout);
        // System-level actions addressed to the magic id 0xFFFFFFFF (broadcast / inject).
        if (targetId == 0xFFFFFFFFu)
            return handleSystemAction(action);

        if (targetId >= kDockAiIdBase) {                 // a docked panel
            uint32_t want = targetId - kDockAiIdBase, idx = 0;
            std::string title;
            m_dockHost->forEachDockPanel(
                [&](const DockWidget* d, const Rect&, bool, int) {
                    if (idx++ == want) title = d->title();
                });
            if (title.empty()) return -1;
            
            if (action == "activate") return m_dockHost->activatePanelByTitle(title) ? 1 : 0;

            if (action == "float") {
                if (onFloatPanelRequested) {
                    onFloatPanelRequested(title);
                    return 1;
                }
                return 0;
            }

            if (action.rfind("set_width:", 0) == 0) {
                float px = 0.f;
                try { px = std::stof(action.substr(10)); } catch (...) { return 0; }
                DockWidget* dw = nullptr;
                m_dockHost->forEachDockPanel([&](const DockWidget* d, const Rect&, bool, int) {
                    if (d->title() == title) dw = const_cast<DockWidget*>(d);
                });
                if (!dw) return 0;
                DockNodeId leafId = m_dockHost->findDock(dw);
                if (!leafId.valid()) return 0;
                return adjustNodeDimension(leafId, true, px) ? 1 : 0;
            }

            if (action.rfind("set_height:", 0) == 0) {
                float px = 0.f;
                try { px = std::stof(action.substr(11)); } catch (...) { return 0; }
                DockWidget* dw = nullptr;
                m_dockHost->forEachDockPanel([&](const DockWidget* d, const Rect&, bool, int) {
                    if (d->title() == title) dw = const_cast<DockWidget*>(d);
                });
                if (!dw) return 0;
                DockNodeId leafId = m_dockHost->findDock(dw);
                if (!leafId.valid()) return 0;
                return adjustNodeDimension(leafId, false, px) ? 1 : 0;
            }

            if (action.rfind("move_to:", 0) == 0) {
                std::string sub = action.substr(8);
                size_t colon = sub.find(':');
                std::string posStr = (colon == std::string::npos) ? sub : sub.substr(0, colon);
                std::string targetTitle = (colon == std::string::npos) ? "" : sub.substr(colon + 1);

                DropPos pos = DropPos::Center;
                if (posStr == "left")        pos = DropPos::Left;
                else if (posStr == "right")  pos = DropPos::Right;
                else if (posStr == "top")    pos = DropPos::Top;
                else if (posStr == "bottom") pos = DropPos::Bottom;
                else if (posStr == "center") pos = DropPos::Center;
                else return 0;

                DockWidget* sourceWidget = nullptr;
                m_dockHost->forEachDockPanel([&](const DockWidget* d, const Rect&, bool, int) {
                    if (d->title() == title) sourceWidget = const_cast<DockWidget*>(d);
                });
                if (!sourceWidget) return 0;

                if (!targetTitle.empty()) {
                    DockWidget* targetWidget = nullptr;
                    m_dockHost->forEachDockPanel([&](const DockWidget* d, const Rect&, bool, int) {
                        if (d->title() == targetTitle) targetWidget = const_cast<DockWidget*>(d);
                    });
                    if (!targetWidget) return 0;

                    DockNodeId targetLeaf = m_dockHost->findDock(targetWidget);
                    if (!targetLeaf.valid()) return 0;

                    m_dockHost->removeDock(sourceWidget);
                    if (pos == DropPos::Center) {
                        m_dockHost->insertDock(sourceWidget, targetLeaf);
                    } else {
                        DockNodeId newLeaf = m_dockHost->splitLeaf(targetLeaf, pos);
                        m_dockHost->insertDock(sourceWidget, newLeaf);
                    }
                } else {
                    DockNodeId targetLeaf = m_dockHost->edgeLeaf();
                    if (!targetLeaf.valid()) return 0;

                    m_dockHost->removeDock(sourceWidget);
                    if (pos == DropPos::Center) {
                        m_dockHost->insertDock(sourceWidget, targetLeaf);
                    } else {
                        DockNodeId newLeaf = m_dockHost->splitLeaf(targetLeaf, pos);
                        m_dockHost->insertDock(sourceWidget, newLeaf);
                    }
                }
                m_dockHost->computeLayout(m_dockHost->hostRect());
                return 1;
            }
            return 0;
        }
        for (auto& w : m_widgets) {
            if (w->getNodeId() == targetId) {
                bool res = w->executeSemanticAction(action);
                std::printf("[dispatchAiAction] Found widget. executeSemanticAction returned %d\n", res);
                std::fflush(stdout);
                return res ? 1 : 0;
            }
        }
        std::printf("[dispatchAiAction] Widget targetId=%u NOT found in m_widgets\n", targetId);
        std::fflush(stdout);
        return -1;
    }

    // --------------------------------------------------------------------------
    // System-level actions (targetId == 0xFFFFFFFF)
    //
    //   inject:<type>:<panel>:<label>
    //       Dynamically create a widget in <panel> at runtime.
    //       <type> is one of: button, label, checkbox, lineedit
    //       <panel> must match an existing dock panel title.
    //       <label> is the display text / placeholder.
    //
    //   remove_widget:<nodeId>
    //       Remove a previously injected widget by node id.
    //
    // --------------------------------------------------------------------------
    int handleSystemAction(const std::string& action) {
        // ---- inject:<type>:<panel>:<label> ----
        if (action.rfind("inject:", 0) == 0) {
            // parse the three colon-delimited fields after "inject:"
            std::string rest = action.substr(7);
            auto split2 = rest.find(':');
            if (split2 == std::string::npos) return 0;
            std::string type  = rest.substr(0, split2);
            rest = rest.substr(split2 + 1);
            auto split3 = rest.find(':');
            std::string panel = (split3 == std::string::npos) ? rest : rest.substr(0, split3);
            std::string label = (split3 == std::string::npos) ? "" : rest.substr(split3 + 1);

            Panel* p = panelByTitle(panel);
            if (!p) {
                std::cerr << "[AI-inject] unknown panel '" << panel << "'\n";
                return 0;
            }

            // Save the current panel context then temporarily redirect into target panel.
            Panel* savedPanel     = m_curPanel;
            NodeId savedContainer = m_curContainer;
            m_curPanel     = p;
            m_curContainer = p->root;

            Widget* injected = nullptr;
            if (type == "button" || type == "Button") {
                injected = add<Button>(m_graph, label.empty() ? "Button" : label, 200.0f, 36.0f);
            } else if (type == "label" || type == "Label") {
                injected = add<Label>(m_graph, label.empty() ? "Label" : label, 300.0f, 20.0f);
            } else if (type == "checkbox" || type == "CheckBox") {
                injected = add<CheckBox>(m_graph, label.empty() ? "CheckBox" : label, 280.0f, 22.0f);
            } else if (type == "lineedit" || type == "LineEdit") {
                injected = add<LineEdit>(m_graph, label.empty() ? "Enter text..." : label, 320.0f, 32.0f);
            } else if (type == "textarea" || type == "TextArea") {
                injected = add<TextArea>(m_graph, label.empty() ? "Enter paragraph..." : label, 320.0f, 100.0f);
            } else {
                std::cerr << "[AI-inject] unknown widget type '" << type << "'\n";
                m_curPanel = savedPanel; m_curContainer = savedContainer;
                return 0;
            }

            // Restore context.
            m_curPanel     = savedPanel;
            m_curContainer = savedContainer;

            // Invalidate layout so the panel reflows on the next frame.
            m_graph.invalidateNode(p->root, DirtySelf);

            std::cout << "[AI-inject] '" << type << "' (id=" << injected->getNodeId()
                      << ") added to panel '" << panel << "' label='" << label << "'\n";
            return 1;
        }

        // ---- remove_widget:<nodeId> ----
        if (action.rfind("remove_widget:", 0) == 0) {
            uint32_t nid = 0;
            try { nid = static_cast<uint32_t>(std::stoul(action.substr(14))); }
            catch (...) { return 0; }

            // Find which panel owns this widget and remove it.
            for (auto& p : m_panels) {
                auto it = std::find_if(p.widgets.begin(), p.widgets.end(),
                    [nid](Widget* w){ return w->getNodeId() == nid; });
                if (it != p.widgets.end()) {
                    p.widgets.erase(it);
                    // Remove from master ownership list too.
                    m_widgets.erase(
                        std::remove_if(m_widgets.begin(), m_widgets.end(),
                            [nid](const std::unique_ptr<Widget>& w){ return w->getNodeId() == nid; }),
                        m_widgets.end());
                    m_graph.invalidateNode(p.root, DirtySelf);
                    std::cout << "[AI-remove] widget id=" << nid << " removed from panel '" << p.title << "'\n";
                    return 1;
                }
            }
            return -1;
        }

        return 0;  // unrecognised system action
    }

    void handleMouse(float x, float y, bool pressed, bool released) {
        // Route to the controls of currently-visible docked panels only.  Floated panels
        // receive input through handleFloatingPanelInput (in their own window's coords).
        bool hitAny = false;
        for (auto& p : m_panels) {
            if (!p.visible) continue;
            for (Widget* w : p.widgets) {
                w->handleMouseMove(x, y);
                if (pressed) {
                    w->handleMousePress(x, y);
                    if (w->hitTest(x, y)) hitAny = true;
                }
                if (released) w->handleMouseRelease(x, y);
            }
        }
        if (pressed && !hitAny) {
            m_focus.setFocus(nullptr);
        }
    }

    // Render a floated panel's content into a floating window (window-local coords).
    void renderFloatingPanel(const std::string& title, PrimitiveBuffer& buf, const Rect& content) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        auto& L = m_graph.getLayout(p->root);
        m_graph.invalidateNode(p->root, DirtySelf);
        L.boundingBox.x = content.x;
        L.boundingBox.y = content.y;
        m_graph.computeLayout(p->root, Constraints{content.width, content.width, 0.0f, 100000.0f});
        buf.pushClip(content.x, content.y, content.width, content.height);
        for (Widget* wt : p->widgets) if (wt->isVisible()) wt->populateRenderPrimitives(buf);
        buf.popClip();
    }

    void renderHostFloatingPanels(DockHost& host, PrimitiveBuffer& buf) {
        host.forEachDockPanel(
            [&](const DockWidget* dock, const Rect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;
                Rect content = host.contentArea(host.findDock(dock));
                renderFloatingPanel(dock->title(), buf, content);
            });
    }

    void handleHostFloatingPanelsInput(DockHost& host, float x, float y, bool pr, bool rl) {
        host.forEachDockPanel(
            [&](const DockWidget* dock, const Rect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;
                Rect content = host.contentArea(host.findDock(dock));
                if (x >= content.x && x < content.x + content.width &&
                    y >= content.y && y < content.y + content.height) {
                    handleFloatingPanelInput(dock->title(), x, y, pr, rl);
                }
            });
    }

    // Drive a floated panel's content (window-local coords).
    void handleFloatingPanelInput(const std::string& title, float x, float y, bool press, bool release) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        bool hitAny = false;
        for (Widget* wt : p->widgets) {
            wt->handleMouseMove(x, y);
            if (press) {
                wt->handleMousePress(x, y);
                if (wt->hitTest(x, y)) hitAny = true;
            }
            if (release) wt->handleMouseRelease(x, y);
        }
        if (press && !hitAny) {
            m_focus.setFocus(nullptr);
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
        section("Terminal Log");
        add<TextArea>(m_graph, "No logs yet. Type here...", 340.0f, 100.0f);
        add<Label>(m_graph, "Genesis UI Toolkit — Zero-dependency, AI-native", 460.0f, 20.0f);

        beginPanel("Output");
        section("Display");
        {
            auto cb1 = add<ComboBox>(m_graph, std::vector<std::string>{"1080p", "1440p", "4K", "8K"}, 200.0f, 34.0f);
            cb1->setCurrentIndex(1);
            cb1->setMode(ComboBoxMode::Popup);
            cb1->onPopupRequested.connect([this](ComboBox* cb) {
                if (onComboBoxPopupRequested) onComboBoxPopupRequested(cb);
            });

            auto cb2 = add<ComboBox>(m_graph, std::vector<std::string>{"60 Hz", "120 Hz", "144 Hz", "240 Hz"}, 200.0f, 34.0f);
            cb2->setCurrentIndex(2);
            cb2->setMode(ComboBoxMode::Popup);
            cb2->onPopupRequested.connect([this](ComboBox* cb) {
                if (onComboBoxPopupRequested) onComboBoxPopupRequested(cb);
            });
        }
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

        if constexpr (std::is_base_of_v<Control, T>) {
            m_focus.registerWidget(ptr);
            ptr->onClicked.connect([this, ptr]() {
                m_focus.setFocus(ptr);
            });
        }

        return ptr;
    }

    Panel* panelByTitle(const std::string& t) {
        for (auto& p : m_panels) if (p.title == t) return &p;
        return nullptr;
    }

    SceneGraph& m_graph;
    FocusManager& m_focus;
    uint32_t    m_winW, m_winH;
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

    std::string winTitle = "Genesis Controls Catalog";
    if (getenv("GENESIS_WIN_TITLE") != nullptr) {
        winTitle = getenv("GENESIS_WIN_TITLE");
    }
    auto window = std::make_unique<LinuxPlatformWindow>(winTitle, W, H);

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
    auto catalog = std::make_unique<ControlsCatalog>(app.sceneGraph(), focus, W, H);

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

    Genesis::FloatingDockOptions g_dockOptions;
    catalog->dockHost().setLivePreviewEnabled(g_dockOptions.livePreviewEnabled);
    std::vector<FloatingDockWindow> floatingDocks;
    std::unique_ptr<PopupWindow> activePopup;
    ComboBox* activePopupComboBox = nullptr;
    PopupWindow* pendingClosePopup = nullptr;

    catalog->onComboBoxPopupRequested = [&](ComboBox* cb) {
        // Toggle: clicking the same combo while its popup is open closes it.
        if (activePopup && activePopupComboBox == cb) {
            activePopup->destroySurface(*hal);
            activePopup.reset();
            activePopupComboBox = nullptr;
            return;
        }
        // Different combo (or no popup): close any existing popup first.
        if (activePopup) {
            activePopup->destroySurface(*hal);
            activePopup.reset();
        }

        NodeId cbNode = cb->getNodeId();
        const auto& bb = app.sceneGraph().getLayoutConst(cbNode).boundingBox;

        int sx = window->screenX() + static_cast<int>(bb.x);
        int sy = window->screenY() + static_cast<int>(bb.y + bb.height);

        uint32_t popupW = static_cast<uint32_t>(bb.width);

        // Build the popup: one PopupItem per combo option, borderless.
        auto popup = std::make_unique<PopupWindow>(
            sx, sy, popupW, 8 /*placeholder height, computed below*/,
            *hal, PopupWindow::Style::Borderless, window->nativeWindow());

        const auto& items = cb->items();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            auto* pi = popup->add<PopupItem>(
                items[i],
                static_cast<float>(popupW), 28.f);

            // Capture by value: i and cb are stable for the popup's lifetime.
            pi->onActivated.connect([cb, i, &pendingClosePopup, &activePopup, &activePopupComboBox]() {
                cb->setCurrentIndex(i);
                pendingClosePopup = activePopup.get();
                activePopupComboBox = nullptr;
            });
        }

        popup->computeNaturalHeight();
        activePopup = std::move(popup);
        activePopupComboBox = cb;
    };

    catalog->onFloatPanelRequested = [&](const std::string& title) {
        DockWidget* dw = nullptr;
        catalog->dockHost().forEachDockPanel([&](const DockWidget* d, const Rect&, bool, int) {
            if (d->title() == title) dw = const_cast<DockWidget*>(d);
        });
        if (!dw) return;

        DockNodeId loc = catalog->dockHost().findDock(dw);
        Rect r = catalog->dockHost().node(loc)->rect;

        int sx = window->screenX() + static_cast<int>(r.x);
        int sy = window->screenY() + static_cast<int>(r.y);

        catalog->dockHost().removeDock(dw);

        DockWidget moved = std::move(*dw);
        moved.setPosition(0.f, 0.f);
        moved.setSize(r.width, r.height);

        int offX = static_cast<int>(r.width) / 2;
        int offY = static_cast<int>(r.height) / 2;

        floatingDocks.emplace_back(
            std::move(moved),
            sx, sy,
            static_cast<uint32_t>(r.width), static_cast<uint32_t>(r.height),
            offX, offY,
            *hal, /*initialDrag=*/false, g_dockOptions, window->nativeWindow()
        );

        {
            ControlsCatalog* cat = catalog.get();
            auto& newFd = floatingDocks.back();
            DockHost* hostPtr = &newFd.dockHost();
            newFd.setContentRenderHost([cat, hostPtr](PrimitiveBuffer& b) {
                cat->renderHostFloatingPanels(*hostPtr, b);
            });
            newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl) {
                cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl);
            });
        }

        catalog->removeInlineDock(dw);
    };

    std::cout << "[GENESIS] Catalog running. Tab/Shift-Tab cycles focus. Close window to exit.\n";
    std::cout << "[HOTKEYS] Tweak drag options at runtime:\n";
    std::cout << "  'd' / 'D': Cycle global title bar drag behavior (Legacy, Always, Conditional)\n";
    std::cout << "  's' / 'S': Toggle Single Dock Drag Moves Window (vs Tears Out)\n";
    std::cout << "  't' / 'T': Toggle Tab Drag Tears Out\n";
    std::cout << "  'x' / 'X': Toggle Split Drag Tears Out\n";
    std::cout << "  'l' / 'L': Toggle Live Drop Preview\n";

    while (!window->shouldClose()) {
        auto now = std::chrono::steady_clock::now();
        float dt  = std::chrono::duration<float>(now - lastTime).count();
        lastTime  = now;

        window->pollNativeEvents();

        static int autoFloatFrames = 0;
        static bool autoFloatTriggered = false;
        if (getenv("GENESIS_AUTO_FLOAT") != nullptr && !autoFloatTriggered) {
            autoFloatFrames++;
            if (autoFloatFrames >= 10) {
                std::cout << "[TEST] Triggering auto-float of tab...\n";
                catalog->forceTear(1);
                autoFloatTriggered = true;
            }
        }

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
            uint32_t minW = static_cast<uint32_t>(std::ceil(catalog->dockHost().minWidthNeeded()));
            uint32_t minH = static_cast<uint32_t>(std::ceil(catalog->dockHost().minHeightNeeded()));
            if (newW > 0 && newH > 0 && (newW < minW || newH < minH)) {
                newW = std::max(newW, minW);
                newH = std::max(newH, minH);
                window->setSize(newW, newH);
            }
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
        catalog->clearPanelVisibility();
        catalog->updateHostDockContent(catalog->dockHost(), wheel, window->mouseX(), window->mouseY());

        // ---- Keyboard ----
        bool wantScreenshot = false;
        bool keyActivity    = false;
        for (auto& ke : window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            keyActivity = true;
            using K = Genesis::KeyEvent::Key;
            bool handled = false;
            if (focus.focused()) {
                handled = focus.focused()->handleKeyEvent(ke);
            }
            if (handled) continue;

            if (ke.key == K::Tab)     focus.nextFocus();
            if (ke.key == K::BackTab) focus.prevFocus();
            if (ke.utf8[0] == 'p' || ke.utf8[0] == 'P') wantScreenshot = true;
            // 'a' pauses/resumes animations (the UI then idles to 0 renders until input).
            if (ke.utf8[0] == 'a' || ke.utf8[0] == 'A') catalog->toggleAnimation();
            if (ke.utf8[0] == 'f' || ke.utf8[0] == 'F') catalog->forceTear(1);

            // Toggle config options:
            if (ke.utf8[0] == 'd' || ke.utf8[0] == 'D') {
                using B = Genesis::FloatingDragBehavior;
                if (g_dockOptions.dragBehavior == B::ConditionalGlobalTitleBar) {
                    g_dockOptions.dragBehavior = B::Legacy;
                    std::cout << "[CONFIG] Drag Behavior: Legacy (No global title bar, Alt+Drag to move)\n";
                } else if (g_dockOptions.dragBehavior == B::Legacy) {
                    g_dockOptions.dragBehavior = B::AlwaysGlobalTitleBar;
                    std::cout << "[CONFIG] Drag Behavior: AlwaysGlobalTitleBar (Always show global title bar)\n";
                } else {
                    g_dockOptions.dragBehavior = B::ConditionalGlobalTitleBar;
                    std::cout << "[CONFIG] Drag Behavior: ConditionalGlobalTitleBar (Show global title bar when nested)\n";
                }
                for (auto& fd : floatingDocks) fd.setOptions(g_dockOptions);
            }
            if (ke.utf8[0] == 's' || ke.utf8[0] == 'S') {
                g_dockOptions.singleDockDragMovesWindow = !g_dockOptions.singleDockDragMovesWindow;
                std::cout << "[CONFIG] Single Dock Drag Moves Window: " << (g_dockOptions.singleDockDragMovesWindow ? "ENABLED" : "DISABLED (Tears out instead)") << "\n";
                for (auto& fd : floatingDocks) fd.setOptions(g_dockOptions);
            }
            if (ke.utf8[0] == 't' || ke.utf8[0] == 'T') {
                g_dockOptions.tabDragTearsOut = !g_dockOptions.tabDragTearsOut;
                std::cout << "[CONFIG] Tab Drag Tears Out: " << (g_dockOptions.tabDragTearsOut ? "ENABLED" : "DISABLED") << "\n";
                for (auto& fd : floatingDocks) fd.setOptions(g_dockOptions);
            }
            if (ke.utf8[0] == 'x' || ke.utf8[0] == 'X') {
                g_dockOptions.splitDragTearsOut = !g_dockOptions.splitDragTearsOut;
                std::cout << "[CONFIG] Split Drag Tears Out: " << (g_dockOptions.splitDragTearsOut ? "ENABLED" : "DISABLED") << "\n";
                for (auto& fd : floatingDocks) fd.setOptions(g_dockOptions);
            }
            if (ke.utf8[0] == 'l' || ke.utf8[0] == 'L') {
                g_dockOptions.livePreviewEnabled = !g_dockOptions.livePreviewEnabled;
                std::cout << "[CONFIG] Live Drop Preview: " << (g_dockOptions.livePreviewEnabled ? "ENABLED" : "DISABLED") << "\n";
                catalog->dockHost().setLivePreviewEnabled(g_dockOptions.livePreviewEnabled);
                for (auto& fd : floatingDocks) fd.setOptions(g_dockOptions);
            }
        }

        bool pressed  = window->consumePress();
        bool released = window->consumeRelease();

        if (activePopup && pressed) {
            NodeId cbNode = activePopupComboBox->getNodeId();
            const auto& bb = app.sceneGraph().getLayoutConst(cbNode).boundingBox;
            float mx = window->mouseX();
            float my = window->mouseY();
            if (mx < bb.x || mx > bb.x + bb.width ||
                my < bb.y || my > bb.y + bb.height) {
                activePopup->destroySurface(*hal);
                activePopup.reset();
                activePopupComboBox = nullptr;
            }
        }

        catalog->handleMouse(window->mouseX(), window->mouseY(), pressed, released);
        catalog->update(dt);

        // ---- DockHost mouse routing (inline docks) ----
        PlatformCursor pc = PlatformCursor::Default;
        auto hc = catalog->dockHost().getHoverCursor(window->mouseX(), window->mouseY());
        if (hc == DockHost::HoverCursor::Horiz)      pc = PlatformCursor::ResizeLeftRight;
        else if (hc == DockHost::HoverCursor::Vert)  pc = PlatformCursor::ResizeUpDown;
        window->setCursor(pc);

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
                    *hal, /*initialDrag=*/true, g_dockOptions, window->nativeWindow());

                // The floated panel carries its catalog content: render & drive it in the
                // floating window so it stays fully functional while detached.
                {
                    ControlsCatalog* cat = catalog.get();
                    auto& newFd = floatingDocks.back();
                    DockHost* hostPtr = &newFd.dockHost();
                    newFd.setContentRenderHost([cat, hostPtr](PrimitiveBuffer& b) {
                        cat->renderHostFloatingPanels(*hostPtr, b);
                    });
                    newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl) {
                        cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl);
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

            catalog->updateHostDockContent(fd.dockHost(), wheel, fd.window().mouseX(), fd.window().mouseY());

            auto pollRes = fd.pollAndMove();

            if (pollRes.type == FloatingDockWindow::PollResult::Type::CommitDrop) {
                DockHost* dropHost = pollRes.dropHost;
                if (auto result = dropHost->tryCommitDrop()) {
                    (void)result;
                    fd.destroySurface(*hal);
                    DockWidget* oldPtr = &fd.dock();
                    if (dropHost == &catalog->dockHost()) {
                        catalog->adoptInlineDock(
                            std::make_unique<DockWidget>(fd.takeDock()), oldPtr);
                    } else {
                        FloatingDockWindow* targetWin = nullptr;
                        for (auto& otherFd : floatingDocks) {
                            if (&otherFd.dockHost() == dropHost) {
                                targetWin = &otherFd;
                                break;
                            }
                        }
                        if (targetWin) {
                            targetWin->adoptDock(
                                std::make_unique<DockWidget>(fd.takeDock()), oldPtr);
                        }
                    }
                    it = floatingDocks.erase(it);
                    continue;
                }
            } else if (pollRes.type == FloatingDockWindow::PollResult::Type::WantsFloat) {
                DockWidget* dw = pollRes.wantsFloatDock;
                Rect r = pollRes.wantsFloatRect;

                int sx = fd.window().screenX() + static_cast<int>(r.x);
                int sy = fd.window().screenY() + static_cast<int>(r.y);

                auto [gx, gy] = fd.window().globalCursorPos();
                int offX = gx - sx;
                int offY = gy - sy;

                std::unique_ptr<DockWidget> movedPtr = fd.releaseDock(dw);
                if (movedPtr) {
                    DockWidget moved = std::move(*movedPtr);
                    moved.setPosition(0.f, 0.f);
                    moved.setSize(r.width, r.height);

                    floatingDocks.emplace_back(
                        std::move(moved),
                        sx, sy,
                        static_cast<uint32_t>(r.width), static_cast<uint32_t>(r.height),
                        offX, offY,
                        *hal, /*initialDrag=*/true, g_dockOptions, window->nativeWindow());

                    {
                        ControlsCatalog* cat = catalog.get();
                        auto& newFd = floatingDocks.back();
                        DockHost* hostPtr = &newFd.dockHost();
                        newFd.setContentRenderHost([cat, hostPtr](PrimitiveBuffer& b) {
                            cat->renderHostFloatingPanels(*hostPtr, b);
                        });
                        newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl) {
                            cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl);
                        });
                    }
                }

                if (fd.dockHost().dockCount() == 0) {
                    fd.destroySurface(*hal);
                    it = floatingDocks.erase(it);
                    continue;
                }
            }

            if (fd.shouldClose()) {
                fd.destroySurface(*hal);
                it = floatingDocks.erase(it);
                continue;
            }

            fd.render(*hal, buffer);
            ++it;
        }

        // ---- Active popup update ----
        if (activePopup) {
            auto res = activePopup->pollEvents(*hal);
            if (res.type == PopupWindow::PollResult::Type::Dismissed) {
                activePopup->destroySurface(*hal);
                activePopup.reset();
                activePopupComboBox = nullptr;
            } else if (activePopup->isViewable()) {
                activePopup->render(*hal, buffer);
            }
        }

        if (pendingClosePopup) {
            if (activePopup && activePopup.get() == pendingClosePopup) {
                activePopup->destroySurface(*hal);
                activePopup.reset();
            }
            pendingClosePopup = nullptr;
        }

        // ---- Inline-only drop (no floating docks in flight) ----
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
            floatingDocks.emplace_back(std::move(state), winX, winY, dragOX, dragOY, *hal, g_dockOptions, window->nativeWindow());

            {
                ControlsCatalog* cat = catalog.get();
                auto& newFd = floatingDocks.back();
                DockHost* hostPtr = &newFd.dockHost();
                newFd.setContentRenderHost([cat, hostPtr](PrimitiveBuffer& b) {
                    cat->renderHostFloatingPanels(*hostPtr, b);
                });
                newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl) {
                    cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl);
                });
            }
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
                        || activePopup || catalog->isAnimating();
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

    if (activePopup) {
        activePopup->destroySurface(*hal);
        activePopup.reset();
    }

    hal->waitIdle();
    a11y.stop();
    return 0;
}
