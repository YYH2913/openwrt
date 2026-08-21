#!/usr/bin/env ucode

'use strict';

import { access, glob, open, readfile, writefile } from 'fs';
import { cursor } from 'uci';
import {
	commit_config, prepare_mode_switch_config, restore_committed_config,
	snapshot_options
} from './airoha-gpon-transaction.uc';

function find_device(pattern, attribute) {
	for (let path in glob(pattern))
		if (access(path + '/' + attribute))
			return path;

	return null;
}

function read_attribute(path, attribute) {
	if (!path)
		return null;

	let value = readfile(path + '/' + attribute);
	return value == null ? null : trim(value);
}

function parse_pairs(value) {
	let result = {};

	for (let field in split(value || '', /\s+/)) {
		let pair = split(field, '=', 2);
		if (length(pair) == 2)
			result[pair[0]] = pair[1];
	}

	return result;
}

function write_sysfs(path, value) {
	if (!path || !access(path))
		return false;
	return !!writefile(path, value + '\n');
}

function read_json(path) {
	let value = readfile(path);
	return value == null ? null : json(value);
}

function read_config() {
	let uci = cursor();
	let password = uci.get('airoha-gpon', 'main', 'password') || '';
	let registration_id = uci.get('airoha-gpon', 'main', 'registration_id') || '';
	let epon_password = uci.get('airoha-epon', 'main', 'password') || '';
	let config = {
		pon_mode: uci.get('airoha-gpon', 'main', 'pon_mode') || 'xgspon',
		serial_number: uci.get('airoha-gpon', 'main', 'serial_number') || '',
		enabled: uci.get('airoha-gpon', 'main', 'enabled') || '0',
		omci_mode: uci.get('airoha-gpon', 'main', 'omci_mode') || 'auto',
		manual_gem: uci.get('airoha-gpon', 'main', 'manual_gem') || '0',
		gem_id: uci.get('airoha-gpon', 'main', 'gem_id') || '0',
		tcont: uci.get('airoha-gpon', 'main', 'tcont') || '0',
		encrypted: uci.get('airoha-gpon', 'main', 'encrypted') || '0',
		epon_onu_mac: uci.get('airoha-gpon', 'main', 'epon_onu_mac') || '',
		epon_llid_mask: uci.get('airoha-gpon', 'main', 'epon_llid_mask') || '0x1',
		epon_silent_time: uci.get('airoha-gpon', 'main', 'epon_silent_time') || '60',
		epon_local_oui: uci.get('airoha-epon', 'main', 'local_oui') || '000db6',
		epon_ctc_enabled: uci.get('airoha-epon', 'main', 'ctc_enabled') || '1',
		epon_ctc_oui: uci.get('airoha-epon', 'main', 'ctc_oui') || '111111',
		epon_dpoe_enabled: uci.get('airoha-epon', 'main', 'dpoe_enabled') || '1',
		epon_loid: uci.get('airoha-epon', 'main', 'loid') || '',
		password_configured: length(password) != 0,
		registration_id_configured: length(registration_id) != 0,
		epon_password_configured: length(epon_password) != 0
	};

	uci.unload('airoha-gpon');
	uci.unload('airoha-epon');
	return config;
}

function valid_flag(value) {
	return value == '0' || value == '1' || value === false || value === true;
}

function normalized_flag(value) {
	return value == '1' || value === true ? '1' : '0';
}

function valid_number(value, maximum) {
	value = '' + value;
	return match(value, /^[0-9]+$/) && +value <= maximum;
}

function epon_mode(value) {
	return value == 'epon-10g-1g' || value == 'epon-10g-10g';
}

function xg_mode(value) {
	return value == 'xgpon' || value == 'xgspon';
}

function valid_epon_mac(value) {
	if (!match(value, /^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$/))
		return false;
	let first = +('0x' + substr(value, 0, 2));
	return !(first & 1) && lc(value) != '00:00:00:00:00:00';
}

function valid_oui(value) {
	return !!match(value, /^([0-9a-fA-F]{6}|[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){2})$/);
}

function valid_password(value) {
	if (value == '')
		return true;
	if (match(value, /^hex:/))
		return !!match(value, /^hex:[0-9a-fA-F]{20}$/);

	return !!match(value, /^[!-~]{1,10}$/);
}

function valid_registration_id(value) {
	if (value == '')
		return true;
	if (match(value, /^hex:/))
		return !!match(value, /^hex:[0-9a-fA-F]{72}$/);

	return !!match(value, /^[!-~]{1,36}$/);
}

function write_config(config) {
	if (type(config) != 'object')
		return { success: false, error: 'invalid configuration' };

	let serial = '' + (config.serial_number || '');
	let password = '' + (config.password || '');
	let registration_id = '' + (config.registration_id || '');
	let epon_password = '' + (config.epon_password || '');
	let pon_mode = '' + (config.pon_mode || '');
	let mode = '' + (config.omci_mode || '');
	let clear_password = normalized_flag(config.clear_password || '0') == '1';
	let clear_registration_id = normalized_flag(
		config.clear_registration_id || '0') == '1';
	let clear_epon_password = normalized_flag(
		config.clear_epon_password || '0') == '1';
	if (pon_mode != 'gpon' && !xg_mode(pon_mode) && !epon_mode(pon_mode))
		return { success: false, error: 'invalid PON mode' };
	if (!epon_mode(pon_mode) &&
	    !match(serial, /^[A-Za-z0-9]{4}[0-9a-fA-F]{8}$/))
		return { success: false, error: 'invalid ONU serial number' };
	if (!valid_password(password) || (password != '' && clear_password))
		return { success: false, error: 'invalid PLOAM password' };
	if (!valid_registration_id(registration_id) ||
	    (registration_id != '' && clear_registration_id))
		return { success: false, error: 'invalid Registration-ID' };
	if (mode != 'auto' && mode != 'manual')
		return { success: false, error: 'invalid provisioning mode' };
	if (!valid_flag(config.enabled) || !valid_flag(config.manual_gem) ||
	    !valid_flag(config.encrypted) || !valid_flag(config.clear_password) ||
	    !valid_flag(config.clear_registration_id) ||
	    !valid_flag(config.epon_ctc_enabled) ||
	    !valid_flag(config.epon_dpoe_enabled) ||
	    !valid_flag(config.clear_epon_password))
		return { success: false, error: 'invalid boolean option' };
	if (!valid_number(config.gem_id, 4095) || !valid_number(config.tcont, 15))
		return { success: false, error: 'invalid GEM or T-CONT value' };
	if (epon_mode(pon_mode)) {
		let epon_mac = '' + (config.epon_onu_mac || '');
		let llid_mask = '' + (config.epon_llid_mask || '');
		let silent_time = '' + (config.epon_silent_time || '');
		let loid = '' + (config.epon_loid || '');
		if (!valid_epon_mac(epon_mac) ||
		    !match(llid_mask, /^(0[xX][0-9a-fA-F]+|[0-9]+)$/) ||
		    +llid_mask < 1 || +llid_mask > 4294967295 ||
		    !valid_number(silent_time, 3600) || +silent_time < 1)
			return { success: false, error: 'invalid EPON ONU MAC, LLID mask or silent time' };
		if (!valid_oui('' + (config.epon_local_oui || '')) ||
		    !valid_oui('' + (config.epon_ctc_oui || '')))
			return { success: false, error: 'invalid EPON OUI' };
		if (length(loid) > 24 || length(epon_password) > 12 ||
		    (epon_password != '' && clear_epon_password))
			return { success: false, error: 'invalid EPON LOID or password' };
	}
	let transaction_lock = open('/var/lock/airoha-pon-config.lock', 'a', 0o600);
	if (!transaction_lock || !transaction_lock.lock('xn')) {
		if (transaction_lock)
			transaction_lock.close();
		return {
			success: false,
			error: 'another PON configuration transaction is active'
		};
	}

	let gpon_options = [
		'pon_mode', 'serial_number', 'enabled', 'omci_mode', 'manual_gem',
		'gem_id', 'tcont', 'encrypted', 'epon_onu_mac', 'epon_llid_mask',
		'epon_silent_time', 'password', 'registration_id'
	];
	let epon_options = [
		'local_oui', 'ctc_enabled', 'ctc_oui', 'dpoe_enabled', 'loid',
		'password'
	];
	let uci = cursor();
	let previous_gpon = snapshot_options(uci, 'airoha-gpon', 'main',
		gpon_options);
	let previous_epon = snapshot_options(uci, 'airoha-epon', 'main',
		epon_options);
	if (!uci.get('airoha-gpon', 'main'))
		uci.set('airoha-gpon', 'main', 'gpon');
	uci.set('airoha-gpon', 'main', 'pon_mode', pon_mode);
	uci.set('airoha-gpon', 'main', 'serial_number', serial);
	uci.set('airoha-gpon', 'main', 'enabled', normalized_flag(config.enabled));
	uci.set('airoha-gpon', 'main', 'omci_mode', mode);
	uci.set('airoha-gpon', 'main', 'manual_gem',
		normalized_flag(config.manual_gem));
	uci.set('airoha-gpon', 'main', 'gem_id', '' + config.gem_id);
	uci.set('airoha-gpon', 'main', 'tcont', '' + config.tcont);
	uci.set('airoha-gpon', 'main', 'encrypted',
		normalized_flag(config.encrypted));
	uci.set('airoha-gpon', 'main', 'epon_onu_mac',
		'' + (config.epon_onu_mac || ''));
	uci.set('airoha-gpon', 'main', 'epon_llid_mask',
		'' + (config.epon_llid_mask || '0x1'));
	uci.set('airoha-gpon', 'main', 'epon_silent_time',
		'' + (config.epon_silent_time || '60'));
	if (clear_password)
		uci.set('airoha-gpon', 'main', 'password', '');
	else if (password != '')
		uci.set('airoha-gpon', 'main', 'password', password);
	if (clear_registration_id)
		uci.set('airoha-gpon', 'main', 'registration_id', '');
	else if (registration_id != '')
		uci.set('airoha-gpon', 'main', 'registration_id', registration_id);
	if (!uci.get('airoha-epon', 'main'))
		uci.set('airoha-epon', 'main', 'oam');
	uci.set('airoha-epon', 'main', 'local_oui',
		'' + (config.epon_local_oui || '000db6'));
	uci.set('airoha-epon', 'main', 'ctc_enabled',
		normalized_flag(config.epon_ctc_enabled));
	uci.set('airoha-epon', 'main', 'ctc_oui',
		'' + (config.epon_ctc_oui || '111111'));
	uci.set('airoha-epon', 'main', 'dpoe_enabled',
		normalized_flag(config.epon_dpoe_enabled));
	uci.set('airoha-epon', 'main', 'loid', '' + (config.epon_loid || ''));
	if (clear_epon_password)
		uci.set('airoha-epon', 'main', 'password', '');
	else if (epon_password != '')
		uci.set('airoha-epon', 'main', 'password', epon_password);
	let committed = commit_config(uci, gpon_options, epon_options,
		previous_gpon, previous_epon);
	if (!committed.success) {
		uci.unload('airoha-gpon');
		uci.unload('airoha-epon');
		transaction_lock.close();
		return {
			success: false,
			error: committed.stage == 'gpon' ? 'failed to commit configuration' :
				(committed.restored ? 'failed to commit EPON OAM configuration' :
				 'failed to commit EPON OAM configuration and configuration rollback failed')
		};
	}

	if (system([ '/usr/libexec/airoha-gpon-config', 'sync-deferred' ]) != 0) {
		let restored = restore_committed_config(uci, gpon_options,
			epon_options, previous_gpon, previous_epon);
		let runtime_restored = restored &&
			system([ '/usr/libexec/airoha-gpon-config', 'sync' ]) == 0;
		uci.unload('airoha-gpon');
		uci.unload('airoha-epon');
		transaction_lock.close();
		return {
			success: false,
			error: runtime_restored ? 'failed to apply PON configuration; previous configuration restored' :
				'failed to apply PON configuration and rollback was incomplete'
		};
	}

	uci.unload('airoha-gpon');
	uci.unload('airoha-epon');
	transaction_lock.close();
	return { success: true };
}

const methods = {
	status: {
		call: function() {
			let config = read_config();
			let gpon = find_device('/sys/bus/platform/drivers/airoha-gpon/*',
				'enabled');
			let xgspon = find_device('/sys/bus/platform/drivers/airoha-xgspon/*',
				'enabled');
			let epon = find_device('/sys/bus/platform/drivers/airoha-en7581-epon/*',
				'enabled');
			let bosa = find_device('/sys/bus/i2c/drivers/airoha-en7572/*',
				'initialized');
			let selected = epon_mode(config.pon_mode) ? epon :
				(xg_mode(config.pon_mode) ? xgspon : gpon);
			let bosa_mode = read_attribute(bosa, 'pon_mode');
			let bosa_matches = bosa_mode == config.pon_mode;
			let safety = config.pon_mode == 'gpon' ?
				parse_pairs(read_attribute(gpon, 'safety_status')) : {};
			let statistics = config.pon_mode == 'gpon' ?
				parse_pairs(read_attribute(gpon, 'stats')) : {};
			let optical_link = config.pon_mode == 'gpon' ?
				parse_pairs(read_attribute(gpon, 'optical_link')) : {};
			let ber_sample = config.pon_mode == 'gpon' ?
				parse_pairs(read_attribute(gpon, 'ber_sample')) : {};
			let diagnostic_fields = split(read_attribute(bosa,
				'optical_diagnostics') || '', /\s+/);
			let omci = read_json('/var/run/airoha-omcid/status.json');
			let platform = read_json('/var/run/airoha-omcid/platform.json');
			let desired = read_json('/var/run/airoha-omcid/desired.json');
			let epon_oam = read_json('/var/run/airoha-epon-oamd/status.json');
			let diagnostics = length(diagnostic_fields) == 5 ? {
				temperature: diagnostic_fields[0],
				supply_voltage: diagnostic_fields[1],
				laser_bias_current: diagnostic_fields[2],
				transmit_power: diagnostic_fields[3],
				receive_power: diagnostic_fields[4]
			} : null;

			let omcc_id = config.pon_mode == 'gpon' ?
				read_attribute(gpon, 'omcc_id') : null;
			let xgspon_activation_raw = read_attribute(xgspon,
				'activation_evidence');
			let xgspon_activation_evidence = xgspon_activation_raw == null ?
				null : parse_pairs(xgspon_activation_raw);
			let xgspon_phy_raw = read_attribute(xgspon, 'phy_evidence');
			let xgspon_phy_evidence = xgspon_phy_raw == null ?
				null : parse_pairs(xgspon_phy_raw);
			let xgs_service = xg_mode(config.pon_mode) ?
				parse_pairs(read_attribute(xgspon, 'service_state')) : {};
			let activation = epon_mode(config.pon_mode) ?
				read_attribute(epon, 'mpcp_state') : read_attribute(selected, 'state');
			let omci_ready = omci != null && omci.state == 'online' &&
				omci.pon_mode == config.pon_mode &&
				((config.pon_mode == 'gpon' && omcc_id != null &&
				  omcc_id != 'unassigned' && match(activation || '', /^5\s/)) ||
				 (xg_mode(config.pon_mode) &&
				  read_attribute(xgspon, 'ready') == '1')) &&
				platform != null && platform.state == 'applied';
			let epon_oam_ready = epon_mode(config.pon_mode) &&
				epon_oam != null && epon_oam.state == 'online' &&
				epon_oam.pon_mode == config.pon_mode &&
				epon_oam.control_plane == 'epon-oam' && epon_oam.omci === false;

			return {
				pon_mode: config.pon_mode,
				present: selected != null,
				gpon_present: gpon != null,
				xgspon_present: xgspon != null,
				epon_present: epon != null,
				xgspon_activation_ready:
					read_attribute(xgspon, 'activation_ready') == '1',
				xgspon_ready: read_attribute(xgspon, 'ready') == '1',
				xgspon_state: read_attribute(xgspon, 'state'),
				xgspon_blockers: read_attribute(xgspon, 'activation_blockers'),
				xgspon_control_abi: read_attribute(xgspon,
					'control_abi_version'),
				xgspon_secure_omcc_info: read_attribute(xgspon,
					'secure_omcc_info'),
				xgspon_registration_id_configured: read_attribute(xgspon,
					'registration_id_configured') == '1',
				xgspon_key_derivation_state: read_attribute(xgspon,
					'key_derivation_state'),
				xgspon_activation_evidence: xgspon_activation_evidence,
				xgspon_phy_evidence: xgspon_phy_evidence,
				xgs_service_state: xgs_service,
				epon_mpcp_state: read_attribute(epon, 'mpcp_state'),
				epon_statistics: parse_pairs(read_attribute(epon, 'statistics')),
				epon_oam_available: !!epon_oam_ready,
				epon_oam: epon_oam,
				bosa_present: bosa != null && bosa_matches,
				bosa_mode: bosa_mode,
				enabled: read_attribute(selected, 'enabled') == '1',
				state: activation,
				onu_id: config.pon_mode == 'gpon' ?
					read_attribute(gpon, 'onu_id') : null,
				omcc_id: omcc_id,
				tconts: config.pon_mode == 'gpon' ?
					read_attribute(gpon, 'tconts') : null,
				data_gem: config.pon_mode == 'gpon' ?
					read_attribute(gpon, 'data_gem') : null,
				data_gems: config.pon_mode == 'gpon' ?
					read_attribute(gpon, 'data_gems') : null,
				equalization_delay: config.pon_mode == 'gpon' ?
					read_attribute(gpon, 'equalization_delay') : null,
				safety: safety,
				statistics: statistics,
				optical_link: optical_link,
				ber_sample: ber_sample,
				bosa_initialized: bosa_matches &&
					read_attribute(bosa, 'initialized') == '1',
				tx_disabled: !bosa_matches ||
					read_attribute(bosa, 'tx_disable') != '0',
				los: bosa_matches && read_attribute(bosa, 'los') == '1',
				calibration_source: bosa_matches ? read_attribute(bosa,
					'calibration_source') : null,
				firmware_version: bosa_matches ?
					read_attribute(bosa, 'firmware_version') : null,
				optical_diagnostics: bosa_matches ? diagnostics : null,
				link_state: read_attribute('/sys/class/net/pon', 'operstate'),
				rx_bytes: read_attribute('/sys/class/net/pon/statistics',
					'rx_bytes'),
				tx_bytes: read_attribute('/sys/class/net/pon/statistics',
					'tx_bytes'),
				omci_available: !!omci_ready,
				omci: omci,
				platform: platform,
				config: config,
				desired_operation: desired ? desired.operation : null,
				desired_mib_data_sync: desired ? desired.mib_data_sync : null,
				service_graph: desired ? desired.service_graph : null
			};
		}
	},
	set_config: {
		args: { config: {} },
		call: function(request) {
			return write_config(request.args?.config);
		}
	},
	set_mode: {
		args: { pon_mode: '' },
		call: function(request) {
			return write_config(prepare_mode_switch_config(read_config(),
				request.args?.pon_mode));
		}
	},
	xgs_service_clear: {
		call: function() {
			if (!xg_mode(read_config().pon_mode))
				return { success: false, error: '10G ITU-T service is unavailable in this mode' };
			let xgspon = find_device('/sys/bus/platform/drivers/airoha-xgspon/*',
				'service_state');
			if (!xgspon)
				return { success: false, error: 'XGS-PON service ABI unavailable' };
			if (!write_sysfs(xgspon + '/service_reset', 'reset') ||
				!write_sysfs(xgspon + '/service_commit', 'commit'))
				return { success: false, error: 'failed to clear XGS-PON service' };
			return { success: true };
		}
	}
};

return { 'luci.airoha-gpon': methods };
