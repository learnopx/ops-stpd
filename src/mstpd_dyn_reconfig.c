/*
 * (c) Copyright 2016 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
/**********************************************************************************
 *    File               : mstpd_dyn_reconfig.c
 *    Description        : MSTP Protocol Dynamic Reconfiguration Changes
 **********************************************************************************/

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

#include <util.h>
#include <daemon.h>
#include <dirs.h>
#include <unixctl.h>
#include <fatal-signal.h>
#include <command-line.h>
#include <vswitch-idl.h>
#include <openvswitch/vlog.h>
#include <assert.h>
#include <eventlog.h>

#include "mstp_fsm.h"
#include "mstp_recv.h"
#include "mstp_ovsdb_if.h"
#include "mstp_inlines.h"

VLOG_DEFINE_THIS_MODULE(mstpd_dyn_reconfig);
/*---------------------------------------------------------------------------
 * Local functions prototypes (forward declarations)
 *---------------------------------------------------------------------------*/
/** ======================================================================= **
 *                                                                           *
 *     Global Functions (externed)                                           *
 *                                                                           *
 ** ======================================================================= **/
static void mstp_updatePortStateToForward();
/**PROC+**********************************************************************
 * Name:      mstp_addLport
 *
 * Purpose:   Add new logical port under MSTP control.
 *            Called by 'stpAddLportToVlan' function, which is in turn gets
 *            called whenever someone dynamically adds a port to a VLAN.
 *
 * Params:    vlan  -> VLAN number where new port has been added to
 *            lport -> logical port number
 *
 * Returns:   none
 *
 * Globals:
 *
 * Constraints:
 **PROC-**********************************************************************/
void
mstp_addLport(LPORT_t lport)
{
   MSTID_t                 mstid;
   char                     portName[PORTNAME_LEN];

   if (!IS_VALID_LPORT(lport))
   {
      STP_ASSERT(0);
      return;
   }

   /*------------------------------------------------------------------------
    * If MSTP is not enabled there is nothing to do
    *------------------------------------------------------------------------*/
   if(MSTP_ENABLED == FALSE)
      return;

   /*------------------------------------------------------------------------
    * Initialize CIST specific port data
    *------------------------------------------------------------------------*/
   mstp_initCistPortData(lport, TRUE);

   /*------------------------------------------------------------------------
    * Initialize MSTI specific port data for every configured MST Instance
    *------------------------------------------------------------------------*/
   for(mstid = MSTP_MSTID_MIN; mstid <= MSTP_MSTID_MAX; mstid++)
   {
      if(MSTP_MSTI_INFO(mstid))
      {
         mstp_initMstiPortData(mstid, lport, TRUE);
      }
   }

   /*---------------------------------------------------------------------
    * No configuration dynamic changes occured yet
    * NOTE: in 'update' routines called in the initialization path through
    *       the 'mstp_initCommonPortData', 'mstp_initCistPortData' and
    *       'mstp_initMstiPortData' function calls in the above the global
    *       variable MSTP_DYN_RECONFIG_CHANGE can be set TRUE if the values
    *       read from config are different from the MSTP operational data
    *       (at the time of initialization it will be the case). To avoid
    *       MSTP re-initialization we set MSTP_DYN_RECONFIG_CHANGE to FALSE
    *---------------------------------------------------------------------*/
   MSTP_DYN_RECONFIG_CHANGE = FALSE;

   /*------------------------------------------------------------------------
    * Initialize per-Port State Machines
    *------------------------------------------------------------------------*/
   MSTP_BEGIN = TRUE;

   /*------------------------------------------------------------------------
    * per-Port per-Bridge SMs
    *------------------------------------------------------------------------*/
   mstp_ppmSm(lport);
   mstp_bdmSm(lport);
   mstp_prxSm(NULL, lport);
   mstp_ptxSm(lport);
   mstp_ptiSm(lport);

   /*------------------------------------------------------------------------
    * per-Port per-Tree SMs
    *------------------------------------------------------------------------*/
   mstp_pimSm(NULL, MSTP_CISTID, lport);
   mstp_prtSm(MSTP_CISTID, lport);
   mstp_pstSm(MSTP_CISTID, lport);
   mstp_tcmSm(MSTP_CISTID, lport);

   for(mstid = MSTP_MSTID_MIN; mstid <= MSTP_MSTID_MAX; mstid++)
   {
      if(MSTP_MSTI_VALID(mstid))
      {
         mstp_pimSm(NULL, mstid, lport);
         mstp_prtSm(mstid, lport);
         mstp_pstSm(mstid, lport);
         mstp_tcmSm(mstid, lport);
      }
   }

   MSTP_BEGIN = FALSE;

   MSTP_DYN_CFG_PRINTF("!ADDED NEW PORT %d", lport);
   intf_get_port_name(lport, portName);
   MSTP_PRINTF_EVENT("Added a new Port - %s", portName);

}

/**PROC+**********************************************************************
 * Name:      mstp_removeLports
 *
 * Purpose:   Remove logical ports out of MSTP control.
 *
 * Params:    lports -> list of logical ports to be removed
 *
 * Returns:   none
 *
 * Globals:
 *
 * Constraints:
 **PROC-**********************************************************************/
void
mstp_removeLport(LPORT_t lport)
{
    MSTID_t mstid;
    /*------------------------------------------------------------------------
     * If we are being called before MSTP has finish initialization then exit
     * (this happens during initialization). 'Stp_Initialized' is also set to
     * FALSE when TFTP frees the memory for downloading.
     *------------------------------------------------------------------------*/
    /*------------------------------------------------------------------
     * Update internal MSTP data structures do not refer to the removing
     * 'lport' (e.g. if this port is the Root Port then some global data
     * structures may keep track of it, as a result it will cause the
     * problems if someone tries to refer to the port's data structure
     * that does not exist).
     * NOTE: this is vitally important if port removal is not being
     *       accompanied with 'port_down' or 'port_up' events or they
     *       are being significantly delayed
     * The function call below will kick appropriate state machines and
     * they will synchronise the protocol data with the environmental
     * changes.
     *------------------------------------------------------------------*/
    if (MSTP_ENABLED)
    {
        mstp_portDisable(lport);
    }

    /*------------------------------------------------------------------
     * Remove port data from every configured MST Instance
     *------------------------------------------------------------------*/
    for(mstid = MSTP_MSTID_MIN; mstid <= MSTP_MSTID_MAX; mstid++)
    {
        if(MSTP_MSTI_INFO(mstid) && MSTP_MSTI_PORT_PTR(mstid, lport))
        {
            mstp_clearMstiPortData(mstid, lport);
        }
    }

    /*------------------------------------------------------------------
     * Remove port data from the CIST
     *------------------------------------------------------------------*/
    if(MSTP_CIST_PORT_PTR(lport))
        mstp_clearCistPortData(lport);

    /*------------------------------------------------------------------
     * Remove common port data (used by the CIST and all the MSTIs)
     *------------------------------------------------------------------*/
    if(MSTP_COMM_PORT_PTR(lport))
        mstp_clearCommonPortData(lport);

    MSTP_DYN_CFG_PRINTF("!REMOVED PORT %d", lport);

    /*------------------------------------------------------------------------
     * After port(s) deletion lets update port maps that MSTP uses when
     * interacts with other subsystems to propagate ports state change info,
     * to make sure we do not have non-existent ports to be set in those port
     * maps (all port maps are located in mstp_CB data structure).
     *------------------------------------------------------------------------*/
    mstp_updateMstpCBPortMaps(lport);

}

/**PROC+**********************************************************************
 * Name:      mstp_adminStatusUpdate
 *
 * Purpose:   Activate/deactivate MSTP on the switch and update internal MSTP
 *            data structures according to the new administrative status of the
 *            protocol.
 *            Do nothing if the state of the protocol has not been changed.
 *
 * Params:    status -> the administrative status of MSTP to be set
 *
 * Returns:   none
 *
 * Globals:   mstp_CB, mstp_Bridge
 *
 * Constraints:
 **PROC-**********************************************************************/
void
mstp_adminStatusUpdate(int status)
{
   if((Spanning == FALSE) && (status == TRUE))
   {/* the protocol has been enabled, run it */

      Spanning = TRUE; /* also used by other features */

      /*---------------------------------------------------------------------
       * Initialize global 'mstp_MstiVlanTable'. By default all VLANs
       * configured on the switch will be mapped to the CIST.
       * Initialize global 'mstp_MstIdToVlanGroupNumTable' - used for
       * mapping MST instance ID to VLAN group number.
       *---------------------------------------------------------------------*/
      VLOG_DBG("%s : MSTP Enable Path", __FUNCTION__);
      mstp_initMstiVlanTables();
      VLOG_DBG("%s : MSTP VLAN tables initialized", __FUNCTION__);

      /*---------------------------------------------------------------------
       * allocate and initialize MSTP data structures with the data read
       * from config.
       * NOTE: TRUE below means that data structures do not exist yet, so
       *       they need to be allocated and filled from config
       *---------------------------------------------------------------------*/
      mstp_initProtocolData(TRUE);
      VLOG_DBG("%s : MSTP Protocol tables initialized", __FUNCTION__);

      /*---------------------------------------------------------------------
       * No configuration dynamic changes occured yet
       * NOTE: in 'update' routines called in the initialization path through
       *       the 'mstp_initProtocolData' function call in above the global
       *       variable MSTP_DYN_RECONFIG_CHANGE can be set TRUE if the values
       *       read from config are different from the MSTP operational data
       *       (at the time of initialization it will be the case). To avoid
       *       MSTP re-initialization we set MSTP_DYN_RECONFIG_CHANGE to FALSE
       *---------------------------------------------------------------------*/
      MSTP_DYN_RECONFIG_CHANGE = FALSE;

      /*----------------------------------------------------------------------
       * clear portmaps used to keep track of lports MSTP has told DB are
       * forwarding or blocked (used to escape message flooding when MSTP
       * ports transitioning states on multiple Trees).
       *---------------------------------------------------------------------*/
      clear_port_map(&MSTP_FWD_LPORTS);
      clear_port_map(&MSTP_BLK_LPORTS);

      /*---------------------------------------------------------------------
       * initialize MSTP state machines,
       *---------------------------------------------------------------------*/
      VLOG_DBG("%s : MSTP Trigerring State machines.", __FUNCTION__);
      mstp_initStateMachines();
      VLOG_DBG("%s : MSTP State machines triggered.", __FUNCTION__);

      /*---------------------------------------------------------------------
       * activate all logical ports that are in 'Up' state.
       *---------------------------------------------------------------------*/
      mstp_enableActiveLogicalPorts();
      /*---------------------------------------------------------------------
       * log VLOG message
       *---------------------------------------------------------------------*/
      MSTP_PRINTF_EVENT("Spanning Tree Protocol enabled");
      log_event("MSTP_ENABLED", NULL);
   }
   else
   if((Spanning == TRUE) && (status == FALSE))
   {/* the protocol has been disabled, clear MSTP data */
      /*---------------------------------------------------------------------
       * clear in-memory data used by MSTP
       *---------------------------------------------------------------------*/
      mstp_clearProtocolData();

      /*---------------------------------------------------------------------
       * indicate that MSTP protocol is not initialized
       *---------------------------------------------------------------------*/
      Spanning = FALSE; /* also used by other features */
      /*---------------------------------------------------------------------
       * Remove from the queue all pending MSTP messages to DB
       *---------------------------------------------------------------------*/
      mstp_clearMstpToOthersMessageQueue();
      /*---------------------------------------------------------------------
       * Put the Ports back into the Forward State
       *--------------------------------------------------------------------*/
      mstp_updatePortStateToForward();
      /*----------------------------------------------------------------------
       * clear portmaps used to keep track of lports MSTP has told DB are
       * forwarding or blocked (used to escape message flooding when MSTP
       * ports transitioning states on multiple Trees).
       *---------------------------------------------------------------------*/
      clear_port_map(&MSTP_FWD_LPORTS);
      clear_port_map(&MSTP_BLK_LPORTS);
      mstp_config_reinit();

      /*---------------------------------------------------------------------
       * log VLOG message
       *---------------------------------------------------------------------*/

      MSTP_PRINTF_EVENT("Spanning Tree Protocol disabled");
      log_event("MSTP_DISABLED", NULL);
   }
}
/**PROC+**********************************************************************
 * Name:      mstp_updatePortStateToForward
 *
 * Purpose:   To put all the L2 Ports into Forwarding State.
 *
 * Params:    none
 *
 * Returns:   none
 *
 * Globals:   none
 *
 * Constraints:
 **PROC-**********************************************************************/
void mstp_updatePortStateToForward()
{
    struct ovsdb_idl_txn *txn = NULL;
    const struct ovsrec_bridge *bridge_row = NULL;
    const struct ovsrec_mstp_common_instance_port *cist_port_row = NULL;
    const struct ovsrec_mstp_instance *mstp_row = NULL;
    const struct ovsrec_mstp_instance_port *mstp_port_row = NULL;
    int mstid = 0, port_id = 0;
    bridge_row = ovsrec_bridge_first(idl);
    MSTP_OVSDB_LOCK;
    txn = ovsdb_idl_txn_create(idl);
    OVSREC_MSTP_COMMON_INSTANCE_PORT_FOR_EACH(cist_port_row, idl)
    {
        ovsrec_mstp_common_instance_port_set_port_state(cist_port_row,MSTP_STATE_FORWARD);
    }
    for (mstid=0; mstid < bridge_row->n_mstp_instances; mstid++) {
        mstp_row = bridge_row->value_mstp_instances[mstid];
        if(!mstp_row) {
            assert(0);
            ovsdb_idl_txn_destroy(txn);
            MSTP_OVSDB_UNLOCK;

            return;
        }

        /* MSTP instance port clean */
        for (port_id=0; port_id < mstp_row->n_mstp_instance_ports; port_id++) {
            mstp_port_row = mstp_row->mstp_instance_ports[port_id];
            if(!mstp_port_row) {
                assert(0);
                ovsdb_idl_txn_destroy(txn);
                MSTP_OVSDB_UNLOCK;
                return;
            }
            ovsrec_mstp_instance_port_set_port_state( mstp_port_row, MSTP_STATE_FORWARD);
        }
    }
    ovsdb_idl_txn_commit_block(txn);
    ovsdb_idl_txn_destroy(txn);
    MSTP_OVSDB_UNLOCK;
    return;
}


/**PROC+**********************************************************************
 * Name:      mstp_updateMstiVidMapping
 *
 * Purpose:   Update global 'mstp_MstiVidTable' from the MSTP MIB data
 *            This function gets called in the context of VID to MSTI dynamic
 *            reconfiguration changes
 *
 * Params:    mstid -> MST Instance Identifier
 *            data  -> pointer to the SNMP MIB data
 *
 * Returns:   TRUE if VIDs to MST Instance mapping has been changed,
 *            FALSE otherwise
 *
 * Globals:   none
 *
 * Constraints:
 **PROC-**********************************************************************/
bool
mstp_updateMstiVidMapping(MSTID_t mstid, VID_MAP newVidMap)
{
   bool     res = FALSE;
   VID_MAP *curVidMap;

   STP_ASSERT(mstid == MSTP_CISTID || MSTP_VALID_MSTID(mstid));

   STP_ASSERT(are_any_vids_set(&newVidMap));

   /*------------------------------------------------------------------------
    * Set pointer to the current VID MAP entry associated with given MSTI
    *------------------------------------------------------------------------*/
   curVidMap = &mstp_MstiVidTable[mstid];

   /*------------------------------------------------------------------------
    * If VIDs to MST Instance mapping has changed we need to do a few things:
    * 1). update MSTI's VID MAP in the global 'mstp_MstiVidTable'
    *------------------------------------------------------------------------*/
   if(!are_vidmaps_equal(&newVidMap, curVidMap))
   {/* VID mapping has changed for this MST Instance */
      VID_MAP addVidMap;
      VID_MAP delVidMap;

      /*---------------------------------------------------------------------
       * Find VIDs that are being removed from the MST Instance.
       *---------------------------------------------------------------------*/
      copy_vid_map(&newVidMap, &delVidMap);
      bit_inverse_vid_map(&delVidMap);
      bit_and_vid_maps(curVidMap, &delVidMap);

      /*---------------------------------------------------------------------
       * Find VIDs that are being added to the MST Instance.
       *---------------------------------------------------------------------*/
      copy_vid_map(curVidMap, &addVidMap);
      bit_inverse_vid_map(&addVidMap);
      bit_and_vid_maps(&newVidMap, &addVidMap);

      /*---------------------------------------------------------------------
       * Handle unmapped VIDs, if any
       *---------------------------------------------------------------------*/
      if(are_any_vids_set(&delVidMap))
      {
         /*------------------------------------------------------------------
          * VIDs that are removed from the MSTI should be mapped back to
          * the CIST in the global 'mstp_MstiVidTable'.
          *------------------------------------------------------------------*/
         bit_or_vid_maps(&delVidMap, &mstp_MstiVidTable[MSTP_CISTID]);
      }

      /*---------------------------------------------------------------------
       * Handle newly mapped VIDs, if any
       *---------------------------------------------------------------------*/
      if(are_any_vids_set(&addVidMap))
      {
         VID_MAP  tmpVidMap;
         /*------------------------------------------------------------------
          * VIDs that are added to the MSTI should be unmapped from the
          * CIST in the global 'mstp_MstiVidTable'.
          *------------------------------------------------------------------*/
         copy_vid_map(&addVidMap, &tmpVidMap);
         bit_inverse_vid_map(&tmpVidMap);
         bit_and_vid_maps(&tmpVidMap, &mstp_MstiVidTable[MSTP_CISTID]);
      }

      /*---------------------------------------------------------------------
       * Store new MSTI's VID mapping data in the global 'mstp_MstiVidTable'
       *---------------------------------------------------------------------*/
      copy_vid_map(&newVidMap, curVidMap);

      /*---------------------------------------------------------------------
       * Indicate that MSTI's VID mapping has changed
       *---------------------------------------------------------------------*/
      res = TRUE;
   }

   return res;
}
