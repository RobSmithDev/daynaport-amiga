/*
 * SCSI DaynaPORT Device (scsidayna.device) by RobSmithDev 
 *
 * based LARGELY on the MNT ZZ9000 Network Driver
 * Copyright (C) 2016-2019, Lukas F. Hartmann <lukas@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 * Copyright (C) 2018 Henryk Richter <henryk.richter@gmx.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

/*
  device.h

  (C) 2018 Henryk Richter <henryk.richter@gmx.net>

  Device Functions and Definitions


*/
#ifndef _INC_DEVICE_H
#define _INC_DEVICE_H

/* includes */
#include "compiler.h"
#include <dos/dos.h>
#include <exec/lists.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/semaphores.h>
#include "debug.h"
#include "sana2.h"

/* reassign Library bases from global definitions to own struct */
#define SysBase       db->db_SysBase
#define DOSBase       db->db_DOSBase
#define UtilityBase   db->db_UtilityBase

struct DevUnit {
	/* HW Data (generic for now) (example only, unused in construct)*/
	ULONG	du_hwl0;
	ULONG	du_hwl1;
	ULONG	du_hwl2;
	APTR	du_hwp0;
	APTR	du_hwp1;
	APTR	du_hwp2;
};

struct devbase {
	struct Library db_Lib;
	BPTR db_SegList;            /* from Device Init */

	struct Library *db_SysBase; /* Exec Base */
	struct Library *db_DOSBase;
	struct Library *db_UtilityBase;
	struct Sana2DeviceStats db_DevStats;

	volatile USHORT db_online;
	volatile USHORT db_currentWifiState;   // the *actual* online state

    // SCSI device (in the main task)
	void* db_scsiSettings;    // A pointer to a ScsiDaynaSettings struct  
	USHORT db_scsiDeviceID;	  // The device ID that should be used going forward (auto-detect)
	USHORT db_scsiMode;       // Scsi mode
	struct List db_ReadList;
	struct SignalSemaphore db_ReadListSem;
	struct List db_WriteList;
	struct SignalSemaphore db_WriteListSem;
	struct List db_EventList;
	struct SignalSemaphore db_EventListSem;   
	struct List db_ReadOrphanList;
	struct SignalSemaphore db_ReadOrphanListSem;
	struct Process* db_Proc;
	struct SignalSemaphore db_ProcExitSem;
};

#ifndef DEVBASETYPE
#define DEVBASETYPE struct devbase
#endif
#ifndef DEVBASEP
#define DEVBASEP DEVBASETYPE *db
#endif

/* PROTOS */

ASM LONG LibNull( void );

ASM SAVEDS struct Device *DevInit(ASMR(d0) DEVBASEP                  ASMREG(d0), 
                                  ASMR(a0) BPTR seglist              ASMREG(a0), 
				  ASMR(a6) struct Library *_SysBase  ASMREG(a6) );

ASM SAVEDS LONG DevOpen( ASMR(a1) struct IOSana2Req *ios2            ASMREG(a1), 
                         ASMR(d0) ULONG unit                         ASMREG(d0), 
                         ASMR(d1) ULONG flags                        ASMREG(d1),
                         ASMR(a6) DEVBASEP                           ASMREG(a6) );

ASM SAVEDS BPTR DevClose(   ASMR(a1) struct IORequest *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS BPTR DevExpunge( ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS VOID DevBeginIO( ASMR(a1) struct IOSana2Req *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS LONG DevAbortIO( ASMR(a1) struct IORequest *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

void DevTermIO( DEVBASETYPE*, struct IORequest * );

/* private functions */
#ifdef DEVICE_MAIN

#endif /* DEVICE_MAIN */

#define HW_ADDRFIELDSIZE          6
#define HW_ETH_HDR_SIZE          14       /* ethernet header: dst, src, type */

typedef BOOL (*BMFunc)(__reg("a0") void* a, __reg("a1") void* b, __reg("d0") long c);

typedef struct BufferManagement
{
  struct MinNode   bm_Node;
  BMFunc           bm_CopyFromBuffer;
  BMFunc           bm_CopyToBuffer;
} BufferManagement;

#endif /* _INC_DEVICE_H */
