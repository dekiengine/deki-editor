A slim device with a bright 1.91 inch AMOLED screen, with or without touch.

## What is set up for you

Display and touch.

## Good to know

- There is no SD card slot, so your game's assets are always in the flash: about 13 MB of the 16 MB.
- An AMOLED has no backlight. Brightness is a command to the panel, which Deki sends when the screen turns off and on.
- On the version without touch, nothing answers and the boot is not slowed by it.
- If the device does not show up as a port, hold BOOT, press RESET and release BOOT.

## Pins

| Part | Pins |
| --- | --- |
| Panel power | GPIO 38 |
| Quad SPI bus | SCK 47, IO0 18, IO1 7, IO2 48, IO3 5 |
| Display (RM67162) | CS 6, reset 17 |
| I2C bus | SDA 3, SCL 2 |
| Touch (CST816T) | Address 0x15, interrupt 21 |
