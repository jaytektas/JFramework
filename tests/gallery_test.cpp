// gallery_test.cpp — the whole-framework "everything composes" integration gate.
//
// One translation unit that pulls in EVERY major JFramework widget + subsystem,
// constructs one of each, renders them all headlessly into a JPrimitiveBuffer
// without crashing, then drives a representative interaction on each subsystem and
// asserts observable state. If two headers ever collide (ODR / redefinition /
// ambiguity) this TU simply won't compile — that failure is itself the signal.
//
// Headless: no window, no GPU. A synthetic monospace font atlas makes glyph
// metrics deterministic so text widgets render real primitives.
//
// Build:
//   g++ -std=c++20 -I<repo>/include -I<repo>/third_party \
//       tests/gallery_test.cpp -o /tmp/gallery_test
// Prints PASS/FAIL per case + a group banner; exits non-zero on any failure.
//
// NOTE ON EXCLUDED HEADERS (real integration defects, reported, not worked around):
//   * j/core/Animator.h  redefines `enum class JEasing` incompatibly with the live
//     j/core/Easing.h + j/core/Animation.h — the two cannot coexist in one TU.
//     The live animation subsystem (JAnimation / JAnimator / jAnimator()) lives in
//     Animation.h; Animator.h is a stale duplicate and is deliberately NOT included.
//   * j/model/TableModel.h redefines `class JGridModel` already defined (and used
//     by the shipping model/view stack) in j/model/ItemModel.h. The canonical
//     JGridModel comes from ItemModel.h; TableModel.h is deliberately NOT included.

// ---- widgets -----------------------------------------------------------------
#include "j/core/BaseWidgets.h"      // JLabel/JButton/JToolButton/JToggleButton/JCheckBox/
                                     // JRadioButton/JSlider/JProgressBar/JScrollBar/JLineEdit/
                                     // JSpinBox/JDoubleSpinBox/JComboBox/JTabWidget/JGroupBox/
                                     // JContainer/JScrollArea/JListView/JSeparator + JTheme/JStyle
#include "j/core/ItemView.h"         // JItemListView / JItemTableView (+ JAbstractItemView)
#include "j/core/Splitter.h"         // JSplitter
// ---- subsystems --------------------------------------------------------------
#include "j/core/FocusManager.h"     // JFocusManager
#include "j/core/Animation.h"        // JAnimation / JAnimationGroup / JAnimator / jAnimator()
#include "j/core/Easing.h"           // JEasing / ease()
#include "j/core/FrameTimer.h"       // JFrameTimer / jTimers() / jPostToNextFrame
#include "j/core/Timer.h"            // JTimer (coexistence with FrameTimer)
#include "j/core/Command.h"          // JFunctionCommand
#include "j/core/UndoStack.h"        // JUndoStack
#include "j/core/KeySequence.h"      // JKeySequence
#include "j/core/Action.h"           // JAction
#include "j/core/ShortcutMap.h"      // jShortcuts()
#include "j/core/StandardDialogs.h"  // JMessageBox / JInputDialog
#include "j/core/Validator.h"        // JIntValidator / JDoubleValidator
#include "j/core/DragDrop.h"         // JMimeData / drag session (coexistence)
// ---- model / view ------------------------------------------------------------
#include "j/model/ItemModel.h"       // JStringListModel / JGridModel / JModelIndex
#include "j/model/SelectionModel.h"  // JItemSelectionModel
#include "j/model/AbstractItemView.h"
#include "j/model/ItemDelegate.h"    // JStyledItemDelegate

#include <cstdio>
#include <string>
#include <vector>

using namespace jf;

// ---- test harness ------------------------------------------------------------
static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("  %-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++g_fail;
}
static void group(const char* name) { std::printf("\n== %s ==\n", name); }

// ---- synthetic monospace atlas (8 px advance / glyph) ------------------------
static void installMonoAtlas() {
    JFontAtlas atl;
    atl.valid = true;
    atl.pixelSize = 12.f; atl.ascent = 12.f; atl.descent = 3.f; atl.lineHeight = 16.f;
    for (uint32_t cp = 32; cp < 127; ++cp) {
        JGlyphInfo g{}; g.advanceX = 8.f; g.pixelW = 6.f; g.pixelH = 10.f;
        g.bearingX = 1.f; g.bearingY = -10.f; atl.glyphs[cp] = g;
    }
    JTextHelper::setAtlas(std::move(atl));
}

// ---- key-event builders ------------------------------------------------------
static JKeyEvent keyC(JKeyEvent::JKey k, bool ctrl=false) {
    JKeyEvent e; e.key = k; e.ctrl = ctrl; e.pressed = true; return e;
}
static JKeyEvent typeCh(char c) {
    JKeyEvent e; e.key = JKeyEvent::JKey::Unknown; e.utf8[0] = c; e.utf8[1] = '\0'; e.pressed = true; return e;
}

// Render a widget headlessly; return how many draw commands it emitted.
static size_t render(JWidget& w, JRect box = {0.f, 0.f, 240.f, 120.f}) {
    w.setBounds(box);
    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    return buf.getCommands().size();
}

// First emitted rectangle's fill (for the theme-swap byte comparison).
static JColor firstFill(JWidget& w) {
    JPrimitiveBuffer buf;
    w.populateRenderPrimitives(buf);
    for (const auto& c : buf.getCommands())
        if (c.kind == JPrimitiveBuffer::JDrawCommand::JKind::JRect)
            return JColor::fromArray(c.rect.color);
    return {};
}

int main() {
    installMonoAtlas();
    JSceneGraph graph;

    // =========================================================================
    group("GROUP 1 — every widget constructs + renders into one buffer");
    // =========================================================================
    size_t total = 0;
    int    widgetTypes = 0;

    // Leaf widgets that draw chrome/text: each MUST emit primitives.
    auto gateLeaf = [&](const char* nm, JWidget& w, JRect box) {
        size_t n = render(w, box);
        total += n; ++widgetTypes;
        check(nm, n > 0);
    };
    // Container widgets: must not crash; count folded into total.
    auto gateContainer = [&](const char* nm, JWidget& w, JRect box) {
        size_t n = render(w, box);
        total += n; ++widgetTypes;
        check(nm, true);            // reaching here == no throw/crash
    };

    JLabel        lbl(graph, "Label");                 gateLeaf("JLabel", lbl, {0,0,240,22});
    JButton       btn(graph, "OK");                    gateLeaf("JButton", btn, {0,0,160,30});
    JToolButton   tbtn(graph, "Tool");                 gateLeaf("JToolButton", tbtn, {0,0,96,30});
    JToggleButton tog(graph, "Toggle");                gateLeaf("JToggleButton", tog, {0,0,160,30});
    JCheckBox     chk(graph, "Check");                 gateLeaf("JCheckBox", chk, {0,0,200,22});
    JRadioButton  rad(graph, "Radio");                 gateLeaf("JRadioButton", rad, {0,0,200,22});
    JSlider       sld(graph);                          gateLeaf("JSlider", sld, {0,0,280,20});
    JProgressBar  prg(graph);      prg.setProgress(0.5f); gateLeaf("JProgressBar", prg, {0,0,280,12});
    JScrollBar    sbar(graph);                         gateLeaf("JScrollBar", sbar, {0,0,280,14});
    JLineEdit     led(graph, "hint");  led.setText("edit"); gateLeaf("JLineEdit", led, {0,0,280,30});
    JSpinBox      spn(graph, 0, 100);  spn.setValue(42); gateLeaf("JSpinBox", spn, {0,0,140,30});
    JDoubleSpinBox dspn(graph, 0.0, 10.0); dspn.setValue(1.5); gateLeaf("JDoubleSpinBox", dspn, {0,0,120,30});
    JComboBox     cmb(graph, {"Red","Green","Blue"});  gateLeaf("JComboBox", cmb, {0,0,200,30});
    JGroupBox     grp(graph, "Group");                 gateLeaf("JGroupBox", grp, {0,0,320,120});
    JListView     lv(graph, {"a","b","c"});            gateLeaf("JListView", lv, {0,0,200,120});
    JSeparator    sep(graph);                          gateLeaf("JSeparator", sep, {0,0,240,2});

    // JTabWidget with one real tab (content owned by us).
    JLabel        tabContent(graph, "Tab body");
    JTabWidget    tabs(graph);  tabs.addTab("General", &tabContent);
    gateLeaf("JTabWidget", tabs, {0,0,400,260});

    // Model/view widgets bound to real models.
    JStringListModel listModel({"crank","cam","map","clt","tps"});
    JItemListView    ilv(graph, 200.f, 200.f); ilv.setModel(&listModel);
    gateLeaf("JItemListView", ilv, {0,0,200,160});

    JGridModel      tableModel(3, 2);
    tableModel.setData(tableModel.index(0,0), JVariant(11));
    JItemTableView   itv(graph, 320.f, 200.f); itv.setModel(&tableModel);
    gateContainer("JItemTableView", itv, {0,0,320,160});

    // Pure containers (no chrome of their own) — just prove they render children safely.
    JButton      childBtn(graph, "child");
    JContainer   ctr(graph);  ctr.setDirection(JFlexDirection::JRow)->add(&childBtn);
    gateContainer("JContainer", ctr, {0,0,300,60});

    JScrollArea  scr(graph);  scr.addChildWidget(&lbl);
    gateContainer("JScrollArea", scr, {0,0,320,200});

    JSplitter    split(graph, JSplitter::JOrientation::Horizontal, 600.f, 400.f);
    JLabel       paneA(graph, "A"), paneB(graph, "B");
    split.addPane(&paneA); split.addPane(&paneB); split.layout();
    gateContainer("JSplitter", split, {0,0,600,400});

    check("all widgets together emitted primitives", total > 0);
    std::printf("  [%d widget types rendered, %zu total draw commands]\n", widgetTypes, total);

    // =========================================================================
    group("GROUP 2 — driven interactions assert observable state");
    // =========================================================================
    int subsystems = 0;

    // 1) JButton click fires onClicked.
    { ++subsystems;
      JButton b(graph, "Go"); b.setBounds({0,0,160,30});
      int clicks = 0; b.onClicked.connect([&]{ ++clicks; });
      b.handleMousePress(10.f, 10.f);
      check("JButton click fires onClicked", clicks == 1);
    }

    // 2) Tri-state JCheckBox cycles Unchecked->Checked->Partial->Unchecked.
    { ++subsystems;
      JCheckBox cb(graph, "opt"); cb.setBounds({0,0,200,22}); cb.setTristate(true);
      cb.handleMousePress(5,11);  bool c1 = cb.checkState() == JCheckBox::Checked;
      cb.handleMousePress(5,11);  bool c2 = cb.checkState() == JCheckBox::PartiallyChecked;
      cb.handleMousePress(5,11);  bool c3 = cb.checkState() == JCheckBox::Unchecked;
      check("JCheckBox tri-state cycles", c1 && c2 && c3);
    }

    // 3) JLineEdit typed via handleKeyEvent reads back.
    { ++subsystems;
      JLineEdit le(graph); le.setBounds({0,0,280,30});
      le.handleKeyEvent(typeCh('h')); le.handleKeyEvent(typeCh('i'));
      check("JLineEdit types 'hi'", le.text() == "hi");
    }

    // 4) JSlider moves and reports value.
    { ++subsystems;
      JSlider s(graph); s.setBounds({0,0,200,20});
      s.setValue(0.75f);
      check("JSlider setValue reads back 0.75", std::fabs(s.getValue() - 0.75f) < 1e-4f);
    }

    // 5) JProgressBar set + read.
    { ++subsystems;
      JProgressBar pb(graph); pb.setProgress(0.33f);
      check("JProgressBar reads back 0.33", std::fabs(pb.getProgress() - 0.33f) < 1e-4f);
    }

    // 6) JItemListView: bind model + keyboard navigate.
    { ++subsystems;
      JStringListModel m({"a","b","c","d"});
      JItemListView v(graph, 200.f, 160.f); v.setModel(&m);
      v.selectionModel()->setCurrentIndex(m.index(0,0),
          JItemSelectionModel::ClearAndSelect | JItemSelectionModel::Current);
      v.handleKeyEvent(keyC(JKeyEvent::JKey::Down));
      bool at1 = v.currentIndex().row() == 1;
      v.handleKeyEvent(keyC(JKeyEvent::JKey::End));
      bool atEnd = v.currentIndex().row() == 3;
      check("JItemListView navigates model (Down, End)", at1 && atEnd);
    }

    // 7) JUndoStack push + undo restores value.
    { ++subsystems;
      int val = 0; JUndoStack us;
      int prev = val;
      us.push(new JFunctionCommand("set5", [&]{ val = 5; }, [&,prev]{ val = prev; }));
      bool applied = (val == 5);
      us.undo();
      check("JUndoStack push applies, undo reverts", applied && val == 0 && us.canRedo());
    }

    // 8) jShortcuts() parse + dispatch Ctrl+S.
    { ++subsystems;
      jShortcuts().clear();
      JAction save("Save"); save.setShortcut(JKeySequence::parse("Ctrl+S"));
      int fired = 0; save.triggered.connect([&]{ ++fired; });
      jShortcuts().registerAction(&save);
      JKeyEvent e; e.key = JKeyEvent::JKey::S; e.ctrl = true; e.pressed = true;
      bool consumed = jShortcuts().dispatch(e);
      check("JKeySequence Ctrl+S dispatched via jShortcuts()", consumed && fired == 1);
      jShortcuts().clear();
    }

    // 9) JAnimation advances toward its target.
    { ++subsystems;
      JAnimation a(0.f, 100.f, 200.f, JEasing::Linear);
      float mid = a.advance(100.f);
      float end = a.advance(100.f);
      check("JAnimation advances 0->50->100", std::fabs(mid-50.f) < 0.5f &&
                                              std::fabs(end-100.f) < 0.01f && a.finished());
    }

    // 10) JFrameTimer fires a timeout under jTimers() ticking.
    { ++subsystems;
      jTimers().clear();
      JFrameTimer t; t.setInterval(100.f); t.setSingleShot(true);
      int fires = 0; t.timeout.connect([&]{ ++fires; });
      t.start();
      jTimers().tick(60.f); bool early = (fires == 0);
      jTimers().tick(60.f); bool fired = (fires == 1);
      check("JFrameTimer fires timeout on tick", early && fired);
      jTimers().clear();
    }

    // 11) JIntValidator classifies input.
    { ++subsystems;
      int pos = 0; JIntValidator iv(0, 100);
      bool ok  = iv.validate("42",  pos) == JValidator::Acceptable;
      bool bad = iv.validate("abc", pos) == JValidator::Invalid;
      bool oor = iv.validate("150", pos) == JValidator::Invalid;
      check("JIntValidator accepts/rejects/range", ok && bad && oor);
    }

    // 12) JMessageBox activateDefault resolves to the default button.
    { ++subsystems;
      JMessageBox box(JMessageIcon::Question, "Quit?", "Save first?",
                      { JDialogButton::Yes, JDialogButton::No });
      box.setDefaultButton(JDialogButton::Yes).setEscapeButton(JDialogButton::No);
      JDialogResult r = box.activateDefault();
      check("JMessageBox activateDefault -> Yes", r.button == JDialogButton::Yes && r.accepted);
    }

    // 13) JMimeData / drag payload round-trips (drag-drop subsystem).
    { ++subsystems;
      JMimeData m; m.setText("payload");
      check("JMimeData carries text/plain", m.hasText() && m.text() == "payload");
    }

    // 14) JStyledItemDelegate reads Display role from a model index.
    { ++subsystems;
      JStringListModel m({"x","y"});
      JStyledItemDelegate d;
      check("JStyledItemDelegate displayText == model Display", d.displayText(m.index(0,0)) == "x");
    }

    // =========================================================================
    group("GROUP 3 — theming drives paint; registries + managers coexist");
    // =========================================================================

    // Palette swap changes the bytes a widget paints.
    {
        JTheme::apply(JTheme::dark());
        JButton bd(graph, "OK");  JColor darkFill = firstFill(bd);
        // Install an explicit custom palette straight onto the theme.
        JPalette custom = JPalette::dark();
        const JColor green{0, 200, 0, 255};
        custom.setColor(JColorRole::Button, JColorGroup::Active, green);
        JTheme t = JTheme::dark(); t.setPalette(custom); JTheme::apply(t);
        JButton bc(graph, "OK");  JColor customFill = firstFill(bc);
        check("JPalette override changes painted bytes",
              customFill == green && customFill != darkFill);

        JTheme::apply(JTheme::light());
        JButton bl(graph, "OK");  JColor lightFill = firstFill(bl);
        check("light theme paints differently than dark", lightFill != darkFill);
        JTheme::apply(JTheme::dark());   // leave global state clean
    }

    // Focus manager, animator registry, and timer registry all live in one TU
    // and operate independently without stepping on each other.
    {
        JFocusManager fm;                       // installs the focus hook
        struct FW : JControl { FW(JSceneGraph& g):JControl(g,"FW"){}
            void populateRenderPrimitives(JPrimitiveBuffer&) override {} };
        FW w(graph); fm.registerWidget(&w);
        int focusEvents = 0; fm.onFocusChanged.connect([&](JWidget*){ ++focusEvents; });
        fm.setFocus(&w);
        bool focusOk = (focusEvents == 1) && w.isFocused();

        jAnimator().clear();
        jAnimator().animate(0.f, 1.f, 100.f, JEasing::Linear);
        jTimers().clear();
        int tick = 0; JFrameTimer::singleShot(50.f, [&]{ ++tick; });
        // Advance BOTH registries together; neither disturbs the other.
        jAnimator().tick(60.f);
        jTimers().tick(60.f);
        bool animReaped  = !jAnimator().hasActive();   // 60ms > 100ms? no -> still active
        bool bothCoexist = jAnimator().activeCount() == 1 && tick == 1;
        (void)animReaped;
        check("FocusManager + Animator + FrameTimer coexist & operate", focusOk && bothCoexist);
        jAnimator().clear(); jTimers().clear();
        fm.unregisterWidget(&w);
    }

    std::printf("\n%s (%d failure%s across %d widget types + %d driven subsystems)\n",
                g_fail ? "FAILURES" : "ALL PASS",
                g_fail, g_fail == 1 ? "" : "s", widgetTypes, subsystems);
    return g_fail ? 1 : 0;
}
