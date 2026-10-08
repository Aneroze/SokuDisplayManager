# Default-value recalibration (2026-10-08)

Earlier by-eye tuning in exclusive fullscreen was skewed by an AMD Adrenalin th123 profile (forced 4x SSAA +
Radeon Image Sharpening 80%), removed 2026-10-07. Values re-picked sighted, live with the hotkeys, exclusive
fullscreen on a 27" 2560x1440 monitor, Copy install, starting from neutral values (Sharp 1.00, xBR 1.00 / 1.0,
Sprites / BG 1.0).

| Setting | x2.25 (CustomResolution 1440x1080) | x3 (FitToScreen) | x2 | Previous default |
| --- | --- | --- | --- | --- |
| `Sharpness` | 4.00 | 2.50 | 2.00 | 1.50 |
| `XbrStrength` | 0.50 | 0.80 | 0.65 (previous) | 0.65 |
| `XbrCorner` | C | C | B (previous) | B |
| `XbrSlopes` | 0 | 0 | 0 (previous) | 0 |
| `XbrWidth` | 0.50 | 0.75 | 2.0 (previous) | 2.0 |
| `BackgroundSharpness` | 4.00 | 2.50 | 1.75 | 1.5 (suggested) |
| `BackgroundRestStrength` | 0.50 (untouched) | 0.30 | 0.30 | 0.5 |
| `SpriteSharpness` | 1.50 | 1.75 | 1.50 | 2.5 (suggested) |
| `SpriteNearStrength` (zoom 1.0) | 0.00 | 0.00 | 0.00 (untouched) | (`SpriteRestStrength` 0) |
| `SpriteFarStrength` | 1.00 | 1.00 | 1.00 (untouched) | (new) |
| `SpriteFarZoom` | 0.65 | 0.70 | 0.70 | (new) |

x2.25 values are the state at the end of each session (logs: 2026-10-08-x2.25.log; sprites, tuned with the screen
filter at Sharp 4.00 and the new camera-zoom ramp: 2026-10-08-x2.25-sprites.log). Sprite values are the user's
"golden" x2.25 values.

x3 (FitToScreen on 2560x1440): all values are the user's golden x3 values, the state at the end of the session
(log: 2026-10-08-x3.log), starting from the same neutral values.

x2 (IntegerScaling x2): golden values from the x2 session (log: 2026-10-08-x2.log), with the near-whole-scale fade
for the characters in. xBR was not tuned at x2: the previous defaults are kept there.
