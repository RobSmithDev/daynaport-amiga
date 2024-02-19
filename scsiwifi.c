/*
 * SCSI DaynaPORT Device (scsidayna.device) by RobSmithDev 
 * DaynaPORT Interface Commands
 *
 */
 
 
#include <proto/exec.h>
#include <exec/execbase.h>
#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <devices/scsidisk.h>
#include <string.h>
#include "macros.h"
#include <stdio.h>
#include <stdlib.h>
#include "debug.h"
#include "scsiwifi.h"


#define SCSI_INQUIRY                        0x12
#define SCSI_NETWORK_WIFI_READFRAME         0x08
#define SCSI_NETWORK_WIFI_GETMACADDRESS     0x09  // gvpscsi.device doesn't like this command code
#define SCSI_NETWORK_WIFI_WRITEFRAME        0x0A
#define SCSI_NETWORK_WIFI_ADDMULTICAST      0x0D
#define SCSI_NETWORK_WIFI_ENABLE            0x0E

// These are custom extra SCSI commands specific to this device
#define SCSI_NETWORK_WIFI_CMD				0x1c	
// Sub commands
#define SCSI_NETWORK_WIFI_OPT_SCAN			0x01	 
#define SCSI_NETWORK_WIFI_OPT_COMPLETE		0x02
#define SCSI_NETWORK_WIFI_OPT_SCAN_RESULTS	0x03
#define SCSI_NETWORK_WIFI_OPT_INFO			0x04
#define SCSI_NETWORK_WIFI_OPT_JOIN			0x05
#define SCSI_NETWORK_WIFI_OPT_GETMACADDRESS 0x09    

#define INQUIRE_BUFFER_SIZE                 64

// SENSE 
#define SCSI_CHECK_CONDITION                2

#ifdef __VBCC__
#pragma pack(2)
struct SENSE_DATA {
  UBYTE  ValidErrorCode;      
  UBYTE  SegmentNumber;
  UBYTE  SenseKeyAndFlags;
  UBYTE  Information[4];
  UBYTE  AdditionalSenseLength;
  UBYTE  CommandSpecificInformation[4];
  UBYTE  AdditionalSenseCode;
  UBYTE  AdditionalSenseCodeQualifier;
  UBYTE  FieldReplaceableUnitCode;
  UBYTE  SenseKeySpecific[3];
};
#pragma pack()
#else
struct STRUCT_ALIGN16 SENSE_DATA {
  UBYTE  ErrorCode  :7;
  UBYTE  Valid  :1;
  UBYTE  SegmentNumber;
  UBYTE  SenseKey  :4;
  UBYTE  Reserved  :1;
  UBYTE  IncorrectLength  :1;
  UBYTE  EndOfMedia  :1;
  UBYTE  FileMark  :1;
  UBYTE  Information[4];
  UBYTE  AdditionalSenseLength;
  UBYTE  CommandSpecificInformation[4];
  UBYTE  AdditionalSenseCode;
  UBYTE  AdditionalSenseCodeQualifier;
  UBYTE  FieldReplaceableUnitCode;
  UBYTE  SenseKeySpecific[3];
};
#endif

// Prepares the SCSI command and resets some of the result values
#define SCSI_PREPCMD(device, cmd, sub, a, b, c, d) \
                device->scsiCommand[0] = cmd; device->scsiCommand[1] = sub; \
                device->scsiCommand[2] = a;   device->scsiCommand[3] = b;     \
                device->scsiCommand[4] = c;   device->scsiCommand[5] = d;     \
                device->Cmd.scsi_SenseActual = 0; device->Cmd.scsi_Actual = 0;  \
                device->Cmd.scsi_Status = 1;   // Default to error

// Internal SCSI device data
struct SCSIDevice {
    struct ExecBase *sc_SysBase;
    struct UtilityBase *sc_UtilityBase;
    struct DosBase *sc_dosBase;

    struct IOStdReq* SCSIReq;
    struct MsgPort* Port;    
    struct SCSICmd Cmd;
    struct SENSE_DATA senseData;
    UBYTE* scsiCommand;    // buffer to hold command, 16-bit aligned (12 bytes)
};

#define SysBase dev->sc_SysBase
#define UtilityBase dev->sc_UtilityBase
#define DOSBase dev->sc_dosBase


typedef struct SCSIDevice* LSCSIDevice;

// not ideal but couldn't get the compiler to give me __lmodu and __ldivu
// I'm sure someone who knows what they're doing can do this much better :)
void muldiv(USHORT num, USHORT divide, USHORT* result, USHORT* mod) {
    *result = 0;
    while (num >= divide) {
        (* result)++;
        num -= divide;
    }
    *mod = num;
}

// convert USHORT to string and appends a new line character
void _ustoa_nl(USHORT num, char* str) {
    char buffer[16];
    char* s = buffer;
    USHORT divres, divmod;
    do {
        muldiv(num, 10, &divres, &divmod);
        *s++ = '0' + divmod;
        num = divres;
    } while (num);
    s--;

    while (s>=buffer) *str++ = *s--;        
    *str++ = '\n';
    *str++ = '\0';
}

// convert SHORT to string and appends a new line character
void _stoa_nl(SHORT num, char* str) {
    char buffer[16];
    char* s = buffer;
    USHORT divres, divmod, number;

    LONG neg = num < 0;
    if (neg) number = -num; else number = num;
    do {
        muldiv(number, 10, &divres, &divmod);
        *s++ = '0' + divmod;
        number = divres;
    } while (number);
    s--;

    if (neg) *str++ = '-';

    while (s>=buffer) *str++ = *s--;        
    *str++ = '\n';
    *str++ = '\0';
}

// Ansi to Unsigned Short
USHORT _atous(char* str) {
    USHORT out = 0;
    while (*str) {
        if ((*str >= '0') && (*str <= '9')) {
            out *= 10;
            out += *str - '0';
        } 
        str++;
    }
    return out;
}

// Ansi to Signed Short
SHORT _atos(char* str) {
    LONG out = 0;
    LONG neg = 0;
    while (*str) {
        if ((*str >= '0') && (*str <= '9')) {
            out *= 10;
            out += *str - '0';
        } else if (*str == '-') neg = 1;  // make it negative!
        str++;
    }
    if (neg) return (SHORT)(-out);
    return out;
}

// Populates settings with default values
void SCSIWifi_defaultSettings(struct ScsiDaynaSettings* settings) {
    strcpy(settings->deviceName, "scsi.device");
    settings->deviceIndex = -1;  // auto detect
    settings->pollDelay = 10;    // controls the pause when idle.  Uses the VBLANK timer, weirdly doesnt have that much effect 
    settings->taskPriority = 1;  // -128 to 127

    settings->autoConnect = 0;   // auto connect to the WIFI?
    strcpy(settings->ssid, "");
    strcpy(settings->key, "");
}

// Rmeoves any trailing newline characters
void removeNL(char* text) {
    while (*text) {
        if (*text == '\n') {
            *text='\0';
            return;
        }
        text++;
    }
}

// Loads settings from the ENV, returns 0 if the settings were bad and defaults were setup
LONG SCSIWifi_loadSettings(struct DosBase *dosBase, struct ScsiDaynaSettings* settings) {
    struct SCSIDevice devTmp;
    LSCSIDevice dev = &devTmp;
    devTmp.sc_dosBase = dosBase;

    SCSIWifi_defaultSettings(settings);
    BPTR fh;
    if (fh = Open("ENV:scsidayna.prefs",MODE_OLDFILE)) {
        UBYTE good = 1;
        if (!FGets(fh, settings->deviceName, 108)) good = 0; else removeNL(settings->deviceName);
        char tmp[20];
        if (FGets(fh, tmp, 20)) settings->deviceIndex = _atos(tmp); else good = 0;
        if (FGets(fh, tmp, 20)) settings->pollDelay = _atous(tmp); else good = 0;
        if (FGets(fh, tmp, 20)) settings->taskPriority = _atos(tmp); else good = 0;
        if (settings->taskPriority>127) settings->taskPriority = 127;
        if (settings->taskPriority<-128) settings->taskPriority = -128;
        if (FGets(fh, tmp, 20)) settings->autoConnect = _atous(tmp); else good = 0;
        if (!FGets(fh, settings->ssid, 64)) good = 0; else removeNL(settings->ssid);
        if (!FGets(fh, settings->key, 64)) good = 0; else removeNL(settings->key);

        if (!good) SCSIWifi_defaultSettings(settings);
        Close(fh);
        return good;
    }

    return 0;
}

// Saves settings back to ENV or ENVARC
LONG SCSIWifi_saveSettings(struct DosBase *dosBase, struct ScsiDaynaSettings* settings, LONG saveToENV) {
    struct SCSIDevice devTmp;
    LSCSIDevice dev = &devTmp;
    devTmp.sc_dosBase = dosBase;
    BPTR fh;
    if (fh = Open(saveToENV ? "ENV:scsidayna.prefs" : "ENVARC:scsidayna.prefs",MODE_NEWFILE)) {
        UBYTE good = 1;
        struct ScsiDaynaSettings tmp = *settings;
        strcat(tmp.deviceName, "\n");
        strcat(tmp.ssid, "\n");
        strcat(tmp.key, "\n");
        
        if (!FPuts(fh, tmp.deviceName)) good = 0;
        char str[20];
        _stoa_nl(tmp.deviceIndex, str);  if (!FPuts(fh, str)) good = 0;
        _ustoa_nl(tmp.pollDelay, str);  if (!FPuts(fh, str)) good = 0;
        _stoa_nl(tmp.taskPriority, str);  if (!FPuts(fh, str)) good = 0;
        _ustoa_nl(tmp.autoConnect, str);  if (!FPuts(fh, str)) good = 0;

        if (!FPuts(fh, tmp.ssid)) good = 0;
        if (!FPuts(fh, tmp.key)) good = 0;

        Close(fh);
        return good;
    }
    return 0;
}

struct IORequest* _CreateExtIO(LSCSIDevice dev, struct MsgPort *replyPort, long size) {
    struct IORequest *io = NULL;

    if (replyPort) {
        if (io = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR)) {
            io->io_Message.mn_ReplyPort = replyPort;
            io->io_Message.mn_Length = size;
            io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        }
    }
    return io;
}

void _DeleteExtIO(LSCSIDevice dev, struct IORequest *io) {
    if (io) {
        long bad = -1;
        io->io_Message.mn_Node.ln_Succ = (void *)bad;
        io->io_Device = (void *)bad;
        FreeMem(io, io->io_Message.mn_Length);
    }
}

struct MsgPort *_CreatePort(LSCSIDevice dev, UBYTE *name, LONG pri) {
    LONG sigBit;
    struct MsgPort *mp;

    if ((sigBit = AllocSignal(-1L)) == -1) return(NULL);

    mp = (struct MsgPort *)AllocMem((ULONG)sizeof(struct MsgPort),(ULONG)MEMF_PUBLIC | MEMF_CLEAR);
    if (!mp) {
        FreeSignal(sigBit);
        return(NULL);
    }
    mp->mp_Node.ln_Name = name;
    mp->mp_Node.ln_Pri  = pri;
    mp->mp_Node.ln_Type = NT_MSGPORT;
    mp->mp_Flags        = PA_SIGNAL;
    mp->mp_SigBit       = sigBit;
    mp->mp_SigTask      = (struct Task *)FindTask(0L);
                                                /* Find THIS task.   */

    if (name) AddPort(mp);
    else NEWLIST(&(mp->mp_MsgList));          /* init message list */

    return(mp);
}

void _DeletePort(LSCSIDevice dev, struct MsgPort *mp) {
    if ( mp->mp_Node.ln_Name ) RemPort(mp);  /* if it was public... */

    mp->mp_SigTask         = (struct Task *) -1;
                            /* Make it difficult to re-use the port */
    mp->mp_MsgList.lh_Head = (struct Node *) -1;

    FreeSignal( mp->mp_SigBit );
    FreeMem( mp, (ULONG)sizeof(struct MsgPort) );
}

// Close and free the open SCSI device
void _SCSIWifi_close(LSCSIDevice dev) {
    if (!dev) return;
    if (dev->SCSIReq) {
        if (!(CheckIO((struct IORequest *)dev->SCSIReq))) {
            AbortIO((struct IORequest *)dev->SCSIReq);      
            WaitIO((struct IORequest *)dev->SCSIReq);       
        }
        CloseDevice((struct IORequest *)dev->SCSIReq);
        _DeleteExtIO(dev, (struct IORequest *)dev->SCSIReq);
    }
    if (dev->scsiCommand) FreeVec(dev->scsiCommand);
    if (dev->Port) _DeletePort(dev, dev->Port);
    FreeVec(dev);
}

// Returns NULL on error (or not found), and a valid struct if its the BlueScsi Network Device
SCSIWIFIDevice SCSIWifi_open(struct SCSIDevice_OpenData* openData, enum SCSIWifi_OpenResult* errorCode) {
    LSCSIDevice dev;
    {
        struct SCSIDevice devTmp;
        dev = &devTmp;
        dev->sc_SysBase = openData->sysBase;
        dev->sc_UtilityBase = openData->utilityBase;
        dev->sc_dosBase = openData->dosBase;
        // dev->sysBase needs to be defined here for this to work!
        dev = (LSCSIDevice)AllocVec(sizeof(struct SCSIDevice),MEMF_PUBLIC|MEMF_CLEAR);
    }
    {
        if (!dev) {
            *errorCode = sworOutOfMem;
            return NULL;
        }

        dev->sc_SysBase = openData->sysBase;
        dev->sc_UtilityBase = openData->utilityBase;
        dev->sc_dosBase = openData->dosBase;

        dev->Port = _CreatePort(dev, NULL, 0);        
        if (!dev->Port) {
            *errorCode = sworOutOfMem;
            return NULL;
        }
        dev->SCSIReq = (struct IOStdReq*)_CreateExtIO(dev, dev->Port, sizeof(struct IOStdReq));
        if (!dev->SCSIReq) {            
            *errorCode = sworOutOfMem;
            _SCSIWifi_close(dev);
            return NULL;
        }
        // 6 bytes for command, 6 used by some of the status replies
        dev->scsiCommand = AllocVec(12, MEMF_PUBLIC|MEMF_CLEAR);

        // Open driver
        BYTE err = OpenDevice(openData->deviceDriverName, openData->deviceID, (struct IORequest*)dev->SCSIReq, 0);
        if (err != 0) {
            *errorCode = sworOpenDeviceFailed;
            _DeleteExtIO(dev, (struct IORequest *)dev->SCSIReq);
            dev->SCSIReq = NULL;
            _SCSIWifi_close(dev);
            return NULL;
        }
        // Setup the SCSI command structure    
        dev->SCSIReq->io_Length  = sizeof(struct SCSICmd);
        dev->SCSIReq->io_Data    = (APTR)&dev->Cmd;
        dev->SCSIReq->io_Command = HD_SCSICMD; 
         
        dev->Cmd.scsi_CmdLength = 6;  
        dev->Cmd.scsi_Command   = dev->scsiCommand;                  
        dev->Cmd.scsi_SenseData = (UBYTE*)&dev->senseData;     
        dev->Cmd.scsi_SenseLength = sizeof(dev->senseData);              

        UBYTE* tmpBuffer = AllocVec(INQUIRE_BUFFER_SIZE+16, MEMF_PUBLIC);
        if (!tmpBuffer) {
            *errorCode = sworOutOfMem;
            _SCSIWifi_close(dev);
            return NULL;
        }
        SCSI_PREPCMD(dev, SCSI_INQUIRY, 0, 0, 0, INQUIRE_BUFFER_SIZE, 0);    
        dev->Cmd.scsi_Data = (UWORD*)tmpBuffer;     
        dev->Cmd.scsi_Length = INQUIRE_BUFFER_SIZE;        
        dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

        DoIO( (struct IORequest*)dev->SCSIReq );   
        // Failed
        if (dev->Cmd.scsi_Status) {
            *errorCode = sworInquireFail;
            FreeVec(tmpBuffer);
            _SCSIWifi_close(dev);
            return NULL;
        }
        // Check the result
        if (dev->Cmd.scsi_Actual > 26) {
            // A little hacky but will work for us
            tmpBuffer[13] = '\0';
            tmpBuffer[25] = '\0';
            // Check it's the device we're looking for
            if ((Stricmp(&tmpBuffer[8], "Dayna") == 0) && 
                (Stricmp(&tmpBuffer[16], "SCSI/Link") == 0)) {
                FreeVec(tmpBuffer);
                *errorCode = sworOK;
                return (SCSIWIFIDevice)dev;
            }      
        } 
        *errorCode = sworNotDaynaDevice;
        FreeVec(tmpBuffer);
        _SCSIWifi_close(dev);
    }
    return NULL;
}

// Close and free the open SCSI device
void SCSIWifi_close(SCSIWIFIDevice device) {
    if (!device) return;
    _SCSIWifi_close((LSCSIDevice)device);
}

// Triggers a WIFI scan.  Returns 1 if successful
LONG SCSIWifi_scan(SCSIWIFIDevice device, enum SCSIWifi_ScanStatus* status) {
    LSCSIDevice dev = (LSCSIDevice)device;
    *status = swssError;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_SCAN, 0, 0, 0, 0);    

    dev->Cmd.scsi_Data = (APTR)&dev->scsiCommand;
    dev->Cmd.scsi_Length = 4;                       // NEEDS to be 4
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    // Failed
    if (dev->Cmd.scsi_Status) return 0;

    // Check the result
    if (dev->Cmd.scsi_Actual == 1) {
        if ((char)dev->scsiCommand[6]==-1) 
                *status = swssBusy; else
                *status = swssError;
        return 1;
    }

    return 0;
}

// Check how a current WIFI scan is progressing
LONG SCSIWifi_scanComplete(SCSIWIFIDevice device, enum SCSIWifi_ScanStatus* status) {
    LSCSIDevice dev = (LSCSIDevice)device;

    *status = swssError;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_COMPLETE, 0, 0, 0, 0);    

    dev->Cmd.scsi_Data = (APTR)&dev->scsiCommand[6];
    dev->Cmd.scsi_Length = 4;                       // NEEDS to be 4
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    // Failed
    if (dev->Cmd.scsi_Status) return 0;

    // Check the result
    if (dev->Cmd.scsi_Actual == 1) {
        switch (dev->scsiCommand[6]) {
            case 1: *status = swssComplete; break;
            case 0: *status = swssBusy; break;
            default: *status = swssNotRunning; break;
        }
        return 1;
    }
    return 0;
}

// Get the results from the WIFI scan
LONG SCSIWifi_getScanResults(SCSIWIFIDevice device, struct SCSIWifi_ScanResults* results) {
    LSCSIDevice dev = (LSCSIDevice)device;
    memset(results, 0, sizeof(struct SCSIWifi_ScanResults));

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_SCAN_RESULTS, 0, 0, 0, 0);    

    dev->Cmd.scsi_Data = (APTR)results;       
    dev->Cmd.scsi_Length = sizeof(struct SCSIWifi_ScanResults);       
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    // Failed
    if (dev->Cmd.scsi_Status) return 0;

    // Check the result - the format exactly matches the struct we're supplying
    if (dev->Cmd.scsi_Actual >= 2) {
        UWORD size = results->count;   // couldn't get DIVIDE in VBCC working :(
        results->count = 0;
        while (size >= sizeof(struct SCSIWifi_NetworkEntry)) {
            size -= sizeof(struct SCSIWifi_NetworkEntry);
            results->count++;
        }
        return 1;
    }
    return 0;
}

// Enable/Disable the WIFI device (this actually resets its circular buffer)
LONG SCSIWifi_enable(SCSIWIFIDevice device, LONG setEnable) {
    LSCSIDevice dev = (LSCSIDevice)device;
    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_ENABLE, 0, 0, 0, 0, setEnable ? 0x80 : 0);    

    dev->Cmd.scsi_Data = NULL;       
    dev->Cmd.scsi_Length = 0;
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq ); 

    if (dev->Cmd.scsi_Status) return 0;

    return 1;
}

// Fetch the MAC address from the Wifi card
LONG SCSIWifi_getMACAddress(SCSIWIFIDevice device, struct SCSIWifi_MACAddress* macAddress) {
    LSCSIDevice dev = (LSCSIDevice)device;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD,  SCSI_NETWORK_WIFI_OPT_GETMACADDRESS, 0, 0, 0, 0);    

    macAddress->valid = 0;
    dev->Cmd.scsi_Data = (APTR)&dev->scsiCommand[6];
    dev->Cmd.scsi_Length = 6;
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    if (dev->Cmd.scsi_Status) return 0;

    if (dev->Cmd.scsi_Actual == 6) {
        memcpy(macAddress->address, &dev->scsiCommand[6], 6);
        macAddress->valid = 1;
        return 1;
    }
    return 0;
}

// Attempt ot join the specified WIFI network - Only way to find out if it worked is to periodically call SCSIWifi_getNetwork
LONG SCSIWifi_joinNetwork(SCSIWIFIDevice device, struct SCSIWifi_JoinRequest* wifi) {
    LSCSIDevice dev = (LSCSIDevice)device;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_JOIN, 0, 
        sizeof(struct SCSIWifi_JoinRequest) >> 8, 
        sizeof(struct SCSIWifi_JoinRequest) & 0xFF, 
        0);    

    dev->Cmd.scsi_Data = (APTR)wifi;       
    dev->Cmd.scsi_Length = sizeof(struct SCSIWifi_JoinRequest);         
    dev->Cmd.scsi_Flags = SCSIF_WRITE | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    if (dev->Cmd.scsi_Status) return 0;
    
    return 1;
}

// Fetch information about the currently connected network
LONG SCSIWifi_getNetwork(SCSIWIFIDevice device, struct SCSIWifi_NetworkEntry* connection) {
    LSCSIDevice dev = (LSCSIDevice)device;
    memset(connection, 0, sizeof(struct SCSIWifi_NetworkEntry));

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_INFO, 0, 0, 0, 0);

    UBYTE* netBuffer = AllocVec(sizeof(struct SCSIWifi_NetworkEntry) + 2,MEMF_PUBLIC|MEMF_CLEAR);
    if (!netBuffer) return 0;

    dev->Cmd.scsi_Data = (APTR)netBuffer;       
    dev->Cmd.scsi_Length = sizeof(struct SCSIWifi_NetworkEntry) + 2;   
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );   

    if (dev->Cmd.scsi_Status) {
        FreeVec(netBuffer);
        return 0;
    }

    // Check the result
    if (dev->Cmd.scsi_Actual > 2) {
        UWORD size = (netBuffer[0] << 8) + netBuffer[1];
        if (size > sizeof(struct SCSIWifi_NetworkEntry)) size = sizeof(struct SCSIWifi_NetworkEntry);
        if (size > dev->Cmd.scsi_Actual-2) size = dev->Cmd.scsi_Actual - 2;
        memcpy(connection, &netBuffer[2], size);
        FreeVec(netBuffer);

        return (size == sizeof(struct SCSIWifi_NetworkEntry)) ? 1 : 0;
    }

    FreeVec(netBuffer);
    return 0;
}

// Add a Multicast Ethernet address to the adapter 
LONG SCSIWifi_addMulticastAddress(SCSIWIFIDevice device, struct SCSIWifi_MACAddress* macAddress) {
    LSCSIDevice dev = (LSCSIDevice)device;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_ADDMULTICAST, 0, 0, 6, 0, 0);
    memcpy(&dev->scsiCommand[6], macAddress->address, 6);     

    dev->Cmd.scsi_Data = (APTR)&dev->scsiCommand[6];       
    dev->Cmd.scsi_Length = 6;
    dev->Cmd.scsi_Flags = SCSIF_WRITE | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );  

    LONG ret = 1;
    if (dev->Cmd.scsi_Status) ret = 0;    
    
    return ret;
}

// Send an ethernet frame (this is actually queued and sent inside the bluescsi/scsi2sd)
LONG SCSIWifi_sendFrame(SCSIWIFIDevice device, UBYTE* packet, UWORD packetSize) {
    LSCSIDevice dev = (LSCSIDevice)device;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_WRITEFRAME, 0, 0, packetSize >> 8, packetSize & 0xFF, 0);
    dev->Cmd.scsi_Data = (APTR)packet;
    dev->Cmd.scsi_Length = packetSize;
    dev->Cmd.scsi_Flags = SCSIF_WRITE | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq );     

    LONG ret = 1;
    if (dev->Cmd.scsi_Status) ret = 0;

    return ret;
}



// On ENTRY, packetSize should be the memory size of packetBuffer, which SHOULD be NETWORK_PACKET_MAX_SIZE + 6
// If returns TRUE and packetSize=0 then no data is waiting to be read
// Else packetSize will be what was read with the first 6 bytes being in the following format:
// packetSize will *need* to be NETWORK_PACKET_MAX_SIZE+6
//   Byte:   0 High Byte of packet size
//           1: Low Byte of packet size
//           2: 0xF8 - magic number. If its NOT this then the device is NOT running the right firmware
//           3, 4 = 0
//           5: 0 if this was the last packet, or 0x10 if there are more to read
//      last 4 bytes are the CRC for the packet
LONG SCSIWifi_receiveFrame(SCSIWIFIDevice device, UBYTE* packetBuffer, UWORD* packetSize) {
    LSCSIDevice dev = (LSCSIDevice)device;

    SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_READFRAME, 0, 0xF8, (*packetSize) >> 8, (*packetSize) & 0xFF, 0);

    dev->Cmd.scsi_Data = (APTR)packetBuffer;
    dev->Cmd.scsi_Length = *packetSize;
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

#ifdef DEBUG
    //for (int a=0; a<*packetSize; a++) packetBuffer[a] = (a) & 0xFF;
#endif 

    DoIO( (struct IORequest*)dev->SCSIReq ); 

    LONG ret = 1;
    if ((dev->Cmd.scsi_Status) || (dev->Cmd.scsi_Actual < 6)) ret = 0;

#ifdef DEBUG
    if (packetBuffer[2] != 0xF8) D(("WARNING: Scsi device is running old firmware"));
#endif    
    *packetSize = dev->Cmd.scsi_Actual;

    return ret;
}
