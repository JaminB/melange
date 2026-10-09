# Changelog

## 0.8.0

- New `wum.input` Lua API: `groups()`, `binding(message)`, `setOptions{...}` and `options()`. Mods can set the
  camera and aim Y-invert ("game", "standard" or "inverted"), apply the camera invert in the Blimp view (which the game
  skips), and scale camera and aim mouse sensitivity (0.25 to 3.0). Options are cleared when the mod unloads. They only
  change the numbers the game puts into its own mouse messages on the sending side, so they cannot affect lockstep
  netplay or Wormsign recordings.
- New `Controls` module (`[Controls] Enabled`, `SmoothMouse`, `Probe`; build #1077): `SmoothMouse` (on by default)
  reads the mouse with Raw Input and feeds the game's own mouse code, removing the aiming and camera jitter caused by
  the per-frame cursor re-centre.
- `wum.ui.hotkey` now also accepts a bare function key (`"F1"` to `"F12"`) without Ctrl, Shift or Alt.
