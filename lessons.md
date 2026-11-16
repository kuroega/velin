# Lessons

Bugs worth remembering: what happened, why, and how to avoid it next time.

## iOS seek bar froze and never seeked (2026-11-16)

**Symptom.** On iOS and iPadOS, dragging the seek bar moved the thumb and the
elapsed time to the new spot, then both froze there. The video never seeked;
it kept playing from where it was. Windows was fine, and resume playback
still worked.

**Cause.** Commit `4bf6d41` (power-use cut) moved the slider into its own
`SeekBar` view in `ios/VelinPlayer/PlayerView.swift`, and its binding getter
returned a value captured when the body was drawn:

```swift
let position = scrubbing ?? clock.position
Slider(value: Binding(get: { position }, set: { scrubbing = $0; ... })) { editing in
    if !editing, let target = scrubbing { onCommit(target); scrubbing = nil }
}
```

After each `set`, the slider read back the old value. In that state SwiftUI's
`Slider` calls `onEditingChanged(true)` but never `onEditingChanged(false)`, so
the seek in the end-of-drag handler never ran and `scrubbing` stayed set. The
label kept showing the stuck target. Seen on the iOS 27.2 simulator; the user
saw it on iOS and iPadOS 17.

**Fix.** The getter reads the live state: `get: { scrubbing ?? clock.position }`.
The time labels can still use the captured value.

**Lessons.**
- A `Binding(get:set:)` getter must return what `set` just stored. Never return
  a value captured in `body`; read the source of truth.
- A refactor of UI code for performance needs a check of the interaction it
  touches (here: drag the seek bar and release), not just a build.
- To debug SwiftUI control callbacks in the simulator, log them and launch
  with `SIMCTL_CHILD_NSUnbufferedIO=YES xcrun simctl launch --console-pty ...`.
  Plain `--stdout=` redirects stayed empty.
- When driving the player with simulator tools, the controls auto-hide after
  3 s, often before an injected drag arrives; the drag then lands on the swipe
  gesture instead of the slider. Stretch the hide delay temporarily while
  testing.
