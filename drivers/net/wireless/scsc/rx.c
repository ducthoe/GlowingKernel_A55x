/*****************************************************************************
 *
 * Copyright (c) 2012 - 2022 Samsung Electronics Co., Ltd. All rights reserved
 *
 ****************************************************************************/
#include <linux/etherdevice.h>
#include <linux/time.h>
#include <linux/interrupt.h>

#include "debug.h"
#include "dev.h"
#include "mgt.h"
#include "mlme.h"
#include "src_sink.h"
#include "const.h"
#include "ba.h"
#include "mib.h"
#include "cac.h"
#include "nl80211_vendor.h"
#include "sap.h"
#include "scsc_wifilogger_ring_wakelock_api.h"
#include "log2us.h"
#include <scsc/scsc_warn.h>

#ifdef CONFIG_SCSC_WLAN_TX_API
#include "tx_api.h"
#endif

#ifdef CONFIG_SCSC_WLAN_ANDROID
#include "scsc_wifilogger_rings.h"
#endif

#if IS_ENABLED(CONFIG_SCSC_LOG_COLLECTION)
#include <scsc/scsc_log_collector.h>
#endif
#include "tdls_manager.h"

static int slsi_freq_to_band(u32 freq)
{
	int slsi_band = SLSI_FREQ_BAND_2GHZ;

	if (freq < SLSI_5GHZ_MIN_FREQ)
		slsi_band = SLSI_FREQ_BAND_2GHZ;
	else if (freq >= SLSI_5GHZ_MIN_FREQ && freq <= SLSI_5GHZ_MAX_FREQ)
		slsi_band = SLSI_FREQ_BAND_5GHZ;
	else
		slsi_band = SLSI_FREQ_BAND_6GHZ;
	return slsi_band;
}

struct ieee80211_channel *slsi_find_scan_channel(struct slsi_dev *sdev, struct ieee80211_mgmt *mgmt, size_t mgmt_len, u16 freq)
{
	int      ielen = mgmt_len - (mgmt->u.beacon.variable - (u8 *)mgmt);
	const u8 *scan_ds = cfg80211_find_ie(WLAN_EID_DS_PARAMS, mgmt->u.beacon.variable, ielen);
	const u8 *scan_ht = cfg80211_find_ie(WLAN_EID_HT_OPERATION, mgmt->u.beacon.variable, ielen);
	u8       chan = 0;

	if (freq < SLSI_5GHZ_MIN_FREQ) {
		/* Use the DS or HT channel where possible as the Offchannel results mean the RX freq is not reliable */
		if (scan_ds)
			chan = scan_ds[2];
		else if (scan_ht)
			chan = scan_ht[2];

		if (chan)
			freq = (u16)ieee80211_channel_to_frequency(chan, NL80211_BAND_2GHZ);
	}
	if (!freq)
		return NULL;

	return ieee80211_get_channel(sdev->wiphy, freq);
}

static struct ieee80211_mgmt *slsi_rx_scan_update_ssid(struct slsi_dev *sdev, struct net_device *dev,
						       struct ieee80211_mgmt *mgmt, size_t mgmt_len, size_t *new_len,
						       u16 freq)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u8 *new_mgmt;
	size_t offset;
	const u8 *mgmt_pos;
	const u8 *ssid;
	int     i;
	int band;

	if (!SLSI_IS_VIF_INDEX_WLAN(ndev_vif))
		return NULL;

	/* update beacon, not probe response as probe response will always have actual ssid.*/
	if (!ieee80211_is_beacon(mgmt->frame_control))
		return NULL;

	ssid = cfg80211_find_ie(WLAN_EID_SSID, mgmt->u.beacon.variable,
				mgmt_len - (mgmt->u.beacon.variable - (u8 *)mgmt));
	if (!ssid) {
		SLSI_WARN(sdev, "beacon with NO SSID IE\n");
		return NULL;
	}
	/* update beacon only if hidden ssid. So, Skip if not hidden ssid*/
	if (ssid[1] > 0 && ssid[2] != '\0')
		return NULL;

	band = slsi_freq_to_band(freq);

	/* check we have a known ssid for a bss */
	for (i = 0; i < SLSI_SCAN_SSID_MAP_MAX; i++) {
		if (SLSI_ETHER_EQUAL(sdev->ssid_map[i].bssid, mgmt->bssid) && sdev->ssid_map[i].band == band) {
			new_mgmt = kmalloc(mgmt_len + 34, GFP_KERNEL);
			if (!new_mgmt) {
				SLSI_ERR_NODEV("malloc failed(len:%ld)\n", mgmt_len + 34);
				return NULL;
			}

			/* copy frame till ssid element */
			memcpy(new_mgmt, mgmt, ssid - (u8 *)mgmt);
			offset = ssid - (u8 *)mgmt;
			/* copy bss ssid into new frame */
			new_mgmt[offset++] = WLAN_EID_SSID;
			new_mgmt[offset++] = sdev->ssid_map[i].ssid_len;
			memcpy(new_mgmt + offset, sdev->ssid_map[i].ssid, sdev->ssid_map[i].ssid_len);
			offset += sdev->ssid_map[i].ssid_len;
			/* copy rest of the frame following ssid */
			mgmt_pos = ssid + ssid[1] + 2;
			memcpy(new_mgmt + offset, mgmt_pos, mgmt_len - (mgmt_pos - (u8 *)mgmt));
			offset += mgmt_len - (mgmt_pos - (u8 *)mgmt);
			*new_len = offset;

			return (struct ieee80211_mgmt *)new_mgmt;
		}
	}
	return NULL;
}

struct ieee80211_channel *slsi_rx_scan_pass_to_cfg80211(struct slsi_dev *sdev, struct net_device *dev,
							struct sk_buff *skb, bool release_skb)
{
	u16                      id = fapi_get_u16(skb, id);
	struct ieee80211_mgmt    *mgmt = fapi_get_mgmt(skb);
	size_t                   mgmt_len = fapi_get_mgmtlen(skb);
	s32                      signal;
	u16                      freq = 0;
	struct ieee80211_channel *channel;
	struct netdev_vif        *ndev_vif = netdev_priv(dev);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0))
	struct timespec64 uptime;
#else
	struct timespec uptime;
#endif
	if (id == MLME_SYNCHRONISED_IND) {
		signal = fapi_get_s16(skb, u.mlme_synchronised_ind.rssi) * 100;
		ndev_vif->sta.bss_cf = SLSI_FREQ_FW_TO_HOST(fapi_get_low16_u32(skb, u.mlme_synchronised_ind.spare_1));
		ndev_vif->sta.ch_width = fapi_get_low16_u32(skb, u.mlme_synchronised_ind.spare_2) & 0x00FF;
		if (ndev_vif->sta.ch_width == SLSI_MLME_FAPI_CHAN_WIDTH_320MHZ)
			ndev_vif->sta.ch_width = 320;
		ndev_vif->sta.primary_chan_pos = (fapi_get_low16_u32(skb, u.mlme_synchronised_ind.spare_2) & 0xFF00) >> 8;

		if (ndev_vif->sta.primary_chan_pos >= (ndev_vif->sta.ch_width / 20)) {
			SLSI_ERR_NODEV("Invalid primary chan position %d for chan_width %d\n",
						   ndev_vif->sta.primary_chan_pos, ndev_vif->sta.ch_width);
			goto do_no_calc;
		}

		if (ndev_vif->sta.ch_width == 20)
			freq = ndev_vif->sta.bss_cf;
		else if (ndev_vif->sta.ch_width == 40)
			freq = ndev_vif->sta.bss_cf + (ndev_vif->sta.primary_chan_pos * 20) - 10;
		else if (ndev_vif->sta.ch_width == 80)
			freq = ndev_vif->sta.bss_cf + (ndev_vif->sta.primary_chan_pos * 20) - 30;
		else if (ndev_vif->sta.ch_width == 160)
			freq = ndev_vif->sta.bss_cf + (ndev_vif->sta.primary_chan_pos * 20) - 70;
		else if (ndev_vif->sta.ch_width == 320)
			freq = ndev_vif->sta.bss_cf + (ndev_vif->sta.primary_chan_pos * 20) - 150;
	}
	else {
		signal = fapi_get_s16(skb, u.mlme_scan_ind.rssi) * 100;
		freq = SLSI_FREQ_FW_TO_HOST(fapi_get_u16(skb, u.mlme_scan_ind.channel_frequency));
	}
do_no_calc:
	channel = slsi_find_scan_channel(sdev, mgmt, mgmt_len, freq);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0))
	uptime = ktime_to_timespec64(ktime_get_boottime());
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
	uptime = ktime_to_timespec(ktime_get_boottime());
#else
	get_monotonic_boottime(&uptime);
#endif
	SLSI_UNUSED_PARAMETER(dev);

	/* update timestamp with device uptime in micro sec */
	mgmt->u.beacon.timestamp = (uptime.tv_sec * 1000000) + (uptime.tv_nsec / 1000);

	if (channel) {
		struct cfg80211_bss *bss;
		struct ieee80211_mgmt *mgmt_new;
		size_t mgmt_new_len = 0;

		mgmt_new = slsi_rx_scan_update_ssid(sdev, dev, mgmt, mgmt_len, &mgmt_new_len, freq);
		if (mgmt_new)
			bss = cfg80211_inform_bss_frame(sdev->wiphy, channel, mgmt_new, mgmt_new_len, signal, GFP_KERNEL);
		else
			bss = cfg80211_inform_bss_frame(sdev->wiphy, channel, mgmt, mgmt_len, signal, GFP_KERNEL);

		slsi_cfg80211_put_bss(sdev->wiphy, bss);
		kfree(mgmt_new);
	} else {
		SLSI_NET_DBG1(dev, SLSI_MLME, "No Channel info found for freq:%d\n", freq);
	}

	if (release_skb)
		kfree_skb(skb);
	return channel;
}

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0))
static int slsi_bssid_rssi_sort(void *priv, const struct list_head *a, const struct list_head *b)
{
#else
static int slsi_bssid_rssi_sort(void *priv, struct list_head *a, struct list_head *b)
{
#endif
	struct slsi_bssid_info *a_element = container_of(a, struct slsi_bssid_info, list);
	struct slsi_bssid_info *b_element = container_of(b, struct slsi_bssid_info, list);

	if ((a_element->etp > b_element->etp) ||
	    ((a_element->etp == b_element->etp) && (a_element->rssi > b_element->rssi)))
		return -1;
	return 1;
}

static void slsi_bssid_list_sort_etp_rssi(struct list_head *bssid_list)
{
	struct slsi_bssid_info *bssid_info, *tmp;

	list_for_each_entry_safe(bssid_info, tmp, bssid_list, list) {
		if (!bssid_info->rssi && !bssid_info->etp)
			list_del(&bssid_info->list);
	}

	list_sort(NULL, bssid_list, slsi_bssid_rssi_sort);
}

static int slsi_add_bssid_list(struct slsi_dev *sdev, struct ieee80211_mgmt *mgmt,
			       struct list_head *bssid_list, int current_rssi, u16 current_freq, int current_etp)
{
	struct slsi_bssid_info *current_result;

	current_result = kzalloc(sizeof(*current_result), GFP_KERNEL);
	if (!current_result) {
		SLSI_ERR(sdev, "Failed to allocate node for bssid info\n");
		return -1;
	}

	SLSI_ETHER_COPY(current_result->bssid, mgmt->bssid);
	current_result->rssi = current_rssi;
	current_result->freq = current_freq;
	current_result->etp = current_etp;
	current_result->connect_attempted = false;

	list_add_tail(&current_result->list, bssid_list);

	return 0;
}

static int slsi_update_bssid_list(struct ieee80211_mgmt *mgmt, struct list_head *bssid_list,
				  int current_rssi, u16 current_freq, s16 rssi_min, int current_etp)
{
	struct slsi_bssid_info *bssid_info, *tmp;

	list_for_each_entry_safe(bssid_info, tmp, bssid_list, list) {
		if (SLSI_ETHER_EQUAL(bssid_info->bssid, mgmt->bssid)) {
			if (current_rssi < rssi_min) {
				list_del(&bssid_info->list);
				return 0;
			}
			/*entry exists for bssid*/
			bssid_info->rssi = current_rssi;
			bssid_info->freq = current_freq;
			bssid_info->etp = current_etp;
			return 0;
		}
	}

	return 1;
}

static int slsi_populate_bssid_info(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				    struct sk_buff *skb, struct list_head *bssid_list)
{
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	int current_rssi, current_etp;
	u16 current_freq;
	s16 rssi_min = sdev->ini_conf_struct.conn_non_hint_target_min_rssi;

	current_rssi =  fapi_get_s16(skb, u.mlme_scan_ind.rssi);
	current_freq = fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency);
	current_etp = fapi_get_u16(skb,u.mlme_scan_ind.spare_1);

	if (!slsi_update_bssid_list(mgmt, bssid_list, current_rssi, current_freq, rssi_min, current_etp))
		return 0;
	if (current_rssi < rssi_min)
		return 0;

	return slsi_add_bssid_list(sdev, mgmt, bssid_list, current_rssi, current_freq, current_etp);
}

static inline void slsi_gen_new_bssid(const u8 *bssid, u8 max_bssid,
				      u8 mbssid_index, u8 *new_bssid)
{
	u64 bssid_u64 = ether_addr_to_u64(bssid);
	u64 mask = GENMASK_ULL(max_bssid - 1, 0);
	u64 new_bssid_u64;

	new_bssid_u64 = bssid_u64 & ~mask;

	new_bssid_u64 |= ((bssid_u64 & mask) + mbssid_index) & mask;

	u64_to_ether_addr(new_bssid_u64, new_bssid);
}

static int slsi_mbssid_to_ssid_list(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				    u8 *scan_ssid, int ssid_len,
				    u8 *bssid, int freq, int rssi, u8 akm_type)
{
	struct net_device *dev = slsi_get_netdev(ndev_vif->sdev, ndev_vif->ifnum);
	struct ieee80211_channel *channel = NULL;
	struct slsi_ssid_info *ssid_info;
	int found = 0;

	channel = ieee80211_get_channel(sdev->wiphy, SLSI_FREQ_FW_TO_HOST(freq));
	if (channel)
		slsi_roam_channel_cache_add_entry(sdev, dev, scan_ssid, ssid_len, bssid,
						  channel->hw_value, channel->band);

	list_for_each_entry(ssid_info, &ndev_vif->sta.ssid_info, list) {
		struct slsi_bssid_info *current_result, *bssid_info;

		if (ssid_info->ssid.ssid_len == ssid_len &&
		    memcmp(ssid_info->ssid.ssid, scan_ssid, ssid_len) == 0 &&
		    ssid_info->akm_type & akm_type) {
			found = 1;

			list_for_each_entry(bssid_info, &ssid_info->bssid_list, list) {
				if (SLSI_ETHER_EQUAL(bssid_info->bssid, bssid)) {
					/*entry exists for bssid*/
					bssid_info->rssi = rssi;
					bssid_info->freq = freq;
					return 0;
				}
			}
			current_result = kzalloc(sizeof(*current_result), GFP_KERNEL);
			if (!current_result) {
				SLSI_ERR(sdev, "Failed to allocate node for bssid info\n");
				return -1;
			}
			SLSI_ETHER_COPY(current_result->bssid, bssid);
			SLSI_DBG3_NODEV(SLSI_MLME, "BSSID Entry : %pM\n", bssid);
			current_result->rssi = rssi;
			current_result->freq = freq;
			current_result->connect_attempted = false;
			list_add_tail(&current_result->list, &ssid_info->bssid_list);
			break;
		}
	}
	if (!found) {
		struct slsi_ssid_info *ssid_info;
		struct slsi_bssid_info *current_result;

		SLSI_DBG3_NODEV(SLSI_MLME, "SSID Entry : %.*s\n", ssid_len, scan_ssid);
		ssid_info = kmalloc(sizeof(*ssid_info), GFP_KERNEL);
		if (ssid_info) {
			ssid_info->ssid.ssid_len = ssid_len;
			memcpy(ssid_info->ssid.ssid, scan_ssid, ssid_len);
			ssid_info->akm_type = akm_type;
			INIT_LIST_HEAD(&ssid_info->bssid_list);
			current_result = kzalloc(sizeof(*current_result), GFP_KERNEL);
			if (!current_result) {
				SLSI_ERR(sdev, "Failed to allocate node for bssid info\n");
				return -1;
			}
			SLSI_ETHER_COPY(current_result->bssid, bssid);
			SLSI_DBG3_NODEV(SLSI_MLME, "New BSSID Entry : %pM\n", bssid);
			current_result->rssi = rssi;
			current_result->freq = freq;
			current_result->connect_attempted = false;
			list_add_tail(&current_result->list, &ssid_info->bssid_list);
			list_add(&ssid_info->list, &ndev_vif->sta.ssid_info);
		} else {
			SLSI_ERR(sdev, "Failed to allocate node for ssid info\n");
		}
	}

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0))
static int slsi_extract_mbssids(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				const struct ieee80211_mgmt *mgmt,
				struct sk_buff *skb, u8 akm_type)
{
	u8 *transmitter_bssid;
	const u8 *probe_beacon;
	const u8 *ie;
	int ie_len;
	int current_rssi;
	u16 current_freq;
	size_t mgmt_len;
	const struct element *elem, *sub_elem;

	if (!sdev->wiphy->support_mbssid)
		return 0;

	mgmt_len = fapi_get_mgmtlen(skb);
	current_rssi = fapi_get_s16(skb, u.mlme_scan_ind.rssi);
	current_freq = fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency);

	transmitter_bssid = (u8 *)mgmt->bssid;
	if (ieee80211_is_beacon(mgmt->frame_control)) {
		probe_beacon = (u8 *)mgmt->u.beacon.variable;
		ie_len = mgmt_len - (mgmt->u.beacon.variable - (u8 *)mgmt);
	} else {
		probe_beacon = (u8 *)mgmt->u.probe_resp.variable;
		ie_len = mgmt_len - (mgmt->u.probe_resp.variable - (u8 *)mgmt);
	}
	ie = probe_beacon;

	if (sdev->wiphy->support_only_he_mbssid &&
	    !cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY, ie, ie_len))
	    return 0;

	for_each_element_id(elem, WLAN_EID_MULTIPLE_BSSID, ie, ie_len) {
		if ((elem->data - ie) + elem->datalen > ie_len) {
			SLSI_WARN(sdev, "Invalid ie length found\n");
			break;
		}

		if (elem->datalen < 4)
			continue;
		if (elem->data[0] < 1 || (int)elem->data[0] > 8)
			continue;

		SLSI_DBG1_NODEV(SLSI_MLME, "MBSSID IE Found\n");
		for_each_element(sub_elem, elem->data + 1, elem->datalen - 1) {
			u8 new_bssid[ETH_ALEN];
			const u8 *scan_ssid;
			const u8 *index;
			const u8 *ssid_ie;
			int ssid_len = 0;

			if ((sub_elem->data - (u8 *)elem) + sub_elem->datalen > elem->datalen + 2) {
				SLSI_WARN(sdev, "Invalid mbssid sub element length found\n");
				break;
			}

			if (sub_elem->id != 0 || sub_elem->datalen < 4) {
				/* not a valid BSS profile */
				continue;
			}

			if (sub_elem->data[0] != WLAN_EID_NON_TX_BSSID_CAP ||
				sub_elem->data[1] != 2) {
				/* The first element of the
				 * Nontransmitted BSSID Profile is not
				 * the Nontransmitted BSSID Capability
				 * element.
				 */
				continue;
			}

			/* found a Nontransmitted BSSID Profile */
			index = cfg80211_find_ie(WLAN_EID_MULTI_BSSID_IDX,
						 sub_elem->data, sub_elem->datalen);
			if (!index || index[1] < 1 || index[2] == 0) {
				/* Invalid MBSSID Index element */
				continue;
			}
			ssid_ie = cfg80211_find_ie(WLAN_EID_SSID, sub_elem->data, sub_elem->datalen);
			if (!ssid_ie || ssid_ie[1] >= IEEE80211_MAX_SSID_LEN)
				continue;
			ssid_len = ssid_ie[1];
			scan_ssid = &ssid_ie[2];
			slsi_gen_new_bssid(transmitter_bssid,
					   elem->data[0], index[2], new_bssid);
			slsi_mbssid_to_ssid_list(sdev, ndev_vif, (u8 *)scan_ssid, ssid_len, new_bssid, current_freq,
						 current_rssi, akm_type);
		}
	}

	return 0;
}
#else
static int slsi_extract_mbssids(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				const struct ieee80211_mgmt *mgmt,
				struct sk_buff *skb, u8 akm_type)
{
	u8 *transmitter_bssid;
	const u8 *sub_elem;
	int elen;
	const u8 *pos;
	const u8 *probe_beacon;
	const u8 *ie;
	int ie_len;
	int current_rssi;
	u16 current_freq;
	size_t mgmt_len;
	int count = 0;

	mgmt_len = fapi_get_mgmtlen(skb);
	current_rssi =  fapi_get_s16(skb, u.mlme_scan_ind.rssi);
	current_freq = fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency);

	transmitter_bssid = (u8 *)mgmt->bssid;
	if (ieee80211_is_beacon(mgmt->frame_control)) {
		probe_beacon = (u8 *)mgmt->u.beacon.variable;
		ie_len = mgmt_len - (mgmt->u.beacon.variable - (u8 *)mgmt);
	} else {
		probe_beacon = (u8 *)mgmt->u.probe_resp.variable;
		ie_len = mgmt_len - (mgmt->u.probe_resp.variable - (u8 *)mgmt);
	}
	ie = probe_beacon;

	while (ie && (ie_len > ie - probe_beacon)) {
		ie = cfg80211_find_ie(WLAN_EID_MULTIPLE_BSSID, ie, ie_len - (ie - probe_beacon));
		if (!ie)
			break;

		SLSI_DBG1_NODEV(SLSI_MLME, "MBSSID IE Found\n");
		pos = &ie[2];
		elen = ie[1];
		for (sub_elem = pos + 1; sub_elem < pos + elen - 1;
		     sub_elem += 2 + sub_elem[1]) {
			u8 sub_len = sub_elem[1];
			u8 new_bssid[ETH_ALEN];
			const u8 *scan_ssid;
			const u8 *index;
			const u8 *ssid_ie;
			int ssid_len = 0;

			count++;
			if (count > 127) {
				SLSI_INFO_NODEV("Infinite Loop\n");
				break;
			}
			if (sub_elem[0] != 0 || sub_elem[1] < 4) {
				/* not a valid BSS profile */
				continue;
			}

			if (sub_elem[2] != WLAN_EID_NON_TX_BSSID_CAP ||
			    sub_elem[3] != 2) {
				/* The first element of the
				 * Nontransmitted BSSID Profile is not
				 * the Nontransmitted BSSID Capability
				 * element.
				 */
				continue;
			}

			/* found a Nontransmitted BSSID Profile */
			index = cfg80211_find_ie(WLAN_EID_MULTI_BSSID_IDX,
						 sub_elem + 2, sub_len);
			if (!index || index[1] < 1 || index[2] == 0) {
				/* Invalid MBSSID Index element */
				continue;
			}
			ssid_ie = cfg80211_find_ie(WLAN_EID_SSID, sub_elem + 2, sub_len);
			if (!ssid_ie || ssid_ie[1] >= IEEE80211_MAX_SSID_LEN)
				continue;
			ssid_len = ssid_ie[1];
			scan_ssid = &ssid_ie[2];
			slsi_gen_new_bssid(transmitter_bssid,
					   pos[0], index[2], new_bssid);
			slsi_mbssid_to_ssid_list(sdev, ndev_vif, (u8 *)scan_ssid, ssid_len, new_bssid, current_freq,
						 current_rssi, akm_type);
		}
		ie += ie[1] + 2;
	}
	return 0;
}
#endif

static void slsi_remove_assoc_disallowed_bssid(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
					       struct slsi_scan_result *scan_result)
{
	struct slsi_ssid_info *ssid_info;

	list_for_each_entry(ssid_info, &ndev_vif->sta.ssid_info, list) {
		struct slsi_bssid_info *bssid_info, *tmp;

		if (ssid_info->ssid.ssid_len != scan_result->ssid_length ||
		    memcmp(ssid_info->ssid.ssid, &scan_result->ssid, scan_result->ssid_length) != 0 ||
		    !(ssid_info->akm_type & scan_result->akm_type))
			continue;

		list_for_each_entry_safe(bssid_info, tmp, &ssid_info->bssid_list, list) {
			if (!SLSI_ETHER_EQUAL(bssid_info->bssid, scan_result->bssid))
				continue;
			list_del(&bssid_info->list);
			kfree(bssid_info);
			break;
		}
	}
}

static int slsi_reject_ap_for_scan_info(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
					const struct ieee80211_mgmt *mgmt,
					size_t mgmt_len, struct slsi_scan_result *scan_result)
{
	const u8 *vendor_ie;
	u8 ie_length;
	bool disassoc_attr = false;

	if (ieee80211_is_beacon(mgmt->frame_control))
		vendor_ie = cfg80211_find_vendor_ie(WLAN_OUI_WFA, SLSI_WLAN_OUI_TYPE_WFA_MBO,
						    mgmt->u.beacon.variable,
						    mgmt_len - (mgmt->u.beacon.variable - (u8 *)mgmt));
	else
		vendor_ie = cfg80211_find_vendor_ie(WLAN_OUI_WFA, SLSI_WLAN_OUI_TYPE_WFA_MBO,
						    mgmt->u.probe_resp.variable,
						    mgmt_len - (mgmt->u.probe_resp.variable - (u8 *)mgmt));

	if (vendor_ie)
		ie_length = vendor_ie[1];
	else
		return 0;
	disassoc_attr = cfg80211_find_ie(SLSI_MBO_ASSOC_DISALLOWED_ATTR_ID, vendor_ie + 6, ie_length - 2);
	if (disassoc_attr) {
		slsi_remove_assoc_disallowed_bssid(sdev, ndev_vif, scan_result);
		return 1;
	}
	return 0;
}

static int slsi_populate_ssid_info(struct slsi_dev *sdev, struct netdev_vif *ndev_vif, u16 scan_id)
{
	bool found = false;
	struct sk_buff *beacon_probe_skb = NULL;
	struct ieee80211_mgmt *mgmt = NULL;
	struct slsi_scan_result *scan_result = ndev_vif->scan[scan_id].scan_results;
	struct slsi_ssid_info *ssid_info, *new_ssid_info;
	int max_count, scanresultcount = 0;

	max_count  = slsi_dev_get_scan_result_count();

	while (scan_result) {
		scanresultcount++;
		if (scanresultcount >= max_count) {
			SLSI_ERR_NODEV("Scan Result More than Max Scan Result Count!!\n");
			break;
		}
		if (scan_result->beacon) {
			beacon_probe_skb = scan_result->beacon;
		} else if (scan_result->probe_resp) {
			beacon_probe_skb = scan_result->probe_resp;
		} else {
			SLSI_ERR_NODEV("Scan entry with no beacon /probe resp!!\n");
			scan_result = scan_result->next;
			continue;
		}

		mgmt = fapi_get_mgmt(beacon_probe_skb);
		if (!scan_result->ssid_length ||
		    slsi_reject_ap_for_scan_info(sdev, ndev_vif, mgmt, fapi_get_mgmtlen(beacon_probe_skb), scan_result)) {
			scan_result = scan_result->next;
			continue;
		}
		found = false;
		list_for_each_entry(ssid_info, &ndev_vif->sta.ssid_info, list) {
			if (ssid_info->ssid.ssid_len != scan_result->ssid_length ||
			    memcmp(ssid_info->ssid.ssid, &scan_result->ssid, scan_result->ssid_length) != 0 ||
			    !(ssid_info->akm_type & scan_result->akm_type))
				continue;
			found = true;
			slsi_populate_bssid_info(sdev, ndev_vif, beacon_probe_skb,
						 &ssid_info->bssid_list);
			break;
		}
		if (found) {
			slsi_extract_mbssids(sdev, ndev_vif, mgmt, beacon_probe_skb,
					     scan_result->akm_type);
			scan_result = scan_result->next;
			continue;
		}

		new_ssid_info = kmalloc(sizeof(*new_ssid_info), GFP_ATOMIC);
		if (new_ssid_info) {
			new_ssid_info->ssid.ssid_len = scan_result->ssid_length;
			memcpy(new_ssid_info->ssid.ssid, &scan_result->ssid, scan_result->ssid_length);
			new_ssid_info->akm_type = scan_result->akm_type;
			INIT_LIST_HEAD(&new_ssid_info->bssid_list);
			slsi_populate_bssid_info(sdev, ndev_vif, beacon_probe_skb,
						 &new_ssid_info->bssid_list);
			list_add(&new_ssid_info->list, &ndev_vif->sta.ssid_info);
		} else {
			SLSI_ERR(sdev, "Failed to allocate entry : %.*s kmalloc() failed\n",
				 scan_result->ssid_length, scan_result->ssid);
		}
		slsi_extract_mbssids(sdev, ndev_vif, mgmt, beacon_probe_skb, scan_result->akm_type);
		scan_result = scan_result->next;
	}

	list_for_each_entry(ssid_info, &ndev_vif->sta.ssid_info, list)
		slsi_bssid_list_sort_etp_rssi(&ssid_info->bssid_list);

	return 0;
}
#endif

static int slsi_add_to_scan_list(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				 struct sk_buff *skb, const u8 *scan_ssid, u16 scan_id)
{
	struct slsi_scan_result *head;
	struct slsi_scan_result *scan_result, *current_result, *prev = NULL;
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	bool found = 0, skb_stored = 0;
	int current_rssi, current_band, current_etp;
	bool is_hidden = 0, ssid_matched = 0;

	SLSI_MUTEX_LOCK(ndev_vif->scan_result_mutex);
	head = ndev_vif->scan[scan_id].scan_results;
	scan_result = head;
	current_rssi =  fapi_get_s16(skb, u.mlme_scan_ind.rssi);
	current_etp = fapi_get_u16(skb, u.mlme_scan_ind.spare_1);
	current_band = slsi_freq_to_band(fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency) / 2);

	while (scan_result) {
		is_hidden = scan_result->hidden && (!scan_ssid || !scan_ssid[1] || scan_ssid[2] == '\0');
		ssid_matched = scan_ssid && scan_ssid[1] && scan_result->ssid_length &&
			       (scan_ssid[1] == scan_result->ssid_length) &&
			       !memcmp(&scan_ssid[2], scan_result->ssid, scan_ssid[1]);
		if ((SLSI_ETHER_EQUAL(scan_result->bssid, mgmt->bssid) && scan_result->band == current_band) &&
		    (is_hidden || ssid_matched)) {
			/*entry exists for bssid*/
			if (!scan_result->probe_resp && ieee80211_is_probe_resp(mgmt->frame_control)) {
				scan_result->probe_resp = skb;
				skb_stored = 1;
			} else if (!scan_result->beacon && ieee80211_is_beacon(mgmt->frame_control)) {
				scan_result->beacon = skb;
				skb_stored = 1;
				if (!scan_ssid || !scan_ssid[1] || scan_ssid[2] == '\0')
					scan_result->hidden = 1;
			}

			/* Use the best RSSI value from all beacons/probe resp for a bssid. If no improvment
			 * in RSSI and beacon and probe response exist, ignore this result
			 */
			if (current_rssi < scan_result->rssi) {
				if (!skb_stored)
					kfree_skb(skb);
				SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
				return 0;
			}

			scan_result->rssi = current_rssi;
			scan_result->etp = current_etp;
			if (!skb_stored) {
				if (ieee80211_is_beacon(mgmt->frame_control)) {
					kfree_skb(scan_result->beacon);
					scan_result->beacon = skb;
				} else {
					kfree_skb(scan_result->probe_resp);
					scan_result->probe_resp = skb;
				}
			}

			/*No change in position if rssi is still less than prev node*/
			if (!prev || prev->rssi > current_rssi) {
				SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
				return 0;
			}

			/*remove and re-insert*/
			found = 1;
			prev->next = scan_result->next;
			scan_result->next = NULL;
			current_result = scan_result;

			break;
		}

		prev = scan_result;
		scan_result = scan_result->next;
	}

	if (!found) {
		/*add_new node*/
		current_result = kzalloc(sizeof(*current_result), GFP_KERNEL);
		if (!current_result) {
			SLSI_ERR(sdev, "Failed to allocate node for scan result\n");
			SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
			return -1;
		}
		SLSI_ETHER_COPY(current_result->bssid, mgmt->bssid);

		current_result->rssi = current_rssi;
		current_result->band = current_band;
		current_result->etp = current_etp;
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
		if (ieee80211_is_beacon(mgmt->frame_control))
			current_result->akm_type = slsi_bss_connect_type_get(sdev, mgmt->u.beacon.variable,
									     fapi_get_mgmtlen(skb) - (mgmt->u.beacon.variable - (u8 *)mgmt), NULL);
		else
			current_result->akm_type = slsi_bss_connect_type_get(sdev, mgmt->u.probe_resp.variable,
									     fapi_get_mgmtlen(skb) - (mgmt->u.probe_resp.variable - (u8 *)mgmt), NULL);
#endif
		if (scan_ssid && scan_ssid[1]) {
			memcpy(current_result->ssid, &scan_ssid[2], scan_ssid[1]);
			current_result->ssid_length = scan_ssid[1];
		} else {
			current_result->ssid_length = 0;
		}
		if (ieee80211_is_beacon(mgmt->frame_control)) {
			current_result->beacon = skb;
			if (!scan_ssid || !scan_ssid[1] || scan_ssid[2] == '\0')
				current_result->hidden = 1;
		} else {
			current_result->probe_resp = skb;
		}
		current_result->next = NULL;

		if (!head) { /*first node*/
			ndev_vif->scan[scan_id].scan_results = current_result;
			SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
			return 0;
		}
	}

	scan_result = head;
	prev = NULL;
	/* insert based on rssi in descending order*/
	while (scan_result) {
		if ((current_result->rssi > scan_result->rssi)) {
			current_result->next = scan_result;
			if (prev)
				prev->next = current_result;
			else
				ndev_vif->scan[scan_id].scan_results = current_result;
			break;
		}
		prev = scan_result;
		scan_result = scan_result->next;
	}
	if (!scan_result) {
		/*insert at the end*/
		prev->next = current_result;
		current_result->next = NULL;
	}

	SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
	return 0;
}

static int slsi_add_to_p2p_scan_list(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
				     struct sk_buff *skb, u16 scan_id)
{
	struct slsi_scan_result *current_result;
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	struct slsi_scan *scan;

	/*add_new node*/
	current_result = kzalloc(sizeof(*current_result), GFP_KERNEL);
	if (!current_result) {
		SLSI_ERR(sdev, "Failed to allocate node for scan result\n");
		return -1;
	}
	SLSI_ETHER_COPY(current_result->bssid, mgmt->bssid);

	SLSI_MUTEX_LOCK(ndev_vif->scan_result_mutex);
	scan = &ndev_vif->scan[scan_id];
	if (ieee80211_is_beacon(mgmt->frame_control))
		current_result->beacon = skb;
	else
		current_result->probe_resp = skb;

	if (!scan->scan_results) {
		scan->scan_results = current_result;
		current_result->next = NULL;
	} else {
		current_result->next = scan->scan_results;
		scan->scan_results = current_result;
	}
	SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);

	return 0;
}

#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
static inline char *slsi_print_bss_sec(u8 sec_ie, u8 akm_type)
{
	switch (sec_ie) {
	case SLSI_BSS_RSN_IE:
		if (akm_type & SLSI_BSS_SECURED_SAE)
			return "WPA3-SAE";
		if (akm_type & SLSI_BSS_SECURED_PSK)
			return "WPA2-PSK";
		if (akm_type & SLSI_BSS_SECURED_1x)
			return "WPA2-DOT1X";
		return "UNKNOWN";
	case SLSI_BSS_WPA_IE:
		if (akm_type & SLSI_BSS_SECURED_SAE)
			return "WPA-SAE";
		if (akm_type & SLSI_BSS_SECURED_PSK)
			return "WPA-PSK";
		if (akm_type & SLSI_BSS_SECURED_1x)
			return "WPA-DOT1X";
		if (akm_type & SLSI_BSS_SECURED_NO)
			return "WPA";
		return "UNKNOWN";
	case SLSI_BSS_NO_IE:
		return "OPEN";
	default:
		return "UNKNOWN";
	}
}

static bool slsi_rx_6g_bss_filter(struct slsi_dev *sdev, struct sk_buff *skb, size_t ie_len)
{
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	struct ieee80211_channel *channel = NULL;
	const u8 *scan_ssid = NULL;
	const u8 *rsnx;
	u8 akm_type = 0, sec_ie = 0;

	if (sdev->wifi_safe_mode || slsi_is_rf_test_mode_enabled())
		return false;

	akm_type = slsi_bss_connect_type_get(sdev, mgmt->u.probe_resp.variable, ie_len, &sec_ie);

	/* Block security types - NONE|WEP|WPA|WPA2|WPA3-Hunting&Pecking */
	if (sec_ie == 0 || sec_ie == SLSI_BSS_NO_IE)
		goto bss_dump;
	if (sec_ie == SLSI_BSS_WPA_IE) {
		if (akm_type & (SLSI_BSS_SECURED_NO | SLSI_BSS_SECURED_1x | SLSI_BSS_SECURED_PSK |
				SLSI_BSS_SECURED_SAE))
			goto bss_dump;
	}
	if (sec_ie == SLSI_BSS_RSN_IE) {
		if (akm_type & SLSI_BSS_SECURED_SAE) {
			rsnx = cfg80211_find_ie(WLAN_EID_RSNX, mgmt->u.probe_resp.variable, ie_len);
			if (rsnx && (ie_len >= 3 + rsnx - mgmt->u.probe_resp.variable) &&
			    rsnx[1] >= 1 && (!(rsnx[2] & SLSI_RSNX_H2E)))
					goto bss_dump;
		} else if (akm_type & (SLSI_BSS_SECURED_NO | SLSI_BSS_SECURED_1x |
				       SLSI_BSS_SECURED_PSK)) {
			goto bss_dump;
		}
	}
	return false;
bss_dump:
	scan_ssid = cfg80211_find_ie(WLAN_EID_SSID, mgmt->u.probe_resp.variable, ie_len);
	channel = ieee80211_get_channel(sdev->wiphy,
					fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency) / 2);
	SLSI_ERR(sdev, "Dropping scan result due to unsupported security mode "
			"(ssid: %.*s bssid: " MACSTR " channel: %d bss_security: %s)\n",
			scan_ssid[1], &scan_ssid[2], MAC2STR(fapi_get_mgmt(skb)->bssid),
			channel->hw_value, slsi_print_bss_sec(sec_ie, akm_type));
	return true;
}
#endif

void slsi_rx_scan_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	u16               scan_id = fapi_get_u16(skb, u.mlme_scan_ind.scan_id);
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	size_t mgmt_len = fapi_get_mgmtlen(skb);
	size_t ie_len = mgmt_len - offsetof(struct ieee80211_mgmt, u.probe_resp.variable);
	const u8 *scan_ssid = NULL;
#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
	int band;
#endif

#ifdef CONFIG_SCSC_WLAN_GSCAN_ENABLE
	if (slsi_is_gscan_id(scan_id)) {
		SLSI_NET_DBG3(dev, SLSI_GSCAN, "scan_id:%#x bssid:%pM\n", scan_id, fapi_get_mgmt(skb)->bssid);
		SLSI_MUTEX_LOCK(ndev_vif->scan_mutex);
		slsi_gscan_handle_scan_result(sdev, dev, skb, scan_id, false);
		SLSI_MUTEX_UNLOCK(ndev_vif->scan_mutex);
		return;
	}
#endif

	scan_ssid = cfg80211_find_ie(WLAN_EID_SSID, mgmt->u.probe_resp.variable, ie_len);

	if (scan_ssid && scan_ssid[1] > IEEE80211_MAX_SSID_LEN) {
		SLSI_NET_ERR(dev, "Dropping scan result due to unexpected ssid length(%d)\n", scan_ssid[1]);
		kfree_skb(skb);
		return;
	}

	if (scan_ssid && scan_ssid[1] && ((ie_len - (scan_ssid - mgmt->u.probe_resp.variable) + 2) < scan_ssid[1])) {
		SLSI_NET_ERR(dev, "Dropping scan result due to skb data is less than ssid len(%d)\n", scan_ssid[1]);
		kfree_skb(skb);
		return;
	}

#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
	band = slsi_freq_to_band(fapi_get_s16(skb, u.mlme_scan_ind.channel_frequency) / 2);
	if (band == SLSI_FREQ_BAND_6GHZ &&
	    slsi_rx_6g_bss_filter(sdev, skb, ie_len)) {
		kfree_skb(skb);
		return;
	}
#endif

	if (sdev->p2p_certif && ndev_vif->iftype == NL80211_IFTYPE_P2P_CLIENT &&
	    (scan_id == (ndev_vif->ifnum << 8 | SLSI_SCAN_HW_ID))) {
		/* When supplicant receives a peer GO probe response with selected registrar set and group
		 * capability as 0, which is invalid, it is unable to store persistent network block. Hence
		 * such probe response is getting ignored here.
		 * This is mainly for an inter-op with Realtek P2P GO in P2P certification
		 */
		if (scan_ssid && scan_ssid[1] > 7) {
			const u8 *p2p_ie = NULL;

			p2p_ie = cfg80211_find_vendor_ie(WLAN_OUI_WFA, WLAN_OUI_TYPE_WFA_P2P, mgmt->u.probe_resp.variable, ie_len);
#define P2P_GROUP_CAPAB_PERSISTENT_GROUP BIT(1)
			if (p2p_ie && !(p2p_ie[10] & P2P_GROUP_CAPAB_PERSISTENT_GROUP)) {
				SLSI_NET_INFO(dev, "Ignoring a peer GO probe response with group_capab as 0\n");
				kfree_skb(skb);
				return;
			}
		}
	}

	scan_id = (scan_id & 0xFF);

	if (WLBT_WARN_ON(scan_id >= SLSI_SCAN_MAX)) {
		kfree_skb(skb);
		return;
	}

	/* Blocking scans already taken scan mutex.
	 * So scan mutex only incase of non blocking scans.
	 */
	if (!ndev_vif->scan[scan_id].is_blocking_scan)
		SLSI_MUTEX_LOCK(ndev_vif->scan_mutex);

	if (fapi_get_vif(skb) != 0 && fapi_get_u16(skb, u.mlme_scan_ind.scan_id) == 0) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "Scan indication received with ID:0, bssid:%pM\n", fapi_get_mgmt(skb)->bssid);
	} else if (ndev_vif->scan[scan_id].scan_req || ndev_vif->scan[scan_id].sched_req ||
		   ndev_vif->scan[scan_id].acs_request ||
		   ndev_vif->scan[SLSI_SCAN_HW_ID].is_blocking_scan) {
		slsi_roam_channel_cache_add(sdev, dev, skb);
		if (SLSI_IS_VIF_INDEX_WLAN(ndev_vif))
			slsi_add_to_scan_list(sdev, ndev_vif, skb, scan_ssid, scan_id);
		else
			slsi_add_to_p2p_scan_list(sdev, ndev_vif, skb, scan_id);
	}

	if (!ndev_vif->scan[scan_id].is_blocking_scan)
		SLSI_MUTEX_UNLOCK(ndev_vif->scan_mutex);
}

#if defined(CONFIG_SLSI_WLAN_STA_FWD_BEACON) && (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 10)
void slsi_rx_beacon_reporting_event_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif     *ndev_vif = netdev_priv(dev);
	u16 reason_code = fapi_get_u16(skb, u.mlme_beacon_reporting_event_ind.abort_reason) -
			  SLSI_FORWARD_BEACON_ABORT_REASON_OFFSET;
	int ret = 0;

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		kfree_skb(skb);
		return;
	}
	if (!ndev_vif->is_wips_running) {
		SLSI_ERR(sdev, "WIPS is not running. Ignore beacon_reporting_event_ind(%u)\n", reason_code);
		kfree_skb(skb);
		return;
	}

	ndev_vif->is_wips_running = false;

	if (reason_code <= SLSI_FORWARD_BEACON_ABORT_REASON_SUSPENDED)
		SLSI_INFO(sdev, "received abort_event from FW with reason(%u)\n", reason_code);
	else
		SLSI_ERR(sdev, "received abort_event unsupporting reason(%u)\n", reason_code);

	ret = slsi_send_forward_beacon_abort_vendor_event(sdev, dev, reason_code);
	if (ret)
		SLSI_ERR(sdev, "Failed to send forward_beacon_abort_event(err=%d)\n", ret);
	kfree_skb(skb);
}

void slsi_handle_wips_beacon(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb,
			     struct ieee80211_mgmt *mgmt, int mgmt_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	size_t ie_len = mgmt_len - offsetof(struct ieee80211_mgmt, u.beacon.variable);
	const u8 *ssid_ie = NULL;
	const u8 *scan_ssid = NULL;
	const u8 *scan_bssid = NULL;
	u16 beacon_int = 0;
	u64 timestamp = 0;
	int ssid_len = 0;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0))
	struct timespec64 sys_time;
#else
	struct timespec sys_time;
#endif
	int ret = 0;

	u8 channel = (ndev_vif->chan) ? (u8)(ndev_vif->chan->hw_value) : 0;

	if (!channel) {
		SLSI_ERR(sdev, "Invalid channel(0) or ndev.chan(%p)\n", ndev_vif->chan);
		return;
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0))
	sys_time = ktime_to_timespec64(ktime_get_boottime());
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
	sys_time = ktime_to_timespec(ktime_get_boottime());
#else
	get_monotonic_boottime(&sys_time);
#endif
	scan_bssid = fapi_get_mgmt(skb)->bssid;

	ssid_ie = cfg80211_find_ie(WLAN_EID_SSID, mgmt->u.beacon.variable, ie_len);
	ssid_len = ssid_ie[1];
	scan_ssid = &ssid_ie[2];
	beacon_int = mgmt->u.beacon.beacon_int;
	timestamp = mgmt->u.beacon.timestamp;

	SLSI_NET_DBG2(dev, SLSI_RX,
		      "forward_beacon from bssid:%pM beacon_int:%u timestamp:%llu system_time:%llu\n",
		      fapi_get_mgmt(skb)->bssid, beacon_int, timestamp,
		      (u64)TIMESPEC_TO_US(sys_time));

	ret = slsi_send_forward_beacon_vendor_event(sdev, dev, scan_ssid, ssid_len, scan_bssid,
						    channel, beacon_int, timestamp,  (u64)TIMESPEC_TO_US(sys_time));
	if (ret)
		SLSI_ERR(sdev, "Failed to forward beacon_event\n");
}
#endif

void slsi_rx_rcl_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif     *ndev_vif = netdev_priv(dev);
	u32                   channel_count = 0;
	u16                   channel_list[MAX_CHANNEL_COUNT] = {0};
	int                   i = 7; /* 1byte (id) + 1byte(length) + 3byte (oui) + 2byte */
	int                   ie_len = 0, sig_data_len = 0;
	u8                    *ptr;
	u16                   channel_val = 0;
	int                   ret = 0;
	__le16                *le16_ptr = NULL;

	SLSI_DBG3(sdev, SLSI_MLME, "RCL Channel List Indication received\n");
	ptr =  fapi_get_data(skb);
	sig_data_len = fapi_get_datalen(skb);
	if (sig_data_len >= 2) {
		ie_len = ptr[1];
	} else {
		SLSI_ERR(sdev, "ERR: Failed to get Fapi data\n");
		goto exit;
	}

	while (i < sig_data_len) {
		le16_ptr = (__le16 *)&ptr[i];
		channel_val = le16_to_cpu(*le16_ptr);
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 12)
		channel_list[channel_count] = channel_val / 2;
#else
		channel_list[channel_count] = ieee80211_frequency_to_channel(channel_val / 2);
		if (channel_list[channel_count] < 1 || channel_list[channel_count] > 196) {
			SLSI_ERR(sdev, "ERR: Invalid channel received %d\n", channel_list[channel_count]);
			break;
		}
#endif
		i += SLSI_SCAN_CHANNEL_DESCRIPTOR_SIZE;
		channel_count += 1;
		if (channel_count >= MAX_CHANNEL_COUNT) {
			SLSI_ERR(sdev, "ERR: Channel list received >= %d\n", MAX_CHANNEL_COUNT);
			break;
		}
		if (i >= ie_len && i < sig_data_len - 7) {
			ie_len = ptr[i + 1];
			i += 7;
		}
	}
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	ndev_vif->sta.last_connected_bss.ssid[ndev_vif->sta.last_connected_bss.ssid_len] = '\0';
	ret = slsi_send_rcl_event(sdev, channel_count, channel_list, ndev_vif->sta.last_connected_bss.ssid,
					       ndev_vif->sta.last_connected_bss.ssid_len + 1);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	if (ret)
		SLSI_ERR(sdev, "ERR: Failed to send RCL channel list\n");
exit:
	kfree_skb(skb);
}

void slsi_rx_start_detect_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	int               power_value = 0;

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	power_value = fapi_get_s16(skb, u.mlme_start_detect_ind.result);
	SLSI_DBG3(sdev, SLSI_MLME, "Start Detect Indication received with power : %d\n", power_value);
	slsi_send_power_measurement_vendor_event(sdev, power_value);

	if (slsi_mlme_del_detect_vif(sdev, dev) != 0)
		SLSI_NET_ERR(dev, "slsi_mlme_del_vif failed for detect vif\n");
	sdev->detect_vif_active = false;

	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

static void slsi_scan_update_ssid_map(struct slsi_dev *sdev, struct net_device *dev, u16 scan_id)
{
	struct netdev_vif     *ndev_vif = netdev_priv(dev);
	struct ieee80211_mgmt *mgmt;
	const u8              *ssid_ie = NULL, *connected_ssid = NULL;
	int                   i, found = 0, is_connected = 0;
	struct slsi_scan_result	*scan_result = NULL;
	int band;

	WLBT_WARN_ON(!SLSI_MUTEX_IS_LOCKED(ndev_vif->vif_mutex));
	WLBT_WARN_ON(!SLSI_MUTEX_IS_LOCKED(ndev_vif->scan_result_mutex));

	if (ndev_vif->activated && ndev_vif->vif_type == FAPI_VIFTYPE_STATION && ndev_vif->sta.sta_bss) {
		band = slsi_freq_to_band(ndev_vif->sta.sta_bss->channel->center_freq);
		is_connected = 1;
		connected_ssid = cfg80211_find_ie(WLAN_EID_SSID, ndev_vif->sta.sta_bss->ies->data, ndev_vif->sta.sta_bss->ies->len);
	}

	/* sanitize map: [remove any old entries] */
	for (i = 0; i < SLSI_SCAN_SSID_MAP_MAX; i++) {
		found = 0;
		if (!sdev->ssid_map[i].ssid_len)
			continue;

		/* We are connected to this hidden AP. So no need to check if this AP is present in scan results */
		if (is_connected && SLSI_ETHER_EQUAL(ndev_vif->sta.sta_bss->bssid, sdev->ssid_map[i].bssid) &&
		    sdev->ssid_map[i].band == band)
			continue;

		/* If this entry AP is found to be non-hidden, remove entry. */
		scan_result = ndev_vif->scan[scan_id].scan_results;
		while (scan_result) {
			if (SLSI_ETHER_EQUAL(sdev->ssid_map[i].bssid, scan_result->bssid) &&
			    sdev->ssid_map[i].band == scan_result->band) {
				/* AP is no more hidden. OR AP is hidden but did not
				 * receive probe resp. Go for expiry.
				 */
				if (!scan_result->hidden || (scan_result->hidden && !scan_result->probe_resp))
					sdev->ssid_map[i].age = SLSI_SCAN_SSID_MAP_EXPIRY_AGE;
				else
					found = 1;
				break;
			}
			scan_result = scan_result->next;
		}

		if (!found) {
			sdev->ssid_map[i].age++;
			if (sdev->ssid_map[i].age > SLSI_SCAN_SSID_MAP_EXPIRY_AGE) {
				sdev->ssid_map[i].ssid_len = 0;
				sdev->ssid_map[i].age = 0;
			}
		}
	}

	scan_result = ndev_vif->scan[scan_id].scan_results;
	/* update/add hidden bss with known ssid */
	while (scan_result) {
		ssid_ie = NULL;

		if (scan_result->hidden) {
			if (is_connected && SLSI_ETHER_EQUAL(ndev_vif->sta.sta_bss->bssid, scan_result->bssid) &&
			    scan_result->band == band) {
				ssid_ie = connected_ssid;
			} else if (scan_result->probe_resp) {
				mgmt = fapi_get_mgmt(scan_result->probe_resp);
				ssid_ie = cfg80211_find_ie(WLAN_EID_SSID, mgmt->u.beacon.variable, fapi_get_mgmtlen(scan_result->probe_resp) - (mgmt->u.beacon.variable - (u8 *)mgmt));
			}
		}

		if (!ssid_ie) {
			scan_result = scan_result->next;
			continue;
		}

		found = 0;
		/* if this bss is in map, update map */
		for (i = 0; i < SLSI_SCAN_SSID_MAP_MAX; i++) {
			if (!sdev->ssid_map[i].ssid_len)
				continue;
			if (SLSI_ETHER_EQUAL(scan_result->bssid, sdev->ssid_map[i].bssid) &&
			    scan_result->band == sdev->ssid_map[i].band) {
				sdev->ssid_map[i].ssid_len = ssid_ie[1];
				memcpy(sdev->ssid_map[i].ssid, &ssid_ie[2], ssid_ie[1]);
				found = 1;
				break;
			}
		}
		if (!found) {
			/* add a new entry in map */
			for (i = 0; i < SLSI_SCAN_SSID_MAP_MAX; i++) {
				if (sdev->ssid_map[i].ssid_len)
					continue;
				SLSI_ETHER_COPY(sdev->ssid_map[i].bssid, scan_result->bssid);
				sdev->ssid_map[i].age = 0;
				sdev->ssid_map[i].ssid_len = ssid_ie[1];
				sdev->ssid_map[i].band = scan_result->band;
				memcpy(sdev->ssid_map[i].ssid, &ssid_ie[2], ssid_ie[1]);
				break;
			}
		}
		scan_result = scan_result->next;
	}
}

void slsi_scan_complete(struct slsi_dev *sdev, struct net_device *dev, u16 scan_id, bool aborted,
			bool flush_scan_results)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct sk_buff    *scan;
	int count = 0;
	int *result_count = NULL, max_count = 0;
	struct cfg80211_scan_info info = {.aborted = aborted};
	int scan_results_count = 0;
	int more_than_max_count = 0;
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
	struct slsi_ssid_info *ssid_info, *ssid_tmp;
	struct slsi_bssid_blacklist_info *blacklist_info, *blacklist_tmp;
#endif

	if (WLBT_WARN_ON(scan_id >= SLSI_SCAN_MAX))
		return;

	if (scan_id == SLSI_SCAN_HW_ID && !ndev_vif->scan[scan_id].scan_req)
		return;

	if (WLBT_WARN_ON(scan_id == SLSI_SCAN_SCHED_ID && !ndev_vif->scan[scan_id].sched_req))
		return;

	SLSI_MUTEX_LOCK(ndev_vif->scan_result_mutex);
	if (SLSI_IS_VIF_INDEX_WLAN(ndev_vif)) {
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
		if (flush_scan_results) {
			list_for_each_entry_safe(ssid_info, ssid_tmp, &ndev_vif->sta.ssid_info, list) {
				struct slsi_bssid_info *bssid_info, *bssid_tmp;

				list_for_each_entry_safe(bssid_info, bssid_tmp, &ssid_info->bssid_list, list) {
					list_del(&bssid_info->list);
					kfree(bssid_info);
				}
				list_del(&ssid_info->list);
				kfree(ssid_info);
			}
			INIT_LIST_HEAD(&ndev_vif->sta.ssid_info);
		}
		list_for_each_entry_safe(blacklist_info, blacklist_tmp, &ndev_vif->sta.blacklist_head, list) {
			if (blacklist_info && (jiffies_to_msecs(jiffies) > blacklist_info->end_time)) {
				list_del(&blacklist_info->list);
				kfree(blacklist_info);
			}
		}
		slsi_populate_ssid_info(sdev, ndev_vif, scan_id);
#endif
		slsi_scan_update_ssid_map(sdev, dev, scan_id);
		max_count  = slsi_dev_get_scan_result_count();
	}

	result_count = &count;
	scan = slsi_dequeue_cached_scan_result(&ndev_vif->scan[scan_id], result_count);
	while (scan) {
		scan_results_count++;
		/* skb freed inside slsi_rx_scan_pass_to_cfg80211 */
		slsi_rx_scan_pass_to_cfg80211(sdev, dev, scan, true);

		if ((SLSI_IS_VIF_INDEX_WLAN(ndev_vif)) && (*result_count >= max_count)) {
			more_than_max_count = 1;
			slsi_purge_scan_results_locked(ndev_vif, scan_id);
			break;
		}
		scan = slsi_dequeue_cached_scan_result(&ndev_vif->scan[scan_id], result_count);
	}
	SLSI_INFO(sdev, "Scan count:%d APs\n", scan_results_count);
	SLSI_NET_DBG3(dev, SLSI_MLME, "interface:%d, scan_id:%d,%s\n", ndev_vif->ifnum, scan_id,
		      more_than_max_count ? "Scan results overflow" : "");
	slsi_roam_channel_cache_prune(dev, SLSI_ROAMING_CHANNEL_CACHE_TIMEOUT, NULL);

	if (scan_id == SLSI_SCAN_HW_ID) {
		if (SLSI_IS_VIF_INDEX_P2P(ndev_vif) && (!SLSI_IS_P2P_GROUP_STATE(sdev))) {
			/* Check for unsync vif as it could be present during the cycle of social channel
			 * scan and listen
			 */
			if (ndev_vif->activated)
				SLSI_P2P_STATE_CHANGE(sdev, P2P_IDLE_VIF_ACTIVE);
			else
				SLSI_P2P_STATE_CHANGE(sdev, P2P_IDLE_NO_VIF);
		}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
		cfg80211_scan_done(ndev_vif->scan[scan_id].scan_req, &info);
#else
		cfg80211_scan_done(ndev_vif->scan[scan_id].scan_req, aborted);
#endif

		ndev_vif->scan[scan_id].scan_req = NULL;
		ndev_vif->scan[scan_id].requeue_timeout_work = false;
	}

	if (scan_id == SLSI_SCAN_SCHED_ID && scan_results_count > 0)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0))
		cfg80211_sched_scan_results(sdev->wiphy, ndev_vif->scan[scan_id].sched_req->reqid);
#else
		cfg80211_sched_scan_results(sdev->wiphy);
#endif
	SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
}

int slsi_set_2g_auto_channel(struct slsi_dev *sdev, struct netdev_vif  *ndev_vif,
			     struct slsi_acs_selected_channels *acs_selected_channels,
			     struct slsi_acs_chan_info *ch_info)
{
	int i = 0, j = 0, avg_load, total_num_ap, total_rssi, adjacent_rssi;
	bool all_bss_load = true;
	int  min_avg_chan_utilization = INT_MAX, min_adjacent_rssi = INT_MAX;
	int ch_idx_min_load = 0, ch_idx_min_rssi = 0;
	int min_avg_chan_utilization_20 = INT_MAX, min_adjacent_rssi_20 = INT_MAX;
	int ch_idx_min_load_20 = 0, ch_idx_min_rssi_20 = 0;
	int ret = 0;
	int ch_list_len = SLSI_NUM_2P4GHZ_CHANNELS;

	acs_selected_channels->ch_width = (sdev->fw_SoftAp_2g_40mhz_enabled &&
					  ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->ch_width >= 40) ? 40 : 20;
	acs_selected_channels->hw_mode = ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode;

	SLSI_DBG3(sdev, SLSI_MLME, "ch_lis_len:%d\n", ch_list_len);
	for (i = 0; i < ch_list_len; i++) {
		if (!ch_info[i].chan)
			continue;
		adjacent_rssi = 0;   /* Assuming ch_list is in sorted order. */
		for (j = -2; j <= 2; j++)
			if (i + j >= 0 && i + j < ch_list_len)
				adjacent_rssi += ch_info[i + j].rssi_factor;
		ch_info[i].adj_rssi_factor = adjacent_rssi;
		if (ch_info[i].num_bss_load_ap != 0) {
			ch_info[i].avg_chan_utilization = ch_info[i].total_chan_utilization /
							  ch_info[i].num_bss_load_ap;
			if (ch_info[i].avg_chan_utilization < min_avg_chan_utilization_20) {
				min_avg_chan_utilization_20 = ch_info[i].avg_chan_utilization;
				ch_idx_min_load_20 = i;
			} else if (ch_info[i].avg_chan_utilization == min_avg_chan_utilization_20 &&
				   ch_info[i].num_ap < ch_info[ch_idx_min_load_20].num_ap) {
				ch_idx_min_load_20 = i;
			}
		} else {
			SLSI_DBG3(sdev, SLSI_MLME, "BSS load IE not found\n");
			all_bss_load = false;
		}
		if (adjacent_rssi < min_adjacent_rssi_20) {
			min_adjacent_rssi_20 = adjacent_rssi;
			ch_idx_min_rssi_20 = i;
		} else if (adjacent_rssi == min_adjacent_rssi_20 &&
			   ch_info[i].num_ap < ch_info[ch_idx_min_rssi_20].num_ap) {
			ch_idx_min_rssi_20 = i;
		}
		SLSI_DBG3(sdev, SLSI_MLME, "min rssi:%d min_rssi_idx:%d\n", min_adjacent_rssi_20, ch_idx_min_rssi_20);
		SLSI_DBG3(sdev, SLSI_MLME, "num_ap:%d,chan:%d,total_util:%d,avg_util:%d,rssi_fac:%d,adj_rssi_fac:%d,"
			  "bss_ap:%d\n", ch_info[i].num_ap, ch_info[i].chan, ch_info[i].total_chan_utilization,
			  ch_info[i].avg_chan_utilization, ch_info[i].rssi_factor, ch_info[i].adj_rssi_factor,
			  ch_info[i].num_bss_load_ap);
	}

	if (acs_selected_channels->ch_width == 40) {
		for (i = 0; i < ch_list_len; i++) {
			if (i + 4 >= ch_list_len || !ch_info[i + 4].chan || !ch_info[i].chan)
				continue;
			avg_load = ch_info[i].avg_chan_utilization + ch_info[i + 4].avg_chan_utilization;
			total_num_ap = ch_info[i].num_ap + ch_info[i + 4].num_ap;
			total_rssi = ch_info[i].adj_rssi_factor + ch_info[i + 4].adj_rssi_factor;

			if (avg_load < min_avg_chan_utilization) {
				min_avg_chan_utilization = avg_load;
				ch_idx_min_load = i;
			} else if (avg_load == min_avg_chan_utilization &&
				   total_num_ap < ch_info[ch_idx_min_load].num_ap +
						  ch_info[ch_idx_min_load + 4].num_ap) {
				ch_idx_min_load = i;
			}
			if (total_rssi < min_adjacent_rssi) {
				min_adjacent_rssi = total_rssi;
				ch_idx_min_rssi = i;
			} else if (total_rssi == min_adjacent_rssi &&
				   total_num_ap < ch_info[ch_idx_min_rssi].num_ap +
				   ch_info[ch_idx_min_rssi + 4].num_ap) {
				ch_idx_min_rssi = i;
			}
		}
		if (all_bss_load) {
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_load].chan;
			acs_selected_channels->sec_channel = ch_info[ch_idx_min_load].chan + 4;
		} else {
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_rssi].chan;
			acs_selected_channels->sec_channel = ch_info[ch_idx_min_rssi].chan + 4;
		}

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 20;
	}

	if (acs_selected_channels->ch_width == 20) {
		if (all_bss_load)
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_load_20].chan;
		else
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_rssi_20].chan;
	}

	acs_selected_channels->band = NL80211_BAND_2GHZ;
	return ret;
}

int slsi_is_40mhz(u8 pri_channel, u8 last_channel, bool is_6g_band)
{
	int i;

	if (is_6g_band) {
		if (last_channel < SLSI_6GHZ_LAST_CHAN &&
			(pri_channel - 1) % 16 == 0 && (last_channel - 5) % 16 == 0)
			return 1;
		else
			return 0;
	} else {
		int slsi_40mhz_chan[12] = {38, 46, 54, 62, 102, 110, 118, 126, 134, 142, 151, 159};

		for (i = 0; i < 12; i++) {
			if (pri_channel == slsi_40mhz_chan[i] - 2 && last_channel == slsi_40mhz_chan[i] + 2)
				return 1;
			else if (pri_channel < slsi_40mhz_chan[i])
				return 0;
		}
	}
	return 0;
}

int slsi_is_80mhz(u8 pri_channel, u8 last_channel, bool is_6g_band)
{
	int i;

	if (is_6g_band) {
		if (last_channel < SLSI_6GHZ_LAST_CHAN &&
			(pri_channel - 1) % 16 == 0 && (last_channel - 13) % 16 == 0)
			return 1;
		else
			return 0;
	} else {
		int slsi_80mhz_chan[6] = {42, 58, 106, 122, 138, 155};

		for (i = 0; i < 6; i++) {
			if (pri_channel == slsi_80mhz_chan[i] - 6 &&
			    last_channel == slsi_80mhz_chan[i] + 6)
				return 1;
			else if (pri_channel < slsi_80mhz_chan[i])
				return 0;
		}
	}
	return 0;
}

int slsi_is_160mhz(u8 pri_channel, u8 last_channel, bool is_6g_band)
{
	int i;

	if (is_6g_band) {
		if (last_channel < SLSI_6GHZ_LAST_CHAN &&
			(pri_channel - 1) % 16 == 0 && (last_channel - 29) % 32 == 0)
			return 1;
		else
			return 0;
	} else {
		int slsi_160mhz_chan[2] = {50, 114};

		for (i = 0; i < 2; i++) {
			if (pri_channel == slsi_160mhz_chan[i] - 14 && last_channel == slsi_160mhz_chan[i] + 14)
				return 1;
			else if (pri_channel < slsi_160mhz_chan[i])
				return 0;
		}
	}
	return 0;
}

int slsi_get_chann_idx(struct slsi_acs_chan_info *ch_info, int num_channel, int band,
		       bool all_bss_load, bool none_bss_load)
{
	int i = 0, j = 0, avg_load, total_num_ap, total_min_load_num_ap;
	int min_num_ap = INT_MAX, min_avg_chan_utilization = INT_MAX;
	int ch_idx_min_load = 0, ch_idx_min_ap = 0, idx = 0;
	int ch_list_len = band == NL80211_BAND_5GHZ ? SLSI_NUM_5GHZ_CHANNELS : SLSI_NUM_6GHZ_CHANNELS;
	bool is_6g_band = band == NL80211_BAND_5GHZ ? false : true;
	bool is_valid_channel = true;

	for (i = 0; i < ch_list_len; i++) {
		if (i + num_channel - 1 >= ch_list_len)
			continue;

		for (j = 0; j < num_channel; j++) {
			if (!ch_info[i + j].chan) {
				is_valid_channel = false;
				break;
			}
		}

		if (!is_valid_channel) {
			is_valid_channel = true;
			continue;
		}

		if (num_channel == SLSI_160MHz_CH_NUM &&
		    !slsi_is_160mhz(ch_info[i].chan, ch_info[i + num_channel - 1].chan, is_6g_band)) {
			SLSI_INFO_NODEV("160 bandwidth channel range - %d to %d is wrong.\n",
					ch_info[i].chan, ch_info[i + num_channel - 1].chan);
			continue;
		} else if (num_channel == SLSI_80MHz_CH_NUM &&
			   !slsi_is_80mhz(ch_info[i].chan, ch_info[i + num_channel - 1].chan, is_6g_band)) {
			SLSI_INFO_NODEV("80 bandwidth channel range - %d to %d is wrong.\n",
					ch_info[i].chan, ch_info[i + num_channel - 1].chan);
			continue;
		} else if (num_channel == SLSI_40MHz_CH_NUM &&
			   !slsi_is_40mhz(ch_info[i].chan, ch_info[i + num_channel - 1].chan, is_6g_band)) {
			SLSI_INFO_NODEV("40 bandwidth channel range - %d to %d is wrong.\n",
					ch_info[i].chan, ch_info[i + num_channel - 1].chan);
			continue;
		}

		avg_load = 0, total_num_ap = 0, total_min_load_num_ap = 0;
		for (j = 0; j < num_channel; j++) {
			avg_load += ch_info[i + j].avg_chan_utilization;
			total_num_ap += ch_info[i + j].num_ap;
			total_min_load_num_ap += ch_info[ch_idx_min_load + j].num_ap;
		}

		if (avg_load < min_avg_chan_utilization) {
			min_avg_chan_utilization = avg_load;
			ch_idx_min_load = i;
		} else if (avg_load == min_avg_chan_utilization && total_num_ap < total_min_load_num_ap) {
			ch_idx_min_load = i;
		}

		if (total_num_ap < min_num_ap) {
			min_num_ap = total_num_ap;
			ch_idx_min_ap = i;
		}

		i += (num_channel - 1);
	}

	if (all_bss_load || min_avg_chan_utilization <= (128 * num_channel))
		idx = ch_idx_min_load;
	else if (none_bss_load || min_avg_chan_utilization > (128 * num_channel))
		idx = ch_idx_min_ap;

	return idx;
}

int slsi_set_5g_auto_channel(struct slsi_dev *sdev, struct netdev_vif  *ndev_vif,
			     struct slsi_acs_selected_channels *acs_selected_channels,
			     struct slsi_acs_chan_info *ch_info)
{
	int i = 0, idx = 0;
	bool all_bss_load = true, none_bss_load = true;
	int min_avg_chan_utilization_20 = INT_MAX, min_num_ap_20 = INT_MAX;
	int ch_idx_min_load_20 = 0, ch_idx_min_ap_20 = 0;
	int ret = 0;
	int ch_list_len = SLSI_NUM_5GHZ_CHANNELS;

	acs_selected_channels->ch_width = ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->ch_width;
	acs_selected_channels->hw_mode = ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode;

	SLSI_DBG3(sdev, SLSI_MLME, "ch_lis_len:%d\n", ch_list_len);
	for (i = 0; i < ch_list_len; i++) {
		if (!ch_info[i].chan)
			continue;
		if (ch_info[i].num_bss_load_ap != 0) {
			ch_info[i].avg_chan_utilization = ch_info[i].total_chan_utilization /
							  ch_info[i].num_bss_load_ap;
			if (ch_info[i].avg_chan_utilization < min_avg_chan_utilization_20) {
				min_avg_chan_utilization_20 = ch_info[i].avg_chan_utilization;
				ch_idx_min_load_20 = i;
			} else if (ch_info[i].avg_chan_utilization == min_avg_chan_utilization_20 &&
				   ch_info[i].num_ap < ch_info[ch_idx_min_load_20].num_ap) {
				ch_idx_min_load_20 = i;
			}
			none_bss_load = false;
		} else {
			if (ch_info[i].num_ap < min_num_ap_20) {
				min_num_ap_20 = ch_info[i].num_ap;
				ch_idx_min_ap_20 = i;
			}
			SLSI_DBG3(sdev, SLSI_MLME, "BSS load IE not found\n");
			if (ch_info[i].num_ap != 0)
				ch_info[i].avg_chan_utilization = 128;
			all_bss_load = false;
		}
		SLSI_DBG3(sdev, SLSI_MLME, "ch_info[%d] num_ap:%d chan:%d, total_chan_util:%d, avg_chan_util:%d, bss_load_ap:%d\n",
			  i, ch_info[i].num_ap, ch_info[i].chan, ch_info[i].total_chan_utilization,
			  ch_info[i].avg_chan_utilization, ch_info[i].num_bss_load_ap);
	}

	if (acs_selected_channels->ch_width == 160) {
		idx = slsi_get_chann_idx(ch_info, SLSI_160MHz_CH_NUM, NL80211_BAND_5GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx].chan;
		acs_selected_channels->sec_channel = ch_info[idx].chan + 4;
		acs_selected_channels->vht_seg1_center_ch = ch_info[idx].chan + 14;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 80;
	}

	if (acs_selected_channels->ch_width == 80) {
		idx = slsi_get_chann_idx(ch_info, SLSI_80MHz_CH_NUM, NL80211_BAND_5GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx].chan;
		acs_selected_channels->sec_channel = ch_info[idx].chan + 4;
		acs_selected_channels->vht_seg0_center_ch = ch_info[idx].chan + 6;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 40;
	}

	if (acs_selected_channels->ch_width == 40) {
		idx = slsi_get_chann_idx(ch_info, SLSI_40MHz_CH_NUM, NL80211_BAND_5GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx].chan;
		acs_selected_channels->sec_channel = ch_info[idx + 1].chan;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 20;
	}

	if (acs_selected_channels->ch_width == 20) {
		if (all_bss_load || min_avg_chan_utilization_20 < 128)
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_load_20].chan;
		else if (none_bss_load || min_avg_chan_utilization_20 >= 128)
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_ap_20].chan;
	}

	acs_selected_channels->band = NL80211_BAND_5GHZ;
	return ret;
}

#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
int slsi_set_6g_auto_channel(struct slsi_dev *sdev, struct netdev_vif *ndev_vif,
			     struct slsi_acs_selected_channels *acs_selected_channels,
			     struct slsi_acs_chan_info *ch_info)
{
	int i = 0, idx = 0;
	bool all_bss_load = true, none_bss_load = true;
	int min_avg_chan_utilization_20 = INT_MAX, min_num_ap_20 = INT_MAX;
	int ch_idx_min_load_20 = 0, ch_idx_min_ap_20 = 0;
	int ret = 0;
	int ch_list_len = SLSI_NUM_6GHZ_CHANNELS;
	bool is_psc_freq = false;
	struct ieee80211_channel *channel = NULL;

	if (sdev->supported_6g_160mhz)
		acs_selected_channels->ch_width = 160;
	else
		acs_selected_channels->ch_width = 80;
	acs_selected_channels->hw_mode = ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode;

	SLSI_DBG3(sdev, SLSI_MLME, "ch_lis_len:%d, ch_width : %d\n", ch_list_len, acs_selected_channels->ch_width);
	for (i = 0; i < ch_list_len; i++) {
		if (!ch_info[i].chan)
			continue;
		channel = ieee80211_get_channel(sdev->wiphy, ieee80211_channel_to_frequency(ch_info[i].chan,
											    NL80211_BAND_6GHZ));
		is_psc_freq = cfg80211_channel_is_psc(channel);
		SLSI_DBG3(sdev, SLSI_MLME, "ch_info[i].chan : %d is_psc_freq :%s\n",
			  ch_info[i].chan, is_psc_freq ? "Y" : "N");

		if (ch_info[i].num_bss_load_ap != 0) {
			ch_info[i].avg_chan_utilization = ch_info[i].total_chan_utilization /
							  ch_info[i].num_bss_load_ap;
			none_bss_load = false;

			if (!is_psc_freq)
				continue;

			if (ch_info[i].avg_chan_utilization < min_avg_chan_utilization_20) {
				min_avg_chan_utilization_20 = ch_info[i].avg_chan_utilization;
				ch_idx_min_load_20 = i;
			} else if (ch_info[i].avg_chan_utilization == min_avg_chan_utilization_20 &&
				   ch_info[i].num_ap < ch_info[ch_idx_min_load_20].num_ap) {
				ch_idx_min_load_20 = i;
			}
		} else {
			if (is_psc_freq && ch_info[i].num_ap < min_num_ap_20) {
				min_num_ap_20 = ch_info[i].num_ap;
				ch_idx_min_ap_20 = i;
			}
			SLSI_DBG3(sdev, SLSI_MLME, "BSS load IE not found\n");
			if (ch_info[i].num_ap != 0)
				ch_info[i].avg_chan_utilization = 128;
			all_bss_load = false;
		}
		SLSI_DBG3(sdev, SLSI_MLME, "ch_info[%d] num_ap:%d chan:%d, total_chan_util:%d, avg_chan_util:%d, bss_load_ap:%d\n",
			  i, ch_info[i].num_ap, ch_info[i].chan, ch_info[i].total_chan_utilization,
			  ch_info[i].avg_chan_utilization, ch_info[i].num_bss_load_ap);
	}

	if (acs_selected_channels->ch_width == 160) {
		idx = slsi_get_chann_idx(ch_info, SLSI_160MHz_CH_NUM, NL80211_BAND_6GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx + 1].chan;
		acs_selected_channels->sec_channel = ch_info[idx + 1].chan + 16;
		acs_selected_channels->vht_seg1_center_ch = ch_info[idx].chan + 14;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 80;
	}

	if (acs_selected_channels->ch_width == 80) {
		idx = slsi_get_chann_idx(ch_info, SLSI_80MHz_CH_NUM, NL80211_BAND_6GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx + 1].chan;
		acs_selected_channels->sec_channel = ch_info[idx + 1].chan + 8;
		acs_selected_channels->vht_seg0_center_ch = ch_info[idx].chan + 6;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 40;
	}

	if (acs_selected_channels->ch_width == 40) {
		idx = slsi_get_chann_idx(ch_info, SLSI_40MHz_CH_NUM, NL80211_BAND_6GHZ, all_bss_load, none_bss_load);

		acs_selected_channels->pri_channel = ch_info[idx + 1].chan;
		acs_selected_channels->sec_channel = ch_info[idx].chan;

		if (!acs_selected_channels->pri_channel)
			acs_selected_channels->ch_width = 20;
	}

	if (acs_selected_channels->ch_width == 20) {
		if (all_bss_load || min_avg_chan_utilization_20 < 128)
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_load_20].chan;
		else if (none_bss_load || min_avg_chan_utilization_20 >= 128)
			acs_selected_channels->pri_channel = ch_info[ch_idx_min_ap_20].chan;
	}

	acs_selected_channels->band = NL80211_BAND_6GHZ;
	return ret;
}
#endif

int slsi_set_band_any_auto_channel(struct slsi_dev *sdev, struct netdev_vif  *ndev_vif,
				   struct slsi_acs_selected_channels *acs_selected_channels,
				   struct slsi_acs_chan_info *ch_info)
{
	struct slsi_acs_chan_info ch_info_2g[SLSI_NUM_2P4GHZ_CHANNELS];
	struct slsi_acs_chan_info ch_info_5g[SLSI_NUM_5GHZ_CHANNELS];
	struct slsi_acs_selected_channels acs_selected_channels_5g;
	struct slsi_acs_selected_channels acs_selected_channels_2g;
	int best_channel_5g = -1;
	int best_channel_5g_num_ap = 0;
	int best_channel_2g = -1;
	int best_channel_2g_num_ap = 0;
	int i, ret = 0;
	int j = 0;
#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
	struct slsi_acs_chan_info ch_info_6g[SLSI_NUM_6GHZ_CHANNELS];
	struct slsi_acs_selected_channels acs_selected_channels_6g;
	int best_channel_6g = -1;
	int best_channel_6g_num_ap = 0;
#endif

	memset(&acs_selected_channels_5g, 0, sizeof(acs_selected_channels_5g));
	memset(&acs_selected_channels_2g, 0, sizeof(acs_selected_channels_2g));
	memset(&ch_info_5g, 0, sizeof(ch_info_5g));
	memset(&ch_info_2g, 0, sizeof(ch_info_2g));
#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
	memset(&acs_selected_channels_6g, 0, sizeof(acs_selected_channels_6g));
	memset(&ch_info_6g, 0, sizeof(ch_info_6g));

	for (i = SLSI_NUM_2P4GHZ_CHANNELS + SLSI_NUM_5GHZ_CHANNELS; i < SLSI_MAX_CHAN_VALUE_ACS; i++) {
		ch_info_6g[j] = ch_info[i];
		j++;
	}
	ret = slsi_set_6g_auto_channel(sdev, ndev_vif, &acs_selected_channels_6g, ch_info_6g);

	if (acs_selected_channels_6g.ch_width == 40 || acs_selected_channels_6g.ch_width == 20)
		ret = -1;

	if (ret == 0) {
		best_channel_6g = acs_selected_channels_6g.pri_channel;
		for (i = 0; i < SLSI_NUM_6GHZ_CHANNELS; i++) {
			if (ch_info_6g[i].chan == best_channel_6g) {
				best_channel_6g_num_ap = ch_info_6g[i].num_ap;
				break;
			}
		}
		SLSI_DBG3(sdev, SLSI_MLME, "Best 6G channel = %d, num_ap = %d\n", best_channel_6g,
			  best_channel_6g_num_ap);

		if (best_channel_6g_num_ap < MAX_AP_THRESHOLD) {
			*acs_selected_channels = acs_selected_channels_6g;
			acs_selected_channels->hw_mode = SLSI_ACS_MODE_IEEE80211A;
			return ret;
		}
	}

	SLSI_DBG3(sdev, SLSI_MLME, "6G AP threshold exceed, trying to select from 5G band\n");
	j = 0;
#endif

	for (i = SLSI_NUM_2P4GHZ_CHANNELS; i < SLSI_NUM_2P4GHZ_CHANNELS + SLSI_NUM_5GHZ_CHANNELS; i++) {
		ch_info_5g[j] = ch_info[i];
		j++;
	}
	ret = slsi_set_5g_auto_channel(sdev, ndev_vif, &acs_selected_channels_5g, ch_info_5g);

	if (ret == 0) {
		best_channel_5g = acs_selected_channels_5g.pri_channel;
		for (i = 0; i < SLSI_NUM_5GHZ_CHANNELS; i++) {
			if (ch_info_5g[i].chan == best_channel_5g) {
				best_channel_5g_num_ap = ch_info_5g[i].num_ap;
				break;
			}
		}
		SLSI_DBG3(sdev, SLSI_MLME, "Best 5G channel = %d, num_ap = %d\n", best_channel_5g,
			  best_channel_5g_num_ap);

		if (best_channel_5g_num_ap < MAX_AP_THRESHOLD) {
			*acs_selected_channels = acs_selected_channels_5g;
			acs_selected_channels->hw_mode = SLSI_ACS_MODE_IEEE80211A;
			return ret;
		}
	}

	SLSI_DBG3(sdev, SLSI_MLME, "5G AP threshold exceed, trying to select from 2G band\n");

	for (i = 0; i < SLSI_NUM_2P4GHZ_CHANNELS; i++)
		ch_info_2g[i] = ch_info[i];
	ret = slsi_set_2g_auto_channel(sdev, ndev_vif, &acs_selected_channels_2g, ch_info_2g);

	if (ret == 0) {
		best_channel_2g = acs_selected_channels_2g.pri_channel;
		for (i = 0; i < SLSI_NUM_2P4GHZ_CHANNELS; i++) {
			if (ch_info_2g[i].chan == best_channel_2g) {
				best_channel_2g_num_ap = ch_info_2g[i].num_ap;
				break;
			}
		}
		SLSI_DBG3(sdev, SLSI_MLME, "Best 2G channel = %d, num_ap = %d\n", best_channel_2g,
			  best_channel_2g_num_ap);
		if (best_channel_5g == -1) {
			*acs_selected_channels = acs_selected_channels_2g;
			acs_selected_channels->hw_mode = SLSI_ACS_MODE_IEEE80211G;
			return ret;
		}
		/* Based on min no of APs selecting channel from that band */
		/* If no. of APs are equal, selecting the 5G channel */
		if (best_channel_5g_num_ap > best_channel_2g_num_ap) {
			*acs_selected_channels = acs_selected_channels_2g;
			acs_selected_channels->hw_mode = SLSI_ACS_MODE_IEEE80211G;
		} else {
			*acs_selected_channels = acs_selected_channels_5g;
			acs_selected_channels->hw_mode = SLSI_ACS_MODE_IEEE80211A;
		}
	}
	return ret;
}

int slsi_acs_get_rssi_factor(struct slsi_dev *sdev, int rssi, u8 ch_util)
{
	int frac_pow_val[10] = {10, 12, 15, 19, 25, 31, 39, 50, 63, 79};
	int res = 1;
	int i;

	if (rssi < 0)
		rssi = 0 - rssi;
	else
		return INT_MAX;
	for (i = 0; i < rssi / 10; i++)
		res *= 10;
	res = (10000000 * ch_util / res)  / frac_pow_val[rssi % 10];

	SLSI_DBG3(sdev, SLSI_MLME, "ch_util:%d\n", ch_util);
	return res;
}

struct slsi_acs_chan_info *slsi_acs_scan_results(struct slsi_dev *sdev, struct netdev_vif *ndev_vif, u16 scan_id)
{
	struct sk_buff *scan_res;
	struct sk_buff *unique_scan;
	struct sk_buff_head unique_scan_results;
	struct slsi_acs_chan_info *ch_info = ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->acs_chan_info;

	SLSI_DBG3(sdev, SLSI_MLME, "Received acs_results\n");
	skb_queue_head_init(&unique_scan_results);
	SLSI_MUTEX_LOCK(ndev_vif->scan_result_mutex);
	scan_res = slsi_dequeue_cached_scan_result(&ndev_vif->scan[SLSI_SCAN_HW_ID], NULL);

	while (scan_res) {
		struct ieee80211_mgmt *mgmt = fapi_get_mgmt(scan_res);
		size_t                mgmt_len = fapi_get_mgmtlen(scan_res);
		struct ieee80211_channel *scan_channel;
		int idx = 0;
		const u8 *ie_data;
		const u8 *ie;
		int ie_len;
		u8 ch_util = 128;
		/* ieee80211_mgmt structure is similar for Probe Response and Beacons */
		size_t   ies_len = mgmt_len - offsetof(struct ieee80211_mgmt, u.beacon.variable);
		/* make sure this BSSID has not already been used */
		skb_queue_walk(&unique_scan_results, unique_scan) {
			struct ieee80211_mgmt *unique_mgmt = fapi_get_mgmt(unique_scan);

			if (compare_ether_addr(mgmt->bssid, unique_mgmt->bssid) == 0)
				goto next_scan;
		}
		skb_queue_head(&unique_scan_results, scan_res);
		scan_channel = slsi_find_scan_channel(sdev, mgmt, mgmt_len,
						      fapi_get_u16(scan_res, u.mlme_scan_ind.channel_frequency) / 2);
		if (!scan_channel)
			goto next_scan;
		SLSI_DBG3(sdev, SLSI_MLME, "scan result (scan_id:%d, %pM, channel:%d, rssi:%d, ie_len = %zu)\n",
			  fapi_get_u16(scan_res, u.mlme_scan_ind.scan_id),
			  fapi_get_mgmt(scan_res)->bssid, scan_channel->hw_value,
			  fapi_get_s16(scan_res, u.mlme_scan_ind.rssi),
			  ies_len);

		idx = slsi_find_chan_idx(scan_channel->hw_value, ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode, scan_channel->band);
		if (idx < 0) {
			SLSI_DBG3(sdev, SLSI_MLME, "idx is not in range idx=%d\n", idx);
			goto next_scan;
		}
		SLSI_DBG3(sdev, SLSI_MLME, "chan_idx:%d chan_value: %d\n", idx, ch_info[idx].chan);

		if (ch_info[idx].chan) {
			ch_info[idx].num_ap += 1;
			ie = cfg80211_find_ie(WLAN_EID_QBSS_LOAD, mgmt->u.beacon.variable, ies_len);
			if (ie) {
				ie_len = ie[1];
				ie_data = &ie[2];
				if (ie_len >= 3) {
					ch_util = ie_data[2];
					ch_info[idx].num_bss_load_ap += 1;
					ch_info[idx].total_chan_utilization += ch_util;
				}
			}
			if (idx == scan_channel->hw_value - 1)  {    /*if 2.4GHZ channel */
				int res = 0;

				res = slsi_acs_get_rssi_factor(sdev, fapi_get_s16(scan_res, u.mlme_scan_ind.rssi),
							       ch_util);
				ch_info[idx].rssi_factor += res;
				SLSI_DBG3(sdev, SLSI_MLME, "ch_info[%d].rssi_factor:%d\n", idx, ch_info[idx].rssi_factor);
			}
		} else {
			goto next_scan;
		}
next_scan:
		scan_res = slsi_dequeue_cached_scan_result(&ndev_vif->scan[scan_id], NULL);
	}
	SLSI_MUTEX_UNLOCK(ndev_vif->scan_result_mutex);
	skb_queue_purge(&unique_scan_results);
	return ch_info;
}

void slsi_acs_scan_complete(struct slsi_dev *sdev, struct net_device *dev,  u16 scan_id)
{
	struct slsi_acs_selected_channels acs_selected_channels;
	struct slsi_acs_chan_info *ch_info;
	int r = 0;
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	memset(&acs_selected_channels, 0, sizeof(acs_selected_channels));
	ch_info = slsi_acs_scan_results(sdev, ndev_vif, scan_id);
	if (ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode == SLSI_ACS_MODE_IEEE80211A) {
#ifdef CONFIG_SCSC_WLAN_SUPPORT_6G
		if (ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->band == NL80211_BAND_6GHZ)
			r = slsi_set_6g_auto_channel(sdev, ndev_vif, &acs_selected_channels, ch_info);
		else
#endif
			r = slsi_set_5g_auto_channel(sdev, ndev_vif, &acs_selected_channels, ch_info);
	} else if (ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode == SLSI_ACS_MODE_IEEE80211B ||
		 ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode == SLSI_ACS_MODE_IEEE80211G)
		r = slsi_set_2g_auto_channel(sdev, ndev_vif, &acs_selected_channels, ch_info);
	else if (ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request->hw_mode == SLSI_ACS_MODE_IEEE80211ANY)
		r = slsi_set_band_any_auto_channel(sdev, ndev_vif, &acs_selected_channels, ch_info);
	else
		r = -EINVAL;
	if (!r) {
		r = slsi_send_acs_event(sdev, dev, acs_selected_channels);
		if (r != 0)
			SLSI_ERR(sdev, "Could not send ACS vendor event up\n");
	} else {
		SLSI_ERR(sdev, "set_auto_channel failed: %d\n", r);
	}
	sdev->acs_channel_switched = true;
	kfree(ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request);
	ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request = NULL;
}

void slsi_rx_scan_done_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	u16               scan_id = fapi_get_u16(skb, u.mlme_scan_done_ind.scan_id);
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	SLSI_MUTEX_LOCK(ndev_vif->scan_mutex);
	SLSI_NET_DBG3(dev, SLSI_GSCAN, "Received scan_id:%#x\n", scan_id);

#ifdef CONFIG_SCSC_WLAN_GSCAN_ENABLE
	if (slsi_is_gscan_id(scan_id)) {
		SLSI_NET_DBG3(dev, SLSI_GSCAN, "scan_id:%#x\n", scan_id);

		slsi_gscan_handle_scan_result(sdev, dev, skb, scan_id, true);

		SLSI_MUTEX_UNLOCK(ndev_vif->scan_mutex);
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		return;
	}
#endif
	scan_id = (scan_id & 0xFF);

	if (scan_id == SLSI_SCAN_HW_ID && (ndev_vif->scan[SLSI_SCAN_HW_ID].scan_req ||
					   ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request))
		cancel_delayed_work(&ndev_vif->scan_timeout_work);
	if (ndev_vif->scan[SLSI_SCAN_HW_ID].acs_request)
		slsi_acs_scan_complete(sdev, dev, scan_id);
	else
		slsi_scan_complete(sdev, dev, scan_id, false, true);

	/* set_cached_channels should be called here as well , apart from connect_ind as */
	/* we can get an AP with the same SSID in the scan results after connection. */
	/* This should only be done if we are in connected state.*/
	if (ndev_vif->vif_type == FAPI_VIFTYPE_STATION && ndev_vif->sta.vif_status == SLSI_VIF_STATUS_CONNECTED &&
	    ndev_vif->iftype != NL80211_IFTYPE_P2P_CLIENT) {
		const u8 *connected_ssid = NULL;
		struct slsi_roaming_network_map_entry *network_map;
		u32 channels_count = 0;
		u16 channels[SLSI_ROAMING_CHANNELS_MAX];
		u16 merged_channels[SLSI_ROAMING_CHANNELS_MAX * 2];
		u32 merge_chan_count = 0;

		memset(merged_channels, 0, sizeof(merged_channels));
		connected_ssid = cfg80211_find_ie(WLAN_EID_SSID, ndev_vif->sta.sta_bss->ies->data,
						  ndev_vif->sta.sta_bss->ies->len);
		network_map = slsi_roam_channel_cache_get(dev, connected_ssid);
		if (network_map) {
			ndev_vif->sta.channels_24_ghz = network_map->channels_24_ghz;
			ndev_vif->sta.channels_5_ghz = network_map->channels_5_ghz;
			if (sdev->band_6g_supported)
				ndev_vif->sta.channels_6_ghz = network_map->channels_6_ghz;

			channels_count = slsi_roam_channel_cache_get_channels_int(dev, network_map, channels);
			SLSI_MUTEX_LOCK(sdev->device_config_mutex);
			merge_chan_count = slsi_merge_lists(channels, channels_count,
							    sdev->device_config.legacy_roam_scan_list.channels,
							    sdev->device_config.legacy_roam_scan_list.n,
							    merged_channels);
			SLSI_MUTEX_UNLOCK(sdev->device_config_mutex);
			if (slsi_mlme_set_cached_channels(sdev, dev, merge_chan_count, merged_channels) != 0)
				SLSI_NET_ERR(dev, "MLME-SET-CACHED-CHANNELS.req failed\n");
		}
	}

	SLSI_MUTEX_UNLOCK(ndev_vif->scan_mutex);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

void slsi_rx_channel_switched_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	u16 freq = 0;
	int width, ch_width;
	int primary_chan_pos;
	u16 temp_chan_info;
	struct cfg80211_chan_def chandef = {};
	u16 cf1 = 0;
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	mutex_lock(&ndev_vif->wdev.mtx);
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit;
	}

	temp_chan_info = fapi_get_u16(skb, u.mlme_channel_switched_ind.channel_information);
	cf1 = fapi_get_u16(skb, u.mlme_channel_switched_ind.channel_frequency);
	cf1 = cf1 / 2;

	primary_chan_pos = (temp_chan_info >> 8);
	width = (temp_chan_info & 0x00FF);

	/* If width is 80MHz/40MHz then do frequency calculation, else store as it is */
	if (width == 40)
		freq = cf1 + (primary_chan_pos * 20) - 10;
	else if (width == 80)
		freq = cf1 + (primary_chan_pos * 20) - 30;
	else if (width == 160)
		freq = cf1 + (primary_chan_pos * 20) - 70;
	else if (width == SLSI_MLME_FAPI_CHAN_WIDTH_320MHZ)
		freq = cf1 + (primary_chan_pos * 20) - 150;
	else
		freq = cf1;

	ch_width = (width == SLSI_MLME_FAPI_CHAN_WIDTH_320MHZ) ? 320 : width;
	if (width == 20)
		width = NL80211_CHAN_WIDTH_20;
	else if (width == 40)
		width =  NL80211_CHAN_WIDTH_40;
	else if (width == 80)
		width =  NL80211_CHAN_WIDTH_80;
	else if (width == 160)
		width =  NL80211_CHAN_WIDTH_160;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
	else if (width == SLSI_MLME_FAPI_CHAN_WIDTH_320MHZ)
		width =  NL80211_CHAN_WIDTH_320;
#endif

	chandef.chan = ieee80211_get_channel(sdev->wiphy, freq);
	if (!chandef.chan) {
		SLSI_NET_WARN(dev, "invalid freq received (cf1=%d, temp_chan_info=%d, freq=%d)\n",
			      (int)cf1, (int)temp_chan_info, (int)freq);
		goto exit;
	}
	chandef.width = width;
	chandef.center_freq1 = cf1;
	chandef.center_freq2 = 0;

	ndev_vif->sta.ch_width = ch_width;
	ndev_vif->sta.bss_cf = cf1;
	ndev_vif->ap.channel_freq = freq; /* updated for GETSTAINFO */
	ndev_vif->chan = chandef.chan;
	ndev_vif->chandef_saved = chandef;
	SLSI_NET_INFO(dev, "width:%dMHz, center_freq1:%dMHz, primary:%dMHz\n",
		      ch_width, (int)chandef.center_freq1, freq);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 94))
        cfg80211_ch_switch_notify(dev, &chandef, 0, 0);
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 41))
	cfg80211_ch_switch_notify(dev, &chandef, 0);
#else
	cfg80211_ch_switch_notify(dev, &chandef);
#endif
exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	mutex_unlock(&ndev_vif->wdev.mtx);
	kfree_skb(skb);
}

#if (KERNEL_VERSION(5, 2, 0) < LINUX_VERSION_CODE)
static void slsi_rx_send_update_owe_info_event(struct net_device *dev,
					       u8 sta_addr[], u8 *owe_ie, u32 owe_ie_len)
{
	struct cfg80211_update_owe_info owe_info;
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	if (!cfg80211_find_ext_ie(SLSI_WLAN_EID_EXT_OWE_DH_PARAM, owe_ie, owe_ie_len))
		return;

	SLSI_NET_INFO(dev, "OWE DH Param (for AssocReq) is present.\n");

	if (!ndev_vif->activated || ndev_vif->iftype != NL80211_IFTYPE_AP) {
		SLSI_NET_INFO(dev, "ndev_vif (type :%d, activated:%d) not valid for OWE\n",
			      ndev_vif->iftype, ndev_vif->activated);
		return;
	}

	memset(&owe_info, 0, sizeof(owe_info));
	SLSI_ETHER_COPY(owe_info.peer, sta_addr);
	owe_info.ie = owe_ie;
	owe_info.ie_len = owe_ie_len;

	cfg80211_update_owe_info_event(dev, &owe_info, GFP_KERNEL);
}
#else
static inline void slsi_rx_send_update_owe_info_event(struct net_device *dev,
						      u8 sta_addr[], u8 *owe_ie, u32 owe_ie_len)
{
}
#endif

void __slsi_rx_blockack_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct ieee80211_mgmt *mgmt;
	struct ieee80211_bar *bar = NULL;
	struct slsi_peer  *peer = NULL;
	u8 *peer_mac_addr;
	u16 params;
	u16 reason_code;
	u16 priority;
	u16 buffer_size = 0;
	u16 sequence_num = 0;

	switch (fapi_get_sigid(skb)) {
	case MA_BLOCKACKREQ_IND:
		SLSI_NET_DBG1(dev, SLSI_MLME,
			      "ma_blockackreq_ind(vif:%d, timestamp:%u)\n",
			      fapi_get_vif(skb),
			      fapi_get_buff(skb, u.ma_blockackreq_ind.timestamp));

		if (fapi_get_datalen(skb) >= sizeof(struct ieee80211_bar)) {
			bar = (struct ieee80211_bar *)fapi_get_data(skb);
		} else {
			SLSI_NET_DBG1(dev, SLSI_MLME, "invalid fapidata length.\n");
			goto invalid;
		}

		if (!bar) {
			WLBT_WARN(1, "invalid bulkdata for BAR frame\n");
			goto invalid;
		}

		peer = slsi_get_peer_from_mac(sdev, dev, bar->ta);
		priority = (bar->control & IEEE80211_BAR_CTRL_TID_INFO_MASK) >> IEEE80211_BAR_CTRL_TID_INFO_SHIFT;
		sequence_num = le16_to_cpu(bar->start_seq_num) >> 4;
		reason_code = FAPI_REASONCODE_UNSPECIFIED_REASON;
		break;
	case MLME_BLOCKACK_ACTION_IND:
		SLSI_NET_DBG1(dev, SLSI_MLME,
			      "mlme_blockack_action_ind(vif:%d, peer_sta_address:%pM, priority:%d, timestamp:%u)\n",
			      fapi_get_vif(skb),
			      fapi_get_buff(skb, u.mlme_blockack_action_ind.peer_sta_address),
			      fapi_get_u16(skb, u.mlme_blockack_action_ind.tid),
			      fapi_get_u16(skb, u.mlme_blockack_action_ind.timestamp));

		peer_mac_addr = fapi_get_buff(skb, u.mlme_blockack_action_ind.peer_sta_address);
		mgmt = (fapi_get_mgmtlen(skb)) ? fapi_get_mgmt(skb) : NULL;
		if (!mgmt) {

			/* If no bulkdata is provided, Delete the Rx BlockAck
			 * for specified Peer and TID.
			 */
			peer = slsi_get_peer_from_mac(sdev, dev, peer_mac_addr);
			priority = fapi_get_u16(skb, u.mlme_blockack_action_ind.tid);
			reason_code = FAPI_REASONCODE_END;
			break;
		}

		if (ieee80211_is_action(mgmt->frame_control) &&
			mgmt->u.action.category == WLAN_CATEGORY_BACK) {
		    switch (mgmt->u.action.u.addba_req.action_code) {
			case WLAN_ACTION_ADDBA_REQ:
				peer = slsi_get_peer_from_mac(sdev, dev, peer_mac_addr);
				sequence_num = le16_to_cpu(mgmt->u.action.u.addba_req.start_seq_num) >> 4;
				params = le16_to_cpu(mgmt->u.action.u.addba_req.capab);
				priority = (params & IEEE80211_ADDBA_PARAM_TID_MASK) >> 2;
				buffer_size = (params & IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK) >> 6;
				reason_code = FAPI_REASONCODE_START;
				break;
			case WLAN_ACTION_DELBA:
				peer = slsi_get_peer_from_mac(sdev, dev, peer_mac_addr);
				params = le16_to_cpu(mgmt->u.action.u.delba.params);
				priority = (params & IEEE80211_DELBA_PARAM_TID_MASK) >> 12;
				reason_code = FAPI_REASONCODE_END;
				break;
			default:
				SLSI_NET_WARN(dev, "invalid action frame\n");
				break;
			}
		} else {
			SLSI_NET_WARN(dev, "invalid frame, not an action frame or not BA category\n");
		}
		break;
	default:
		SLSI_NET_WARN(dev, "invalid signal ID\n");
		break;
	}
	WLBT_WARN_ON(!peer);

	if (peer) {
		if (priority >= NUM_BA_SESSIONS_PER_PEER) {
			SLSI_NET_DBG3(dev, SLSI_MLME, "priority is invalid (priority:%d)\n", priority);
			goto invalid;
		}

		/* Buffering of frames before the mlme_connected_ind */
		if (ndev_vif->vif_type == FAPI_VIFTYPE_AP && peer->connected_state == SLSI_STA_CONN_STATE_CONNECTING) {
			SLSI_NET_DBG3(dev, SLSI_MLME, "buffering MA_BLOCKACKREQ_IND\n");
			skb_queue_tail(&peer->buffered_frames[priority], skb);
			return;
		}

		/* Buffering of Block Ack Request frame before the Add BA Request */
		if ((reason_code == FAPI_REASONCODE_UNSPECIFIED_REASON) && !peer->ba_session_rx[priority]) {
			SLSI_NET_DBG3(dev, SLSI_MLME, "buffering MA_BLOCKACKREQ_IND (peer:%pM, priority:%d)\n", peer->address, priority);
			skb_queue_tail(&peer->buffered_frames[priority], skb);
			return;
		}

		slsi_handle_blockack(
			dev,
			peer,
			reason_code,
			priority,
			buffer_size,
			sequence_num
			);
	}
invalid:
	kfree_skb(skb);
}

void slsi_rx_blockack_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		kfree_skb(skb);
		return;
	}
	__slsi_rx_blockack_ind(sdev, dev, skb);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
}

void slsi_rx_ma_to_mlme_delba_req(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct sap_drv_ma_to_mlme_delba_req *delba_req = (struct sap_drv_ma_to_mlme_delba_req *)(skb->data);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		kfree_skb(skb);
		return;
	}

	slsi_mlme_delba_req(sdev,
			    dev,
			    delba_req->peer_qsta_address,
			    delba_req->user_priority,
			    delba_req->direction,
			    delba_req->sequence_number,
			    delba_req->reason);

	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

static bool get_wmm_ie_from_resp_ie(struct slsi_dev *sdev, struct net_device *dev, u8 *resp_ie, size_t resp_ie_len, const u8 **wmm_elem, u16 *wmm_elem_len)
{
	struct ieee80211_vendor_ie *ie;

	SLSI_UNUSED_PARAMETER(sdev);

	if (!resp_ie) {
		SLSI_NET_ERR(dev, "Received invalid pointer to the ie's of the association response\n");
		return false;
	}

	*wmm_elem = resp_ie;
	while (*wmm_elem && (*wmm_elem - resp_ie < resp_ie_len)) {
		/* parse response ie elements and return the wmm ie */
		*wmm_elem = cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WMM, *wmm_elem,
						    resp_ie_len - (*wmm_elem - resp_ie));
		/* re-assoc-res can contain wmm parameter IE and wmm TSPEC IE.
		 * we want wmm parameter Element)
		 */
		if (*wmm_elem && (*wmm_elem)[1] > 6 && (*wmm_elem)[6] == WMM_OUI_SUBTYPE_PARAMETER_ELEMENT)
			break;
		if (*wmm_elem)
			*wmm_elem += (*wmm_elem)[1];
	}

	if (!(*wmm_elem)) {
		SLSI_NET_DBG2(dev, SLSI_MLME, "No WMM IE\n");
		return false;
	}
	ie = (struct ieee80211_vendor_ie *)*wmm_elem;
	*wmm_elem_len = ie->len + 2;

	SLSI_NET_DBG3(dev, SLSI_MLME, "WMM IE received and parsed successfully\n");
	return true;
}

static bool sta_wmm_update_uapsd(struct slsi_dev *sdev, struct net_device *dev, struct slsi_peer *peer, u8 *assoc_req_ie, size_t assoc_req_ie_len)
{
	const u8 *wmm_information_ie;

	if (!assoc_req_ie) {
		SLSI_NET_ERR(dev, "null reference to IE\n");
		return false;
	}

	wmm_information_ie = cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WMM, assoc_req_ie, assoc_req_ie_len);
	if (!wmm_information_ie) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "no WMM IE\n");
		return false;
	}

	peer->uapsd = wmm_information_ie[8];
	SLSI_NET_DBG1(dev, SLSI_MLME, "peer->uapsd = 0x%x\n", peer->uapsd);
	return true;
}

static bool sta_wmm_update_wmm_ac_ies(struct slsi_dev *sdev, struct net_device *dev, struct slsi_peer *peer,
				      u8 *assoc_rsp_ie, size_t assoc_rsp_ie_len)
{
	u16   left;
	const u8 *pos;
	const u8 *wmm_elem = NULL;
	u16   wmm_elem_len = 0;
	struct netdev_vif  *ndev_vif = netdev_priv(dev);
	struct slsi_wmm_ac *wmm_ac = &ndev_vif->sta.wmm_ac[0];

	if (!get_wmm_ie_from_resp_ie(sdev, dev, assoc_rsp_ie, assoc_rsp_ie_len, &wmm_elem, &wmm_elem_len)) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "No WMM IE received\n");
		return false;
	}

	if (wmm_elem_len < 10 || wmm_elem[7] /* version */ != 1) {
		SLSI_NET_WARN(dev, "Invalid WMM IE: wmm_elem_len=%lu, wmm_elem[7]=%d\n", (unsigned long int)wmm_elem_len, (int)wmm_elem[7]);
		return false;
	}

	pos = wmm_elem + 10;
	left = wmm_elem_len - 10;

	for (; left >= 4; left -= 4, pos += 4) {
		int aci = (pos[0] >> 5) & 0x03;
		int acm = (pos[0] >> 4) & 0x01;

		memcpy(wmm_ac, pos, sizeof(struct slsi_wmm_ac));

		switch (aci) {
		case 1:                                            /* AC_BK */
			if (acm)
				peer->wmm_acm |= BIT(1) | BIT(2);  /* BK/- */
			break;
		case 2:                                            /* AC_VI */
			if (acm)
				peer->wmm_acm |= BIT(4) | BIT(5);  /* CL/VI */
			break;
		case 3:                                            /* AC_VO */
			if (acm)
				peer->wmm_acm |= BIT(6) | BIT(7);  /* VO/NC */
			break;
		case 0:                                            /* AC_BE */
		default:
			if (acm)
				peer->wmm_acm |= BIT(0) | BIT(3); /* BE/EE */
			break;
		}
		wmm_ac++;
	}

	SLSI_NET_DBG3(dev, SLSI_MLME, "WMM ies have been updated successfully\n");
	return true;
}

#ifdef CONFIG_SCSC_WLAN_KEY_MGMT_OFFLOAD
enum slsi_wlan_vendor_attr_roam_auth {
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_INVALID = 0,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_BSSID,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_REQ_IE,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_RESP_IE,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_AUTHORIZED,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_KEY_REPLAY_CTR,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_PTK_KCK,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_PTK_KEK,
	SLSI_WLAN_VENDOR_ATTR_ROAM_BEACON_IE,
	/* keep last */
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_AFTER_LAST,
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_MAX =
	SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_AFTER_LAST - 1
};

int slsi_send_roam_vendor_event(struct slsi_dev *sdev, const u8 *bssid,
				const u8 *req_ie, u32 req_ie_len, const u8 *resp_ie, u32 resp_ie_len,
				const u8 *beacon_ie, u32 beacon_ie_len, bool authorized)
{
	bool                                   is_secured_bss;
	struct sk_buff                         *skb = NULL;
	u8 err = 0;

	is_secured_bss = cfg80211_find_ie(WLAN_EID_RSN, req_ie, req_ie_len) ||
					cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WPA, req_ie, req_ie_len);

	SLSI_DBG2(sdev, SLSI_MLME, "authorized:%d, is_secured_bss:%d\n", authorized, is_secured_bss);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0))
	skb = cfg80211_vendor_event_alloc(sdev->wiphy, NULL, NLMSG_DEFAULT_SIZE,
					  SLSI_NL80211_VENDOR_SUBCMD_KEY_MGMT_ROAM_AUTH, GFP_KERNEL);
#else
	skb = cfg80211_vendor_event_alloc(sdev->wiphy, NLMSG_DEFAULT_SIZE,
					  SLSI_NL80211_VENDOR_SUBCMD_KEY_MGMT_ROAM_AUTH, GFP_KERNEL);
#endif
	if (!skb) {
		SLSI_ERR_NODEV("Failed to allocate skb for VENDOR Roam event\n");
		return -ENOMEM;
	}

	err |= nla_put(skb, SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_BSSID, ETH_ALEN, bssid) ? BIT(1) : 0;
	err |= nla_put(skb, SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_AUTHORIZED, 1, &authorized) ? BIT(2) : 0;
	err |= (req_ie && nla_put(skb, SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_REQ_IE, req_ie_len, req_ie)) ? BIT(3) : 0;
	err |= (resp_ie && nla_put(skb, SLSI_WLAN_VENDOR_ATTR_ROAM_AUTH_RESP_IE, resp_ie_len, resp_ie)) ? BIT(4) : 0;
	err |= (beacon_ie && nla_put(skb, SLSI_WLAN_VENDOR_ATTR_ROAM_BEACON_IE, beacon_ie_len, beacon_ie)) ? BIT(5) : 0;
	if (err) {
		SLSI_ERR_NODEV("Failed nla_put ,req_ie_len=%d,resp_ie_len=%d,beacon_ie_len=%d,condition_failed=%d\n",
			       req_ie_len, resp_ie_len, beacon_ie_len, err);
		kfree_skb(skb);
		return -EINVAL;
	}
	SLSI_DBG3_NODEV(SLSI_MLME, "Event: KEY_MGMT_ROAM_AUTH(%d)\n", SLSI_NL80211_VENDOR_SUBCMD_KEY_MGMT_ROAM_AUTH);
	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;
}
#endif /* offload */

void slsi_rx_roamed_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif      *ndev_vif = netdev_priv(dev);
	struct ieee80211_mgmt  *mgmt = fapi_get_mgmt(skb);
	struct slsi_peer       *peer;
	u16                    temporal_keys_required = fapi_get_u16(skb, u.mlme_roamed_ind.temporal_keys_required);
	u16                    flow_id = fapi_get_u16(skb, u.mlme_roamed_ind.flow_id);
	struct ieee80211_channel *cur_channel = NULL;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0))
	enum ieee80211_privacy bss_privacy;
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0))
	struct cfg80211_roam_info roam_info = {};
#endif

	rtnl_lock();
	cancel_work_sync(&ndev_vif->set_multicast_filter_work);
	cancel_work_sync(&ndev_vif->update_pkt_filter_work);
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_roamed_ind(vif:%d)\n", fapi_get_vif(skb));
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit;
	}

	peer = slsi_get_peer_from_qs(sdev, dev, SLSI_STA_PEER_QUEUESET);
	if (WLBT_WARN_ON(!peer))
		goto exit;

	if (WLBT_WARN_ON(!ndev_vif->sta.sta_bss))
		goto exit;

	slsi_rx_ba_stop_all(dev, peer);

	if (fapi_get_mgmtlen(skb) >= (offsetof(struct ieee80211_mgmt, bssid) + ETH_ALEN)) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_roamed_ind : Roaming to " MACSTR "\n",
			      MAC2STR(mgmt->bssid));
		SLSI_ETHER_COPY(peer->address, mgmt->bssid);
	} else {
		SLSI_NET_ERR(dev, "invalid fapi mgmt length.\n");
		goto exit;
	}

	slsi_wake_lock(&sdev->wlan_wl_roam);

	if (ndev_vif->sta.mlme_scan_ind_skb) {
		/* saved skb [mlme_scan_ind] freed inside slsi_rx_scan_pass_to_cfg80211 */
		cur_channel = slsi_rx_scan_pass_to_cfg80211(sdev, dev, ndev_vif->sta.mlme_scan_ind_skb, true);
		ndev_vif->sta.mlme_scan_ind_skb = NULL;
	} else {
		SLSI_NET_ERR(dev, "mlme_scan_ind_skb is not available, mlme_synchronised_ind not received");
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0))
	if (ndev_vif->sta.sta_bss->capability & WLAN_CAPABILITY_PRIVACY)
		bss_privacy = IEEE80211_PRIVACY_ON;
	else
		bss_privacy = IEEE80211_PRIVACY_OFF;
#endif

	slsi_cfg80211_put_bss(sdev->wiphy, ndev_vif->sta.sta_bss);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0))
	ndev_vif->sta.sta_bss = cfg80211_get_bss(sdev->wiphy, cur_channel, peer->address, NULL, 0,
						 IEEE80211_BSS_TYPE_ANY, bss_privacy);
#else
	ndev_vif->sta.sta_bss = cfg80211_get_bss(sdev->wiphy, cur_channel, peer->address, NULL, 0,  0, 0);
#endif

	if (!ndev_vif->sta.sta_bss || !ndev_vif->sta.roam_mlme_procedure_started_ind) {
		if (!ndev_vif->sta.sta_bss)
			SLSI_INFO(sdev, "BSS not updated in cfg80211\n");
		if (!ndev_vif->sta.roam_mlme_procedure_started_ind)
			SLSI_INFO(sdev, "procedure-started-ind not received before roamed-ind\n");
		netif_dormant_on(dev);
		slsi_mlme_disconnect(sdev, dev, peer->address, 0, true);
		slsi_handle_disconnect(sdev, dev, peer->address, 0, NULL, 0);
		slsi_wake_unlock(&sdev->wlan_wl_roam);
	} else {
		u8  *assoc_ie = NULL;
		size_t assoc_ie_len = 0;
		u8  *assoc_rsp_ie = NULL;
		size_t assoc_rsp_ie_len = 0;

		slsi_peer_reset_stats(sdev, dev, peer);
		slsi_peer_update_assoc_req(sdev, dev, peer, ndev_vif->sta.roam_mlme_procedure_started_ind);
		slsi_peer_update_assoc_rsp(sdev, dev, peer, skb);

		/* skb is consumed by slsi_peer_update_assoc_rsp. So do not access this anymore. */
		skb = NULL;

		if (peer->assoc_ie) {
			assoc_ie = peer->assoc_ie->data;
			assoc_ie_len = peer->assoc_ie->len;
		}

		if (peer->assoc_resp_ie) {
			assoc_rsp_ie = peer->assoc_resp_ie->data;
			assoc_rsp_ie_len = peer->assoc_resp_ie->len;
		}

		/* this is the right place to initialize the bitmasks for
		 * acm bit and tspec establishment
		 */
		peer->wmm_acm = 0;
		peer->tspec_established = 0;
		peer->uapsd = 0;
		peer->flow_id = flow_id;

		/* update the uapsd bitmask according to the bit values
		 * in wmm information element of association request
		 */
		if (!sta_wmm_update_uapsd(sdev, dev, peer, assoc_ie, assoc_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM uapsd\n");

		/* update the acm bitmask according to the acm bit values that
		 * are included in wmm ie element of association response
		 */
		if (!sta_wmm_update_wmm_ac_ies(sdev, dev, peer, assoc_rsp_ie, assoc_rsp_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM AC ies\n");

		ndev_vif->sta.roam_mlme_procedure_started_ind = NULL;

		if (temporal_keys_required) {
			peer->pairwise_key_set = 0;
			slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_DOING_KEY_CONFIG);
		}

		WLBT_WARN_ON(assoc_ie_len && !assoc_ie);
		WLBT_WARN_ON(assoc_rsp_ie_len && !assoc_rsp_ie);

		SLSI_NET_DBG3(dev, SLSI_MLME, "cfg80211_roamed()\n");

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0))
		/* cfg80211 does not require bss pointer in roam_info.
		 * If bss pointer is given in roam_info, cfg80211 bss
		 * data base goes bad and results in random panic.
		 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 41))
		roam_info.links[0].channel = ndev_vif->sta.sta_bss->channel;
		roam_info.links[0].bssid = peer->address;
		roam_info.links[0].bss = ndev_vif->sta.sta_bss;
#else
		roam_info.channel = ndev_vif->sta.sta_bss->channel;
		roam_info.bssid = peer->address;
		roam_info.bss = ndev_vif->sta.sta_bss;
#endif
		roam_info.req_ie = assoc_ie;
		roam_info.req_ie_len = assoc_ie_len;
		roam_info.resp_ie = assoc_rsp_ie;
		roam_info.resp_ie_len = assoc_rsp_ie_len;
		cfg80211_ref_bss(sdev->wiphy, ndev_vif->sta.sta_bss);
		cfg80211_roamed(dev, &roam_info, GFP_KERNEL);
#else
		cfg80211_roamed(dev,
				ndev_vif->sta.sta_bss->channel,
				peer->address,
				assoc_ie,
				assoc_ie_len,
				assoc_rsp_ie,
				assoc_rsp_ie_len,
				GFP_KERNEL);
#endif
#ifdef CONFIG_SCSC_WLAN_KEY_MGMT_OFFLOAD
		if (slsi_send_roam_vendor_event(sdev, peer->address, assoc_ie, assoc_ie_len,
						assoc_rsp_ie, assoc_rsp_ie_len,
						ndev_vif->sta.sta_bss->ies->data, ndev_vif->sta.sta_bss->ies->len,
						!temporal_keys_required) != 0) {
			SLSI_NET_ERR(dev, "Couldnt send Roam vendor event");
		}
#endif
		SLSI_NET_DBG3(dev, SLSI_MLME, "cfg80211_roamed() Done\n");

		ndev_vif->sta.roam_in_progress = false;
		ndev_vif->chan = ndev_vif->sta.sta_bss->channel;
		SLSI_ETHER_COPY(ndev_vif->sta.bssid, peer->address);

		slsi_wake_unlock(&sdev->wlan_wl_roam);

		SLSI_NET_DBG1(dev, SLSI_MLME, "Taking a wakelock for DHCP to finish after roaming\n");
		slsi_wake_lock_timeout(&sdev->wlan_wl_roam, msecs_to_jiffies(10 * 1000));
#if IS_ENABLED(CONFIG_SCSC_WIFILOGGER)
		SCSC_WLOG_WAKELOCK(WLOG_NORMAL, WL_TAKEN, "wlan_wl_roam", WL_REASON_ROAM);
#endif

		if (!temporal_keys_required) {
			slsi_mlme_roamed_resp(sdev, dev);
			cac_update_roam_traffic_params(sdev, dev);
		} else {
			ndev_vif->sta.resp_id = MLME_ROAMED_RES;
		}
	}

exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	rtnl_unlock();
	kfree_skb(skb);
}

void slsi_rx_roam_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif         *ndev_vif = netdev_priv(dev);

	SLSI_UNUSED_PARAMETER(sdev);

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_roam_ind(vif:%d, aid:0, result:0x%04x )\n",
		      fapi_get_vif(skb),
		      fapi_get_u16(skb, u.mlme_roam_ind.result_code));

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	WLBT_WARN(ndev_vif->vif_type != FAPI_VIFTYPE_STATION, "Not a Station VIF\n");

	if (fapi_get_u16(skb, u.mlme_roam_ind.result_code) != FAPI_RESULTCODE_SUCCESS) {
		SLSI_NET_ERR(dev, "mlme_roam_ind(result:0x%04x) ERROR\n",
			     fapi_get_u16(skb, u.mlme_roam_ind.result_code));
		ndev_vif->sta.roam_in_progress = false;
	}

exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

static void slsi_tdls_event_discovered(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif     *ndev_vif = netdev_priv(dev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	slsi_tdls_manager_event_discovered(sdev, dev, skb);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);

	kfree_skb(skb);
}

static void slsi_tdls_event_connected(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	rtnl_lock();
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	slsi_tdls_manager_event_connected(sdev, dev, skb);

	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	rtnl_unlock();
	kfree_skb(skb);
}

static void slsi_tdls_event_disconnected(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	slsi_tdls_manager_event_disconnected(sdev, dev, skb);

	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

/* Handling for MLME-TDLS-PEER.indication
 */
void slsi_tdls_peer_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u16 tdls_event =  fapi_get_u16(skb, u.mlme_tdls_peer_ind.tdls_event);

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_tdls_peer_ind tdls_event: %d\n", tdls_event);
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		kfree_skb(skb);
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		return;
	}
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);

	switch (tdls_event) {
	case FAPI_TDLSEVENT_CONNECTED:
		slsi_tdls_event_connected(sdev, dev, skb);
		break;
	case FAPI_TDLSEVENT_DISCONNECTED:
		slsi_tdls_event_disconnected(sdev, dev, skb);
		break;
	case FAPI_TDLSEVENT_DISCOVERED:
		slsi_tdls_event_discovered(sdev, dev, skb);
		break;
	default:
		WLBT_WARN_ON((tdls_event == 0) || (tdls_event > 4));
		kfree_skb(skb);
		break;
	}
}

/* Retrieve any buffered frame before connected_ind and pass them up. */
void slsi_rx_buffered_frames(struct slsi_dev *sdev, struct net_device *dev, struct slsi_peer *peer, u8 priority)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct sk_buff    *buff_frame = NULL;
	u8 i = 0;

	WLBT_WARN_ON(!SLSI_MUTEX_IS_LOCKED(ndev_vif->vif_mutex));
	if (WLBT_WARN(!peer, "Peer is NULL"))
		return;
	WLBT_WARN(peer->connected_state == SLSI_STA_CONN_STATE_CONNECTING, "Wrong state");

	if (priority < NUM_BA_SESSIONS_PER_PEER) {
		SLSI_NET_DBG2(dev, SLSI_MLME,
			      "Processing buffered MA_BLOCKACK_IND received before ADDBA request for (vif:%d, aid:%d, priority:%d)\n",
			      ndev_vif->ifnum, peer->aid, priority);
		buff_frame = skb_dequeue(&peer->buffered_frames[priority]);
		while (buff_frame) {
			switch (fapi_get_sigid(buff_frame)) {
			case MA_BLOCKACKREQ_IND:
				SLSI_NET_DBG2(dev, SLSI_MLME, "transferring buffered MA_BLOCKACK_IND frame");
				__slsi_rx_blockack_ind(sdev, dev, buff_frame);
				break;
			default:
				SLSI_NET_WARN(dev, "Unexpected Data: 0x%.4x\n", fapi_get_sigid(buff_frame));
				kfree_skb(buff_frame);
				break;
			}
			buff_frame = skb_dequeue(&peer->buffered_frames[priority]);
		}
		return;
	}

	for (i = 0; i < NUM_BA_SESSIONS_PER_PEER; i++) {
		SLSI_NET_DBG2(dev, SLSI_MLME,
			      "Processing buffered RX frames received before mlme_connected_ind for (vif:%d, aid:%d, priority:%d)\n",
			      ndev_vif->ifnum, peer->aid, i);
		buff_frame = skb_dequeue(&peer->buffered_frames[i]);
		while (buff_frame) {
			slsi_debug_frame(sdev, dev, buff_frame, "RX_BUFFERED");
			switch (fapi_get_sigid(buff_frame)) {
			case MA_BLOCKACKREQ_IND:
				SLSI_NET_DBG2(dev, SLSI_MLME, "transferring buffered MA_BLOCKACK_IND frame");
				__slsi_rx_blockack_ind(sdev, dev, buff_frame);
				break;
			default:
				SLSI_NET_WARN(dev, "Unexpected Data: 0x%.4x\n", fapi_get_sigid(buff_frame));
				kfree_skb(buff_frame);
				break;
			}
			buff_frame = skb_dequeue(&peer->buffered_frames[i]);
		}
	}
}

void slsi_rx_synchronised_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct cfg80211_external_auth_params auth_request;
	struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);
	size_t mgmt_len = fapi_get_mgmtlen(skb);
	int ie_len = mgmt_len - (mgmt->u.probe_resp.variable - (u8 *)mgmt);
	const u8 *rsn = cfg80211_find_ie(WLAN_EID_RSN, mgmt->u.probe_resp.variable, ie_len);
	int r, synch_ind_time = 0;
	u16 sae_auth = 0;
	u8 bssid[ETH_ALEN] = {0};
#if (KERNEL_VERSION(6, 1, 0) <= LINUX_VERSION_CODE)
	const struct cfg80211_bss_ies *beacon_ies;
	const struct element *ml_ie = NULL;
#endif

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		kfree_skb(skb);
		return;
	}
	SLSI_ETHER_COPY(bssid, fapi_get_buff(skb, u.mlme_synchronised_ind.bssid));
	sae_auth = fapi_get_high16_u32(skb, u.mlme_synchronised_ind.spare_1) & 0x00FF;

	if (sae_auth > 1) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "Invalid value received for SAE Auth Request= %d\n", sae_auth);
		goto exit;
	}

	SLSI_NET_DBG1(dev, SLSI_MLME, "Received synchronised_ind, bssid:%pM SAE Auth Request = %d\n", bssid, sae_auth);
	if (!sae_auth) {
		slsi_rx_scan_pass_to_cfg80211(sdev, dev, skb, false);
		if (!slsi_wake_lock_active(&ndev_vif->wlan_wl_sae))
			slsi_wake_lock(&ndev_vif->wlan_wl_sae);

		synch_ind_time = jiffies_to_msecs(jiffies);
		if (synch_ind_time < ndev_vif->sta.connect_cnf_time + SLSI_RX_SYNCH_IND_DELAY)
			udelay(((ndev_vif->sta.connect_cnf_time + 50) - synch_ind_time) * 1000);

		memset(&auth_request, 0x00, sizeof(auth_request));
		auth_request.action = NL80211_EXTERNAL_AUTH_START;
#if (KERNEL_VERSION(6, 1, 0) <= LINUX_VERSION_CODE)
		if (ndev_vif->sta.sta_bss && sdev->fw_eht_enabled) {
			beacon_ies = ndev_vif->sta.sta_bss->ies;
			if (beacon_ies) {
				ml_ie = cfg80211_find_ext_elem(WLAN_EID_EXT_EHT_MULTI_LINK,
							       beacon_ies->data, beacon_ies->len);
/* 2 byte multilink ctrl field,
 * 1 byte common info len field(basic),
 * 6 bytes MLD Mac
 */
#define BASIC_ML_IE_COMMON_INFO_LEN (2 + 1 + ETH_ALEN)
				if (ml_ie && ml_ie->datalen > BASIC_ML_IE_COMMON_INFO_LEN) {
					ether_addr_copy(auth_request.mld_addr, ml_ie->data + 4);
					SLSI_INFO(sdev, "[MLD] Multi Link Authentication AP MLD %d addr: "MACSTR"\n",
						  MAC2STR(auth_request.mld_addr));
				}
			}
		}
#endif

		memcpy(auth_request.bssid, bssid, ETH_ALEN);
		memcpy(auth_request.ssid.ssid, ndev_vif->sta.ssid, ndev_vif->sta.ssid_len);
		auth_request.ssid.ssid_len = ndev_vif->sta.ssid_len;
		SLSI_ETHER_COPY(ndev_vif->sta.external_auth_bssid, bssid);
		if (rsn) {
			if (ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_PSK ||
			    ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_PSK_SHA) {
				int i, pos = 0;

				pos = 7 + 2 + (rsn[8] * 4) + 2;
				for (i = 0; i < rsn[pos - 1]; i++) {
					if (rsn[pos + (i + 1) * 4] == 0x08) {
						pos += i * 4;
						break;
					}
				}
				if (rsn[pos + 4] == 0x08) {
					ndev_vif->sta.crypto.akm_suites[0] = ((rsn[pos + 1] << 24) |
									      (rsn[pos + 2] << 16) |
									      (rsn[pos + 3] << 8) |
									      (rsn[pos + 4]));
				} else {
					SLSI_NET_ERR(dev, "SAE AKM Suite(00-0F-AC:8) is NOT in Probe Response\n");
					goto exit;
				}
				ndev_vif->sta.use_set_pmksa = 1;
				ndev_vif->sta.rsn_ie_len = rsn[1];
				kfree(ndev_vif->sta.rsn_ie);
				ndev_vif->sta.rsn_ie = NULL;
				/* Len+2 because RSN IE TAG and Length */
				ndev_vif->sta.rsn_ie = kmalloc(ndev_vif->sta.rsn_ie_len + 2, GFP_KERNEL);

				/* len+2 because RSNIE TAG and Length */
				if (ndev_vif->sta.rsn_ie)
					memcpy(ndev_vif->sta.rsn_ie, rsn, ndev_vif->sta.rsn_ie_len + 2);
			}
		}
		ndev_vif->sta.crypto.wpa_versions = 3;

		auth_request.key_mgmt_suite = ndev_vif->sta.crypto.akm_suites[0];
		if (ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_FILS_SHA256 ||
		    ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_FILS_SHA384 ||
		    ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_FT_FILS_SHA256 ||
		    ndev_vif->sta.crypto.akm_suites[0] == SLSI_KEY_MGMT_FT_FILS_SHA384)
			ndev_vif->sta.fils_connection = true;
		else
			ndev_vif->sta.fils_connection = false;
		r = cfg80211_external_auth_request(dev, &auth_request, GFP_KERNEL);
		if (r)
			SLSI_NET_DBG1(dev, SLSI_MLME, "cfg80211_external_auth_request failed");

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
		if (ndev_vif->sta.crypto.wpa_versions == 3)
			ndev_vif->sta.wpa3_auth_state = SLSI_WPA3_AUTHENTICATING;
#endif
	}
	/* Connect/Roaming scan data : Save for processing later */
	kfree_skb(ndev_vif->sta.mlme_scan_ind_skb);
	ndev_vif->sta.mlme_scan_ind_skb = skb;
	if (ndev_vif->iftype == NL80211_IFTYPE_STATION)
		sdev->conn_log2us_ctx.conn_flag = true;
exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
}

static void slsi_add_blacklist_info(struct slsi_dev *sdev, struct net_device *dev, struct netdev_vif *ndev_vif, u8 *addr, u32 retention_time)
{
	struct slsi_bssid_blacklist_info *data;
	int blacklist_received_time;
	struct slsi_bssid_blacklist_info *blacklist_info, *tmp;

	/*Check if mac is already present ,
	 * if present then update the rentention time
	 */
	list_for_each_entry_safe(blacklist_info, tmp, &ndev_vif->acl_data_fw_list, list) {
		if (blacklist_info && SLSI_ETHER_EQUAL(blacklist_info->bssid, addr)) {
			blacklist_received_time =  jiffies_to_msecs(jiffies);
			blacklist_info->end_time = blacklist_received_time + retention_time * 1000;
			return;
		}
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);

	if (!data) {
		SLSI_NET_ERR(dev, "Blacklist_add: Unable to add blacklist MAC:" MACSTR "\n", MAC2STR(addr));
		return;
	}
	ether_addr_copy(data->bssid, addr);
	blacklist_received_time =  jiffies_to_msecs(jiffies);
	data->end_time = blacklist_received_time + retention_time * 1000;
	list_add(&data->list, &ndev_vif->acl_data_fw_list);

	/* send set acl down */
	slsi_set_acl(sdev, dev);
}

int slsi_set_acl(struct slsi_dev *sdev, struct net_device *dev)
{
	struct cfg80211_acl_data *acl_data_total = NULL;
	int fw_acl_entries_count = 0;
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	int ret = 0;
	int num_bssid_total = 0;
	struct slsi_bssid_blacklist_info *blacklist_info, *tmp;
	int ioctl_acl_entries_count = 0;

	/* acl is required only for wlan index */
	if (!SLSI_IS_VIF_INDEX_WLAN(ndev_vif))
		return -EINVAL;

	list_for_each_entry_safe(blacklist_info, tmp, &ndev_vif->acl_data_fw_list, list)
		fw_acl_entries_count++;

	list_for_each_entry_safe(blacklist_info, tmp, &ndev_vif->acl_data_ioctl_list, list)
		ioctl_acl_entries_count++;

	if (ndev_vif->acl_data_supplicant)
		num_bssid_total += ndev_vif->acl_data_supplicant->n_acl_entries;
	if (ndev_vif->acl_data_hal)
		num_bssid_total += ndev_vif->acl_data_hal->n_acl_entries;
	num_bssid_total += fw_acl_entries_count;
	num_bssid_total += ioctl_acl_entries_count;

	acl_data_total = kmalloc(sizeof(*acl_data_total) + (sizeof(struct mac_address) * num_bssid_total), GFP_KERNEL);

	if (!acl_data_total) {
		SLSI_ERR(sdev, "Blacklist: Failed to allocate memory\n");
		return -ENOMEM;
	}
	acl_data_total->n_acl_entries = 0;
	acl_data_total->acl_policy = FAPI_ACLPOLICY_BLACKLIST;
	if (ndev_vif->acl_data_supplicant && ndev_vif->acl_data_supplicant->n_acl_entries) {
		memcpy(acl_data_total->mac_addrs[acl_data_total->n_acl_entries].addr,
		       ndev_vif->acl_data_supplicant->mac_addrs[0].addr,
		       ndev_vif->acl_data_supplicant->n_acl_entries * ETH_ALEN);
		acl_data_total->n_acl_entries += ndev_vif->acl_data_supplicant->n_acl_entries;
	}
	if (ndev_vif->acl_data_hal && ndev_vif->acl_data_hal->n_acl_entries) {
		memcpy(acl_data_total->mac_addrs[acl_data_total->n_acl_entries].addr,
		       ndev_vif->acl_data_hal->mac_addrs[0].addr,
		       ndev_vif->acl_data_hal->n_acl_entries * ETH_ALEN);
		acl_data_total->n_acl_entries += ndev_vif->acl_data_hal->n_acl_entries;
	}

	list_for_each_entry_safe(blacklist_info, tmp, &ndev_vif->acl_data_fw_list, list) {
		if (blacklist_info) {
			memcpy(acl_data_total->mac_addrs[acl_data_total->n_acl_entries].addr, blacklist_info->bssid, ETH_ALEN);
			acl_data_total->n_acl_entries++;
		}
	}

	list_for_each_entry_safe(blacklist_info, tmp, &ndev_vif->acl_data_ioctl_list, list) {
		if (blacklist_info) {
			memcpy(acl_data_total->mac_addrs[acl_data_total->n_acl_entries].addr, blacklist_info->bssid, ETH_ALEN);
			acl_data_total->n_acl_entries++;
		}
	}

	ret = slsi_mlme_set_acl(sdev, dev, 0, acl_data_total->acl_policy, acl_data_total->n_acl_entries,
				acl_data_total->mac_addrs);
	kfree(acl_data_total);
	return ret;
}

void slsi_rx_blacklisted_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u8 *mac_addr;
	u32 retention_time;

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_blacklisted_ind(vif:%d, MAC:" MACSTR " )\n",
		      fapi_get_vif(skb),
		      MAC2STR(fapi_get_buff(skb, u.mlme_blacklisted_ind.bssid)));

	cancel_delayed_work_sync(&ndev_vif->blacklist_del_work);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	mac_addr = fapi_get_buff(skb, u.mlme_blacklisted_ind.bssid);
	retention_time = fapi_get_u32(skb, u.mlme_blacklisted_ind.reassociation_retry_delay);
	slsi_add_blacklist_info(sdev, dev, ndev_vif, mac_addr, retention_time);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);

	queue_delayed_work(sdev->device_wq, &ndev_vif->blacklist_del_work, 0);
	kfree_skb(skb);
}

#ifdef CONFIG_SCSC_WLAN_EHT
#define SLSI_MIN_BASIC_ML_IE_COMMON_INFO_LEN \
	(2 + /* Multi-Link Control field */ \
	 1 + /* Common Info Length field (Basic) */ \
	 ETH_ALEN) /* MLD MAC Address field (Basic) */

static void slsi_mhs_get_peer_mld_addr(const u8 *ml_data, u8 *peer_mld_addr)
{
	const struct multi_link_elem *mle = (const void *)ml_data;

	SLSI_ETHER_COPY(peer_mld_addr, mle->variable + 1);
}

static void slsi_mhs_fill_ml_peer_sta_params(struct net_device *dev, struct slsi_peer *peer)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	const struct element *ml_ie = NULL;
	u8 link_id;

	ml_ie = cfg80211_find_ext_elem(WLAN_EID_EXT_EHT_MULTI_LINK, peer->assoc_ie->data,
				       peer->assoc_ie->len);

	if (!ml_ie)
		return;

	if (ml_ie->datalen < SLSI_MIN_BASIC_ML_IE_COMMON_INFO_LEN) {
		SLSI_NET_ERR(dev, "No valid link ml_ie->datalen %d\n", ml_ie->datalen);
		return;
	}

	slsi_mhs_get_peer_mld_addr(ml_ie->data, peer->sinfo.mld_addr);

	link_id = ffs(ndev_vif->ap.ap_link.valid_links) - 1;

	if (link_id > 15)
		SLSI_NET_ERR(dev, "No valid link id %d\n", link_id);
	else
		peer->sinfo.assoc_link_id = link_id;

	peer->sinfo.mlo_params_valid = 1;
}
#else
static void slsi_mhs_fill_ml_peer_sta_params(struct net_device *dev, struct slsi_peer *peer)
{
}
#endif

void slsi_rx_connected_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_peer  *peer = NULL;
	u16               flow_id = fapi_get_u16(skb, u.mlme_connected_ind.flow_id);
	u16               aid = (flow_id >> 8);

	/* For AP mode, peer_index value is equivalent to aid(association_index) value */

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_connected_ind(vif:%d, flow_id:%d)\n",
		      fapi_get_vif(skb),
		      flow_id);
	SLSI_INFO(sdev, "Received Association Response\n");

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	if (WLBT_WARN(ndev_vif->vif_type == FAPI_VIFTYPE_STATION, "STA VIF and Not Roaming"))
		goto exit_with_lock;

	switch (ndev_vif->vif_type) {
	case FAPI_VIFTYPE_AP:
	{
		if (aid < SLSI_PEER_INDEX_MIN || aid > SLSI_PEER_INDEX_MAX) {
			SLSI_NET_ERR(dev, "Received incorrect peer_index: %d\n", aid);
			goto exit_with_lock;
		}

		peer = slsi_get_peer_from_qs(sdev, dev, aid - 1);
		if (!peer) {
			SLSI_NET_ERR(dev, "Peer (aid:%d) Not Found - Disconnect peer\n", aid);
			goto exit_with_lock;
		}

		peer->flow_id = flow_id;
		slsi_mhs_fill_ml_peer_sta_params(dev, peer);
		cfg80211_new_sta(dev, peer->address, &peer->sinfo, GFP_KERNEL);

		if (ndev_vif->ap.privacy) {
			peer->connected_state = SLSI_STA_CONN_STATE_DOING_KEY_CONFIG;
			slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_DOING_KEY_CONFIG);
		} else {
			peer->connected_state = SLSI_STA_CONN_STATE_CONNECTED;
			slsi_mlme_connected_resp(sdev, dev, flow_id);
			slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_CONNECTED);
		}
		slsi_rx_buffered_frames(sdev, dev, peer, 0xFF);
		break;
	}

	default:
		SLSI_NET_WARN(dev, "mlme_connected_ind(vif:%d, unexpected vif type:%d)\n", fapi_get_vif(skb), ndev_vif->vif_type);
		break;
	}
exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

void slsi_rx_reassoc_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif         *ndev_vif = netdev_priv(dev);
	enum ieee80211_statuscode status = WLAN_STATUS_SUCCESS;
	struct slsi_peer          *peer = NULL;
	u8                        *assoc_ie = NULL;
	size_t                    assoc_ie_len = 0;
	u8                        *reassoc_rsp_ie = NULL;
	size_t                    reassoc_rsp_ie_len = 0;

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_reassoc_ind(vif:%d, result:0x%04x)\n",
		      fapi_get_vif(skb),
		      fapi_get_u16(skb, u.mlme_reassociate_ind.result_code));

	rtnl_lock();
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	if (WLBT_WARN(ndev_vif->vif_type != FAPI_VIFTYPE_STATION, "Not a Station VIF\n"))
		goto exit_with_lock;

	peer = slsi_get_peer_from_qs(sdev, dev, 0);
	if (WLBT_WARN_ON(!peer)) {
		SLSI_NET_ERR(dev, "PEER Not found\n");
		goto exit_with_lock;
	}

	if (fapi_get_u16(skb, u.mlme_reassociate_ind.result_code) != FAPI_RESULTCODE_SUCCESS) {
		status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		slsi_rx_ba_stop_all(dev, peer);
	} else {
		peer->pairwise_key_set = 0;

		if (peer->assoc_ie) {
			assoc_ie = peer->assoc_ie->data;
			assoc_ie_len = peer->assoc_ie->len;
			WLBT_WARN_ON(assoc_ie_len && !assoc_ie);
		}

		slsi_peer_reset_stats(sdev, dev, peer);

		peer->sinfo.assoc_req_ies = assoc_ie;
		peer->sinfo.assoc_req_ies_len = assoc_ie_len;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0))
		peer->sinfo.filled |= STATION_INFO_ASSOC_REQ_IES;
#endif
		slsi_peer_update_assoc_rsp(sdev, dev, peer, skb);
		/* skb is consumed by slsi_peer_update_assoc_rsp. So do not access this anymore. */
		skb = NULL;
		if (peer->assoc_resp_ie) {
			reassoc_rsp_ie = peer->assoc_resp_ie->data;
			reassoc_rsp_ie_len = peer->assoc_resp_ie->len;
			WLBT_WARN_ON(reassoc_rsp_ie_len && !reassoc_rsp_ie);
		}

		/* update the uapsd bitmask according to the bit values
		 * in wmm information element of association request
		 */
		if (!sta_wmm_update_uapsd(sdev, dev, peer, assoc_ie, assoc_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM uapsd\n");

		/* update the acm bitmask according to the acm bit values that
		 * are included in wmm ie elements of re-association response
		 */
		if (!sta_wmm_update_wmm_ac_ies(sdev, dev, peer, reassoc_rsp_ie, reassoc_rsp_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM AC ies\n");
	}

	if (!assoc_ie || !assoc_ie_len)
		status = WLAN_STATUS_UNSPECIFIED_FAILURE;

	cfg80211_ref_bss(sdev->wiphy, ndev_vif->sta.sta_bss);
	cfg80211_connect_bss(dev, peer->address, ndev_vif->sta.sta_bss, assoc_ie, assoc_ie_len, reassoc_rsp_ie,
			     reassoc_rsp_ie_len, status, GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);

	if (status == WLAN_STATUS_SUCCESS) {
		ndev_vif->sta.vif_status = SLSI_VIF_STATUS_CONNECTED;

		/* For Open & WEP AP,send reassoc response.
		 * For secured AP, all this would be done after handshake
		 */
		if ((peer->capabilities & WLAN_CAPABILITY_PRIVACY) &&
		    (cfg80211_find_ie(WLAN_EID_RSN, assoc_ie, assoc_ie_len) ||
		     cfg80211_find_ie(SLSI_WLAN_EID_WAPI, assoc_ie, assoc_ie_len) ||
		     cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WPA, assoc_ie, assoc_ie_len))) {
			/*secured AP*/
			ndev_vif->sta.resp_id = MLME_REASSOCIATE_RES;
			slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_DOING_KEY_CONFIG);
			peer->connected_state = SLSI_STA_CONN_STATE_DOING_KEY_CONFIG;
		} else {
			/*Open/WEP AP*/
			slsi_mlme_reassociate_resp(sdev, dev);
			slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_CONNECTED);
			peer->connected_state = SLSI_STA_CONN_STATE_CONNECTED;
		}
	} else {
		netif_dormant_on(dev);
		if (slsi_mlme_del_vif(sdev, dev) != 0)
			SLSI_NET_ERR(dev, "slsi_mlme_del_vif failed\n");
		slsi_vif_deactivated(sdev, dev);
#if (KERNEL_VERSION(4, 2, 0) <= LINUX_VERSION_CODE)
		cfg80211_disconnected(dev, 0, NULL, 0, false, GFP_KERNEL);
#else
		cfg80211_disconnected(dev, 0, NULL, 0, GFP_KERNEL);
#endif
	}

exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	rtnl_unlock();
	kfree_skb(skb);
}

void slsi_connect_result_code(struct netdev_vif *ndev_vif, u16 fw_result_code, int *status, enum nl80211_timeout_reason *timeout_reason)
{
	*status = fw_result_code;
	switch (fw_result_code) {
	case FAPI_RESULTCODE_PROBE_TIMEOUT:
		*timeout_reason = NL80211_TIMEOUT_SCAN;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		*status = SLSI_CONNECT_NO_NETWORK_FOUND;
#endif
		break;
	case FAPI_RESULTCODE_AUTH_TIMEOUT:
		*status = WLAN_STATUS_AUTH_TIMEOUT;
		*timeout_reason = NL80211_TIMEOUT_AUTH;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		if (ndev_vif->sta.crypto.wpa_versions == 3)
			*status = SLSI_CONNECT_AUTH_SAE_NO_RESP;
		else
			*status = SLSI_CONNECT_AUTH_NO_RESP;
#endif
		break;
	case FAPI_RESULTCODE_AUTH_NO_ACK:
		*timeout_reason = NL80211_TIMEOUT_AUTH;
		*status = WLAN_STATUS_AUTH_TIMEOUT;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		if (ndev_vif->sta.crypto.wpa_versions == 3)
			*status = SLSI_CONNECT_AUTH_SAE_NO_ACK;
		else
			*status = SLSI_CONNECT_AUTH_NO_ACK;
#endif
		break;
	case FAPI_RESULTCODE_AUTH_TX_FAIL:
		*timeout_reason = NL80211_TIMEOUT_AUTH;
		*status = WLAN_STATUS_AUTH_TIMEOUT;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		if (ndev_vif->sta.crypto.wpa_versions == 3)
			*status = SLSI_CONNECT_AUTH_SAE_TX_FAIL;
		else
			*status = SLSI_CONNECT_AUTH_TX_FAIL;
#endif
		break;
	case FAPI_RESULTCODE_ASSOC_TIMEOUT:
		*timeout_reason = NL80211_TIMEOUT_ASSOC;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		*status = SLSI_CONNECT_ASSOC_NO_RESP;
#endif
		break;
	case FAPI_RESULTCODE_ASSOC_ABORT:
		*status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		break;
	case FAPI_RESULTCODE_ASSOC_NO_ACK:
		*timeout_reason = NL80211_TIMEOUT_ASSOC;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		*status = SLSI_CONNECT_ASSOC_NO_ACK;
#endif
		break;
	case FAPI_RESULTCODE_ASSOC_TX_FAIL:
		*timeout_reason = NL80211_TIMEOUT_ASSOC;
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
		*status = SLSI_CONNECT_ASSOC_TX_FAIL;
#endif
		break;
	default:
		*status = fw_result_code;
	}
}

#if (KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE)
void slsi_set_twt_config(struct net_device *dev, u8 *assoc_ie, int assoc_ie_len)
{
	struct netdev_vif           *ndev_vif = netdev_priv(dev);
	struct slsi_vif_sta         *sta = &ndev_vif->sta;
	const struct element        *hecap_bie = NULL, *hecap_rie = NULL;
	const struct element        *he_twt_opr = NULL;
	bool                        twt_responder_support = false, twt_requester_support = false;

	hecap_bie = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY, ndev_vif->sta.sta_bss->ies->data,
					   ndev_vif->sta.sta_bss->ies->len);
	if (hecap_bie && hecap_bie->datalen > 4) {
		if (hecap_bie->data[1] & IEEE80211_HE_MAC_CAP0_TWT_RES) {
			sta->twt_peer_cap |= SLSI_GETCAP_TWT_RESPONDER_SUPPORT;
			twt_responder_support = true;
		}
		if (hecap_bie->data[3] & IEEE80211_HE_MAC_CAP2_BCAST_TWT)
			sta->twt_peer_cap |= SLSI_GETCAP_BROADCAST_TWT_SUPPORT;
	}
	hecap_rie = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY, assoc_ie, assoc_ie_len);
	if (hecap_rie && hecap_rie->datalen > 2 && (hecap_rie->data[1] & IEEE80211_HE_MAC_CAP0_TWT_REQ))
		twt_requester_support = true;
	sta->twt_allowed = twt_responder_support && twt_requester_support;
	he_twt_opr = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_OPERATION, ndev_vif->sta.sta_bss->ies->data,
					    ndev_vif->sta.sta_bss->ies->len);
	if (he_twt_opr && he_twt_opr->datalen > 2 && (he_twt_opr->data[1] & IEEE80211_HE_OPERATION_TWT_REQUIRED))
		sta->twt_peer_cap |= SLSI_GETCAP_TWT_REQUIRED;
}
#endif

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
#if (KERNEL_VERSION(5, 15, 41) <= LINUX_VERSION_CODE)

/**
 * mle_common_size - check multi-link element common size
 * @data: multi-link element, must already be checked for size using
 * ieee80211_mle_size_ok()
 */
static inline u8 mle_common_size(struct slsi_dev *sdev, const u8 *data)
{
	const struct multi_link_elem *mle = (const void *)data;
	u16 control = le16_to_cpu(mle->control);
	u8 common = 0;

	switch (u16_get_bits(control, IEEE80211_ML_CONTROL_TYPE)) {
	case IEEE80211_ML_CONTROL_TYPE_BASIC:
		common += sizeof(struct multi_link_elem);
		break;
	case IEEE80211_ML_CONTROL_TYPE_PREQ:
		common += sizeof(struct ieee80211_mle_preq_common_info);
		break;
	case IEEE80211_ML_CONTROL_TYPE_TDLS:
	/* The length is the first octet pointed by mle->variable so no
	 * need to add anything
	 */
		break;
	case IEEE80211_ML_CONTROL_TYPE_RECONF:
		if (control & IEEE80211_MLC_RECONF_PRES_MLD_MAC_ADDR)
			common += ETH_ALEN;
		return common;
	case IEEE80211_ML_CONTROL_TYPE_PRIO_ACCESS:
		if (control & IEEE80211_MLC_PRIO_ACCESS_PRES_AP_MLD_MAC_ADDR)
			common += ETH_ALEN;
		return common;
	default:
		SLSI_WARN(sdev, "[MLD] Invalid control type\n");
	return 0;
	}
	return common + mle->variable[0];
}

static void slsi_get_mld_addr(struct slsi_dev *sdev, struct net_device *dev,
			      const u8 *data, bool is_assoc_req)
{
	const struct multi_link_elem *mle = (const void *)data;
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_vif_sta *sta = &ndev_vif->sta;
	u16 control;

/* Common Info Length field (Basic) + MLD MAC Address field (Basic) */
#define BASIC_ML_IE_COMMON_INFO_LINK_ID_IDX (1 + ETH_ALEN)

	control = le16_to_cpu(mle->control);
	if (is_assoc_req) {
		SLSI_ETHER_COPY(sta->sta_mld_addr, mle->variable + 1);
	} else {
		if (control & IEEE80211_MLC_BASIC_PRES_LINK_ID) {
			sta->assoc_resp_link_id  = mle->variable[BASIC_ML_IE_COMMON_INFO_LINK_ID_IDX];
			sta->valid_links |= BIT(sta->assoc_resp_link_id);
			SLSI_ETHER_COPY(sta->ap_mld_addr, mle->variable + 1);
		}
	}
}

static size_t slsi_mlo_defrag_data(struct slsi_dev *sdev, const u8 *data, size_t data_len,
				   u8 *buf, size_t buf_size, size_t total_ie_len, size_t fragment_id)
{
	const u8 *pos, *end = data + total_ie_len;
	size_t min_defrag_len = 255;
	size_t offset = 0;

	if (!data || !data_len || !buf)
		return 0;

	if (data_len < min_defrag_len) {
		if (data_len <= buf_size) {
			memcpy(buf, data, data_len);
			return data_len;
		} else {
			SLSI_ERR(sdev, "[MLD] Lesser Buffer size (%ld) allocated needed (%ld)\n", buf_size, data_len);
			return 0;
		}
	}
	if (min_defrag_len <= buf_size) {
		memcpy(&buf[offset], data, min_defrag_len - 1);
		offset += min_defrag_len;
		total_ie_len -= min_defrag_len - 1;
		buf_size -= min_defrag_len - 1;
	} else {
		SLSI_ERR(sdev, "[MLD] Lesser Buffer size (%ld) allocated needed (%ld)\n", buf_size, data_len);
		return 0;
	}
	pos = &data[min_defrag_len];
	while (data_len == min_defrag_len && pos[0] == fragment_id && pos[1]) {
		size_t elen = 2 + pos[1];
		data_len = pos[1];

		if (elen > (size_t)(end - pos))
			break;
		if (pos[1] <= buf_size) {
			/* Copy only the fragment data (without the EID and length) */
			memcpy(&buf[offset], &pos[2], pos[1]);
			pos += elen;
			total_ie_len -= elen;
			buf_size -= elen - 2;
			offset += elen;
		} else {
			SLSI_ERR(sdev, "[MLD] Lesser Buffer size (%ld) allocated needed (%ld)\n", buf_size, data_len);
			return 0;
		}
	}
	return offset;
}


static bool slsi_process_mlo_ie(struct slsi_dev *sdev, struct net_device *dev,
				u8 *ie, size_t ie_len, bool is_assoc_req)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_vif_sta *sta = &ndev_vif->sta;
	const struct element *ml_ie = NULL, *sub_elem = NULL;
	int total_ml_ie_len = 0;
	u8 *defrag_ml_data;
	u8 *defrag_prof_data;

	ml_ie = cfg80211_find_ext_elem(WLAN_EID_EXT_EHT_MULTI_LINK, ie, ie_len);
	if (!ml_ie)
		return false;

#define BASIC_ML_IE_COMMON_INFO_LEN \
        (2 + /* Multi-Link Control field */ \
         1 + /* Common Info Length field (Basic) */ \
         ETH_ALEN) /* MLD MAC Address field (Basic) */

	if (ml_ie->datalen <= BASIC_ML_IE_COMMON_INFO_LEN)
		return false;

	defrag_ml_data = kmalloc((ie_len - ((u8 *)ml_ie - ie)), GFP_KERNEL);
	if (!defrag_ml_data) {
		SLSI_ERR(sdev, "[MLD] malloc failed for ML IE(len:%ld)\n", (ie_len - ((u8 *)ml_ie - ie)));
		return false;
	}
	total_ml_ie_len = slsi_mlo_defrag_data(sdev, ml_ie->data, ml_ie->datalen, defrag_ml_data,
					       (ie_len - ((u8 *)ml_ie - ie)), ie_len, WLAN_EID_FRAGMENT);

	if (total_ml_ie_len == 0) {
		kfree(defrag_ml_data);
		return false;
	}

	slsi_get_mld_addr(sdev, dev, defrag_ml_data, is_assoc_req);

	defrag_prof_data = kmalloc(total_ml_ie_len, GFP_KERNEL);
	if (!defrag_prof_data) {
		SLSI_ERR(sdev, "[MLD] malloc failed for STA profile(len:%ld)\n", total_ml_ie_len);
		kfree(defrag_ml_data);
		return false;
	}
	for_each_element(sub_elem, defrag_ml_data + mle_common_size(sdev, defrag_ml_data),
			 total_ml_ie_len - mle_common_size(sdev, defrag_ml_data)) {
		if (sub_elem->id != IEEE80211_MLE_SUBELEM_PER_STA_PROFILE) {
			continue;
		} else {
#define WLAN_STA_PROFILE_EID_FRAGMENT (254)
			int sta_profile_len = slsi_mlo_defrag_data(sdev, sub_elem->data, sub_elem->datalen,
								   defrag_prof_data,
								   (total_ml_ie_len - ((u8 *)sub_elem - defrag_ml_data)),
								   total_ml_ie_len, WLAN_STA_PROFILE_EID_FRAGMENT);
			struct ieee80211_mle_per_sta_profile *prof = (struct ieee80211_mle_per_sta_profile *)defrag_prof_data;
			u16 control = 0;
			u8 link_id = -1;

			control = le16_to_cpu(prof->control);
			link_id = control & IEEE80211_MLE_STA_CONTROL_LINK_ID;
			if (link_id >= MAX_NUM_MLD_LINKS)
				continue;

#define BASIC_ML_IE_STA_INFO_LEN \
        (2 + /* STA Control field */ \
         1 + /* STA Info Length field (Basic) */ \
         ETH_ALEN) /* STA MAC Address field (Basic) */
			if (sta_profile_len < BASIC_ML_IE_STA_INFO_LEN)
				continue;

			if (control & IEEE80211_MLE_STA_CONTROL_STA_MAC_ADDR_PRESENT) {
				if (is_assoc_req) {
					SLSI_ETHER_COPY(sta->links[link_id].addr, prof->variable);
				} else {
					sta->valid_links |= BIT(link_id);
					SLSI_ETHER_COPY(sta->links[link_id].bssid, prof->variable);
				}
			}
		}
	}
	kfree(defrag_prof_data);
	kfree(defrag_ml_data);
	return true;
}

static void slsi_notify_mld_connect_done(struct slsi_dev *sdev,
					 struct net_device *dev, const u8 *bssid,
					 const u8 *sta_addr,
					 struct cfg80211_bss *bss, int status,
					 enum nl80211_timeout_reason timeout_reason,
					 u8 *assoc_ie, size_t assoc_ie_len,
					 u8 *assoc_rsp_ie, size_t assoc_rsp_ie_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct cfg80211_connect_resp_params params;
	int link_id = -1;

	memset(&params, 0, sizeof(params));
	ndev_vif->sta.valid_links = 0;

	/* Process Assoc Resp MLO IE to get BSSID and STA MAC address */
	if(!slsi_process_mlo_ie(sdev, dev, assoc_rsp_ie, assoc_rsp_ie_len, false)) {
		SLSI_ERR(sdev, "[MLD] Failed to process Multi Link IE in Association Response\n");
		return;
	}

	/* Process Assoc req MLO IE to get BSSID and STA MAC address */
	if(!slsi_process_mlo_ie(sdev, dev, assoc_ie, assoc_ie_len, true)) {
		SLSI_ERR(sdev, "[MLD] Failed to process Multi Link IE in Association Request\n");
		return;
	}

	for (link_id = 0; link_id < MAX_NUM_MLD_LINKS; link_id++) {
		if (!(ndev_vif->sta.valid_links & BIT(link_id)))
			continue;
		if (link_id == ndev_vif->sta.assoc_resp_link_id) {
			params.links[link_id].bssid = bssid;
			params.links[link_id].addr = sta_addr;
			memcpy(ndev_vif->sta.links[link_id].bssid, bssid, ETH_ALEN);
			memcpy(ndev_vif->sta.links[link_id].addr, sta_addr, ETH_ALEN);
		} else {
			params.links[link_id].bssid = ndev_vif->sta.links[link_id].bssid;
			params.links[link_id].addr = ndev_vif->sta.links[link_id].addr;
		}
		SLSI_INFO(sdev, "[MLD] Multi Link Association link %d addr: "MACSTR" BSSID: "MACSTR"\n", link_id,
			  MAC2STR(params.links[link_id].addr), MAC2STR(params.links[link_id].bssid));
	}
	params.status = status;
	params.req_ie = assoc_ie;
	params.req_ie_len = assoc_ie_len;
	params.resp_ie = assoc_rsp_ie;
	params.resp_ie_len = assoc_rsp_ie_len;
	params.timeout_reason = timeout_reason;
	params.valid_links = ndev_vif->sta.valid_links;
	params.ap_mld_addr = ndev_vif->sta.ap_mld_addr;
	params.links[ndev_vif->sta.assoc_resp_link_id].bss = bss;

	cfg80211_ref_bss(sdev->wiphy, ndev_vif->sta.sta_bss);
	cfg80211_connect_done(dev, &params, GFP_KERNEL);
}
#endif
#endif

#if (KERNEL_VERSION(4, 11, 0) <= LINUX_VERSION_CODE)
static void slsi_rx_notify_connection_result(struct slsi_dev *sdev,
					     struct net_device *dev,
					     enum nl80211_timeout_reason timeout_reason,
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
					     u16 fw_result_code,
#ifdef CONFIG_SCSC_WLAN_KEY_MGMT_OFFLOAD
					     struct slsi_peer *peer,
#endif
#endif
					     const unsigned char *bssid,
					     const unsigned char *sta_addr,
					     int status, u8 *assoc_ie, size_t assoc_ie_len,
					     u8 *assoc_rsp_ie, size_t assoc_rsp_ie_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
	if (fw_result_code >= FAPI_RESULTCODE_PROBE_TIMEOUT && fw_result_code <= FAPI_RESULTCODE_ASSOC_TIMEOUT) {
		cfg80211_connect_timeout(dev, bssid, assoc_ie, assoc_ie_len,
					 GFP_KERNEL, timeout_reason);
	} else {
		if (!ndev_vif->sta.roam_on_disconnect || status != WLAN_STATUS_SUCCESS) {
#if (KERNEL_VERSION(5, 15, 41) <= LINUX_VERSION_CODE)
			const struct element *ml_ie = NULL;

			ml_ie = cfg80211_find_ext_elem(WLAN_EID_EXT_EHT_MULTI_LINK, assoc_rsp_ie,
						       assoc_rsp_ie_len);
			if (!ml_ie) {
#endif
				cfg80211_ref_bss(sdev->wiphy, ndev_vif->sta.sta_bss);
				cfg80211_connect_bss(dev, bssid, ndev_vif->sta.sta_bss, assoc_ie,
						     assoc_ie_len, assoc_rsp_ie, assoc_rsp_ie_len,
						     status, GFP_KERNEL, timeout_reason);
#if (KERNEL_VERSION(5, 15, 41) <= LINUX_VERSION_CODE)
			} else  {
				slsi_notify_mld_connect_done(sdev, dev, bssid, sta_addr, ndev_vif->sta.sta_bss,
							     status, timeout_reason, assoc_ie, assoc_ie_len,
							     assoc_rsp_ie, assoc_rsp_ie_len);
			}
#endif

		} else {
#if (KERNEL_VERSION(4, 12, 0) <= LINUX_VERSION_CODE)
			struct cfg80211_roam_info roam_info = {};

			/* cfg80211 does not require bss pointer in roam_info.
			 * If bss pointer is given in roam_info, cfg80211 bss
			 * data base goes bad and results in random panic.
			 */
#if (KERNEL_VERSION(5, 15, 41) <= LINUX_VERSION_CODE)
			roam_info.links[0].channel = ndev_vif->sta.sta_bss->channel;
			roam_info.links[0].bssid = ndev_vif->sta.sta_bss->bssid;
#else
			roam_info.channel = ndev_vif->sta.sta_bss->channel;
			roam_info.bssid = ndev_vif->sta.sta_bss->bssid;
#endif
			roam_info.req_ie = assoc_ie;
			roam_info.req_ie_len = assoc_ie_len;
			roam_info.resp_ie = assoc_rsp_ie;
			roam_info.resp_ie_len = assoc_rsp_ie_len;
			cfg80211_roamed(dev, &roam_info, GFP_KERNEL);
#else
			cfg80211_roamed(dev,
					ndev_vif->sta.sta_bss->channel,
					ndev_vif->sta.sta_bss->bssid,
					assoc_ie,
					assoc_ie_len,
					assoc_rsp_ie,
					assoc_rsp_ie_len,
					GFP_KERNEL);
#endif
#ifdef CONFIG_SCSC_WLAN_KEY_MGMT_OFFLOAD
			if (slsi_send_roam_vendor_event(sdev, peer->address, assoc_ie, assoc_ie_len,
							assoc_rsp_ie, assoc_rsp_ie_len,
							ndev_vif->sta.sta_bss->ies->data,
							ndev_vif->sta.sta_bss->ies->len,
							false) != 0) {
				SLSI_NET_ERR(dev, "Couldnt send Roam event");
			}
#endif
			ndev_vif->sta.roam_on_disconnect = false;
		}
	}
#else
	cfg80211_ref_bss(sdev->wiphy, ndev_vif->sta.sta_bss);
	cfg80211_connect_bss(dev, bssid, ndev_vif->sta.sta_bss, assoc_ie, assoc_ie_len, assoc_rsp_ie,
			     assoc_rsp_ie_len, status, GFP_KERNEL, timeout_reason);
#endif
}
#endif

static void slsi_rx_abort_external_auth(struct slsi_dev *sdev, struct net_device *dev, u16 fw_result_code)
{
	int r;
	struct cfg80211_external_auth_params auth_request;
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	if (!(fw_result_code == FAPI_RESULTCODE_AUTH_TIMEOUT ||
	      fw_result_code == FAPI_RESULTCODE_AUTH_NO_ACK ||
	      fw_result_code == FAPI_RESULTCODE_AUTH_TX_FAIL))
		return;

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
	if ((ndev_vif->sta.crypto.wpa_versions == 3 &&
	     ndev_vif->sta.wpa3_auth_state == SLSI_WPA3_AUTHENTICATING) || ndev_vif->sta.fils_connection) {
#else
	if (ndev_vif->sta.crypto.wpa_versions == 3 || ndev_vif->sta.fils_connection) {
#endif
		(void)slsi_mlme_reset_dwell_time(sdev, dev);
		memset(&auth_request, 0x00, sizeof(auth_request));
		auth_request.action = NL80211_EXTERNAL_AUTH_ABORT;
		memcpy(auth_request.bssid, ndev_vif->sta.bssid, ETH_ALEN);
		memcpy(auth_request.ssid.ssid, ndev_vif->sta.ssid, ndev_vif->sta.ssid_len);
		auth_request.ssid.ssid_len = ndev_vif->sta.ssid_len;
		auth_request.key_mgmt_suite = ndev_vif->sta.crypto.akm_suites[0];
		r = cfg80211_external_auth_request(dev, &auth_request, GFP_KERNEL);
		if (r)
			SLSI_NET_DBG1(dev, SLSI_MLME, "cfg80211_external_auth_request Abort failed");
	}
}

static bool slsi_rx_connect_ind_fw_result_success(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff **skb,
						  struct slsi_peer *peer, int *status,
						  u8 **assoc_ie, size_t *assoc_ie_len,
						  u8 **assoc_rsp_ie, size_t *assoc_rsp_ie_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct ieee80211_channel *cur_channel = NULL;

	if (!peer || !peer->assoc_ie) {
		if (peer)
			WLBT_WARN(!peer->assoc_ie, "proc-started-ind not received before connect-ind");
		*status = WLAN_STATUS_UNSPECIFIED_FAILURE;
	} else {
		peer->flow_id = fapi_get_u16(*skb, u.mlme_connect_ind.flow_id);

		if (peer->assoc_ie) {
			*assoc_ie = peer->assoc_ie->data;
			*assoc_ie_len = peer->assoc_ie->len;
		}

		slsi_peer_update_assoc_rsp(sdev, dev, peer, *skb);
		/* skb is consumed by slsi_peer_update_assoc_rsp. So do not access this anymore. */
		*skb = NULL;

		if (peer->assoc_resp_ie) {
			*assoc_rsp_ie = peer->assoc_resp_ie->data;
			*assoc_rsp_ie_len = peer->assoc_resp_ie->len;
		}

		/* this is the right place to initialize the bitmasks for
		 * acm bit and tspec establishment
		 */
		peer->wmm_acm = 0;
		peer->tspec_established = 0;
		peer->uapsd = 0;

		/* update the uapsd bitmask according to the bit values
		 * in wmm information element of association request
		 */
		if (!sta_wmm_update_uapsd(sdev, dev, peer, *assoc_ie, *assoc_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM uapsd\n");

		/* update the wmm ac bitmasks according to the bit values that
		 * are included in wmm ie elements of association response
		 */
		if (!sta_wmm_update_wmm_ac_ies(sdev, dev, peer, *assoc_rsp_ie, *assoc_rsp_ie_len))
			SLSI_NET_DBG1(dev, SLSI_MLME, "Fail to update WMM AC ies\n");

		WLBT_WARN_ON(!(*assoc_rsp_ie_len) && !(*assoc_rsp_ie));
	}

	WLBT_WARN(!ndev_vif->sta.mlme_scan_ind_skb, "mlme_scan.ind not received before connect-ind");

	if (ndev_vif->sta.mlme_scan_ind_skb) {
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
		struct ieee80211_mgmt *mgmt = fapi_get_mgmt(ndev_vif->sta.mlme_scan_ind_skb);

		ndev_vif->sta.beacon_int = mgmt->u.beacon.beacon_int;
#endif
		SLSI_NET_DBG1(dev, SLSI_MLME, "Sending scan indication to cfg80211, bssid: %pM\n",
			      fapi_get_mgmt(ndev_vif->sta.mlme_scan_ind_skb)->bssid);
		/* saved skb [mlme_scan_ind] freed inside slsi_rx_scan_pass_to_cfg80211 */
		cur_channel = slsi_rx_scan_pass_to_cfg80211(sdev, dev, ndev_vif->sta.mlme_scan_ind_skb, true);
		ndev_vif->sta.mlme_scan_ind_skb = NULL;
	}

	if (!ndev_vif->sta.sta_bss) {
		if (peer)
#if (KERNEL_VERSION(4, 1, 0) <= LINUX_VERSION_CODE)
			ndev_vif->sta.sta_bss = cfg80211_get_bss(sdev->wiphy, cur_channel, peer->address,
								 NULL, 0,  IEEE80211_BSS_TYPE_ANY,
								 IEEE80211_PRIVACY_ANY);
#else
			ndev_vif->sta.sta_bss = cfg80211_get_bss(sdev->wiphy, cur_channel, peer->address,
								 NULL, 0,  0, 0);
#endif
		if (!ndev_vif->sta.sta_bss) {
			SLSI_NET_ERR(dev, "sta_bss is not available, terminating the connection (peer: %p)\n", peer);
			*status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		}
	}

	return true;
}

static bool slsi_rx_connect_ind_fw_result_failure(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff **skb,
						  enum nl80211_timeout_reason *timeout_reason,
						  u16 *fw_result_code, int *status,
						  u8 **assoc_rsp_ie, size_t *assoc_rsp_ie_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
	slsi_rx_abort_external_auth(sdev, dev, *fw_result_code);
	if (ndev_vif->sta.drv_bss_selection && slsi_retry_connection(sdev, dev)) {
		SLSI_INFO(sdev, "Connect ind : retrying connection\n");
		if (ndev_vif->sta.crypto.wpa_versions == 3)
			ndev_vif->sta.wpa3_sae_reconnection = true;
		return false;
	}
	ndev_vif->sta.drv_connect_req_ongoing = false;
	slsi_free_connection_params(sdev, dev);
#endif
	if (*fw_result_code == FAPI_RESULTCODE_AUTH_TIMEOUT) {
		SLSI_INFO(sdev, "Connect failed,Result code: Auth Timeout\n");
	} else if (*fw_result_code == FAPI_RESULTCODE_ASSOC_TIMEOUT) {
		SLSI_INFO(sdev, "Connect failed,Result code: Assoc Timeout\n");
	} else if (*fw_result_code == FAPI_RESULTCODE_PROBE_TIMEOUT) {
		SLSI_INFO(sdev, "Connect failed,Result code: Probe Timeout\n");
	} else if (*fw_result_code >= FAPI_RESULTCODE_AUTH_FAILED_CODE && *fw_result_code <= 0x81FF) {
		if (*fw_result_code != FAPI_RESULTCODE_AUTH_FAILED_CODE)
			*fw_result_code = *fw_result_code & 0x00FF;
		SLSI_INFO(sdev, "Connect failed(Auth failure), Result code:0x%04x\n", *fw_result_code);
	} else if (*fw_result_code >= FAPI_RESULTCODE_ASSOC_FAILED_CODE && *fw_result_code <= 0x82FF) {
		if (*fw_result_code != FAPI_RESULTCODE_ASSOC_FAILED_CODE)
			*fw_result_code = *fw_result_code & 0x00FF;
		SLSI_INFO(sdev, "Connect failed(Assoc Failure), Result code:0x%04x\n", *fw_result_code);

		if (fapi_get_datalen(*skb)) {
			int mgmt_hdr_len;
			struct ieee80211_mgmt *mgmt = fapi_get_mgmt((*skb));

			if (ieee80211_is_assoc_resp(mgmt->frame_control)) {
				mgmt_hdr_len = (mgmt->u.assoc_resp.variable - (u8 *)mgmt);
			} else if (ieee80211_is_reassoc_resp(mgmt->frame_control)) {
				mgmt_hdr_len = (mgmt->u.reassoc_resp.variable - (u8 *)mgmt);
			} else {
				SLSI_NET_DBG1(dev, SLSI_MLME, "Assoc/Reassoc response not found!\n");
				return false;
			}

			*assoc_rsp_ie = (char *)mgmt + mgmt_hdr_len;
			*assoc_rsp_ie_len = fapi_get_datalen(*skb) - mgmt_hdr_len;
		}
	} else {
		SLSI_INFO(sdev, "Connect failed,Result code:0x%04x\n", *fw_result_code);
	}

	slsi_connect_result_code(ndev_vif, *fw_result_code, status, timeout_reason);
#if IS_ENABLED(CONFIG_SCSC_LOG_COLLECTION)
	/* Trigger log collection if fw result code is not success */
	scsc_log_collector_schedule_collection(SCSC_LOG_HOST_WLAN, SCSC_LOG_HOST_WLAN_REASON_CONNECT_ERR);
#endif
#if (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
	slsi_rx_abort_external_auth(sdev, dev, *fw_result_code);
#endif

	return true;
}

static bool slsi_rx_connect_ind_connected_handle(struct slsi_dev *sdev, struct net_device *dev, struct slsi_peer *peer,
						 u8 *assoc_ie, int assoc_ie_len)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	ndev_vif->sta.vif_status = SLSI_VIF_STATUS_CONNECTED;

#if (KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE)
	slsi_set_twt_config(dev, assoc_ie, assoc_ie_len);
#endif

	if (ndev_vif->sta.fils_connection)
		return false;

	/* For Open & WEP AP,set the power mode (static IP scenario),
	 * send connect response and install the packet filters .
	 * For secured AP, all this would be done after handshake
	 */
	if ((peer->capabilities & WLAN_CAPABILITY_PRIVACY) &&
	    (cfg80211_find_ie(WLAN_EID_RSN, assoc_ie, assoc_ie_len) ||
	     cfg80211_find_ie(SLSI_WLAN_EID_WAPI, assoc_ie, assoc_ie_len) ||
	     cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WPA, assoc_ie, assoc_ie_len))) {
		/*secured AP*/
		slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_DOING_KEY_CONFIG);
		ndev_vif->sta.resp_id = MLME_CONNECT_RES;
	} else {
		/*Open/WEP AP*/
		slsi_mlme_connect_resp(sdev, dev);
		if (ndev_vif->ipaddress)
			slsi_ip_address_changed(sdev, dev, ndev_vif->ipaddress);

		slsi_set_acl(sdev, dev);
		slsi_set_packet_filters(sdev, dev);

		if (ndev_vif->ipaddress)
			slsi_mlme_powermgt(sdev, dev, ndev_vif->set_power_mode);
		slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_CONNECTED);
	}

	/* For P2PCLI, set the Connection Timeout (beacon miss) mib to 10 seconds
	 * This MIB set failure does not cause any fatal isuue. It just varies the
	 * detection time of GO's absence from 10 sec to FW default. So Do not disconnect
	 */
	if (ndev_vif->iftype == NL80211_IFTYPE_P2P_CLIENT)
		SLSI_P2P_STATE_CHANGE(sdev, P2P_GROUP_FORMED_CLI);

	/*Update the firmware with cached channels*/
#ifdef CONFIG_SCSC_WLAN_WES_NCHO
	if (!sdev->device_config.roam_scan_mode && ndev_vif->vif_type == FAPI_VIFTYPE_STATION && ndev_vif->activated &&
	    ndev_vif->iftype != NL80211_IFTYPE_P2P_CLIENT) {
#else
	if (ndev_vif->vif_type == FAPI_VIFTYPE_STATION && ndev_vif->activated &&
	    ndev_vif->iftype != NL80211_IFTYPE_P2P_CLIENT) {
#endif
		const u8 *ssid = cfg80211_find_ie(WLAN_EID_SSID, assoc_ie, assoc_ie_len);
		struct slsi_roaming_network_map_entry *network_map;
		u16 channels[SLSI_ROAMING_CHANNELS_MAX];
		u32 channels_count = slsi_roaming_scan_configure_channels(sdev, dev, ssid, channels);
		u16 merged_channels[SLSI_ROAMING_CHANNELS_MAX * 2];
		u32 merge_chan_count = 0;

		memset(merged_channels, 0, sizeof(merged_channels));

		network_map = slsi_roam_channel_cache_get(dev, ssid);
		if (network_map) {
			ndev_vif->sta.channels_24_ghz = network_map->channels_24_ghz;
			ndev_vif->sta.channels_5_ghz = network_map->channels_5_ghz;
			if (sdev->band_6g_supported)
				ndev_vif->sta.channels_6_ghz = network_map->channels_6_ghz;
		}

		SLSI_MUTEX_LOCK(sdev->device_config_mutex);
		merge_chan_count = slsi_merge_lists(channels, channels_count,
						    sdev->device_config.legacy_roam_scan_list.channels,
						    sdev->device_config.legacy_roam_scan_list.n,
						    merged_channels);
		SLSI_MUTEX_UNLOCK(sdev->device_config_mutex);
		if (slsi_mlme_set_cached_channels(sdev, dev, merge_chan_count, merged_channels) != 0)
			SLSI_NET_ERR(dev, "MLME-SET-CACHED-CHANNELS.req failed\n");
	}
	return true;
}

void slsi_rx_connect_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif           *ndev_vif = netdev_priv(dev);
	int                         status = WLAN_STATUS_SUCCESS;
	struct slsi_peer            *peer = NULL;
	u8                          *assoc_ie = NULL;
	size_t                      assoc_ie_len = 0;
	u8                          *assoc_rsp_ie = NULL;
	size_t                      assoc_rsp_ie_len = 0;
	u8                          bssid[ETH_ALEN];
	u16                         fw_result_code;
	enum nl80211_timeout_reason timeout_reason = NL80211_TIMEOUT_UNSPECIFIED;
	int                         conn_fail_reason = 3;
	u8                          sta_addr[ETH_ALEN];
	struct                      ieee80211_hdr *hdr = NULL;

	cancel_work_sync(&ndev_vif->set_multicast_filter_work);
	cancel_work_sync(&ndev_vif->update_pkt_filter_work);
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	fw_result_code = fapi_get_u16(skb, u.mlme_connect_ind.result_code);
	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_connect_ind(vif:%d, result:0x%04x)\n",
		      fapi_get_vif(skb), fw_result_code);

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	if (WLBT_WARN(ndev_vif->vif_type != FAPI_VIFTYPE_STATION, "Not a Station VIF\n"))
		goto exit_with_lock;

	if (ndev_vif->sta.vif_status != SLSI_VIF_STATUS_CONNECTING) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not in connecting state\n");
		goto exit_with_lock;
	}

	peer = slsi_get_peer_from_qs(sdev, dev, SLSI_STA_PEER_QUEUESET);
	if (peer) {
		SLSI_ETHER_COPY(bssid, peer->address);
	} else {
		SLSI_NET_ERR(dev, "!!NO peer record for AP\n");
		eth_zero_addr(bssid);
	}

	if (!sdev->conn_log2us_ctx.conn_flag) {
		if (!ndev_vif->sta.mlme_scan_ind_skb) {
			conn_fail_reason = 1;
			slsi_conn_log2us_connecting_fail(sdev, dev, bssid,
							 ndev_vif->chan ? ndev_vif->chan->center_freq : 0,
							 conn_fail_reason);
		} else {
			slsi_conn_log2us_connecting_fail(sdev, dev, bssid,
							 ndev_vif->chan ? ndev_vif->chan->center_freq : 0,
							 conn_fail_reason);
		}
	}
	sdev->assoc_result_code = fw_result_code;
	if (fw_result_code == FAPI_RESULTCODE_SUCCESS) {
		SLSI_NET_INFO(dev, "Received Association Response\n");
		hdr = (struct ieee80211_hdr *)fapi_get_data(skb);
		SLSI_ETHER_COPY(sta_addr, hdr->addr1);
		slsi_rx_connect_ind_fw_result_success(sdev, dev, &skb, peer, &status, &assoc_ie,
						      &assoc_ie_len, &assoc_rsp_ie, &assoc_rsp_ie_len);
	} else {
		if (!slsi_rx_connect_ind_fw_result_failure(sdev, dev, &skb, &timeout_reason, &fw_result_code,
							   &status, &assoc_rsp_ie, &assoc_rsp_ie_len))
			goto exit_with_lock;
	}

#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION < 11)
	ndev_vif->sta.wpa3_sae_reconnection = false;
#endif

	if (!peer && status == WLAN_STATUS_SUCCESS)
		status = WLAN_STATUS_UNSPECIFIED_FAILURE;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0))
	slsi_rx_notify_connection_result(sdev,
					 dev,
					 timeout_reason,
#if !(defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 11)
					 fw_result_code,
#ifdef CONFIG_SCSC_WLAN_KEY_MGMT_OFFLOAD
					 peer,
#endif
#endif
					 bssid, sta_addr, status, assoc_ie, assoc_ie_len,
					 assoc_rsp_ie, assoc_rsp_ie_len);
#else
	/* cfg80211_connect_result will take a copy of any ASSOC or
	 * ASSOC RSP IEs passed to it
	 */
	cfg80211_connect_result(dev,
				bssid,
				assoc_ie, assoc_ie_len,
				assoc_rsp_ie, assoc_rsp_ie_len,
				(u16)status,
				GFP_KERNEL);
#endif
	if (status == WLAN_STATUS_SUCCESS) {
		if (!slsi_rx_connect_ind_connected_handle(sdev, dev, peer, assoc_ie, assoc_ie_len))
			goto exit_with_lock;
	} else {
		/* Firmware reported connection success, but driver reported failure to cfg80211:
		 * send mlme-disconnect.req to firmware
		 */
		if (fw_result_code == FAPI_RESULTCODE_SUCCESS && peer) {
			slsi_mlme_disconnect(sdev, dev, peer->address, FAPI_REASONCODE_UNSPECIFIED_REASON, true);
			slsi_handle_disconnect(sdev, dev, peer->address, FAPI_REASONCODE_UNSPECIFIED_REASON, NULL, 0);
		} else {
			slsi_handle_disconnect(sdev, dev, NULL, FAPI_REASONCODE_UNSPECIFIED_REASON, NULL, 0);
		}
	}

exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

void slsi_rx_disconnected_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u16 reason;
	u8 *disassoc_rsp_ie = NULL;
	u32 disassoc_rsp_ie_len = 0;

	cancel_work_sync(&ndev_vif->set_multicast_filter_work);
	cancel_work_sync(&ndev_vif->update_pkt_filter_work);
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit;
	}
	reason = fapi_get_u16(skb, u.mlme_disconnected_ind.reason_code);
	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_disconnected_ind(vif:%d, reason:%d, MAC:%pM)\n",
		      fapi_get_vif(skb),
		      fapi_get_u16(skb, u.mlme_disconnected_ind.reason_code),
		      fapi_get_buff(skb, u.mlme_disconnected_ind.peer_sta_address));
#if IS_ENABLED(CONFIG_SCSC_LOG_COLLECTION)
	scsc_log_collector_schedule_collection(SCSC_LOG_HOST_WLAN, SCSC_LOG_HOST_WLAN_REASON_DISCONNECTED_IND);
#else
#ifndef SLSI_TEST_DEV
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	SLSI_NET_INFO(dev, "SCSC_LOG_COLLECTION not enabled. Sable will not be triggered\n");
#else
	mx140_log_dump();
#endif
#endif
#endif
	if (reason <= 0xFF) {
		SLSI_INFO(sdev, "Received DEAUTH, reason = %d\n", reason);
	} else if (reason >= 0x8100 && reason <= 0x81FF) {
		reason = reason & 0x00FF;
		SLSI_INFO(sdev, "Received DEAUTH, reason = %d\n", reason);
	} else if (reason >= 0x8200 && reason <= 0x82FF) {
		reason = reason & 0x00FF;
		SLSI_INFO(sdev, "Received DISASSOC, reason = %d\n", reason);
	} else {
		SLSI_INFO(sdev, "Received DEAUTH, reason = Local Disconnect <%d>\n", reason);
	}

	/* Populate wake reason stats here */
	if (unlikely(slsi_skb_cb_get(skb)->wakeup)) {
		schedule_work(&sdev->wakeup_time_work);
		slsi_rx_update_mlme_stats(sdev, skb);
	}

	if (fapi_get_datalen(skb) >= offsetof(struct ieee80211_mgmt, u.deauth.reason_code) + 2) {
		struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);

		if (ieee80211_is_deauth(mgmt->frame_control)) {
			disassoc_rsp_ie = (char *)&mgmt->u.deauth.reason_code + 2;
			disassoc_rsp_ie_len = fapi_get_datalen(skb) -
							(u32)(disassoc_rsp_ie - (u8 *)fapi_get_data(skb));
		} else if (ieee80211_is_disassoc(mgmt->frame_control)) {
			disassoc_rsp_ie = (char *)&mgmt->u.disassoc.reason_code + 2;
			disassoc_rsp_ie_len = fapi_get_datalen(skb) -
							(u32)(disassoc_rsp_ie - (u8 *)fapi_get_data(skb));
		} else {
			SLSI_NET_DBG1(dev, SLSI_MLME, "Not a disassoc/deauth packet\n");
		}
	}

	if (ndev_vif->vif_type == FAPI_VIFTYPE_AP) {
		if (fapi_get_u16(skb, u.mlme_disconnected_ind.reason_code) ==
		    FAPI_REASONCODE_HOTSPOT_MAX_CLIENT_REACHED) {
			SLSI_NET_DBG1(dev, SLSI_MLME,
				      "Sending max hotspot client reached notification to user space\n");
			cfg80211_conn_failed(dev, fapi_get_buff(skb, u.mlme_disconnected_ind.peer_sta_address),
					     NL80211_CONN_FAIL_MAX_CLIENTS, GFP_KERNEL);
			goto exit;
		}
	}

	slsi_handle_disconnect(sdev,
			       dev,
			       fapi_get_buff(skb, u.mlme_disconnected_ind.peer_sta_address),
			       fapi_get_u16(skb, u.mlme_disconnected_ind.reason_code),
			       disassoc_rsp_ie,
			       disassoc_rsp_ie_len);

exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

/* Handle Procedure Started (Type = Device Discovered) indication for P2P */
static void slsi_rx_p2p_device_discovered_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	int               mgmt_len;

	SLSI_UNUSED_PARAMETER(sdev);

	if (ndev_vif->chan) {
		SLSI_NET_DBG2(dev, SLSI_CFG80211, "Freq = %d\n", ndev_vif->chan->center_freq);
	} else {
		SLSI_NET_ERR(dev, "ndev_vif->chan is NULL\n");
		return;
	}

	/* Only Probe Request is expected as of now */
	mgmt_len = fapi_get_mgmtlen(skb);
	if (mgmt_len) {
		struct ieee80211_mgmt *mgmt = fapi_get_mgmt(skb);

		if (ieee80211_is_mgmt(mgmt->frame_control)) {
			if (ieee80211_is_probe_req(mgmt->frame_control)) {
				SLSI_NET_DBG3(dev, SLSI_CFG80211, "Received Probe Request\n");
				cfg80211_rx_mgmt(&ndev_vif->wdev, ndev_vif->chan->center_freq, 0, (const u8 *)mgmt, mgmt_len, GFP_ATOMIC);
			} else {
				SLSI_NET_ERR(dev, "Ignore Indication - Not Probe Request frame\n");
			}
		} else {
			SLSI_NET_ERR(dev, "Ignore Indication - Not Management frame\n");
		}
	}
}

void slsi_rx_procedure_started_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_peer  *peer = NULL;

	rtnl_lock();
	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_procedure_started_ind(vif:%d, type:%d, flow_id:%d)\n",
		      fapi_get_vif(skb),
		      fapi_get_u16(skb, u.mlme_procedure_started_ind.procedure_type),
		      fapi_get_u16(skb, u.mlme_procedure_started_ind.flow_id));
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}
	if (fapi_get_u16(skb, u.mlme_procedure_started_ind.procedure_type) != FAPI_PROCEDURETYPE_DEVICE_DISCOVERED)
		SLSI_INFO(sdev, "Send Association Request\n");

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	switch (fapi_get_u16(skb, u.mlme_procedure_started_ind.procedure_type)) {
	case FAPI_PROCEDURETYPE_CONNECTION_STARTED:
		switch (ndev_vif->vif_type) {
		case FAPI_VIFTYPE_AP:
		{
			u16 flow_id = fapi_get_u16(skb, u.mlme_procedure_started_ind.flow_id);
			u16 aid = (flow_id >> 8);

			/* Check for MAX client */
			if ((ndev_vif->peer_sta_records + 1) > SLSI_AP_PEER_CONNECTIONS_MAX) {
				SLSI_NET_ERR(dev, "MAX Station limit reached. Ignore ind for aid:%d\n", aid);
				goto exit_with_lock;
			}

			if (aid < SLSI_PEER_INDEX_MIN || aid > SLSI_PEER_INDEX_MAX) {
				SLSI_NET_ERR(dev, "Received incorrect aid: %d\n", aid);
				goto exit_with_lock;
			}

			slsi_spinlock_lock(&ndev_vif->peer_lock);
			peer = slsi_peer_add(sdev, dev, (fapi_get_mgmt(skb))->sa, aid);
			if (!peer) {
				slsi_spinlock_unlock(&ndev_vif->peer_lock);
				SLSI_NET_ERR(dev, "Peer NOT Created\n");
				goto exit_with_lock;
			}
			slsi_peer_update_assoc_req(sdev, dev, peer, skb);
			/* skb is consumed by slsi_peer_update_assoc_req. So do not access this anymore. */
			skb = NULL;
			peer->connected_state = SLSI_STA_CONN_STATE_CONNECTING;

			if (ndev_vif->iftype == NL80211_IFTYPE_P2P_GO &&
			    peer->assoc_ie &&
			    cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT, WLAN_OUI_TYPE_MICROSOFT_WPS, peer->assoc_ie->data, peer->assoc_ie->len)) {
				SLSI_NET_DBG2(dev, SLSI_MLME,  "WPS IE is present. Setting peer->is_wps to TRUE\n");
				peer->is_wps = true;
			}

			if (peer->assoc_ie)
				slsi_rx_send_update_owe_info_event(dev, peer->address,
								   peer->assoc_ie->data, peer->assoc_ie->len);
			slsi_spinlock_unlock(&ndev_vif->peer_lock);
			/* Take a wakelock to avoid platform suspend before
			 * EAPOL exchanges (to avoid connection delay)
			 */
			slsi_wake_lock_timeout(&sdev->wlan_wl_mlme, msecs_to_jiffies(SLSI_WAKELOCK_TIME_MSEC_EAPOL));
			break;
		}
		case FAPI_VIFTYPE_STATION:
		{
			peer = slsi_get_peer_from_qs(sdev, dev, SLSI_STA_PEER_QUEUESET);
			if (WLBT_WARN_ON(!peer)) {
				SLSI_NET_ERR(dev, "Peer NOT FOUND\n");
				goto exit_with_lock;
			}

			slsi_peer_update_assoc_req(sdev, dev, peer, skb);
			/* skb is consumed by slsi_peer_update_assoc_rsp. So do not access this anymore. */
			skb = NULL;
			break;
		}
		default:
			SLSI_NET_ERR(dev, "Incorrect vif type for proceduretype_connection_started\n");
			break;
		}
		break;
	case FAPI_PROCEDURETYPE_DEVICE_DISCOVERED:
		/* Expected only in P2P Device and P2P GO role */
		if (!SLSI_IS_VIF_INDEX_P2P(ndev_vif) && ndev_vif->iftype != NL80211_IFTYPE_P2P_GO) {
			SLSI_NET_DBG1(dev, SLSI_MLME, "PROCEDURETYPE_DEVICE_DISCOVERED recd in non P2P role\n");
			goto exit_with_lock;
		}
		/* Send probe request to supplicant only if in listening state. Issues were seen earlier if
		 * Probe request was sent to supplicant while waiting for GO Neg Req from peer.
		 * Send Probe request to supplicant if received in GO mode
		 */
		if (sdev->p2p_state == P2P_LISTENING || ndev_vif->iftype == NL80211_IFTYPE_P2P_GO)
			slsi_rx_p2p_device_discovered_ind(sdev, dev, skb);
		break;
	case FAPI_PROCEDURETYPE_ROAMING_STARTED:
	{
		SLSI_NET_DBG1(dev, SLSI_MLME, "Roaming Procedure Starting with %pM\n", (fapi_get_mgmt(skb))->bssid);
		if (WLBT_WARN_ON(ndev_vif->vif_type != FAPI_VIFTYPE_STATION))
			goto exit_with_lock;
		if (WLBT_WARN_ON(!ndev_vif->peer_sta_record[SLSI_STA_PEER_QUEUESET] || !ndev_vif->peer_sta_record[SLSI_STA_PEER_QUEUESET]->valid))
			goto exit_with_lock;
		kfree_skb(ndev_vif->sta.roam_mlme_procedure_started_ind);
		ndev_vif->sta.roam_mlme_procedure_started_ind = skb;
		/* skb is consumed here. So remove reference to this.*/
		skb = NULL;
		break;
	}
	default:
		SLSI_NET_DBG1(dev, SLSI_MLME, "Unknown Procedure: %d\n", fapi_get_u16(skb, u.mlme_procedure_started_ind.procedure_type));
		goto exit_with_lock;
	}

exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	rtnl_unlock();
	kfree_skb(skb);
}

void slsi_rx_frame_transmission_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif	*ndev_vif = netdev_priv(dev);
	struct slsi_peer	*peer;
	u16					host_tag = fapi_get_u16(skb, u.mlme_frame_transmission_ind.host_tag);
	u16					tx_status = fapi_get_u16(skb, u.mlme_frame_transmission_ind.transmission_status);
	bool				ack = true;

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	SLSI_NET_DBG2(dev, SLSI_MLME,
		      "vif:%d host_tag:0x%x transmission_status:%d\n",
		      fapi_get_vif(skb), host_tag, tx_status);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		kfree_skb(skb);
		return;
	}

#if defined(CONFIG_SCSC_WLAN_TX_API) || defined(CONFIG_SCSC_WLAN_ARP_FLOW_CONTROL)
#if defined(CONFIG_SCSC_WLAN_TX_API)
	slsi_tx_mlme_ind(sdev, dev, skb);
#else
	if (host_tag & SLSI_HOST_TAG_ARP_MASK) {
		if (atomic_read(&sdev->arp_tx_count) > 0)
			atomic_dec(&sdev->arp_tx_count);
		else
			SLSI_INFO(sdev, "Unable to decrement sdev arp_tx_count\n");
		if (atomic_read(&ndev_vif->arp_tx_count) > 0)
			atomic_dec(&ndev_vif->arp_tx_count);
		else
			SLSI_INFO(sdev, "Unable to decrement ndev arp_tx_count\n");

		if (atomic_read(&sdev->ctrl_pause_state) &&
		    atomic_read(&sdev->arp_tx_count) < (sdev->fw_max_arp_count - SLSI_ARP_UNPAUSE_THRESHOLD))
			scsc_wifi_unpause_arp_q_all_vif(sdev);
	}
#endif
#endif

	if (ndev_vif->mgmt_tx_data.host_tag == host_tag) {
		struct netdev_vif *ndev_vif_to_cfg = ndev_vif;

		/* If frame tx failed allow del_vif work to take care of vif deletion.
		 * This work would be queued as part of frame_tx with the wait duration
		 */
		if (tx_status != FAPI_TRANSMISSIONSTATUS_SUCCESSFUL) {
			ack = false;
			if (SLSI_IS_VIF_INDEX_WLAN(ndev_vif)) {
				if (sdev->wlan_unsync_vif_state == WLAN_UNSYNC_VIF_TX)
					sdev->wlan_unsync_vif_state = WLAN_UNSYNC_VIF_ACTIVE; /*We wouldn't delete VIF*/
			} else {
				if (sdev->p2p_group_exp_frame != SLSI_PA_INVALID)
					slsi_clear_offchannel_data(sdev, false);
				else if (ndev_vif->mgmt_tx_data.exp_frame != SLSI_PA_INVALID)
					(void)slsi_mlme_reset_dwell_time(sdev, dev);
				ndev_vif->mgmt_tx_data.exp_frame = SLSI_PA_INVALID;
			}
		}

		/* Change state if frame tx was in Listen as peer response is not expected */
		if (SLSI_IS_VIF_INDEX_P2P(ndev_vif) && ndev_vif->mgmt_tx_data.exp_frame == SLSI_PA_INVALID) {
			if (delayed_work_pending(&ndev_vif->unsync.roc_expiry_work))
				SLSI_P2P_STATE_CHANGE(sdev, P2P_LISTENING);
			else
				SLSI_P2P_STATE_CHANGE(sdev, P2P_IDLE_VIF_ACTIVE);
		} else if (SLSI_IS_VIF_INDEX_P2P_GROUP(sdev, ndev_vif)) {
			const struct ieee80211_mgmt *mgmt = (const struct ieee80211_mgmt *)ndev_vif->mgmt_tx_data.buf;

			/* If frame transmission was initiated on P2P device vif by supplicant,
			 * then use the net_dev of that vif (i.e. p2p0)
			 */
			if ((mgmt) && (memcmp(mgmt->sa, dev->dev_addr, ETH_ALEN) != 0)) {
				struct net_device *ndev = slsi_get_netdev(sdev, SLSI_NET_INDEX_P2P);

				SLSI_NET_DBG2(dev, SLSI_MLME,
					      "Frame Tx was requested with device address"
					      " - Change ndev_vif for tx_status\n");

				ndev_vif_to_cfg = netdev_priv(ndev);
				if (!ndev_vif_to_cfg) {
					SLSI_NET_ERR(dev, "Getting P2P Index netdev failed\n");
					ndev_vif_to_cfg = ndev_vif;
				}
			}
		}
#ifdef CONFIG_SCSC_WLAN_WES_NCHO
		if (!sdev->device_config.wes_mode) {
#endif
			cfg80211_mgmt_tx_status(&ndev_vif_to_cfg->wdev, ndev_vif->mgmt_tx_data.cookie, ndev_vif->mgmt_tx_data.buf, ndev_vif->mgmt_tx_data.buf_len, ack, GFP_KERNEL);
#ifdef CONFIG_SCSC_WLAN_WES_NCHO
		}
#endif
		(void)slsi_set_mgmt_tx_data(ndev_vif, 0, 0, NULL, 0);
	}

	if (tx_status == FAPI_TRANSMISSIONSTATUS_SUCCESSFUL || tx_status == FAPI_TRANSMISSIONSTATUS_RETRY_LIMIT) {
#ifdef CONFIG_SCSC_WLAN_STA_ENHANCED_ARP_DETECT
		if (ndev_vif->enhanced_arp_detect_enabled && ndev_vif->vif_type == FAPI_VIFTYPE_STATION) {
			int i = 0;

			for (i = 0; i < SLSI_MAX_ARP_SEND_FRAME; i++) {
				if (ndev_vif->enhanced_arp_host_tag[i] == host_tag) {
					ndev_vif->enhanced_arp_host_tag[i] = 0;
					ndev_vif->enhanced_arp_stats.arp_req_rx_count_by_lower_mac++;
					if (tx_status == FAPI_TRANSMISSIONSTATUS_SUCCESSFUL)
						ndev_vif->enhanced_arp_stats.arp_req_count_tx_success++;
					break;
				}
			}
		}
#endif
		if (ndev_vif->vif_type == FAPI_VIFTYPE_STATION &&
		    ndev_vif->sta.m4_host_tag == host_tag) {
			switch (ndev_vif->sta.resp_id) {
			case MLME_ROAMED_RES:
				slsi_mlme_roamed_resp(sdev, dev);
				peer = slsi_get_peer_from_qs(sdev, dev, SLSI_STA_PEER_QUEUESET);
				if (WLBT_WARN_ON(!peer))
					break;
				slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_CONNECTED);
				cac_update_roam_traffic_params(sdev, dev);
				break;
			case MLME_CONNECT_RES:
				slsi_mlme_connect_resp(sdev, dev);
				if (ndev_vif->ipaddress)
					slsi_ip_address_changed(sdev, dev, ndev_vif->ipaddress);
				slsi_set_acl(sdev, dev);
				slsi_set_packet_filters(sdev, dev);
				peer = slsi_get_peer_from_qs(sdev, dev, SLSI_STA_PEER_QUEUESET);
				if (WLBT_WARN_ON(!peer))
					break;
				slsi_ps_port_control(sdev, dev, peer, SLSI_STA_CONN_STATE_CONNECTED);
				break;
			case MLME_REASSOCIATE_RES:
				slsi_mlme_reassociate_resp(sdev, dev);
				break;
			default:
				break;
			}
			ndev_vif->sta.m4_host_tag = 0;
			ndev_vif->sta.resp_id = 0;
		}
		if (tx_status == FAPI_TRANSMISSIONSTATUS_RETRY_LIMIT) {
			if (ndev_vif->iftype == NL80211_IFTYPE_STATION &&
			    ndev_vif->sta.eap_hosttag == host_tag) {
				if (ndev_vif->sta.sta_bss) {
					SLSI_NET_WARN(dev, "Disconnect as EAP frame transmission failed\n");
					slsi_mlme_disconnect(sdev, dev, ndev_vif->sta.sta_bss->bssid, FAPI_REASONCODE_UNSPECIFIED_REASON, false);
				} else {
					SLSI_NET_WARN(dev, "EAP frame transmission failed, sta_bss not available\n");
				}
			}
			ndev_vif->stats.tx_errors++;
		}
	} else {
		ndev_vif->stats.tx_errors++;
	}
	slsi_eapol_eap_handle_tx_status(sdev, ndev_vif, host_tag, tx_status);
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

void slsi_rx_received_frame_logging(struct net_device *dev, struct sk_buff *skb, char *log_str_buffer, int buffer_size)
{
	u16 protocol = 0;
	u8 *eapol = NULL;
	u8 *eap = NULL;
	u16 eap_length = 0;
	u32 dhcp_message_type = SLSI_DHCP_MESSAGE_TYPE_INVALID;
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_dev *sdev = ndev_vif->sdev;

	if ((skb->len - sizeof(struct ethhdr)) >= 99)
		eapol = skb->data + sizeof(struct ethhdr);

	if ((skb->len - sizeof(struct ethhdr)) >= 8) {
		eap_length = (skb->len - sizeof(struct ethhdr)) - 4;
		eap = skb->data + sizeof(struct ethhdr);
	}
	if (skb->len >= 285 && slsi_is_dhcp_packet(skb->data) != SLSI_TX_IS_NOT_DHCP)
		dhcp_message_type = skb->data[284];

	skb->protocol = eth_type_trans(skb, dev);
	protocol = ntohs(skb->protocol);

	if (protocol == ETH_P_PAE) {
		if (eapol && eapol[SLSI_EAPOL_IEEE8021X_TYPE_POS] == SLSI_IEEE8021X_TYPE_EAPOL_KEY &&
		    (eapol[SLSI_EAPOL_KEY_INFO_LOWER_BYTE_POS] & SLSI_EAPOL_KEY_INFO_KEY_TYPE_BIT_IN_LOWER_BYTE)) {
			if ((eapol[SLSI_EAPOL_TYPE_POS] == SLSI_EAPOL_TYPE_RSN_KEY ||
			     eapol[SLSI_EAPOL_TYPE_POS] == SLSI_EAPOL_TYPE_WPA_KEY) &&
				(eapol[SLSI_EAPOL_KEY_INFO_HIGHER_BYTE_POS] &
				 SLSI_EAPOL_KEY_INFO_MIC_BIT_IN_HIGHER_BYTE) &&
				eapol[SLSI_EAPOL_KEY_DATA_LENGTH_HIGHER_BYTE_POS] == 0 &&
				eapol[SLSI_EAPOL_KEY_DATA_LENGTH_LOWER_BYTE_POS] == 0) {
				snprintf(log_str_buffer, buffer_size, "4way-H/S, M4");
			} else if (!(eapol[SLSI_EAPOL_KEY_INFO_HIGHER_BYTE_POS] &
				   SLSI_EAPOL_KEY_INFO_MIC_BIT_IN_HIGHER_BYTE)) {
				snprintf(log_str_buffer, buffer_size, "4way-H/S, M1");
				slsi_conn_log2us_eapol_ptk(sdev, dev, 1);
			} else if ((eapol[SLSI_EAPOL_KEY_INFO_HIGHER_BYTE_POS] &
				   SLSI_EAPOL_KEY_INFO_SECURE_BIT_IN_HIGHER_BYTE)) {
				snprintf(log_str_buffer, buffer_size, "4way-H/S, M3");
				slsi_conn_log2us_eapol_ptk(sdev, dev, 3);
			} else {
				snprintf(log_str_buffer, buffer_size, "4way-H/S, M2");
			}
		} else if (eapol && eapol[SLSI_EAPOL_IEEE8021X_TYPE_POS] == SLSI_IEEE8021X_TYPE_EAPOL_KEY &&
			   !(eapol[SLSI_EAPOL_KEY_INFO_LOWER_BYTE_POS] &
			   SLSI_EAPOL_KEY_INFO_KEY_TYPE_BIT_IN_LOWER_BYTE)) {
			if ((eapol[SLSI_EAPOL_KEY_INFO_HIGHER_BYTE_POS] &
			    SLSI_EAPOL_KEY_INFO_SECURE_BIT_IN_HIGHER_BYTE) &&
			    (eapol[SLSI_EAPOL_KEY_INFO_LOWER_BYTE_POS] &
			    SLSI_EAPOL_KEY_INFO_ACK_BIT_IN_LOWER_BYTE)) {
				snprintf(log_str_buffer, buffer_size, "GTK-H/S, M1");
				slsi_conn_log2us_eapol_gtk(sdev, dev, 1);
			} else {
				snprintf(log_str_buffer, buffer_size, "GTK-H/S, M2");
			}
		} else if (eap && eap[SLSI_EAPOL_IEEE8021X_TYPE_POS] == SLSI_IEEE8021X_TYPE_EAP_PACKET) {
			switch (eap[SLSI_EAP_CODE_POS]) {
			case SLSI_EAP_PACKET_REQUEST:
				snprintf(log_str_buffer, buffer_size, "EAP-Request (%d)", eap_length);
				slsi_conn_log2us_eap_with_len(sdev, dev, eap, eap_length);
				break;
			case SLSI_EAP_PACKET_RESPONSE:
				snprintf(log_str_buffer, buffer_size, "EAP-Response (%d)", eap_length);
				break;
			case SLSI_EAP_PACKET_SUCCESS:
				snprintf(log_str_buffer, buffer_size, "EAP-Success (%d)", eap_length);
				slsi_conn_log2us_eap(sdev, dev, eap);
				break;
			case SLSI_EAP_PACKET_FAILURE:
				snprintf(log_str_buffer, buffer_size, "EAP-Failure (%d)", eap_length);
				slsi_conn_log2us_eap(sdev, dev, eap);
				break;
			}
		}
	} else if (protocol == ETH_P_IP) {
		switch (dhcp_message_type) {
		case SLSI_DHCP_MESSAGE_TYPE_DISCOVER:
			snprintf(log_str_buffer, buffer_size, "DHCP [DISCOVER]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_OFFER:
			snprintf(log_str_buffer, buffer_size, "DHCP [OFFER]");
			slsi_conn_log2us_dhcp(sdev, dev, "OFFER");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_REQUEST:
			snprintf(log_str_buffer, buffer_size, "DHCP [REQUEST]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_DECLINE:
			snprintf(log_str_buffer, buffer_size, "DHCP [DECLINE]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_ACK:
			snprintf(log_str_buffer, buffer_size, "DHCP [ACK]");
			slsi_conn_log2us_dhcp(sdev, dev, "ACK");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_NAK:
			snprintf(log_str_buffer, buffer_size, "DHCP [NAK]");
			slsi_conn_log2us_dhcp(sdev, dev, "NAK");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_RELEASE:
			snprintf(log_str_buffer, buffer_size, "DHCP [RELEASE]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_INFORM:
			snprintf(log_str_buffer, buffer_size, "DHCP [INFORM]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_FORCERENEW:
			snprintf(log_str_buffer, buffer_size, "DHCP [FORCERENEW]");
			break;
		case SLSI_DHCP_MESSAGE_TYPE_INVALID:
			SLSI_DBG1(sdev, SLSI_RX, "Received IP pkt but not DHCP\n");
			break;
		default:
			snprintf(log_str_buffer, buffer_size, "DHCP [INVALID]");
			break;
		}
	}
}

void slsi_rx_received_frame_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u16 data_unit_descriptor = fapi_get_u16(skb, u.mlme_received_frame_ind.data_unit_descriptor);
	u16 frequency = SLSI_FREQ_FW_TO_HOST(fapi_get_u16(skb, u.mlme_received_frame_ind.channel_frequency));
	int subtype = SLSI_PA_INVALID;
	char log_str_buffer[128] = {0};
	struct sk_buff *log_skb = NULL;
	bool is_dropped = false;

	SLSI_NET_DBG2(dev, SLSI_MLME, "mlme_received_frame_ind(vif:%d, data descriptor:%d, freq:%d)\n",
		      fapi_get_vif(skb),
		      data_unit_descriptor,
		      frequency);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (data_unit_descriptor == FAPI_DATAUNITDESCRIPTOR_IEEE802_11_FRAME) {
		struct ieee80211_mgmt *mgmt;
		int mgmt_len;

		mgmt_len = fapi_get_mgmtlen(skb);
		if (mgmt_len < offsetof(struct ieee80211_mgmt, frame_control) + 2) {
			SLSI_NET_ERR(dev, "invalid fapi mgmt data\n");
			goto exit;
		}
		mgmt = fapi_get_mgmt(skb);
		if (ieee80211_is_auth(mgmt->frame_control)) {
			cfg80211_rx_mgmt(&ndev_vif->wdev, frequency, 0, (const u8 *)mgmt, mgmt_len, GFP_ATOMIC);

			if (ndev_vif->vif_type == FAPI_VIFTYPE_STATION)
				if (mgmt->u.auth.auth_transaction == ndev_vif->sta.sae_auth_type)
					(void)slsi_mlme_reset_dwell_time(sdev, dev);

			SLSI_INFO(sdev, "Received Auth Frame\n");
			goto exit;
		}
		/* Populate wake reason stats here */
		if (unlikely(slsi_skb_cb_get(skb)->wakeup)) {
			schedule_work(&sdev->wakeup_time_work);
			slsi_rx_update_mlme_stats(sdev, skb);
		}

#if defined(CONFIG_SLSI_WLAN_STA_FWD_BEACON) && (defined(SCSC_SEP_VERSION) && SCSC_SEP_VERSION >= 10)
		if (ieee80211_is_beacon(mgmt->frame_control)) {
			if (!ndev_vif->is_wips_running) {
				SLSI_NET_WARN(dev, "Unwanted beacon is forwarded");
				slsi_mlme_set_forward_beacon(sdev, dev, false);
			} else if (SLSI_IS_VIF_INDEX_WLAN(ndev_vif)) {
				slsi_handle_wips_beacon(sdev, dev, skb, mgmt, mgmt_len);
			}

			goto exit;
		}
#endif
		if (!(ieee80211_is_action(mgmt->frame_control))) {
			SLSI_NET_ERR(dev, "Expected an Action Frame\n");
			goto exit;
		}

		subtype = slsi_get_public_action_subtype(mgmt);
		if (SLSI_IS_VIF_INDEX_WLAN(ndev_vif)) {
#ifdef CONFIG_SCSC_WLAN_WES_NCHO
			if (slsi_is_wes_action_frame(mgmt)) {
				SLSI_NET_DBG1(dev, SLSI_CFG80211, "Received NCHO WES VS action frame\n");
				if (!sdev->device_config.wes_mode)
					goto exit;
			} else {
#endif
				if (mgmt->u.action.category == WLAN_CATEGORY_WMM) {
					cac_rx_wmm_action(sdev, dev, mgmt, mgmt_len);
			} else {
				slsi_wlan_dump_public_action_subtype(sdev, mgmt, false);
				if (sdev->wlan_unsync_vif_state == WLAN_UNSYNC_VIF_TX)
					sdev->wlan_unsync_vif_state = WLAN_UNSYNC_VIF_ACTIVE;
				if (ndev_vif->mgmt_tx_data.exp_frame != SLSI_PA_INVALID && subtype == ndev_vif->mgmt_tx_data.exp_frame) {
					ndev_vif->mgmt_tx_data.exp_frame = SLSI_PA_INVALID;
					(void)slsi_mlme_reset_dwell_time(sdev, dev);
				}
			}
#ifdef CONFIG_SCSC_WLAN_WES_NCHO
			}
#endif
		} else {
			SLSI_NET_DBG2(dev, SLSI_CFG80211, "Received action frame (%s)\n", slsi_pa_subtype_text(subtype));

			if (SLSI_IS_P2P_UNSYNC_VIF(ndev_vif) && ndev_vif->mgmt_tx_data.exp_frame != SLSI_PA_INVALID &&
			    subtype == ndev_vif->mgmt_tx_data.exp_frame) {
				if (sdev->p2p_state == P2P_LISTENING)
					SLSI_NET_WARN(dev, "Driver in incorrect P2P state (P2P_LISTENING)");

				cancel_delayed_work(&ndev_vif->unsync.del_vif_work);
				/* Sending down the Unset channel is delayed when listen
				 * work expires in middle of P2P procedure. For example,
				 * When Listen work expires after sending provision
				 * discovery req,unset channel is not sent to FW. After
				 * receiving the PROV_DISC_RESP, if listen work is not
				 * present, unset channel to be sent down. Similarly,
				 * during P2P Negotiation procedure, unset channel is
				 * not sent to FW. Once Negotiation is complete if listen
				 * work is not present Unset channel to be sent down.
				 */
				if (subtype == SLSI_P2P_PA_GO_NEG_CFM || subtype == SLSI_P2P_PA_PROV_DISC_RSP) {
					ndev_vif->drv_in_p2p_procedure = false;
					if (!delayed_work_pending(&ndev_vif->unsync.roc_expiry_work)) {
						if (delayed_work_pending(&ndev_vif->unsync.unset_channel_expiry_work))
							cancel_delayed_work(&ndev_vif->unsync.unset_channel_expiry_work);
						queue_delayed_work(ndev_vif->sdev->device_wq, &ndev_vif->unsync.unset_channel_expiry_work,
								   msecs_to_jiffies(SLSI_P2P_DELAY_UNSET_CHANNEL_AFTER_P2P_PROCEDURE));
					} else {
						(void)slsi_mlme_reset_dwell_time(sdev, dev);
					}
				} else {
					/* reset dwell time in not required when unset channel
					 * is being queued.
					 */
					(void)slsi_mlme_reset_dwell_time(sdev, dev);
				}

				ndev_vif->mgmt_tx_data.exp_frame = SLSI_PA_INVALID;
				if (delayed_work_pending(&ndev_vif->unsync.roc_expiry_work)) {
					SLSI_P2P_STATE_CHANGE(sdev, P2P_LISTENING);
				} else {
					queue_delayed_work(sdev->device_wq, &ndev_vif->unsync.del_vif_work,
							   msecs_to_jiffies(SLSI_P2P_UNSYNC_VIF_EXTRA_MSEC));
					SLSI_P2P_STATE_CHANGE(sdev, P2P_IDLE_VIF_ACTIVE);
				}
				/* Case when previous P2P mgmt TX has been sucessful but
				 * we do not receive the transmission indication instead
				 * received the expected received frame. Indicating the
				 * upper layer explicitly that last P2P mgmt tx is
				 * successful and clearing the stored mgmt_tx_data and
				 * processing the received frame.
				 */
				if (ndev_vif->mgmt_tx_data.host_tag) {
					cfg80211_mgmt_tx_status(&ndev_vif->wdev, ndev_vif->mgmt_tx_data.cookie, ndev_vif->mgmt_tx_data.buf, ndev_vif->mgmt_tx_data.buf_len, true, GFP_KERNEL);
					(void)slsi_set_mgmt_tx_data(ndev_vif, 0, 0, NULL, 0);
				}
			} else if ((sdev->p2p_group_exp_frame != SLSI_PA_INVALID) && (sdev->p2p_group_exp_frame == subtype)) {
				SLSI_NET_DBG2(dev, SLSI_MLME, "Expected action frame (%s) received on Group VIF\n", slsi_pa_subtype_text(subtype));
				slsi_clear_offchannel_data(sdev,
							   (!SLSI_IS_VIF_INDEX_P2P_GROUP(sdev,
											 ndev_vif)) ? true : false);
			/* Case to handle when we don't receive the transmission indication
			 * of SLSI_P2P_PA_GO_NEG_RSP with status != SLSI_P2P_STATUS_CODE_SUCCESS
			 * and we received SLSI_P2P_PA_GO_NEG_REQ directly from peer,
			 * assuming the last P2P mgmt tx of SLSI_P2P_PA_GO_NEG_RSP is
			 * success. This can only happen when listen channels are same on
			 * both the devices.
			 */
			} else if (SLSI_IS_P2P_UNSYNC_VIF(ndev_vif) && (ndev_vif->mgmt_tx_data.exp_frame == SLSI_PA_INVALID) && (subtype == SLSI_P2P_PA_GO_NEG_REQ)) {
				if (ndev_vif->mgmt_tx_data.host_tag) {
					if (delayed_work_pending(&ndev_vif->unsync.roc_expiry_work))
						SLSI_P2P_STATE_CHANGE(sdev, P2P_LISTENING);
					else
						SLSI_P2P_STATE_CHANGE(sdev, P2P_IDLE_VIF_ACTIVE);
					cfg80211_mgmt_tx_status(&ndev_vif->wdev, ndev_vif->mgmt_tx_data.cookie, ndev_vif->mgmt_tx_data.buf, ndev_vif->mgmt_tx_data.buf_len, true, GFP_KERNEL);
					(void)slsi_set_mgmt_tx_data(ndev_vif, 0, 0, NULL, 0);
				}
			}
		}
		cfg80211_rx_mgmt(&ndev_vif->wdev, frequency, 0, (const u8 *)mgmt, mgmt_len, GFP_ATOMIC);
	} else if (data_unit_descriptor == FAPI_DATAUNITDESCRIPTOR_IEEE802_3_FRAME) {
		struct slsi_peer *peer = NULL;
		struct ethhdr *ehdr = (struct ethhdr *)fapi_get_data(skb);
		bool delayed = (fapi_get_u16(skb, u.mlme_received_frame_ind.spare_1) & FAPI_OPTION_DELAYED);
		bool last_pkt = (fapi_get_u16(skb, u.mlme_received_frame_ind.spare_1) & FAPI_OPTION_LAST);

		if (delayed && last_pkt) {
			sdev->last_delayd_pkt.pkt_size = (skb->len - fapi_get_siglen(skb) > MAX_LAST_DELAYD_PKT_SIZE) ?
							 MAX_LAST_DELAYD_PKT_SIZE : skb->len - fapi_get_siglen(skb);
			if (sdev->last_delayd_pkt.pkt_size)
				memcpy(sdev->last_delayd_pkt.pkt, ehdr, sdev->last_delayd_pkt.pkt_size);
		}

		/* Populate wake reason stats here */
		if (unlikely(slsi_skb_cb_get(skb)->wakeup)) {
			schedule_work(&sdev->wakeup_time_work);
			skb->mark = SLSI_WAKEUP_PKT_MARK;
			slsi_rx_update_wake_stats(sdev, ehdr, skb->len - fapi_get_siglen(skb), skb);
		}

		if (fapi_get_datalen(skb) < sizeof(struct ethhdr)) {
			SLSI_DBG1(sdev, SLSI_RX, "invalid fapi data length.\n");
			goto exit;
		}

		peer = slsi_get_peer_from_mac(sdev, dev, ehdr->h_source);
		if (!peer) {
			SLSI_DBG1(sdev, SLSI_RX, "drop packet as No peer found\n");
			goto exit;
		}

		/* skip BA engine if the destination address is Multicast */
		if (ndev_vif->vif_type == FAPI_VIFTYPE_STATION && (is_multicast_ether_addr(ehdr->h_dest))) {
			SLSI_NET_DBG2(dev, SLSI_RX, "multicast/broadcast packet received in STA mode skip BA\n");
			/* skip BA check */
			goto ba_check_done;
		}

		/*
		 * Check if frame belongs to an established Block Ack. If so, we don't want to buffer this
		 * MLME frame in BA, and also don't want the BA engine to wait for this frame either.
		 * So just update the BA receive window, and pass the frame to upper processes as it was
		 *
		 * Note: the change applies only to Control_SAP hipVersions=14.6.13 or later
		 */
		if ((FAPI_MAJOR_VERSION(FAPI_CONTROL_SAP_VERSION) >= 0x0e) &&
			(FAPI_MINOR_VERSION(FAPI_CONTROL_SAP_VERSION) >= 0x06) &&
			(FAPI_CONTROL_SAP_ENG_VERSION >= 0x0d)) {
			u16 seq_num;
			u16 priority;

			seq_num = fapi_get_u16(skb, u.mlme_received_frame_ind.sequence_number);
			priority = fapi_get_u16(skb, u.mlme_received_frame_ind.user_priority);

			SLSI_NET_DBG2(dev, SLSI_RX, "mlme_received_frame_ind(vif:%d, dest:%pM, src:%pM, priority:%d, s:%d delayed:%d last:%d\n",
					  fapi_get_vif(skb),
					  ehdr->h_dest,
					  ehdr->h_source,
					  priority,
					  seq_num, delayed, last_pkt);

			if (slsi_ba_check(peer, priority)) {
				slsi_ba_update_window(dev, peer->ba_session_rx[priority], ((seq_num + 1) & 0xFFF));
			}
		}
ba_check_done:
		/* strip signal and any signal/bulk roundings/offsets */
		skb_pull(skb, fapi_get_siglen(skb));

		skb->dev = dev;
		skb->ip_summed = CHECKSUM_NONE;

		ndev_vif->stats.rx_packets++;
		ndev_vif->stats.rx_bytes += skb->len;
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(4, 10, 0))
		dev->last_rx = jiffies;
#endif
		/* Storing Data for Logging Information */
		slsi_rx_received_frame_logging(dev, skb, log_str_buffer, sizeof(log_str_buffer));
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		SLSI_DBG2(sdev, SLSI_MLME, "pass %u bytes up (proto:%d)\n", skb->len, ntohs(skb->protocol));
		slsi_skb_cb_init(skb);
		log_skb = skb_copy(skb, GFP_ATOMIC);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
		is_dropped = (NET_RX_DROP == netif_rx(skb));
#else
		is_dropped = (NET_RX_DROP == netif_rx_ni(skb));
#endif
		if (log_str_buffer[0])
			SLSI_INFO(sdev, "%s %s\n", (is_dropped ? "Dropped" : "Received"), log_str_buffer);
		if (log_skb) {
			if (is_dropped)
				SLSI_INFO_HEX_NODEV(log_skb->data,
						    (log_skb->len < 128 ? log_skb->len : 128), "HEX Dump:\n");
			kfree_skb(log_skb);
		}
		slsi_wake_lock_timeout(&sdev->wlan_wl_mlme, msecs_to_jiffies(SLSI_WAKELOCK_TIME_MSEC_EAPOL));
		return;
	}
exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

void slsi_rx_mic_failure_ind(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u8 *mac_addr;
	u16 key_type, key_id;
	enum nl80211_key_type nl_key_type;

	SLSI_UNUSED_PARAMETER(sdev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit;
	}

	mac_addr = fapi_get_buff(skb, u.mlme_disconnected_ind.peer_sta_address);
	key_type = fapi_get_u16(skb, u.mlme_mic_failure_ind.key_type);
	key_id = fapi_get_u16(skb, u.mlme_mic_failure_ind.key_id);

	SLSI_NET_DBG1(dev, SLSI_MLME, "mlme_mic_failure_ind(vif:%d, MAC:%pM, key_type:%d, key_id:%d)\n",
		      fapi_get_vif(skb), mac_addr, key_type, key_id);

	if (WLBT_WARN_ON(key_type != FAPI_KEYTYPE_GROUP && key_type != FAPI_KEYTYPE_PAIRWISE))
		goto exit;

	nl_key_type = (key_type == FAPI_KEYTYPE_GROUP) ? NL80211_KEYTYPE_GROUP : NL80211_KEYTYPE_PAIRWISE;

	cfg80211_michael_mic_failure(dev, mac_addr, nl_key_type, key_id, NULL, GFP_KERNEL);

exit:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

/**
 * Handler for mlme_listen_end_ind.
 * The listen_end_ind would be received when the total Listen Offloading time is over.
 * Indicate completion of Listen Offloading to supplicant by sending Cancel-ROC event
 * with cookie 0xffff. Queue delayed work for unsync vif deletion.
 */
void slsi_rx_listen_end_ind(struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	SLSI_NET_DBG2(dev, SLSI_CFG80211, "Inform completion of P2P Listen Offloading\n");
	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
		kfree_skb(skb);
		return;
	}

	cfg80211_remain_on_channel_expired(&ndev_vif->wdev, 0xffff, ndev_vif->chan, GFP_KERNEL);

	ndev_vif->unsync.listen_offload = false;

	slsi_p2p_queue_unsync_vif_del_work(ndev_vif, SLSI_P2P_UNSYNC_VIF_EXTRA_MSEC);

	SLSI_P2P_STATE_CHANGE(ndev_vif->sdev, P2P_IDLE_VIF_ACTIVE);

	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}

static int slsi_rx_wait_ind_match(u16 recv_id, u16 wait_id)
{
	if (recv_id == wait_id)
		return 1;
	return 0;
}

#if defined(CONFIG_SCSC_WLAN_TAS)
static bool slsi_rx_tas_drop_cfm(u16 id, u16 pid)
{
	/* TAS SAR.req signal uses no-cfm and cfm both.
	 * In order to use SAR.req on BH disabled context (ndo_start_tx), cfm signal
	 * should be dropped and in this case, there is no reason to need CFM.
	 */
	if (pid == SLSI_TX_PROCESS_ID_TAS_NO_CFM && id == MLME_SAR_CFM)
		return true;
	return false;
}
#endif

int slsi_rx_blocking_signals(struct slsi_dev *sdev, struct sk_buff *skb)
{
	u16 pid, id;
	struct slsi_sig_send *sig_wait;
	u16 vif = fapi_get_vif(skb);

	sig_wait = &sdev->sig_wait;
	id = fapi_get_sigid(skb);
	pid = fapi_get_u16(skb, receiver_pid);

	/* ALL mlme cfm signals MUST have blocking call waiting for it (Per Vif or Global) */
	if (fapi_is_cfm(skb)) {
		struct net_device *dev;
		struct netdev_vif *ndev_vif;

#if defined(CONFIG_SCSC_WLAN_TAS)
		if (slsi_rx_tas_drop_cfm(id, pid))
			return 0;
#endif
		rcu_read_lock();
		if (vif == SLSI_NET_INDEX_DETECT &&
		    (id == MLME_ADD_VIF_CFM ||
		     id == MLME_START_DETECT_CFM ||
		     id == MLME_DEL_VIF_CFM))
			vif = 1;

		/* Route vif index to 1 if monitor mode is enabled */
		if (sdev->monitor_mode && vif == SLSI_NET_INDEX_MONITOR2)
			vif = SLSI_NET_INDEX_MONITOR;

		dev = slsi_get_netdev_rcu(sdev, vif);
		if (dev) {
			ndev_vif = netdev_priv(dev);
			sig_wait = &ndev_vif->sig_wait;
		}
		spin_lock_bh(&sig_wait->send_signal_lock);
		if (id == sig_wait->cfm_id && pid == sig_wait->process_id) {
			if (WLBT_WARN_ON(sig_wait->cfm))
				kfree_skb(sig_wait->cfm);
			sig_wait->cfm = skb;
			spin_unlock_bh(&sig_wait->send_signal_lock);
			complete(&sig_wait->completion);
			rcu_read_unlock();
			return 0;
		}
		/**
		 * Important data frames such as EAPOL, ARP, DHCP are send
		 * over MLME. For these frames driver does not block on confirms.
		 * So there can be unexpected confirms here for such data frames.
		 * These confirms are treated as normal.
		 * Incase of ARP, for ARP flow control this needs to be sent to mlme
		 */
		if (id != MLME_SEND_FRAME_CFM)
			SLSI_DBG1(sdev, SLSI_MLME, "Unexpected cfm(0x%.4x, pid:0x%.4x, vif:%d)\n", id, pid, vif);
		spin_unlock_bh(&sig_wait->send_signal_lock);
		rcu_read_unlock();
		return -EINVAL;
	}
	/* Some mlme ind signals have a blocking call waiting (Per Vif or Global) */
	if (fapi_is_ind(skb)) {
		struct net_device *dev;
		struct netdev_vif *ndev_vif;

		rcu_read_lock();
		dev = slsi_get_netdev_rcu(sdev, vif);
		if (dev) {
			ndev_vif = netdev_priv(dev);
			sig_wait = &ndev_vif->sig_wait;
		}
		spin_lock_bh(&sig_wait->send_signal_lock);
		if (slsi_rx_wait_ind_match(id, sig_wait->ind_id) && pid == sig_wait->process_id) {
			if (WLBT_WARN_ON(sig_wait->ind))
				kfree_skb(sig_wait->ind);
			sig_wait->ind = skb;
			spin_unlock_bh(&sig_wait->send_signal_lock);
			complete(&sig_wait->completion);
			rcu_read_unlock();
			return 0;
		}
		spin_unlock_bh(&sig_wait->send_signal_lock);
		rcu_read_unlock();
	}
	return -EINVAL;
}

#if !defined(CONFIG_SCSC_WLAN_TX_API) && defined(CONFIG_SCSC_WLAN_ARP_FLOW_CONTROL)
void slsi_rx_send_frame_cfm_async(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	u16               host_tag = fapi_get_u16(skb, u.mlme_send_frame_cfm.host_tag);
	u16               req_status = fapi_get_u16(skb, u.mlme_send_frame_cfm.result_code);

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);
	if (host_tag & SLSI_HOST_TAG_ARP_MASK && req_status != FAPI_RESULTCODE_SUCCESS) {
		if (atomic_read(&sdev->arp_tx_count) > 0)
			atomic_dec(&sdev->arp_tx_count);
		else
			SLSI_INFO(sdev, "Unable to decrement sdev arp_tx_count\n");
		if (atomic_read(&ndev_vif->arp_tx_count) > 0)
			atomic_dec(&ndev_vif->arp_tx_count);
		else
			SLSI_INFO(sdev, "Unable to decrement ndev arp_tx_count\n");
		if (atomic_read(&sdev->ctrl_pause_state) &&
		    atomic_read(&sdev->arp_tx_count) < (sdev->fw_max_arp_count - SLSI_ARP_UNPAUSE_THRESHOLD))
			scsc_wifi_unpause_arp_q_all_vif(sdev);
		SLSI_NET_DBG2(dev, SLSI_MLME,
			      "vif:%d host_tag:0x%x req_status:%d\n",
			      fapi_get_vif(skb), host_tag, req_status);
	}
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}
#endif

void slsi_rx_twt_setup_info_event(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct slsi_twt_setup_event setup_event;
	u8 status = 1, setup_id_idx;

	setup_event.setup_id = fapi_get_u16(skb, u.mlme_twt_setup_ind.twt_setup_id);

	switch (fapi_get_u16(skb, u.mlme_twt_setup_ind.result_code)) {
	case FAPI_RESULTCODE_SUCCESS:
		status = 0;
		setup_event.reason_code = TWT_SETUP_EVENT_SUCCESS;
		break;
	case FAPI_RESULTCODE_TWT_SETUP_REJECTED:
		setup_event.reason_code = TWT_SETUP_EVENT_REJECTED;
		break;
	case FAPI_RESULTCODE_TWT_SETUP_TIMEOUT:
		setup_event.reason_code = TWT_SETUP_EVENT_TIMEOUT;
		break;
	case FAPI_RESULTCODE_TWT_SETUP_INVALID_IE:
		setup_event.reason_code = TWT_SETUP_EVENT_INVALID_IE;
		break;
	case FAPI_RESULTCODE_TWT_SETUP_PARAMS_VALUE_REJECTED:
		setup_event.reason_code = TWT_SETUP_EVENT_PARAMS_VALUE_REJECTED;
		break;
	case FAPI_RESULTCODE_TWT_SETUP_AP_NO_TWT_INFO:
		setup_event.reason_code = TWT_SETUP_EVENT_AP_NO_TWT_INFO;
		break;
	default:
		setup_event.reason_code = TWT_RESULTCODE_UNKNOWN;
	}
	if (fapi_get_u16(skb, u.mlme_twt_setup_ind.result_code) != FAPI_RESULTCODE_SUCCESS) {
		for (setup_id_idx = 0; setup_id_idx < SLSI_MAX_NUMBER_SETUP_ID; setup_id_idx++) {
			if (sdev->twt_setup_id[setup_id_idx] == setup_event.setup_id) {
				sdev->twt_setup_id[setup_id_idx] = 0;
				break;
			}
		}
	}
	setup_event.negotiation_type = fapi_get_u16(skb, u.mlme_twt_setup_ind.twt_negotiation_type);
	setup_event.flow_type = fapi_get_u16(skb, u.mlme_twt_setup_ind.twt_flow_type);
	setup_event.triggered_type = fapi_get_u16(skb, u.mlme_twt_setup_ind.twt_trigger_type);
	setup_event.wake_time = fapi_get_u32(skb, u.mlme_twt_setup_ind.twt_wake_time);
	setup_event.wake_duration = fapi_get_u32(skb, u.mlme_twt_setup_ind.twt_wake_duration);
	setup_event.wake_interval = fapi_get_u32(skb, u.mlme_twt_setup_ind.twt_wake_interval);

	slsi_send_twt_setup_event(sdev, dev,  setup_event);
	kfree_skb(skb);
}

void slsi_rx_twt_teardown_indication(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	u16 setup_id;
	u8  reason_code, setup_id_idx;

	setup_id = fapi_get_u16(skb, u.mlme_twt_teardown_ind.twt_setup_id);

	switch (fapi_get_u16(skb, u.mlme_twt_teardown_ind.reason_code)) {
	case FAPI_REASONCODE_HOST_INITIATED:
		reason_code = TWT_TEARDOWN_HOST_INITIATED;
		break;
	case FAPI_REASONCODE_PEER_INITIATED:
		reason_code = TWT_TEARDOWN_PEER_INITIATED;
		break;
	case FAPI_REASONCODE_CONCURRENT_OPERATION_SAME_BAND:
		reason_code = TWT_TEARDOWN_CONCURRENT_OPERATION_SAME_BAND;
		break;
	case FAPI_REASONCODE_CONCURRENT_OPERATION_DIFFERENT_BAND:
		reason_code = TWT_TEARDOWN_CONCURRENT_OPERATION_DIFFERENT_BAND;
		break;
	case FAPI_REASONCODE_ROAMING_OR_ECSA:
		reason_code = TWT_TEARDOWN_ROAMING_OR_ECSA;
		break;
	case FAPI_REASONCODE_BT_COEX:
		reason_code = TWT_TEARDOWN_BT_COEX;
		break;
	case FAPI_REASONCODE_TIMEOUT:
		reason_code = TWT_TEARDOWN_TIMEOUT;
		break;
	case FAPI_REASONCODE_PS_DISABLE:
		reason_code = TWT_TEARDOWN_PS_DISABLE;
		break;
	default:
		reason_code = TWT_RESULTCODE_UNKNOWN;
	}
	if (!setup_id) {
		memset(sdev->twt_setup_id, 0, SLSI_MAX_NUMBER_SETUP_ID * sizeof(int));
	} else {
		for (setup_id_idx = 0; setup_id_idx < SLSI_MAX_NUMBER_SETUP_ID; setup_id_idx++) {
			if (sdev->twt_setup_id[setup_id_idx] == setup_id) {
				sdev->twt_setup_id[setup_id_idx] = 0;
				break;
			}
		}
	}
	slsi_send_twt_teardown(sdev, dev, setup_id, reason_code);
	kfree_skb(skb);
}

void slsi_rx_twt_notification_indication(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	slsi_send_twt_notification(sdev, dev);
	kfree_skb(skb);
}

int slsi_send_scheduled_pm_teardown_ind(struct slsi_dev *sdev, struct net_device *dev, u16 result_code)
{
	struct sk_buff    *skb = NULL;
	u8                err = 0;
	struct netdev_vif *ndev_vif = netdev_priv(dev);

	skb = cfg80211_vendor_event_alloc(sdev->wiphy, &ndev_vif->wdev, NLMSG_DEFAULT_SIZE,
					  SLSI_NL80211_VENDOR_SCHED_PM_TEARDOWN_EVENT, GFP_KERNEL);
	if (!skb) {
		SLSI_ERR_NODEV("Failed to allocate skb for schedule pm teardown\n");
		return -ENOMEM;
	}
	err = nla_put_u8(skb, SLSI_VENDOR_ATTR_SCHED_PM_TEARDOWN_RESULT_CODE, result_code);
	if (err) {
		SLSI_ERR_NODEV("Failed nla_put err=%d\n", err);
		kfree_skb(skb);
		return -EINVAL;
	}
	SLSI_DBG1(sdev, SLSI_CFG80211, "Event: SLSI_NL80211_VENDOR_SCHED_PM_TEARDOWN_EVENT\n");
	cfg80211_vendor_event(skb, GFP_KERNEL);
	return 0;
}

void slsi_rx_scheduled_pm_teardown_indication(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	u8  reason_code;

	reason_code = fapi_get_u16(skb, u.mlme_scheduled_pm_teardown_ind.reason_code);

	slsi_send_scheduled_pm_teardown_ind(sdev, dev, reason_code);
	kfree_skb(skb);
}

void slsi_rx_scheduled_pm_leaky_ap_detect_indication(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct sk_buff    *event_skb = NULL;

	event_skb = cfg80211_vendor_event_alloc(sdev->wiphy, &ndev_vif->wdev, NLMSG_DEFAULT_SIZE,
					  SLSI_NL80211_VENDOR_SCHED_PM_LEAKY_AP_DETECT_EVENT, GFP_KERNEL);
	if (!event_skb) {
		SLSI_ERR_NODEV("Failed to allocate skb for schedule pm leaky ap detect\n");
		return;
	}
	SLSI_DBG1(sdev, SLSI_CFG80211, "Event: SLSI_NL80211_VENDOR_SCHED_PM_LEAKY_AP_DETECT_EVENT\n");
	cfg80211_vendor_event(event_skb, GFP_KERNEL);
	kfree_skb(skb);
}

void slsi_rx_delayed_wakeup_indication(struct slsi_dev *sdev, struct net_device *dev,
				       struct sk_buff *skb)
{
	struct slsi_delayed_wakeup_ind delay_wakeup_ind;


	delay_wakeup_ind.wakeup_reason = fapi_get_u16(skb, u.mlme_delayed_wakeup_ind.wakeup_reason);
	delay_wakeup_ind.delayed_pkt_count = fapi_get_u16(skb,
						       u.mlme_delayed_wakeup_ind.number_of_inds);
	slsi_vendor_delay_wakeup_event(sdev, dev, delay_wakeup_ind);
	kfree_skb(skb);
}

void slsi_rx_sr_params_changed_indication(struct slsi_dev *sdev, struct net_device *dev, struct sk_buff *skb)
{
	struct netdev_vif *ndev_vif = netdev_priv(dev);
	struct slsi_spatial_reuse_params sr_ind;

	SLSI_MUTEX_LOCK(ndev_vif->vif_mutex);

	if (!ndev_vif->activated) {
		SLSI_NET_DBG1(dev, SLSI_MLME, "VIF not activated\n");
		goto exit_with_lock;
	}

	if (WLBT_WARN(ndev_vif->vif_type != FAPI_VIFTYPE_STATION, "Not a Station VIF\n"))
		goto exit_with_lock;

	sr_ind.srg_obss_pd_min_offset = fapi_get_buff(skb, u.mlme_spatial_reuse_parameters_ind.srg_obss_pd_min_offset);
	sr_ind.srg_obss_pd_max_offset = fapi_get_buff(skb, u.mlme_spatial_reuse_parameters_ind.srg_obss_pd_max_offset);
	sr_ind.non_srg_obss_pd_max_offset =
		fapi_get_buff(skb, u.mlme_spatial_reuse_parameters_ind.non_srg_obss_pd_max_offset);
	sr_ind.hesiga_sr_value15allowed =
		fapi_get_buff(skb, u.mlme_spatial_reuse_parameters_ind.hesiga_spatial_reuse_value15allowed);
	sr_ind.non_srg_obss_pd_sr_allowed =
		fapi_get_buff(skb, u.mlme_spatial_reuse_parameters_ind.non_srg_obss_pd_sr_allowed);

	ndev_vif->sta.srg_obss_pd_min_offset = sr_ind.srg_obss_pd_min_offset;
	ndev_vif->sta.srg_obss_pd_max_offset = sr_ind.srg_obss_pd_max_offset;
	ndev_vif->sta.non_srg_obss_pd_max_offset = sr_ind.non_srg_obss_pd_max_offset;
	ndev_vif->sta.hesiga_spatial_reuse_value15allowed = sr_ind.hesiga_sr_value15allowed;
	ndev_vif->sta.non_srg_obss_pd_sr_allowed = sr_ind.non_srg_obss_pd_sr_allowed;

	slsi_vendor_change_sr_parameter_event(sdev, dev, sr_ind);

exit_with_lock:
	SLSI_MUTEX_UNLOCK(ndev_vif->vif_mutex);
	kfree_skb(skb);
}
