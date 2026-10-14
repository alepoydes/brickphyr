// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>

LOG_MODULE_REGISTER(native_diagnostics_test, LOG_LEVEL_INF);

int native_diagnostics_check(void)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	size_t length;
	const char *output;

	/* Start with an empty retained queue, independent of boot diagnostics. */
	if (shell_execute_cmd(shell, "diagnostics")) return -EIO;
	shell_backend_dummy_clear_output(shell);
	LOG_WRN("native diagnostic warning marker");
	LOG_ERR("native diagnostic error marker");
	LOG_INF("native diagnostic info marker");
	log_flush();
	if (shell_execute_cmd(shell, "diagnostics")) return -EIO;
	output = shell_backend_dummy_get_output(shell, &length);
	if (!strstr(output, "native diagnostic warning marker") ||
	    !strstr(output, "native diagnostic error marker") ||
	    strstr(output, "native diagnostic info marker")) return -EIO;

	shell_backend_dummy_clear_output(shell);
	if (shell_execute_cmd(shell, "diagnostics")) return -EIO;
	output = shell_backend_dummy_get_output(shell, &length);
	if (strstr(output, "native diagnostic warning marker") ||
	    strstr(output, "native diagnostic error marker")) return -EIO;
	return 0;
}
