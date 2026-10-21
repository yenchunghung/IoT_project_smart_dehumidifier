#ifndef _DEHUM_EVENT_H_
#define _DEHUM_EVENT_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <linux/types.h>
#include <stdint.h>
#endif

// 核心推播給 User Space 的二進位事件封包 (16 Bytes，記憶體緊湊對齊)
struct __attribute__((__packed__)) dehum_event_packet {
    __u64 timestamp_ns;     // 核心取樣時間戳 (納秒)
    __s32 temperature_mc;   // 溫度 (毫度攝氏，例如 24500 = 24.5°C)
    __u32 humidity_mp;      // 相對濕度 (千分比，例如 65200 = 65.2%)
    __u8  light_state;      // 光敏指示燈狀態 (1 = 亮, 0 = 熄)
    __u8  reserved[3];      // 對齊保留欄位
};

#endif
