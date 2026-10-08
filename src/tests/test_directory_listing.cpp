// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Roughley <pis.controller@gmail.com>

// JDirectoryListing: folders before files, each by name; the extension filter (any case); hidden entries
// (a leading '.') only when asked for; sizes and times; a typed path made whole — a picker that can show them can reach ~/.openpnp2/machine.xml.
#include <j/io/DirectoryListing.h>

#include <cassert>
#include <filesystem>
#include <fstream>

using namespace jf;
namespace fs = std::filesystem;

static std::vector<std::string> names(const std::vector<JDirectoryListing::Entry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.isDir ? e.name + "/" : e.name);
    return out;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "jf_test_directory_listing";
    fs::remove_all(root);
    fs::create_directories(root / "boards");
    fs::create_directories(root / ".openpnp2");
    for (const char* f : { "b.xml", "a.XML", "notes.txt", ".hidden.xml" }) std::ofstream(root / f) << "x";

    using V = std::vector<std::string>;
    assert(names(JDirectoryListing::list(root, {}, false)) == (V{ "boards/", "a.XML", "b.xml", "notes.txt" }));
    assert(names(JDirectoryListing::list(root, { "xml" }, false)) == (V{ "boards/", "a.XML", "b.xml" }));
    assert(names(JDirectoryListing::list(root, { "xml" }, true))
           == (V{ ".openpnp2/", "boards/", ".hidden.xml", "a.XML", "b.xml" }));

    assert(JDirectoryListing::isHidden(".openpnp2") && !JDirectoryListing::isHidden("boards"));
    assert(!JDirectoryListing::passesFilter("Makefile", { "xml" }));
    assert(JDirectoryListing::passesFilter("Board.JOB.xml", { "job.xml" }) && !JDirectoryListing::passesFilter("board.xml", { "job.xml" }));
    assert(JDirectoryListing::list(root / "missing", {}, true).empty());

    // Sizes and times, for the picker's Size and Modified columns.
    for (const auto& e : JDirectoryListing::list(root, { "txt" }, false))
        if (e.name == "notes.txt") assert(e.size == 1 && e.modified != fs::file_time_type{});

    // A typed path made whole: from the folder shown, "~" the home folder, dots folded.
    assert(JDirectoryListing::resolve("boards/x.xml", root) == (root / "boards" / "x.xml"));
    assert(JDirectoryListing::resolve("../y", root / "boards") == (root / "y"));
    assert(JDirectoryListing::resolve("/tmp/z", root) == fs::path("/tmp/z"));
    assert(JDirectoryListing::resolve("~", root) == JDirectoryListing::homeFolder().lexically_normal());
    assert(JDirectoryListing::resolve("~/a", root) == (JDirectoryListing::homeFolder() / "a").lexically_normal());

    fs::remove_all(root);
    return 0;
}
