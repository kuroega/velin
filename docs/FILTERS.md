# Audio and video filters

Open **Video > Manage video filters** or **Audio > Manage audio filters** to
inspect and change the current mpv filter chain. Each row is an mpv filter
object in JSON form. The Add and Edit dialogs accept objects such as:

```json
{"name": "hflip"}
```

An audio filter with named options can be entered as:

```json
{"name": "volume", "params": {"volume": "0.8"}}
```

Filter names and parameters depend on the bundled mpv build. Velin displays
mpv's error if a change cannot be applied. Canceling an edit leaves the chain
unchanged. Changes to the active chain take effect immediately and are not
automatically restored when the application restarts.

Select an active filter and choose **Save as preset** to save its definition.
Select a saved preset and choose **Toggle** to add or remove that preset's
filter from the active chain. Editing a preset changes the saved definition;
disable and enable an active preset to apply the edited version. Deleting a
preset leaves any current active filter in place. Presets are saved in
`filter-presets.json` in the application data directory and survive restarts.
The audio and video managers share this file while showing their own presets.

The manager uses mpv's native node representation and is intended for users
familiar with mpv filter names and options. It does not provide a graphical
parameter editor or automatically migrate raw filter settings into presets.
