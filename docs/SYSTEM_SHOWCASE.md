# System Showcase

This document demonstrates the runtime performance of the Smart Dehumidifier system, including Linux kernel event-driven logs and the cross-platform monitoring interface.

---

## 1. Real-Time Kernel Event Streaming (Terminal View)

The backend runs as a continuous systemd service (`smart-dehum.service`). Unlike traditional user-space polling, the system uses kernel `hrtimer` and `epoll_wait()`, drawing **0.0% CPU** during idle waiting periods.
<figure>
  <img src="/assets/real_time_shell_log.png" width="600" height="500">
</figure>  
Figure 1: System journal (`journalctl -u smart-dehum.service -f`) showcasing 1Hz kernel event push and 5-second ODE/MLP decision cycles.

---

## 2. iOS Telemetry Client Across Operating States

The system exposes a lightweight RESTful telemetry interface on port `8080`. Below are the three primary operational states displayed in the iOS monitor application:

### State A: Standby Mode (Moderate Humidity)
When the indoor relative humidity is below the auto-start threshold (`TARGET_RH_HIGH = 65%`) and no compressor action is needed, the system remains in standby mode.

<figure>
  <img src="/assets/app_standby.jfif" width="300" height="600">
</figure>
Figure 2: Standby state showing moderate humidity, predictive lookahead, and idle status.

---

### State B: Active Dehumidification (Power Up / Running)
When ambient humidity exceeds the threshold or the user triggers a manual override from the app, the servo fires a press sequence. Once the LM393 sensor verifies the power LED via kernel majority voting, the system transitions into the active dehumidification state.

<figure>
  <img src="/assets/app_power_up.jfif" width="300" height="600">
</figure>
Figure 3: Active dehumidification state showing real-time drop in predicted RH and operational timer.

---

### State C: Water Tank Full Alert (Hardware Fault / Safety Lock)
If the actuator attempts startup consecutive times without detecting the power LED, the system asserts the `is_water_full` state lock.

<figure>
  <img src="/assets/app_full_reservoir.jfif" width="300" height="600">
</figure>
Figure 4: Full water alert state locking further automated startups to prevent overflow.

- **Safety Mechanism**: Automatically disengages automated actuation routines to protect the compressor until the water tank is emptied and the user manually clears the state by pressing the startup button.

---

## 3. Telemetry Payload Example (`GET /status`)

```json
{
  "temp": 26.8,
  "rh": 69.2,
  "abs_h": 17.65,
  "dew_point": 20.6,
  "is_on": true,
  "is_water_full": false,
  "failed_startup_count": 0,
  "pred_abs_h_5m": 16.92,
  "pred_rh_5m": 65.4,
  "action_required": "NONE",
  "reason": "Dehumidifying (Predicted RH: 65%)"
}
```
