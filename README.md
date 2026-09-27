# UsbStore

**A pocket file server made from an ESP32-S3 and a pile of pendrives.**

Plug ordinary USB pendrives into a hub, plug the hub into a thumb-sized
ESP32-S3 Super Mini, and every device on your WiFi can browse, download and
upload files from a web page. No PC, no Raspberry Pi, no SD card. It runs on
its own, updates itself over WiFi, recovers by itself from a bad firmware
update, and protects itself from overheating.

It is a small **LAN file-sharing box** — a private "cloud drive" for your home
network — not a Synology. It's honest about that below.

```
                 ┌──────────── your WiFi ────────────┐
   phone ─┐      │                                   │
   laptop ├──────┤   http://usbstore.local           │
   tablet ┘      │          │                        │
                 │   ┌──────┴───────┐  USB-C (OTG)   │
                 │   │ ESP32-S3     ├──────┐         │
                 │   │ Super Mini   │      │         │
                 │   └──────────────┘   USB hub      │
                 │                     ├─ pendrive   │
                 │                     ├─ pendrive   │
                 │                     └─ pendrive   │
                 └───────────────────────────────────┘
```

## Why this one is different

If you have tried to turn an ESP32-S3 into a NAS and got stuck, you probably hit
one of these. This project gets past all of them, and documents how:

- **Real USB drives, several at once.** Most ESP32 file servers use an SD card.
  This one runs the S3 as a **USB host** with an **external hub**, mounting
  each pendrive as its own volume with hot-plug.
- **More drives than the chip has room for.** The S3's USB controller can only
  keep two drives open behind a hub. **Park-and-swap** keeps up to four
  recognised and swaps them in on demand — see [Drives](#drives).
- **The one-USB-port problem, solved.** The S3 has a single USB port. The
  moment it hosts your hub, you lose USB flashing *and* the serial console.
  So after the first flash, everything happens over WiFi: a setup access point,
  **OTA updates with automatic rollback**, live logs in the browser, and panic
  backtraces saved to flash.
- **Built on ESP-IDF 6.1.** IDF 6 moved the USB host stack and mDNS out of the
  core framework and dropped the bundled cJSON, so code written for IDF 5.x
  needs changes before it builds. This targets 6.1 directly.
- **The power trap, documented.** The most common reason a setup like this
  "sees nothing" isn't code. See [Hardware and power](#hardware-and-power-read-this-first).
- **Written to be learned from.** [How it works](#how-it-works-tips-and-tricks)
  walks through the techniques that let a chip with ~2.5 MB of RAM do all this
  in plain C.

## Features

- Web file browser: a tab per drive, folders, download, upload with progress,
  new folder, delete
- **Concurrent transfers**: two downloads/uploads at once, and the page, log
  and temperature stay live while they run
- Up to 4 pendrives through a USB hub, hot-plug in and out, **park-and-swap**
  beyond two
- First-run **setup access point** — no credentials in the source code
- Reachable at **http://usbstore.local** (mDNS); reconnects on its own if the
  router drops it
- **OTA updates** from the web page with **rollback**
- **Thermal protection**: live chip temperature, throttling when warm, a safe
  unmount-and-sleep cool-down when hot
- **Admin password** protecting every write action and firmware updates
- Debugging with no serial adapter: live log, UDP log broadcast, crash dumps to
  flash, and a **USB bus census** listing every device the S3 can see

## Measured performance

Measured over WiFi, downloading a 4.8 MB file from a SanDisk pendrive through
the web UI (median of 3 runs, all byte-exact), CPU at the default 160 MHz:

| | |
|---|---|
| **Read** | **520 KB/s** measured |
| Write | ~350 KB/s (Espressif's figure — not yet measured here) |
| Switching to a parked drive | **~80 ms** measured (median of 9 swaps across 3 drives, including listing the folder) |

That read figure is ~96% of Espressif's bench number for the bus alone
(~540 KB/s), so the web server and WiFi add almost nothing. **The ceiling is the
chip:** the ESP32-S3's USB is Full Speed only (USB 1.1, 12 Mbit/s), and every
drive on the hub shares it. That's also why raising the CPU from 160 to 240 MHz
would buy at most ~4% — the processor isn't the bottleneck, the bus is — while
adding heat.

| File | Download | Upload |
|---|---|---|
| 10 MB | ~20 s | ~30 s |
| 700 MB | ~23 min | ~34 min |
| 4 GB | ~2 h 15 min | ~3 h 20 min |

Great for documents, photos, code, small backups and passing files between
devices. Not a media server.

## Hardware and power (read this first)

**Tested with:**
- ESP32-S3 Super Mini — ESP32-S3FH4R2, 4 MB flash, 2 MB **quad** PSRAM
- SanDisk Cruzer Blade 4 GB and 16 GB, SanDisk 3.2 Gen1 128 GB — all three at once
- A generic unpowered USB hub
- A fixed 5 V / 2 A phone charger

### What works

```
5 V charger ──► S3 [5V + GND pins]
                S3 USB-C ──(OTG adapter)──► unpowered hub ──► pendrives
```

Put 5 V into the S3's **`5V` pin**. On the Super Mini that pin is wired
straight through to the USB-C port's VBUS, so the S3 powers the hub and the
drives, and stays the USB host. (Check your board: if a diode blocks the 5V pin
from VBUS, the hub stays dark and you'll need a powered OTG "Y" adapter.)

### What doesn't — and what it looks like

These cost real hours. The symptom is the same for all of them: **the S3 sees
nothing on USB**, and the log shows no error, just silence.

| Wiring | What happens |
|---|---|
| Charger fed **into one of the hub's downstream ports** | The hub pushes that 5 V backwards out of its upstream port, into the host. A phone in the same setup won't even charge or show the drive. |
| S3 powered from **another board's 3.3 V pin** | Nothing supplies the 5 V the drives need. The hub's LED still lights (LEDs light at ~2.5 V) — misleadingly. Plugging a drive in then **browns out the S3** (reset reason 9). |
| USB-C **PD** charger with a sketchy adapter | If it negotiates 9 V+, that lands on the 5 V rail and kills the board instantly. Use a plain fixed-5 V charger. |

The firmware's **USB bus census** (on the web page, and in the log) is what
tells these apart from a software problem: an empty bus means no signal is
reaching the S3 at all — go check power and cables before touching code.

Two more rules: never feed power into a hub's downstream port, and don't power
the S3 from its 5V pin *and* plug its USB-C into a PC at the same time.

**It runs warm, and that's the regulator.** The hottest part of the board is
the small 5 V → 3.3 V linear regulator near the USB-C port. It burns the
difference as heat — about (5 − 3.3) V × the board's current, 0.25–0.4 W with
WiFi active, in a package the size of a grain of rice. Hot to the touch is
normal for it. Don't enclose the board without airflow.

## Getting started

### Build

Needs **ESP-IDF 5.3 or newer** (external hub support); built and tested on
**v6.1**.

```sh
idf.py set-target esp32s3     # once - a wrong target is baked into build/ and sdkconfig
idf.py build
```

If you edit `sdkconfig.defaults`, delete `sdkconfig` before building — defaults
only apply when `sdkconfig` is generated fresh.

`main/idf_component.yml` pulls three components from the ESP Component Registry:

| Component | Why |
|---|---|
| `espressif/usb_host_msc` | USB mass-storage class driver |
| `espressif/usb` | the USB Host Library itself (left IDF core in 6.0) |
| `espressif/mdns` | `http://usbstore.local` |

**Windows note:** if you installed ESP-IDF through the VS Code extension and
`export.ps1` fails to find its Python environment, `tools/idf.ps1` sets the
environment up directly (`.\tools\idf.ps1 build`). Its paths match a default
extension install of v6.1 — edit them for yours.

### First flash (USB, hub unplugged)

```sh
idf.py -p COMx flash
```

Once this firmware runs, it takes the USB port over for the hub, and the
board's USB serial port disappears from your PC. That's expected. To ever flash
over USB again, hold **BOOT** while plugging the board in. You normally won't
need to — updates go over WiFi.

### First run

1. The board raises a WiFi access point: **`usbstore-setup`**, password
   `usbstore123`.
2. Join it and open **http://192.168.4.1**.
3. Enter your WiFi details **and a new admin password**. It reboots onto your
   network.
4. Open **http://usbstore.local** (or the IP shown in the log).

Your router may hand the board a different LAN IP after a lease renewal;
`usbstore.local` follows it automatically. Android doesn't always resolve
`.local` names — if yours doesn't, reserve a fixed IP for the board's MAC
address in your router's DHCP settings. (Your ISP changing your *public* IP
doesn't matter at all: nothing here uses it.)

## Drives

- **FAT32 only.** ESP-IDF's FatFs is built without exFAT. Large drives work —
  the 128 GB SanDisk above is FAT32 — but Windows' built-in formatter won't
  offer FAT32 above 32 GB, so use a third-party formatter.
- FAT32 caps a single file at **4 GB**.
- The firmware **never formats** a drive it can't mount; its tab turns red and
  says so. Reformat on a PC.
- Drives are numbered `/usb0`, `/usb1`, … in **plug-in order**, so numbers can
  swap after a replug.

### Park-and-swap: up to four drives

The S3's USB controller has **8 host channels**, and every open data pipe
holds one for as long as the device is attached. Measured on this hardware:

| | channels |
|---|---|
| The hub | 2 |
| Each drive, just recognised | 1 (control pipe) |
| Each drive, mounted | +2 (bulk in + bulk out) |

Two mounted drives plus the hub is 2 + 2 + 4 = **8 of 8**, and a third drive
can't even enumerate — the log says `HCD DWC: No more HCD channels available`.

So drives have three states:

| State | Channels | Meaning |
|---|---|---|
| **open** | 3 | mounted, ready |
| **parked** | 1 | recognised, not mounted — dashed tab |
| **failed** | 1 | couldn't mount (not FAT32) — red tab |

With **1–2 drives** both may be open. With **3–4**, one is open at a time, and
clicking a parked drive's tab swaps it in — measured at ~80 ms, including listing the folder. A drive is never
parked in the middle of a transfer: every request holds a reference on the
drive it uses, and only drives with no references can be parked. If a swap
needs the only open drive while a long transfer is using it, the tab says
*busy* instead of cutting the transfer off; quick operations (listings,
deletes) are simply waited for.

To keep a channel free for the *next* plug-in, a drive only opens by itself
when nothing else is open, and when two are open the less recently used one is
parked after 15 s idle. If you plug a drive in within 15 s of using two others
and it doesn't appear, unplug and replug it.

## Thermal protection

The web page shows the chip's temperature, and the firmware acts on it:

| Chip temperature | What happens |
|---|---|
| below 70 °C | normal |
| **70 °C** | **throttle, stay online**: CPU 160 → 80 MHz, WiFi radio power saving. Transfers get slower; everything keeps working. Back to normal below 62 °C. |
| **85 °C** | **cool-down**: transfers stop, every drive is **unmounted** (so FAT is never left half-written), the USB bus is suspended, WiFi is turned off, and the chip **light-sleeps** in 15 s rounds until it's below 60 °C — then reboots cleanly. The page says what happened afterwards. |

The reading is the chip's **die** temperature from its built-in sensor. It
can't see the voltage regulator, which is the hottest part of the board — but
throttling cuts the current the regulator has to carry, so it cools that too.
For protection keyed to the regulator itself, a thermistor taped to it is on
the roadmap.

**Test it without heating anything:** *Chip health → Test cool-down* runs the
real 85 °C sequence for 30 s. Measured on the tested board: **30 s of light
sleep took the chip from 64.2 °C to 35.2 °C**, and it came back with all three
drives.

**Measured under load:** a long transfer took the chip from 57 °C to 70 °C in
about a minute; throttling engaged by itself, held it at 71–76 °C for the rest
of the transfer, and released at 61 °C once it ended. Throttling slows the rise
but doesn't reverse it during a long transfer — in a warm room or a closed case,
the 85 °C cool-down is what would stop it.

## Data safety

- **Reading never writes.** FatFs doesn't update access times, so browsing and
  downloading can't damage a drive.
- **Writes are the risky moment.** FAT32 has no journal: losing power or
  pulling a drive mid-upload or mid-delete can leave a half-written file or
  orphaned clusters. Failed uploads are deleted automatically, and every file
  is closed after each operation, so **pulling a drive while it's idle is
  safe.**
- **Unstable power is the real-world risk.** A brownout during a write is how
  FAT gets damaged — see [Hardware and power](#hardware-and-power-read-this-first).
- **One pendrive is not a backup.** That's what RAID 1 on the roadmap is for.
  If a drive was pulled mid-write, run `chkdsk X: /f` on a PC.

## Updating over WiFi

Web page → **Firmware → Upload firmware** → `build/usbstore.bin`.

The new version boots **on probation** and is only confirmed after a minute of
running — long enough to cover mounting drives that were already plugged in,
the likeliest place for a bad build to crash. If it crashes or hangs first, the
next reset boots the previous version on its own. Power-cycling during that
minute also counts as a failure, so give it the minute.

## Debugging without a serial adapter

| Where | Catches | Misses |
|---|---|---|
| **Console pane** in the web page (`/api/log`) | everything once WiFi is up | panics |
| **UDP broadcast**, port 9999 | the same, from any machine | panics |
| **Crash banner** (`/api/crash`, core dump in flash) | panics, with backtrace | — (shown after the reboot) |
| **USB bus** line on the page | every USB device the S3 can see | hubs (handled inside the USB stack) |

Watch the UDP log from Windows, no netcat needed:

```powershell
$u = New-Object System.Net.Sockets.UdpClient 9999
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ($true) { [Text.Encoding]::UTF8.GetString($u.Receive([ref]$ep)) -replace "`0" }
```

Turn a backtrace into line numbers against the exact build that crashed:

```sh
xtensa-esp32s3-elf-addr2line -pfiaC -e build/usbstore.elf 0x42008abc 0x42009def
```

Want a real serial console anyway? A spare ESP32 DevKit is a USB-serial
adapter: hold its `EN` pin to `GND`, wire the S3's **GPIO 43** to the DevKit's
**`TX0`** plus a common ground, and open the DevKit's COM port at 115200.

## Security model

- **LAN only.** Plain HTTP. Do not port-forward it.
- **Reads are open** on your network: browsing, downloading, status, log.
- **Writes need the admin password** (HTTP Basic, user `admin`): upload,
  delete, new folder, changing WiFi, clearing the crash dump, the cool-down
  test, and **firmware updates** — an open write API would let anyone on your
  WiFi reflash the board.
- Over plain HTTP the password stops a guest on your WiFi, not someone
  capturing traffic.
- Every path is decoded, then validated against the live drives before any
  filesystem call — `..` is rejected outright, including when disguised as
  `%2E%2E`.
- File and drive names are HTML-escaped in the UI: they come from whoever wrote
  the pendrive, and an unescaped crafted filename could otherwise run script in
  your browser while the admin password is cached.

## HTTP API

| Method | Route | Auth | Does |
|---|---|---|---|
| GET | `/api/status` | — | WiFi, version, temperature, drives (with `state`), USB bus census |
| GET | `/api/list?p=/usb0/dir` | — | directory listing (streamed); swaps a parked drive in |
| GET | `/api/dl?p=/usb0/file` | — | download |
| POST | `/api/ul?p=/usb0/dir/file` | ✔ | upload (raw body) |
| POST | `/api/mkdir?p=…` | ✔ | new folder |
| POST | `/api/rm?p=…` | ✔ | delete file or *empty* folder |
| GET | `/api/log?since=N` | — | log tail; next cursor in `X-Log-Cursor` |
| GET | `/api/crash` | — | last panic: task, PC, backtrace |
| POST | `/api/ota` | ✔ | firmware update (raw `.bin` body) |
| POST | `/api/thermaltest` | ✔ | run the cool-down sequence for 30 s |
| POST | `/api/wifi` | ✔* | set WiFi (+ first admin password) |
| POST | `/api/passwd` | current pw | set or change the admin password |

\* Open only until an admin password exists.

## How it works: tips and tricks

The ESP32-S3 has 512 KB of on-chip RAM, 2 MB of external PSRAM and a dual-core
processor running at 160 MHz. There's no Linux — just FreeRTOS, a small
real-time scheduler — and the whole firmware is ~1 MB of C. These are the
techniques that make it enough, most of them learned the hard way on this
project.

### 1. Split the work into tasks, and never block in a callback

Every job has its own FreeRTOS task:

| Task | Job |
|---|---|
| `usb_host` | pumps the USB Host Library's event loop |
| `usb_census` | a watcher client that sees every USB device come and go |
| MSC driver | the mass-storage class driver's own events |
| `usb_mount` | mounts and unmounts drives, fed by a queue |
| `usb_idle` | parks the idle drive when two are open |
| `thermal` | reads the temperature, throttles, runs the cool-down |
| `httpd` | the web server: pages, status, listings |
| `xfer` × 2 | downloads and uploads, handed off by the web server |
| `netlog` | broadcasts the log over UDP |

The USB tasks and the transfer workers are pinned to **core 1**; ESP-IDF runs
WiFi on **core 0**, so the radio never shares a core with USB traffic.

Callbacks — USB events, the log hook — run on *someone else's* task, so they
only copy a few bytes into a queue or ring buffer and return. The slow work
(mounting a drive, sending a packet) happens in a task that owns it. Blocking
inside a callback is the classic way to deadlock an embedded system.

### 2. More tasks, not more CPU — and count who's using what

The first version had one lock held for the whole of every file operation, and
ESP-IDF's web server runs every handler on a single task. Simple and safe — and
during a five-minute download the page stopped answering entirely. The board
still replied to ping in 1 ms; only the web task was stuck.

The fix wasn't more processing power: both cores were mostly idle, one waiting
on the USB bus. It was **more tasks**:

- Long transfers are handed to two **worker tasks** with
  `httpd_req_async_handler_begin()`, and the web task goes straight back to
  serving the page. The scheduler runs them on different cores.
- Instead of one global lock, each drive has a **reference count**. A request
  takes a ref while it uses a drive; only drives with no refs can be parked or
  unmounted. Unplugging a drive mid-transfer marks it gone, and the *last*
  user's release does the unmount — never under an open file.
- FatFs already locks each volume internally (`FF_FS_REENTRANT`), so two
  requests on one drive are safe without any extra locking.
- Each worker has its **own** 8 KB buffer: a buffer shared between tasks is a
  corruption bug waiting to happen.

Measured after the change: status answers in ~70 ms *during* a download, and
two downloads run at once.

### 3. Stream, never buffer

A 4 GB file never touches RAM. Every transfer moves through one 8 KB buffer, and
directory listings go out as chunked JSON while the directory is still being
read. Memory use is independent of file size and folder size — that's how a
chip with ~2.5 MB of RAM serves multi-gigabyte files.

### 4. Big buffers in PSRAM, internal RAM for the radio

`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` puts the transfer buffer and the
16 KB log ring in external PSRAM, leaving the faster internal RAM for WiFi and
DMA, which need it. The PSRAM must be configured as **quad** on this R2 part —
selecting octal breaks boot.

### 5. Ship the web page inside the firmware

`EMBED_FILES` in CMake compiles `index.html` into the binary. There's no
filesystem for assets, nothing to upload separately, and an OTA update replaces
the UI and the firmware together.

### 6. Write your own JSON (it's 30 lines)

IDF 6 dropped the bundled cJSON. The responses here are flat objects plus one
streamed list, so a small escaping function and `snprintf` do the job — and,
unlike building a cJSON tree, it streams.

### 7. Budget the hardware first, then design around it

Park-and-swap came from one log line: `No more HCD channels available`. The S3
has 8 USB host channels; count what each device holds and the limit falls out:
hub 2 + 1 per recognised drive + 2 per mounted drive. The design (only mount
what you're using) follows directly. Measure the constraint before you design
the workaround.

### 8. USB data toggles — the bug that looked like a dead drive

Every USB bulk packet carries an alternating DATA0/DATA1 bit. When a drive is
parked, the host frees its end of the connection — but the **drive keeps its
own toggle state**. On re-open, the host starts at DATA0, the drive expects
DATA1, and it silently discards the first command as a duplicate. The symptom
was "opens once, then every re-open times out".

The fix is one standard request: `CLEAR_FEATURE(ENDPOINT_HALT)` on both bulk
endpoints resets the drive's toggle to DATA0 *even when nothing is halted*
(USB 2.0 §9.4.5), matching the fresh host pipe.

A trap next to it: the MSC driver's `msc_host_reset_recovery()` looks like the
right tool but is only safe on a genuinely stalled drive. On a healthy one it
halts the host pipe and returns before un-halting it, adding ~10 s of timeouts
to every call. Reading the driver source found both — when the log says
"timeout", the answer is usually in someone else's code.

### 9. When your only USB port is taken, the network is your debugger

- `esp_log_set_vprintf()` hooks every log line into a 16 KB ring buffer, served
  over HTTP and broadcast over UDP. The hook runs inside whoever logged, so it
  never allocates, never blocks, and never logs.
- A panic kills the network before anything can be sent, so **core dumps go to
  a flash partition** and the next boot shows the backtrace.
- `esp_reset_reason()` is gold: reset reason **9 (brownout)** is what proved
  the "no drives" problem was power, not code.
- The **USB bus census** answers "is this hardware or software?" in one line.

### 10. OTA that can't brick itself

Two firmware slots, bootloader rollback, and a **60-second probation** before a
new image is confirmed — long enough to cover mounting the drives already
plugged in. A bad build that crashes at boot is rolled back automatically, so
you can never lock yourself out of the only way in.

### 11. Decode, *then* validate; escape on output

The browser sends paths URL-encoded (`/usb0` arrives as `%2Fusb0`). ESP-IDF's
query parser doesn't decode, so every path failed validation until decoding was
added — *before* the check, so a disguised `%2E%2E` is caught as `..`. On the
way out, every filename is HTML-escaped: a pendrive can contain anything.

### 12. Kconfig is part of your code

Each of these cost debugging time, and none shows up as a compile error:

| Setting | Why |
|---|---|
| `idf.py set-target esp32s3` | the default is the classic ESP32, which has no USB at all |
| `CONFIG_USB_HOST_HUBS_SUPPORTED` | without it a hub enumerates but its ports are invisible |
| `CONFIG_FATFS_VOLUME_COUNT=4` | the default 2 caps you at two mounted drives |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | the default secondary console shares the USB PHY with the hub |
| `CONFIG_LWIP_MAX_SOCKETS=16` | the default 10 is used up by the web server + log socket |
| `CONFIG_PM_ENABLE` | needed to change the CPU clock at runtime for throttling |
| `format_if_mount_failed = false` | otherwise an exFAT drive gets "fixed" by erasing it |

And: `sdkconfig.defaults` only applies when `sdkconfig` is generated fresh.
Edit it and forget to delete `sdkconfig`, and your change is silently ignored.

### 13. Measure before you optimise

Reading 520 KB/s against a 540 KB/s bus ceiling says the CPU isn't the
bottleneck — so running at 160 MHz instead of 240 costs nothing and runs
cooler. The same habit put the thermal limits on the chip's own sensor while
being honest that the regulator is the real hot spot.

### 14. Survive a reboot with RTC memory

After a cool-down the board reboots, but the page still explains what happened.
The record lives in `RTC_NOINIT_ATTR` memory, which survives `esp_restart()`
but is garbage after power-on — so it's guarded by a magic number and only
trusted after a software reset.

### 15. Retry forever, with backoff, from a timer

If the router drops the connection, the board reconnects on its own, waiting
2, 4, 8 … up to 30 s between attempts. The wait lives in a one-shot
`esp_timer`, because blocking inside the WiFi event handler would stall every
other event.

## Roadmap: toward a pendrive RAID

The groundwork is here: several drives at once, per-drive volumes, hot-plug,
and park-and-swap. What's next, in order of usefulness:

1. **Stable drive identity.** Mount by USB serial number or volume label
   instead of plug order, so `/usb0` is always the same stick. Every RAID mode
   below needs to know which physical drive is which.
2. **RAID 1 — mirroring (the valuable one).** Write every file to two
   pendrives, so either can die without data loss. It fits the S3 exactly: two
   drives open at once is its limit. ESP-IDF 6's MSC driver exposes each drive
   as an `esp_blockdev` block device, so it can be done properly at the block
   level — a mirrored block device fanning writes out to both drives, FAT on
   top. Then **degraded mode** (serve with one mirror missing), **resync**
   (rebuild a replaced drive in the background) and **scrub** (compare both
   copies periodically).
3. **JBOD / spanning.** Pool drives into one namespace, for capacity rather
   than safety.
4. **Checksums.** A hash per file, verified on read, so silent corruption on
   cheap flash is caught rather than copied to the mirror.
5. **Photo backup.** WebDAV support, so phone apps like PhotoSync or FolderSync
   can auto-upload a camera roll, plus a gallery view built from the small
   thumbnail most phone JPEGs already carry in their EXIF data — no image
   decoding on the chip.
6. **Regulator temperature.** A 10 kΩ NTC thermistor taped to the voltage
   regulator, on a spare ADC pin, so thermal protection measures the actual
   hot spot.

### Why not RAID 0 (striping)?

**On the S3 it buys nothing.** Measured reads already run at ~96% of the single
12 Mbit/s bus, and every drive shares that bus through the hub — striping a
file across two drives splits the same bandwidth. It would also need every
drive open at once (park-and-swap can't give that beyond two), and losing
*either* drive loses *everything*.

**On an ESP32-P4 it would help — until the network gets in the way.** The P4's
USB is High-Speed (480 Mbit/s), so striping two slow pendrives could roughly
double write speed. But the P4 has **no built-in WiFi**: it needs an ESP32-C6
companion chip (a few MB/s) or wired Ethernet (~11 MB/s), and that becomes the
bottleneck. A P4 port with Ethernet is the realistic path to a fast pendrive
NAS; RAID 1 remains the mode worth building first on either chip.

Also planned: resumable uploads, hold-BOOT-to-reset the admin password, HTTPS,
and USB device mode (appearing as a drive to a PC — needs mode switching, since
two masters on one FAT table corrupts it).

## Project layout

| File | Does |
|---|---|
| `main/main.c` | start order, heartbeat, OTA confirmation |
| `main/usb_storage.c` | USB host + MSC, hub, bus census, park-and-swap, toggle reset |
| `main/thermal.c` | temperature sensor, throttling, light-sleep cool-down |
| `main/wifi_mgr.c` | NVS credentials, setup-AP fallback, reconnect with backoff, mDNS |
| `main/web_server.c` | HTTP routes, transfer workers, hand-rolled JSON, path decoding |
| `main/auth.c` | admin password (NVS), Basic auth gate |
| `main/ota.c` | firmware upload, image checks, rollback confirmation |
| `main/netlog.c` | log ring buffer, UDP broadcast |
| `main/www/index.html` | the web UI, embedded into flash |
| `partitions.csv` | two 1.875 MB OTA slots + core dump, in 4 MB |
| `sdkconfig.defaults` | every non-default setting, each with its reason |
| `tools/idf.ps1` | build from plain PowerShell on Windows |

## License

MIT — see [LICENSE](LICENSE).
