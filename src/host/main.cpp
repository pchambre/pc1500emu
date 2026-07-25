#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <utility>
#include <vector>

#include "bus.h"
#include "keyboard.h"
#include "lcd.h"
#include "lh5801.h"

namespace {

constexpr int kScale = 6;         // pixels per dot
constexpr int kMarginTop = 20;
constexpr int kMarginBottom = 20;
constexpr int kMarginLeft = 3 * kScale;
constexpr int kMarginRight = 3 * kScale;
constexpr int kWindowW = pc1500::Lcd::kColumns * kScale + kMarginLeft + kMarginRight;

// Fixed-segment status indicator row, shown above the dot matrix (see
// docs/pc1500_hardware_reference.md -- bytes 764EH/764FH, not part of the
// 156x7 graphic display). Laid out left to right per real-hardware
// confirmation from Paul.
constexpr int kIndicatorBarHeight = 18;
constexpr int kIndicatorFontPtSize = 11;
// A .ttc font collection; index picks a language face, but katakana glyphs
// don't differ meaningfully between the CJK variants for our purposes.
constexpr const char* kIndicatorFontPath = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
constexpr int kIndicatorFontFaceIndex = 0;

constexpr int kWindowH =
    pc1500::Lcd::kRows * kScale + kMarginTop + kMarginBottom + kIndicatorBarHeight;

// The fixed set of strings the indicator row can ever show -- rendered
// once into cached textures at startup rather than every frame.
constexpr const char* kBusy = "BUSY";
constexpr const char* kShift = "SHIFT";
constexpr const char* kKatakana = "\xE3\x82\xAB\xE3\x82\xBF";  // katakana ka-ta
constexpr const char* kSmall = "SMALL";
constexpr const char* kDeg = "DEG";
constexpr const char* kRad = "RAD";
constexpr const char* kGrad = "GRAD";
constexpr const char* kRun = "RUN";
constexpr const char* kPro = "PRO";
constexpr const char* kReserve = "RESERVE";
constexpr const char* kDef = "DEF";
constexpr const char* kOne = "I";
constexpr const char* kTwo = "II";
constexpr const char* kThree = "III";
constexpr const char* kAllIndicatorTexts[] = {
    kBusy, kShift, kKatakana, kSmall, kDeg, kRad, kGrad, kRun, kPro, kReserve, kDef, kOne, kTwo, kThree,
};
constexpr int kIndicatorSlotCount = 12;  // left-to-right positions on the real display

// Approximate: 2.6MHz crystal / 2 = 1.3MHz internal machine cycle (manual
// section 4-2-1). Not cycle-accurate frame pacing, just a reasonable bring
// -up budget so the emulator runs at roughly the real machine's speed.
constexpr int kCyclesPerSecond = 1300000;
constexpr int kFramesPerSecond = 60;
constexpr int kCyclesPerFrame = kCyclesPerSecond / kFramesPerSecond;

// Timer tick rate: approximated as machine-cycle/64, by analogy with the
// PC-2's documented crystal/128 divider (see lh5801::CPU::tickTimer's
// comment) -- not confirmed for the PC-1500's own divider depth.
constexpr int kCyclesPerTimerTick = 64;

// SDL keycode -> PC-1500 matrix key. Host physical key state maps directly
// to PC-1500 physical key state (ignoring host shift/modifiers) since the
// PC-1500's own Shift/Sml keys are themselves ordinary matrix keys --
// interpreting Shift+X combinations is the ROM's job, not ours. Where
// there's no natural 1:1 host key, a reasonable nearby substitute is
// picked (documented inline).
struct KeyMapping {
  SDL_Keycode keycode;
  pc1500::Key key;
};

// clang-format off
constexpr KeyMapping kKeyMap[] = {
    {SDLK_0, pc1500::Key::Digit0}, {SDLK_1, pc1500::Key::Digit1},
    {SDLK_2, pc1500::Key::Digit2}, {SDLK_3, pc1500::Key::Digit3},
    {SDLK_4, pc1500::Key::Digit4}, {SDLK_5, pc1500::Key::Digit5},
    {SDLK_6, pc1500::Key::Digit6}, {SDLK_7, pc1500::Key::Digit7},
    {SDLK_8, pc1500::Key::Digit8}, {SDLK_9, pc1500::Key::Digit9},
    {SDLK_KP_0, pc1500::Key::Digit0}, {SDLK_KP_1, pc1500::Key::Digit1},
    {SDLK_KP_2, pc1500::Key::Digit2}, {SDLK_KP_3, pc1500::Key::Digit3},
    {SDLK_KP_4, pc1500::Key::Digit4}, {SDLK_KP_5, pc1500::Key::Digit5},
    {SDLK_KP_6, pc1500::Key::Digit6}, {SDLK_KP_7, pc1500::Key::Digit7},
    {SDLK_KP_8, pc1500::Key::Digit8}, {SDLK_KP_9, pc1500::Key::Digit9},
    {SDLK_a, pc1500::Key::A}, {SDLK_b, pc1500::Key::B}, {SDLK_c, pc1500::Key::C},
    {SDLK_d, pc1500::Key::D}, {SDLK_e, pc1500::Key::E}, {SDLK_f, pc1500::Key::F},
    {SDLK_g, pc1500::Key::G}, {SDLK_h, pc1500::Key::H}, {SDLK_i, pc1500::Key::I},
    {SDLK_j, pc1500::Key::J}, {SDLK_k, pc1500::Key::K}, {SDLK_l, pc1500::Key::L},
    {SDLK_m, pc1500::Key::M}, {SDLK_n, pc1500::Key::N}, {SDLK_o, pc1500::Key::O},
    {SDLK_p, pc1500::Key::P}, {SDLK_q, pc1500::Key::Q}, {SDLK_r, pc1500::Key::R},
    {SDLK_s, pc1500::Key::S}, {SDLK_t, pc1500::Key::T}, {SDLK_u, pc1500::Key::U},
    {SDLK_v, pc1500::Key::V}, {SDLK_w, pc1500::Key::W}, {SDLK_x, pc1500::Key::X},
    {SDLK_y, pc1500::Key::Y}, {SDLK_z, pc1500::Key::Z},
    {SDLK_MINUS, pc1500::Key::Minus}, {SDLK_EQUALS, pc1500::Key::Equals},
    {SDLK_SLASH, pc1500::Key::Slash}, {SDLK_PERIOD, pc1500::Key::Period},
    {SDLK_LEFTBRACKET, pc1500::Key::LeftParen},   // nearest substitute for (
    {SDLK_RIGHTBRACKET, pc1500::Key::RightParen}, // nearest substitute for )
    {SDLK_KP_PLUS, pc1500::Key::Plus},
    {SDLK_KP_MULTIPLY, pc1500::Key::Asterisk},
    {SDLK_LEFT, pc1500::Key::Left}, {SDLK_RIGHT, pc1500::Key::Right},
    {SDLK_UP, pc1500::Key::Up}, {SDLK_DOWN, pc1500::Key::Down},
    // Deliberately no {SDLK_LSHIFT, ...} or {SDLK_RSHIFT, ...} entry --
    // both host Shift keys behave identically, as pure modifiers, never
    // mapped directly to any PC-1500 key. Holding PC-1500 Shift
    // continuously for the whole time a host Shift is down means the
    // ROM's key-scan sees "Shift alone" as its own key event *before*
    // whatever other key is being combined with it -- and, just like
    // the MODE/CL debounce we traced earlier, that latches a ~0.2s "a
    // key was just processed" window that swallows the very next key
    // event (the actual target key), which arrives well within normal
    // Shift+key typing speed. So host Shift (either one) is read only
    // as a modifier (event.key.keysym.mod) and only ever engages
    // PC-1500 Shift atomically together with a specific target key, via
    // kSymbolMap below. For cases kSymbolMap doesn't cover, Tab sends a
    // direct, standalone PC-1500 Shift keypress (real hardware's own
    // Shift is a tap-to-toggle key, not a hold, so this matches that
    // directly rather than needing the tap-sequence machinery below).
    {SDLK_TAB, pc1500::Key::Shift},
    {SDLK_BACKSPACE, pc1500::Key::Left},           // duplicates the left arrowhead
    {SDLK_RETURN, pc1500::Key::Ent},               // the *small* Ent key -- the
                                                    // main ENTER key's matrix
                                                    // position was never
                                                    // located (see hardware
                                                    // reference doc)
    {SDLK_SPACE, pc1500::Key::Space},
    {SDLK_F1, pc1500::Key::F1}, {SDLK_F2, pc1500::Key::F2},
    {SDLK_F3, pc1500::Key::F3}, {SDLK_F4, pc1500::Key::F4},
    {SDLK_F5, pc1500::Key::F5}, {SDLK_F6, pc1500::Key::F6},
    {SDLK_F7, pc1500::Key::Cl}, {SDLK_F8, pc1500::Key::Mode},
    {SDLK_F9, pc1500::Key::Def}, {SDLK_F11, pc1500::Key::Rcl},
};
// clang-format on

// F10 and F12 need host-modifier awareness (Shift/Ctrl), so they're
// handled specially rather than through the plain kKeyMap table:
//   F10          -> Sml
//   Shift+F10    -> the up/down rocker key
//   F12          -> On (not part of the 8x8 matrix -- wired straight to
//                  the CPU's BFI pin, so it's handled separately from
//                  every other key, same as on real hardware)
//   Shift+F12    -> Off
//   Ctrl+F12     -> host-only convenience key mimicking the real
//                  machine's recessed ALL RESET pinhole switch (also not
//                  part of the matrix) -- re-runs CPU::reset() without
//                  otherwise touching RAM.

// Punctuation/symbol passthrough: on the real PC-1500, typing these
// requires PC-1500 Shift with a specific matrix key (confirmed by Paul
// on real hardware) -- e.g. Shift+F6 always types '&' regardless of
// whether F6 has been separately programmed as a command shortcut, same
// as Shift+/ always types '?'. Pass Shift+F6 as PC-1500 Shift+F6 and
// plain F6 as plain F6, and let the ROM decide what to do -- don't try
// to be clever about current RUN/PRO mode here.
//
// Crucially, PC-1500 Shift is a *toggle*, not a hold: on real hardware
// you tap Shift (press and release), which lights the Shift indicator
// indefinitely (no timeout) until Shift or any other key is pressed,
// and that next keypress is shifted. Shift is never held down
// simultaneously with another key. Simulating a hold instead (as a
// modern keyboard's Shift works) breaks things: the ROM's key-scan
// would see "Shift alone" as its own key event the instant host Shift
// goes down, *before* the target key arrives, which latches the same
// ~0.2s "a key was just processed" debounce window we traced for
// MODE/CL -- and that window is well within normal Shift+key typing
// speed, so it swallows the target key's press entirely.
//
// So each symbol is fired as a queued, fire-and-forget sequence of
// PC-1500 key actions (see QueuedKeyAction/kSymbolActionQueue below),
// entirely decoupled from how long the *host* key is actually held:
//   1. Press PC-1500 Shift, hold for kTapFrames (long enough for one
//      ROM scan cycle to see it as its own event).
//   2. Release Shift, then wait *idle* (nothing pressed) for
//      kIdleFrames -- confirmed empirically that this is what actually
//      matters: the debounce counter only advances while nothing is
//      pressed, so it's the gap *after* release that has to clear the
//      ~0.2s (~260,000-cycle) window, not the hold duration. (Real
//      hardware needs no such delay at all -- Shift stays lit
//      indefinitely -- so this points at a real, still-unresolved bug
//      in our own debounce logic; this queue is a working workaround,
//      not a fix for that deeper issue.)
//   3. Press the target key, hold for kTapFrames, then release.
// Firing this as a queue (rather than tracking per-key held/released
// state) also fixes an earlier, more serious bug: since a normal human
// keypress is much shorter than the ~0.3s this sequence takes, tying it
// to "is the host key still held" meant the sequence was almost always
// cancelled partway through, before the target key was ever pressed --
// which is why symbols "mostly didn't work" before. Once queued, a
// press is committed and runs to completion regardless of when the
// host key is released; OS key-repeat while a host key is held is
// filtered out (event.key.repeat) so a long hold doesn't flood the
// queue with duplicate sequences.
//
// Each entry fires when the *host* types the target symbol (typically
// host Shift+key on a US layout). Most keys fall through to kKeyMap
// when host Shift isn't held (e.g. plain "1" -> Digit1); SDLK_COMMA and
// SDLK_SEMICOLON are the exception -- they type a *different* symbol
// unshifted (also via a PC-1500 Shift tap, just a different base key)
// rather than falling through, since the PC-1500 has no plain "," or
// ";" key of its own.
constexpr int kTapFrames = 4;    // ~67ms -- spans the ROM's ~25ms scan cadence
constexpr int kIdleFrames = 15;  // ~250ms -- clears the ~0.2s debounce window
struct SymbolMapping {
  SDL_Keycode keycode;
  pc1500::Key shiftedTarget;     // host Shift+keycode -> PC-1500 Shift-tap then this
  bool hasUnshiftedTarget;       // if true, plain keycode also -> PC-1500 Shift-tap then unshiftedTarget
  pc1500::Key unshiftedTarget;   // (only meaningful if hasUnshiftedTarget)
};
// clang-format off
constexpr SymbolMapping kSymbolMap[] = {
    {SDLK_1, pc1500::Key::F1, false, {}},              // !
    {SDLK_QUOTE, pc1500::Key::F2, false, {}},           // " (US layout: Shift+')
    {SDLK_QUOTEDBL, pc1500::Key::F2, true, pc1500::Key::F2},  // " (some layouts/setups
                                                               // report this keycode
                                                               // directly instead of
                                                               // SDLK_QUOTE+Shift)
    {SDLK_3, pc1500::Key::F3, false, {}},               // #
    {SDLK_4, pc1500::Key::F4, false, {}},               // $
    {SDLK_5, pc1500::Key::F5, false, {}},               // %
    {SDLK_7, pc1500::Key::F6, false, {}},               // &
    {SDLK_2, pc1500::Key::Equals, false, {}},           // @
    {SDLK_6, pc1500::Key::Space, false, {}},            // ^
    {SDLK_PERIOD, pc1500::Key::RightParen, false, {}},  // >
    {SDLK_SLASH, pc1500::Key::Slash, false, {}},        // ?
    {SDLK_COMMA, pc1500::Key::LeftParen, true, pc1500::Key::Minus},    // < (shifted) / , (plain)
    {SDLK_SEMICOLON, pc1500::Key::Asterisk, true, pc1500::Key::Plus}, // : (shifted) / ; (plain)
    // Insert/Delete have only one meaning regardless of host Shift (no
    // plain-key gesture exists for either), so both targets are the same
    // key -- see the comment where hasTarget is computed for why.
    {SDLK_INSERT, pc1500::Key::Right, true, pc1500::Key::Right},  // Insert -> Shift+Right
    {SDLK_DELETE, pc1500::Key::Left, true, pc1500::Key::Left},    // Delete -> Shift+Left
};
// clang-format on

// One step of a queued symbol-tap sequence (see kSymbolMap above): set a
// PC-1500 key's state, then wait `framesToWait` real frames before the
// next queued action runs. Processed one action at a time in the main
// loop, independent of host key state -- see the "fire-and-forget"
// rationale in the kSymbolMap comment.
struct QueuedKeyAction {
  pc1500::Key key;
  bool pressed;
  int framesToWait;
};

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  pc1500::Keyboard keyboard;
  pc1500::Bus bus(keyboard);
  lh5801::CPU cpu(bus);
  pc1500::Lcd lcd;

  if (argc > 1) {
    std::vector<uint8_t> rom = readFile(argv[1]);
    if (rom.empty()) {
      std::fprintf(stderr, "pc1500emu: could not read ROM file '%s'\n", argv[1]);
      return 1;
    }
    bus.loadME0(0xC000, rom.data(), rom.size());
    std::printf("pc1500emu: loaded %zu-byte ROM from %s\n", rom.size(), argv[1]);
  } else {
    std::printf(
        "pc1500emu: no ROM given (usage: %s <rom.bin>) -- running with "
        "ROM area 0xFF-filled\n",
        argv[0]);
  }
  cpu.reset();

  // Known limitation: HLT sets a one-way halted flag. Real hardware wakes
  // from HLT via interrupt, but this core doesn't implement interrupt
  // delivery yet (only the SIE/RIE/IE flag bookkeeping), so a HLT'd
  // program will not resume in this emulator.
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "pc1500emu: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "pc1500emu: TTF_Init failed: %s\n", TTF_GetError());
    return 1;
  }
  TTF_Font* indicatorFont = TTF_OpenFontIndex(kIndicatorFontPath, kIndicatorFontPtSize,
                                               kIndicatorFontFaceIndex);
  if (!indicatorFont) {
    std::fprintf(stderr, "pc1500emu: could not load indicator font '%s': %s\n",
                 kIndicatorFontPath, TTF_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("pc1500emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        kWindowW, kWindowH, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  // Pre-render every indicator label once; each frame just picks which
  // (if any) cached texture to blit per slot, based on the live 764EH/
  // 764FH bits -- matches how the dot matrix itself is drawn (fixed
  // "lit" color, no per-frame text shaping cost).
  SDL_Color indicatorColor{0x20, 0x28, 0x18, 255};  // same "lit" tone as the dot matrix
  std::vector<std::pair<const char*, SDL_Texture*>> indicatorTextures;
  for (const char* text : kAllIndicatorTexts) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(indicatorFont, text, indicatorColor);
    if (!surf) {
      std::fprintf(stderr, "pc1500emu: TTF_RenderUTF8_Blended('%s') failed: %s\n", text,
                   TTF_GetError());
      return 1;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    indicatorTextures.emplace_back(text, tex);
  }
  auto indicatorTexture = [&](const char* text) -> SDL_Texture* {
    for (auto& [t, tex] : indicatorTextures) {
      if (t == text) return tex;  // pointer equality is fine -- all callers pass the kXxx constants
    }
    return nullptr;
  };

  auto readByte = [&](uint16_t addr) { return bus.readME0(addr); };

  bool running = true;
  int cyclesSinceTimerTick = 0;
  // Tracks which action F10/F12 actually triggered on press, so release
  // matches it even if Shift/Ctrl changed state while the key was held
  // (otherwise a modifier change mid-hold could release the wrong
  // PC-1500 key and leave the other one stuck "pressed" forever).
  bool f10IsRocker = false;
  // Pending symbol-tap sequences (see kSymbolMap/QueuedKeyAction above),
  // processed one action at a time regardless of host key state.
  std::deque<QueuedKeyAction> symbolActionQueue;
  int symbolActionFramesRemaining = 0;
  // Per kSymbolMap entry (by index): whether the *last* press of that
  // host key fell through to kKeyMap (no PC-1500-Shift meaning at the
  // time) rather than being symbol-queued, so release matches whichever
  // actually happened even if Shift's state changed in between.
  std::array<bool, std::size(kSymbolMap)> engagedViaKeyMap{};
  // Temporary: logs every real key event with a precise timestamp, so a
  // captured typing sample can be replayed against the C++ core exactly
  // as typed (see /tmp/pc1500emu_keycapture.log).
  std::ofstream keyCaptureLog("/tmp/pc1500emu_keycapture.log", std::ios::app);
  auto captureStart = std::chrono::steady_clock::now();
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        bool pressed = (event.type == SDL_KEYDOWN);
        SDL_Keycode kc = event.key.keysym.sym;
        Uint16 mod = event.key.keysym.mod;
        bool shiftHeld = (mod & KMOD_SHIFT) != 0;
        bool ctrlHeld = (mod & KMOD_CTRL) != 0;
        {
          auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - captureStart)
                               .count();
          keyCaptureLog << elapsedUs << ',' << kc << ',' << (pressed ? 1 : 0) << ','
                        << (event.key.repeat ? 1 : 0) << ',' << mod << '\n';
          keyCaptureLog.flush();
        }
        if (kc == SDLK_F12) {
          if (pressed && ctrlHeld) {
            cpu.reset();
          } else if (pressed && shiftHeld) {
            bus.setKeyState(pc1500::Key::Off, true);
          } else if (pressed) {
            cpu.pressOnKey();
            bus.ioPort().setOnKeyLine(true);
          } else {
            // Unconditionally release both -- whichever was actually
            // pressed gets released; releasing the other is a no-op.
            bus.setKeyState(pc1500::Key::Off, false);
            bus.ioPort().setOnKeyLine(false);
          }
        } else if (kc == SDLK_F10) {
          if (pressed) f10IsRocker = shiftHeld;
          bus.setKeyState(f10IsRocker ? pc1500::Key::UpDownRocker : pc1500::Key::Sml, pressed);
        } else {
          bool handledBySymbolMap = false;
          for (size_t i = 0; i < std::size(kSymbolMap); i++) {
            const SymbolMapping& sm = kSymbolMap[i];
            if (sm.keycode != kc) continue;
            if (pressed) {
              if (event.key.repeat) {
                // Don't flood the queue with duplicate sequences while
                // the host key is held past the OS's key-repeat delay.
                handledBySymbolMap = true;
                break;
              }
              // Some of these keycodes are *shared*: on QWERTY, SDLK_1 is
              // plain "1" alone, or "!" when host Shift is also held --
              // shiftHeld must gate the choice, and plain "1" needs to
              // fall through to kKeyMap (there's no PC-1500-Shift meaning
              // for it at all). Others (Insert/Delete, or a layout like
              // AZERTY reporting SDLK_QUOTEDBL directly for ") have only
              // one meaning regardless of host Shift -- those are encoded
              // by giving hasUnshiftedTarget=true with unshiftedTarget set
              // to the *same* key as shiftedTarget, so they always queue
              // the tap sequence without ever depending on shiftHeld.
              bool hasTarget = true;
              pc1500::Key target = pc1500::Key::Space;
              if (shiftHeld) {
                target = sm.shiftedTarget;
              } else if (sm.hasUnshiftedTarget) {
                target = sm.unshiftedTarget;
              } else {
                hasTarget = false;
              }
              if (hasTarget) {
                // Fire-and-forget: queue the whole tap sequence now: it
                // runs to completion regardless of how long the host
                // key ends up being held, or when it's released.
                symbolActionQueue.push_back({pc1500::Key::Shift, true, kTapFrames});
                symbolActionQueue.push_back({pc1500::Key::Shift, false, kIdleFrames});
                symbolActionQueue.push_back({target, true, kTapFrames});
                symbolActionQueue.push_back({target, false, 0});
                engagedViaKeyMap[i] = false;
                handledBySymbolMap = true;
              } else {
                // No PC-1500-Shift meaning right now (e.g. plain "1")
                // -- falls through to kKeyMap, remembered so release
                // matches even if Shift's state changes meanwhile.
                engagedViaKeyMap[i] = true;
              }
            } else if (!engagedViaKeyMap[i]) {
              // This host key's last press was symbol-queued, not
              // passed through to kKeyMap -- the queue above already
              // handles releasing the target key on its own schedule,
              // so there's nothing to do here.
              handledBySymbolMap = true;
            }
            // else: last press fell through to kKeyMap, so let release
            // fall through too (handledBySymbolMap stays false).
            break;
          }
          if (!handledBySymbolMap) {
            for (const KeyMapping& m : kKeyMap) {
              if (m.keycode == kc) {
                bus.setKeyState(m.key, pressed);
                break;
              }
            }
          }
        }
      }
    }

    // Process one step of the pending symbol-tap queue per frame,
    // independent of host key state (see kSymbolMap/QueuedKeyAction).
    if (!symbolActionQueue.empty() && --symbolActionFramesRemaining <= 0) {
      QueuedKeyAction action = symbolActionQueue.front();
      symbolActionQueue.pop_front();
      bus.setKeyState(action.key, action.pressed);
      symbolActionFramesRemaining = action.framesToWait;
    }

    // step() checks for pending interrupts even while halted (and clears
    // halted_ if one dispatches), so keep calling it either way rather
    // than skipping entirely -- that's the only way HLT can ever resume.
    // Deliberately no early-exit on cpu.halted(): breaking out here would
    // stop the timer from advancing too, so a halted CPU would only ever
    // see its timer interrupt move forward by one cycle per rendered
    // frame (~9 minutes of real time to accumulate one interrupt period,
    // instead of ~25ms) -- exactly the kind of thing that makes keyboard
    // input look almost entirely unresponsive.
    int cyclesRun = 0;
    while (cyclesRun < kCyclesPerFrame) {
      int c = cpu.step();
      int used = (c > 0) ? c : 1;
      cyclesRun += used;
      cyclesSinceTimerTick += used;
      bus.advanceCycles(used);
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
    }

    SDL_SetRenderDrawColor(renderer, 0xC8, 0xD8, 0xC0, 255);  // unlit LCD background
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 0x20, 0x28, 0x18, 255);  // lit dot
    for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        if (lcd.dot(col, row, cpu.disp(), readByte)) {
          SDL_Rect r{kMarginLeft + col * kScale, kIndicatorBarHeight + kMarginTop + row * kScale,
                     kScale - 1, kScale - 1};
          SDL_RenderFillRect(renderer, &r);
        }
      }
    }

    // Fixed-segment status indicators (764EH/764FH) -- left to right:
    // BUSY, SHIFT, katakana, SMALL, DEG/RAD/GRAD, RUN, PRO, RESERVE, DEF,
    // I, II, III.
    uint8_t ind1 = bus.readME0(0x764E);
    uint8_t ind2 = bus.readME0(0x764F);
    bool de = ind2 & 0x01, g = ind2 & 0x02, rad = ind2 & 0x04;
    const char* angleMode = nullptr;
    if (de && g) angleMode = kDeg;
    else if (g) angleMode = kGrad;
    else if (rad) angleMode = kRad;
    const char* slotText[kIndicatorSlotCount] = {
        (ind1 & 0x01) ? kBusy : nullptr,      // Busy
        (ind1 & 0x02) ? kShift : nullptr,     // Shift
        (ind1 & 0x04) ? kKatakana : nullptr,  // Japanese
        (ind1 & 0x08) ? kSmall : nullptr,     // Small
        angleMode,                            // Deg/Rad/Grad
        (ind2 & 0x40) ? kRun : nullptr,        // Run
        (ind2 & 0x20) ? kPro : nullptr,        // Pro
        (ind2 & 0x10) ? kReserve : nullptr,    // Reserve
        (ind1 & 0x80) ? kDef : nullptr,        // Def
        (ind1 & 0x40) ? kOne : nullptr,        // I
        (ind1 & 0x20) ? kTwo : nullptr,        // II
        (ind1 & 0x10) ? kThree : nullptr,      // III
    };
    int slotWidth = kWindowW / kIndicatorSlotCount;
    for (int slot = 0; slot < kIndicatorSlotCount; slot++) {
      if (!slotText[slot]) continue;
      SDL_Texture* tex = indicatorTexture(slotText[slot]);
      if (!tex) continue;
      int texW, texH;
      SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
      SDL_Rect dst{slot * slotWidth + (slotWidth - texW) / 2, (kIndicatorBarHeight - texH) / 2,
                   texW, texH};
      SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / kFramesPerSecond);
  }

  for (auto& [text, tex] : indicatorTextures) {
    (void)text;
    SDL_DestroyTexture(tex);
  }
  TTF_CloseFont(indicatorFont);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
