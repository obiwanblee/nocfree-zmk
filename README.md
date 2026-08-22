# NocFree "&" — ZMK port

A [ZMK](https://zmk.dev) firmware port for the **NocFree &** split keyboard
(nRF52833 halves), including a **custom BLE-HOG → USB dongle bridge** so the
board keeps all three of its output modes under ZMK: wired USB, direct
Bluetooth, and a 2.4 GHz dongle.

> **This is the ANSI layout only.** The keymap was recovered from an ANSI
> unit's own stock firmware and covers that key set. NocFree also ship **ISO,
> JP and KR** variants — those have different key counts and positions, and KR
> adds a fourth I²C expander at `0x21` for an extra row, which this port does
> not read at all. On a non-ANSI board, expect keys in the wrong place or dead.
> The numpad accessory is not supported either.

> ## ⚠️ Use entirely at your own risk
>
> This is a personal hobby project shared as-is. **I provide no warranty, no
> guarantee that it works, and no support of any kind.** I am under no
> obligation to respond to issues, questions, or pull requests, and nothing
> here is promised to be maintained, correct, or safe for your hardware.
>
> Flashing custom firmware **can permanently brick your keyboard or dongle**
> (the dongle especially — it has no buttons and no reset pinhole; see
> [`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md)). You alone are responsible
> for anything that happens to your devices, your data, or anything else. If
> you are not comfortable recovering a bricked nRF52 device yourself, **do not
> flash this.**
>
> Not affiliated with, endorsed by, or supported by the manufacturer. All
> trademarks belong to their respective owners. By using anything in this
> repository you accept full responsibility for the outcome. See the
> [MIT license](LICENSE) — note in particular the "AS IS", NO-WARRANTY clause.

## Capabilities

Everything below is implemented and running as a daily driver.

### Connectivity

| Capability | Detail |
|---|---|
| Three output modes | Physical slide switch selects **USB** / **Bluetooth** / **2.4 GHz**, decoded from the stock two-GPIO encoding |
| Vendor switch behaviour | Warm reboot on a mode change, matching stock — a live re-select leaves host links up and starves the dongle of a slot |
| "Switch is law" | BT and 2.4 GHz never silently fall back to USB HID just because a charge cable is plugged in |
| Bluetooth | Direct BLE HID, multi-profile, with one profile reserved for the dongle |
| 2.4 GHz | Custom BLE-HOG → USB dongle bridge (see below) |
| Split link | Left is the split central, right the peripheral, at the 7.5 ms BLE floor |
| Latency tuning | Split latency 1 and a dongle-enforced 15 ms host link, so right-half keys can't be reordered behind left-half ones |

### Keyboard

| Capability | Detail |
|---|---|
| Full keymap | 96 matrix positions across Win / Fn / Mac / Recover layers, **ANSI only** — decoded from the stock firmware's own keymap, so key-for-key it matches what the board shipped with |
| Function keys | The top row is the vendor's media row by default (brightness, backlight, transport, volume), exactly as stock: **`Fn` + top row gives F1–F12.** The keycaps are labelled F1–F12, but stock sends media unless `Fn` is held, and this port keeps that behaviour |
| ZMK Studio | Live keymap editing on the left half over USB (Fn+Home to unlock) |
| Media keys | Consumer HID, forwarded over the dongle as well as BT/USB |
| Key scanning | Three PCA9555 I2C expanders, polled, on a **dedicated scan thread** so BLE traffic can never delay a scan |
| Backlight | Left→right sync over the split link, brightness + on/off, idle-off after 5 min |

### Power

| Capability | Detail |
|---|---|
| Deep sleep | SYSTEM OFF after 30 min on battery; wake on any keypress via the expanders' shared interrupt |
| Soft-off | Fn+B hold (~2 s) for a deliberate power-down |
| Session-aware idling | The central pushes activity to the peripheral, so both halves idle and sleep together instead of the right half dropping mid-session |
| Battery | Stock-style percentage curve; the host is shown the weaker of the two halves |

### Reliability

These exist because each one was a real failure that took a while to pin down:

| Capability | Detail |
|---|---|
| Stuck-key prevention | The scan runs on its own thread, so a busy system workqueue can't make it miss a key release — and can't cascade into a BLE supervision timeout |
| Serialized HID forwarding | The dongle's report queue is drained under a lock, so concurrent BLE/USB callbacks can't drop the final key release of a burst |
| No silently dropped releases | The left's BLE-HID send used to discard a report on any radio-fade error — if that report was a key release, the key stuck. It now retries until delivered and counts every incident on the console (`HOGDROP`) |
| Boot-time settings GC | A settings-store page erase (~85 ms, atomic) can never fit between 7.5 ms radio events, so mid-session garbage collection could stall the firmware mid-typing. Each boot now rotates the sector while the radio is still off, so it never has to happen mid-session |
| Advertising watchdog | ZMK restarts advertising only from BLE events; one failed start otherwise leaves the radio silently dead forever. A watchdog forces it back |
| Bond-deadlock self-heal | A dongle restart used to deadlock pairing until a power cycle; the left now reopens the slot on its own within ~30–60 s |
| Dongle link recovery | Verified subscriptions (no "connected but dead"), stale-connection isolation, and exponential backoff on radio resets |
| Recovery path | 1200-baud CDC touch into the UF2 bootloader on **all three** devices — the only way back in on hardware with no buttons |

### Build safety

| Capability | Detail |
|---|---|
| Trial-first ladder | Trial images carry an auto-DFU timer **and** a pre-kernel hardware watchdog, so even a hang before init returns the device to the bootloader by itself |
| Gated keeper builds | Keeper builds refuse to produce an image that is missing a load-bearing option, or whose local ZMK patches silently reverted |

## The dongle bridge (the interesting part)

ZMK is **BLE-only** — it has no support for the proprietary 2.4 GHz (Nordic
ESB) protocol the stock dongle uses, so the stock dongle can't talk to a ZMK
keyboard at all. This port solves that with a bridge instead:

```
right half  --BLE split-->  left half  --BLE HOG-->  dongle  --USB HID-->  host
(peripheral)                (central +              (BLE central +
                             HID peripheral)         HID-over-GATT client)
```

The dongle runs custom firmware (`dongle_bridge/`) as a **BLE HOG central**: it
connects to the left half (which advertises as a BLE HID peripheral), subscribes
to its HID reports, and re-presents them to the host as USB HID. So "2.4 GHz
mode" is really just another BLE link that happens to live in the 2.4 GHz band —
but it keeps the split intact and gives you a plug-and-go USB receiver.

This is **not** ZMK's standard "dongle-as-split-central" topology; the left
stays the split central and the dongle is a *second* BLE central hanging off it.
See [`docs/BRIDGE.md`](docs/BRIDGE.md).

## Repository layout

| Path | What |
|------|------|
| `flash.ps1` / `flash.sh` | One-command flasher — Windows / macOS-Linux (no Python) |
| `firmware/` | Prebuilt keeper UF2s (flash without building) |
| `config/` | ZMK board definition (dts, keymap, Kconfig, board C sources) |
| `dongle_bridge/` | Standalone bridge firmware for the dongle |
| `patches/` | Local ZMK patches (applied to the `zmk/` tree at build time) |
| `tools/` | Build (Docker) and flash helpers |
| `docs/` | Bridge design + dongle flashing safety |
| `.github/workflows/` | ZMK GitHub Actions build for the two halves |

## Building

**The two halves** build with the standard ZMK GitHub Action (see
`build.yaml` / `.github/workflows/build.yml`) — fork this repo and let CI build
them, or build locally.

**The dongle bridge** is a separate Zephyr app and builds via the Docker
scripts in `tools/` (it is *not* built by the ZMK Action):

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace \
  zmkfirmware/zmk-build-arm:stable \
  bash tools/inner_build_dongle_bridge_trial.sh   # trial (safe: auto-DFU)
```

Local half builds use the same image with `tools/inner_build_left_keeper.sh` /
`inner_build_right_keeper.sh`. On Windows, run the Docker commands from
PowerShell (Git Bash mangles the `-w /workspace` path).

## Flashing

Prebuilt keeper firmware is in [`firmware/`](firmware/) — you don't have to
build anything to flash. All devices use the Adafruit nRF52 UF2 bootloader,
entered with a **1200-baud touch** (no buttons needed).

**Just run it with no arguments** and it walks you through the whole thing —
it shows which of your devices it can see, asks what to flash, and does them in
the right order. Nothing happens until you pick something, and `Q` quits.

```powershell
.\flash.ps1            # Windows -- stock PowerShell, no Python, no installs
```

```bash
./flash.sh             # macOS / Linux
```

```
  NocFree & -- ZMK firmware flasher
  ---------------------------------

  Connected devices
    Right half  running   (COM33)
    Left half   running   (COM35)
    Dongle      running   (COM31)

  What would you like to flash?
    1) Both halves      (right, then left -- the recommended order)
    2) Right half only
    3) Left half only
    4) Dongle           (brick risk -- read docs\DONGLE_SAFETY.md first)
    R) Re-scan devices
    Q) Quit             (nothing has been touched)
```

You can also name a target directly and skip the menu:

```powershell
.\flash.ps1 left
.\flash.ps1 right
.\flash.ps1 dongle     # requires typing a confirmation -- see the warning below
```

```bash
./flash.sh left        # on macOS/Linux, plug in only the device being flashed
```

Both flashers:

- refuse to copy onto a UF2 drive that doesn't identify itself as a NocFree
  board, so another UF2 device (a CircuitPython board, another keyboard) that
  happens to be mounted can't be overwritten by mistake;
- pick up a device that is **already** in the bootloader — the normal state
  after a cancelled attempt or a self-returning trial image;
- treat a copy error at the very end as success-pending-verification, because
  the board reboots the instant the last block lands and the volume disappears
  mid-write. Success is decided by the drive going away, not by the copy's
  return code.

> `flash.sh` was developed on Windows and is **untested on real Mac/Linux
> hardware** — watch it on first use. If it can't find the device or the
> volume, fall back to the fully manual method: trigger the bootloader with a
> 1200-baud open, then copy the file onto the drive that appears.
>
> ```bash
> # Linux:  stty -F /dev/ttyACM0 1200 hupcl
> # macOS:  stty -f  /dev/cu.usbmodemXXXX 1200
> cp firmware/nocfree_left.uf2 /path/to/NOCFREE_BOOT/
> ```
>
> On Linux the `hupcl` matters: the reset is triggered by the DTR line
> *dropping*, and DTR is only lowered on close when HUPCL is set. Without it
> the command sets a baud rate and nothing else happens.

Suggested order: **right → left → dongle**.

### Stuck? Hand it to an AI

[`docs/AI_HELPER_PROMPT.md`](docs/AI_HELPER_PROMPT.md) has a copy-paste prompt
that primes an assistant with this hardware's specifics *and* its guardrails —
including the counter-intuitive parts it would otherwise get wrong, like the
missing `hupcl`, why a copy error at the end usually means success, and why
"just replug the dongle" is the one thing not to do. There's a second, narrower
prompt for working out whether a dongle is actually bricked.

### ⚠️ Read this before flashing the dongle

The dongle can be **bricked permanently**: no buttons, no reset pinhole, so its
only way back is the software 1200-baud touch — and firmware that hangs before
USB enumerates never gets to offer it. `flash.ps1 dongle` / `flash.sh dongle`
therefore make you type a confirmation first.

**Dump and keep a backup of your own dongle's stock firmware before you flash
anything**, and follow the trial-first ladder in
[`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md) — trial images return
themselves to the bootloader, which is what makes a first flash survivable.

**You most likely cannot dump the stock firmware yourself**, so read
[`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md) before you decide. In short:
the stock firmware's 1200-baud touch starts the bootloader in *serial-only* mode
(a COM port, no drive), and that protocol has no read-back command. The
mass-storage drive — and the `CURRENT.UF2` read-back on it — only appear once
*this* firmware is installed, which is already too late to capture stock. So
your retreat has to be a vendor image you obtain separately, an SWD dump, or a
deliberate decision that flashing the dongle is one-way.

Not having a stock image doesn't make bricking more likely — the trial-first
ladder handles that, and needs no backup. It only decides whether you can go
*back*.

If you *do* get hold of a vendor image, restoring it is easy: they're ordinary
UF2s, so it's the same drag-and-drop as anything else here. Note that it's
one-way in practice — stock's bootloader is serial-only, so returning to this
firmware afterwards needs the `adafruit-nrfutil` route. Both directions are
written up in [`docs/DONGLE_SAFETY.md`](docs/DONGLE_SAFETY.md).

> Once you're on this firmware, `CURRENT.UF2` **is** available and worth keeping
> before you change anything. Never share one: it spans the settings partition,
> which holds your Bluetooth pairing keys and the addresses of paired devices.
> [`tools/uf2_rescue.py`](tools/uf2_rescue.py) converts a read-back back into a
> flashable image.

## Status

Daily-driver stable. Reverse-engineering notes and vendor firmware used during
development are intentionally not included in this repository.

## Credits

[NocFreeKB/NocFree-and-zmk](https://github.com/NocFreeKB/NocFree-and-zmk) — a
separate, independently written community ZMK port for this keyboard (by Jarrod
Cugley, MIT), hosted by NocFree alongside their hardware porting guide. It is
deliberately narrower in scope than this one, and it is worth reading: it is a
careful, well-documented piece of work.

Two of its findings are merged here, with thanks:

- **The split notify-drop.** ZMK's split peripheral dequeues the other half's
  complete key-state bitmap *before* calling `bt_gatt_notify()`, and only
  `LOG_DBG`s a failure — which is compiled out in a build with logging off. On a
  fading link the send fails and the state is silently lost; if it carried a
  release, that key stays held at the host. Verified independently against this
  codebase before adopting their deeper-TX-pool mitigation, and instrumented
  here so the remaining drops are no longer invisible.
- **Verifying the expander polarity registers.** Their PCA9555 scan driver reads
  those registers back after writing them, where this port only wrote. An ACKed
  I2C write proves the byte was accepted, not that the register holds it — and
  the failure mode is every idle key reading as pressed.

Their porting notes also corrected two hardware details here: `P0.09`/`P0.17` is
the red charge indicator rather than a general status LED (with a separate blue
status LED on `P0.10` this port had missed entirely), and the battery divider
scale factor is 130/100.

Built on [ZMK](https://zmk.dev) and [Zephyr](https://zephyrproject.org), which
carry their own licenses.

## License

The MIT license in [`LICENSE`](LICENSE) covers the original work in this
repository: the ZMK board definition, the dongle-bridge firmware, and the
tooling. It does **not** extend to ZMK or Zephyr, which remain under their own
licenses, nor to any vendor firmware — none of which is distributed here.
