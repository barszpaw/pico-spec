# Timex hi-res mode (`timex_mode == 6`) downsamples real 512px resolution to 256px — breaks 80-column/hi-res-dependent software

**Status:** `Config::timex_video` ogólnie działa poprawnie — niezależnie potwierdzone demem `TC2048 Hi-Res Colour Demo` (standardowy test trybów Timex, niepowiązany z ZX-Uno). Problem dotyczy konkretnie trybu 6 (prawdziwy hi-res monochromatyczny, 512 pikseli w poziomie).

## Przyczyna (zweryfikowana w kodzie)

`OUT (#FF), data` jest poprawnie dekodowane (`Ports.cpp:2178-2187`, `VIDEO::timex_mode = data & 0x07`). Dla `data=0x3E` daje `timex_mode=6`. Handler tego trybu (`Video.cpp:2830-2840`) ma komentarz:

```cpp
// Hi-res mode 6 (512->256): real SCLD alternates byte-columns from
// screen0 and screen1 at same address, 64 cols x 8 bits = 512 pixels.
// For 256px output, OR-merge each pair (s0[addr] | s1[addr]) into
// 8 output pixels — preserves all set bits, covers all 32 addresses.
```

pico-spec **świadomie** redukuje realny tryb hi-res (512px, dwa banki ekranu przeplatane) do 256px wyjścia, łącząc bitowym OR każdą parę bajtów z dwóch banków. Moon Rabbit's 80-column font rysuje zakładając pełną, nieskompresowaną rozdzielczość 512px — OR-merge dwóch różnych kolumn w jedną daje właśnie efekt "nakładających się znaków".

## Odrzucona wcześniejsza teoria #1

Podejrzewano brakujący rejestr ZX-Uno `SCANDBLCTRL` (`#0B` via `#FC3B`/`#FD3B`). Błędny trop — jego własny komentarz w źródle (`moon-rabbit-zx/vdp/timex80.asm`) mówi `;; Force turbo mode`: to realne przyspieszenie CPU na fizycznym sprzęcie (żeby prawdziwy Z80 nadążył z odświeżaniem gęstszego ekranu), nie coś definiujące wyświetlaną treść. Emulator nieograniczony prawdziwym taktowaniem Z80 nie potrzebuje tego wcale.

## Odrzucona wcześniejsza teoria #2 (ten dokument sam ją zawierał wcześniej)

Pierwsza wersja tej analizy twierdziła, że `SPEC_W = 256` (`Video.h:46`) jest "fundamentalną stałą używaną w całym pipeline wideo" i że naprawa wymagałaby zmiany architektonicznej (bufor ramki, timing HDMI). **To było błędne** — zweryfikowane ponownie po pytaniu Pawła:

- `SPEC_W` jest użyte w **jednym miejscu** w `Video.cpp` (liczenie szerokości bordera: `(vga.xres - SPEC_W) >> 1`).
- Realny bufor wyjściowy (`vga.xres`) to **640 albo 720px** (prawdziwa rozdzielczość HDMI/VGA, `VIDEO::changeMode()`, tabela `vidmodes[][hRes]`) — jest mnóstwo fizycznego miejsca na 512px, to nigdy nie był limit pojemności bufora czy pasma wyświetlacza.

## Prawdziwe ograniczenie

pico-spec rysuje piksele **na żywo, w takcie emulowanego zegara Z80** — `loopCount = statestoadd >> 2` (`Video.cpp:2814`, `:3005`): ile pikseli narysować w danym wywołaniu wynika z liczby upłynniętych T-states, nie ze stałej liczby kolumn. Prawdziwy hi-res wymaga wypisania **16 niezależnych pikseli ink/paper na parę bajtów** (nie 8-pikselowego OR-merge) w **tym samym** budżecie czasowym co zwykła linia. To jest pytanie o **czas CPU na core1 na linię skanowania**, nie o pamięć czy pasmo.

## Zweryfikowany algorytm (źródła Fuse)

Sprawdzono jak **Fuse** (referencyjny emulator, już potwierdzony jako renderujący to poprawnie) implementuje SCLD hi-res — `/home/pawelb/src/fusex/display.c:426-427` + `ui/fb/fbdisplay.c:uidisplay_plot16`:

```c
libspectrum_word hires_data = (data << 8) + data2;  /* połącz oba banki */
/* 16 niezależnych pikseli, MSB first, bit=1 -> ink, bit=0 -> paper: */
for (i = 15; i >= 0; i--) pixel[15-i] = (data16 & (1 << i)) ? ink : paper;
```

Żadnego przeplatania ani trików — proste rozwinięcie 16 bitów na 16 pikseli. Przeniesione na handler `Video.cpp:2830`, oznacza to **4 zapisy do `lineptr32` zamiast 2** na parę bajtów wejściowych (dwa razy więcej pracy na linię), przez nową, 2-kolorową (ink/paper) tabelę LUT na nibble, zamiast obecnej pełno-atrybutowej `AluByte`.

## Potwierdzony symptom

1. `moon-rabbit-zx` build `zxuno-80` (80 kolumn, Timex) — znaki nakładają się na siebie na pico-spec.
2. Ten sam plik na **Fuse** — renderuje poprawnie.
3. Niezależnie odtworzone drugim, minimalnym testem: `Threetwosevensixseven/TimexHiResExample`'s `PortFF.tap` — ten sam wzorzec: Fuse OK, pico-spec nakładanie.

## Zweryfikowany wniosek

To jest **konkretna, ograniczona zmiana w renderze, warta spróbowania na RP2350** — nie ściana architektoniczna, jak pierwsza wersja tego dokumentu błędnie twierdziła. RP2350 już dźwiga ciężej obciążone tryby na linię skanowania w czasie rzeczywistym (Gigascreen, ULA+, 16col) — podwojenie liczby zapisów pikseli tylko dla rzadkiego trybu hi-res jest prawdopodobnie w budżecie. Otwarte ryzyko: czy core1 zdąży w tej samej, rzeczywistej linii czasowej HDMI/VGA — jeśli nie, efektem byłoby widoczne rozjechanie skanowania, nie płynne "opóźnienie".

## Pierwsza próba (2026-10-05, ODRZUCONA — przepełnienie bufora)

Pierwsze podejście zastąpiło OR-merge czterema niezależnymi odczytami `AluByte` (16 pikseli ink/paper na parę bajtów wejściowych, zamiast 8 po OR-zie) — czyli próbowało fizycznie wypisać 512 niezależnych pikseli do bufora linii. Zbudowało się i wgrało bez błędu, ale na realnym sprzęcie (potwierdzone zrzutem BMP przez `CaptureToBmp`/PrtScr, nie zdjęciem telefonem) dało **przepełnienie**: aktywny tryb wideo tej płytki (`VGA_HDMI`, `VgaMode_320x240` z `VGA.h`) ma logiczny bufor **320px** (32 border + 256 treść + 32 border) — 512 pikseli nie mieściło się, dane rozlewały się na cały border. Wycofane.

Kluczowa lekcja: fizyczny ekran ma więcej miejsca (640/720px, bo sterownik HDMI/VGA **sprzętowo dubluje** każdy piksel logiczny), ale to nie to samo co liczba niezależnie adresowalnych kolumn w buforze, do którego pisze renderer — ten pozostaje 320-elementowy niezależnie od trybu.

## Druga próba (2026-10-06, krótkotrwała) — pair-encoding przez zarezerwowane indeksy palety

Zamiast zwiększać liczbę zapisywanych pikseli, wykorzystano istniejący w kodzie wzorzec z trybu Profi "DS80" (`profi_pair_lookup`): jeden bajt bufora koduje DWA niezależne piksele przez 4 ręcznie zarezerwowane indeksy palety (245-248). Zadziałało technicznie (potwierdzone zrzutem BMP — surowe indeksy `{0,245,246,247,248}` w obszarze treści, tekst czytelny) — ale miało **błąd kolorów**: papier był zakodowany jako czarny na stałe. Odkryto to po sprawdzeniu `drewpo28/pico-speccy` (ten sam autor, siostrzane repo) — tam mode 6 renderuje się identycznie jak Fuse, co ten wariant nie robił.

## Finalna wersja (2026-10-06/07, POTWIERDZONA NA SPRZĘCIE) — port z pico-speccy

Pełny port mechanizmu `timex_hires_live`/`timexHiresRequest`/`timexHiresApplyPending` z `drewpo28/pico-speccy`, który **renderuje identycznie jak Fuse** (potwierdzone przez Pawła na trzech niezależnych testach). Różnice względem obu wcześniejszych prób:

1. **Prawdziwy kolor: jaskrawy ink na jego jaskrawym dopełnieniu bitowym**, nie ink-na-czarnym. `timexHiresInk() = (timex_hires_ink&7)|8`, `timexHiresPaper() = (~timex_hires_ink&7)|8`. Potwierdzone niezależnie trzema źródłami: (a) komentarz w `Threetwosevensixseven/TimexHiResExample/src/PortFF.asm`: `ld a,%00'001'110 ; 001 = set bright blue on yellow` — ink=blue(001), paper=yellow=~001&7=110 ✓; (b) zrzut ekranu Pawła z tego samego testu: niebieski tekst na żółtym tle ✓; (c) `moon-rabbit-zx` z ink=biały daje biały-na-czarnym (bo dopełnienie białego to czarny) — wygląda jak klasyczny terminal b/w, co jest poprawne, nie bug.
2. **Border też jest jaskrawym papierem w mode 6**, nie niezależnym kolorem ULA — przegapione w pierwszej wersji portu (Paweł zauważył czarny border tam gdzie referencja miała żółty); `updateBorderBrd()` dostał dedykowaną gałąź `if (timex_hires_live)` analogiczną do DS80's.
3. **Reużywa CAŁĄ istniejącą infrastrukturę DS80** (`profi_pair_lookup` - wszystkie 250 slotów, `profi_ds80_driver_set`, `Graphics8BitPalette::ds80_active`) przez 16-elementowy LUT (`timex_hr_lut[16]`, budowany z 2 kolorów × 4-bitowe kombinacje) — zamiast ręcznie zbudowanych 4 slotów. Mniej nowego kodu, więcej przetestowanej ścieżki.
4. **Bezpieczne przełączanie trybu na granicy ramki** (`timex_hires_pending_on/off` + `timexHiresApplyPending()` wołane z `EndFrame()`, ten sam wzorzec co już istniejące `profiPaletteApplyPending()`/DS80 activate-deactivate) — czyści framebuffer przy wejściu/wyjściu z mode 6, więc nie ma ramki ze starymi, źle zinterpretowanymi bajtami.
5. **Zmiana koloru w środku ramki jest "beam-exact"** (`timexHiresColour()`, tylko przebudowa 16-wpisowego LUT, bez przebudowy sterownika) — bezpieczne bez czekania na vblank, tak jak zmiana koloru bordera przez `OUT (#FE)`.
6. Zachowuje fallback OR-merge (dla SOFTTV/TFT bez sterownika pair) — ale teraz też z poprawnymi kolorami (jaskrawy ink na jaskrawym dopełnieniu), nie czarnym papierem.

Zmienione pliki: `src/Video.h` (nowe pola/metody `VIDEO::timex_hires_live` i `timexHires*`), `src/Video.cpp` (implementacje + `MainScreen`'s `timex_mode==6` branch + `updateBorderBrd()` + `EndFrame()` hook + `Reset()` cleanup), `src/Ports.cpp` (`#FF` handler: `timexHiresRequest()` na zmianę trybu, `timexHiresColour()` na zmianę inku w trakcie). Usunięto nieużywany kod z pierwszej próby (`TIMEX6_PAIR_BASE`, `hdmi_set_timex6_pair`/`vga_set_timex6_pair` w `drivers/hdmi/hdmi.c`/`drivers/vga-nextgen/vga.c`/`drivers/graphics/graphics.h`).

**Zweryfikowane na realnym RP2350-PiZero przez Pawła:**
1. `PortFF.tap` — "działa tak jak oczekujemy" — border/papier żółty, ink niebieski, identycznie jak Fuse.
2. `moon-rabbit-zx` zxuno-80 — "kontrastowy terminal b/w" — tekst czytelny, bez nakładania znaków, poprawne kolory (biały na czarnym, bo ink=biały).
3. F2/F5 (file dialog, patrz niżej — niezależna regresja build-type) działają razem z tą zmianą, zero interakcji.

Build wymaga `CMAKE_BUILD_TYPE=MinSizeRel` (patrz sekcja "Build type regression" / `project_arm_toolchain_requirement` w pamięci) — niezależne od tej funkcji, ale odkryte w tej samej sesji.
