# esphome-evse
EVSE running entirely on ESP32 MCU ESPHome YAML

BETA YAML firmware - there WILL still be bugs I am sure.. But base functionality is there! 
- No RCD code yet. 
- No safety checks.
- Auto button doesn't do anything yet - but controlling the current setpoint programmatically from Home Assistant is the main reason I went to all this trouble.. 
- Use at your own risk!


This could be run without any LCD HMI but I always wanted to have local user control possible and its easier to remove functionality vs add it later so there you go.. 

The source was modular with multiple external "packages" YAML used - but I have condensed it all into one file for this initial release just so its easier to follow.. I may go back to modular format but maybe not. 

[Post in the ESPHome Discord "Show Off" Channel](https://discord.com/channels/429907082951524364/1308643575449518160) 


[Inspired by this great ESP32 (not ESPHome) project!](https://github.com/dzurikmiroslav/esp32-evse) 

[Miroslav Dzúrik explains the CP protocol and hardware to generate it in depth on his Wiki here.](https://github.com/dzurikmiroslav/esp32-evse/wiki/CP-calibration)

This example image of the GUI(during dev) is without hardware besides the LCD - hence the numbers dont all make sense:

<img src="https://github.com/user-attachments/assets/d5dc5018-489b-47c6-98ba-9c22a993e556" alt="Alt Text" style="width:50%; height:auto;">

Charging real car at 6A - current setpoint can be changed in real time and vehicle will adjust its draw.

<img src="https://github.com/user-attachments/assets/bde0680d-757e-4c0f-a392-faaf9ced7648" alt="Alt Text" style="width:30%; height:auto;">
<img src="https://github.com/user-attachments/assets/6664beeb-25bf-4822-97a6-7bf8c94cb628" alt="Alt Text" style="width:30%; height:auto;">

PCB Designed by me - produced by JLCPCB

<img src="https://github.com/user-attachments/assets/edd2551b-6f4c-4058-9826-bdea2e6f5002" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/63f8fcb8-f410-4154-9450-279c3e58c243" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/bf221ddc-6771-4154-95e3-2095ccb661a0" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/48726e65-868e-46b8-bb4f-d90d5c7a817b" alt="Alt Text" style="width:40%; height:auto;">
<img src="https://github.com/user-attachments/assets/52ee4612-118a-437d-b911-eaf96cbfea1c" alt="Alt Text" style="width:40%; height:auto;">
