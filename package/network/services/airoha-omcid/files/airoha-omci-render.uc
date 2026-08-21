#!/usr/bin/env ucode

'use strict';

import { readfile } from 'fs';

// A zero G.988 burst requests the ONU factory policy. XG2010G Ethernet UNIs
// advertise a 2000-byte maximum frame, so one maximum frame is the minimum
// useful token bucket for a non-zero rate.
let factory_burst = 2000;

function fail(message) {
	warn(`airoha-omci-render: ${message}\n`);
	exit(1);
}

function number(value, minimum, maximum, name) {
	let parsed = +value;
	if (parsed != value || parsed < minimum || parsed > maximum || parsed % 1)
		fail(`invalid ${name}`);
	return parsed;
}

function address(value, name) {
	if (type(value) != 'string' || !length(value))
		fail(`invalid ${name}`);
	return value;
}

function multicast_acl(entry, description, gem_port_limit) {
	number(entry.row_key, 0, 1023, `${description} row key`);
	let version = number(entry.ip_version, 4, 6, `${description} IP version`);
	if (version != 4 && version != 6)
		fail(`invalid ${description} IP version`);
	number(entry.gem_port_id, 0, gem_port_limit, `${description} GEM Port-ID`);
	let vlan = number(entry.vlan_id, 0, 65535, `${description} VLAN ID`);
	if (vlan > 4097 && vlan != 65535)
		fail(`invalid ${description} VLAN ID`);
	address(entry.source, `${description} source address`);
	address(entry.start, `${description} range start`);
	address(entry.stop, `${description} range stop`);
	number(entry.imputed_bandwidth, 0, 0xffffffff, `${description} imputed bandwidth`);
	number(entry.preview_length, 0, 65535, `${description} preview length`);
	number(entry.preview_repeat_time, 0, 65535, `${description} preview repeat time`);
	number(entry.preview_repeat_count, 0, 65535, `${description} preview repeat count`);
	number(entry.preview_reset_time, 0, 65535, `${description} preview reset time`);
}

function merge_target(targets, index, target, description) {
	if (targets[index] != null && targets[index] != target)
		fail(`conflicting ${description} mapping ${targets[index]}/${target}`);
	targets[index] = target;
}

function add_unique(values, value) {
	if (index(values, value) < 0)
		push(values, value);
}

function centiseconds(value) {
	return int((value * 100 + 128) / 256);
}

function bpf_label(program, name) {
	push(program, { label: name });
}

function bpf_instruction(program, code, k) {
	push(program, { code, k, jt: null, jf: null, target: null });
}

function bpf_equal(program, k, yes, no) {
	push(program, { code: 21, k, jt: yes, jf: no, target: null });
}

function bpf_jump(program, target) {
	push(program, { code: 5, k: 0, jt: null, jf: null, target });
}

function assemble_bpf(program) {
	let labels = {};
	let count = 0;
	for (let item in program) {
		if (item.label != null)
			labels[item.label] = count;
		else
			count++;
	}
	let encoded = [];
	let pc = 0;
	for (let item in program) {
		if (item.label != null)
			continue;
		let jt = item.jt != null ? labels[item.jt] - pc - 1 : 0;
		let jf = item.jf != null ? labels[item.jf] - pc - 1 : 0;
		let k = item.target != null ? labels[item.target] - pc - 1 : item.k;
		if (jt < 0 || jt > 255 || jf < 0 || jf > 255 || k < 0)
			fail('VLAN filter BPF branch is out of range');
		push(encoded, `${item.code} ${jt} ${jf} ${k}`);
		pc++;
	}
	return `${count},${join(',', encoded)}`;
}

function vlan_forward_policy(operation) {
	let untagged = index([ 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x10,
		0x12, 0x14, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f, 0x21 ], operation) >= 0 ? 'c' : 'a';
	switch (operation) {
	case 0x00:
	case 0x02:
	case 0x15:
		return { tagged: 'a', criterion: 'none', untagged };
	case 0x01:
		return { tagged: 'c', criterion: 'none', untagged };
	case 0x03:
	case 0x04:
	case 0x0f:
	case 0x10:
	case 0x1c:
	case 0x1d:
		return { tagged: 'h', criterion: 'vid', untagged };
	case 0x05:
	case 0x06:
		return { tagged: 'g', criterion: 'vid', untagged };
	case 0x07:
	case 0x08:
	case 0x11:
	case 0x12:
	case 0x1e:
	case 0x1f:
		return { tagged: 'h', criterion: 'priority', untagged };
	case 0x09:
	case 0x0a:
		return { tagged: 'g', criterion: 'priority', untagged };
	case 0x0b:
	case 0x0c:
	case 0x13:
	case 0x14:
	case 0x20:
	case 0x21:
		return { tagged: 'h', criterion: 'tci', untagged };
	case 0x0d:
	case 0x0e:
		return { tagged: 'g', criterion: 'tci', untagged };
	case 0x16:
	case 0x17:
		return { tagged: 'j', criterion: 'vid', untagged };
	case 0x18:
	case 0x19:
		return { tagged: 'j', criterion: 'priority', untagged };
	case 0x1a:
	case 0x1b:
		return { tagged: 'j', criterion: 'tci', untagged };
	default:
		fail(`reserved VLAN forward operation ${operation}`);
	}
}

function vlan_filter_bpf(untagged, tagged, criterion, entries, egress) {
	let tagged_policy = 'pass';
	if (tagged == 'c')
		tagged_policy = 'drop';
	else if (tagged == 'g' && egress)
		tagged_policy = 'drop-match';
	else if (tagged == 'h')
		tagged_policy = 'drop-mismatch';
	else if (tagged == 'j' && egress)
		tagged_policy = 'drop-mismatch';

	if (untagged == 'a' && tagged_policy == 'pass')
		return null;

	let program = [];
	// Prefer skb VLAN metadata, then recognize an in-frame 802.1Q/802.1ad tag.
	bpf_instruction(program, 32, 4294963248); // SKF_AD_VLAN_TAG_PRESENT
	bpf_equal(program, 0, 'wire-protocol', 'metadata-tag');
	bpf_label(program, 'metadata-tag');
	bpf_instruction(program, 32, 4294963244); // SKF_AD_VLAN_TAG
	bpf_instruction(program, 84, 65535);
	bpf_jump(program, 'tagged');
	bpf_label(program, 'wire-protocol');
	bpf_instruction(program, 40, 12);
	bpf_equal(program, 0x8100, 'wire-tag', 'wire-ad');
	bpf_label(program, 'wire-ad');
	bpf_equal(program, 0x88a8, 'wire-tag', 'wire-9100');
	bpf_label(program, 'wire-9100');
	bpf_equal(program, 0x9100, 'wire-tag', 'untagged');
	bpf_label(program, 'wire-tag');
	bpf_instruction(program, 40, 14);
	bpf_jump(program, 'tagged');
	bpf_label(program, 'untagged');
	bpf_instruction(program, 6, untagged == 'c' ? 4294967295 : 0);
	bpf_label(program, 'tagged');
	if (tagged_policy == 'pass')
		bpf_instruction(program, 6, 0);
	else if (tagged_policy == 'drop')
		bpf_instruction(program, 6, 4294967295);
	else {
		let mask = criterion == 'vid' ? 0x0fff : (criterion == 'priority' ? 0xe000 : 0xffff);
		bpf_instruction(program, 84, mask);
		for (let entry in entries)
			bpf_equal(program, entry & mask, 'matched', null);
		bpf_instruction(program, 6, tagged_policy == 'drop-mismatch' ? 4294967295 : 0);
		bpf_label(program, 'matched');
		bpf_instruction(program, 6, tagged_policy == 'drop-match' ? 4294967295 : 0);
	}
	return assemble_bpf(program);
}

function emit_vlan_filter(ifname, hook, bytecode, entity) {
	if (bytecode == null)
		return;
	print(`vfilter ${ifname} ${hook} ${replace(bytecode, ' ', ':')}:${entity}\n`);
}

function claim_tagging_target(targets, class_id, entity_id, owner) {
	let key = `${class_id}:${entity_id}`;
	if (targets[key] != null)
		fail(`VLAN operation ${owner} shares target ${key} with ${targets[key]}`);
	targets[key] = owner;
}

function normalized_filter(tag, input_tpid, name) {
	let priority = number(tag.priority, 0, 15, `${name} filter priority`);
	if (priority > 8 && priority != 14 && priority != 15)
		fail(`reserved ${name} filter priority`);
	let vid = number(tag.vid, 0, 4096, `${name} filter VID`);
	if (vid == 4095)
		fail(`reserved ${name} filter VID`);
	let tpid_dei = number(tag.tpid_dei, 0, 7, `${name} filter TPID/DEI`);
	if (tpid_dei != 0 && tpid_dei < 4)
		fail(`reserved ${name} filter TPID/DEI`);
	return {
		priority: priority <= 7 ? priority : -1,
		vid: vid <= 4094 ? vid : -1,
		tpid: tpid_dei == 4 ? 0x8100 : (tpid_dei >= 5 ? input_tpid : 0),
		dei: tpid_dei >= 6 ? tpid_dei - 6 : -1
	};
}

function treatment_descriptor(tag, name) {
	let priority = number(tag.priority, 0, 15, `${name} treatment priority`);
	let vid = number(tag.vid, 0, 4097, `${name} treatment VID`);
	let tpid_dei = number(tag.tpid_dei, 0, 7, `${name} treatment TPID/DEI`);
	if (priority == 15)
		return null;
	if (priority > 10)
		fail(`reserved ${name} treatment priority`);
	if (tpid_dei == 5)
		fail(`reserved ${name} treatment TPID/DEI`);
	return { priority, vid, tpid_dei };
}

function copied_component(source, field, source_name, tags, name) {
	if (tags == 0)
		fail(`${name} copies ${field} from a tag on an untagged frame`);
	// A single physical tag is described by the G.988 inner fields but is the
	// outer tag in the kernel snapshot used by the atomic transform action.
	let physical_source = tags == 1 && source_name == 'inner' ? 'outer' : source_name;
	return { value: source[field], source: physical_source };
}

function resolved_treatment(treatment, received_outer, received_inner, tags,
		output_tpid, name) {
	if (treatment == null)
		return null;

	let priority = treatment.priority;
	let priority_source = 'fixed';
	if (priority == 8 || priority == 9) {
		let source_name = priority == 8 ? 'inner' : 'outer';
		let source = priority == 8 ? received_inner : received_outer;
		let copied = copied_component(source, 'priority', source_name, tags, name);
		priority = copied.value;
		priority_source = copied.source;
	}
	// Priority 10 remains symbolic until emit_evto() expands the rule into
	// packet-specific IPv4 and IPv6 classifiers.

	let vid = treatment.vid;
	let vid_source = 'fixed';
	if (vid >= 4096) {
		let source_name = vid == 4096 ? 'inner' : 'outer';
		let source = vid == 4096 ? received_inner : received_outer;
		let copied = copied_component(source, 'vid', source_name, tags, name);
		vid = copied.value;
		vid_source = copied.source;
	}

	let source_name = (treatment.tpid_dei == 0 || treatment.tpid_dei == 2) ? 'inner' : 'outer';
	let source = source_name == 'inner' ? received_inner : received_outer;
	let tpid;
	let dei;
	let tpid_source = 'fixed';
	let dei_source = 'fixed';
	switch (treatment.tpid_dei) {
	case 0:
	case 1:
		let copied_tpid = copied_component(source, 'tpid', source_name, tags, name);
		let copied_dei = copied_component(source, 'dei', source_name, tags, name);
		tpid = copied_tpid.value;
		dei = copied_dei.value;
		tpid_source = copied_tpid.source;
		dei_source = copied_dei.source;
		break;
	case 2:
	case 3:
		tpid = output_tpid;
		let copied = copied_component(source, 'dei', source_name, tags, name);
		dei = copied.value;
		dei_source = copied.source;
		break;
	case 4:
		tpid = 0x8100;
		dei = 0;
		break;
	case 6:
	case 7:
		tpid = output_tpid;
		dei = treatment.tpid_dei - 6;
		break;
	}
	if (tpid_source == 'fixed' && tpid != 0x8100 && tpid != 0x88a8)
		fail(`${name} treatment TPID is not supported by tc vlan`);
	return {
		priority, vid, tpid, dei,
		priority_source, vid_source, tpid_source, dei_source
	};
}

function normalized_rule(rule, input_tpid, output_tpid, mapping) {
	let received_outer = normalized_filter(rule.filter_outer || {}, input_tpid, 'outer');
	let received_inner = normalized_filter(rule.filter_inner || {}, input_tpid, 'inner');
	let outer = received_outer;
	let inner = received_inner;
	let outer_priority = number((rule.filter_outer || {}).priority, 0, 15, 'outer filter priority');
	let inner_priority = number((rule.filter_inner || {}).priority, 0, 15, 'inner filter priority');
	let tags;
	if (inner_priority == 15) {
		if (outer_priority != 15)
			fail('zero-tag rule has an outer tag filter');
		tags = 0;
	}
	else if (outer_priority == 15)
		tags = 1;
	else
		tags = 2;

	let treatment_outer = treatment_descriptor(rule.treatment_outer || {}, 'outer');
	let treatment_inner = treatment_descriptor(rule.treatment_inner || {}, 'inner');
	if (treatment_outer != null && treatment_inner == null)
		fail('outer treatment cannot add a tag without an inner treatment');
	let additions = treatment_outer != null ? 2 : (treatment_inner != null ? 1 : 0);
	let remove = number(rule.tags_to_remove, 0, 3, 'tags to remove');
	if (remove < 3 && remove > tags)
		fail(`cannot remove ${remove} tags from a ${tags}-tag frame`);
	let ethertype = number(rule.ethertype, 0, 5, 'filter Ethertype');
	let extended = number(rule.extended_criteria, 0, 2, 'extended filter criteria');
	if (extended && ethertype && !((extended == 1 && ethertype == 1) ||
		(extended == 2 && ethertype == 4)))
		fail('extended protocol criteria conflicts with Ethertype filter');
	treatment_outer = resolved_treatment(treatment_outer, received_outer, received_inner, tags,
		output_tpid, 'outer treatment');
	treatment_inner = resolved_treatment(treatment_inner, received_outer, received_inner, tags,
		output_tpid, 'inner treatment');

	// A single physical tag is encoded in the G.988 inner fields.
	if (tags == 1) {
		outer = inner;
		inner = { priority: -1, vid: -1, tpid: 0, dei: -1 };
	}
	else if (tags == 0) {
		outer = { priority: -1, vid: -1, tpid: 0, dei: -1 };
		inner = { priority: -1, vid: -1, tpid: 0, dei: -1 };
	}

	return {
		order: number(rule.order, 0, 65535, 'VLAN rule order'),
		tags,
		outer,
		inner,
		ethertype,
		extended,
		drop: remove == 3 ? 1 : 0,
		remove: remove == 3 ? 0 : remove,
		additions,
		add_outer: treatment_outer,
		add_inner: treatment_inner
	};
}

function restored_tag(filter) {
	return {
		priority: filter.priority >= 0 ? filter.priority : 0,
		vid: filter.vid >= 0 ? filter.vid : 0,
		tpid: filter.tpid || 0x8100,
		dei: filter.dei >= 0 ? filter.dei : 0,
		priority_source: 'fixed', vid_source: 'fixed',
		tpid_source: 'fixed', dei_source: 'fixed'
	};
}

function dynamic_component(tag, field, source) {
	tag[`${field}_source`] = source;
	tag[field] = field == 'tpid' ? 0 : -1;
}

function component_known(tag, field) {
	return field == 'tpid' ? tag[field] != 0 : tag[field] >= 0;
}

function copied_output_source(rule, original_source, field) {
	let candidates = [];
	if (rule.additions == 2)
		push(candidates, { tag: rule.add_outer, location: 'outer' });
	if (rule.additions >= 1)
		push(candidates, {
			tag: rule.add_inner,
			location: rule.additions == 2 ? 'inner' : 'outer'
		});
	for (let candidate in candidates)
		if (candidate.tag[`${field}_source`] == original_source)
			return candidate.location;
	return null;
}

function restore_packet_components(rule, restored, filter, original_source) {
	for (let field in [ 'priority', 'vid', 'tpid', 'dei' ]) {
		if (component_known(filter, field))
			continue;
		let source = copied_output_source(rule, original_source, field);
		if (source != null)
			dynamic_component(restored, field, source);
	}
}

function corresponding_treatment_source(rule, restored_outer) {
	if (rule.additions == 0)
		return null;
	if (restored_outer)
		return rule.additions == 2 ? 'outer' : null;
	return rule.additions == 2 ? 'inner' : 'outer';
}

function inverse_rule(rule, downstream_mode) {
	if (rule.drop)
		return null;

	let empty = { priority: -1, vid: -1, tpid: 0, dei: -1 };
	let received = [];
	if (rule.tags == 1)
		push(received, rule.outer);
	else if (rule.tags == 2) {
		push(received, rule.outer);
		push(received, rule.inner);
	}
	let transmitted = [];
	if (rule.additions == 2)
		push(transmitted, rule.add_outer);
	if (rule.additions >= 1)
		push(transmitted, rule.add_inner);
	for (let index = rule.remove; index < length(received); index++)
		push(transmitted, received[index]);

	let tags = length(transmitted);
	let outer = tags > 0 ? { ...transmitted[0] } : { ...empty };
	// G.988 permits more than two resulting tags but defines downstream
	// filtering from the outer tag when the inner information is unavailable.
	let inner = tags == 2 ? { ...transmitted[1] } : { ...empty };
	let restore_outer = null;
	let restore_inner = null;
	if (rule.remove == 1)
		restore_inner = restored_tag(received[0]);
	else if (rule.remove == 2) {
		restore_outer = restored_tag(received[0]);
		restore_inner = restored_tag(received[1]);
	}
	if (restore_outer != null)
		restore_packet_components(rule, restore_outer, received[0], 'outer');
	if (restore_inner != null)
		restore_packet_components(rule, restore_inner,
			received[rule.remove == 1 ? 0 : 1], rule.remove == 1 ? 'outer' : 'inner');
	if (downstream_mode == 3 || downstream_mode == 6) {
		// VID-only inverse matching passes P-bit and DEI through from the
		// corresponding treatment tag.
		outer.priority = -1;
		outer.dei = -1;
		inner.priority = -1;
		inner.dei = -1;
		let outer_source = corresponding_treatment_source(rule, true);
		let inner_source = corresponding_treatment_source(rule, false);
		if (restore_outer != null && outer_source != null) {
			dynamic_component(restore_outer, 'priority', outer_source);
			dynamic_component(restore_outer, 'dei', outer_source);
		}
		if (restore_inner != null && inner_source != null) {
			dynamic_component(restore_inner, 'priority', inner_source);
			dynamic_component(restore_inner, 'dei', inner_source);
		}
	}
	else if (downstream_mode == 4 || downstream_mode == 7) {
		// P-bit-only inverse matching and treatment retain VID and DEI from the
		// corresponding treatment tag.
		outer.vid = -1;
		outer.dei = -1;
		inner.vid = -1;
		inner.dei = -1;
		let outer_source = corresponding_treatment_source(rule, true);
		let inner_source = corresponding_treatment_source(rule, false);
		if (restore_outer != null && outer_source != null) {
			dynamic_component(restore_outer, 'vid', outer_source);
			dynamic_component(restore_outer, 'dei', outer_source);
		}
		if (restore_inner != null && inner_source != null) {
			dynamic_component(restore_inner, 'vid', inner_source);
			dynamic_component(restore_inner, 'dei', inner_source);
		}
	}
	return {
		order: rule.order,
		tags,
		outer,
		inner,
		ethertype: rule.ethertype,
		extended: rule.extended,
		drop: 0,
		remove: rule.additions,
		additions: rule.remove,
		add_outer: restore_outer,
		add_inner: restore_inner
	};
}

function emit_evto_record(kind, ifname, entity, rule, gem, outer_priority, inner_priority,
		add_outer_priority, add_inner_priority, ip_version, dscp, branch) {
	let empty = {
		priority: -1, vid: -1, tpid: 0, dei: -1,
		priority_source: 'fixed', vid_source: 'fixed',
		tpid_source: 'fixed', dei_source: 'fixed'
	};
	let outer = rule.add_outer || empty;
	let inner = rule.add_inner || empty;
	let packed = `${entity}:${rule.order}:${rule.tags}:` +
		`${outer_priority}:${rule.outer.vid}:${rule.outer.tpid}:` +
		`${inner_priority}:${rule.inner.vid}:${rule.inner.tpid}:` +
		`${rule.ethertype}:${rule.extended}:${rule.drop}:${rule.remove}:${rule.additions}:` +
		`${add_outer_priority}:${outer.vid}:${outer.tpid}:${outer.dei}:` +
		`${add_inner_priority}:${inner.vid}:${inner.tpid}:${inner.dei}:${rule.slot}:` +
		`${rule.outer.dei}:${rule.inner.dei}:` +
		`${ip_version}:${dscp}:${branch}:` +
		`${outer.priority_source}:${outer.vid_source}:${outer.tpid_source}:${outer.dei_source}:` +
		`${inner.priority_source}:${inner.vid_source}:${inner.tpid_source}:${inner.dei_source}`;
	print(`${kind} ${ifname} ${packed} ${gem}\n`);
}

function emit_evto(kind, ifname, entity, rule, gem, mapping) {
	let outer = rule.add_outer || { priority: -1 };
	let inner = rule.add_inner || { priority: -1 };
	let derived_filter = rule.outer.priority == 10 || rule.inner.priority == 10;
	let derived_treatment = !rule.drop && (outer.priority == 10 || inner.priority == 10);
	if (derived_filter && derived_treatment)
		fail('DSCP-derived EVTO filter and treatment cannot be combined');

	if (derived_filter) {
		let emitted = {};
		for (let dscp = 0; dscp < 64; dscp++) {
			let priority = mapping[dscp];
			if (emitted[priority])
				continue;
			emitted[priority] = true;
			emit_evto_record(kind, ifname, entity, rule, gem,
				rule.outer.priority == 10 ? priority : rule.outer.priority,
				rule.inner.priority == 10 ? priority : rule.inner.priority,
				outer.priority, inner.priority, 0, -1, priority);
		}
		return;
	}

	if (!derived_treatment) {
		emit_evto_record(kind, ifname, entity, rule, gem, rule.outer.priority,
			rule.inner.priority, outer.priority, inner.priority, 0, -1, 0);
		return;
	}

	let versions;
	if (rule.extended == 1 || rule.ethertype == 1)
		versions = [ 4 ];
	else if (rule.extended == 2 || rule.ethertype == 4)
		versions = [ 6 ];
	else if (rule.ethertype == 0)
		versions = [ 4, 6 ];
	else
		fail('DSCP-derived EVTO treatment requires an IPv4, IPv6 or wildcard Ethertype filter');

	for (let ip_version in versions) {
		for (let dscp = 0; dscp < 64; dscp++) {
			let priority = mapping[dscp];
			let branch = (ip_version == 6 ? 64 : 0) + dscp;
			emit_evto_record(kind, ifname, entity, rule, gem, rule.outer.priority,
				rule.inner.priority, outer.priority == 10 ? priority : outer.priority,
				inner.priority == 10 ? priority : inner.priority,
				ip_version, dscp, branch);
		}
	}
}

let path = shift(ARGV) || '/var/run/airoha-omcid/desired.json';
let document = json(readfile(path));
if (!document || +document.version != 7 ||
	(document.state_domain != 'xg2010g:gpon' &&
	 document.state_domain != 'xg2010g:xgpon' &&
	 document.state_domain != 'xg2010g:xgspon') ||
	!document.mib_state || !document.service_graph)
	fail('unsupported or invalid platform ABI');
let operations = {
	'create': true, 'set': true, 'set-table': true, 'delete': true,
	'reset': true, 'command': true, 'autonomous': true
};
if (!operations[document.operation])
	fail('invalid operation');
number(document.mib_data_sync, 0, 255, 'MIB data sync');
if (+document.mib_state.version != 1 ||
	document.mib_state.state_domain != document.state_domain ||
	+document.mib_state.mib_data_sync != +document.mib_data_sync ||
	type(document.mib_state.instances) != 'array' || !length(document.mib_state.instances))
fail('invalid MIB state');
let pon_mode = substr(document.state_domain, 8);
let xg_mode = pon_mode == 'xgpon' || pon_mode == 'xgspon';
let gem_port_limit = xg_mode ? 65534 : 4095;
let graph = document.service_graph;
if (graph.pon_mode != null && graph.pon_mode != pon_mode)
	fail(`service graph PON mode ${graph.pon_mode} does not match ${pon_mode}`);
print(`pon-mode ${pon_mode} 0 0\n`);
let expected_interfaces = {
	'257': 'lan1',
	'258': 'lan2',
	'259': 'lan3',
	'260': 'lan4'
};
let unis = {};
let gems = {};
let gem_ports = {};
let tconts = {};
let tcont_qos = {};
let next_xgs_slot = 1;
let traffic_descriptors = {};
let rate_limiters = {};
let interworking = {};
let multicast_interworking = {};
let multicast_profiles = {};
let multicast_subscribers = [];
let mappers = {};
let bridges = {};
let bridge_ports = {};

for (let uni in graph.unis || []) {
	let entity = number(uni.entity_id, 257, 260, 'Ethernet UNI entity ID');
	let ifname = uni.interface;
	if (expected_interfaces[entity] == null || ifname != expected_interfaces[entity])
		fail(`invalid XG2010G interface mapping for UNI ${entity}`);
	if (unis[entity] != null)
		fail(`duplicate Ethernet UNI ${entity}`);
	let administrative = number(uni.administrative_state, 0, 1, 'UNI administrative state');
	number(uni.operational_state, 0, 1, 'UNI operational state');
	number(uni.configuration, 0, 255, 'UNI configuration');
	unis[entity] = { ifname, administrative };
	print(`uni ${ifname} ${administrative} ${entity}\n`);
}

for (let tcont in graph.tconts || []) {
	let entity = number(tcont.entity_id, 0, 65535, 'T-CONT entity ID');
	let alloc = number(tcont.alloc_id, 0, 65535, 'T-CONT Alloc-ID');
	if (alloc != 65535 &&
	    ((xg_mode && alloc > 16383) ||
	     (pon_mode == 'gpon' && alloc > 4095)))
		fail(`T-CONT ${entity} has an out-of-range Alloc-ID`);
	let policy = number(tcont.scheduler_policy || 0, 0, 2, 'T-CONT scheduler policy');
	let weight = number(tcont.scheduler_weight || 0, 0, 255, 'T-CONT scheduler weight');
	let queue_entities = tcont.queue_entities || [];
	let queue_weights = tcont.queue_weights || [];
	if (length(queue_entities) != 8 || length(queue_weights) != 8)
		fail(`T-CONT ${entity} does not contain eight queue entries`);
	let queues = [];
	let have_weight = false;
	for (let i = 0; i < 8; i++) {
		queues[i] = number(queue_entities[i], 0, 65535, `T-CONT ${entity} queue entity`);
		queue_weights[i] = number(queue_weights[i], 0, 255, `T-CONT ${entity} queue weight`);
		have_weight = have_weight || queue_weights[i] != 0;
	}
	if (policy == 2 && !have_weight)
		fail(`T-CONT ${entity} WRR scheduler has no non-zero queue weight`);
	if (tconts[entity] != null)
		fail(`duplicate T-CONT entity ${entity}`);
	let slot = null;
	if (xg_mode && alloc != 65535) {
		if (next_xgs_slot >= 32)
			fail('XG-PON/XGS-PON has more than 31 business T-CONTs');
		slot = next_xgs_slot++;
	}
	tconts[entity] = { entity, alloc, policy, weight, queues, queue_weights, slot };
}

for (let descriptor in graph.traffic_descriptors || []) {
	let entity = number(descriptor.entity_id, 0, 65535, 'traffic descriptor entity ID');
	if (traffic_descriptors[entity] != null)
		fail(`duplicate traffic descriptor ${entity}`);
	let cir = number(descriptor.cir, 0, 0xffffffff, `traffic descriptor ${entity} CIR`);
	let pir = number(descriptor.pir, 0, 0xffffffff, `traffic descriptor ${entity} PIR`);
	let cbs = number(descriptor.cbs, 0, 0xffffffff, `traffic descriptor ${entity} CBS`);
	let pbs = number(descriptor.pbs, 0, 0xffffffff, `traffic descriptor ${entity} PBS`);
	let colour = number(descriptor.colour_mode || 0, 0, 1,
		`traffic descriptor ${entity} colour mode`);
	let ingress_marking = number(descriptor.ingress_colour_marking || 0, 0, 7,
		`traffic descriptor ${entity} ingress colour marking`);
	let egress_marking = number(descriptor.egress_colour_marking || 0, 0, 7,
		`traffic descriptor ${entity} egress colour marking`);
	let meter = number(descriptor.meter_type || 0, 0, 2,
		`traffic descriptor ${entity} meter type`);
	if (colour || ingress_marking || egress_marking)
		fail(`traffic descriptor ${entity} colour marking requires native QDMA remarking`);
	if (meter == 1)
		fail(`traffic descriptor ${entity} RFC 4115 meter requires native QDMA coupling`);
	if (pir && cir > pir)
		fail(`traffic descriptor ${entity} CIR is above PIR`);
	traffic_descriptors[entity] = { entity, cir, pir, cbs, pbs, meter };
}

for (let gem in graph.gem_ports || []) {
	let entity = number(gem.entity_id, 0, 65535, 'GEM entity ID');
	let port = number(gem.port_id, xg_mode ? 1 : 0,
		gem_port_limit, 'GEM Port-ID');
	let alloc = number(gem.alloc_id, 0, 65535, 'Alloc-ID');
	let direction = number(gem.direction, 1, 3, 'GEM direction');
	let encryption = number(gem.encryption_key_ring || 0, 0, 3,
		`GEM ${entity} encryption key ring`);
	if (gems[entity] != null)
		fail(`duplicate GEM entity ${entity}`);
	let tcont = gem.tcont == null ? null : number(gem.tcont, 0, 65535, 'GEM T-CONT pointer');
	let upstream_td = number(gem.upstream_traffic_descriptor == null ? 65535 :
		gem.upstream_traffic_descriptor, 0, 65535, 'GEM upstream traffic descriptor');
	let downstream_td = number(gem.downstream_traffic_descriptor == null ? 65535 :
		gem.downstream_traffic_descriptor, 0, 65535, 'GEM downstream traffic descriptor');
	if (tcont != null && tconts[tcont] == null)
		fail(`GEM ${entity} references missing T-CONT ${tcont}`);
	if (upstream_td != 65535 && traffic_descriptors[upstream_td] == null)
		fail(`GEM ${entity} references missing upstream traffic descriptor ${upstream_td}`);
	if (downstream_td != 65535 && traffic_descriptors[downstream_td] == null)
		fail(`GEM ${entity} references missing downstream traffic descriptor ${downstream_td}`);
	if (gem_ports[port] != null)
		fail(`duplicate GEM Port-ID ${port}`);
	let configured = { port, alloc, direction, tcont, encryption, unicast: 1 };
	gems[entity] = { port, alloc, direction, tcont, upstream_td, downstream_td,
		encryption };
	gem_ports[port] = configured;
}

// One QDMA WAN channel represents one T-CONT. Multiple GEMs may share that
// channel, but their upstream descriptors must agree because EN7581 exposes
// one egress TRTCM meter per channel. Downstream red-drop policing is emitted
// separately per receive GEM mark below.
for (let tcont_id, tcont in tconts) {
	if (tcont.alloc == 65535)
		continue;
	let descriptor = null;
	for (let gem_id, gem in gems) {
		if (gem.tcont != tcont.entity)
			continue;
		if (gem.upstream_td == 65535)
			continue;
		let candidate = traffic_descriptors[gem.upstream_td];
		if (descriptor != null && descriptor.entity != candidate.entity)
			fail(`T-CONT ${tcont.entity} has conflicting upstream traffic descriptors`);
		descriptor = candidate;
	}
	if (descriptor == null)
		tcont_qos[tcont.entity] = { cir: 0, pir: 0, cbs: 0, pbs: 0 };
	else {
		let cbs = descriptor.cir && !descriptor.cbs ? factory_burst : descriptor.cbs;
		let pbs = descriptor.pir && !descriptor.pbs ? factory_burst : descriptor.pbs;
		tcont_qos[tcont.entity] = { cir: descriptor.cir, pir: descriptor.pir, cbs, pbs };
	}
	if (pon_mode == 'gpon') {
		let qos = tcont_qos[tcont.entity];
		print(`qos ${tcont.alloc} ${tcont.policy}:${join(':', tcont.queue_weights)} ` +
			`${qos.cir}:${qos.pir}:${qos.cbs}:${qos.pbs}\n`);
	}
}

for (let iw in graph.gem_interworking || []) {
	let entity = number(iw.entity_id, 0, 65535, 'GEM IW entity ID');
	let gem = number(iw.gem_port, 0, 65535, 'GEM CTP pointer');
	let option = number(iw.option, 1, 5, 'GEM IW option');
	if (option != 1 && option != 5)
		fail(`unsupported GEM IW option ${option}`);
	let service = number(iw.service_profile, 0, 65535, 'GEM IW service profile');
	if (interworking[entity] != null)
		fail(`duplicate GEM IW entity ${entity}`);
	if (gems[gem] == null)
		fail(`GEM IW ${entity} references missing GEM CTP ${gem}`);
	interworking[entity] = {
		port: gems[gem].port,
		direction: gems[gem].direction,
		option,
		service
	};
}

for (let iw in graph.multicast_gem_interworking || []) {
	let entity = number(iw.entity_id, 0, 65534, 'multicast GEM IW entity ID');
	let gem = number(iw.gem_port, 0, 65535, 'multicast GEM CTP pointer');
	let base = gems[gem];
	if (base == null)
		fail(`multicast GEM IW ${entity} references missing GEM CTP ${gem}`);
	if (!(base.direction & 1))
		fail(`multicast GEM IW ${entity} connectivity GEM is not downstream capable`);
	gem_ports[base.port].unicast = 0;
	let port_id = number(iw.port_id, 0, gem_port_limit, 'multicast connectivity GEM Port-ID');
	if (port_id != base.port)
		fail(`multicast GEM IW ${entity} connectivity Port-ID does not match GEM CTP`);
	let alloc = number(iw.alloc_id, 0, 65535, 'multicast Alloc-ID');
	if (alloc != base.alloc)
		fail(`multicast GEM IW ${entity} Alloc-ID does not match GEM CTP`);
	let option = number(iw.option, 0, 5, 'multicast GEM IW option');
	if (option != 0 && option != 1 && option != 5)
		fail(`unsupported multicast GEM IW option ${option}`);
	let service = number(iw.service_profile, 0, 65535, 'multicast GEM IW service profile');
	let ports = [];
	let ranges = [];
	for (let range in iw.ipv4_ranges || [])
		push(ranges, range);
	for (let range in iw.ipv6_ranges || [])
		push(ranges, range);
	for (let range in ranges) {
		let port = number(range.gem_port_id, 0, gem_port_limit, 'multicast address-table GEM Port-ID');
		number(range.secondary_key, 0, 65535, 'multicast address-table secondary key');
		if (type(range.start) != 'string' || !length(range.start) ||
		    type(range.stop) != 'string' || !length(range.stop))
			fail(`multicast GEM IW ${entity} has invalid normalized address range`);
		add_unique(ports, port);
		let configured = gem_ports[port];
		if (configured != null) {
			if (configured.alloc != alloc || !(configured.direction & 1))
				fail(`multicast GEM Port-ID ${port} conflicts with an existing GEM mapping`);
		}
		else {
			configured = { port, alloc, direction: 1, tcont: base.tcont,
				encryption: 0, unicast: 0 };
			gem_ports[port] = configured;
		}
	}
	if (multicast_interworking[entity] != null)
		fail(`duplicate multicast GEM IW entity ${entity}`);
	multicast_interworking[entity] = { entity, gem, port: port_id, option, service, ports };
}

if (xg_mode) {
	for (let entity, tcont in tconts) {
		if (tcont.alloc == 65535)
			continue;
		let qos = tcont_qos[tcont.entity] || { cir: 0, pir: 0, cbs: 0, pbs: 0 };
		let packed = `${tcont.policy}:${tcont.weight}:${join(':', tcont.queue_weights)}:` +
			`${qos.cir}:${qos.pir}:${qos.cbs}:${qos.pbs}`;
		print(`xgs-tcont ${tcont.slot} ${tcont.alloc} ${packed}\n`);
	}
	for (let port, gem in gem_ports) {
		let tcont = tconts[gem.tcont];
		if (gem.alloc == 65535 || tcont == null || tcont.slot == null)
			fail(`XG-PON/XGS-PON GEM Port-ID ${gem.port} has no assigned T-CONT`);
		let encrypted = gem.encryption && (gem.direction & 2) ? 1 : 0;
		let packed = `${gem.direction}:${tcont.slot}:${tcont.slot}:0:` +
			`${gem.unicast}:${encrypted}`;
		print(`xgs-xgem ${gem.port} ${tcont.slot} ${packed}\n`);
	}
}
else {
	for (let port, gem in gem_ports)
		print(`gem ${gem.port} ${gem.alloc} ${gem.direction}\n`);
}

// The PON driver preserves the receive GEM Port-ID in skb->mark. For the
// supported colour-blind/no-remarking profile, G.988 drops only red packets;
// PIR/PBS therefore form the exact downstream admission bucket while
// CIR/CBS distinguish green from yellow without affecting forwarding.
for (let entity, gem in gems) {
	if (gem.downstream_td == 65535)
		continue;
	if (!(gem.direction & 1))
		fail(`GEM ${entity} has a downstream traffic descriptor without downstream direction`);
	let descriptor = traffic_descriptors[gem.downstream_td];
	if (!descriptor.pir)
		continue;
	let pbs = descriptor.pbs || factory_burst;
	print(`down-meter ${gem.port} ${descriptor.pir} ${pbs}\n`);
}

for (let profile in graph.multicast_operations_profiles || []) {
	let entity = number(profile.entity_id, 1, 65534, 'multicast operations profile entity ID');
	if (multicast_profiles[entity] != null)
		fail(`duplicate multicast operations profile ${entity}`);
	let version = number(profile.igmp_version, 1, 17, 'IGMP/MLD version');
	if (index([ 1, 2, 3, 16, 17 ], version) < 0)
		fail(`multicast operations profile ${entity} uses unsupported protocol version ${version}`);
	let igmp_function = number(profile.igmp_function, 0, 2, 'IGMP function');
	let immediate_leave = number(profile.immediate_leave, 0, 1, 'immediate-leave state');
	let upstream_tag = number(profile.upstream_tag_control, 0, 3, 'upstream IGMP tag control');
	let upstream_rate = number(profile.upstream_rate, 0, 0xffffffff, 'upstream IGMP rate');
	let downstream_tag = number(profile.downstream_tag_control, 0, 7, 'downstream multicast tag control');
	let dynamic_acl = profile.dynamic_acl || [];
	let static_acl = profile.static_acl || [];
	for (let entry in dynamic_acl)
		multicast_acl(entry, `multicast profile ${entity} dynamic ACL`, gem_port_limit);
	for (let entry in static_acl)
		multicast_acl(entry, `multicast profile ${entity} static ACL`, gem_port_limit);
	number(profile.upstream_tci, 0, 65535, 'upstream IGMP TCI');
	number(profile.downstream_tci, 0, 65535, 'downstream multicast TCI');
	number(profile.robustness, 0, 255, 'multicast robustness');
	number(profile.querier_ip_address, 0, 0xffffffff, 'multicast querier address');
	number(profile.query_interval, 0, 0xffffffff, 'multicast query interval');
	number(profile.query_max_response_time, 0, 0xffffffff, 'multicast query response time');
	number(profile.last_member_query_interval, 0, 0xffffffff, 'last-member query interval');
	number(profile.unauthorized_join_behaviour, 0, 1, 'unauthorized join behaviour');
	multicast_profiles[entity] = { entity, immediate_leave, igmp_function,
		upstream_tag, upstream_rate, downstream_tag };
}

for (let mapper in graph.pbit_mappers || []) {
	let entity = number(mapper.entity_id, 0, 65535, 'mapper entity ID');
	if (mappers[entity] != null)
		fail(`duplicate mapper entity ${entity}`);
	let tp_type = number(mapper.tp_type, 0, 1, 'mapper TP type');
	let tp_pointer = number(mapper.tp_pointer, 0, 65535, 'mapper TP pointer');
	if (tp_type == 1 && unis[tp_pointer] == null)
		fail(`mapper ${entity} references missing Ethernet UNI ${tp_pointer}`);
	let pbits = mapper.pbits || [];
	if (length(pbits) != 8)
		fail('mapper does not contain eight P-bit branches');
	let option = number(mapper.unmarked_frame_option, 0, 1, 'unmarked frame option');
	let default_pbit = number(mapper.default_pbit, 0, 7, 'default P-bit');
	let dscp = mapper.dscp_to_pbit || [];
	if (length(dscp) != 64)
		fail('mapper does not contain 64 DSCP mappings');
	mappers[entity] = { entity, tp_type, tp_pointer, pbits, option, default_pbit, dscp,
		interfaces: [], direct_bridge: null, direct_ani: null, direct_peer: null };
}

for (let bridge in graph.bridges || []) {
	let entity = number(bridge.entity_id, 0, 65535, 'MAC bridge entity ID');
	if (bridges[entity] != null)
		fail(`duplicate MAC bridge ${entity}`);
	let spanning_tree = number(bridge.spanning_tree, 0, 1, 'MAC bridge spanning-tree state');
	let learning = number(bridge.learning, 0, 1, 'MAC bridge learning state');
	let port_bridging = number(bridge.port_bridging, 0, 1, 'MAC bridge port-bridging state');
	let priority = number(bridge.priority, 0, 65535, 'MAC bridge priority');
	let max_age = number(bridge.max_age_256ths, 0x0600, 0x2800, 'MAC bridge max age');
	let hello_time = number(bridge.hello_time_256ths, 0x0100, 0x0a00, 'MAC bridge hello time');
	let forward_delay = number(bridge.forward_delay_256ths, 0x0400, 0x1e00, 'MAC bridge forward delay');
	let unknown_discard = number(bridge.unknown_mac_discard, 0, 1, 'MAC bridge unknown-MAC policy');
	let learning_depth = number(bridge.mac_learning_depth, 0, 255, 'MAC bridge learning depth');
	let age = number(bridge.dynamic_filtering_age_time_seconds, 0, 1000000,
		'MAC bridge dynamic filtering age');
	if (age > 0 && age < 10)
		fail(`MAC bridge ${entity} dynamic filtering age is below 10 seconds`);
		let resolved = {
			entity, spanning_tree, learning, port_bridging, priority, max_age, hello_time,
			forward_delay, unknown_discard, learning_depth, age,
			interfaces: [], mappers: [], interworking: [], multicast: [], ports: []
		};
	for (let port in bridge.ports || []) {
		let port_entity = number(port.entity_id, 0, 65535, 'MAC bridge port entity ID');
		if (bridge_ports[port_entity] != null)
			fail(`duplicate MAC bridge port ${port_entity}`);
		let port_number = number(port.port, 0, 255, 'MAC bridge port number');
		let tp_type = number(port.tp_type, 1, 6, 'MAC bridge port TP type');
		let tp = number(port.tp, 0, 65535, 'MAC bridge port TP pointer');
		let port_priority = number(port.priority, 0, 255, 'MAC bridge port priority');
		let path_cost = number(port.path_cost, 1, 65535, 'MAC bridge port path cost');
		let port_stp = number(port.spanning_tree, 0, 1, 'MAC bridge port spanning-tree state');
		let outbound_td = number(port.outbound_td, 0, 65535, 'MAC bridge port outbound TD');
		let inbound_td = number(port.inbound_td, 0, 65535, 'MAC bridge port inbound TD');
		if (outbound_td != 65535 && traffic_descriptors[outbound_td] == null)
			fail(`MAC bridge port ${port_entity} references missing outbound traffic descriptor ${outbound_td}`);
		if (inbound_td != 65535 && traffic_descriptors[inbound_td] == null)
			fail(`MAC bridge port ${port_entity} references missing inbound traffic descriptor ${inbound_td}`);
		let port_learning_depth = number(port.mac_learning_depth, 0, 255, 'MAC bridge port learning depth');
		let ifname = null;
		if (tp_type == 1) {
			if (unis[tp] == null)
				fail(`MAC bridge ${entity} references missing Ethernet UNI ${tp}`);
			ifname = unis[tp].ifname;
			add_unique(resolved.interfaces, ifname);
		}
		else if (tp_type == 3)
			add_unique(resolved.mappers, tp);
		else if (tp_type == 5)
			add_unique(resolved.interworking, tp);
		else if (tp_type == 6)
			add_unique(resolved.multicast, tp);
		else
			fail(`unsupported MAC bridge port TP type ${tp_type}`);
		let resolved_port = {
			entity: port_entity, bridge: entity, port: port_number, tp_type, tp, ifname,
			priority: port_priority, path_cost, spanning_tree: port_stp,
			outbound_td, inbound_td, learning_depth: port_learning_depth, no_flood: false,
			fastleave: 0, max_groups: 0
		};
		push(resolved.ports, resolved_port);
		bridge_ports[port_entity] = resolved_port;
	}
		bridges[entity] = resolved;
}

for (let subscriber in graph.multicast_subscribers || []) {
	let entity = number(subscriber.entity_id, 0, 65535, 'multicast subscriber entity ID');
	let me_type = number(subscriber.me_type, 0, 1, 'multicast subscriber ME type');
	let port = null;
	let mapper = null;
	if (me_type == 0) {
		port = bridge_ports[entity];
		if (port == null || port.tp_type != 1)
			fail(`multicast subscriber ${entity} does not resolve to an Ethernet UNI bridge port`);
	}
	else {
		mapper = mappers[entity];
		if (mapper == null)
			fail(`multicast subscriber ${entity} does not resolve to an IEEE 802.1p mapper`);
	}
	let packages = subscriber.service_packages || [];
	for (let service in packages) {
		number(service.row_key, 0, 1023, `multicast subscriber ${entity} service row key`);
		let vlan = number(service.vlan_id, 0, 65535, `multicast subscriber ${entity} service VLAN`);
		if (vlan > 4097 && vlan != 65535)
			fail(`multicast subscriber ${entity} has invalid service VLAN ${vlan}`);
		number(service.max_simultaneous_groups, 0, 65535,
			`multicast subscriber ${entity} service maximum groups`);
		number(service.max_multicast_bandwidth, 0, 0xffffffff,
			`multicast subscriber ${entity} service maximum bandwidth`);
		let package_profile = number(service.operations_profile, 1, 65534,
			`multicast subscriber ${entity} service operations profile`);
		if (multicast_profiles[package_profile] == null)
			fail(`multicast subscriber ${entity} service references missing operations profile ${package_profile}`);
	}
	let previews = subscriber.allowed_preview_groups || [];
	for (let preview in previews) {
		number(preview.row_key, 0, 1023, `multicast subscriber ${entity} preview row key`);
		let version = number(preview.ip_version, 4, 6, `multicast subscriber ${entity} preview IP version`);
		if (version != 4 && version != 6)
			fail(`multicast subscriber ${entity} has invalid preview IP version`);
		address(preview.source, `multicast subscriber ${entity} preview source`);
		address(preview.destination, `multicast subscriber ${entity} preview destination`);
		number(preview.ani_vlan, 0, 4095, `multicast subscriber ${entity} preview ANI VLAN`);
		number(preview.uni_vlan, 0, 4095, `multicast subscriber ${entity} preview UNI VLAN`);
		number(preview.duration_minutes, 0, 65535, `multicast subscriber ${entity} preview duration`);
		number(preview.time_left_minutes, 0, 65535, `multicast subscriber ${entity} preview time left`);
	}
	let profile_id = number(subscriber.profile, 1, 65534, 'multicast operations profile pointer');
	let profile = multicast_profiles[profile_id];
	if (!length(packages) && profile == null)
		fail(`multicast subscriber ${entity} references missing operations profile ${profile_id}`);
	let max_groups = number(subscriber.max_simultaneous_groups, 0, 65535,
		`multicast subscriber ${entity} maximum groups`);
	let max_bandwidth = number(subscriber.max_multicast_bandwidth, 0, 0xffffffff,
		`multicast subscriber ${entity} maximum bandwidth`);
	let enforcement = number(subscriber.bandwidth_enforcement, 0, 1,
		`multicast subscriber ${entity} bandwidth enforcement`);
	push(multicast_subscribers, { entity, me_type, port, mapper, max_groups });
}

let vlan_filter_interfaces = {};
for (let filter in graph.vlan_filters || []) {
	let entity = number(filter.entity_id, 0, 65535, 'VLAN filter entity ID');
	let bridge_port = number(filter.bridge_port, 0, 65535, 'VLAN filter bridge port');
	if (entity != bridge_port)
		fail(`VLAN filter ${entity} is not implicitly linked to bridge port ${bridge_port}`);
	let port = bridge_ports[bridge_port];
	if (port == null)
		fail(`VLAN filter ${entity} references missing bridge port ${bridge_port}`);
	if (port.tp_type != 1)
		fail(`VLAN filter ${entity} on non-UNI bridge port requires the native bridge backend`);
	let uni = unis[port.tp];
	if (uni == null)
		fail(`VLAN filter ${entity} references missing Ethernet UNI ${port.tp}`);
	if (vlan_filter_interfaces[uni.ifname] != null)
		fail(`multiple VLAN filters resolve to ${uni.ifname}`);
	let operation = number(filter.forward_operation, 0, 0x21, 'VLAN forward operation');
	let policy = vlan_forward_policy(operation);
	if (filter.tagged_action != policy.tagged ||
		filter.tagged_criterion != policy.criterion || filter.untagged_action != policy.untagged)
		fail(`VLAN filter ${entity} normalized policy does not match forward operation`);
	if (policy.tagged == 'j')
		port.no_flood = true;
	let entries = filter.entries || [];
	if (length(entries) > 12)
		fail(`VLAN filter ${entity} has more than 12 entries`);
	for (let i = 0; i < length(entries); i++) {
		entries[i] = number(entries[i], 0, 65535, `VLAN filter ${entity} entry`);
		if ((entries[i] & 0x0fff) == 0x0fff)
			fail(`VLAN filter ${entity} entry uses reserved VID 4095`);
	}
	vlan_filter_interfaces[uni.ifname] = entity;
	emit_vlan_filter(uni.ifname, 'ingress',
		vlan_filter_bpf(policy.untagged, policy.tagged, policy.criterion, entries, false), entity);
	emit_vlan_filter(uni.ifname, 'egress',
		vlan_filter_bpf(policy.untagged, policy.tagged, policy.criterion, entries, true), entity);
}

let port_meter_targets = {};

function emit_port_meter(port, ifname, hook, pointer) {
	if (pointer == 65535)
		return;
	let descriptor = traffic_descriptors[pointer];
	if (!descriptor.pir)
		return;
	let key = `${ifname}:${hook}`;
	if (port_meter_targets[key] != null)
		fail(`MAC bridge ports ${port_meter_targets[key]} and ${port.entity} overlap on ${key}`);
	port_meter_targets[key] = port.entity;
	let pbs = descriptor.pbs || factory_burst;
	print(`port-meter ${ifname} ${hook} ${descriptor.pir}:${pbs}\n`);
}

let bridge_interfaces = {};
for (let bridge_id, bridge in bridges) {
	for (let mapper in bridge.mappers)
		if (mappers[mapper] == null)
			fail(`MAC bridge ${bridge.entity} references missing mapper ${mapper}`);
	for (let iw in bridge.interworking)
		if (interworking[iw] == null)
			fail(`MAC bridge ${bridge.entity} references missing GEM IW ${iw}`);
	for (let iw in bridge.multicast)
		if (multicast_interworking[iw] == null)
			fail(`MAC bridge ${bridge.entity} references missing multicast GEM IW ${iw}`);

	let ani_ports = length(bridge.mappers) + length(bridge.interworking);
	let multicast_ports = length(bridge.multicast);
	if (!length(bridge.interfaces) || (!ani_ports && !multicast_ports))
		continue;
	if (ani_ports != 1 && (ani_ports != 0 || !multicast_ports))
		fail(`MAC bridge ${bridge.entity} has ${ani_ports} unicast ANI logical ports; the XG2010G backend supports one`);
		let name = sprintf('omb%04x', bridge.entity);
	let ani_ifname = sprintf('oma%04x', bridge.entity);
	let ani_peer = sprintf('omp%04x', bridge.entity);
		let packed = `${bridge.spanning_tree}:${bridge.learning}:${bridge.port_bridging}:${bridge.priority}:` +
			`${centiseconds(bridge.max_age)}:${centiseconds(bridge.hello_time)}:` +
			`${centiseconds(bridge.forward_delay)}:${bridge.unknown_discard}:${bridge.age * 100}:` +
			`${bridge.learning_depth}`;
	print(`bridge ${name} ${bridge.entity} ${packed}\n`);

	let ani_port = null;
	let multicast_ani_ports = [];
	for (let port in bridge.ports) {
		if (bridge.spanning_tree && !port.spanning_tree)
			fail(`MAC bridge port ${port.entity} cannot opt out of Linux bridge STP`);
		if (port.priority % 4)
			fail(`MAC bridge port ${port.entity} priority cannot be represented by Linux bridge STP`);
		if (port.tp_type == 1) {
			if (bridge_interfaces[port.ifname] != null)
				fail(`${port.ifname} belongs to more than one active MAC bridge`);
			bridge_interfaces[port.ifname] = name;
			let flood = bridge.unknown_discard || port.no_flood ? 0 : 1;
			let isolated = bridge.port_bridging ? 0 : 1;
			let port_packed = `${port.entity}:${int(port.priority / 4)}:${port.path_cost}:` +
				`${port.spanning_tree}:${bridge.learning}:${flood}:${isolated}:` +
				`${port.fastleave}:${port.max_groups}:${port.learning_depth}`;
			print(`bridge-port ${name} ${port.ifname} ${port_packed}\n`);
			emit_port_meter(port, port.ifname, 'egress', port.outbound_td);
			emit_port_meter(port, port.ifname, 'ingress', port.inbound_td);
		}
		else if (port.tp_type == 6)
			push(multicast_ani_ports, port);
		else
			ani_port = port;
	}
	if (ani_port == null && length(multicast_ani_ports))
		ani_port = multicast_ani_ports[0];
	if (ani_port == null)
		fail(`MAC bridge ${bridge.entity} has no ANI bridge port`);
	for (let port in multicast_ani_ports)
		if (port.priority != ani_port.priority || port.path_cost != ani_port.path_cost ||
		    port.spanning_tree != ani_port.spanning_tree || port.no_flood != ani_port.no_flood ||
		    port.learning_depth != ani_port.learning_depth ||
		    port.outbound_td != ani_port.outbound_td || port.inbound_td != ani_port.inbound_td)
			fail(`MAC bridge ${bridge.entity} multicast and unicast ANI port policies cannot share one endpoint`);
	print(`bridge-ani ${name} ${ani_ifname} ${ani_peer}\n`);
	let ani_flood = bridge.unknown_discard || ani_port.no_flood ? 0 : 1;
	let ani_packed = `${ani_port.entity}:${int(ani_port.priority / 4)}:${ani_port.path_cost}:` +
		`${ani_port.spanning_tree}:${bridge.learning}:${ani_flood}:0:0:0:${ani_port.learning_depth}`;
	print(`bridge-port ${name} ${ani_ifname} ${ani_packed}\n`);
	emit_port_meter(ani_port, ani_ifname, 'egress', ani_port.outbound_td);
	emit_port_meter(ani_port, ani_ifname, 'ingress', ani_port.inbound_td);
	bridge.ani_ifname = ani_ifname;
	bridge.ani_peer = ani_peer;
}

let rate_limiter_targets = {};

function activate_direct_mapper_bridge(mapper) {
	if (mapper.direct_bridge != null)
		return mapper.direct_ani;
	let uni = unis[mapper.tp_pointer];
	if (uni == null)
		fail(`direct mapper ${mapper.entity} references missing Ethernet UNI ${mapper.tp_pointer}`);
	let ifname = uni.ifname;
	if (bridge_interfaces[ifname] != null)
		fail(`direct mapper ${mapper.entity} conflicts with active bridge ${bridge_interfaces[ifname]} on ${ifname}`);
	let name = sprintf('omm%04x', mapper.entity);
	let ani = sprintf('omx%04x', mapper.entity);
	let peer = sprintf('omy%04x', mapper.entity);
	let bridge_policy = '0:1:0:32768:2000:200:1500:0:30000:0';
	let port_policy = `${mapper.entity}:32:10:0:1:1:0:0:0:0`;
	print(`bridge ${name} ${mapper.entity} ${bridge_policy}\n`);
	print(`bridge-ani ${name} ${ani} ${peer}\n`);
	print(`bridge-port ${name} ${ifname} ${port_policy}\n`);
	print(`bridge-port ${name} ${ani} ${port_policy}\n`);
	bridge_interfaces[ifname] = name;
	mapper.direct_bridge = name;
	mapper.direct_ani = ani;
	mapper.direct_peer = peer;
	return ani;
}

function emit_rate_limiter(limiter, ifname, category, pointer) {
	if (pointer == 65535)
		return;
	let descriptor = traffic_descriptors[pointer];
	if (descriptor == null)
		fail(`dot1 rate limiter ${limiter} references missing traffic descriptor ${pointer}`);
	if (!descriptor.pir || !descriptor.pbs)
		fail(`dot1 rate limiter ${limiter} requires explicit non-zero PIR and PBS`);
	let key = `${ifname}:${category}`;
	if (rate_limiter_targets[key] != null)
		fail(`dot1 rate limiters ${rate_limiter_targets[key]} and ${limiter} overlap on ${key}`);
	rate_limiter_targets[key] = limiter;
	print(`rate-limit ${ifname} ${category} ${descriptor.pir}:${descriptor.pbs}\n`);
}

for (let limiter in graph.dot1_rate_limiters || []) {
	let entity = number(limiter.entity_id, 0, 65535, 'dot1 rate limiter entity ID');
	if (rate_limiters[entity] != null)
		fail(`duplicate dot1 rate limiter ${entity}`);
	let parent = number(limiter.parent_me, 0, 65535, `dot1 rate limiter ${entity} parent`);
	let tp_type = number(limiter.tp_type, 1, 2, `dot1 rate limiter ${entity} TP type`);
	let unknown = number(limiter.upstream_unicast_flood_traffic_descriptor == null ? 65535 :
		limiter.upstream_unicast_flood_traffic_descriptor, 0, 65535,
		`dot1 rate limiter ${entity} unknown-unicast descriptor`);
	let broadcast = number(limiter.upstream_broadcast_traffic_descriptor == null ? 65535 :
		limiter.upstream_broadcast_traffic_descriptor, 0, 65535,
		`dot1 rate limiter ${entity} broadcast descriptor`);
	let multicast = number(limiter.upstream_multicast_payload_traffic_descriptor == null ? 65535 :
		limiter.upstream_multicast_payload_traffic_descriptor, 0, 65535,
		`dot1 rate limiter ${entity} multicast descriptor`);
	let target = null;
	if (tp_type == 1) {
		let bridge = bridges[parent];
		if (bridge == null)
			fail(`dot1 rate limiter ${entity} references missing MAC bridge ${parent}`);
		target = bridge.ani_ifname;
	}
	else {
		let mapper = mappers[parent];
		if (mapper == null)
			fail(`dot1 rate limiter ${entity} references missing mapper ${parent}`);
		for (let bridge_id, bridge in bridges) {
			if (bridge.ani_ifname == null || index(bridge.mappers, parent) < 0)
				continue;
			if (target != null && target != bridge.ani_ifname)
				fail(`dot1 rate limiter ${entity} mapper resolves to multiple bridge endpoints`);
			target = bridge.ani_ifname;
		}
		if (target == null && (unknown != 65535 || broadcast != 65535 || multicast != 65535) &&
		    mapper.tp_type == 1)
			target = activate_direct_mapper_bridge(mapper);
	}
	rate_limiters[entity] = { entity, parent, tp_type };
	// An OLT may create the limiter before the parent bridge has all ports. It
	// becomes active atomically once the bridge obtains an ANI endpoint.
	if (target == null)
		continue;
	emit_rate_limiter(entity, target, 'unknown', unknown);
	emit_rate_limiter(entity, target, 'broadcast', broadcast);
	emit_rate_limiter(entity, target, 'multicast', multicast);
}

function add_attachment(targets, ifname, side, gem) {
	for (let target in targets)
		if (target.ifname == ifname && target.side == side && target.gem == gem)
			return;
	push(targets, { ifname, side, gem });
}

function mapper_attachments(mapper, targets) {
	for (let bridge_id, bridge in bridges)
		if (bridge.ani_ifname != null && index(bridge.mappers, mapper.entity) >= 0)
			add_attachment(targets, bridge.ani_ifname, 'ani', -1);
	if (!length(targets) && mapper.tp_type == 1)
		add_attachment(targets, unis[mapper.tp_pointer].ifname, 'uni', -1);
}

function tagging_attachments(class_id, entity_id, owner) {
	let targets = [];
	if (class_id == 11) {
		let uni = unis[entity_id];
		if (uni == null)
			fail(`VLAN operation ${owner} references missing Ethernet UNI ${entity_id}`);
		add_attachment(targets, uni.ifname, 'uni', -1);
	}
	else if (class_id == 47) {
		let port = bridge_ports[entity_id];
		if (port == null)
			fail(`VLAN operation ${owner} references missing MAC bridge port ${entity_id}`);
		if (port.tp_type == 1)
			add_attachment(targets, port.ifname, 'uni', -1);
		else {
			let bridge = bridges[port.bridge];
			if (bridge.ani_ifname != null)
				add_attachment(targets, bridge.ani_ifname, 'ani', -1);
		}
	}
	else if (class_id == 130) {
		let mapper = mappers[entity_id];
		if (mapper == null)
			fail(`VLAN operation ${owner} references missing mapper ${entity_id}`);
		mapper_attachments(mapper, targets);
	}
	else if (class_id == 266) {
		let iw = interworking[entity_id];
		if (iw == null)
			fail(`VLAN operation ${owner} references missing GEM IW ${entity_id}`);
		if (iw.option == 1) {
			let bridge = bridges[iw.service];
			if (bridge != null && bridge.ani_ifname != null)
				add_attachment(targets, bridge.ani_ifname, 'ani', iw.port);
		}
		else {
			let mapper = mappers[iw.service];
			if (mapper == null)
				fail(`VLAN operation ${owner} GEM IW references missing mapper ${iw.service}`);
			for (let bridge_id, bridge in bridges)
				if (bridge.ani_ifname != null && index(bridge.mappers, mapper.entity) >= 0)
					add_attachment(targets, bridge.ani_ifname, 'ani', iw.port);
			if (!length(targets) && mapper.tp_type == 1)
				fail(`VLAN operation ${owner} on a direct GEM IW requires post-mapper hardware support`);
		}
	}
	return targets;
}

let tagging_targets = {};
let evto_interfaces = {};
let evto_interface_counts = {};

function claim_evto_interface(ifname, gem, entity) {
	let wildcard = `${ifname}:-1`;
	let key = `${ifname}:${gem}`;
	let count = evto_interface_counts[ifname] || 0;
	if (evto_interfaces[key] != null ||
	    (gem >= 0 && evto_interfaces[wildcard] != null))
		fail(`multiple VLAN operation profiles resolve to ${ifname} GEM ${gem}`);
	if (gem < 0 && count)
		fail(`multiple VLAN operation profiles resolve to ${ifname}`);
	evto_interfaces[key] = entity;
	evto_interface_counts[ifname] = count + 1;
}

for (let operation in graph.vlan_operations || []) {
	let entity = number(operation.entity_id, 0, 65535, 'VLAN operation entity ID');
	let association = number(operation.association_type, 0, 13, 'VLAN operation association type');
	let associated_class = number(operation.associated_class, 0, 65535, 'VLAN operation associated class');
	let associated = number(operation.associated_me, 0, 65535, 'VLAN operation associated ME');
	let expected_class;
	switch (association) {
	case 0:
	case 10:
		expected_class = 11;
		break;
	case 2:
		expected_class = 130;
		break;
	case 3:
		expected_class = 47;
		break;
	case 5:
		expected_class = 266;
		break;
	default:
		fail(`VLAN operation ${entity} uses unsupported association type ${association}`);
	}
	if (associated_class != expected_class)
		fail(`VLAN operation ${entity} has inconsistent associated class ${associated_class}`);
	claim_tagging_target(tagging_targets, associated_class, associated, entity);
	let upstream_mode = number(operation.upstream_mode, 0, 2, 'VLAN operation upstream mode');
	let tci = number(operation.upstream_tci, 0, 65535, 'VLAN operation upstream TCI');
	let downstream_mode = number(operation.downstream_mode, 0, 1, 'VLAN operation downstream mode');
	if (upstream_mode && (tci & 0x0fff) == 0x0fff)
		fail(`VLAN operation ${entity} uses reserved VID 4095`);
	if (upstream_mode && (tci & 0x1000))
		fail(`VLAN operation ${entity} DEI treatment requires the native VLAN backend`);
	if (associated_class != 11) {
		if (upstream_mode || downstream_mode)
			fail(`VLAN operation ${entity} association type ${association} requires the native bridge backend`);
		continue;
	}
	let uni = unis[associated];
	if (uni == null)
		fail(`VLAN operation ${entity} references missing Ethernet UNI ${associated}`);
	let ifname = uni.ifname;
	claim_evto_interface(ifname, -1, entity);
	print(`vlan-op-up ${ifname} ${entity}:${upstream_mode}:${tci} 0\n`);
	print(`vlan-op-down ${ifname} ${entity}:${downstream_mode}:0 0\n`);
	print(`evto-up-policy ${ifname} pass ${entity}\n`);
	print(`evto-down-policy ${ifname} pass ${entity}\n`);
}

for (let mapper_id, mapper in mappers) {
	if (mapper.tp_type == 1) {
		let owner = bridge_interfaces[unis[mapper.tp_pointer].ifname];
		if (owner != null && owner != mapper.direct_bridge)
			fail(`direct mapper ${mapper.entity} conflicts with an active bridge on ${unis[mapper.tp_pointer].ifname}`);
		add_unique(mapper.interfaces, unis[mapper.tp_pointer].ifname);
	}
	else {
		for (let bridge_id, bridge in bridges)
			if (index(bridge.mappers, mapper.entity) >= 0)
				for (let ifname in bridge.interfaces)
					add_unique(mapper.interfaces, ifname);
	}
}

let multicast_interfaces = {};
for (let subscriber in multicast_subscribers) {
	let interfaces = subscriber.me_type == 0 ? [ subscriber.port.ifname ] : subscriber.mapper.interfaces;
	if (!length(interfaces))
		fail(`multicast subscriber ${subscriber.entity} has no Ethernet UNI attachment`);
	for (let ifname in interfaces) {
		if (multicast_interfaces[ifname] != null)
			fail(`${ifname} is shared by multicast subscribers ${multicast_interfaces[ifname]} and ${subscriber.entity}`);
		multicast_interfaces[ifname] = subscriber.entity;
		print(`mcast-uni ${ifname} ${subscriber.entity} 0\n`);
	}
}

function resolve_target(pointer, upstream) {
	let iw = number(pointer, 0, 65535, 'mapper GEM IW pointer');
	if (iw == 65535)
		return -1;
	let target = interworking[iw];
	if (target == null)
		fail(`mapper references missing GEM IW ${iw}`);
	if (upstream && target.direction != 2 && target.direction != 3)
		fail(`mapper references GEM ${target.port} without upstream direction`);
	if (!upstream && target.direction != 1 && target.direction != 3)
		return -1;
	return target.port;
}

let classifiers = {};

function classifier(ifname) {
	if (classifiers[ifname] == null)
		classifiers[ifname] = {
			pbits: [ null, null, null, null, null, null, null, null ],
			dscp: [],
			default_target: null
		};
	return classifiers[ifname];
}


for (let mapper_id, mapper in mappers) {
	for (let ifname in mapper.interfaces) {
		let state = classifier(ifname);
		for (let pbit = 0; pbit < 8; pbit++)
			merge_target(state.pbits, pbit, resolve_target(mapper.pbits[pbit], true),
				`${ifname} P-bit ${pbit}`);

		if (mapper.option == 1) {
			let target = resolve_target(mapper.pbits[mapper.default_pbit], true);
			if (state.default_target != null && state.default_target != target)
				fail(`conflicting ${ifname} unmarked mapping ${state.default_target}/${target}`);
			state.default_target = target;
		}
		else {
			for (let dscp = 0; dscp < 64; dscp++) {
				let pbit = number(mapper.dscp[dscp], 0, 7, `DSCP ${dscp} P-bit`);
				merge_target(state.dscp, dscp, resolve_target(mapper.pbits[pbit], true),
					`${ifname} DSCP ${dscp}`);
			}
			let target = resolve_target(mapper.pbits[0], true);
			if (state.default_target != null && state.default_target != target)
				fail(`conflicting ${ifname} non-IP unmarked mapping ${state.default_target}/${target}`);
			state.default_target = target;
		}
	}
}

// A bridge with one direct GEM IW and no mapper is an unambiguous transparent
// service. More complex direct-GEM bridges are selected by their VLAN rules.
for (let bridge_id, bridge in bridges) {
	if (length(bridge.mappers))
		continue;
	let targets = [];
	for (let iw_id in bridge.interworking) {
		let target = interworking[iw_id];
		if (target != null && (target.direction == 2 || target.direction == 3))
			add_unique(targets, target.port);
	}
	if (length(targets) == 1)
		for (let ifname in bridge.interfaces) {
			let state = classifier(ifname);
			if (state.default_target != null && state.default_target != targets[0])
				fail(`conflicting ${ifname} transparent mapping ${state.default_target}/${targets[0]}`);
			state.default_target = targets[0];
		}
}

for (let ifname, state in classifiers) {
	for (let pbit = 0; pbit < 8; pbit++)
		if (state.pbits[pbit] != null)
			print(`pbit ${ifname} ${pbit} ${state.pbits[pbit]}\n`);
	for (let dscp = 0; dscp < 64; dscp++)
		if (state.dscp[dscp] != null)
			print(`dscp ${ifname} ${dscp} ${state.dscp[dscp]}\n`);
	if (state.default_target != null)
		print(`default ${ifname} ${state.default_target} 0\n`);
}

let downstream = {};

function add_downstream(target, ifname) {
	if (downstream[target] == null)
		downstream[target] = [];
	add_unique(downstream[target], ifname);
}

for (let iw_id, iw in interworking) {
	let target = resolve_target(iw_id, false);
	if (target < 0)
		continue;
	if (iw.option == 1) {
		let bridge = bridges[iw.service];
		if (bridge == null)
			fail(`GEM IW for port ${iw.port} references missing bridge ${iw.service}`);
		if (bridge.ani_peer != null)
			add_downstream(target, bridge.ani_peer);
	}
	else {
		let mapper = mappers[iw.service];
		if (mapper == null)
			fail(`GEM IW for port ${iw.port} references missing mapper ${iw.service}`);
		if (mapper.tp_type == 1) {
			if (mapper.direct_peer != null)
				add_downstream(target, mapper.direct_peer);
			else
				for (let ifname in mapper.interfaces)
					add_downstream(target, ifname);
		}
		else {
			for (let bridge_id, bridge in bridges)
				if (bridge.ani_peer != null && index(bridge.mappers, mapper.entity) >= 0)
					add_downstream(target, bridge.ani_peer);
		}
	}
}

for (let iw_id, iw in multicast_interworking)
	for (let port in iw.ports)
		for (let bridge_id, bridge in bridges)
			if (bridge.ani_peer != null && index(bridge.multicast, iw.entity) >= 0)
				add_downstream(port, bridge.ani_peer);

for (let target, interfaces in downstream)
	for (let ifname in interfaces)
		print(`down ${target} ${ifname} 0\n`);

for (let evto in graph.extended_vlans || []) {
	let entity = number(evto.entity_id, 0, 65535, 'extended VLAN entity ID');
	let association = number(evto.association_type, 0, 12, 'extended VLAN association type');
	let associated_class = number(evto.associated_class, 0, 65535, 'extended VLAN associated class');
	let associated = number(evto.associated_me, 0, 65535, 'extended VLAN associated ME');
	let input_tpid = number(evto.input_tpid, 0, 65535, 'extended VLAN input TPID');
	let output_tpid = number(evto.output_tpid, 0, 65535, 'extended VLAN output TPID');
	let downstream_mode = number(evto.downstream_mode, 0, 8, 'extended VLAN downstream mode');
	let enhanced = number(evto.enhanced_mode, 0, 1, 'extended VLAN enhanced mode');
	let rules = evto.rules || [];
	let mapping = evto.dscp_to_pbit || [];
	if (length(mapping) != 64)
		fail(`extended VLAN ${entity} does not contain 64 DSCP mappings`);
	for (let dscp = 0; dscp < 64; dscp++)
		number(mapping[dscp], 0, 7, `extended VLAN DSCP ${dscp} P-bit`);

	let expected_class = association == 0 ? 47 :
		(association == 1 ? 130 : (association == 2 ? 11 : (association == 5 ? 266 : -1)));
	if (expected_class < 0 || associated_class != expected_class)
		fail(`extended VLAN ${entity} has inconsistent association type/class ${association}/${associated_class}`);
	claim_tagging_target(tagging_targets, associated_class, associated, entity);
	let attachments = tagging_attachments(associated_class, associated, entity);
	if (!length(attachments))
		fail(`extended VLAN ${entity} association does not resolve to an active service`);
	for (let attachment in attachments) {
		let ifname = attachment.ifname;
		let gem = attachment.gem;
		let up_kind = attachment.side == 'ani' ? 'evto-ani-up' : 'evto-up';
		let down_kind = attachment.side == 'ani' ? 'evto-ani-down' : 'evto-down';
		let up_policy_kind = attachment.side == 'ani' ? 'evto-ani-up-policy' : 'evto-up-policy';
		let down_policy_kind = attachment.side == 'ani' ? 'evto-ani-down-policy' : 'evto-down-policy';
		claim_evto_interface(ifname, gem, entity);

		for (let slot = 0; slot < length(rules); slot++) {
			let rule = rules[slot];
			let direction = number(rule.direction, 0, 2, 'enhanced VLAN direction');
			if (!enhanced && direction != 0)
				fail('classic extended VLAN rule has a direction');
			let normalized = normalized_rule(rule, input_tpid, output_tpid, mapping);
			normalized.slot = slot;
			if (direction == 0 || direction == 1)
				emit_evto(up_kind, ifname, entity, normalized, gem, mapping);
			if (direction == 2)
				emit_evto(down_kind, ifname, entity, normalized, gem, mapping);
			else if (direction == 0 && downstream_mode != 1 && downstream_mode != 8) {
				let inverse = inverse_rule(normalized, downstream_mode);
				if (inverse != null) {
					inverse.slot = slot;
					emit_evto(down_kind, ifname, entity, inverse, gem, mapping);
				}
			}
		}

		// Classic tables contain explicit per-tag defaults. Enhanced tables and
		// downstream modes 0/5..8 discard unmatched traffic by specification or
		// by the XG2010G implementation choice allowed for mode 0.
		print(`${up_policy_kind} ${ifname} drop ${gem}\n`);
		let downstream_policy = (downstream_mode == 1 ||
			downstream_mode == 2 || downstream_mode == 3 || downstream_mode == 4) ? 'pass' : 'drop';
		print(`${down_policy_kind} ${ifname} ${downstream_policy} ${gem}\n`);
	}
}
