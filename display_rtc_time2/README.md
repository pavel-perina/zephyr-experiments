Demonstration code using XIAO expansion board with
PCF8563 RTC and SSD1306 display without LVGL
(Light and Versatile Graphics Library)
and universal RTC driver which does not work with wrong RTC time.

RTC communication is done via I2C interface and display is
used via display driver.

This simplifies the code, but somewhat violates best practices
and portability, because these things are defined in the code
rather than the device tree.

It's kind of based on my experiments with MicroPython and
"Nokia 5110" or PCD8544 display which has the same layout
and the same font rendering technique.

Article about creating font: https://www.pavelp.cz/posts/eng-python-bitmap-fonts/
Article mentioning framebuffer layout and font rendering: https://www.pavelp.cz/posts/eng-raspberry-pico-nokia-display/#deciphering-frambuffer-layout

The first version uses RTC driver.

Pavel Perina, 2025-07-13

