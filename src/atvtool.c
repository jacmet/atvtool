/*
 * atvtool: Appletv utility program
 *
 * Copyright (C) 2008 Peter Korsgaard <jacmet@sunsite.dk>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <usb.h>

#define VENDOR_APPLE		0x05ac
#define PRODUCT_IR		0x8241

#define LEDMODE_OFF		0
#define LEDMODE_AMBER		1
#define LEDMODE_AMBER_BLINK	2
#define LEDMODE_WHITE		3
#define LEDMODE_WHITE_BLINK	4
#define LEDMODE_BOTH		5
#define LEDMODE_MAX		5

/* from libusb usbi.h */
struct usb_dev_handle {
	int fd;

	struct usb_bus *bus;
	struct usb_device *device;

	int config;
	int interface;
	int altsetting;

	/* Added by RMT so implementations can store other per-open-device data */
	void *impl_info;
};

/* figure out kernel name corresponding to usb device */
static int usb_make_kernel_name(usb_dev_handle *dev, int interface,
				char *name, int namelen)
{
	DIR *dir;
	struct dirent *ent;
	char busstr[10], devstr[10];
	int buslen, devlen;

	/* kernel names are in the form of:
	   <busnum>-<devpath>:<config>.<interface>
	   We have everything besides devpath, but there doesn't seem to be
	   an easy of going from devnum to devpath, so we scan sysfs */

	buslen = sprintf(busstr, "%d-", atoi(dev->bus->dirname));
	devlen = sprintf(devstr, "%d\n", dev->device->devnum);

	/* scan /sys/bus/usb/devices/<busnum>-* and compare devnum */
	if (chdir("/sys/bus/usb/devices"))
		return -1;

	dir = opendir(".");
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		char buf[PATH_MAX];
		int fd;

		/* only check devices on busnum bus */
		if (strncmp(busstr, ent->d_name, buslen))
			continue;

		sprintf(buf, "%s/devnum", ent->d_name);
		fd = open(buf, O_RDONLY);
		if (fd == -1)
			continue;

		if ((read(fd, buf, sizeof(buf)) == devlen)
		    && !strncmp(buf, devstr, devlen)) {

			close(fd);

			if (snprintf(name, namelen, "%s:%d.%d", ent->d_name,
				     1, interface) >= namelen)
				goto out;

			/* closedir could invalidate ent, so do it after the
			   snprintf */
			closedir(dir);
			return 0;
		}

		close(fd);
	}

 out:
	closedir(dir);
	return -1;
}

/* (re)attach usb device to kernel driver (need hotplug support in kernel) */
static int usb_attach_kernel_driver_np(usb_dev_handle *dev, int interface,
				       const char *driver)
{
	char name[PATH_MAX], buf[PATH_MAX];

	if (!dev || !driver || interface < 0)
		return -1;

	if (!usb_make_kernel_name(dev, interface, name, sizeof(name))) {
		int fd, ret, len;

		/* (re)bind driver to device */
		sprintf(buf, "/sys/bus/usb/drivers/%s/bind", driver);
		len = strlen(name);

		fd = open(buf, O_WRONLY);
		if (fd == -1)
			return -1;

		ret = write(fd, name, len);
		close(fd);

		if (ret != len)
			return -1;
		else
			return 0;
	}

	return -1;
}

static usb_dev_handle *find_ir(void)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	for (bus = usb_busses; bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next)
			if (dev->descriptor.idVendor == VENDOR_APPLE
			    && dev->descriptor.idProduct == PRODUCT_IR)
				return usb_open(dev);
	}

	return NULL;
}

static usb_dev_handle *get_ir(void)
{
	static usb_dev_handle *ir = NULL;

	if (!ir) {
		usb_init();
		usb_find_busses();
		usb_find_devices();

		ir = find_ir();
		if (!ir) {
			fprintf(stderr, "IR receiver not found, quitting\n");
			exit(1);
		}

		/* interface is normally handled by hiddev */
		usb_detach_kernel_driver_np(ir, 0);
		usb_claim_interface(ir, 0);
		usb_set_configuration(ir, 1);
	}

	return ir;
}

static void reattach(void)
{
	usb_dev_handle *ir;

	ir = get_ir();
	if (ir) {
		usb_release_interface(ir, 0);
		/* attach fails if we still have the file
		   descriptor open */
		usb_close(ir);
		usb_attach_kernel_driver_np(ir, 0, "usbhid");
	}
}

static int set_report(void *data, int len)
{
	unsigned char *type = data;
	int val;

	val = 0x300 | *type;

	return (usb_control_msg(get_ir(), USB_ENDPOINT_OUT | USB_TYPE_CLASS
				| USB_RECIP_INTERFACE, 9, val, 0,
				data, len, 1000) != len);
}

static void set_fan(int full)
{
	unsigned char buf[2];

	buf[0] = 0xf; buf[1] = full ? 1 : 0;

	set_report(buf, sizeof(buf));
}

static void set_led(int mode)
{
	unsigned char buf[5];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xd; buf[1] = mode;

	switch (mode) {
	case LEDMODE_OFF:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		break;

	case LEDMODE_AMBER:
		set_report(buf, sizeof(buf));
		buf[1] = 3; buf[2] = 1;
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 0;
		set_report(buf, 3);
		break;

	case LEDMODE_AMBER_BLINK:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		buf[1] = 3; buf[2] = 2;
		set_report(buf, 3);
		break;

	case LEDMODE_WHITE:
		set_report(buf, sizeof(buf));
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 1;
		set_report(buf, 3);
		break;

	case LEDMODE_WHITE_BLINK:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 2;
		set_report(buf, 3);
		break;

	case LEDMODE_BOTH:
		buf[1] = 7;
		set_report(buf, sizeof(buf));
		buf[1] = 6; buf[2] = 1;
		set_report(buf, 3);
		break;
	}
}

static void set_led_brightness(int high)
{
	unsigned char buf[5];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xd;

	if (high) {
		buf[1] = 6;
		set_report(buf, sizeof(buf));
		buf[1] = 5; buf[2] = 1;
		set_report(buf, 3);
	} else {
		buf[1] = 5;
		set_report(buf, sizeof(buf));
		set_report(buf, 3);
	}
}


static void usage(void)
{
	fprintf(stderr, "usage: atvtool [OPTION] ...\n"
		"  -h, --help\t\tshow usage info\n"
		"  -v, --version\t\tshow version info\n"
		"  -r, --reattach\treattach kernel driver to IR device\n"
		"  -f, --fan-off\t\tturn fan off\n"
		"  -F, --fan-on\t\tturn fan on\n"
		"  -b, --brightness-low\tuse low led brightness\n"
		"  -B, --brightness-high\tuse high led brightness\n"
		"  -l, --led\t\tset led mode to one of:\n"
		"\t\t\t  0\toff\n"
		"\t\t\t  1\tamber\n"
		"\t\t\t  2\tamber blink\n"
		"\t\t\t  3\twhite\n"
		"\t\t\t  4\twhite blink\n"
		"\t\t\t  5\tboth blink\n");
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "help",		no_argument,	 	0, 'h' },
		{ "version",		no_argument,	 	0, 'v' },
		{ "reattach",		no_argument, 		0, 'r' },
		{ "fan-off",		no_argument, 		0, 'f' },
		{ "fan-on",		no_argument,		0, 'F' },
		{ "brightness-low",	no_argument,		0, 'b' },
		{ "brightness-high",	no_argument,	 	0, 'B' },
		{ "led",		required_argument,	0, 'l' },
		{ 0, 0, 0, 0 }
	};
	int c;

	if (argc == 1)
		usage();
	else do {
		c = getopt_long(argc, argv, "hvrfFbBl:", options, 0);

		switch (c) {
		case 'v':
			printf("atvtool version " VERSION ", (C) 2008 "
			       "Peter Korsgaard <jacmet@sunsite.dk>\n");
			exit(0);
			break;

		case 'r':
			reattach();
			break;

		case 'f':
		case 'F':
			set_fan(c == 'F');
			break;

		case 'b':
		case 'B':
			set_led_brightness(c == 'B');
			break;

		case 'l':
		{
			int val;
			char *endp;

			val = strtol(optarg, &endp, 0);
			if ((*endp) || (val > LEDMODE_MAX) || (val < 0)) {
				fprintf(stderr,
					"invalid led mode '%s'\n", optarg);
				usage();
				exit(1);
			}
			set_led(val);
		}
		break;

		case -1:
			break;

		default:
			usage();
			break;
		}

	} while (c != -1);

	return 0;
}
