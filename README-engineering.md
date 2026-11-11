# chipguy_ESP32-S3-Touch-AMOLED-2.06_display — engineering notes

The reasoning behind the display and touch drivers. The complete reading of the
manufacturer's schematic — every part, pin and net — is in
[schematic-description.md](https://github.com/chipguyhere/ESP32-S3-Touch-AMOLED-2.06_board/blob/main/schematic-description.md)
in the board library; this file keeps only what the drivers depend on and why.

**Bench status.** The drivers were run on one board on 2026-10-05 (USB power, no
battery) with a probe sketch. Measured there, and marked "measured" below: the
init sequence, colour order and orientation, quad-mode chunking, 80 MHz QSPI, the
full-frame write times, the TE signal and the panel's refresh rate, tear-free
operation, the visible area and corner radius, and touch mapping in rotation 0.
Everything else is derived from the schematic, the CO5300/FT3x68 register usage in
the manufacturer's examples (Arduino_GFX `Arduino_CO5300`, Arduino_DriveBus
`Arduino_FT3x68`), and the ESP-IDF `esp_lcd` sources, and is marked
**UNVERIFIED** or **TODO (bench)** where only a bench run settles it.

On 2026-10-28 the [partial-width line](#partial-width-writes-leave-a-line) was
measured on two boards, and the framebuffer path that avoids it was checked on one.

---

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| QSPI SIO0..3 | 4, 5, 6, 7 | `QSPI_SIO0` / `QSPI_SI1..3` on the FPC |
| QSPI SCLK | 11 | |
| CS | 12 | |
| RESET | 8 | active LOW, driven by the ESP32 |
| TE | 13 | tearing-effect output from the panel |
| Touch SDA / SCL | 15 / 14 | the board's shared I²C bus (2.2 kΩ pull-ups) |
| Touch INT | 38 | active LOW, no board pull-up (R26 NC) — driver enables the internal one |
| Touch RESET | 9 | active LOW |

The panel connector J3 also carries `IM0` (10 kΩ to GND) and `IM1` (10 kΩ to
VCC3V3), strapping the CO5300 for QSPI; the unused MIPI-DSI pairs, all tied to GND;
`VCI` and `VDDIO`, both on VCC3V3 (the AXP2101's DCDC1, always on while the board
is on); and `DSI_PWR_EN`, pulled up to the AXP2101's **ALDO2** through R10 (10 kΩ).
See schematic-description.md for the full J3 pinout.

## QSPI framing

The CO5300 (like the SH8601 it descends from) frames every transfer as a 32-bit
"command" phase on one line, followed by data:

| Opcode | Phase layout | Used for |
|---|---|---|
| `0x02` | `02 00 RR 00` on SIO0, then parameters on SIO0 (1-1-1) | register writes |
| `0x32` | `32 00 2C 00` on SIO0, then pixels on SIO0..3 (1-1-4) | RAMWR |

This maps directly onto `esp_lcd_new_panel_io_spi()` with `lcd_cmd_bits = 32`,
`lcd_param_bits = 8` and `flags.quad_mode = 1`: the driver passes
`(opcode << 24) | (reg << 8)` as the "command". In quad mode esp_lcd sends the
command phase single-line, parameters single-line, and colour data with
`SPI_TRANS_MODE_QIO`. This is the same arrangement Espressif's `esp_lcd_sh8601`
component uses, and Waveshare's IDF BSP for this board builds on that component.

**SPI3_HOST, not SPI2.** On the S3 the Arduino core's default `SPI` object is FSPI =
SPI2_HOST, and the microSD slot is wired for SPI mode on its own pins (1/2/3/17).
Putting the display on SPI3 leaves SPI2 for `SPI.begin(2, 3, 1, 17); SD.begin(17);`.
SPI3 on the S3 has the four data lines quad mode needs.

**Clock.** `AMOLED206_QSPI_HZ` defaults to **80 MHz**. The pins go through the GPIO
matrix (the display isn't on SPI3's IO_MUX pins), for which 80 MHz is the S3's
ceiling. Measured on one board (2026-10-05): clean at 80 MHz, with no speckles,
shifted rows or colour errors on a test screen with a 1-px border, colour swatches
and text, through continuous animation. A full-frame write (`fillScreen` +
`waitFlush`) takes **12.6 ms at 80 MHz** against **22.8 ms at 40 MHz** (the
manufacturer's Arduino examples' rate, Arduino_GFX's default). Build with
`-DAMOLED206_QSPI_HZ=40000000` to drop back if another unit misbehaves.
**TODO (bench):** 80 MHz on more units.

## Init sequence

`begin()` does, in order:

1. `board.panelPower(true)` — ALDO2 to 3.3 V and on. See [DSI_PWR_EN](#dsi_pwr_en-and-what-sleep-does).
2. Hardware reset on GPIO8: high 5 ms, low 20 ms, high, wait 150 ms. (Arduino_GFX
   waits 200 ms each side; the CO5300 needs well under that. It came up every time
   on the bench, including after full power removal; if a cold boot ever comes up
   black, lengthen it.)
3. Two `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` buffers of
   `AMOLED206_FLUSH_MAX_PIXELS` (502 × 40) pixels, ~40 KB each.
4. SPI bus + panel IO (above), queue depth 4.
5. Commands:

| Command | Value | Why |
|---|---|---|
| `11` SLPOUT | — | then 120 ms |
| `FE` | `00` | select the user command page |
| `C4` SPIMODECTL | `80` | enable QSPI RAM write (from the manufacturer's sequence) |
| `3A` COLMOD | `55` | RGB565 |
| `36` MADCTL | `00` | no flips — rotation is ours |
| `35` TEON | `00` | TE output on V-blank (GPIO13; see [Tear-free updates](#tear-free-updates)) |
| `53` WRCTRLD | `20` | brightness control on |
| `63` | `FF` | high-brightness-mode ceiling, as the manufacturer sets it |
| `58` WRCE | `00` | sunlight contrast enhancement off |
| `20` INVOFF | — | |
| `51` WRDISBV | `00` | dark while the frame memory is cleared |

6. `fillScreen(0)` so the panel doesn't show power-up garbage, `29` DISPON, then
   `51` to the stored brightness (default 80 %).

The manufacturer's sequence turns the display on before setting brightness `D0`;
ours clears first and lights last, which avoids a flash of random frame memory.
Measured: the panel comes up clean with this order, colours correct (RGB, not BGR)
and the picture upright in rotation 0.

## The 22-column offset

The CO5300's column RAM is wider than the glass: the manufacturer's setup passes
`col_offset1 = 22` and no row offset. `_setNativeWindow()` adds 22 to both column
addresses. Callers never see it. Measured: with the offset, a 1-px border on the
outermost pixels shows evenly on all four sides (see [the visible area](#the-visible-area-and-the-rounded-corners)).

## Even alignment

The CO5300 requires every window to **start on an even column/row and span an even
number** of them (Waveshare's own LVGL examples install the same rounder).

- **The framebuffer path** writes whole native rows, so columns are 0–409 every
  time, and `commit()` rounds the first changed row down to even and the last up to
  odd. LVGL's own areas need no rounding at all: `drawRegion()` takes any rectangle.
- **The no-PSRAM fallback** keeps an `LV_EVENT_INVALIDATE_AREA` rounder. It
  stretches each area to the full native width and clears bit 0 of the start / sets
  bit 0 of the end the other way. LVGL 9 also consults the rounder when it splits a
  tall area into bands (`get_max_row()`), so band boundaries stay even.
- Both screen dimensions (410, 502) are even, so `x2 | 1` never leaves the screen,
  and the rotation transforms preserve the property: e.g. rotation 1 maps logical
  `y2` (odd) to native `nx1 = 409 − y2` (even), because `W − 1` and `H − 1` are odd.
- The 22-column offset is even, so it doesn't break it either.

Direct callers of `flushRegion()` must pass even-aligned areas themselves.

## Partial-width writes leave a line

**The erratum.** A write window narrower than the panel leaves a faint vertical
line, 2–4 px wide, at the window's left edge. It shows on light backgrounds (on
black the pixels are off) and its pixels change each time the window is rewritten.
Before the fix it appeared in every sketch with a light screen: the Wi-Fi keyboard,
and a white-background diagnostic whose text block's left edge it marked. Moving
that text a few pixels a minute (an anti-burn-in drift) spread it into two stripes,
one per edge position. It is not a defect of one panel: it showed on two boards,
and the same two boards ran the manufacturer's ESP-Brookesia demo, whose
full-white drawing app showed nothing. That firmware's esp_lcd path writes
differently.

**The test** (`amoled206-line-test`, 2026-10-28, measured on the second board): a
white screen, a block of text redrawn four times a second whose left edge was at
x = 101, and an extra rounder that ran after the library's, cycled by touch:

| Mode | Window columns | Line |
|---|---|---|
| EVEN | the library's rounding (start even) | present |
| RAM4 | start/end on 4-column boundaries of the controller's memory (offset 22 counted) | present |
| RAM8 | the same, 8 columns | present |
| SCR4 | 4-column boundaries of the screen (offset ignored) | present |
| FULL | the whole width, 0–409 | **gone** |

So it is not about block alignment of the window start: only full-width windows
avoid it. **Inferred:** the controller keeps each frame-memory row as a unit
(compressed, or otherwise processed per row) and rebuilds the row from mixed new and
old data when a write covers part of it; the seam lands at the window's left edge.
The AXS15231B (another QSPI panel controller with its own frame memory) has an
erratum with the same symptom and the same workaround, which suggests this is a
family trait of such controllers rather than one chip's bug.

**The fix** is the framebuffer path below: every write is whole native rows, so a
window never starts anywhere but column 0. Rechecked with the line test after the
change: no line in any mode, because the driver widens whatever it is given.
`flushRegion()` still writes the window it is handed, and shows the line if that is
narrower than the panel.

## Rotation is done in software

The CO5300's MADCTL offers horizontal and vertical flips only — **no row/column
exchange** (Arduino_GFX: "CO5300 does not support rotation"). Arduino_GFX imitates
rotation 1/3 with flips, which isn't a 90° rotation at all, and a flipped X axis
also moves the visible window relative to the 22-column offset. Rather than chase
that, this driver keeps MADCTL at `00` and transforms pixels while copying
(`drawRegion()` into the framebuffer, `flushRegion()` into a DMA buffer):

| Rotation | logical → native | copy |
|---|---|---|
| 0 | `(x, y)` | byte swap |
| 1 | `(409 − y, x)` | transpose + swap |
| 2 | `(409 − x, 501 − y)` | reverse + swap |
| 3 | `(y, 501 − x)` | transpose + swap |

The copy is needed anyway — LVGL renders little-endian RGB565 and the panel wants
big-endian — so the transform costs roughly nothing extra. `drawRegion()` walks the
**native** rows top to bottom and each left to right, so its PSRAM writes are always
sequential; in rotations 1–3 the out-of-order access lands on the reads from LVGL's
band, which is in internal RAM where a strided read costs little. **TODO (bench):**
time `drawRegion()` for a 502×40 band in rotation 1 vs 0.

## The flush pipeline

`lv_setup` runs LVGL in **PARTIAL** mode with **one** band buffer (502 × 40 × 2 B,
internal RAM, falling back to PSRAM), feeding the driver's **framebuffer**: a
410 × 502 copy of the screen in PSRAM (~400 KB), in native orientation and already
in the panel's byte order.

1. **Each band** goes to `drawRegion()`, which copies it into the framebuffer (byte
   swap and rotation, see above) and widens the range of native rows changed this
   frame. Nothing is sent. `lv_display_flush_ready()` is called at once, because the
   band has been copied out; a second LVGL buffer would buy nothing.
2. **The frame's last band** (`lv_display_flush_is_last()`) also calls `commit()`,
   which sends the changed rows — rounded to even/odd — as full-width strips. A row
   that several bands or areas touched goes out once. The parts of each row LVGL did
   not redraw come from the framebuffer, which is what makes full-width writes
   possible at all (see [Partial-width writes](#partial-width-writes-leave-a-line)).
3. **Each strip** is one sequential `memcpy` from PSRAM into driver DMA buffer A
   (alternating A/B; the S3's SPI DMA can't read PSRAM), then CASET and RASET with
   `esp_lcd_panel_io_tx_param()`, then RAMWR + pixels with
   `esp_lcd_panel_io_tx_color()`. esp_lcd only issues a polling parameter transfer
   after collecting every queued colour transaction, so **the window command is where
   the previous strip is known complete**. While strip n is on the wire, strip n+1 is
   copied.

The strip is `AMOLED206_FLUSH_MAX_PIXELS / 410` rows rounded down to even: 48 rows,
39 KB. `fillScreen()` writes the panel directly and leaves the framebuffer alone.
`wake()` resends the whole framebuffer after the re-init, so LVGL needn't redraw.

**Without PSRAM** (the board options set it off) `beginFramebuffer()` fails and
`lv_setup` falls back to sending each band straight to the panel through
`flushRegion()`, with a rounder that stretches every area to the full native width.
In portrait that is line-free. In landscape it isn't: LVGL splits a full-height area
into bands of logical rows, which are narrow strips of native *columns*. The board's
ESP32-S3R8 has 8 MB of PSRAM, so this is only a fallback.

**Buffer safety.** Flush N writes into buffer (N mod 2). The transfer that last read
that buffer was flush N−2, which finished before flush N−1's window went out. So
by the time flush N converts into it, it is free — without any explicit wait.

**Completion tracking.** `on_color_trans_done` gives a counting semaphore from the
ISR. `_outstanding` counts queued colour writes. After every `tx_param`, `_reap()`
takes the semaphore non-blocking until `_outstanding` reaches 0 (those transfers
are known done, so the takes succeed); `waitFlush()` takes it blocking. esp_lcd
fires the callback once per `tx_color`, on its last chunk, so `_outstanding` never
exceeds 1 in practice.

**Chunking.** The S3's SPI caps one transaction at 2¹⁸ bits = 32 KB; `max_transfer_sz`
is set to 32 KB and esp_lcd splits larger colour writes, holding CS between chunks.
A 40 KB band is two chunks. Measured: esp_lcd's chunking keeps quad mode across
chunks on the Arduino core's IDF (core 3.3.11) — full-frame fills and 40-row bands
draw cleanly. If a future core breaks it, the symptom would be the bottom of each
band garbled; drop `LV_SETUP_BUFFER_ROWS` (and `AMOLED206_FLUSH_MAX_PIXELS`) so a
band is under 32 KB.

**Timing.** A full frame is 410 × 502 × 16 bit = 3.29 Mbit: 20.6 ms on the wire at
40 MHz × 4 lines, 10.3 ms at 80 MHz. Measured (one board, 2026-10-05, `fillScreen`
in 12 strips + `waitFlush`): **22.8 ms at 40 MHz, 12.6 ms at 80 MHz**. The difference
from the wire time is per-strip command overhead and the CPU fill/convert work.

## The visible area and the rounded corners

The glass's aperture is not the full rectangle of the pixel array: its corners are
rounded off. Measured on one board (2026-10-05), three ways:

1. **Calipers** on a 2-px white border drawn on the outermost pixels: the lit area is
   ≈ 32.5 mm × 40 mm (so the pitch is ≈ 12.6 px/mm: 410 px / 32.5 mm, 502 px / 40 mm),
   and the straight runs of the border are 17.5 mm (horizontal) and 24.5 mm
   (vertical). That puts the start of each corner curve 7.5–7.75 mm, ≈ 95–97 px,
   from the corner — an under-read, since a thin line fades out before the curve's
   true tangent point.
2. **Nested arcs:** quarter circles of radius r centred r px in from both edges (a
   rounded-rectangle corner of radius r). Every arc with r below the glass's radius
   lies wholly in the masked corner and is invisible. At 10-px steps only r = 120
   was visible; at 5-px steps r = 115 was visible on the left-hand corners and 120
   on the right-hand ones.
3. **Fitted outline (decisive):** a 1-px rounded-rectangle outline with adjustable
   per-side insets and radius. With **all insets 0 and radius 115** it traced the
   visible edge uniformly all the way round, about half of the line's width cut
   off; moving any parameter inward brightened it.

So: **the whole 410×502 grid is visible to the edges (the outermost row and column
half-masked), and each corner is cut on a circle of radius ≈ 115 px.** No column
offset correction is needed beyond the controller's 22. `AMOLED206_CORNER_RADIUS`
(115) in `chipguy_AMOLED206_display.h` carries the number for UI code. The corners
are symmetric, so it applies in every rotation. A point at (d, d) from a corner is
visible when (115 − d)·√2 ≤ 115, i.e. d ≥ 34 px.

## Tear-free updates

**The signal.** `begin()` sends `TEON` with parameter 0x00: TE mode 0, a pulse that is
high during vertical blanking only. It arrives on GPIO13 (J3 pin 27, `LCD_TE`). The
driver attaches a RISING-edge interrupt, so each interrupt marks the **start of
V-blank**, just before the panel's scan restarts at native row 0. The ISR counts
edges and gives a binary semaphore. `measureRefreshHz(ms)` counts edges over a window.
The CO5300's refresh rate is not on any document read. **Measured: 58.98–59.02 Hz**
(one board, 2026-10-05, several runs), i.e. a 16.95 ms frame. TE works as described.

**The principle.** The panel copies frame memory to the glass row by row, top to
bottom in native coordinates, once per refresh. A tear is the scan passing the write:
rows above the crossing show the new frame and rows below show the old. If the write
**starts at the top at V-blank and runs faster than the scan**, it is always below
the rows the scan has yet to reach, and the scan only ever sees finished rows.
Strictly, the condition is: **writing the dirty box's rows takes less time than the
scan takes to reach the box's bottom row.** A full-screen box needs write time < the
active scan time (one frame period minus V-blank).

**The arithmetic, measured.** The panel scans a frame in **16.95 ms** (59 Hz). A
full-frame write takes **22.8 ms at 40 MHz** — so at 40 MHz a full-screen update
cannot stay ahead of the scan; it will be "late" and can tear in its lower part
(boxes up to roughly two-thirds of the screen's height can still stay ahead). At
**80 MHz, the default, it takes 12.6 ms** and fits with room to spare. Bench result
(2026-10-05, 80 MHz, tear-free mode, continuous finger-drag animation with the host
draining the serial port): **684 consecutive frames TE-synced, zero late**. The 3
late frames seen in an earlier run all coincided with `Serial.print` stalls (host
not reading the USB console) — a CPU stall between the TE wait and the write, not a
throughput limit. Visually: no tears, no artifacts.

**What `commit()` does** (`lv_setup`, at the end of each LVGL frame):

1. Takes the range of native rows changed since the last commit, rounded to even
   start / odd end. Rows are the panel's scan unit, so this is exactly the "dirty
   box" the principle above needs, and always full width.
2. `waitFlush()`, so the previous frame is fully out. Then, with sync on, it waits up
   to 50 ms for the next TE edge, after first dropping any stale semaphore count.
3. Streams the rows top to bottom in strips (see [the flush pipeline](#the-flush-pipeline)):
   one sequential copy from PSRAM per strip, no conversion, since the framebuffer is
   already native and byte-swapped. `tx_param` waits for strip n's DMA before strip
   n+1's window goes out, which is also what makes reusing the buffers safe.
4. Returns once the last strip is queued. LVGL may render into its band again, and
   `drawRegion()` may change the framebuffer, because the bytes on the wire are in
   the DMA buffer.

Because the rotation work happens in `drawRegion()`, while LVGL renders, the time
between the TE edge and the last strip is the same in all four rotations: copies
and wire time only. **TODO (bench):** re-measure the full-frame write time with the
framebuffer path (expected at or under the 12.6 ms above, which included a
byte-swapping gather).

`flushFrameRegion()` (a caller-owned full-screen framebuffer in the logical
orientation) remains for sketches that drive the panel without `lv_setup`. It
follows the same steps but gathers and converts as it goes, and it too now sends
the covered rows full width.

**The late-frame metric.** In `_onColorDone` (the DMA-done ISR) the driver records the
TE count at completion. At the start of the next frame's write (`_startFrame()`), after
`waitFlush()`, the previous synced frame is judged. If the TE count at its last
DMA's completion differs from the count captured right after its TE wait, another
V-blank began before the write finished. The scan lapped the write, and that frame
**may** have torn: `lateFrames()`. `syncedFrames()` counts frames written in sync.
This test is conservative: a write can be lapped without a second TE edge occurring
(once it falls behind the scan within one frame). It catches the main failure, a
write slower than a whole frame. TearDemo prints both counters.

**Fallbacks.** `lv_setup` is tear-free when the framebuffer allocates **and**
`setTearSync(true)` sees a TE edge within 100 ms; it says which on Serial
(`LVGL: framebuffer, tear-free …`). Without TE it still uses the framebuffer, just
unsynchronised. With sync on, a missing TE edge inside 50 ms makes that frame go out
unsynchronised rather than hang. `LV_SETUP_TEAR_FREE 0` skips the TE wait.
`setTearSync(false)` stops waiting at run time, which is useful for A/B comparison
(TearDemo's BOOT button).

## DSI_PWR_EN, and what `sleep()` does

The module's `DSI_PWR_EN` (J3 pin 21) is pulled up to ALDO2 through 10 kΩ; a second
footprint (R54, NC) would route it to test pad TP14. The manufacturer's examples
never touch ALDO2, so the PMIC's power-on defaults have it on (measured); `begin()`
turns it on regardless.

**ALDO2 must stay on (measured 2026-10-06).** With ALDO2 off, so the enable is low,
the board drew **15.3 mA** in deep sleep against **2.2 mA** with it on. A 10 kΩ
pull-up can't carry anything like that, so the AMOLED module itself reacts to the
pin. The likely reading is that its internal supplies or the controller misbehave
with the logic supplies (VCI/VDDIO, on VCC3V3) still up. So
`AMOLED206_Board::panelPower(false)` and `power.setRail(ALDO2, false)` refuse.

**`sleep()`** sends DISPOFF, SLPIN, waits 120 ms, then **`4F` = 01: deep standby**.
Measured: deep standby is **0.35 mA** below SLPIN alone (2.42 vs 2.77 mA in the same
run). The CO5300 leaves deep standby only through a hardware reset, and the frame
memory is lost. So **`wake()`** runs the full bring-up (`_initPanel()`: reset pulse,
init sequence, clear to black, DISPON, restore brightness), shared with `begin()`.
`lv_setup.wake()` follows it with `lv_obj_invalidate(lv_screen_active())` so LVGL
repaints. The SPI bus, DMA buffers and TE interrupt survive; only the panel is
re-initialized.

Also measured: holding the panel **in reset** (GPIO8 low) through sleep costs
**+0.35 mA** compared with deep standby. The power library therefore holds GPIO8
**high** through deep sleep.

## Touch and sleep

The FT3168 cannot be put to sleep in software (measured, 2026-10-06, PPK2 on the
cell):

- After every deep-sleep entry the board draws about **1.3 mA extra for ~13.7 s**,
  in pulses every 60 ms. It then settles into small pulses every ~100 ms. Holding
  the controller in reset removes both, which identifies it.
- Writing **hibernate** (`A5` = 3), even straight after a reset when the controller is
  awake and the write is ACKed, did not shorten the burst.
- FocalTech's usual monitor-mode registers (`86` = 1 auto-monitor, `87` = 1 s
  timeout) ACK but read back `00`, and the burst stayed 13.7 s. This FT3168 doesn't
  implement them, or not at these addresses. The chip-ID register `A0` also reads
  `00`.
- Holding it **in reset** through sleep costs **+0.28 mA** continuously. The burst
  costs about 18 mA·s per sleep, so break-even is a sleep of **~64 s**. For longer
  sleeps — normal for a watch — letting it idle is cheaper. That is what the
  libraries do: the power library holds GPIO9 **high** through sleep, so the
  controller doesn't float into and out of reset.

`touch().sleep()` still sends the hibernate command, and `wake()` still resets the
controller. That is harmless, and right for any FT3x68 that does honour it.

## Touch: FT3168

Address 0x38, on the shared bus. The driver uses:

| Register | Meaning |
|---|---|
| `02` | TD_STATUS: number of touch points (low nibble) |
| `03`..`06` | P1 XH, XL, YH, YL; XH bits 7:6 = event (0 down, 1 lift, 2 contact) |
| `A0` | chip ID — documented `03` for the FT3168, but reads **`00`** on the bench unit while touch works normally; used only as an "it answers" probe |
| `A5` | power mode: 0 active, 1 monitor, 2 standby, 3 hibernate |

**Bring-up.** Reset low 20 ms, then poll the chip-ID register every 20 ms for up to
300 ms (the manufacturer waits a flat 50 ms). Then power mode = **monitor**, as the
manufacturer's examples do: the controller slows its scan when idle and wakes on a
touch.

**INT-gated polling.** A falling-edge ISR on GPIO38 sets a flag. `read()` only
touches I²C if the flag is set, INT is low right now, or a touch is in progress (so
the lift is seen). That keeps LVGL's 30 Hz polling off the shared bus while nobody
is touching, and avoids talking to a controller in its slow-scan state. It reads
registers 02..06 in one transaction. Zero points, more than two, or a lift event
means released. **UNVERIFIED:** whether the FT3168 keeps pulsing INT while a finger
is held still (the `_down` flag covers it either way).

**Coordinates** arrive in the native 410×502 frame (the manufacturer's examples use
them raw), clamped, and mapped through the inverse of the display transform.
Measured in rotation 0: the full 0..409 × 0..501 range is reported and the point
lands under the finger.

| Rotation | native `(tx, ty)` → logical |
|---|---|
| 0 | `(tx, ty)` |
| 1 | `(ty, 409 − tx)` |
| 2 | `(409 − tx, 501 − ty)` |
| 3 | `(501 − ty, tx)` |

**Sleep.** `touch().sleep()` writes hibernate (3) and `wake()` resets the
controller — but this FT3168 ignores hibernate; see [Touch and sleep](#touch-and-sleep).

**Wake from sleep.** GPIO38 is **not** an RTC GPIO (the S3's are 0–21), so touch
cannot wake the chip from deep sleep. It can wake **light** sleep through
`gpio_wakeup_enable()` — **TODO (bench)** with the controller in monitor mode.

---

## LVGL's printf has no floats

The bundled `lv_conf.h` builds LVGL's own `printf` (used by
`lv_label_set_text_fmt()`) without floating-point support. A `%f` in the format
doesn't print a number: it silently consumes the wrong arguments, and a `%s` after it
then reads a garbage pointer. Seen on the bench (2026-10-06) as a `LoadProhibited`
panic in `lv_strnlen` from a `"%.2f V%s"` battery line. Format floats with
`snprintf()` into a buffer and pass that to `lv_label_set_text()`; the GlanceWatch
example does this.

## Opportunities

- **Start mid-frame:** the CO5300's tear scanline command (0x44, "set tear scan
  line") moves the TE edge to a chosen row. For a dirty box low on the screen, starting
  the write when the scan passes the box's top (the write then chases *behind* the
  scan) allows a slower write than starting at V-blank.
- **Slow the panel down:** if the CO5300 has a frame-rate register (TODO datasheet),
  a lower refresh rate would let full-screen writes at 40 MHz fit inside one scan
  (no longer needed at the 80 MHz default).
- Finding what keeps the board's ~2 mA deep-sleep floor up (the AMOLED module's own supplies are a candidate; ALDO2 off is not the way — measured +13 mA).
