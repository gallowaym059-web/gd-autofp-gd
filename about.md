# Manual Frame Assist

Spam-assist for fast manual clicking. It does **not** click for you.

## The problem
When you spam fast, a press and its release (or a release and your next press) can land between two physics steps. The player never sees that state, so the click is silently lost. That is why spam feels random in wave, ship, UFO, etc.

## What it does
Once your real CPS passes the **Activation CPS** setting, every press and every release is kept visible to at least one physics step, in the exact order you made them. Below that CPS, input passes straight through with zero added delay.

- Works on every gamemode (shared input path), both players, platformer buttons.
- No extra clicks, no gamemode-specific physics edits.
- State resets on respawn, restart and quit.
