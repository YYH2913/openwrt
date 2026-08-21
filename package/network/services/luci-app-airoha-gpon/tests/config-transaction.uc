'use strict';

import {
	commit_config, prepare_mode_switch_config, snapshot_options
} from '../root/usr/share/rpcd/ucode/airoha-gpon-transaction.uc';

const gpon_options = [ 'pon_mode', 'password', 'registration_id' ];
const epon_options = [ 'local_oui', 'loid', 'password' ];

function clone(value) {
	return value == null ? null : json(sprintf('%J', value));
}

function check(condition, message) {
	if (!condition)
		die(message + '\n');
}

function fake_cursor(initial, fail_at) {
	let state = {
		working: clone(initial),
		committed: clone(initial),
		commit_count: {},
		fail_at: fail_at || {}
	};
	let api = {
		get: function(package, section, option) {
			let value = state.working[package];
			if (value == null)
				return null;
			if (option == null)
				return value.type;
			return value.options[option] ?? null;
		},
		set: function(package, section, option, value) {
			if (value == null) {
				state.working[package] = { type: option, options: {} };
				return true;
			}
			if (state.working[package] == null)
				state.working[package] = { type: 'unknown', options: {} };
			state.working[package].options[option] = value;
			return true;
		},
		delete: function(package, section, option) {
			if (option == null) {
				state.working[package] = null;
				return true;
			}
			if (state.working[package] != null)
				delete state.working[package].options[option];
			return true;
		},
		commit: function(package) {
			let count = (state.commit_count[package] || 0) + 1;
			state.commit_count[package] = count;
			if (state.fail_at[package + ':' + count])
				return false;
			state.committed[package] = clone(state.working[package]);
			return true;
		},
		state: state
	};
	return api;
}

function initial_config(include_epon) {
	return {
		'airoha-gpon': {
			type: 'gpon',
			options: {
				pon_mode: 'gpon',
				password: 'old-secret',
				registration_id: 'old-registration'
			}
		},
		'airoha-epon': include_epon ? {
			type: 'oam',
			options: {
				local_oui: '000db6',
				loid: 'old-loid',
				password: 'old-epon-secret'
			}
		} : null
	};
}

function stage_candidate(uci) {
	uci.set('airoha-gpon', 'main', 'pon_mode', 'epon-10g-10g');
	uci.set('airoha-gpon', 'main', 'password', 'new-secret');
	if (uci.get('airoha-epon', 'main') == null)
		uci.set('airoha-epon', 'main', 'oam');
	uci.set('airoha-epon', 'main', 'loid', 'new-loid');
	uci.set('airoha-epon', 'main', 'password', 'new-epon-secret');
}

function snapshots(uci) {
	return {
		gpon: snapshot_options(uci, 'airoha-gpon', 'main', gpon_options),
		epon: snapshot_options(uci, 'airoha-epon', 'main', epon_options)
	};
}

// The first package has committed when the second package fails. Both package
// snapshots, including secrets which RPC status never returns, must be durable.
let uci = fake_cursor(initial_config(true), { 'airoha-epon:1': true });
let previous = snapshots(uci);
stage_candidate(uci);
let result = commit_config(uci, gpon_options, epon_options,
	previous.gpon, previous.epon);
check(!result.success && result.stage == 'epon' && result.restored,
	'EPON commit failure was not rolled back');
check(uci.state.committed['airoha-gpon'].options.pon_mode == 'gpon',
	'GPON mode survived failed dual-package commit');
check(uci.state.committed['airoha-gpon'].options.password == 'old-secret',
	'write-only PLOAM password was not restored');
check(uci.state.committed['airoha-epon'].options.password == 'old-epon-secret',
	'write-only EPON password was not restored');

// A missing pre-existing EPON section must be removed rather than retained as
// a partially-created section after the same failure.
uci = fake_cursor(initial_config(false), { 'airoha-epon:1': true });
previous = snapshots(uci);
stage_candidate(uci);
result = commit_config(uci, gpon_options, epon_options,
	previous.gpon, previous.epon);
check(!result.success && result.restored,
	'absent EPON section rollback failed');
check(uci.state.committed['airoha-epon'] == null,
	'new EPON section survived a failed transaction');

// A rollback commit failure is surfaced; callers must not claim that old
// persistent and runtime state were restored.
uci = fake_cursor(initial_config(true), {
	'airoha-epon:1': true,
	'airoha-gpon:2': true
});
previous = snapshots(uci);
stage_candidate(uci);
result = commit_config(uci, gpon_options, epon_options,
	previous.gpon, previous.epon);
check(!result.success && !result.restored,
	'rollback commit failure was hidden');

// The success path commits both new packages without altering the snapshots.
uci = fake_cursor(initial_config(true), {});
previous = snapshots(uci);
stage_candidate(uci);
result = commit_config(uci, gpon_options, epon_options,
	previous.gpon, previous.epon);
check(result.success, 'valid dual-package transaction failed');
check(uci.state.committed['airoha-gpon'].options.pon_mode == 'epon-10g-10g' &&
	uci.state.committed['airoha-epon'].options.loid == 'new-loid',
	'valid dual-package transaction was not committed');

// The privileged mode-only RPC must not need any write-only credential in its
// request. Empty values retain them, and every clear flag is explicitly false.
let mode_config = prepare_mode_switch_config({
	pon_mode: 'xgpon',
	serial_number: 'HWTC12345678',
	password_configured: true,
	registration_id_configured: true,
	epon_password_configured: true
}, 'epon-10g-10g');
check(mode_config.pon_mode == 'epon-10g-10g' &&
	mode_config.serial_number == 'HWTC12345678',
	'mode-only request did not preserve non-secret configuration');
check(mode_config.password == '' && mode_config.registration_id == '' &&
	mode_config.epon_password == '' && mode_config.clear_password == '0' &&
	mode_config.clear_registration_id == '0' &&
	mode_config.clear_epon_password == '0',
	'mode-only request could expose or clear a write-only credential');

print('LuCI PON configuration transaction tests passed\n');
