# PiNES
PiNES is a Nintendo Entertainment System emulator developed for the Raspberry Pi Pico (RP2040) using the Arduino IDE environment.
The project integrates support for the ILI9341 SPI display, game loading from SD or flash and i2s audio output.

# 🚀 Features
  -  Core: Emulation of 6502 CPU, PPU, APU.
  -  Display: ILI9341 320×240 SPI support.
  -  Storage: Loading .nes files and .jpg previews from flash via LittleFS or MicroSD.
  -  Audio: i2s sound generation.
  -  Input: One/Dual joystick ports (via GPIO).


# 🛠️ Hardware Requirements
To build the project, you will need the following components:

 - Microcontroller: Raspberry Pi Pico (RP2040).
       <br/><img width="300" height="300" alt="pico" src="https://github.com/user-attachments/assets/57d94fe2-ba95-4041-8d19-b6f1106df1b2" />
     
 - Display: 2.8" ILI9341 SPI (320×240) with integrated SD reader.
       <br/><img width="640" height="200" alt="tft-display-2 8-spi" src="https://github.com/user-attachments/assets/6522d432-fa3d-4541-953e-193235030034" />

 - Audio:
    - MAX98357 amplifier.
          <br/><img width="231" height="231" alt="image" src="https://github.com/user-attachments/assets/7d0264ac-8058-4daf-ae16-1d583d8d29f8" />

    - 3W / 4–8Ω speaker
          <br/><img width="300" height="300" alt="speaker" src="https://github.com/user-attachments/assets/8fcebdf6-4df0-4d92-b6e8-447f233d5c6f" />

 - Input:
    - 2× directional joystick boards with 5‑way switch + 2 buttons (Joystick 1 and 2) + some other button if you want full compatibility.
          <br/><br/><img width="300" height="161" alt="COM-5WS-01" src="https://github.com/user-attachments/assets/ae02c59a-e821-4228-8e59-e08bd2625be7" />



# 🔌 Wiring
The Raspberry Pi Pico is the core of the system. Below are the detailed connections, divided by module.

## 1. ILI9341 Display & SD Card

| Component | Module Pin | Pico Pin (GPIO) | Note |
|----------|-------------|------------------|------|
| Display  | VCC         | VSYS             | Powered directly from Pico |
|          | GND         | GND              | |
|          | SDO (MISO)  | GP16             | |
|          | LED         | 3V3              | |
|          | SCK         | GP18             | |
|          | SDI (MOSI)  | GP19             | |
|          | DC          | GP21             | |
|          | RESET       | GP20             | |
|          | CS          | GP17             | |
| SD Card  | SD_CS       | GP13             | |
|          | SD_SCK      | GP10             | |
|          | SD_MOSI     | GP11             | |
|          | SD_MISO     | GP12             | |

## 2. Audio (i2s Amplifier)

| Component | Module Pin | Pico Pin (GPIO) | Note |
|----------|-------------|------------------|------|
| Ampl.    | VCC         | VBUS             | 5V |
|          | GND         | GND              | |
|          | BCLK        | GP26             | |
|          | LRC         | GP27             | |
|          | DIN         | GP28             | |


## 3. Joysticks

Joysticks use internal pull‑up resistors.
Each button closes the contact to GND.
| Module Pin | Pico Pin (GPIO) |
|------------|-----------------|
| JOY1_UP_PIN|       2| 
|JOY1_DOWN_PIN|    3|
|JOY1_LEFT_PIN|    4|
|JOY1_RIGHT_PIN|   5|
|JOY1_A_PIN|       6|
|JOY1_B_PIN|       7|
|JOY1_SELECT_PIN|  0|
|JOY1_START_PIN|   1|


TODO: remap to available pins in your project and update nes_joystick_config.h if you want to use 2
| Module Pin | Pico Pin (GPIO) |
|------------|-----------------|
|JOY2_UP_PIN|      0|
|JOY2_DOWN_PIN|    0|
|JOY2_LEFT_PIN|    0|
|JOY2_RIGHT_PIN|   0|
|JOY2_A_PIN|       0|
|JOY2_B_PIN|       0|
|JOY2_SELECT_PIN|  0|
|JOY2_START_PIN|   0|


# 📝 Technical Notes

- Power:<br/>
    Pico powered via USB.<br/>
    Audio amplifier requires 5V<br/>

- TFT_eSPI Configuration:<br/> 
    Replace User_Setup.h with:<br/>
        - Driver: ILI9341<br/>
        - SPI Frequency: 80MHz<br/>
        - DMA: Enabled<br/>

If you notice graphical glitches, reduce SPI frequency to 40–50MHz.

# 📂 Software Setup

The project is developed for Arduino IDE using the RP2040 core.<br/> 

## Required Libraries

Make sure you have installed:<br/> 
    - TFT_eSPI (configured for ILI9341 on Pico)<br/> 
    - TJpg_Decoder (just for game previews, you can disable this function)<br/> 
    - LittleFS<br/> 

## File System Structure

The emulator looks for files inside the /PiNES directory.<br/>
Organize your FS like this:<br/>

        PiNES/
        ├─ games/
        │   ├─ game1.nes
        │   ├─ game1.jpg
        │   ├─ gamex.nes
        │   ├─ gamex.jpg
        │   └─ noImage.jpg
        
Images must be 200x150px jpg no progressive format.<br/>

## Using LittleFS Instead of a Physical SD Card

Instead of using a physical SD card, you can store the entire PiNES folder directly inside the Pico’s onboard flash memory.<br/>
To do this, use the LittleFS Upload Tool for Arduino IDE, which uploads the contents of your local /data folder into the Pico’s flash filesystem.<br/>
Official documentation:<br/>
Arduino-Pico LittleFS guide:<br/>  
https://arduino-pico.readthedocs.io/en/latest/filesystems.html (arduino-pico.readthedocs.io in Bing)<br/>
LittleFS Upload Tool (arduino-littlefs-upload):<br/> 
https://github.com/earlephilhower/arduino-littlefs-upload (github.com in Bing)<br/>

# 🎮 Usage

1. **Startup**: On boot, the main menu “NESMenu” appears.<br/>
2. **Navigation**: Use the joysticks to move between options:<br/>
   **GAMES BROWSER**: Scroll through the game list with the joystick; press Fire to load the selected game.<br/>

# ⚡Performance & Overclock

To achieve acceptable emulation speed, the Raspberry Pi Pico is pushed beyond its factory specifications (276MHz).<br/>

            
According to available documentation, 276MHz is generally achievable without special precautions.<br/>
This frequency allows:<br/>
    - SPI bus at 80MHz for the display<br/>
    - Real‑time processing CPU cycles and audio<br/>
You may still notice slowdowns in some games, graphical glitches, or audio ticks due to timing desynchronization.<br/>

# ⚠️ Disclaimer

This project is provided “as is”, for hobby and experimental use.<br/>
