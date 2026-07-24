**✅ Working — tested and running reliably on my setup**

This started as a proof of concept, but it's been running as my Thread Border
Router for a while now and does its job well: connecting Matter sensors over
Thread to Home Assistant. It's not a polished commercial product, and how
smoothly setup goes for you depends a lot on your network (IPv6 routing, NAT66,
and VM networking all matter — see [Network Notes](#network-notes)), but the
firmware itself has proven stable in day-to-day use.

**Honest limitations to be aware of:**
- Uses the ESP32-C6's internal PCB antenna — RF range is limited, so keep it 
  close to both your WiFi router and your Thread devices. I'll try the external
  antenna variant if I find the time.
- WiFi and Thread share a single RF path — fine for polling sensors, but worse
  than a dedicated two-chip border router and not built for heavy load.
- Tested primarily on the author's own setup, so expect to do some network
  tweaking to fit yours.

**AI disclosure:** I'm just an enthusiast, not an embedded systems expert. 
This project was built with very heavy assistance from 
Claude Sonnet 4.6, including the firmware patches, troubleshooting, and this
writeup. The AI was essential  — I could not have done this without it. Use 
the code accordingly and please improve on it!

# ESP32-C6 Thread Border Router for Home Assistant

Turn a $5 XIAO ESP32-C6 into a fully functional Thread Border Router that connects Matter/Thread devices (like the IKEA Alpstuga air quality sensor) to Home Assistant — with no additional hardware required.

![ESP32-C6 Thread Border Router](https://img.shields.io/badge/ESP32--C6-Thread%20Border%20Router-blue)
![Home Assistant](https://img.shields.io/badge/Home%20Assistant-Matter%2FThread-green)
![License](https://img.shields.io/badge/license-CC0-lightgrey)

## What this does

- Runs a **Thread Border Router** on the ESP32-C6's built-in 802.15.4 radio
- Connects to your **WiFi network** (no USB data connection needed — power only)
- Exposes an **OTBR-compatible REST API** on port 8080 so Home Assistant can discover and use it
- Lets you commission and control **Matter over Thread** devices from Home Assistant

## Hardware required

- [Seeed Studio XIAO ESP32-C6](https://www.seeedstudio.com/Seeed-Studio-XIAO-ESP32C6-p-5884.html) (~$5-$10)
- USB-C cable for data and power (charge-only cable is fine after flashing)
- A PC/Mac/Linux machine for flashing (one time only)

## Compatibility

Tested with, and in continuous use on:
- IKEA Alpstuga (Matter over Thread air quality sensor)
- Home Assistant running in a VM (with bridged networking)
- pfsense router (see [Network Notes](#network-notes) for IPv6 requirements)

Home Assistant auto-discovers the border router over mDNS, the Thread network
attaches and stays attached, and Matter commissioning works end-to-end. Should
work with any Matter over Thread device.

## Prerequisites

- Home Assistant with the **Matter** integration and **Matter Server** add-on installed
- A router that supports IPv6 (required for Thread/Matter)
- **NAT66 must be disabled** on your router — Matter commissioning requires real IPv6 end-to-end connectivity

---

## Step 1 — Install ESP-IDF

ESP-IDF is Espressif's development framework. You only need to do this once.

### Windows

1. Download and run the [ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/)
2. During installation, select **ESP32-C6** as the target chip
3. Or clone manually:
   ```cmd
   git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   install.bat esp32c6
   ```

### Linux / macOS

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
```

> **Important:** This project is developed and verified on ESP-IDF **v5.5.4**. The version
> isn't magic — the real rule is to run that version's `install.sh esp32c6` and source its
> `export.sh` in a **fresh terminal**. The "other versions break the toolchain" symptom is
> just having multiple ESP-IDF checkouts sourced in one shell. (It was originally built on
> v5.4.2; v5.5.x is the current in-service line and ships ~2 years of OpenThread/coexistence
> fixes. Any in-service 5.5.x should work.)

---

## Step 2 — Set up the environment

Every time you open a new terminal you must activate the ESP-IDF environment first.

### Windows
```cmd
cd path\to\esp-idf
export.bat
```

### Linux / macOS
```bash
cd path/to/esp-idf
. ./export.sh
```

---

## Step 3 — Copy the project files

Navigate to the border router example:

```
cd examples/openthread/ot_br
```

Copy the following files from this repository into the example folder:

| File | Destination |
|------|-------------|
| `partitions_4mb_otbr.csv` | `examples/openthread/ot_br/` |
| `sdkconfig.defaults` | append to `examples/openthread/ot_br/sdkconfig.defaults` |
| `main/esp_ot_br.c` | replace `examples/openthread/ot_br/main/esp_ot_br.c` |
| `main/CMakeLists.txt` | replace `examples/openthread/ot_br/main/CMakeLists.txt` (adds `esp_http_server` to the example's requires) |

> **For sdkconfig.defaults:** Open the existing file and paste the contents of this repo's `sdkconfig.defaults` at the **bottom**. Do not replace the whole file.

---

## Step 4 — Configure WiFi and target

Set the build target:
```
idf.py set-target esp32c6
```

Open the configuration menu:
```
idf.py menuconfig
```

Navigate to **Example Configuration** and set your WiFi SSID and password.

Also verify:
- `Component config → OpenThread → Thread Core Features → Thread 15.4 Radio Link` → **Native 15.4 radio**
- `Serial flasher config → Flash size` → **4 MB**

Save with **S**, quit with **Q**.

---

## Step 5 — Build and flash

Connect your XIAO ESP32-C6 via USB, then:

### Windows
```cmd
idf.py build flash -p COM11 monitor
```
(replace `COM11` with your actual COM port — check Device Manager)

### Linux / macOS
```bash
idf.py build flash -p /dev/ttyACM0 monitor
```
(The XIAO ESP32-C6 uses native USB-Serial-JTAG, so on Linux it's usually `/dev/ttyACM0`, **not** `/dev/ttyUSB0`. Check `ls /dev/ttyACM*` and replace if needed.)

The build takes ~10 minutes the first time. After flashing, watch the monitor output. After ~20 seconds you should see the device connect to WiFi and get an IP address.

---

## Step 6 — Verify the REST API

Once connected to WiFi (check your router's DHCP list for the ESP's IP), open these URLs in your browser:

```
http://<ESP_IP>:8080/node
```
Should return: `{"State":4}`

```
http://<ESP_IP>:8080/networks/dataset/active
```
Should return: `{"ActiveDataset":"0e08..."}`

If both work, your border router is ready.

### Reading logs over WiFi

The device mirrors its recent console output into an in-RAM ring buffer, readable
without a serial cable:

```
curl http://<ESP_IP>:8080/logs
```

This returns the last ~80–100 log lines (oldest first) as plain text — useful for
checking on the board remotely. Note it is a snapshot, not a live stream: re-run
the command to refresh. The buffer is RAM-only, so it resets on reboot, and an
idle/attached border router is legitimately quiet (OpenThread logs at WARN).

There is also a heap diagnostic (each Thread/Matter device the router advertises
onto the LAN costs a little RAM, so this shows how much headroom is left):

```
curl http://<ESP_IP>:8080/heap
```

Returns `{"FreeHeap":...,"MinFreeHeap":...,"LargestFreeBlock":...}` in bytes;
`MinFreeHeap` is the low-water mark since boot.

---

## Step 7 — Configure Home Assistant

### Add the Thread network dataset

1. In HA go to **Settings → System → Thread**
2. Your ESP border router should appear — click it and add the Thread dataset from `http://<ESP_IP>:8080/networks/dataset/active`

### Commission your Matter device

1. Install the **Home Assistant Companion app** on your phone
2. Go to **Settings → Companion app → Troubleshooting → Sync Thread credentials**
3. Go to **Settings → Devices & Services → Add Integration → Matter**
4. Scan the QR code on your device or enter the setup code (only got it working using the code for now)
5. Follow the prompts — the device will join the Thread network and appear in HA

---

## Network Notes

Thread/Matter requires proper **end-to-end IPv6 connectivity**. Several common network setups need extra configuration:

### NAT66
**Disable NAT66** on your router. Matter commissioning uses direct IPv6 and will fail if packets are being NAT'd.

### Home Assistant in a VM
Make sure the VM uses **bridged networking** (not NAT). The VM must be on the same network segment as the ESP border router.

### Route to the Thread (OMR) prefix
The border router advertises a route to its **OMR prefix** (a ULA /64 it generates, e.g. `fd20:1f93:f982:1::/64`) on WiFi via an RFC 4191 Route Information Option in its RAs. Hosts that honor RIOs (modern Linux, incl. most HA hosts on the same LAN) install this route automatically — you can confirm with `ip -6 route` (you'll see `<omr-prefix> via fe80::… proto ra`). If your router or controller is on a **different segment**, or doesn't honor RIOs, add a static IPv6 route:

- **Destination:** your OMR prefix (find it via `ip -6 route` on a LAN host, or it's the non-default route the ESP advertises)
- **Gateway:** the ESP's link-local IPv6 address (in your router's neighbor table)

> The OMR prefix is auto-generated and *can* change if the Thread network re-forms; a hard-coded static route can go stale. OpenThread keeps it stable in practice.

### pfsense specific
1. Add a static IPv6 gateway pointing to the ESP's link-local address
2. Add a firewall rule on LAN (above the default IPv6 rule) routing eg `fd55:ec6e:b588::/48` via the ESP gateway
3. Disable NAT66

---

## Troubleshooting

### WiFi won't connect
- Check SSID and password in menuconfig
- The onboard antenna is small — keep the ESP within 5m of your router (or closer, same with the thread device!)
- Signal below -85 dBm causes instability with Thread coexistence

### Build fails with "Tool doesn't match supported version"
You have multiple ESP-IDF versions installed. Always open a **fresh terminal** and run `export.bat` / `. ./export.sh` from your **cloned v5.5.4** folder before running any `idf.py` command. (Each ESP-IDF version ships its own matched toolchain + Python venv; run that version's `install.sh esp32c6` once.)

### Build fails with "esp_http_server.h: No such file or directory"
The REST API in `esp_ot_br.c` depends on the `esp_http_server` component. This repo's `main/CMakeLists.txt` (which you copy over the example's) already lists it in `PRIV_REQUIRES`. If you skipped that file, add `esp_http_server` to the `PRIV_REQUIRES` list in `examples/openthread/ot_br/main/CMakeLists.txt`, then rebuild.

### LAN IPv6 misbehaves when the ESP is powered on
The border router emits IPv6 Router Advertisements on WiFi (this is normal and required — it's how Thread devices become reachable). Its RAs carry **Router Lifetime 0**, so it does **not** become your LAN default gateway and cannot hijack your default route. What it *does* add is a route to its Thread prefix (a Route Information Option). If your network misbehaves, fix it at the router, not in firmware:
- Disable **NAT66**.
- Ensure only **one** device advertises the default route / your main prefix (your real router). The ESP isn't one of them, but a misconfigured second router could be.
- If hosts don't pick up the route to the Thread prefix automatically, add a static route (see [Network Notes](#network-notes)).

(Older firmware shipped an "RA suppression" task to address this; it was removed — it didn't stop the RAs and it broke Thread→LAN connectivity. See [How it works](#how-it-works).)

### Matter commissioning fails with "PASESession timed out"
- Check that NAT66 is disabled
- Verify the static route to the Thread prefix is in place
- Check firewall rules aren't blocking IPv6 between HA and the Thread prefix
- Make sure HA has a working IPv6 address and default gateway

### "No Thread border router" error during commissioning
- Verify `/node` returns `{"State":4}` — if State is 1, Thread hasn't attached yet, wait 30 seconds and try again
- Do **Settings → Companion app → Troubleshooting → Sync Thread credentials** on your phone before each commissioning attempt

### Pairing hangs at "Connecting to Thread network" (first device works, next doesn't)
The border router mirrors every Thread device's Matter service onto the LAN via
mDNS, and each device costs one mDNS slot **per fabric** it is joined to (Home
Assistant + Apple/Google = 2–3 slots each), plus a temporary `_matterc._udp` slot
while its commissioning window is open. Once `CONFIG_MDNS_MAX_SERVICES` is
exhausted, the next device's SRP registration is rejected and pairing hangs —
typically right after one pairing succeeded. Rebooting the border router clears
stale entries and frees a slot or two, which is why the problem appears to be
intermittent. Fix: raise `CONFIG_MDNS_MAX_SERVICES` (128 in the current
`sdkconfig.defaults`) and reflash. Confirm by checking `http://<ESP_IP>:8080/logs`
for "Cannot add more services" while a pairing is stuck, and watch heap headroom
via `http://<ESP_IP>:8080/heap` (`MinFreeHeap` is the low-water mark since boot).

### ESP crashes after ~30 seconds (SW_CPU reset)
Not observed with the current firmware. ESP-IDF's `esp_openthread_border_router_init()` already enables the OpenThread routing manager via the supported path, so don't call `otBorderRoutingSetEnabled()` yourself — a duplicate/early call (before the infra netif is up, or without holding the OpenThread lock) is the likely cause of the historical crash. The current code uses the stock high-level bring-up (`esp_openthread_start()` → `esp_openthread_border_router_start()`) and does not call it.

---

## How it works

The ESP32-C6 contains both a 2.4GHz WiFi radio and an 802.15.4 Thread radio on the same chip, sharing a single RF path. The ESP-IDF `ot_br` example connects these together to form a border router.

On top of the base example, this project adds:

1. **OTBR-compatible REST API** — Home Assistant's Matter integration expects specific endpoints (`/node`, `/networks/dataset/active`) that the base example doesn't provide. We add a minimal HTTP server implementing these endpoints.

2. **4MB partition table** — The default partition layout assumes 8MB+ flash. This custom layout fits everything into 4MB by removing OTA and RCP update slots.

> **Note — no "RA suppression":** earlier versions of this firmware ran a task that
> deleted the border router's default-route on-mesh (OMR) prefix, believing it stopped
> Router Advertisements from "breaking" the LAN. That was removed. The OpenThread routing
> manager (which ESP-IDF enables automatically) advertises RAs with **Router Lifetime 0** —
> the device is never your LAN's default gateway — and the OMR prefix it publishes is exactly
> the route your Thread/Matter devices need to reach the LAN/internet. Deleting it was both
> ineffective (RAs are still sent) and harmful (it broke Thread→LAN routing). Handle any real
> LAN-side IPv6 conflict at the router (see [Network Notes](#network-notes)), not in firmware.

---

## Known limitations

- **Single RF path (officially "unstable"):** WiFi and Thread share one 2.4 GHz radio, time-sliced via software coexistence — the chip can't receive both at once. Espressif's own coexistence matrix rates *WiFi-STA + Thread-Router* (what a border router is) as **C1 / unstable**, and recommends a dual-chip design (e.g. ESP32-S3 host + ESP32-H2 RCP) for a "real" border router. No config fixes this — it's inherent. **Fine for sensor polling (Matter air-quality/contact sensors); not for high-bandwidth or many chatty devices.** Keeping your 2.4 GHz WiFi channel away from the Thread channel helps.
- **No RCP auto-update:** Disabled to save flash space.
- **No web GUI:** Disabled to save flash space.
- **WiFi credentials in NVS:** Stored in flash. The backup `.bin` file contains your credentials — don't share it.

---

## Backup and restore

To back up the full flash (including WiFi credentials and Thread dataset):
```
python -m esptool --chip esp32c6 -p <PORT> read_flash 0x0 0x400000 backup.bin
```

To restore:
```
python -m esptool --chip esp32c6 -p <PORT> write_flash 0x0 backup.bin
```

> Keep your backup `.bin` private — it contains your WiFi password.

---

## Contributing

It works well for sensor use, but there's plenty of room for improvement. Known areas:

- ESPHome integration (would make deployment much easier, that would be nice)
- Automatic IPv6 route advertisement to upstream routers
- OTA update support

Pull requests welcome!

---

## Credits

Built with [ESP-IDF](https://github.com/espressif/esp-idf) and the `openthread/ot_br` example as a base. Thanks to the Home Assistant and OpenThread communities.

---

## License

The modifications in this repository are released under CC0 (public domain), matching the license of the original Espressif example code.
