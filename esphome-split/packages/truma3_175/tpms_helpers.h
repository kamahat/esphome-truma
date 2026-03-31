#pragma once
#include "esphome.h"

inline void tpms_parse(
    const esphome::esp32_ble_tracker::ESPBTDevice &x,
    esphome::template_::TemplateSensor *bat,
    esphome::template_::TemplateSensor *temp,
    esphome::template_::TemplateSensor *pres)
{
  for (auto data : x.get_manufacturer_datas()) {
    if (data.data.size() < 3) continue;
    uint16_t mfr_id    = data.uuid.get_uuid().uuid.uuid16;
    float battery      = ((mfr_id >> 8) & 0xFF) * 0.1f;
    float temperature  = (float)data.data[0];
    uint16_t raw_pres  = (data.data[1] << 8) | data.data[2];
    float pressure_bar = (raw_pres / 10.0f - 14.696f) / 14.5038f;
    if (pressure_bar > 0.5f) {
      bat->publish_state(battery);
      temp->publish_state(temperature);
      pres->publish_state(pressure_bar);
    }
  }
}
