# DaynaPORT Driver for BlueSCSI (Amiga)

This is an implementation of a SANA-II driver for the Amiga, that will use the BlueScsi V2 WIFI version to give internet access to the machine!
This code is based on the MNT ZZ9000Net driver by Lukas F. Hartmann (which is based on work by Henryk Richter) and also borrows a little from the PlipBox device by, 
well, theres several people.

This requires the updates in https://github.com/RobSmithDev/BlueSCSI-v2 in order to work.

I've tested it with the following setup:
-	Amiga 500+ (2M Chip)
-	A590 HDD (2M fast ram, 7.0 ROM) 
-	Blue Scsi V2 Wifi version
-	Kickstart 3.2
	
I've tried it on this configuration and it *sometimes* works but most of the time doesnt.
I think it just doesnt manage to get a DHCP lease in time and gives up. Sometimes you get GVP errors
-	Amiga 2000 (2M Chip)
-	Impact A2000-HC+8 Series II (8M Fast Ram) 
-	Blue Scsi V2 Wifi version
-	Kickstart 3.1
	
Side effects: the hdd light constantly flashes while the driver is in use.
The driver needs to be copied to the Devs:Networks folder and then setup your TCP/IP stack as normal.

scsidayna.prefs contains an example config file for the device. This needs to be copied to ENVARC on the Amiga and rebooted
The format of that file is:
scsi-device-driver-name.device
`
-	deviceID  (0-7) or -1 for auto-detect
-	40		(device delay time when idle)
-	1		(task priority, -128-127)
-	1		(1/0 for auto connect to WIFI - you can configure this in the bluescsi.ini file too)
-	ssid	(wifi SSID)
-	pwd		(wifi password)
`

