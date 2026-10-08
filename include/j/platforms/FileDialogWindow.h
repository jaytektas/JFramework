// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

#pragma once

// ============================================================================
// JFileDialogWindow — the framework's in-app file/folder picker: a dialog window of
// its own (JDialogWindow), drawn by the toolkit's own widgets, the folder walked
// with std::filesystem — no zenity, no GetOpenFileName, the same on every platform.
//
// Driven by a file-kind JDialogRequest (OpenFile / SaveFile / OpenFolder):
//   title       — window caption
//   extensions  — what is shown ("json", "gui"); empty = every file
//   filters     — opt-in: named file types to choose between (JFileFilter)
//   startPath   — opt-in: the folder (or file) to begin at
//   onInput     — the chosen absolute path
//   onCancel    — Cancel, Escape or the [x]: whenever nothing was chosen
//
// What a desktop's file chooser has, laid out as one:
//   * Up, Home, the folder as an editable path (type or paste one, Enter goes there; a file's path
//     opens it; "~" is home), New Folder (named in place, Enter makes it);
//   * Places (home, Desktop, Documents, Downloads, the file system, mounted drives) beside
//   * the folder: Name, Size and Modified, folders first, sorted by a header click; a click chooses,
//     a double-click (or Enter) opens a folder or takes a file;
//   * Name: a real text field (caret, selection, paste) — a file name, or a path to go to;
//   * Show hidden (Ctrl+H) and the file type; Cancel and Open / Save / Choose.
// Saving over a file that exists asks first (the button becomes Replace). Alt+Up goes up, Alt+Home
// home, Ctrl+L to the path, F5 lists the folder again. The last folder and Show hidden are remembered
// across opens.
// ============================================================================

#include <j/app/JDialogWindow.h>
#include <j/core/Dialog.h>
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JDataGrid.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JFileFilter.h>
#include <j/core/JLineEdit.h>
#include <j/core/JListView.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/io/DirectoryListing.h>
#include <j/io/JFilePlaces.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

inline namespace jf {

class JFileDialogWindow : public JDialogWindow {
public:
    // Its opening size (openModal's and the app window's placement read these); it can be resized.
    static constexpr uint32_t kW = 780, kH = 520;

    JFileDialogWindow(JDialogRequest req, JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parentWindow = {})
        : JDialogWindow(req.title, kW, kH, hal, screenX, screenY, parentWindow)
        , m_req(std::move(req)) {
        setResizable(true, kW * 2 / 3, kH * 2 / 3);
        JSceneGraph& g = graph();
        const bool folders = m_req.kind == JDialogRequest::JKind::OpenFolder;
        const bool save = m_req.kind == JDialogRequest::JKind::SaveFile;

        m_up = std::make_unique<JButton>(g, "Up", 0.f);
        m_up->setTooltip("The folder this one is in (Alt+Up)");
        m_up->onClicked.connect([this] { _goUp(); });
        m_home = std::make_unique<JButton>(g, "Home", 0.f);
        m_home->setTooltip("Your home folder (Alt+Home)");
        m_home->onClicked.connect([this] { _navigate(JDirectoryListing::homeFolder()); });
        m_path = std::make_unique<JLineEdit>(g, "Folder");
        m_path->setTooltip("The folder shown: type or paste another and press Enter to go there (Ctrl+L)");
        m_newFolder = std::make_unique<JButton>(g, "New Folder", 0.f);
        m_newFolder->setTooltip("Make a folder here");
        m_newFolder->onClicked.connect([this] { _startNewFolder(); });
        for (JWidget* w : std::initializer_list<JWidget*>{ m_up.get(), m_home.get(), m_path.get(), m_newFolder.get() }) add(w);

        // Naming a new folder, in place (shown only while it is being named).
        m_folderName = std::make_unique<JLineEdit>(g, "Folder name");
        m_folderName->setTooltip("The new folder's name: Enter makes it, Escape does not");
        m_create = std::make_unique<JButton>(g, "Create", 0.f);
        m_create->onClicked.connect([this] { _createFolder(); });
        m_createCancel = std::make_unique<JButton>(g, "Cancel", 0.f);
        m_createCancel->onClicked.connect([this] { _endNewFolder(); });

        m_places = std::make_unique<JListView>(g);
        m_placeList = JFilePlaces::list();
        std::vector<std::string> placeNames;
        for (const auto& p : m_placeList) placeNames.push_back(p.name);
        m_places->setItems(placeNames);
        m_places->setTooltip("Places to go to");
        m_places->onItemActivated.connect([this](int i) {
            if (i >= 0 && i < (int)m_placeList.size()) _navigate(m_placeList[size_t(i)].path);
        });
        add(m_places.get());

        m_list = std::make_unique<JDataGrid>(g, std::vector<std::string>{ "Name", "Size", "Modified" });
        m_list->setSortable(true);
        m_list->setColumnAlignment(1, JDataGrid::ColAlign::Right);
        m_list->setSortComparator([this](int a, int b, int column, bool ascending) { return _before(a, b, column, ascending); });
        m_list->sortByColumn(0, true);
        m_list->onSelectionChanged.connect([this](int row) { _chose(row); });
        m_list->onRowActivated.connect([this](int row) { m_activated = row; });
        add(m_list.get());

        m_name = std::make_unique<JLineEdit>(g, folders ? "This folder" : save ? "File name" : "File name, or a path");
        m_name->setTooltip(folders ? "The folder chosen: the one picked in the list, else the one shown"
                                   : "The file's name (a path goes there); Enter takes it");
        m_name->setReadOnly(folders);
        m_name->onTextChanged.connect([this](const std::string&) { _unconfirm(); });
        add(m_name.get());

        m_hidden = std::make_unique<JCheckBox>(g, kShowHidden, 0.f);
        m_hidden->setChecked(s_showHidden);
        m_hidden->setTooltip("Show files and folders whose names begin with a dot (Ctrl+H)");
        m_hidden->onStateChanged.connect([this](bool on) {
            s_showHidden = on;
            _list();
        });
        add(m_hidden.get());

        // The file types: those asked for (or one made of the extensions), then every file.
        if (!folders) {
            m_filters = m_req.filters;
            if (m_filters.empty() && !m_req.extensions.empty()) m_filters.push_back({ "", m_req.extensions });
            m_filters.push_back({ "All files", {} });
            std::vector<std::string> labels;
            for (const JFileFilter& f : m_filters) labels.push_back(f.label());
            m_type = std::make_unique<JComboBox>(g, labels);
            m_type->setCurrentIndex(0);
            m_type->setTooltip("The kind of file shown");
            m_type->onIndexChanged.connect([this](int) { _list(); });
            add(m_type.get());
        }

        m_buttons = std::make_unique<JDialogButtonBox>(g);
        m_buttons->addButton("Cancel", JDialogButtonBox::Role::Reject)->setTooltip("Close without choosing");
        m_ok = m_buttons->addButton(_okLabel(), JDialogButtonBox::Role::Accept);
        m_ok->setTooltip(folders ? "Choose this folder" : save ? "Save as this file" : "Open this file");
        m_buttons->onReject.connect([this] { close(); });
        m_buttons->onAccept.connect([this] { _accept(); });
        add(m_buttons.get());

        // Where to begin: the start asked for (a file: its folder, its name), else where it was last.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path start = s_lastDir.empty() ? fs::current_path(ec) : fs::path(s_lastDir);
        std::string startName;
        if (!m_req.startPath.empty()) {
            const fs::path asked = JDirectoryListing::resolve(m_req.startPath, start);
            if (fs::is_directory(asked, ec)) start = asked;
            else {
                start = asked.parent_path();
                startName = asked.filename().string();
            }
        }
        if (!fs::is_directory(start, ec)) start = JDirectoryListing::homeFolder();
        _navigate(start);
        if (save) m_name->setText(startName.empty() ? _defaultSaveName() : startName);
        else if (!folders && !startName.empty()) m_name->setText(startName);
    }

    ~JFileDialogWindow() override {
        // Nothing chosen (Cancel, Escape, the [x], the app closing): the asker is told so.
        if (!m_accepted && m_req.onCancel) m_req.onCancel();
    }

protected:
    void layout(float w, float h) override {
        const JStyle& st = JStyle::current();
        const float pad = st.spacing * 2, gap = st.spacing, row = rowH(), bh = btnH();
        float y = contentTop();

        // Top: Up, Home, the path, New Folder.
        float x = pad;
        auto place = [&](JWidget* wd, float width, float height) { wd->setBounds({ x, y, width, height }); x += width + gap; };
        place(m_up.get(), JButton::labelWidth(m_up->label()), row);
        place(m_home.get(), JButton::labelWidth(m_home->label()), row);
        const float nfW = JButton::labelWidth(m_newFolder->label());
        m_path->setBounds({ x, y, std::max(0.f, w - pad - nfW - gap - x), row });
        m_newFolder->setBounds({ w - pad - nfW, y, nfW, row });
        y += row + gap;

        // Naming a new folder.
        if (m_naming) {
            const float createW = JButton::labelWidth(m_create->label()), cancelW = JButton::labelWidth(m_createCancel->label());
            const float labelW = JTextHelper::measureWidth(kNewFolderLabel) + gap;
            m_folderLabelY = y;
            m_folderName->setBounds({ pad + labelW, y, std::max(0.f, w - 2 * pad - labelW - createW - cancelW - 2 * gap), row });
            m_create->setBounds({ w - pad - cancelW - gap - createW, y, createW, row });
            m_createCancel->setBounds({ w - pad - cancelW, y, cancelW, row });
            y += row + gap;
        }

        // Bottom up: the buttons, the type and Show hidden, the status line, the name.
        const float buttonsY = h - pad - bh;
        const float statusY = buttonsY - gap - JTextHelper::lineHeight();
        const float nameY = statusY - gap - row;
        m_statusY = statusY;
        m_hidden->setBounds({ pad, buttonsY + (bh - st.checkHeight) * 0.5f, _hiddenW(), st.checkHeight });
        const float buttonsW = JButton::dialogButtonWidth({ _okLabel(), "Replace", "Cancel" }) * 2 + gap;
        m_buttons->setBounds({ w - pad - buttonsW, buttonsY, buttonsW, bh });
        if (m_type) {
            const float typeX = pad + _hiddenW() + 2 * gap;
            m_type->setBounds({ typeX, buttonsY + (bh - row) * 0.5f, std::max(0.f, w - pad - buttonsW - gap - typeX), row });
        }
        m_nameLabelW = JTextHelper::measureWidth(_nameLabel()) + gap;
        m_nameY = nameY;
        m_name->setBounds({ pad + m_nameLabelW, nameY, std::max(0.f, w - 2 * pad - m_nameLabelW), row });

        // The middle: places beside the folder.
        const float bodyH = std::max(0.f, nameY - gap - y);
        float placesW = 0;
        for (const auto& p : m_placeList) placesW = std::max(placesW, JTextHelper::measureWidth(p.name));
        placesW += 2 * st.itemPadding + st.scrollBarWidth;
        m_places->setBounds({ pad, y, placesW, bodyH });
        const float listX = pad + placesW + gap, listW = std::max(0.f, w - pad - listX);
        m_list->setBounds({ listX, y, listW, bodyH });
        const float sizeW = std::max(JTextHelper::measureWidth("000.0 MB"), JTextHelper::measureWidth("000 bytes"))
                            + 2 * st.gridCellPadding + st.itemPadding;
        const float timeW = JTextHelper::measureWidth("0000-00-00 00:00") + 2 * st.gridCellPadding + st.itemPadding;
        m_list->setColumnWidths({ std::max(0.f, listW - sizeW - timeW - st.scrollBarWidth), sizeW, timeW });
    }

    void paint(JPrimitiveBuffer& buf, float, float) override {
        const JStyle& st = JStyle::current();
        const float pad = st.spacing * 2, lh = JTextHelper::lineHeight(), row = rowH();
        JTextHelper::pushText(buf, pad, m_nameY + (row - lh) * 0.5f, _nameLabel(), Colors::TextSecondary);
        if (m_naming)
            JTextHelper::pushText(buf, pad, m_folderLabelY + (row - lh) * 0.5f, kNewFolderLabel, Colors::TextSecondary);
        if (!m_status.empty())
            JTextHelper::pushText(buf, pad, m_statusY, m_status, m_statusIsError ? Colors::Danger : Colors::TextSecondary,
                                  static_cast<float>(width()) - 2 * pad);
    }

    bool onKey(const JKeyEvent& ke) override {
        using K = JKeyEvent::JKey;
        const JDialogKeyBindings& kb = JDialogManager::keyBindings();
        if (ke.alt && ke.key == K::Up) { _goUp(); return true; }
        if (ke.alt && ke.key == K::Home) { _navigate(JDirectoryListing::homeFolder()); return true; }
        if (ke.ctrl && ke.key == K::H) { m_hidden->setChecked(!m_hidden->isChecked()); return true; }
        if (ke.ctrl && ke.key == K::L) { m_path->requestFocus(); m_path->selectAll(); return true; }
        if (ke.key == K::F5) { _list(); return true; }
        // Enter in the path goes there; in the new folder's name makes it — neither is the dialog's accept.
        if (ke.key == kb.accept && m_path->isFocused()) { _goTo(m_path->text()); return true; }
        if (m_naming && ke.key == kb.accept && m_folderName->isFocused()) { _createFolder(); return true; }
        if (m_naming && ke.key == kb.cancel) { _endNewFolder(); return true; }
        return false;
    }

    void onMouse(float, float, bool pressed, bool, bool) override {
        // Focus where the work is, once, after the window's own first pick (which takes the first field):
        // the name to save as (its text chosen), else the folder's list.
        if (!m_focusPlaced) {
            m_focusPlaced = true;
            if (_save()) {
                m_name->requestFocus();
                m_name->selectAll();
            } else {
                m_list->requestFocus();
            }
        }
        // A row activated: by Enter (no press this frame), or by a double-click — a single click only chose it.
        if (m_activated >= 0) {
            const int row = m_activated;
            m_activated = -1;
            if (!pressed || JWidget::s_doubleClick) _activate(row);
        }
    }

private:
    using Entry = JDirectoryListing::Entry;
    static constexpr const char* kNewFolderLabel = "New folder:";
    static constexpr const char* kShowHidden = "Show hidden";

    static std::string s_lastDir;   // remembered across opens
    static bool        s_showHidden;

    bool _folders() const { return m_req.kind == JDialogRequest::JKind::OpenFolder; }
    bool _save() const { return m_req.kind == JDialogRequest::JKind::SaveFile; }
    const char* _okLabel() const { return _save() ? "Save" : _folders() ? "Choose" : "Open"; }
    float _hiddenW() const {
        const JStyle& st = JStyle::current();
        return st.checkHeight + st.itemPadding + JTextHelper::measureWidth(kShowHidden) + 1.f;
    }
    const char* _nameLabel() const { return _folders() ? "Folder:" : "Name:"; }

    // The extensions shown now (the type chosen; a folder picker: none, as it lists folders only).
    std::vector<std::string> _extensions() const {
        if (!m_type) return {};
        const int i = m_type->currentIndex();
        return (i >= 0 && i < (int)m_filters.size()) ? m_filters[size_t(i)].extensions : std::vector<std::string>{};
    }

    std::string _defaultSaveName() const {
        const auto ext = _extensions();
        if (!ext.empty()) return "untitled." + ext.front();
        if (!m_req.extensions.empty()) return "untitled." + m_req.extensions.front();
        return "untitled";
    }

    // ---- The folder ----------------------------------------------------------------------------
    void _navigate(const std::filesystem::path& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) { _say("No folder " + dir.string(), true); return; }
        fs::path canon = fs::weakly_canonical(dir, ec);
        m_cwd = ec ? dir : canon;
        s_lastDir = m_cwd.string();
        m_path->setText(m_cwd.string());
        _endNewFolder();
        _say("", false);
        _list();
        m_list->scrollToTop();
    }

    void _list() {
        std::vector<Entry> all = JDirectoryListing::list(m_cwd, _extensions(), s_showHidden);
        if (_folders()) all.erase(std::remove_if(all.begin(), all.end(), [](const Entry& e) { return !e.isDir; }), all.end());
        m_entries = std::move(all);
        std::vector<std::vector<std::string>> rows;
        rows.reserve(m_entries.size());
        for (const Entry& e : m_entries)
            rows.push_back({ e.isDir ? e.name + "/" : e.name, e.isDir ? std::string() : _size(e.size), _when(e.modified) });
        m_list->setSelectedIndex(-1);
        m_list->setRows(rows);
        _unconfirm();
    }

    void _goUp() {
        if (m_cwd.has_parent_path() && m_cwd.parent_path() != m_cwd) {
            const std::string from = m_cwd.filename().string();
            _navigate(m_cwd.parent_path());
            for (size_t i = 0; i < m_entries.size(); ++i)   // the folder come out of, chosen
                if (m_entries[i].isDir && m_entries[i].name == from) m_list->setSelectedIndex(int(i));
        }
    }

    // A path typed (the path field, or a path in the name): a folder is gone to; a file, in a file picker,
    // is taken (its folder shown, its name in the field).
    void _goTo(const std::string& typed) {
        namespace fs = std::filesystem;
        if (typed.empty()) return;
        const fs::path p = JDirectoryListing::resolve(typed, m_cwd);
        std::error_code ec;
        if (fs::is_directory(p, ec)) { _navigate(p); return; }
        if (!_folders() && fs::is_directory(p.parent_path(), ec) && (fs::exists(p, ec) || _save())) {
            _navigate(p.parent_path());
            m_name->setText(p.filename().string());
            _accept();
            return;
        }
        _say("No such folder or file: " + p.string(), true);
    }

    // ---- Choosing ------------------------------------------------------------------------------
    void _chose(int row) {
        if (row < 0 || row >= (int)m_entries.size()) return;
        const Entry& e = m_entries[size_t(row)];
        if (_folders()) m_name->setText(e.name);
        else if (!e.isDir) m_name->setText(e.name);
    }

    void _activate(int row) {
        if (row < 0 || row >= (int)m_entries.size()) return;
        const Entry e = m_entries[size_t(row)];
        if (e.isDir) { _navigate(m_cwd / e.name); return; }
        m_name->setText(e.name);
        _accept();
    }

    void _accept() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (_folders()) {
            const int row = m_list->selectedIndex();
            const fs::path chosen = (row >= 0 && row < (int)m_entries.size()) ? m_cwd / m_entries[size_t(row)].name : m_cwd;
            _finish(chosen.string());
            return;
        }
        std::string name = m_name->text();
        if (name.empty()) {
            const int row = m_list->selectedIndex();
            if (row >= 0 && row < (int)m_entries.size() && m_entries[size_t(row)].isDir) _activate(row);
            else _say(_save() ? "Type a name to save as" : "Choose a file to open", true);
            return;
        }
        // A path in the name: a folder is gone to; a file elsewhere is taken from there.
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name[0] == '~') {
            const fs::path p = JDirectoryListing::resolve(name, m_cwd);
            if (fs::is_directory(p, ec)) { _navigate(p); m_name->setText(""); return; }
            if (!fs::is_directory(p.parent_path(), ec)) { _say("No folder " + p.parent_path().string(), true); return; }
            if (p.parent_path() != m_cwd) _navigate(p.parent_path());
            name = p.filename().string();
            m_name->setText(name);
        }
        const fs::path named = m_cwd / name;
        if (fs::is_directory(named, ec)) { _navigate(named); m_name->setText(""); return; }
        if (_save()) {
            // The type's ending added to a name without it (all files: to a name without any).
            fs::path target = named;
            const auto ext = _extensions();
            if (!ext.empty() && !JDirectoryListing::passesFilter(name, ext)) target += "." + ext.front();
            else if (ext.empty() && !target.has_extension() && !m_req.extensions.empty()) target += "." + m_req.extensions.front();
            if (fs::exists(target, ec) && !m_confirming) {
                m_confirming = true;
                m_ok->setLabel("Replace");
                _say("\"" + target.filename().string() + "\" is already here: Replace overwrites it", true);
                return;
            }
            _finish(target.string());
            return;
        }
        if (!fs::exists(named, ec)) { _say("No file \"" + name + "\" here", true); return; }
        _finish(named.string());
    }

    void _finish(const std::string& path) {
        m_accepted = true;
        if (m_req.onInput) m_req.onInput(path);
        close();
    }

    void _unconfirm() {
        if (!m_confirming) return;
        m_confirming = false;
        m_ok->setLabel(_okLabel());
        _say("", false);
    }

    // ---- New folder ----------------------------------------------------------------------------
    void _startNewFolder() {
        if (!m_naming) {
            m_naming = true;
            add(m_folderName.get());
            add(m_create.get());
            add(m_createCancel.get());
        }
        m_folderName->setText(_freeName("New Folder"));
        m_folderName->requestFocus();
        m_folderName->selectAll();
        _say("", false);
    }

    void _endNewFolder() {
        if (!m_naming) return;
        m_naming = false;
        remove(m_folderName.get());
        remove(m_create.get());
        remove(m_createCancel.get());
        m_list->requestFocus();
    }

    void _createFolder() {
        namespace fs = std::filesystem;
        const std::string name = m_folderName->text();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos
            || name.find('\\') != std::string::npos) {
            _say("A folder's name cannot be empty, \".\", \"..\" or have a slash in it", true);
            return;
        }
        std::error_code ec;
        if (fs::exists(m_cwd / name, ec)) { _say("\"" + name + "\" is already here", true); return; }
        if (!fs::create_directory(m_cwd / name, ec)) {
            _say("Could not make \"" + name + "\": " + (ec ? ec.message() : std::string("refused")), true);
            return;
        }
        _endNewFolder();
        _list();
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].isDir && m_entries[i].name == name) m_list->setSelectedIndex(int(i));
        _say("Made \"" + name + "\"", false);
    }

    std::string _freeName(const std::string& base) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(m_cwd / base, ec)) return base;
        for (int n = 2;; ++n)
            if (!fs::exists(m_cwd / (base + " " + std::to_string(n)), ec)) return base + " " + std::to_string(n);
    }

    // ---- Showing -------------------------------------------------------------------------------
    void _say(const std::string& text, bool error) {
        m_status = text;
        m_statusIsError = error;
    }

    // Folders first whichever way; then the column (names without case, sizes and times by value).
    bool _before(int a, int b, int column, bool ascending) const {
        if (a < 0 || b < 0 || a >= (int)m_entries.size() || b >= (int)m_entries.size()) return a < b;
        const Entry& x = m_entries[size_t(a)];
        const Entry& y = m_entries[size_t(b)];
        if (x.isDir != y.isDir) return x.isDir;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        };
        int c = 0;
        if (column == 1 && x.size != y.size) c = x.size < y.size ? -1 : 1;
        else if (column == 2 && x.modified != y.modified) c = x.modified < y.modified ? -1 : 1;
        if (c == 0) {
            const std::string lx = lower(x.name), ly = lower(y.name);
            c = lx < ly ? -1 : lx > ly ? 1 : 0;
        }
        return ascending ? c < 0 : c > 0;
    }

    static std::string _size(std::uintmax_t bytes) {
        char buf[32];
        if (bytes < 1000) std::snprintf(buf, sizeof buf, "%ju bytes", bytes);
        else if (bytes < 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f kB", double(bytes) / 1e3);
        else if (bytes < 1000ull * 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / 1e6);
        else std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / 1e9);
        return buf;
    }

    static std::string _when(std::filesystem::file_time_type t) {
        if (t == std::filesystem::file_time_type{}) return "";
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
        return buf;
    }

    JDialogRequest                   m_req;
    std::filesystem::path            m_cwd;
    std::vector<Entry>               m_entries;
    std::vector<JFilePlaces::Place>  m_placeList;
    std::vector<JFileFilter>         m_filters;

    std::unique_ptr<JButton>          m_up, m_home, m_newFolder, m_create, m_createCancel;
    std::unique_ptr<JLineEdit>        m_path, m_folderName, m_name;
    std::unique_ptr<JListView>        m_places;
    std::unique_ptr<JDataGrid>        m_list;
    std::unique_ptr<JCheckBox>        m_hidden;
    std::unique_ptr<JComboBox>        m_type;
    std::unique_ptr<JDialogButtonBox> m_buttons;
    JButton*                          m_ok = nullptr;

    std::string m_status;
    bool  m_statusIsError = false;
    bool  m_accepted = false, m_confirming = false, m_naming = false, m_focusPlaced = false;
    int   m_activated = -1;
    float m_nameY = 0, m_nameLabelW = 0, m_statusY = 0, m_folderLabelY = 0;
};

inline std::string JFileDialogWindow::s_lastDir;
inline bool        JFileDialogWindow::s_showHidden = false;

} // inline namespace jf
