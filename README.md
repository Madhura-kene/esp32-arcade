
# ESP32 Arcade Hub

A handheld retro arcade built on a single ESP32. Five classic games run from one Arduino sketch on a 1.8" colour TFT, controlled by an analog joystick with a push button.

## Games

| # | Game | Controls | Goal |
|---|------|----------|------|
| 1 | **Pac-Man** | Joystick moves one cell per tick | Eat every dot in the maze while avoiding the ghost |
| 2 | **Snake** | Joystick steers | Eat food to grow; avoid walls and your own tail |
| 3 | **Space Invaders** | Left/Right moves, **click** fires | Destroy all invaders before they reach the bottom |
| 4 | **Tetris** | Left/Right moves, Down soft-drops, **click** rotates | Clear lines; 100 points per line |
| 5 | **Flappy Bird** | **Click** flaps | Fly through the gaps in the pipes |

## Features

- Scrollable main menu with colour-coded game selection
- Joystick auto-calibration at startup (no manual tuning of the centre point)
- Debounced button handling
- Per-game tick rates for smooth gameplay
- Game Over screen with final score and one-click restart
- Hardware shortcut back to the menu at any time via the BOOT button

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | ESP32-WROOM-32 DevKit |
| Display | 1.8" ST7735 SPI TFT, 128x160, V1.1 |
| Input | Analog joystick module with push button (SW) |

## Wiring

### TFT display

| TFT pin | ESP32 pin |
|---------|-----------|
| VCC | 3V3 |
| LED | 3V3 |
| GND | GND |
| SCK | GPIO18 |
| SDA / MOSI | GPIO23 |
| CS | GPIO5 |
| DC / A0 | GPIO2 |
| RESET | GPIO4 |

### Joystick

| Joystick pin | ESP32 pin |
|--------------|-----------|
| VRx | GPIO34 |
| VRy | GPIO35 |
| SW | GPIO27 |
| VCC | 3V3 |
| GND | GND |

> **Note:** GPIO34 and GPIO35 are input-only pins, which makes them well suited for analog joystick axes. The BOOT button (GPIO0) is used to return to the menu. Do not use the EN pin as an input: it is the ESP32 reset line.

## Software Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) with the ESP32 board package installed
- Libraries (install through the Library Manager):
  - **Adafruit GFX Library**
  - **Adafruit ST7735 and ST7789 Library**

`SPI.h`, `math.h` and `string.h` are included with the toolchain.

## Getting Started

1. Wire the display and joystick as described above.
2. Open the sketch in the Arduino IDE.
3. Select **ESP32 Dev Module** (or your specific ESP32 DevKit) under *Tools > Board*.
4. Select the correct COM port.
5. Click **Upload**.
6. **Keep the joystick centred while the board powers up.** The sketch calibrates the stick's resting position during the splash screen.

## How to Play

- **Menu:** push the joystick up or down to browse, click to launch.
- **In game:** use the joystick for direction and click for the action button.
- **Game Over:** click to play again, or press **BOOT** to return to the menu.
- **At any time:** press **BOOT** to quit to the menu.

## Project Structure

The project is a single sketch, organised into sections:

| Section | Purpose |
|---------|---------|
| Pin and colour definitions | Hardware mapping and palette |
| Input handling | Joystick reading, calibration, direction mapping, debounced click |
| Menu | Game selection screen |
| Game modules | `init`, `update` and `draw` functions for each game |
| Game flow | Game Over screen, launch and restart logic, main loop |

To add a new game: create its `init`, `update` and `draw` functions, add an entry to the `Screen` enum, the `gameNames`/`gameColors` arrays and the `startGame()` and main-loop `switch` statements, then increase the menu count.

## Troubleshooting

**Joystick directions are inverted or swapped**
Joystick modules differ in how they are mounted and wired. Direction mapping lives in two functions:

```cpp
int dirX() { return -axisDir(joyY, joyCenterY); }
int dirY() { return -axisDir(joyX, joyCenterX); }
```

- To swap the axes, exchange `joyX`/`joyCenterX` and `joyY`/`joyCenterY`.
- To invert a direction, add or remove the leading minus sign on that line.

**Display shows a white screen or nothing**
Check the wiring, confirm 3V3 power on VCC and LED, and verify that the CS, DC and RESET pins match the definitions at the top of the sketch.

**Colours are wrong, or there is a thin border or noise at the screen edges**
Different 1.8" ST7735 panels need different initialisation. The sketch uses `INITR_BLACKTAB`. Try `INITR_GREENTAB` or `INITR_REDTAB` in `tft.initR()`.

**Menu moves on its own or games steer without input**
The joystick was not centred during startup calibration. Release the stick and reset the board.

**Cannot upload to the board**
Hold the BOOT button while the IDE shows "Connecting...", then release it once the upload begins.

## Known Limitations

- The Pac-Man ghost is stationary and acts as an obstacle rather than a chasing enemy.
- The screen is redrawn fully on every frame, which can cause slight flicker in fast games.
- Scores are not saved between sessions.

## Ideas for Future Work

- Moving ghosts with chase behaviour in Pac-Man
- Persistent high scores stored in flash (NVS)
- Sound effects with a passive buzzer
- Partial redraws to reduce flicker
- Additional games (Breakout, Pong)

## License

Add a license of your choice (for example MIT) before publishing the repository.

