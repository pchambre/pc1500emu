#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
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
constexpr int kWindowW = pc1500::Lcd::kColumns * kScale;

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
    {SDLK_SEMICOLON, pc1500::Key::Colon},
    {SDLK_LEFTBRACKET, pc1500::Key::LeftParen},   // nearest substitute for (
    {SDLK_RIGHTBRACKET, pc1500::Key::RightParen}, // nearest substitute for )
    {SDLK_KP_PLUS, pc1500::Key::Plus},
    {SDLK_KP_MULTIPLY, pc1500::Key::Asterisk},
    {SDLK_LEFT, pc1500::Key::Left}, {SDLK_RIGHT, pc1500::Key::Right},
    {SDLK_UP, pc1500::Key::Up}, {SDLK_DOWN, pc1500::Key::Down},
    {SDLK_PAGEUP, pc1500::Key::UpDownRocker}, {SDLK_PAGEDOWN, pc1500::Key::UpDownRocker},
    {SDLK_LSHIFT, pc1500::Key::Shift},
    {SDLK_RSHIFT, pc1500::Key::Sml},               // PC-1500's second shift key
    {SDLK_TAB, pc1500::Key::Mode},
    {SDLK_BACKSPACE, pc1500::Key::Def},
    {SDLK_END, pc1500::Key::Off},
    {SDLK_DELETE, pc1500::Key::Cl},
    {SDLK_INSERT, pc1500::Key::Rcl},
    {SDLK_RETURN, pc1500::Key::Ent},               // the *small* Ent key -- the
                                                    // main ENTER key's matrix
                                                    // position was never
                                                    // located (see hardware
                                                    // reference doc)
    {SDLK_SPACE, pc1500::Key::Space},
    {SDLK_F1, pc1500::Key::F1}, {SDLK_F2, pc1500::Key::F2},
    {SDLK_F3, pc1500::Key::F3}, {SDLK_F4, pc1500::Key::F4},
    {SDLK_F5, pc1500::Key::F5}, {SDLK_F6, pc1500::Key::F6},
};
// clang-format on

// ON is not part of the 8x8 matrix (wired straight to CPU BFI) so it's
// handled separately from kKeyMap.
constexpr SDL_Keycode kOnKeycode = SDLK_F12;

// Host-only convenience key -- mimics the real machine's recessed ALL
// RESET pinhole button, which isn't part of the 8x8 matrix either.
constexpr SDL_Keycode kResetKeycode = SDLK_F11;

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
        "ROM area zero-filled\n",
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
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        bool pressed = (event.type == SDL_KEYDOWN);
        SDL_Keycode kc = event.key.keysym.sym;
        if (kc == kResetKeycode) {
          if (pressed) cpu.reset();
        } else if (kc == kOnKeycode) {
          if (pressed) cpu.pressOnKey();
          bus.ioPort().setOnKeyLine(pressed);
        } else {
          for (const KeyMapping& m : kKeyMap) {
            if (m.keycode == kc) {
              keyboard.setKeyState(m.key, pressed);
              break;
            }
          }
        }
      }
    }

    // step() checks for pending interrupts even while halted (and clears
    // halted_ if one dispatches), so keep calling it either way rather
    // than skipping entirely -- that's the only way HLT can ever resume.
    int cyclesRun = 0;
    while (cyclesRun < kCyclesPerFrame) {
      int c = cpu.step();
      cyclesRun += (c > 0) ? c : 1;
      cyclesSinceTimerTick += (c > 0) ? c : 1;
      while (cyclesSinceTimerTick >= kCyclesPerTimerTick) {
        cpu.tickTimer();
        cyclesSinceTimerTick -= kCyclesPerTimerTick;
      }
      if (cpu.halted()) break;
    }

    SDL_SetRenderDrawColor(renderer, 0xC8, 0xD8, 0xC0, 255);  // unlit LCD background
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 0x20, 0x28, 0x18, 255);  // lit dot
    for (int col = 0; col < pc1500::Lcd::kColumns; col++) {
      for (int row = 0; row < pc1500::Lcd::kRows; row++) {
        if (lcd.dot(col, row, cpu.disp(), readByte)) {
          SDL_Rect r{col * kScale, kIndicatorBarHeight + kMarginTop + row * kScale, kScale - 1,
                     kScale - 1};
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
