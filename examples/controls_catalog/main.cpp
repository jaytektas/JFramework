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
#if defined(_WIN32)
#include <genesis/platforms/windows/WindowsPlatformWindow.h>
#else
#include <genesis/platforms/linux/LinuxPlatformWindow.h>
#endif
#include <genesis/platforms/linux/FloatingDockWindow.h>
#include <genesis/platforms/NativeDialogWindow.h>
#include <genesis/platforms/PopupWindow.h>
#include <genesis/core/MenuSystem.h>
#include <genesis/core/Dialog.h>
#include <genesis/core/Animator.h>
#include <genesis/core/Splitter.h>
#include <genesis/core/ImageWidget.h>
#include <genesis/platform/Clipboard.h>
#include <genesis/platform/FileDialog.h>

#if defined(_WIN32)
using PlatformWindowImpl = jf::JWindowsPlatformWindow;
#else
using PlatformWindowImpl = jf::JLinuxPlatformWindow;
#endif

#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <type_traits>
#include <thread>
#include <cmath>
#include <algorithm>

using namespace jf;

// ============================================================================
// Controls Catalog — showcases every Genesis widget with live rendering
// ============================================================================

class ControlsCatalog {
public:
    // AI node ids at/above this base are docked panels (offset past scene-graph node ids).
    static constexpr uint32_t kDockAiIdBase = 0x40000000u;

    std::function<void(JComboBox*)> onComboBoxPopupRequested;
    std::function<void(const std::string&)> onFloatPanelRequested;

    explicit ControlsCatalog(JSceneGraph& graph, JFocusManager& focus, uint32_t winW, uint32_t winH)
        : m_graph(graph), m_focus(focus), m_winW(winW), m_winH(winH) { buildUI(); }

    void update(float dt) {
        if (m_animPaused) return;
        m_elapsed += dt;
        if (m_progressBar)
            m_progressBar->setProgress(0.5f + 0.5f * std::sin(m_elapsed * 0.8f));

        m_demoAnimator.advance(dt);
        if (m_animBar0) m_animBar0->setProgress(m_demoAnimator.value(m_animSlot0));
        if (m_animBar1) m_animBar1->setProgress(m_demoAnimator.value(m_animSlot1));
        if (m_animBar2) m_animBar2->setProgress(m_demoAnimator.value(m_animSlot2));
        if (m_demoAnimator.isDone() && m_animBar0) {
            m_animForward = !m_animForward;
            float tgt = m_animForward ? 1.0f : 0.0f;
            m_demoAnimator.animateTo(m_animSlot0, tgt, 1200.0f, jf::JEasing::EaseInOut);
            m_demoAnimator.animateTo(m_animSlot1, tgt, 1800.0f, jf::JEasing::EaseOutElastic);
            m_demoAnimator.animateTo(m_animSlot2, tgt, 2400.0f, jf::JEasing::EaseOutBounce);
        }
    }

    // True while something is visually animating and the frame must be redrawn
    // continuously.  Here only the demo progress bar animates; a real app would OR
    // together all active animators / transitions.
    bool isAnimating() const { return !m_animPaused && (m_progressBar != nullptr || !m_demoAnimator.isDone()); }
    void toggleAnimation()    { m_animPaused = !m_animPaused; }
    bool showPanelScrollbars() const { return m_showPanelScrollbars; }
    void setShowPanelScrollbars(bool show) {
        m_showPanelScrollbars = show;
        for (auto& p : m_panels) {
            if (p && p->root != InvalidNodeId) {
                m_graph.invalidateNode(p->root, DirtySelf);
            }
        }
    }

    jf::JMenuBar* menuBar() const noexcept { return m_menuBar.get(); }
    jf::JMenu* viewMenu() const noexcept { return m_menus.size() > 2 ? m_menus[2].get() : nullptr; }
    void layoutMenuBar(float winW, float yOffset = 0.f) {
        if (m_menuBar) {
            auto& l = m_graph.getLayout(m_menuBar->getNodeId());
            l.boundingBox = { 0.f, yOffset, winW, 32.f };
            m_graph.invalidateNode(m_menuBar->getNodeId(), DirtySelf);
            m_graph.computeLayout(m_menuBar->getNodeId(), { winW, winW, 32.f, 32.f });
        }
    }

    void render(JPrimitiveBuffer& buf) {
        // 1. Dock chrome (panel backgrounds, tab bars, borders) as the base layer.
        if (m_dockHost) m_dockHost->populateRenderPrimitives(buf);

        // 2. Each visible panel's content, clipped + scrolled within its viewport.
        if (m_dockHost) {
            m_dockHost->forEachDockPanel(
                [&](const JDockWidget* dock, const JRect&, bool active, int tabCount) {
                    if (tabCount > 1 && !active) return;
                    Panel* p = panelByTitle(dock->title());
                    if (!p) return;
                    JRect content = m_dockHost->contentArea(m_dockHost->findDock(dock));
                    buf.pushClip(content.x, content.y, content.width, content.height);
                    for (JWidget* w : p->widgets)
                        if (w->isVisible()) w->populateRenderPrimitives(buf);
                    
                    if (m_showPanelScrollbars && p->contentH > content.height) {
                        float scrollBarW = 6.0f;
                        float trackX = content.x + content.width - scrollBarW - 2.0f;
                        float handleH = std::max(20.0f, (content.height / p->contentH) * content.height);
                        float maxScroll = p->contentH - content.height;
                        float handleY = content.y + (p->scrollY / maxScroll) * (content.height - handleH);
                        
                        uint8_t handleC[4] = {255, 255, 255, 60};
                        buf.pushRectangle(trackX, handleY, scrollBarW, handleH, handleC, scrollBarW * 0.5f);
                    }
                    buf.popClip();
                });
        }

        // 3. Drag ghost + dock drop overlay on top of everything.
        if (m_tearableTab) m_tearableTab->populateDragGhost(buf);
        if (m_dockHost)    m_dockHost->populateOverlay(buf);
        if (m_menuBar)     m_menuBar->populateRenderPrimitives(buf);
    }

    // Per-frame: place each docked panel's content at its content area, lay it out at the
    // panel width (controls stretch), measure height, clamp scroll, and apply the wheel.
    void clearPanelVisibility() {
        for (auto& p : m_panels) if (p) p->visible = false;
    }

    // Per-frame: place each docked panel's content at its content area, lay it out at the
    // panel width (controls stretch), measure height, clamp scroll, and apply the wheel.
    void updateHostDockContent(JDockHost& host, float wheelDelta, float mouseX, float mouseY) {
        host.forEachDockPanel(
            [&](const JDockWidget* dock, const JRect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;          // only the active tab shows
                Panel* p = panelByTitle(dock->title());
                if (!p) return;
                JRect content = host.contentArea(host.findDock(dock));
                p->viewport = content;
                p->visible  = true;

                if (wheelDelta != 0.0f &&
                    mouseX >= content.x && mouseX < content.x + content.width &&
                    mouseY >= content.y && mouseY < content.y + content.height) {
                    bool consumed = false;
                    for (JWidget* w : p->widgets) {
                        if (w->handleScroll(mouseX, mouseY, wheelDelta)) {
                            consumed = true;
                        }
                    }
                    if (!consumed) {
                        p->scrollY -= wheelDelta * 40.0f;
                    }
                }

                auto& L = m_graph.getLayout(p->root);
                m_graph.computeMinSize(p->root);
                float panelMinW = m_graph.getLayoutConst(p->root).minWidth;
                float panelMinH = m_graph.getLayoutConst(p->root).minHeight;
                const_cast<JDockWidget*>(dock)->setMinSize(panelMinW, panelMinH);

                JConstraints cc{ content.width, content.width, 0.0f, 100000.0f };
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
        out.reserve(m_widgets.size() + 20);

        auto addWidgetNode = [&](const JWidget* w) {
            AiNodeDescriptor d{};
            NodeId nid = w->getNodeId();
            d.id = nid;
            const auto& bb = m_graph.getLayoutConst(nid).boundingBox;
            d.x = bb.x; d.y = bb.y; d.width = bb.width; d.height = bb.height;

            JAISemanticNode sn = w->getSemanticNode();
            aiSetField(d.role,  sizeof(d.role),  sn.role);
            aiSetField(d.name,  sizeof(d.name),  sn.label);
            aiSetField(d.value, sizeof(d.value), sn.value);

            uint32_t f = 0;
            if (w->isEnabled())  f |= AiEnabled;
            if (w->isVisible())  f |= AiVisible;
            if (sn.interactable) f |= AiInteractable;
            if (w->getState() == JWidgetState::Pressed) f |= AiPressed;
            if (w->getState() == JWidgetState::Focused) f |= AiFocused;
            d.stateFlags = f;
            out.push_back(d);
        };

        for (const auto& w : m_widgets) {
            addWidgetNode(w.get());
        }

        if (m_menuBar) {
            addWidgetNode(m_menuBar.get());
            // Expose the menu bar item buttons individually so they can be clicked
            for (size_t i = 0; i < m_menuBar->entries().size(); ++i) {
                const auto& entry = m_menuBar->entries()[i];
                AiNodeDescriptor d{};
                d.id = entry.btnId;
                const auto& bb = m_graph.getLayoutConst(entry.btnId).boundingBox;
                d.x = bb.x; d.y = bb.y; d.width = bb.width; d.height = bb.height;
                aiSetField(d.role,  sizeof(d.role),  "JButton");
                aiSetField(d.name,  sizeof(d.name),  entry.title);
                uint32_t f = AiVisible | AiEnabled | AiInteractable;
                if (m_menuBar->activeIndex() == static_cast<int>(i)) f |= AiFocused | AiPressed;
                d.stateFlags = f;
                out.push_back(d);
            }
        }

        // Docked panels (containers).  Synthetic ids offset past widget node ids so the
        // AI can see & address the docking layout too.  Floating panels are separate OS
        // windows and not included here.
        if (m_dockHost) {
            uint32_t dockId = kDockAiIdBase;
            m_dockHost->forEachDockPanel(
                [&](const JDockWidget* dock, const JRect& r, bool active, int tabCount) {
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

    bool adjustNodeDimension(JDockNodeId leafId, bool horizontal, float desiredPixels) {
        if (!m_dockHost) return false;
        JDockNodeId curr = leafId;
        while (curr.valid()) {
            JDockNode* cNode = m_dockHost->node(curr);
            if (!cNode) break;
            JDockNodeId parentId = cNode->parent;
            if (!parentId.valid()) break;
            JDockNode* parent = m_dockHost->node(parentId);
            JSplitDir targetDir = horizontal ? JSplitDir::Horizontal : JSplitDir::Vertical;
            if (parent->splitDir == targetDir) {
                int idx = -1;
                for (int i = 0; i < static_cast<int>(parent->children.size()); ++i) {
                    if (parent->children[i] == curr) { idx = i; break; }
                }
                if (idx == -1) return false;
                
                float totalDim = horizontal ? parent->rect.width : parent->rect.height;
                float handleSpace = JDockHost::HANDLE_HALF * 2.0f;
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
                [&](const JDockWidget* d, const JRect&, bool, int) {
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
                JDockWidget* dw = nullptr;
                m_dockHost->forEachDockPanel([&](const JDockWidget* d, const JRect&, bool, int) {
                    if (d->title() == title) dw = const_cast<JDockWidget*>(d);
                });
                if (!dw) return 0;
                JDockNodeId leafId = m_dockHost->findDock(dw);
                if (!leafId.valid()) return 0;
                return adjustNodeDimension(leafId, true, px) ? 1 : 0;
            }

            if (action.rfind("set_height:", 0) == 0) {
                float px = 0.f;
                try { px = std::stof(action.substr(11)); } catch (...) { return 0; }
                JDockWidget* dw = nullptr;
                m_dockHost->forEachDockPanel([&](const JDockWidget* d, const JRect&, bool, int) {
                    if (d->title() == title) dw = const_cast<JDockWidget*>(d);
                });
                if (!dw) return 0;
                JDockNodeId leafId = m_dockHost->findDock(dw);
                if (!leafId.valid()) return 0;
                return adjustNodeDimension(leafId, false, px) ? 1 : 0;
            }

            if (action.rfind("move_to:", 0) == 0) {
                std::string sub = action.substr(8);
                size_t colon = sub.find(':');
                std::string posStr = (colon == std::string::npos) ? sub : sub.substr(0, colon);
                std::string targetTitle = (colon == std::string::npos) ? "" : sub.substr(colon + 1);

                JDropPos pos = JDropPos::Center;
                if (posStr == "left")        pos = JDropPos::Left;
                else if (posStr == "right")  pos = JDropPos::Right;
                else if (posStr == "top")    pos = JDropPos::Top;
                else if (posStr == "bottom") pos = JDropPos::Bottom;
                else if (posStr == "center") pos = JDropPos::Center;
                else return 0;

                JDockWidget* sourceWidget = nullptr;
                m_dockHost->forEachDockPanel([&](const JDockWidget* d, const JRect&, bool, int) {
                    if (d->title() == title) sourceWidget = const_cast<JDockWidget*>(d);
                });
                if (!sourceWidget) return 0;

                if (!targetTitle.empty()) {
                    JDockWidget* targetWidget = nullptr;
                    m_dockHost->forEachDockPanel([&](const JDockWidget* d, const JRect&, bool, int) {
                        if (d->title() == targetTitle) targetWidget = const_cast<JDockWidget*>(d);
                    });
                    if (!targetWidget) return 0;

                    JDockNodeId targetLeaf = m_dockHost->findDock(targetWidget);
                    if (!targetLeaf.valid()) return 0;

                    m_dockHost->removeDock(sourceWidget);
                    if (pos == JDropPos::Center) {
                        m_dockHost->insertDock(sourceWidget, targetLeaf);
                    } else {
                        JDockNodeId newLeaf = m_dockHost->splitLeaf(targetLeaf, pos);
                        m_dockHost->insertDock(sourceWidget, newLeaf);
                    }
                } else {
                    JDockNodeId targetLeaf = m_dockHost->edgeLeaf();
                    if (!targetLeaf.valid()) return 0;

                    m_dockHost->removeDock(sourceWidget);
                    if (pos == JDropPos::Center) {
                        m_dockHost->insertDock(sourceWidget, targetLeaf);
                    } else {
                        JDockNodeId newLeaf = m_dockHost->splitLeaf(targetLeaf, pos);
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
        if (m_menuBar && m_menuBar->getNodeId() == targetId) {
            bool res = m_menuBar->executeSemanticAction(action);
            std::printf("[dispatchAiAction] Found JMenuBar. executeSemanticAction returned %d\n", res);
            std::fflush(stdout);
            return res ? 1 : 0;
        }
        if (m_menuBar) {
            for (const auto& entry : m_menuBar->entries()) {
                if (entry.btnId == targetId) {
                    // Clicking a menu button opens the menu
                    if (action == "click") {
                        // Locate entry index
                        for (size_t i = 0; i < m_menuBar->entries().size(); ++i) {
                            if (m_menuBar->entries()[i].btnId == targetId) {
                                // Simulate click: open menu
                                float mouseX = 0.f, mouseY = 0.f;
                                const auto& bb = m_graph.getLayoutConst(entry.btnId).boundingBox;
                                m_menuBar->handleMousePress(bb.x + 5.f, bb.y + 5.f);
                                std::printf("[dispatchAiAction] Clicked MenuBarBtn '%s'\n", entry.title.c_str());
                                std::fflush(stdout);
                                return 1;
                            }
                        }
                    }
                    return 0;
                }
            }
        }

        std::printf("[dispatchAiAction] JWidget targetId=%u NOT found in m_widgets\n", targetId);
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

            JWidget* injected = nullptr;
            if (type == "button" || type == "JButton") {
                injected = add<JButton>(m_graph, label.empty() ? "JButton" : label, 200.0f, 36.0f);
            } else if (type == "label" || type == "JLabel") {
                injected = add<JLabel>(m_graph, label.empty() ? "JLabel" : label, 300.0f, 20.0f);
            } else if (type == "checkbox" || type == "JCheckBox") {
                injected = add<JCheckBox>(m_graph, label.empty() ? "JCheckBox" : label, 280.0f, 22.0f);
            } else if (type == "lineedit" || type == "JLineEdit") {
                injected = add<JLineEdit>(m_graph, label.empty() ? "Enter text..." : label, 320.0f, 32.0f);
            } else if (type == "textarea" || type == "JTextArea") {
                injected = add<JTextArea>(m_graph, label.empty() ? "Enter paragraph..." : label, 320.0f, 100.0f);
            } else if (type == "listview" || type == "JListView") {
                injected = add<JListView>(m_graph, std::vector<std::string>{"Item A", "Item B", "Item C"}, 240.0f, 120.0f);
            } else if (type == "treeview" || type == "JTreeView") {
                auto* tv = add<JTreeView>(m_graph, 240.0f, 150.0f);
                tv->setRootNode(JTreeViewNode{
                    "Root", true, false, {
                        {"Injected Node A", false, false, {}},
                        {"Injected Node B", true, false, {
                            {"Injected Child B1", false, false, {}}
                        }}
                    }
                });
                injected = tv;
            } else if (type == "datagrid" || type == "JDataGrid") {
                auto* dg = add<JDataGrid>(m_graph, std::vector<std::string>{"Col 1", "Col 2"}, 320.0f, 150.0f);
                dg->setRows({
                    {"Injected JRow 1 Cel 1", "Injected JRow 1 Cel 2"},
                    {"Injected JRow 2 Cel 1", "Injected JRow 2 Cel 2"}
                });
                injected = dg;
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
            for (auto& p_ptr : m_panels) {
                if (!p_ptr) continue;
                auto& p = *p_ptr;
                auto it = std::find_if(p.widgets.begin(), p.widgets.end(),
                    [nid](JWidget* w){ return w->getNodeId() == nid; });
                if (it != p.widgets.end()) {
                    p.widgets.erase(it);
                    // Remove from master ownership list too.
                    m_widgets.erase(
                        std::remove_if(m_widgets.begin(), m_widgets.end(),
                            [nid](const std::unique_ptr<JWidget>& w){ return w->getNodeId() == nid; }),
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

    void handleMouse(float x, float y, bool pressed, bool released, float wheel = 0.0f) {
        if (m_menuBar) {
            m_menuBar->handleMouseMove(x, y);
            if (pressed) m_menuBar->handleMousePress(x, y);
            if (released) m_menuBar->handleMouseRelease(x, y);
            if (y >= 0.f && y < 32.f) {
                return;
            }
        }

        if (released) {
            m_scrollDraggingPanel = nullptr;
        }

        if (m_scrollDraggingPanel) {
            Panel* p = m_scrollDraggingPanel;
            float trackH = p->viewport.height;
            float handleH = std::max(20.0f, (p->viewport.height / p->contentH) * p->viewport.height);
            float thumbRange = trackH - handleH;
            float maxScroll = p->contentH - p->viewport.height;
            if (thumbRange > 0.0f) {
                p->scrollY = std::clamp(m_scrollDragStartScrollY + (y - m_scrollDragStartY) * maxScroll / thumbRange, 0.0f, maxScroll);
                m_graph.invalidateNode(p->root, DirtySelf);
            }
            return;
        }

        // Route to the controls of currently-visible docked panels only.  Floated panels
        // receive input through handleFloatingPanelInput (in their own window's coords).
        bool hitAny = false;
        for (auto& p_ptr : m_panels) {
            if (!p_ptr) continue;
            Panel& p = *p_ptr;
            if (!p.visible) continue;

            if (pressed && m_showPanelScrollbars && p.contentH > p.viewport.height) {
                float scrollBarW = 6.0f;
                float trackX = p.viewport.x + p.viewport.width - scrollBarW - 2.0f;
                if (x >= trackX - 10.0f && x <= trackX + scrollBarW + 10.0f &&
                    y >= p.viewport.y && y <= p.viewport.y + p.viewport.height) {
                    m_scrollDraggingPanel = &p;
                    m_scrollDragStartY = y;
                    m_scrollDragStartScrollY = p.scrollY;
                    hitAny = true;
                    break;
                }
            }

            for (JWidget* w : p.widgets) {
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
    void renderFloatingPanel(const std::string& title, JPrimitiveBuffer& buf, const JRect& content) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        auto& L = m_graph.getLayout(p->root);
        m_graph.invalidateNode(p->root, DirtySelf);
        L.boundingBox.x = content.x;
        L.boundingBox.y = content.y - p->scrollY;
        m_graph.computeLayout(p->root, JConstraints{content.width, content.width, 0.0f, 100000.0f});
        buf.pushClip(content.x, content.y, content.width, content.height);
        for (JWidget* wt : p->widgets) if (wt->isVisible()) wt->populateRenderPrimitives(buf);
        
        if (m_showPanelScrollbars && p->contentH > content.height) {
            float scrollBarW = 6.0f;
            float trackX = content.x + content.width - scrollBarW - 2.0f;
            float handleH = std::max(20.0f, (content.height / p->contentH) * content.height);
            float maxScroll = p->contentH - content.height;
            float handleY = content.y + (p->scrollY / maxScroll) * (content.height - handleH);
            
            uint8_t handleC[4] = {255, 255, 255, 60};
            buf.pushRectangle(trackX, handleY, scrollBarW, handleH, handleC, scrollBarW * 0.5f);
        }
        buf.popClip();
    }

    void renderHostFloatingPanels(JDockHost& host, JPrimitiveBuffer& buf) {
        host.forEachDockPanel(
            [&](const JDockWidget* dock, const JRect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;
                JRect content = host.contentArea(host.findDock(dock));
                renderFloatingPanel(dock->title(), buf, content);
            });
    }

    void handleHostFloatingPanelsInput(JDockHost& host, float x, float y, bool pr, bool rl, float wheel = 0.0f) {
        if (rl) {
            m_scrollDraggingPanel = nullptr;
        }

        if (m_scrollDraggingPanel) {
            handleFloatingPanelInput(m_scrollDraggingPanel->title, x, y, pr, rl, wheel);
            return;
        }

        host.forEachDockPanel(
            [&](const JDockWidget* dock, const JRect&, bool active, int tabCount) {
                if (tabCount > 1 && !active) return;
                JRect content = host.contentArea(host.findDock(dock));
                if (x >= content.x && x < content.x + content.width &&
                    y >= content.y && y < content.y + content.height) {
                    handleFloatingPanelInput(dock->title(), x, y, pr, rl, wheel);
                }
            });
    }

    // Drive a floated panel's content (window-local coords).
    void handleFloatingPanelInput(const std::string& title, float x, float y, bool press, bool release, float wheel = 0.0f) {
        if (release) {
            m_scrollDraggingPanel = nullptr;
        }

        if (m_scrollDraggingPanel && m_scrollDraggingPanel->title == title) {
            Panel* p = m_scrollDraggingPanel;
            float trackH = p->viewport.height;
            float handleH = std::max(20.0f, (p->viewport.height / p->contentH) * p->viewport.height);
            float thumbRange = trackH - handleH;
            float maxScroll = p->contentH - p->viewport.height;
            if (thumbRange > 0.0f) {
                p->scrollY = std::clamp(m_scrollDragStartScrollY + (y - m_scrollDragStartY) * maxScroll / thumbRange, 0.0f, maxScroll);
                m_graph.invalidateNode(p->root, DirtySelf);
            }
            return;
        }

        Panel* p = panelByTitle(title);
        if (!p) return;
        bool hitAny = false;

        if (press && m_showPanelScrollbars && p->contentH > p->viewport.height) {
            float scrollBarW = 6.0f;
            float trackX = p->viewport.x + p->viewport.width - scrollBarW - 2.0f;
            if (x >= trackX - 4.0f && x <= trackX + scrollBarW + 4.0f &&
                y >= p->viewport.y && y <= p->viewport.y + p->viewport.height) {
                m_scrollDraggingPanel = p;
                m_scrollDragStartY = y;
                m_scrollDragStartScrollY = p->scrollY;
                hitAny = true;
            }
        }

        if (!hitAny) {
            for (JWidget* wt : p->widgets) {
                wt->handleMouseMove(x, y);
                if (press) {
                    wt->handleMousePress(x, y);
                    if (wt->hitTest(x, y)) hitAny = true;
                }
                if (release) wt->handleMouseRelease(x, y);
            }
        }

        if (press && !hitAny) {
            m_focus.setFocus(nullptr);
        }
    }

    void forceTear(int idx) {
        if (m_tearableTab) m_tearableTab->forceTear(idx);
    }

    std::optional<std::pair<JTornTabState, std::pair<float,float>>> consumeNewFloat() {
        if (!m_tearableTab || !m_tearableTab->hasTornTab()) return std::nullopt;
        auto state = m_tearableTab->consumeTornTab();
        float dx = m_tearableTab->lastDragX();
        float dy = m_tearableTab->lastDragY();
        return std::make_pair(std::move(state), std::make_pair(dx, dy));
    }

    JDockHost& dockHost() { return *m_dockHost; }

    void removeInlineDock(JDockWidget* ptr) {
        m_inlineDocks.erase(
            std::remove_if(m_inlineDocks.begin(), m_inlineDocks.end(),
                [ptr](const auto& u){ return u.get() == ptr; }),
            m_inlineDocks.end());
    }

    // Transfer a dock from a JFloatingDockWindow into this catalog's ownership
    // after a successful re-dock.  oldPtr is the address tryCommitDrop already
    // stored in the JDockHost tree; we fix it up to point at the new allocation.
    void adoptInlineDock(std::unique_ptr<JDockWidget> d, JDockWidget* oldPtr) {
        JDockWidget* raw = d.get();
        m_inlineDocks.push_back(std::move(d));
        if (m_dockHost) m_dockHost->retargetDock(oldPtr, raw);
    }

    void buildMenus() {
        // Create menus
        auto fileMenu = std::make_unique<jf::JMenu>("File");
        fileMenu->add(m_graph, "New Project", {jf::JKeyEvent::JKey::N, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] New Project triggered\n";
        });
        fileMenu->add(m_graph, "Open Project...", {jf::JKeyEvent::JKey::O, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Open Project triggered\n";
        });
        fileMenu->addSeparator(m_graph);
        fileMenu->add(m_graph, "Save", {jf::JKeyEvent::JKey::S, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Save triggered\n";
        });
        fileMenu->add(m_graph, "Save As...", {jf::JKeyEvent::JKey::S, true, false, true})->onTriggered.connect([]() {
            std::cout << "[MENU] Save As triggered\n";
        });
        fileMenu->addSeparator(m_graph);
        fileMenu->add(m_graph, "Exit", {jf::JKeyEvent::JKey::Unknown, false, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Exit triggered\n";
        });

        auto editMenu = std::make_unique<jf::JMenu>("Edit");
        editMenu->add(m_graph, "Undo", {jf::JKeyEvent::JKey::Z, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Undo triggered\n";
        });
        editMenu->add(m_graph, "Redo", {jf::JKeyEvent::JKey::Y, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Redo triggered\n";
        });
        editMenu->addSeparator(m_graph);
        editMenu->add(m_graph, "Cut", {jf::JKeyEvent::JKey::X, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Cut triggered\n";
        });
        editMenu->add(m_graph, "Copy", {jf::JKeyEvent::JKey::C, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Copy triggered\n";
        });
        editMenu->add(m_graph, "Paste", {jf::JKeyEvent::JKey::V, true, false, false})->onTriggered.connect([]() {
            std::cout << "[MENU] Paste triggered\n";
        });
        
        auto prefMenu = std::make_unique<jf::JMenu>("Preferences");
        auto* prefItem1 = prefMenu->add(m_graph, "Embedded JSlider");
        prefItem1->setEmbeddedWidgetFactory([](JSceneGraph& g) -> std::unique_ptr<JWidget> {
            auto slider = std::make_unique<jf::JSlider>(g, 120.f, 20.f);
            slider->setValue(0.5f);
            return slider;
        });
        
        jf::JMenu* prefMenuPtr = prefMenu.get();
        m_submenus.push_back(std::move(prefMenu));
        
        editMenu->addSeparator(m_graph);
        editMenu->add(m_graph, "Preferences...", {}, prefMenuPtr);

        auto viewMenu = std::make_unique<jf::JMenu>("View");
        auto* scrollbarsItem = viewMenu->add(m_graph, "Show Scrollbars");
        scrollbarsItem->setCheckable(true);
        scrollbarsItem->setChecked(m_showPanelScrollbars);
        scrollbarsItem->onTriggered.connect([this, scrollbarsItem]() {
            setShowPanelScrollbars(scrollbarsItem->isChecked());
            std::cout << "[MENU] Toggle Scrollbars: " << m_showPanelScrollbars << "\n";
        });

        auto* animItem = viewMenu->add(m_graph, "Animate Progress");
        animItem->setCheckable(true);
        animItem->setChecked(!m_animPaused);
        animItem->onTriggered.connect([this, animItem]() {
            m_animPaused = !animItem->isChecked();
            std::cout << "[MENU] Animate Progress: " << !m_animPaused << "\n";
        });

        auto helpMenu = std::make_unique<jf::JMenu>("Help");
        helpMenu->add(m_graph, "Documentation")->onTriggered.connect([]() {
            std::cout << "[MENU] Help Documentation triggered\n";
        });
        helpMenu->addSeparator(m_graph);
        helpMenu->add(m_graph, "About Genesis UI...") ->onTriggered.connect([]() {
            std::cout << "[MENU] About Genesis UI triggered\n";
        });

        // Set tooltips!
        for (const auto& item : fileMenu->items()) {
            if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                mi->setTooltip("Execute " + mi->label() + " action");
            }
        }
        for (const auto& item : editMenu->items()) {
            if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                mi->setTooltip("Edit: " + mi->label());
            }
        }
        for (const auto& item : viewMenu->items()) {
            if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                mi->setTooltip("Configure: " + mi->label());
            }
        }

        m_menus.push_back(std::move(fileMenu));
        m_menus.push_back(std::move(editMenu));
        m_menus.push_back(std::move(viewMenu));
        m_menus.push_back(std::move(helpMenu));
        
        m_menuBar = std::make_unique<jf::JMenuBar>(m_graph);
        for (const auto& m : m_menus) {
            m_menuBar->addMenu(m.get());
        }

        // Register global shortcuts
        jf::JMenuManager::instance().clearShortcuts();
        for (const auto& m : m_menus) {
            for (const auto& item : m->items()) {
                if (!item) continue;
                if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                    if (mi->shortcut().key != jf::JKeyEvent::JKey::Unknown) {
                        jf::JMenuManager::instance().registerShortcut(mi->shortcut(), [mi]() {
                            mi->onTriggered.emit();
                        });
                    }
                }
            }
        }
    }

private:
    void buildUI() {
        buildMenus();
        // ---- Content, grouped into dock panels (each is a scrollable flex column) ----
        beginPanel("Navigator");
        section("Navigation");
        add<JTabBar>(m_graph, std::vector<std::string>{"Overview", "Controls", "Themes", "JSettings"}, 200.0f, 34.0f);
        section("Actions");
        add<JButton>(m_graph, "Primary JAction", 200.0f, 36.0f);
        add<JButton>(m_graph, "Secondary",      160.0f, 36.0f);
        section("Dialogs");
        {
            auto* msgBtn = add<JButton>(m_graph, "Message JDialog", 200.0f, 34.0f);
            msgBtn->onClicked.connect([]{
                jf::JDialog::message("Genesis UI",
                    "This is a native genesis dialog overlay.\nNo OS dependency required.");
            });
            auto* cfmBtn = add<JButton>(m_graph, "Confirm JDialog", 200.0f, 34.0f);
            cfmBtn->onClicked.connect([]{
                jf::JDialog::confirm("Delete File?",
                    "Are you sure you want to delete the selected file?\nThis cannot be undone.",
                    []{ jf::JDialog::message("Deleted", "File deleted."); },
                    []{ jf::JDialog::message("Cancelled", "Nothing was deleted."); });
            });
            auto* inBtn = add<JButton>(m_graph, "Input JDialog", 200.0f, 34.0f);
            inBtn->onClicked.connect([]{
                jf::JDialog::input("Rename File",
                    "Enter the new file name:",
                    [](std::string name){
                        jf::JDialog::message("Renamed", "File renamed to: " + name);
                    },
                    {}, "new_name.cpp");
            });
        }
        add<JToggleButton>(m_graph, "Dark JMode", 180.0f, 34.0f);
        add<JToggleButton>(m_graph, "Auto-save", 180.0f, 34.0f)->setToggled(true);
        section("Project Files");
        add<JListView>(m_graph, std::vector<std::string>{
            "src/core/JSceneGraph.cpp",
            "src/core/BaseWidgets.cpp",
            "src/core/JStyleEngine.cpp",
            "src/graphics/VulkanGpuHal.cpp",
            "src/graphics/JFontEngine.cpp",
            "src/platforms/linux/Platform.cpp",
            "include/genesis/core/JWidget.h"
        }, 200.0f, 120.0f);

        section("Workspace Tree");
        {
            auto* tv = add<JTreeView>(m_graph, 200.0f, 150.0f);
            tv->setRootNode(JTreeViewNode{
                "Root", true, false, {
                    {"src", true, false, {
                        {"core", true, false, {
                            {"JSceneGraph.cpp", false, false, {}},
                            {"BaseWidgets.cpp", false, false, {}}
                        }},
                        {"graphics", false, false, {
                            {"VulkanGpuHal.cpp", false, false, {}}
                        }}
                    }},
                    {"include", false, false, {
                        {"genesis", false, false, {}}
                    }},
                    {"CMakeLists.txt", false, false, {}}
                }
            });
        }

        beginPanel("Properties");
        section("Toggles");
        add<JCheckBox>(m_graph, "Enable hardware acceleration", 320.0f, 22.0f)->setChecked(true);
        add<JCheckBox>(m_graph, "Show tooltips",                280.0f, 22.0f);
        separator();
        section("Backend");
        add<JRadioButton>(m_graph, "Vulkan backend",  260.0f, 22.0f)->setSelected(true);
        add<JRadioButton>(m_graph, "Metal backend",   260.0f, 22.0f);
        add<JRadioButton>(m_graph, "Software (lvp)",  260.0f, 22.0f);

        beginPanel("Inspector");
        section("Range");
        add<JSlider>(m_graph, 340.0f, 24.0f)->setValue(0.65f);
        add<JSlider>(m_graph, 340.0f, 24.0f)->setValue(0.30f);
        m_progressBar = add<JProgressBar>(m_graph, 340.0f, 14.0f);
        add<JProgressBar>(m_graph, 340.0f, 14.0f)->setProgress(1.0f);
        add<JScrollBar>(m_graph, 340.0f, 14.0f, 0.25f)->setScrollPosition(0.35f);
        section("Steppers");
        add<JSpinBox>(m_graph, 0, 255, 160.0f, 32.0f)->setValue(42);
        add<JSpinBox>(m_graph, 0, 100, 160.0f, 32.0f)->setValue(75);
        section("Data Grid");
        {
            auto* dg = add<JDataGrid>(m_graph, std::vector<std::string>{"File", "Lines", "Size"}, 340.0f, 150.0f);
            dg->setColumnWidths({160.0f, 80.0f, 80.0f});
            dg->setRows({
                {"JSceneGraph.cpp", "824", "32.4 KB"},
                {"BaseWidgets.cpp", "2450", "95.8 KB"},
                {"VulkanGpuHal.cpp", "1480", "58.2 KB"},
                {"JFontEngine.cpp", "920", "36.1 KB"},
                {"Platform.cpp", "540", "21.0 KB"}
            });
        }

        beginPanel("Console");
        section("Filters");
        add<JLineEdit>(m_graph, "Search widgets...", 340.0f, 32.0f);
        add<JLineEdit>(m_graph, "API endpoint URL", 340.0f, 32.0f);
        section("Terminal Log");
        add<JTextArea>(m_graph, "No logs yet. JType here...", 340.0f, 100.0f);
        add<JLabel>(m_graph, "Genesis UI Toolkit — Zero-dependency, AI-native", 460.0f, 20.0f);

        beginPanel("Output");
        section("Display");
        {
            auto cb1 = add<JComboBox>(m_graph, std::vector<std::string>{"1080p", "1440p", "4K", "8K"}, 200.0f, 34.0f);
            cb1->setCurrentIndex(1);
            cb1->setMode(JComboBoxMode::Popup);
            cb1->onPopupRequested.connect([this](JComboBox* cb) {
                if (onComboBoxPopupRequested) onComboBoxPopupRequested(cb);
            });

            auto cb2 = add<JComboBox>(m_graph, std::vector<std::string>{"60 Hz", "120 Hz", "144 Hz", "240 Hz"}, 200.0f, 34.0f);
            cb2->setCurrentIndex(2);
            cb2->setMode(JComboBoxMode::Popup);
            cb2->onPopupRequested.connect([this](JComboBox* cb) {
                if (onComboBoxPopupRequested) onComboBoxPopupRequested(cb);
            });
        }
        add<JGroupBox>(m_graph, "Render JSettings", 340.0f, 90.0f);

        beginPanel("Assets");
        section("Tear-off Tabs — drag a tab down to detach");
        {
            auto tb = std::make_unique<JTabBar>(m_graph,
                std::vector<std::string>{"Properties", "Console", "Assets"}, 300.0f, 34.0f);
            tb->setTearable(true);
            m_tearableTab = tb.get();
            m_graph.addChild(m_curContainer, tb->getNodeId());
            m_curPanel->widgets.push_back(tb.get());
            m_widgets.push_back(std::move(tb));
        }
        section("JScrollArea Sandbox");
        {
            auto sa = std::make_unique<JScrollArea>(m_graph, 340.0f, 160.0f);
            
            // Create a JGroupBox inside the JScrollArea
            auto innerGb = new JGroupBox(m_graph, "Asset Directory", 320.0f, 300.0f);
            sa->addChildWidget(innerGb);
            m_widgets.push_back(std::unique_ptr<JWidget>(innerGb));

            // Populate the JGroupBox with children
            for (int i = 1; i <= 8; ++i) {
                auto cb = new JCheckBox(m_graph, "src/widget/File_" + std::to_string(i) + ".cpp", 280.0f, 22.0f);
                m_graph.addChild(innerGb->getNodeId(), cb->getNodeId());
                m_widgets.push_back(std::unique_ptr<JWidget>(cb));
            }
            
            m_graph.addChild(m_curContainer, sa->getNodeId());
            m_curPanel->widgets.push_back(sa.get());
            m_widgets.push_back(std::move(sa));
        }

        beginPanel("New Features");
        section("JAnimator — EaseInOut / EaseOutElastic / EaseOutBounce");
        m_animBar0 = add<JProgressBar>(m_graph, 340.0f, 12.0f);
        m_animBar1 = add<JProgressBar>(m_graph, 340.0f, 12.0f);
        m_animBar2 = add<JProgressBar>(m_graph, 340.0f, 12.0f);
        m_animSlot0 = m_demoAnimator.add(0.0f);
        m_animSlot1 = m_demoAnimator.add(0.0f);
        m_animSlot2 = m_demoAnimator.add(0.0f);
        m_demoAnimator.animateTo(m_animSlot0, 1.0f, 1200.0f, jf::JEasing::EaseInOut);
        m_demoAnimator.animateTo(m_animSlot1, 1.0f, 1800.0f, jf::JEasing::EaseOutElastic);
        m_demoAnimator.animateTo(m_animSlot2, 1.0f, 2400.0f, jf::JEasing::EaseOutBounce);

        section("JTextArea — Shift+Arrow selects, Ctrl+C/V/X/A");
        {
            auto* ta = add<JTextArea>(m_graph, "", 340.0f, 80.0f);
            ta->setText("Genesis UI — zero-dependency widget toolkit.\n"
                        "Select text with Shift+Arrow or Ctrl+A for all.\n"
                        "Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes.");
        }

        section("JClipboard");
        {
            auto* copyBtn  = add<JButton>(m_graph, "Copy sample text", 200.0f, 32.0f);
            auto* pasteBtn = add<JButton>(m_graph, "Paste into label", 200.0f, 32.0f);
            m_clipboardLabel = add<JLabel>(m_graph, "(click Paste to show clipboard)", 340.0f, 18.0f);
            JLabel* lbl = m_clipboardLabel;
            copyBtn->onClicked.connect([lbl] {
                JClipboard::setText("Hello from Genesis UI clipboard!");
                lbl->setText("Copied sample text to clipboard.");
            });
            pasteBtn->onClicked.connect([lbl] {
                std::string t = JClipboard::getText();
                lbl->setText(t.empty() ? "(clipboard is empty)" : t.substr(0, 48));
            });
        }

        section("File Dialogs");
        {
            m_fileDialogLabel = add<JLabel>(m_graph, "(no path selected)", 340.0f, 18.0f);
            JLabel* lbl = m_fileDialogLabel;
            auto* openBtn = add<JButton>(m_graph, "Open File...", 150.0f, 32.0f);
            auto* saveBtn = add<JButton>(m_graph, "Save File...", 150.0f, 32.0f);
            auto* dirBtn  = add<JButton>(m_graph, "Open Folder...", 160.0f, 32.0f);
            openBtn->onClicked.connect([lbl] {
                std::string f = JFileDialog::openFile("Select a File");
                lbl->setText(f.empty() ? "(cancelled)" : f);
            });
            saveBtn->onClicked.connect([lbl] {
                std::string f = JFileDialog::saveFile("Save As", "output.txt");
                lbl->setText(f.empty() ? "(cancelled)" : f);
            });
            dirBtn->onClicked.connect([lbl] {
                std::string f = JFileDialog::openDirectory("Select Folder");
                lbl->setText(f.empty() ? "(cancelled)" : f);
            });
        }

        section("Image JWidget");
        m_imageWidget = add<JImageWidget>(m_graph, kNullTexture, 200.0f, 100.0f);
        add<JLabel>(m_graph, "RGBA gradient via JGpuHal::uploadTexture()", 340.0f, 16.0f);

        section("JSplitter — drag the divider");
        {
            auto* lblA = new JLabel(m_graph, "JPane A", 140.0f, 50.0f);
            auto* lblB = new JLabel(m_graph, "JPane B", 140.0f, 50.0f);
            auto split = std::make_unique<JSplitter>(m_graph, JSplitter::JOrientation::Horizontal, 340.0f, 50.0f);
            m_demoSplitter = split.get();
            split->addPane(lblA, 0.5f);
            split->addPane(lblB, 0.5f);
            m_curPanel->widgets.push_back(lblA);
            m_curPanel->widgets.push_back(lblB);
            m_widgets.push_back(std::unique_ptr<JWidget>(lblA));
            m_widgets.push_back(std::unique_ptr<JWidget>(lblB));
            m_graph.addChild(m_curContainer, split->getNodeId());
            m_curPanel->widgets.push_back(split.get());
            m_widgets.push_back(std::move(split));
        }

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

        m_dockHost = std::make_unique<JDockHost>();
        m_dockHost->setRootSplit(JSplitDir::Horizontal);

        // ---- Left column (Navigator + Assets) ----
        JDockNodeId leftPanel  = m_dockHost->addSplit(m_dockHost->rootId(), JSplitDir::Vertical, 0.22f);
        JDockNodeId navLeaf    = m_dockHost->addLeaf(leftPanel,  "nav-zone",   0.40f);
        JDockNodeId assetLeaf  = m_dockHost->addLeaf(leftPanel,  "asset-zone", 0.60f);

        // ---- Right column (Properties, Inspector, output group) ----
        JDockNodeId rightPanel = m_dockHost->addSplit(m_dockHost->rootId(), JSplitDir::Vertical, 0.78f);
        JDockNodeId topRight   = m_dockHost->addSplit(rightPanel, JSplitDir::Horizontal, 0.42f);

        JDockNodeId propsLeaf   = m_dockHost->addLeaf(topRight,   "props-zone",   0.50f);
        JDockNodeId inspectLeaf = m_dockHost->addLeaf(topRight,   "inspect-zone", 0.50f);
        JDockNodeId outputLeaf  = m_dockHost->addLeaf(rightPanel, "output-group", 0.58f);

        // All panels are floatable, tabifiable, and accept drops everywhere, so any panel
        // can be re-docked anywhere after floating.  (The constraint API — affinity,
        // allowedDrops, setFloatable/Tabifiable, min/max size — still exists; this demo
        // just keeps it permissive for free-form docking.)
        auto makeDock = [&](const char* title, float w, float h, JDockNodeId leaf) {
            auto d = std::make_unique<JDockWidget>(title, 0.f, 0.f, w, h);
            d->setMinSize(120.f, 80.f);
            m_dockHost->insertDock(d.get(), leaf);
            m_inlineDocks.push_back(std::move(d));
        };
        makeDock("Properties", 260.f, 200.f, propsLeaf);
        makeDock("Inspector",  260.f, 200.f, inspectLeaf);
        makeDock("Console",       260.f, 160.f, outputLeaf);
        makeDock("Output",        260.f, 160.f, outputLeaf);
        makeDock("New Features",  260.f, 200.f, outputLeaf);
        makeDock("Navigator",  200.f, 300.f, navLeaf);
        makeDock("Assets",     200.f, 280.f, assetLeaf);

        constexpr float kMenuY_ = 28.f + 32.f;  // kTitleH + menuBar height
        m_dockHost->computeLayout({0.f, kMenuY_,
            static_cast<float>(m_winW), static_cast<float>(m_winH) - kMenuY_});
        layoutMenuBar(static_cast<float>(m_winW), 28.f);
    }

    // A dock panel's scrollable content: a flex-column root + the widgets it owns-by-ref,
    // its scroll offset, measured content height, and on-screen viewport this frame.
    struct Panel {
        std::string          title;
        NodeId               root{InvalidNodeId};
        std::vector<JWidget*> widgets;        // non-owning; rendered clipped to viewport
        float                scrollY{0.0f};
        float                contentH{0.0f};
        JRect                 viewport{};     // dock content area (set each frame)
        bool                 visible{false};
    };

    // Begin filling a dock panel by title.  Subsequent add()/section()/spacer() go here.
    void beginPanel(const std::string& title) {
        NodeId root = m_graph.createNode("PanelContent:" + title);
        auto& L = m_graph.getLayout(root);
        L.direction  = JFlexDirection::Column;
        L.padding    = 14.0f;
        L.gap        = 8.0f;
        L.alignItems = JAlignItems::Stretch;   // controls fill the panel width
        m_panels.push_back(std::make_unique<Panel>(Panel{title, root, {}, 0.f, 0.f, {}, false}));
        m_curPanel     = m_panels.back().get();
        m_curContainer = root;
    }

public:
    void layoutExtra() {
        if (m_demoSplitter) m_demoSplitter->layout();
    }

    void initTextures(JGpuHal& hal) {
        std::vector<uint8_t> rgba(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                int i = (y * 64 + x) * 4;
                rgba[i + 0] = 255;
                rgba[i + 1] = static_cast<uint8_t>(x * 4);
                rgba[i + 2] = static_cast<uint8_t>(y * 4);
                rgba[i + 3] = 255;
            }
        }
        m_imageTex = hal.uploadTexture(rgba.data(), 64, 64);
        if (m_imageWidget && m_imageTex != kNullTexture)
            m_imageWidget->setTexture(m_imageTex);
    }

    void createPanelFromMenu(jf::JMenu* menu) {
        if (panelByTitle(menu->title())) return;

        beginPanel(menu->title());
        // JMenu items have a natural popup width (~152px content, matching the 180px popup
        // minus the 14px padding on each side). Don't stretch them to fill wider dock slots.
        m_graph.getLayout(m_curContainer).alignItems = JAlignItems::Start;
        static constexpr float kMenuItemMinW = 152.f;
        for (const auto& item : menu->items()) {
            if (!item) continue;
            if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                auto* added = add<jf::JMenuItem>(m_graph, mi->label(), mi->shortcut(), mi->submenu());
                added->setCheckable(mi->isCheckable());
                added->setChecked(mi->isChecked());
                added->setTooltip(mi->tooltip());
                if (mi->embeddedWidgetFactory()) {
                    added->setEmbeddedWidgetFactory(mi->embeddedWidgetFactory());
                }
                added->onTriggered.connect([mi]() {
                    mi->onTriggered.emit();
                });
                m_graph.getLayout(added->getNodeId()).minWidth = kMenuItemMinW;
            } else if (auto* sep = dynamic_cast<jf::JMenuSeparator*>(item.get())) {
                (void)sep;
                auto* added = add<jf::JMenuSeparator>(m_graph);
                m_graph.getLayout(added->getNodeId()).minWidth = kMenuItemMinW;
            }
        }
    }

    void section(const std::string& name) {
        add<JLabel>(m_graph, name, 300.0f, 18.0f);
    }

    void separator() {
        spacer(2.0f);
        add<JSeparator>(m_graph, JSeparator::JOrientation::Horizontal, 300.0f);
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

        if constexpr (std::is_base_of_v<JControl, T>) {
            m_focus.registerWidget(ptr);
            ptr->onClicked.connect([this, ptr]() {
                m_focus.setFocus(ptr);
            });
        }

        return ptr;
    }

    Panel* panelByTitle(const std::string& t) {
        for (auto& p : m_panels) if (p && p->title == t) return p.get();
        return nullptr;
    }

    float panelMinHeight(const std::string& t) {
        Panel* p = panelByTitle(t);
        if (!p) return 0.f;
        m_graph.computeMinSize(p->root);
        return m_graph.getLayoutConst(p->root).minHeight;
    }

    JSceneGraph& m_graph;
    JFocusManager& m_focus;
    uint32_t    m_winW, m_winH;
    std::vector<std::unique_ptr<JWidget>> m_widgets;
    std::vector<std::unique_ptr<Panel>> m_panels;
    NodeId      m_curContainer{InvalidNodeId};
    Panel*      m_curPanel{nullptr};
    JProgressBar* m_progressBar{nullptr};
    JTabBar*      m_tearableTab{nullptr};

    std::unique_ptr<JDockHost>                m_dockHost;
    std::vector<std::unique_ptr<JDockWidget>> m_inlineDocks;
    float m_elapsed{0.0f};
    bool  m_animPaused{false};
    bool  m_showPanelScrollbars{true};

    Panel* m_scrollDraggingPanel{nullptr};
    float  m_scrollDragStartY{0.0f};
    float  m_scrollDragStartScrollY{0.0f};

    std::unique_ptr<jf::JMenuBar> m_menuBar;
    std::vector<std::unique_ptr<jf::JMenu>> m_menus;
    std::vector<std::unique_ptr<jf::JMenu>> m_submenus;

    // ---- New-feature demos ----
    jf::JAnimator m_demoAnimator;
    size_t         m_animSlot0{0}, m_animSlot1{0}, m_animSlot2{0};
    JProgressBar*   m_animBar0{nullptr};
    JProgressBar*   m_animBar1{nullptr};
    JProgressBar*   m_animBar2{nullptr};
    bool           m_animForward{true};
    JSplitter*      m_demoSplitter{nullptr};
    JLabel*         m_clipboardLabel{nullptr};
    JLabel*         m_fileDialogLabel{nullptr};
    JImageWidget*   m_imageWidget{nullptr};
    TextureHandle  m_imageTex{kNullTexture};
};

// ============================================================================
// JEntry point
// ============================================================================

int main() {
    std::cout << "[GENESIS] Controls Catalog starting...\n";

    constexpr uint32_t W = 760, H = 860;
    uint32_t curW = W, curH = H;   // swapchain size, kept locked to the window size

    // ---- Custom title bar constants ----------------------------------------
    constexpr float kTitleH  = 28.f;   // custom title bar height
    constexpr float kMenuY   = kTitleH; // menu bar y-offset below title bar
    constexpr float kDockY   = kMenuY + 32.f;  // dock starts below menu bar
    constexpr float kBtnW    = 28.f;   // width of each window-control button

    // (Title-bar dragging is handled by _NET_WM_MOVERESIZE — no local state needed.)

    jf::JTranslationEngine::instance().setSearchPath("./translations");
    jf::JTranslationEngine::instance().setLocale("en");

    std::string winTitle = "Genesis Controls Catalog";
    if (getenv("GENESIS_WIN_TITLE") != nullptr) {
        winTitle = getenv("GENESIS_WIN_TITLE");
    }
    auto window = std::make_unique<PlatformWindowImpl>(
        winTitle, W, H, 100, 100, jf::JPlatformWindowStyle::Borderless);

    JNativeWindowHandle handle = window->nativeHandle();

    auto hal = JGpuHal::create(JGpuApiType::Vulkan, handle);
    if (!hal) { std::cerr << "[GENESIS] Failed to create Vulkan HAL\n"; return -1; }
    hal->resizeSwapchain(W, H);

    jf::JFontEngine fontEngine;
    if (fontEngine.loadSystemFont()) {
        auto atlas = fontEngine.buildAtlas(14.0f * window->dpiScale());
        jf::JTextHelper::setAtlas(atlas);
        hal->uploadFontAtlas(atlas.bitmap.data(), atlas.width, atlas.height);
        std::cout << "[GENESIS] Font atlas ready (DPI scale: " << window->dpiScale() << ").\n";
    } else {
        std::cout << "[GENESIS] No system font found — text will use placeholder rendering.\n";
    }

    jf::JAccessibilityBridge a11y;
    a11y.start("GenesisControlsCatalog");

    JGuiApplication app;
    jf::JFocusManager focus;
    auto catalog = std::make_unique<ControlsCatalog>(app.sceneGraph(), focus, W, H);
    catalog->initTextures(*hal);

    // Register the main dock host so FloatingDockWindows can find it by cursor pos.
    JDockRegistry::instance().registerHost(
        catalog->dockHost(), window->screenX(), window->screenY(), W, H);

    JPrimitiveBuffer buffer;
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
    uint32_t lastHintMinW = 0, lastHintMinH = 0;  // last minimum size sent to the WM

    jf::JFloatingDockOptions g_dockOptions;
    catalog->dockHost().setLivePreviewEnabled(g_dockOptions.livePreviewEnabled);
    std::vector<JFloatingDockWindow> floatingDocks;
    std::vector<jf::JNativeDialogWindow> activeDialogs;
    std::vector<std::unique_ptr<JPopupWindow>> floatingMenus;
    std::unique_ptr<JPopupWindow> activePopup;
    JComboBox* activePopupComboBox = nullptr;
    JPopupWindow* pendingClosePopup = nullptr;

    catalog->onComboBoxPopupRequested = [&](JComboBox* cb) {
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

        // Build the popup: one JPopupItem per combo option, borderless.
        auto popup = std::make_unique<JPopupWindow>(
            sx, sy, popupW, 8 /*placeholder height, computed below*/,
            *hal, JPopupWindow::JStyle::Borderless, window->nativeWindow());

        const auto& items = cb->items();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            auto* pi = popup->add<jf::JPopupItem>(
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

    std::vector<std::unique_ptr<JPopupWindow>> activeMenuPopups;
    std::vector<std::unique_ptr<JPopupWindow>> deferredMenuPopups;
    std::vector<std::function<void()>> deferredMenuActions;
    bool isPollingMenuEvents = false;

    jf::JMenuManager::instance().onOpenMenu = [&](jf::JMenu* menu, int sx, int sy, bool parentTorn) {
        auto action = [menu, sx, sy, parentTorn, &activeMenuPopups, &deferredMenuPopups, &floatingMenus, &floatingDocks, &g_dockOptions, &hal, &window, &catalog, &isPollingMenuEvents, &deferredMenuActions]() {
            if (!menu) {
                for (auto& p : activeMenuPopups) p->destroySurface(*hal);
                activeMenuPopups.clear();
                for (auto& p : deferredMenuPopups) p->destroySurface(*hal);
                deferredMenuPopups.clear();
                catalog->menuBar()->closeMenu();
                return;
            }

            if (!parentTorn) {
                for (auto& p : activeMenuPopups) p->destroySurface(*hal);
                activeMenuPopups.clear();
            }

            auto popup = std::make_unique<JPopupWindow>(
                sx, sy, 180, 8, *hal, JPopupWindow::JStyle::Bordered, window->nativeWindow(),
#if defined(_WIN32)
                GetModuleHandle(NULL)
#else
                nullptr
#endif
            );

            if (menu->isTearOffEnabled() && jf::JMenuManager::instance().isTearOffEnabled()) {
                auto* handle = popup->add<jf::JTearOffHandle>();
                handle->onTornOff.connect([menu, &activeMenuPopups, &deferredMenuPopups, &floatingMenus, &hal, &window, &isPollingMenuEvents, &deferredMenuActions, &catalog]() {
                    auto tearAction = [menu, &activeMenuPopups, &deferredMenuPopups, &floatingMenus, &hal, &window, &catalog]() {
                        // Park any submenus off-screen — we only promote the main popup.
                        for (size_t i = 1; i < activeMenuPopups.size(); ++i) {
                            activeMenuPopups[i]->releasePointerGrab();
                            activeMenuPopups[i]->window().setPosition(-10000, -10000);
                            deferredMenuPopups.push_back(std::move(activeMenuPopups[i]));
                        }

                        if (!activeMenuPopups.empty()) {
                            auto& p = activeMenuPopups.front();
                            p->releasePointerGrab();
                            p->enableCloseButton();
                            // Leave the popup at its current screen position —
                            // the user just dragged it there via the JTearOffHandle.
                            floatingMenus.push_back(std::move(p));
                        }
                        activeMenuPopups.clear();
                        catalog->menuBar()->closeMenu();  // reset m_activeIdx so hover-to-switch can't open another menu
                    };
                    if (isPollingMenuEvents) {
                        deferredMenuActions.push_back(std::move(tearAction));
                    } else {
                        tearAction();
                    }
                });
            }

            for (const auto& item : menu->items()) {
                if (!item) continue;
                if (auto* mi = dynamic_cast<jf::JMenuItem*>(item.get())) {
                    auto* added = popup->add<jf::JMenuItem>(mi->label(), mi->shortcut(), mi->submenu());
                    added->setCheckable(mi->isCheckable());
                    added->setChecked(mi->isChecked());
                    added->setTooltip(mi->tooltip());
                    if (mi->embeddedWidgetFactory()) {
                        added->setEmbeddedWidgetFactory(mi->embeddedWidgetFactory());
                    }
                    added->onTriggered.connect([mi, &activeMenuPopups, &hal, &isPollingMenuEvents, &deferredMenuActions, &catalog]() {
                        mi->onTriggered.emit();
                        if (!mi->submenu()) {
                            auto triggerAction = [&activeMenuPopups, &hal, &catalog]() {
                                for (auto& p : activeMenuPopups) p->destroySurface(*hal);
                                activeMenuPopups.clear();
                                // Must reset m_activeIdx so hover-to-switch logic doesn't
                                // reopen a menu when the cursor moves after a click.
                                catalog->menuBar()->closeMenu();
                            };
                            if (isPollingMenuEvents) {
                                deferredMenuActions.push_back(std::move(triggerAction));
                            } else {
                                triggerAction();
                            }
                        }
                    });

                    if (mi->submenu()) {
                        JPopupWindow* parentPopup = popup.get();
                        added->onHoverEntered.connect([added, parentPopup, &hal]() {
                            const auto& layout = parentPopup->graph().getLayoutConst(added->getNodeId());
                            int sx = parentPopup->window().screenX() + static_cast<int>(layout.boundingBox.x + layout.boundingBox.width);
                            int sy = parentPopup->window().screenY() + static_cast<int>(layout.boundingBox.y);
                            jf::JMenuManager::instance().onOpenMenu(added->submenu(), sx, sy, true);
                        });
                    }
                } else if (dynamic_cast<jf::JMenuSeparator*>(item.get())) {
                    popup->add<jf::JMenuSeparator>();
                }
            }

            popup->computeNaturalHeight();
            activeMenuPopups.push_back(std::move(popup));
        };

        if (isPollingMenuEvents) {
            deferredMenuActions.push_back(std::move(action));
        } else {
            action();
        }
    };

    catalog->menuBar()->onQueryScreenPos = [&](float localX, float localY) -> std::pair<int, int> {
        return { window->screenX() + static_cast<int>(localX), window->screenY() + static_cast<int>(localY) };
    };

    catalog->onFloatPanelRequested = [&](const std::string& title) {
        JDockWidget* dw = nullptr;
        catalog->dockHost().forEachDockPanel([&](const JDockWidget* d, const JRect&, bool, int) {
            if (d->title() == title) dw = const_cast<JDockWidget*>(d);
        });
        if (!dw) return;

        JDockNodeId loc = catalog->dockHost().findDock(dw);
        JRect r = catalog->dockHost().node(loc)->rect;

        int sx = window->screenX() + static_cast<int>(r.x);
        int sy = window->screenY() + static_cast<int>(r.y);

        // Restore pre-dock dimensions.
        float floatW = std::max(dw->width(),  dw->minW());
        float floatH = std::max(dw->height(), dw->minH());

        catalog->dockHost().removeDock(dw);

        JDockWidget moved = std::move(*dw);
        moved.setPosition(0.f, 0.f);
        moved.setSize(floatW, floatH);

        int offX = static_cast<int>(floatW) / 2;
        int offY = static_cast<int>(floatH) / 2;

        floatingDocks.emplace_back(
            std::move(moved),
            sx, sy,
            static_cast<uint32_t>(floatW), static_cast<uint32_t>(floatH),
            offX, offY,
            *hal, /*initialDrag=*/false, g_dockOptions, window->nativeWindow()
        );

        {
            ControlsCatalog* cat = catalog.get();
            auto& newFd = floatingDocks.back();
            JDockHost* hostPtr = &newFd.dockHost();
            newFd.setContentRenderHost([cat, hostPtr](JPrimitiveBuffer& b) {
                cat->renderHostFloatingPanels(*hostPtr, b);
            });
            newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl, float wheel) {
                cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl, wheel);
            });
        }

        catalog->removeInlineDock(dw);
    };

    std::cout << "[GENESIS] Catalog running. Tab/Shift-Tab cycles focus. Close window to exit.\n";
    std::cout << "[HOTKEYS] Tweak drag options at runtime:\n";
    std::cout << "  'd' / 'D': Cycle global title bar drag behavior (Legacy, Always, Conditional)\n";
    std::cout << "  's' / 'S': Toggle Single Dock Drag Moves JWindow (vs Tears Out)\n";
    std::cout << "  't' / 'T': Toggle Tab Drag Tears Out\n";
    std::cout << "  'x' / 'X': Toggle Split Drag Tears Out\n";
    std::cout << "  'l' / 'L': Toggle Live Drop Preview\n";
    std::cout << "  'r' / 'R': Toggle Global JMenu TearOff\n";

    // When the window is WM-maximized (full screen), the first _NET_WM_MOVERESIZE
    // causes the WM to un-maximize (size + position change) but the drag doesn't
    // start because the cursor is now outside the restored window bounds. We
    // restart MOVERESIZE with the correct offset once the restore completes.
    bool     titleMoveInitiated  = false;
    float    titlePressLocalXFrac = 0.f;
    float    titlePressLocalY     = 0.f;
    uint32_t titlePressWinW       = 0;   // window width at the time of the title-bar press
    uint32_t titlePressWinH       = 0;   // window height at the time of the title-bar press
    bool     closePendingRelease  = false;

    while (!window->shouldClose()) {
        auto now = std::chrono::steady_clock::now();
        float dt  = std::chrono::duration<float>(now - lastTime).count();
        lastTime  = now;

        window->pollNativeEvents();

        // After we hand a drag to the WM (startWindowMove), the WM may un-dock the
        // window. For a full-screen CSD un-maximize WE restored the geometry, so the
        // WM's MOVERESIZE grab still references the old full-size window — restart it
        // with the corrected offset. For native left/right tile un-tiling the WM
        // drives everything, so stay hands-off (manual dragging would fight the WM's
        // edge tiling and the window would land off the edge).
        if (titleMoveInitiated) {
            bool resized = window->consumeWasResized();
            bool held    = window->isLeftButtonDown();
            if (resized) {
                uint32_t newW = window->width();
                uint32_t newH = window->height();
                bool widthShrunk   = (newW < titlePressWinW);
                bool fromCSDUnsnap = window->consumeWasUnsnapped();
                if (held && widthShrunk && fromCSDUnsnap) {
                    titleMoveInitiated  = false;
                    closePendingRelease = false;
                    float restoredW = static_cast<float>(newW);
                    auto [cx, cy] = window->globalCursorPos();
                    titlePressLocalXFrac = (restoredW > 0.f)
                        ? static_cast<float>(cx - window->screenX()) / restoredW
                        : 0.f;
                    titlePressLocalY  = static_cast<float>(cy - window->screenY());
                    titlePressWinW    = newW;
                    titlePressWinH    = newH;
                    titleMoveInitiated = true;
                    window->consumeWasResized();
                    window->startWindowMove();
                } else {
                    titleMoveInitiated = false;
                }
            }
        }
        if (!window->isLeftButtonDown()) {
            titleMoveInitiated  = false;
            // Note: closePendingRelease is cleared in the release handler below,
            // NOT here — the release event may arrive same frame as button-up.
        }

        float mouseX_val = window->mouseX();
        float mouseY_val = window->mouseY();
        // Detect window position change this frame so we can refresh stale coords.
        static int prevWinX = 0, prevWinY = 0;
        int curWinX = window->screenX(), curWinY = window->screenY();
        bool windowMovedThisFrame = (curWinX != prevWinX || curWinY != prevWinY);
        prevWinX = curWinX; prevWinY = curWinY;
        if (!activeMenuPopups.empty() || windowMovedThisFrame) {
            auto [gx, gy] = window->globalCursorPos();
            mouseX_val = static_cast<float>(gx - window->screenX());
            mouseY_val = static_cast<float>(gy - window->screenY());
        }

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

            // Keep the WM informed so it enforces the minimum during interactive
            // resize and never delivers a ConfigureNotify below this threshold.
            if (minW != lastHintMinW || minH != lastHintMinH) {
                window->setMinSize(minW, minH);
                lastHintMinW = minW;
                lastHintMinH = minH;
            }

            if (newW > 0 && newH > 0 && (newW < minW || newH < minH)) {
                newW = std::max(newW, minW);
                newH = std::max(newH, minH);
                window->setSize(newW, newH);
                // Do NOT re-read window->width()/height() here.  setSize() is an
                // async XCB request; the ConfigureNotify reply hasn't arrived yet,
                // so re-reading picks up the WM's old (smaller) cached value.
                // That caused the swapchain to oscillate between the clamped and
                // the WM-assigned sizes every other frame, producing the flicker.
                // newW/newH already hold the correct clamped values.
            }
            if (newW > 0 && newH > 0 && (newW != curW || newH != curH)) {
                curW = newW;
                curH = newH;
                hal->resizeSwapchain(curW, curH);
                resized = true;
            }
            catalog->dockHost().computeLayout(
                {0.f, kDockY, static_cast<float>(curW), static_cast<float>(curH) - kDockY});
            catalog->layoutMenuBar(static_cast<float>(curW), kMenuY);
        }

        // Keep the registry's bounds current (WM may have moved or resized our window).
        JDockRegistry::instance().updateBounds(
            catalog->dockHost(), window->screenX(), window->screenY(), curW, curH);

        // ---- Place dock-panel content at its area (handles scroll wheel) ----
        float wheel = window->consumeWheel();
        catalog->clearPanelVisibility();
        catalog->updateHostDockContent(catalog->dockHost(), wheel, window->mouseX(), window->mouseY());
        catalog->layoutExtra();

        // Block widget input only when a modal dialog is active.
        // Modal = any active native dialog window is modal
        bool dialogActive = std::any_of(activeDialogs.begin(), activeDialogs.end(),
                                        [](const jf::JNativeDialogWindow& d){ return d.isModal(); });

        // ---- Keyboard ----
        bool wantScreenshot = false;
        bool keyActivity    = false;
        auto frameKeys = window->consumeAllKeys();   // consume once, share with dialog
        for (auto& ke : frameKeys) {
            if (!ke.pressed) continue;
            keyActivity = true;
            // When a dialog is active it owns keyboard input; skip widget/accelerator dispatch.
            if (dialogActive) continue;
            if (jf::JMenuManager::instance().processAccelerator(ke)) {
                continue;
            }
            using K = jf::JKeyEvent::JKey;
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
                using B = jf::JFloatingDragBehavior;
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
                std::cout << "[CONFIG] Single Dock Drag Moves JWindow: " << (g_dockOptions.singleDockDragMovesWindow ? "ENABLED" : "DISABLED (Tears out instead)") << "\n";
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
            if (ke.utf8[0] == 'r' || ke.utf8[0] == 'R') {
                bool on = !jf::JMenuManager::instance().isTearOffEnabled();
                jf::JMenuManager::instance().setTearOffEnabled(on);
                std::cout << "[CONFIG] Global TearOff: " << (on ? "ENABLED" : "DISABLED") << "\n";
            }
        }

        bool pressed      = window->consumePress();
        bool released     = window->consumeRelease();
        bool rightPressed = window->consumeRightPress();

        if (released && !deferredMenuPopups.empty()) {
            for (auto& p : deferredMenuPopups) p->destroySurface(*hal);
            deferredMenuPopups.clear();
        }

        if (rightPressed) {
            // Check for a widget-specific context menu first, fall back to the app view menu.
            jf::JMenu* ctxMenu = nullptr;
            for (auto* w : jf::JWidget::s_activeWidgets) {
                if (w->contextMenu() && w->isVisible() && w->hitTest(mouseX_val, mouseY_val)) {
                    ctxMenu = w->contextMenu();
                    break;
                }
            }
            if (!ctxMenu) ctxMenu = catalog->viewMenu();
            if (ctxMenu && jf::JMenuManager::instance().onOpenMenu) {
                int sx = window->screenX() + static_cast<int>(mouseX_val);
                int sy = window->screenY() + static_cast<int>(mouseY_val);
                jf::JMenuManager::instance().onOpenMenu(ctxMenu, sx, sy, false);
            }
        }

        if (!activeMenuPopups.empty() && (pressed || rightPressed)) {
            bool clickOutsideMenuBar = (mouseX_val < 0.f || mouseX_val >= curW ||
                                        mouseY_val < 0.f || mouseY_val >= 32.f);
            if (clickOutsideMenuBar) {
                for (auto& p : activeMenuPopups) p->destroySurface(*hal);
                activeMenuPopups.clear();
                for (auto& p : deferredMenuPopups) p->destroySurface(*hal);
                deferredMenuPopups.clear();
                catalog->menuBar()->closeMenu();
            }
        }

        if (activePopup && activePopupComboBox && pressed) {
            NodeId cbNode = activePopupComboBox->getNodeId();
            const auto& bb = app.sceneGraph().getLayoutConst(cbNode).boundingBox;
            float mx = mouseX_val;
            float my = mouseY_val;
            if (mx < bb.x || mx > bb.x + bb.width ||
                my < bb.y || my > bb.y + bb.height) {
                activePopup->destroySurface(*hal);
                activePopup.reset();
                activePopupComboBox = nullptr;
            }
        }

        window->consumeMouseLeave();  // drains the flag; m_mouseX/Y already reset in platform on leave

        // ---- Custom title bar: buttons and drag (intercept before catalog) ----
        // ---- Custom title bar — WM-delegated drag/resize ----
        // All moves and resizes hand off to _NET_WM_MOVERESIZE so the WM owns the
        // operation: snap preview, multi-monitor crossing, and restore state all work
        // correctly without us duplicating WM logic.
        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastTitleClick{};
        uint32_t wmResizeDir = UINT32_MAX;   // edge/corner resize direction this frame
        {
            float W      = static_cast<float>(curW);
            float H      = static_cast<float>(curH);
            float closeX = W - kBtnW;
            float maxX   = W - kBtnW * 2.f;
            float minX   = W - kBtnW * 3.f;
            // If the window moved this frame (position changed since last poll),
            // the XCB event_x/y coordinates are stale — query the live cursor.
            // Otherwise use the fast cached values to avoid a synchronous round-trip.
            float tbMx = mouseX_val;
            float tbMy = mouseY_val;
            if (windowMovedThisFrame) {
                auto [tbGx, tbGy] = window->globalCursorPos();
                tbMx = static_cast<float>(tbGx - window->screenX());
                tbMy = static_cast<float>(tbGy - window->screenY());
            }
            bool inBar   = tbMy >= 0.f && tbMy < kTitleH;
            bool inClose = inBar && tbMx >= closeX;
            bool inMax   = inBar && tbMx >= maxX && tbMx < closeX;
            bool inMin   = inBar && tbMx >= minX && tbMx < maxX;
            bool inDrag  = inBar && tbMx < minX;

            // Handle pending close-button release (fires on release, not press).
            if (released) {
                if (closePendingRelease && inClose) {
                    window->requestClose();
                }
                closePendingRelease = false;
            }

            if (pressed) {
                if (inClose) {
                    pressed = false;
                    closePendingRelease = true;
                    // When maximized, also start the move so the user can drag out by
                    // pressing-and-dragging from the close button area (common gesture).
                    if (window->isMaximized()) {
                        titleMoveInitiated   = true;
                        titlePressLocalXFrac = (W > 0.f) ? mouseX_val / W : 0.f;
                        titlePressLocalY     = mouseY_val;
                        titlePressWinW       = window->width();
                        titlePressWinH       = window->height();
                        window->consumeWasResized();
                        window->startWindowMove();
                    }
                } else if (inMax) {
                    pressed = false;
                    window->setMaximized(!window->isMaximized());
                } else if (inMin) {
                    pressed = false;
                    window->minimize();
                } else if (inDrag) {
                    pressed = false;
                    auto clickNow = Clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  clickNow - lastTitleClick).count();
                    lastTitleClick = clickNow;
                    if (ms < 300) {
                        titleMoveInitiated = false;
                        window->setMaximized(!window->isMaximized());
                    } else {
                        titleMoveInitiated   = true;
                        titlePressLocalXFrac = (W > 0.f) ? mouseX_val / W : 0.f;
                        titlePressLocalY     = mouseY_val;
                        titlePressWinW       = window->width();
                        titlePressWinH       = window->height();
                        window->consumeWasResized();  // drain any stale startup/prior resize
                        window->startWindowMove();
                    }
                } else if (!window->isMaximized()) {
                    // ---- Edge / corner resize zones ----
                    // Top edges conflict with title bar so we skip them; left/right
                    // start below the title bar, bottom edges cover the full width.
                    constexpr float kEdge = 6.f;
                    constexpr float kCorn = 14.f;
                    bool onLeft   = mouseX_val < kEdge   && mouseY_val >= kTitleH;
                    bool onRight  = mouseX_val >= W-kEdge && mouseY_val >= kTitleH;
                    bool onBottom = mouseY_val >= H-kEdge;
                    bool onBL     = mouseX_val < kCorn   && mouseY_val >= H-kCorn;
                    bool onBR     = mouseX_val >= W-kCorn && mouseY_val >= H-kCorn;

                    if      (onBL)     wmResizeDir = 6;
                    else if (onBR)     wmResizeDir = 4;
                    else if (onBottom) wmResizeDir = 5;
                    else if (onLeft)   wmResizeDir = 7;
                    else if (onRight)  wmResizeDir = 3;

                    if (wmResizeDir != UINT32_MAX) {
                        pressed = false;
                        window->startWindowResize(wmResizeDir);
                    }
                }
            } else if (!window->isMaximized()) {
                // Hover: compute resize direction for cursor shape
                constexpr float kEdge = 6.f;
                constexpr float kCorn = 14.f;
                bool onLeft   = mouseX_val < kEdge   && mouseY_val >= kTitleH;
                bool onRight  = mouseX_val >= W-kEdge && mouseY_val >= kTitleH;
                bool onBottom = mouseY_val >= H-kEdge;
                bool onBL     = mouseX_val < kCorn   && mouseY_val >= H-kCorn;
                bool onBR     = mouseX_val >= W-kCorn && mouseY_val >= H-kCorn;

                if      (onBL)     wmResizeDir = 6;
                else if (onBR)     wmResizeDir = 4;
                else if (onBottom) wmResizeDir = 5;
                else if (onLeft)   wmResizeDir = 7;
                else if (onRight)  wmResizeDir = 3;
            }
        }

        if (!dialogActive) {
            if (!activeMenuPopups.empty()) {
                catalog->menuBar()->handleMouseMove(mouseX_val, mouseY_val);
            } else {
                catalog->handleMouse(mouseX_val, mouseY_val, pressed, released, wheel);
            }
        }
        catalog->update(dt);

        // ---- JDockHost mouse routing (inline docks) ----
        JPlatformCursor pc = JPlatformCursor::Default;
        if (wmResizeDir != UINT32_MAX) {
            // Map _NET_WM_MOVERESIZE direction → cursor shape
            switch (wmResizeDir) {
                case 0: case 4: pc = JPlatformCursor::ResizeTopLeft;    break; // TL / BR
                case 2: case 6: pc = JPlatformCursor::ResizeTopRight;   break; // TR / BL
                case 1: case 5: pc = JPlatformCursor::ResizeUpDown;     break; // T  / B
                case 3: case 7: pc = JPlatformCursor::ResizeLeftRight;  break; // R  / L
                default: break;
            }
        } else {
            auto hc = catalog->dockHost().getHoverCursor(mouseX_val, mouseY_val);
            if (hc == JDockHost::JHoverCursor::Horiz)     pc = JPlatformCursor::ResizeLeftRight;
            else if (hc == JDockHost::JHoverCursor::Vert) pc = JPlatformCursor::ResizeUpDown;
        }
        window->setCursor(pc);

        if (auto ev = catalog->dockHost().handleMouse(
                mouseX_val, mouseY_val, pressed, released))
        {
            if (ev->type == JDockHost::JDockEvent::JType::WantsFloat) {
                JDockWidget* dw  = ev->dock;
                JDockNodeId  loc = catalog->dockHost().findDock(dw);
                JRect        r   = catalog->dockHost().node(loc)->rect;

                // Restore the dock's own pre-dock dimensions rather than the
                // current split-tree rect, so size is preserved across dock/undock.
                float floatW = std::max(dw->width(),  dw->minW());
                float floatH = std::max(dw->height(), dw->minH());

                // Screen position of the dock's top-left corner (from its current rect).
                int sx = window->screenX() + static_cast<int>(r.x);
                int sy = window->screenY() + static_cast<int>(r.y);

                // Cursor offset within the restored floating window.
                auto [gx, gy] = window->globalCursorPos();
                int offX = std::clamp(gx - sx, 0, static_cast<int>(floatW) - 1);
                int offY = std::clamp(gy - sy, 0, static_cast<int>(floatH) - 1);

                catalog->dockHost().removeDock(dw);

                JDockWidget moved = std::move(*dw);
                moved.setPosition(0.f, 0.f);
                moved.setSize(floatW, floatH);  // keep JDockWidget in sync with window

                floatingDocks.emplace_back(
                    std::move(moved),
                    sx, sy,
                    static_cast<uint32_t>(floatW), static_cast<uint32_t>(floatH),
                    offX, offY,
                    *hal, /*initialDrag=*/true, g_dockOptions, window->nativeWindow());

                // The floated panel carries its catalog content: render & drive it in the
                // floating window so it stays fully functional while detached.
                {
                    ControlsCatalog* cat = catalog.get();
                    auto& newFd = floatingDocks.back();
                    JDockHost* hostPtr = &newFd.dockHost();
                    newFd.setContentRenderHost([cat, hostPtr](JPrimitiveBuffer& b) {
                        cat->renderHostFloatingPanels(*hostPtr, b);
                    });
                    newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl, float wheel) {
                        cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl, wheel);
                    });
                }

                catalog->removeInlineDock(dw);
            }
            if (ev->type == JDockHost::JDockEvent::JType::CloseRequested) {
                catalog->dockHost().removeDock(ev->dock);
                catalog->removeInlineDock(ev->dock);
            }
        }

        // ---- Floating dock update: drag / re-dock / close / render ----
        for (auto it = floatingDocks.begin(); it != floatingDocks.end(); ) {
            auto& fd = *it;
            auto pollRes = fd.pollAndMove();
            catalog->updateHostDockContent(fd.dockHost(), fd.lastWheel(), fd.window().mouseX(), fd.window().mouseY());

            if (pollRes.type == JFloatingDockWindow::JPollResult::JType::CommitDrop) {
                JDockHost* dropHost = pollRes.dropHost;
                if (auto result = dropHost->tryCommitDrop()) {
                    (void)result;
                    fd.destroySurface(*hal);
                    JDockWidget* oldPtr = &fd.dock();
                    if (dropHost == &catalog->dockHost()) {
                        catalog->adoptInlineDock(
                            std::make_unique<JDockWidget>(fd.takeDock()), oldPtr);
                    } else {
                        JFloatingDockWindow* targetWin = nullptr;
                        for (auto& otherFd : floatingDocks) {
                            if (&otherFd.dockHost() == dropHost) {
                                targetWin = &otherFd;
                                break;
                            }
                        }
                        if (targetWin) {
                            targetWin->adoptDock(
                                std::make_unique<JDockWidget>(fd.takeDock()), oldPtr);
                        }
                    }
                    it = floatingDocks.erase(it);
                    continue;
                }
            } else if (pollRes.type == JFloatingDockWindow::JPollResult::JType::WantsFloat) {
                JDockWidget* dw = pollRes.wantsFloatDock;
                JRect r = pollRes.wantsFloatRect;

                int sx = fd.window().screenX() + static_cast<int>(r.x);
                int sy = fd.window().screenY() + static_cast<int>(r.y);

                auto [gx, gy] = fd.window().globalCursorPos();
                int offX = gx - sx;
                int offY = gy - sy;

                std::unique_ptr<JDockWidget> movedPtr = fd.releaseDock(dw);
                if (movedPtr) {
                    JDockWidget moved = std::move(*movedPtr);
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
                        JDockHost* hostPtr = &newFd.dockHost();
                        newFd.setContentRenderHost([cat, hostPtr](JPrimitiveBuffer& b) {
                            cat->renderHostFloatingPanels(*hostPtr, b);
                        });
                        newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl, float wheel) {
                            cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl, wheel);
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
            if (res.type == JPopupWindow::JPollResult::JType::Dismissed) {
                activePopup->destroySurface(*hal);
                activePopup.reset();
                activePopupComboBox = nullptr;
            } else if (activePopup->isViewable()) {
                activePopup->render(*hal, buffer);
            }
        }

        // ---- Active menu popups update ----
        bool dismissed = false;
        isPollingMenuEvents = true;
        for (auto it = activeMenuPopups.begin(); it != activeMenuPopups.end(); ) {
            auto& popup = *it;
            auto res = popup->pollEvents(*hal);
            if (res.type == JPopupWindow::JPollResult::JType::Dismissed) {
                dismissed = true;
                break;
            }
            ++it;
        }
        isPollingMenuEvents = false;

        if (!deferredMenuActions.empty()) {
            for (const auto& action : deferredMenuActions) {
                action();
            }
            deferredMenuActions.clear();
        }

        if (dismissed) {
            for (auto& p : activeMenuPopups) p->destroySurface(*hal);
            activeMenuPopups.clear();
            catalog->menuBar()->closeMenu();
        } else {
            for (auto& popup : activeMenuPopups) {
                if (popup->isViewable()) {
                    popup->render(*hal, buffer);
                }
            }
        }



        // ---- Floating menus (torn-off popup, still looks like a menu) ----
        for (auto it = floatingMenus.begin(); it != floatingMenus.end(); ) {
            auto& fm = *it;
            auto res = fm->pollFloating();
            if (res == JPopupWindow::JFloatPollResult::Close) {
                fm->destroySurface(*hal);
                it = floatingMenus.erase(it);
            } else {
                fm->render(*hal, buffer);
                ++it;
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

        // ---- Torn tab → new JFloatingDockWindow ----
        if (auto newFloat = catalog->consumeNewFloat()) {
            auto& [state, pos] = *newFloat;
            auto [gx, gy] = window->globalCursorPos();
            constexpr int kTabH = static_cast<int>(JDockHost::TAB_BAR_SZ) / 2;
            int winX   = gx - static_cast<int>(JFloatingDockWindow::kDefaultW) / 2;
            int winY   = gy - kTabH;
            int dragOX = static_cast<int>(JFloatingDockWindow::kDefaultW) / 2;
            int dragOY = kTabH;
            floatingDocks.emplace_back(std::move(state), winX, winY, dragOX, dragOY, *hal, g_dockOptions, window->nativeWindow());

            {
                ControlsCatalog* cat = catalog.get();
                auto& newFd = floatingDocks.back();
                JDockHost* hostPtr = &newFd.dockHost();
                newFd.setContentRenderHost([cat, hostPtr](JPrimitiveBuffer& b) {
                    cat->renderHostFloatingPanels(*hostPtr, b);
                });
                newFd.setContentInputHost([cat, hostPtr](float x, float y, bool pr, bool rl, float wheel) {
                    cat->handleHostFloatingPanelsInput(*hostPtr, x, y, pr, rl, wheel);
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
        bool mouseMoved = (mouseX_val != lastMouseX || mouseY_val != lastMouseY);
        lastMouseX = mouseX_val; lastMouseY = mouseY_val;

        bool activity = keyActivity || pressed || released || mouseMoved || resized
                        || wantScreenshot || aiActed || wheel != 0.0f || !floatingDocks.empty()
                        || activePopup || !activeMenuPopups.empty() || !floatingMenus.empty()
                        || catalog->isAnimating();
        if (activity) redrawFrames = 4;        // render now + a short settle tail

        bool doRender = (redrawFrames > 0);
        if (doRender) --redrawFrames;

        // ---- Spawn JNativeDialogWindow for each pending dialog request ----
        while (jf::JDialogManager::instance().hasPending()) {
            const auto* req = jf::JDialogManager::instance().front();
            const auto& opts = req->options;

            int dlgW = static_cast<int>(jf::JNativeDialogWindow::kW);
            int dlgH = static_cast<int>(jf::JNativeDialogWindow::calcHeight(req->kind, opts));

            int dlgX, dlgY;
            using Pos = jf::JDialogOptions::JPosition;
            switch (opts.position) {
            case Pos::Fixed:
                dlgX = opts.x; dlgY = opts.y;
                break;
            case Pos::CenterOnScreen: {
                auto [sw, sh] = window->virtualDesktopSize();
                dlgX = (sw - dlgW) / 2; dlgY = (sh - dlgH) / 2;
                break;
            }
            case Pos::AtCursor: {
                auto [gx, gy] = window->globalCursorPos();
                dlgX = gx; dlgY = gy;
                break;
            }
            case Pos::TopLeft:
                dlgX = window->screenX() + 16;
                dlgY = window->screenY() + 16;
                break;
            case Pos::TopRight:
                dlgX = window->screenX() + static_cast<int>(curW) - dlgW - 16;
                dlgY = window->screenY() + 16;
                break;
            case Pos::TopCenter:
                dlgX = window->screenX() + (static_cast<int>(curW) - dlgW) / 2;
                dlgY = window->screenY() + 16;
                break;
            case Pos::BottomLeft:
                dlgX = window->screenX() + 16;
                dlgY = window->screenY() + static_cast<int>(curH) - dlgH - 16;
                break;
            case Pos::BottomRight:
                dlgX = window->screenX() + static_cast<int>(curW) - dlgW - 16;
                dlgY = window->screenY() + static_cast<int>(curH) - dlgH - 16;
                break;
            case Pos::BottomCenter:
                dlgX = window->screenX() + (static_cast<int>(curW) - dlgW) / 2;
                dlgY = window->screenY() + static_cast<int>(curH) - dlgH - 16;
                break;
            case Pos::CenterOnParent:
            default:
                dlgX = window->screenX() + (static_cast<int>(curW) - dlgW) / 2;
                dlgY = window->screenY() + (static_cast<int>(curH) - dlgH) / 2;
                break;
            }

            activeDialogs.emplace_back(*req, *hal, dlgX, dlgY,
                static_cast<jf::JNativeDialogWindow::NativeWinHandleType>(
                    window->rawWindowId()));
            jf::JDialogManager::instance().pop();
        }

        // ---- Poll and render all native dialog windows ----
        for (auto it = activeDialogs.begin(); it != activeDialogs.end(); ) {
            if (!it->pollAndRender(*hal, buffer)) {
                it->destroySurface(*hal);
                it = activeDialogs.erase(it);
            } else {
                ++it;
            }
        }

        // ---- Render main window (event-driven: only when something changed) ----
        if (doRender) {
            auto frame = hal->beginFrame();
            buffer.clear();
            catalog->render(buffer);
            JWidget::renderTooltips(buffer, mouseX_val, mouseY_val);

            // ---- Custom title bar (drawn last so it sits on top of everything) ----
            {
                float W = static_cast<float>(curW);
                float lh = jf::JTextHelper::lineHeight();

                // Background strip
                uint8_t tbg[4] = {22, 22, 28, 255};
                buffer.pushRectangle(0.f, 0.f, W, kTitleH, tbg, 0.f);

                // App title
                uint8_t tc[4]; std::copy(jf::Colors::TextSecondary,
                                         jf::Colors::TextSecondary + 4, tc);
                jf::JTextHelper::pushText(buffer, 10.f, (kTitleH - lh) * 0.5f,
                                              winTitle, tc, W - kBtnW * 3.f - 20.f);

                // JSeparator line between title bar and menu bar
                uint8_t sep[4] = {jf::Colors::Border[0], jf::Colors::Border[1],
                                  jf::Colors::Border[2], jf::Colors::Border[3]};
                buffer.pushRectangle(0.f, kTitleH - 1.f, W, 1.f, sep, 0.f);

                // JWindow control buttons: [−] [□/⊡] [×]
                float closeX = W - kBtnW;
                float maxX   = W - kBtnW * 2.f;
                float minX   = W - kBtnW * 3.f;

                auto hovBtn = [&](float bx) {
                    return mouseY_val >= 0.f && mouseY_val < kTitleH &&
                           mouseX_val >= bx  && mouseX_val < bx + kBtnW;
                };

                // Minimize −
                {
                    bool h = hovBtn(minX);
                    if (h) { uint8_t hbg[4]={60,60,70,200}; buffer.pushRectangle(minX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
                    float cx = minX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
                    uint8_t ic[4] = {190,190,200,220};
                    buffer.pushRectangle(cx, cy, 9.f, 2.f, ic, 0.f);
                }

                // Maximize □ / restore ⊡
                {
                    bool h = hovBtn(maxX);
                    if (h) { uint8_t hbg[4]={60,60,70,200}; buffer.pushRectangle(maxX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
                    float cx = maxX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 4.f;
                    uint8_t ic[4]   = {190,190,200,220};
                    uint8_t nf[4]   = {0,0,0,0};
                    uint8_t wbg[4]  = {22,22,28,255};
                    if (!window->isMaximized()) {
                        buffer.pushRectangle(cx, cy, 9.f, 9.f, nf, 1.f, 1.5f, ic);
                    } else {
                        // Two overlapping rects = restore icon
                        buffer.pushRectangle(cx + 2.f, cy,      7.f, 7.f, nf,  0.f, 1.f, ic);
                        buffer.pushRectangle(cx,        cy + 2.f, 7.f, 7.f, wbg, 0.f, 1.f, ic);
                    }
                }

                // Close ×
                {
                    bool h = hovBtn(closeX);
                    if (h) { uint8_t hbg[4]={180,40,40,220}; buffer.pushRectangle(closeX, 0.f, kBtnW, kTitleH, hbg, 0.f); }
                    float cx = closeX + kBtnW * 0.5f - 4.f, cy = kTitleH * 0.5f - 1.f;
                    uint8_t ic[4] = {210,210,220,230};
                    buffer.pushRectangle(cx, cy, 9.f, 2.f, ic, 1.f);
                    buffer.pushRectangle(cx + 3.5f, cy - 3.5f, 2.f, 9.f, ic, 1.f);
                }
            }

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
            
            // Also append active popup menus so they are visible to the AI agent
            for (const auto& popup : activeMenuPopups) {
                for (const auto& w : popup->widgets()) {
                    AiNodeDescriptor d{};
                    NodeId nid = w->getNodeId();
                    d.id = nid;
                    // Global screen coordinates of popup widgets
                    const auto& bb = popup->graph().getLayoutConst(nid).boundingBox;
                    d.x = static_cast<float>(popup->window().screenX()) + bb.x;
                    d.y = static_cast<float>(popup->window().screenY()) + bb.y;
                    d.width = bb.width;
                    d.height = bb.height;

                    JAISemanticNode sn = w->getSemanticNode();
                    aiSetField(d.role,  sizeof(d.role),  sn.role);
                    aiSetField(d.name,  sizeof(d.name),  sn.label);
                    aiSetField(d.value, sizeof(d.value), sn.value);

                    uint32_t f = 0;
                    if (w->isEnabled())  f |= AiEnabled;
                    if (w->isVisible())  f |= AiVisible;
                    if (sn.interactable) f |= AiInteractable;
                    if (w->getState() == JWidgetState::Pressed) f |= AiPressed;
                    if (w->getState() == JWidgetState::Focused) f |= AiFocused;
                    d.stateFlags = f;
                    aiNodes.push_back(d);
                }
            }

            app.aiBus().publishNodes(aiNodes.data(), static_cast<uint32_t>(aiNodes.size()));
            a11y.update(aiNodes);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    JDockRegistry::instance().unregisterHost(catalog->dockHost());

    // Destroy floating surfaces before HAL teardown.
    for (auto& fd : floatingDocks)
        fd.destroySurface(*hal);
    floatingDocks.clear();

    for (auto& p : activeMenuPopups)
        p->destroySurface(*hal);
    activeMenuPopups.clear();



    if (activePopup) {
        activePopup->destroySurface(*hal);
        activePopup.reset();
    }

    hal->waitIdle();
    a11y.stop();
    return 0;
}
