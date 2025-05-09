/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * drivers/media/radio/exynos/s621/fm_low_ref.h
 *
 * FM Radio Rx Low level driver header for SAMSUNG S621 chip
 *
 * Copyright (c) 2022 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */
#ifndef FM_LOW_REF_H
#define FM_LOW_REF_H

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/slab.h>

/******************************************************************************
 *	definition
 ******************************************************************************/
#define ABS(a)	(((a) < 0) ? -(a) : (a))

void print_ana_register(void);
int fm_boot(struct s621_radio *radio);
void fm_power_off(void);
u16 if_count_device_to_host(struct s621_radio *radio, u16 val);
static u16 aggr_rssi_host_to_device(u8 val);
u8 aggr_rssi_device_to_host(u16 val);
u16 rssi_device_to_host(u16 digi_rssi, u16 agc_gain, u16 rssi_adj);
#ifdef USE_SPUR_CANCEL
void fm_rx_en_spur_removal(struct s621_radio *radio);
void fm_rx_dis_spur_removal(void);
void fm_rx_check_spur(struct s621_radio *radio);
void fm_rx_check_spur_mono(struct s621_radio *radio);
#endif
void fm_set_freq(struct s621_radio *radio, u32 freq, bool mix_hi);
void fm_set_volume_enable(bool enable);
void fm_set_mute(bool mute);
void fm_set_blend_mute(struct s621_radio *radio);
static void fm_rds_flush_buffers(struct s621_radio *radio,
		bool clear_buffer);
void fm_rds_enable(struct s621_radio *radio);
void fm_rds_disable(struct s621_radio *radio);
bool fm_radio_on(struct s621_radio *radio);
void fm_radio_off(struct s621_radio *radio);
void fm_rds_on(struct s621_radio *radio);
void fm_rds_off(struct s621_radio *radio);
void fm_initialize(struct s621_radio *radio);
u16 fm_get_flags(struct s621_radio *radio);
void fm_set_flags(struct s621_radio *radio, u16 flags);
void fm_set_handler_if_count(void (*fn)(struct s621_radio *radio));
void fm_set_handler_audio_pause(void (*fn)(struct s621_radio *radio));
void fm_update_if_count(struct s621_radio *radio);
void fm_update_if_count_work(struct s621_radio *radio);
void fm_update_rssi(struct s621_radio *radio);
void fm_update_rssi_work(struct s621_radio *radio);
void fm_update_snr(struct s621_radio *radio);
void fm_update_snr_work(struct s621_radio *radio);
void fm_update_sig_info(struct s621_radio *radio);
void fm_update_sig_info_work(struct s621_radio *radio);
void fm_update_rds_sync_status(struct s621_radio *radio,
	bool synced);
u16 fm_update_rx_status(struct s621_radio *radio, u16 d_status);
void fm_update_tuner_mode(struct s621_radio *radio);
bool fm_check_rssi_level(u16 limit);
int low_get_search_lvl(struct s621_radio *radio, u16 *value);
int low_set_if_limit(struct s621_radio *radio, u16 value);
int low_set_search_lvl(struct s621_radio *radio, u16 value);
int low_set_freq(struct s621_radio *radio, u32 value);
int low_set_tuner_mode(struct s621_radio *radio, u16 value);
int low_set_mute_state(struct s621_radio *radio, u16 value);
int low_set_most_mode(struct s621_radio *radio, u16 value);
int low_set_most_blend(struct s621_radio *radio, u16 value);
int low_set_pause_lvl(struct s621_radio *radio, u16 value);
int low_set_pause_dur(struct s621_radio *radio, u16 value);
int low_set_demph_mode(struct s621_radio *radio, u16 value);
int low_set_rds_cntr(struct s621_radio *radio, u16 value);
int low_set_power(struct s621_radio *radio, u16 value);

void fm_set_interrupt_source(u16 sources, bool enable);
void fm_isr(struct s621_radio *radio);
void fm_rx_ana_start(void);
void fm_rx_ana_stop(void);
void fm_setup_iq_imbalance(void);
void fm_rx_init(void);
void fm_lo_off(void);
void fm_lo_prepare_setup(struct s621_radio *radio);
void fm_lo_set(const struct fm_lo_setup lo_set);
void fm_lo_initialize(struct s621_radio *radio);
void fm_sx_reset(void);
void fm_sx_start(void);
bool fm_aux_pll_initialize(void);
void fm_aux_pll_off(void);
void fm_set_band(struct s621_radio *radio, u8 index);
void fm_set_freq_step(struct s621_radio *radio, u8 index);
bool fm_band_trim(struct s621_radio *radio, u32 *freq);
void fm_get_if_filter_config(struct s621_radio *radio);
static bool fm_tuner_push_freq(struct s621_radio *radio,
	bool down);
static void fm_tuner_enable_rds(struct s621_radio *radio,
	bool enable);
void fm_set_rssi_thresh(struct s621_radio *radio,
	enum fm_tuner_state_low state);
static void fm_tuner_control_mute(struct s621_radio *radio);
void fm_tuner_set_force_mute(struct s621_radio *radio, bool mute);
void fm_tuner_set_mute_audio(struct s621_radio *radio, bool mute);
#ifdef MONO_SWITCH_INTERF
void fm_check_interferer(struct s621_radio *radio);
void fm_reset_force_mono_interf(struct s621_radio *radio);
#endif
void fm_start_if_counter(void);
static void fm_preset_tuned(struct s621_radio *radio);
static void fm_search_done(struct s621_radio *radio, u16 flags);
static void fm_search_check_signal2(unsigned long data);
static void fm_search_check_signal1(struct s621_radio *radio, bool rssi_oor);
static void fm_search_tuned(unsigned long data);
static void fm_start_tune(struct s621_radio *radio,
		enum fm_tuner_state_low new_state);
static void fm_tuner_change_state(struct s621_radio *radio,
		enum fm_tuner_state_low new_state);
void cancel_tuner_timer(struct s621_radio *radio);
static void fm_tuner_exit_state(struct s621_radio *radio);
void fm_set_tuner_mode(struct s621_radio *radio);
static bool fm_tuner_on(struct s621_radio *radio);
static void fm_tuner_off(struct s621_radio *radio);
void fm_tuner_rds_on(struct s621_radio *radio);
void fm_tuner_rds_off(struct s621_radio *radio);
bool fm_tuner_set_power_state(struct s621_radio *radio,
		bool fm_on, bool rds_on);
int init_low_struc(struct s621_radio *radio);
void fm_iclkaux_set(u32 data);

#ifdef	ENABLE_RDS_WORK_QUEUE
void s621_rds_work(struct work_struct *work);
#endif	/*ENABLE_RDS_WORK_QUEUE*/
#ifdef	ENABLE_IF_WORK_QUEUE
void s621_if_work(struct work_struct *work);
#endif	/*ENABLE_RDS_WORK_QUEUE*/

void s621_sig2_work(struct work_struct *work);
void s621_tune_work(struct work_struct *work);

#ifdef	RDS_POLLING_ENABLE
void fm_rds_periodic_update(unsigned long data);
void fm_rds_periodic_cancel(unsigned long data);
#endif	/*RDS_POLLING_ENABLE*/
#ifdef	IDLE_POLLING_ENABLE
void fm_idle_periodic_update(unsigned long data);
void fm_idle_periodic_cancel(unsigned long data);
#endif	/*IDLE_POLLING_ENABLE*/
void fm_set_audio_gain(struct s621_radio *radio, u16 gain);
void fm_ds_set(u32 data);
void fm_get_version_number(void);
extern void fmspeedy_wakeup(void);
extern void fm_pwron(void);
extern void fm_pwroff(void);
extern int fmspeedy_set_reg_field(
	u32 addr, u32 shift, u32 mask, u32 data);
extern int fmspeedy_set_reg_field_work(
	u32 addr, u32 shift, u32 mask, u32 data);
extern int fmspeedy_set_reg(u32 addr, u32 data);
extern int fmspeedy_set_reg_work(u32 addr, u32 data);
extern u32 fmspeedy_get_reg(u32 addr);
extern u32 fmspeedy_get_reg_work(u32 addr);
extern u32 fmspeedy_get_reg_field(u32 addr, u32 shift, u32 mask);
extern u32 fmspeedy_get_reg_field_work(u32 addr, u32 shift, u32 mask);
extern void fm_audio_check_work(void);
extern void fm_audio_check(void);
extern void fm_audio_control(struct s621_radio *radio,
	bool audio_out, bool lr_switch,
	u32 req_time, u32 audio_addr);
extern void fm_set_audio_enable(bool enable);
extern int fm_read_rds_data(struct s621_radio *radio, u8 *buffer, int size,
		u16 *blocks);
extern void fm_process_rds_data(struct s621_radio *radio);
extern struct s621_radio *gradio;
extern u32 *vol_level_init;
extern u32 *fm_spur_trf_init;
extern u32 *fm_dual_clk_init;
#ifdef USE_SPUR_CANCEL
extern u32 *fm_spur_init;
#endif
#endif	/*FM_LOW_REF_H*/
