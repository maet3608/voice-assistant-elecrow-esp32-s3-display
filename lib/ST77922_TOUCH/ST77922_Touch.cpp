#include "ST77922_Touch.h"

ST77922_TOUCH::ST77922_TOUCH(void) {
  width = TOUCH_WIDTH;
  height = TOUCH_HEIGHT;
  rotation = 0;
  max_points = 0;

  i2c_config_t touch_i2c_cfg = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = TOUCH_SDA,
      .scl_io_num = TOUCH_SCL,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master = {
          .clk_speed = I2C_SPEED,
      },
      .clk_flags = 0,
  };
  i2c_param_config(static_cast<i2c_port_t>(I2C_NUM), &touch_i2c_cfg);
  i2c_driver_install(static_cast<i2c_port_t>(I2C_NUM), I2C_MODE_MASTER, 0, 0, 0);
}

void ST77922_TOUCH::init(void) {
  uint8_t data;
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, HIGH);
  pinMode(TOUCH_INT, INPUT);
  reset();

  do {
    Read_Data(STATUS, &data, 1);
  } while (data & 0x0F);

  Read_Data(MAX_TOUCHES, &data, 1);
  max_points = data;
  if (max_points > MAX_TOUCH_POINTS) {
    max_points = MAX_TOUCH_POINTS;
  }
}

void ST77922_TOUCH::reset(void) {
  TOUCH_RST_LOW;
  delay(100);
  TOUCH_RST_HIGH;
  delay(100);
}

void ST77922_TOUCH::Set_Rotation(uint8_t r) {
  rotation = r;
  switch (rotation) {
    case 0:
    case 2:
      width = TOUCH_WIDTH;
      height = TOUCH_HEIGHT;
      break;
    case 1:
    case 3:
      width = TOUCH_HEIGHT;
      height = TOUCH_WIDTH;
      break;
    default:
      break;
  }
}

void ST77922_TOUCH::Read_Data(uint16_t reg, uint8_t* rbuf, size_t rlen) {
  const uint8_t wbuf[2] = {
      static_cast<uint8_t>((reg >> 8) & 0xFF),
      static_cast<uint8_t>(reg & 0xFF),
  };
  i2c_master_write_read_device(static_cast<i2c_port_t>(I2C_NUM), TOUCH_ADDR,
                               wbuf, sizeof(wbuf), rbuf, rlen, 1000);
}

bool ST77922_TOUCH::Get_Touch(void) {
  uint8_t data[7 * MAX_TOUCH_POINTS] = {0};
  uint8_t update = 0;
  Read_Data(TOUCH_INFO, &update, 1);

  if (!(update & 0x08) || max_points == 0) {
    return false;
  }

  Read_Data(TOUCH_POINT0, data, 7 * max_points);
  for (uint8_t i = 0; i < max_points; i++) {
    if (data[i * 7] & 0x80) {
      touch.id[i] = (data[i * 7] & 0x80) >> 7;
      const uint16_t rawX = ((data[i * 7] & 0x3F) << 8) | data[i * 7 + 1];
      const uint16_t rawY = ((data[i * 7 + 2] & 0x3F) << 8) | data[i * 7 + 3];

      switch (rotation) {
        case 0:
          touch.x[i] = rawX;
          touch.y[i] = rawY;
          break;
        case 1:
          touch.x[i] = rawY;
          touch.y[i] = height - rawX;
          break;
        case 2:
          touch.x[i] = width - rawX;
          touch.y[i] = height - rawY;
          break;
        case 3:
          touch.x[i] = width - rawY;
          touch.y[i] = rawX;
          break;
      }
    }
  }

  return true;
}