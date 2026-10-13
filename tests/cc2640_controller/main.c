// SPDX-License-Identifier: Apache-2.0
#include "cc2640_controller.h"
#include "cc2640_gatt.h"
#include "cc2640_npi.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

/* Literal startup HCI packets from the pinned controller command definitions. */
static const uint8_t address[] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba};
static const uint8_t set_address[] = {1, 0x0c, 0xfc, 6,
	0x10, 0x32, 0x54, 0x76, 0x98, 0xba};
static const uint8_t read_address[] = {1, 0x09, 0x10, 0};
static const uint8_t set_power[] = {1, 0x01, 0xfc, 1, 7};
static const uint8_t read_version[] = {1, 0x01, 0x10, 0};
static const uint8_t set_data_length[] = {1, 0x21, 0xfc, 8,
	0x1b, 0, 0x48, 1, 0x1b, 0, 0x48, 1};
static const uint8_t set_features[] = {1, 0x06, 0xfc, 8,
	1, 0, 0, 0, 0, 0, 0, 0};

/* Vendor event code is LE16 at raw offsets 3:4, status at 5. Standard
 * CommandComplete carries ncmd, opcode, status at offsets 3, 4:5, 6. */
static const uint8_t address_set[] = {4, 0xff, 3, 0x0c, 4, 0};
static const uint8_t address_read[] = {4, 0x0e, 10, 1, 0x09, 0x10, 0,
	0x10, 0x32, 0x54, 0x76, 0x98, 0xba};
static const uint8_t power_set[] = {4, 0xff, 3, 0x01, 4, 0};
static const uint8_t version_read[] = {4, 0x0e, 12, 1, 0x01, 0x10, 0,
	8, 0x34, 0x12, 8, 0x0d, 0, 0, 0};
static const uint8_t data_length_set[] = {4, 0xff, 3, 0x21, 4, 0};
static const uint8_t features_set[] = {4, 0xff, 3, 0x06, 4, 0};
static const uint8_t wrong_completion[] = {4, 0x0e, 4, 1, 0x34, 0x12, 0};
static const uint8_t status_success[] = {4, 0xff, 6, 0x7f, 6, 0, 0x0c, 0xfc, 0};
static const uint8_t status_failure[] = {4, 0xff, 6, 0x7f, 6, 0x0c, 0x0c, 0xfc, 0};
static const uint8_t wrong_address[] = {4, 0x0e, 10, 1, 0x09, 0x10, 0,
	0x11, 0x32, 0x54, 0x76, 0x98, 0xba};
static const uint8_t short_version[] = {4, 0x0e, 4, 1, 0x01, 0x10, 0};

struct command_fixture {
	const uint8_t *command;
	size_t command_size;
	const uint8_t *reply;
	size_t reply_size;
};
static const struct command_fixture startup[] = {
	{set_address, sizeof(set_address), address_set, sizeof(address_set)},
	{read_address, sizeof(read_address), address_read, sizeof(address_read)},
	{set_power, sizeof(set_power), power_set, sizeof(power_set)},
	{read_version, sizeof(read_version), version_read, sizeof(version_read)},
	{set_data_length, sizeof(set_data_length), data_length_set, sizeof(data_length_set)},
	{set_features, sizeof(set_features), features_set, sizeof(features_set)},
};

enum scenario {
	NORMAL,
	INTERLEAVED,
	STATUS_FAILURE,
	ADDRESS_MISMATCH,
	NO_REPLY,
	TRUNCATED_VERSION,
};
static struct {
	enum scenario scenario;
	size_t commands;
	int active;
	size_t events;
	int resets;
	int offs;
	int off_error;
	bool mismatch;
	bool registering, corrupt_range;
	unsigned int service, attribute;
	uint16_t pending_opcode;
} radio;

/* Controller-allocated database for the six management services. This is a
 * synthetic reply fixture, not an expectation derived from the DUT's table. */
static const uint16_t ranges[][2] = {{1, 1}, {2, 8}, {9, 15}, {16, 21}, {22, 27}, {28, 33}};

static int registration_exchange(const uint8_t *command, size_t length, uint8_t *event)
{
	zassert_true(radio.service < ARRAY_SIZE(ranges));
	uint16_t first = ranges[radio.service][0], last = ranges[radio.service][1];
	if (length) {
		zassert_equal(radio.pending_opcode, 0);
		zassert_equal(command[0], 1);
		zassert_equal(command[3] + 4, length);
		radio.pending_opcode = sys_get_le16(command + 1);
		zassert_equal(radio.pending_opcode, radio.attribute ? 0xfdfe : 0xfdfc);
		if (!radio.attribute) {
			zassert_equal(sys_get_le16(command + 4), 0x2800);
			zassert_equal(sys_get_le16(command + 6), last - first + 1);
		}
		return 0;
	}
	zassert_not_equal(radio.pending_opcode, 0);
	bool complete = radio.attribute == last - first;
	uint8_t status[] = {4, 0xff, complete ? 10 : 6, 0x7f, 6, 0,
		(uint8_t)radio.pending_opcode, 0xfd, complete ? 4 : 0, 0, 0, 0, 0};
	if (complete) {
		/* Same-sized, overlapping allocation must still fail admission. */
		if (radio.corrupt_range && radio.service == 3) {
			first = 14;
			last = 19;
		}
		sys_put_le16(first, status + 9);
		sys_put_le16(last, status + 11);
		radio.service++;
		radio.attribute = 0;
	} else radio.attribute++;
	radio.pending_opcode = 0;
	size_t size = complete ? sizeof(status) : 9;
	memcpy(event, status, size);
	return size;
}

static int init_mock(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}
DEVICE_DEFINE(mock_npi, "mock_npi", init_mock, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

int cc2640_npi_reset(const struct device *dev)
{
	zassert_equal(dev, DEVICE_GET(mock_npi));
	radio.resets++;
	return 0;
}

int cc2640_npi_off(const struct device *dev)
{
	zassert_equal(dev, DEVICE_GET(mock_npi));
	radio.offs++;
	return radio.off_error;
}

int cc2640_npi_exchange(const struct device *dev, const uint8_t *command,
		       size_t length, uint8_t *event, k_timeout_t timeout)
{
	zassert_equal(dev, DEVICE_GET(mock_npi));
	zassert_not_null(event);
	if (radio.registering) return registration_exchange(command, length, event);
	if (length) {
		if (radio.active >= 0 || radio.commands >= ARRAY_SIZE(startup)) {
			radio.mismatch = true;
			return -EINVAL;
		}
		const struct command_fixture *step = &startup[radio.commands];
		if (!command || length != step->command_size ||
		    memcmp(command, step->command, length)) {
			radio.mismatch = true;
			return -EINVAL;
		}
		radio.active = radio.commands++;
		radio.events = 0;
		return 0; /* The NPI transaction had no simultaneous event. */
	}
	if (radio.active < 0) {
		radio.mismatch = true;
		return -EINVAL;
	}
	const uint8_t *reply = startup[radio.active].reply;
	size_t size = startup[radio.active].reply_size;
	if (radio.scenario == NO_REPLY && radio.active == 0) {
		/* Advances native_sim virtual time without a wall-clock wait. */
		k_sleep(timeout);
		return -EAGAIN;
	}
	if (radio.active == 0 && radio.scenario == INTERLEAVED) {
		if (radio.events == 0) {
			reply = wrong_completion;
			size = sizeof(wrong_completion);
		} else if (radio.events == 1) {
			reply = status_success;
			size = sizeof(status_success);
		}
	} else if (radio.active == 0 && radio.scenario == STATUS_FAILURE) {
		reply = status_failure;
		size = sizeof(status_failure);
	} else if (radio.active == 1 && radio.scenario == ADDRESS_MISMATCH) {
		reply = wrong_address;
		size = sizeof(wrong_address);
	} else if (radio.active == 3 && radio.scenario == TRUNCATED_VERSION) {
		reply = short_version;
		size = sizeof(short_version);
	}
	/* The controller must use a bounded wait for every reply. */
	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		radio.mismatch = true;
		return -EINVAL;
	}
	memcpy(event, reply, size);
	radio.events++;
	if (radio.scenario != INTERLEAVED || radio.active != 0 || radio.events >= 3) {
		radio.active = -1;
	}
	return size;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&radio, 0, sizeof(radio));
	radio.active = -1;
}

static struct cc2640_info sentinel(void)
{
	struct cc2640_info info;
	memset(&info, 0xa5, sizeof(info));
	return info;
}

static void assert_failure(enum scenario scenario, int expected)
{
	radio.scenario = scenario;
	radio.off_error = -EACCES;
	struct cc2640_info info = sentinel();
	struct cc2640_info original = info;
	zassert_equal(cc2640_controller_start(DEVICE_GET(mock_npi), address, &info), expected);
	zassert_mem_equal(&info, &original, sizeof(info));
	zassert_equal(radio.resets, 1);
	zassert_true(radio.offs >= 1);
	zassert_false(radio.mismatch);
}

ZTEST(cc2640_controller, test_literal_startup_and_interleaved_events)
{
	radio.scenario = INTERLEAVED;
	struct cc2640_info info = sentinel();
	zassert_ok(cc2640_controller_start(DEVICE_GET(mock_npi), address, &info));
	zassert_equal(radio.resets, 1);
	zassert_equal(radio.commands, ARRAY_SIZE(startup));
	zassert_equal(radio.active, -1);
	zassert_equal(radio.offs, 0);
	zassert_false(radio.mismatch);
	zassert_mem_equal(info.address, address, sizeof(address));
	zassert_equal(info.revision, 0x1234);
	zassert_equal(info.manufacturer, 0x000d);
	zassert_true(info.unrelated_events >= 1);
}

ZTEST(cc2640_controller, test_matching_status_failure_keeps_first_error)
{
	assert_failure(STATUS_FAILURE, -EIO);
	zassert_equal(radio.commands, 1);
}

ZTEST(cc2640_controller, test_read_address_must_match_factory_value)
{
	assert_failure(ADDRESS_MISMATCH, -EBADMSG);
	zassert_equal(radio.commands, 2);
}

ZTEST(cc2640_controller, test_missing_reply_times_out)
{
	assert_failure(NO_REPLY, -ETIMEDOUT);
	zassert_equal(radio.commands, 1);
}

ZTEST(cc2640_controller, test_truncated_completion_cannot_publish_info)
{
	assert_failure(TRUNCATED_VERSION, -EBADMSG);
	zassert_equal(radio.commands, 4);
}

ZTEST(cc2640_controller, test_gatt_registration_publishes_only_a_complete_database)
{
	struct cc2640_info info;
	zassert_ok(cc2640_controller_start(DEVICE_GET(mock_npi), address, &info));
	struct cc2640_gatt gatt;
	memset(&gatt, 0xa5, sizeof(gatt));
	radio.registering = true;
	zassert_ok(cc2640_controller_gatt(&gatt));
	zassert_equal(radio.service, ARRAY_SIZE(ranges));
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) zassert_equal(gatt.first[i], ranges[i][0]);
	for (size_t i = 0; i < ARRAY_SIZE(gatt.subscribed); i++) zassert_equal(gatt.subscribed[i], 0);
	zassert_equal(cc2640_controller_gatt(&gatt), -EALREADY);

	before(NULL);
	zassert_ok(cc2640_controller_start(DEVICE_GET(mock_npi), address, &info));
	struct cc2640_gatt previous = gatt;
	radio.registering = radio.corrupt_range = true;
	radio.off_error = -EACCES;
	zassert_equal(cc2640_controller_gatt(&gatt), -EBADMSG);
	zassert_mem_equal(&gatt, &previous, sizeof(gatt));
	zassert_equal(radio.offs, 1);
	zassert_equal(radio.service, 4);
	zassert_equal(cc2640_controller_gatt(&gatt), -ENODEV);
}

ZTEST_SUITE(cc2640_controller, NULL, NULL, before, NULL, NULL);
