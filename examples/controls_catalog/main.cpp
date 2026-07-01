#include <j/core/ApplicationCore.h>
#include <j/core/GenesisComponents.h>
#include <j/core/BaseWidgets.h>
#include <j/core/DockWidget.h>
#include <j/core/DockManager.h>
#include <j/core/DockRegistry.h>
#include <j/app/JAppWindow.h>
#include <j/core/TranslationEngine.h>
#include <j/core/FocusManager.h>
#include <j/core/AccessibilityBridge.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/FontEngine.h>
#if defined(_WIN32)
#include <j/platforms/windows/WindowsPlatformWindow.h>
#else
#include <j/platforms/linux/LinuxPlatformWindow.h>
#endif
#include <j/platforms/linux/FloatingDockWindow.h>
#include <j/platforms/NativeDialogWindow.h>
#include <j/platforms/PopupWindow.h>
#include <j/core/MenuSystem.h>
#include <j/core/Dialog.h>
#include <j/core/Animator.h>
#include <j/core/Splitter.h>
#include <j/core/ImageWidget.h>
#include <j/platform/Clipboard.h>
#include <j/platform/FileDialog.h>

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










    // Render a panel's content into a dock content rect (host-local coords). Used as the
    // JDockWidget::onRenderContent hook, so the framework calls this wherever the dock lives
    // — inline in the dock space or torn into a floating window. Also refreshes the panel's
    // viewport / measured height so onInputContent can route wheel + clicks correctly.
    void renderFloatingPanel(const std::string& title, JPrimitiveBuffer& buf, const JRect& content) {
        Panel* p = panelByTitle(title);
        if (!p) return;
        // A dock can collapse to ~0 size mid-drag/resize; laying content out into it yields
        // degenerate widget rects. Skip — nothing meaningful renders at that size anyway.
        if (content.width < 2.0f || content.height < 2.0f) { p->visible = false; return; }
        auto& L = m_graph.getLayout(p->root);
        m_graph.invalidateNode(p->root, DirtySelf);
        L.boundingBox.x = content.x;
        L.boundingBox.y = content.y - p->scrollY;
        m_graph.computeLayout(p->root, JConstraints{content.width, content.width, 0.0f, 100000.0f});
        p->viewport = content;
        p->visible  = true;
        p->contentH = m_graph.getLayoutConst(p->root).boundingBox.height;
        p->scrollY  = std::clamp(p->scrollY, 0.0f, std::max(0.0f, p->contentH - content.height));
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

        // The menu BAR is the runner's (win.menuBar()); the app only builds the JMenu objects
        // here and attaches them via installMenus() once the window exists. Shortcuts are
        // registered globally below — the runner's key routing calls processAccelerator.

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

    // Create one dock per panel, wire its content hooks (which travel WITH the dock through
    // tear-out / re-dock — the hooks capture only `this` + the panel title, both stable),
    // and place each in a dock-space area. The runner lays out, renders, routes input, and
    // owns tear-out; the app only declares which panel goes where.
    // Attach the app's menus to the runner's menu bar. The runner renders the bar strip,
    // opens/services the popups, and routes clicks — the app just supplies the JMenus.
    void installMenus(jf::JMenuBar& bar) {
        for (const auto& m : m_menus) bar.addMenu(m.get());
    }

    void installDocks(JDockSpace& space) {
        // Reserve the outer areas — an area shows only while it has a reserved size AND holds
        // docks, so emptying one (tear-out) collapses it and the centre reclaims the space.
        space.setLeftWidth(240.f);
        space.setRightWidth(260.f);
        space.setBottomHeight(220.f);

        auto place = [this](JDockHost& host, const char* t) {
            std::string title = t;
            if (!panelByTitle(title)) return;
            auto d = std::make_unique<JDockWidget>(title, 0.f, 0.f, 260.f, 200.f);
            d->setMinSize(120.f, 80.f);
            d->onRenderContent = [this, title](JPrimitiveBuffer& b, const JRect& r) {
                renderFloatingPanel(title, b, r);
            };
            d->onInputContent = [this, title](float x, float y, bool p, bool rl, float w) {
                inputPanelContent(title, x, y, p, rl, w);
            };
            host.addDock(d.get());
            m_inlineDocks.push_back(std::move(d));
        };
        place(space.left(),   "Navigator");
        place(space.left(),   "Assets");
        place(space.right(),  "Properties");
        place(space.right(),  "Inspector");
        place(space.bottom(), "Console");
        place(space.bottom(), "Output");
        place(space.center(), "New Features");
    }

    // Route content input (wheel + widget hover/click) to a panel. Used as the
    // JDockWidget::onInputContent hook — the runner calls it for the dock under the cursor,
    // inline or floating. Focus-on-click is handled by the runner; keyboard by the focused
    // widget. Coordinates are host-local (they match the rect passed to onRenderContent).
    void inputPanelContent(const std::string& title, float x, float y,
                           bool pressed, bool released, float wheel) {
        Panel* p = panelByTitle(title);
        if (!p || !p->visible) return;
        const JRect content = p->viewport;
        if (wheel != 0.0f && x >= content.x && x < content.x + content.width
                          && y >= content.y && y < content.y + content.height) {
            bool consumed = false;
            for (JWidget* w : p->widgets) if (w->handleScroll(x, y, wheel)) consumed = true;
            if (!consumed) {
                p->scrollY -= wheel * 40.0f;
                p->scrollY = std::clamp(p->scrollY, 0.0f, std::max(0.0f, p->contentH - content.height));
            }
        }
        for (JWidget* w : p->widgets) {
            w->handleMouseMove(x, y);
            if (pressed)  w->handleMousePress(x, y);
            if (released) w->handleMouseRelease(x, y);
        }
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

    std::vector<std::unique_ptr<JDockWidget>> m_inlineDocks;
    float m_elapsed{0.0f};
    bool  m_animPaused{false};
    bool  m_showPanelScrollbars{true};

    Panel* m_scrollDraggingPanel{nullptr};
    float  m_scrollDragStartY{0.0f};
    float  m_scrollDragStartScrollY{0.0f};

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

    jf::JTranslationEngine::instance().setSearchPath("./translations");
    jf::JTranslationEngine::instance().setLocale("en");

    std::string winTitle = "Genesis Controls Catalog";
    if (const char* t = getenv("GENESIS_WIN_TITLE")) winTitle = t;

    jf::JAccessibilityBridge a11y;
    a11y.start("GenesisControlsCatalog");

    JGuiApplication app;
    JAppWindow win(winTitle, 760, 860);
    if (!win.valid()) { std::cerr << "[GENESIS] window/HAL init failed\n"; return -1; }

    // The catalog builds its panels + widgets; the runner owns the window, chrome, dock
    // space, tear-out/re-dock, keyboard focus and the loop. Each panel is installed as a
    // dock whose content hooks render/route it wherever it lives (inline or floating).
    ControlsCatalog catalog(app.sceneGraph(), win.focus(), 760, 860);
    catalog.initTextures(win.hal());
    catalog.installMenus(win.menuBar());     // File / Edit / View / Help on the runner's bar
    catalog.installDocks(win.dockSpace());

    // ---- Overlay OS-windows the app owns: combo dropdowns + native dialogs -------------
    // These are separate top-level windows with their own surfaces (their own beginFrame),
    // so they're serviced in onInput — which the runner calls every loop iteration, BEFORE
    // it opens the main window's frame — never nested inside it. (Menu-bar popups are the
    // runner's own; only these app-created overlays are serviced here.)
    std::unique_ptr<JPopupWindow>       comboPopup;
    JComboBox*                          comboOwner    = nullptr;
    JPopupWindow*                       comboCloseReq = nullptr;
    std::vector<jf::JNativeDialogWindow> dialogs;

    catalog.onComboBoxPopupRequested = [&](JComboBox* cb) {
        auto& hal = win.hal();
        auto& pw  = win.window();
        // Toggle: clicking the same combo while open closes it; a different combo replaces it.
        if (comboPopup && comboOwner == cb) { comboPopup->destroySurface(hal); comboPopup.reset(); comboOwner = nullptr; return; }
        if (comboPopup) { comboPopup->destroySurface(hal); comboPopup.reset(); }

        const auto& bb = app.sceneGraph().getLayoutConst(cb->getNodeId()).boundingBox;
        int sx = pw.screenX() + static_cast<int>(bb.x);
        int sy = pw.screenY() + static_cast<int>(bb.y + bb.height);
        auto popupW = static_cast<uint32_t>(bb.width);
        auto popup = std::make_unique<JPopupWindow>(sx, sy, popupW, 8, hal,
                        JPopupWindow::JStyle::Borderless,
                        static_cast<JPopupWindow::NativeWinHandleType>(pw.rawWindowId()));
        const auto& items = cb->items();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            auto* pi = popup->add<jf::JPopupItem>(items[i], static_cast<float>(popupW), 28.f);
            pi->onActivated.connect([cb, i, &comboPopup, &comboCloseReq, &comboOwner]() {
                cb->setCurrentIndex(i);
                comboCloseReq = comboPopup.get();   // defer close until after pollEvents returns
                comboOwner = nullptr;
            });
        }
        popup->computeNaturalHeight();
        comboPopup = std::move(popup);
        comboOwner = cb;
    };

    auto serviceOverlays = [&]() {
        auto& hal = win.hal();
        auto& pw  = win.window();
        static JPrimitiveBuffer scratch;   // popups render into their OWN surfaces; this is scratch

        if (comboPopup) {
            auto res = comboPopup->pollEvents(hal);
            const bool close = res.type == JPopupWindow::JPollResult::JType::Dismissed
                               || comboCloseReq == comboPopup.get();
            comboCloseReq = nullptr;
            if (close) { comboPopup->destroySurface(hal); comboPopup.reset(); comboOwner = nullptr; }
            else { if (comboPopup->isViewable()) comboPopup->render(hal, scratch); win.requestRedraw(); }
        }

        // Drain queued dialog requests into native dialog windows.
        while (jf::JDialogManager::instance().hasPending()) {
            const auto* req  = jf::JDialogManager::instance().front();
            const auto& opts = req->options;
            const int dlgW = static_cast<int>(jf::JNativeDialogWindow::kW);
            const int dlgH = static_cast<int>(jf::JNativeDialogWindow::calcHeight(req->kind, opts));
            const int cW = static_cast<int>(win.width()), cH = static_cast<int>(win.height());
            int dlgX, dlgY;
            using Pos = jf::JDialogOptions::JPosition;
            switch (opts.position) {
            case Pos::Fixed:          dlgX = opts.x; dlgY = opts.y; break;
            case Pos::CenterOnScreen: { auto [sw, sh] = pw.virtualDesktopSize(); dlgX = (sw - dlgW) / 2; dlgY = (sh - dlgH) / 2; break; }
            case Pos::AtCursor:       { auto [gx, gy] = pw.globalCursorPos();     dlgX = gx; dlgY = gy; break; }
            case Pos::TopLeft:        dlgX = pw.screenX() + 16;                   dlgY = pw.screenY() + 16; break;
            case Pos::TopRight:       dlgX = pw.screenX() + cW - dlgW - 16;       dlgY = pw.screenY() + 16; break;
            case Pos::TopCenter:      dlgX = pw.screenX() + (cW - dlgW) / 2;      dlgY = pw.screenY() + 16; break;
            case Pos::BottomLeft:     dlgX = pw.screenX() + 16;                   dlgY = pw.screenY() + cH - dlgH - 16; break;
            case Pos::BottomRight:    dlgX = pw.screenX() + cW - dlgW - 16;       dlgY = pw.screenY() + cH - dlgH - 16; break;
            case Pos::BottomCenter:   dlgX = pw.screenX() + (cW - dlgW) / 2;      dlgY = pw.screenY() + cH - dlgH - 16; break;
            case Pos::CenterOnParent:
            default:                  dlgX = pw.screenX() + (cW - dlgW) / 2;      dlgY = pw.screenY() + (cH - dlgH) / 2; break;
            }
            dialogs.emplace_back(*req, hal, dlgX, dlgY,
                static_cast<jf::JNativeDialogWindow::NativeWinHandleType>(pw.rawWindowId()));
            jf::JDialogManager::instance().pop();
        }
        for (auto it = dialogs.begin(); it != dialogs.end();) {
            if (!it->pollAndRender(hal, scratch)) { it->destroySurface(hal); it = dialogs.erase(it); }
            else ++it;
        }
        if (!dialogs.empty()) win.requestRedraw();
    };

    // Latest cursor position (for tooltips; content input is routed by the runner straight
    // to each dock's onInputContent hook, so no manual widget dispatch here).
    float mx = -1.f, my = -1.f;
    win.onInput = [&](float x, float y, bool, bool) { mx = x; my = y; serviceOverlays(); };

    // App hotkeys for keys the runner didn't route to a focused widget.
    win.onKey = [&](const JKeyEvent& ke) {
        if (!ke.pressed) return;
        if (ke.utf8[0] == 'a' || ke.utf8[0] == 'A') catalog.toggleAnimation();
        if (ke.utf8[0] == 'f' || ke.utf8[0] == 'F') catalog.forceTear(1);
    };

    auto last = std::chrono::steady_clock::now();
    win.onRender = [&](JPrimitiveBuffer& buf) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        catalog.update(dt);
        catalog.layoutExtra();
        if (catalog.isAnimating()) win.requestRedraw();   // keep frames coming while animating
        JWidget::renderTooltips(buf, mx, my);
    };

    std::cout << "[GENESIS] Catalog on the JAppWindow runner. Tab cycles focus; close to exit.\n";
    return win.run();
}

