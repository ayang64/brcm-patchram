/*******************************************************************************
 *
 *  Copyright (C) 2009-2011 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*****************************************************************************
**                                                                           
**  Name:          brcm_patchram_plus_h5.c
**
**  Description:   This program downloads a patchram files in the HCD format
**                 to Broadcom Bluetooth based silicon and combo chips and
**				   and other utility functions.
**
**                 It can be invoked from the command line in the form
**						<-d> to print a debug log
**						<--patchram patchram_file>
**						<--baudrate baud_rate>
**						<--bd_addr bd_address>
**						<--enable_lpm>
**						<--enable_h4 | --enable_h5>
**						<--use_baudrate_for_download>
**						<--scopcm=sco_routing,pcm_interface_rate,frame_type,
**							sync_mode,clock_mode,lsb_first,fill_bits,
**							fill_method,fill_num,right_justify>
**
**							Where
**
**							sco_routing is 0 for PCM, 1 for Transport,
**							2 for Codec and 3 for I2S,
**
**							pcm_interface_rate is 0 for 128KBps, 1 for
**							256 KBps, 2 for 512KBps, 3 for 1024KBps,
**							and 4 for 2048Kbps,
**
**							frame_type is 0 for short and 1 for long,
**
**							sync_mode is 0 for slave and 1 for master,
**
**							clock_mode is 0 for slabe and 1 for master,
**
**							lsb_first is 0 for false and 1 for true,
**
**							fill_bits is the value in decimal for unused bits,
**
**							fill_method is 0 for 0's and 1 for 1's, 2 for
**								signed and 3 for programmable,
**
**							fill_num is the number or bits to fill,
**
**							right_justify is 0 for false and 1 for true
**
**						<--i2s=i2s_enable,is_master,sample_rate,clock_rate>
**
**							Where
**
**							i2s_enable is 0 for disable and 1 for enable,
**
**							is_master is 0 for slave and 1 for master,
**
**							sample_rate is 0 for 8KHz, 1 for 16Khz and
**								2 for 4 KHz,
**
**							clock_rate is 0 for 128KHz, 1 for 256KHz, 3 for
**								1024 KHz and 4 for 2048 KHz.
**
**						<--no2bytes skips waiting for two byte confirmation
**							before starting patchram download. Newer chips
**                          do not generate these two bytes.>
**						<--tosleep=number of microsseconds to sleep before
**							patchram download begins.>
**						uart_device_name
**
**                 For example:
**
**                 brcm_patchram_plus -d --patchram  \
**						BCM2045B2_002.002.011.0348.0349.hcd /dev/ttyHS0
**
**                 It will return 0 for success and a number greater than 0
**                 for any errors.
**
**                 For Android, this program invoked using a 
**                 "system(2)" call from the beginning of the bt_enable
**                 function inside the file 
**                 system/bluetooth/bluedroid/bluetooth.c.
**
**                 If the Android system property "ro.bt.bcm_bdaddr_path" is
**                 set, then the bd_addr will be read from this path.
**                 This is overridden by --bd_addr on the command line.
**  
******************************************************************************/

#include <stdio.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdlib.h>

#ifdef ANDROID
#include <termios.h>
#else
#include <sys/termios.h>
#include <sys/ioctl.h>
#include <limits.h>
#endif

#include <string.h>
#include <signal.h>
#include <time.h>

#include "common.h"

#ifdef ANDROID
#include <cutils/properties.h>
#define LOG_TAG "brcm_patchram_plus"
#include <cutils/log.h>
#undef printf
#define printf LOGD
#undef fprintf
#define fprintf(x, ...) \
  { if(x==stderr) LOGE(__VA_ARGS__); else fprintf(x, __VA_ARGS__); }

#endif //ANDROID

#ifndef N_HCI
#define N_HCI	15
#endif

#define HCIUARTSETPROTO		_IOW('U', 200, int)
#define HCIUARTGETPROTO		_IOR('U', 201, int)
#define HCIUARTGETDEVICE	_IOR('U', 202, int)

#define HCI_UART_H4		0
#define HCI_UART_BCSP	1
#define HCI_UART_3WIRE	2
#define HCI_UART_H4DS	3
#define HCI_UART_LL		4
#define HCI_UART_H5		5

#define CHIP_ID_4330B2 0x43
#define CHIP_ID_4329B1 0x29

typedef unsigned char uchar;

int uart_fd = -1;
int hcdfile_fd = -1;
int termios_baudrate = 0;
int bdaddr_flag = 0;
int enable_lpm = 0;
int enable_h4 = 0;
int enable_h5 = 0;
int use_baudrate_for_download = 0;
int debug = 0;
int scopcm = 0;
int i2s = 0;
int no2bytes = 0;
int tosleep = 0;

struct termios termios;
uchar buffer[1024];

uchar hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };

uchar hci_download_minidriver[] = { 0x01, 0x2e, 0xfc, 0x00 };

uchar hci_update_baud_rate[] = { 0x01, 0x18, 0xfc, 0x06, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00 };

uchar hci_write_bd_addr[] = { 0x01, 0x01, 0xfc, 0x06,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

uchar hci_write_sleep_mode[] = { 0x01, 0x27, 0xfc, 0x0c,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00 };

uchar hci_write_sco_pcm_int[] =
	{ 0x01, 0x1C, 0xFC, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };

uchar hci_write_pcm_data_format[] =
	{ 0x01, 0x1e, 0xFC, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };

uchar hci_write_i2spcm_interface_param[] =
	{ 0x01, 0x6d, 0xFC, 0x04, 0x00, 0x00, 0x00, 0x00 };

uchar hci_read_verbose_config_version_info[] =
	{ 0x01, 0x79, 0xfc, 0x00 };

uchar slip_sync[] = 
	{ 0xc0, 0x00, 0x2f, 0x00, 0xd0, 0x01, 0x7e, 0xc0 };

uchar slip_sync_response[] = 
	{ 0xc0, 0x00, 0x2f, 0x00, 0xd0, 0x02, 0x7d, 0xc0 };

uchar slip_config[] = 
	{ 0xc0, 0x00, 0x3f, 0x00, 0xdb, 0xdc, 0x03, 0xfc, 0x1b, 0xc0 };

uchar slip_config_response[] = 
	{ 0xc0, 0x00, 0x3f, 0x00, 0xdb, 0xdc, 0x04, 0x7b, 0x1b, 0xc0 };
	
uchar slip_config_null_response[] = 
	{ 0xc0, 0x00, 0x2f, 0x00, 0xd0, 0x04, 0x7b, 0xc0 };

uchar slip_read_local_version_info[] = 
	{ 0xc0, 0xdb, 0xdc, 0x31, 0x00, 0x0e, 0x01, 0x10, 0x00, 0x06, 0x09, 0xc0 };

uchar slip_ack[] = 
	{ 0xc0, 0x48, 0x00, 0x00, 0xb7, 0x5e, 0x8c, 0xc0 };

int
parse_patchram(char *optarg)
{
	char *p;

	if (!(p = strrchr(optarg, '.'))) {
		fprintf(stderr, "file %s not an HCD file\n", optarg);
		exit(3);
	}

	p++;

	if (strcasecmp("hcd", p) != 0) {
		fprintf(stderr, "file %s not an HCD file\n", optarg);
		exit(4);
	}

	if ((hcdfile_fd = open(optarg, O_RDONLY)) == -1) {
		fprintf(stderr, "file %s could not be opened, error %d\n", optarg, errno);
		exit(5);
	}

	return 0;
}

void
BRCM_encode_baud_rate(uint baud_rate, uchar *encoded_baud)
{
	if(baud_rate == 0 || encoded_baud == NULL) {
		fprintf(stderr, "Baudrate not supported!");
		return;
	}

	encoded_baud[3] = (uchar)(baud_rate >> 24);
	encoded_baud[2] = (uchar)(baud_rate >> 16);
	encoded_baud[1] = (uchar)(baud_rate >> 8);
	encoded_baud[0] = (uchar)(baud_rate & 0xFF);
}


int
parse_baudrate(char *optarg)
{
	int baudrate = atoi(optarg);

	termios_baudrate = validate_baudrate(baudrate);

	if (termios_baudrate == -1)
		return -1;

	BRCM_encode_baud_rate(baudrate, &hci_update_baud_rate[6]);
	return termios_baudrate;
}

int
parse_bdaddr(char *optarg)
{
	int bd_addr[6];
	int i;

	sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X",
		&bd_addr[5], &bd_addr[4], &bd_addr[3],
		&bd_addr[2], &bd_addr[1], &bd_addr[0]);

	for (i = 0; i < 6; i++) {
		hci_write_bd_addr[4 + i] = bd_addr[i];
	}

	bdaddr_flag = 1;

	return 0;
}

int
parse_enable_lpm(void)
{
	enable_lpm = 1;
	return 0;
}

int
parse_use_baudrate_for_download(void)
{
	use_baudrate_for_download = 1;
	return 0;
}

int
parse_enable_h4(void)
{
	enable_h4 = 1;
	return 0;
}

int
parse_enable_h5(void)
{
	enable_h5 = 1;
	return 0;
}

static int
parse_scopcm(char *optarg)
{
	int param[10];
	int ret;
	int i;

	ret = sscanf(optarg, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
		&param[0], &param[1], &param[2], &param[3], &param[4],
		&param[5], &param[6], &param[7], &param[8], &param[9]);

	if (ret != 10) {
		return 1;
	}

	scopcm = 1;

	for (i = 0; i < 5; i++) {
		hci_write_sco_pcm_int[4 + i] = param[i];
	}

	for (i = 0; i < 5; i++) {
		hci_write_pcm_data_format[4 + i] = param[5 + i];
	}

	return 0;
}

int
parse_i2s(char *optarg)
{
	int param[4];
	int ret;
	int i;

	ret = sscanf(optarg, "%d,%d,%d,%d", &param[0], &param[1], &param[2],
		&param[3]);

	if (ret != 4) {
		return 1;
	}

	i2s = 1;

	for (i = 0; i < 4; i++) {
		hci_write_i2spcm_interface_param[4 + i] = param[i];
	}

	return 0;
}

int
parse_no2bytes(void)
{
	no2bytes = 1;
	return 0;
}

int
parse_tosleep(char *optarg)
{
	tosleep = atoi(optarg);

	if (tosleep <= 0) {
		return 1;
	}

	return 0;
}

void
usage(char *argv0)
{
	printf("Usage %s:\n", argv0);
	printf("\t<-d> to print a debug log\n");
	printf("\t<--patchram patchram_file>\n");
	printf("\t<--baudrate baud_rate>\n");
	printf("\t<--bd_addr bd_address>\n");
	printf("\t<--enable_lpm>\n");
	printf("\t<--enable_h4 |--enable_h5>\n");
	printf("\t<--use_baudrate_for_download> - Uses the\n");
	printf("\t\tbaudrate for downloading the firmware\n");
	printf("\t<--scopcm=sco_routing,pcm_interface_rate,frame_type,\n");
	printf("\t\tsync_mode,clock_mode,lsb_first,fill_bits,\n");
	printf("\t\tfill_method,fill_num,right_justify>\n");
	printf("\n\t\tWhere\n");
	printf("\n\t\tsco_routing is 0 for PCM, 1 for Transport,\n");
	printf("\t\t2 for Codec and 3 for I2S,\n");
	printf("\n\t\tpcm_interface_rate is 0 for 128KBps, 1 for\n");
	printf("\t\t256 KBps, 2 for 512KBps, 3 for 1024KBps,\n");
	printf("\t\tand 4 for 2048Kbps,\n");
	printf("\n\t\tframe_type is 0 for short and 1 for long,\n");
	printf("\t\tsync_mode is 0 for slave and 1 for master,\n");
	printf("\n\t\tclock_mode is 0 for slabe and 1 for master,\n");
	printf("\n\t\tlsb_first is 0 for false aand 1 for true,\n");
	printf("\n\t\tfill_bits is the value in decimal for unused bits,\n");
	printf("\n\t\tfill_method is 0 for 0's and 1 for 1's, 2 for\n");
	printf("\t\tsigned and 3 for programmable,\n");
	printf("\n\t\tfill_num is the number or bits to fill,\n");
	printf("\n\t\tright_justify is 0 for false and 1 for true\n");
	printf("\n\t<--i2s=i2s_enable,is_master,sample_rate,clock_rate>\n");
	printf("\n\t\tWhere\n");
	printf("\n\t\ti2s_enable is 0 for disable and 1 for enable,\n");
	printf("\n\t\tis_master is 0 for slave and 1 for master,\n");
	printf("\n\t\tsample_rate is 0 for 8KHz, 1 for 16Khz and\n");
	printf("\t\t2 for 4 KHz,\n");
	printf("\n\t\tclock_rate is 0 for 128KHz, 1 for 256KHz, 3 for\n");
	printf("\t\t1024 KHz and 4 for 2048 KHz.\n\n");
	printf("\t<--no2bytes skips waiting for two byte confirmation\n");
	printf("\t\tbefore starting patchram download. Newer chips\n");
	printf("\t\tdo not generate these two bytes.>\n");
	printf("\t<--tosleep=microseconds>\n");
	printf("\tuart_device_name\n");
}

int
parse_cmd_line(int argc, char **argv)
{
	int c;
	int ret = 0;

	typedef int (*PFI)();

	PFI parse[] = { parse_patchram, parse_baudrate,
		parse_bdaddr, parse_enable_lpm, parse_enable_h4,
		parse_enable_h5, parse_use_baudrate_for_download,
		parse_scopcm, parse_i2s, parse_no2bytes, parse_tosleep};


	while (1) {
		int option_index = 0;

		static struct option long_options[] = {
			{"patchram", 1, 0, 0},
			{"baudrate", 1, 0, 0},
			{"bd_addr", 1, 0, 0},
			{"enable_lpm", 0, 0, 0},
			{"enable_h4", 0, 0, 0},
			{"enable_h5", 0, 0, 0},
			{"use_baudrate_for_download", 0, 0, 0},
			{"scopcm", 1, 0, 0},
			{"i2s", 1, 0, 0},
			{"no2bytes", 0, 0, 0},
			{"tosleep", 1, 0, 0},
			{0, 0, 0, 0}
		};

		c = getopt_long_only (argc, argv, "d", long_options,
				&option_index);

		if (c == -1) {
			break;
		}

		switch (c) {
			case 0:
				if (debug) {
					printf ("option %s",
						long_options[option_index].name);
					if (optarg)
						printf (" with arg %s", optarg);
					printf ("\n");
				}

				ret = parse[option_index](optarg);

				break;
			case 'd':
				debug = 1;
				break;

			case '?':
				// nobreak;
			default:
				usage(argv[0]);
				break;
		}

		if (ret) {
			usage(argv[0]);
			break;
		}
	}

	if (ret) {
		return 1;
	}

	if (optind < argc) {
		if (debug)
			printf ("%s \n", argv[optind]);
		if ((uart_fd = open(argv[optind], O_RDWR | O_NOCTTY)) == -1) {
			fprintf(stderr, "port %s could not be opened, error %d\n",
					argv[2], errno);
		}
	}

	return 0;
}

void
init_uart()
{
	tcflush(uart_fd, TCIOFLUSH);
	tcgetattr(uart_fd, &termios);

#ifndef __CYGWIN__
	cfmakeraw(&termios);
#else
	termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                | INLCR | IGNCR | ICRNL | IXON);
	termios.c_oflag &= ~OPOST;
	termios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	termios.c_cflag &= ~(CSIZE | PARENB);
	termios.c_cflag |= CS8;
#endif

	termios.c_cflag &= ~CRTSCTS;
	tcsetattr(uart_fd, TCSANOW, &termios);
	tcflush(uart_fd, TCIOFLUSH);
	tcsetattr(uart_fd, TCSANOW, &termios);
	tcflush(uart_fd, TCIOFLUSH);
	tcflush(uart_fd, TCIOFLUSH);
	cfsetospeed(&termios, B115200);
	cfsetispeed(&termios, B115200);
	tcsetattr(uart_fd, TCSANOW, &termios);
}

void
dump(uchar *out, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (i && !(i % 16)) {
			fprintf(stderr, "\n");
		}

		fprintf(stderr, "%02x ", out[i]);
	}

	fprintf(stderr, "\n");
}

void
read_event(int fd, uchar *buffer)
{
	int i = 0;
	int len = 3;
	int count;

	i = 0;
	len = 3;

	while ((count = read(fd, &buffer[i], len)) < len) {
		if (debug) {
			count += i;

			fprintf(stderr, "received %d len %d\n", count, len);
			dump(buffer, count);
		}

		i += count;
		len -= count;
	}

	i += count;
	len = buffer[2];

	while ((count = read(fd, &buffer[i], len)) < len) {
		i += count;
		len -= count;
	}

	if (debug) {
		count += i;

		fprintf(stderr, "received %d\n", count);
		dump(buffer, count);
	}
}

void
hci_send_cmd(uchar *buf, int len)
{
	if (debug) {
		fprintf(stderr, "writing\n");
		dump(buf, len);
	}

	write(uart_fd, buf, len);
}

void
expired(int sig __attribute__ ((unused)))
{
	hci_send_cmd(hci_reset, sizeof(hci_reset));
	alarm(4);
}

static void
slip_expired(int sig __attribute__ ((unused)))
{
	hci_send_cmd(slip_sync, sizeof(slip_sync));
	alarm(4);
}

static void
slip_config_expired(int sig __attribute ((unused)))
{
	hci_send_cmd(slip_config, sizeof(slip_config));
	alarm(4);
}

static void
proc_reset()
{
	signal(SIGALRM, expired);

	hci_send_cmd(hci_reset, sizeof(hci_reset));

	alarm(4);

	read_event(uart_fd, buffer);

	alarm(0);
}

static void
proc_patchram()
{
	int len;
	uchar chip_id;

	hci_send_cmd(hci_read_verbose_config_version_info, 
		sizeof(hci_read_verbose_config_version_info));

	read_event(uart_fd, buffer);

	chip_id = buffer[7];

	if (debug) {
		fprintf(stderr, "chip_id is %02x\n", chip_id);
	}

	if (chip_id == CHIP_ID_4330B2) {
		no2bytes = 1;
	}

	hci_send_cmd(hci_download_minidriver, sizeof(hci_download_minidriver));

	read_event(uart_fd, buffer);

	if (!no2bytes) {
		read(uart_fd, &buffer[0], 2);
	}

	if (tosleep) {
		usleep(tosleep);
	}

	while (read(hcdfile_fd, &buffer[1], 3)) {
		buffer[0] = 0x01;

		len = buffer[3];

		read(hcdfile_fd, &buffer[4], len);

		hci_send_cmd(buffer, len + 4);

		read_event(uart_fd, buffer);
	}

	if (use_baudrate_for_download) {
		cfsetospeed(&termios, B115200);
		cfsetispeed(&termios, B115200);
		tcsetattr(uart_fd, TCSANOW, &termios);
	}

	proc_reset();
}

static void
proc_baudrate()
{
	hci_send_cmd(hci_update_baud_rate, sizeof(hci_update_baud_rate));

	read_event(uart_fd, buffer);

	cfsetospeed(&termios, termios_baudrate);
	cfsetispeed(&termios, termios_baudrate);
	tcsetattr(uart_fd, TCSANOW, &termios);

	if (debug) {
		fprintf(stderr, "Done setting baudrate\n");
	}

	sleep(1);
}

static void
proc_bdaddr()
{
	hci_send_cmd(hci_write_bd_addr, sizeof(hci_write_bd_addr));

	read_event(uart_fd, buffer);
}

static void
proc_enable_lpm()
{
	hci_send_cmd(hci_write_sleep_mode, sizeof(hci_write_sleep_mode));

	read_event(uart_fd, buffer);
}

static void
proc_scopcm()
{
	hci_send_cmd(hci_write_sco_pcm_int,
		sizeof(hci_write_sco_pcm_int));

	read_event(uart_fd, buffer);

	hci_send_cmd(hci_write_pcm_data_format,
		sizeof(hci_write_pcm_data_format));

	read_event(uart_fd, buffer);
}

static void
proc_i2s()
{
	hci_send_cmd(hci_write_i2spcm_interface_param,
		sizeof(hci_write_i2spcm_interface_param));

	read_event(uart_fd, buffer);
}

static void
proc_enable_hci()
{
	int i = N_HCI;
	int proto = (enable_h4 ? HCI_UART_H4 : HCI_UART_H5);

	if (ioctl(uart_fd, TIOCSETD, &i) < 0) {
		fprintf(stderr, "Can't set line discipline\n");
		return;
	}

	if (ioctl(uart_fd, HCIUARTSETPROTO, proto) < 0) {
		fprintf(stderr, "Can't set hci protocol\n");
		return;
	}

	if (debug) {
		fprintf(stderr, "Done setting line discpline\n");
	}

	return;
}

#ifdef ANDROID
void
read_default_bdaddr()
{
	int sz;
	int fd;

	char path[PROPERTY_VALUE_MAX];

	char bdaddr[18];
	int len = 17;
	memset(bdaddr, 0, (len + 1) * sizeof(char));

	property_get("ro.bt.bdaddr_path", path, "");

	if (path[0] == 0)
		return;

	fd = open(path, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s (%d)", path, strerror(errno),
				errno);
		return;
	}

	sz = read(fd, bdaddr, len);

	if (sz < 0) {
		fprintf(stderr, "read(%s) failed: %s (%d)", path, strerror(errno),
				errno);
		close(fd);
		return;
	} else if (sz != len) {
		fprintf(stderr, "read(%s) unexpected size %d", path, sz);
		close(fd);
		return;
	}

	if (debug) {
		printf("Read default bdaddr of %s\n", bdaddr);
	}

	parse_bdaddr(bdaddr);
}
#endif

static int
proc_slip_sync()
{
	int count;
	int ret = 0;

	signal(SIGALRM, slip_expired);

	hci_send_cmd(slip_sync, sizeof(slip_sync));

	alarm(4);

	while (!ret) {
		count = read(uart_fd, buffer, sizeof(slip_sync));

		if (debug) {
			fprintf(stderr, "received slip sync %d\n", count);
			dump(buffer, count);
		}

		if (buffer[6] == 0x7d) {
			alarm(0);
			ret = 1;
		} else { 
			hci_send_cmd(slip_sync_response, sizeof(slip_sync_response));
		}
	}

	return ret;
}

static int
proc_slip_config()
{
	int count;
	int ret = 0;

	signal(SIGALRM, slip_config_expired);

	hci_send_cmd(slip_config, sizeof(slip_config));

	alarm(4);

	while (!ret) {
		count = read(uart_fd, buffer, sizeof(slip_config_response));

		if (debug) {
			fprintf(stderr, "received slip config %d\n", count);
			dump(buffer, count);
		}

		if (count == 8) {
			if (buffer[5] == 0x03 && buffer[6] == 0xfc) {
				hci_send_cmd(slip_config_null_response, 
					sizeof(slip_config_null_response));
			} else {
				hci_send_cmd(slip_sync_response, sizeof(slip_sync_response));
			}
		} else if (buffer[7] == 0x7b) {
			alarm(0);
			ret = 1;
		} else { 
			hci_send_cmd(slip_config_response, sizeof(slip_config_response));
		}
	}

	return(ret);
}

int
main (int argc, char **argv)
{
#ifdef ANDROID
	read_default_bdaddr();
#endif

	if (parse_cmd_line(argc, argv)) {
		exit(1);
	}

	if (uart_fd < 0) {
		exit(2);
	}

	init_uart();

	proc_reset();

	if (use_baudrate_for_download) {
		if (termios_baudrate) {
			proc_baudrate();
		}
	}

	if (hcdfile_fd > 0) {
		proc_patchram();
	}

	if (termios_baudrate) {
		proc_baudrate();
	}

	if (bdaddr_flag) {
		proc_bdaddr();
	}

	if (enable_lpm) {
		proc_enable_lpm();
	}

	if (scopcm) {
		proc_scopcm();
	}

	if (i2s) {
		proc_i2s();
	}

	if (enable_h5) {
		time_t t;

		time(&t);
		fprintf(stderr, "start %s\n", ctime(&t));
	
		while (!proc_slip_sync()) {
			sleep(4);
		}

		proc_slip_config();

		time(&t);
		fprintf(stderr, "end %s\n", ctime(&t));
	}

	if (enable_h4 && enable_h5) {
		fprintf(stderr, "Both H4 and H5 cannot be enabled at the same time\n");
		exit(1);
	}

	if (enable_h4 || enable_h5) {

		if (enable_h5) {
			// turn XON/XOFF back on
			tcgetattr(uart_fd, &termios);
			termios.c_iflag |= (IXON | IXOFF);
			termios.c_lflag |= ICANON;
			tcsetattr(uart_fd, TCSANOW, &termios);
		}

		proc_enable_hci();

		while (1) {
			sleep(UINT_MAX);
		}
	}

	exit(0);
}
