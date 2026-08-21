// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;

#include "../src/airoha-gpon-port-transaction.h"

#define MAX_PORTS 4096
#define TEST_ERROR (-5)

struct simulation {
	bool gem_valid[MAX_PORTS];
	bool gem_encrypted[MAX_PORTS];
	u16 ethernet_port;
	bool ethernet_valid;
	u16 committed_port;
	bool committed_valid;
	unsigned int acknowledgements;
	unsigned int event_count;
	unsigned int fail_at;
	unsigned int fail_at_second;
	unsigned int fail_closed_count;
	char events[32];
};

static bool event_fails(struct simulation *simulation, char event)
{
	simulation->events[simulation->event_count++] = event;
	simulation->events[simulation->event_count] = '\0';
	return simulation->fail_at == simulation->event_count ||
	       simulation->fail_at_second == simulation->event_count;
}

static int set_gem(void *context, u16 port, bool valid, bool encrypted)
{
	struct simulation *simulation = context;

	if (event_fails(simulation, valid ? 'G' : 'g'))
		return TEST_ERROR;
	simulation->gem_valid[port] = valid;
	simulation->gem_encrypted[port] = valid && encrypted;
	return 0;
}

static int set_omcc(void *context, u16 port, bool valid)
{
	struct simulation *simulation = context;

	if (event_fails(simulation, valid ? 'O' : 'o'))
		return TEST_ERROR;
	simulation->ethernet_port = valid ? port : 0;
	simulation->ethernet_valid = valid;
	return 0;
}

static void commit_omcc(void *context, u16 port, bool valid)
{
	struct simulation *simulation = context;

	event_fails(simulation, valid ? 'C' : 'c');
	simulation->committed_port = valid ? port : 0;
	simulation->committed_valid = valid;
}

static int acknowledge(void *context)
{
	struct simulation *simulation = context;
	bool fail = event_fails(simulation, 'A');

	simulation->acknowledgements++;
	return fail ? TEST_ERROR : 0;
}

static void fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->events[simulation->event_count++] = 'F';
	simulation->events[simulation->event_count] = '\0';
	simulation->fail_closed_count++;
}

static const struct airoha_gpon_port_transaction_ops operations = {
	.set_gem = set_gem,
	.set_omcc = set_omcc,
	.commit_omcc = commit_omcc,
	.acknowledge = acknowledge,
	.fail_closed = fail_closed,
};

struct reset_simulation {
	unsigned int event_count;
	unsigned int fail_at;
	unsigned int committed_state;
	bool tx_disabled;
	char events[16];
};

static int reset_event(struct reset_simulation *simulation, char event)
{
	simulation->events[simulation->event_count++] = event;
	simulation->events[simulation->event_count] = '\0';
	return simulation->fail_at == simulation->event_count ? TEST_ERROR : 0;
}

static int reset_disable_tx(void *context)
{
	struct reset_simulation *simulation = context;

	simulation->tx_disabled = true;
	return reset_event(simulation, 'T');
}

static int reset_clear_data(void *context)
{
	return reset_event(context, 'D');
}

static int reset_disable_omcc(void *context)
{
	return reset_event(context, 'O');
}

static int reset_clear_gem_table(void *context)
{
	return reset_event(context, 'G');
}

static void reset_commit(void *context, unsigned int state)
{
	struct reset_simulation *simulation = context;

	reset_event(simulation, state == 1 ? 'F' : 'C');
	simulation->committed_state = state;
}

static const struct airoha_gpon_session_reset_ops reset_operations = {
	.disable_tx = reset_disable_tx,
	.clear_data = reset_clear_data,
	.disable_omcc = reset_disable_omcc,
	.clear_gem_table = reset_clear_gem_table,
	.commit_reset = reset_commit,
};

static void initialize_active(struct simulation *simulation, u16 port,
			      bool encrypted)
{
	memset(simulation, 0, sizeof(*simulation));
	simulation->gem_valid[port] = true;
	simulation->gem_encrypted[port] = encrypted;
	simulation->ethernet_port = port;
	simulation->ethernet_valid = true;
	simulation->committed_port = port;
	simulation->committed_valid = true;
}

static int configure(struct simulation *simulation, u16 port, bool enable)
{
	struct airoha_gpon_port_snapshot snapshot = {
		.active_port = simulation->committed_port,
		.active_valid = simulation->committed_valid,
		.active_encrypted = simulation->gem_encrypted[simulation->committed_port],
		.target_valid = simulation->gem_valid[port],
		.target_encrypted = simulation->gem_encrypted[port],
	};

	return airoha_gpon_configure_port_transaction(
		&operations, simulation, port, enable, &snapshot);
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
		return 1; \
	} \
} while (0)

static int test_enable(void)
{
	struct simulation simulation;
	int ret;

	initialize_active(&simulation, 100, true);
	ret = configure(&simulation, 200, true);
	CHECK(!ret);
	CHECK(!strcmp(simulation.events, "GOgCA"));
	CHECK(simulation.gem_valid[200] && !simulation.gem_encrypted[200]);
	CHECK(!simulation.gem_valid[100]);
	CHECK(simulation.ethernet_valid && simulation.ethernet_port == 200);
	CHECK(simulation.committed_valid && simulation.committed_port == 200);
	CHECK(simulation.acknowledgements == 1);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 1;
	ret = configure(&simulation, 200, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "Gg"));
	CHECK(simulation.gem_valid[100] && !simulation.gem_valid[200]);
	CHECK(simulation.ethernet_port == 100 && simulation.committed_port == 100);
	CHECK(!simulation.acknowledgements);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 2;
	ret = configure(&simulation, 200, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GOg"));
	CHECK(simulation.gem_valid[100] && !simulation.gem_valid[200]);
	CHECK(simulation.ethernet_port == 100 && simulation.committed_port == 100);
	CHECK(!simulation.acknowledgements);

	initialize_active(&simulation, 100, true);
	simulation.gem_valid[200] = true;
	simulation.gem_encrypted[200] = true;
	simulation.fail_at = 2;
	ret = configure(&simulation, 200, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GOG"));
	CHECK(simulation.gem_valid[200] && simulation.gem_encrypted[200]);
	CHECK(simulation.ethernet_port == 100 && simulation.committed_port == 100);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 3;
	ret = configure(&simulation, 200, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GOgGOg"));
	CHECK(simulation.gem_valid[100] && !simulation.gem_valid[200]);
	CHECK(simulation.ethernet_port == 100 && simulation.committed_port == 100);
	CHECK(!simulation.acknowledgements);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 5;
	ret = configure(&simulation, 200, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GOgCA"));
	CHECK(!simulation.gem_valid[100] && simulation.gem_valid[200]);
	CHECK(simulation.ethernet_port == 200 && simulation.committed_port == 200);
	CHECK(simulation.acknowledgements == 1);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 3;
	simulation.fail_at_second = 4;
	ret = configure(&simulation, 200, true);
	CHECK(ret == AIROHA_GPON_PORT_TRANSACTION_ROLLBACK_FAILED);
	CHECK(!strcmp(simulation.events, "GOgGOgF"));
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

static int test_disable(void)
{
	struct simulation simulation;
	int ret;

	initialize_active(&simulation, 100, true);
	ret = configure(&simulation, 100, false);
	CHECK(!ret && !strcmp(simulation.events, "gocA"));
	CHECK(!simulation.gem_valid[100]);
	CHECK(!simulation.ethernet_valid && !simulation.committed_valid);
	CHECK(simulation.acknowledgements == 1);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 1;
	ret = configure(&simulation, 100, false);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "gG"));
	CHECK(simulation.gem_valid[100] && simulation.gem_encrypted[100]);
	CHECK(simulation.ethernet_valid && simulation.committed_valid);
	CHECK(!simulation.acknowledgements);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 2;
	ret = configure(&simulation, 100, false);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "goGO"));
	CHECK(simulation.gem_valid[100] && simulation.gem_encrypted[100]);
	CHECK(simulation.ethernet_valid && simulation.committed_valid);
	CHECK(!simulation.acknowledgements);

	initialize_active(&simulation, 100, false);
	ret = configure(&simulation, 200, false);
	CHECK(ret == AIROHA_GPON_PORT_TRANSACTION_REJECTED);
	CHECK(!simulation.event_count && !simulation.acknowledgements);
	CHECK(simulation.gem_valid[100] && simulation.committed_port == 100);

	memset(&simulation, 0, sizeof(simulation));
	ret = configure(&simulation, 100, false);
	CHECK(!ret && !strcmp(simulation.events, "A"));
	CHECK(simulation.acknowledgements == 1);

	initialize_active(&simulation, 100, true);
	simulation.fail_at = 2;
	simulation.fail_at_second = 3;
	ret = configure(&simulation, 100, false);
	CHECK(ret == AIROHA_GPON_PORT_TRANSACTION_ROLLBACK_FAILED);
	CHECK(!strcmp(simulation.events, "goGOF"));
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

static int test_encryption(void)
{
	struct simulation simulation;
	int ret;

	memset(&simulation, 0, sizeof(simulation));
	ret = airoha_gpon_encrypted_port_transaction(
		&operations, &simulation, 200, false, false, true);
	CHECK(ret == AIROHA_GPON_PORT_TRANSACTION_REJECTED);
	CHECK(!simulation.event_count && !simulation.acknowledgements);

	memset(&simulation, 0, sizeof(simulation));
	simulation.gem_valid[200] = true;
	simulation.fail_at = 1;
	ret = airoha_gpon_encrypted_port_transaction(
		&operations, &simulation, 200, true, false, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GG"));
	CHECK(simulation.gem_valid[200] && !simulation.gem_encrypted[200]);
	CHECK(!simulation.acknowledgements);

	memset(&simulation, 0, sizeof(simulation));
	simulation.gem_valid[200] = true;
	ret = airoha_gpon_encrypted_port_transaction(
		&operations, &simulation, 200, true, false, true);
	CHECK(!ret && !strcmp(simulation.events, "GA"));
	CHECK(simulation.gem_valid[200] && simulation.gem_encrypted[200]);
	CHECK(simulation.acknowledgements == 1);

	memset(&simulation, 0, sizeof(simulation));
	simulation.gem_valid[200] = true;
	simulation.fail_at = 2;
	ret = airoha_gpon_encrypted_port_transaction(
		&operations, &simulation, 200, true, false, true);
	CHECK(ret == TEST_ERROR && !strcmp(simulation.events, "GA"));
	CHECK(simulation.gem_valid[200] && simulation.gem_encrypted[200]);
	CHECK(simulation.acknowledgements == 1);

	memset(&simulation, 0, sizeof(simulation));
	simulation.gem_valid[200] = true;
	simulation.fail_at = 1;
	simulation.fail_at_second = 2;
	ret = airoha_gpon_encrypted_port_transaction(
		&operations, &simulation, 200, true, false, true);
	CHECK(ret == AIROHA_GPON_PORT_TRANSACTION_ROLLBACK_FAILED);
	CHECK(!strcmp(simulation.events, "GGF"));
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

static int test_session_reset(void)
{
	struct reset_simulation simulation;
	unsigned int fail_at;
	int ret;

	memset(&simulation, 0, sizeof(simulation));
	ret = airoha_gpon_session_reset_transaction(
		&reset_operations, &simulation, 2, 1);
	CHECK(!ret && !strcmp(simulation.events, "TDOGC"));
	CHECK(simulation.tx_disabled && simulation.committed_state == 2);

	for (fail_at = 1; fail_at <= 4; fail_at++) {
		memset(&simulation, 0, sizeof(simulation));
		simulation.fail_at = fail_at;
		ret = airoha_gpon_session_reset_transaction(
			&reset_operations, &simulation, 2, 1);
		CHECK(ret == TEST_ERROR);
		CHECK(!strcmp(simulation.events, "TDOGF"));
		CHECK(simulation.tx_disabled && simulation.committed_state == 1);
	}
	return 0;
}

int main(void)
{
	if (test_enable() || test_disable() || test_encryption() ||
	    test_session_reset())
		return 1;

	puts("GPON Port-ID transaction fault injection passed");
	return 0;
}
