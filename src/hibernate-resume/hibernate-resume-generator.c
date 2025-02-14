/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "dropin.h"
#include "generator.h"
#include "hibernate-resume-config.h"
#include "initrd-util.h"
#include "log.h"
#include "main-func.h"
#include "parse-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "static-destruct.h"
#include "string-util.h"
#include "unit-name.h"

static const char *arg_dest = NULL;
static char *arg_resume_options = NULL;
static char *arg_root_options = NULL;
static bool arg_noresume = false;

STATIC_DESTRUCTOR_REGISTER(arg_resume_options, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root_options, freep);

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        assert(key);

        if (streq(key, "resumeflags")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                if (!strextend_with_separator(&arg_resume_options, ",", value))
                        return log_oom();

        } else if (streq(key, "rootflags")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                if (!strextend_with_separator(&arg_root_options, ",", value))
                        return log_oom();

        } else if (streq(key, "noresume")) {

                if (value) {
                        log_warning("'noresume' kernel command line option specified with an argument, ignoring.");
                        return 0;
                }

                arg_noresume = true;
        }

        return 0;
}

static int process_resume(const HibernateInfo *info) {
        _cleanup_free_ char *device_unit = NULL;
        int r;

        assert(info);

        r = unit_name_from_path(info->device, ".device", &device_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate device unit name from path '%s': %m", info->device);

        r = generator_write_device_timeout(arg_dest, info->device, arg_resume_options ?: arg_root_options, NULL);
        if (r < 0)
                log_warning_errno(r, "Failed to write device timeout drop-in, ignoring: %m");
        if (r <= 0) {
                /* No timeout explicitly defined? Wait infinitely if resume= is specified, 2min if from EFI
                 * HibernateLocation variable. In the latter case, we avoid blocking the boot process forever
                 * if a stale var is detected while the swap device is not present. */
                r = write_drop_in_format(arg_dest, device_unit, 40, "device-timeout",
                                         "# Automatically generated by systemd-hibernate-resume-generator\n\n"
                                         "[Unit]\n"
                                         "JobTimeoutSec=%s\n",
                                         info->cmdline ? "infinity" : "2min");
                if (r < 0)
                        log_warning_errno(r, "Failed to write fallback device timeout drop-in, ignoring: %m");
        }

        r = write_drop_in_format(arg_dest, SPECIAL_HIBERNATE_RESUME_SERVICE, 90, "device-dependency",
                                 "# Automatically generated by systemd-hibernate-resume-generator\n\n"
                                 "[Unit]\n"
                                 "BindsTo=%1$s\n"
                                 "After=%1$s\n",
                                 device_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to write device dependency drop-in: %m");

        return generator_add_symlink(arg_dest, SPECIAL_SYSINIT_TARGET, "wants", SPECIAL_HIBERNATE_RESUME_SERVICE);
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        _cleanup_(hibernate_info_done) HibernateInfo info = {};
        int r;

        arg_dest = ASSERT_PTR(dest);

        /* Don't even consider resuming outside of initrd. */
        if (!in_initrd()) {
                log_debug("Not running in initrd, exiting.");
                return 0;
        }

        if (generator_soft_rebooted()) {
                log_debug("Running in an initrd entered through soft-reboot, not initiating resume.");
                return 0;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (arg_noresume) {
                log_info("Found 'noresume' on the kernel command line, exiting.");
                return 0;
        }

        r = acquire_hibernate_info(&info);
        if (r == -ENODEV) {
                log_debug_errno(r, "No resume device found, exiting.");
                return 0;
        }
        if (r < 0)
                return r;

        return process_resume(&info);
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
