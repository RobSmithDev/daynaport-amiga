/*
 * SCSI DaynaPORT Device (scsidayna.device) by RobSmithDev 
 * DaynaPORT Interface Commands
 *
 */
#ifndef SCSI_WIFI_H
#define SCSI_WIFI_H 1

#include "compiler.h"
#include <dos/dos.h>
#include <exec/lists.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/semaphores.h>
#include "debug.h"



// Maximum number of WIFI networks the BlueScsi/SCSI2SD firmware will return
#define SCSIWIFI_MAX_NETWORK_COUNT 10

// This is defined in the BlueSCSI/SCSI2SD firmware. MTU would be ~1500 - the extra 20 bytes are ethernet overhead I guess!?
#define SCSIWIFI_PACKET_MAX_SIZE     1520
#define SCSIWIFI_PACKET_MTU_SIZE     1500

// Result from calling SCSIWifi_open
enum SCSIWifi_OpenResult {sworOK, sworOpenDeviceFailed, sworOutOfMem, sworInquireFail, sworNotDaynaDevice};

// Current status of a WIFI scan
enum SCSIWifi_ScanStatus {swssBusy, swssComplete, swssNotRunning, swssError};

#ifdef __VBCC__
#pragma pack(1)
#define STRUCT_PACKED 
#define STRUCT_ALIGN16 
#else
#define STRUCT_PACKED  __attribute__((packed))
#define STRUCT_ALIGN16 __attribute__((aligned (16)))
#endif

// Structure for MAC addresses from WIFI scsi
struct STRUCT_PACKED SCSIWifi_MACAddress {
    UBYTE valid;
    UBYTE _padding;
    UBYTE address[6];
};

// Needs completing in order to join a WIFI network
struct STRUCT_PACKED SCSIWifi_JoinRequest {
	char ssid[64];
	char key[64];
	UBYTE channel;   // the channel number isn't used
	UBYTE _padding;
};

// A single result received from a WIFI scan
struct STRUCT_PACKED SCSIWifi_NetworkEntry {
	char ssid[64]; 
	char bssid[6]; // Not implemented with getting current wifi status
	char rssi;     // if this is 0 its not connected
	UBYTE channel;
	UBYTE flags;
	UBYTE _padding;
};

// Disk settings
struct ScsiDaynaSettings {
  // SCSI device driver
  char deviceName[108];
  // Device ID  
  SHORT deviceID;     // if this is <0 or >7 then it auto-detects
  // Priority for the READING task
  SHORT taskPriority;
  // Driver mode
  USHORT scsiMode;
  // Auto-connect to this wifi network
  USHORT autoConnect;
  char ssid[64];
  char key[64];
};

#ifdef __VBCC__
#pragma pack(2)
#endif

// The full result from a wifi scan - 742 bytes - word aligned
struct STRUCT_ALIGN16 SCSIWifi_ScanResults { 
    UWORD count;               // Number of results
    struct SCSIWifi_NetworkEntry networks[SCSIWIFI_MAX_NETWORK_COUNT];   // 740 bytes
};

#ifdef __VBCC__
#pragma pack()
#endif


// Device handle - yeah you don't need to know what's inside
typedef void* SCSIWIFIDevice;

// Internal SCSI device data
struct SCSIDevice_OpenData {
    struct ExecBase *sysBase;            // Library needs these
    struct UtilityBase *utilityBase;
    struct DosBase *dosBase;

    SHORT deviceID;                     // SCSI ID (0-7)
    USHORT scsiMode;                  // Special mode, from settings

    char* deviceDriverName;             // SCSI Driver to use (eg: scsi.device or gvpscsi.device etc)
};

// Populates settings with default values
void SCSIWifi_defaultSettings(struct ScsiDaynaSettings* settings);

// Loads settings from the ENV, returns 0 if the settings were bad and defaults were setup
LONG SCSIWifi_loadSettings(void *utilityBase, void *dosBase, struct ScsiDaynaSettings* settings);

// Saves settings back to ENV or ENVARC - returns 0 if it failed
LONG SCSIWifi_saveSettings(struct DosBase *dosBase, struct ScsiDaynaSettings* settings, LONG saveToENV);

// Attempt to open the DAYNA scsi device. 
SCSIWIFIDevice SCSIWifi_open(struct SCSIDevice_OpenData* openData, enum SCSIWifi_OpenResult* errorCode);

// Free and release any memory allocated as a result of SCSIWifi_open. 
void SCSIWifi_close(SCSIWIFIDevice device);

// Triggers a WIFI scan.  Returns 1 if successful
LONG SCSIWifi_scan(SCSIWIFIDevice device, enum SCSIWifi_ScanStatus* status);

// Check how a current WIFI scan is progressing
LONG SCSIWifi_scanComplete(SCSIWIFIDevice device, enum SCSIWifi_ScanStatus* status);

// Get the results from the WIFI scan
LONG SCSIWifi_getScanResults(SCSIWIFIDevice device, struct SCSIWifi_ScanResults* results);

// Enable/Disable the WIFI device (this actually resets its circular buffer) - (setEnable !=0 to enable)
LONG SCSIWifi_enable(SCSIWIFIDevice device, LONG setEnable);

// Fetch the MAC address from the Wifi card
LONG SCSIWifi_getMACAddress(SCSIWIFIDevice device, struct SCSIWifi_MACAddress* macAddress);

// Attempt ot join the specified WIFI network - Only way to find out if it worked is to periodically call SCSIWifi_getNetwork
LONG SCSIWifi_joinNetwork(SCSIWIFIDevice device, struct SCSIWifi_JoinRequest* wifi);

// Fetch information about the currently connected network
LONG SCSIWifi_getNetwork(SCSIWIFIDevice device, struct SCSIWifi_NetworkEntry* connection);

// Add a Multicast Ethernet address to the adapter
LONG SCSIWifi_addMulticastAddress(SCSIWIFIDevice device, struct SCSIWifi_MACAddress* macAddress);

// Send an ethernet frame (this is actually queued and sent inside the bluescsi/scsi2sd)
LONG SCSIWifi_sendFrame(SCSIWIFIDevice device, UBYTE* packet, UWORD packetSize);

// On ENTRY, packetSize should be the memory size of packetBuffer, which SHOULD be SCSIWIFI_PACKET_MAX_SIZE + 6
// If returns TRUE and packetSize=0 then no data is waiting to be read
// Else packetSize will be what was read with the first 6 bytes being in the following format:
// packetSize will *need* to be SCSIWIFI_PACKET_MAX_SIZE+6
//   Byte:   0 High Byte of packet size
//           1: Low Byte of packet size
//           2, 3, 4 = 0
//           5: 0 if this was the last packet, or 0x10 if there are more to read
LONG SCSIWifi_receiveFrame(SCSIWIFIDevice device, UBYTE* packetBuffer, UWORD* packetSize);


#endif