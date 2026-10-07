# Plan: start the game window with a Latin input method (IME) — implemented

Status: **implemented 2026-10-06** as `[Input] StartInLatinInput` (default 0), tested with Microsoft Pinyin
(docs/TEST-CHECKLIST.md). Findings while testing:
- The reported start state reproduces: with the shared input method on Pinyin, th123 starts in Chinese mode.
- But game keys are NOT hidden from the game: in that state its DirectInput keyboard buffer (0x8A01B8) still saw
  injected scancode presses (windowed), and the window kept the foreground.
- The actual symptom (hands-on test by the user, 2026-10-06, own keybinds, windowed): the game plays fine, but Pinyin
  keeps composing from the game's letter keys, so its composition / candidate box keeps popping up over the game.
  Distracting, not input-blocking. Exclusive fullscreen: to check (a pop-up could steal focus and minimize it).
- Activation messages (WM_ACTIVATE / WM_ACTIVATEAPP / WM_SETFOCUS) never reach DM's wndProc with this mod set
  (NoFocusNoBgm suspected), so DM waits for the foreground with a 200 ms thread timer instead.
- ImmIsIME is useless under TSF (true for every layout); Latin layouts are picked by language instead.
- Full matrix with Microsoft's Chinese / Japanese / Korean IMEs: docs/TEST-CHECKLIST.md. The no-Latin fallback is
  language-specific: Chinese / Japanese = turn the IME off (Pinyin ignores a cleared native bit), Korean = clear the
  native (Hangul) bit (it keeps composing when turned off). Japanese and Korean IMEs start off / in English mode, so
  DM only acts on them if they start on in native mode.

## Request (鱼的千千 / Fishuwako, SokuShaderPro dev)
CJK players usually have two input methods (English + Chinese). th123's window always starts with the Chinese
IME active, which blocks normal game input until the player switches to English by hand. They want the game window
to *start* on a Latin keyboard. It must NOT disable the IME: lobby players switch back to Chinese at any time to
chat. (He tried disabling the IME in WindowResizer+ and had to roll it back because players couldn't type; his
"switch to a Latin layout" attempt there breaks when no Latin layout is installed.)

## Proposed behavior (once, when the game window first comes up; never again afterwards)
1. Active input method is an IME (`ImmIsIME(GetKeyboardLayout(windowThread))`) and a Latin / non-IME layout is
   installed (`GetKeyboardLayoutList`, prefer en-US 0x0409): switch the window to it (`ActivateKeyboardLayout`).
2. No Latin layout installed: keep the IME but put it in its English / alphanumeric mode (IME closed via
   `ImmSetOpenStatus(ImmGetContext(hwnd), FALSE)`, or `ImmSetConversionStatus` with `IME_CMODE_ALPHANUMERIC`) - the
   state Shift toggles in Microsoft Pinyin. Keys pass through; the player can switch back to Chinese any time.
3. Active input method is not an IME (all non-CJK players): do nothing.

Option: `[Input] StartInLatinInput=1`, next to `AllowWinKey`. Could default to on, since it's a no-op without an IME.
Hook point: the window thread, after the first-spawn window setup (applyWindowState / installWndProc path).

## Caveats / open questions
- With Windows' default single input method for all apps ("Let me use a different input method for each app window"
  off), switching the game window's layout may switch it system-wide. Same as switching by hand, but tell players.
- Step 2 depends on the IME honoring the IMM32 calls: Microsoft Pinyin (TSF) should; Sogou and other third-party
  IMEs need testing.
- th123 reads game keys via DirectInput, which usually sees raw keys even with an IME on - ask exactly how the IME
  "blocks input" (keys ignored? candidate box? Shift toggling?), which IME, and the per-app input setting.
- Decide: DM `[Input]` option vs. a small standalone mod; default on vs. opt-in.

## Testing
Needs a CJK IME: either add Microsoft Pinyin to the dev machine's input settings (system change, removable) or have
Fishuwako test a build.
