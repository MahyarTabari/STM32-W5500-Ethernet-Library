# AGENTS.md

STM32F401RCT6 + WIZnet W5500 over SPI1. STM32CubeIDE project (`w5500`), C/gnu11, no tests/CI/lint.
Primary artifact is a flashable `.elf`; "verification" means building and reading UART1 (115200 8N1).

## Build

- `make -C Debug all` — the **only** working build command.
- **Never run bare `make -C Debug`.** The default goal is `clean`, not `all`: the first rule
  make sees is the `clean:` rule from the first `-include`d `subdir.mk`, so a bare `make`
  silently deletes every build artifact. This is easy to get wrong because it *looks* like a
  successful build.
- `arm-none-eabi-gcc` must be on `PATH`. It is **not installed in this environment**, so builds
  cannot be verified locally — say so instead of claiming a change compiles.
- `Debug/makefile` hardcodes the linker script to the original author's machine:
  `/home/mahyar/STM32CubeIDE/workspace_1.14.0/w5500/STM32F401RCTX_FLASH.ld`.
  The link step fails on any other machine. The real script is `STM32F401RCTX_FLASH.ld` at the
  repo root; a makefile override is enough to fix it locally.
- `Debug/` is **committed to git and there is no `.gitignore`**: `.o`, `.d`, `.su`, `.cyclo`,
  `makefile`, `subdir.mk` are all tracked. `make -C Debug all` rewrites tracked files, so expect
  a noisy `git status` after building. Don't hand-edit them (CubeIDE regenerates them), and don't
  `git checkout -- Debug` to "clean up" unless you intend to discard a real build you made.
- Compile flags / include paths live in `Debug/*/subdir.mk`:
  `-mcpu=cortex-m4 -std=gnu11 -DSTM32F401xC -DUSE_HAL_DRIVER -mfpu=fpv4-sp-d16 -mfloat-abi=hard`,
  includes `../Core/Inc ../Core/Src/w5500 ../Core/Src/w5500/W5500` + HAL/CMSIS.
  Debug is `-O0 -Wall`, Release is `-Os`. **Adding a `.c` file means also adding it to the
  matching `Debug/.../subdir.mk`** (and `objects.list`) or it won't be compiled or linked.

## Architecture

- `Core/Src/w5500/` is the portable library; `Core/Src/` and `Core/Inc/` are CubeMX-generated
  scaffolding. The library is designed to be copied into other projects (see README), so it must
  not depend on `main.h`.
- Layering: `W5500/w5500.c` (register access) → `wizchip_conf.c` (dispatcher + I/O callback
  table) → `w5500_spi.c` (the actual SPI/GPIO callbacks) → `w5500_host_config.c` /
  `w5500_phy.c` (user-facing API). `socket.c` + `dhcp.c` are the socket/DHCP layer.
  `http_server.c` sits on top of `socket.c` and is also part of the portable library.
- `socket.c`, `dhcp.c`, `W5500/w5500.c` are vendored WIZnet ioLibrary files (BSD headers).
  `dhcp.c` already carries one local deviation from upstream (`dhcp_tick_1s` made `volatile`,
  documented in its own header) — re-syncing from upstream is not a clean overwrite.

## HTTP server (own module, `Core/Src/w5500/http_server.c`)

- `http_server_poll()` is the whole server. No interrupts, no threads, no `accept()`.
- The W5500 `INTn` line is **not wired**, so polling is the only completion signal.
- Two ioLibrary facts drive the design, and both are easy to get wrong:
  - A listening socket is *consumed* by the client that connects to it. There is no
    `accept()`, so each connection is closed and the socket re-opened afterwards.
  - `recv()` returns `SOCK_BUSY` (numerically **0**) when there is no data in
    non-blocking mode, so 0 means "nothing yet", not "peer gone". Liveness comes from
    `getSn_SR() == SOCK_CLOSED`. `send()` must be fed in chunks bounded by
    `getSn_TX_FSR()`.
- Responses are arrays of `seg_t` in flash, walked twice: once to count bytes (exact
  `Content-Length`, no chunked encoding) and once to stream. Per-connection values are
  addressed by `seg_kind_t` (e.g. `SEG_IP`), never by pointer, so one flash array serves
  every connection.
- `SEG_SNAP` fragments are appended to a per-connection string by the route handler and
  consumed in list order. **Adding a `SEG_SNAP` means adding a `snap_add()` in the same
  position** in `snapshot_dashboard()` / `snapshot_status()`, or fields render shifted.
- `seg_t.kind` is `uint16_t` on purpose. With `SEG_END = 0xFF` first in the enum every other
  kind is >= 256 and truncates to 0, which silently empties every response body.
- Socket budget is 4 (`HTTP_SERVER_MAX_CONN`); DHCP keeps socket 0, so HTTP uses 1..4.
  ~1.5 KB of static RAM per connection, ~6 KB total.
- `POST /api/net` defers the address write to `conn_recycle()`, i.e. until after the
  response body has drained. Writing `SIPR` under a live connection makes the browser see a
  reset instead of the confirmation, and it would then reload to a different URL.

## Portability coupling (things an agent will not see from filenames)

- `w5500_spi.c:1` and `w5500_phy.c:10` include `"stm32f4xx_hal.h"` directly. Retargeting to
  another MCU family means editing those includes.
- `w5500_spi.c` hardwires `extern SPI_HandleTypeDef hspi1;` and drives `hspi1.Instance`
  registers directly. The SPI handle is defined in `main.c`, and any SPI change must be made
  on both sides.
- Pin macros are in `w5500_spi.h`: `SCS_PIN` = PA1, `RESET_PIN` = PA0.
- `wizchip_conf.h` includes `"W5500/w5500.h"`, so `Core/Src/w5500` itself must stay on the
  include path. `_WIZCHIP_` = `W5500`, `_WIZCHIP_IO_MODE_` = `_WIZCHIP_IO_MODE_SPI_VDM_`.
- `w5500_phy.c` depends on `HAL_Delay`, so the library needs a running SysTick/HAL.

## printf goes to UART1 via newlib

- `main.c` overrides `__io_putchar` to busy-wait on `UART_FLAG_TXE` and write `huart1.Instance->DR`.
  `setbuf(stdout, NULL)` must come after `HAL_Init()`. No IT/DMA. This is why the library can
  `printf` without owning a console.
- `w5500_phy.c` alone accounts for ~29 `printf` calls; expect chatty UART output.

## Two manual edits that must live inside CubeMX `USER CODE` blocks

Both are required and are easy to lose when regenerating from `w5500.ioc`:

1. `__HAL_SPI_ENABLE(&hspi1);` at the end of `MX_SPI1_Init()` (`Core/Src/main.c`) — without it
   the SPI peripheral is not enabled and the W5500 never responds.
2. The 1000-tick divider calling `DHCP_time_handler()` in `SysTick_Handler()`
   (`Core/Src/stm32f4xx_it.c`). DHCP has no time source without it; the WIZnet code busy-waits
   on `dhcp_tick_1s`, so a missing tick handler hangs rather than failing cleanly.

## Init sequence blocks — silence on UART is usually not a crash

`main()` calls these in order, and two of them never return under normal bench conditions:

- `w5500_init()` (`w5500_spi.c`) — resets the chip, registers the CS/SPI/spi-burst callbacks,
  then `ctlwizchip(CW_INIT_WIZCHIP, ...)` with 2 KB TX + 2 KB RX per socket for 8 sockets.
  On failure it prints and spins in `while(1)`.
- `dynamic_host_configuration()` (`w5500_host_config.c`) — busy-loops `DHCP_run()` until
  `ip_assigned`, **with no timeout**. Note it also declares `uint8_t dhcp_buffer[1024]` as a
  *stack local* (1 KB of the F401RC's 64 KB RAM, `STM32F401RCTX_FLASH.ld` gives RAM as
  `0x20000000` + `64K`, not 32K) and the pointer handed to `DHCP_init()` dangles
  once the function returns. `static_host_configuration()` is currently commented out in
  `main.c`; `net_info` in `main.c` is the single source of MAC/IP for both paths.
- `check_cable_presence()` (`w5500_phy.c`) — loops with `HAL_Delay(1500)` until PHY link is up.

So "no UART output" typically means: no cable, or no DHCP server on the segment. Confirm the
link/DHCP state before suspecting the SPI wiring.

## Conventions

- Tabs in the W5500 port files, 4 spaces in the CubeMX-generated `Core/Src` files. Match the file
  you are editing; do not reformat generated files.
- Comments are explanatory about the W5500/HAL hardware, not restating the code. Keep that style.
- README.md documents the public API (`w5500_init`, `static_host_configuration`,
  `dynamic_host_configuration`, `check_cable_presence`, `check_phy_status`,
  `print_current_host_configuration`). Update it when the public API changes.
- No tests exist. Don't add a host test harness for hardware-dependent code; verify by build plus
  UART output on hardware.
