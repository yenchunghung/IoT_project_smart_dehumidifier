# Smart Dehumidifier System (Raspberry Pi 5)

A non-invasive, driver-backed automated dehumidifier control system integrating a Linux kernel character device, edge numerical modeling (ODE & lightweight MLP), and a RESTful HTTP telemetry API.

## System Architecture
- **Hardware Layer**: Raspberry Pi 5 (RP1 I/O), SHT30 I2C sensor, MG90S servo, LM393 light sensor.
- **Kernel Layer (`dehum_cdev.ko`)**: Custom character device driver providing PWM pulses (`600µs`/`900µs`), with status reading on `/dev/dehum_actuator`.
- **User-Space Engine**: C++ predictive engine using thermodynamic dew-point calculations, ODE trends, and a light MLP forward inference to anticipate humidity 5 minutes ahead. Includes a 20-sample majority-voting debounce filter for indicator light stability.
- **API**: Built-in HTTP server listening on port 8080 (`/dehum_status.json` and `/trigger`).

## Build & Deploy

### 1. Build User-Space Application (Cross-Compilation on WSL2/Host)
If compiling from an x86_64 host targeting the Raspberry Pi 5 (ARM64), use the provided toolchain file:

```bash
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake ..
make -j$(nproc)
```

(Note: If compiling directly on Raspberry Pi 5, standard cmake .. && make -j$(nproc) works without the toolchain file).

### 2. Kernel Driver Setup (Raspberry Pi 5)
The system requires the custom character device driver to handle atomic servo PWM and indicator sensing.

On the Raspberry Pi:
```bash
cd driver/
make
sudo insmod dehum_cdev.ko
# Verify device node is created
ls -l /dev/dehum_actuator
```

### 3. Deploy & Run via systemd
#### 1. Transfer the compiled binary from host to Raspberry Pi:
```bash
scp smart_dehumidifier <user>@<rpi_ip>:~/smart_dehumidifier_bin
```
#### 2. On the Raspberry Pi, update the binary, and start the daemon:
```bash
sudo cp ~/smart_dehumidifier_bin /usr/local/bin/smart_dehumidifier
sudo chmod +x /usr/local/bin/smart_dehumidifier
sudo systemctl start smart-dehum.service
```
#### 3. Monitor real-time logs:
```bash
journalctl -u smart-dehum.service -f
```
