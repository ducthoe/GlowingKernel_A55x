/*
 * Copyrights (C) 2018 Samsung Electronics, Inc.
 * Copyrights (C) 2018 Silicon Mitus, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/sched.h>
#if defined(CONFIG_BATTERY_NOTIFIER)
#include <linux/battery/battery_notifier.h>
#else
#include <linux/battery/sec_pd.h>
#endif
#include <linux/mfd/sm/sm5714/sm5714_log.h>
#include <linux/usb/typec/sm/sm5714/sm5714_pd.h>
#include <linux/usb/typec/sm/sm5714/sm5714_typec.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/time.h>
#if IS_ENABLED(CONFIG_USB_NOTIFY_LAYER)
#include <linux/usb_notify.h>
#endif
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
#include "../../../../battery/common/sec_charging_common.h"
#endif

static unsigned int SRC_CHECK_LIST[][3] = {
	{MODE_MSG, MSG_GET_SRC_CAP, PE_SRC_Give_Source_Cap},
	{MODE_MSG, MSG_GET_SNK_CAP, PE_SRC_Give_Sink_Cap},
	{MODE_MSG, MSG_REQUEST, PE_SRC_Negotiate_Capability},
	{MODE_MSG, MSG_ACCEPT, PE_SRC_Send_Soft_Reset},
	{MODE_MSG, MSG_REJECT, PE_SRC_Send_Reject},
	{MODE_MSG, MSG_PR_SWAP, PE_PRS_SRC_SNK_Evaluate_Swap},
	{MODE_MSG, MSG_DR_SWAP, PE_DRS_Evaluate_Port},
	{MODE_MSG, MSG_VCONN_SWAP, PE_VCS_Evaluate_Swap},
	{MODE_MSG, MSG_BIST_M2, PE_BIST_CARRIER_M2},
#if IS_ENABLED(CONFIG_PDIC_PD30)
	{MODE_MSG, MSG_NOT_SUPPORTED, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_SRC_CAP_EXT, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_FR_SWAP, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_PPS_STATUS, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_COUNTRY_CODES, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_SNK_CAP_EXT, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_BAT_STATUS, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_COUNTRY_INFO, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_SRC_CAP_EXT, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_BAT_CAP, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_BAT_STATUS, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_MANUF_INFO, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_MANUF_INFO, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_SECURITY_REQ, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_SECURITY_RES, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_FW_UPDATE_REQ, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_FW_UPDATE_RES, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_PPS_STATUS, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_COUNTRY_INFO, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_COUNTRY_CODES, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_SNK_CAP_EXT, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_GET_SRC_INFO, PE_SRC_Send_Not_Supported},
	{MODE_MSG, MSG_RESERVED, PE_SRC_Send_Not_Supported},
#endif
	{MODE_MSG, VDM_DISCOVER_IDENTITY, PE_DFP_VDM_Get_Identity},
	{MODE_MSG, VDM_DISCOVER_SVID, PE_UFP_VDM_Get_SVIDs_NAK},
	{MODE_MSG, VDM_DISCOVER_MODE, PE_UFP_VDM_Get_Modes_NAK},
	{MODE_MSG, VDM_ENTER_MODE, PE_UFP_VDM_Evaluate_Mode_Entry},
	{MODE_MSG, VDM_ATTENTION, PE_DFP_VDM_Attention_Request},
	{MODE_MSG, VDM_DP_STATUS_UPDATE, PE_UFP_VDM_Evaluate_Status},
	{MODE_MSG, VDM_DP_CONFIGURE, PE_UFP_VDM_Evaluate_Configure},
	{MODE_MSG, UVDM_MSG, PE_DFP_UVDM_Receive_Message},
	{MODE_CMD, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message},
	{MODE_CMD, MANAGER_REQ_GET_SNKCAP, PE_SRC_Get_Sink_Cap},
	{MODE_CMD, MANAGER_REQ_GOTOMIN, PE_SRC_Transition_Supply},
	{MODE_CMD, MANAGER_REQ_SRCCAP_CHANGE, PE_SRC_Send_Capabilities},
	{MODE_CMD, MANAGER_REQ_PR_SWAP, PE_PRS_SRC_SNK_Send_Swap},
	{MODE_CMD, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port},
	{MODE_CMD, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_IDENTITY,
		PE_DFP_VDM_Identity_Request},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request},
	{MODE_CMD, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request},
	{MODE_CMD, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update},
	{MODE_CMD, MANAGER_REQ_VDM_DisplayPort_Configure,
		PE_DFP_VDM_DisplayPort_Configure},
	{MODE_CMD, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request},
};

static unsigned int SNK_CHECK_LIST[][3] = {
	{MODE_MSG, MSG_ACCEPT, PE_SNK_Send_Soft_Reset},
	{MODE_MSG, MSG_REJECT, PE_SNK_Send_Reject},
	{MODE_MSG, MSG_GET_SRC_CAP, PE_SNK_Give_Source_Cap},
	{MODE_MSG, MSG_GET_SNK_CAP, PE_SNK_Give_Sink_Cap},
	{MODE_MSG, MSG_SRC_CAP, PE_SNK_Evaluate_Capability},
	{MODE_MSG, MSG_PR_SWAP, PE_PRS_SNK_SRC_Evaluate_Swap},
	{MODE_MSG, MSG_DR_SWAP, PE_DRS_Evaluate_Port},
	{MODE_MSG, MSG_VCONN_SWAP, PE_VCS_Evaluate_Swap},
	{MODE_MSG, MSG_BIST_M2, PE_BIST_CARRIER_M2},
	{MODE_MSG, MSG_GET_STATUS, PE_SNK_Get_Source_Status},
	{MODE_MSG, MSG_GET_PPS_STATUS, PE_SNK_Get_Source_PPS_Status},
#if IS_ENABLED(CONFIG_PDIC_PD30)
	{MODE_MSG, MSG_NOT_SUPPORTED, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_SRC_CAP_EXT, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_FR_SWAP, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_COUNTRY_CODES, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_SNK_CAP_EXT, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_BAT_STATUS, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_COUNTRY_INFO, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_SRC_CAP_EXT, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_BAT_CAP, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_BAT_STATUS, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_GET_BAT_STATUS, PE_SNK_Give_Battery_Status},
	{MODE_MSG, MSG_GET_MANUF_INFO, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_MANUF_INFO, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_SECURITY_REQ, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_SECURITY_RES, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_FW_UPDATE_REQ, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_FW_UPDATE_RES, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_PPS_STATUS, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_COUNTRY_INFO, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_COUNTRY_CODES, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_SNK_CAP_EXT, PE_SNK_Send_Not_Supported},
	{MODE_MSG, MSG_RESERVED, PE_SNK_Send_Not_Supported},
#endif
	{MODE_MSG, VDM_DISCOVER_IDENTITY, PE_UFP_VDM_Get_Identity},
	{MODE_MSG, VDM_DISCOVER_SVID, PE_UFP_VDM_Get_SVIDs},
	{MODE_MSG, VDM_DISCOVER_MODE, PE_UFP_VDM_Get_Modes},
	{MODE_MSG, VDM_ENTER_MODE, PE_UFP_VDM_Evaluate_Mode_Entry},
	{MODE_MSG, VDM_EXIT_MODE, PE_UFP_VDM_Mode_Exit},
	{MODE_MSG, VDM_ATTENTION, PE_DFP_VDM_Attention_Request},
	{MODE_MSG, VDM_DP_STATUS_UPDATE, PE_UFP_VDM_Evaluate_Status},
	{MODE_MSG, VDM_DP_CONFIGURE, PE_UFP_VDM_Evaluate_Configure},
	{MODE_MSG, UVDM_MSG, PE_DFP_UVDM_Receive_Message},
	{MODE_CMD, MANAGER_REQ_UVDM_SEND_MESSAGE, PE_DFP_UVDM_Send_Message},
	{MODE_CMD, MANAGER_REQ_NEW_POWER_SRC, PE_SNK_Select_Capability},
	{MODE_CMD, MANAGER_REQ_PR_SWAP, PE_PRS_SNK_SRC_Send_Swap},
	{MODE_CMD, MANAGER_REQ_DR_SWAP, PE_DRS_Evaluate_Send_Port},
	{MODE_CMD, MANAGER_REQ_VCONN_SWAP, PE_VCS_Send_Swap},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_IDENTITY,
		PE_DFP_VDM_Identity_Request},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_SVID, PE_DFP_VDM_SVIDs_Request},
	{MODE_CMD, MANAGER_REQ_VDM_DISCOVER_MODE, PE_DFP_VDM_Modes_Request},
	{MODE_CMD, MANAGER_REQ_VDM_ATTENTION, PE_UFP_VDM_Attention_Request},
	{MODE_CMD, MANAGER_REQ_VDM_ENTER_MODE, PE_DFP_VDM_Mode_Entry_Request},
	{MODE_CMD, MANAGER_REQ_VDM_STATUS_UPDATE, PE_DFP_VDM_Status_Update},
	{MODE_CMD, MANAGER_REQ_VDM_DisplayPort_Configure,
		PE_DFP_VDM_DisplayPort_Configure},
};

static inline unsigned int CHECK_MSG_CMD(struct sm5714_usbpd_data *pd,
	unsigned int mode, unsigned int msg, unsigned int ret)
{
	switch (mode) {
	case MODE_MSG:
		if (pd->phy_ops.get_status(pd, msg))
			return ret;
		break;
	case MODE_CMD:
		if (pd->manager.cmd == msg) {
			pd->manager.cmd = 0;
			return ret;
		}
		break;
	default:
		break;
	}
	return 0;
}

static policy_state sm5714_usbpd_policy_src_startup(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (sm5714_check_vbus_state(pd_data)) {
		if (pdic_data->reset_done == 0) {
			pd_data->counter.caps_counter = 0;
			sm5714_usbpd_init_protocol(pd_data); /* prl reset  */
		} else {
			pdic_data->reset_done = 0;
			if (pdic_data->vconn_en && !pdic_data->pd_support) {
				policy->origin_message = 0x01; /* SOP' Msg */
				sm5714_set_enable_pd_function(pd_data, PD_ENABLE);
				return PE_SRC_VDM_Identity_Request;
			} else
				return PE_SRC_Send_Capabilities;
		}
	}
	return PE_SRC_Startup;
}

static policy_state sm5714_usbpd_policy_src_vdm_identity_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		if (pd_data->counter.discover_identity_counter <= USBPD_nDiscoverIdentityCount)
			pd_data->counter.discover_identity_counter++;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if ((policy->last_state == PE_SRC_Discovery) &&
				(pd_data->policy.rx_msg_header.spec_revision == USBPD_REV_20))
			pd_data->specification_revision = USBPD_REV_20;
		else
			pd_data->specification_revision = USBPD_REV_30;
#endif
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = 0;
		policy->tx_msg_header.port_power_role = 0;
		policy->tx_msg_header.num_data_objs = 1;
		policy->tx_msg_header.extended = 0;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					Discover_Identity;

		policy->origin_message = 0x01; /* SOP' Msg */
		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data,
					BITMSG(VDM_DISCOVER_IDENTITY), tVDMSenderResponse)) {
				pd_data->counter.discover_identity_counter = 0;

				dev_info(pd_data->dev, "Msg header objs(%d)\n",
					policy->rx_msg_header.num_data_objs);
				dev_info(pd_data->dev, "VDM header type(%d)\n",
					policy->rx_data_obj[0].structured_vdm.command_type);
				dev_info(pd_data->dev, "ID Header VDO 0x%x\n",
					policy->rx_data_obj[1].object);
				dev_info(pd_data->dev, "Cert Stat VDO 0x%x\n",
					policy->rx_data_obj[2].object);
				dev_info(pd_data->dev, "Product VDO 0x%x\n",
					policy->rx_data_obj[3].object);
				dev_info(pd_data->dev, "Product Type VDO 0x%x\n",
					policy->rx_data_obj[4].object);
			}
			return PE_SRC_Send_Capabilities;
		} else {
			return PE_SRC_Send_Capabilities;
		}
	}
	return PE_SRC_VDM_Identity_Request;
}

static policy_state sm5714_usbpd_policy_src_discovery(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);
	msleep(tSendSourceCap);
	if (pdic_data->vconn_en && !pdic_data->pd_support) {
		if (pd_data->counter.discover_identity_counter <= USBPD_nDiscoverIdentityCount)
			return PE_SRC_VDM_Identity_Request;
	}
	if (pd_data->counter.caps_counter <= USBPD_nCapsCount)
		return PE_SRC_Send_Capabilities;
	else
		return PE_SRC_Disabled;
}

static policy_state sm5714_usbpd_policy_src_send_capabilities(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (!pdic_data->vconn_en && !pdic_data->pd_support) {
			if ((policy->last_state == PE_SRC_Discovery) &&
					(pd_data->policy.rx_msg_header.spec_revision == USBPD_REV_20))
				pd_data->specification_revision = USBPD_REV_20;
			else
				pd_data->specification_revision = USBPD_REV_30;
		}
#endif
		policy->tx_msg_header.word = pd_data->source_msg_header.word;
		policy->tx_data_obj[0].object = pd_data->source_data_obj[0].object;
		if (!pd_data->thermal_state)
			policy->tx_data_obj[1].object = pd_data->source_data_obj[1].object;
		pd_data->counter.caps_counter++;
		policy->origin_message = 0x00;
		sm5714_usbpd_send_msg(pd_data,
				&policy->tx_msg_header, policy->tx_data_obj);
		return PE_SRC_Send_Capabilities;
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		u64 msg = 0;

		msg |= BITMSG(MSG_REQUEST);
		msg |= BITMSG(MSG_GET_SNK_CAP);
		if (sm5714_usbpd_wait_msg(pd_data, msg, tSenderResponse)) {
			if (policy->rx_msg_header.msg_type == USBPD_Request &&
					policy->rx_msg_header.num_data_objs > 0) {
#if IS_ENABLED(CONFIG_PDIC_PD30)
				if (!pdic_data->vconn_en &&
						(pd_data->policy.rx_msg_header.spec_revision == USBPD_REV_20))
					pd_data->specification_revision = USBPD_REV_20;
#endif
				pd_data->counter.hard_reset_counter = 0;
				pd_data->counter.caps_counter = 0;
				pd_data->source_request_obj.object
						= policy->rx_data_obj[0].object;
				dev_info(pd_data->dev, "got Request.\n");
				sm5714_set_enable_pd_function(pd_data, PD_ENABLE);
				return PE_SRC_Negotiate_Capability;
			} else if (policy->rx_msg_header.msg_type ==
					USBPD_Get_Sink_Cap) {
				return PE_SRC_Send_Soft_Reset;
			}
			dev_err(pd_data->dev,
				"Not get request object\n");
			goto hard_reset;
		} else if (pd_data->phy_ops.get_status(pd_data, MSG_GOODCRC)) {
			if (policy->abnormal_state)
				return PE_SRC_Send_Capabilities;
			pd_data->counter.caps_counter = 0;
			dev_err(pd_data->dev,
				"%s NoResponseTimer\n", __func__);
			goto hard_reset;
		} else {
			if (pd_data->protocol_tx.status == MESSAGE_SENT)
				return PE_SRC_Hard_Reset;
			else
				return PE_SRC_Discovery;
		}
	} else {
		return PE_SRC_Send_Capabilities;
	}
hard_reset:
	if (pd_data->counter.hard_reset_counter > USBPD_nHardResetCount) {
		if (pdic_data->pd_support)
			return Error_Recovery;
		else
			return PE_SRC_Disabled;
	}
	return PE_SRC_Hard_Reset;
}

static policy_state sm5714_usbpd_policy_src_negotiate_capability(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		if (policy->last_state == PE_SRC_Ready) {
			pd_data->source_request_obj.object
				= policy->rx_data_obj[0].object;
		}

		if (sm5714_usbpd_match_request(pd_data) == 0)
			sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
						USBPD_Accept, USBPD_DFP, USBPD_SOURCE);
		else
			return PE_SRC_Capability_Response;
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (pd_data->source_request_obj.request_data_object.object_position == 1) {
				if (!pdic_data->is_otg_vboost) {
					sm5714_info("%s booster off\n", __func__);
					sm5714_usbpd_turn_off_reverse_booster(pd_data);
				}
			} else if (pd_data->source_request_obj.request_data_object.object_position == 2) {
				sm5714_info("%s otg off\n", __func__);
				sm5714_vbus_turn_on_ctrl(pdic_data, 0);
			}
			return PE_SRC_Transition_Supply;
		} else
			return PE_SRC_Send_Soft_Reset;
	}
	return PE_SRC_Negotiate_Capability;
}

static policy_state sm5714_usbpd_policy_src_transition_supply(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		if (pd_data->source_request_obj.request_data_object.object_position == 1) {
			if (!pdic_data->is_otg_vboost) {
				sm5714_info("%s otg on\n", __func__);
				sm5714_usbpd_turn_on_source(pd_data);
			}
		} else if (pd_data->source_request_obj.request_data_object.object_position == 2) {
			sm5714_info("%s booster on\n", __func__);
			sm5714_usbpd_turn_on_reverse_booster(pd_data);
		}
		msleep(tSrcTransition);
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
					USBPD_PS_RDY, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			pd_data->phy_ops.get_power_role(pd_data, &power_role);
			return PE_SRC_Ready;
		} else {
			return PE_SRC_Send_Soft_Reset;
		}
	}

	return PE_SRC_Transition_Supply;
}

static policy_state sm5714_usbpd_policy_src_ready(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int i = 0, data_role = 0;
	unsigned int ret;

	dev_info(pd_data->dev, "%s\n", __func__);

	sm5714_usbpd_power_ready(pd_data->dev, TYPE_C_ATTACH_SRC);

#ifndef CONFIG_SEC_FACTORY
#if IS_ENABLED(CONFIG_PDIC_PD30)
	if (pd_data->specification_revision == USBPD_REV_30) {
		if (policy->last_state != policy->state)
			sm5714_usbpd_set_ams_control(pd_data, AUTO_RP_CNTL);
		sm5714_usbpd_set_ams_control(pd_data, END_AMS_PRL);
	} else
#endif
		sm5714_usbpd_set_rp_scr_sel(pd_data, PLUG_CTRL_RP180);
#endif

	for (i = 0; i < ARRAY_SIZE(SRC_CHECK_LIST); ++i) {
		ret = CHECK_MSG_CMD(pd_data, SRC_CHECK_LIST[i][0],
			SRC_CHECK_LIST[i][1], SRC_CHECK_LIST[i][2]);
		if (ret > 0) {
#if !defined(CONFIG_SEC_FACTORY) && defined(CONFIG_PDIC_PD30)
			if (pd_data->specification_revision == USBPD_REV_30) {
				switch (ret) {
				case PE_DFP_VDM_Identity_Request:
				case PE_DFP_VDM_SVIDs_Request:
				case PE_DFP_VDM_Modes_Request:
				case PE_DFP_VDM_Mode_Entry_Request:
				case PE_DFP_VDM_Status_Update:
				case PE_DFP_VDM_DisplayPort_Configure:
				case PE_PRS_SRC_SNK_Send_Swap:
				case PE_DRS_Evaluate_Send_Port:
					sm5714_usbpd_set_ams_control(pd_data, STR_AMS_PRL);
					break;
				default:
					break;
				}
			}
#endif
			return ret;
		}
	}

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (data_role == USBPD_DFP)
		sm5714_usbpd_vdm_request_enabled(pd_data);

	return PE_SRC_Ready;
}

static policy_state sm5714_usbpd_policy_src_disabled(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	return PE_SRC_Disabled;
}

static policy_state sm5714_usbpd_policy_src_capability_response(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
					USBPD_Reject, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (pdic_data->pd_support)
				return PE_SRC_Ready;
			else
				return PE_SRC_Discovery;
		} else
			return PE_SRC_Hard_Reset;
	}

	return PE_SRC_Capability_Response;
}

static policy_state sm5714_usbpd_policy_src_hard_reset(
			struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		pd_data->phy_ops.hard_reset(pd_data);
		pd_data->counter.hard_reset_counter++;
		msleep(tPSHardReset);
	} else if (pdic_data->reset_done == 1) {
		pdic_data->reset_done = 0;
		return PE_SRC_Transition_to_default;
	}
	return PE_SRC_Hard_Reset;
}

static policy_state sm5714_usbpd_policy_src_hard_reset_received(
					struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	msleep(tPSHardReset);

	return PE_SRC_Transition_to_default;
}

static policy_state sm5714_usbpd_policy_src_transition_to_default(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.driver_reset(pd_data);
		sm5714_src_transition_to_default(pd_data);
		sm5714_cc_state_hold_on_off(pd_data, 0); /* CC State Hold Off */
	} else if (!sm5714_check_vbus_state(pd_data)) {
		msleep(tSrcRecover);
		if (policy->plug_valid) {
			sm5714_src_transition_to_pwr_on(pd_data);
			return PE_SRC_Startup;
		} else {
			sm5714_info("%s : cable is detached!\n", __func__);
			return 0;
		}
	}
	return PE_SRC_Transition_to_default;
}

static policy_state sm5714_usbpd_policy_src_give_source_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Source_Capabilities;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = USBPD_SOURCE;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].power_data_obj.max_current = 50;
		policy->tx_data_obj[0].power_data_obj.voltage = 100;
		policy->tx_data_obj[0].power_data_obj.peak_current = 0;
		policy->tx_data_obj[0].power_data_obj.reserved = 0;
		policy->tx_data_obj[0].power_data_obj.data_role_swap = 1;
		policy->tx_data_obj[0].power_data_obj.usb_comm_capable = 1;
		policy->tx_data_obj[0].power_data_obj.externally_powered = 0;
		policy->tx_data_obj[0].power_data_obj.usb_suspend_support = 1;
		policy->tx_data_obj[0].power_data_obj.dual_role_power = 1;
		policy->tx_data_obj[0].power_data_obj.supply = 0;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(MSG_REQUEST), tSenderResponse))
				return PE_SRC_Negotiate_Capability;
			else
				return PE_SRC_Hard_Reset;
		} else {
			return PE_SRC_Send_Soft_Reset;
		}
	}
	return PE_SRC_Give_Source_Cap;
}

static policy_state sm5714_usbpd_policy_src_give_sink_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Sink_Capabilities;
		policy->tx_msg_header.port_data_role = data_role;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].power_data_obj_sink.op_current = 3000 / 10;
		policy->tx_data_obj[0].power_data_obj_sink.voltage = 5000 / 50;
		policy->tx_data_obj[0].power_data_obj_sink.reserved = 0;
		policy->tx_data_obj[0].power_data_obj_sink.data_role_swap = 1;
		policy->tx_data_obj[0].power_data_obj_sink.usb_comm_capable = 1;
		policy->tx_data_obj[0].power_data_obj_sink.externally_powered = 0;
		policy->tx_data_obj[0].power_data_obj_sink.higher_capability = 0;
		policy->tx_data_obj[0].power_data_obj_sink.dual_role_power = 1;
		policy->tx_data_obj[0].power_data_obj_sink.supply_type =
								POWER_TYPE_FIXED;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
							policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SRC_Ready;
		else
			return PE_SRC_Send_Soft_Reset;
	}
	return PE_SRC_Give_Sink_Cap;
}

static policy_state sm5714_usbpd_policy_src_send_reject(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (policy->last_state != policy->state)
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, data_role, USBPD_SOURCE);
	else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE)
		return PE_SRC_Ready;
	return PE_SRC_Send_Reject;
}

static policy_state sm5714_usbpd_policy_src_get_sink_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Get_Sink_Cap, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (sm5714_usbpd_wait_msg(pd_data,
				BITMSG(MSG_SNK_CAP), tSenderResponse)) {
			if (pd_data->protocol_tx.status == MESSAGE_SENT) {
				dev_info(pd_data->dev, "got SinkCap.\n");
				return PE_SRC_Ready;
			} else {
				return PE_SRC_Send_Soft_Reset;
			}
		} else {
			return PE_SRC_Ready;
		}
	}
	return PE_SRC_Get_Sink_Cap;
}

static policy_state sm5714_usbpd_policy_src_wait_new_capabilities(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return PE_SRC_Send_Capabilities;
}

static policy_state sm5714_usbpd_policy_src_send_soft_reset(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_init_protocol(pd_data);
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Soft_Reset, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (sm5714_usbpd_wait_msg(pd_data,
				BITMSG(MSG_ACCEPT), tSenderResponse)) {
			if (pd_data->protocol_tx.status == MESSAGE_SENT)
				return PE_SRC_Send_Capabilities;
		} else {
			return PE_SRC_Hard_Reset;
		}
	}
	return PE_SRC_Send_Soft_Reset;
}

static policy_state sm5714_usbpd_policy_src_soft_reset(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_init_protocol(pd_data);
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SRC_Send_Capabilities;
		else
			return PE_SRC_Hard_Reset;
	}
	return PE_SRC_Soft_Reset;
}

static policy_state sm5714_usbpd_policy_src_send_source_info(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Source_Info;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = USBPD_SOURCE;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].source_info.port_type = 1;
		policy->tx_data_obj[0].source_info.reserved = 0;
		policy->tx_data_obj[0].source_info.port_maximun_pdp = 25;
		policy->tx_data_obj[0].source_info.port_present_pdp = 25;
		policy->tx_data_obj[0].source_info.port_reported_pdp = 25;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SRC_Ready;
		else
			return PE_SRC_Send_Soft_Reset;
	}
	return PE_SRC_Send_Source_Info;
}

static policy_state sm5714_usbpd_policy_snk_startup(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s\n", __func__);
#if defined(CONFIG_SEC_FACTORY)
	if (pdic_data->reset_done == 1 || policy->last_state == 0) {
#else
	if (pdic_data->reset_done == 1) {
#endif
		pdic_data->reset_done = 0;
		return PE_SNK_Discovery;
	}
	if (policy->last_state != policy->state)
		sm5714_usbpd_init_protocol(pd_data); /* prl reset */
	return PE_SNK_Startup;
}

static policy_state sm5714_usbpd_policy_snk_discovery(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	/* ST_PE_SNK_DISCOVERY */
	sm5714_cc_state_hold_on_off(pd_data, 0); /* CC State Hold Off */

	if (sm5714_check_vbus_state(pd_data) || pdic_data->pd_support) {
		sm5714_set_enable_pd_function(pd_data, PD_ENABLE);
		return PE_SNK_Wait_for_Capabilities;
	}

	return PE_SNK_Discovery;
}

static policy_state sm5714_usbpd_policy_snk_wait_for_capabilities(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
#if !defined(CONFIG_SEC_FACTORY)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif

	dev_info(pd_data->dev, "%s\n", __func__);

	/* ST_PE_SNK_WAIT_CAP */
	if (sm5714_usbpd_wait_msg(pd_data, BITMSG(MSG_SRC_CAP), tSinkWaitCap))
		return PE_SNK_Evaluate_Capability;

	if (policy->abnormal_state)
		return PE_SNK_Wait_for_Capabilities;
#if !defined(CONFIG_SEC_FACTORY)
	if (pd_data->counter.hard_reset_counter <= USBPD_nHardResetCount) {
		if (!sm5714_check_vbus_state(pd_data) && !pdic_data->pd_support) {
			sm5714_set_enable_pd_function(pd_data, PD_DISABLE);
			return PE_SNK_Discovery;
		}
		pd_data->counter.hard_reset_counter++;
		return PE_SNK_Hard_Reset;
	} else {
#if IS_ENABLED(CONFIG_SEC_DISPLAYPORT) && defined(CONFIG_SM5714_SUPPORT_SBU)
		if (pdic_data->is_1st_short && pdic_data->is_lpcharge)
			sm5714_usbpd_delayed_sbu_short_notify(pd_data);
#endif
#if defined(CONFIG_USB_LPM_CHARGING_SYNC)
		set_lpm_charging_type_done(get_otg_notify(), 1);
#endif
	}
#endif
	return PE_SNK_Wait_for_Capabilities;
}

static policy_state sm5714_usbpd_policy_snk_evaluate_capability(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	union power_supply_propval val;
	struct power_supply *psy;
#endif
	int sink_request_obj_num = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->counter.hard_reset_counter = 0;
	sm5714_cc_state_hold_on_off(pd_data, 0); /* CC State Hold Off */

	if (pd_data->pd_noti.sink_status.selected_pdo_num == 0) {
		pd_data->pd_noti.sink_status.selected_pdo_num = 1;
		if (policy->sink_cap_received) {
			policy->send_sink_cap = 1;
			policy->sink_cap_received = 0;
		}
	}
	sink_request_obj_num =
			sm5714_usbpd_evaluate_capability(pd_data);

	if (sink_request_obj_num > 0) {
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
		psy = power_supply_get_by_name("battery");
		if (pdic_data->pd_support)
		policy->source_cap_received = 1;
		if (psy) {
			if (!pdic_data->pd_support) {
				val.intval = 1;
				psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_SRCCAP, val);
			}
		} else {
			sm5714_err("%s: Fail to get psy battery\n", __func__);
		}
#endif
		return PE_SNK_Select_Capability;
	} else
		return PE_SNK_Hard_Reset;
}

static policy_state sm5714_usbpd_policy_snk_select_capability(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s, tx_status = %d\n",
			__func__, pd_data->protocol_tx.status);
	/* ST_PE_SNK_REQUEST */
	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Request;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = USBPD_SINK;
		policy->tx_msg_header.num_data_objs = 1;
		policy->tx_data_obj[0] =
				sm5714_usbpd_select_capability(pd_data);
		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		u64 msg, msg_status = 0;

		msg_status |= BITMSG(MSG_ACCEPT);
		msg_status |= BITMSG(MSG_REJECT);
		msg_status |= BITMSG(MSG_WAIT);
		msg_status |= BITMSG(MSG_GET_SNK_CAP);

		msg = sm5714_usbpd_wait_msg(pd_data, msg_status, tSenderResponse);
		if (policy->abnormal_state)
			return PE_SNK_Select_Capability;
		if (msg & BITMSG(MSG_ACCEPT))
			return PE_SNK_Transition_Sink;
		else if (msg & BITMSG(MSG_REJECT) || msg & BITMSG(MSG_WAIT)) {
			if (pd_data->pd_noti.sink_status.current_pdo_num == 0)
				return PE_SNK_Wait_for_Capabilities;
			else
				return PE_SNK_Ready;
		} else if (msg & BITMSG(MSG_GET_SNK_CAP))
			return PE_SNK_Send_Soft_Reset;
		else
			return PE_SNK_Hard_Reset;
	}
	return PE_SNK_Select_Capability;
}

static policy_state sm5714_usbpd_policy_snk_transition_sink(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
#if defined(CONFIG_SM5714_SUPPORT_SBU) || IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	union power_supply_propval val;
	struct power_supply *psy;
#endif

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_SNK_TRANSITION */
	if (policy->last_state != policy->state) {
		u64 msg, msg_status = 0;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
		psy = power_supply_get_by_name("battery");
		if (psy) {
			if (pdic_data->pd_support && policy->source_cap_received) {
				policy->source_cap_received = 0;
				val.intval = 0;
				psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_SRCCAP, val);
			}
		} else {
			sm5714_err("%s: Fail to get psy battery\n", __func__);
		}
#endif
#if defined(CONFIG_SM5714_SUPPORT_SBU)
		if (!pdic_data->pd_support)
			sm5714_short_state_check(pdic_data);
#if IS_ENABLED(CONFIG_SEC_DISPLAYPORT)
		if (pdic_data->is_lpcharge && !pdic_data->is_sbu_vbus_short &&
			!(pdic_data->is_1st_short & pdic_data->is_2nd_short))
			pdic_data->is_sbu_abnormal_state = false;
#endif
#endif
		msg_status |= BITMSG(MSG_PSRDY);
		msg_status |= BITMSG(MSG_GET_SNK_CAP);

		msg = sm5714_usbpd_wait_msg(pd_data,
				msg_status, tPSTransition);

		if (msg & BITMSG(MSG_PSRDY)) {
			dev_info(pd_data->dev, "got PS_READY.\n");
			complete(&pd_data->pd_completion);
			pd_data->pd_noti.sink_status.current_pdo_num =
					pd_data->pd_noti.sink_status.selected_pdo_num;
			return PE_SNK_Ready;
		} else
			return PE_SNK_Hard_Reset;
	}
	return PE_SNK_Select_Capability;
}

static policy_state sm5714_usbpd_policy_snk_ready(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0, i = 0;
	unsigned int ret;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (data_role == USBPD_UFP) {
		if (sm5714_usbpd_ext_request_enabled(pd_data))
			return PE_SNK_Get_Source_Cap_Ext;
	}

	sm5714_usbpd_power_ready(pd_data->dev, TYPE_C_ATTACH_SNK);

	for (i = 0; i < ARRAY_SIZE(SNK_CHECK_LIST); ++i) {
		ret = CHECK_MSG_CMD(pd_data, SNK_CHECK_LIST[i][0],
			SNK_CHECK_LIST[i][1], SNK_CHECK_LIST[i][2]);
		if (ret > 0)
			return ret;
	}

	if (data_role == USBPD_DFP)
		sm5714_usbpd_vdm_request_enabled(pd_data);

	return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_policy_snk_hard_reset(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	if (policy->abnormal_state) {
		sm5714_info("%s : cable is detached!\n", __func__);
		return 0;
	}

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		pd_data->phy_ops.hard_reset(pd_data);
		pd_data->counter.hard_reset_counter++;
	} else if (pdic_data->reset_done == 1) {
		pdic_data->reset_done = 0;
		return PE_SNK_Transition_to_default;
	}
	return PE_SNK_Hard_Reset;
}

static policy_state sm5714_usbpd_policy_snk_transition_to_default(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.driver_reset(pd_data);

	sm5714_snk_transition_to_default(pd_data);

	return PE_SNK_Startup;
}

static policy_state sm5714_usbpd_policy_snk_give_sink_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	/* TEST.PD.PHY.PORT.1 Invalid Reset Signals */
	if (policy->is_bist_test_mode)
		return PE_SNK_Ready;

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		pd_data->pd_noti.sink_status.selected_pdo_num = 0;

		policy->tx_msg_header.msg_type = USBPD_Sink_Capabilities;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = USBPD_SINK;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].power_data_obj_sink.op_current = 3000 / 10;
		policy->tx_data_obj[0].power_data_obj_sink.voltage = 5000 / 50;
		policy->tx_data_obj[0].power_data_obj_sink.reserved = 0;
		policy->tx_data_obj[0].power_data_obj_sink.data_role_swap = 1;
		policy->tx_data_obj[0].power_data_obj_sink.usb_comm_capable = 1;
		policy->tx_data_obj[0].power_data_obj_sink.externally_powered = 0;
		policy->tx_data_obj[0].power_data_obj_sink.higher_capability = 0;
		policy->tx_data_obj[0].power_data_obj_sink.dual_role_power = 1;
		policy->tx_data_obj[0].power_data_obj_sink.supply_type =
					POWER_TYPE_FIXED;

		policy->sink_cap_received = 1;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Give_Sink_Cap;
}

static policy_state sm5714_usbpd_policy_snk_get_source_cap_ext(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Get_Src_Cap_Ext, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Get_Source_Cap_Ext;
}

static policy_state sm5714_usbpd_policy_snk_get_source_status(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Get_Status, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Get_Source_Status;
}

static policy_state sm5714_usbpd_policy_snk_get_source_pps_status(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Get_PPS_Status, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Get_Source_PPS_Status;
}

static policy_state sm5714_usbpd_policy_snk_get_source_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Get_Source_Cap, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Get_Source_Cap;
}

static policy_state sm5714_usbpd_policy_snk_give_battery_status(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Battery_Status;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = USBPD_SINK;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].battery_status.batt_present_capacity = 0xffff;
		policy->tx_data_obj[0].battery_status.batt_info_reserved = 0;
		if (policy->rx_data_obj[0].object != 0x00) {
			policy->tx_data_obj[0].battery_status.batt_info_charging_status = 0;
			policy->tx_data_obj[0].battery_status.batt_info_batt_present = 0;
			policy->tx_data_obj[0].battery_status.batt_info_invalid_batt_ref = 1;
		} else {
			policy->tx_data_obj[0].battery_status.batt_info_charging_status = 2;
			policy->tx_data_obj[0].battery_status.batt_info_batt_present = 1;
			policy->tx_data_obj[0].battery_status.batt_info_invalid_batt_ref = 0;
		}
		policy->tx_data_obj[0].battery_status.reserved = 0;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Give_Battery_Status;
}

static policy_state sm5714_usbpd_policy_snk_send_soft_reset(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_init_protocol(pd_data);
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Soft_Reset, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (sm5714_usbpd_wait_msg(pd_data,
				BITMSG(MSG_ACCEPT), tSenderResponse)) {
			if (pd_data->protocol_tx.status == MESSAGE_SENT)
				return PE_SNK_Wait_for_Capabilities;
		} else {
			return PE_SNK_Hard_Reset;
		}
	}
	return PE_SNK_Send_Soft_Reset;
}

static policy_state sm5714_usbpd_policy_snk_soft_reset(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_init_protocol(pd_data);

		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Wait_for_Capabilities;
		else
			return PE_SNK_Hard_Reset;
	}
	return PE_SNK_Soft_Reset;
}

static policy_state sm5714_usbpd_policy_snk_give_source_cap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Source_Capabilities;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = USBPD_SINK;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].power_data_obj.max_current = 50;
		policy->tx_data_obj[0].power_data_obj.voltage = 100;
		policy->tx_data_obj[0].power_data_obj.peak_current = 0;
		policy->tx_data_obj[0].power_data_obj.reserved = 0;
		policy->tx_data_obj[0].power_data_obj.data_role_swap = 1;
		policy->tx_data_obj[0].power_data_obj.usb_comm_capable = 1;
		policy->tx_data_obj[0].power_data_obj.externally_powered = 0;
		policy->tx_data_obj[0].power_data_obj.usb_suspend_support = 1;
		policy->tx_data_obj[0].power_data_obj.dual_role_power = 1;
		policy->tx_data_obj[0].power_data_obj.supply = 0;


		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_SNK_Ready;
		else
			return PE_SNK_Send_Soft_Reset;
	}
	return PE_SNK_Give_Source_Cap;
}

static policy_state sm5714_usbpd_policy_snk_send_reject(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (policy->last_state != policy->state)
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, data_role, USBPD_SINK);
	else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE)
		return PE_SNK_Ready;
	return PE_SNK_Send_Reject;
}

static policy_state sm5714_usbpd_policy_drs_evaluate_port(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->modal_operation) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		if (power_role == USBPD_SOURCE)
			return PE_SRC_Hard_Reset;
		else
			return PE_SNK_Hard_Reset;
	}

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (data_role == USBPD_DFP)
		return PE_DRS_DFP_UFP_Evaluate_DR_Swap;
	else
		return PE_DRS_UFP_DFP_Evaluate_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_evaluate_send_port(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->modal_operation) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		if (power_role == USBPD_SOURCE)
			return PE_SRC_Hard_Reset;
		else
			return PE_SNK_Hard_Reset;
	}

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (data_role == USBPD_DFP)
		return PE_DRS_DFP_UFP_Send_DR_Swap;
	else
		return PE_DRS_UFP_DFP_Send_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_dfp_ufp_evaluate_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	bool drs_ok;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_EVAL */
	drs_ok = sm5714_usbpd_data_role_swap(pd_data);

	if (drs_ok)
		return PE_DRS_DFP_UFP_Accept_DR_Swap;
	else
		return PE_DRS_DFP_UFP_Reject_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_dfp_ufp_accept_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_ACCEPT */
	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_DFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_DRS_DFP_UFP_Change_to_UFP;
		else
			return PE_SRC_Send_Soft_Reset; /* Need? */
	}
	return PE_DRS_DFP_UFP_Accept_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_dfp_ufp_change_to_ufp(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
#if defined(CONFIG_TYPEC)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif
	/* ST_PE_DR_SWAP_CHG */
	pd_data->phy_ops.set_data_role(pd_data, USBPD_UFP);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
#if defined(CONFIG_TYPEC)
	/* check usbc_data->typec_try_state_change == TRY_ROLE_SWAP_TYPE */
	if (pdic_data->typec_try_state_change == TRY_ROLE_SWAP_DR &&
		pdic_data->pd_support) {
		/* Role change try and new mode detected */
		dev_info(pd_data->dev, "%s - typec_reverse_completion\n", __func__);
		pdic_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
		complete(&pdic_data->typec_reverse_completion);
	}
#endif

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Ready;
	else
		return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_policy_drs_dfp_ufp_send_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	u64 msg, msg_status = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	/* ST_PE_DR_SWAP_SEND */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_DR_Swap, USBPD_DFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			msg_status |= BITMSG(MSG_ACCEPT);
			msg_status |= BITMSG(MSG_REJECT);
			msg_status |= BITMSG(MSG_WAIT);
			msg = sm5714_usbpd_wait_msg(pd_data,
					msg_status, tSenderResponse);
			if (msg & BITMSG(MSG_ACCEPT))
				return PE_DRS_DFP_UFP_Change_to_UFP;
			if (power_role == USBPD_SOURCE)
				return PE_SRC_Ready;
			else
				return PE_SNK_Ready;
		}
	}
	return PE_DRS_DFP_UFP_Send_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_dfp_ufp_reject_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_REJECT */
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, USBPD_DFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (power_role == USBPD_SOURCE)
				return PE_SRC_Ready;
			else
				return PE_SNK_Ready;
		}
	}
	return PE_DRS_DFP_UFP_Reject_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_ufp_dfp_evaluate_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	bool drs_ok;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_EVAL */
	drs_ok = sm5714_usbpd_data_role_swap(pd_data);

	if (drs_ok && sm5714_check_vbus_state(pd_data))
		return PE_DRS_UFP_DFP_Accept_DR_Swap;
	else
		return PE_DRS_UFP_DFP_Reject_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_ufp_dfp_accept_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_ACCEPT */
	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_UFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_DRS_UFP_DFP_Change_to_DFP;
	}
	return PE_DRS_UFP_DFP_Accept_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_ufp_dfp_change_to_dfp(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
#if defined(CONFIG_TYPEC)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif
	pd_data->phy_ops.set_data_role(pd_data, USBPD_DFP);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
#if defined(CONFIG_TYPEC)
	/* check usbc_data->typec_try_state_change == TRY_ROLE_SWAP_TYPE */
	if (pdic_data->typec_try_state_change == TRY_ROLE_SWAP_DR &&
		pdic_data->pd_support) {
		/* Role change try and new mode detected */
		dev_info(pd_data->dev, "%s - typec_reverse_completion\n", __func__);
		pdic_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
		complete(&pdic_data->typec_reverse_completion);
	}
#endif

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Ready;
	else
		return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_policy_drs_ufp_dfp_send_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	/* ST_PE_DR_SWAP_SEND */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_DR_Swap, USBPD_UFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			u64 msg, msg_status = 0;

			msg_status |= BITMSG(MSG_ACCEPT);
			msg_status |= BITMSG(MSG_REJECT);
			msg_status |= BITMSG(MSG_WAIT);
			msg = sm5714_usbpd_wait_msg(pd_data,
					msg_status, tSenderResponse);
			if (msg & BITMSG(MSG_ACCEPT))
				return PE_DRS_UFP_DFP_Change_to_DFP;
			if (power_role == USBPD_SOURCE)
				return PE_SRC_Ready;
			else
				return PE_SNK_Ready;
		}
	}
	return PE_DRS_UFP_DFP_Send_DR_Swap;
}

static policy_state sm5714_usbpd_policy_drs_ufp_dfp_reject_dr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_DR_SWAP_REJECT */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			pd_data->phy_ops.get_power_role(pd_data, &power_role);
			if (power_role == USBPD_SOURCE)
				return PE_SRC_Ready;
			else
				return PE_SNK_Ready;
		}
	}
	return PE_DRS_UFP_DFP_Reject_DR_Swap;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_reject_pr_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_REJECT */
	if (policy->last_state != policy->state)
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, USBPD_DFP, USBPD_SOURCE);
	else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE)
		return PE_SRC_Ready;
	return PE_PRS_SRC_SNK_Reject_PR_Swap;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_evaluate_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	bool prs_ok;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_EVAL */
	prs_ok = sm5714_usbpd_power_role_swap(pd_data);

	if (prs_ok)
		return PE_PRS_SRC_SNK_Accept_Swap;
	else
		return PE_PRS_SRC_SNK_Reject_PR_Swap;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_send_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	/* ST_PE_PR_SWAP_SEND */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_PR_Swap, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			u64 msg, msg_status = 0;

			msg_status |= BITMSG(MSG_ACCEPT);
			msg_status |= BITMSG(MSG_REJECT);
			msg_status |= BITMSG(MSG_WAIT);
			msg = sm5714_usbpd_wait_msg(pd_data, msg_status, tSenderResponse);
			if (msg & BITMSG(MSG_ACCEPT))
				return PE_PRS_SRC_SNK_Transition_off;
			else
				return PE_SRC_Ready;
		}
	}
	return PE_PRS_SRC_SNK_Send_Swap;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_accept_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	/* ST_PE_PR_SWAP_ACCEPT */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_DFP, USBPD_SOURCE);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_PRS_SRC_SNK_Transition_off;
	}
	return PE_PRS_SRC_SNK_Accept_Swap;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_transition_to_off(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	/* ST_PE_PR_SWAP_SRC_OFF */
	if (policy->plug_valid) {
		msleep(25);
		sm5714_cc_state_hold_on_off(pd_data, 2); /* CC State Freeze On */
		sm5714_usbpd_turn_off_power_supply(pd_data);
#ifndef CONFIG_SEC_FACTORY
		sm5714_usbpd_set_rp_scr_sel(pd_data, PLUG_CTRL_RP80);
#endif
		msleep(350);
		sm5714_cc_state_hold_on_off(pd_data, 1); /* CC State Hold On */
	}
	return PE_PRS_SRC_SNK_Assert_Rd;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_assert_rd(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	pd_data->phy_ops.set_power_role(pd_data, USBPD_SINK);
	/* ST_PE_PR_SWAP_ASSERT_RD */
	return PE_PRS_SRC_SNK_Wait_Source_on;
}

static policy_state sm5714_usbpd_policy_prs_src_snk_wait_source_on(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
#if defined(CONFIG_TYPEC)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif
	/* ST_PE_PR_SWAP_WAIT_SRC_ON */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_PS_RDY, USBPD_DFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		/* need to check 'tx_done'? => tx error - error_recovery */
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(MSG_PSRDY), tPSSourceOn)) {
				pd_data->counter.swap_hard_reset_counter = 0;
				dev_info(pd_data->dev, "got PSRDY.\n");
				/* CC State Hold Off */
				sm5714_cc_state_hold_on_off(pd_data, 0);
#if defined(CONFIG_TYPEC)
				if (pdic_data->typec_try_state_change == TRY_ROLE_SWAP_PR &&
						pdic_data->pd_support) {
					pdic_data->typec_power_role = TYPEC_SINK;
					typec_set_pwr_role(pdic_data->port, pdic_data->typec_power_role);
					/* Role change try and new mode detected */
					dev_info(pd_data->dev, "%s - typec_reverse_completion\n", __func__);
					pdic_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
					complete(&pdic_data->typec_reverse_completion);
				}
#endif
				return PE_SNK_Startup;
			}
			if (policy->abnormal_state) /* Detach */
				return PE_PRS_SRC_SNK_Wait_Source_on;
			goto hard_reset; /* Timeout */
		} else
			return Error_Recovery;
	}
	return PE_PRS_SRC_SNK_Wait_Source_on;

hard_reset:
#if IS_ENABLED(CONFIG_PDIC_PD30)
	return Error_Recovery;
#else
	if (pd_data->counter.swap_hard_reset_counter > USBPD_nHardResetCount)
		return Error_Recovery;

	return PE_SNK_Hard_Reset;
#endif
}

static policy_state sm5714_usbpd_policy_prs_snk_src_reject_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_REJECT */
	if (policy->last_state != policy->state)
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Reject, USBPD_UFP, USBPD_SINK);
	else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE)
		return PE_SNK_Ready;
	return PE_PRS_SNK_SRC_Reject_Swap;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_evaluate_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	bool prs_ok;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_EVAL */
	prs_ok = sm5714_usbpd_power_role_swap(pd_data);

	if (prs_ok)
		return PE_PRS_SNK_SRC_Accept_Swap;
	else
		return PE_PRS_SNK_SRC_Reject_Swap;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_send_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_SEND */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
			USBPD_PR_Swap, USBPD_UFP, USBPD_SINK);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			u64 msg, msg_status = 0;

			msg_status |= BITMSG(MSG_ACCEPT);
			msg_status |= BITMSG(MSG_REJECT);
			msg_status |= BITMSG(MSG_WAIT);
			msg = sm5714_usbpd_wait_msg(pd_data, msg_status, tSenderResponse);
			if (msg & BITMSG(MSG_ACCEPT))
				return PE_PRS_SNK_SRC_Transition_off;
			else if (msg & BITMSG(MSG_REJECT) || msg & BITMSG(MSG_WAIT))
				return PE_SNK_Ready;
		}
	}

	return PE_PRS_SNK_SRC_Send_Swap;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_accept_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_ACCEPT */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, USBPD_UFP, USBPD_SINK);

	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT)
			return PE_PRS_SNK_SRC_Transition_off;
	}
	return PE_PRS_SNK_SRC_Accept_Swap;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_transition_to_off(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	/* ST_PE_PR_SWAP_SNK_OFF */
	if (policy->last_state != policy->state) {
		sm5714_cc_state_hold_on_off(pd_data, 1); /* CC State Hold On */
		sm5714_usbpd_turn_off_power_sink(pd_data);
		if (sm5714_usbpd_wait_msg(pd_data, BITMSG(MSG_PSRDY), tPSSourceOff)) {
			pd_data->counter.swap_hard_reset_counter = 0;
			dev_info(pd_data->dev, "got PSRDY.\n");
			return PE_PRS_SNK_SRC_Assert_Rp;
		}
	}
	if (policy->abnormal_state) /* Detach */
		return PE_PRS_SNK_SRC_Transition_off;
#if IS_ENABLED(CONFIG_PDIC_PD30)
	return Error_Recovery;
#else
	if (pd_data->counter.swap_hard_reset_counter > USBPD_nHardResetCount)
		return Error_Recovery;
	else
		return PE_SNK_Hard_Reset;
#endif
	return PE_PRS_SNK_SRC_Transition_off;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_assert_rp(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_ASSERT_RP */
	pd_data->phy_ops.set_power_role(pd_data, USBPD_SOURCE);

	return PE_PRS_SNK_SRC_Source_on;
}

static policy_state sm5714_usbpd_policy_prs_snk_src_source_on(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
#if defined(CONFIG_TYPEC)
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
#endif
	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_PR_SWAP_SRC_ON */

	if (policy->last_state != policy->state) {
		sm5714_cc_state_hold_on_off(pd_data, 0); /* CC State Hold Off */
		sm5714_usbpd_turn_on_source(pd_data);

		msleep(160);
#ifndef CONFIG_SEC_FACTORY
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			sm5714_usbpd_set_rp_scr_sel(pd_data, PLUG_CTRL_RP330);
#endif
#endif
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_PS_RDY, USBPD_UFP, USBPD_SOURCE);
#if defined(CONFIG_TYPEC)
		if (pdic_data->typec_try_state_change == TRY_ROLE_SWAP_PR &&
			pdic_data->pd_support) {
			pdic_data->typec_power_role = TYPEC_SOURCE;
			typec_set_pwr_role(pdic_data->port, pdic_data->typec_power_role);
			/* Role change try and new mode detected */
			dev_info(pd_data->dev, "%s - typec_reverse_completion\n", __func__);
			pdic_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
			complete(&pdic_data->typec_reverse_completion);
		}
#endif
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			msleep(tSwapSourceStart); /* 20ms */
			return PE_SRC_Startup;
		} else {
			return Error_Recovery;
		}
	}
	return PE_PRS_SNK_SRC_Source_on;
}

static policy_state sm5714_usbpd_policy_vcs_evaluate_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	bool vcs_ok;

	dev_info(pd_data->dev, "%s\n", __func__);
	vcs_ok = sm5714_usbpd_vconn_source_swap(pd_data);
	/* ST_PE_VC_SWAP_EVAL */
	if (vcs_ok)
		return PE_VCS_Accept_Swap;
	else
		return PE_VCS_Reject_VCONN_Swap;
}

static policy_state sm5714_usbpd_policy_vcs_accept_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int vconn_source = 0;
	int power_role = 0;
	int data_role = 0;

	pd_data->phy_ops.get_vconn_source(pd_data, &vconn_source);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	pd_data->phy_ops.get_data_role(pd_data, &data_role);
	/* ST_PE_VC_SWAP_ACCEPT */
	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_Accept, data_role, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (vconn_source)
				return PE_VCS_Wait_for_VCONN;
			else
				return PE_VCS_Turn_On_VCONN;
		}
	}
	return PE_VCS_Accept_Swap;
}

static policy_state sm5714_usbpd_policy_vcs_send_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int vconn_source = 0;
	int power_role = 0;

	pd_data->phy_ops.get_vconn_source(pd_data, &vconn_source);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	/* ST_PE_VC_SWAP_SEND */

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_VCONN_Swap, USBPD_DFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (vconn_source)
				return PE_VCS_Wait_for_VCONN;
			else
				return PE_VCS_Turn_On_VCONN;
		}
	}
	return PE_VCS_Send_Swap;
}

static policy_state sm5714_usbpd_policy_vcs_wait_for_vconn(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	/* ST_PE_VC_SWAP_WAIT */
	if (sm5714_usbpd_wait_msg(pd_data, BITMSG(MSG_PSRDY), tVCONNSourceTimeout)) {
		pd_data->counter.swap_hard_reset_counter = 0;
		dev_info(pd_data->dev, "got PSRDY.\n");
		return PE_VCS_Turn_Off_VCONN;
	}

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Hard_Reset;
	else
		return PE_SNK_Hard_Reset;
}

static policy_state sm5714_usbpd_policy_vcs_turn_off_vconn(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_VC_SWAP_OFF */
	pd_data->phy_ops.set_vconn_source(pd_data, USBPD_VCONN_OFF);

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Ready;
	else
		return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_policy_vcs_turn_on_vconn(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_VC_SWAP_ON */
	pd_data->phy_ops.set_vconn_source(pd_data, USBPD_VCONN_ON);
	msleep(tVCONNSourceOn);
	return PE_VCS_Send_PS_RDY;
}

static policy_state sm5714_usbpd_policy_vcs_send_ps_rdy(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;
	int data_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_VC_SWAP_PS_RDY */
	pd_data->phy_ops.get_power_role(pd_data, &power_role);
	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
				USBPD_PS_RDY, data_role, data_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (power_role == USBPD_SOURCE)
				return PE_SRC_Ready;
			else
				return PE_SNK_Ready;
		} else {
			/* hard reset ? soft reset? */
		}
	}
	return PE_VCS_Send_PS_RDY;
}

static policy_state sm5714_usbpd_policy_vcs_reject_vconn_swap(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	/* ST_PE_VC_SWAP_REJECT */
	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	if (policy->last_state != policy->state) {
		sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
			USBPD_Reject, USBPD_DFP, power_role);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SOURCE)
			return PE_SRC_Ready;
		else
			return PE_SNK_Ready;
	}

	return PE_VCS_Reject_VCONN_Swap;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_identity(
		struct sm5714_policy_data *policy)
{
	if (policy->rx_data_obj[0].structured_vdm.svid == PD_SID)
		return PE_UFP_VDM_Send_Identity;
	else
		return PE_UFP_VDM_Get_Identity_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_send_identity(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_msg_header.num_data_objs = 5;
		else
#endif
			policy->tx_msg_header.num_data_objs = 4;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					Discover_Identity;
		policy->tx_data_obj[1].id_header.data_capable_usb_host = 1;
		policy->tx_data_obj[1].id_header.data_capable_usb_device = 1;
		policy->tx_data_obj[1].id_header.product_type = SAMSUNG_PRODUCT_TYPE;
		policy->tx_data_obj[1].id_header.modal_operation_supported = 0;
		policy->tx_data_obj[1].id_header.product_type_dfp = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[1].id_header.connector_type = SAMSUNG_PRODUCT_TYPE;
		else
#endif
			policy->tx_data_obj[1].id_header.connector_type = 0; /* PD2.0 Reserved Bit */
		policy->tx_data_obj[1].id_header.reserved = 0;
		policy->tx_data_obj[1].id_header.usb_vendor_id = SAMSUNG_VENDOR_ID;
		policy->tx_data_obj[2].cert_stat_vdo.reserved = 0;
		policy->tx_data_obj[2].cert_stat_vdo.cert_test_id = 0;
		policy->tx_data_obj[3].product_vdo.product_id = SAMSUNG_PRODUCT_ID;
		policy->tx_data_obj[3].product_vdo.device_version = 0x0a01;

#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30) {
			policy->tx_data_obj[4].ufp_vdo.ufp_vdo_version = SAMSUNG_PRODUCT_TYPE;
			policy->tx_data_obj[4].ufp_vdo.reserved2 = 0;
			policy->tx_data_obj[4].ufp_vdo.device_capability = SAMSUNG_PRODUCT_TYPE;
			policy->tx_data_obj[4].ufp_vdo.connector_type = SAMSUNG_PRODUCT_TYPE;
			policy->tx_data_obj[4].ufp_vdo.reserved1 = 0;
			policy->tx_data_obj[4].ufp_vdo.highest_speed = 1;
		}
#endif

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Send_Identity;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_identity_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s, power_role = %d\n", __func__, power_role);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					Discover_Identity;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Get_Identity_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_svids(
		struct sm5714_policy_data *policy)
{
	if ((policy->rx_data_obj[0].structured_vdm.svid == PD_SID) &&
			!policy->skip_ufp_svid_ack)
		return PE_UFP_VDM_Send_SVIDs;
	else
		return PE_UFP_VDM_Get_SVIDs_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_send_svids(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 2;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

		policy->tx_data_obj[1].vdm_svid.svid_0 = SAMSUNG_VENDOR_ID;
		policy->tx_data_obj[1].vdm_svid.svid_1 = 0;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Send_SVIDs;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_svids_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
				Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Get_SVIDs_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_modes(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s: svid(0x%4x)\n", __func__,
		policy->rx_data_obj[0].structured_vdm.svid);
	if (policy->rx_data_obj[0].structured_vdm.svid
			== SAMSUNG_VENDOR_ID)
		return PE_UFP_VDM_Send_Modes;
	else
		return PE_UFP_VDM_Get_Modes_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_send_modes(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Send_Modes;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_get_modes_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Get_Modes_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_evaluate_mode_entry(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;

	dev_info(pd_data->dev, "%s SVID = %x\n", __func__,
				policy->rx_data_obj[0].structured_vdm.svid);
	if (policy->rx_data_obj[0].structured_vdm.svid == SAMSUNG_VENDOR_ID) {
		sm5714_pdic_event_work(pdic_data, PDIC_NOTIFY_DEV_ALL,
			PDIC_NOTIFY_ID_SVID_INFO, SAMSUNG_VENDOR_ID, 0, 0);
	}

	return PE_UFP_VDM_Mode_Entry_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_mode_entry_ack(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Mode_Entry_ACK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_mode_entry_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Mode_Entry_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_mode_exit(
		struct sm5714_policy_data *policy)
{
#if 0
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (pd_data->phy_ops.get_status(pd_data, VDM_EXIT_MODE)) {
		if (policy->rx_data_obj[0].structured_vdm.command
				== Exit_Mode) {
			unsigned int mode_pos;

			mode_pos =
				policy->rx_data_obj[0].structured_vdm.obj_pos;
			if (sm5714_usbpd_exit_mode(
					pd_data, mode_pos) == 0)
				return PE_UFP_VDM_Mode_Exit_ACK;
			else
				return PE_UFP_VDM_Mode_Exit_NAK;
		}
	}
#endif
	return PE_UFP_VDM_Mode_Exit_NAK;

}

static policy_state sm5714_usbpd_policy_ufp_vdm_mode_exit_ack(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Mode_Exit_ACK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_mode_exit_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid =
				policy->rx_data_obj[0].structured_vdm.svid;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Mode_Exit_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_attention_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.command = Attention;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Attention_Request;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_evaluate_status(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return PE_UFP_VDM_Evaluate_Status;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_status_ack(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.command =
					DisplayPort_Status_Update;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Status_ACK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_status_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					DisplayPort_Status_Update;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Status_NAK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_evaluate_configure(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return PE_UFP_VDM_Evaluate_Configure;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_configure_ack(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_ACK;
		policy->tx_data_obj[0].structured_vdm.command =
					DisplayPort_Configure;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
					policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Configure_ACK;
}

static policy_state sm5714_usbpd_policy_ufp_vdm_configure_nak(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_UFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type =
					Responder_NAK;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					DisplayPort_Configure;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (power_role == USBPD_SINK)
			return PE_SNK_Ready;
		else
			return PE_SRC_Ready;
	}
	return PE_UFP_VDM_Configure_NAK;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_get_identity(
		struct sm5714_policy_data *policy)
{
#if IS_ENABLED(CONFIG_PDIC_PD30)
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	if ((pd_data->specification_revision == USBPD_REV_30) &&
			(policy->rx_data_obj[0].structured_vdm.svid == PD_SID))
		return PE_UFP_VDM_Send_Identity;
#endif
	return PE_UFP_VDM_Get_Identity_NAK;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_identity_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);
	manager->is_sent_pin_configuration = 0;
	manager->is_samsung_accessory_enter_mode = 0;

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (pd_data->specification_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
					Discover_Identity;

		pd_data->counter.discover_identity_counter++;
		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data,
					BITMSG(VDM_DISCOVER_IDENTITY), tVDMSenderResponse)) {
				pd_data->counter.discover_identity_counter = 0;
				/* TD.PD.VDMD.E3 Incorrect Identity Fields */
				if (policy->rx_data_obj[0].structured_vdm.command_type == Initiator)
					return PE_DFP_VDM_Identity_NAKed;

				dev_info(pd_data->dev, "Msg header objs(%d)\n",
					policy->rx_msg_header.num_data_objs);
				dev_info(pd_data->dev, "VDM header type(%d)\n",
					policy->rx_data_obj[0].structured_vdm.command_type);
				dev_info(pd_data->dev, "ID Header VDO 0x%x\n",
					policy->rx_data_obj[1].object);
				dev_info(pd_data->dev, "Cert Stat VDO 0x%x\n",
					policy->rx_data_obj[2].object);
				dev_info(pd_data->dev, "Product VDO 0x%x\n",
					policy->rx_data_obj[3].object);

				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK)
					return PE_DFP_VDM_Identity_ACKed;
				else
					return PE_DFP_VDM_Identity_NAKed;
			} else {
				return PE_DFP_VDM_Identity_NAKed;
			}
		} else {
			return PE_DFP_VDM_Identity_NAKed;
		}
	}

	return PE_DFP_VDM_Identity_Request;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_response(
					struct sm5714_policy_data *policy,
					sm5714_usbpd_manager_event_type event)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	sm5714_usbpd_inform_event(pd_data, event);

	if (pd_data->phy_ops.get_power_role)
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

	if (power_role == USBPD_SINK)
		return PE_SNK_Ready;
	else
		return PE_SRC_Ready;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_identity_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_IDENTITY_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_identity_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_IDENTITY_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_svids_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.reserved2 = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_SVIDs;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_DISCOVER_SVID), tVDMSenderResponse)) {
				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK)
					return PE_DFP_VDM_SVIDs_ACKed;
				else
					return PE_DFP_VDM_SVIDs_NAKed;
			} else
				return PE_DFP_VDM_SVIDs_NAKed;
		} else
			return PE_DFP_VDM_SVIDs_NAKed;
	}
	return PE_DFP_VDM_SVIDs_Request;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_svids_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

#ifndef CONFIG_DISABLE_LOCKSCREEN_USB_RESTRICTION
	dev_info(pd_data->dev, "%s, altmode : %d\n", __func__, pd_data->altmode_enable);

	if (pd_data->altmode_enable)
		return sm5714_usbpd_policy_dfp_vdm_response(policy,
					MANAGER_DISCOVER_SVID_ACKED);
	else
		return sm5714_usbpd_policy_dfp_vdm_response(policy,
					MANAGER_DISCOVER_SVID_NAKED);
#else
	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_SVID_ACKED);
#endif
}

static policy_state sm5714_usbpd_policy_dfp_vdm_svids_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_SVID_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_modes_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = manager->SVID_0;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.obj_pos = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Discover_Modes;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,	policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_DISCOVER_MODE), tVDMSenderResponse)) {
				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK) {
					if ((policy->rx_data_obj[0].structured_vdm.svid == PD_SID_1) &&
						(manager->SVID_DP == PD_SID_1)) {
						if ((policy->rx_data_obj[1].displayport_capabilities.port_capability == UFP_D_Capable) &&
								(policy->rx_data_obj[1].displayport_capabilities.receptacle_indication == USB_TYPE_C_Receptacle)) {
							manager->pin_assignment = policy->rx_data_obj[1].displayport_capabilities.ufp_d_pin_assignments;
							dev_info(pd_data->dev, "1. support UFP_D 0x%08x\n", manager->pin_assignment);
						} else if ((policy->rx_data_obj[1].displayport_capabilities.port_capability == UFP_D_Capable) &&
								(policy->rx_data_obj[1].displayport_capabilities.receptacle_indication == USB_TYPE_C_PLUG)) {
							manager->pin_assignment = policy->rx_data_obj[1].displayport_capabilities.dfp_d_pin_assignments;
							dev_info(pd_data->dev, "2. support DFP_D 0x%08x\n", manager->pin_assignment);
						} else if (policy->rx_data_obj[1].displayport_capabilities.port_capability == DFP_D_and_UFP_D_Capable) {
							if (policy->rx_data_obj[1].displayport_capabilities.receptacle_indication == USB_TYPE_C_PLUG) {
								manager->pin_assignment = policy->rx_data_obj[1].displayport_capabilities.dfp_d_pin_assignments;
								dev_info(pd_data->dev, "3. support DFP_D 0x%08x\n", manager->pin_assignment);
							} else {
								manager->pin_assignment = policy->rx_data_obj[1].displayport_capabilities.ufp_d_pin_assignments;
								dev_info(pd_data->dev, "4. support UFP_D 0x%08x\n", manager->pin_assignment);
							}
						} else if (policy->rx_data_obj[1].displayport_capabilities.port_capability == DFP_D_Capable) {
							manager->pin_assignment = DE_SELECT_PIN;
							dev_info(pd_data->dev, "do not support Port_Capability DFP_D_Capable\n");
							return PE_DFP_VDM_Modes_NAKed;
						} else {
							manager->pin_assignment = DE_SELECT_PIN;
							dev_info(pd_data->dev, "there is not valid object information\n");
							return PE_DFP_VDM_Modes_NAKed;
						}
					}
					return PE_DFP_VDM_Modes_ACKed;
				} else
					return PE_DFP_VDM_Modes_NAKed;
			} else {
				return PE_DFP_VDM_Modes_NAKed;
			}
		} else {
			return PE_DFP_VDM_Modes_NAKed;
		}
	}
	return PE_DFP_VDM_Modes_Request;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_modes_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_MODE_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_modes_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
				MANAGER_DISCOVER_MODE_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_entry_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].object = 0;
		policy->tx_data_obj[0].structured_vdm.svid = manager->SVID_0;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		/* Todo select which_mode */
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Enter_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_ENTER_MODE), tVDMWaitModeEntry)) {
				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK)
					return PE_DFP_VDM_Mode_Entry_ACKed;
				else
					return PE_DFP_VDM_Mode_Entry_NAKed;
			} else {
				return PE_DFP_VDM_Mode_Entry_NAKed;
			}
		} else {
			return PE_DFP_VDM_Mode_Entry_NAKed;
		}
	}
	return PE_DFP_VDM_Mode_Entry_Request;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_entry_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_ENTER_MODE_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_entry_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_ENTER_MODE_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_exit_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 1;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command = Exit_Mode;

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header, policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_EXIT_MODE), tVDMWaitModeExit)) {
				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK)
					return PE_DFP_VDM_Mode_Exit_ACKed;
				else
					return PE_DFP_VDM_Mode_Exit_NAKed;
			} else {
				return PE_DFP_VDM_Mode_Exit_NAKed;
			}
		} else {
			return PE_DFP_VDM_Mode_Exit_NAKed;
		}
	}
	return PE_DFP_VDM_Mode_Exit_Request;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_exit_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_EXIT_MODE_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_exit_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_EXIT_MODE_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_attention_request(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	int hpd = 0;
	int hpdirq = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		if (manager->SVID_DP == PD_SID_1) {
			if (policy->rx_data_obj[1].displayport_status.hpd_state == 1)
				hpd = 1;
			else if (policy->rx_data_obj[1].displayport_status.hpd_state == 0)
				hpd = 0;

			if (policy->rx_data_obj[1].displayport_status.irq_hpd == 1)
				hpdirq = 2;

			sm5714_info("%s : dp_selected_pin : %d, hpd : %d, hpdirq : %d\n",
					__func__, manager->dp_selected_pin,
					hpd, hpdirq);
			sm5714_pdic_event_work(pdic_data,
					PDIC_NOTIFY_DEV_DP,
					PDIC_NOTIFY_ID_DP_HPD,
					hpd, hpdirq, 0);
			if (manager->is_sent_pin_configuration == 0)
				return PE_DFP_VDM_Status_Update_ACKed;
		}
	}

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_ATTENTION_REQUEST);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_status_update(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int hpd = 0;
	int hpdirq = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 2;

		policy->tx_data_obj[0].object = 0;
		policy->tx_data_obj[0].structured_vdm.svid = PD_SID_1;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		/* Todo select which_mode */
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
				DisplayPort_Status_Update;

		/* second object for vdo */
		policy->tx_data_obj[1].object = 0;
		policy->tx_data_obj[1].displayport_status.port_connected =
				Connect_DFP_D;
		dev_info(pd_data->dev, "%s %d\n", __func__, __LINE__);

		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_DP_STATUS_UPDATE), tVDMSenderResponse)) {
				sm5714_info("%s : command(%d), command_type(%d), obj_pos(%d), version(%d), vdm_type(%d)\n",
					__func__, policy->rx_data_obj[0].structured_vdm.command,
				policy->rx_data_obj[0].structured_vdm.command_type,
				policy->rx_data_obj[0].structured_vdm.obj_pos,
				policy->rx_data_obj[0].structured_vdm.version,
				policy->rx_data_obj[0].structured_vdm.vdm_type);

				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK) {
					if (policy->rx_data_obj[1].displayport_status.port_connected == 0x00) {
						sm5714_info("%s : port disconnected!\n", __func__);
					} else {
						if (policy->rx_data_obj[1].displayport_status.multi_function_preferred == 1) {
							if (manager->pin_assignment & PIN_ASSIGNMENT_D)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_D;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_B)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_B;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_F)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_F;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_C)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_C;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_E)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_E;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_A)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_A;
							else
								sm5714_info("%s : Wrong pin assignment value\n", __func__);
						} else {
							if (manager->pin_assignment & PIN_ASSIGNMENT_C)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_C;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_E)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_E;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_A)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_A;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_D)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_D;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_B)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_B;
							else if (manager->pin_assignment & PIN_ASSIGNMENT_F)
								manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_F;
							else
								sm5714_info("%s : Wrong pin assignment value\n", __func__);
						}
						manager->is_sent_pin_configuration = 1;
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
						if (manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_C ||
									manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_E ||
									manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_A)
							usb_restart_host_mode(pdic_data->man, 4);
						else
							usb_restart_host_mode(pdic_data->man, 2);
#endif
					}

					if (policy->rx_data_obj[1].displayport_status.hpd_state == 1)
						hpd = 1;
					else if (policy->rx_data_obj[1].displayport_status.hpd_state == 0)
						hpd = 0;

					if (policy->rx_data_obj[1].displayport_status.irq_hpd == 1)
						hpdirq = 2;

					/* Notify to DP */
					sm5714_info("%s : dp_selected_pin : %d, hpd : %d, hpdirq : %d\n",
							__func__, manager->dp_selected_pin,
							hpd, hpdirq);
					sm5714_pdic_event_work(pdic_data,
							PDIC_NOTIFY_DEV_DP,
							PDIC_NOTIFY_ID_DP_HPD,
							hpd, hpdirq, 0);
					if (policy->rx_data_obj[1].displayport_status.port_connected == 0x00)
						return PE_DFP_VDM_Status_Update_NAKed;
					else
						return PE_DFP_VDM_Status_Update_ACKed;
				} else
					return PE_DFP_VDM_Status_Update_NAKed;
			} else {
				return PE_DFP_VDM_Status_Update_NAKed;
			}
		} else {
			return PE_DFP_VDM_Status_Update_NAKed;
		}
	}
	return PE_DFP_VDM_Status_Update;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_status_update_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_STATUS_UPDATE_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_status_update_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_STATUS_UPDATE_NAKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_displayport_configure(
	struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_phydrv_data *pdic_data = pd_data->phy_driver_data;
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
	int pin_assignment = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	if (policy->last_state != policy->state) {
		pd_data->phy_ops.get_power_role(pd_data, &power_role);

		policy->tx_msg_header.msg_type = USBPD_Vendor_Defined;
		policy->tx_msg_header.port_data_role = USBPD_DFP;
		policy->tx_msg_header.port_power_role = power_role;
		policy->tx_msg_header.num_data_objs = 2;

		policy->tx_data_obj[0].structured_vdm.svid = PD_SID_1;
		policy->tx_data_obj[0].structured_vdm.vdm_type = Structured_VDM;
#if IS_ENABLED(CONFIG_PDIC_PD30)
		if (policy->rx_msg_header.spec_revision == USBPD_REV_30)
			policy->tx_data_obj[0].structured_vdm.version = 1;
		else
#endif
			policy->tx_data_obj[0].structured_vdm.version = 0;
		policy->tx_data_obj[0].structured_vdm.reserved2 = 0;
		/* Todo select which_mode */
		policy->tx_data_obj[0].structured_vdm.obj_pos = 1;
		policy->tx_data_obj[0].structured_vdm.command_type = Initiator;
		policy->tx_data_obj[0].structured_vdm.reserved1 = 0;
		policy->tx_data_obj[0].structured_vdm.command =
			DisplayPort_Configure;

		/* second object for vdo */
		policy->tx_data_obj[1].object = 0;
		policy->tx_data_obj[1].displayport_configurations.select_configuration =
				USB_U_AS_UFP_D;
		policy->tx_data_obj[1].displayport_configurations.displayport_protocol =
				DP_V_1_3;
		policy->tx_data_obj[1].displayport_configurations.rsvd1 = 0;
		policy->tx_data_obj[1].displayport_configurations.rsvd2 = 0;

		if (manager->is_sent_pin_configuration == 0) {
			if (policy->rx_data_obj[1].displayport_status.multi_function_preferred == 1) {
				if (manager->pin_assignment & PIN_ASSIGNMENT_D)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_D;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_B)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_B;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_F)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_F;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_C)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_C;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_E)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_E;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_A)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_A;
				else
					sm5714_info("%s : Wrong pin assignment value\n", __func__);
			} else {
				if (manager->pin_assignment & PIN_ASSIGNMENT_C)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_C;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_E)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_E;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_A)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_A;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_D)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_D;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_B)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_B;
				else if (manager->pin_assignment & PIN_ASSIGNMENT_F)
					manager->dp_selected_pin = PDIC_NOTIFY_DP_PIN_F;
				else
					sm5714_info("%s : Wrong pin assignment value\n", __func__);
			}
			manager->is_sent_pin_configuration = 1;
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
			if (manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_C ||
						manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_E ||
						manager->dp_selected_pin == PDIC_NOTIFY_DP_PIN_A)
				usb_restart_host_mode(pdic_data->man, 4);
			else
				usb_restart_host_mode(pdic_data->man, 2);
#endif

		}

		switch (manager->dp_selected_pin) {
		case 1:
			pin_assignment = PIN_ASSIGNMENT_A;
			break;
		case 2:
			pin_assignment = PIN_ASSIGNMENT_B;
			break;
		case 3:
			pin_assignment = PIN_ASSIGNMENT_C;
			break;
		case 4:
			pin_assignment = PIN_ASSIGNMENT_D;
			break;
		case 5:
			pin_assignment = PIN_ASSIGNMENT_E;
			break;
		case 6:
			pin_assignment = PIN_ASSIGNMENT_F;
			break;
		default:
			pin_assignment = PIN_ASSIGNMENT_D;
			break;
		}
		policy->tx_data_obj[1].displayport_configurations.ufp_u_pin_assignment =
				pin_assignment;

		/* TODO: obj_pos , vdo should be set by device manager */
		sm5714_usbpd_send_msg(pd_data, &policy->tx_msg_header,
				policy->tx_data_obj);
	} else if (pd_data->protocol_tx.status != DEFAULT_PROTOCOL_NONE) {
		if (pd_data->protocol_tx.status == MESSAGE_SENT) {
			if (sm5714_usbpd_wait_msg(pd_data, BITMSG(VDM_DP_CONFIGURE), tVDMSenderResponse)) {
				if (policy->rx_data_obj[0].structured_vdm.command_type == Responder_ACK) {
					if (manager->SVID_DP == PD_SID_1) {
						/* Notify to DP_LINK_CONFIGURE */
						sm5714_pdic_event_work(pdic_data, PDIC_NOTIFY_DEV_DP,
							PDIC_NOTIFY_ID_DP_LINK_CONF, manager->dp_selected_pin, 0, 0);
					}
					if ((policy->rx_data_obj[0].structured_vdm.svid == PD_SID_1) &&
							(manager->SVID_1 == SAMSUNG_VENDOR_ID)) {
						manager->SVID_0 = SAMSUNG_VENDOR_ID;
						sm5714_info("%s : Dex discover mode request\n", __func__);
						sm5714_usbpd_dex_vdm_request(pd_data);
					}
					return PE_DFP_VDM_DisplayPort_Configure_ACKed;
				} else
					return PE_DFP_VDM_DisplayPort_Configure_NAKed;
			} else {
				return PE_DFP_VDM_DisplayPort_Configure_NAKed;
			}
		} else {
			return PE_DFP_VDM_DisplayPort_Configure_NAKed;
		}
	}
	return PE_DFP_VDM_DisplayPort_Configure;
}

static policy_state sm5714_usbpd_policy_dfp_vdm_displayport_configure_acked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_DisplayPort_Configure_ACKED);
}

static policy_state sm5714_usbpd_policy_dfp_vdm_displayport_configure_naked(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_info(pd_data->dev, "%s\n", __func__);

	return sm5714_usbpd_policy_dfp_vdm_response(policy,
			MANAGER_DisplayPort_Configure_NACKED);
}

static policy_state sm5714_usbpd_policy_dfp_uvdm_send_message(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.set_check_msg_pass(pd_data, CHECK_MSG_PASS);

	sm5714_usbpd_send_msg(pd_data, &manager->uvdm_msg_header,
			manager->uvdm_data_obj);

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Ready;
	else
		return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_policy_dfp_uvdm_receive_message(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	sm5714_usbpd_inform_event(pd_data,
		MANAGER_UVDM_RECEIVE_MESSAGE);

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	if (power_role == USBPD_SOURCE)
		return PE_SRC_Ready;
	else
		return PE_SNK_Ready;
}

void sm5714_usbpd_policy_reset(
		struct sm5714_usbpd_data *pd_data, unsigned int flag)
{
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;

	manager->pn_flag = false;
	if (flag == HARDRESET_RECEIVED) {
		pd_data->policy.rx_hardreset = 1;
		dev_info(pd_data->dev, "%s Hard reset\n", __func__);
	} else if (flag == SOFTRESET_RECEIVED) {
		pd_data->policy.rx_softreset = 1;
		dev_info(pd_data->dev, "%s Soft reset\n", __func__);
	} else if (flag == PLUG_EVENT) {
		pd_data->policy.plug = 1;
		pd_data->policy.plug_valid = 1;
		dev_info(pd_data->dev, "%s ATTACHED\n", __func__);
	} else if (flag == PLUG_DETACHED) {
		pd_data->policy.plug_valid = 0;
		dev_info(pd_data->dev, "%s DETACHED\n", __func__);
	}
}

static policy_state sm5714_usbpd_policy_bist_carrier_m2(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int power_role = 0;

	pd_data->phy_ops.get_power_role(pd_data, &power_role);

	dev_info(pd_data->dev, "%s\n", __func__);

	sm5714_set_bist_carrier_m2(pd_data);

	if (power_role == USBPD_SINK)
		return PE_SNK_Transition_to_default;
	else
		return PE_SRC_Transition_to_default;
}

static policy_state sm5714_usbpd_policy_src_send_not_supported(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if ((policy->rx_msg_header.extended)
			&& (policy->rx_msg_ext_header.chunked)
			&& (policy->rx_msg_ext_header.data_size >= 26)) {
		msleep(tChunkingNotSupported);
	}

	sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
			USBPD_Not_Supported, data_role, USBPD_SOURCE);

	return PE_SRC_Ready;
}

static policy_state sm5714_usbpd_policy_snk_send_not_supported(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);
	int data_role = 0;

	dev_info(pd_data->dev, "%s\n", __func__);

	pd_data->phy_ops.get_data_role(pd_data, &data_role);

	if ((policy->rx_msg_header.extended)
			&& (policy->rx_msg_ext_header.chunked)
			&& (policy->rx_msg_ext_header.data_size >= 26)) {
		msleep(tChunkingNotSupported);
	}

	sm5714_usbpd_send_ctrl_msg(pd_data, &policy->tx_msg_header,
			USBPD_Not_Supported, data_role, USBPD_SINK);

	return PE_SNK_Ready;
}

static policy_state sm5714_usbpd_error_recovery(
		struct sm5714_policy_data *policy)
{
	struct sm5714_usbpd_data *pd_data = policy_to_usbpd(policy);

	dev_err(pd_data->dev, "%s\n", __func__);
	if (policy->last_state != policy->state)
		sm5714_error_recovery_mode(pd_data);
	return Error_Recovery;
}

void sm5714_usbpd_policy_work(struct work_struct *work)
{
	struct sm5714_usbpd_data *pd_data = container_of(work, struct sm5714_usbpd_data,
			worker);
	struct sm5714_policy_data *policy = &pd_data->policy;
	struct sm5714_usbpd_manager_data *manager = &pd_data->manager;
	int power_role = 0;
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
	int event = 0;
#endif
	policy_state next_state = 0;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
	union power_supply_propval val;
	struct power_supply *psy;
#endif

	dev_info(pd_data->dev, "%s Start, last_state = %x, state = %x\n",
		__func__, policy->last_state, policy->state);

	do {
		if (!policy->plug_valid) {
			sm5714_info("%s : usbpd cable is empty\n", __func__);
			break;
		}
		next_state = policy->state;

		if (policy->rx_hardreset || policy->rx_softreset
				|| policy->plug) {
			next_state = 0;
		}

		if (next_state == PE_SRC_Ready || next_state == PE_SNK_Ready)
			manager->pn_flag = true;
		else
			manager->pn_flag = false;

		dev_info(pd_data->dev, "%s last_state = %x, next_state = %x, state = %x\n",
				__func__, policy->last_state,
				next_state, policy->state);
		switch (next_state) {
		case PE_SRC_Startup:
			policy->state =
				sm5714_usbpd_policy_src_startup(policy);
			break;
		case PE_SRC_VDM_Identity_Request:
			policy->state =
				sm5714_usbpd_policy_src_vdm_identity_request(policy);
			break;
		case PE_SRC_Discovery:
			policy->state =
				sm5714_usbpd_policy_src_discovery(policy);
			break;
		case PE_SRC_Send_Capabilities:
			policy->state =
				sm5714_usbpd_policy_src_send_capabilities(
					policy);
			break;
		case PE_SRC_Negotiate_Capability:
			policy->state =
				sm5714_usbpd_policy_src_negotiate_capability(
					policy);
			break;
		case PE_SRC_Transition_Supply:
			policy->state =
				sm5714_usbpd_policy_src_transition_supply(
					policy);
			break;
		case PE_SRC_Ready:
			policy->state =
				sm5714_usbpd_policy_src_ready(policy);
			break;
		case PE_SRC_Disabled:
			policy->state =
				sm5714_usbpd_policy_src_disabled(policy);
			break;
		case PE_SRC_Capability_Response:
			policy->state =
				sm5714_usbpd_policy_src_capability_response(
					policy);
			break;
		case PE_SRC_Hard_Reset:
			policy->state =
				sm5714_usbpd_policy_src_hard_reset(policy);
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
			event = NOTIFY_EXTRA_HARDRESET_SENT;
			store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
			break;
		case PE_SRC_Hard_Reset_Received:
			policy->state =
				sm5714_usbpd_policy_src_hard_reset_received(
					policy);
#if defined(CONFIG_USB_NOTIFY_PROC_LOG)
			event = NOTIFY_EXTRA_HARDRESET_RECEIVED;
			store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
			break;
		case PE_SRC_Transition_to_default:
			policy->state =
				sm5714_usbpd_policy_src_transition_to_default(
					policy);
			break;
		case PE_SRC_Give_Source_Cap:
			policy->state =
				sm5714_usbpd_policy_src_give_source_cap(policy);
			break;
		case PE_SRC_Get_Sink_Cap:
			policy->state =
				sm5714_usbpd_policy_src_get_sink_cap(policy);
			break;
		case PE_SRC_Wait_New_Capabilities:
			policy->state =
				sm5714_usbpd_policy_src_wait_new_capabilities(
					policy);
			break;
		case PE_SRC_Give_Sink_Cap:
			policy->state =
				sm5714_usbpd_policy_src_give_sink_cap(policy);
			break;
		case PE_SRC_Send_Reject:
			policy->state =
				sm5714_usbpd_policy_src_send_reject(policy);
			break;
		case PE_SRC_Send_Soft_Reset:
			policy->state =
				sm5714_usbpd_policy_src_send_soft_reset(policy);
			break;
		case PE_SRC_Soft_Reset:
			policy->state =
				sm5714_usbpd_policy_src_soft_reset(policy);
			break;
		case PE_SRC_Send_Source_Info:
			policy->state =
				sm5714_usbpd_policy_src_send_source_info(policy);
			break;
		case PE_SNK_Startup:
			policy->state =
				sm5714_usbpd_policy_snk_startup(policy);
			break;
		case PE_SNK_Discovery:
			policy->state =
				sm5714_usbpd_policy_snk_discovery(policy);
			break;
		case PE_SNK_Wait_for_Capabilities:
			policy->state =
				sm5714_usbpd_policy_snk_wait_for_capabilities(
					policy);
			break;
		case PE_SNK_Evaluate_Capability:
			policy->state =
				sm5714_usbpd_policy_snk_evaluate_capability(
					policy);
			break;
		case PE_SNK_Select_Capability:
			policy->state =
				sm5714_usbpd_policy_snk_select_capability(
					policy);
			break;
		case PE_SNK_Transition_Sink:
			policy->state =
				sm5714_usbpd_policy_snk_transition_sink(policy);
			break;
		case PE_SNK_Ready:
			policy->state =
				sm5714_usbpd_policy_snk_ready(policy);
			break;
		case PE_SNK_Hard_Reset:
			policy->state =
				sm5714_usbpd_policy_snk_hard_reset(policy);
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
			psy = power_supply_get_by_name("battery");
			if (psy) {
				val.intval = 1;
				psy_do_property("battery", set,
					POWER_SUPPLY_EXT_PROP_HARDRESET_OCCUR, val);
			} else {
				sm5714_err("%s: Fail to get psy battery\n", __func__);
			}
#endif
			break;
		case PE_SNK_Transition_to_default:
			policy->state =
				sm5714_usbpd_policy_snk_transition_to_default(
					policy);
			break;
		case PE_SNK_Give_Sink_Cap:
			policy->state =
				sm5714_usbpd_policy_snk_give_sink_cap(policy);
			break;
		case PE_SNK_Get_Source_Cap:
			policy->state =
				sm5714_usbpd_policy_snk_get_source_cap(policy);
			break;
		case PE_SNK_Send_Soft_Reset:
			policy->state =
				sm5714_usbpd_policy_snk_send_soft_reset(policy);
			break;
		case PE_SNK_Soft_Reset:
			policy->state =
				sm5714_usbpd_policy_snk_soft_reset(policy);
			break;
		case PE_SNK_Give_Source_Cap:
			policy->state =
				sm5714_usbpd_policy_snk_give_source_cap(policy);
			break;
		case PE_SNK_Send_Reject:
			policy->state =
				sm5714_usbpd_policy_snk_send_reject(policy);
			break;
		case PE_SRC_Send_Not_Supported:
			policy->state =
				sm5714_usbpd_policy_src_send_not_supported(policy);
			break;
		case PE_SNK_Send_Not_Supported:
			policy->state =
				sm5714_usbpd_policy_snk_send_not_supported(policy);
			break;
		case PE_SNK_Get_Source_Cap_Ext:
			policy->state =
				sm5714_usbpd_policy_snk_get_source_cap_ext(policy);
			break;
		case PE_SNK_Get_Source_Status:
			policy->state =
				sm5714_usbpd_policy_snk_get_source_status(policy);
			break;
		case PE_SNK_Get_Source_PPS_Status:
			policy->state =
				sm5714_usbpd_policy_snk_get_source_pps_status(policy);
			break;
		case PE_SNK_Give_Battery_Status:
			policy->state =
				sm5714_usbpd_policy_snk_give_battery_status(policy);
			break;
		case PE_DRS_Evaluate_Port:
			policy->state =
				sm5714_usbpd_policy_drs_evaluate_port(policy);
			break;
		case PE_DRS_Evaluate_Send_Port:
			policy->state =
				sm5714_usbpd_policy_drs_evaluate_send_port(
					policy);
			break;
		case PE_DRS_DFP_UFP_Evaluate_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_dfp_ufp_evaluate_dr_swap(
					policy);
			break;
		case PE_DRS_DFP_UFP_Accept_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_dfp_ufp_accept_dr_swap(
					policy);
			break;
		case PE_DRS_DFP_UFP_Change_to_UFP:
			policy->state =
				sm5714_usbpd_policy_drs_dfp_ufp_change_to_ufp(
					policy);
			break;
		case PE_DRS_DFP_UFP_Send_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_dfp_ufp_send_dr_swap(
					policy);
			break;
		case PE_DRS_DFP_UFP_Reject_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_dfp_ufp_reject_dr_swap(
					policy);
			break;
		case PE_DRS_UFP_DFP_Evaluate_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_ufp_dfp_evaluate_dr_swap(
					policy);
			break;
		case PE_DRS_UFP_DFP_Accept_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_ufp_dfp_accept_dr_swap(
					policy);
			break;
		case PE_DRS_UFP_DFP_Change_to_DFP:
			policy->state =
				sm5714_usbpd_policy_drs_ufp_dfp_change_to_dfp(
					policy);
			break;
		case PE_DRS_UFP_DFP_Send_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_ufp_dfp_send_dr_swap(
					policy);
			break;
		case PE_DRS_UFP_DFP_Reject_DR_Swap:
			policy->state =
				sm5714_usbpd_policy_drs_ufp_dfp_reject_dr_swap(
					policy);
			break;
		case PE_PRS_SRC_SNK_Reject_PR_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_reject_pr_swap(
					policy);
			break;
		case PE_PRS_SRC_SNK_Evaluate_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_evaluate_swap(
					policy);
			break;
		case PE_PRS_SRC_SNK_Send_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_send_swap(
					policy);
			break;
		case PE_PRS_SRC_SNK_Accept_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_accept_swap(
					policy);
			break;
		case PE_PRS_SRC_SNK_Transition_off:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_transition_to_off(
					policy);
			break;
		case PE_PRS_SRC_SNK_Assert_Rd:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_assert_rd(
					policy);
			break;
		case PE_PRS_SRC_SNK_Wait_Source_on:
			policy->state =
				sm5714_usbpd_policy_prs_src_snk_wait_source_on(
					policy);
			break;
		case PE_PRS_SNK_SRC_Reject_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_reject_swap(
					policy);
			break;
		case PE_PRS_SNK_SRC_Evaluate_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_evaluate_swap(
					policy);
			break;
		case PE_PRS_SNK_SRC_Send_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_send_swap(
					policy);
			break;
		case PE_PRS_SNK_SRC_Accept_Swap:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_accept_swap(
					policy);
			break;
		case PE_PRS_SNK_SRC_Transition_off:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_transition_to_off(
					policy);
			break;
		case PE_PRS_SNK_SRC_Assert_Rp:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_assert_rp(
					policy);
			break;
		case PE_PRS_SNK_SRC_Source_on:
			policy->state =
				sm5714_usbpd_policy_prs_snk_src_source_on(
					policy);
			break;
		case PE_VCS_Evaluate_Swap:
			policy->state =
				sm5714_usbpd_policy_vcs_evaluate_swap(policy);
			break;
		case PE_VCS_Accept_Swap:
			policy->state =
				sm5714_usbpd_policy_vcs_accept_swap(policy);
			break;
		case PE_VCS_Wait_for_VCONN:
			policy->state =
				sm5714_usbpd_policy_vcs_wait_for_vconn(policy);
			break;
		case PE_VCS_Turn_Off_VCONN:
			policy->state =
				sm5714_usbpd_policy_vcs_turn_off_vconn(policy);
			break;
		case PE_VCS_Turn_On_VCONN:
			policy->state =
				sm5714_usbpd_policy_vcs_turn_on_vconn(policy);
			break;
		case PE_VCS_Send_PS_RDY:
			policy->state =
				sm5714_usbpd_policy_vcs_send_ps_rdy(policy);
			break;
		case PE_VCS_Send_Swap:
			policy->state =
				sm5714_usbpd_policy_vcs_send_swap(policy);
			break;
		case PE_VCS_Reject_VCONN_Swap:
			policy->state =
				sm5714_usbpd_policy_vcs_reject_vconn_swap(
					policy);
			break;
		case PE_UFP_VDM_Get_Identity:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_identity(
					policy);
			break;
		case PE_UFP_VDM_Send_Identity:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_send_identity(
					policy);
			break;
		case PE_UFP_VDM_Get_Identity_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_identity_nak(
					policy);
			break;
		case PE_UFP_VDM_Get_SVIDs:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_svids(policy);
			break;
		case PE_UFP_VDM_Send_SVIDs:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_send_svids(policy);
			break;
		case PE_UFP_VDM_Get_SVIDs_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_svids_nak(
					policy);
			break;
		case PE_UFP_VDM_Get_Modes:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_modes(policy);
			break;
		case PE_UFP_VDM_Send_Modes:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_send_modes(policy);
			break;
		case PE_UFP_VDM_Get_Modes_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_get_modes_nak(
					policy);
			break;
		case PE_UFP_VDM_Evaluate_Mode_Entry:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_evaluate_mode_entry(
					policy);
			break;
		case PE_UFP_VDM_Mode_Entry_ACK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_mode_entry_ack(
					policy);
			break;
		case PE_UFP_VDM_Mode_Entry_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_mode_entry_nak(
					policy);
			break;
		case PE_UFP_VDM_Mode_Exit:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_mode_exit(policy);
			break;
		case PE_UFP_VDM_Mode_Exit_ACK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_mode_exit_ack(
					policy);
			break;
		case PE_UFP_VDM_Mode_Exit_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_mode_exit_nak(
					policy);
			break;
		case PE_UFP_VDM_Attention_Request:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_attention_request(
					policy);
			break;
		case PE_UFP_VDM_Evaluate_Status:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_evaluate_status(
					policy);
			break;
		case PE_UFP_VDM_Status_ACK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_status_ack(policy);
			break;
		case PE_UFP_VDM_Status_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_status_nak(policy);
			break;
		case PE_UFP_VDM_Evaluate_Configure:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_evaluate_configure(
					policy);
			break;
		case PE_UFP_VDM_Configure_ACK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_configure_ack(
					policy);
			break;
		case PE_UFP_VDM_Configure_NAK:
			policy->state =
				sm5714_usbpd_policy_ufp_vdm_configure_nak(
					policy);
			break;
		case PE_DFP_VDM_Get_Identity:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_get_identity(
					policy);
			break;
		case PE_DFP_VDM_Identity_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_identity_request(
					policy);
			break;
		case PE_DFP_VDM_Identity_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_identity_acked(
					policy);
			break;
		case PE_DFP_VDM_Identity_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_identity_naked(
					policy);
			break;
		case PE_DFP_VDM_SVIDs_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_svids_request(
					policy);
			break;
		case PE_DFP_VDM_SVIDs_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_svids_acked(policy);
			break;
		case PE_DFP_VDM_SVIDs_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_svids_naked(policy);
			break;
		case PE_DFP_VDM_Modes_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_modes_request(
					policy);
			break;
		case PE_DFP_VDM_Modes_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_modes_acked(policy);
			break;
		case PE_DFP_VDM_Modes_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_modes_naked(policy);
			break;
		case PE_DFP_VDM_Mode_Entry_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_entry_request(
					policy);
			break;
		case PE_DFP_VDM_Mode_Entry_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_entry_acked(policy);
			break;
		case PE_DFP_VDM_Mode_Entry_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_entry_naked(policy);
			break;
		case PE_DFP_VDM_Mode_Exit_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_exit_request(
					policy);
			break;
		case PE_DFP_VDM_Mode_Exit_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_exit_acked(policy);
			break;
		case PE_DFP_VDM_Mode_Exit_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_exit_naked(policy);
			break;
		case PE_DFP_VDM_Attention_Request:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_attention_request(
					policy);
			break;
		case PE_DFP_VDM_Status_Update:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_status_update(
					policy);
			break;
		case PE_DFP_VDM_Status_Update_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_status_update_acked(
					policy);
			break;
		case PE_DFP_VDM_Status_Update_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_status_update_naked(
					policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_displayport_configure(
					policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure_ACKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_displayport_configure_acked(
					policy);
			break;
		case PE_DFP_VDM_DisplayPort_Configure_NAKed:
			policy->state =
				sm5714_usbpd_policy_dfp_vdm_displayport_configure_naked(
					policy);
			break;
		case PE_DFP_UVDM_Send_Message:
			policy->state =
				sm5714_usbpd_policy_dfp_uvdm_send_message(
					policy);
			break;
		case PE_DFP_UVDM_Receive_Message:
			policy->state =
				sm5714_usbpd_policy_dfp_uvdm_receive_message(
					policy);
			break;
		case PE_BIST_CARRIER_M2:
			policy->state = sm5714_usbpd_policy_bist_carrier_m2(policy);
			break;
		case Error_Recovery:
			policy->state = sm5714_usbpd_error_recovery(policy);
			break;
		default:
			pd_data->phy_ops.get_power_role(pd_data, &power_role);

			if (power_role == USBPD_SINK) {
				sm5714_info("%s, SINK\n", __func__);
				if (policy->rx_hardreset) {
					policy->rx_hardreset = 0;
					policy->state =
						PE_SNK_Transition_to_default;
#if IS_ENABLED(CONFIG_BATTERY_SAMSUNG)
					psy = power_supply_get_by_name("battery");
					if (psy) {
						val.intval = 0;
						psy_do_property("battery", set,
							POWER_SUPPLY_EXT_PROP_HARDRESET_OCCUR, val);
					} else {
						sm5714_err("%s: Fail to get psy battery\n", __func__);
					}
#endif
				} else if (policy->rx_softreset) {
					policy->rx_softreset = 0;
					policy->state = PE_SNK_Soft_Reset;
				} else if (policy->plug) {
					policy->plug = 0;
					policy->state = PE_SNK_Startup;
				} else {
					policy->state = PE_SNK_Startup;
				}
			} else {
				sm5714_info("%s, SOURCE\n", __func__);
				if (policy->rx_hardreset) {
					policy->rx_hardreset = 0;
					policy->state =
						PE_SRC_Hard_Reset_Received;
				} else if (policy->rx_softreset) {
					policy->rx_softreset = 0;
					policy->state = PE_SRC_Soft_Reset;
				} else if (policy->plug) {
					policy->plug = 0;
					policy->state = PE_SRC_Startup;
				} else {
					policy->state = PE_SRC_Startup;
				}
			}

			break;
		}
		policy->last_state = next_state;
	} while (policy->state != next_state);

	dev_info(pd_data->dev, "%s Finished, last_state = %x\n",
			__func__, policy->last_state);
}

void sm5714_usbpd_init_policy(struct sm5714_usbpd_data *pd_data)
{
	int i;
	struct sm5714_policy_data *policy = &pd_data->policy;

	dev_info(pd_data->dev, "%s policy state = %x\n",
			__func__, policy->state);

	policy->state = 0;
	policy->last_state = 0;
	policy->rx_hardreset = 0;
	policy->rx_softreset = 0;
	policy->plug = 0;
	policy->rx_msg_header.word = 0;
	policy->tx_msg_header.word = 0;
	policy->rx_msg_ext_header.word = 0;
	policy->modal_operation = 0;
	policy->origin_message = 0x0;
	policy->sink_cap_received = 0;
	policy->source_cap_received = 0;
	policy->send_sink_cap = 0;
	policy->skip_ufp_svid_ack = 0;
	policy->is_bist_test_mode = 0;
	pd_data->pd_noti.sink_status.current_pdo_num = 0;
	pd_data->pd_noti.sink_status.selected_pdo_num = 0;
	pd_data->pd_noti.sink_status.available_pdo_num = 0;
	for (i = 0; i < USBPD_MAX_COUNT_MSG_OBJECT; i++) {
		policy->rx_data_obj[i].object = 0;
		policy->tx_data_obj[i].object = 0;
	}
}

void sm5714_usbpd_kick_policy_work(struct device *dev)
{
	struct sm5714_usbpd_data *pd_data = dev_get_drvdata(dev);

	schedule_work(&pd_data->worker);
}
