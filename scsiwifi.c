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
#define SCSI_NETWORK_WIFI_OPT_ALTREAD       0x08    
#define SCSI_NETWORK_WIFI_OPT_GETMACADDRESS 0x09    

#define INQUIRE_BUFFER_SIZE                 64

#define NUM_TOKENS 7
static char* CONFIG_TOKENS[NUM_TOKENS] = {"DEVICE","DEVICEID","PRIORITY","MODE","AUTOCONNECT","SSID","KEY"};

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
    char senseData[20];
    USHORT scsiMode;
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
void _ustoa(USHORT num, char* str) {
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
    *str++ = '\0';
}

// convert SHORT to string and appends a new line character
void _stoa(SHORT num, char* str) {
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

// Looks at data, which should be TOKEN=VALUE format.
// If valid then the value will be populated to the first character after the "=" symbol
// Returns 0 if the line is invalid
LONG tokeniseSetting(char* data, char** value) {
    *value = NULL;
    while (*data) {
        if (*data == '=') {
            *data = '\0';
            *value = data+1;
            return 1;
        } 
        data++;
    }
    return 0;
}

// Safe (I hope ;) implementation to prevent buffer overflows
void strcpy_s(char* dest, char* src, USHORT maxLength) {
    USHORT i = strlen(src);
    if (i>=maxLength) i = maxLength;
    memcpy(dest, src, i-1);
    dest[i-1] = '\0';
}

// Removes any trailing newline characters
void removeNL(char* text) {
    while (*text) {
        if (*text == '\n') {
            *text='\0';
            return;
        }
        text++;
    }
}

// Populates settings with default values
void SCSIWifi_defaultSettings(struct ScsiDaynaSettings* settings) {
    strcpy(settings->deviceName, "scsi.device");
    settings->deviceID = -1;  // auto detect
    settings->taskPriority = 0;  // -128 to 127  - probably should be 0 but works faster set as 1!
    settings->scsiMode = 1;      // Driver mode. 0=DynaPORT, 1=24 Byte Patch (scsi.device), 2=Single Write Mode (gvpscsi.device)
    settings->autoConnect = 0;   // auto connect to the WIFI?
    strcpy(settings->ssid, "");
    strcpy(settings->key, "");
}

// Loads settings from the ENV, returns 0 if the settings were bad and defaults were setup
LONG SCSIWifi_loadSettings(void *utilityBase, void* dosBase, struct ScsiDaynaSettings* settings) {
    struct SCSIDevice devTmp;
    LSCSIDevice dev = &devTmp;
    devTmp.sc_dosBase = dosBase;
    devTmp.sc_UtilityBase = utilityBase;

    USHORT modeConfigured = 0;
    SCSIWifi_defaultSettings(settings);
    BPTR fh;
    if (fh = Open("ENV:scsidayna.prefs",MODE_OLDFILE)) {
        char buffer[128];
        USHORT matches = 0;
        while (FGets(fh, buffer, 128)) {
            char* value;
            if (tokeniseSetting(buffer, &value)) {
                // find a match
                for (USHORT token = 0; token < NUM_TOKENS; token++) {
                    matches++;
                    if (Stricmp(CONFIG_TOKENS[token], buffer) == 0) {
                        switch (token) {
                            case 0: strcpy_s(settings->deviceName, value, 108); break;
                            case 1: settings->deviceID = _atos(value); break;
                            case 2: settings->taskPriority = _atos(value); 
                                    if (settings->taskPriority>127) settings->taskPriority = 127;
                                    if (settings->taskPriority<-128) settings->taskPriority = -128;
                                    break;
                            case 3: settings->scsiMode = _atous(value); 
                                    if (settings->scsiMode>2) settings->scsiMode=2;
                                    modeConfigured = 1;
                                    break;
                            case 4: settings->autoConnect = _atous(value); break;
                            case 5: strcpy_s(settings->ssid, value, 64); break;
                            case 6: strcpy_s(settings->key, value, 64); break;
                            default: matches--; break;
                        }
                        break;
                    }
                }
            }
        }
        if (matches < 1) SCSIWifi_defaultSettings(settings);
        Close(fh);
        return matches > 0;
    }

    // If no mode was set, but a GVP device was specified then jump to mode 2. It will default to 1 anyway
    if (!modeConfigured) {
        if ((ToUpper(settings->deviceName[0]) == 'G') && (ToUpper(settings->deviceName[0]) == 'V') && (ToUpper(settings->deviceName[0]) == 'P')) settings->scsiMode = 2;
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
        // Save each setting in tern
        USHORT good = 1;
        char tmp[20];  // temp buffer
        for (USHORT token = 0; token < NUM_TOKENS; token++) {
            if (!FPuts(fh, CONFIG_TOKENS[token])) good = 0;
            if (!FPuts(fh, "=")) good = 0;
            switch (token) {
                case 0:  if (!FPuts(fh, settings->deviceName)) good = 0; break;
                case 1:  _stoa(settings->deviceID, tmp);  if (!FPuts(fh, tmp)) good = 0; break;
                case 2:  _stoa(settings->taskPriority, tmp);  if (!FPuts(fh, tmp)) good = 0; break;
                case 3:  _ustoa(settings->scsiMode, tmp);  if (!FPuts(fh, tmp)) good = 0; break;
                case 4:  _ustoa(settings->autoConnect, tmp);  if (!FPuts(fh, tmp)) good = 0; break;
                case 5:  if (!FPuts(fh, settings->ssid)) good = 0; break;
                case 6:  if (!FPuts(fh, settings->key)) good = 0; break;
            }
            if (!FPuts(fh, "\n")) good = 0;
        }

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
        dev->scsiMode = openData->scsiMode;

        // Setup the SCSI command structure    
        dev->SCSIReq->io_Length  = sizeof(struct SCSICmd);
        dev->SCSIReq->io_Data    = (APTR)&dev->Cmd;
        dev->SCSIReq->io_Command = HD_SCSICMD; 
         
        dev->Cmd.scsi_CmdLength = 6;  
        dev->Cmd.scsi_Command   = dev->scsiCommand;                  
        dev->Cmd.scsi_SenseData = (UBYTE*)&dev->senseData;     
        dev->Cmd.scsi_SenseLength = 20;              

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

    if (dev->Cmd.scsi_Status) return 0;
    return 1;
}



// On ENTRY, packetSize should be the memory size of packetBuffer, which SHOULD be NETWORK_PACKET_MAX_SIZE + 6
// If returns TRUE and packetSize=0 then no data is waiting to be read
// Else packetSize will be what was read with the first 6 bytes being in the following format:
// packetSize will *need* to be NETWORK_PACKET_MAX_SIZE+6
//   Byte:   0 High Byte of packet size
//           1: Low Byte of packet size
//           2: 0xA8/A8/0 - magic number. 
//           3, 4 = 0
//           5: 0 if this was the last packet, or 0x10 if there are more to read
//      last 4 bytes are the CRC for the packet which we dont care about!
LONG SCSIWifi_receiveFrame(SCSIWIFIDevice device, UBYTE* packetBuffer, UWORD* packetSize) {
    LSCSIDevice dev = (LSCSIDevice)device;

   switch (dev->scsiMode) {
       case 1:  // scsi.device mode
            SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_ALTREAD,  0xA8, (*packetSize) >> 8, (*packetSize) & 0xFF, 0);
            break;

        case 2:  // gvpscsi.device mode
            SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_CMD, SCSI_NETWORK_WIFI_OPT_ALTREAD,  0xA9, (*packetSize) >> 8, (*packetSize) & 0xFF, 0);
            break;

        default:
            SCSI_PREPCMD(dev, SCSI_NETWORK_WIFI_READFRAME, 0, 0, (*packetSize) >> 8, (*packetSize) & 0xFF, 0);
            break;
    }
    dev->Cmd.scsi_Data = (APTR)packetBuffer;
    dev->Cmd.scsi_Length = *packetSize;
    dev->Cmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    DoIO( (struct IORequest*)dev->SCSIReq ); 

    if ((dev->Cmd.scsi_Status) || (dev->Cmd.scsi_Actual < 6)) return 0;

    *packetSize = dev->Cmd.scsi_Actual;

    return 1;
}
