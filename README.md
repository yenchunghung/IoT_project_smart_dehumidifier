# Smart Dehumidifier System (Raspberry Pi 5)

A non-invasive, hardware-level automated dehumidifier controller built on Linux Kernel Platform Drivers, native Device Tree overlays, edge thermodynamic numerical modeling (ODE + light MLP), and an event-driven `epoll` architecture delivering 0.0% idle CPU usage.

The project serves to monitor the humidity of my bedroom and automatically turn on the dehumidifier if necessary.

---

## 🌟 What's New in v3.0 (Kernel Event-Driven Overhaul)

1. **Native Device Tree Auto-Probing**:
   - Replaced manual `insmod` steps with custom Device Tree Overlays (`.dtbo`). The actuator platform device is automatically created and probed at boot time by the firmware bootloader.
2. **Native SHT3x Kernel Driver Integration**:
   - Replaced user-space raw I2C polling with the standard Linux `hwmon` SHT3x driver bound via I2C device tree overlays.
3. **Event-Driven Push Engine (`hrtimer` + `epoll`)**:
   - The kernel driver periodically samples sensors (1Hz) via `work_struct` and streams a compact 16-byte binary packet (`dehum_event_packet`) to `/dev/dehum_actuator`.
   - The C++ runtime sleeps completely on `epoll_wait()`, reducing user-space monitoring CPU usage.
4. **Kernel-Space Hardware Debouncing**:
   - Implemented a 25-sample majority-voting debounce filter directly inside the kernel bottom-half task to eliminate PWM LED flicker and ambient noise without blocking user-space threads.
5. **Non-Blocking Asynchronous State Machine**:
   - Replaced blocking `sleep_for` delays during startup with timestamp deadlines, ensuring continuous HTTP telemetry serving and zero thread-locking.

---

## System Architecture
- **Hardware Layer**: Raspberry Pi 5 (RP1 I/O), SHT30 I2C sensor, MG90S servo, LM393 light sensor.
- **Linux Kernel Space**: SHT3x Driver (hwmon subsystem), Device Tree Overlays (dehum-actuator.dtbo & sht3x.dtbo), dehum_cdev.ko (hrtimer 1Hz, 25-sample debounce, wait_queue).
- **User-Space Engine**: DehumHardware (epoll_wait listener), thermodynamic modelling (dew-point calculations), ODE + light MLP forward inference to anticipate humidity 5 minutes ahead. 
- **API**: Built-in HTTP server listening on port 8080 (`/dehum_status.json` and `/trigger`).

---

## Customization & Hardware Tuning

Before building, customize the following operational parameters to fit your specific dehumidifier model and servo mounting:

### 1. Servo Calibration (`kernel_driver/dehum_cdev.c`)
Because mechanical tolerances and 3D printed brackets vary, adjust the servo pressing angle:
```cpp
// In kernel_driver/dehum_cdev.c:
#define PULSE_STANDBY_US 600 // Pulse sent for zero
#define PULSE_PRESS_US 900   // Pulse sent for pressing
```

### 2. Humidity Thresholds & Run-Time Safety (`src/main.cpp`)
```cpp
constexpr double TARGET_RH_HIGH   = 65.0; // Auto startup trigger (% Relative Humidity)
constexpr double TARGET_RH_LOW    = 60.0; // Auto shutdown target (% Relative Humidity)
constexpr int MIN_RUN_TIME_SEC    = 60;   // Compressor short-cycle protection (minimum run time)
constexpr int MIN_REST_TIME_SEC   = 60;   // Compressor cooling protection (minimum standby time)
```

### 3. Light Sensor Sensitivity & Debounce (`kernel_driver/dehum_cdev.c`)
If the power LED on your dehumidifier uses high-frequency PWM dimming:
```cpp
// In kernel_driver/dehum_cdev.c -> sample_work_func():
int TOTAL_SAMPLES = 25;  // Number of samples (sampled every 12ms, covering ~300ms)
int THRESHOLD = 3;       // Minimum low-level triggers to classify as ON
```

## Installation & Deployment Guide

This project follows the standard embedded Linux workflow: **Host Machine (WSL2/PC)** handles code management and cross-compilation, while the **Target Device (Raspberry Pi 5)** handles kernel driver execution and hardware I/O.

---

### 1. Build User-Space Application (Cross-Compilation on WSL2/Host)
1. Clone the repository on the development host
2. If compiling from an x86_64 host targeting the Raspberry Pi 5 (ARM64), use the provided toolchain file:
```bash
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake ..
make -j$(nproc)
```
3. Deploy driver files, service configs, and the compiled binary to Raspberry Pi:
```bash
# In the project root directory:
scp -r kernel_driver <rpi_name>@<rpi_ip>:~/
scp -r systemd <rpi_name>@<rpi_ip>:~/
scp build/smart_dehumidifier <rpi_name>@<rpi_ip>:~/smart_dehumidifier_bin
```

### 2. Device Tree & Kernel Driver Setup (Raspberry Pi 5)
SSH into your Raspberry Pi:
1. Compile and install Device Tree Overlays:
```bash
cd ~/kernel_driver
# Compile overlays
dtc -@ -I dts -O dtb -o dehum-actuator.dtbo dehum-actuator-overlay.dts
dtc -@ -I dts -O dtb -o sht3x.dtbo sht3x-overlay.dts

# Copy to boot firmware directory
sudo cp dehum-actuator.dtbo /boot/firmware/overlays/
sudo cp sht3x.dtbo /boot/firmware/overlays/
```
2. Enable Overlays in `/boot/firmware/config.txt`:
```bash
sudo nano /boot/firmware/config.txt
# Append the following lines at the bottom:
dtoverlay=dehum-actuator
dtoverlay=sht3x
```
3. Compile and install the kernel character device driver:
```bash
cd ~/kernel_driver
make
sudo cp dehum_cdev.ko /lib/modules/$(uname -r)/kernel/drivers/misc/
sudo depmod -a
echo "dehum_cdev" | sudo tee /etc/modules-load.d/dehum.conf
```
4. Reboot the Raspberry Pi to apply boot-time overlays and drivers:
```bash
sudo reboot
```
5. **Verification**:
   After reboot, verify that both the character device node and the SHT3x hwmon sensor driver are loaded properly:

```bash
   # 1. Verify actuator character device node
   ls -l /dev/dehum_actuator

   # 2. Verify SHT30 hardware monitoring driver binding
   cat /sys/class/hwmon/hwmon*/name | grep sht3x

   # 3. Test binary packet stream (outputs one 16-byte packet per second)
   sudo hexdump -n 16 -C /dev/dehum_actuator
```
Expected packet layout (16 Bytes, packed):

* `0x00 - 0x07`: `__u64` Timestamp (ns)

* `0x08 - 0x0B`: `__s32` Temperature (m°C, e.g., `0x00006590` = 26000 = 26.0°C)

* `0x0C - 0x0F`: `__u32` Relative Humidity (m%, e.g., `0x000109A0` = 68000 = 68.0%)

* `0x10`: `__u8` Debounced light indicator state (`01` = ON, `00` = OFF)


### 3. Service Daemon Setup on Raspberry Pi 5
1. Install binary to system path:
```bash
sudo cp ~/smart_dehumidifier_bin /usr/local/bin/smart_dehumidifier
sudo chmod +x /usr/local/bin/smart_dehumidifier
```
2. Register & start systemd background service:
```bash
sudo cp ~/systemd/smart-dehum.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now smart-dehum.service
```
#### 3. Monitor real-time logs:
```bash
journalctl -u smart-dehum.service -f 
```

**(Optional)** 
If you do not use WSL2/PC cross-compilation, you can clone the repository directly onto the Raspberry Pi, compile the kernel module inside `kernel_driver/`, and compile the application via `cmake .. && make -j$(nproc)` natively without the toolchain file.
