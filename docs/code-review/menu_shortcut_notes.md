# Menu + Shortcut Review Notes

Scope: MenuBuilder, ShortcutBinder.

## Findings

1) Debug menu assumes QML root object exists
- MenuBuilder::buildDebugMenu dereferences `qquickView()->rootObject()` without a null check.
- Risk: crash if QML failed to load or root object is null.
- Fix: guard rootObject() before findChild.
- Files: libs/gui/mainwindow/MenuBuilder.cpp

2) F2 shortcut is a no-op
- ShortcutBinder registers F2 but the handler is empty.
- Risk: dead shortcut; confusing UX and debug noise.
- Fix: remove it or wire to an intended dock.
- Files: libs/gui/mainwindow/ShortcutBinder.cpp
