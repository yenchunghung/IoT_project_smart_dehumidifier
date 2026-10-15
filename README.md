# Smart Dehumidifier System (Raspberry Pi 5)

A non-invasive, driver-backed automated dehumidifier control system integrating a Linux kernel character device, edge numerical modeling (ODE & lightweight MLP), and a RESTful HTTP telemetry API.

## System Architecture
- **Hardware Layer**: Raspberry Pi 5 (RP1 I/O), SHT30 I2C sensor, MG90S servo, LM393 light sensor.
- **Kernel Layer (`dehum_cdev.ko`)**: Custom character device driver providing atomic microsecond PWM pulses (`600µs`/`900µs`) to prevent servo jitter, with status reading on `/dev/dehum_actuator`.
- **User-Space Engine**: C++ predictive engine using thermodynamic dew-point calculations, ODE trends, and MLP forward inference to anticipate humidity 5 minutes ahead. Includes a 7-sample majority-voting debounce filter for indicator light stability.
- **API**: Built-in HTTP server listening on port 8080 (`/dehum_status.json` and `/trigger`).

## Build & Deploy
```bash
# Build via CMake
mkdir -p build && cd build
cmake ..
make -j$(nproc)
