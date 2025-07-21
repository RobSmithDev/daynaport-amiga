# Amiga DaynaPORT Driver for BlueSCSI V2 and ZuluSCSI
Created by RobSmithDev

This is an implementation of a SANA-II driver for the Amiga, which allows you to use either a Pico W enabled BlueSCSI V2, or various WiFi equipped ZuluSCSI models to give internet access to the machine!

This code is based on the MNT ZZ9000Net driver by Lukas F. Hartmann (which is based on work by Henryk Richter) and also borrows a little from the PlipBox device driver, which has several contributors.

Setup Guides:
- [YouTube video by Retronaut](https://www.youtube.com/watch?v=FDtqd04bq-k)
- [BlueSCSI Docs](https://github.com/blueSCSI/blueSCSI-v2/wiki/WiFi-Amiga)
- [Discord](https://discord.gg/pQsU3CR9fe)
- [Workbench GUI Config Tool](https://github.com/AidanHolmes/BlueSCSIUI/releases/)

### SCSI Hardware Requirements:
- [BlueSCSI Pico (V2) Wi-fi version](https://bluescsi.com/docs/WiFi-DaynaPORT) (v2024.10.26 or later firmware)
- [ZuluSCSI Pico OSHW board with Pico W](https://zuluscsi.com/oshw/)  (v2024.03.07 or later firmware)
- [ZuluSCSI Pico Slim (DB25)](https://shop.rabbitholecomputing.com/products/zuluscsi-rp2040-pico-slim)
- [ZuluSCSI Blaster (RP2350B)](https://zuluscsi.com) with RM2 WiFi module

Tested configurations:

### Amiga 500/+ (2M Chip)
- A590 HDD (2M Fast RAM, 7.0 ROM) with Kickstart 3.2 (scsi.device)
- Ematrix 530 (ematscsi.device)

### Amiga 1200
- Blizzard 1230IV / SCSI Kit (1230scsi.device)

### Amiga 2000 (2M Chip)
- GVP Impact A2000-HC+8 Series II (8M Fast RAM), Kickstart 3.1 (gvpscsi.device)
- A2091, Kickstart 3.1 (scsi.device)
- phase5 Blizzard 2060 SCSI, Kickstart 3.1 (2060scsi.device)
	
#### Side effects
With BlueSCSI V2 hardware, the HDD light constantly flashes while the driver is in use.
The driver needs to be copied to the `DEVS:Networks` folder and then setup your TCP/IP stack as normal.

#### Notes
If you find your data transfer is *very* slow, like less than 5K/s then check you've turned the debug log off within your BlueSCSI or ZuluSCSI configuration!

### Config File (IMPORTANT)
`scsidayna.prefs` contains an example config file for the device. This needs to be copied to `ENVARC:` on the Amiga and rebooted. 
**If you change this file, they will not be picked up until restart or you copy it to ENV:**
You can also manage this file with the [Workbench GUI config tool by Aidan Holmes](https://github.com/AidanHolmes/BlueSCSIUI/releases/).

The format of that file is:
```
DEVICE=scsi.device
DEVICEID=-1
PRIORITY=0
MODE=1
AUTOCONNECT=0
SSID=
KEY=
```

where:
- DEVICE is the name of the SCSI driver, eg: scsi.device or gvpscsi.device
- DEVICEID is the SCSI device index the DaynaPORT is on, or -1 for Auto Detect
- PRIORITY -128 to 127, sets the I/O task priority, see below
- MODE see below
- AUTOCONNECT 0/1 if 1, the driver will attempt to connect to the WIFI device (you can also configure BlueSCSI or ZuluSCSI to do this)
- SSID The SSID/Wifi name to connect to if autoconnect=1
- KEY the wifi key/password

## Mode
This patches around weirdness in the various SCSI drivers. Mode should be:
- 0: This runs in normal mode
- 1: Runs in 24-byte pad mode (required for scsi.device - A590/A2091)
- 2: Runs in 'single transfer' mode (required for gvpscsi.device)

## DEVICE
This needs to match the SCSI interface you're using. You can check this using HDToolbox (see what device it uses in the tool type) or SCSIMounter etc.

## Task Priority
A small note about task priority. If left at 0 the device will function perfectly fine, however the throughput of data is somewhat all over the place.
If you want a really stable throughput, then set this to '1', but also expect this will possibly slow down some of the other applications running on your system.
