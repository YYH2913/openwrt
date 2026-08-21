#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airoha-epon-ctc-transaction.h"

enum operation {
	OP_GET_FEC,
	OP_SET_FEC,
	OP_GET_HOLDOVER,
	OP_SET_HOLDOVER,
	OP_APPLY_MANAGEMENT,
};

struct fake_backend {
	bool fec_rx;
	bool fec_tx;
	bool holdover_enabled;
	uint32_t holdover_time_ms;
	enum operation log[16];
	unsigned int calls;
	unsigned int fail_call[2];
	int failure[2];
};

static int record(struct fake_backend *backend, enum operation operation)
{
	unsigned int fault;

	backend->log[backend->calls++] = operation;
	for (fault = 0; fault < 2; fault++)
		if (backend->calls == backend->fail_call[fault])
			return backend->failure[fault];
	return 0;
}

static int fake_get_fec(void *context, unsigned int llid, bool *rx,
		bool *tx)
{
	struct fake_backend *backend = context;
	int result;

	assert(llid == 7);
	result = record(backend, OP_GET_FEC);
	if (!result) {
		*rx = backend->fec_rx;
		*tx = backend->fec_tx;
	}
	return result;
}

static int fake_set_fec(void *context, unsigned int llid, bool rx, bool tx)
{
	struct fake_backend *backend = context;
	int result;

	assert(llid == 7);
	result = record(backend, OP_SET_FEC);
	if (!result) {
		backend->fec_rx = rx;
		backend->fec_tx = tx;
	}
	return result;
}

static int fake_get_holdover(void *context, bool *enabled, uint32_t *time_ms)
{
	struct fake_backend *backend = context;
	int result = record(backend, OP_GET_HOLDOVER);

	if (!result) {
		*enabled = backend->holdover_enabled;
		*time_ms = backend->holdover_time_ms;
	}
	return result;
}

static int fake_set_holdover(void *context, bool enabled, uint32_t time_ms)
{
	struct fake_backend *backend = context;
	int result = record(backend, OP_SET_HOLDOVER);

	if (!result) {
		backend->holdover_enabled = enabled;
		backend->holdover_time_ms = time_ms;
	}
	return result;
}

static int fake_apply_management(void *context, unsigned int llid,
		struct airoha_ctc_management_set *management)
{
	struct fake_backend *backend = context;
	int result;

	assert(llid == 7 && management->aging_present);
	result = record(backend, OP_APPLY_MANAGEMENT);
	if (!result)
		management->aging_applied = true;
	return result;
}

static const struct airoha_ctc_set_transaction_ops fake_ops = {
	.get_fec = fake_get_fec,
	.set_fec = fake_set_fec,
	.get_holdover = fake_get_holdover,
	.set_holdover = fake_set_holdover,
	.apply_management = fake_apply_management,
};

static struct airoha_ctc_set_transaction_request request_for(
		struct airoha_ctc_management_set *management)
{
	struct airoha_ctc_set_transaction_request request = {
		.llid = 7,
		.fec_present = true,
		.fec_enabled = true,
		.holdover_present = true,
		.holdover_enabled = true,
		.holdover_time_ms = 900,
		.management_present = true,
		.management = management,
	};

	return request;
}

static void assert_operation(const struct fake_backend *backend,
		unsigned int index, enum operation operation)
{
	assert(index < backend->calls);
	assert(backend->log[index] == operation);
}

static void test_holdover_failure_restores_fec(void)
{
	struct fake_backend backend = {
		.fec_rx = false,
		.fec_tx = false,
		.holdover_time_ms = 125,
		.fail_call = { 4 },
		.failure = { -EIO },
	};
	struct airoha_ctc_management_set management = {
		.aging_present = true,
		.aging_valid = true,
		.aging_time_seconds = 300,
	};
	struct airoha_ctc_set_transaction_request request =
		request_for(&management);
	struct airoha_ctc_set_transaction_result result;
	struct airoha_ctc_set_transaction_ops ops = fake_ops;

	ops.context = &backend;
	assert(airoha_ctc_execute_set_transaction(&ops, &request, &result) ==
	       -EIO);
	assert(backend.calls == 5);
	assert_operation(&backend, 0, OP_GET_FEC);
	assert_operation(&backend, 1, OP_GET_HOLDOVER);
	assert_operation(&backend, 2, OP_SET_FEC);
	assert_operation(&backend, 3, OP_SET_HOLDOVER);
	assert_operation(&backend, 4, OP_SET_FEC);
	assert(!backend.fec_rx && !backend.fec_tx);
	assert(!management.aging_applied);
	assert(!airoha_ctc_set_transaction_fatal(&result));
}

static void test_management_failure_restores_holdover_and_fec(void)
{
	struct fake_backend backend = {
		.fec_rx = false,
		.fec_tx = true,
		.holdover_enabled = false,
		.holdover_time_ms = 125,
		.fail_call = { 5 },
		.failure = { -EBUSY },
	};
	struct airoha_ctc_management_set management = {
		.aging_present = true,
		.aging_valid = true,
		.aging_time_seconds = 300,
	};
	struct airoha_ctc_set_transaction_request request =
		request_for(&management);
	struct airoha_ctc_set_transaction_result result;
	struct airoha_ctc_set_transaction_ops ops = fake_ops;

	ops.context = &backend;
	assert(airoha_ctc_execute_set_transaction(&ops, &request, &result) ==
	       -EBUSY);
	assert(backend.calls == 7);
	assert_operation(&backend, 4, OP_APPLY_MANAGEMENT);
	assert_operation(&backend, 5, OP_SET_HOLDOVER);
	assert_operation(&backend, 6, OP_SET_FEC);
	assert(!backend.fec_rx && backend.fec_tx);
	assert(!backend.holdover_enabled && backend.holdover_time_ms == 125);
	assert(!management.aging_applied);
	assert(!airoha_ctc_set_transaction_fatal(&result));
}

static void test_rollback_failure_is_fatal_and_does_not_skip_fec(void)
{
	struct fake_backend backend = {
		.fec_rx = false,
		.fec_tx = true,
		.holdover_time_ms = 125,
		.fail_call = { 5, 6 },
		.failure = { -EADDRINUSE, -EIO },
	};
	struct airoha_ctc_management_set management = {
		.aging_present = true,
		.aging_valid = true,
		.aging_time_seconds = 300,
	};
	struct airoha_ctc_set_transaction_request request =
		request_for(&management);
	struct airoha_ctc_set_transaction_result result;
	struct airoha_ctc_set_transaction_ops ops = fake_ops;

	ops.context = &backend;
	assert(airoha_ctc_execute_set_transaction(&ops, &request, &result) ==
	       -EADDRINUSE);
	assert(backend.calls == 7);
	assert_operation(&backend, 4, OP_APPLY_MANAGEMENT);
	assert_operation(&backend, 5, OP_SET_HOLDOVER);
	assert_operation(&backend, 6, OP_SET_FEC);
	assert(!backend.fec_rx && backend.fec_tx);
	assert(backend.holdover_enabled && backend.holdover_time_ms == 900);
	assert(result.failure == -EADDRINUSE);
	assert(result.rollback_failure == -EIO);
	assert(airoha_ctc_set_transaction_fatal(&result));
}

static void test_platform_internal_rollback_failure_is_fatal(void)
{
	struct fake_backend backend = {
		.fec_tx = true,
		.holdover_time_ms = 125,
		.fail_call = { 5 },
		.failure = { -EUCLEAN },
	};
	struct airoha_ctc_management_set management = {
		.aging_present = true,
		.aging_valid = true,
		.aging_time_seconds = 300,
	};
	struct airoha_ctc_set_transaction_request request =
		request_for(&management);
	struct airoha_ctc_set_transaction_result result;
	struct airoha_ctc_set_transaction_ops ops = fake_ops;

	ops.context = &backend;
	assert(airoha_ctc_execute_set_transaction(&ops, &request, &result) ==
	       -EUCLEAN);
	assert(backend.calls == 7);
	assert_operation(&backend, 5, OP_SET_HOLDOVER);
	assert_operation(&backend, 6, OP_SET_FEC);
	assert(result.rollback_failure == -EUCLEAN);
	assert(airoha_ctc_set_transaction_fatal(&result));
}

int main(void)
{
	test_holdover_failure_restores_fec();
	test_management_failure_restores_holdover_and_fec();
	test_rollback_failure_is_fatal_and_does_not_skip_fec();
	test_platform_internal_rollback_failure_is_fatal();
	return 0;
}
