#ifndef _ST77922_TOUCH_H_
#define _ST77922_TOUCH_H_

#include "Arduino.h"
#include "driver/i2c.h"
#include "hal/gpio_ll.h"

#define TOUCH_ADDR 0x55
#define I2C_NUM 0
#define I2C_SPEED 100000

#define TOUCH_WIDTH  320
#define TOUCH_HEIGHT 480
#define MAX_TOUCH_POINTS  10

#define STATUS       0x0001
#define MAX_TOUCHES  0x0009
#define TOUCH_INFO   0x0010
#define TOUCH_POINT0 0x0014

#define TOUCH_SCL  (GPIO_NUM_39)
#define TOUCH_SDA  (GPIO_NUM_38)
#define TOUCH_RST  48
#define TOUCH_INT  47

#if ((TOUCH_RST >= 0) && (TOUCH_RST < 32))
    #define TOUCH_RST_LOW  GPIO.out_w1tc = (1 << TOUCH_RST)
    #define TOUCH_RST_HIGH GPIO.out_w1ts = (1 << TOUCH_RST)
#elif (TOUCH_RST >= 32)
    #define TOUCH_RST_LOW  GPIO.out1_w1tc.val = (1 << (TOUCH_RST - 32))
    #define TOUCH_RST_HIGH GPIO.out1_w1ts.val = (1 << (TOUCH_RST - 32))
#endif

class ST77922_TOUCH {
public:
  ST77922_TOUCH(void);
  void init(void);
  void reset(void);
  void Set_Rotation(uint8_t r);
  void Read_Data(uint16_t reg, uint8_t* rbuf, size_t rlen);
  bool Get_Touch(void);

  uint8_t max_points;
  struct _touch_dev {
    uint8_t id[MAX_TOUCH_POINTS];
    uint16_t x[MAX_TOUCH_POINTS];
    uint16_t y[MAX_TOUCH_POINTS];
  } touch;

private:
  uint16_t width, height;
  uint8_t rotation;
};

#endif