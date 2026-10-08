# ZiFi ZX-Uno bridge (`#FC3B`/`#FD3B`) reports status bits at wrong positions — breaks all real-hardware-targeting clients

## Opis

`ZiFi::unoUartRead()`'s status register (`uno_addr == 0xC7`) returns RX-ready at bit0 (`0x01`) and TX-busy at bit1 (`0x02`):

```cpp
case 0xC7: { // UART status
    uint8_t st = rxAvailable() ? 0x01 : 0x00;
    if (fifo_full(zifi_out_head, zifi_out_tail)) st |= 0x02; // TX_BUSY
    return st;
}
```

This doesn't match the real ZX-Uno/divMMC hardware convention. Every client written against actual hardware (checked three independent, unrelated projects — same convention, not a coincidence):

- `BridgeZX` (`IgnacioMonge/BridgeZX`), `server/drivers/divmmc.asm`
- `SpecTalkZX` (`witchcraft2001/SpecTalkZX`), `asm/divmmc_uart.asm`
- `moon-rabbit-zx` (`nihirash/moon-rabbit-zx`), `drivers/uno-uart.asm`

all define:
```asm
UART_DATA_REG      = #c6
UART_STAT_REG      = #c7
UART_BYTE_RECEIVED = #80   ; bit7
UART_BYTE_SENDING  = #40   ; bit6
```
and poll via `ADD A,A` (shift bit7 into carry) / `AND #40`. With bits at 0/1 instead of 7/6, none of these guest drivers can ever see the correct status — connections hang indefinitely waiting for a bit that's set, but at the wrong position.

## Fix

```cpp
case 0xC7: {
    uint8_t st = rxAvailable() ? 0x80 : 0x00;        // UART_BYTE_RECEIVED
    if (fifo_full(zifi_out_head, zifi_out_tail)) st |= 0x40; // UART_BYTE_SENDING
    return st;
}
```
(`src/ZiFi.cpp`, `ZiFi::unoUartRead`, the `case 0xC7` block.)

## Confirmed on real hardware

Waveshare RP2350-PiZero, pico-spec v1.2.30, custom build. Before the fix, `BridgeZX` got stuck at "Connecting"; after, it reaches "Waiting for files" with a working TCP server, and a file transfer completes successfully. Likely also fixes `SpecTalkZX` and `NetManZX`/`netman-zx`, not yet independently tested.
