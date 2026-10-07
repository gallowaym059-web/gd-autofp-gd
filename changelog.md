# Changelog

## 1.0.1

- Rewrote the input logic: a quick tap no longer gets its press and release applied in the same physics step (that dropped the click).
- Every press/release now stays visible for at least one physics step while spamming; order is always preserved.
- No added latency when not spamming; zero-delay pass-through below the activation CPS.
- Fixed CPS calculation so it decays after you stop clicking.
- Input state now clears on respawn, restart and quit.
- Default Activation CPS lowered to 6.
- mod.json: `incompatibilities` converted to the array format; CI actions pinned to stable `@v4`; C++ standard set to 20.

## 1.0.0

- Initial release.
