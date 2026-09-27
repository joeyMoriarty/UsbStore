# UsbStore

**A pocket file server made from an ESP32-S3 and a pile of pendrives.**

Plug ordinary USB pendrives into a hub, plug the hub into a thumb-sized
ESP32-S3 Super Mini, and every device on your WiFi can browse, download and upload files
from a web page. No PC, no Raspberry Pi, no SD card. It runs on its own,
updates itself over WiFi, and recovers by itself from a bad firmware update.

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
                 │                     └─ pendrive   │
                 └───────────────────────────────────┘
```

## Why this one is different

If you have tried to turn an ESP32-S3 into a NAS and got stuck, you probably hit
one of these. This project gets past all of them, and documents how:

- **Real USB drives, several at once.** Most ESP32 file servers use an SD card.
  This one runs the S3 as a **USB host**, with an **external hub**, mounting
  each pendrive as its own volume (`/usb0`, `/usb1`, …) with hot-plug.
- **The one-USB-port problem, solved.** The S3 has a single USB port. The
  moment it hosts your hub, you lose USB flashing *and* the serial console.
  So after the first flash, everything happens over WiFi:
  - a setup access point for first-time configuration,
  - **OTA firmware updates with automatic rollback**,
  - live logs in the browser and over UDP,
  - panic backtraces saved to flash and shown in the UI after the reboot.
- **Built on ESP-IDF 6.1.** IDF 6 moved the USB host stack and mDNS out of the
  core framework and dropped the bundled cJSON, so code written for IDF 5.x
  needs changes before it builds. This targets 6.1 directly.
- **The power trap, documented.** The most common reason a setup like this
  "sees nothing" isn't code. See [Hardware and power](#hardware-and-power-read-this-first).

## Features

- Web file browser: drive tabs, folders, download, upload with progress, new
  folder, delete
- Multiple pendrives through a USB hub, hot-plug in and out
- First-run **setup access point** — no credentials in the source code
- Reachable at **http://usbstore.local** (mDNS)
- **OTA updates** from the web page, with **rollback**: a new build runs on
  probation for a minute and reverts on its own if it crashes
- **Admin password** protecting every write action and firmware updates
- Remote debugging without a serial adapter: live log, UDP log broadcast,
  crash dumps to flash, and a **USB bus census** listing every device the S3
  can see

## Measured performance

Measured on this build, over WiFi, downloading a 4.8 MB file from a SanDisk
pendrive through the web UI (median of 3 runs, all byte-exact):

| | |
|---|---|
| **Read** | **520 KB/s** measured |
| Write | ~350 KB/s (Espressif's figure — not yet measured here) |

That read figure sits right at Espressif's own bench number for the bus alone
(~540 KB/s), so the web server and WiFi add almost nothing. **The ceiling is the
chip:** the ESP32-S3's USB is Full Speed only (USB 1.1, 12 Mbit/s), and every
drive on the hub shares it.

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
- SanDisk Cruzer Blade 16 GB + SanDisk 3.2 Gen1 128 GB, both mounted at once
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

### Drives

- **FAT32 only.** ESP-IDF's FatFs is built without exFAT. Large drives work —
  the 128 GB SanDisk above is FAT32 — but Windows' built-in formatter won't
  offer FAT32 above 32 GB, so use a third-party formatter.
- FAT32 caps a single file at **4 GB**.
- The firmware **never formats** a drive it can't mount. Reformat on a PC.
- **At most 2 drives through a hub** — a hard limit of the chip, measured, not
  estimated. The S3 has **8 USB host channels**, and each open pipe holds one
  for as long as the device is attached: the hub takes 2, each pendrive 3
  (control + bulk in + bulk out). Two drives fill all 8, so a third can't even
  enumerate — the log shows `HCD DWC: No more HCD channels available`. A single
  drive plugged straight into the S3 (no hub) uses 3.
- Drives are numbered `/usb0`, `/usb1`, … in **plug-in order**, so numbers can
  swap after a replug.

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

A panic kills the network stack before anything can be sent, so crashes are
written to a flash partition and reported on the next boot.

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
  delete, new folder, changing WiFi, clearing the crash dump, and **firmware
  updates** — an open write API would let anyone on your WiFi reflash the
  board.
- Over plain HTTP the password stops a guest on your WiFi, not someone
  capturing traffic.
- Every path is decoded, then validated against the live mounts before any
  filesystem call — `..` is rejected outright, including when disguised as
  `%2E%2E`.
- File and drive names are HTML-escaped in the UI: they come from whoever wrote
  the pendrive, and an unescaped crafted filename could otherwise run script in
  your browser while the admin password is cached.

## HTTP API

| Method | Route | Auth | Does |
|---|---|---|---|
| GET | `/api/status` | — | WiFi, version, drives, USB bus census |
| GET | `/api/list?p=/usb0/dir` | — | directory listing (streamed) |
| GET | `/api/dl?p=/usb0/file` | — | download |
| POST | `/api/ul?p=/usb0/dir/file` | ✔ | upload (raw body) |
| POST | `/api/mkdir?p=…` | ✔ | new folder |
| POST | `/api/rm?p=…` | ✔ | delete file or *empty* folder |
| GET | `/api/log?since=N` | — | log tail; next cursor in `X-Log-Cursor` |
| GET | `/api/crash` | — | last panic: task, PC, backtrace |
| POST | `/api/ota` | ✔ | firmware update (raw `.bin` body) |
| POST | `/api/wifi` | ✔* | set WiFi (+ first admin password) |
| POST | `/api/passwd` | current pw | set or change the admin password |

\* Open only until an admin password exists.

## Roadmap: toward a pendrive RAID

The groundwork for a real multi-drive NAS is already here: several drives
mounted at once, per-drive volumes, and hot-plug events. What's next, in order
of usefulness:

The S3's channel budget (see [Drives](#drives)) allows exactly **two** drives
open at once — which happens to be exactly what RAID 1 needs.

1. **Stable drive identity.** Mount by USB serial number or volume label
   instead of plug order, so `/usb0` is always the same stick. Every RAID mode
   below needs to know which physical drive is which.
2. **RAID 1 — mirroring (the valuable one).** Write every file to two
   pendrives, so either can die without data loss. ESP-IDF 6's MSC driver
   exposes each drive as an `esp_blockdev` block device, so this can be done
   properly at the block level: a mirrored block device that fans writes out
   to both drives and reads from either, with FAT mounted on top. Then:
   - **degraded mode** — keep serving when one mirror is missing,
   - **resync** — rebuild a replaced drive in the background,
   - **scrub** — periodically read both copies and compare.
3. **Park-and-swap mounting, for more than two drives.** Keep every drive
   enumerated but hold only one drive's bulk pipes open at a time, remounting
   on demand when you switch drives. The channel budget then fits up to four
   drives on the hub — at the cost of never having two open together, which
   rules out mirroring between them.
4. **JBOD / spanning.** Pool several drives into one namespace, for capacity
   rather than safety.
5. **Checksums.** Store a hash per file and verify on read, so silent
   corruption on cheap flash gets caught, not copied to the mirror.

**Why not RAID 0 (striping)?** On this chip it buys nothing: every drive shares
a single 12 Mbit/s bus, so splitting a file across two drives doesn't make it
any faster. Parity RAID (RAID 5) is possible in principle across 3 drives, but
the same bus limit and the S3's channel budget make it slow and fragile.

**Where striping does start to make sense: ESP32-P4.** The P4 has a High-Speed
(480 Mbit/s) USB host — forty times the bandwidth — and more host channels, so
more drives open at once. Porting this project to the P4 is the path to a
genuinely fast, multi-drive pendrive NAS.

Also planned:
- **WebDAV**, so the box mounts as a network drive in Windows, macOS and Linux
- Resumable uploads (`Range` / `Content-Range`)
- Hold-BOOT-to-reset the admin password
- HTTPS
- USB device mode — appearing as a drive to a PC (needs mode switching: two
  masters on one FAT table corrupts it)

## Project layout

| File | Does |
|---|---|
| `main/main.c` | start order, heartbeat, OTA confirmation |
| `main/usb_storage.c` | USB host + MSC, hub, bus census, mount table, path validation |
| `main/wifi_mgr.c` | NVS credentials, station with setup-AP fallback, mDNS |
| `main/web_server.c` | HTTP routes, hand-rolled JSON, path decoding |
| `main/auth.c` | admin password (NVS), Basic auth gate |
| `main/ota.c` | firmware upload, image checks, rollback confirmation |
| `main/netlog.c` | log ring buffer, UDP broadcast |
| `main/www/index.html` | the web UI, embedded into flash |
| `partitions.csv` | two 1.875 MB OTA slots + core dump, in 4 MB |
| `sdkconfig.defaults` | hub support, console routing, PSRAM, rollback, FAT volumes, sockets |
| `tools/idf.ps1` | build from plain PowerShell on Windows |

## Design notes

- **Hub support is a compile-time flag** (`CONFIG_USB_HOST_HUBS_SUPPORTED`).
  Without it a hub enumerates but its ports are invisible — it looks exactly
  like a dead hub.
- **One FAT volume per drive** (`CONFIG_FATFS_VOLUME_COUNT=4`). The FatFs
  default of 2 would cap the box at two drives on its own; it's raised so the
  firmware is ready for park-and-swap, where the USB channel budget (2 open
  drives) rather than FatFs becomes the only limit.
- **PSRAM is quad, not octal.** The R2 part is 2 MB quad SPI; selecting octal
  breaks boot.
- **The console is on UART0 only.** IDF also enables a secondary console on USB
  Serial/JTAG by default, which shares the USB PHY with the OTG controller
  hosting the hub. It's switched off.
- **Every filesystem call is serialised** behind one lock, so a drive pulled
  mid-read can't be unmounted under an open file.
- **Failed uploads are deleted.** A half-written file that looks complete is
  worse than none.
- **Deletes are non-recursive.** No recursive delete is exposed over the
  network.
- **Refused uploads close the socket** instead of draining a body that could
  be gigabytes; the UI triggers the login prompt before a large transfer
  starts.

## License

MIT — see [LICENSE](LICENSE).
