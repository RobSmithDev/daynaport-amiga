/*
 * SCSI DaynaPORT Device (scsidayna.device) Copyright (C) 2024-2026 RobSmithDev 
 *
 *
 * Originally based on the MNT ZZ9000 Network Driver Copyright (C) 2016-2023, Lukas F. Hartmann <lukas@mntre.com>
 *          which was Based on code copyright (C) 2018 Henryk Richter <henryk.richter@gmx.net>
 * Released under GPLv3+ with permission.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#define DEVICE_MAIN

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <exec/types.h>
#include <dos/dostags.h>
#include <exec/ports.h>
#include <utility/tagitem.h>
#include <exec/lists.h>
#include <exec/errors.h>
#include <exec/tasks.h>
#include <proto/utility.h>
#include <exec/execbase.h>
#include "scsiwifi.h"
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "sana2.h"


#ifdef HAVE_VERSION_H
#include "version.h"
#endif

#include "newstyle.h"
#include "device.h"
#include "macros.h"

const UWORD dev_supportedcmds[] = { NSCMD_DEVICEQUERY, CMD_READ, CMD_WRITE, /*S2_SANA2HOOK, */S2_GETGLOBALSTATS, S2_BROADCAST, CMD_WRITE, S2_ONEVENT, S2_READORPHAN, S2_ONLINE, S2_OFFLINE, S2_GETSTATIONADDRESS, S2_DEVICEQUERY, S2_GETSPECIALSTATS, 0 };


#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

__saveds void frame_proc();
char *frame_proc_name = "AmigaNetPacketScheduler";

static UBYTE HW_MAC[] = {0x00,0x00,0x00,0x00,0x00,0x00};

struct ProcInit {
   struct Message msg;
   struct devbase *db;
   BOOL  error;
   UBYTE pad[2];
};

// Free's anything left after init
void freeInit(DEVBASEP) {
	if (db->db_scsiSettings) FreeVec(db->db_scsiSettings); 
	db->db_scsiSettings = NULL;
	if (db->db_debugConsole) Close(db->db_debugConsole);
	db->db_debugConsole = 0;
	if (DOSBase) CloseLibrary(DOSBase); 
	DOSBase = NULL;
	if (UtilityBase) CloseLibrary(UtilityBase); 
	UtilityBase = NULL;
}

void DevTermIO( DEVBASEP, struct IORequest *ioreq );

// Simple logging to console window
void logMessage(struct devbase* db, const char *message) {
	struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
	if (!settings->debug) return;
	if (!db->db_debugConsole) db->db_debugConsole = Open("CON:10/10/620/100/SCSIDayna Debug Output (RobSmithDev)/AUTO/CLOSE/WAIT", MODE_NEWFILE);
    if (db->db_debugConsole) {
		FPuts(db->db_debugConsole, message);
		FPuts(db->db_debugConsole, "\n");
	}
}

// Formatted logging to console window
static const UWORD stuffChar[] = {0x16c0, 0x4e75};
void logMessagef(struct devbase* db, const char *messageFormat, ...) {
	struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
	if (!settings->debug) return;
	char buf[100];
    va_list args;
    va_start(args, messageFormat);	 
    RawDoFmt((STRPTR)messageFormat, (APTR)args, (void (*)(void))stuffChar, buf);
	va_end(args);
	logMessage(db, buf);
}

// Simple device init that saves all the real errors until later
__saveds struct Device *DevInit( ASMR(d0) DEVBASEP ASMREG(d0), ASMR(a0) BPTR seglist ASMREG(a0), ASMR(a6) struct Library *_SysBase  ASMREG(a6) ) {	
	db->db_SysBase = _SysBase;
	db->db_SegList = seglist;
	db->db_DOSBase = NULL;
	db->db_UtilityBase = NULL;
	db->db_scsiSettings = NULL;
	db->db_online = 0;
	db->db_decrementCountOnFail = 0;
	db->db_debugConsole = 0;
	db->db_amigaNetMode = 0;
  
	DOSBase = OpenLibrary("dos.library", 36);
	if (!DOSBase) {
		D(("scsidayna: Failed to open dos.library (36)\n"));
		return 0;
	}

	UtilityBase = OpenLibrary("utility.library", 37);
	if (!UtilityBase) {		
		D(("scsidayna: Failed to open utility.library (37)\n"));
		freeInit(db);
		return 0;
	}

	// Load in the settings.  Theres a few
	db->db_scsiSettings = AllocVec(sizeof(struct ScsiDaynaSettings),MEMF_CLEAR);
	if (!db->db_scsiSettings) {
		D(("scsidayna: Out of memory (settings)\n"));
		freeInit(db);
		return 0;
	}
	
	struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
	if (SCSIWifi_loadSettings((void*)UtilityBase, (void*)DOSBase, settings))
		D(("scsidayna: settings loaded")); 
	else D(("scsidayna: Invalid or missing settings file, reverting to defaults\n"));
	
	// Log config on startup
	if (settings->debug) {
		logMessage(db, "Loaded Configuration:");
		logMessagef(db, "	SCSI Device: %s", settings->deviceName);
		if ((settings->deviceID<0) || (settings->deviceID>7)) logMessage(db, "	Unit ID: Auto Detect"); else logMessagef(db, "	Unit ID: %ld", settings->deviceID);
		logMessagef(db, "	Priority: %ld", settings->taskPriority);
		logMessagef(db, "	Max Transfer Size: %ld", settings->maxDataSize);
		logMessagef(db, "	Mode: %ld", settings->scsiMode);
		if (settings->autoConnect) {
			logMessagef(db, "	Auto Connect Wifi: Yes");
			logMessagef(db, "	SSID: %s", settings->ssid);
			logMessagef(db, "	Key: %s", settings->key);
		} else {
			logMessagef(db, "	Auto Connect Wifi: No");
		}
	}
  
	return (struct Device*)db;
}

// Return an error and clean up
LONG returnError(struct devbase* db, struct IOSana2Req *ioreq, LONG errorCode) {
	ioreq->ios2_Req.io_Error = errorCode; 
	ioreq->ios2_Req.io_Unit = (struct Unit *)0;   
	ioreq->ios2_Req.io_Device = (struct Device *)0;	
	if (db->db_decrementCountOnFail) {
		db->db_decrementCountOnFail = 0;
		db->db_Lib.lib_OpenCnt--;
	}
	return errorCode;
}

// Device open!
__saveds LONG DevOpen( ASMR(a1) struct IOSana2Req *ioreq ASMREG(a1), ASMR(d0) ULONG unit ASMREG(d0), ASMR(d1) ULONG flags ASMREG(d1), ASMR(a6) DEVBASEP ASMREG(a6) ) {		
	struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
	db->db_decrementCountOnFail = 0;		
			
    // promiscuous mode not supported
	if ((flags & SANA2OPF_PROM) && (unit)) {
		logMessagef(db, "DevOpen: SANA2OPF_PROM not supported on unit id %ld", unit);
		return returnError(db, ioreq, IOERR_OPENFAIL);
	}	

	if (strlen(settings->deviceName)<1) {
		logMessage(db, "DevOpen: CSI device name not set");
		return returnError(db, ioreq, IOERR_OPENFAIL);
	}
		
	db->db_Lib.lib_OpenCnt++; /* avoid Expunge, see below for separate "unit" open count */			
	db->db_decrementCountOnFail = 1;
	if (unit==0 && db->db_Lib.lib_OpenCnt==1) {		
		SCSIWIFIDevice* wifiDevice = NULL;
		struct SCSIDevice_OpenData openData;
		openData.sysBase = (struct ExecBase*)SysBase;
		openData.utilityBase = (void*)UtilityBase;
		openData.dosBase = (void*)DOSBase;
		openData.deviceDriverName = settings->deviceName;
		openData.deviceID = settings->deviceID;
		openData.scsiMode = settings->scsiMode;
		enum SCSIWifi_OpenResult scsiResult;
	
		// Open it
		if ((settings->deviceID<0) || (settings->deviceID>7)) {			
			D(("scsidayna: Searching for DaynaPORT Device to Configure\n"));
			// Highly likely it will be on 4 as its in the example so start there!
			for (USHORT deviceID=4; deviceID<4+8; deviceID++) {
				openData.deviceID = deviceID & 7;  
				D(("scsidayna: Searching on DeviceID %ld\n", openData.deviceID));
				wifiDevice = SCSIWifi_open(&openData, &scsiResult);
				if (wifiDevice) {
					settings->deviceID = openData.deviceID;
					logMessagef(db, "DevOpen: Detected Network Device on Unit %ld", openData.deviceID );
					break;
				}
			}
		} else {			
			wifiDevice = SCSIWifi_open(&openData, &scsiResult);
		}
			
		if (!wifiDevice) {
			switch (scsiResult) {
				case sworOpenDeviceFailed: 	logMessagef(db, "DevOpen: Failed to open SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;  
				case sworOutOfMem:  		logMessagef(db, "DevOpen: Out of memory opening SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				case sworInquireFail:      	logMessagef(db, "DevOpen: Inquiry of SCSI device failed \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				case sworNotDaynaDevice:   	logMessagef(db, "DevOpen: Device is not a DaynaPort SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				default: logMessagef(db, "DevOpen:  Unknown error occured opening device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
			}
			return returnError(db, ioreq, IOERR_OPENFAIL);
		}
		
		if (scsiResult == sworGreat) {
			db->db_amigaNetMode = 1;
			logMessagef(db, "DevOpen: AmigaNET Interface Detected"); 
			// Device open. Fetch MAC address
			struct SCSIWifi_DeviceInfo devInfo;
			if (!SCSIWifi_getDeviceInfo(wifiDevice, &devInfo)) {
				logMessagef(db, "DevOpen: Failed to fetch device info from Device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); 
				SCSIWifi_close(wifiDevice);
				return returnError(db, ioreq, IOERR_OPENFAIL);
			}
			// Take a copy of the MAC Address
			memcpy(HW_MAC, devInfo.macAddress, 6);
			db->db_maxPacketsSize = devInfo.maxPacketsSize;
			db->db_maxPackets = devInfo.maxPackets;
			D(("scsidayna: MAC Address stored, checking WIFI status\n"));
			logMessagef(db, "DevOpen: Max Data Transfer Size: %ld  (limited to %ld), Max Packets: %ld",db->db_maxPacketsSize, settings->maxDataSize, db->db_maxPackets);
			if (db->db_maxPacketsSize > settings->maxDataSize) db->db_maxPacketsSize = settings->maxDataSize;
			// Ensure db->db_maxPacketsSize is even
			if (db->db_maxPacketsSize&1) db->db_maxPacketsSize++;
		} else {
			db->db_amigaNetMode = 0;
			db->db_maxPacketsSize = 0;
			db->db_maxPackets = 0;
			logMessagef(db, "DevOpen: Legacy Daynaport Interface Detected (Upgrade SCSI Firmware)"); 
			// Device open. Fetch MAC address
			struct SCSIWifi_MACAddress macAddress;
			if (!SCSIWifi_getMACAddress(wifiDevice, &macAddress)) {
				logMessagef(db, "DevOpen: Failed to fetch MAC Address from Device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); 
				SCSIWifi_close(wifiDevice);
				return returnError(db, ioreq, IOERR_OPENFAIL);
			}
			// Take a copy of the MAC Address
			D(("scsidayna: MAC Address stored, checking WIFI status\n"));
			memcpy(HW_MAC, macAddress.address, 6);
		}				
		logMessagef(db, "DevOpen: MAC Address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx",HW_MAC[0],HW_MAC[1],HW_MAC[2],HW_MAC[3],HW_MAC[4],HW_MAC[5]); 
			
		
		// Should we be attempting to connect to wifi?
		if ((settings->autoConnect) && (strlen(settings->ssid))) {
			// Fetch what the current network is
			struct SCSIWifi_NetworkEntry currentNetwork;
			if (!SCSIWifi_getNetwork(wifiDevice, &currentNetwork)) memset(&currentNetwork, 0, sizeof(struct SCSIWifi_NetworkEntry));
			if (strcmp(currentNetwork.ssid, settings->ssid) == 0) {
				logMessage(db, "DevOpen: Already Connected to Specified WIFI Network"); 
				D(("scsidayna: Already connected to requested WIFI network\n"));
			} else {
				struct SCSIWifi_JoinRequest request;
				strcpy(request.ssid, settings->ssid);
				strcpy(request.key, settings->key);
				SCSIWifi_joinNetwork(wifiDevice, &request);
				logMessage(db, "DevOpen: Requesting to Join WIFI Network"); 
				D(("scsidayna: Attempting to connect to WIFI network\n"));     
			}
		}
		SCSIWifi_close(wifiDevice);		
		D(("scsidayna: SCSI Device OK for %ld\n",unit));
		
		struct BufferManagement *bm;
		if ((bm = (struct BufferManagement*)AllocVec(sizeof(struct BufferManagement), MEMF_CLEAR|MEMF_PUBLIC))) {
			bm->bm_CopyToBuffer = (BMFunc)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
			bm->bm_CopyFromBuffer = (BMFunc)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement); 
			
			ioreq->ios2_BufferManagement = (VOID *)bm;
			ioreq->ios2_Req.io_Error = 0;
			ioreq->ios2_Req.io_Unit = (struct Unit *)unit; // not a real pointer, but id integer
			ioreq->ios2_Req.io_Device = (struct Device *)db;

			NewList(&db->db_ReadList);			InitSemaphore(&db->db_ReadListSem);
			NewList(&db->db_WriteList);			InitSemaphore(&db->db_WriteListSem);
			NewList(&db->db_EventList);			InitSemaphore(&db->db_EventListSem);
			NewList(&db->db_ReadOrphanList); 	InitSemaphore(&db->db_ReadOrphanListSem);

			InitSemaphore(&db->db_ProcSem);
			db->db_online = 1;

			struct ProcInit init;
			struct MsgPort *port;

			if (port = CreateMsgPort()) {
				D(("scsidayna: Starting Server\n"));
				if (db->db_Proc = CreateNewProcTags(NP_Entry, frame_proc, NP_Name, frame_proc_name, NP_Priority, 0, TAG_DONE)) {
					init.error = 1;
					init.db = db;
					init.msg.mn_Length = sizeof(init);
					init.msg.mn_ReplyPort = port;

					D(("scsidayna: handover db: %lx\n",init.db));
					PutMsg(&db->db_Proc->pr_MsgPort, (struct Message*)&init);
					WaitPort(port);

					if (init.error) {
						logMessagef(db,"DevOpen: Process startup error"); 
						return returnError(db, ioreq, IOERR_OPENFAIL);
					}
				} else {
					logMessagef(db,"DevOpen: Couldn't create process"); 
					return returnError(db, ioreq, IOERR_OPENFAIL);
				}
				DeleteMsgPort(port);
			} else {
				logMessagef(db,"DevOpen: Failed to create message port"); 
				return returnError(db, ioreq, IOERR_OPENFAIL);
			}
		}
	}
		
	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	db->db_Lib.lib_Flags &= ~LIBF_DELEXP;
	
	D(("scsidayna: DevOpen\n"));
	logMessage(db, "DevOpen: Ready"); 
	return 0;
}

__saveds BPTR DevClose( ASMR(a1) struct IORequest *ioreq ASMREG(a1), ASMR(a6) DEVBASEP ASMREG(a6) ) {
	BPTR  ret = (0);

	D(("scsidayna: DevClose open count %ld\n",db->db_Lib.lib_OpenCnt));
	if (!ioreq) return ret;
	db->db_Lib.lib_OpenCnt--;

	if (db->db_Lib.lib_OpenCnt == 0) {
		if (db->db_Proc) {
			D(("scsidayna: End Proc...\n"));
			Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_C);

			// Wait for shutdown
			ObtainSemaphore(&db->db_ProcSem);
			ReleaseSemaphore(&db->db_ProcSem);
			db->db_Proc = 0;
		}   
	}
	
	ioreq->io_Device = (0);
	ioreq->io_Unit   = (struct Unit *)(-1);
	if (db->db_Lib.lib_Flags & LIBF_DELEXP) ret = DevExpunge(db);

	return ret;
}

__saveds BPTR DevExpunge( ASMR(a6) DEVBASEP ASMREG(a6) ) {
	BPTR seglist = db->db_SegList;

	if( db->db_Lib.lib_OpenCnt ) {
		db->db_Lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}

	D(("scsidayna: Remove Device Node...\n"));
	Remove((struct Node*)db);

	freeInit(db);
	FreeMem( ((BYTE*)db)-db->db_Lib.lib_NegSize,(ULONG)(db->db_Lib.lib_PosSize + db->db_Lib.lib_NegSize));

	return seglist;
}

__saveds VOID DevBeginIO( ASMR(a1) struct IOSana2Req *ioreq ASMREG(a1), ASMR(a6) DEVBASEP ASMREG(a6) ) {    
	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
	ioreq->ios2_Req.io_Error = S2ERR_NO_ERROR;
	ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;

	switch( ioreq->ios2_Req.io_Command ) {
	case NSCMD_DEVICEQUERY: 
		{
			D(("NSCMD_DEVICEQUERY"));
			struct IOStdReq *ioreq2 = (struct IOStdReq *)ioreq;
			ULONG size = sizeof(struct NSDeviceQueryResult);
			if((ioreq2->io_Data == NULL) || (ioreq2->io_Length < size)) {
				ioreq2->io_Error = IOERR_BADLENGTH;
			}
			else {
				struct NSDeviceQueryResult *info = ioreq2->io_Data;
				ioreq2->io_Error = 0;
				ioreq2->io_Actual = size;
				info->SizeAvailable = size;
				info->DevQueryFormat = 0;
				info->DeviceType = NSDEVTYPE_SANA2;
				info->DeviceSubType = 0;
				info->SupportedCommands = (APTR)dev_supportedcmds;
			}

			if (!(ioreq2->io_Flags & IOF_QUICK)) {
				ioreq2->io_Message.mn_Node.ln_Type = NT_MESSAGE;
				ReplyMsg((struct Message *)ioreq2);
			}
			else {
				/* otherwise just mark it as done */
				ioreq2->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
			}		
			return;
		}
		break;
		
	case CMD_READ:
		if (ioreq->ios2_BufferManagement == NULL) {
			ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
			ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
		} else if (!db->db_currentWifiState) {
			ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			ioreq->ios2_WireError = S2WERR_UNIT_OFFLINE;
		} else {
			ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
			ObtainSemaphore(&db->db_ReadListSem);
			AddTail((struct List*)&db->db_ReadList, (struct Node*)ioreq);
			ReleaseSemaphore(&db->db_ReadListSem);
			ioreq = NULL;
		}
		break;

	case S2_GETGLOBALSTATS:
		memcpy(ioreq->ios2_StatData, &db->db_DevStats, sizeof(struct Sana2DeviceStats));
		break;

	case S2_BROADCAST:   
		if (ioreq->ios2_DstAddr) {
			memset(ioreq->ios2_DstAddr, 0xFF, HW_ADDRFIELDSIZE);
		} else {
			D(("bcast: invalid dst addr\n"));
			ioreq->ios2_Req.io_Error = S2ERR_BAD_ADDRESS;
			ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
		}
		// fall through!	
	case CMD_WRITE: 
		if (ioreq->ios2_BufferManagement == NULL) {
			ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
			ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
		} else if (!db->db_currentWifiState) {
			ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			ioreq->ios2_WireError = S2WERR_UNIT_OFFLINE;
		} else {	
			ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
			ioreq->ios2_Req.io_Error = 0;
			ObtainSemaphore(&db->db_WriteListSem);
			// The sending process reads from the head of the list,
			// so add to the tail here, otherwise packets could go out in swapped order
			AddTail((struct List*)&db->db_WriteList, (struct Node*)ioreq);
			ReleaseSemaphore(&db->db_WriteListSem);
			Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_F);
			ioreq = NULL;
		}
		break;
  
    case S2_ONEVENT:
      if (((ioreq->ios2_WireError & S2EVENT_ONLINE) && (db->db_currentWifiState)) ||
         ((ioreq->ios2_WireError & S2EVENT_OFFLINE) && (!db->db_currentWifiState))) {
           ioreq->ios2_Req.io_Error = 0;
           ioreq->ios2_WireError &= (S2EVENT_ONLINE|S2EVENT_OFFLINE);
           DevTermIO(db, (struct IORequest*)ioreq);
           ioreq = NULL;
      } else
      if ((ioreq->ios2_WireError & (S2EVENT_ONLINE|S2EVENT_OFFLINE|S2EVENT_ERROR|S2EVENT_TX|S2EVENT_RX|S2EVENT_BUFF|S2EVENT_HARDWARE|S2EVENT_SOFTWARE)) != ioreq->ios2_WireError) {
        // we cannot handle such events 
        ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
        ioreq->ios2_WireError = S2WERR_BAD_EVENT;
      }
      else {
        // Queue anything else 
        ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
        ObtainSemaphore(&db->db_EventListSem);
        AddTail((struct List*)&db->db_EventList, (struct Node*)ioreq);
        ReleaseSemaphore(&db->db_EventListSem);
        ioreq = NULL;
      }
      break;  

	case S2_READORPHAN:
		if (ioreq->ios2_BufferManagement == NULL) {
			ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
			ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
		} else if (!db->db_currentWifiState) {
			ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
			ioreq->ios2_WireError = S2WERR_UNIT_OFFLINE;
		} else {                      
			ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
			ObtainSemaphore(&db->db_ReadOrphanListSem);
			AddTail((struct List*)&db->db_ReadOrphanList, (struct Node*)ioreq);
			ReleaseSemaphore(&db->db_ReadOrphanListSem);
			ioreq = NULL;
		}
		break;      

	case S2_ONLINE:
		db->db_online = 1;
		break;

	case S2_OFFLINE:
		db->db_online = 0;
		break;

	case S2_CONFIGINTERFACE:   
		break;

	case S2_GETSTATIONADDRESS:
		memcpy(ioreq->ios2_SrcAddr, HW_MAC, HW_ADDRFIELDSIZE); /* current */
		memcpy(ioreq->ios2_DstAddr, HW_MAC, HW_ADDRFIELDSIZE); /* default */
		break;
		
	case S2_DEVICEQUERY: {
			struct Sana2DeviceQuery *devquery;
			devquery = ioreq->ios2_StatData;
			devquery->DevQueryFormat = 0;        // this is format 0
			devquery->DeviceLevel = 0;           // this spec defines level 0
			if (devquery->SizeAvailable >= 18) devquery->AddrFieldSize = HW_ADDRFIELDSIZE * 8; // in bits
			if (devquery->SizeAvailable >= 22) devquery->MTU           = SCSIWIFI_PACKET_MTU_SIZE; // max size
			if (devquery->SizeAvailable >= 26) devquery->BPS           = 1000*1000*100;   // unlikely
			if (devquery->SizeAvailable >= 30) devquery->HardwareType  = S2WireType_Ethernet;
			if (devquery->SizeAvailable >= 34) devquery->RawMTU        = SCSIWIFI_PACKET_MAX_SIZE;
			devquery->SizeSupplied = (devquery->SizeAvailable<34?devquery->SizeAvailable:34);
		}
		break;
	case S2_GETSPECIALSTATS:
		{
		  struct Sana2SpecialStatHeader *s2ssh = (struct Sana2SpecialStatHeader *)ioreq->ios2_StatData;
		  s2ssh->RecordCountSupplied = 0;
		}
		break;
			/*
	case S2_SANA2HOOK:
		{			
			struct Sana2Hook *s2h = (struct Sana2Hook *)ioreq->ios2_Data;
			db->db_logHook = NULL;
			if (s2h && s2h->s2h_Methods) {
				struct TagItem *state = (struct TagItem *)s2h->s2h_Methods;
				struct TagItem *tag;

				while ((tag = NextTagItem(&state))) {
					D(("Tag %d", tag->ti_Tag));
					if (tag->ti_Tag == S2_Log) {
						db->db_logHook = &s2h->s2h_Hook;
						break;
					}
				}
			}
			
			ioreq->ios2_Req.io_Error = 0;
			ReplyMsg((struct Message *)ioreq);
			return;
		}		
*/			
	default:
		{
			ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
			ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;
			break;
		}
	}

	if (ioreq) DevTermIO(db, (struct IORequest*)ioreq);
}

// SANA-2 Event management
void DoEvent(DEVBASEP, long event) {
	struct IOSana2Req *ior, *ior2;
	D(("event is %lx\n",event));

	ObtainSemaphore(&db->db_EventListSem );

	for(ior = (struct IOSana2Req *) db->db_EventList.lh_Head; (ior2 = (struct IOSana2Req *) ior->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL; ior = ior2 ) {
		if (ior->ios2_WireError & event) {
			Remove((struct Node*)ior);
			DevTermIO(db, (struct IORequest *)ior);
		}
	}
	ReleaseSemaphore(&db->db_EventListSem );
}

__saveds LONG DevAbortIO( ASMR(a1) struct IORequest *ioreq ASMREG(a1), ASMR(a6) DEVBASEP ASMREG(a6) ) {
	LONG   ret = 0;
	struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;

	D(("scsidayna: AbortIO on %lx\n",(ULONG)ioreq));

	Remove((struct Node*)ioreq);

	ioreq->io_Error = IOERR_ABORTED;
	ios2->ios2_WireError = 0;

	ReplyMsg((struct Message*)ioreq);
	return ret;
}

void DevTermIO( DEVBASEP, struct IORequest *ioreq ) {
	struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;

	if (!(ios2->ios2_Req.io_Flags & SANA2IOF_QUICK)) {
		ReplyMsg((struct Message *)ioreq);
	} else {
		ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	}
}


ULONG write_frame(struct IOSana2Req *req, UBYTE* frame, SCSIWIFIDevice scsiDevice, DEVBASEP) {
   USHORT sz=0;
   UBYTE* inputFrame = frame;

	// Calculate packet size
   if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
      sz = req->ios2_DataLength;
   } else {
      sz = req->ios2_DataLength + HW_ETH_HDR_SIZE;
      *((USHORT*)(frame+6+6)) = (USHORT)req->ios2_PacketType;
      memcpy(frame, req->ios2_DstAddr, HW_ADDRFIELDSIZE);
      memcpy(frame+6, HW_MAC, HW_ADDRFIELDSIZE);
      frame+=HW_ETH_HDR_SIZE;
   }
   
   // Block packets that are too big
   if(sz > SCSIWIFI_PACKET_MAX_SIZE) {
	  req->ios2_Req.io_Error  = S2ERR_MTU_EXCEEDED;
	  req->ios2_WireError = S2WERR_BUFF_ERROR;
	  DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
	  D(("MTU Buffer Exceeded"));
	  return 0;
   }
   
   // Skip if no packet
   if (sz < 1) {
	   D(("Zero size packet rejected"));
	   DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
	   return 0;
   }

   struct BufferManagement *bm = (struct BufferManagement *)req->ios2_BufferManagement;

	// Copy the buffer 
	if (!(*bm->bm_CopyFromBuffer)(frame, req->ios2_Data, req->ios2_DataLength)) {
		req->ios2_Req.io_Error = S2ERR_SOFTWARE;
		req->ios2_WireError = S2WERR_BUFF_ERROR;
		DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
		D(("bm_CopyFromBuffer FAIL"));
		return 0;
	}
	
	// Send it
	if (SCSIWifi_sendFrame(scsiDevice, inputFrame, sz)) {
		req->ios2_Req.io_Error = req->ios2_WireError = 0;
		db->db_DevStats.PacketsSent++;
		return 1;
	} else {
		req->ios2_Req.io_Error = S2ERR_TX_FAILURE;
		req->ios2_WireError = S2WERR_GENERIC_ERROR;
		DoEvent(db, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);
		D(("SEND FAIL"));
		return 0;
	}
}

ULONG read_frame(DEVBASEP, struct IOSana2Req *req, UBYTE *frm, USHORT packetSize) {
	ULONG datasize;
	BYTE *frame_ptr;
	BOOL broadcast;
	ULONG res = 0;

	// This length includes 4 bytes for the CRC at the end, but we dont need that
	ULONG sz   = ((ULONG)frm[0]<<8)|((ULONG)frm[1]);
	if (sz<4) return 1;
	sz -= 4;

	req->ios2_PacketType = ((USHORT)frm[12+6]<<8)|((USHORT)frm[13+6]);

	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		frame_ptr = frm+6;
		datasize = sz;
		req->ios2_Req.io_Flags = SANA2IOF_RAW;
	} else {
		frame_ptr = frm+6+HW_ETH_HDR_SIZE;
		datasize = sz-HW_ETH_HDR_SIZE;
		req->ios2_Req.io_Flags = 0;
	}
	req->ios2_DataLength = datasize;

	// copy frame to device user (probably tcp/ip system)
	struct BufferManagement *bm = (struct BufferManagement *)req->ios2_BufferManagement;
	if (!(*bm->bm_CopyToBuffer)(req->ios2_Data, frame_ptr, datasize)) {
		req->ios2_Req.io_Error = S2ERR_SOFTWARE;
		req->ios2_WireError = S2WERR_BUFF_ERROR;
		DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
		return 0;
	}
  
	req->ios2_Req.io_Error = req->ios2_WireError = 0;

	memcpy(req->ios2_SrcAddr, frm+6+6, HW_ADDRFIELDSIZE);
	memcpy(req->ios2_DstAddr, frm+6, HW_ADDRFIELDSIZE);  

	broadcast = TRUE;
	for (int i=0; i<HW_ADDRFIELDSIZE; i++) {
		if (frm[i+6] != 0xff) {
			broadcast = FALSE;
			break;
		}
	}
	
	if (broadcast) req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
	return 1;
}

// Receive a packet
ULONG receivePacket(DEVBASEP, UBYTE* packet, USHORT packetSize, struct IOSana2Req *req) {	
	ULONG datasize;
	BYTE *frame_ptr;
	BOOL broadcast;
	ULONG res = 0;

	req->ios2_PacketType = ((USHORT)packet[12]<<8)|((USHORT)packet[13]);

	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		frame_ptr = packet;
		datasize = packetSize;
		req->ios2_Req.io_Flags = SANA2IOF_RAW;
	} else {
		frame_ptr = packet+HW_ETH_HDR_SIZE;
		datasize = packetSize-HW_ETH_HDR_SIZE;
		req->ios2_Req.io_Flags = 0;
	}
	req->ios2_DataLength = datasize;
	
	// copy frame to device user (probably tcp/ip system)
	struct BufferManagement *bm = (struct BufferManagement *)req->ios2_BufferManagement;
	if (!(*bm->bm_CopyToBuffer)(req->ios2_Data, frame_ptr, datasize)) {
		req->ios2_Req.io_Error = S2ERR_SOFTWARE;
		req->ios2_WireError = S2WERR_BUFF_ERROR;
		DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
		return 0;
	}
  
	req->ios2_Req.io_Error = req->ios2_WireError = 0;

	memcpy(req->ios2_SrcAddr, packet+6, HW_ADDRFIELDSIZE);
	memcpy(req->ios2_DstAddr, packet, HW_ADDRFIELDSIZE);  

	broadcast = TRUE;
	for (int i=0; i<HW_ADDRFIELDSIZE; i++) {
		if (packet[i] != 0xff) {
			broadcast = FALSE;
			break;
		}
	}
	
	if (broadcast) req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
	return 1;
}


void rejectAllPackets(DEVBASEP) {
  struct IOSana2Req *ior;

  D(("Reject all Packets\n"));

   ObtainSemaphore(&db->db_WriteListSem);
   for (ior = (struct IOSana2Req *)db->db_WriteList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {      
      ior->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ior->ios2_WireError = S2WERR_UNIT_OFFLINE;
      Remove((struct Node*)ior);
      DevTermIO(db, (struct IORequest*)ior);
   }
   ReleaseSemaphore(&db->db_WriteListSem);

   ObtainSemaphore(&db->db_ReadListSem);
   for (ior = (struct IOSana2Req *)db->db_ReadList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
      ior->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ior->ios2_WireError = S2WERR_UNIT_OFFLINE;
      Remove((struct Node*)ior);
      DevTermIO(db, (struct IORequest*)ior);
   }
   ReleaseSemaphore(&db->db_ReadListSem);

   ObtainSemaphore(&db->db_ReadOrphanListSem);
   for (ior = (struct IOSana2Req *)db->db_ReadOrphanList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
      ior->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ior->ios2_WireError = S2WERR_UNIT_OFFLINE;
      Remove((struct Node*)ior);
      DevTermIO(db, (struct IORequest*)ior);
   }
   ReleaseSemaphore(&db->db_ReadOrphanListSem);   

   D(("Reject all Packets done\n"));
}

// This runs as a separate task!
__saveds void frame_proc() {
	D(("scsidayna_task: frame_proc()\n"));

	struct ProcInit* init; 
	{
		struct { void *db_SysBase; } *db = (void*)0x4;
		struct Process* proc;

		proc = (struct Process*)FindTask(NULL);
		WaitPort(&proc->pr_MsgPort);
		init = (struct ProcInit*)GetMsg(&proc->pr_MsgPort);
	}

	struct devbase* db = init->db;
	// This semaphore must be obtained by this process before it replies its init message, and then
	// hold it for its entire lifetime, otherwise the process exit won't be arbitrated properly.
	ObtainSemaphore(&db->db_ProcSem);
  
	// Temporary packet store
	UBYTE* packetData;
	struct IOSana2Req** pendingSends = NULL;
	if (db->db_amigaNetMode) {	
		 packetData = AllocVec(db->db_maxPacketsSize + 2, MEMF_PUBLIC);	
		 pendingSends = (struct IOSana2Req**)AllocVec(db->db_maxPackets * sizeof(struct IOSana2Req*), MEMF_PUBLIC);	
	} else packetData = AllocVec(SCSIWIFI_PACKET_MAX_SIZE + 6, MEMF_PUBLIC);	
	
	struct MsgPort timerPort;
	timerPort.mp_Node.ln_Pri = 0;                       
	timerPort.mp_SigBit      = AllocSignal(-1);
	timerPort.mp_SigTask     = (struct Task *)FindTask(0);
	NewList(&timerPort.mp_MsgList);

	LONG errorDevOpen = 0;
	struct timerequest* time_req = NULL;

	if (((char)timerPort.mp_SigBit)>=0) {
		time_req = (struct timerequest*) CreateIORequest(&timerPort, sizeof (struct timerequest));
		if (time_req) errorDevOpen = OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *)time_req, 0);
	}
	
	struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
	
	SCSIWIFIDevice* scsiDevice = NULL;
	struct SCSIDevice_OpenData openData;
	openData.sysBase = (struct ExecBase*)SysBase;
	openData.utilityBase = (void*)UtilityBase;
	openData.dosBase = (void*)DOSBase;
	openData.deviceDriverName = settings->deviceName;
	openData.deviceID = settings->deviceID;
	openData.scsiMode = settings->scsiMode;
	enum SCSIWifi_OpenResult scsiResult;
	
	D(("scsidayna: Opening SCSI Device\n"));
	logMessagef(db,"PacketServer: Starting Wifi Device"); 
	scsiDevice = SCSIWifi_open(&openData, &scsiResult);

	if ((!packetData) || (errorDevOpen !=0) || (((char)timerPort.mp_SigBit) < 0) || (!time_req) | (!scsiDevice)) {
		init->error = 1;
		DoEvent(db, S2EVENT_OFFLINE);
		db->db_online = 0;
		
		if (!scsiDevice) {
			switch (scsiResult) {
				case sworOpenDeviceFailed: logMessagef(db,"PacketServer: Failed to open SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;  
				case sworOutOfMem:  	   logMessagef(db,"PacketServer: Out of memory opening SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				case sworInquireFail:      logMessagef(db,"PacketServer: Inquiry of SCSI device failed \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				case sworNotDaynaDevice:   logMessagef(db,"PacketServer: Device is not a DaynaPort SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
				default: logMessagef(db,"PacketServer: Unknown error occured opening device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID); break;
			}			
		} else {
			SCSIWifi_close(scsiDevice);
			scsiDevice =0;
		}
		
		if (errorDevOpen != 0) {
			logMessage(db,"PacketServer: Out of memory [3]");
			D(("scsidayna_task: Out of memory [3]\n")); 
		} else {			
			CloseDevice((struct IORequest *)time_req);
			if (!time_req) {
				logMessage(db,"PacketServer: Out of memory [2]");
				D(("scsidayna_task: Out of memory [2]\n")); 			
			} else {
				DeleteIORequest((struct IORequest *)time_req);
			}
		}
		
		if (!packetData) {
			logMessage(db,"PacketServer: Out of memory [1]");
			D(("scsidayna_task: Out of memory [1]\n")); 
		} else FreeVec(packetData);
				
		if (((char)timerPort.mp_SigBit)>=0) FreeSignal(timerPort.mp_SigBit);
		ReplyMsg((struct Message*)init);
		Forbid();
		ReleaseSemaphore(&db->db_ProcSem);
		D(("scsidayna_task: shutdown\n"));
		return;
	}

	// Helpful!
	struct Library *TimerBase = (APTR) time_req->tr_node.io_Device;

	init->error = 0;
	ReplyMsg((struct Message*)init);
	unsigned long timerSignalMask = (1UL << timerPort.mp_SigBit);

	time_req->tr_node.io_Command = TR_ADDREQUEST; time_req->tr_time.tv_secs = 0;

	ULONG recv = 0;
	USHORT currentWifiState = 0;	
	
	// Change task priority
	if (settings->taskPriority != 0) SetTaskPri((struct Task*)db->db_Proc,settings->taskPriority);      

	struct timeval timeLastWifiCheck = {0UL,0UL};
	struct timeval timeWifiCheck = {0UL,0UL};
	USHORT lastWifiStatus = 1;    // assume OK, although this should get overwritten straight away

	D(("scsidayna_task: starting loop\n"));
	while (!(recv & SIGBREAKF_CTRL_C)) {
		struct IOSana2Req *nextwrite;
		USHORT shouldBeEnabled = db->db_online;

		GetSysTime(&timeWifiCheck);
		// Every 5 seconds check WIFI status
		if (abs(timeWifiCheck.tv_secs-timeLastWifiCheck.tv_secs)>=5) {
			D(("scsidayna_task: Check WIFI Status\n"));
			struct SCSIWifi_NetworkEntry wifi;
			if (SCSIWifi_getNetwork(scsiDevice, &wifi)) {
				if (wifi.rssi == 0) {
					logMessage(db,"PacketServer: Wifi not connected");
					D(("scsidayna_task: WIFI not connected\n"));
					lastWifiStatus = 0;
				} else {
					lastWifiStatus = 1;
					logMessagef(db,"PacketServer: Wifi Connected, Signal Strength: %ld dB\n", wifi.rssi);
					D(("scsidayna_task: WIFI connected with strength %ld dB\n", wifi.rssi));
				}
			}
			timeLastWifiCheck.tv_secs = timeWifiCheck.tv_secs;
		}
		if (!lastWifiStatus) shouldBeEnabled = 0;

		// Handle state toggle - also goes offline if theres no connections
		if (currentWifiState != shouldBeEnabled) {
			D(("scsidayna_task: Wifi Status Changed\n"));
			currentWifiState = shouldBeEnabled;
			SCSIWifi_enable(scsiDevice, shouldBeEnabled); 
			if (!shouldBeEnabled) rejectAllPackets(db);
			if (shouldBeEnabled) GetSysTime(&db->db_DevStats.LastStart);
			DoEvent(db, shouldBeEnabled ? S2EVENT_ONLINE : S2EVENT_OFFLINE);
			db->db_currentWifiState = currentWifiState;
		}
    
		if (currentWifiState) {
			UBYTE morePackets = 0;
			USHORT counter = 0;   
			do {
				
				if (db->db_amigaNetMode) {					
					ULONG dataReceived = SCSIWifi_AmigaNetRecvFrames(scsiDevice, packetData,db->db_maxPacketsSize);					
					if (dataReceived<4) {
						morePackets = 0;
						D(("RECV FAILED\n"));
						logMessage(db,"PacketServer: Warning - Batch Recv Failed from Device");
						DoEvent(db, S2EVENT_ERROR | S2EVENT_HARDWARE | S2EVENT_RX);
					} else {
						USHORT numPackets = ((USHORT)packetData[0] << 8) | (USHORT)packetData[1];						
						if (packetData[2]) morePackets=1; else morePackets=0;						
						UBYTE* dataStart = &packetData[4];
						dataReceived -= 4;
												
						// Receive packets
						while (numPackets>0) {
							if (dataReceived<4) {
								logMessage(db,"PacketServer: Buffer underrun [1]");
								break;
							}
							const USHORT packetSize = ((USHORT)dataStart[0]  << 8) | (USHORT)dataStart[1];
							dataStart+= 2;
							dataReceived-=2;
							
							// Check packet has minimum size for Ethernet header
							if (packetSize < 14) {
								logMessage(db,"PacketServer: Warn - Packet too small");
								dataStart += packetSize;
								dataReceived -= packetSize;
								numPackets--;
								continue;
							}
							
							const USHORT packetType = ((USHORT)dataStart[12] << 8)| ((USHORT)dataStart[13]);							
							//logMessagef(db,"PacketServer: Received Packet type %lx received, size=%ld", packetType, packetSize);
							
							if (packetSize > dataReceived) {
								logMessage(db,"PacketServer: Buffer underrun [2]");
								break;
							}
							
							ObtainSemaphore(&db->db_ReadListSem);							
							struct IOSana2Req *ior = NULL;						
							for (ior = (struct IOSana2Req *)db->db_ReadList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
								if (ior->ios2_PacketType == packetType) {
									db->db_DevStats.PacketsReceived++;
									Remove((struct Node*)ior);
									receivePacket(db, dataStart, packetSize, ior);
									DevTermIO(db, (struct IORequest *)ior);
									counter++;
									ior = NULL;
									break;
								}
							}
							ReleaseSemaphore(&db->db_ReadListSem);
							
							// Nothing wanted it?
							if (ior) {			
								db->db_DevStats.UnknownTypesReceived++;
								ObtainSemaphore(&db->db_ReadOrphanListSem);
								ior = (struct IOSana2Req *)RemHead((struct List*)&db->db_ReadOrphanList);
								ReleaseSemaphore(&db->db_ReadOrphanListSem);

								if (!ior) {
									// No orphan buffer - signal problem
									DoEvent(db, S2EVENT_BUFF | S2EVENT_RX);  // Signal BEFORE dropping        
									// Very brief delay (1-2 ticks) to let RoadShow post a buffer
									Delay(1);
									// Try ONE more time
									ObtainSemaphore(&db->db_ReadOrphanListSem);
									ior = (struct IOSana2Req *)RemHead((struct List*)&db->db_ReadOrphanList);
									ReleaseSemaphore(&db->db_ReadOrphanListSem);        
									if (!ior) {
										// Still no buffer - drop it
										logMessagef(db,"PacketServer: Warn - Orphaned packet not picked up of type %lx", packetType);
										db->db_DevStats.Overruns++;
										DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE | S2EVENT_RX);
									} else {
										//logMessagef(db, "PacketServer: Buffer arrived after signal - packet saved");
										receivePacket(db, dataStart, packetSize, ior);
										DevTermIO(db, (struct IORequest *)ior);
									}
								} else {
									receivePacket(db, dataStart, packetSize, ior);
									DevTermIO(db, (struct IORequest *)ior);  									
									//logMessagef(db,"PacketServer: Warn - Orphaned packet picked up of type %lx", packetType);
								} 								
							}
							
							dataReceived -= packetSize;
							
							numPackets--;
							dataStart += packetSize;
						}						
					}										
				} else {
					USHORT packetSize = SCSIWifi_receiveFrame(scsiDevice, packetData, SCSIWIFI_PACKET_MAX_SIZE + 6);
					if (packetSize) {    
						morePackets = packetData[5];						

						if (packetSize > 6) {
							USHORT packet_type = ((USHORT)packetData[18]<<8)|((USHORT)packetData[19]);   

							ObtainSemaphore(&db->db_ReadListSem);
							struct IOSana2Req *ior = NULL;						
							for (ior = (struct IOSana2Req *)db->db_ReadList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
								if (ior->ios2_PacketType == packet_type) {
									db->db_DevStats.PacketsReceived++;
									Remove((struct Node*)ior);
									read_frame(db, ior, packetData, packetSize);        
									DevTermIO(db, (struct IORequest *)ior);
									counter++;
									ior = NULL;
									break;
								}
							}
							ReleaseSemaphore(&db->db_ReadListSem);
							
							// Nothing wanted it?
							if (ior) {
								db->db_DevStats.UnknownTypesReceived++;
								ObtainSemaphore(&db->db_ReadOrphanListSem);
								ior = (struct IOSana2Req *)RemHead((struct List*)&db->db_ReadOrphanList);
								ReleaseSemaphore(&db->db_ReadOrphanListSem);
								if (ior) {
									read_frame(db, ior, packetData, packetSize);
									DevTermIO(db, (struct IORequest *)ior);  
									D(("Orphan Packet Picked Up (proto %lx) !\n", packet_type));
								} 
							}
						}
					} else {
						morePackets = 0;
						D(("RECV FAILED\n"));
						logMessage(db,"PacketServer: Warning - Recv Failed from Device");
						DoEvent(db, S2EVENT_ERROR | S2EVENT_HARDWARE | S2EVENT_RX);
					}
				}

				recv = SetSignal(0, SIGBREAKF_CTRL_C|SIGBREAKF_CTRL_F);
				// Keep going until we're told theres no more data, or we need to send, or terminate
			} while ((morePackets) && (!recv));

			// Prevent delaying if there was data incoming
			if (counter >= 2) morePackets = 1;
						
			if (db->db_amigaNetMode) {
				// Batch packet sending
				counter = 0;
				UBYTE* dataOut = &packetData[2];  // 2 bytes header at the front
				USHORT spaceRemaining = db->db_maxPacketsSize - 2;
				struct IOSana2Req** pendingSendsSave = pendingSends;
				struct IOSana2Req *nextwrite;
				// Collect packets until not enough data space or too many
				ObtainSemaphore(&db->db_WriteListSem);
				struct IOSana2Req *ior = (struct IOSana2Req *)db->db_WriteList.lh_Head;
			    while ((nextwrite = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL) {
					USHORT sz = ior->ios2_DataLength;					
					UBYTE* rewind = dataOut;
					const USHORT rewindSize = spaceRemaining;

					// Calculate packet size
				    if (ior->ios2_Req.io_Flags & SANA2IOF_RAW) {
						if (sz + 2 > spaceRemaining) break;
						dataOut[0] = sz >> 8;
						dataOut[1] = sz & 0xFF;
						dataOut+=2;
						spaceRemaining -= 2;
						
				    } else {
						USHORT fullSize = sz + HW_ETH_HDR_SIZE;
						if (fullSize + 2 > spaceRemaining) break;
						dataOut[0] = fullSize >> 8;
						dataOut[1] = fullSize & 0xFF;
						dataOut+=2;
						spaceRemaining -= 2;

						*((USHORT*)(dataOut+12)) = (USHORT)ior->ios2_PacketType;
						// Add ethernet header
						memcpy(dataOut, ior->ios2_DstAddr, HW_ADDRFIELDSIZE);
						memcpy(dataOut+6, HW_MAC, HW_ADDRFIELDSIZE);
						dataOut += HW_ETH_HDR_SIZE;
						spaceRemaining -= HW_ETH_HDR_SIZE;
				    }
				    // Add the data
				    struct BufferManagement *bm = (struct BufferManagement *)ior->ios2_BufferManagement;				   
					if (!(*bm->bm_CopyFromBuffer)(dataOut, ior->ios2_Data, sz)) {
						ior->ios2_Req.io_Error = S2ERR_SOFTWARE;
						ior->ios2_WireError = S2WERR_BUFF_ERROR;
						DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
						D(("bm_CopyFromBuffer FAIL"));		
						dataOut = rewind;		
						spaceRemaining = rewindSize;					
						Remove((struct Node*)ior);
						DevTermIO(db, (struct IORequest *)ior);
					} else {						
						if (pendingSendsSave) {
							*pendingSendsSave = ior; 
							pendingSendsSave++;
						} else {
							ior->ios2_Req.io_Error = ior->ios2_WireError = 0;
							db->db_DevStats.PacketsSent++;
							DevTermIO(db, (struct IORequest *)ior);
						}						
						Remove((struct Node*)ior);
						dataOut += sz;
						spaceRemaining -= sz;
						counter++;
					}
					if (counter>=db->db_maxPackets) break;   // limit packet total
					ior = nextwrite;
				}
				ReleaseSemaphore(&db->db_WriteListSem);
				// Now actually transmit them
				if (counter) {
					const USHORT totalSize = dataOut-packetData;
					packetData[0] = counter >> 8;
					packetData[1] = counter & 0xFF;
					if (!SCSIWifi_AmigaNetSendFrames(scsiDevice, packetData, totalSize)) {
						D(("SEND FAIL"));
						logMessage(db,"PacketServer: Warning - Send Failed to Device");
						if (pendingSends) {							
							for (struct IOSana2Req **req = pendingSends; req<pendingSendsSave; req++) {							
								(*req)->ios2_Req.io_Error = S2ERR_TX_FAILURE; (*req)->ios2_WireError = S2WERR_GENERIC_ERROR;
								DevTermIO(db, (struct IORequest *)(*req));
								DoEvent(db, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);
							}
						}
					} else {
						//logMessagef(db,"PacketServer: Sent %ld Packets (Total Size=%ld)", counter, totalSize);
						if (pendingSends) {
							for (struct IOSana2Req **req = pendingSends; req<pendingSendsSave; req++) {												
								(*req)->ios2_Req.io_Error = (*req)->ios2_WireError = 0;
								DevTermIO(db, (struct IORequest *)(*req));								
							}
						}
						db->db_DevStats.PacketsSent+=counter;
					}
				}
			} else {
				// Send packets
				ObtainSemaphore(&db->db_WriteListSem);
				counter = 8;   // Max of 8 per loop      
				for(struct IOSana2Req *ior = (struct IOSana2Req *)db->db_WriteList.lh_Head; (nextwrite = (struct IOSana2Req *) ior->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL; ior = nextwrite ) {
					ULONG res = write_frame(ior, packetData, scsiDevice, db);
					Remove((struct Node*)ior);
					DevTermIO(db, (struct IORequest *)ior);
					morePackets=1;
					counter--;
					if (!counter) break;
				}
				ReleaseSemaphore(&db->db_WriteListSem);
			}
			
			if (recv & SIGBREAKF_CTRL_C) {
				D(("Terminate Requested"));
			} else {
				if (!morePackets) {
					// we use unit VBLANK therefore the granularity of our wait will be 1/50th (1/60th)
					// of a second. So essentially this will wait until the next vblank, unless
					// signaled, which is good enough to yield.
					time_req->tr_time.tv_micro = 1L;
					SendIO((struct IORequest *)time_req);
					recv = Wait(SIGBREAKF_CTRL_C | timerSignalMask | SIGBREAKF_CTRL_F);
				}
			}
			
		} else {
			// Not enabled? Pause for a decent amount of time
			time_req->tr_time.tv_micro = 250 * 1000L;
			SendIO((struct IORequest *)time_req);
			recv = Wait(SIGBREAKF_CTRL_C | timerSignalMask | SIGBREAKF_CTRL_F);
			AbortIO((struct IORequest *)time_req);
		}
	}
	
	D(("scsidayna_task: exiting loop\n"));
	logMessage(db,"PacketServer: Shutting down [1]");
	
	// Make sure it's finished - this prevents an intermittent crash at shutdown!
	if (!CheckIO((struct IORequest *)time_req)) { // IO is pending
		AbortIO((struct IORequest *)time_req);
		WaitIO((struct IORequest *)time_req);  // wait until IO fully done
	}
	
	D(("scsidayna_task: i/o shutdown\n"));
	logMessage(db,"PacketServer: Shutting down [2]");

	SCSIWifi_enable(scsiDevice, 0); 
	DoEvent(db, S2EVENT_OFFLINE);
	rejectAllPackets(db);
	FreeVec(packetData);
	if (pendingSends) FreeVec(pendingSends);
	
	SCSIWifi_close(scsiDevice);
	
	logMessage(db,"PacketServer: Shutting down [3]");
	
	CloseDevice((struct IORequest *)time_req);
	DeleteIORequest((struct IORequest *)time_req);
	FreeSignal(timerPort.mp_SigBit);
	
	logMessage(db,"PacketServer: Closed");
	
	Forbid();
	ReleaseSemaphore(&db->db_ProcSem);
}
