# Volvo VNS CUP28

`-M cup28` instantiates a separate SH-4 board in the same QEMU build as
`cup2006`. The firmware on the Western disc identifies itself as
`CUP28.SHx.ORG`. The launcher checks the B000FF records and loads the image
at physical `0x00080000`. The CPU starts at `0x80080004`. This boots the
stock SHx image directly. The original reset ROM and M-loader update process
are outside the supported launch path.

| Device | Address or interface | Scope |
|---|---|---|
| DVD data ports | `0x07410000` | Read-only sector transfers from the supplied ISO |
| CPLD interrupt control | `0x07200000` | Shared CPLD: programs the IRL interrupt priority lines |
| SH775x DMAC channel 0 | SoC DMAC registers | DVD sector DMA, instant (STUB) |
| Audio | `0x07100100` | Playback DMA, PCM and KIWI voice decoding; host audio backend |
| GPS UART | `0x07300000` | Receiver registers; position arrives over MOST |
| MOST | OS8104 control port `0x07400000` | A Volvo vehicle peer, remote keys, North American settings, navigation GPS messages and sound-channel negotiation |
| EEPROM window | `0x07800000` | Onboard EEPROM image plus MOST GPIO shadow |
| 16550 UART | `0x04800000` | Serial console |
| Q2SD | UGM `0x10000000`, registers `0x11000000` | Q2SD display lists, palette, scanout and frame interrupts |
| FLR window | `0x01a00000` | flr.dll's 256 KiB flash-loader window |
| Selftest / RTC | `0xff93c000`, `0xffc80000` (alias `0x1fc80000`) | Board-revision ADC sample; real-time clock |
| Nonvolatile state | XIP flash at `0x0` (alias `0x02000000`), battery SRAM at `0x12000000`, backup EEPROM at `0x18000000` | File-backed flash and battery SRAM. Other backup EEPROM windows are volatile approximations |

The vehicle peer sends market `06` in FBlock `F5/2B2` and US English
`11` in `F5/2C1`. The firmware's MOST handler reads those settings and
persists them. On the tested disc, the stock guidance files are used on
subsequent boots. Speech recognition and text-to-speech are outside the
verified path.

The replay drives the receiver fix, cumulative MOST wheel count and analog
gyro sample from one virtual clock. CUP28 receives position and fix validity
over MOST `52/E00` and satellite status over `52/E01`. This is a receiver
model with a fixed satellite pattern while a fix is available. The turn and
distance scales are approximations. The firmware's locator handles loss of fix
and dead reckoning, so path error can accumulate during an outage.

Q2SD implements the commands used by the tested firmware, with fixed frame
timing. Original panel timing and exact pixel fidelity are unverified. Save
states, migration, optical-drive updates and other disc editions are outside
the tested scope. Use a cold restart for testing saved unit state.
