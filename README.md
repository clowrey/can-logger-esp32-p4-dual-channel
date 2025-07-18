# ESP32 LED Blink Project (ESP-IDF)

This is a native ESP-IDF project for ESP32 boards that blinks an LED connected to GPIO pin 2.

## Hardware Requirements

- ESP32 development board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, or ESP32-P4)
- LED connected to GPIO pin 2 (or use built-in LED if available)
- Resistor (220-330 ohm) for LED current limiting (if using external LED)

## Software Requirements

- ESP-IDF (Espressif IoT Development Framework)
- CMake (version 3.16 or higher)
- A supported toolchain (xtensa-esp32-elf for ESP32)

## Project Structure

```
esp32-p4-testing/
├── CMakeLists.txt          # Main CMake configuration
├── main/
│   ├── CMakeLists.txt      # Main component CMake configuration
│   └── main.c              # Main source code file
├── sdkconfig               # ESP-IDF configuration (auto-generated)
└── README.md               # This file
```

## Setup Instructions

1. **Install ESP-IDF**: Follow the official ESP-IDF installation guide:
   - [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)

2. **Set up the environment**: 
   ```bash
   # On Linux/macOS
   . $HOME/esp/esp-idf/export.sh
   
   # On Windows
   %userprofile%\esp\esp-idf\export.bat
   ```

3. **Configure the project**:
   ```bash
   idf.py menuconfig
   ```

4. **Build the project**:
   ```bash
   idf.py build
   ```

5. **Flash to ESP32**:
   ```bash
   idf.py -p PORT flash
   ```
   Replace `PORT` with your ESP32's serial port (e.g., `/dev/ttyUSB0` on Linux, `COM3` on Windows)

6. **Monitor the output**:
   ```bash
   idf.py -p PORT monitor
   ```

7. **Build, flash, and monitor in one command**:
   ```bash
   idf.py -p PORT flash monitor
   ```

## Configuration

- **LED Pin**: The LED is configured to use GPIO pin 2. You can change this in `main/main.c` by modifying the `LED_PIN` definition
- **Blink Interval**: The LED blinks every 1 second (1000ms). You can adjust the delay values in the code
- **Serial Monitor**: The project outputs debug information using ESP-IDF's logging system

## Code Features

This project demonstrates:
- Native ESP-IDF GPIO configuration and control
- ESP-IDF logging system (`ESP_LOGI`)
- FreeRTOS task delays (`vTaskDelay`)
- Proper GPIO initialization with `gpio_config_t`

## LED Connection

If using an external LED:
- Connect the LED's positive leg (longer leg) to GPIO pin 2
- Connect a 220-330 ohm resistor in series with the LED
- Connect the other end of the resistor to ground (GND)

## Troubleshooting

- **Build errors**: Ensure ESP-IDF is properly installed and the environment is set up
- **Upload issues**: Check that your ESP32 board is properly connected and the correct port is specified
- **Permission errors**: On Linux/macOS, you might need to add your user to the dialout group or use sudo
- **Monitor not working**: Make sure to use the same port for both flashing and monitoring

## ESP-IDF vs Arduino Framework

This project uses the native ESP-IDF framework instead of Arduino, which provides:
- Better performance and lower-level control
- Access to all ESP32 features
- More efficient memory usage
- Professional development environment 