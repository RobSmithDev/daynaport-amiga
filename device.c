/*
 * SCSI DaynaPORT Device (scsidayna.device) by RobSmithDev 
 *
 * based LARGELY on the MNT ZZ9000 Network Driver
 * Copyright (C) 2016-2023, Lukas F. Hartmann <lukas@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * Based on code copyright (C) 2018 Henryk Richter <henryk.richter@gmx.net>
 * Released under GPLv3+ with permission.
 *
 * More Info: https://mntre.com/zz9000
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


#ifdef HAVE_VERSION_H
#include "version.h"
#endif
/* NSD support is optional */
#ifdef NEWSTYLE
#include <devices/newstyle.h>
#endif /* NEWSTYLE */
#ifdef DEVICES_NEWSTYLE_H

const UWORD dev_supportedcmds[] = {
	NSCMD_DEVICEQUERY,
	CMD_READ,
	CMD_WRITE,
	/* ... add all cmds here that are supported by BeginIO */
	0
};

const struct NSDeviceQueryResult NSDQueryAnswer = {
	0,
	16, /* up to SupportedCommands (inclusive) TODO: correct number */
	NSDEVTYPE_SANA2, /* TODO: proper device type */
	0,  /* subtype */
	(UWORD*)dev_supportedcmds
};
#endif /* DEVICES_NEWSTYLE_H */

#include "device.h"
#include "macros.h"


__saveds void frame_proc();
char *frame_proc_name = "SCSIDaynaPacketTask";

static UBYTE HW_MAC[] = {0x00,0x00,0x00,0x00,0x00,0x00};

struct ProcInit
{
   struct Message msg;
   struct devbase *db;
   BOOL  error;
   UBYTE pad[2];
};

// Free's anything left after init
void freeInit(DEVBASEP) {
  if (db->db_scsiSettings) FreeVec(db->db_scsiSettings); 
  db->db_scsiSettings = NULL;
  if (DOSBase) CloseLibrary(DOSBase); 
  DOSBase = NULL;
  if (UtilityBase) CloseLibrary(UtilityBase); 
  UtilityBase = NULL;
}


void DevTermIO( DEVBASEP, struct IORequest *ioreq );

__saveds struct Device *DevInit( ASMR(d0) DEVBASEP                  ASMREG(d0),
                                 ASMR(a0) BPTR seglist              ASMREG(a0),
				                         ASMR(a6) struct Library *_SysBase  ASMREG(a6) ) {	
	db->db_SysBase = _SysBase;
	db->db_SegList = seglist;
  db->db_DOSBase = NULL;
  db->db_UtilityBase = NULL;
  db->db_scsiSettings = NULL;
  db->db_online = 0;

  volatile struct List db_EventList;
	struct SignalSemaphore db_EventListSem;     
  
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
    D(("scsidayna: settings loaded")); else D(("scsidayna: Invalid or missing settings file, reverting to defaults\n"));

  if (strlen(settings->deviceName)<1) {
    D(("scsidayna: SCSI device name not set\n"));
    freeInit(db);
    return 0;
  }

  struct SCSIDevice_OpenData openData;
  openData.sysBase = (struct ExecBase*)SysBase;
  openData.utilityBase = (void*)UtilityBase;
  openData.dosBase = (void*)DOSBase;
  openData.deviceDriverName = settings->deviceName;
  openData.deviceID = settings->deviceID;
  openData.scsiMode = settings->scsiMode;
  db->db_scsiMode = settings->scsiMode;

  
  enum SCSIWifi_OpenResult scsiResult;
  SCSIWIFIDevice* wifiDevice;
  
  if ((settings->deviceID<0) || (settings->deviceID>7)) {
    D(("scsidayna: Searching for DaynaPORT Device to Configure\n"));
    // Highly likely it will be on 4 as its in the example so start there!
    for (USHORT deviceID=4; deviceID<4+8; deviceID++) {
      openData.deviceID = deviceID & 7;  
      D(("scsidayna: Searching on DeviceID %ld\n", openData.deviceID));
      wifiDevice = SCSIWifi_open(&openData, &scsiResult);
      if (wifiDevice) break;
    }
  } else {
    D(("scsidayna: Opening Device to Configure\n"));
    openData.deviceID = settings->deviceID;  
    wifiDevice = SCSIWifi_open(&openData, &scsiResult);
  }
  if (!wifiDevice) {
    switch (scsiResult) {
      case sworOpenDeviceFailed:
        D(("scsidayna: Failed to open SCSI device \"%s\" ID %ld\n", settings->deviceName, settings->deviceID));
        break;  
      case sworOutOfMem:  D(("scsidayna: Out of memory opening SCSI device\n"));    break;
      case sworInquireFail:  D(("scsidayna: Inquiry of SCSI device failed\n"));    break;
      case sworNotDaynaDevice:  D(("scsidayna: Device is not a DaynaPort SCSI device\n"));    break;
    }
    freeInit(db);

    return 0;
  }
  db->db_scsiDeviceID = openData.deviceID;

  D(("scsidayna: successfully opened. Fetching MAC\n"));

  // Device open. Fetch MAC address
  struct SCSIWifi_MACAddress macAddress;
  if (!SCSIWifi_getMACAddress(wifiDevice, &macAddress)) {
    D(("scsidayna: Failed to fetch hardware MAC address\n"));
    SCSIWifi_close(wifiDevice);
    freeInit(db);
    return 0;
  }

  memcpy(HW_MAC, macAddress.address, 6);

  D(("scsidayna: MAC Address stored, checking WIFI status\n"));

  // Should we be attempting to connect to wifi?
  if ((settings->autoConnect) && (strlen(settings->ssid))) {
    // Fetch what the current network is
    struct SCSIWifi_NetworkEntry currentNetwork;
    if (!SCSIWifi_getNetwork(wifiDevice, &currentNetwork)) memset(&currentNetwork, 0, sizeof(struct SCSIWifi_NetworkEntry));
    if (strcmp(currentNetwork.ssid, settings->ssid) == 0) {
      D(("scsidayna: Already connected to requested WIFI network\n"));
    } else {
      struct SCSIWifi_JoinRequest request;
      strcpy(request.ssid, settings->ssid);
      strcpy(request.key, settings->key);
      SCSIWifi_joinNetwork(wifiDevice, &request);
      D(("scsidayna: Attempting to connect to WIFI network\n"));     
    }
  }

  D(("scsidayna: Closing Device\n"));
  SCSIWifi_close(wifiDevice);

  D(("scsidayna: Closed\n"));
	return (struct Device*)db;
}

__saveds LONG DevOpen( ASMR(a1) struct IOSana2Req *ioreq           ASMREG(a1),
                         ASMR(d0) ULONG unit                         ASMREG(d0),
                         ASMR(d1) ULONG flags                        ASMREG(d1),
                         ASMR(a6) DEVBASEP                           ASMREG(a6) )
{
	LONG ok = 0,ret = IOERR_OPENFAIL;
  struct BufferManagement *bm;

  // promiscuous mode not supported
  if ((flags & SANA2OPF_PROM) && (unit)) {
      ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
      ioreq->ios2_Req.io_Unit = (struct Unit *) 0;
      ioreq->ios2_Req.io_Device = (struct Device *) 0;
      D(("scsidayna: SANA2OPF_PROM not supported %ld\n",unit));
      return IOERR_OPENFAIL;
  }

	D(("scsidayna: DevOpen for %ld\n",unit));

	db->db_Lib.lib_OpenCnt++; /* avoid Expunge, see below for separate "unit" open count */

  if (unit==0 && db->db_Lib.lib_OpenCnt==1) {
    if ((bm = (struct BufferManagement*)AllocVec(sizeof(struct BufferManagement), MEMF_CLEAR|MEMF_PUBLIC))) {
      bm->bm_CopyToBuffer = (BMFunc)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
      bm->bm_CopyFromBuffer = (BMFunc)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement); 

      ioreq->ios2_BufferManagement = (VOID *)bm;
      ioreq->ios2_Req.io_Error = 0;
      ioreq->ios2_Req.io_Unit = (struct Unit *)unit; // not a real pointer, but id integer
      ioreq->ios2_Req.io_Device = (struct Device *)db;

      NewList(&db->db_ReadList);
      InitSemaphore(&db->db_ReadListSem);

      NewList(&db->db_WriteList);
      InitSemaphore(&db->db_WriteListSem);

      NewList(&db->db_EventList);
      InitSemaphore(&db->db_EventListSem);

      NewList(&db->db_ReadOrphanList);
      InitSemaphore(&db->db_ReadOrphanListSem);

      db->db_online = 1;
    
      struct ProcInit init;
      struct MsgPort *port;

      if (port = CreateMsgPort()) {
        D(("scsidayna: Starting Server\n"));
        if (db->db_Proc = CreateNewProcTags(NP_Entry, frame_proc, NP_Name,
                                            frame_proc_name, NP_Priority, 0, TAG_DONE)) {
          InitSemaphore(&db->db_ProcExitSem);

          init.error = 1;
          init.db = db;
          init.msg.mn_Length = sizeof(init);
          init.msg.mn_ReplyPort = port;

          D(("scsidayna: handover db: %lx\n",init.db));

          PutMsg(&db->db_Proc->pr_MsgPort, (struct Message*)&init);
          WaitPort(port);   

          if (init.error) {
            D(("scsidayna:process startup error\n"));
            ret = IOERR_OPENFAIL;
            ok = 0;
          } else {
            ret = 0;
            ok = 1;                        
          }
        } else {
          D(("scsidayna:couldn't create process\n"));
        }
        DeleteMsgPort(port);
      }
    }
  }

	if (ok) {
    D(("scsidayna: OK\n"));
		ret = 0;
    db->db_Lib.lib_Flags &= ~LIBF_DELEXP;
	}

	if (ret == IOERR_OPENFAIL) {
		ioreq->ios2_Req.io_Unit   = (0);
		ioreq->ios2_Req.io_Device = (0);
		ioreq->ios2_Req.io_Error  = ret;
		db->db_Lib.lib_OpenCnt--;
    D(("scsidayna: Err\n"));
	}
	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;

	D(("scsidayna: DevOpen return code %ld\n",ret));

	return ret;
}

__saveds BPTR DevClose(   ASMR(a1) struct IORequest *ioreq        ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	/* ULONG unit; */
	BPTR  ret = (0);

	D(("scsidayna: DevClose open count %ld\n",db->db_Lib.lib_OpenCnt));

	if (!ioreq)
		return ret;

	db->db_Lib.lib_OpenCnt--;

  if (db->db_Lib.lib_OpenCnt == 0) {

    if (db->db_Proc) {
      D(("scsidayna: End Proc...\n"));
      Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_C);
      db->db_Proc = 0;

      ObtainSemaphore(&db->db_ProcExitSem);
      ReleaseSemaphore(&db->db_ProcExitSem);
    }   
  }

	ioreq->io_Device = (0);
	ioreq->io_Unit   = (struct Unit *)(-1);

	if (db->db_Lib.lib_Flags & LIBF_DELEXP)
		ret = DevExpunge(db);

	return ret;
}

__saveds BPTR DevExpunge( ASMR(a6) DEVBASEP                        ASMREG(a6) )
{
	BPTR seglist = db->db_SegList;

	if( db->db_Lib.lib_OpenCnt )
	{
		db->db_Lib.lib_Flags |= LIBF_DELEXP;
		return (0);
	}

  D(("scsidayna: Remove Device Node...\n"));
  Remove((struct Node*)db);

	freeInit(db);
	FreeMem( ((BYTE*)db)-db->db_Lib.lib_NegSize,(ULONG)(db->db_Lib.lib_PosSize + db->db_Lib.lib_NegSize));

	return seglist;
}

__saveds VOID DevBeginIO( ASMR(a1) struct IOSana2Req *ioreq       ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	ULONG unit = (ULONG)ioreq->ios2_Req.io_Unit;
  int mtu;

	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
  ioreq->ios2_Req.io_Error = S2ERR_NO_ERROR;
  ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;

	//D(("BeginIO command %ld unit %ld\n",(LONG)ioreq->ios2_Req.io_Command,unit));

	switch( ioreq->ios2_Req.io_Command ) {
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
      AddHead((struct List*)&db->db_ReadList, (struct Node*)ioreq);
      ReleaseSemaphore(&db->db_ReadListSem);
      ioreq = NULL;
    }
    break;

  case S2_GETGLOBALSTATS:
      memcpy(ioreq->ios2_StatData, &db->db_DevStats, sizeof(struct Sana2DeviceStats));
      break;

  case S2_BROADCAST:
    /* set broadcast addr: ff:ff:ff:ff:ff:ff */
    if (ioreq->ios2_DstAddr) {
      memset(ioreq->ios2_DstAddr, 0xff, HW_ADDRFIELDSIZE);
    } else {
      D(("bcast: invalid dst addr\n"));
    }
    /* fall through */
  case CMD_WRITE: 
    if (ioreq->ios2_BufferManagement == NULL) {
      ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
      ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
    } 
   else if (!db->db_currentWifiState) {
     ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
     ioreq->ios2_WireError = S2WERR_UNIT_OFFLINE;
   }
    else {
      ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
      ioreq->ios2_Req.io_Error = 0;
      ObtainSemaphore(&db->db_WriteListSem);
      AddHead((struct List*)&db->db_WriteList, (struct Node*)ioreq);
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
      if ((ioreq->ios2_WireError & (S2EVENT_ONLINE|S2EVENT_OFFLINE|S2EVENT_ERROR|S2EVENT_TX|S2EVENT_RX|S2EVENT_BUFF|S2EVENT_HARDWARE|S2EVENT_SOFTWARE)) != ioreq->ios2_WireError)
      {
        /* we cannot handle such events */
        ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
        ioreq->ios2_WireError = S2WERR_BAD_EVENT;
      }
      else
      {
        /* Queue anything else */
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
      } else
      {                      
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
  case S2_DEVICEQUERY:
    {
      struct Sana2DeviceQuery *devquery;

      devquery = ioreq->ios2_StatData;
      devquery->DevQueryFormat = 0;        /* "this is format 0" */
      devquery->DeviceLevel = 0;           /* "this spec defines level 0" */

      if (devquery->SizeAvailable >= 18) devquery->AddrFieldSize = HW_ADDRFIELDSIZE * 8; /* Bits! */
      if (devquery->SizeAvailable >= 22) devquery->MTU           = SCSIWIFI_PACKET_MTU_SIZE;
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
  // Todo: Add S2_ADDMULTICASTADDRESS
  default:
    {
      ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
      ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;
      break;
    }
	}

	if (ioreq) {
		DevTermIO(db, (struct IORequest*)ioreq);
  }
}

   /*
   ** SANA-2 Event management
   */
void DoEvent(DEVBASEP, long event)
{
   struct IOSana2Req *ior, *ior2;

   D(("event is %lx\n",event));

   ObtainSemaphore(&db->db_EventListSem );
   
   for(ior = (struct IOSana2Req *) db->db_EventList.lh_Head; (ior2 = (struct IOSana2Req *) ior->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL; ior = ior2 )
   {
      if (ior->ios2_WireError & event)
      {
         Remove((struct Node*)ior);
         DevTermIO(db, (struct IORequest *)ior);
      }
   }
   
   ReleaseSemaphore(&db->db_EventListSem );
}

__saveds LONG DevAbortIO( ASMR(a1) struct IORequest *ioreq        ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	LONG   ret = 0;
  struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;

	D(("scsidayna: AbortIO on %lx\n",(ULONG)ioreq));

  Remove((struct Node*)ioreq);

	ioreq->io_Error = IOERR_ABORTED;
  ios2->ios2_WireError = 0;

	ReplyMsg((struct Message*)ioreq);
	return ret;
}

void DevTermIO( DEVBASEP, struct IORequest *ioreq )
{
  struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;

  if (!(ios2->ios2_Req.io_Flags & SANA2IOF_QUICK)) {
    ReplyMsg((struct Message *)ioreq);
  } else {
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
  }
}


ULONG write_frame(struct IOSana2Req *req, UBYTE* frame, SCSIWIFIDevice scsiDevice, DEVBASEP)
{
   ULONG rc=0;
   struct BufferManagement *bm;
   USHORT sz=0;
   UBYTE* inputFrame = frame;

   if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
      sz = req->ios2_DataLength;
   } else {
      sz = req->ios2_DataLength + HW_ETH_HDR_SIZE;
      *((USHORT*)(frame+6+6)) = (USHORT)req->ios2_PacketType;
      memcpy(frame, req->ios2_DstAddr, HW_ADDRFIELDSIZE);
      memcpy(frame+6, HW_MAC, HW_ADDRFIELDSIZE);
      frame+=HW_ETH_HDR_SIZE;
   }

   if (sz>0) {
     bm = (struct BufferManagement *)req->ios2_BufferManagement;
    
     if (!(*bm->bm_CopyFromBuffer)(frame, req->ios2_Data, req->ios2_DataLength)) {
       rc = 0; 
       req->ios2_Req.io_Error = S2ERR_SOFTWARE;
       req->ios2_WireError = S2WERR_BUFF_ERROR;
       DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
       D(("bm_CopyFromBuffer FAIL"));
     }
     else {
       // buffer was  
       if (SCSIWifi_sendFrame(scsiDevice, inputFrame, sz)) {
         rc = 1;
         //if (req->ios2_Req.io_Flags & SANA2IOF_RAW) D(("FRAME RAW SENT %ld bytes", sz)); else D(("FRAME SENT %ld bytes", sz));
         req->ios2_Req.io_Error = req->ios2_WireError = 0;
         db->db_DevStats.PacketsSent++;
       } else {
         rc = 0;  
         req->ios2_Req.io_Error = S2ERR_TX_FAILURE;
         req->ios2_WireError = S2WERR_GENERIC_ERROR;
         DoEvent(db, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);
         D(("SEND FAIL"));
       }
     }
   } else {
    D(("no size"));
   }

   return rc;
}

ULONG read_frame(DEVBASEP, struct IOSana2Req *req, UBYTE *frm, USHORT packetSize)
{
  ULONG datasize;
  BYTE *frame_ptr;
  BOOL broadcast;
  ULONG res = 0;
  struct BufferManagement *bm;

  // This length includes 4 bytes for the CRC at the end, but we dont need that
  ULONG sz   = ((ULONG)frm[0]<<8)|((ULONG)frm[1]);
  if (sz<4) return 1;
  sz -= 4;

  req->ios2_PacketType = ((USHORT)frm[12+6]<<8)|((USHORT)frm[13+6]);
  
  if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
    frame_ptr = frm+6;
    datasize = sz;
    req->ios2_Req.io_Flags = SANA2IOF_RAW;
  }
  else {
    frame_ptr = frm+6+HW_ETH_HDR_SIZE;
    datasize = sz-HW_ETH_HDR_SIZE;
    req->ios2_Req.io_Flags = 0;
  }

  req->ios2_DataLength = datasize;

  // copy frame to device user (probably tcp/ip system)
  bm = (struct BufferManagement *)req->ios2_BufferManagement;
  if (!(*bm->bm_CopyToBuffer)(req->ios2_Data, frame_ptr, datasize)) {
    req->ios2_Req.io_Error = S2ERR_SOFTWARE;
    req->ios2_WireError = S2WERR_BUFF_ERROR;
    DoEvent(db, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
    res = 0;
  }
  else {
    req->ios2_Req.io_Error = req->ios2_WireError = 0;
    res = 1;
  }

  memcpy(req->ios2_SrcAddr, frm+6+6, HW_ADDRFIELDSIZE);
  memcpy(req->ios2_DstAddr, frm+6, HW_ADDRFIELDSIZE);
  

  broadcast = TRUE;
  for (int i=0; i<HW_ADDRFIELDSIZE; i++) {
    if (frm[i+6] != 0xff) {
      broadcast = FALSE;
      break;
    }
  }
  if (broadcast) {
    req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
  }
  return res;
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

// This runs as a seperate task!
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
  // Need to open a seperate connection to the SCSI device, This has the advantage that it can communicate at the same time!

  struct ScsiDaynaSettings* settings = (struct ScsiDaynaSettings*)db->db_scsiSettings;
  struct SCSIDevice_OpenData openData;
  openData.sysBase = (struct ExecBase*)SysBase;
  openData.utilityBase = (void*)UtilityBase;
  openData.dosBase = (void*)DOSBase;
  openData.deviceDriverName = settings->deviceName;
  openData.deviceID = db->db_scsiDeviceID;
  openData.scsiMode = db->db_scsiMode;

  D(("Opening with scsimode %ld", openData.scsiMode));

  enum SCSIWifi_OpenResult scsiResult;
  SCSIWIFIDevice scsiDevice = SCSIWifi_open(&openData, &scsiResult);

  char* packetData = AllocVec(SCSIWIFI_PACKET_MAX_SIZE + 6, MEMF_PUBLIC);
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

  if ((!scsiDevice) || (!packetData) || (errorDevOpen !=0) || (((char)timerPort.mp_SigBit) < 0) || (!time_req) ) {
    init->error = 1;
    if (errorDevOpen != 0) D(("scsidayna_task: Out of memory [3]\n")); else CloseDevice((struct IORequest *)time_req);
    if (!packetData) D(("scsidayna_task: Out of memory [1]\n")); else FreeVec(packetData);
    if (!time_req) D(("scsidayna_task: Out of memory [2]\n")); else DeleteIORequest((struct IORequest *)time_req);

    switch (scsiResult) {
      case sworOpenDeviceFailed: D(("scsidayna_task: Failed to open SCSI device \"%s\" ID %d\n", settings->deviceName, settings->deviceID)); break;  
      case sworOutOfMem:  D(("scsidayna_task: Out of memory opening SCSI device\n"));    break;
      case sworInquireFail:  D(("scsidayna_task: Inquiry of SCSI device failed\n"));    break;
      case sworNotDaynaDevice:  D(("scsidayna_task: Device is not a DaynaPort SCSI device\n"));    break;
    }

    if (((char)timerPort.mp_SigBit)>=0) FreeSignal(timerPort.mp_SigBit);
    ObtainSemaphore(&db->db_ProcExitSem);
    ReplyMsg((struct Message*)init);
    Forbid();
    ReleaseSemaphore(&db->db_ProcExitSem);
    D(("scsidayna_task: shutdown\n"));
    return;
  }

  // Helpful!
  struct Library *TimerBase = (APTR) time_req->tr_node.io_Device;

  init->error = 0;
  ObtainSemaphore(&db->db_ProcExitSem);  
  ReplyMsg((struct Message*)init);

  unsigned long timerSignalMask = (1UL << timerPort.mp_SigBit);

  time_req->tr_node.io_Command = TR_ADDREQUEST;
  time_req->tr_time.tv_secs = 0;

  ULONG recv = 0;
  USHORT currentWifiState = 0;

 if (settings->taskPriority != 0)
   SetTaskPri((struct Task*)db->db_Proc,settings->taskPriority);      

  struct timeval timeLastWifiCheck = {0UL,0UL};
  struct timeval timeWifiCheck = {0UL,0UL};
  USHORT lastWifiStatus = 1;    // assume OK, although this should get overwritten straight away

  D(("scsidayna_task: starting loop 1.0\n"));
  while (!(recv & SIGBREAKF_CTRL_C)) {
    struct IOSana2Req *ior = NULL, *nextwrite;
    USHORT shouldBeEnabled = db->db_online;

    GetSysTime(&timeWifiCheck);
    // Every 5 seconds check WIFI status
    if (abs(timeWifiCheck.tv_secs-timeLastWifiCheck.tv_secs)>=5) {
      struct SCSIWifi_NetworkEntry wifi;
      if (SCSIWifi_getNetwork(scsiDevice, &wifi)) {
        if (wifi.rssi == 0) {
          D(("scsidayna_task: WIFI not connected\n"));
          lastWifiStatus = 0;
        } else {
          lastWifiStatus = 1;
          D(("scsidayna_task: WIFI connected with strength %ld dB\n", wifi.rssi));
        }
      }
      timeLastWifiCheck.tv_secs = timeWifiCheck.tv_secs;
    }
    if (!lastWifiStatus) shouldBeEnabled = 0;

    // Handle state toggle - also goes offline if theres no connections
    if (currentWifiState != shouldBeEnabled) {
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
        USHORT packetSize = SCSIWIFI_PACKET_MAX_SIZE + 6;
        if (SCSIWifi_receiveFrame(scsiDevice, packetData, &packetSize)) {    
          morePackets = packetData[5];
          db->db_DevStats.PacketsReceived++;

          if (packetSize > 6) {
            USHORT packet_type = ((USHORT)packetData[18]<<8)|((USHORT)packetData[19]);   

            ObtainSemaphore(&db->db_ReadListSem);
            for (ior = (struct IOSana2Req *)db->db_ReadList.lh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ; ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
              if (ior->ios2_PacketType == packet_type) {
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
          DoEvent(db, S2EVENT_ERROR | S2EVENT_HARDWARE | S2EVENT_RX);
        }
        recv = SetSignal(0, SIGBREAKF_CTRL_C|SIGBREAKF_CTRL_F);
        // Keep going until we're told theres no more data, or we need to send, or terminate
      } while ((morePackets) && (!recv));

      // Prevent delaying if there was data incoming
      if (counter >=2 ) morePackets = 1;

      // Send packets
      ObtainSemaphore(&db->db_WriteListSem);
      counter = 8;   // Max of 8 per loop      
      for(ior = (struct IOSana2Req *)db->db_WriteList.lh_Head; (nextwrite = (struct IOSana2Req *) ior->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL; ior = nextwrite ) {
          ULONG res = write_frame(ior, packetData, scsiDevice, db);
          Remove((struct Node*)ior);
          DevTermIO(db, (struct IORequest *)ior);
          morePackets=1;
          counter--;
          if (!counter) break;
      }
      ReleaseSemaphore(&db->db_WriteListSem);

      if (recv & SIGBREAKF_CTRL_C) {
        D(("Terminate Requested"));
      } else {
        if (morePackets)
          time_req->tr_time.tv_micro = 1000L; // Still a yield, but less. If we take up too much SCSI time the file access slows down
        else time_req->tr_time.tv_micro = 10000L;
        SendIO((struct IORequest *)time_req);
        recv = Wait(SIGBREAKF_CTRL_C | timerSignalMask | SIGBREAKF_CTRL_F);        
      }
    } else {
        // Not enabled? Pause for a decent amount of time
        time_req->tr_time.tv_micro = 250 * 1000L;
        SendIO((struct IORequest *)time_req);
        recv = Wait(SIGBREAKF_CTRL_C | timerSignalMask | SIGBREAKF_CTRL_F);
        AbortIO((struct IORequest *)time_req);
    }
  }

  SCSIWifi_enable(scsiDevice, 0); 
  DoEvent(db, S2EVENT_OFFLINE);
  rejectAllPackets(db);
  FreeVec(packetData);
  CloseDevice((struct IORequest *)time_req);
  DeleteIORequest((struct IORequest *)time_req);
  FreeSignal(timerPort.mp_SigBit);
  SCSIWifi_close(scsiDevice);

  Forbid();
  ReleaseSemaphore(&db->db_ProcExitSem);
}
