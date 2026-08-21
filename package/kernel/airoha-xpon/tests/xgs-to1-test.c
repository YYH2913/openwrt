// SPDX-License-Identifier: GPL-2.0-only
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-xgs-to1.h"

#define O4_STATE 4U
#define TEST_TX_ERROR (-5)
#define TEST_SESSION_ERROR (-6)

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

struct cleanup_simulation {
	int disable_error;
	int session_error;
	char events[3];
	unsigned int event_count;
};

static int disable_tx(void *context)
{
	struct cleanup_simulation *simulation = context;

	simulation->events[simulation->event_count++] = 'T';
	simulation->events[simulation->event_count] = '\0';
	return simulation->disable_error;
}

static int clear_session(void *context)
{
	struct cleanup_simulation *simulation = context;

	simulation->events[simulation->event_count++] = 'C';
	simulation->events[simulation->event_count] = '\0';
	return simulation->session_error;
}

static const struct airoha_xgs_to1_cleanup_ops cleanup_ops = {
	.disable_tx = disable_tx,
	.clear_session = clear_session,
};

static struct airoha_xgs_to1_state valid_state(unsigned long deadline)
{
	struct airoha_xgs_to1_state state = {
		.enabled = true,
		.hardware_selected = true,
		.mac_present = true,
		.deadline = deadline,
		.armed_generation = 7,
		.current_generation = 7,
		.expected_state = O4_STATE,
		.current_state = O4_STATE,
	};

	return state;
}

static int test_decisions(void)
{
	struct airoha_xgs_to1_state state = valid_state(100);

	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_WAIT);
	CHECK(airoha_xgs_to1_decide(&state, 100, O4_STATE) ==
	      AIROHA_XGS_TO1_EXPIRE);
	CHECK(airoha_xgs_to1_decide(&state, 101, O4_STATE) ==
	      AIROHA_XGS_TO1_EXPIRE);

	state.removing = true;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.enabled = false;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.hardware_selected = false;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.mac_present = false;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(0);
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.armed_generation++;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.current_state = 5;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);
	state = valid_state(100);
	state.expected_state = 5;
	state.current_state = 5;
	CHECK(airoha_xgs_to1_decide(&state, 99, O4_STATE) ==
	      AIROHA_XGS_TO1_DISARM);

	state = valid_state(3);
	CHECK(airoha_xgs_to1_decide(&state, ULONG_MAX - 5, O4_STATE) ==
	      AIROHA_XGS_TO1_WAIT);
	CHECK(airoha_xgs_to1_decide(&state, 3, O4_STATE) ==
	      AIROHA_XGS_TO1_EXPIRE);
	return 0;
}

static int run_cleanup(int disable_error, int session_error,
		       int expected_error)
{
	struct cleanup_simulation simulation = {
		.disable_error = disable_error,
		.session_error = session_error,
	};
	struct airoha_xgs_to1_cleanup_result result;
	int ret;

	ret = airoha_xgs_to1_cleanup_transaction(
		&cleanup_ops, &simulation, &result);
	CHECK(ret == expected_error);
	CHECK(result.disable_error == disable_error);
	CHECK(result.session_error == session_error);
	CHECK(!strcmp(simulation.events, "TC"));
	return 0;
}

static int test_cleanup(void)
{
	CHECK(!run_cleanup(0, 0, 0));
	CHECK(!run_cleanup(TEST_TX_ERROR, 0, TEST_TX_ERROR));
	CHECK(!run_cleanup(0, TEST_SESSION_ERROR, TEST_SESSION_ERROR));
	CHECK(!run_cleanup(TEST_TX_ERROR, TEST_SESSION_ERROR, TEST_TX_ERROR));
	return 0;
}

int main(void)
{
	if (test_decisions() || test_cleanup())
		return 1;
	puts("XG/XGS TO1 state and cleanup transactions passed");
	return 0;
}
