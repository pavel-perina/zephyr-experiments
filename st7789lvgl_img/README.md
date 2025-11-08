NOTE: example is based on links in overlay file and also ili9341fb

https://shop.pimoroni.com/products/pico-display-pack-2-8?variant=42047194005587

Schematic

I2C
I2C_SDA GP6
I2C_SCL GP7

Buttons (pressed->connected to ground)
A GP12
B GP13
X GP14
Y GP15

Display SPI
GP20_LCD_BACKLIGHT
GP19 LCD_MOSI
GP18 LCD_SCLK
GP17 LCD_CS
GP16 LCD_DC
GP21 LCD_TE

RGB LED (PWM controlled)
active=low
LED_R 26
LED_G 27
LED_B 28
r16 = GAMMA_8BIT(led_r); r16 *= brightness, if polarity==active_low r16 == UINT16_MAX - r16;
pwm_set_gpio_level(pin_r, r16)


gamma-https://github.com/pimoroni/pimoroni-pico/blob/1e7fb9e723c18fea24aa9353e767cadee2a87d70/common/pimoroni_common.hpp#L81

