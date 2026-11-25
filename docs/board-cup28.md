# Volvo VNS CUP28

`-M cup28` instantiates a separate SH-4 board in the same QEMU build as
`cup2006`. The firmware on the Western disc identifies itself as
`CUP28.SHx.ORG`. The launcher checks the B000FF records and loads the image
at physical `0x00080000`. The CPU starts at `0x80080004`. This boots the
stock SHx image directly. The original reset ROM and M-loader update process
are outside the supported launch path.

The same disc's `LOADING.KWI` also lists a `CUP2001E.MLD.ORG` /
`CUP2001E.SHx.ORG` pair (the SHx image links at `0x80060000`, the M-loader at
`0x8c040000`, as `CUP28.MLD.ORG` does). The unit boots the `CUP28.SHx.ORG`
image — that is the board modelled here, hence the machine name `cup28`; the
role of the `CUP2001E` modules is not modelled.

| Device | Address or interface | Scope |
|---|---|---|
| DVD data ports | `0x07410000` | Read-only sector transfers from the supplied ISO |
| CPLD interrupt control | `0x07200000` | Shared CPLD: programs the IRL interrupt priority lines |
| SH775x DMAC channel 0 | SoC DMAC registers | DVD sector DMA, instant (STUB) |
| Audio | `0x07100100` | Playback DMA, PCM and KIWI voice decoding; host audio backend |
| GPS UART | `0x07300000` | Receiver registers; position arrives over MOST |
| MOST | OS8104 control port `0x07400000` | A Volvo vehicle peer, remote keys, North American settings, navigation GPS messages and sound-channel negotiation |
| EEPROM window | `0x07800000` | Erased (0xFF) RAM behind the EEPROM registers, plus the MOST GPIO shadow; the `+0x010` block is the A/D sample service, not a transfer engine |
| 16550 UART | `0x04800000` | Serial console |
| Q2SD | UGM `0x10000000`, registers `0x11000000` | Q2SD display lists, palette, scanout and frame interrupts |
| FLR window | `0x01a00000` | flr.dll's 256 KiB flash-loader window |
| Selftest / RTC | `0xff93c000`, `0xffc80000` (alias `0x1fc80000`) | Board-revision ADC sample; real-time clock |
| Nonvolatile state | XIP flash at `0x0` (alias `0x02000000`), battery SRAM at `0x12000000`, backup EEPROM at `0x18000000` | File-backed flash, battery SRAM and backup EEPROM. The guest formats the EEPROM itself (`EEPROM  ` header at offset 0) and keeps its power-on counters and clock at `+0x70` |

The About screen's HW version is the eight bytes at offset `0x7ff0` of the
backup EEPROM: five packed-BCD bytes shown as ten digits, then a three-byte
suffix. An erased tail shows as ten `?` and three `0xff` bytes, so the machine
seeds those eight bytes with the placeholder `0000000000 AA` when they are all
`0xff`, and leaves an identity a unit already holds alone. The placeholder
stands in for the factory part number, which this machine does not have. The
unit keeps the identity in `eeprom.bin` alongside its guest-managed state.

The vehicle peer announces the car's market in FBlock `F5/2B2` and its
language in `F5/2C1`; the firmware's MOST handler reads them as
`NMOSTHandler::ParseNavigationRegion naviRegion=` and `ParseLanguage
language=`, applies them and persists them. STUB: a real car has both set in
the workshop, so this machine takes them from the map disc in the drive,
whose volume name carries the market (`NA_0900_06A_WESTERN`): `NA_…` gives
market `06` and US English `11` (miles), `EU_…` gives market `04` and states
no language, so the unit keeps its own factory language (UK English, and
kilometres), and any other name keeps the tested pair. `-M
cup28,carmarket=…,carlanguage=…` (or `CUP28_CARMARKET` / `CUP28_CARLANGUAGE`)
forces either byte; `carlanguage=00` means the car states none, so that
message is left out of the announcement.

A unit's first boot cannot apply them: the language needs the files the guest
builds in its flash during that boot, so it logs `VPL_DataManager SetLangError`
three times, 3 s apart, and stays in its factory default (UK English, so
metric) for the whole session. Every later boot starts with the announced pair
in effect, which is why `boot` spends one headless power-on on a new unit
(`first-power-on.log`). Measured on the North American disc: the guest's setup
writes land within 5 s of power-on and its first-boot sequence ends at about
10 s; a later boot of that unit is pixel-identical to the second boot the old
workaround needed. A unit follows a
*change* the same way, one power-on later: announcing `04`/`01` to a unit set
up as `06`/`11` still comes up US on that boot and Scandinavian on the next
(the menus read `Vejledning` for `02`, `Vägledning` for `01`). The measured
codes, by the language the menus come up in, are `01` Swedish, `02` Danish and
`11` English (US).

The stock guidance files are used on subsequent boots. Speech recognition and
text-to-speech are outside the verified path.

The replay drives the receiver fix, cumulative MOST wheel count and analog
gyro sample from one virtual clock. CUP28 receives position and fix validity
over MOST `52/E00` and satellite status over `52/E01`. This is a receiver
model with a fixed satellite pattern while a fix is available.  The
receiver's clock is made up (STUB): it starts at a fixed 2026-11-22 12:00:00
and advances monotonically with virtual time, never jumping back. The turn and
distance scales are approximations. The firmware's locator handles loss of fix
and dead reckoning, so path error can accumulate during an outage.

Q2SD implements the commands used by the tested firmware, with fixed frame
timing. Original panel timing and exact pixel fidelity are unverified. Save
states, migration, optical-drive updates and other disc editions are outside
the tested scope. Use a cold restart for testing saved unit state.
