# Contributing to JFramework

## Before a first patch

Read `CLA.md` and add yourself to `CONTRIBUTORS.md` in the same change. Sign off
each commit:

    git commit -s

The short version of the agreement: you keep the copyright in your work, and you
grant this project a licence broad enough to relicense it. That last part is what
lets the framework continue to be offered under terms other than GPLv3, which it
loses the ability to do the first time a contribution arrives without it.

If you would rather not grant that, a bug report, a failing test or a description
of the fix is still welcome and needs no agreement.

## What the code has to look like

`CLAUDE.md` at the root is the style: one public class per header, file name
matching the class, no hardcoded visual constants, no shims or TODO placeholders,
widgets that never position themselves, no platform types in public headers, and
`JLOGC` rather than `printf`. It is worth reading before writing much — it is
specific, and a patch that ignores it will be asked to change.

## Before submitting

    cmake --build build-native --target j_platform
    cmake --build build-native && ctest --test-dir build-native

A change to a shared header affects the applications that link it; building one
of them against your change is worth the few minutes.

Say what the change does and why the previous behaviour was wrong. A commit
message that explains the reasoning is worth more than one that lists the files
touched — the diff already lists the files.
