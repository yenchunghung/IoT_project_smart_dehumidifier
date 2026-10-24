# Hardware Setup
This document provides a detailed guide on the physical component connections, mechanical mounting of the non-invasive servo push mechanism, and optical sensor calibration for the Smart Dehumidifier system.

---

## 1. Bill of Materials (BOM)

| Component | Description | Interface / Pinout |
| :--- | :--- | :--- |
| **Raspberry Pi 5** | Host controller running Linux kernel platform driver | 40-Pin GPIO Header |
| **M5Stack ENV III Sensor** | Integrated Environmental Sensor (**SHT30** temperature/humidity + **QMP6988** pressure) | I2C1 (SDA: GPIO 2, SCL: GPIO 3) |
| **TowerPro MG90S** | Metal gear micro servo for button actuation | GPIO 18 |
| **LM393 Photoresistor** | Light sensor module with digital output (DO) | GPIO 23 |
| **Duct Tape** | Damage-free, tool-free mechanical mounting method | Fastens servo & probe onto the dehumidifier panel |
| **External 5V Power Supply** | Shared power supply powering Raspberry Pi 5 & Servo VCC | - |

---

## 2. Wiring & Pinout Configuration

To protect the Raspberry Pi from voltage drops and current spikes induced by servo motion, the MG90S servo is powered via an external breadboard rail sharing a **common ground** with the Raspberry Pi 5.

The peripherals are connected according to the following mapping:

| Peripheral | Peripheral Pin / Wire | Raspberry Pi 5 Header Pin | BCM GPIO | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **ENV III (SHT30)** | Red (VCC) | Pin 2 (5V) | - | 5V supply logic |
| | Black (GND) | Pin 6 (GND) | - | Common ground |
| | Yellow (SDA) | Pin 3 (SDA1) | GPIO 2 | Bound via `sht3x.dtbo` |
| | White (SCL) | Pin 5 (SCL1) | GPIO 3 | Bound via `sht3x.dtbo` |
| **MG90S Servo** | Yellow (Signal) | Pin 12 | GPIO 18 | Actuator pulse output |
| | Red (VCC) | Breadboard 5V Rail | - | Requires 5V supply |
| | Brown (GND) | Breadboard Common Ground | - | Shared ground rail |
| **LM393 Optical**| VCC | Pin 1 (3.3V) | - | 3.3V logic level |
| | GND | Breadboard Common Ground | - | Shared ground rail |
| | DO (Digital Out) | Pin 16 | GPIO 23 | Active-Low light signal |

> **Common Ground Reminder**: A ground **pin 9** from the Raspberry Pi is connected to the breadboard's ground rail to maintain a shared reference potential across the servo, light sensor, and logic lines.
---

## 3. Mechanical Mounting with Duct Tape

The device is mounted on the dehumidifier panel using directly duct tape.

![Hardware Assembly](../assets/hardware_setup.jpg)
*Figure 1: Non-invasive servo push mechanism and optical probe secured with duct tape.*
