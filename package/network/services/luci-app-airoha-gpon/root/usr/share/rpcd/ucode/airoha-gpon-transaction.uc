'use strict';

export function prepare_mode_switch_config(config, pon_mode) {
	config.pon_mode = '' + (pon_mode || '');
	// Empty write-only values preserve the committed credentials in write_config().
	config.password = '';
	config.clear_password = '0';
	config.registration_id = '';
	config.clear_registration_id = '0';
	config.epon_password = '';
	config.clear_epon_password = '0';
	return config;
}

export function snapshot_options(uci, package, section, options) {
	let snapshot = {
		exists: uci.get(package, section) != null,
		values: {},
		present: {}
	};

	for (let option in options) {
		let value = uci.get(package, section, option);
		if (value != null) {
			snapshot.values[option] = value;
			snapshot.present[option] = true;
		}
	}

	return snapshot;
}

export function restore_options(uci, package, section, section_type, options,
				 snapshot) {
	if (!snapshot.exists) {
		uci.delete(package, section);
		return;
	}
	if (uci.get(package, section) == null)
		uci.set(package, section, section_type);
	for (let option in options) {
		if (snapshot.present[option])
			uci.set(package, section, option, snapshot.values[option]);
		else
			uci.delete(package, section, option);
	}
}

export function restore_committed_config(uci, gpon_options, epon_options,
					 previous_gpon, previous_epon) {
	restore_options(uci, 'airoha-gpon', 'main', 'gpon', gpon_options,
		previous_gpon);
	restore_options(uci, 'airoha-epon', 'main', 'oam', epon_options,
		previous_epon);
	let gpon_restored = uci.commit('airoha-gpon');
	let epon_restored = uci.commit('airoha-epon');
	return !!gpon_restored && !!epon_restored;
}

export function commit_config(uci, gpon_options, epon_options,
			      previous_gpon, previous_epon) {
	if (!uci.commit('airoha-gpon'))
		return { success: false, stage: 'gpon', restored: true };
	if (!uci.commit('airoha-epon'))
		return {
			success: false,
			stage: 'epon',
			restored: restore_committed_config(uci, gpon_options,
				epon_options, previous_gpon, previous_epon)
		};
	return { success: true };
}
