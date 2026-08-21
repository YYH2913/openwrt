'use strict';
'require dom';
'require form';
'require poll';
'require rpc';
'require ui';
'require view';

const callStatus = rpc.declare({
	object: 'luci.airoha-gpon',
	method: 'status'
});

const callSetConfig = rpc.declare({
	object: 'luci.airoha-gpon',
	method: 'set_config',
	params: [ 'config' ]
});

const callXgsServiceClear = rpc.declare({
	object: 'luci.airoha-gpon',
	method: 'xgs_service_clear'
});

function text(value) {
	return value == null || value === '' ? '-' : String(value);
}

function setText(id, value) {
	const node = document.getElementById(id);
	if (node)
		node.textContent = text(value);
}

function stateLabel(status) {
	if (!status.present)
		return _('Driver unavailable');
	if ((status.pon_mode === 'xgpon' || status.pon_mode === 'xgspon') &&
	    status.state === 'inactive-board-mode')
		return _('Hardware mode not selected');
	if ((status.pon_mode === 'xgpon' || status.pon_mode === 'xgspon') &&
	    !status.xgspon_activation_ready)
		return _('Blocked');
	if (!status.enabled)
		return _('Disabled');
	return status.state || _('Activating');
}

function opticalLabel(status) {
	if (status.pon_mode === 'xgpon' || status.pon_mode === 'xgspon') {
		const evidence = status.xgspon_activation_evidence || {};
		return evidence.optical_tx === 'enabled' ?
			_('Laser active') : _('Laser disabled');
	}
	if (!status.bosa_present || !status.bosa_initialized)
		return _('BOSA unavailable');
	if (status.safety && status.safety.rogue_fault === '1')
		return _('Locked by rogue-ONU protection');
	if (status.los)
		return _('Loss of signal');
	return status.tx_disabled ? _('Laser disabled') : _('Laser active');
}

function ponModeLabel(mode) {
	switch (mode) {
	case 'xgpon':
		return _('XG-PON (10G/2.5G)');
	case 'xgspon':
		return _('XGS-PON (10G/10G)');
	case 'epon-10g-1g':
		return _('10G-EPON (10G/1G)');
	case 'epon-10g-10g':
		return _('10G-EPON (10G/10G)');
	default:
		return _('GPON');
	}
}

function xgsBlockersLabel(value) {
	const labels = {
		'board-mode': _('Hardware mode'),
		phy: _('PHY/PCS'),
		calibration: _('Calibration'),
		'tc-ploam': _('TC/PLOAM'),
		'secure-omcc': _('Secure OMCC'),
		xgem: _('XGEM/T-CONT')
	};
	const blockers = String(value || '').split(/\s+/).filter(Boolean);
	return blockers.length ? blockers.map(function(blocker) {
		return labels[blocker] || blocker;
	}).join(', ') : '-';
}

function xgsKeyDerivationLabel(value) {
	switch (value) {
	case 'registration-msk-unset':
		return _('MSK not established');
	case 'registration-msk-ready-pon-tag-pending':
		return _('MSK ready, awaiting authenticated PON tag');
	case 'session-keys-ready-hardware-pending':
		return _('Session keys ready, awaiting secure hardware paths');
	case 'session-keys-loaded-tx-disabled':
		return _('Session keys loaded, optical transmission remains disabled');
	case 'session-keys-loaded-awaiting-onu-id-tx-disabled':
		return _('Session keys loaded, awaiting ONU-ID; optical transmission remains disabled');
	case 'onu-id-assigned-registration-pending':
		return _('ONU-ID assigned, awaiting Registration completion');
	case 'registered-secure-omcc-ready':
		return _('Registered, secure OMCC ready');
	case 'hardware-state-uncertain-tx-disabled':
		return _('Hardware state uncertain; optical transmission remains disabled');
	default:
		return '-';
	}
}

function xgsActivationEvidenceLabel(evidence) {
	if (!evidence)
		return '-';
	if (evidence.hardware_selected !== '1')
		return _('Hardware mode not selected');

	return _('MAC O%s, optical TX %s, TX sync %s, PLOAM key %s, OMCI key %s, upstream IRQ %s, unexpected events %s').format(
		text(evidence.activation_state),
		evidence.optical_tx === 'enabled' ? _('enabled') : _('disabled'),
		evidence.tx_sync_ready === '1' ? _('ready') : _('not ready'),
		text(evidence.current_pik),
		text(evidence.current_oik),
		text(evidence.upstream_interrupt_enable),
		text(evidence.unexpected_upstream_events));
}

function xgsPhyEvidenceLabel(evidence) {
	var state;

	if (!evidence)
		return '-';
	if (evidence.hardware_selected !== '1')
		return _('Hardware mode not selected');
	if (evidence.fault === '1')
		state = _('Fault');
	else if (evidence.recovering === '1')
		state = _('Recovering');
	else if (evidence.los === '1')
		state = _('LOS');
	else if (evidence.lof === '1')
		state = _('LOF');
	else
		state = evidence.driver_ready === '1' ? _('Ready') : _('Not ready');

	return _('State %s; PHYA %s, RX %s (state %s), TX sync %s, PHY IRQ %s; recovery attempt %s, successes %s, failures %s').format(
		state,
		evidence.phya_ready === '1' ? _('ready') : _('not ready'),
		evidence.rx_sync === '1' ? _('synchronized') : _('not synchronized'),
		text(evidence.rx_sync_state),
		evidence.tx_sync_ready === '1' ? _('ready') : _('not ready'),
		evidence.irq_owned === '1' ? _('owned') : _('observation only'),
		text(evidence.recovery_attempt), text(evidence.recoveries),
		text(evidence.recovery_failures));
}

function phyLinkLabel(link) {
	link = link || {};
	if (link.los === '1')
		return _('LOS');
	if (link.lof === '1')
		return _('LOF');
	return link.phy_ready === '1' ? _('Synchronized') : _('Not synchronized');
}

function berLabel(sample) {
	sample = sample || {};
	if (sample.sequence == null)
		return '-';
	return _('%s BIP errors / %s ms (sample %s)').format(
		text(sample.bip_count), text(sample.interval_ms), text(sample.sequence));
}

function softwarePhaseLabel(phase) {
	switch (phase) {
	case 'idle':
		return _('Idle');
	case 'downloading':
		return _('Downloading');
	case 'staged':
		return _('Ready to activate');
	case 'activating':
		return _('Activating image');
	case 'committed':
		return _('Committed');
	case 'failed':
		return _('Failed');
	default:
		return text(phase);
	}
}

function platformStateLabel(state) {
	switch (state) {
	case 'applied':
		return _('Applied');
	case 'idle':
		return _('Idle');
	case 'waiting-mib':
		return _('Waiting for OLT provisioning');
	case 'waiting-o5':
		return _('Waiting for O5 activation');
	case 'waiting-tcont':
		return _('Waiting for T-CONT allocation');
	case 'driver-unavailable':
		return _('PON driver unavailable');
	case 'invalid-graph':
		return _('OLT service graph rejected');
	case 'bridge-conflict':
		return _('LAN or PON already belongs to another bridge');
	case 'bridge-apply-failed':
		return _('MAC bridge apply failed');
	case 'gem-apply-failed':
		return _('GEM mapping apply failed');
	case 'uni-apply-failed':
		return _('Ethernet UNI apply failed');
	case 'classifier-apply-failed':
		return _('VLAN or GEM classifier apply failed');
	default:
		return text(state);
	}
}

function signed16(value) {
	value = Number(value) & 0xffff;
	return value & 0x8000 ? value - 0x10000 : value;
}

function opticalPower(value) {
	value = Number(value);
	return value > 0 ? _('%s dBm').format((10 * Math.log10(value) - 40).toFixed(2)) :
		_('No optical signal');
}

function entityID(value) {
	return value == null ? '-' : '0x' + Number(value).toString(16).padStart(4, '0');
}

function directionLabel(value) {
	switch (Number(value)) {
	case 1:
		return _('Downstream');
	case 2:
		return _('Upstream');
	case 3:
		return _('Bidirectional');
	default:
		return text(value);
	}
}

function tpTypeLabel(value) {
	switch (Number(value)) {
	case 1:
		return _('Ethernet UNI');
	case 3:
		return _('802.1p mapper');
	case 5:
		return _('GEM interworking');
	default:
		return text(value);
	}
}

function associationLabel(value) {
	switch (Number(value)) {
	case 0:
		return _('MAC bridge port');
	case 1:
		return _('802.1p mapper');
	case 2:
		return _('Ethernet UNI');
	case 5:
		return _('GEM interworking');
	default:
		return text(value);
	}
}

function replaceRows(id, rows, columns) {
	const body = document.getElementById(id);
	if (!body)
		return;

	while (body.firstChild)
		body.removeChild(body.firstChild);

	if (!rows.length) {
		body.appendChild(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'colspan': columns }, _('Not provisioned'))
		]));
		return;
	}

	for (const row of rows)
		body.appendChild(E('tr', { 'class': 'tr' }, row.map(function(value) {
			return E('td', { 'class': 'td left' }, text(value));
		})));
}

function updateServiceGraph(graph) {
	graph = graph || {};
	const gems = graph.gem_ports || [];
	const interworking = graph.gem_interworking || [];
	const mappers = graph.pbit_mappers || [];
	const bridges = graph.bridges || [];
	const extendedVLANs = graph.extended_vlans || [];
	const gemByEntity = {};
	const interworkingByEntity = {};

	for (const gem of gems)
		gemByEntity[gem.entity_id] = gem;
	for (const iw of interworking)
		interworkingByEntity[iw.entity_id] = iw;

	replaceRows('gpon-gem-rows', gems.map(function(gem) {
		return [
			gem.port_id,
			gem.alloc_id,
			directionLabel(gem.direction),
			entityID(gem.upstream_queue),
			entityID(gem.downstream_queue)
		];
	}), 5);

	const mapperRows = [];
	for (const mapper of mappers) {
		for (let pbit = 0; pbit < 8; pbit++) {
			const iw = interworkingByEntity[(mapper.pbits || [])[pbit]];
			const gem = iw ? gemByEntity[iw.gem_port] : null;
			mapperRows.push([
				entityID(mapper.entity_id),
				pbit,
				gem ? gem.port_id : '-',
				iw ? entityID(iw.entity_id) : '-'
			]);
		}
	}
	replaceRows('gpon-mapper-rows', mapperRows, 4);

	const bridgeRows = [];
	for (const bridge of bridges)
		for (const port of bridge.ports || [])
			bridgeRows.push([
				entityID(bridge.entity_id),
				port.port,
				tpTypeLabel(port.tp_type),
				entityID(port.tp)
			]);
	replaceRows('gpon-bridge-rows', bridgeRows, 4);

	replaceRows('gpon-vlan-rows', extendedVLANs.map(function(vlan) {
		const rules = vlan.rules || {};
		const ruleCount = Array.isArray(rules) ? rules.length :
			(rules.num_rows == null ? rules.NumRows : rules.num_rows);
		return [
			entityID(vlan.entity_id),
			associationLabel(vlan.association_type),
			entityID(vlan.associated_me),
			vlan.enhanced_mode ? _('Enhanced') : _('Classic'),
			text(ruleCount),
			vlan.downstream_mode
		];
	}), 6);
}

function updateStatus(status) {
	status = status || {};
	const omci = status.omci || {};
	const eponOam = status.epon_oam || {};
	const platform = status.platform || {};
	const diagnostics = status.optical_diagnostics || {};
	setText('gpon-mode', ponModeLabel(status.pon_mode));
	setText('gpon-xgs-ready', status.xgspon_ready ? _('Ready') : _('Blocked'));
	setText('gpon-xgs-blockers', xgsBlockersLabel(status.xgspon_blockers));
	setText('gpon-xgs-registration', status.config &&
		status.config.registration_id_configured ? _('Configured') : _('Not configured'));
	setText('gpon-xgs-key-derivation',
		xgsKeyDerivationLabel(status.xgspon_key_derivation_state));
	setText('gpon-xgs-activation-evidence',
		xgsActivationEvidenceLabel(status.xgspon_activation_evidence));
	setText('gpon-xgs-phy-evidence',
		xgsPhyEvidenceLabel(status.xgspon_phy_evidence));
	setText('gpon-xgs-control-abi', status.xgspon_control_abi);
	const service = status.xgs_service_state || {};
	setText('gpon-xgs-service-state',
		(status.pon_mode === 'xgpon' || status.pon_mode === 'xgspon') ?
		(service.version ? _('Generation %s, %s T-CONTs, %s XGEMs').format(
			text(service.generation), text(service.tconts), text(service.xgems)) :
			_('Unavailable')) : _('Not applicable'));
	setText('gpon-xgs-secure-omcc', Number(status.xgspon_secure_omcc_info || 0) & 0x300 ?
		_('Partial') : _('Unavailable'));
	setText('gpon-activation', stateLabel(status));
	setText('gpon-optical', opticalLabel(status));
	setText('gpon-phy-link', phyLinkLabel(status.optical_link));
	setText('gpon-ber-sample', berLabel(status.ber_sample));
	setText('gpon-link', status.link_state);
	setText('gpon-onu-id', status.onu_id);
	setText('gpon-omcc-id', status.omcc_id);
	setText('gpon-tconts', status.tconts);
	setText('gpon-data-gem', status.data_gems || status.data_gem);
	setText('gpon-eqd', status.equalization_delay);
	setText('gpon-calibration', status.calibration_source);
	setText('gpon-fw-version', status.firmware_version);
	setText('gpon-temperature', diagnostics.temperature == null ? '-' :
		_('%s C').format((signed16(diagnostics.temperature) / 256).toFixed(2)));
	setText('gpon-voltage', diagnostics.supply_voltage == null ? '-' :
		_('%s V').format((Number(diagnostics.supply_voltage) / 10000).toFixed(4)));
	setText('gpon-bias', diagnostics.laser_bias_current == null ? '-' :
		_('%s mA').format((Number(diagnostics.laser_bias_current) / 500).toFixed(2)));
	setText('gpon-tx-power', diagnostics.transmit_power == null ? '-' :
		opticalPower(diagnostics.transmit_power));
	setText('gpon-rx-power', diagnostics.receive_power == null ? '-' :
		opticalPower(diagnostics.receive_power));
	setText('gpon-rx-bytes', status.rx_bytes);
	setText('gpon-tx-bytes', status.tx_bytes);
	setText('gpon-omci', status.omci_available ? _('Automatic') :
		_('Unavailable'));
	setText('gpon-omci-state', omci.state);
	setText('gpon-epon-mpcp', status.epon_mpcp_state);
	setText('gpon-epon-oam', status.epon_oam_available ? _('Online') : _('Unavailable'));
	setText('gpon-epon-oam-frames', _('%s received / %s sent').format(
		text(eponOam.rx_frames), text(eponOam.tx_frames)));
	setText('gpon-epon-oam-errors', _('%s malformed / %s dropped').format(
		text(eponOam.malformed_frames), text(eponOam.dropped_frames)));
	setText('gpon-epon-extensions', _('CTC/CUC: %s; DPoE: %s').format(
		eponOam.ctc_management ? _('management ready') :
			(eponOam.ctc_dispatch ? _('dispatch only') : _('disabled')),
		eponOam.dpoe_management ? _('management ready') :
			(eponOam.dpoe_dispatch ? _('dispatch only') : _('disabled'))));
	setText('gpon-mib-sync', omci.mib_data_sync);
	setText('gpon-mib-entries', omci.mib_entries);
	setText('gpon-omci-frames', _('%s received / %s sent').format(
		text(omci.rx_frames), text(omci.tx_frames)));
	setText('gpon-omci-errors', _('%s decode / %s transport / %s event').format(
		text(omci.decode_errors), text(omci.transport_errors),
		text(omci.event_errors)));
	setText('gpon-omci-notifications', _('%s sent, last message 0x%s').format(
		text(omci.notification_frames),
		Number(omci.last_notification_type || 0).toString(16).padStart(2, '0')));
	setText('gpon-omci-last', omci.last_transaction_id == null ? '-' :
		_('TCI %s, message 0x%s').format(omci.last_transaction_id,
			Number(omci.last_message_type || 0).toString(16).padStart(2, '0')));
	setText('gpon-software-state', softwarePhaseLabel(omci.software_phase));
	setText('gpon-software-progress', omci.software_image_size ?
		_('Image %s: %s / %s bytes').format(text(omci.software_image_id),
			text(omci.software_bytes), text(omci.software_image_size)) : '-');
	setText('gpon-software-hash', omci.software_image_hash);
	setText('gpon-platform-state', platformStateLabel(platform.state));
	setText('gpon-platform-gem', platform.configured_gems);
	setText('gpon-platform-count', platform.provisioned_gems);
	setText('gpon-platform-bridges', platform.configured_bridges);
	setText('gpon-platform-classifiers', platform.classifiers);
	updateServiceGraph(status.service_graph);

	const safety = status.safety || {};
	setText('gpon-safety', _('Ready: %s, fault: %s, OLT disabled: %s').format(
		safety.ready === '1' ? _('yes') : _('no'),
		safety.rogue_fault === '1' ? _('yes') : _('no'),
		safety.olt_disabled === '1' ? _('yes') : _('no')));
}

function statusRow(label, id) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '35%' }, label),
		E('td', { 'class': 'td left', 'id': id }, '-')
	]);
}

function renderStatus() {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('Live Status')),
		E('table', { 'class': 'table' }, [
			statusRow(_('PON protocol'), 'gpon-mode'),
			statusRow(_('XGS-PON readiness'), 'gpon-xgs-ready'),
			statusRow(_('XGS-PON blockers'), 'gpon-xgs-blockers'),
			statusRow(_('Registration-ID'), 'gpon-xgs-registration'),
				statusRow(_('XGS key derivation'), 'gpon-xgs-key-derivation'),
				statusRow(_('XGS activation evidence'), 'gpon-xgs-activation-evidence'),
				statusRow(_('XGS PHY evidence'), 'gpon-xgs-phy-evidence'),
			statusRow(_('XGS control ABI'), 'gpon-xgs-control-abi'),
			statusRow(_('XGS service'), 'gpon-xgs-service-state'),
			statusRow(_('Secure OMCC'), 'gpon-xgs-secure-omcc'),
			statusRow(_('Activation state'), 'gpon-activation'),
			statusRow(_('Optical transmitter'), 'gpon-optical'),
			statusRow(_('GPON PHY state'), 'gpon-phy-link'),
			statusRow(_('Latest BER interval'), 'gpon-ber-sample'),
			statusRow(_('PON link'), 'gpon-link'),
			statusRow(_('Safety chain'), 'gpon-safety'),
			statusRow(_('ONU ID'), 'gpon-onu-id'),
			statusRow(_('OMCC GEM ID'), 'gpon-omcc-id'),
			statusRow(_('T-CONTs'), 'gpon-tconts'),
			statusRow(_('Data GEMs'), 'gpon-data-gem'),
			statusRow(_('Equalization delay'), 'gpon-eqd'),
			statusRow(_('OMCI control plane'), 'gpon-omci'),
			statusRow(_('OMCI daemon'), 'gpon-omci-state'),
			statusRow(_('EPON MPCP/LLIDs'), 'gpon-epon-mpcp'),
			statusRow(_('EPON OAM control plane'), 'gpon-epon-oam'),
			statusRow(_('EPON OAM frames'), 'gpon-epon-oam-frames'),
			statusRow(_('EPON OAM errors'), 'gpon-epon-oam-errors'),
			statusRow(_('EPON extension support'), 'gpon-epon-extensions'),
			statusRow(_('MIB data sync'), 'gpon-mib-sync'),
			statusRow(_('Managed entities'), 'gpon-mib-entries'),
			statusRow(_('OMCI frames'), 'gpon-omci-frames'),
			statusRow(_('OMCI errors'), 'gpon-omci-errors'),
			statusRow(_('Autonomous notifications'), 'gpon-omci-notifications'),
			statusRow(_('Last OMCI transaction'), 'gpon-omci-last'),
			statusRow(_('Software image state'), 'gpon-software-state'),
			statusRow(_('Software download'), 'gpon-software-progress'),
			statusRow(_('Software image MD5'), 'gpon-software-hash'),
			statusRow(_('Service apply state'), 'gpon-platform-state'),
			statusRow(_('Applied GEM mapping'), 'gpon-platform-gem'),
			statusRow(_('Provisioned GEMs'), 'gpon-platform-count'),
			statusRow(_('Applied service bridges'), 'gpon-platform-bridges'),
			statusRow(_('Installed traffic classifiers'), 'gpon-platform-classifiers'),
			statusRow(_('Calibration source'), 'gpon-calibration'),
			statusRow(_('BOSA firmware'), 'gpon-fw-version'),
			statusRow(_('Optical temperature'), 'gpon-temperature'),
			statusRow(_('Optical supply voltage'), 'gpon-voltage'),
			statusRow(_('Laser bias current'), 'gpon-bias'),
			statusRow(_('Transmit optical power'), 'gpon-tx-power'),
			statusRow(_('Receive optical power'), 'gpon-rx-power'),
			statusRow(_('Received bytes'), 'gpon-rx-bytes'),
			statusRow(_('Transmitted bytes'), 'gpon-tx-bytes')
		])
	]);
}

function graphTable(title, headers, bodyID) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		E('table', { 'class': 'table' }, [
			E('thead', {}, [ E('tr', { 'class': 'tr table-titles' }, headers.map(function(header) {
				return E('th', { 'class': 'th left' }, header);
			})) ]),
			E('tbody', { 'id': bodyID }, [])
		])
	]);
}

function renderServiceGraph() {
	return E('div', { 'class': 'cbi-section-node' }, [
		E('h2', {}, _('OLT Provisioned Service')),
		graphTable(_('GEM channels'), [
			_('GEM port'), _('Alloc-ID'), _('Direction'),
			_('Upstream queue'), _('Downstream queue')
		], 'gpon-gem-rows'),
		graphTable(_('Priority mapping'), [
			_('Mapper'), _('P-bit'), _('GEM port'), _('Interworking TP')
		], 'gpon-mapper-rows'),
		graphTable(_('Bridge attachments'), [
			_('Bridge'), _('Port'), _('Termination type'), _('Termination point')
		], 'gpon-bridge-rows'),
		graphTable(_('VLAN policies'), [
			_('Entity'), _('Association'), _('Target'), _('Mode'),
			_('Rules'), _('Downstream mode')
		], 'gpon-vlan-rows')
	]);
}

return view.extend({
	load: function() {
		return callStatus();
	},

	render: function(status) {
		let m, s, o;
		const config = status.config || {};

		this.formData = { main: {
			pon_mode: config.pon_mode || 'gpon',
			serial_number: config.serial_number || '',
			password: '',
			clear_password: '0',
			registration_id: '',
			clear_registration_id: '0',
			enabled: config.enabled || '0',
			omci_mode: config.omci_mode || 'auto',
			manual_gem: config.manual_gem || '0',
			gem_id: config.gem_id || '0',
			tcont: config.tcont || '0',
			encrypted: config.encrypted || '0',
			epon_onu_mac: config.epon_onu_mac || '',
			epon_llid_mask: config.epon_llid_mask || '0x1',
			epon_silent_time: config.epon_silent_time || '60',
			epon_local_oui: config.epon_local_oui || '000db6',
			epon_ctc_enabled: config.epon_ctc_enabled || '1',
			epon_ctc_oui: config.epon_ctc_oui || '111111',
			epon_dpoe_enabled: config.epon_dpoe_enabled || '1',
			epon_loid: config.epon_loid || '',
			epon_password: '',
			clear_epon_password: '0'
		} };
		m = new form.JSONMap(this.formData, _('PON Optical Port'));
		m.readonly = !L.hasViewPermission();

		s = m.section(form.NamedSection, 'main', 'main',
			_('ONU Registration'));
		s.anonymous = true;

		o = s.option(form.ListValue, 'pon_mode', _('PON protocol'));
		o.value('gpon', _('GPON'));
		o.value('xgpon', _('XG-PON (10G/2.5G)'));
		o.value('xgspon', _('XGS-PON (10G/10G)'));
		o.value('epon-10g-1g', _('10G-EPON (10G/1G)'));
		o.value('epon-10g-10g', _('10G-EPON (10G/10G)'));
		o.default = 'gpon';
		o.rmempty = false;

		o = s.option(form.Value, 'serial_number', _('ONU serial number'));
		o.depends('pon_mode', 'gpon');
		o.depends('pon_mode', 'xgpon');
		o.depends('pon_mode', 'xgspon');
		o.placeholder = 'HWTC12345678';
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			return /^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$/.test(value || '') ||
				_('Use four vendor characters followed by eight hexadecimal digits.');
		};

		o = s.option(form.Value, 'password', _('PLOAM password'));
		o.depends('pon_mode', 'gpon');
		o.password = true;
		o.placeholder = config.password_configured ? _('Configured') : _('Not configured');
		o.validate = function(sectionId, value) {
			if ((value || '').startsWith('hex:'))
				return /^hex:[0-9A-Fa-f]{20}$/.test(value) ||
					_('Use at most ten printable ASCII characters or hex: followed by twenty hexadecimal digits.');
			return value === '' || /^[\x21-\x7e]{1,10}$/.test(value || '') ||
				_('Use at most ten printable ASCII characters or hex: followed by twenty hexadecimal digits.');
		};

		o = s.option(form.Flag, 'clear_password', _('Clear PLOAM password'));
		o.depends('pon_mode', 'gpon');
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.Value, 'registration_id', _('Registration-ID'));
		o.password = true;
		o.depends('pon_mode', 'xgpon');
		o.depends('pon_mode', 'xgspon');
		o.placeholder = config.registration_id_configured ?
			_('Configured') : _('Not configured');
		o.validate = function(sectionId, value) {
			if ((value || '').startsWith('hex:'))
				return /^hex:[0-9A-Fa-f]{72}$/.test(value) ||
					_('Use at most 36 printable ASCII characters or hex: followed by 72 hexadecimal digits.');
			return value === '' || /^[\x21-\x7e]{1,36}$/.test(value || '') ||
				_('Use at most 36 printable ASCII characters or hex: followed by 72 hexadecimal digits.');
		};

		o = s.option(form.Flag, 'clear_registration_id', _('Clear Registration-ID'));
		o.depends('pon_mode', 'xgpon');
		o.depends('pon_mode', 'xgspon');
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.Flag, 'enabled', _('Enable PON activation'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.ListValue, 'omci_mode', _('Service provisioning'));
		o.value('auto', _('Automatic (OMCI)'));
		o.value('manual', _('Manual'));
		o.default = 'auto';
		o.rmempty = false;
		o.depends('pon_mode', 'gpon');

		s = m.section(form.NamedSection, 'main', 'main',
			_('Manual Service Channel'));
		s.anonymous = true;

		o = s.option(form.Flag, 'manual_gem', _('Enable manual data GEM'));
		o.default = '0';
		o.rmempty = false;
		o.depends({ pon_mode: 'gpon', omci_mode: 'manual' });

		o = s.option(form.Value, 'gem_id', _('GEM port ID'));
		o.datatype = 'range(0,4095)';
		o.depends({ pon_mode: 'gpon', omci_mode: 'manual', manual_gem: '1' });
		o.rmempty = false;

		o = s.option(form.Value, 'tcont', _('T-CONT channel'));
		o.datatype = 'range(0,15)';
		o.depends({ pon_mode: 'gpon', omci_mode: 'manual', manual_gem: '1' });
		o.rmempty = false;

		o = s.option(form.Flag, 'encrypted', _('Encrypted downstream'));
		o.default = '0';
		o.depends({ pon_mode: 'gpon', omci_mode: 'manual', manual_gem: '1' });
		o.rmempty = false;

		s = m.section(form.NamedSection, 'main', 'main', _('EPON OAM'));
		s.anonymous = true;

		o = s.option(form.Value, 'epon_onu_mac', _('EPON ONU MAC address'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.placeholder = '02:11:22:33:44:55';
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			if (!/^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(value || ''))
				return _('Enter a unicast MAC address.');
			return !(parseInt(value.slice(0, 2), 16) & 1) &&
				value.toLowerCase() !== '00:00:00:00:00:00' ||
				_('Enter a non-zero unicast MAC address.');
		};

		o = s.option(form.Value, 'epon_llid_mask', _('Enabled LLIDs'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.placeholder = '0x1';
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			if (!/^(0[xX][0-9A-Fa-f]+|[0-9]+)$/.test(value || ''))
				return _('Use a non-zero 32-bit decimal or hexadecimal LLID mask.');
			const number = Number(value);
			return number >= 1 && number <= 0xffffffff ||
				_('Use a non-zero 32-bit decimal or hexadecimal LLID mask.');
		};

		o = s.option(form.Value, 'epon_silent_time', _('Registration retry delay'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.datatype = 'range(1,3600)';
		o.rmempty = false;

		o = s.option(form.Value, 'epon_local_oui', _('Local OUI'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			return /^([0-9A-Fa-f]{6}|[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){2})$/.test(value || '') ||
				_('Use three hexadecimal OUI octets.');
		};

		o = s.option(form.Flag, 'epon_ctc_enabled', _('Enable CTC/CUC OAM'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Value, 'epon_ctc_oui', _('CTC/CUC OUI'));
		o.depends({ pon_mode: 'epon-10g-1g', epon_ctc_enabled: '1' });
		o.depends({ pon_mode: 'epon-10g-10g', epon_ctc_enabled: '1' });
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			return /^([0-9A-Fa-f]{6}|[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){2})$/.test(value || '') ||
				_('Use three hexadecimal OUI octets.');
		};

		o = s.option(form.Flag, 'epon_dpoe_enabled', _('Enable DPoE OAM'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Value, 'epon_loid', _('LOID'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.validate = function(sectionId, value) {
			return String(value || '').length <= 24 || _('Use at most 24 characters.');
		};

		o = s.option(form.Value, 'epon_password', _('EPON OAM password'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.password = true;
		o.placeholder = config.epon_password_configured ? _('Configured') : _('Not configured');
		o.validate = function(sectionId, value) {
			return String(value || '').length <= 12 || _('Use at most 12 characters.');
		};

		o = s.option(form.Flag, 'clear_epon_password', _('Clear EPON OAM password'));
		o.depends('pon_mode', 'epon-10g-1g');
		o.depends('pon_mode', 'epon-10g-10g');
		o.default = '0';
		o.rmempty = false;

		poll.add(function() {
			return L.resolveDefault(callStatus(), {}).then(updateStatus);
		}, 2);

		return m.render().then(function(formNode) {
			const statusNode = renderStatus();
			const graphNode = renderServiceGraph();
			const serviceNode = E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('10G ITU-T PON Service')),
				E('p', { 'class': ' cbi-section-descr' },
					_('OMCI normally provisions XGS T-CONT and XGEM entries automatically.')),
				E('button', {
					'class': 'cbi-button cbi-button-reset',
					'click': function() {
						if (status.pon_mode !== 'xgpon' && status.pon_mode !== 'xgspon')
							return;
						return callXgsServiceClear().then(function(result) {
							if (!result || !result.success)
								throw new Error(result && result.error || _('Failed to clear XGS service.'));
							ui.addNotification(null, E('p',
								_('XGS-PON service has been cleared.')), 'info');
							return callStatus().then(updateStatus);
						});
					},
					'disabled': (status.pon_mode !== 'xgpon' &&
						status.pon_mode !== 'xgspon') || !status.xgspon_present
				}, _('Clear XGS service'))
			]);
			requestAnimationFrame(function() { updateStatus(status); });
			return E('div', {}, [ statusNode, serviceNode, graphNode, formNode ]);
		});
	},

	saveConfiguration: function() {
		const map = document.querySelector('.cbi-map');

		return dom.callClassMethod(map, 'save').then(L.bind(function() {
			return callSetConfig(this.formData.main);
		}, this)).then(function(result) {
			if (!result || !result.success)
				throw new Error(result && result.error || _('Failed to save PON configuration.'));
			ui.addNotification(null, E('p', _('PON configuration has been applied.')), 'info');
		});
	},

	handleSave: function() {
		return this.saveConfiguration();
	},

	handleSaveApply: function() {
		return this.saveConfiguration();
	}
});
