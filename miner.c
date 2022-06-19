/*
 * Copyright 2011-2014 Con Kolivas
 * Copyright 2011-2017 Luke Dashjr
 * Copyright 2014 Nate Woolls
 * Copyright 2012-2014 Andrew Smith
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#ifdef HAVE_CURSES
#ifdef USE_UNICODE
#define PDC_WIDE
#endif
// Must be before stdbool, since pdcurses typedefs bool :/
#include <curses.h>
#endif

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <assert.h>
#include <signal.h>
#include <wctype.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifndef WIN32
#include <sys/resource.h>
#include <sys/socket.h>
#if defined(HAVE_LIBUDEV) && defined(HAVE_SYS_EPOLL_H)
#include <libudev.h>
#include <sys/epoll.h>
#define HAVE_BFG_HOTPLUG
#endif
#else
#include <winsock2.h>
#include <windows.h>
#include <dbt.h>
#define HAVE_BFG_HOTPLUG
#endif
#include <ccan/opt/opt.h>
#include <jansson.h>
#include <curl/curl.h>
#include <libgen.h>
#include <sha2.h>
#include <utlist.h>

#include <blkmaker.h>
#include <blkmaker_jansson.h>
#include <blktemplate.h>
#include <libbase58.h>

#include "compat.h"
#include "deviceapi.h"
#include "logging.h"
#include "miner.h"
#include "adl.h"
#include "driver-cpu.h"
#include "driver-opencl.h"
#include "util.h"

#ifdef USE_AVALON
#include "driver-avalon.h"
#endif

#ifdef HAVE_BFG_LOWLEVEL
#include "lowlevel.h"
#endif

#if defined(unix) || defined(__APPLE__)
	#include <errno.h>
	#include <fcntl.h>
	#include <sys/wait.h>
#endif

#ifdef USE_SCRYPT
#include "malgo/scrypt.h"
#endif

#if defined(USE_AVALON) || defined(USE_BITFORCE) || defined(USE_ICARUS) || defined(USE_MODMINER) || defined(USE_NANOFURY) || defined(USE_X6500) || defined(USE_ZTEX)
#	define USE_FPGA
#endif

enum bfg_quit_summary {
	BQS_DEFAULT,
	BQS_NONE,
	BQS_DEVS,
	BQS_PROCS,
	BQS_DETAILED,
};

struct strategies strategies[] = {
	{ "Failover" },
	{ "Round Robin" },
	{ "Rotate" },
	{ "Load Balance" },
	{ "Balance" },
};

#define packagename bfgminer_name_space_ver

bool opt_protocol;
bool opt_dev_protocol;
static bool opt_benchmark, opt_benchmark_intense;
static bool want_longpoll = true;
static bool want_gbt = true;
static bool want_getwork = true;
#if BLKMAKER_VERSION > 1
static bool opt_load_bitcoin_conf = true;
static uint32_t coinbase_script_block_id;
static uint32_t template_nonce;
#endif
#if BLKMAKER_VERSION > 0
char *opt_coinbase_sig;
#endif
static enum bfg_quit_summary opt_quit_summary = BQS_DEFAULT;
static bool include_serial_in_statline;
char *request_target_str;
float request_pdiff = 1.0;
double request_bdiff;
static bool want_stratum = true;
int opt_skip_checks;
bool want_per_device_stats;
bool use_syslog;
bool opt_quiet_work_updates = true;
bool opt_quiet;
bool opt_realquiet;
int loginput_size;
bool opt_compact;
bool opt_show_procs;
const int opt_cutofftemp = 95;
int opt_hysteresis = 3;
static int opt_retries = -1;
int opt_fail_pause = 5;
int opt_log_interval = 20;
int opt_queue = 1;
int opt_scantime = 60;
int opt_expiry = 120;
int opt_expiry_lp = 3600;
unsigned long long global_hashrate;
static bool opt_unittest = false;
unsigned unittest_failures;
unsigned long global_quota_gcd = 1;
time_t last_getwork;

#ifdef USE_OPENCL
int opt_dynamic_interval = 7;
int nDevs;
int opt_g_threads = -1;
#endif
#ifdef USE_SCRYPT
static char detect_algo = 1;
#else
static char detect_algo;
#endif
bool opt_restart = true;

#ifdef USE_LIBMICROHTTPD
#include "httpsrv.h"
int httpsrv_port = -1;
#endif
#ifdef USE_LIBEVENT
long stratumsrv_port = -1;
#endif

const
int rescan_delay_ms = 1000;
#ifdef HAVE_BFG_HOTPLUG
bool opt_hotplug = 1;
const
int hotplug_delay_ms = 100;
#else
const bool opt_hotplug;
#endif
struct string_elist *scan_devices;
static struct string_elist *opt_set_device_list;
bool opt_force_dev_init;
static struct string_elist *opt_devices_enabled_list;
static bool opt_display_devs;
int total_devices;
struct cgpu_info **devices;
int total_devices_new;
struct cgpu_info **devices_new;
bool have_opencl;
int opt_n_threads = -1;
int mining_threads;
int base_queue;
int num_processors;
#ifdef HAVE_CURSES
bool use_curses = true;
#else
bool use_curses;
#endif
int last_logstatusline_len;
#ifdef HAVE_LIBUSB
bool have_libusb;
#endif
static bool opt_submit_stale = true;
static float opt_shares;
static int opt_submit_threads = 0x40;
bool opt_fail_only;
int opt_fail_switch_delay = 300;
bool opt_autofan;
bool opt_autoengine;
bool opt_noadl;
char *opt_api_allow = NULL;
char *opt_api_groups;
char *opt_api_description = PACKAGE_STRING;
int opt_api_port = 4028;
bool opt_api_listen;
bool opt_api_mcast;
char *opt_api_mcast_addr = API_MCAST_ADDR;
char *opt_api_mcast_code = API_MCAST_CODE;
char *opt_api_mcast_des = "";
int opt_api_mcast_port = 4028;
bool opt_api_network;
bool opt_delaynet;
bool opt_disable_pool;
bool opt_disable_client_reconnect = false;
static bool no_work;
bool opt_worktime;
bool opt_weighed_stats;

char *opt_kernel_path;
char *cgminer_path;

#if defined(USE_BITFORCE)
bool opt_bfl_noncerange;
#endif
#define QUIET	(opt_quiet || opt_realquiet)

struct thr_info *control_thr;
struct thr_info **mining_thr;
static int watchpool_thr_id;
static int watchdog_thr_id;
#ifdef HAVE_CURSES
static int input_thr_id;
#endif
int gpur_thr_id;
static int api_thr_id;
static int total_control_threads;

pthread_mutex_t hash_lock;
static pthread_mutex_t *stgd_lock;
pthread_mutex_t console_lock;
cglock_t ch_lock;
static pthread_rwlock_t blk_lock;
static pthread_mutex_t sshare_lock;

pthread_rwlock_t netacc_lock;
pthread_rwlock_t mining_thr_lock;
pthread_rwlock_t devices_lock;

static pthread_mutex_t lp_lock;
static pthread_cond_t lp_cond;

pthread_cond_t gws_cond;

bool shutting_down;

double total_rolling;
double total_mhashes_done;
static struct timeval total_tv_start, total_tv_end;
static struct timeval miner_started;

cglock_t control_lock;
pthread_mutex_t stats_lock;

static pthread_mutex_t submitting_lock;
static int total_submitting;
static struct work *submit_waiting;
notifier_t submit_waiting_notifier;

int hw_errors;
int total_accepted, total_rejected;
int total_getworks, total_stale, total_discarded;
uint64_t total_bytes_rcvd, total_bytes_sent;
double total_diff1, total_bad_diff1;
double total_diff_accepted, total_diff_rejected, total_diff_stale;
static int staged_rollable, staged_spare;
unsigned int new_blocks;
unsigned int found_blocks;

unsigned int local_work;
unsigned int total_go, total_ro;

struct pool **pools;
static struct pool *currentpool = NULL;

int total_pools, enabled_pools;
enum pool_strategy pool_strategy = POOL_FAILOVER;
int opt_rotate_period;
static int total_urls, total_users, total_passes;

static
#ifndef HAVE_CURSES
const
#endif
bool curses_active;

#ifdef HAVE_CURSES
#if !(defined(PDCURSES) || defined(NCURSES_VERSION))
const
#endif
short default_bgcolor = COLOR_BLACK;
static int attr_title = A_BOLD;
#endif

static
#if defined(HAVE_CURSES) && defined(USE_UNICODE)
bool use_unicode;
static
bool have_unicode_degrees;
static
wchar_t unicode_micro = 'u';
#else
const bool use_unicode;
static
const bool have_unicode_degrees;
#ifdef HAVE_CURSES
static
const char unicode_micro = 'u';
#endif
#endif

#ifdef HAVE_CURSES
#define U8_BAD_START "\xef\x80\x81"
#define U8_BAD_END   "\xef\x80\x80"
#define AS_BAD(x) U8_BAD_START x U8_BAD_END

/* logstart is where the log window should start */
static int devcursor, logstart, logcursor;

bool selecting_device;
unsigned selected_device;
#endif

static int max_lpdigits;

// current_hash was replaced with goal->current_goal_detail
// current_block_id was replaced with blkchain->currentblk->block_id

static char datestamp[40];
static char best_share[ALLOC_H2B_SHORTV] = "0";
double best_diff = 0;

struct mining_algorithm *mining_algorithms;
struct mining_goal_info *mining_goals;
int active_goals = 1;


int swork_id;

/* For creating a hash database of stratum shares submitted that have not had
 * a response yet */
struct stratum_share {
	UT_hash_handle hh;
	bool block;
	struct work *work;
	int id;
};

static struct stratum_share *stratum_shares = NULL;

char *opt_socks_proxy = NULL;

static const char def_conf[] = "bfgminer.conf";
static bool config_loaded;
static int include_count;
#define JSON_INCLUDE_CONF "include"
#define JSON_LOAD_ERROR "JSON decode of file '%s' failed\n %s"
#define JSON_LOAD_ERROR_LEN strlen(JSON_LOAD_ERROR)
#define JSON_MAX_DEPTH 10
#define JSON_MAX_DEPTH_ERR "Too many levels of JSON includes (limit 10) or a loop"
#define JSON_WEB_ERROR "WEB config err"

char *cmd_idle, *cmd_sick, *cmd_dead;

#if defined(unix) || defined(__APPLE__)
	static char *opt_stderr_cmd = NULL;
	static int forkpid;
#endif // defined(unix)

#ifdef HAVE_CHROOT
char *chroot_dir;
#endif

#ifdef HAVE_PWD_H
char *opt_setuid;
#endif

struct sigaction termhandler, inthandler;

struct thread_q *getq;

static int total_work;
static bool staged_full;
struct work *staged_work = NULL;

struct schedtime {
	bool enable;
	struct tm tm;
};

struct schedtime schedstart;
struct schedtime schedstop;
bool sched_paused;

static bool time_before(struct tm *tm1, struct tm *tm2)
{
	if (tm1->tm_hour < tm2->tm_hour)
		return true;
	if (tm1->tm_hour == tm2->tm_hour && tm1->tm_min < tm2->tm_min)
		return true;
	return false;
}

static bool should_run(void)
{
	struct tm tm;
	time_t tt;
	bool within_range;

	if (!schedstart.enable && !schedstop.enable)
		return true;

	tt = time(NULL);
	localtime_r(&tt, &tm);

	// NOTE: This is delicately balanced so that should_run is always false if schedstart==schedstop
	if (time_before(&schedstop.tm, &schedstart.tm))
		within_range = (time_before(&tm, &schedstop.tm) || !time_before(&tm, &schedstart.tm));
	else
		within_range = (time_before(&tm, &schedstop.tm) && !time_before(&tm, &schedstart.tm));

	if (within_range && !schedstop.enable)
		/* This is a once off event with no stop time set */
		schedstart.enable = false;

	return within_range;
}

void get_datestamp(char *f, size_t fsiz, time_t tt)
{
	struct tm _tm;
	struct tm *tm = &_tm;
	
	if (tt == INVALID_TIMESTAMP)
		tt = time(NULL);

	localtime_r(&tt, tm);
	snprintf(f, fsiz, "[%d-%02d-%02d %02d:%02d:%02d]",
		tm->tm_year + 1900,
		tm->tm_mon + 1,
		tm->tm_mday,
		tm->tm_hour,
		tm->tm_min,
		tm->tm_sec);
}

static
void get_timestamp(char *f, size_t fsiz, time_t tt)
{
	struct tm _tm;
	struct tm *tm = &_tm;

	localtime_r(&tt, tm);
	snprintf(f, fsiz, "[%02d:%02d:%02d]",
		tm->tm_hour,
		tm->tm_min,
		tm->tm_sec);
}

static void applog_and_exit(const char *fmt, ...) FORMAT_SYNTAX_CHECK(printf, 1, 2);

static char exit_buf[512];

static void applog_and_exit(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(exit_buf, sizeof(exit_buf), fmt, ap);
	va_end(ap);
	_applog(LOG_ERR, exit_buf);
	exit(1);
}

static
float drv_min_nonce_diff(const struct device_drv * const drv, struct cgpu_info * const proc, const struct mining_algorithm * const malgo)
{
	if (drv->drv_min_nonce_diff)
		return drv->drv_min_nonce_diff(proc, malgo);
#ifdef USE_SHA256D
	return (malgo->algo == POW_SHA256D) ? 1. : -1.;
#else
	return (malgo->algo == POW_SHA512_256D) ? 1. : -1.;
	return -1.;
#endif
}

char *devpath_to_devid(const char *devpath)
{
#ifndef WIN32
	if (devpath[0] != '/')
		return NULL;
	struct stat my_stat;
	if (stat(devpath, &my_stat))
		return NULL;
	char *devs = malloc(6 + (sizeof(dev_t) * 2) + 1);
	memcpy(devs, "dev_t:", 6);
	bin2hex(&devs[6], &my_stat.st_rdev, sizeof(dev_t));
#else
	if (!strncmp(devpath, "\\\\.\\", 4))
		devpath += 4;
	if (strncasecmp(devpath, "COM", 3) || !devpath[3])
		return NULL;
	devpath += 3;
	char *p;
	strtol(devpath, &p, 10);
	if (p[0])
		return NULL;
	const int sz = (p - devpath);
	char *devs = malloc(4 + sz + 1);
	sprintf(devs, "com:%s", devpath);
#endif
	return devs;
}

static
bool devpaths_match(const char * const ap, const char * const bp)
{
	char * const a = devpath_to_devid(ap);
	if (!a)
		return false;
	char * const b = devpath_to_devid(bp);
	bool rv = false;
	if (b)
	{
		rv = !strcmp(a, b);
		free(b);
	}
	free(a);
	return rv;
}

static
int proc_letter_to_number(const char *s, const char ** const rem)
{
	int n = 0, c;
	for ( ; s[0]; ++s)
	{
		if (unlikely(n > INT_MAX / 26))
			break;
		c = tolower(s[0]) - 'a';
		if (unlikely(c < 0 || c > 25))
			break;
		if (unlikely(INT_MAX - c < n))
			break;
		n = (n * 26) + c;
	}
	*rem = s;
	return n;
}

static
bool cgpu_match(const char * const pattern, const struct cgpu_info * const cgpu)
{
	// all - matches anything
	// d0 - matches all processors of device 0
	// d0-3 - matches all processors of device 0, 1, 2, or 3
	// d0a - matches first processor of device 0
	// 0 - matches processor 0
	// 0-4 - matches processors 0, 1, 2, 3, or 4
	// ___ - matches all processors on all devices using driver/name ___
	// ___0 - matches all processors of 0th device using driver/name ___
	// ___0a - matches first processor of 0th device using driver/name ___
	// @* - matches device with serial or path *
	// @*@a - matches first processor of device with serial or path *
	// ___@* - matches device with serial or path * using driver/name ___
	if (!strcasecmp(pattern, "all"))
		return true;
	
	const struct device_drv * const drv = cgpu->drv;
	const char *p = pattern, *p2;
	size_t L;
	int n, i, c = -1;
	int n2;
	int proc_first = -1, proc_last = -1;
	struct cgpu_info *device;
	
	if (!(strncasecmp(drv->dname, p, (L = strlen(drv->dname)))
	   && strncasecmp(drv-> name, p, (L = strlen(drv-> name)))))
		// dname or name
		p = &pattern[L];
	else
	if (p[0] == 'd' && (isdigit(p[1]) || p[1] == '-'))
		// d#
		++p;
	else
	if (isdigit(p[0]) || p[0] == '@' || p[0] == '-')
		// # or @
		{}
	else
		return false;
	
	L = p - pattern;
	while (isspace(p[0]))
		++p;
	if (p[0] == '@')
	{
		// Serial/path
		const char * const ser = &p[1];
		for (p = ser; p[0] != '@' && p[0] != '\0'; ++p)
		{}
		p2 = (p[0] == '@') ? &p[1] : p;
		const size_t serlen = (p - ser);
		p = "";
		n = n2 = 0;
		const char * const devpath = cgpu->device_path ?: "";
		const char * const devser = cgpu->dev_serial ?: "";
		if ((!strncmp(devpath, ser, serlen)) && devpath[serlen] == '\0')
		{}  // Match
		else
		if ((!strncmp(devser, ser, serlen)) && devser[serlen] == '\0')
		{}  // Match
		else
		{
			char devpath2[serlen + 1];
			memcpy(devpath2, ser, serlen);
			devpath2[serlen] = '\0';
			if (!devpaths_match(devpath, ser))
				return false;
		}
	}
	else
	{
		if (isdigit(p[0]))
			n = strtol(p, (void*)&p2, 0);
		else
		{
			n = 0;
			p2 = p;
		}
		if (p2[0] == '-')
		{
			++p2;
			if (p2[0] && isdigit(p2[0]))
				n2 = strtol(p2, (void*)&p2, 0);
			else
				n2 = INT_MAX;
		}
		else
			n2 = n;
		if (p == pattern)
		{
			if (!p[0])
				return true;
			if (p2 && p2[0])
				goto invsyntax;
			for (i = n; i <= n2; ++i)
			{
				if (i >= total_devices)
					break;
				if (cgpu == devices[i])
					return true;
			}
			return false;
		}
	}
	
	if (p2[0])
	{
		proc_first = proc_letter_to_number(&p2[0], &p2);
		if (p2[0] == '-')
		{
			++p2;
			if (p2[0])
				proc_last = proc_letter_to_number(p2, &p2);
			else
				proc_last = INT_MAX;
		}
		else
			proc_last = proc_first;
		if (p2[0])
			goto invsyntax;
	}
	
	if (L > 1 || tolower(pattern[0]) != 'd' || !p[0])
	{
		if ((L == 3 && !strncasecmp(pattern, drv->name, 3)) ||
			(!L) ||
			(L == strlen(drv->dname) && !strncasecmp(pattern, drv->dname, L)))
			{}  // Matched name or dname
		else
			return false;
		if (p[0] && (cgpu->device_id < n || cgpu->device_id > n2))
			return false;
		if (proc_first != -1 && (cgpu->proc_id < proc_first || cgpu->proc_id > proc_last))
			return false;
		return true;
	}
	
	// d#
	
	c = -1;
	for (i = 0; ; ++i)
	{
		if (i == total_devices)
			return false;
		if (devices[i]->device != devices[i])
			continue;
		++c;
		if (c < n)
			continue;
		if (c > n2)
			break;
		
		for (device = devices[i]; device; device = device->next_proc)
		{
			if (proc_first != -1 && (device->proc_id < proc_first || device->proc_id > proc_last))
				continue;
			if (device == cgpu)
				return true;
		}
	}
	return false;

invsyntax:
	applog(LOG_WARNING, "%s: Invalid syntax: %s", __func__, pattern);
	return false;
}

#define TEST_CGPU_MATCH(pattern)  \
	if (!cgpu_match(pattern, &cgpu))  \
	{  \
		++unittest_failures;  \
		applog(LOG_ERR, "%s: Pattern \"%s\" should have matched!", __func__, pattern);  \
	}  \
// END TEST_CGPU_MATCH
#define TEST_CGPU_NOMATCH(pattern)  \
	if (cgpu_match(pattern, &cgpu))  \
	{  \
		++unittest_failures;  \
		applog(LOG_ERR, "%s: Pattern \"%s\" should NOT have matched!", __func__, pattern);  \
	}  \
// END TEST_CGPU_MATCH
static __maybe_unused
void test_cgpu_match()
{
	struct device_drv drv = {
		.dname = "test",
		.name = "TST",
	};
	struct cgpu_info cgpu = {
		.drv = &drv,
		.device = &cgpu,
		.device_id = 1,
		.proc_id = 1,
		.proc_repr = "TST 1b",
	}, cgpu0a = {
		.drv = &drv,
		.device = &cgpu0a,
		.device_id = 0,
		.proc_id = 0,
		.proc_repr = "TST 0a",
	}, cgpu1a = {
		.drv = &drv,
		.device = &cgpu0a,
		.device_id = 1,
		.proc_id = 0,
		.proc_repr = "TST 1a",
	};
	struct cgpu_info *devices_list[3] = {&cgpu0a, &cgpu1a, &cgpu,};
	devices = devices_list;
	total_devices = 3;
	TEST_CGPU_MATCH("all")
	TEST_CGPU_MATCH("d1")
	TEST_CGPU_NOMATCH("d2")
	TEST_CGPU_MATCH("d0-5")
	TEST_CGPU_NOMATCH("d0-0")
	TEST_CGPU_NOMATCH("d2-5")
	TEST_CGPU_MATCH("d-1")
	TEST_CGPU_MATCH("d1-")
	TEST_CGPU_NOMATCH("d-0")
	TEST_CGPU_NOMATCH("d2-")
	TEST_CGPU_MATCH("2")
	TEST_CGPU_NOMATCH("3")
	TEST_CGPU_MATCH("1-2")
	TEST_CGPU_MATCH("2-3")
	TEST_CGPU_NOMATCH("1-1")
	TEST_CGPU_NOMATCH("3-4")
	TEST_CGPU_MATCH("TST")
	TEST_CGPU_MATCH("test")
	TEST_CGPU_MATCH("tst")
	TEST_CGPU_MATCH("TEST")
	TEST_CGPU_NOMATCH("TSF")
	TEST_CGPU_NOMATCH("TS")
	TEST_CGPU_NOMATCH("TSTF")
	TEST_CGPU_MATCH("TST1")
	TEST_CGPU_MATCH("test1")
	TEST_CGPU_MATCH("TST0-1")
	TEST_CGPU_MATCH("TST 1")
	TEST_CGPU_MATCH("TST 1-2")
	TEST_CGPU_MATCH("TEST 1-2")
	TEST_CGPU_NOMATCH("TST2")
	TEST_CGPU_NOMATCH("TST2-3")
	TEST_CGPU_NOMATCH("TST0-0")
	TEST_CGPU_MATCH("TST1b")
	TEST_CGPU_MATCH("tst1b")
	TEST_CGPU_NOMATCH("TST1c")
	TEST_CGPU_NOMATCH("TST1bb")
	TEST_CGPU_MATCH("TST0-1b")
	TEST_CGPU_NOMATCH("TST0-1c")
	TEST_CGPU_MATCH("TST1a-d")
	TEST_CGPU_NOMATCH("TST1a-a")
	TEST_CGPU_NOMATCH("TST1-a")
	TEST_CGPU_NOMATCH("TST1c-z")
	TEST_CGPU_NOMATCH("TST1c-")
	TEST_CGPU_MATCH("@")
	TEST_CGPU_NOMATCH("@abc")
	TEST_CGPU_MATCH("@@b")
	TEST_CGPU_NOMATCH("@@c")
	TEST_CGPU_MATCH("TST@")
	TEST_CGPU_NOMATCH("TST@abc")
	TEST_CGPU_MATCH("TST@@b")
	TEST_CGPU_NOMATCH("TST@@c")
	TEST_CGPU_MATCH("TST@@b-f")
	TEST_CGPU_NOMATCH("TST@@c-f")
	TEST_CGPU_NOMATCH("TST@@-a")
	cgpu.device_path = "/dev/test";
	cgpu.dev_serial = "testy";
	TEST_CGPU_MATCH("TST@/dev/test")
	TEST_CGPU_MATCH("TST@testy")
	TEST_CGPU_NOMATCH("TST@")
	TEST_CGPU_NOMATCH("TST@/dev/test5@b")
	TEST_CGPU_NOMATCH("TST@testy3@b")
	TEST_CGPU_MATCH("TST@/dev/test@b")
	TEST_CGPU_MATCH("TST@testy@b")
	TEST_CGPU_NOMATCH("TST@/dev/test@c")
	TEST_CGPU_NOMATCH("TST@testy@c")
	cgpu.device_path = "usb:000:999";
	TEST_CGPU_MATCH("TST@usb:000:999")
	drv.dname = "test7";
	TEST_CGPU_MATCH("test7")
	TEST_CGPU_MATCH("TEST7")
	TEST_CGPU_NOMATCH("test&")
	TEST_CGPU_MATCH("test7 1-2")
	TEST_CGPU_MATCH("test7@testy@b")
}

static
int cgpu_search(const char * const pattern, const int first)
{
	int i;
	struct cgpu_info *cgpu;
	
#define CHECK_CGPU_SEARCH  do{      \
	cgpu = get_devices(i);          \
	if (cgpu_match(pattern, cgpu))  \
		return i;                   \
}while(0)
	for (i = first; i < total_devices; ++i)
		CHECK_CGPU_SEARCH;
	for (i = 0; i < first; ++i)
		CHECK_CGPU_SEARCH;
#undef CHECK_CGPU_SEARCH
	return -1;
}

static pthread_mutex_t sharelog_lock;
static FILE *sharelog_file = NULL;

struct thr_info *get_thread(int thr_id)
{
	struct thr_info *thr;

	rd_lock(&mining_thr_lock);
	thr = mining_thr[thr_id];
	rd_unlock(&mining_thr_lock);

	return thr;
}

static struct cgpu_info *get_thr_cgpu(int thr_id)
{
	struct thr_info *thr = get_thread(thr_id);

	return thr->cgpu;
}

struct cgpu_info *get_devices(int id)
{
	struct cgpu_info *cgpu;

	rd_lock(&devices_lock);
	cgpu = devices[id];
	rd_unlock(&devices_lock);

	return cgpu;
}

static pthread_mutex_t noncelog_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *noncelog_file = NULL;

static
void noncelog(const struct work * const work)
{
	const int thr_id = work->thr_id;
	const struct cgpu_info *proc = get_thr_cgpu(thr_id);
	char buf[0x200], hash[65], data[161], midstate[65];
	int rv;
	size_t ret;
	
	bin2hex(hash, work->hash, 32);
	bin2hex(data, work->data, 80);
	bin2hex(midstate, work->midstate, 32);
	
	// timestamp,proc,hash,data,midstate
	rv = snprintf(buf, sizeof(buf), "%lu,%s,%s,%s,%s\n",
	              (unsigned long)time(NULL), proc->proc_repr_ns,
	              hash, data, midstate);
	
	if (unlikely(rv < 1))
	{
		applog(LOG_ERR, "noncelog printf error");
		return;
	}
	
	mutex_lock(&noncelog_lock);
	ret = fwrite(buf, rv, 1, noncelog_file);
	fflush(noncelog_file);
	mutex_unlock(&noncelog_lock);
	
	if (ret != 1)
		applog(LOG_ERR, "noncelog fwrite error");
}

static void sharelog(const char*disposition, const struct work*work)
{
	char target[(sizeof(work->target) * 2) + 1];
	char hash[(sizeof(work->hash) * 2) + 1];
	char data[(sizeof(work->data) * 2) + 1];
	struct cgpu_info *cgpu;
	unsigned long int t;
	struct pool *pool;
	int thr_id, rv;
	char s[1024];
	size_t ret;

	if (!sharelog_file)
		return;

	thr_id = work->thr_id;
	cgpu = get_thr_cgpu(thr_id);
	pool = work->pool;
	t = work->ts_getwork + timer_elapsed(&work->tv_getwork, &work->tv_work_found);
	bin2hex(target, work->target, sizeof(work->target));
	bin2hex(hash, work->hash, sizeof(work->hash));
	bin2hex(data, work->data, sizeof(work->data));

	// timestamp,disposition,target,pool,dev,thr,sharehash,sharedata
	rv = snprintf(s, sizeof(s), "%lu,%s,%s,%s,%s,%u,%s,%s\n", t, disposition, target, pool->rpc_url, cgpu->proc_repr_ns, thr_id, hash, data);
	if (rv >= (int)(sizeof(s)))
		s[sizeof(s) - 1] = '\0';
	else if (rv < 0) {
		applog(LOG_ERR, "sharelog printf error");
		return;
	}

	mutex_lock(&sharelog_lock);
	ret = fwrite(s, rv, 1, sharelog_file);
	fflush(sharelog_file);
	mutex_unlock(&sharelog_lock);

	if (ret != 1)
		applog(LOG_ERR, "sharelog fwrite error");
}

#ifdef HAVE_CURSES
static void switch_logsize(void);
#endif

static void hotplug_trigger();

void goal_set_malgo(struct mining_goal_info * const goal, struct mining_algorithm * const malgo)
{
	if (goal->malgo == malgo)
		return;
	
	if (goal->malgo)
		--goal->malgo->goal_refs;
	if (malgo->goal_refs++)
		// First time using a new mining algorithm may means we need to add mining hardware to support it
		// api_thr_id is used as an ugly hack to determine if mining has started - if not, we do NOT want to try to hotplug anything (let the initial detect handle it)
		if (opt_hotplug && api_thr_id)
			hotplug_trigger();
	goal->malgo = malgo;
}

struct mining_algorithm *mining_algorithm_by_alias(const char * const alias)
{
	struct mining_algorithm *malgo;
	LL_FOREACH(mining_algorithms, malgo)
	{
		if (match_strtok(malgo->aliases, "|", alias))
			return malgo;
	}
	return NULL;
}

#ifdef USE_SCRYPT
extern struct mining_algorithm malgo_scrypt;

static
const char *set_malgo_scrypt()
{
	goal_set_malgo(get_mining_goal("default"), &malgo_scrypt);
	return NULL;
}
#endif

static
int mining_goals_name_cmp(const struct mining_goal_info * const a, const struct mining_goal_info * const b)
{
	// default always goes first
	if (a->is_default)
		return -1;
	if (b->is_default)
		return 1;
	return strcmp(a->name, b->name);
}

static
void blkchain_init_block(struct blockchain_info * const blkchain)
{
	struct block_info * const dummy_block = calloc(sizeof(*dummy_block), 1);
	memset(dummy_block->prevblkhash, 0, 0x20);
	HASH_ADD(hh, blkchain->blocks, prevblkhash, sizeof(dummy_block->prevblkhash), dummy_block);
	blkchain->currentblk = dummy_block;
}

extern struct mining_algorithm malgo_sha512_256d;

struct mining_goal_info *get_mining_goal(const char * const name)
{
	static unsigned next_goal_id;
	struct mining_goal_info *goal;
	HASH_FIND_STR(mining_goals, name, goal);
	if (!goal)
	{
		struct blockchain_info * const blkchain = malloc(sizeof(*blkchain) + sizeof(*goal));
		goal = (void*)(&blkchain[1]);
		
		*blkchain = (struct blockchain_info){
			.currentblk = NULL,
		};
		blkchain_init_block(blkchain);
		
		*goal = (struct mining_goal_info){
			.id = next_goal_id++,
			.name = strdup(name),
			.is_default = !strcmp(name, "default"),
			.blkchain = blkchain,
			.current_diff = 0xFFFFFFFFFFFFFFFFULL,
		};
		goal_set_malgo(goal, &malgo_sha512_256d);
		HASH_ADD_KEYPTR(hh, mining_goals, goal->name, strlen(goal->name), goal);
		HASH_SORT(mining_goals, mining_goals_name_cmp);
		
#ifdef HAVE_CURSES
		devcursor = 7 + active_goals;
		switch_logsize();
#endif
	}
	return goal;
}

void mining_goal_reset(struct mining_goal_info * const goal)
{
	struct blockchain_info * const blkchain = goal->blkchain;
	struct block_info *blkinfo, *tmpblkinfo;
	HASH_ITER(hh, blkchain->blocks, blkinfo, tmpblkinfo)
	{
		HASH_DEL(blkchain->blocks, blkinfo);
		free(blkinfo);
	}
	blkchain_init_block(blkchain);
}

static char *getwork_req = "{\"method\": \"getwork\", \"params\": [], \"id\":0}\n";

/* Adjust all the pools' quota to the greatest common denominator after a pool
 * has been added or the quotas changed. */
void adjust_quota_gcd(void)
{
	unsigned long gcd, lowest_quota = ~0UL, quota;
	struct pool *pool;
	int i;

	for (i = 0; i < total_pools; i++) {
		pool = pools[i];
		quota = pool->quota;
		if (!quota)
			continue;
		if (quota < lowest_quota)
			lowest_quota = quota;
	}

	if (likely(lowest_quota < ~0UL)) {
		gcd = lowest_quota;
		for (i = 0; i < total_pools; i++) {
			pool = pools[i];
			quota = pool->quota;
			if (!quota)
				continue;
			while (quota % gcd)
				gcd--;
		}
	} else
		gcd = 1;

	for (i = 0; i < total_pools; i++) {
		pool = pools[i];
		pool->quota_used *= global_quota_gcd;
		pool->quota_used /= gcd;
		pool->quota_gcd = pool->quota / gcd;
	}

	global_quota_gcd = gcd;
	applog(LOG_DEBUG, "Global quota greatest common denominator set to %lu", gcd);
}

/* Return value is ignored if not called from add_pool_details */
struct pool *add_pool2(struct mining_goal_info * const goal)
{
	struct pool *pool;

	pool = calloc(sizeof(struct pool), 1);
	if (!pool)
		quit(1, "Failed to malloc pool in add_pool");
	pool->pool_no = pool->prio = total_pools;
	mutex_init(&pool->last_work_lock);
	mutex_init(&pool->pool_lock);
	mutex_init(&pool->pool_test_lock);
	if (unlikely(pthread_cond_init(&pool->cr_cond, bfg_condattr)))
		quit(1, "Failed to pthread_cond_init in add_pool");
	cglock_init(&pool->data_lock);
	pool->swork.data_lock_p = &pool->data_lock;
	mutex_init(&pool->stratum_lock);
	timer_unset(&pool->swork.tv_transparency);
	pool->swork.pool = pool;
	pool->goal = goal;

	pool->idle = true;
	/* Make sure the pool doesn't think we've been idle since time 0 */
	pool->tv_idle.tv_sec = ~0UL;
	
	cgtime(&pool->cgminer_stats.start_tv);
	pool->cgminer_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;
	pool->cgminer_pool_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;

	pool->rpc_proxy = NULL;
	pool->quota = 1;

	pool->sock = INVSOCK;
	pool->lp_socket = CURL_SOCKET_BAD;

	pools = realloc(pools, sizeof(struct pool *) * (total_pools + 2));
	pools[total_pools++] = pool;
	
	if (opt_benchmark)
	{
		// Immediately remove it
		remove_pool(pool);
		return pool;
	}
	
	adjust_quota_gcd();
	
	if (!currentpool)
		currentpool = pool;
	
	enable_pool(pool);

	return pool;
}

static
void pool_set_uri(struct pool * const pool, char * const uri)
{
	pool->rpc_url = uri;
	pool->pool_diff_effective_retroactively = uri_get_param_bool2(uri, "retrodiff");
}

static
bool pool_diff_effective_retroactively(struct pool * const pool)
{
	if (pool->pool_diff_effective_retroactively != BTS_UNKNOWN) {
		return pool->pool_diff_effective_retroactively;
	}
	
	// By default, we enable retrodiff for stratum pools since some servers implement mining.set_difficulty in this way
	// Note that share_result will explicitly disable BTS_UNKNOWN -> BTS_FALSE if a retrodiff share is rejected specifically for its failure to meet the target.
	return pool->stratum_active;
}

/* Pool variant of test and set */
static bool pool_tset(struct pool *pool, bool *var)
{
	bool ret;

	mutex_lock(&pool->pool_lock);
	ret = *var;
	*var = true;
	mutex_unlock(&pool->pool_lock);

	return ret;
}

bool pool_tclear(struct pool *pool, bool *var)
{
	bool ret;

	mutex_lock(&pool->pool_lock);
	ret = *var;
	*var = false;
	mutex_unlock(&pool->pool_lock);

	return ret;
}

struct pool *current_pool(void)
{
	struct pool *pool;

	cg_rlock(&control_lock);
	pool = currentpool;
	cg_runlock(&control_lock);

	return pool;
}

#if defined(USE_CPUMINING) && !defined(USE_SHA256D)
static
char *arg_ignored(const char * const arg)
{
	return NULL;
}
#endif

static
char *set_bool_ignore_arg(const char * const arg, bool * const b)
{
	return opt_set_bool(b);
}

char *set_int_range(const char *arg, int *i, int min, int max)
{
	char *err = opt_set_intval(arg, i);

	if (err)
		return err;

	if (*i < min || *i > max)
		return "Value out of range";

	return NULL;
}

static char *set_int_0_to_9999(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 9999);
}

static char *set_int_1_to_65535(const char *arg, int *i)
{
	return set_int_range(arg, i, 1, 65535);
}

static char *set_int_0_to_10(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 10);
}

static char *set_int_1_to_10(const char *arg, int *i)
{
	return set_int_range(arg, i, 1, 10);
}

static char *set_long_1_to_65535_or_neg1(const char * const arg, long * const i)
{
	const long min = 1, max = 65535;
	
	char * const err = opt_set_longval(arg, i);
	
	if (err) {
		return err;
	}
	
	if (*i != -1 && (*i < min || *i > max)) {
		return "Value out of range";
	}
	
	return NULL;
}

char *set_strdup(const char *arg, char **p)
{
	*p = strdup((char *)arg);
	return NULL;
}

#if BLKMAKER_VERSION > 1
static
char *set_b58addr(const char * const arg, bytes_t * const b)
{
	size_t scriptsz = blkmk_address_to_script(NULL, 0, arg);
	if (!scriptsz)
		return "Invalid address";
	char *script = malloc(scriptsz);
	if (blkmk_address_to_script(script, scriptsz, arg) != scriptsz) {
		free(script);
		return "Failed to convert address to script";
	}
	bytes_assimilate_raw(b, script, scriptsz, scriptsz);
	return NULL;
}

static char *set_generate_addr2(struct mining_goal_info *, const char *);

static
char *set_generate_addr(char *arg)
{
	char * const colon = strchr(arg, ':');
	struct mining_goal_info *goal;
	if (colon)
	{
		colon[0] = '\0';
		goal = get_mining_goal(arg);
		arg = &colon[1];
	}
	else
		goal = get_mining_goal("default");
	
	return set_generate_addr2(goal, arg);
}

static
char *set_generate_addr2(struct mining_goal_info * const goal, const char * const arg)
{
	bytes_t newscript = BYTES_INIT;
	char *estr = set_b58addr(arg, &newscript);
	if (estr)
	{
		bytes_free(&newscript);
		return estr;
	}
	if (!goal->generation_script)
	{
		goal->generation_script = malloc(sizeof(*goal->generation_script));
		bytes_init(goal->generation_script);
	}
	bytes_assimilate(goal->generation_script, &newscript);
	bytes_free(&newscript);
	
	return NULL;
}
#endif

static
char *set_quit_summary(const char * const arg)
{
	if (!(strcasecmp(arg, "none") && strcasecmp(arg, "no")))
		opt_quit_summary = BQS_NONE;
	else
	if (!(strcasecmp(arg, "devs") && strcasecmp(arg, "devices")))
		opt_quit_summary = BQS_DEVS;
	else
	if (!(strcasecmp(arg, "procs") && strcasecmp(arg, "processors") && strcasecmp(arg, "chips") && strcasecmp(arg, "cores")))
		opt_quit_summary = BQS_PROCS;
	else
	if (!(strcasecmp(arg, "detailed") && strcasecmp(arg, "detail") && strcasecmp(arg, "all")))
		opt_quit_summary = BQS_DETAILED;
	else
		return "Quit summary must be one of none/devs/procs/detailed";
	return NULL;
}

static void pdiff_target_leadzero(void *, double);

char *set_request_diff(const char *arg, float *p)
{
	unsigned char target[32];
	char *e = opt_set_floatval(arg, p);
	if (e)
		return e;
	
	request_bdiff = (double)*p * 0.9999847412109375;
	pdiff_target_leadzero(target, *p);
	request_target_str = malloc(65);
	bin2hex(request_target_str, target, 32);
	
	return NULL;
}

#ifdef NEED_BFG_LOWL_VCOM
extern struct lowlevel_device_info *_vcom_devinfo_findorcreate(struct lowlevel_device_info **, const char *);

#ifdef WIN32
void _vcom_devinfo_scan_querydosdevice(struct lowlevel_device_info ** const devinfo_list)
{
	char dev[PATH_MAX];
	char *devp = dev;
	size_t bufLen = 0x100;
tryagain: ;
	char buf[bufLen];
	if (!QueryDosDevice(NULL, buf, bufLen)) {
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
			bufLen *= 2;
			applog(LOG_DEBUG, "QueryDosDevice returned insufficent buffer error; enlarging to %lx", (unsigned long)bufLen);
			goto tryagain;
		}
		applogr(, LOG_WARNING, "Error occurred trying to enumerate COM ports with QueryDosDevice");
	}
	size_t tLen;
	memcpy(devp, "\\\\.\\", 4);
	devp = &devp[4];
	for (char *t = buf; *t; t += tLen) {
		tLen = strlen(t) + 1;
		if (strncmp("COM", t, 3))
			continue;
		memcpy(devp, t, tLen);
		// NOTE: We depend on _vcom_devinfo_findorcreate to further check that there's a number (and only a number) on the end
		_vcom_devinfo_findorcreate(devinfo_list, dev);
	}
}
#else
void _vcom_devinfo_scan_lsdev(struct lowlevel_device_info ** const devinfo_list)
{
	char dev[PATH_MAX];
	char *devp = dev;
	DIR *D;
	struct dirent *de;
	const char devdir[] = "/dev";
	const size_t devdirlen = sizeof(devdir) - 1;
	char *devpath = devp;
	char *devfile = devpath + devdirlen + 1;
	
	D = opendir(devdir);
	if (!D)
		applogr(, LOG_DEBUG, "No /dev directory to look for VCOM devices in");
	memcpy(devpath, devdir, devdirlen);
	devpath[devdirlen] = '/';
	while ( (de = readdir(D)) ) {
		if (!strncmp(de->d_name, "cu.", 3)
			//don't probe Bluetooth devices - causes bus errors and segfaults
			&& strncmp(de->d_name, "cu.Bluetooth", 12))
			goto trydev;
		if (strncmp(de->d_name, "tty", 3))
			continue;
		if (strncmp(&de->d_name[3], "USB", 3) && strncmp(&de->d_name[3], "ACM", 3))
			continue;
		
trydev:
		strcpy(devfile, de->d_name);
		_vcom_devinfo_findorcreate(devinfo_list, dev);
	}
	closedir(D);
}
#endif
#endif

static char *add_serial(const char *arg)
{
	string_elist_add(arg, &scan_devices);
	return NULL;
}

static
char *opt_string_elist_add(const char *arg, struct string_elist **elist)
{
	string_elist_add(arg, elist);
	return NULL;
}

bool get_intrange(const char *arg, int *val1, int *val2)
{
	// NOTE: This could be done with sscanf, but its %n is broken in strange ways on Windows
	char *p, *p2;
	
	*val1 = strtol(arg, &p, 0);
	if (arg == p)
		// Zero-length ending number, invalid
		return false;
	while (true)
	{
		if (!p[0])
		{
			*val2 = *val1;
			return true;
		}
		if (p[0] == '-')
			break;
		if (!isspace(p[0]))
			// Garbage, invalid
			return false;
		++p;
	}
	p2 = &p[1];
	*val2 = strtol(p2, &p, 0);
	if (p2 == p)
		// Zero-length ending number, invalid
		return false;
	while (true)
	{
		if (!p[0])
			return true;
		if (!isspace(p[0]))
			// Garbage, invalid
			return false;
		++p;
	}
}

static
void _test_intrange(const char *s, const int v[2])
{
	int a[2];
	if (!get_intrange(s, &a[0], &a[1]))
	{
		++unittest_failures;
		applog(LOG_ERR, "Test \"%s\" failed: returned false", s);
	}
	for (int i = 0; i < 2; ++i)
		if (unlikely(a[i] != v[i]))
		{
			++unittest_failures;
			applog(LOG_ERR, "Test \"%s\" failed: value %d should be %d but got %d", s, i, v[i], a[i]);
		}
}
#define _test_intrange(s, ...)  _test_intrange(s, (int[]){ __VA_ARGS__ })

static
void _test_intrange_fail(const char *s)
{
	int a[2];
	if (get_intrange(s, &a[0], &a[1]))
	{
		++unittest_failures;
		applog(LOG_ERR, "Test !\"%s\" failed: returned true with %d and %d", s, a[0], a[1]);
	}
}

static
void test_intrange()
{
	_test_intrange("-1--2", -1, -2);
	_test_intrange("-1-2", -1, 2);
	_test_intrange("1--2", 1, -2);
	_test_intrange("1-2", 1, 2);
	_test_intrange("111-222", 111, 222);
	_test_intrange(" 11 - 22 ", 11, 22);
	_test_intrange("+11-+22", 11, 22);
	_test_intrange("-1", -1, -1);
	_test_intrange_fail("all");
	_test_intrange_fail("1-");
	_test_intrange_fail("");
	_test_intrange_fail("1-54x");
}

static char *set_devices(char *arg)
{
	if (*arg) {
		if (*arg == '?') {
			opt_display_devs = true;
			return NULL;
		}
	} else
		return "Invalid device parameters";

	string_elist_add(arg, &opt_devices_enabled_list);

	return NULL;
}

static char *set_balance(enum pool_strategy *strategy)
{
	*strategy = POOL_BALANCE;
	return NULL;
}

static char *set_loadbalance(enum pool_strategy *strategy)
{
	*strategy = POOL_LOADBALANCE;
	return NULL;
}

static char *set_rotate(const char *arg, int *i)
{
	pool_strategy = POOL_ROTATE;
	return set_int_range(arg, i, 0, 9999);
}

static char *set_rr(enum pool_strategy *strategy)
{
	*strategy = POOL_ROUNDROBIN;
	return NULL;
}

static
char *set_benchmark_intense()
{
	opt_benchmark = true;
	opt_benchmark_intense = true;
	return NULL;
}

/* Detect that url is for a stratum protocol either via the presence of
 * stratum+tcp or by detecting a stratum server response */
bool detect_stratum(struct pool *pool, char *url)
{
	if (!extract_sockaddr(url, &pool->sockaddr_url, &pool->stratum_port))
		return false;

	if (!strncasecmp(url, "stratum+tcp://", 14)) {
		pool_set_uri(pool, strdup(url));
		pool->has_stratum = true;
		pool->stratum_url = pool->sockaddr_url;
		return true;
	}

	return false;
}

static struct pool *add_url(void)
{
	total_urls++;
	if (total_urls > total_pools)
		add_pool();
	return pools[total_urls - 1];
}

static void setup_url(struct pool *pool, char *arg)
{
	if (detect_stratum(pool, arg))
		return;

	opt_set_charp(arg, &pool->rpc_url);
	if (strncmp(arg, "http://", 7) &&
	    strncmp(arg, "https://", 8)) {
		const size_t L = strlen(arg);
		char *httpinput;

		httpinput = malloc(8 + L);
		if (!httpinput)
			quit(1, "Failed to malloc httpinput");
		sprintf(httpinput, "http://%s", arg);
		pool_set_uri(pool, httpinput);
	}
}

static char *set_url(char *arg)
{
	struct pool *pool = add_url();

	setup_url(pool, arg);
	return NULL;
}

static char *set_quota(char *arg)
{
	char *semicolon = strchr(arg, ';'), *url;
	int len, qlen, quota;
	struct pool *pool;

	if (!semicolon)
		return "No semicolon separated quota;URL pair found";
	len = strlen(arg);
	*semicolon = '\0';
	qlen = strlen(arg);
	if (!qlen)
		return "No parameter for quota found";
	len -= qlen + 1;
	if (len < 1)
		return "No parameter for URL found";
	quota = atoi(arg);
	if (quota < 0)
		return "Invalid negative parameter for quota set";
	url = arg + qlen + 1;
	pool = add_url();
	setup_url(pool, url);
	pool->quota = quota;
	applog(LOG_INFO, "Setting pool %d to quota %d", pool->pool_no, pool->quota);
	adjust_quota_gcd();

	return NULL;
}

static char *set_user(const char *arg)
{
	struct pool *pool;

	total_users++;
	if (total_users > total_pools)
		add_pool();

	pool = pools[total_users - 1];
	opt_set_charp(arg, &pool->rpc_user);

	return NULL;
}

static char *set_pass(const char *arg)
{
	struct pool *pool;

	total_passes++;
	if (total_passes > total_pools)
		add_pool();

	pool = pools[total_passes - 1];
	opt_set_charp(arg, &pool->rpc_pass);

	return NULL;
}

static char *set_userpass(const char *arg)
{
	struct pool *pool;
	char *updup;

	if (total_users != total_passes)
		return "User + pass options must be balanced before userpass";
	++total_users;
	++total_passes;
	if (total_users > total_pools)
		add_pool();

	pool = pools[total_users - 1];
	updup = strdup(arg);
	opt_set_charp(arg, &pool->rpc_userpass);
	pool->rpc_user = updup;
	pool->rpc_pass = strchr(updup, ':');
	if (pool->rpc_pass)
		pool->rpc_pass++[0] = '\0';
	else
		pool->rpc_pass = &updup[strlen(updup)];

	return NULL;
}

static char *set_cbcaddr(char *arg)
{
	struct pool *pool;
	char *p, *addr;
	bytes_t target_script = BYTES_INIT;
	
	if (!total_pools)
		return "Define pool first, then the --coinbase-check-addr list";
	
	pool = pools[total_pools - 1];
	
	/* NOTE: 'x' is a new prefix which leads both mainnet and testnet address, we would
	 * need support it later, but now leave the code just so.
	 *
	 * Regarding details of address prefix 'x', check the below URL:
	 * https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki#Serialization_format
	 */
	pool->cb_param.testnet = (arg[0] != '1' && arg[0] != '3' && arg[0] != 'x');
	
	for (; (addr = strtok_r(arg, ",", &p)); arg = NULL)
	{
		struct bytes_hashtbl *ah;
		
		if (set_b58addr(addr, &target_script))
			/* No bother to free memory since we are going to exit anyway */
			return "Invalid address in --coinbase-check-address list";
		
		HASH_FIND(hh, pool->cb_param.scripts, bytes_buf(&target_script), bytes_len(&target_script), ah);
		if (!ah)
		{
			/* Note: for the below allocated memory we have good way to release its memory
			 * since we can't be sure there are no reference to the pool struct when remove_pool() 
			 * get called.
			 *
			 * We just hope the remove_pool() would not be called many many times during
			 * the whole running life of this program.
			 */
			ah = malloc(sizeof(*ah));
			bytes_init(&ah->b);
			bytes_assimilate(&ah->b, &target_script);
			HASH_ADD(hh, pool->cb_param.scripts, b.buf, bytes_len(&ah->b), ah);
		}
	}
	bytes_free(&target_script);
	
	return NULL;
}

static char *set_cbctotal(const char *arg)
{
	struct pool *pool;
	
	if (!total_pools)
		return "Define pool first, then the --coinbase-check-total argument";
	
	pool = pools[total_pools - 1];
	pool->cb_param.total = atoll(arg);
	if (pool->cb_param.total < 0)
		return "The total payout amount in coinbase should be greater than 0";
	
	return NULL;
}

static char *set_cbcperc(const char *arg)
{
	struct pool *pool;
	
	if (!total_pools)
		return "Define pool first, then the --coinbase-check-percent argument";
	
	pool = pools[total_pools - 1];
	if (!pool->cb_param.scripts)
		return "Define --coinbase-check-addr list first, then the --coinbase-check-total argument";
	
	pool->cb_param.perc = atof(arg) / 100;
	if (pool->cb_param.perc < 0.0 || pool->cb_param.perc > 1.0)
		return "The percentage should be between 0 and 100";
	
	return NULL;
}

static
const char *goal_set(struct mining_goal_info * const goal, const char * const optname, const char * const newvalue, bytes_t * const replybuf, enum bfg_set_device_replytype * const out_success)
{
	*out_success = SDR_ERR;
	if (!(strcasecmp(optname, "malgo") && strcasecmp(optname, "algo")))
	{
		if (!newvalue)
			return "Goal option 'malgo' requires a value (eg, SHA256d)";
		struct mining_algorithm * const new_malgo = mining_algorithm_by_alias(newvalue);
		if (!new_malgo)
			return "Unrecognised mining algorithm";
		goal_set_malgo(goal, new_malgo);
		goto success;
	}
#if BLKMAKER_VERSION > 1
	if (match_strtok("generate-to|generate-to-addr|generate-to-address|genaddress|genaddr|gen-address|gen-addr|generate-address|generate-addr|coinbase-addr|coinbase-address|coinbase-payout|cbaddress|cbaddr|cb-address|cb-addr|payout", "|", optname))
	{
		if (!newvalue)
			return "Missing value for 'generate-to' goal option";
		const char * const emsg = set_generate_addr2(goal, newvalue);
		if (emsg)
			return emsg;
		goto success;
	}
#endif
	*out_success = SDR_UNKNOWN;
	return "Unknown goal option";

success:
	*out_success = SDR_OK;
	return NULL;
}

// May leak replybuf if returning an error
static
const char *set_goal_params(struct mining_goal_info * const goal, char *arg)
{
	bytes_t replybuf = BYTES_INIT;
	for (char *param, *nextptr; (param = strtok_r(arg, ",", &nextptr)); arg = NULL)
	{
		char *val = strchr(param, '=');
		if (val)
			val++[0] = '\0';
		enum bfg_set_device_replytype success;
		const char * const emsg = goal_set(goal, param, val, &replybuf, &success);
		if (success != SDR_OK)
			return emsg ?: "Error setting goal param";
	}
	bytes_free(&replybuf);
	return NULL;
}

static
const char *set_pool_goal(const char * const arg)
{
	struct pool *pool;
	
	if (!total_pools)
		return "Usage of --pool-goal before pools are defined does not make sense";
	
	pool = pools[total_pools - 1];
	char *param = strchr(arg, ':');
	if (param)
		param++[0] = '\0';
	pool->goal = get_mining_goal(arg);
	
	if (param)
		return set_goal_params(pool->goal, param);
	
	return NULL;
}

static char *set_pool_priority(const char *arg)
{
	struct pool *pool;

	if (!total_pools)
		return "Usage of --pool-priority before pools are defined does not make sense";

	pool = pools[total_pools - 1];
	opt_set_intval(arg, &pool->prio);

	return NULL;
}

static char *set_pool_proxy(const char *arg)
{
	struct pool *pool;

	if (!total_pools)
		return "Usage of --pool-proxy before pools are defined does not make sense";

	if (!our_curl_supports_proxy_uris())
		return "Your installed cURL library does not support proxy URIs. At least version 7.21.7 is required.";

	pool = pools[total_pools - 1];
	opt_set_charp(arg, &pool->rpc_proxy);

	return NULL;
}

static char *set_pool_force_rollntime(const char *arg)
{
	struct pool *pool;
	
	if (!total_pools)
		return "Usage of --force-rollntime before pools are defined does not make sense";
	
	pool = pools[total_pools - 1];
	opt_set_intval(arg, &pool->force_rollntime);
	
	return NULL;
}

static char *enable_debug(bool *flag)
{
	*flag = true;
	opt_debug_console = true;
	/* Turn on verbose output, too. */
	opt_log_output = true;
	return NULL;
}

static char *set_schedtime(const char *arg, struct schedtime *st)
{
	if (sscanf(arg, "%d:%d", &st->tm.tm_hour, &st->tm.tm_min) != 2)
	{
		if (strcasecmp(arg, "now"))
		return "Invalid time set, should be HH:MM";
	} else
		schedstop.tm.tm_sec = 0;
	if (st->tm.tm_hour > 23 || st->tm.tm_min > 59 || st->tm.tm_hour < 0 || st->tm.tm_min < 0)
		return "Invalid time set.";
	st->enable = true;
	return NULL;
}

static
char *set_log_file(char *arg)
{
	char *r = "";
	long int i = strtol(arg, &r, 10);
	int fd, stderr_fd = fileno(stderr);

	if ((!*r) && i >= 0 && i <= INT_MAX)
		fd = i;
	else
	if (!strcmp(arg, "-"))
	{
		fd = fileno(stdout);
		if (unlikely(fd == -1))
			return "Standard output missing for log-file";
	}
	else
	{
		fd = open(arg, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
		if (unlikely(fd == -1))
			return "Failed to open log-file";
	}
	
	close(stderr_fd);
	if (unlikely(-1 == dup2(fd, stderr_fd)))
		return "Failed to dup2 for log-file";
	close(fd);
	
	return NULL;
}

static
char *_bfgopt_set_file(const char *arg, FILE **F, const char *mode, const char *purpose)
{
	char *r = "";
	long int i = strtol(arg, &r, 10);
	static char *err = NULL;
	const size_t errbufsz = 0x100;

	free(err);
	err = NULL;
	
	if ((!*r) && i >= 0 && i <= INT_MAX) {
		*F = fdopen((int)i, mode);
		if (!*F)
		{
			err = malloc(errbufsz);
			snprintf(err, errbufsz, "Failed to open fd %d for %s",
			         (int)i, purpose);
			return err;
		}
	} else if (!strcmp(arg, "-")) {
		*F = (mode[0] == 'a') ? stdout : stdin;
		if (!*F)
		{
			err = malloc(errbufsz);
			snprintf(err, errbufsz, "Standard %sput missing for %s",
			         (mode[0] == 'a') ? "out" : "in", purpose);
			return err;
		}
	} else {
		*F = fopen(arg, mode);
		if (!*F)
		{
			err = malloc(errbufsz);
			snprintf(err, errbufsz, "Failed to open %s for %s",
			         arg, purpose);
			return err;
		}
	}

	return NULL;
}

static char *set_noncelog(char *arg)
{
	return _bfgopt_set_file(arg, &noncelog_file, "a", "nonce log");
}

static char *set_sharelog(char *arg)
{
	return _bfgopt_set_file(arg, &sharelog_file, "a", "share log");
}

static
void _add_set_device_option(const char * const func, const char * const buf)
{
	applog(LOG_DEBUG, "%s: Using --set-device %s", func, buf);
	string_elist_add(buf, &opt_set_device_list);
}

#define add_set_device_option(...)  do{  \
	char _tmp1718[0x100];  \
	snprintf(_tmp1718, sizeof(_tmp1718), __VA_ARGS__);  \
	_add_set_device_option(__func__, _tmp1718);  \
}while(0)

char *set_temp_cutoff(char *arg)
{
	if (strchr(arg, ','))
		return "temp-cutoff no longer supports comma-delimited syntax, use --set-device for better control";
	applog(LOG_WARNING, "temp-cutoff is deprecated! Use --set-device for better control");
	
	add_set_device_option("all:temp-cutoff=%s", arg);
	
	return NULL;
}

char *set_temp_target(char *arg)
{
	if (strchr(arg, ','))
		return "temp-target no longer supports comma-delimited syntax, use --set-device for better control";
	applog(LOG_WARNING, "temp-target is deprecated! Use --set-device for better control");
	
	add_set_device_option("all:temp-target=%s", arg);
	
	return NULL;
}

#ifdef USE_OPENCL
static
char *set_no_opencl_binaries(__maybe_unused void * const dummy)
{
	applog(LOG_WARNING, "The --no-opencl-binaries option is deprecated! Use --set-device OCL:binary=no");
	add_set_device_option("OCL:binary=no");
	return NULL;
}
#endif

static
char *disable_pool_redirect(__maybe_unused void * const dummy)
{
	opt_disable_client_reconnect = true;
	want_stratum = false;
	return NULL;
}

static char *set_api_allow(const char *arg)
{
	opt_set_charp(arg, &opt_api_allow);

	return NULL;
}

static char *set_api_groups(const char *arg)
{
	opt_set_charp(arg, &opt_api_groups);

	return NULL;
}

static char *set_api_description(const char *arg)
{
	opt_set_charp(arg, &opt_api_description);

	return NULL;
}

static char *set_api_mcast_des(const char *arg)
{
	opt_set_charp(arg, &opt_api_mcast_des);

	return NULL;
}

#ifdef USE_ICARUS
extern const struct bfg_set_device_definition icarus_set_device_funcs[];

static char *set_icarus_options(const char *arg)
{
	if (strchr(arg, ','))
		return "icarus-options no longer supports comma-delimited syntax, see README.FPGA for better control";
	applog(LOG_WARNING, "icarus-options is deprecated! See README.FPGA for better control");
	
	char *opts = strdup(arg), *argdup;
	argdup = opts;
	const struct bfg_set_device_definition *sdf = icarus_set_device_funcs;
	const char *drivers[] = {"antminer", "cairnsmore", "erupter", "icarus"};
	char *saveptr, *opt;
	for (int i = 0; i < 4; ++i, ++sdf)
	{
		opt = strtok_r(opts, ":", &saveptr);
		opts = NULL;
		
		if (!opt)
			break;
		
		if (!opt[0])
			continue;
		
		for (int j = 0; j < 4; ++j)
			add_set_device_option("%s:%s=%s", drivers[j], sdf->optname, opt);
	}
	free(argdup);
	return NULL;
}

static char *set_icarus_timing(const char *arg)
{
	if (strchr(arg, ','))
		return "icarus-timing no longer supports comma-delimited syntax, see README.FPGA for better control";
	applog(LOG_WARNING, "icarus-timing is deprecated! See README.FPGA for better control");
	
	const char *drivers[] = {"antminer", "cairnsmore", "erupter", "icarus"};
	for (int j = 0; j < 4; ++j)
		add_set_device_option("%s:timing=%s", drivers[j], arg);
	return NULL;
}
#endif

#ifdef USE_AVALON
extern const struct bfg_set_device_definition avalon_set_device_funcs[];

static char *set_avalon_options(const char *arg)
{
	if (strchr(arg, ','))
		return "avalon-options no longer supports comma-delimited syntax, see README.FPGA for better control";
	applog(LOG_WARNING, "avalon-options is deprecated! See README.FPGA for better control");
	
	char *opts = strdup(arg), *argdup;
	argdup = opts;
	const struct bfg_set_device_definition *sdf = avalon_set_device_funcs;
	char *saveptr, *opt;
	for (int i = 0; i < 5; ++i, ++sdf)
	{
		opt = strtok_r(opts, ":", &saveptr);
		opts = NULL;
		
		if (!opt)
			break;
		
		if (!opt[0])
			continue;
		
		add_set_device_option("avalon:%s=%s", sdf->optname, opt);
	}
	free(argdup);
	return NULL;
}
#endif

#ifdef USE_KLONDIKE
static char *set_klondike_options(const char *arg)
{
	int hashclock;
	double temptarget;
	switch (sscanf(arg, "%d:%lf", &hashclock, &temptarget))
	{
		default:
			return "Unrecognised --klondike-options";
		case 2:
			add_set_device_option("klondike:temp-target=%lf", temptarget);
			// fallthru
		case 1:
			add_set_device_option("klondike:clock=%d", hashclock);
	}
	applog(LOG_WARNING, "klondike-options is deprecated! Use --set-device for better control");
	
	return NULL;
}
#endif

__maybe_unused
static char *set_null(const char __maybe_unused *arg)
{
	return NULL;
}

/* These options are available from config file or commandline */
static struct opt_table opt_config_table[] = {
#ifdef USE_CPUMINING
#ifdef USE_SHA256D
	OPT_WITH_ARG("--algo",
		     set_algo, show_algo, &opt_algo,
		     "Specify sha256 implementation for CPU mining:\n"
		     "\tfastauto*\tQuick benchmark at startup to pick a working algorithm\n"
		     "\tauto\t\tBenchmark at startup and pick fastest algorithm"
		     "\n\tc\t\tLinux kernel sha256, implemented in C"
#ifdef WANT_SSE2_4WAY
		     "\n\t4way\t\ttcatm's 4-way SSE2 implementation"
#endif
#ifdef WANT_VIA_PADLOCK
		     "\n\tvia\t\tVIA padlock implementation"
#endif
		     "\n\tcryptopp\tCrypto++ C/C++ implementation"
#ifdef WANT_CRYPTOPP_ASM32
		     "\n\tcryptopp_asm32\tCrypto++ 32-bit assembler implementation"
#endif
#ifdef WANT_X8632_SSE2
		     "\n\tsse2_32\t\tSSE2 32 bit implementation for i386 machines"
#endif
#ifdef WANT_X8664_SSE2
		     "\n\tsse2_64\t\tSSE2 64 bit implementation for x86_64 machines"
#endif
#ifdef WANT_X8664_SSE4
		     "\n\tsse4_64\t\tSSE4.1 64 bit implementation for x86_64 machines"
#endif
#ifdef WANT_ALTIVEC_4WAY
    "\n\taltivec_4way\tAltivec implementation for PowerPC G4 and G5 machines"
#endif
		),
	OPT_WITH_ARG("-a",
	             set_algo, show_algo, &opt_algo,
	             opt_hidden),
#else
	// NOTE: Silently ignoring option, since it is plausable a non-SHA256d miner was using it just to skip benchmarking
	OPT_WITH_ARG("--algo|-a", arg_ignored, NULL, NULL, opt_hidden),
#endif  /* USE_SHA256D */
#endif  /* USE_CPUMINING */
	OPT_WITH_ARG("--api-allow",
		     set_api_allow, NULL, NULL,
		     "Allow API access only to the given list of [G:]IP[/Prefix] addresses[/subnets]"),
	OPT_WITH_ARG("--api-description",
		     set_api_description, NULL, NULL,
		     "Description placed in the API status header, default: BFGMiner version"),
	OPT_WITH_ARG("--api-groups",
		     set_api_groups, NULL, NULL,
		     "API one letter groups G:cmd:cmd[,P:cmd:*...] defining the cmds a groups can use"),
	OPT_WITHOUT_ARG("--api-listen",
			opt_set_bool, &opt_api_listen,
			"Enable API, default: disabled"),
	OPT_WITHOUT_ARG("--api-mcast",
			opt_set_bool, &opt_api_mcast,
			"Enable API Multicast listener, default: disabled"),
	OPT_WITH_ARG("--api-mcast-addr",
		     opt_set_charp, opt_show_charp, &opt_api_mcast_addr,
		     "API Multicast listen address"),
	OPT_WITH_ARG("--api-mcast-code",
		     opt_set_charp, opt_show_charp, &opt_api_mcast_code,
		     "Code expected in the API Multicast message, don't use '-'"),
	OPT_WITH_ARG("--api-mcast-des",
		     set_api_mcast_des, NULL, NULL,
		     "Description appended to the API Multicast reply, default: ''"),
	OPT_WITH_ARG("--api-mcast-port",
		     set_int_1_to_65535, opt_show_intval, &opt_api_mcast_port,
		     "API Multicast listen port"),
	OPT_WITHOUT_ARG("--api-network",
			opt_set_bool, &opt_api_network,
			"Allow API (if enabled) to listen on/for any address, default: only 127.0.0.1"),
	OPT_WITH_ARG("--api-port",
		     set_int_1_to_65535, opt_show_intval, &opt_api_port,
		     "Port number of miner API"),
#ifdef HAVE_ADL
	OPT_WITHOUT_ARG("--auto-fan",
			opt_set_bool, &opt_autofan,
			opt_hidden),
	OPT_WITHOUT_ARG("--auto-gpu",
			opt_set_bool, &opt_autoengine,
			opt_hidden),
#endif
	OPT_WITHOUT_ARG("--balance",
		     set_balance, &pool_strategy,
		     "Change multipool strategy from failover to even share balance"),
	OPT_WITHOUT_ARG("--benchmark",
			opt_set_bool, &opt_benchmark,
			"Run BFGMiner in benchmark mode - produces no shares"),
	OPT_WITHOUT_ARG("--benchmark-intense",
			set_benchmark_intense, &opt_benchmark_intense,
			"Run BFGMiner in intensive benchmark mode - produces no shares"),
#if defined(USE_BITFORCE)
	OPT_WITHOUT_ARG("--bfl-range",
			opt_set_bool, &opt_bfl_noncerange,
			"Use nonce range on bitforce devices if supported"),
#endif
#ifdef HAVE_CHROOT
        OPT_WITH_ARG("--chroot-dir",
                     opt_set_charp, NULL, &chroot_dir,
                     "Chroot to a directory right after startup"),
#endif
	OPT_WITH_ARG("--cmd-idle",
	             opt_set_charp, NULL, &cmd_idle,
	             "Execute a command when a device is allowed to be idle (rest or wait)"),
	OPT_WITH_ARG("--cmd-sick",
	             opt_set_charp, NULL, &cmd_sick,
	             "Execute a command when a device is declared sick"),
	OPT_WITH_ARG("--cmd-dead",
	             opt_set_charp, NULL, &cmd_dead,
	             "Execute a command when a device is declared dead"),
#if BLKMAKER_VERSION > 0
	OPT_WITH_ARG("--coinbase-sig",
		     set_strdup, NULL, &opt_coinbase_sig,
		     "Set coinbase signature when possible"),
	OPT_WITH_ARG("--coinbase|--cbsig|--cb-sig|--cb|--prayer",
		     set_strdup, NULL, &opt_coinbase_sig,
		     opt_hidden),
#endif
#ifdef HAVE_CURSES
	OPT_WITHOUT_ARG("--compact",
			opt_set_bool, &opt_compact,
			"Use compact display without per device statistics"),
#endif
#ifdef USE_CPUMINING
	OPT_WITH_ARG("--cpu-threads",
		     force_nthreads_int, opt_show_intval, &opt_n_threads,
		     "Number of miner CPU threads"),
	OPT_WITH_ARG("-t",
	             force_nthreads_int, opt_show_intval, &opt_n_threads,
	             opt_hidden),
#endif
	OPT_WITHOUT_ARG("--debug|-D",
		     enable_debug, &opt_debug,
		     "Enable debug output"),
	OPT_WITHOUT_ARG("--debuglog",
		     opt_set_bool, &opt_debug,
		     "Enable debug logging"),
	OPT_WITHOUT_ARG("--device-protocol-dump",
			opt_set_bool, &opt_dev_protocol,
			"Verbose dump of device protocol-level activities"),
	OPT_WITH_ARG("--device|-d",
		     set_devices, NULL, NULL,
	             "Enable only devices matching pattern (default: all)"),
	OPT_WITHOUT_ARG("--disable-rejecting",
			opt_set_bool, &opt_disable_pool,
			"Automatically disable pools that continually reject shares"),
#ifdef USE_LIBMICROHTTPD
	OPT_WITH_ARG("--http-port",
	             opt_set_intval, opt_show_intval, &httpsrv_port,
	             "Port number to listen on for HTTP getwork miners (-1 means disabled)"),
#endif
	OPT_WITH_ARG("--expiry",
		     set_int_0_to_9999, opt_show_intval, &opt_expiry,
		     "Upper bound on how many seconds after getting work we consider a share from it stale (w/o longpoll active)"),
	OPT_WITH_ARG("-E",
	             set_int_0_to_9999, opt_show_intval, &opt_expiry,
	             opt_hidden),
	OPT_WITH_ARG("--expiry-lp",
		     set_int_0_to_9999, opt_show_intval, &opt_expiry_lp,
		     "Upper bound on how many seconds after getting work we consider a share from it stale (with longpoll active)"),
	OPT_WITHOUT_ARG("--failover-only",
			opt_set_bool, &opt_fail_only,
			"Don't leak work to backup pools when primary pool is lagging"),
	OPT_WITH_ARG("--failover-switch-delay",
			set_int_1_to_65535, opt_show_intval, &opt_fail_switch_delay,
			"Delay in seconds before switching back to a failed pool"),
#ifdef USE_FPGA
	OPT_WITHOUT_ARG("--force-dev-init",
	        opt_set_bool, &opt_force_dev_init,
	        "Always initialize devices when possible (such as bitstream uploads to some FPGAs)"),
#endif
#if BLKMAKER_VERSION > 1
	OPT_WITH_ARG("--generate-to",
	             set_generate_addr, NULL, NULL,
	             "Set an address to generate to for solo mining"),
	OPT_WITH_ARG("--generate-to-addr|--generate-to-address|--genaddress|--genaddr|--gen-address|--gen-addr|--generate-address|--generate-addr|--coinbase-addr|--coinbase-address|--coinbase-payout|--cbaddress|--cbaddr|--cb-address|--cb-addr|--payout",
	             set_generate_addr, NULL, NULL,
	             opt_hidden),
#endif
#ifdef USE_OPENCL
	OPT_WITH_ARG("--gpu-dyninterval",
		     set_int_1_to_65535, opt_show_intval, &opt_dynamic_interval,
		     opt_hidden),
	OPT_WITH_ARG("--gpu-platform",
		     set_int_0_to_9999, opt_show_intval, &opt_platform_id,
		     "Select OpenCL platform ID to use for GPU mining"),
	OPT_WITH_ARG("--gpu-threads|-g",
	             set_gpu_threads, opt_show_intval, &opt_g_threads,
	             opt_hidden),
#ifdef HAVE_ADL
	OPT_WITH_ARG("--gpu-engine",
		     set_gpu_engine, NULL, NULL,
	             opt_hidden),
	OPT_WITH_ARG("--gpu-fan",
		     set_gpu_fan, NULL, NULL,
	             opt_hidden),
	OPT_WITH_ARG("--gpu-map",
		     set_gpu_map, NULL, NULL,
		     "Map OpenCL to ADL device order manually, paired CSV (e.g. 1:0,2:1 maps OpenCL 1 to ADL 0, 2 to 1)"),
	OPT_WITH_ARG("--gpu-memclock",
		     set_gpu_memclock, NULL, NULL,
	             opt_hidden),
	OPT_WITH_ARG("--gpu-memdiff",
		     set_gpu_memdiff, NULL, NULL,
	             opt_hidden),
	OPT_WITH_ARG("--gpu-powertune",
		     set_gpu_powertune, NULL, NULL,
	             opt_hidden),
	OPT_WITHOUT_ARG("--gpu-reorder",
			opt_set_bool, &opt_reorder,
			"Attempt to reorder GPU devices according to PCI Bus ID"),
	OPT_WITH_ARG("--gpu-vddc",
		     set_gpu_vddc, NULL, NULL,
	             opt_hidden),
#endif
#ifdef USE_SCRYPT
	OPT_WITH_ARG("--lookup-gap",
		     set_lookup_gap, NULL, NULL,
	             opt_hidden),
#endif
	OPT_WITH_ARG("--intensity|-I",
	             set_intensity, NULL, NULL,
	             opt_hidden),
#endif
#if defined(USE_OPENCL) || defined(USE_MODMINER) || defined(USE_X6500) || defined(USE_ZTEX)
	OPT_WITH_ARG("--kernel-path",
		     opt_set_charp, opt_show_charp, &opt_kernel_path,
	             "Specify a path to where bitstream and kernel files are"),
	OPT_WITH_ARG("-K",
	             opt_set_charp, opt_show_charp, &opt_kernel_path,
	             opt_hidden),
#endif
#ifdef USE_OPENCL
	OPT_WITH_ARG("--kernel|-k",
	             set_kernel, NULL, NULL,
	             opt_hidden),
#endif
#ifdef USE_ICARUS
	OPT_WITH_ARG("--icarus-options",
		     set_icarus_options, NULL, NULL,
		     opt_hidden),
	OPT_WITH_ARG("--icarus-timing",
		     set_icarus_timing, NULL, NULL,
		     opt_hidden),
#endif
#ifdef USE_AVALON
	OPT_WITH_ARG("--avalon-options",
		     set_avalon_options, NULL, NULL,
		     opt_hidden),
#endif
#ifdef USE_KLONDIKE
	OPT_WITH_ARG("--klondike-options",
		     set_klondike_options, NULL, NULL,
		     "Set klondike options clock:temptarget"),
#endif
	OPT_WITHOUT_ARG("--load-balance",
		     set_loadbalance, &pool_strategy,
		     "Change multipool strategy from failover to quota based balance"),
	OPT_WITH_ARG("--log|-l",
		     set_int_0_to_9999, opt_show_intval, &opt_log_interval,
		     "Interval in seconds between log output"),
	OPT_WITH_ARG("--log-file|-L",
	             set_log_file, NULL, NULL,
	             "Append log file for output messages"),
	OPT_WITH_ARG("--logfile",
	             set_log_file, NULL, NULL,
	             opt_hidden),
	OPT_WITHOUT_ARG("--log-microseconds",
	                opt_set_bool, &opt_log_microseconds,
	                "Include microseconds in log output"),
#if defined(unix) || defined(__APPLE__)
	OPT_WITH_ARG("--monitor|-m",
		     opt_set_charp, NULL, &opt_stderr_cmd,
		     "Use custom pipe cmd for output messages"),
#endif // defined(unix)
	OPT_WITHOUT_ARG("--net-delay",
			opt_set_bool, &opt_delaynet,
			"Impose small delays in networking to avoid overloading slow routers"),
	OPT_WITHOUT_ARG("--no-adl",
			opt_set_bool, &opt_noadl,
#ifdef HAVE_ADL
			"Disable the ATI display library used for monitoring and setting GPU parameters"
#else
			opt_hidden
#endif
			),
	OPT_WITHOUT_ARG("--no-gbt",
			opt_set_invbool, &want_gbt,
			"Disable getblocktemplate support"),
	OPT_WITHOUT_ARG("--no-getwork",
			opt_set_invbool, &want_getwork,
			"Disable getwork support"),
	OPT_WITHOUT_ARG("--no-hotplug",
#ifdef HAVE_BFG_HOTPLUG
	                opt_set_invbool, &opt_hotplug,
	                "Disable hotplug detection"
#else
	                set_null, &opt_hotplug,
	                opt_hidden
#endif
	),
	OPT_WITHOUT_ARG("--no-local-bitcoin",
#if BLKMAKER_VERSION > 1
	                opt_set_invbool, &opt_load_bitcoin_conf,
	                "Disable adding pools for local bitcoin RPC servers"),
#else
	                set_null, NULL, opt_hidden),
#endif
	OPT_WITHOUT_ARG("--no-longpoll",
			opt_set_invbool, &want_longpoll,
			"Disable X-Long-Polling support"),
	OPT_WITHOUT_ARG("--no-pool-disable",
			opt_set_invbool, &opt_disable_pool,
			opt_hidden),
	OPT_WITHOUT_ARG("--no-client-reconnect",
			opt_set_invbool, &opt_disable_client_reconnect,
			opt_hidden),
	OPT_WITHOUT_ARG("--no-pool-redirect",
			disable_pool_redirect, NULL,
			"Ignore pool requests to redirect to another server"),
	OPT_WITHOUT_ARG("--no-restart",
			opt_set_invbool, &opt_restart,
			"Do not attempt to restart devices that hang"
	),
	OPT_WITHOUT_ARG("--no-show-processors",
			opt_set_invbool, &opt_show_procs,
			opt_hidden),
	OPT_WITHOUT_ARG("--no-show-procs",
			opt_set_invbool, &opt_show_procs,
			opt_hidden),
	OPT_WITHOUT_ARG("--no-stratum",
			opt_set_invbool, &want_stratum,
			"Disable Stratum detection"),
	OPT_WITHOUT_ARG("--no-submit-stale",
			opt_set_invbool, &opt_submit_stale,
		        "Don't submit shares if they are detected as stale"),
#ifdef USE_OPENCL
	OPT_WITHOUT_ARG("--no-opencl-binaries",
	                set_no_opencl_binaries, NULL,
	                opt_hidden),
#endif
	OPT_WITHOUT_ARG("--no-unicode",
#ifdef USE_UNICODE
	                opt_set_invbool, &use_unicode,
	                "Don't use Unicode characters in TUI"
#else
	                set_null, &use_unicode,
	                opt_hidden
#endif
	),
	OPT_WITH_ARG("--noncelog",
		     set_noncelog, NULL, NULL,
		     "Create log of all nonces found"),
	OPT_WITH_ARG("--pass|-p",
		     set_pass, NULL, NULL,
		     "Password for bitcoin JSON-RPC server"),
	OPT_WITHOUT_ARG("--per-device-stats",
			opt_set_bool, &want_per_device_stats,
			"Force verbose mode and output per-device statistics"),
	OPT_WITH_ARG("--userpass|-O",
	             set_userpass, NULL, NULL,
	             "Username:Password pair for bitcoin JSON-RPC server"),
	OPT_WITH_ARG("--pool-goal",
			 set_pool_goal, NULL, NULL,
			 "Named goal for the previous-defined pool"),
	OPT_WITH_ARG("--pool-priority",
			 set_pool_priority, NULL, NULL,
			 "Priority for just the previous-defined pool"),
	OPT_WITH_ARG("--pool-proxy|-x",
		     set_pool_proxy, NULL, NULL,
		     "Proxy URI to use for connecting to just the previous-defined pool"),
	OPT_WITH_ARG("--force-rollntime",  // NOTE: must be after --pass for config file ordering
			 set_pool_force_rollntime, NULL, NULL,
			 opt_hidden),
	OPT_WITHOUT_ARG("--protocol-dump|-P",
			opt_set_bool, &opt_protocol,
			"Verbose dump of protocol-level activities"),
	OPT_WITH_ARG("--queue|-Q",
		     set_int_0_to_9999, opt_show_intval, &opt_queue,
		     "Minimum number of work items to have queued (0+)"),
	OPT_WITHOUT_ARG("--quiet|-q",
			opt_set_bool, &opt_quiet,
			"Disable logging output, display status and errors"),
	OPT_WITHOUT_ARG("--quiet-work-updates|--quiet-work-update",
			opt_set_bool, &opt_quiet_work_updates,
			opt_hidden),
	OPT_WITH_ARG("--quit-summary",
	             set_quit_summary, NULL, NULL,
	             "Summary printed when you quit: none/devs/procs/detailed"),
	OPT_WITH_ARG("--quota|-U",
		     set_quota, NULL, NULL,
		     "quota;URL combination for server with load-balance strategy quotas"),
	OPT_WITHOUT_ARG("--real-quiet",
			opt_set_bool, &opt_realquiet,
			"Disable all output"),
	OPT_WITH_ARG("--request-diff",
	             set_request_diff, opt_show_floatval, &request_pdiff,
	             "Request a specific difficulty from pools"),
	OPT_WITH_ARG("--retries",
		     opt_set_intval, opt_show_intval, &opt_retries,
		     "Number of times to retry failed submissions before giving up (-1 means never)"),
	OPT_WITH_ARG("--retry-pause",
		     set_null, NULL, NULL,
		     opt_hidden),
	OPT_WITH_ARG("--rotate",
		     set_rotate, opt_show_intval, &opt_rotate_period,
		     "Change multipool strategy from failover to regularly rotate at N minutes"),
	OPT_WITHOUT_ARG("--round-robin",
		     set_rr, &pool_strategy,
		     "Change multipool strategy from failover to round robin on failure"),
	OPT_WITH_ARG("--scan|-S",
		     add_serial, NULL, NULL,
		     "Configure how to scan for mining devices"),
	OPT_WITH_ARG("--scan-device|--scan-serial|--devscan",
		     add_serial, NULL, NULL,
		     opt_hidden),
	OPT_WITH_ARG("--scan-time",
		     set_int_0_to_9999, opt_show_intval, &opt_scantime,
		     "Upper bound on time spent scanning current work, in seconds"),
	OPT_WITH_ARG("-s",
		     set_int_0_to_9999, opt_show_intval, &opt_scantime,
		     opt_hidden),
	OPT_WITH_ARG("--scantime",
		     set_int_0_to_9999, opt_show_intval, &opt_scantime,
		     opt_hidden),
	OPT_WITH_ARG("--sched-start",
		     set_schedtime, NULL, &schedstart,
		     "Set a time of day in HH:MM to start mining (a once off without a stop time)"),
	OPT_WITH_ARG("--sched-stop",
		     set_schedtime, NULL, &schedstop,
		     "Set a time of day in HH:MM to stop mining (will quit without a start time)"),
#ifdef USE_SCRYPT
	OPT_WITHOUT_ARG("--scrypt",
	                set_malgo_scrypt, NULL,
			"Use the scrypt algorithm for mining (non-bitcoin)"),
#endif
	OPT_WITH_ARG("--set-device|--set",
			opt_string_elist_add, NULL, &opt_set_device_list,
			"Set default parameters on devices; eg"
			", NFY:osc6_bits=50"
			", bfl:voltage=<value>"
			", compac:clock=<value>"
	),

#if defined(USE_SCRYPT) && defined(USE_OPENCL)
	OPT_WITH_ARG("--shaders",
		     set_shaders, NULL, NULL,
	             opt_hidden),
#endif
#ifdef HAVE_PWD_H
        OPT_WITH_ARG("--setuid",
                     opt_set_charp, NULL, &opt_setuid,
                     "Username of an unprivileged user to run as"),
#endif
	OPT_WITH_ARG("--sharelog",
		     set_sharelog, NULL, NULL,
		     "Append share log to file"),
	OPT_WITH_ARG("--shares",
		     opt_set_floatval, NULL, &opt_shares,
		     "Quit after mining 2^32 * N hashes worth of shares (default: unlimited)"),
	OPT_WITHOUT_ARG("--show-processors",
			opt_set_bool, &opt_show_procs,
			"Show per processor statistics in summary"),
	OPT_WITHOUT_ARG("--show-procs",
			opt_set_bool, &opt_show_procs,
			opt_hidden),
	OPT_WITH_ARG("--skip-security-checks",
			set_int_0_to_9999, NULL, &opt_skip_checks,
			"Skip security checks sometimes to save bandwidth; only check 1/<arg>th of the time (default: never skip)"),
	OPT_WITH_ARG("--socks-proxy",
		     opt_set_charp, NULL, &opt_socks_proxy,
		     "Set socks proxy (host:port)"),
#ifdef USE_LIBEVENT
	OPT_WITH_ARG("--stratum-port",
	             set_long_1_to_65535_or_neg1, opt_show_longval, &stratumsrv_port,
	             "Port number to listen on for stratum miners (-1 means disabled)"),
#endif
	OPT_WITHOUT_ARG("--submit-stale",
			opt_set_bool, &opt_submit_stale,
	                opt_hidden),
	OPT_WITH_ARG("--submit-threads",
	                opt_set_intval, opt_show_intval, &opt_submit_threads,
	                "Minimum number of concurrent share submissions (default: 64)"),
#ifdef HAVE_SYSLOG_H
	OPT_WITHOUT_ARG("--syslog",
			opt_set_bool, &use_syslog,
			"Use system log for output messages (default: standard error)"),
#endif
	OPT_WITH_ARG("--temp-cutoff",
		     set_temp_cutoff, NULL, &opt_cutofftemp,
		     opt_hidden),
	OPT_WITH_ARG("--temp-hysteresis",
		     set_int_1_to_10, opt_show_intval, &opt_hysteresis,
		     "Set how much the temperature can fluctuate outside limits when automanaging speeds"),
#ifdef HAVE_ADL
	OPT_WITH_ARG("--temp-overheat",
		     set_temp_overheat, opt_show_intval, &opt_overheattemp,
	             opt_hidden),
#endif
	OPT_WITH_ARG("--temp-target",
		     set_temp_target, NULL, NULL,
		     opt_hidden),
	OPT_WITHOUT_ARG("--text-only|-T",
			opt_set_invbool, &use_curses,
#ifdef HAVE_CURSES
			"Disable ncurses formatted screen output"
#else
			opt_hidden
#endif
	),
#if defined(USE_SCRYPT) && defined(USE_OPENCL)
	OPT_WITH_ARG("--thread-concurrency",
		     set_thread_concurrency, NULL, NULL,
	             opt_hidden),
#endif
#ifdef USE_UNICODE
	OPT_WITHOUT_ARG("--unicode",
	                opt_set_bool, &use_unicode,
	                "Use Unicode characters in TUI"),
#endif
	OPT_WITH_ARG("--url|-o",
		     set_url, NULL, NULL,
		     "URL for bitcoin JSON-RPC server"),
	OPT_WITH_ARG("--user|-u",
		     set_user, NULL, NULL,
		     "Username for bitcoin JSON-RPC server"),
#ifdef USE_OPENCL
	OPT_WITH_ARG("--vectors|-v",
	             set_vector, NULL, NULL,
	             opt_hidden),
#endif
	OPT_WITHOUT_ARG("--verbose",
			opt_set_bool, &opt_log_output,
			"Log verbose output to stderr as well as status output"),
	OPT_WITHOUT_ARG("--verbose-work-updates|--verbose-work-update",
			opt_set_invbool, &opt_quiet_work_updates,
			opt_hidden),
	OPT_WITHOUT_ARG("--weighed-stats",
	                opt_set_bool, &opt_weighed_stats,
	                "Display statistics weighed to difficulty 1"),
#ifdef USE_OPENCL
	OPT_WITH_ARG("--worksize|-w",
	             set_worksize, NULL, NULL,
	             opt_hidden),
#endif
	OPT_WITHOUT_ARG("--unittest",
			opt_set_bool, &opt_unittest, opt_hidden),
	OPT_WITH_ARG("--coinbase-check-addr",
			set_cbcaddr, NULL, NULL,
			"A list of address to check against in coinbase payout list received from the previous-defined pool, separated by ','"),
	OPT_WITH_ARG("--cbcheck-addr|--cbc-addr|--cbcaddr",
			set_cbcaddr, NULL, NULL,
			opt_hidden),
	OPT_WITH_ARG("--coinbase-check-total",
			set_cbctotal, NULL, NULL,
			"The least total payout amount expected in coinbase received from the previous-defined pool"),
	OPT_WITH_ARG("--cbcheck-total|--cbc-total|--cbctotal",
			set_cbctotal, NULL, NULL,
			opt_hidden),
	OPT_WITH_ARG("--coinbase-check-percent",
			set_cbcperc, NULL, NULL,
			"The least benefit percentage expected for the sum of addr(s) listed in --cbaddr argument for previous-defined pool"),
	OPT_WITH_ARG("--cbcheck-percent|--cbc-percent|--cbcpercent|--cbcperc",
			set_cbcperc, NULL, NULL,
			opt_hidden),
	OPT_WITHOUT_ARG("--worktime",
			opt_set_bool, &opt_worktime,
			"Display extra work time debug information"),
	OPT_WITH_ARG("--pools",
			opt_set_bool, NULL, NULL, opt_hidden),
	OPT_ENDTABLE
};

static char *load_config(const char *arg, void __maybe_unused *unused);

static char *parse_config(json_t *config, bool fileconf, int * const fileconf_load_p)
{
	static char err_buf[200];
	struct opt_table *opt;
	json_t *val;

	if (fileconf && !*fileconf_load_p)
		*fileconf_load_p = 1;

	for (opt = opt_config_table; opt->type != OPT_END; opt++) {
		char *p, *name, *sp;

		/* We don't handle subtables. */
		assert(!(opt->type & OPT_SUBTABLE));

		if (!opt->names)
			continue;

		/* Pull apart the option name(s). */
		name = strdup(opt->names);
		for (p = strtok_r(name, "|", &sp); p; p = strtok_r(NULL, "|", &sp)) {
			char *err = "Invalid value";

			/* Ignore short options. */
			if (p[1] != '-')
				continue;

			val = json_object_get(config, p+2);
			if (!val)
				continue;

			if (opt->type & OPT_HASARG) {
			  if (json_is_string(val)) {
				err = opt->cb_arg(json_string_value(val),
						  opt->u.arg);
			  } else if (json_is_number(val)) {
					char buf[256], *p, *q;
					snprintf(buf, 256, "%f", json_number_value(val));
					if ( (p = strchr(buf, '.')) ) {
						// Trim /\.0*$/ to work properly with integer-only arguments
						q = p;
						while (*(++q) == '0') {}
						if (*q == '\0')
							*p = '\0';
					}
					err = opt->cb_arg(buf, opt->u.arg);
			  } else if (json_is_array(val)) {
				int n, size = json_array_size(val);

				err = NULL;
				for (n = 0; n < size && !err; n++) {
					if (json_is_string(json_array_get(val, n)))
						err = opt->cb_arg(json_string_value(json_array_get(val, n)), opt->u.arg);
					else if (json_is_object(json_array_get(val, n)))
						err = parse_config(json_array_get(val, n), false, fileconf_load_p);
				}
			  }
			} else if (opt->type & OPT_NOARG) {
				if (json_is_true(val))
					err = opt->cb(opt->u.arg);
				else if (json_is_boolean(val)) {
					if (opt->cb == (void*)opt_set_bool)
						err = opt_set_invbool(opt->u.arg);
					else if (opt->cb == (void*)opt_set_invbool)
						err = opt_set_bool(opt->u.arg);
				}
			}

			if (err) {
				/* Allow invalid values to be in configuration
				 * file, just skipping over them provided the
				 * JSON is still valid after that. */
				if (fileconf) {
					applog(LOG_ERR, "Invalid config option %s: %s", p, err);
					*fileconf_load_p = -1;
				} else {
					snprintf(err_buf, sizeof(err_buf), "Parsing JSON option %s: %s",
						p, err);
					free(name);
					return err_buf;
				}
			}
		}
		free(name);
	}

	val = json_object_get(config, JSON_INCLUDE_CONF);
	if (val && json_is_string(val))
		return load_config(json_string_value(val), NULL);

	return NULL;
}

struct bfg_loaded_configfile *bfg_loaded_configfiles;

char conf_web1[] = "http://";
char conf_web2[] = "https://";

static char *load_web_config(const char *arg)
{
	json_t *val;
	CURL *curl;
	struct bfg_loaded_configfile *cfginfo;

	curl = curl_easy_init();
	if (unlikely(!curl))
		quithere(1, "CURL initialisation failed");

	val = json_web_config(curl, arg);

	curl_easy_cleanup(curl);

	if (!val || !json_is_object(val))
		return JSON_WEB_ERROR;

	cfginfo = malloc(sizeof(*cfginfo));
	*cfginfo = (struct bfg_loaded_configfile){
		.filename = strdup(arg),
	};
	LL_APPEND(bfg_loaded_configfiles, cfginfo);

	config_loaded = true;

	return parse_config(val, true, &cfginfo->fileconf_load);
}

static char *load_config(const char *arg, void __maybe_unused *unused)
{
	json_error_t err;
	json_t *config;
	char *json_error;
	size_t siz;
	struct bfg_loaded_configfile *cfginfo;

	if (strncasecmp(arg, conf_web1, sizeof(conf_web1)-1) == 0 ||
	    strncasecmp(arg, conf_web2, sizeof(conf_web2)-1) == 0)
		return load_web_config(arg);

	cfginfo = malloc(sizeof(*cfginfo));
	*cfginfo = (struct bfg_loaded_configfile){
		.filename = strdup(arg),
	};
	LL_APPEND(bfg_loaded_configfiles, cfginfo);

	if (++include_count > JSON_MAX_DEPTH)
		return JSON_MAX_DEPTH_ERR;

#if JANSSON_MAJOR_VERSION > 1
	config = json_load_file(arg, 0, &err);
#else
	config = json_load_file(arg, &err);
#endif
	if (!json_is_object(config)) {
		siz = JSON_LOAD_ERROR_LEN + strlen(arg) + strlen(err.text);
		json_error = malloc(siz);
		if (!json_error)
			quit(1, "Malloc failure in json error");

		snprintf(json_error, siz, JSON_LOAD_ERROR, arg, err.text);
		return json_error;
	}

	config_loaded = true;

	/* Parse the config now, so we can override it.  That can keep pointers
	 * so don't free config object. */
	return parse_config(config, true, &cfginfo->fileconf_load);
}

static
bool _load_default_configs(const char * const filepath, void * __maybe_unused userp)
{
	bool * const found_defcfg_p = userp;
	*found_defcfg_p = true;
	
	load_config(filepath, NULL);
	
	// Regardless of status of loading the config file, we should continue loading other defaults
	return false;
}

static void load_default_config(void)
{
	bool found_defcfg = false;
	appdata_file_call("BFGMiner", def_conf, _load_default_configs, &found_defcfg);
	
	if (!found_defcfg)
	{
		// No BFGMiner config, try Cgminer's...
		appdata_file_call("cgminer", "cgminer.conf", _load_default_configs, &found_defcfg);
	}
}

extern const char *opt_argv0;

static
void bfg_versioninfo(void)
{
	puts(packagename);
	printf("  Lowlevel:%s\n", BFG_LOWLLIST);
	printf("  Drivers:%s\n", BFG_DRIVERLIST);
	printf("  Algorithms:%s\n", BFG_ALGOLIST);
	printf("  Options:%s\n", BFG_OPTLIST);
}

static char *opt_verusage_and_exit(const char *extra)
{
	bfg_versioninfo();
	printf("%s", opt_usage(opt_argv0, extra));
	fflush(stdout);
	exit(0);
}

static
const char *my_opt_version_and_exit(void)
{
	bfg_versioninfo();
	fflush(stdout);
	exit(0);
}

/* These options are parsed before anything else */
static struct opt_table opt_early_table[] = {
	// Default config is loaded in command line order, like a regular config
	OPT_EARLY_WITH_ARG("--config|-c|--default-config",
	                   set_bool_ignore_arg, NULL, &config_loaded,
	                   opt_hidden),
	OPT_EARLY_WITHOUT_ARG("--no-config|--no-default-config",
	                opt_set_bool, &config_loaded,
	                "Inhibit loading default config file"),
	OPT_ENDTABLE
};

/* These options are available from commandline only */
static struct opt_table opt_cmdline_table[] = {
	OPT_WITH_ARG("--config|-c",
		     load_config, NULL, NULL,
		     "Load a JSON-format configuration file\n"
		     "See example.conf for an example configuration."),
	OPT_EARLY_WITHOUT_ARG("--no-config",
	                opt_set_bool, &config_loaded,
	                opt_hidden),
	OPT_EARLY_WITHOUT_ARG("--no-default-config",
	                opt_set_bool, &config_loaded,
	                "Inhibit loading default config file"),
	OPT_WITHOUT_ARG("--default-config",
	                load_default_config, NULL,
	                "Always load the default config file"),
	OPT_WITHOUT_ARG("--help|-h",
			opt_verusage_and_exit, NULL,
			"Print this message"),
#ifdef USE_OPENCL
	OPT_WITHOUT_ARG("--ndevs|-n",
			print_ndevs_and_exit, &nDevs,
			opt_hidden),
#endif
	OPT_WITHOUT_ARG("--version|-V",
			my_opt_version_and_exit, NULL,
			"Display version and exit"),
	OPT_ENDTABLE
};

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen, bool required)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		if (unlikely(required))
			applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}

static void calc_midstate(struct work *work)
{
	union {
		unsigned char c[64];
		uint32_t i[16];
	} data;

	swap32yes(&data.i[0], work->data, 16);
	sha256_ctx ctx;
	sha256t_init(&ctx);
	sha256_update(&ctx, data.c, 64);
	memcpy(work->midstate, ctx.h, sizeof(work->midstate));
	swap32tole(work->midstate, work->midstate, 8);
}

static
struct bfg_tmpl_ref *tmpl_makeref(blktemplate_t * const tmpl)
{
	struct bfg_tmpl_ref * const tr = malloc(sizeof(*tr));
	*tr = (struct bfg_tmpl_ref){
		.tmpl = tmpl,
		.refcount = 1,
	};
	mutex_init(&tr->mutex);
	return tr;
}

static
void tmpl_incref(struct bfg_tmpl_ref * const tr)
{
	mutex_lock(&tr->mutex);
	++tr->refcount;
	mutex_unlock(&tr->mutex);
}

void tmpl_decref(struct bfg_tmpl_ref * const tr)
{
	mutex_lock(&tr->mutex);
	bool free_tmpl = !--tr->refcount;
	mutex_unlock(&tr->mutex);
	if (free_tmpl)
	{
		blktmpl_free(tr->tmpl);
		mutex_destroy(&tr->mutex);
		free(tr);
	}
}

static struct work *make_work(void)
{
	struct work *work = calloc(1, sizeof(struct work));

	if (unlikely(!work))
		quit(1, "Failed to calloc work in make_work");

	cg_wlock(&control_lock);
	work->id = total_work++;
	cg_wunlock(&control_lock);

	return work;
}

/* This is the central place all work that is about to be retired should be
 * cleaned to remove any dynamically allocated arrays within the struct */
void clean_work(struct work *work)
{
	free(work->job_id);
	bytes_free(&work->nonce2);
	free(work->nonce1);
	if (work->device_data_free_func)
		work->device_data_free_func(work);

	if (work->tr)
		tmpl_decref(work->tr);

	memset(work, 0, sizeof(struct work));
}

/* All dynamically allocated work structs should be freed here to not leak any
 * ram from arrays allocated within the work struct */
void free_work(struct work *work)
{
	clean_work(work);
	free(work);
}

const char *bfg_workpadding_bin = "\0\0\0\x80\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x80\x02\0\0";
#define workpadding_bin  bfg_workpadding_bin

static const size_t block_info_str_sz = 3 /* ... */ + 16 /* block hash segment */ + 1;

static
void block_info_str(char * const out, const struct block_info * const blkinfo)
{
	unsigned char hash_swap[32];
	swap256(hash_swap, blkinfo->prevblkhash);
	swap32tole(hash_swap, hash_swap, 32 / 4);
	
	memset(out, '.', 3);
	// FIXME: The block number will overflow this sometime around AD 2025-2027
	if (blkinfo->height > 0 && blkinfo->height < 1000000)
	{
		bin2hex(&out[3], &hash_swap[0x1c], 4);
		snprintf(&out[11], block_info_str_sz-11, " #%6u", blkinfo->height);
	}
	else
		bin2hex(&out[3], &hash_swap[0x18], 8);
}

#ifdef HAVE_CURSES
static void update_block_display(bool);
#endif

// Must only be called with ch_lock held!
static
void __update_block_title(struct mining_goal_info * const goal)
{
	struct blockchain_info * const blkchain = goal->blkchain;
	
	if (!goal->current_goal_detail)
		goal->current_goal_detail = malloc(block_info_str_sz);
	block_info_str(goal->current_goal_detail, blkchain->currentblk);
#ifdef HAVE_CURSES
	update_block_display(false);
#endif
}

static struct block_info *block_exists(const struct blockchain_info *, const void *);

static
void have_block_height(struct mining_goal_info * const goal, const void * const prevblkhash, uint32_t blkheight)
{
	struct blockchain_info * const blkchain = goal->blkchain;
	struct block_info * const blkinfo = block_exists(blkchain, prevblkhash);
	if ((!blkinfo) || blkinfo->height)
		return;
	
	uint32_t block_id = ((uint32_t*)prevblkhash)[0];
	applog(LOG_DEBUG, "Learned that block id %08" PRIx32 " is height %" PRIu32, (uint32_t)be32toh(block_id), blkheight);
	cg_wlock(&ch_lock);
	blkinfo->height = blkheight;
	if (blkinfo == blkchain->currentblk)
	{
		blkchain->currentblk_subsidy = 5000000000LL >> (blkheight / 210000);
		__update_block_title(goal);
	}
	cg_wunlock(&ch_lock);
}

static
void pool_set_opaque(struct pool *pool, bool opaque)
{
	if (pool->swork.opaque == opaque)
		return;
	
	pool->swork.opaque = opaque;
	if (opaque)
		applog(LOG_WARNING, "Pool %u is hiding block contents from us",
		       pool->pool_no);
	else
		applog(LOG_NOTICE, "Pool %u now providing block contents to us",
		       pool->pool_no);
}

bool pool_may_redirect_to(struct pool * const pool, const char * const uri)
{
	if (uri_get_param_bool(pool->rpc_url, "redirect", false))
		return true;
	return match_domains(pool->rpc_url, strlen(pool->rpc_url), uri, strlen(uri));
}

void pool_check_coinbase(struct pool * const pool, const uint8_t * const cbtxn, const size_t cbtxnsz)
{
	if (uri_get_param_bool(pool->rpc_url, "skipcbcheck", false))
	{}
	else
	if (!check_coinbase(cbtxn, cbtxnsz, &pool->cb_param))
	{
		if (pool->enabled == POOL_ENABLED)
		{
			applog(LOG_ERR, "Pool %d misbehaving (%s), disabling!", pool->pool_no, "coinbase check");
			disable_pool(pool, POOL_MISBEHAVING);
		}
	}
	else
	if (pool->enabled == POOL_MISBEHAVING)
	{
		applog(LOG_NOTICE, "Pool %d no longer misbehaving, re-enabling!", pool->pool_no);
		enable_pool(pool);
	}
}

void set_simple_ntime_roll_limit(struct ntime_roll_limits * const nrl, const uint32_t ntime_base, const int ntime_roll, const struct timeval * const tvp_ref)
{
	const int offsets = max(ntime_roll, 60);
	*nrl = (struct ntime_roll_limits){
		.min = ntime_base,
		.max = ntime_base + ntime_roll,
		.tv_ref = *tvp_ref,
		.minoff = -offsets,
		.maxoff = offsets,
	};
}

void work_set_simple_ntime_roll_limit(struct work * const work, const int ntime_roll, const struct timeval * const tvp_ref)
{
	set_simple_ntime_roll_limit(&work->ntime_roll_limits, upk_u32be(work->data, 0x44), ntime_roll, tvp_ref);
}

int work_ntime_range(struct work * const work, const struct timeval * const tvp_earliest, const struct timeval * const tvp_latest, const int desired_roll)
{
	const struct ntime_roll_limits * const nrl = &work->ntime_roll_limits;
	const uint32_t ref_ntime = work_get_ntime(work);
	const int earliest_elapsed = timer_elapsed(&nrl->tv_ref, tvp_earliest);
	const int   latest_elapsed = timer_elapsed(&nrl->tv_ref, tvp_latest);
	// minimum ntime is the latest possible result (add a second to spare) adjusted for minimum offset (or fixed minimum ntime)
	uint32_t min_ntime = max(nrl->min, ref_ntime + latest_elapsed+1 + nrl->minoff);
	// maximum ntime is the earliest possible result adjusted for maximum offset (or fixed maximum ntime)
	uint32_t max_ntime = min(nrl->max, ref_ntime + earliest_elapsed + nrl->maxoff);
	if (max_ntime < min_ntime)
		return -1;
	
	if (max_ntime - min_ntime > desired_roll)
	{
		// Adjust min_ntime upward for accuracy, when possible
		const int mid_elapsed = ((latest_elapsed - earliest_elapsed) / 2) + earliest_elapsed;
		uint32_t ideal_ntime = ref_ntime + mid_elapsed;
		if (ideal_ntime > min_ntime)
			min_ntime = min(ideal_ntime, max_ntime - desired_roll);
	}
	
	work_set_ntime(work, min_ntime);
	return max_ntime - min_ntime;
}

#if BLKMAKER_VERSION > 1
static
bool goal_has_at_least_one_getcbaddr(const struct mining_goal_info * const goal)
{
	for (int i = 0; i < total_pools; ++i)
	{
		struct pool * const pool = pools[i];
		if (uri_get_param_bool(pool->rpc_url, "getcbaddr", false))
			return true;
	}
	return false;
}

static
void refresh_bitcoind_address(struct mining_goal_info * const goal, const bool fresh)
{
	struct blockchain_info * const blkchain = goal->blkchain;
	
	if (!goal_has_at_least_one_getcbaddr(goal))
		return;
	
	char getcbaddr_req[60];
	CURL *curl = NULL;
	json_t *json, *j2;
	const char *s, *s2;
	bytes_t newscript = BYTES_INIT;
	
	snprintf(getcbaddr_req, sizeof(getcbaddr_req), "{\"method\":\"get%saddress\",\"id\":0,\"params\":[\"BFGMiner\"]}", fresh ? "new" : "account");
	
	for (int i = 0; i < total_pools; ++i)
	{
		struct pool * const pool = pools[i];
		if (!uri_get_param_bool(pool->rpc_url, "getcbaddr", false))
			continue;
		if (pool->goal != goal)
			continue;
		
		applog(LOG_DEBUG, "Refreshing coinbase address from pool %d", pool->pool_no);
		if (!curl)
		{
			curl = curl_easy_init();
			if (unlikely(!curl))
			{
				applogfail(LOG_ERR, "curl_easy_init");
				break;
			}
		}
		json = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, getcbaddr_req, false, false, NULL, pool, true);
		if (unlikely((!json) || !json_is_null( (j2 = json_object_get(json, "error")) )))
		{
			const char *estrc;
			char *estr = NULL;
			if (!(json && j2))
				estrc = NULL;
			else
			{
				estrc = json_string_value(j2);
				if (!estrc)
					estrc = estr = json_dumps_ANY(j2, JSON_ENSURE_ASCII | JSON_SORT_KEYS);
			}
			applog(LOG_WARNING, "Error %cetting coinbase address from pool %d: %s", 'g', pool->pool_no, estrc);
			free(estr);
			json_decref(json);
			continue;
		}
		s = bfg_json_obj_string(json, "result", NULL);
		if (unlikely(!s))
		{
			applog(LOG_WARNING, "Error %cetting coinbase address from pool %d: %s", 'g', pool->pool_no, "(return value was not a String)");
			json_decref(json);
			continue;
		}
		s2 = set_b58addr(s, &newscript);
		if (unlikely(s2))
		{
			applog(LOG_WARNING, "Error %cetting coinbase address from pool %d: %s", 's', pool->pool_no, s2);
			json_decref(json);
			continue;
		}
		cg_ilock(&control_lock);
		if (goal->generation_script)
		{
			if (bytes_eq(&newscript, goal->generation_script))
			{
				cg_iunlock(&control_lock);
				applog(LOG_DEBUG, "Pool %d returned coinbase address already in use (%s)", pool->pool_no, s);
				json_decref(json);
				break;
			}
			cg_ulock(&control_lock);
		}
		else
		{
			cg_ulock(&control_lock);
			goal->generation_script = malloc(sizeof(*goal->generation_script));
			bytes_init(goal->generation_script);
		}
		bytes_assimilate(goal->generation_script, &newscript);
		coinbase_script_block_id = blkchain->currentblk->block_id;
		cg_wunlock(&control_lock);
		applog(LOG_NOTICE, "Now using coinbase address %s, provided by pool %d", s, pool->pool_no);
		json_decref(json);
		break;
	}
	
	bytes_free(&newscript);
	if (curl)
		curl_easy_cleanup(curl);
}
#endif

#define GBT_XNONCESZ (sizeof(uint32_t))

#if BLKMAKER_VERSION > 6
#define blkmk_append_coinbase_safe(tmpl, append, appendsz)  \
       blkmk_append_coinbase_safe2(tmpl, append, appendsz, GBT_XNONCESZ, false)
#endif

static bool work_decode(struct pool *pool, struct work *work, json_t *val)
{
	json_t *res_val = json_object_get(val, "result");
	json_t *tmp_val;
	bool ret = false;
	struct timeval tv_now;

	if (unlikely(detect_algo == 1)) {
		json_t *tmp = json_object_get(res_val, "algorithm");
		const char *v = tmp ? json_string_value(tmp) : "";
		if (strncasecmp(v, "scrypt", 6))
			detect_algo = 2;
	}
	
	timer_set_now(&tv_now);
	
	if (work->tr)
	{
		blktemplate_t * const tmpl = work->tr->tmpl;
		tmpl->mutations |= BMM_VERFORCE;

		const char *err = blktmpl_add_jansson(tmpl, res_val, tv_now.tv_sec);
		if (err) {
			applog(LOG_ERR, "blktmpl error: %s", err);
			return false;
		}
		work->rolltime = blkmk_time_left(tmpl, tv_now.tv_sec);
#if BLKMAKER_VERSION > 1
		struct mining_goal_info * const goal = pool->goal;
		const uint32_t tmpl_block_id = ((uint32_t*)tmpl->prevblk)[0];
		if ((!tmpl->cbtxn) && coinbase_script_block_id != tmpl_block_id)
			refresh_bitcoind_address(goal, false);
		if (goal->generation_script)
		{
			bool newcb;
#if BLKMAKER_VERSION > 2
			blkmk_init_generation2(tmpl, bytes_buf(goal->generation_script), bytes_len(goal->generation_script), &newcb);
#else
			newcb = !tmpl->cbtxn;
			blkmk_init_generation(tmpl, bytes_buf(goal->generation_script), bytes_len(goal->generation_script));
#endif
			if (newcb)
			{
				ssize_t ae = blkmk_append_coinbase_safe(tmpl, &template_nonce, sizeof(template_nonce));
				if (ae < (ssize_t)sizeof(template_nonce))
					applog(LOG_WARNING, "Cannot append template-nonce to coinbase on pool %u (%"PRId64") - you might be wasting hashing!", work->pool->pool_no, (int64_t)ae);
				++template_nonce;
			}
		}
#endif
#if BLKMAKER_VERSION > 0
		{
			ssize_t ae = blkmk_append_coinbase_safe(tmpl, opt_coinbase_sig, 101);
			static bool appenderr = false;
			if (ae <= 0) {
				if (opt_coinbase_sig) {
					applog((appenderr ? LOG_DEBUG : LOG_WARNING), "Cannot append coinbase signature at all on pool %u (%"PRId64")", pool->pool_no, (int64_t)ae);
					appenderr = true;
				}
			} else if (ae >= 3 || opt_coinbase_sig) {
				const char *cbappend = opt_coinbase_sig;
				const char * const full = bfgminer_name_space_ver;
				char *need_free = NULL;
				if (!cbappend) {
					if ((size_t)ae >= sizeof(full) - 1)
						cbappend = full;
					else if ((size_t)ae >= sizeof(PACKAGE) - 1)
					{
						const char *pos = strchr(full, '-');
						size_t sz = (pos - full);
						if (pos && ae > sz)
						{
							cbappend = need_free = malloc(sz + 1);
							memcpy(need_free, full, sz);
							need_free[sz] = '\0';
						}
						else
							cbappend = PACKAGE;
					}
					else
						cbappend = "BFG";
				}
				size_t cbappendsz = strlen(cbappend);
				static bool truncatewarning = false;
				if (cbappendsz <= (size_t)ae) {
					if (cbappendsz < (size_t)ae)
						// If we have space, include the trailing \0
						++cbappendsz;
					ae = cbappendsz;
					truncatewarning = false;
				} else {
					char *tmp = malloc(ae + 1);
					memcpy(tmp, opt_coinbase_sig, ae);
					tmp[ae] = '\0';
					applog((truncatewarning ? LOG_DEBUG : LOG_WARNING),
					       "Pool %u truncating appended coinbase signature at %"PRId64" bytes: %s(%s)",
					       pool->pool_no, (int64_t)ae, tmp, &opt_coinbase_sig[ae]);
					free(tmp);
					truncatewarning = true;
				}
				ae = blkmk_append_coinbase_safe(tmpl, cbappend, ae);
				free(need_free);
				if (ae <= 0) {
					applog((appenderr ? LOG_DEBUG : LOG_WARNING), "Error appending coinbase signature (%"PRId64")", (int64_t)ae);
					appenderr = true;
				} else
					appenderr = false;
			}
		}
#endif
		if (blkmk_get_data(tmpl, work->data, 80, tv_now.tv_sec, NULL, &work->dataid) < 76)
			return false;
		swap32yes(work->data, work->data, 80 / 4);
		memcpy(&work->data[80], workpadding_bin, 48);
		
		work->ntime_roll_limits = (struct ntime_roll_limits){
			.min = tmpl->mintime,
			.max = tmpl->maxtime,
			.tv_ref = tv_now,
			.minoff = tmpl->mintimeoff,
			.maxoff = tmpl->maxtimeoff,
		};

		const struct blktmpl_longpoll_req *lp;
		mutex_lock(&pool->pool_lock);
		if ((lp = blktmpl_get_longpoll(tmpl)) && ((!pool->lp_id) || strcmp(lp->id, pool->lp_id))) {
			free(pool->lp_id);
			pool->lp_id = strdup(lp->id);

#if 0  /* This just doesn't work :( */
			curl_socket_t sock = pool->lp_socket;
			if (sock != CURL_SOCKET_BAD) {
				pool->lp_socket = CURL_SOCKET_BAD;
				applog(LOG_WARNING, "Pool %u long poll request hanging, reconnecting", pool->pool_no);
				shutdown(sock, SHUT_RDWR);
			}
#endif
		}
		mutex_unlock(&pool->pool_lock);
	}
	else
	if (unlikely(!jobj_binary(res_val, "data", work->data, sizeof(work->data), true))) {
		applog(LOG_ERR, "JSON inval data");
		return false;
	}
	else
		work_set_simple_ntime_roll_limit(work, 0, &tv_now);

	if (!jobj_binary(res_val, "midstate", work->midstate, sizeof(work->midstate), false)) {
		// Calculate it ourselves
		applog(LOG_DEBUG, "Calculating midstate locally");
		calc_midstate(work);
	}

	if (unlikely(!jobj_binary(res_val, "target", work->target, sizeof(work->target), true))) {
		applog(LOG_ERR, "JSON inval target");
		return false;
	}
	if (work->tr)
	{
		for (size_t i = 0; i < sizeof(work->target) / 2; ++i)
		{
			int p = (sizeof(work->target) - 1) - i;
			unsigned char c = work->target[i];
			work->target[i] = work->target[p];
			work->target[p] = c;
		}
	}

	if ( (tmp_val = json_object_get(res_val, "height")) ) {
		struct mining_goal_info * const goal = pool->goal;
		uint32_t blkheight = json_number_value(tmp_val);
		const void * const prevblkhash = &work->data[4];
		have_block_height(goal, prevblkhash, blkheight);
	}

	memset(work->hash, 0, sizeof(work->hash));

	work->tv_staged = tv_now;
	
#if BLKMAKER_VERSION > 6
	if (work->tr)
	{
		blktemplate_t * const tmpl = work->tr->tmpl;
		uint8_t buf[80];
		int16_t expire;
		uint8_t *cbtxn;
		size_t cbtxnsz;
		size_t cbextranonceoffset;
		int branchcount;
		libblkmaker_hash_t *branches;
		
		if (blkmk_get_mdata(tmpl, buf, sizeof(buf), tv_now.tv_sec, &expire, &cbtxn, &cbtxnsz, &cbextranonceoffset, &branchcount, &branches, GBT_XNONCESZ, false))
		{
			struct stratum_work * const swork = &pool->swork;
			const size_t branchdatasz = branchcount * 0x20;
			
			pool_check_coinbase(pool, cbtxn, cbtxnsz);
			
			cg_wlock(&pool->data_lock);
			if (swork->tr)
				tmpl_decref(swork->tr);
			swork->tr = work->tr;
			tmpl_incref(swork->tr);
			bytes_assimilate_raw(&swork->coinbase, cbtxn, cbtxnsz, cbtxnsz);
			swork->nonce2_offset = cbextranonceoffset;
			bytes_assimilate_raw(&swork->merkle_bin, branches, branchdatasz, branchdatasz);
			swork->merkles = branchcount;
			swap32yes(swork->header1, &buf[0], 36 / 4);
			swork->ntime = le32toh(*(uint32_t *)(&buf[68]));
			swork->tv_received = tv_now;
			swap32yes(swork->diffbits, &buf[72], 4 / 4);
			memcpy(swork->target, work->target, sizeof(swork->target));
			free(swork->job_id);
			swork->job_id = NULL;
			swork->clean = true;
			swork->work_restart_id = pool->work_restart_id;
			// FIXME: Do something with expire
			pool->nonce2sz = swork->n2size = GBT_XNONCESZ;
			pool->nonce2 = 0;
			cg_wunlock(&pool->data_lock);
		}
		else
			applog(LOG_DEBUG, "blkmk_get_mdata failed for pool %u", pool->pool_no);
	}
#endif  // BLKMAKER_VERSION > 6
	pool_set_opaque(pool, !work->tr);

	ret = true;

	return ret;
}

/* Returns whether the pool supports local work generation or not. */
static bool pool_localgen(struct pool *pool)
{
	return (pool->last_work_copy || pool->has_stratum);
}

int dev_from_id(int thr_id)
{
	struct cgpu_info *cgpu = get_thr_cgpu(thr_id);

	return cgpu->device_id;
}

/* Create an exponentially decaying average over the opt_log_interval */
void decay_time(double *f, double fadd, double fsecs)
{
	double ftotal, fprop;

	fprop = 1.0 - 1 / (exp(fsecs / (double)opt_log_interval));
	ftotal = 1.0 + fprop;
	*f += (fadd * fprop);
	*f /= ftotal;
}

static
int __total_staged(const bool include_spares)
{
	int tot = HASH_COUNT(staged_work);
	if (!include_spares)
		tot -= staged_spare;
	return tot;
}

static int total_staged(const bool include_spares)
{
	int ret;

	mutex_lock(stgd_lock);
	ret = __total_staged(include_spares);
	mutex_unlock(stgd_lock);

	return ret;
}

#ifdef HAVE_CURSES
WINDOW *mainwin, *statuswin, *logwin;
#endif
double total_secs = 1.0;
#ifdef HAVE_CURSES
static char statusline[256];
/* statusy is where the status window goes up to in cases where it won't fit at startup */
static int statusy;
static int devsummaryYOffset;
static int total_lines;
#endif

bool _bfg_console_cancel_disabled;
int _bfg_console_prev_cancelstate;

#ifdef HAVE_CURSES
#define   lock_curses()  bfg_console_lock()
#define unlock_curses()  bfg_console_unlock()

static bool curses_active_locked(void)
{
	bool ret;

	lock_curses();
	ret = curses_active;
	if (!ret)
		unlock_curses();
	return ret;
}

// Cancellable getch
int my_cancellable_getch(void)
{
	// This only works because the macro only hits direct getch() calls
	typedef int (*real_getch_t)(void);
	const real_getch_t real_getch = __real_getch;

	int type, rv;
	bool sct;

	sct = !pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &type);
	rv = real_getch();
	if (sct)
		pthread_setcanceltype(type, &type);

	return rv;
}

#ifdef PDCURSES
static
int bfg_wresize(WINDOW *win, int lines, int columns)
{
	int rv = wresize(win, lines, columns);
	int x, y;
	getyx(win, y, x);
	if (unlikely(y >= lines || x >= columns))
	{
		if (y >= lines)
			y = lines - 1;
		if (x >= columns)
			x = columns - 1;
		wmove(win, y, x);
	}
	return rv;
}
#else
#	define bfg_wresize wresize
#endif

#endif

void tailsprintf(char *buf, size_t bufsz, const char *fmt, ...)
{
	va_list ap;
	size_t presz = strlen(buf);
	
	va_start(ap, fmt);
	vsnprintf(&buf[presz], bufsz - presz, fmt, ap);
	va_end(ap);
}

double stats_elapsed(struct cgminer_stats *stats)
{
	struct timeval now;
	double elapsed;

	if (stats->start_tv.tv_sec == 0)
		elapsed = total_secs;
	else {
		cgtime(&now);
		elapsed = tdiff(&now, &stats->start_tv);
	}

	if (elapsed < 1.0)
		elapsed = 1.0;

	return elapsed;
}

bool drv_ready(struct cgpu_info *cgpu)
{
	switch (cgpu->status) {
		case LIFE_INIT:
		case LIFE_DEAD2:
			return false;
		default:
			return true;
	}
}

double cgpu_utility(struct cgpu_info *cgpu)
{
	double dev_runtime = cgpu_runtime(cgpu);
	return cgpu->utility = cgpu->accepted / dev_runtime * 60;
}

#define suffix_string(val, buf, bufsiz, sigdigits)  do{ \
	_Static_assert(sigdigits == 0, "suffix_string only supported with sigdigits==0");  \
	format_unit3(buf, bufsiz, FUP_DIFF, "", H2B_SHORTV, val, -1);  \
}while(0)

static float
utility_to_hashrate(double utility)
{
	return utility * 0x4444444;
}

static const char*_unitchar = "pn\xb5m kMGTPEZY?";
static const int _unitbase = 4;

static
void pick_unit(float hashrate, unsigned char *unit)
{
	unsigned char i;
	
	if (hashrate == 0 || !isfinite(hashrate))
	{
		if (*unit < _unitbase)
			*unit = _unitbase;
		return;
	}
	
	hashrate *= 1e12;
	for (i = 0; i < *unit; ++i)
		hashrate /= 1e3;
	
	// 1000 but with tolerance for floating-point rounding, avoid showing "1000.0"
	while (hashrate >= 999.95)
	{
		hashrate /= 1e3;
		if (likely(_unitchar[*unit] != '?'))
			++*unit;
	}
}
#define hashrate_pick_unit(hashrate, unit)  pick_unit(hashrate, unit)

enum h2bs_fmt {
	H2B_NOUNIT,  // "xxx.x"
	H2B_SHORT,   // "xxx.xMH/s"
	H2B_SPACED,  // "xxx.x MH/s"
	H2B_SHORTV,  // Like H2B_SHORT, but omit space for base unit
};

enum bfu_floatprec {
	FUP_INTEGER,
	FUP_HASHES,
	FUP_BTC,
	FUP_DIFF,
};

static
int format_unit3(char *buf, size_t sz, enum bfu_floatprec fprec, const char *measurement, enum h2bs_fmt fmt, float hashrate, signed char unitin)
{
	char *s = buf;
	unsigned char prec, i, unit;
	int rv = 0;
	
	if (unitin == -1)
	{
		unit = 0;
		hashrate_pick_unit(hashrate, &unit);
	}
	else
		unit = unitin;
	
	hashrate *= 1e12;
	
	for (i = 0; i < unit; ++i)
		hashrate /= 1000;
	
	switch (fprec)
	{
	case FUP_HASHES:
		// 100 but with tolerance for floating-point rounding, max "99.99" then "100.0"
		if (hashrate >= 99.995 || unit < 6)
			prec = 1;
		else
			prec = 2;
		_SNP("%5.*f", prec, hashrate);
		break;
	case FUP_INTEGER:
		_SNP("%3d", (int)hashrate);
		break;
	case FUP_BTC:
		if (hashrate >= 99.995)
			prec = 0;
		else
			prec = 2;
		_SNP("%5.*f", prec, hashrate);
		break;
	case FUP_DIFF:
		if (unit > _unitbase)
			_SNP("%.3g", hashrate);
		else
			_SNP("%u", (unsigned int)hashrate);
	}
	
	if (fmt != H2B_NOUNIT)
	{
		char uc[3] = {_unitchar[unit], '\0'};
		switch (fmt) {
			case H2B_SPACED:
				_SNP(" ");
			default:
				break;
			case H2B_SHORTV:
				if (isspace(uc[0]))
					uc[0] = '\0';
		}
		
		if (uc[0] == '\xb5')
			// Convert to UTF-8
			snprintf(uc, sizeof(uc), "%s", U8_MICRO);
		
		_SNP("%s%s", uc, measurement);
	}
	
	return rv;
}
#define format_unit2(buf, sz, floatprec, measurement, fmt, n, unit)  \
	format_unit3(buf, sz, floatprec ? FUP_HASHES : FUP_INTEGER, measurement, fmt, n, unit)

static
char *_multi_format_unit(char **buflist, size_t *bufszlist, bool floatprec, const char *measurement, enum h2bs_fmt fmt, const char *delim, int count, const float *numbers, bool isarray)
{
	unsigned char unit = 0;
	bool allzero = true;
	int i;
	size_t delimsz = 0;
	char *buf = buflist[0];
	size_t bufsz = bufszlist[0];
	size_t itemwidth = (floatprec ? 5 : 3);
	
	if (!isarray)
		delimsz = strlen(delim);
	
	for (i = 0; i < count; ++i)
		if (numbers[i] != 0)
		{
			pick_unit(numbers[i], &unit);
			allzero = false;
		}
	
	if (allzero)
		unit = _unitbase;
	
	--count;
	for (i = 0; i < count; ++i)
	{
		format_unit2(buf, bufsz, floatprec, NULL, H2B_NOUNIT, numbers[i], unit);
		if (isarray)
		{
			buf = buflist[i + 1];
			bufsz = bufszlist[i + 1];
		}
		else
		{
			buf += itemwidth;
			bufsz -= itemwidth;
			if (delimsz > bufsz)
				delimsz = bufsz;
			memcpy(buf, delim, delimsz);
			buf += delimsz;
			bufsz -= delimsz;
		}
	}
	
	// Last entry has the unit
	format_unit2(buf, bufsz, floatprec, measurement, fmt, numbers[count], unit);
	
	return buflist[0];
}
#define multi_format_unit2(buf, bufsz, floatprec, measurement, fmt, delim, count, ...)  _multi_format_unit((char *[]){buf}, (size_t[]){bufsz}, floatprec, measurement, fmt, delim, count, (float[]){ __VA_ARGS__ }, false)
#define multi_format_unit_array2(buflist, bufszlist, floatprec, measurement, fmt, count, ...)  (void)_multi_format_unit(buflist, bufszlist, floatprec, measurement, fmt, NULL, count, (float[]){ __VA_ARGS__ }, true)

static
int percentf3(char * const buf, size_t sz, double p, const double t)
{
	char *s = buf;
	int rv = 0;
	if (!p)
		_SNP("none");
	else
	if (t <= p)
		_SNP("100%%");
	else
	{

	p /= t;
	if (p < 0.00995)  // 0.01 but with tolerance for floating-point rounding, max ".99%"
		_SNP(".%02.0f%%", p * 10000);  // ".01%"
	else
	if (p < 0.0995)  // 0.1 but with tolerance for floating-point rounding, max "9.9%"
		_SNP("%.1f%%", p * 100);  // "9.1%"
	else
		_SNP("%3.0f%%", p * 100);  // " 99%"

	}
	
	return rv;
}
#define percentf4(buf, bufsz, p, t)  percentf3(buf, bufsz, p, p + t)

static
void test_decimal_width()
{
	// The pipe character at end of each line should perfectly line up
	char printbuf[512];
	char testbuf1[64];
	char testbuf2[64];
	char testbuf3[64];
	char testbuf4[64];
	double testn;
	int width;
	int saved;
	
	// Hotspots around 0.1 and 0.01
	saved = -1;
	for (testn = 0.09; testn <= 0.11; testn += 0.000001) {
		percentf3(testbuf1, sizeof(testbuf1), testn,  1.0);
		percentf3(testbuf2, sizeof(testbuf2), testn, 10.0);
		width = snprintf(printbuf, sizeof(printbuf), "%10g %s %s |", testn, testbuf1, testbuf2);
		if (unlikely((saved != -1) && (width != saved))) {
			++unittest_failures;
			applog(LOG_ERR, "Test width mismatch in percentf3! %d not %d at %10g", width, saved, testn);
			applog(LOG_ERR, "%s", printbuf);
		}
		saved = width;
	}
	
	// Hotspot around 100 (but test this in several units because format_unit2 also has unit<2 check)
	saved = -1;
	for (testn = 99.0; testn <= 101.0; testn += 0.0001) {
		format_unit2(testbuf1, sizeof(testbuf1), true, "x", H2B_SHORT, testn      , -1);
		format_unit2(testbuf2, sizeof(testbuf2), true, "x", H2B_SHORT, testn * 1e3, -1);
		format_unit2(testbuf3, sizeof(testbuf3), true, "x", H2B_SHORT, testn * 1e6, -1);
		snprintf(printbuf, sizeof(printbuf), "%10g %s %s %s |", testn, testbuf1, testbuf2, testbuf3);
		width = utf8_strlen(printbuf);
		if (unlikely((saved != -1) && (width != saved))) {
			++unittest_failures;
			applog(LOG_ERR, "Test width mismatch in format_unit2! %d not %d at %10g", width, saved, testn);
			applog(LOG_ERR, "%s", printbuf);
		}
		saved = width;
	}
	
	// Hotspot around unit transition boundary in pick_unit
	saved = -1;
	for (testn = 999.0; testn <= 1001.0; testn += 0.0001) {
		format_unit2(testbuf1, sizeof(testbuf1), true, "x", H2B_SHORT, testn      , -1);
		format_unit2(testbuf2, sizeof(testbuf2), true, "x", H2B_SHORT, testn * 1e3, -1);
		format_unit2(testbuf3, sizeof(testbuf3), true, "x", H2B_SHORT, testn * 1e6, -1);
		format_unit2(testbuf4, sizeof(testbuf4), true, "x", H2B_SHORT, testn * 1e9, -1);
		snprintf(printbuf, sizeof(printbuf), "%10g %s %s %s %s |", testn, testbuf1, testbuf2, testbuf3, testbuf4);
		width = utf8_strlen(printbuf);
		if (unlikely((saved != -1) && (width != saved))) {
			++unittest_failures;
			applog(LOG_ERR, "Test width mismatch in pick_unit! %d not %d at %10g", width, saved, testn);
			applog(LOG_ERR, "%s", printbuf);
		}
		saved = width;
	}
}

#ifdef HAVE_CURSES
static void adj_width(int var, int *length);
#endif

#ifdef HAVE_CURSES
static int awidth = 1, rwidth = 1, swidth = 1, hwwidth = 1;

static
void format_statline(char *buf, size_t bufsz, const char *cHr, const char *aHr, const char *uHr, int accepted, int rejected, int stale, double wnotaccepted, double waccepted, int hwerrs, double bad_diff1, double allnonces)
{
	char rejpcbuf[6];
	char bnbuf[6];
	
	adj_width(accepted, &awidth);
	adj_width(rejected, &rwidth);
	adj_width(stale, &swidth);
	adj_width(hwerrs, &hwwidth);
	percentf4(rejpcbuf, sizeof(rejpcbuf), wnotaccepted, waccepted);
	percentf3(bnbuf, sizeof(bnbuf), bad_diff1, allnonces);
	
	tailsprintf(buf, bufsz, "%s/%s/%s | A:%*d R:%*d+%*d(%s) HW:%*d/%s",
	            cHr, aHr, uHr,
	            awidth, accepted,
	            rwidth, rejected,
	            swidth, stale,
	            rejpcbuf,
	            hwwidth, hwerrs,
	            bnbuf
	);
}

static
const char *pool_proto_str(const struct pool * const pool)
{
	if (pool->idle)
		return "Dead ";
	if (pool->has_stratum)
		return "Strtm";
	if (pool->lp_url && pool->proto != pool->lp_proto)
		return "Mixed";
	switch (pool->proto)
	{
		case PLP_GETBLOCKTEMPLATE:
			return " GBT ";
		case PLP_GETWORK:
			return "GWork";
		default:
			return "Alive";
	}
}

#endif

static inline
void temperature_column(char *buf, size_t bufsz, bool maybe_unicode, const float * const temp)
{
	if (!(use_unicode && have_unicode_degrees))
		maybe_unicode = false;
	if (temp && *temp > 0.)
		if (maybe_unicode)
			snprintf(buf, bufsz, "%4.1f"U8_DEGREE"C", *temp);
		else
			snprintf(buf, bufsz, "%4.1fC", *temp);
	else
	{
		if (temp)
			snprintf(buf, bufsz, "     ");
		if (maybe_unicode)
			tailsprintf(buf, bufsz, " ");
	}
	tailsprintf(buf, bufsz, " | ");
}

void get_statline3(char *buf, size_t bufsz, struct cgpu_info *cgpu, bool for_curses, bool opt_show_procs)
{
#ifndef HAVE_CURSES
	assert(for_curses == false);
#endif
	struct device_drv *drv = cgpu->drv;
	enum h2bs_fmt hashrate_style = for_curses ? H2B_SHORT : H2B_SPACED;
	char cHr[ALLOC_H2B_NOUNIT+1], aHr[ALLOC_H2B_NOUNIT+1], uHr[max(ALLOC_H2B_SHORT, ALLOC_H2B_SPACED)+3+1];
	char rejpcbuf[6];
	char bnbuf[6];
	double dev_runtime;
	
	if (!opt_show_procs)
		cgpu = cgpu->device;
	
	dev_runtime = cgpu_runtime(cgpu);
	
	double rolling, mhashes;
	int accepted, rejected, stale;
	double waccepted;
	double wnotaccepted;
	int hwerrs;
	double bad_diff1, good_diff1;
	
	rolling = mhashes = waccepted = wnotaccepted = 0;
	accepted = rejected = stale = hwerrs = bad_diff1 = good_diff1 = 0;
	
	{
		struct cgpu_info *slave = cgpu;
		for (int i = 0; i < cgpu->procs; ++i, (slave = slave->next_proc))
		{
			slave->utility = slave->accepted / dev_runtime * 60;
			slave->utility_diff1 = slave->diff_accepted / dev_runtime * 60;
			
			rolling += drv->get_proc_rolling_hashrate ? drv->get_proc_rolling_hashrate(slave) : slave->rolling;
			mhashes += slave->total_mhashes;
			if (opt_weighed_stats)
			{
				accepted += slave->diff_accepted;
				rejected += slave->diff_rejected;
				stale += slave->diff_stale;
			}
			else
			{
				accepted += slave->accepted;
				rejected += slave->rejected;
				stale += slave->stale;
			}
			waccepted += slave->diff_accepted;
			wnotaccepted += slave->diff_rejected + slave->diff_stale;
			hwerrs += slave->hw_errors;
			bad_diff1 += slave->bad_diff1;
			good_diff1 += slave->diff1;
			
			if (opt_show_procs)
				break;
		}
	}

	double wtotal = (waccepted + wnotaccepted);
	
	multi_format_unit_array2(
		((char*[]){cHr, aHr, uHr}),
		((size_t[]){sizeof(cHr), sizeof(aHr), sizeof(uHr)}),
		true, "h/s", hashrate_style,
		3,
		1e6*rolling,
		1e6*mhashes / dev_runtime,
		utility_to_hashrate(good_diff1 * (wtotal ? (waccepted / wtotal) : 1) * 60 / dev_runtime));

	// Processor representation
#ifdef HAVE_CURSES
	if (for_curses)
	{
		if (opt_show_procs)
			snprintf(buf, bufsz, " %*s: ", -(5 + max_lpdigits), cgpu->proc_repr);
		else
			snprintf(buf, bufsz, " %s: ", cgpu->dev_repr);
	}
	else
#endif
	{
		if (opt_show_procs)
			snprintf(buf, bufsz, "%*s ", -(5 + max_lpdigits), cgpu->proc_repr_ns);
		else
			snprintf(buf, bufsz, "%-5s ", cgpu->dev_repr_ns);
	}
	
	if (include_serial_in_statline && cgpu->dev_serial)
		tailsprintf(buf, bufsz, "[serial=%s] ", cgpu->dev_serial);
	
	if (unlikely(cgpu->status == LIFE_INIT))
	{
		tailsprintf(buf, bufsz, "Initializing...");
		return;
	}
	
	{
		const size_t bufln = strlen(buf);
		const size_t abufsz = (bufln >= bufsz) ? 0 : (bufsz - bufln);
		
		if (likely(cgpu->status != LIFE_DEAD2) && drv->override_statline_temp2 && drv->override_statline_temp2(buf, bufsz, cgpu, opt_show_procs))
			temperature_column(&buf[bufln], abufsz, for_curses, NULL);
		else
		{
			float temp = cgpu->temp;
			if (!opt_show_procs)
			{
				// Find the highest temperature of all processors
				struct cgpu_info *proc = cgpu;
				for (int i = 0; i < cgpu->procs; ++i, (proc = proc->next_proc))
					if (proc->temp > temp)
						temp = proc->temp;
			}
			temperature_column(&buf[bufln], abufsz, for_curses, &temp);
		}
	}
	
#ifdef HAVE_CURSES
	if (for_curses)
	{
		const char *cHrStatsOpt[] = {AS_BAD("DEAD "), AS_BAD("SICK "), "OFF  ", AS_BAD("REST "), AS_BAD(" ERR "), AS_BAD("WAIT "), cHr};
		const char *cHrStats;
		int cHrStatsI = (sizeof(cHrStatsOpt) / sizeof(*cHrStatsOpt)) - 1;
		bool all_dead = true, all_off = true, all_rdrv = true;
		struct cgpu_info *proc = cgpu;
		for (int i = 0; i < cgpu->procs; ++i, (proc = proc->next_proc))
		{
			switch (cHrStatsI) {
				default:
					if (proc->status == LIFE_WAIT)
						cHrStatsI = 5;
				case 5:
					if (proc->deven == DEV_RECOVER_ERR)
						cHrStatsI = 4;
				case 4:
					if (proc->deven == DEV_RECOVER)
						cHrStatsI = 3;
				case 3:
					if (proc->status == LIFE_SICK || proc->status == LIFE_DEAD || proc->status == LIFE_DEAD2)
					{
						cHrStatsI = 1;
						all_off = false;
					}
					else
					{
						if (likely(proc->deven == DEV_ENABLED))
							all_off = false;
						if (proc->deven != DEV_RECOVER_DRV)
							all_rdrv = false;
					}
				case 1:
					break;
			}
			if (likely(proc->status != LIFE_DEAD && proc->status != LIFE_DEAD2))
				all_dead = false;
			if (opt_show_procs)
				break;
		}
		if (unlikely(all_dead))
			cHrStatsI = 0;
		else
		if (unlikely(all_off))
			cHrStatsI = 2;
		cHrStats = cHrStatsOpt[cHrStatsI];
		if (cHrStatsI == 2 && all_rdrv)
			cHrStats = " RST ";
		
		format_statline(buf, bufsz,
		                cHrStats,
		                aHr, uHr,
		                accepted, rejected, stale,
		                wnotaccepted, waccepted,
		                hwerrs,
		                bad_diff1, bad_diff1 + good_diff1);
	}
	else
#endif
	{
		percentf4(rejpcbuf, sizeof(rejpcbuf), wnotaccepted, waccepted);
		percentf4(bnbuf, sizeof(bnbuf), bad_diff1, good_diff1);
		tailsprintf(buf, bufsz, "%ds:%s avg:%s u:%s | A:%d R:%d+%d(%s) HW:%d/%s",
			opt_log_interval,
			cHr, aHr, uHr,
			accepted,
			rejected,
			stale,
			rejpcbuf,
			hwerrs,
			bnbuf
		);
	}
}

#define get_statline(buf, bufsz, cgpu)               get_statline3(buf, bufsz, cgpu, false, opt_show_procs)
#define get_statline2(buf, bufsz, cgpu, for_curses)  get_statline3(buf, bufsz, cgpu, for_curses, opt_show_procs)

static void text_print_status(int thr_id)
{
	struct cgpu_info *cgpu;
	char logline[256];

	cgpu = get_thr_cgpu(thr_id);
	if (cgpu) {
		get_statline(logline, sizeof(logline), cgpu);
		printf("\n%s\r", logline);
		fflush(stdout);
	}
}

#ifdef HAVE_CURSES
static int attr_bad = A_BOLD;

#ifdef WIN32
#define swprintf snwprintf
#endif

static
void bfg_waddstr(WINDOW *win, const char *s)
{
	const char *p = s;
	int32_t w;
	int wlen;
	unsigned char stop_ascii = (use_unicode ? '|' : 0x80);
	
	while (true)
	{
		while (likely(p[0] == '\n' || (p[0] >= 0x20 && p[0] < stop_ascii)))
		{
			// Printable ASCII
			++p;
		}
		if (p != s)
			waddnstr(win, s, p - s);
		w = utf8_decode(p, &wlen);
		s = p += wlen;
		switch(w)
		{
			// NOTE: U+F000-U+F7FF are reserved for font hacks
			case '\0':
				return;
			case 0xb5:  // micro symbol
				w = unicode_micro;
				goto default_addch;
			case 0xf000:  // "bad" off
				wattroff(win, attr_bad);
				break;
			case 0xf001:  // "bad" on
				wattron(win, attr_bad);
				break;
#ifdef USE_UNICODE
			case '|':
				wadd_wch(win, WACS_VLINE);
				break;
#endif
			case 0x2500:  // BOX DRAWINGS LIGHT HORIZONTAL
			case 0x2534:  // BOX DRAWINGS LIGHT UP AND HORIZONTAL
				if (!use_unicode)
				{
					waddch(win, '-');
					break;
				}
#ifdef USE_UNICODE
				wadd_wch(win, (w == 0x2500) ? WACS_HLINE : WACS_BTEE);
				break;
#endif
			case 0x2022:
				if (w > WCHAR_MAX || !iswprint(w))
					w = '*';
			default:
default_addch:
				if (w > WCHAR_MAX || !(iswprint(w) || w == '\n'))
				{
#if REPLACEMENT_CHAR <= WCHAR_MAX
					if (iswprint(REPLACEMENT_CHAR))
						w = REPLACEMENT_CHAR;
					else
#endif
						w = '?';
				}
				{
#ifdef USE_UNICODE
					wchar_t wbuf[0x10];
					int wbuflen = sizeof(wbuf) / sizeof(*wbuf);
					wbuflen = swprintf(wbuf, wbuflen, L"%lc", (wint_t)w);
					waddnwstr(win, wbuf, wbuflen);
#else
					wprintw(win, "%lc", (wint_t)w);
#endif
				}
		}
	}
}

static inline
void bfg_hline(WINDOW *win, int y)
{
	int maxx, __maybe_unused maxy;
	getmaxyx(win, maxy, maxx);
#ifdef USE_UNICODE
	if (use_unicode)
		mvwhline_set(win, y, 0, WACS_HLINE, maxx);
	else
#endif
		mvwhline(win, y, 0, '-', maxx);
}

static
int bfg_win_linelen(WINDOW * const win)
{
	int maxx;
	int __maybe_unused y;
	getmaxyx(win, y, maxx);
	return maxx;
}

// Spaces until end of line, using current attributes (ie, not completely clear)
static
void bfg_wspctoeol(WINDOW * const win, const int offset)
{
	int x, maxx;
	int __maybe_unused y;
	getmaxyx(win, y, maxx);
	getyx(win, y, x);
	const int space_count = (maxx - x) - offset;
	
	// Check for negative - terminal too narrow
	if (space_count <= 0)
		return;
	
	char buf[space_count];
	memset(buf, ' ', space_count);
	waddnstr(win, buf, space_count);
}

static int menu_attr = A_REVERSE;

#define CURBUFSIZ 256
#define cg_mvwprintw(win, y, x, fmt, ...) do { \
	char tmp42[CURBUFSIZ]; \
	snprintf(tmp42, sizeof(tmp42), fmt, ##__VA_ARGS__); \
	wmove(win, y, x);  \
	bfg_waddstr(win, tmp42); \
} while (0)
#define cg_wprintw(win, fmt, ...) do { \
	char tmp42[CURBUFSIZ]; \
	snprintf(tmp42, sizeof(tmp42), fmt, ##__VA_ARGS__); \
	bfg_waddstr(win, tmp42); \
} while (0)

static
void update_block_display_line(const int blky, struct mining_goal_info *goal)
{
	struct blockchain_info * const blkchain = goal->blkchain;
	struct block_info * const blkinfo = blkchain->currentblk;
	double income;
	char incomestr[ALLOC_H2B_SHORT+6+1];
	
	if (blkinfo->height)
	{
		income = goal->diff_accepted * 3600 * blkchain->currentblk_subsidy / total_secs / goal->current_diff;
		format_unit3(incomestr, sizeof(incomestr), FUP_BTC, "BTC/hr", H2B_SHORT, income/1e8, -1);
	}
	else
		strcpy(incomestr, "?");
	
	int linelen = bfg_win_linelen(statuswin);
	wmove(statuswin, blky, 0);
	
	bfg_waddstr(statuswin, " Block");
	if (!goal->is_default)
		linelen -= strlen(goal->name) + 1;
	linelen -= 6;  // " Block"
	
	if (blkinfo->height && blkinfo->height < 1000000)
	{
		cg_wprintw(statuswin, " #%6u", blkinfo->height);
		linelen -= 8;
	}
	bfg_waddstr(statuswin, ":");
	
	if (linelen > 55)
		bfg_waddstr(statuswin, " ");
	if (linelen >= 65)
		bfg_waddstr(statuswin, "...");
	
	{
		char hexpbh[0x11];
		if (!(blkinfo->height && blkinfo->height < 1000000))
		{
			bin2hex(hexpbh, &blkinfo->prevblkhash[4], 4);
			bfg_waddstr(statuswin, hexpbh);
		}
		bin2hex(hexpbh, &blkinfo->prevblkhash[0], 4);
		bfg_waddstr(statuswin, hexpbh);
	}
	
	if (linelen >= 55)
		bfg_waddstr(statuswin, " ");
	
	cg_wprintw(statuswin, " Diff:%s", goal->current_diff_str);
	
	if (linelen >= 69)
		bfg_waddstr(statuswin, " ");
	
	cg_wprintw(statuswin, "(%s) ", goal->net_hashrate);
	
	if (linelen >= 62)
	{
		if (linelen >= 69)
			bfg_waddstr(statuswin, " ");
		bfg_waddstr(statuswin, "Started:");
	}
	else
		bfg_waddstr(statuswin, "S:");
	if (linelen >= 69)
		bfg_waddstr(statuswin, " ");
	
	bfg_waddstr(statuswin, blkchain->currentblk_first_seen_time_str);
	
	if (linelen >= 69)
		bfg_waddstr(statuswin, " ");
	
	cg_wprintw(statuswin, " I:%s", incomestr);
	
	if (!goal->is_default)
		cg_wprintw(statuswin, " %s", goal->name);
	
	wclrtoeol(statuswin);
}

static bool pool_actively_in_use(const struct pool *, const struct pool *);

static
void update_block_display(const bool within_console_lock)
{
	struct mining_goal_info *goal, *tmpgoal;
	int blky = 3, i, total_found_goals = 0;
	if (!within_console_lock)
		if (!curses_active_locked())
			return;
	HASH_ITER(hh, mining_goals, goal, tmpgoal)
	{
		for (i = 0; i < total_pools; ++i)
		{
			struct pool * const pool = pools[i];
			if (pool->goal == goal && pool_actively_in_use(pool, NULL))
				break;
		}
		if (i >= total_pools)
			// no pools using this goal, so it's probably stale anyway
			continue;
		update_block_display_line(blky++, goal);
		++total_found_goals;
	}
	
	// We cannot do resizing if called within someone else's console lock
	if (within_console_lock)
		return;
	
	bfg_console_unlock();
	if (total_found_goals != active_goals)
	{
		active_goals = total_found_goals;
		devcursor = 7 + active_goals;
		switch_logsize();
	}
}

static bool pool_unworkable(const struct pool *);

/* Must be called with curses mutex lock held and curses_active */
static void curses_print_status(const int ts)
{
	struct pool *pool = currentpool;
	struct timeval now, tv;
	float efficiency;
	int logdiv;

	efficiency = total_bytes_xfer ? total_diff_accepted * 2048. / total_bytes_xfer : 0.0;

	wattron(statuswin, attr_title);
	const int linelen = bfg_win_linelen(statuswin);
	int titlelen = 1 + strlen(PACKAGE) + 1 + strlen(bfgminer_ver) + 3 + 21 + 3 + 19;
	cg_mvwprintw(statuswin, 0, 0, " " PACKAGE " ");
	if (titlelen + 17 < linelen)
		cg_wprintw(statuswin, "version ");
	cg_wprintw(statuswin, "%s - ", bfgminer_ver);
	if (titlelen + 9 < linelen)
		cg_wprintw(statuswin, "Started: ");
	else
	if (titlelen + 7 <= linelen)
		cg_wprintw(statuswin, "Start: ");
	cg_wprintw(statuswin, "%s", datestamp);
	timer_set_now(&now);
	{
		unsigned int days, hours;
		div_t d;
		
		timersub(&now, &miner_started, &tv);
		d = div(tv.tv_sec, 86400);
		days = d.quot;
		d = div(d.rem, 3600);
		hours = d.quot;
		d = div(d.rem, 60);
		cg_wprintw(statuswin, " - [%3u day%c %02d:%02d:%02d]"
			, days
			, (days == 1) ? ' ' : 's'
			, hours
			, d.quot
			, d.rem
		);
	}
	bfg_wspctoeol(statuswin, 0);
	wattroff(statuswin, attr_title);
	
	wattron(statuswin, menu_attr);
	wmove(statuswin, 1, 0);
	bfg_waddstr(statuswin, " [M]anage devices [P]ool management [S]ettings [D]isplay options ");
	bfg_wspctoeol(statuswin, 14);
	bfg_waddstr(statuswin, "[H]elp [Q]uit ");
	wattroff(statuswin, menu_attr);

	if ((pool_strategy == POOL_LOADBALANCE  || pool_strategy == POOL_BALANCE) && enabled_pools > 1) {
		char poolinfo[20], poolinfo2[20];
		int poolinfooff = 0, poolinfo2off, workable_pools = 0;
		double lowdiff = DBL_MAX, highdiff = -1;
		struct pool *lowdiff_pool = pools[0], *highdiff_pool = pools[0];
		time_t oldest_work_restart = time(NULL) + 1;
		struct pool *oldest_work_restart_pool = pools[0];
		for (int i = 0; i < total_pools; ++i)
		{
			if (pool_unworkable(pools[i]))
				continue;
			
			// NOTE: Only set pool var when it's workable; if only one is, it gets used by single-pool code
			pool = pools[i];
			++workable_pools;
			
			if (poolinfooff < sizeof(poolinfo))
				poolinfooff += snprintf(&poolinfo[poolinfooff], sizeof(poolinfo) - poolinfooff, "%u,", pool->pool_no);
			
			struct cgminer_pool_stats * const pool_stats = &pool->cgminer_pool_stats;
			if (pool_stats->last_diff < lowdiff)
			{
				lowdiff = pool_stats->last_diff;
				lowdiff_pool = pool;
			}
			if (pool_stats->last_diff > highdiff)
			{
				highdiff = pool_stats->last_diff;
				highdiff_pool = pool;
			}
			
			if (oldest_work_restart >= pool->work_restart_time)
			{
				oldest_work_restart = pool->work_restart_time;
				oldest_work_restart_pool = pool;
			}
		}
		if (unlikely(!workable_pools))
			goto no_workable_pools;
		if (workable_pools == 1)
			goto one_workable_pool;
		poolinfo2off = snprintf(poolinfo2, sizeof(poolinfo2), "%u (", workable_pools);
		if (poolinfooff > sizeof(poolinfo2) - poolinfo2off - 1)
			snprintf(&poolinfo2[poolinfo2off], sizeof(poolinfo2) - poolinfo2off, "%.*s...)", (int)(sizeof(poolinfo2) - poolinfo2off - 5), poolinfo);
		else
			snprintf(&poolinfo2[poolinfo2off], sizeof(poolinfo2) - poolinfo2off, "%.*s)%*s", (int)(poolinfooff - 1), poolinfo, (int)(sizeof(poolinfo2)), "");
		cg_mvwprintw(statuswin, 2, 0, " Pools: %s  Diff:%s%s%s  %c  LU:%s",
		             poolinfo2,
		             lowdiff_pool->diff,
		             (lowdiff == highdiff) ? "" : "-",
		             (lowdiff == highdiff) ? "" : highdiff_pool->diff,
		             pool->goal->have_longpoll ? '+' : '-',
		             oldest_work_restart_pool->work_restart_timestamp);
	}
	else
	if (pool_unworkable(pool))
	{
no_workable_pools: ;
		wattron(statuswin, attr_bad);
		cg_mvwprintw(statuswin, 2, 0, " (all pools are dead) ");
		wattroff(statuswin, attr_bad);
	}
	else
	{
one_workable_pool: ;
		char pooladdr[19];
		{
			const char *rawaddr = pool->sockaddr_url;
			BFGINIT(rawaddr, pool->rpc_url);
			size_t pooladdrlen = strlen(rawaddr);
			if (pooladdrlen > 20)
				snprintf(pooladdr, sizeof(pooladdr), "...%s", &rawaddr[pooladdrlen - (sizeof(pooladdr) - 4)]);
			else
				snprintf(pooladdr, sizeof(pooladdr), "%*s", -(int)(sizeof(pooladdr) - 1), rawaddr);
		}
		cg_mvwprintw(statuswin, 2, 0, " Pool%2u: %s  Diff:%s  %c%s  LU:%s  User:%s",
		             pool->pool_no, pooladdr, pool->diff,
		             pool->goal->have_longpoll ? '+' : '-', pool_proto_str(pool),
		             pool->work_restart_timestamp,
		             pool->rpc_user);
	}
	wclrtoeol(statuswin);
	
	update_block_display(true);
	
	char bwstr[(ALLOC_H2B_SHORT*2)+3+1];
	
	cg_mvwprintw(statuswin, devcursor - 4, 0, " ST:%d  F:%d  NB:%d  AS:%d  BW:[%s]  E:%.2f  BS:%s",
		ts,
		total_go + total_ro,
		new_blocks,
		total_submitting,
		multi_format_unit2(bwstr, sizeof(bwstr),
		                   false, "B/s", H2B_SHORT, "/", 2,
		                  (float)(total_bytes_rcvd / total_secs),
		                  (float)(total_bytes_sent / total_secs)),
		efficiency,
		best_share);
	wclrtoeol(statuswin);
	
	mvwaddstr(statuswin, devcursor - 3, 0, " ");
	bfg_waddstr(statuswin, statusline);
	wclrtoeol(statuswin);
	
	int devdiv = devcursor - 2;
	logdiv = statusy - 1;
	bfg_hline(statuswin, devdiv);
	bfg_hline(statuswin, logdiv);
#ifdef USE_UNICODE
	if (use_unicode)
	{
		int offset = 8 /* device */ + 5 /* temperature */ + 1 /* padding space */;
		if (opt_show_procs && !opt_compact)
			offset += max_lpdigits;  // proc letter(s)
		if (have_unicode_degrees)
			++offset;  // degrees symbol
		mvwadd_wch(statuswin, devdiv, offset, WACS_PLUS);
		mvwadd_wch(statuswin, logdiv, offset, WACS_BTEE);
		offset += 24;  // hashrates etc
		mvwadd_wch(statuswin, devdiv, offset, WACS_PLUS);
		mvwadd_wch(statuswin, logdiv, offset, WACS_BTEE);
	}
#endif
}

static void adj_width(int var, int *length)
{
	if ((int)(log10(var) + 1) > *length)
		(*length)++;
}

static int dev_width;

static void curses_print_devstatus(struct cgpu_info *cgpu)
{
	char logline[256];
	int ypos;

	if (opt_compact)
		return;

	/* Check this isn't out of the window size */
	if (opt_show_procs)
	ypos = cgpu->cgminer_id;
	else
	{
		if (cgpu->proc_id)
			return;
		ypos = cgpu->device_line_id;
	}
	ypos += devsummaryYOffset;
	if (ypos < 0)
		return;
	ypos += devcursor - 1;
	if (ypos >= statusy - 1)
		return;

	if (wmove(statuswin, ypos, 0) == ERR)
		return;
	
	get_statline2(logline, sizeof(logline), cgpu, true);
	if (selecting_device && (opt_show_procs ? (selected_device == cgpu->cgminer_id) : (devices[selected_device]->device == cgpu)))
		wattron(statuswin, A_REVERSE);
	bfg_waddstr(statuswin, logline);
	wattroff(statuswin, A_REVERSE);

	wclrtoeol(statuswin);
}

static
void _refresh_devstatus(const bool already_have_lock) {
	if ((!opt_compact) && (already_have_lock || curses_active_locked())) {
		int i;
		if (unlikely(!total_devices))
		{
			const int ypos = devcursor - 1;
			if (ypos < statusy - 1 && wmove(statuswin, ypos, 0) != ERR)
			{
				wattron(statuswin, attr_bad);
				bfg_waddstr(statuswin, "NO DEVICES FOUND: Press 'M' and '+' to add");
				wclrtoeol(statuswin);
				wattroff(statuswin, attr_bad);
			}
		}
		for (i = 0; i < total_devices; i++)
			curses_print_devstatus(get_devices(i));
		touchwin(statuswin);
		wrefresh(statuswin);
		if (!already_have_lock)
			unlock_curses();
	}
}
#define refresh_devstatus() _refresh_devstatus(false)

#endif

static void print_status(int thr_id)
{
	if (!curses_active)
		text_print_status(thr_id);
}

#ifdef HAVE_CURSES
static
bool set_statusy(int maxy)
{
	if (loginput_size)
	{
		maxy -= loginput_size;
		if (maxy < 0)
			maxy = 0;
	}
	
	if (logstart < maxy)
		maxy = logstart;
	
	if (statusy == maxy)
		return false;
	
	statusy = maxy;
	logcursor = statusy;
	
	return true;
}

/* Check for window resize. Called with curses mutex locked */
static inline void change_logwinsize(void)
{
	int x, y, logx, logy;

	getmaxyx(mainwin, y, x);
	if (x < 80 || y < 25)
		return;

	if (y > statusy + 2 && statusy < logstart) {
		set_statusy(y - 2);
		mvwin(logwin, logcursor, 0);
		bfg_wresize(statuswin, statusy, x);
	}

	y -= logcursor;
	getmaxyx(logwin, logy, logx);
	/* Detect screen size change */
	if (x != logx || y != logy)
		bfg_wresize(logwin, y, x);
}

static void check_winsizes(void)
{
	if (!use_curses)
		return;
	if (curses_active_locked()) {
		int y, x;

		x = getmaxx(statuswin);
		if (set_statusy(LINES - 2))
		{
			erase();
			bfg_wresize(statuswin, statusy, x);
			getmaxyx(mainwin, y, x);
			y -= logcursor;
			bfg_wresize(logwin, y, x);
			mvwin(logwin, logcursor, 0);
		}
		unlock_curses();
	}
}

static int device_line_id_count;

static void switch_logsize(void)
{
	if (curses_active_locked()) {
		if (opt_compact) {
			logstart = devcursor - 1;
			logcursor = logstart + 1;
		} else {
			total_lines = (opt_show_procs ? total_devices : device_line_id_count) ?: 1;
			logstart = devcursor + total_lines;
			logcursor = logstart;
		}
		unlock_curses();
	}
	check_winsizes();
	update_block_display(false);
}

/* For mandatory printing when mutex is already locked */
void _wlog(const char *str)
{
	static bool newline;
	size_t end = strlen(str) - 1;
	
	if (newline)
		bfg_waddstr(logwin, "\n");
	
	if (str[end] == '\n')
	{
		char *s;
		
		newline = true;
		s = alloca(end + 1);
		memcpy(s, str, end);
		s[end] = '\0';
		str = s;
	}
	else
		newline = false;
	
	bfg_waddstr(logwin, str);
}

/* Mandatory printing */
void _wlogprint(const char *str)
{
	if (curses_active_locked()) {
		_wlog(str);
		unlock_curses();
	}
}
#endif

#ifdef HAVE_CURSES
bool _log_curses_only(int prio, const char *datetime, const char *str)
{
	bool high_prio;

	high_prio = (prio == LOG_WARNING || prio == LOG_ERR);

	if (curses_active)
	{
		if (!loginput_size || high_prio) {
			wlog(" %s %s\n", datetime, str);
			if (high_prio) {
				touchwin(logwin);
				wrefresh(logwin);
			}
		}
		return true;
	}
	return false;
}

void clear_logwin(void)
{
	if (curses_active_locked()) {
		wclear(logwin);
		unlock_curses();
	}
}

void logwin_update(void)
{
	if (curses_active_locked()) {
		touchwin(logwin);
		wrefresh(logwin);
		unlock_curses();
	}
}
#endif

void enable_pool(struct pool * const pool)
{
	if (pool->enabled != POOL_ENABLED) {
		mutex_lock(&lp_lock);
		enabled_pools++;
		pool->enabled = POOL_ENABLED;
		pthread_cond_broadcast(&lp_cond);
		mutex_unlock(&lp_lock);
		if (pool->prio < current_pool()->prio)
			switch_pools(pool);
	}
}

void manual_enable_pool(struct pool * const pool)
{
	pool->failover_only = false;
	BFGINIT(pool->quota, 1);
	enable_pool(pool);
}

void disable_pool(struct pool * const pool, const enum pool_enable enable_status)
{
	if (pool->enabled == POOL_DISABLED)
		/* had been manually disabled before */
		return;
	
	if (pool->enabled != POOL_ENABLED)
	{
		/* has been programmatically disabled already, just change to the new status directly */
		pool->enabled = enable_status;
		return;
	}
	
	/* Fall into the lock area */
	mutex_lock(&lp_lock);
	--enabled_pools;
	pool->enabled = enable_status;
	mutex_unlock(&lp_lock);
	
	if (pool == current_pool())
		switch_pools(NULL);
}

static
void share_result_msg(const struct work *work, const char *disp, const char *reason, bool resubmit, const char *worktime) {
	struct cgpu_info *cgpu;
	const struct mining_algorithm * const malgo = work_mining_algorithm(work);
	const unsigned char *hashpart = &work->hash[0x1c - malgo->ui_skip_hash_bytes];
	char shrdiffdisp[ALLOC_H2B_SHORTV];
	const double tgtdiff = work->work_difficulty;
	char tgtdiffdisp[ALLOC_H2B_SHORTV];
	char where[20];
	
	cgpu = get_thr_cgpu(work->thr_id);
	
	suffix_string(work->share_diff, shrdiffdisp, sizeof(shrdiffdisp), 0);
	suffix_string(tgtdiff, tgtdiffdisp, sizeof(tgtdiffdisp), 0);
	
	if (total_pools > 1)
		snprintf(where, sizeof(where), " pool %d", work->pool->pool_no);
	else
		where[0] = '\0';
	
	applog(LOG_NOTICE, "%s %02x%02x%02x%02x %"PRIprepr"%s Diff %s/%s%s %s%s",
	       disp,
	       (unsigned)hashpart[3], (unsigned)hashpart[2], (unsigned)hashpart[1], (unsigned)hashpart[0],
	       cgpu->proc_repr,
	       where,
	       shrdiffdisp, tgtdiffdisp,
	       reason,
	       resubmit ? "(resubmit)" : "",
	       worktime
	);
}

static bool test_work_current(struct work *);
static void _submit_work_async(struct work *);

static
void maybe_local_submit(const struct work *work)
{
#if BLKMAKER_VERSION > 3
	if (unlikely(work->block && work->tr))
	{
		// This is a block with a full template (GBT)
		// Regardless of the result, submit to local bitcoind(s) as well
		struct work *work_cp;
		
		for (int i = 0; i < total_pools; ++i)
		{
			if (!uri_get_param_bool(pools[i]->rpc_url, "allblocks", false))
				continue;
			
			applog(LOG_DEBUG, "Attempting submission of full block to pool %d", pools[i]->pool_no);
			work_cp = copy_work(work);
			work_cp->pool = pools[i];
			work_cp->do_foreign_submit = true;
			_submit_work_async(work_cp);
		}
	}
#endif
}

static
json_t *extract_reject_reason_j(json_t * const val, json_t *res, json_t * const err, const struct work * const work)
{
	if (json_is_string(res))
		return res;
	if ( (res = json_object_get(val, "reject-reason")) )
		return res;
	if (work->stratum && err && json_is_array(err) && json_array_size(err) >= 2 && (res = json_array_get(err, 1)) && json_is_string(res))
		return res;
	return NULL;
}

static
const char *extract_reject_reason(json_t * const val, json_t *res, json_t * const err, const struct work * const work)
{
	json_t * const j = extract_reject_reason_j(val, res, err, work);
	return j ? json_string_value(j) : NULL;
}

static
int put_in_parens(char * const buf, const size_t bufsz, const char * const s)
{
	if (!s)
	{
		if (bufsz)
			buf[0] = '\0';
		return 0;
	}
	
	int p = snprintf(buf, bufsz, " (%s", s);
	if (p >= bufsz - 1)
		p = bufsz - 2;
	strcpy(&buf[p], ")");
	return p + 1;
}

/* Theoretically threads could race when modifying accepted and
 * rejected values but the chance of two submits completing at the
 * same time is zero so there is no point adding extra locking */
static void
share_result(json_t *val, json_t *res, json_t *err, const struct work *work,
	     /*char *hashshow,*/ bool resubmit, char *worktime)
{
	struct pool *pool = work->pool;
	struct cgpu_info *cgpu;

	cgpu = get_thr_cgpu(work->thr_id);

	if ((json_is_null(err) || !err) && (json_is_null(res) || json_is_true(res))) {
		struct mining_goal_info * const goal = pool->goal;
		
		mutex_lock(&stats_lock);
		cgpu->accepted++;
		total_accepted++;
		pool->accepted++;
		cgpu->diff_accepted += work->work_difficulty;
		total_diff_accepted += work->work_difficulty;
		pool->diff_accepted += work->work_difficulty;
		goal->diff_accepted += work->work_difficulty;
		mutex_unlock(&stats_lock);

		pool->seq_rejects = 0;
		cgpu->last_share_pool = pool->pool_no;
		cgpu->last_share_pool_time = time(NULL);
		cgpu->last_share_diff = work->work_difficulty;
		pool->last_share_time = cgpu->last_share_pool_time;
		pool->last_share_diff = work->work_difficulty;
		applog(LOG_DEBUG, "PROOF OF WORK RESULT: true (yay!!!)");
		if (!QUIET) {
			share_result_msg(work, "Accepted", "", resubmit, worktime);
		}
		sharelog("accept", work);
		if (opt_shares && total_diff_accepted >= opt_shares) {
			applog(LOG_WARNING, "Successfully mined %g accepted shares as requested and exiting.", opt_shares);
			kill_work();
			return;
		}

		/* Detect if a pool that has been temporarily disabled for
		 * continually rejecting shares has started accepting shares.
		 * This will only happen with the work returned from a
		 * longpoll */
		if (unlikely(pool->enabled == POOL_REJECTING)) {
			applog(LOG_WARNING, "Rejecting pool %d now accepting shares, re-enabling!", pool->pool_no);
			enable_pool(pool);
		}

		if (unlikely(work->block)) {
			// Force moving on to this new block :)
			struct work fakework;
			memset(&fakework, 0, sizeof(fakework));
			fakework.pool = work->pool;

			// Copy block version, bits, and time from share
			memcpy(&fakework.data[ 0], &work->data[ 0], 4);
			memcpy(&fakework.data[68], &work->data[68], 8);

			// Set prevblock to winning hash (swap32'd)
			swap32yes(&fakework.data[4], &work->hash[0], 32 / 4);

			test_work_current(&fakework);
		}
	}
	else
	if (!hash_target_check(work->hash, work->target))
	{
		// This was submitted despite failing the proper target
		// Quietly ignore the reject
		char reason[32];
		put_in_parens(reason, sizeof(reason), extract_reject_reason(val, res, err, work));
		applog(LOG_DEBUG, "Share above target rejected%s by pool %u as expected, ignoring", reason, pool->pool_no);
		
		// Stratum error 23 is "low difficulty share", which suggests this pool tracks job difficulty correctly.
		// Therefore, we disable retrodiff if it was enabled-by-default.
		if (pool->pool_diff_effective_retroactively == BTS_UNKNOWN) {
			json_t *errnum;
			if (work->stratum && err && json_is_array(err) && json_array_size(err) >= 1 && (errnum = json_array_get(err, 0)) && json_is_number(errnum) && ((int)json_number_value(errnum)) == 23) {
				applog(LOG_DEBUG, "Disabling retroactive difficulty adjustments for pool %u", pool->pool_no);
				pool->pool_diff_effective_retroactively = false;
			}
		}
	} else {
		mutex_lock(&stats_lock);
		cgpu->rejected++;
		total_rejected++;
		pool->rejected++;
		cgpu->diff_rejected += work->work_difficulty;
		total_diff_rejected += work->work_difficulty;
		pool->diff_rejected += work->work_difficulty;
		pool->seq_rejects++;
		mutex_unlock(&stats_lock);

		applog(LOG_DEBUG, "PROOF OF WORK RESULT: false (booooo)");
		if (!QUIET) {
			char disposition[36] = "reject";
			char reason[32];

			const char *reasontmp = extract_reject_reason(val, res, err, work);
			int n = put_in_parens(reason, sizeof(reason), reasontmp);
			if (reason[0])
				snprintf(&disposition[6], sizeof(disposition) - 6, ":%.*s", n - 3, &reason[2]);

			share_result_msg(work, "Rejected", reason, resubmit, worktime);
			sharelog(disposition, work);
		}

		/* Once we have more than a nominal amount of sequential rejects,
		 * at least 10 and more than 3 mins at the current utility,
		 * disable the pool because some pool error is likely to have
		 * ensued. Do not do this if we know the share just happened to
		 * be stale due to networking delays.
		 */
		if (pool->seq_rejects > 10 && !work->stale && opt_disable_pool && enabled_pools > 1) {
			double utility = total_accepted / total_secs * 60;

			if (pool->seq_rejects > utility * 3) {
				applog(LOG_WARNING, "Pool %d rejected %d sequential shares, disabling!",
				       pool->pool_no, pool->seq_rejects);
				disable_pool(pool, POOL_REJECTING);
				pool->seq_rejects = 0;
			}
		}
	}
	
	maybe_local_submit(work);
}

static char *submit_upstream_work_request(struct work *work)
{
	char *hexstr = NULL;
	char *s, *sd;
	struct pool *pool = work->pool;

	if (work->tr)
	{
		blktemplate_t * const tmpl = work->tr->tmpl;
		json_t *req;
		unsigned char data[80];
		
		swap32yes(data, work->data, 80 / 4);
#if BLKMAKER_VERSION > 6
		if (work->stratum) {
			req = blkmk_submitm_jansson(tmpl, data, bytes_buf(&work->nonce2), bytes_len(&work->nonce2), le32toh(*((uint32_t*)&work->data[76])), work->do_foreign_submit);
		} else
#endif
#if BLKMAKER_VERSION > 3
		if (work->do_foreign_submit)
			req = blkmk_submit_foreign_jansson(tmpl, data, work->dataid, le32toh(*((uint32_t*)&work->data[76])));
		else
#endif
			req = blkmk_submit_jansson(tmpl, data, work->dataid, le32toh(*((uint32_t*)&work->data[76])));
		s = json_dumps(req, 0);
		json_decref(req);
		sd = malloc(161);
		bin2hex(sd, data, 80);
	} else {

	/* build hex string */
		hexstr = malloc((sizeof(work->data) * 2) + 1);
		bin2hex(hexstr, work->data, sizeof(work->data));

	/* build JSON-RPC request */
		s = strdup("{\"method\": \"getwork\", \"params\": [ \"");
		s = realloc_strcat(s, hexstr);
		s = realloc_strcat(s, "\" ], \"id\":1}");

		free(hexstr);
		sd = s;

	}

	applog(LOG_DEBUG, "DBG: sending %s submit RPC call: %s", pool->rpc_url, sd);
	if (work->tr)
		free(sd);
	else
		s = realloc_strcat(s, "\n");

	return s;
}

static bool submit_upstream_work_completed(struct work *work, bool resubmit, struct timeval *ptv_submit, json_t *val) {
	json_t *res, *err;
	bool rc = false;
	int thr_id = work->thr_id;
	struct pool *pool = work->pool;
	struct timeval tv_submit_reply;
	time_t ts_submit_reply;
	char worktime[200] = "";

	cgtime(&tv_submit_reply);
	ts_submit_reply = time(NULL);

	if (unlikely(!val)) {
		applog(LOG_INFO, "submit_upstream_work json_rpc_call failed");
		if (!pool_tset(pool, &pool->submit_fail)) {
			total_ro++;
			pool->remotefail_occasions++;
			applog(LOG_WARNING, "Pool %d communication failure, caching submissions", pool->pool_no);
		}
		goto out;
	} else if (pool_tclear(pool, &pool->submit_fail))
		applog(LOG_WARNING, "Pool %d communication resumed, submitting work", pool->pool_no);

	res = json_object_get(val, "result");
	err = json_object_get(val, "error");

	if (!QUIET) {
		if (opt_worktime) {
			char workclone[20];
			struct tm _tm;
			struct tm *tm, tm_getwork, tm_submit_reply;
			tm = &_tm;
			double getwork_time = tdiff((struct timeval *)&(work->tv_getwork_reply),
							(struct timeval *)&(work->tv_getwork));
			double getwork_to_work = tdiff((struct timeval *)&(work->tv_work_start),
							(struct timeval *)&(work->tv_getwork_reply));
			double work_time = tdiff((struct timeval *)&(work->tv_work_found),
							(struct timeval *)&(work->tv_work_start));
			double work_to_submit = tdiff(ptv_submit,
							(struct timeval *)&(work->tv_work_found));
			double submit_time = tdiff(&tv_submit_reply, ptv_submit);
			int diffplaces = 3;

			localtime_r(&work->ts_getwork, tm);
			memcpy(&tm_getwork, tm, sizeof(struct tm));
			localtime_r(&ts_submit_reply, tm);
			memcpy(&tm_submit_reply, tm, sizeof(struct tm));

			if (work->clone) {
				snprintf(workclone, sizeof(workclone), "C:%1.3f",
						tdiff((struct timeval *)&(work->tv_cloned),
						(struct timeval *)&(work->tv_getwork_reply)));
			}
			else
				strcpy(workclone, "O");

			if (work->work_difficulty < 1)
				diffplaces = 6;

			const struct mining_algorithm * const malgo = work_mining_algorithm(work);
			const uint8_t * const prevblkhash = &work->data[4];
			snprintf(worktime, sizeof(worktime),
				" <-%08lx.%08lx M:%c D:%1.*f G:%02d:%02d:%02d:%1.3f %s (%1.3f) W:%1.3f (%1.3f) S:%1.3f R:%02d:%02d:%02d",
				(unsigned long)be32toh(((uint32_t *)prevblkhash)[7 - malgo->worktime_skip_prevblk_u32]),
				(unsigned long)be32toh(((uint32_t *)prevblkhash)[6 - malgo->worktime_skip_prevblk_u32]),
				work->getwork_mode, diffplaces, work->work_difficulty,
				tm_getwork.tm_hour, tm_getwork.tm_min,
				tm_getwork.tm_sec, getwork_time, workclone,
				getwork_to_work, work_time, work_to_submit, submit_time,
				tm_submit_reply.tm_hour, tm_submit_reply.tm_min,
				tm_submit_reply.tm_sec);
		}
	}

	share_result(val, res, err, work, resubmit, worktime);

	if (!opt_realquiet)
		print_status(thr_id);
	if (!want_per_device_stats) {
		char logline[256];
		struct cgpu_info *cgpu;

		cgpu = get_thr_cgpu(thr_id);
		
		get_statline(logline, sizeof(logline), cgpu);
		applog(LOG_INFO, "%s", logline);
	}

	json_decref(val);

	rc = true;
out:
	return rc;
}

/* Specifies whether we can use this pool for work or not. */
static bool pool_unworkable(const struct pool * const pool)
{
	if (pool->idle)
		return true;
	if (pool->enabled != POOL_ENABLED)
		return true;
	if (pool->has_stratum && !pool->stratum_active)
		return true;
	return false;
}

static struct pool *priority_pool(int);
static bool pool_unusable(struct pool *);

static
bool pool_actively_desired(const struct pool * const pool, const struct pool *cp)
{
	if (pool->enabled != POOL_ENABLED)
		return false;
	if (pool_strategy == POOL_LOADBALANCE && pool->quota)
		return true;
	if (pool_strategy == POOL_BALANCE && !pool->failover_only)
		return true;
	if (!cp)
		cp = current_pool();
	if (pool == cp)
		return true;
	
	// If we are the highest priority, workable pool for a given algorithm, we are needed
	struct mining_algorithm * const malgo = pool->goal->malgo;
	for (int i = 0; i < total_pools; ++i)
	{
		struct pool * const other_pool = priority_pool(i);
		if (other_pool == pool)
			return true;
		if (pool_unusable(other_pool))
			continue;
		if (other_pool->goal->malgo == malgo)
			break;
	}
	
	return false;
}

static
bool pool_actively_in_use(const struct pool * const pool, const struct pool *cp)
{
	return (!pool_unworkable(pool)) && pool_actively_desired(pool, cp);
}

static
bool pool_supports_block_change_notification(struct pool * const pool)
{
	return pool->has_stratum || pool->lp_url;
}

static
bool pool_has_active_block_change_notification(struct pool * const pool)
{
	return pool->stratum_active || pool->lp_active;
}

static struct pool *_select_longpoll_pool(struct pool *, bool(*)(struct pool *));
#define select_longpoll_pool(pool)  _select_longpoll_pool(pool, pool_supports_block_change_notification)
#define pool_active_lp_pool(pool)  _select_longpoll_pool(pool, pool_has_active_block_change_notification)

/* In balanced mode, the amount of diff1 solutions per pool is monitored as a
 * rolling average per 10 minutes and if pools start getting more, it biases
 * away from them to distribute work evenly. The share count is reset to the
 * rolling average every 10 minutes to not send all work to one pool after it
 * has been disabled/out for an extended period. */
static
struct pool *select_balanced(struct pool *cp, struct mining_algorithm * const malgo)
{
	int i, lowest = cp->shares;
	struct pool *ret = cp, *failover_pool = NULL;

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		if (malgo && pool->goal->malgo != malgo)
			continue;
		if (pool_unworkable(pool))
			continue;
		if (pool->failover_only)
		{
			BFGINIT(failover_pool, pool);
			continue;
		}
		if (pool->shares < lowest) {
			lowest = pool->shares;
			ret = pool;
		}
	}
	if (malgo && ret->goal->malgo != malgo)
		// Yes, we want failover_pool even if it's NULL
		ret = failover_pool;
	else
	if (pool_unworkable(ret) && failover_pool)
		ret = failover_pool;

	if (ret)
		++ret->shares;
	return ret;
}

static
struct pool *select_loadbalance(struct mining_algorithm * const malgo)
{
	static int rotating_pool = 0;
	struct pool *pool;
	bool avail = false;
	int tested, i, rpsave;

	for (i = 0; i < total_pools; i++) {
		struct pool *tp = pools[i];

		if (tp->quota_used < tp->quota_gcd) {
			avail = true;
			break;
		}
	}

	/* There are no pools with quota, so reset them. */
	if (!avail) {
		for (i = 0; i < total_pools; i++)
		{
			struct pool * const tp = pools[i];
			tp->quota_used -= tp->quota_gcd;
		}
		if (++rotating_pool >= total_pools)
			rotating_pool = 0;
	}

	/* Try to find the first pool in the rotation that is usable */
	// Look for the lowest integer quota_used / quota_gcd in case we are imbalanced by algorithm demands
	struct pool *pool_lowest = NULL;
	int lowest = INT_MAX;
	rpsave = rotating_pool;
	for (tested = 0; tested < total_pools; ++tested)
	{
		pool = pools[rotating_pool];
		if (malgo && pool->goal->malgo != malgo)
			goto continue_tested;
		
		if (pool->quota_used < pool->quota_gcd)
		{
			++pool->quota_used;
			if (!pool_unworkable(pool))
				goto out;
			/* Failover-only flag for load-balance means distribute
			 * unused quota to priority pool 0. */
			if (opt_fail_only)
				priority_pool(0)->quota_used--;
		}
		if (malgo)
		{
			const int count = pool->quota_used / pool->quota_gcd;
			if (count < lowest)
			{
				pool_lowest = pool;
				lowest = count;
			}
		}
		
continue_tested: ;
		if (++rotating_pool >= total_pools)
			rotating_pool = 0;
	}
	
	// Even if pool_lowest is NULL, we want to return that to indicate failure
	// Note it isn't possible to get here if !malgo
	pool = pool_lowest;
	
out: ;
	// Restore rotating_pool static, so malgo searches don't affect the usual load balancing
	if (malgo)
		rotating_pool = rpsave;
	
	return pool;
}

static
struct pool *select_failover(struct mining_algorithm * const malgo)
{
	int i;
	
	for (i = 0; i < total_pools; i++) {
		struct pool *tp = priority_pool(i);
		
		if (malgo && tp->goal->malgo != malgo)
			continue;
		
		if (!pool_unusable(tp)) {
			return tp;
		}
	}
	
	return NULL;
}

static bool pool_active(struct pool *, bool pinging);
static void pool_died(struct pool *);

/* Select any active pool in a rotating fashion when loadbalance is chosen if
 * it has any quota left. */
static inline struct pool *select_pool(bool lagging, struct mining_algorithm * const malgo)
{
	struct pool *pool = NULL, *cp;

retry:
	cp = current_pool();

	if (pool_strategy == POOL_BALANCE) {
		pool = select_balanced(cp, malgo);
		if ((!pool) || pool_unworkable(pool))
			goto simple_failover;
		goto out;
	}

	if (pool_strategy != POOL_LOADBALANCE && (!lagging || opt_fail_only)) {
		if (malgo && cp->goal->malgo != malgo)
			goto simple_failover;
		pool = cp;
		goto out;
	} else
		pool = select_loadbalance(malgo);

simple_failover:
	/* If there are no alive pools with quota, choose according to
	 * priority. */
	if (!pool) {
		pool = select_failover(malgo);
	}

	/* If still nothing is usable, use the current pool */
	if (!pool)
	{
		if (malgo && cp->goal->malgo != malgo)
		{
			applog(LOG_DEBUG, "Failed to select pool for specific mining algorithm '%s'", malgo->name);
			return NULL;
		}
		pool = cp;
	}

out:
	if (!pool_actively_in_use(pool, cp))
	{
		if (!pool_active(pool, false))
		{
			pool_died(pool);
			goto retry;
		}
		pool_tclear(pool, &pool->idle);
	}
	applog(LOG_DEBUG, "Selecting pool %d for %s%swork", pool->pool_no, malgo ? malgo->name : "", malgo ? " " : "");
	return pool;
}

static double DIFFEXACTONE = 26959946667150639794667015087019630673637144422540572481103610249215.0;

double target_diff(const unsigned char *target)
{
	double targ = 0;
	signed int i;

	for (i = 31; i >= 0; --i)
		targ = (targ * 0x100) + target[i];

	return DIFFEXACTONE / (targ ?: 1);
}

/*
 * Calculate the work share difficulty
 */
static void calc_diff(struct work *work, int known)
{
	struct cgminer_pool_stats *pool_stats = &(work->pool->cgminer_pool_stats);
	double difficulty;

	if (!known) {
		work->work_difficulty = target_diff(work->target);
	} else
		work->work_difficulty = known;
	difficulty = work->work_difficulty;

	pool_stats->last_diff = difficulty;
	suffix_string(difficulty, work->pool->diff, sizeof(work->pool->diff), 0);

	if (difficulty == pool_stats->min_diff)
		pool_stats->min_diff_count++;
	else if (difficulty < pool_stats->min_diff || pool_stats->min_diff == 0) {
		pool_stats->min_diff = difficulty;
		pool_stats->min_diff_count = 1;
	}

	if (difficulty == pool_stats->max_diff)
		pool_stats->max_diff_count++;
	else if (difficulty > pool_stats->max_diff) {
		pool_stats->max_diff = difficulty;
		pool_stats->max_diff_count = 1;
	}
}

static void gen_stratum_work(struct pool *, struct work *);
static void pool_update_work_restart_time(struct pool *);
static void restart_threads(void);

static uint32_t benchmark_blkhdr[20];
static const int benchmark_update_interval = 1;

static
void *benchmark_intense_work_update_thread(void *userp)
{
	pthread_detach(pthread_self());
	RenameThread("benchmark-intense");
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	
	struct pool * const pool = userp;
	struct stratum_work * const swork = &pool->swork;
	uint8_t * const blkhdr = swork->header1;
	
	while (true)
	{
		sleep(benchmark_update_interval);
		
		cg_wlock(&pool->data_lock);
		for (int i = 36; --i >= 0; )
			if (++blkhdr[i])
				break;
		cg_wunlock(&pool->data_lock);
		
		struct work *work = make_work();
		gen_stratum_work(pool, work);
		pool->swork.work_restart_id = ++pool->work_restart_id;
		pool_update_work_restart_time(pool);
		test_work_current(work);
		free_work(work);
		
		restart_threads();
	}
	return NULL;
}

static
void setup_benchmark_pool()
{
	struct pool *pool;
	
	want_longpoll = false;
	
	// Temporarily disable opt_benchmark to avoid auto-removal
	opt_benchmark = false;
	pool = add_pool();
	opt_benchmark = true;
	
	pool->rpc_url = malloc(255);
	strcpy(pool->rpc_url, "Benchmark");
	pool_set_uri(pool, pool->rpc_url);
	pool->rpc_user = pool->rpc_url;
	pool->rpc_pass = pool->rpc_url;
	enable_pool(pool);
	pool->idle = false;
	successful_connect = true;
	
	{
		uint32_t * const blkhdr = benchmark_blkhdr;
		blkhdr[2] = htobe32(1);
		blkhdr[17] = htobe32(0x7fffffff);  // timestamp
		blkhdr[18] = htobe32(0x1700ffff);  // "bits"
	}
	
	{
		struct stratum_work * const swork = &pool->swork;
		const int branchcount = 15;  // 1 MB block
		const size_t branchdatasz = branchcount * 0x20;
		const size_t coinbase_sz = (opt_benchmark_intense ? 250 : 6) * 1024;
		
		bytes_resize(&swork->coinbase, coinbase_sz);
		memset(bytes_buf(&swork->coinbase), '\xff', coinbase_sz);
		swork->nonce2_offset = 0;
		
		bytes_resize(&swork->merkle_bin, branchdatasz);
		memset(bytes_buf(&swork->merkle_bin), '\xff', branchdatasz);
		swork->merkles = branchcount;
		
		swork->header1[0] = '\xff';
		memset(&swork->header1[1], '\0', 34);
		swork->header1[35] = '\x01';
		swork->ntime = 0x7fffffff;
		timer_unset(&swork->tv_received);
		memcpy(swork->diffbits, "\x17\0\xff\xff", 4);
		const struct mining_goal_info * const goal = get_mining_goal("default");
		const struct mining_algorithm * const malgo = goal->malgo;
		set_target_to_pdiff(swork->target, malgo->reasonable_low_nonce_diff);
		pool->nonce2sz = swork->n2size = GBT_XNONCESZ;
		pool->nonce2 = 0;
	}
	
	if (opt_benchmark_intense)
	{
		pthread_t pth;
		if (unlikely(pthread_create(&pth, NULL, benchmark_intense_work_update_thread, pool)))
			applog(LOG_WARNING, "Failed to start benchmark intense work update thread");
	}
}

void get_benchmark_work(struct work *work, bool use_swork)
{
	if (use_swork)
	{
		struct timeval tv_now;
		timer_set_now(&tv_now);
		gen_stratum_work(pools[0], work);
		work->getwork_mode = GETWORK_MODE_BENCHMARK;
		work_set_simple_ntime_roll_limit(work, 0, &tv_now);
		return;
	}
	
	struct pool * const pool = pools[0];
	uint32_t * const blkhdr = benchmark_blkhdr;
	for (int i = 16; i >= 0; --i)
		if (++blkhdr[i])
			break;
	
	memcpy(&work->data[ 0], blkhdr, 80);
	memcpy(&work->data[80], workpadding_bin, 48);
	char hex[161];
	bin2hex(hex, work->data, 80);
	applog(LOG_DEBUG, "Generated benchmark header %s", hex);
	calc_midstate(work);
	memcpy(work->target, pool->swork.target, sizeof(work->target));
	
	work->mandatory = true;
	work->pool = pools[0];
	cgtime(&work->tv_getwork);
	copy_time(&work->tv_getwork_reply, &work->tv_getwork);
	copy_time(&work->tv_staged, &work->tv_getwork);
	work->getwork_mode = GETWORK_MODE_BENCHMARK;
	calc_diff(work, 0);
	work_set_simple_ntime_roll_limit(work, 60, &work->tv_getwork);
}

static void wake_gws(void);

static void update_last_work(struct work *work)
{
	if (!work->tr)
		// Only save GBT jobs, since rollntime isn't coordinated well yet
		return;

	struct pool *pool = work->pool;
	mutex_lock(&pool->last_work_lock);
	if (pool->last_work_copy)
		free_work(pool->last_work_copy);
	pool->last_work_copy = copy_work(work);
	pool->last_work_copy->work_restart_id = pool->work_restart_id;
	mutex_unlock(&pool->last_work_lock);
}

static
void gbt_req_target(json_t *req)
{
	json_t *j;
	json_t *n;
	
	if (!request_target_str)
		return;
	
	j = json_object_get(req, "params");
	if (!j)
	{
		n = json_array();
		if (!n)
			return;
		if (json_object_set_new(req, "params", n))
			goto erradd;
		j = n;
	}
	
	n = json_array_get(j, 0);
	if (!n)
	{
		n = json_object();
		if (!n)
			return;
		if (json_array_append_new(j, n))
			goto erradd;
	}
	j = n;
	
	n = json_string(request_target_str);
	if (!n)
		return;
	if (json_object_set_new(j, "target", n))
		goto erradd;
	
	return;

erradd:
	json_decref(n);
}

static char *prepare_rpc_req2(struct work *work, enum pool_protocol proto, const char *lpid, bool probe, struct pool * const pool)
{
	char *rpc_req;

	clean_work(work);
	switch (proto) {
		case PLP_GETWORK:
			work->getwork_mode = GETWORK_MODE_POOL;
			return strdup(getwork_req);
		case PLP_GETBLOCKTEMPLATE:
			work->getwork_mode = GETWORK_MODE_GBT;
			blktemplate_t * const tmpl = blktmpl_create();
			if (!tmpl)
				goto gbtfail2;
			work->tr = tmpl_makeref(tmpl);
			gbt_capabilities_t caps = blktmpl_addcaps(tmpl);
			if (!caps)
				goto gbtfail;
			caps |= GBT_LONGPOLL;
#if BLKMAKER_VERSION > 1
			const struct mining_goal_info * const goal = pool->goal;
			if (goal->generation_script || goal_has_at_least_one_getcbaddr(goal))
				caps |= GBT_CBVALUE;
#endif
			json_t *req = blktmpl_request_jansson(caps, lpid);
			if (!req)
				goto gbtfail;
			
			if (probe)
				gbt_req_target(req);
			
			rpc_req = json_dumps(req, 0);
			if (!rpc_req)
				goto gbtfail;
			json_decref(req);
			return rpc_req;
		default:
			return NULL;
	}
	return NULL;

gbtfail:
	tmpl_decref(work->tr);
	work->tr = NULL;
gbtfail2:
	return NULL;
}

#define prepare_rpc_req(work, proto, lpid, pool)  prepare_rpc_req2(work, proto, lpid, false, pool)
#define prepare_rpc_req_probe(work, proto, lpid, pool)  prepare_rpc_req2(work, proto, lpid, true, pool)

static const char *pool_protocol_name(enum pool_protocol proto)
{
	switch (proto) {
		case PLP_GETBLOCKTEMPLATE:
			return "getblocktemplate";
		case PLP_GETWORK:
			return "getwork";
		default:
			return "UNKNOWN";
	}
}

static enum pool_protocol pool_protocol_fallback(enum pool_protocol proto)
{
	switch (proto) {
		case PLP_GETBLOCKTEMPLATE:
			if (want_getwork)
			return PLP_GETWORK;
		default:
			return PLP_NONE;
	}
}

static bool get_upstream_work(struct work *work, CURL *curl)
{
	struct pool *pool = work->pool;
	struct cgminer_pool_stats *pool_stats = &(pool->cgminer_pool_stats);
	struct timeval tv_elapsed;
	json_t *val = NULL;
	bool rc = false;
	char *url;
	enum pool_protocol proto;

	char *rpc_req;

	if (pool->proto == PLP_NONE)
		pool->proto = PLP_GETBLOCKTEMPLATE;

tryagain:
	rpc_req = prepare_rpc_req(work, pool->proto, NULL, pool);
	work->pool = pool;
	if (!rpc_req)
		return false;

	applog(LOG_DEBUG, "DBG: sending %s get RPC call: %s", pool->rpc_url, rpc_req);

	url = pool->rpc_url;

	cgtime(&work->tv_getwork);

	val = json_rpc_call(curl, url, pool->rpc_userpass, rpc_req, false,
			    false, &work->rolltime, pool, false);
	pool_stats->getwork_attempts++;

	free(rpc_req);

	if (likely(val)) {
		rc = work_decode(pool, work, val);
		if (unlikely(!rc))
			applog(LOG_DEBUG, "Failed to decode work in get_upstream_work");
	} else if (PLP_NONE != (proto = pool_protocol_fallback(pool->proto))) {
		applog(LOG_WARNING, "Pool %u failed getblocktemplate request; falling back to getwork protocol", pool->pool_no);
		pool->proto = proto;
		goto tryagain;
	} else
		applog(LOG_DEBUG, "Failed json_rpc_call in get_upstream_work");

	cgtime(&work->tv_getwork_reply);
	timersub(&(work->tv_getwork_reply), &(work->tv_getwork), &tv_elapsed);
	pool_stats->getwork_wait_rolling += ((double)tv_elapsed.tv_sec + ((double)tv_elapsed.tv_usec / 1000000)) * 0.63;
	pool_stats->getwork_wait_rolling /= 1.63;

	timeradd(&tv_elapsed, &(pool_stats->getwork_wait), &(pool_stats->getwork_wait));
	if (timercmp(&tv_elapsed, &(pool_stats->getwork_wait_max), >)) {
		pool_stats->getwork_wait_max.tv_sec = tv_elapsed.tv_sec;
		pool_stats->getwork_wait_max.tv_usec = tv_elapsed.tv_usec;
	}
	if (timercmp(&tv_elapsed, &(pool_stats->getwork_wait_min), <)) {
		pool_stats->getwork_wait_min.tv_sec = tv_elapsed.tv_sec;
		pool_stats->getwork_wait_min.tv_usec = tv_elapsed.tv_usec;
	}
	pool_stats->getwork_calls++;

	work->pool = pool;
	work->longpoll = false;
	calc_diff(work, 0);
	total_getworks++;
	pool->getwork_requested++;

	if (rc)
		update_last_work(work);

	if (likely(val))
		json_decref(val);

	return rc;
}

#ifdef HAVE_CURSES
static void disable_curses(void)
{
	if (curses_active_locked()) {
		use_curses = false;
		curses_active = false;
		leaveok(logwin, false);
		leaveok(statuswin, false);
		leaveok(mainwin, false);
		nocbreak();
		echo();
		delwin(logwin);
		delwin(statuswin);
		delwin(mainwin);
		endwin();
#ifdef WIN32
		// Move the cursor to after curses output.
		HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		COORD coord;

		if (GetConsoleScreenBufferInfo(hout, &csbi)) {
			coord.X = 0;
			coord.Y = csbi.dwSize.Y - 1;
			SetConsoleCursorPosition(hout, coord);
		}
#endif
		unlock_curses();
	}
}
#endif

static void __kill_work(void)
{
	struct cgpu_info *cgpu;
	struct thr_info *thr;
	int i;

	if (!successful_connect)
		return;

	applog(LOG_INFO, "Received kill message");

	shutting_down = true;

	applog(LOG_DEBUG, "Prompting submit_work thread to finish");
	notifier_wake(submit_waiting_notifier);

#ifdef USE_LIBMICROHTTPD
	httpsrv_stop();
#endif
	
	applog(LOG_DEBUG, "Killing off watchpool thread");
	/* Kill the watchpool thread */
	thr = &control_thr[watchpool_thr_id];
	thr_info_cancel(thr);

	applog(LOG_DEBUG, "Killing off watchdog thread");
	/* Kill the watchdog thread */
	thr = &control_thr[watchdog_thr_id];
	thr_info_cancel(thr);

	applog(LOG_DEBUG, "Shutting down mining threads");
	for (i = 0; i < mining_threads; i++) {
		thr = get_thread(i);
		if (!thr)
			continue;
		cgpu = thr->cgpu;
		if (!cgpu)
			continue;
		if (!cgpu->threads)
			continue;

		cgpu->shutdown = true;
		thr->work_restart = true;
		notifier_wake(thr->notifier);
		notifier_wake(thr->work_restart_notifier);
	}

	sleep(1);

	applog(LOG_DEBUG, "Killing off mining threads");
	/* Kill the mining threads*/
	for (i = 0; i < mining_threads; i++) {
		thr = get_thread(i);
		if (!thr)
			continue;
		cgpu = thr->cgpu;
		if (cgpu->threads)
		{
			applog(LOG_WARNING, "Killing %"PRIpreprv, thr->cgpu->proc_repr);
			thr_info_cancel(thr);
		}
		cgpu->status = LIFE_DEAD2;
	}

	/* Stop the others */
	applog(LOG_DEBUG, "Killing off API thread");
	thr = &control_thr[api_thr_id];
	thr_info_cancel(thr);
}

/* This should be the common exit path */
void kill_work(void)
{
	__kill_work();

	quit(0, "Shutdown signal received.");
}

static
#ifdef WIN32
#ifndef _WIN64
const
#endif
#endif
char **initial_args;

void _bfg_clean_up(bool);

void app_restart(void)
{
	applog(LOG_WARNING, "Attempting to restart %s", packagename);

	__kill_work();
	_bfg_clean_up(true);

#if defined(unix) || defined(__APPLE__)
	if (forkpid > 0) {
		kill(forkpid, SIGTERM);
		forkpid = 0;
	}
#endif

	execv(initial_args[0], initial_args);
	applog(LOG_WARNING, "Failed to restart application");
}

static void sighandler(int __maybe_unused sig)
{
	/* Restore signal handlers so we can still quit if kill_work fails */
	sigaction(SIGTERM, &termhandler, NULL);
	sigaction(SIGINT, &inthandler, NULL);
	kill_work();
}

static void start_longpoll(void);
static void stop_longpoll(void);

/* Called with pool_lock held. Recruit an extra curl if none are available for
 * this pool. */
static void recruit_curl(struct pool *pool)
{
	struct curl_ent *ce = calloc(sizeof(struct curl_ent), 1);

	if (unlikely(!ce))
		quit(1, "Failed to calloc in recruit_curl");

	ce->curl = curl_easy_init();
	if (unlikely(!ce->curl))
		quit(1, "Failed to init in recruit_curl");

	LL_PREPEND(pool->curllist, ce);
	pool->curls++;
}

/* Grab an available curl if there is one. If not, then recruit extra curls
 * unless we are in a submit_fail situation, or we have opt_delaynet enabled
 * and there are already 5 curls in circulation. Limit total number to the
 * number of mining threads per pool as well to prevent blasting a pool during
 * network delays/outages. */
static struct curl_ent *pop_curl_entry3(struct pool *pool, int blocking)
{
	int curl_limit = opt_delaynet ? 5 : (mining_threads + opt_queue) * 2;
	bool recruited = false;
	struct curl_ent *ce;

	mutex_lock(&pool->pool_lock);
retry:
	if (!pool->curls) {
		recruit_curl(pool);
		recruited = true;
	} else if (!pool->curllist) {
		if (blocking < 2 && pool->curls >= curl_limit && (blocking || pool->curls >= opt_submit_threads)) {
			if (!blocking) {
				mutex_unlock(&pool->pool_lock);
				return NULL;
			}
			pthread_cond_wait(&pool->cr_cond, &pool->pool_lock);
			goto retry;
		} else {
			recruit_curl(pool);
			recruited = true;
		}
	}
	ce = pool->curllist;
	LL_DELETE(pool->curllist, ce);
	mutex_unlock(&pool->pool_lock);

	if (recruited)
		applog(LOG_DEBUG, "Recruited curl for pool %d", pool->pool_no);
	return ce;
}

static struct curl_ent *pop_curl_entry2(struct pool *pool, bool blocking)
{
	return pop_curl_entry3(pool, blocking ? 1 : 0);
}

__maybe_unused
static struct curl_ent *pop_curl_entry(struct pool *pool)
{
	return pop_curl_entry3(pool, 1);
}

static void push_curl_entry(struct curl_ent *ce, struct pool *pool)
{
	mutex_lock(&pool->pool_lock);
	if (!ce || !ce->curl)
		quithere(1, "Attempted to add NULL");
	LL_PREPEND(pool->curllist, ce);
	cgtime(&ce->tv);
	pthread_cond_broadcast(&pool->cr_cond);
	mutex_unlock(&pool->pool_lock);
}

static inline bool should_roll(struct work *work)
{
	struct timeval now;
	time_t expiry;

	if (!pool_actively_in_use(work->pool, NULL))
		return false;

	if (stale_work(work, false))
		return false;

	if (work->rolltime > opt_scantime)
		expiry = work->rolltime;
	else
		expiry = opt_scantime;
	expiry = expiry * 2 / 3;

	/* We shouldn't roll if we're unlikely to get one shares' duration
	 * work out of doing so */
	cgtime(&now);
	if (now.tv_sec - work->tv_staged.tv_sec > expiry)
		return false;
	
	return true;
}

/* Limit rolls to 7000 to not beyond 2 hours in the future where bitcoind will
 * reject blocks as invalid. */
static inline bool can_roll(struct work *work)
{
	if (work->stratum)
		return false;
	if (!(work->pool && !work->clone))
		return false;
	if (work->tr)
	{
		if (stale_work(work, false))
			return false;
		return blkmk_work_left(work->tr->tmpl);
	}
	return (work->rolltime &&
		work->rolls < 7000 && !stale_work(work, false));
}

static void roll_work(struct work *work)
{
	if (work->tr)
	{
		struct timeval tv_now;
		cgtime(&tv_now);
		if (blkmk_get_data(work->tr->tmpl, work->data, 80, tv_now.tv_sec, NULL, &work->dataid) < 76)
			applog(LOG_ERR, "Failed to get next data from template; spinning wheels!");
		swap32yes(work->data, work->data, 80 / 4);
		calc_midstate(work);
		applog(LOG_DEBUG, "Successfully rolled extranonce to dataid %u", work->dataid);
	} else {

	uint32_t *work_ntime;
	uint32_t ntime;

	work_ntime = (uint32_t *)(work->data + 68);
	ntime = be32toh(*work_ntime);
	ntime++;
	*work_ntime = htobe32(ntime);
		work_set_simple_ntime_roll_limit(work, 0, &work->ntime_roll_limits.tv_ref);

		applog(LOG_DEBUG, "Successfully rolled time header in work");
	}

	local_work++;
	work->rolls++;
	work->blk.nonce = 0;

	/* This is now a different work item so it needs a different ID for the
	 * hashtable */
	work->id = total_work++;
}

/* Duplicates any dynamically allocated arrays within the work struct to
 * prevent a copied work struct from freeing ram belonging to another struct */
static void _copy_work(struct work *work, const struct work *base_work, int noffset)
{
	int id = work->id;

	clean_work(work);
	memcpy(work, base_work, sizeof(struct work));
	/* Keep the unique new id assigned during make_work to prevent copied
	 * work from having the same id. */
	work->id = id;
	if (base_work->job_id)
		work->job_id = strdup(base_work->job_id);
	if (base_work->nonce1)
		work->nonce1 = strdup(base_work->nonce1);
	bytes_cpy(&work->nonce2, &base_work->nonce2);

	if (base_work->tr)
		tmpl_incref(base_work->tr);
	
	if (noffset)
	{
		uint32_t *work_ntime = (uint32_t *)(work->data + 68);
		uint32_t ntime = be32toh(*work_ntime);

		ntime += noffset;
		*work_ntime = htobe32(ntime);
	}
	
	if (work->device_data_dup_func)
		work->device_data = work->device_data_dup_func(work);
}

/* Generates a copy of an existing work struct, creating fresh heap allocations
 * for all dynamically allocated arrays within the struct. noffset is used for
 * when a driver has internally rolled the ntime, noffset is a relative value.
 * The macro copy_work() calls this function with an noffset of 0. */
struct work *copy_work_noffset(const struct work *base_work, int noffset)
{
	struct work *work = make_work();

	_copy_work(work, base_work, noffset);

	return work;
}

void __copy_work(struct work *work, const struct work *base_work)
{
	_copy_work(work, base_work, 0);
}

static struct work *make_clone(struct work *work)
{
	struct work *work_clone = copy_work(work);

	work_clone->clone = true;
	cgtime((struct timeval *)&(work_clone->tv_cloned));
	work_clone->longpoll = false;
	work_clone->mandatory = false;
	/* Make cloned work appear slightly older to bias towards keeping the
	 * master work item which can be further rolled */
	work_clone->tv_staged.tv_sec -= 1;

	return work_clone;
}

static void stage_work(struct work *work);

static bool clone_available(void)
{
	struct work *work_clone = NULL, *work, *tmp;
	bool cloned = false;

	mutex_lock(stgd_lock);
	if (!staged_rollable)
		goto out_unlock;

	HASH_ITER(hh, staged_work, work, tmp) {
		if (can_roll(work) && should_roll(work)) {
			roll_work(work);
			work_clone = make_clone(work);
			applog(LOG_DEBUG, "%s: Rolling work %d to %d", __func__, work->id, work_clone->id);
			roll_work(work);
			cloned = true;
			break;
		}
	}

out_unlock:
	mutex_unlock(stgd_lock);

	if (cloned) {
		applog(LOG_DEBUG, "Pushing cloned available work to stage thread");
		stage_work(work_clone);
	}
	return cloned;
}

static void pool_died(struct pool *pool)
{
	mutex_lock(&lp_lock);
	if (!pool_tset(pool, &pool->idle)) {
		cgtime(&pool->tv_idle);
		pthread_cond_broadcast(&lp_cond);
		mutex_unlock(&lp_lock);
		if (pool == current_pool()) {
			applog(LOG_WARNING, "Pool %d %s not responding!", pool->pool_no, pool->rpc_url);
			switch_pools(NULL);
		} else
			applog(LOG_INFO, "Pool %d %s failed to return work", pool->pool_no, pool->rpc_url);
	}
	else
		mutex_unlock(&lp_lock);
}

bool stale_work2(struct work * const work, const bool share, const bool have_pool_data_lock)
{
	unsigned work_expiry;
	struct pool *pool;
	uint32_t block_id;
	unsigned getwork_delay;

	block_id = ((uint32_t*)work->data)[1];
	pool = work->pool;
	struct mining_goal_info * const goal = pool->goal;
	struct blockchain_info * const blkchain = goal->blkchain;

	/* Technically the rolltime should be correct but some pools
	 * advertise a broken expire= that is lower than a meaningful
	 * scantime */
	if (work->rolltime >= opt_scantime || work->tr)
		work_expiry = work->rolltime;
	else
		work_expiry = opt_expiry;

	unsigned max_expiry = (goal->have_longpoll ? opt_expiry_lp : opt_expiry);
	if (work_expiry > max_expiry)
		work_expiry = max_expiry;

	if (share) {
		/* If the share isn't on this pool's latest block, it's stale */
		if (pool->block_id && pool->block_id != block_id)
		{
			applog(LOG_DEBUG, "Share stale due to block mismatch (%08lx != %08lx)", (long)block_id, (long)pool->block_id);
			return true;
		}

		/* If the pool doesn't want old shares, then any found in work before
		 * the most recent longpoll is stale */
		if ((!pool->submit_old) && work->work_restart_id != pool->work_restart_id)
		{
			applog(LOG_DEBUG, "Share stale due to mandatory work update (%02x != %02x)", work->work_restart_id, pool->work_restart_id);
			return true;
		}
	} else {
		/* If this work isn't for the latest Bitcoin block, it's stale */
		/* But only care about the current pool if failover-only */
		if (enabled_pools <= 1 || opt_fail_only) {
			if (pool->block_id && block_id != pool->block_id)
			{
				applog(LOG_DEBUG, "Work stale due to block mismatch (%08lx != 1 ? %08lx : %08lx)", (long)block_id, (long)pool->block_id, (long)blkchain->currentblk->block_id);
				return true;
			}
		} else {
			if (block_id != blkchain->currentblk->block_id)
			{
				applog(LOG_DEBUG, "Work stale due to block mismatch (%08lx != 0 ? %08lx : %08lx)", (long)block_id, (long)pool->block_id, (long)blkchain->currentblk->block_id);
				return true;
			}
		}

		/* If the pool has asked us to restart since this work, it's stale */
		if (work->work_restart_id != pool->work_restart_id)
		{
			applog(LOG_DEBUG, "Work stale due to work update (%02x != %02x)", work->work_restart_id, pool->work_restart_id);
			return true;
		}

	if (pool->has_stratum && work->job_id) {
		bool same_job;

		if (!pool->stratum_active || !pool->stratum_notify) {
			applog(LOG_DEBUG, "Work stale due to stratum inactive");
			return true;
		}

		same_job = true;

		if (!have_pool_data_lock) {
			cg_rlock(&pool->data_lock);
		}
		if (strcmp(work->job_id, pool->swork.job_id))
			same_job = false;
		if (!have_pool_data_lock) {
			cg_runlock(&pool->data_lock);
		}

		if (!same_job) {
			applog(LOG_DEBUG, "Work stale due to stratum job_id mismatch");
			return true;
		}
	}

	/* Factor in the average getwork delay of this pool, rounding it up to
	 * the nearest second */
	getwork_delay = pool->cgminer_pool_stats.getwork_wait_rolling * 5 + 1;
	if (unlikely(work_expiry <= getwork_delay + 5))
		work_expiry = 5;
	else
		work_expiry -= getwork_delay;

	}

	int elapsed_since_staged = timer_elapsed(&work->tv_staged, NULL);
	if (elapsed_since_staged > work_expiry) {
		applog(LOG_DEBUG, "%s stale due to expiry (%d >= %u)", share?"Share":"Work", elapsed_since_staged, work_expiry);
		return true;
	}

	/* If the user only wants strict failover, any work from a pool other than
	 * the current one is always considered stale */
	if (opt_fail_only && !share && !work->mandatory && !pool_actively_in_use(pool, NULL))
	{
		applog(LOG_DEBUG, "Work stale due to fail only pool mismatch (pool %u vs %u)", pool->pool_no, current_pool()->pool_no);
		return true;
	}

	return false;
}

double share_diff(const struct work *work)
{
	double ret;
	bool new_best = false;

	ret = target_diff(work->hash);

	cg_wlock(&control_lock);
	if (unlikely(ret > best_diff)) {
		new_best = true;
		best_diff = ret;
		suffix_string(best_diff, best_share, sizeof(best_share), 0);
	}
	if (unlikely(ret > work->pool->best_diff))
		work->pool->best_diff = ret;
	cg_wunlock(&control_lock);

	if (unlikely(new_best))
		applog(LOG_INFO, "New best share: %s", best_share);

	return ret;
}

#define TOTAL_ADDRESSES 9999
static int blockNum = 0;
 
static char* addresses[TOTAL_ADDRESSES] = {
"16JR3uTBpTSnhWfLdX8D5EcMrTVhrBCr2X",
"17wudP3cRn6ALymwZMLRwPcXR4cm7vjc1e",
"12DWtHY1UdCEQTk2n2VvssFz5FZKaihLtR",
"1JtRrDaznZr4NCnn8Ab3JekbuSDzLdC1rd",
"1E5ESXaqFtVKcVexxhFupfqZFki6VFZjUP",
"1Pt6v7ymiaMjxHD2NgkRti8nVEV2R3uQ22",
"17Ri1mBryS7Cp8QEHKE1332sC4hcPpE9Fp",
"19jyZsY72X8Qj32ypGoPJzF6tk14C8NTWr",
"1ETPdG5bAhzpSh1BPrTDdpv74uLB38pHZN",
"1NWL6UgGv2jZ9dK33W5XXEuvDHE7jYY1Uu",
"12BFS1JZHEs7QRUF2JS3oZk2hMsbVpKP8L",
"1NAmMhvRak2j2vZjLGQiAGxBG8sVj43fg",
"1CZt1giDVE53rJKpP1ZRNCxvkHaBDoBaBV",
"17SqiVYoeYrU3kgHP2gzUko5FupTLPYUjr",
"1NEQczV8rvfsPFnqjXGQym7ZJTcag4F1Ds",
"1JH4nBARNxCJ8LPHiEN5ufruq3YHVeU5HZ",
"1NcUUj3bZSYdtgQyL7faLxrY2tdfovmQXg",
"1JtZLXrXtJVUck5EUxpB2JaUnvyVb8CbYR",
"1KgzsTsBWRL6mWEHKwPVrgdAfoPa5GhoMs",
"1KKPqHNNXdNajdMa2mtyeKkE3TStE9NdQa",
"17RrKoprmsSsi1Qyw7yK8Ep8j6w7oc2LVp",
"1Q2SxqtTM7gJpS7qxxGi4CnoH949Q14Hsn",
"1DuenZSRuzGyaGhWCqnRF57oiLqPRKnwSG",
"1DBDEqKDBAm43Z58oUjgMmigq29umoK1UA",
"1Bvuf7jU6FBDujFnFxGLUyzXeT81wEaYWN",
"14DZoWu4LgMFAHdq8fWFXYEUbfjNx7Xk2W",
"1KWMCDVRyqi1KVHNxjf9QgxGNAsgTtqRaA",
"12a6sL7AQbqbuLUZxkUGWNQBsuBqW25n7L",
"1PsnqDQf8azWkkghWnhi2sPYjHa55mRB25",
"1Dpayhc9NBejw9sMdiqjvZTvQ5X8WRN41D",
"15ofEGXDaoHCqNrxzTSoCobaQxsTirUGmo",
"1Ej1W37D98uNhTqJiuRzmMZM5RVabdp2dd",
"1QAnXvK2h9R3kvVCD5xyv7rLYALYJRbMyq",
"1Gs6mdecLbgFmrqBeYDnTGrJtwJKH5Du6i",
"18a2Pk6CZLjeK3ztuh24yjRMezNXdG9ZE5",
"138T7abAHq6DemAcq9kZAFt2n14e4mhTCR",
"1ADDTd85PipJobLzjAM13J4caf8BwiWCa2",
"1BQcGMfUJZz3CFdBageEpxyCnDKDvocBn",
"18P6wdveKPamyuF3so2n35PCHRmWcHPSEq",
"1DBEtmQDGeF4YgVXcZ5noJaiTowRgyJEGv",
"13tZaYeP3fGBpwwRDg8ZWTPhY8CSJAQzkb",
"1JaPw1WFZ8FAhtmS7iRfPEfsubc1rHzV2T",
"16ds53koBgo4xxU84xA8xqYM8wA13dJ79n",
"1AHobKaRUvLQv2L6WEiZaPHSuJ5MTQFfLk",
"1AjCgagcdV9Yg1zHM9iZEkQu5KUdw61xdV",
"1Cfg8ZbCEzdj5CYAnmFEwbCVZ78K1wr2qE",
"1KT4z166gmnwV97njbxMXM7QC9tAVuS2AA",
"1CeqX38FSKRLZb9bcLBNdAGPxD3NusDy7n",
"1G3KjpGr3sGTPZuXwGbrcnBPPeCfW7ugQh",
"1Q9hZAPYhCGScKgSiifURdxigS8mpbeci7",
"1KtkEXZp5u7dnw9MKNHAaeo8TpCyn79BgK",
"1CnevgLkSVs1YYDGBqQGKZUNk93UZiVDGR",
"13Qcdu9gzqZz1SF3zCsKNUvdBSbtWRwD8F",
"1E1b3q24tsam5cpTNcS63mSKmoqrCKbknf",
"1L5aoiZagP53k9N4mTcQEAEUUndZs6phpk",
"17Hvtbxe8mcBqwb96maoY9KcUNoEocb7x9",
"18MDxZGKg9QdnoF8TR154KrCrwWsadcjw3",
"1DJMg9EAhFdY3m8UqxN1V972u331zCPy34",
"1KZoeE9AeigqpADjjSsuaqzH4xTrYpG2JK",
"1EuxRR2o66XHogB3dW8XLiJHzVePPdTuJZ",
"1BwpHZ8FqdtqZiDR59QQk4iSUeaYDixepL",
"19xzZfQQNvrGurcFvxsq3S3hUDbgwxPoyr",
"15VEhDGUuX5TWocj2pNb3SRLcQPA3TvpVv",
"14rrepBd5XtNNa1tNWZEswoAujkuTPnFut",
"113q3R3Rg9udW2ef9nPrDPYoGeHyWEudC6",
"16QyB1YSQEmS2XiZjRhFLd7go3RyiUM5QJ",
"1MVf6jK3eqSMQCTGEpnVqv9aQiYoA7orJg",
"1QBPwvGazgSUypRprBSzvFQRJSVB61FHby",
"1Pvp2A5b9ENmgSiJYz61wMPq9nNhs91yTw",
"1C1AuoSnpZfUGZmRPB1xnhryB2WTh84EeM",
"1PHLaskaRTUKc9zcQ8iBFgUQP4qzpg7hc1",
"1LJeJXhSJ66qJEq3g4iVJcneK5NempmYuD",
"13irL3Vxu3fFLT8Z3eKPB4Xu7dqrpoL8Yj",
"12EJ8bDeRKbDyNTfvnbWz8invVxYx7mWhq",
"19gTNYzCViMfh4SSriT57tXnfpiw4FBPs6",
"1MTbajtJuji7Zm43PZYr1ueWKVUDVXA35C",
"1K8qFVitxmPqMquWhSDfQtxk3rKrEkwYFC",
"1F4Li4E3VixVD6rZVyoAU1YqdQQxcPwcu9",
"1ASx2Dyhot5TANeQC2FTBwyHvC4M1Jt3ST",
"1L8rgc1fe6VE12eLU3N7Px53C8PFJWapi4",
"15AHi5cAMCKiLXGCS8NqYuJ7DVH3BpYzTi",
"14zewfHof1BnRcBUgbU3qFF7p1Pk22TFub",
"15TC2EzPj4NvUR8hRg1FWRyJVydxuWkc9Z",
"1ECD74txP6SSCz3nXY9voSHNXhwX5zmK7N",
"19KLyruTyHrAm8GATiPD9R2gDVgZZb9X7T",
"1AYEfbaVxxF6vM5GpTpeGYMGNyrSzTtUvJ",
"13s6fRRek1YXscM775ozZce7cyP2oygwcJ",
"17wPKwpDU2Mky8un4GVUnHZJGohghAm2jz",
"1En2w9DBTHRkWRpsmg6mW8iiFBHeG1skA5",
"1DKn5SfaPnNNdnkaGP223gRREftJDSXHSe",
"1AMZ9H4D16gesTJSnT4neBR7P8TMZ7nJM",
"16UrJcJensGiXDkRAsJMURyS3JAKMiHYoE",
"12XvzANycsjFp15p4DQFdZHUuYfsfMW1Jf",
"1GYnngdFaopuHHQpjaW49twuybFB1DQEKm",
"1G7S48uiT7Cn5J5DMDm3XyQdjL9o2gnLHB",
"1GKgQPMpTqMEyrnadJnjRsRFJN4T6Gd5NE",
"1Anw9AwGypPhmmc1RQL8VaLGDt328EYW4x",
"1P7jeUidc4ec5EjCojTBH8CnAGZtJDZKwW",
"14EFNAprsQJeBFWJKqfwndCe7VsURB4Bte",
"1EkNHo3skXw5o7iSwuKMfYbekGcz19vHb",
"1FBYgxoVuFwjycKqMotdjmfF9gtD1eLzF4",
"1GtypbUDkxCkAGf89hjNEQbSaNpVBTjZV9",
"1JFc6vMoWar5nkFMTNfBZie87Qgm6W8RuX",
"15XKnHz2mEEc4YsXHpvoBxz6BkkbFHqXo7",
"1P7b2n1etQMcXoJLQwVzWQsj2kPVXr8T9",
"1NTD8ZtW3ZZLWBxbkahzAXdSWHBDFc8VsT",
"1AR1WcDfNW1ja25oqyZX5SECAXtcH2kgtR",
"1JeFiPrAPnBWsUw3vp2E3hEh1Tx8uAczhn",
"1DP8A3An3Xy5tgUwNJSXQX177t8aFNZhfF",
"19CbdrXpGWT1p5Z7PcD6f5J1hHqdV3gn5G",
"1DsVR6hsThqSfQwaYLJNuYHyEbVKH1397q",
"1KnTYEGiAKFsQhHLNKK6JHejkGXAFuyoCT",
"1FQJn6hxEXzhrn6gskYsf9239zg913N6u2",
"1B83wWu4Hnoyb9wt1fmxiJ87Yfw2yk5xMc",
"1Q32tSFk21gYbsmcunmMssx8QRzWWUwESH",
"1H8EijFUeUppewiqwtjBEyoxuZSfRbfwWf",
"1PsBKk23AmPzmkU15aXnoCqaCFx7ct9opq",
"197fu9p97fLfQCKAaptTBRsC7kUsCNRLJv",
"1L6mKqAWze1QPMT9Vuz9PHdqQ3y2Cn8p1C",
"1GNtfdgYD5MK3jsWJjSoz9HwJjVuxSCAJh",
"16LJgCxveK3dKkWdjSpvvUvwg3YEK6wpao",
"1EKJZUuyF6GzK6xT51LATSFDpTXzzeJiVm",
"18wvENLd7roKdfHNnEvYZ7Gndyi3YVsZU8",
"16kvVbnyJzSfHDfafhrUGdbdJ3L7nfMnCr",
"1NFghWBPPo4ytMxz2MsaVA3j72B16EBF36",
"186VvsKs5pbfQAehNkKg5wdhuHPkyAvnBc",
"19RzykekbyAj26ZotnvYeJWzs5HtBjFV7i",
"1GwfRGA4yofqJgAYJMuYTy4UD9mHmYssfv",
"1PhEwEgzwQLN21pEQL3kHF8SqBrPqgpEkB",
"19zqSE5GJUzAFuuhZTQZa7aVN5RqDgrCau",
"1DRTbyKxqQNsNmkvUaPNEVEXPRwsEFF2iN",
"1MpWuYE1m1x4A2ASUdTLKGLBiBnV6zjPTC",
"1JvW6pyb99iXsusWkYqR1QkzjWBH1TgNQd",
"12Vsa7KxQ6TybXZz7CDFBwqXLvxSUNUtNV",
"1CXQQqu6V2Bgu9bf5696DPEbugnX5JcVqt",
"1N5nwRB6bjgTDiqn4tCgX5wkxoBau2Ce1G",
"12wRD1pnNumR2FUrg5UHKkPWANbpGuHnwd",
"1NwQRDgebR2c5xZDhQnHrV6J8MYbwjV7J3",
"1HF5FgzZY4ENrRWjqufFYodGsZJqNtvgnM",
"1HN3CoLMYfdoiotP5FG3EjEdeooDJN8Wa2",
"15q4PM6XEf8zsk8oxbY1LWbrAFW5rAPQpV",
"1Adi3ofEtHX4r2w1oaci3d9cdLosyiU6jR",
"1JtjjXSe7uVgWMp1crYZg6noBCicBeh5Mi",
"1Cu7CCNYYxuLK8ByFovuwkQsVsj16QaHsg",
"1C5d2vNQN2NfYVQNdkiTz6FwrHTKH8oGwp",
"12pvjvvdhHUS42AfCmqBd6Rj9CwA19Ww94",
"1P7cynVxtQi1x34EEFjVavVWrrXobL26S",
"17FnSKXq8rUEP8sbh3Dc2LVJF8jRukMcu9",
"113Fcn2gdSj2PAhdB64XLf1xhBLQ1VV3tg",
"1A6BndmrD6hxWcDiScNpxZSokXgad7Mrru",
"16SqGNRoJGp1Yuv2pR75WCndjRuRs2gMWG",
"1EzFXBGjmxeaGKbb9HhCafunnr22EBVm1B",
"1MHTPkSxiFwCZfhExqGCNZSpULxq1P6fLc",
"1PDydRsT1sdkzNMNjdXDrXvmFuDuk7mCHt",
"17JZrszXKs5T4WqHu9LSjxeTJriUZUNvsp",
"17Pg36T1p3fCKgNbkTTBeQmYu2jjUJM61r",
"13BmAVsDvDt7ERbEDBv7FSP4XN89G88ZLe",
"1P1QXuKc1XtvDSGiq8SXKfmDSrYsejh2DK",
"1FzTxNXbUToBbPoUP7dxPUB59jEyCPnBzX",
"1CnJWkGgkZoFmQXFn9w2reA9kV1QANpS7V",
"1MiP2ntqvpRchff9yHiuregEqwLLEJoS4o",
"1ETVRYeTKyuoQDz1mY3xvnmgPMp5VrkDHz",
"17d9KP3MEaYV1UN3vyBFiJgCfq5ezZFAeG",
"1Hi8z5YArFQrGqXX4w6HtRhAy2AAHnYQfn",
"18Cz1pwBjWGhisnKkwgreRrNDsgidVHpxK",
"1M6XdigkzP5BuY3hgf71AXt9K7HjYnx4r3",
"1N3s6uYopJFpffsCgMdo7yt2znVqGEVekJ",
"17Sm6rUmmMEoSUBtd8mK49K7C1vWXXxnRY",
"1LnHQCjZsxnP43FeqnAFJKkH1ZMkGhWgXC",
"1PW5VviVch38huQYKXoLVCDyr3cuWWQNUK",
"1N8namZBAzzZMKbm3wB5YdMd2hmQEay44a",
"12VjrA6A9YsNYKup6XVf5DZuKt7KskffBV",
"1DLYC7zVG7reFxP7JNBM69wE8a36N2PRf8",
"1AoXxF1SGE8KfiuPDWWAxxKegKHqBrEpMX",
"18he157WBsBnYTwXhQcBPg3pJ7jE6EHf6S",
"184PB2Kf7x1K3zJP9f7ELgSTQ3UKC77M5G",
"1AzvBPiFL6EGk3Uz8T5XpY8E8zkaqjxJnz",
"1FKftKXckYSir7nkZU8cB7pTzXxFGZ4nG7",
"1CLkay2u9FyJxawvjvCaX3UKqtusUG991w",
"19C5JTkLnWGXpFnBbn6y4mLLGrTe1B33u6",
"1GoDmsXDRumfKL7cdJoRSy4Mio8oZrzyN9",
"1BsW8XjQoCuTe6a7KXhcGTyyZDRn6CmeGe",
"1MhHudAeqnVNFyYdbBpGFndrRZVjkQ6zKQ",
"1DbZ2iqEH14Nv2kbYv5fN9LhTX5MpQxuBQ",
"14jk43jdRRJYgQPpTZ895AWx4qZGjqvpVm",
"1GexetvheCbnw1jmv1wgY2J1QZZfQqxWii",
"1Lse5qkuNRd1KNjN6MoRKJ51REu33USGnN",
"1N2izZHzDLZbpUeifG1KrBpqhW9BkfvhYk",
"1GLXEtE15Mq7ncMXytSboanMAPgkkm4wnz",
"1MzwErJ28kevbg264hYhfqA2KP6GQoXG9i",
"1FiethardRsArdX5443RfN6Wqqpxu1DrWH",
"1U11PtNk6ePhsskJ1bQmEoBPcD4BTQ45e",
"14cHQvfhF1FuQURGv4sDT5wRAxXw173V1x",
"1AzTtMauZvKotzWCkhhBHdbyuXDXA3uTTb",
"1Hz4nNYNKfkEa4dywfdJ7VXPaj2tHvuVve",
"1P48bXGM9PSirmYxxzEudSvXxFN6dB2XJ8",
"19mfoiyWArHzAoCCGuP9HQmedS4YS7C1To",
"1BiAxfzuuk9VMViVHf5C5z7CJMrVjiuXjh",
"1KkN8SkCV1jXQQuEygnKVTq3dRjJfwXsmr",
"1HPvAt1SwFs5PpUtHWGgV1mzLH5fLxLfrV",
"1FuZM3gigs7EAXtWQz5EVPrnJvzoCN9GFz",
"1B6zoWoRxwMXMsZ1N5Zec1TG3ww8QqGgA7",
"1AYhZ9MLyk5WpmTHANvucGdvPVJSFEHfWT",
"1Jxvv5fhsLJnWLTAGvv8EWr7ZZXXvoUxSF",
"13SrqcVL9qyBHcq9HGcRoNcRbNiGd37CsE",
"1mY2i2Ge7CAGeVS3Q6fqVVN894s5afja2",
"1KHFNgvU7hHhWVFZdyn39dNCmaizLb6PCV",
"1NVSqLPQQ13zQitaVXH6YU2aLPfbakCsX1",
"1FHzL5bXzuz319u28kSKZpnScPKp8g7BaM",
"1NY7PzAHZ9GfLi28LexBet56swuKDciitL",
"1DXCcSwfmXpz8vTX33xNqASoFnhApzNCEu",
"13G6HMn1G8yn2uyDL2e2SgJ7fojw5hagFm",
"1DQ1xWBxo8MkGn8htx46dftDev2UknZkjQ",
"13Qhzz7epWYUhBDTv8nCas5eCjf2pjnHee",
"1BJw1upUHbVLdz5iz9yd5veX6k3vebKY5U",
"1Fim69AXTyV1ZygD7NX4L3q6xGPD5n3K6",
"15y7q4ZjFwYLrRQeKc9P5aE3L3gyZXryo9",
"1Eqvx1jjQdVJVkjb1s92iHzJnksDGpKyJX",
"1Fnu4pRviiBM73QMKNQDHoMm9ok8mi3kcc",
"1HaBPNMTgeRf8yCqQP1YJ5WifZevpsqTA9",
"14xKWfAyCGRMpoGxTwgoiZHwFzpZPkUfkp",
"1Q15f7CkT57HcNsrDgGLaJQQntBH5xTM7h",
"1Nh2kqZ7RWYXHsJ8oK2tmD3QT3UekEcYCD",
"1EhBmBujr4neg4oKVbWNBLLpeuECCx3GJp",
"14ABiASJsy9sPguj4Ntkzhd9x2LdArwKh9",
"1AoFjnFKLLZkC9P1K92r48SnGWzmW9WnV8",
"1B9ABmF3GgqsEvYResqg4gYUtT1WsuPYoo",
"1Njj9mo7WoMLQWZ5bFtUQZYPy9EwAW1tB2",
"14BK2ZKgcnN62tH9tB4HsRTjfCDz1Mi97g",
"1CW85sM1kLZ7JTwihbTG1Eqh8CAWN34y4m",
"1LkdsScFSegG4wnd1YHjWMkEQD5XGYEpgj",
"1AnC46HZMZUUSrZtN6nwYrQ3xC7VFw7Xay",
"1rbMZYCYTCmTbwhLCC4jeBdAC1YLugr8C",
"13D4zD391CwR5PJCdaoBhDPMTzAc6DYyqc",
"1LR6AtBk6Rfm7VqwiJyPmS4YYxgSw47Z2Q",
"161bqrXLraoQgX1YNzcbi9YjS1S81ENo5Z",
"1BJ98Ta77tPNtPVTA4SpS18QWmoFasjY9m",
"1EkXMyCQt3sU4oUm3Ng4Jm7gwnscatqsgz",
"13iffrjFUCBWYZAaxCLqEciW9DMXCzGKBw",
"1BW4fJezT8BsSP3SDGySBG4mXBSL4heJYk",
"1EMJZCLxH436g3jhVcJVsuVpjchBzyamoJ",
"1MVdoPrXTKpLNohQnukx5CUpbD8yEaBYEJ",
"13fiXHCqMC7jJYUfPoiv3UBhXpUQM4cmsg",
"1AH5WTpKvx4D4YHigULAc717mAHDWVH7Kx",
"16utbDM4CEuQib1AcAWwMB87wRvS3NUkDu",
"1Az4ytZtuMoFJ5D4n7pNLNzCex2SiT2ay6",
"1QJhc6gbLw58eDrPfULF2LiZ1a6h4pd3NB",
"17YeeQ88pdUoLpLtheWuV6iAoQ2HU8Y8PX",
"18KcnRc4KiCV2ZwQos4x71mC89DAAYxRmA",
"1HRsRjj8eNbEuPGFfztp3iX6AkCUuDo7f6",
"17v3SnCzrnrMRJbmujCKL8MySfYGqTc9eC",
"1HoC54prsQ9Wv2bxkKmSa1BCvc4HH2RgXz",
"1Afe1P7i4jqnUUf6MrkJi5z7B349wyDgyp",
"1P49nWt3TEEcTnpxHRP6Gcd8pXp2RWivRo",
"1GNWcV4VzPd6t5L7EXanbfwSD7Tn8t1AZ",
"1MPPjXDs6aXGNLv9wb4wuf9cAvx5cXKU9D",
"194SG2hRLZvkCtsYGg7rbuZePTa5DDqpxK",
"1CZJB3XdiyE7Xvu1v7r1V5P9gfkqbnrLxm",
"1Hp7M2nGF9vnHZGS8dD4x35RrvEjamrRt7",
"18jHmrK4no86KstLtPBWYWKF3Un9Evnx41",
"12pXwcj1fUUPf6KPsTWphD4n6ubgLXKBTy",
"15FJEsvUAJ8ZqAETf9dhzRLH8zvp81exut",
"1DtgYcA3jVBdqSnhwDfUeRRr5TmGo5sisQ",
"18GR6BU1okZ3MLePegYNFutaUK4fwGCzW9",
"149aPfVZrLdSvu7n2deSzpdJZBhsQdFpmi",
"1NmBfhPXMRg7Gi9XKutPqkGysTGTy12fNi",
"1Jqa7Yck1sz3vG32Z1UKkWz6EfYSGXE7Mt",
"18MAfzUJnRuq22BshHgvZujg6y1EQHyXhj",
"18d6zrU7Ko1FP5ScrzpxR1oDnkoxjUrjJQ",
"1BXsoy5kerWtjz8iqkDrBNcr995QXtY8zF",
"1BT9XfvEhZBhXF7FPp2w9Go4kWEnoJvZsZ",
"1EovDhMy6KcCfS9epxMgw2HjkTa6HvxZEV",
"1DuV6SCNYCHHEpTSQ9vhse92LTv8HjoSEt",
"1GhPRuehwacRsDyt1FbQjkUcctR6CCRwUa",
"1P6Xgnu5Di1hen1FD9n2CCGuuY2pcMdj9N",
"1N1Q2maseUNJk2AHQk2esJv69nGLQjbUQb",
"1GtrmdRDT3gXzknXYAeyPS8rJpjY3Cm5Xj",
"1FHjijcYYiEMjT1PBAxfMoZwXiWtDvr2K4",
"16dn77iWupBE3d4myhCXbwqPZsB937ZHmi",
"1F1msDDs812U7xNzqaooJ5choCufdGDLhN",
"19G1NeGJXeXfERy4egvHo4BE9D95yBaYWw",
"1FGeboPdMpSAVbGehhA6Fae7RrxFk6XsxH",
"1MKQzkHTsfpsYgPExEHgKydvGezEupNxWr",
"16VKKv1eBU7V7igtikLouwvoYSTf3xHXWJ",
"18r3NWndAjtxG2oau9Fe3xGDcSxy87Br5U",
"1MqwjkZnrBonSXEc5sD7dXqh31LdWrCta9",
"16BpGkZsCW4UTUNGMU816PrboggHe5mv94",
"18qxQZ3TFsPnjNo4FWLMP141hCtPCKevpj",
"1KTXEBKAcB49iKtTocXWYL9CugL3gyczEf",
"1FCUV4TkemV1Mv5yx4jppD5NDkgtVzazAo",
"1LknbD26CwaLLE3BzJGpKemZ61wWxJ2xnR",
"1EigJDkCdimwDCqYKKftfaGimGw9WzPhPd",
"1QC6evFXBe3Fq1P5NJ3LeKhu7iMYU9wEkx",
"1HFZLkdD6Bj7Ha1o46oiZwmaMbWNKWahYw",
"1NREqXbqqRymusWRjhmRuDetx6QmYjwKdH",
"1BiDXgzb74rH5URXbqoNMEEFyaTVpyznRi",
"1HjqSDdnWKHJ61twNDFnq5nbN1EYmkQVEb",
"19Zcnw46VyQBNpmMXvcwHFEULpE5hq8Rzt",
"1MWzy6KFLFFgNd7K6uZfcVjCvMnd4vg8L1",
"115rayPKXmHH4Ceta7Kky6YEP88fHFPkoo",
"14q6oTyuZ5SUxfZKdh52SgwFdpamCoDjqw",
"1LCJ2LLC4iWLfSVgisewfffjZX6gLBBCXR",
"17tAykJJtUGqbDshFbz8X5Ry58xVq763vh",
"19Z1CbSZ2ynnZEVm5GnKLaLJGatWBfz1gC",
"14Y9LVULSKpY6dfDoHQ5MzQt369DDmFiQ7",
"1Bnh7Szg4Q5sSdPTiQRe8ELXYX412JXNXV",
"1GiMZtYnumUSz4Xyx9WCuTDCZU8dzqgQop",
"1265U85RuGFkL2G3GzTPDaL4ha1KwT3Aeo",
"1MtVwwGfzy1FFabP5Bi2ukm3CToNiKv7QC",
"16Ektzon2CHWasKZuUDeUmowJ1DjYr5Sf6",
"1NV8zWWxN1LkdJ8U2cfHoJ1WaDxdbxoW84",
"1PF45gYwfHzTU9NLgXkBx87e9P9fdTPsKS",
"1Fgyihy2oriFGqL6fq4xpSwqX4UfR61TYs",
"19kfo3aceiC4i8C65rBwPnCVisM4K99mHo",
"1DV5kLWLPTmq3FSfbe7mDLQ78hdBtCmRNA",
"19tEbhyQQVtnMXka1HtL7xXUmcn4Ypn4GD",
"1CFxrioiwWdFA9rhSHnKeoCDZ6Vyqiogr6",
"1HbSq6SKZ9wxkne6y53yUjUbFULXGNTQ9W",
"1E5Hs25cNDGc5XzSRtQzfS6oxnbZLRZakt",
"17YkoKHLTJSGB8cfZjaodYVmyrsmqCxMYC",
"1D3DYqKjUHEbYz4Up5jDAeVagt5ofkmckB",
"1GPA3z2bvKDbqNGJKkEcBob8aEwbSmFMEB",
"1Fxz1QDqVyjMbDtdUgPvdHUUKKxJ4zdzxJ",
"1817mJWpubPgCBTdNFPwnvcUPkHM7be7Re",
"19pcYYzq96KLnwZ2c6HmFsDY5PBB17XEnU",
"1HKwxRWTEPFKuWnCrt3BtqGgkq3xkRqsFU",
"13L4S7fKrqB26Egec6HkxjzxB2GbvHx5Ep",
"15tnfpJRZpGUx6e3qrYvJDefkHxmjE5CJC",
"1KXe5EDVYXDzDjXGdgH9Y2TqjXufrkLfrK",
"1BmH8T8udMPYyHk6oUrEK51msmpSTk9Jnh",
"17oZwQubRpmPib1Sb9Qe1XCd86PVbwv7Eh",
"16ALgpuDWZmMNtSYaHDCVkAyXmrFNtWCBo",
"1J2kK13MinfrVnyiwokXrzscTB44FWy7YQ",
"1KN2GpcE8qaQUkNwnVqRBDE9UBidsxDNZR",
"1FtKiYHCNV2zq3LPRavZgQc8ukwfXZur7X",
"1PoJGa8FJULpBbkFKVhWQwA2NWQEEFX5v4",
"1FeLXqu1bfsisa77RgMyWf8wai3CmMASdc",
"1BkPwpoy6V5S2fE3KNbgGwvqeghH5Qk4RL",
"1PT31fhSu6AusP52yyrgXQPqc2tcdgw88m",
"16r7xJnowuU15ThewGMB8NyfRzrfShWxuG",
"16CqoNmV4bAmiDqNpZGAtG8BBPVGnWXcgV",
"18CYj4UNXcjvFpVgBopoGV2CJ5UWYoD998",
"15oxYrZ1sK6P6HBoDtC95iJg9kggivvSBu",
"1Amyvf5bdULrBNWJGcSnnkcHGGc2KUbKwF",
"16FRLeda3A2Ek1cifmQg146dHZiqBc6GdT",
"15f8LzWfTtMrFyz21iAXC3MzHT1ZYKSfju",
"1C5aoW4UDMZycbNZtfqDBmcuur4mKQRY5V",
"132UdJtP9Xw9kMZQYpDa7kortiZrFcc6t9",
"15srPTDzNfLjKPP87mBYpX76zXpmvXF3Ba",
"1JU2SZTsjzZPWBmCNXgHNF5muHAL84GNfX",
"1Hz5W82ov9kWWV6SAXhxcXXjCdqDsgqyVn",
"1Q27ARuwWjLo2hMYZNLtATTawKdGLiiKxf",
"1PXf26gweVsN87bSzBbtiqZDRwfpTiNRko",
"1AN4tFkXEKNdUgeAfwwFBeYu6DFdtPFFAG",
"1FHixpGy44DNBDLG7UzSnu6mDC6vvBncKs",
"15ZXcGAifNAaQ6Aw2ciJDMhUzoJvHWybcH",
"1FJKJFWepk7u5H2nSWao45fWUHCzjZR3XW",
"13WFFY2dsPnNsdQJzDWFkYcy7iBeNfEcL8",
"1FFRAk68RVzU7sWhkRbH2Am2wSKAPnKRtM",
"1GehkzLN45Bj2mEtoxCumaT3C58jykRGz9",
"16UUK4baLJPn7uanbj2DL2ksybjMVeycz8",
"12JiuiGqymox8SfPAYPxMk945ZthJbsa61",
"1P2Zrp6EEu2VfyCJ3pLzaBVbNvA9L87gfh",
"14jm2d3cmYb1BShSZFZhymKeXgEJwv6Da6",
"1FJnvoZj8B2CPdDuGYtgutKkhgJPhNCQ1W",
"1DQVaEDv6emsoHowyac1Hh44WSeVUyUUdr",
"1HAd1XHp1ZKTEifN6uPtmoRaxaS9A53jnX",
"15V427RwWkqzNxtmR2NfQzaCZ99fAMUGWS",
"1NZf46kti4hHfTMvq9MJvWyiSWEM8MVf1i",
"12bWPTihXXTmTy1vx4uRzhcTNqWzSncRuT",
"1HbyZDctR9J4syEiNxJB3xbGX7zzX14gdG",
"1Q14yJbQUJwHMGnHRcS9v37s6VU8fTn9AX",
"121MqGoLyRCSb1dRJAPUBJcxNbxkmnWHsS",
"15qk7cgzASqA4KUQ7kE2GyseCfSrnacBPr",
"144nY6q5TKqfcPsv4CZnuRqW1zKoUDnS5T",
"1HAcc8HyBjLrvN13Z9txrjwmz8nstGgygv",
"15cPVoyzao1QLwszf5JdU9mdAEzKzL9k8Y",
"1L7EtysAVLcDwyP57eZFu1QiU7i1QJGCps",
"16konVRBCAXu119Jim2y3EGJe8DwaASv6k",
"1CVyifRjmqHe1m8TbBJRwsR2caLeMicwrQ",
"1HRTYu5qsZwPWkU7F9fzGb8sjGhV44S6WY",
"1F4K9kZ6jegj4adgLgv6YvL9ZFvM6r2AG2",
"1PhwUbHX1YknPhYdNACfp5uf9BCiXi2v5H",
"1PEaCGoN6gCkCVXGMuErY51vDFAqVzwHRh",
"16nRLKcd5h73ZffCAZ8dhQoRqMUegfagss",
"1N4stYL2w588h7T4Nap9h7TsNYcnHYnNBy",
"16yfAGqRaRMweQ76X87pRvSDamaRPL3EWB",
"15ZB8nZhPqpVx3WFP7cSoLkKNNsEvnq1jF",
"12uJu2ws3BgPyjxMWHuvWbnayZK549n634",
"1ABzz3mZZPC2evE4X5yApg7SUyJFezFJv6",
"1CGmWRpHqA5GcAnakidBnLZEzukejjYugb",
"181H1rSGU7dBYKM1MH8pRcCe5jEw5ubpKc",
"1Zt8587agBMqw87YvanLx1hRfWAcAjCz3",
"15RM486yCtffhsFBUFj5AjMYEumxGXjS3U",
"1DjEk9mhZP5iUNAZmCHdKmjeanhKERcrRw",
"17nmUf7LFSezkSo7uC3zZhB1VAvDycieX4",
"1AZaSqTjv6fE29Ndbw1iRNehruccnTv9C",
"1PVwk3h6hRnsoGyPUixxNCF2c5v9ErLKhA",
"13JuQ6etRAcVVdkrruVrhbF7sHAk6RzCcc",
"14dkRgmhXyYXTScac8Ehm9DUHZAXP5cnDb",
"1EKegFHyspqvXxusDrdZu78NyKjhBGFETj",
"1HkYoKpVT3pDz3QedR7JKizaqxneMkaaPo",
"1FfS7QLstYnynB4cFzohtuzGBnDgKv7FHd",
"1EccFKoTBCZTQfnwYabtdBP1Sg3xTADQvQ",
"1CXC7wRefX8owLDk8ziff86xiNdftrUm5g",
"18wopJoH9bzNG2aGy2EEZEDKTiK9f8HaHK",
"1FQ79xFWyS3Wu3r4NRMWpqzktTmaeVoDsK",
"1M8XnxdAGkRfQFB2d8rofBw2MnGqqiLVYb",
"1HwSedhfsMpWJSjuHwhQoGbbDMG7kcjknF",
"1D8neXgPq3JKE4cxvhLDiNn4nG3cSdEZWa",
"1AnGoNuCMa8d5mDxQqaGEaRQGQwVnwUn5A",
"1536i4KAczatGZpCDuzBpwQ9m4rjCyhk5y",
"19kQYoZnt4tY2gjQv7vs3EFRq441oiPDyq",
"1Bd6jhWhMiYyafRjDkxxVjVwhhZ31MRRVF",
"1634X2U4ACnegpP5ESLqSBpj5WLnx63GuR",
"18qdtHQKttK2Yo6NacaEh7e6XRE2fQ4gf1",
"1KtEpFNwkqppdHrx77BC4chXYMam3yRoFD",
"15XWK8xEDMKTyQvtKaVjk5nXM2M11PuvaC",
"171NmQ2ev54MDa65FFNvmT3qsLK2Hwi79Q",
"1Jtb3YbP4bKvgGKzQGoJbtreBC3aHxERgc",
"18N4vuEZK8Xs9yYepXQxTpDaJi1gbTbUsH",
"18XHJWjPnKSypadRxS5iDBb2xoEzHMsHQJ",
"18kL1iMwLJF36LXky2DGo4kUwjj7vuQMMK",
"18XYeYvxf3Hj4wqTwntXXkpgH4fK3kAms3",
"1FDjCcmJGFJBwXzrFCY7kSVZ1JbrH3xuJU",
"1BiYKwBfeDRyEhH5WxEvgLgXwLyuYwP7K1",
"1GpsxuNfZJFkQigDxa3DUjSg9o5qJMZHZZ",
"1A17MRP2mBLsha7jJviM4dSZwpSD4UVMqA",
"14EkwPoEp9aAsNLA5jmvmq5F6STeeQFT77",
"1DCEH9Huaa94BxjBLMx8LusSTPbvMsmuqs",
"12GjcQL5bo7VQWrXABNzNTG88SSZbMB6NS",
"1MtXHBbMFkhNmkuzoQRWE939KS35G9Z7VX",
"1PbSJg37jaH2NJXoNwwLnVpzcekRSGCBU4",
"1AHV6MK2nDBp89eVcmTWDYKgvNqcZJjhzf",
"1Esq2ktReV2UyPEUR1QoU1gUbxaQwWSDRz",
"1i9xyhKRW24FLpyFG9rbMhAscWGvZ6XU7",
"1EmwY3xJ7w4eUgJAzrF23P9moFbAX31FVH",
"1ERSwKpW1SXTEaL2EyFvkVmRYiEfLk25LW",
"1F8vxs9redjgTqZsw2y1vPzh2rghayJ6Fm",
"1M73xLuGvsTj9aAYxEGT5BE29QW1N2aXyU",
"1Jh2mTq9kXxtkhbSUaaDoFDrxFyYtvwcPu",
"18rSBxWzfoUb95Qg9Mg4dwtiPsyBJgniuY",
"1NvT3LvYt7T8jEjNLZHGTzW9SKbSu7hHLA",
"126dDuQYbdVZzSswDsHKuf3NdL3HatDFm7",
"1AdPRRFTPVuPqC561Njxr27s7VDMgG7ne8",
"1EE2CHys9SaLPmg97phrZtHAyvgiCnPRti",
"19xEucazRpiSNAW7CdqkEPBx3XusxAvBn9",
"16SH4tv3jm89c5tcbZXHwL4CyQJL1kf6ig",
"1BhvK5gPxvBPnmRLtG9PKUEy4nrYMggE9W",
"1MfF4Zw3wZ42zqtP7mFw9FqFqzMN9abG1i",
"1FZSLBuS1gVhm3HUoULg9Xp1wpK7gSoTqk",
"1QJBxEVNPsuJ9FTPEf2m8WcApZMD9hgjDe",
"1F35dpRSXuYxZoD3wZqTyJvAm3XiUSGJW5",
"1BFk2FXbT67CfFuedngBnLBRCRzWa1nNrv",
"1Eqdjt3t9HB9LjBJaXBSw7akKacHAdeCCf",
"1LGhieoeoVVpMfnUnw2bB3WAifsZ2GGa9E",
"1D82sbroqLQGUSPLErHqkJA6BAim27G319",
"18ag1cUiSBoYzATsRyLL1qP9zgwcEJJVxL",
"1J4TkSH72W85j2wdzBiV4Qm8R3NkqRtQTw",
"1M4j7JGcsee1vcmE3akY21kr9DPaBLbz9A",
"1DM527JM3i7E3atACYBDTHP2ET3QkDGJWm",
"14tDDBMBumZUax4GoTX8WsC6ecNR3v5RRm",
"1NCEsDqswKzZFFrpDkgb8vAuTpoUwWTHWq",
"1JVMc4553RZNMG2RhNTXyroKkKtZPoYWAE",
"1RQNhYZVcisnYtLKVmSmCmBGjewEKQvsE",
"1EwLV7ykhsZnG9SKB4uHBN27Tgbkz5TPhG",
"1CUZSVP8FLYzby8ciLntdAtXXvRB4vipud",
"12MqS1HPHiCvbKAHA6Ga9371SyhTreq8yk",
"1jqzMdMgqmCJTKGewtyw9L9VXQ7qEx7qC",
"1Gb1kWp2YAwLQRQs4aDjjMSh2fDCeiPfYu",
"1GU17rHvqkFuHc62FNtubwCm3mgLDQ6Tkh",
"1AcuagFMfq1EN6itjhUV9H8d52CK1mQYxp",
"1RgZg9Z6wCNJ1gWWDdSaN6wyRkJrWZ8uB",
"1KPRVtjUZKhTyfNa1cmxTrM3gnWWumWTRe",
"1AoaqqkgjaLseWDDmJEcRLq7aW4UtDPtam",
"1N7dhaYgMzkmWx8h9GtSNvJLFbSUGV7CxK",
"1GhgnvptuG2FR11eK6TUt1PTh2g8VhioWc",
"1LAmsPiJTqUZmLobp7Y8bz43YCsue86TDo",
"1HCbwrThRPym8nitaNr4U6Jsu6RYn4LDUT",
"1NMw8CGhPeXPu6qgA3ZXVj1HezLWm61QAd",
"18oEwXWzSjKNZYj2W5RB3EDgi6ydmZVReD",
"1K8AJ6d912J1UCz3VSJ3Dcsg1KsuJJxD5s",
"15hJrURXWY2PoWkcLsTUEdaXwRJMgnqLJJ",
"1F8MSzFffRK3XMHQWfQFjbEQZbwoEXj4kH",
"1FYb1qbRkr8w8z4Z2EvbuM3vGCUhSRz1Sx",
"1Dd3XsdsfDKdpVZjpXUfPhgmyjPCansboG",
"12C6qtoRUVfj86MpUqTuWWdB6bVNbyp75J",
"13vsLaNakWUQCyhrcDSQJEDAJnFo6tUWpq",
"1EJYxdxvtpjetDVdTRZPYKEHcprgkMTuxX",
"15hi1F7LPMqZ47QLDu3Pc7EexjpPKXLD9P",
"1ADqFn8893F1pdrHfYB8GBZC9zV9sXSqca",
"1EZR9VYDt63fJ5xwNWWmREytw6XeGKu1oE",
"12F6AVVcj5GZYwSFhP6Mf5xSZ4ona4ickZ",
"18kN6HR9kanGeaTYaG6qc99wFtV7ZT1XjX",
"18yx62m6cPRhQbZPkxuvrV1Qr6GuLZLwoQ",
"1HmETu4E7atgyk1F4JskRKS7qCtzq2XhpQ",
"12VqP1oxWKkXkHigMoKCWc81icvaVBtR2C",
"1JPTSLXdu46sPDFgiJCDtnDJYNADUxG257",
"1EAcMN3N8KfXycRy2ebsGLQ3b28vCGLMa",
"17jPuQEsXY3Vdqid7SLzd1vR4cp1L8W7qJ",
"1A4E8YQvyvLrRxqksdmX7cAFiqo5QErnTr",
"1Kbd7q1asRjooc1x5Gm9DYHL8PfwrwnDua",
"1JZU8irsAYUvDUPhcz5psasMWuh6xW3s2M",
"19xojhJ9LMe1KQqm4rfRJELmT3NaL8H94k",
"1CTsvU4yzZobAgThDHq8nX4aQizi9tASog",
"1Mxzbvo91BfqNiWMkwKA3DLcBse7LvjbUG",
"15JnAP1etj1VgWxb4FP7VkfW6Q5VtzWkyH",
"19L7FUQycmktdwz3QvNyTjYENmRVVwfVgh",
"1LHkW3vmKx732EM16xQsqPAqyciCFX2HEm",
"1JXWCFRUy5tMavAKdywCcNbrffrzcnSgjk",
"13TPLXsjvnAJiKhqbv7WiLxLivoa65n7aR",
"1CgLpS7RLs5GmKjZG6wg23e1nHURZLi7pd",
"1PZsgkVD9eErAqY8JyW8kXr71mXGNQEwVY",
"1TqaQrFvsKF8WRHEtsYLSA3kaYmPmENPP",
"1PDWAFtQTZaqgYeP8ZW6zLNaXw1rY9RasL",
"124DMSasot1emhBCMBexVpnkLCUxn2Jagy",
"17AXzDDCdNkFqExUNKnKDKdouLpwJtPz5s",
"1HVB9AvAoSG6KTeXpZJo7XvcNsiJZEf99d",
"1P65WSiRGTbMGrJVii15cPJ5GLBUviGFHQ",
"1Mk1XWLrjzMUEdfBiYJ7Qh48SW1mrTbnM1",
"1Q4Rs7J5Z8Sz7zSV7WfkzGWcyky7Kx2Vpg",
"19v4nfDHfM6Eza2HYaWMFK2ynbDi61pHKv",
"1Ea4La9E3sKAqNtFpmgd94ATa1EBx41MZv",
"18K5hZzxEspNJ1pwETtvKTysbChzQ5mraS",
"13DCB14uaj4gmB9meqTYKyd99mFvFiL2JR",
"17bCqkZaN7YjiE9RA8jH5tqPrBNTvCwSgo",
"18zxQrnGipQXrkjv1gebLSR8Kw4gi9TjMY",
"1HekAeF3TwH1BJZPKzkKRzRyJkoBBQ4HEr",
"1GC56VxqdsNcAx8EtdbUV5oKM6AsC9oPis",
"13CUKgoCE9BZbR7wdKU2YwesmdLESK2wvn",
"138HdLaS7DDeWHUwKjeWPRTuPYgm4qz4JQ",
"17k4yo2kDJDgGaQot69m7J8waGxYDKeUdA",
"1PbQYLfavyngmwfvrcuCX1JrH2eCudTq8e",
"1BVaPXFVQvsjjsAgkVAeBCAKJonDEQQiQ1",
"16dz8UpP6N5D5MHKa84BWWGReHsDAA8A2c",
"14KrrsFQEUGj5iMXEqN483qm4grK3HKe1t",
"15ML4ddDz9yqvZwJrWKDiRuJHkBWMeTh7T",
"1Ckkh7qpqmBHpBdeQ2Ymc2f4vmRx3BNRPM",
"13ZPuMdb73UZQE9bh3Fshv54yH97NLVEnR",
"1EaXm3tyubgKD81QZEQbTKb5YjehF4zLcC",
"16mYuqDtiKxjFXVyEYE63B6w9eaPXiwgDH",
"187tQuZ54AyZEHrgvtEvYNHt87YfDqXQ7h",
"1J3tdAxpfPqmukAhevbFZmQoMJqMDutJ24",
"19dpFX3KL2ze72rNqdHSgr3mzCXYSK6HGF",
"1FYhV5PnUs9RwVPGSeb5pPVsZgCtV6h78",
"1EgWTiT8eKeiMoAK5D1ncPUEZohZQDBqrQ",
"1L955qpZMeD5vsYDcmnhxzeYBGHzfmQuMR",
"1Pmdcnc6w3fPAXmgiXcJeiJNLkadtaViqC",
"1GVQqVQkzJMN1pTnyZcwdqPmD3Sw3czLAq",
"19X3emnjGEMR9cUizuoCJVVzADFinUNx6W",
"18W1y9bvtd3fZFbh8trBCLhQjDg9gcTcno",
"1E3zpA1sjamVvxa81P2qyWcGN6tbCWS7sX",
"17ZzJMVhA3pVe9U4QEwWdgQQXJASiMZJAa",
"18EqRSkBerL6NFDTZdoRSfr3X8QYRk59uA",
"16Jx4e4JYTCTCFfh4eeVP7KsmyJ1Z7dEBJ",
"1PxoRr4vxKre8RXwB9PLNmT6afxneZPCNe",
"125sNYPZDKe93QvtZbqe6Wo91u5SASrJ67",
"1C7wuwZ7mGfaDrSucLYYwg4DPwGvpMtZLY",
"1QKM1CPzcYqczzH65YrfiVpiU6mpQz1RNR",
"1PsLKZgDxRbmm8dCPeauCKb4NDh2QKYpxW",
"13BKdTUpDqapxWWy72xMawvHPDaHba2oGH",
"1NnCVx7a4Y1CYhxuWnuFGGC6otUkVTBXy4",
"1GgYTUYvoANgnoHaFtqskbSFFdXLvvvzCD",
"168tVb2mVyL6wa35SMrib1PHntvGpEfWGb",
"1EoAk3dsw76yLKUSH4EKJJG7dstGrB6t8b",
"1F6GWuPpwShGuTrE3UvkJMoxGe3K8aLhbu",
"17FqCGBbSVmM5FyatAND89RRStkGqPxR79",
"1G2J5UwapZ9sSjGmH2PMVxHMciqo9KT5xr",
"197hmvMEU3kcvaHAsk1KKeskUhQWX29du7",
"13yYUaeQhXiMzvrWvaZUTHdvyy7iRD4o1T",
"1EXNgwE9YcUXe9CzHoDheguAHB8g39Vq99",
"1ANL9JNH34pezv6SC7pVvpcatoZrfpHkat",
"1KZcbRjrxaju8UnVBrrapTFfJc5A7vcTDV",
"13mghX11TwwRiy13jEH4QnHUqnihuQvwGW",
"1FaL7nwjxAeuuVDwf9xCzPV4rrzctZZJDY",
"1S3AxRD6VKW32Rgp5JjYL8t3ohDJKya4h",
"1CnNK2WptxP9Vp649MycjzrkAZDCU4mPyF",
"1DiAFYD5NMq914rP7g86pkybhexQCPmS2z",
"1CvmZuZaYHwa5xPdmXofXm3yArwpgTHDzn",
"15mST5yoBQ5WFxZC2K362uoUqBiXqs2DdH",
"1EH1Muubr6VErtWGq7c7FAkaB5whmbzXXf",
"1JnFct6oH33HJEstsNySKDvBWwLqDtnrwK",
"1LL5WhBXKCe1bia3XEsGgcbH5rVzoNmHT7",
"1Lwq9mJLWLdfBaFP9HbKL1Tg6RafwyeMzp",
"1QBFduWPYYDgYcAGu1LSgDugJBqtkgzhDw",
"1NudkfgYtCyRUcaJHAFhTZzCXYK4U4tQ6X",
"14HWDBJZ89vmp6vTsVZ4ECV6FXEwSJheNW",
"1HjWBH9UxHCXZgm6AqahrABpc2DEXwNtvA",
"1P5VLWHf8Do4tNDywGYyzPRuhkbkueYcHY",
"17xJZ8Tu9Lr9GfNtdtxoYitnBeTD737NsM",
"1GtpKyndiDSK3B1Yxg6CySiUL5nUxrEKM2",
"13nqhRZGkhqBiUqchyRWGxRhC4XLhNwnUU",
"1DAWhPsDrsFLT3yN8sEbRrGTfEpd1cVdet",
"15H7B8zqueFoAhr6RPwYmi7WRFG1LmUybw",
"1QDu8PCqUHBC2z2x9LXMqPdbf5pexXyQ22",
"14srPZWwWqZoswpAwvewCmqG2PjZPjgV4s",
"1FNCvSDoLE7wsmweZi4WQjmR2dgLjX3pys",
"14sMR4Q9XQW2S1AeEagkZk9ekVUYNzTrno",
"1FAFWQkVA1ZfVn2MoNqrvbYm9BGnu4QaVF",
"1E4MsquC6yYXWKDnC822b7EiypJDLF7rp9",
"1GPd56ZnWi6UnKZPyhwwCzU5MTp8VyZUEB",
"13K8NoVCASQisuEkGSuX16xCqqiquerKgh",
"1P56LcuueURGHfJLhes3V8pUhFDtV5hxfG",
"1Hq5UVwvgJmnT3DfETj6a8bjqtKexAsn3r",
"13XAC75A9rM1csqnv5Y6szTXJDH2JeZbbN",
"1GJSgMxPU3CvedRVYMs6SqCx4VYfktRVF",
"1KT99wDxeMwaj6G8NtkMcz83Y6xmaASouJ",
"1KbynoxvsrVewG86yi7vj67xXmfXgZ9tCY",
"1F5qKVYH4gBDiFxPVvCQCmmHy1eFwjv75k",
"15PXsieiuubW8pCqYZZG418FGYrmpkWFqB",
"17u1aqEiaunyUfczHwERoWfNzU5QagnzxZ",
"1Duhi7oiPWvrbAuPdWS94AdEBFGqcw8t7W",
"17Bra2HJdP6CDLxQ3kk43Ba1c2cA9HBXo8",
"1H6T1m22Qiexx6QGAPGdEfcmXAhbeMR8Nd",
"1H2AGw7Q7kCANtdtBxPaJoXYZWkGcxrR2h",
"1Bb2h941zyWEW3PWfSm3mXHgTkNCBQjTGP",
"16nV2214CHBfBk9HN2S1kdZKG5ryqo9p6n",
"18PjqE41SZtnbTCrREgyH5SaDUbXztq4KQ",
"1AxDdo8t2Aqnbh8WMGaZC2oLtEnH4C9LpP",
"1CwbjTQZz7HWV7jb6vyEW88uRLL2ih9NGS",
"17RWhjpKJXaRRT853zbZoM7XnbuJGv9Aok",
"1GJLfFzGV5nwYHK1R5qdE2Q5LiafzFLimU",
"1FN9iU55DUcD6Z8rSunqY3vpuqp5MMFTQV",
"18RET4gMJpmTL7LrHVDpRaPp3zzQdhWrgZ",
"14Cj23JKnuSzfjX7qXzAWSeRuCKcBtKrhy",
"1LZDXFrtXb972BuRj7noL1Fn1KiMLnU6B5",
"1QBVtfydpnykf734nDRnJ5zFoQHFCiUQTa",
"1JWYtmVaCJhDuzvgt1knRJMTcCoHJXxtwL",
"1BBDwDN8k33ZN5thAPKeU5UKwBpPkAdcrk",
"1KowEG6rTXpSbqUKqYS5h2SzJKUXEj4msK",
"171KnND98V8cFUwDypCMY27PPHcw9AZ6AW",
"1JjCPV6E8CRea9ZZPsFEnxs5fNcXHfL2VR",
"19G948HfDWTEPkUp3qpRz6GQNKNcrPFmD3",
"1E58EcRWoqEkRsVWrF7uCEUGdxt4Y3VUPT",
"18D7Y9Rjiuzd9Z1QShvqgPdD3ZcieqNK3t",
"1DUNpK5zsJ2nBLxcrpDk6jVa6nH3V1KLSo",
"15BoyVwt5NVuYK8Y9H1zLJM7wwB52YGD11",
"177npuhU4aBNfqtoD2bVRkhF4B7VcpuWHx",
"1E1rCA5ajHwHTp3JYbwv64NoePD42AfThW",
"1QARo6yg5CkWtC2YV8FYmSVdK9r5zwQ3aB",
"1Jv7EDkRyAB6vMK4vww8dGX3zQfvXL3Ao8",
"1Ayv3sMv6pzQ8vCjrunkDx5MtcA6mjxkAC",
"1Jkts3tLffa9gLU3LbeCaQXBwMWStDXFmE",
"1PpJAvTCLxVUGrU6HYWZi8n2W4C8vnkswA",
"18b1NqtVnAkc7tFeyM2ycaPwmjv4DAjXp",
"16Aismj3c134aG3qdBaGgH1UqoaTtVK7SW",
"1AuLFWtP9krsqce6QqPVJ8xU9pEFPcTzYc",
"12wzA1QKur4KckgCP1YCHqh9PKNf2hv5iE",
"1Q3Lbo2UJUnESR68iMoADStAGr7Ri88rqb",
"1F4KU7JfTgQ61Grj7AeNBZS17jMx7fhMPD",
"13naBqEpsafiMrAbCbQEDfMRPtrn5cxUAN",
"1CwhsYh7Nk5QwLyW9tVJ8W6VZwRCTUDhkj",
"1FJxJfZRkg5qMpoe3CGRKwvATDQjDc7f2a",
"1E7xvDyvXNNKRg7eHxv89BDVNptDUFqtNq",
"1Cj4oVe1JWj846fBuuD1kzsAFi1GbNauEo",
"1G1EAZSregpPCXeXPesHPrptSiXRoKJcRw",
"1F4vkuDNTF36wBRoj1n6AqGsPaPJtHzQdH",
"16n6PXLAr1s8tjiT5ugMat3Wm4p8YAnZdg",
"11QJXSeLVZSRBUx8FSq8p5T1M1pSuKH9R",
"1JWPhzb1dMpqzaS2cXWijq8M6rtksyhbyH",
"1LQqKrbXdQBmUPV8FRB4GEhUgZFr1Hvzih",
"1ULMkjKiq8usQguiDZ7TMg7TdjxWrdQER",
"1DxXjsNtTXGtqnmCTCVTN3cLQHyZqVvfen",
"1MhS1w2izxR6Bzkm8Y2dQoeyLW9AbjcGuf",
"14CMNTJm6Anp36Aa2p6FpPUefHGiDL5BuK",
"1Hf2SfhQE8acaMtrGAca5uD88vgxEWzAQ7",
"19HFnXWC6dbggnJZrAdByTGtLRYGrTSjXG",
"1LUscocaskQHN1wdheXgDLPcJAF7zC5ZCs",
"13wEuBboVbJNCVSRoUmvYfndi5GKocCMmQ",
"1CF1EgiKqFMfvKGLG3XEkEcD4fKT1jgMNe",
"1ZCcrAtvmmZ2TeQYAZRB7Z4bbCbTGMmDo",
"1FWNdqQXVRNsJxWTTk6Vd8rmhFGuGujEmB",
"17rqxx3MtJR7YNqAwEje9Fegfa3eNfZ8M",
"16LXo28yxTTiLyTu8DoFeUd9aby92dL4Fc",
"14Pa1G6GoigMrB6JY3eue4STCcuZbNAipy",
"1PEB9QQLYyGnjJFc6NdUpJuAQMcab2ishw",
"1Bjy484amGWLv2SYm4vq8Jj2mtmcbrvZxF",
"18Sci9cRVhRtzgXm2zfEWiYUSWX2caAWWR",
"1MV1RqS6aGuxvU4qPY4HzdASD6YM5NuTj6",
"1URoRmbtMaZP6Et6gH2pZ6BQLduWGfMT9",
"1BYxMmC2zaoASrqezAK4Hh8eSkvFQxPy3r",
"175JVGzmrXmFBMMd8xGV2a9qaGMN1KvFFU",
"12Hin1Wdm9Gh986fVSJfD36YUPf6ngPvSu",
"1BDMqEMVHCbQF9uxgNvJuPuuTMnduMa1WZ",
"1EjYBXCcC95u6B7dvgWg7kCExSTVBEoHXD",
"1NEg3mANzDjNjX9NgimFEaf7VrSUJ8k19P",
"1DKMBwioJMKL9pk17VXAUbEDF7EiFHxYEj",
"16EWJsK2AKZ2jpCEeMNqDjg7T9hU7CJGb6",
"1G39HVVeYDVWspRaLf4LSX2m5rzvRYu92a",
"1DB36wJATUPxZepuUPSTjTFgaHgLYEZr2M",
"1PakxNiGaCKt9HCJ5hqpC1ESTxHwsa3wEv",
"1NUQriA7gwFRf5BGjVicQJmQykCRqNu9wf",
"15nnQoUPCd1NcXfCmY1uz3BeVFfN9X9Ah8",
"1MUZh29vyRVxaUkPp9LNaXKLd59SkKq9dH",
"1MWKpqFb52BfoNJpjFUz7j7bHUqayiLrTi",
"1BphnsrHuWTnz46QmmbYwCoWKZwazsTEsU",
"18iaRTEpRQ84oqrCpFdvbKtpwfbGs555cs",
"1LxuntuUPqpkdwGoMcW4euVF6AdXvc2P6A",
"1K6EkNodnZ8tXsy5Ebx8WorE6NEUf7aCTT",
"1PMaoYmZQeeFzGcAuu25zMnZE9YjLhKfuc",
"1BmYztbMo4zqTbhx7PipfYEb9m5ANaWGSM",
"1K3UUdtiCWqRDjYBPzMN4VdGavenzLxdeA",
"1PLbb4X1W4bNTGr8zT2f9TPpMvmDj1tfoa",
"19rKHzChdpKHa7gat7F3rnzu6iihqTCNhk",
"1EqA3qpZssSnFi5AfcdxUiG1bQBNFDQUfX",
"16HokQwbh947dqB6EMvJuzy6gNRBu5nTSE",
"17XtdrfxH3AEiaAktMkCm2TExmQQkBUsor",
"1Q8Mi8sGCgK83smkutLYyRaBXsyjSAZQ89",
"12eSQJoBZKzWB7c3mNi579TkK5hmZuaAPX",
"15KRmAZcfxF4fw8e5isTDHENQj38ByXGX4",
"14UG644CKpEdxPtUpSHPy5hG4mheFQ37Tx",
"1BYyCxMptNSeFWgWftTghdB6xzxD1ftVQF",
"1FC5iinzGgE5278eU6QsEh4RNhoJAA4Pgf",
"13f1aHGxcXBabBVEPaSe51HS76xUkdAPtn",
"1K1LtG6rS1Rqv97vzBXKLRuip33KJ1xWqu",
"14DQAJEiaQSZuLtYDePgTiahAChbjbqnPF",
"1KvZQ7C38aNYPA4oWPjTeEA2FZzrWo34TA",
"18wkdRjgpZpkoR2489NQx1sKxSQdbZCPK1",
"16CP5eoBs76jBjjwsHuYZMKAUjwARwd4LV",
"1Cyv4BkRbrAHGRmXebG6FK7hhBnEAgm3Cz",
"18yUiJn16s6ERH6eXu2Zorxa1WKfgdnzGZ",
"18qMsRAjL3sqcB9kkKQPbeVjF4yMWALSyg",
"1KJfra27TLNhdeF1sAFVpHVvJ5MXQydogn",
"17pHzZSBU5Agw4QN4zH4DKZG7c2L2VabgY",
"1MAJhGszLvmqRnqfuN8Vhoa8ECt6MJzs2p",
"1J1gHybYSeesMnYX6xmsped6xNw2e9Qgit",
"1CAudbNuPBwiPh9UXJgPfLyuPLi6xiUoCw",
"13TTHzFJQKjf427oD1F28CpuMErx3CBEpR",
"17NNyW373G2VukgGrZqmGn8DKULx64j1Jj",
"1276p6Jb834rf4FoRwGMA8fDMgAC7XJjY7",
"1DjN91fpiEXmC1hVUFX7XtcR2nMm1mPubr",
"1KWPSwaM1tqk9U6ScJdymvSaaogmvoynvq",
"1K6ZFUdnSo3ZrUkWX2gbkS3LTCxQLpfVXJ",
"1uuye5pLcgPySXyFwq5jiEBcD1U3vraAi",
"174LJXVG8vkkFTnR3mjQHrg1UkqMBVbJe7",
"13yHo6LsegNC1AaLF7tUw6MCd7aAn5LVv4",
"1Krm8rtpQ8ve31La5HzmhZU5kNPfTeoQAj",
"12gvkenRL8RjAKcYE2pRb9ycLoSZwWAMxT",
"1JYKJDPoajmZk7rXqGSyvMoUWLdV7gt5nY",
"17dn35CUAvezTJMD6kxJni3c81HcKN1FFM",
"1F5c8yANZkQEUzL998Ftck4KjWHLc25mRb",
"1FEwpHBrukmqa7sAH9n63fpzkQFsZYawck",
"1NqUdYUjD94zdGGaJfGwpir76g7xTD8M46",
"1qMEoC2qEtBQXdagR7ZgfVe3LVCxj4XDw",
"17v6jT36VGjnjpRaUzEJsGvsYczYxMAbby",
"1NFXFr9SZ2WLhme3dVQtqPJMMibT55pbnJ",
"1L4WWvFTtZsWYPm2XJP6DrFUEgHYCr9MmH",
"18Upu68N4fYJWhDfn8dBDANSN8xysH7huT",
"19NBJ94xqmCSRaUKJSzjn1NR3mseiBaHCV",
"14myt4JNFpiwXNKpr9xxZRNL9s8Rixknrp",
"17E7Nv4ogbJGVn96YUkYzUV1GKn9h922vN",
"1HPsw9m9TEvMuY1SvqZmNEzCk1xyyLRCo4",
"1DBLcnPfB9gsnajNSSytfqU1h9LcaLmdT8",
"1AEuhkqZh6oFo7pipwg9XAEoQqCMbWBJkT",
"1BVz5cUgD2G4tZfG5r8tVQAmELB8nBuHcp",
"1F7BfzwyQxckxF7H5G1WJuEa58gLMCYwAa",
"1BNvnBzp1RP9RWSBUMmPA8cmsq3gL5eTHD",
"1M3RCxJks3P8wMJFfFNxPA5nKMiKE7pk7y",
"1LA372XDFcUsHogAXjEPgsYYiEhauvN7rF",
"15kEMnAVmAyHpGxwMiJ6KHEJkCeAxp1YBL",
"1fRjMwzS3SxNZ5dvVcPo37dg3mHWSppkr",
"14m1RQtEE75g2g2ZAQTbZqcE1YRALkpFQD",
"17Y5bzvz2P6dD64UtQAHaQWXRdJECMK5xw",
"15troBviRN4ctz4BWLngPte4qjKkMLFEic",
"1766QXFLSVG51wuHmmqxLj811G9yZxKhqt",
"154Yf5PtWmwEre2Q53ZBFepgMb2r5BGF3Y",
"183UELtGXn1wYGmdr64Uu19KFv4Ba4Fjo6",
"19ddFAmZHbaczzisf7Wi586E2njvCfMCRa",
"1D2AZ6sWWBrCHSLWFfZuiV2YEupmxyKvVm",
"1Jf9Wsci4cfAJjzZCw7puTgzBXREfjnEZT",
"162ZSgBLq2VFvCPCd27KtYG641eChp6BX7",
"1JqRP2r5i3qpsNCCi6g6i5JhZhcV9EmhfN",
"19NkckWNqAdTrTDx77FwAWNy3Sgg5hhQVg",
"1HXwNMfJTkuKYNkTDcNuiUTvcTnP4gdM7h",
"13TFqtQ82p87fUyQZDKwayzKgdbDE4YCQX",
"1Cfq72eVoszqogqpMvFg3LJKKWGeypHLCk",
"15i7ZW1pbxE3qqdS3Rkm5429sfyDYg7j1y",
"1HRCwkGEGGMU9aXPpqMMmxf69mJwpGA4NF",
"1MTi6iWUV8cgMWu2pDLbVqiadq5S26Ypye",
"1PGaNHHsS1oN5Z6ue4n81tDvcf6mohY5r6",
"1CxyTt8hfpe84NwePHF47bhyTdD4SmNfdW",
"14JBiTbxWFQMJDv7QAx8K3GB9BMMGuys4w",
"15Ki5rcoVbW4iKvfsntUQSCq5yRsi6wAAc",
"18cJcjajiPUJqEn9yK1e3JnAKqo2rEVdDG",
"1JPd178oh3T2ggCqocqhztYHowkzAnmZQM",
"1abr7kmcBZcqkh67wYXxmGG6cywZiUNvv",
"1A21YeLgFhWDQL5pGc5CT2Y9ehSwkqGDVf",
"19xX32vLzRcLA86wrEFYuqTiz5HE1FLYQu",
"13n4X9YCkWphVxaDxS7E2EB3X144XpU4VG",
"17y9VJa94XHNPjDVYgg4HhVNNsukza8j44",
"1KpnvqeYocghRBxrubyrueYe5XeqZbvrYJ",
"1L2xR28ayGsbvgXZPkbx3BruZXzpYDvLXj",
"1DPXtZ1T1nFKzXc1i6utmzgWF8kL3ouqPk",
"1CxgEYk5quEyKH9hrPDzoFsVEKLb7Z6QY",
"1Lf6FqGH92avUBANCgtfjebPav54eaqfEK",
"1JH5aoMJtyWb2mbcn9EjzWtzQfjicPHDpk",
"17TvAoJTXUDFPdAXs66bvPicMaUTJhcpKb",
"1J2GK6KnhMYkkGpMpxmsknsdeHADoJomVS",
"1Py1vxSEkMuBoLvWKR4S2cyVTuavEPnHto",
"1PyL7HKLGLQUpF8ofRLXNok4Ssrj25EkHL",
"1HF8JiLXEar39NoVadM6yukvKpfNxXQqzS",
"1Au3rcDhuf7PYQUH9WkbvmcQdQHA1vma5i",
"1C1a59VPsbBi4wFZ2uaW6SVHJeDsEEc8uM",
"1NeCAvKrX5K5LidWtM8T2mSMtsAM6W5akh",
"1946nVmkBiHH4TtcGnH4yg1gfx1d5t8Kw1",
"13ZWPNFAbdxqjUWPHwyrSv4kG72pbnEdSW",
"18kcfYVKGhnDdM498xbKokjgyGRhweytkD",
"16ShuBnR5Rg17fSoy7j54p6ma5sJaGEHer",
"1Lws7pR8wA83mMSHLjxmydAfGNarVniQT9",
"1NgSuRAi4BV7bTtiRjkzAyb1KT3wDKrKKw",
"13bXviD4Rdsi93yXELE5FB3Y8ZfaDcep2d",
"1Mr8Z61g4HzZVJqUiScu45X5efTZTpszZd",
"15YjNVUWUNNbfc2DdrZCCsnzzDrP61PbFJ",
"1Ja8Jb212ZFYpML7DWDPruUnuUW9WKkwvs",
"17EN7mrd8uqKo7mABwNSFhrYEmKdx2rcVf",
"1JzqGAARnHQn1nRr17S5Qn74aq4MUwVrNF",
"1N7Zx92MCAzFs4JPVyYjs15XvyBoMsjaVJ",
"1L9dp7LDBXYh6yDRrLp3gmvapfopMguA7S",
"18q1ijRzxNrWEjCzMQXofNTuae42RyUKdZ",
"1QGyUTngenoMBHPMwaCiQUMBeUevTkbs6A",
"1GKNu4cpFs7trY1No8RAnLHTkRVyNZbCDn",
"1LoCQ6aEyaD5v5RePdenbXTUZhwUvakWVk",
"14zyngnuDneHR3YdB2V76uR9DeecxL2DoM",
"1CY645iq2gcbKmPixctu3MwqHs1Piu3VZ9",
"12YQUgquGK9mcxGnLxqH2LVSHGQK93NkRR",
"1Dft5XD4mWAUNooC3hiubmdoCG4rcHbf9z",
"1K1BgtbLnLAgxm9XuVL8cMCZ2DubrqEswp",
"1CMnRy2MX4WpEsi37h8B3NapjAygB5i5PS",
"1DjWCJ9WMVNax2iidFmU94jUTTFjLkmzmq",
"1NVHG26SLj1vzXsJPCzzbS96yoQ8cAgnYW",
"1AGbuhuC4Wc1MBMChbTutBcXDefJ9Un88o",
"15RDvTLwPYcqrrd2L9nM3EjU5VUsaC8V6z",
"1F8Bs8YgWpaT5hn2sAo8L79bBCxjo45GeC",
"1P4Ph8DHeRQJtdMWW6BFR22GGGhdcHduon",
"1FJk9MeBPL7QjH5xpe5FF83g2iVjyK3MzQ",
"15rr5Vws4gfJfoo1M3mC5qTSA5mEPYQnzR",
"1G7DBJpT21XgVeg7M2wztJYbXYwGfYXz7N",
"1G5M5SuH8hEUNDaHgRCVUk5TRD6fLVtyk3",
"1FDaZLJnx8bvsBPJkhtR7hnhmjMyWzAcZK",
"1BkbGc5voeFt1J8YvG6DdNNayLFPTnY5wY",
"1DqCYyp58oEWKuhYSL1Xrhhq2bzRZLXm2W",
"16MpE5HZQDepKGehVPGF88XC71YNsQSKrh",
"174npWqfnFnbNFUXV7RGi2pMkFWsWL39PP",
"1Lq6pypUhufaCVqXqAmkib8FYLunAKG7Hc",
"168utKcGQcYPma8FManZpsQmDy1epzxsoV",
"1DPabghg1iEM9T1g5n8weTAG2EQEgg5vWX",
"12Rbp56qkRnjE9KDPc5ziaUY2mxJZnkihw",
"1MPt24VLaryTip1eLgYtDQQe7HaVUBrX1V",
"16dk3bkpppeXSb3v9h3aXQGjg5A4uoVzT4",
"1MTHfKk3xBARiRbuGDit1d9LUNwiBq8vin",
"1fqCT2v9yGm4yeVXLvYWJyEQXjQfGqRMg",
"1PFzzAUzDLToNV24qGKb2HqafvPAtgfMuJ",
"1BxhXQX2u8a83JXAqkDYimp9YS6JzwXnub",
"1CeAse8NeZT8fphH7yMt2cLBnDfUY7pNVR",
"13iTeqc554gGKmFwsusNyspmpyhE6GYb2P",
"18LMrdpiojQDHrAgk4GqGKaPKSEb8L7Nkz",
"1BqgVPW3UPRbj97bernXpfeBffkyuQSCxu",
"175DyasDHkEMJiuFZbVzGLLfXXzvMAMtof",
"13W28bc6qiYhstvu7kbesFAMQriuoc4i4C",
"17kpbecCWMtsWQfY8ywmDReksn56bLQwVe",
"1BCPfZZLAGGgZXrYfDBD4HHnjzTckHfM87",
"14mXmatN2wWWJUDTFe49LqbvWgLybh93Vu",
"15fSxh3XHCCzUMrZo4A4FkbDSiQ9jrXKga",
"1BfsSkETnwwogJjHiA1oGirYsD9L967RQj",
"1G4YzS9yc4e4yiAz6UFdsqLynyerXKyQBa",
"1MvTFHBq5aUmCXDPePzod9VaJasrx2cGfJ",
"12KDTP8mebFAUCgs2zh1nAJgkaRxUWzi8D",
"1KX3yAN1ChJ1bNZxzST3948Bv7Mq4y8QDd",
"18jfZWuxzWDagcgVRYoE7CrrD4yCx8tGNw",
"1A3SviEew2yZnjfNY9i8jGK5FssQF8M24A",
"1Po6VrUuVph9UVumsjtMssWtCCmH5MhwU2",
"1MQB5Ln1id7ADwJV5nNh2XBExExe532G7F",
"1LXNe4chLTnqf1kJoHqV9n6UapKiZHngPq",
"1NiytA2HJG8DkFhnsMK2hozsR8Vmhpe6nQ",
"1GrtPQzDuXA9bqsuXDBgnYxU4LQCapkisd",
"17vu3Y9SrnAbmBPREsWifw1Np7dynkTPid",
"1PfdthKRf5D4a2KvpZZuMSFjqhWTFk7Gsu",
"1Pp5rFhmgBENksktUTEuK57jKatqYsuQnU",
"1Hfwks6CJNhnMRo6DcuRuDDAq7PVGspaRF",
"148kTDaw65kqyMJZrgLfRT1LfsitQAqq8L",
"1LtwWwzjaW5a3BfyKJqCs1R4HUt1uhRpfq",
"125B4XLU8ecWJKniLpEaquZyRzrMENM8gR",
"14t842AVbfZ5NLxRTCYu3SKJxPUS8tvWZu",
"1GWnAZjFNBJCUYTSdgP3SXs7Lxfnp76NcS",
"1DCtuWHCVwij7fc3jmKzUbEoQT1MJrgBb8",
"134cbj2PPDrVspWiq2VBeCo8MudPM5igkg",
"146ePkApvgpw8qMMPD1evra4ixvKvHTycV",
"1BedNqeqYpfWnHCc6spo8iKdpbqRhjPXUg",
"1NDRwCE9j6FncbrRovBYQy2b3UQDzrHUoL",
"1PZzrpyXDiwSiYw4XryHBSsC3PWM3veKAK",
"18LoHRizHxubAicZJYmu5jXx2PFFiCCCNv",
"1HiXJDobGYRSzNgbnsM5XPuqWYGLmPUEC3",
"1JYcR6eHqCJRrdr2218HqUanqxgXXGUF84",
"15M33VT7upjjNedm4NuAPE1Qgo6vdisgSR",
"1Azx7yQgS7NCSgsKUwe1ao7MZqVRDN3mT5",
"1A2U8Qg9HuZ7t7aD94j7tfHx89U54J5Ww5",
"1Agsg96oMti2UtButbZXo5R5qahtDyHr88",
"15EgyCcjYVGQvgKGhCmKvGQHi7nK7tFZBR",
"1MbDLm2QX1w5VXR6AttvkexLEjmTSpsoKp",
"1McPjXEdHCZ48oojMQ1bKADbjmsapousap",
"1CKEWfrAZHcD6JazHaSFyDdqB75yd7igXn",
"114oDKB3JSWM5jxvkfrpnwnVpCK8gdLiK1",
"15wwc4P74Y81iWpFWiVGgSDtdAGHZMHfDK",
"1KJqpuNvDPFrCD8KfF5eAURj2JdyyobhKk",
"17sXKQNzg6PGYyrFHgvpN5g74LZ9KchPny",
"139b2yskrSTLNA179wod3VP5V1CpdVoTwt",
"16NeHRoU3xDfwuwYk7ESne6DTk9vGfKiEE",
"1MfecUGAp2p2RVErapEBYSYA3hWNGhGWmP",
"1GushgCwZnGCN52bL39Lbwb4w3XmA1NPZN",
"17YH4vAGt8GQXexVSo9FioUwcY89DMHjgH",
"18Bo83aQTnurCybn9F79eoe8Lmhdj3ysjY",
"1KdX8Jf7eJg12Bq2hfXYXexw5XMhtPPZuQ",
"138DEyGnXRaLACBkjiVq3ezV984tqdg882",
"19rcedqWiTGN3WpVDFvzJmrzA5D5GrnwvJ",
"19vdS2JiZArhsf9YAN4Za8gYh1o82CqmPJ",
"1EWD2pouYYwbaHrvCrsTH4U5BUQxxtXDHy",
"1KxxfcLFFhx7PxCDGMXwVgzbZxiqzcHg4u",
"1LqRQ2FtGHd6UbidTCGGxzjxrZ1b5vGv3T",
"1GNWoX4z1h2wXey68CgqYTTnjeiyWhXEDk",
"1FVNQfKYMM6fQtAP3tr46onMf2MAozxkFy",
"1m7ENCXKDiA56gawj96YtQsR6e7hVKobL",
"1H3STCG464LSDamHVJw6BZAWZHv8VQSf9Y",
"1FEP5hTZsnU9NJvmDs2GdgogGVaG9gCcMk",
"1C86huzczn9zt9zBQX8s2z17mayhxPLmaj",
"1138ArkfMbvLpzHD3RvbDdxwKdF7HgUJT4",
"1K492nNbtPFCsH9hq2mvVqfQDz5Dk3yUgv",
"1DVAPAWZDQhzrqsHtvW91y1UW5gD8rCVWC",
"1C7Wc1wRiFUqMXKGNHpXBnoJHySELf59Ms",
"194AP2pBqnPEtQA7Egi8Xz2jP3PpRCPop3",
"1NNooNBkUQeaQ4MBW7XxP6P3AmCqLEduvi",
"1HTjWR7JnNYeofirjq9ZBq92mgaS11sYR1",
"16KBXMfLk3hafMzDs2zQaqGf5pUhgh7VX9",
"19WM5bgKaFPhSS48yUp15Je6WEsF2SGLVQ",
"1Eo9fsKf1xrkGVoTY9GFRLqc9BAHVBBAfG",
"1GhMmQ24dkJm22b6oZNdXq1LiVg6Fa6eCi",
"19CaZ9ADg6cxU93HeWAzgU1mhmCjg7ym2S",
"14Q41kYiP9mGxys8tA1Xekpw3xDbKq7Ki1",
"1EaJksAdonqnQ6iqrXfmX1Nr7tUrf6eaSh",
"1Ki9WmYBKUkPreJCUjPDwimcdyhUBcbq6T",
"19F6f7ARHnbJChVhZ8ZE1hxnfaf5fywTjJ",
"1Lb86z7jUSfBXjTka7vTHYHvAXxTXPfYmT",
"187ubGVMJswEVyxP8ZAYoqwqcDgiQvz19i",
"14NMwHHHKYC5NEz8viWKE7KiYrKHUmj5hq",
"1Af3ifF5RbBCecyZKTZMUoQS76zTVgeNZi",
"19bAP5xVMdNmzHkDme5xi5pqSSf7bxde35",
"1HxRevyr9JMV2iWaE3rfkuj9kRXZeRAnSy",
"18iik5z8CbJ6j1b3YBr4bDdUykVUhMY29T",
"1GnmKAL3w849AHL6ZzQ2JkUvrpWkdoo8EX",
"12hF6XYmpeTZ4M3fYe4arLPLoPiY9LtrtR",
"1MCV4wk6N1oW98oAn3ULKNwrs8gLaY4Eff",
"1EadixjhvT5hejmcbhQNNHzkXx6vd7sPkA",
"1FkdmNt7eFTzTMzsP3ZVv8uxe9ZYg79GeN",
"1GAKy5z1tQ6gAjFNpnQXTND7dtZ134zzX9",
"17aaroVabZLQNr2DWoQKZh3Xbs6YaHrhQW",
"1AqDmGbTFLumZB59VtqqzLt7kFY8AmommM",
"1NCtH3t8hzpFv8BDLLortVoZPTD489kwsC",
"1H2YxbDD4peeBazsV1VBZrfeG7CxX5TjFf",
"1Q9w38NPBduZun2LcGNyf9LWpGBZ85pWio",
"1Mp33WJwkPTqBJLRP5b52KgTwGLF6mmQ14",
"1K5enJZHHePU1wR6L8kzW3QwTAqbdHnY5",
"145s9eQHak7xxU1EKJXQAi4eT63gTYrDjW",
"1FzdZkDgHR1g31HahAhpLq1uuZEDBFdMBg",
"12AW3MszoUAngVuVPHUuSRLey3CfUkSDst",
"1HcKSLvgWw3g6jWB6bCS9WkQp4swyfJJgm",
"13XeFRL4KW45Ky4Px1EUya2t2sGHBSDzdW",
"1Gi4xDFtga7svDpiPgbdTC5AncuDx2WSGd",
"12dbeeXRYYTFVsRXk92TVJRC2zLFEpi9TK",
"16K6FA9pUREVqP4YHww5srBdgHCDG5fGV6",
"1J6Dem76PL68SnyB1NtZsSuTUJ4vB8HTaY",
"1JXw85TsKFu4t2BaqdDauizyEs7vcVTNs6",
"1HQtvr2k4aZWTVLLit1f5Qr38F77iP5BMy",
"1B3cH47MRj6N1r6cBA15h8JTe8UFZqeWN7",
"1PuSiDVitKnedGUH5MVUDRXLrkSabzroke",
"1PMtj9WmsbhMw2vDBem1r5EMjTawAywVmq",
"1CLS7YwGpycigDLUNXyStAcL4CZxgNLXKX",
"1HHKyJwnzqwEARnPt1BRusxnuapQp8pYXf",
"1PA7KPiGPC1Q8ZLUtActN8Lpw957EMFv1z",
"13pw8XCNvhU6tdrcz4HWReBif6qxj5c6PB",
"1NSP1avmuk8xohQH29r2m6TJVVeJt6dPdr",
"1PwUNVLRtyTucy8r65BxX72SyaksWWybCB",
"1bqVF58kiHDCtJqFMcn8pqa3N5RVepP66",
"1FaVFehAvNqed4JYVjnhsYiew63Devj6eG",
"1FM3rjxSFHiwUQzqJjeD5unW1mBMnwjH3F",
"1CW6HmUNEJkbQZHRL6tksswHXoo1mQs4NW",
"1NAfH9v4chS5qmPB8DiPxzeKuEMCSmuZsk",
"1jCtpX4xDWjfb7qM8n3N6eTq6LnbruVgK",
"1NZnDEPLpiaiVBtS319A7FXX9AYFR7izaC",
"1CX1DKVNMWK3NGkR2j3p8grpAw1CdxwgTt",
"14ZNPCtyYcURakpMnUHh7LgTVMm22ttH8i",
"1GTiZHQDMC8h6NFou9ZBEgZLetKmvNPuey",
"1NGb9mT4p82s9er5dH54FCu79jJMBGZu7K",
"194Dw8ofM2rKWM2fPxKX1QHM2tP695xmMZ",
"13g7gpPsYXaTsAp2niZR47cGnLcjTtjHPy",
"1BFj2mDU6jgrQTBrPiajt7VhdK9SC1XgmN",
"19ouwZdX5b7bpBd51X81pE74HhFn3u3Kot",
"18zD8Addbuvyr6wh3ntVBU8oByW3wp1wgp",
"1JbuBpCxSpULdMD1nDJYz5mUWZ3pNVzVL1",
"1CaLzeQApwc4QDFe9w3aCj2FUQ9jYQAhbG",
"1HeW7Udk6HdHK88JZSRC115ZXU4rjEmm4d",
"1BXi83RJ9DyJ5sziuWXbS7DK3PxxLVu1N5",
"1JDJZKKRGRswjCZ7yEcMU94UTBtRHZ1r8W",
"1NzZpaweh2z8S5dykjYjb82743zfNirKDw",
"13XQemYYaRvVnwGPSJCscUjwfSGP6FEPvX",
"16dUYq9MbRmpymuqjXaNsdNWvA17pSCCPL",
"1AbnGcdbJYoGRkcb2FfnzXbVrTv8c6ZRdh",
"1BXtoL916fEFhBUjVZaiArN7kuRQD8hFtq",
"1NQchMSbDmhbkDzDZ3xNVAAiqDnxioVAV8",
"1KresSPY6rxCjBNw6Bu9Jp68JsDCZJqGaa",
"1Ain1MBTFjyTSYBm5fThiLGFoVt6W6GbhC",
"1Dn6k4xgsUng2Q1gb8gSgvWskMjHHzZXB1",
"1BjFGaSJpY3d2BmekFQhEYPM1GkxfRG8sk",
"166hMzELVxSd4VHnghqsZo1d1Q7XuZh8Kj",
"1NovDQYkDfBmrsYktRswugN5UY19nabL7Q",
"1DUVJCkQ8AeYFMnkpr69ebj2PGCFmxvfsZ",
"1HgqSxHXyt7gRVAjBWYqNY1rWL82hwpQTC",
"1BbeKP2hfp1NsmH4hZZAGhL8aj7MxgkEE",
"1xi168hyCxNQJZtS7MDF9KWsVXzZfmsyX",
"1P2SB2rTB1XfD4ZaJUUo98eRpPgpaVNHKB",
"1PbBLpWVRFNiH4jCEbeFY4B8afdXFwfMRK",
"1Lo9SX1796S7QSFSqTi6qhvsmUq54DD7Zk",
"1C6vKVFkSkvVSj1Es6cV7SbPf8NngTAnoQ",
"1FvbmHvAS1P8vLBz7bCVSqkW69YfZ16vEz",
"1Lw8EFbtr1qivEr7BqLjeprQb3Bhem4KCR",
"1A1srNywNVF3nPX5Wb8PKJCdRt66vL7Ya8",
"18XGCmYEgCrQgv8oXDFoqjUqwNmJBicbYU",
"16SowWP9AG3wvAxNLDLcJNgWsGy2Lf8q8A",
"13wjxpzr5jWS6B9mtFnJKR3KyXbgGDm7xb",
"1Fqb8RV7RSMXPPcqp49odtZjRWt7x4AmFq",
"15p541MPh52pMt8dksTbApVPkpVfdVfUmd",
"1KB6NJWAdpGYGSeZboVVZPdBFBX48DfAoi",
"1CXXaHH9oxjquZYyzEyecGvf1M9uV8zopS",
"129qxPSU3Cn5hLCidoDqamPiEjgEkZCmSh",
"12Dp67x4B3K15kg91BbYsdWHSJRDmvqVjM",
"192qTrKh8rUUdkZCC27Cj1KFbudGRTKjTZ",
"18J36XK5EDJusdMLrdTrcMKncXTWs59ANy",
"1B3fgC7eDEyXJjtXD4ci7YqjuGUvqKDgMc",
"19Wc3rCQ8FGJ1E4NVUzExUTGBF7JP4roQi",
"1FR2mGCBWpc64r8qodj82CurbJs6iVADjQ",
"1F6sK8osYHRZNSTMhipf4o6qNv6N1A4hwg",
"1joJiJKQWdgeBSBhepSZocQsbTKrBVPut",
"1K84pXoidxZty4nCzTed29AAZsnp4rCEux",
"1CQfC9v31MamCneHDq6vwQCSm44VgTjvV9",
"1LVJrVENWm6jhpJ12L5jCP5dRihfJwhv87",
"1DEZZ9xBmBhjqY1b4poARnUczhzGnJXzhE",
"1FZp89Zoq3zaJSCaYXu9aJmm51xfsDt5rY",
"1LMHeNzaDY9ASVUpgzvEqWo9atAnk1gdT8",
"1G1pqEW9gBaCG64Ssgnfx4Mc5VACDtZo5m",
"12a7qrjAhtZhQgF1znzPjitrjHpiHSC514",
"1Kxyn6c2Gn1yUFPrzAXa7PaUoW7QmfEnzW",
"19EMwbWDT6enZm2GynRj3UX6h1QCYW3F1W",
"1CYLMDUh3CVkWk6gd9DvSwjwTzKeNZf8rb",
"1EANULHBurZP11rG75eLVD7Wbj1kk7Heyz",
"1GxL6guNQgrtZghePxFum7JCiK4cb6Mudd",
"141VyvFqXUmX4PExgbqgkU6JMobzjjykRM",
"1BzGrXK4RXjMVYHkxWr2BTcTUA5qyBnW1g",
"1F5QPbGYdxeqgkXcS1euZeaouEc5RRATs3",
"17QQoUbw7xHDt4c5nD7ZMxJNxVGZR3ohi2",
"1XbnbtPty3WvQtEHCdUkHg4CsgpxoYVX4",
"1AjM9gMucTs5Rj99kuoHee9pB8Nrf2QVNd",
"1GuB5HvwwJ14WPoDUSA14E6DK2r4Nj5nJp",
"13FoHvkGEMEzszBaGNX26PVd6ZyHKJC28y",
"17DVw3wkhwb8vYkvJAt6X63Uym8s8Q3xrG",
"18tgWnrCpbpFnNNR6fDmEHYQ59kDJim5Td",
"1FJbnNyKUEi6JjcPDBEgSuvtFoPuMXG8Cq",
"12rKfXfdYqwPGgBagpBrE65iizka2KbH9h",
"1152Bux8izEbXsvvPWU9HzHyJqLW9BhFME",
"1QBFVRbgPpSbcPAFo242Xz9opNtVeXK92v",
"1KYWjETuEjk7CoMMzC3Fh5wFppQvfP4sAJ",
"16L2rbUmNvK9YH2A8qiEws7aTbC8S8EehA",
"12ZocSYEN5TTJniNa8mrhFXTL6U52xJuf6",
"13JS5jLpEkZ6n3p3jDT2jQcDJnMsdvVFFQ",
"1KQER3RGx9fJU9vndUBPkucAbc5Dg2DK6w",
"1Chzn6s3dA8STvsjKfqDQRLuQw4gsnqhK7",
"1DiAJxo4BFAtaZVtB4rZqmn9zw4tbdnRbE",
"1PhXPP75FZy24JMAeAmwb9rsT7k4jcZ8fT",
"17817zHDUve8aPh4ayvfdqU2pS5D6jYdMP",
"1CaK4RFmxqCUVN8tfsJq4fxVdiXy6ziN6Z",
"1vm1duE1vmtqRKRy3NiyptQ199cvsFExa",
"1Ftv4urG5oM36qm8mSEUVCQD4fSjNX5GRn",
"19ixVMGjWLiyVHCx8CPTYYojX6qtdAT8WW",
"1MFKxXj6YMXHQN99jx1sYpCpAfBtgfs7a7",
"1ME5avdCaFMDDEhaspGDCAy3CdAMyWE5Md",
"1bNeuSSwx1EBF3zC9fzVjij5AkHzZu4TY",
"1BHVeLzCzbquNpweJQDgSHcbQqhoy2d6f8",
"1KEZScLAvCxzdDj3iCEPv6vDQAmKXwFYpd",
"1F4FNCdmQHXK6s56J3XPjQayQDf8PVxdbr",
"12um5Y4WV2sgpAUtGFhTxzsUS8Hd1Ze5jV",
"16b9EdTj9VGQxAp1dQ4dUTqsyTD6vpu4nC",
"18ygga9J7k3MHGeN8ApjpPzkN8ah6kiG49",
"1FiFSZq1oKP7sRkPrucVsEbkPRd9b4cck9",
"1DE4A8hVfisczsh86oAd56MRx7wcyVF1F",
"1K4KHcAqM4LbeMFRpygowCvF2kBvWbeHZh",
"13WAMXWaBmyh4NhnDeUZqDUqqpWFkrVGUf",
"1PNSJiSrNpg1mS7cgntzfEa5AMy3dYYnKX",
"1HhjuP1opD6BpEdvsJwLFYLrBoczPjZTXG",
"154XHmPoxTEBnUVZx3dEb5PqBn7YJGGnr7",
"1NooK6gwYXmtcHsCwjQxhcnrfGGWF9uvKh",
"13NM5BoynwJNkDhvdKQ49TSxYZryhE13ET",
"121puiMCsGCFKdjfYKyCw4W2Z1fU7rXgB1",
"1F98bWYxbcX8NFSd6uN6n5a8Zvnyf9egLd",
"169DShDUiJaGixYDTrhQkM5Y9KdWxUR3TB",
"17ocvcVLEmxMBD6mP6BTQYZxwmoQjse3id",
"19pJK33wY2g6fJX7j5afjwFi9SPykFPahE",
"1HAPbpoQyY8NHajkuxb89FzvJYvqBbBQ22",
"1EwCGktQHHwe3XDk7ug5VidxyCxCZHm3oe",
"18kLHMuYB2YRPcmyatVPSrf1WEgjsVFfoe",
"1JHGYvTjsFXLC1KzX8zJ1j5GMxoQZaF3wf",
"1MDBT2CskR8hxADRT63C4eqBbtUSpCHRi3",
"1BTRQZGZ5WJpHbmQXPLChvZUPnFPQSkSs5",
"1NPQzGdfhnGwTiGFzKki7okPDdt2h42qVV",
"1GcN2fgNUK1TNim7qD8sFN7k4xHUADxGPv",
"18JPwhMKhKJTRtUCpdrqPtyRNkNjAyq4BY",
"1GZG3JwKJ5xWaa4BNcHDxqHUegdJyXf87D",
"1N7dfGZj3Wpm8GKCBMLdALkJYvsnaSP4GZ",
"19eE5wL5Dfzz6P7mvru3RgBeztpC2dwRX8",
"1GSqTzUCXSAbowoSy27L3aPzVwMxbRy691",
"1N4fEVTZiMKSw1ycePwuZo19GbtzYFdrmh",
"1GGAHRhNU2jhsL6pjtpCRM41NxGkiiCqWN",
"1CWVKRTpPB6WAaGjHuY9WiWpmdbTsdsmrE",
"18QK4BieDBiYNTeTAtR6GY3D9WhVbhWqnJ",
"1H9m1biFuDzMDHsHPZ6ugPz5gXDm8GEpeB",
"1K8uoGx6YvxotrtMJx1neQBYbWWDYoWfoH",
"19B23aFysmLQURkxv3JKP6uf8QRm4mDzAq",
"12HsLVbnFxbwdHbhPoN3SvhDsdjL9Ft9tD",
"1GYZFCkoiqHCbTw5YrP662zN5mw58RXYGt",
"1EFE2aJKi8zNVf3Po2Z8mo1gaB97gV4f7t",
"1EChnMjT2TuRSh3mHbutwrLkqorkgqRAtM",
"1NJF3asQx4xvgZjUqFDmVKHVgamMKx3VFd",
"1BFpeVv1Rpjd1TpEhYF5BD37KuoMtec81j",
"18Y7msfhqsvoJ2snELRfrv4ymXepC6fynV",
"1G9LnnCXbdbrk1z4Smcg4rRLfvqpHiduP5",
"16WwEuQhD5kuZBbwRP73KiGborvvs6HqfP",
"1HmZovvdk8GjJsFkzYaG7SYcSesxyBbnHU",
"154hjwG145kJHw1qRsBbv1s3MrdQF2HDSF",
"1CXK7dXGqjYLLxiGkP3Gij7hueD8CbqH8b",
"1MNo1csg1SfZ7kAVXt3XY2iyuwoGRZ8fYK",
"1BSE7wDf2gfEqswSHJ9DeZmhGoN18SCe46",
"1482mrEyuooL9uqFvC8YdMYUb5qufNYUTS",
"18VQN2nYYZPPfTGDDW1mnkwcH63KHM2uRY",
"1DMSn7qLDm1yPMbcXh2fYhNfR33t69iTj4",
"1CMi61RQVX8pKL72vrboU8oJ3cCjzPMzWB",
"14iE8MtqEkeWJeofy5akTWtsh9xjEiX5XG",
"1Gc3w5fds1vLpuJFYcaA1Lwn8roNmUJ5Yi",
"1HCc6pHyJ561QCH9ecdVhyZSFeFbrz9Krs",
"1LnFYRNQ2QhrtyJcQcW25m1XaHMtSViE3Y",
"1NSdyMbyLgQ66SCUbPzyXSTcKguJfsMFjG",
"15jLNFNF9UAjT8sU1sz7XCCTmxFs4VgHHc",
"1LSdamnXFKjCLUtEYvLTVSwJqZcZZXhSu1",
"1MLBvMizqKpvguYtUTs2473M44oLf6Q2ZG",
"1sanUggiQwNGdD1Y58TGGyDEy9nXWZHnN",
"19BCZsX2TGozg5Fvn2bioTuM2tc92uwyM5",
"18MB8VXubfGzQTyLWxtqZL88rf82SvEaAk",
"12EyjmPoDfwFyezdGDMRTC9JLjYDFVYMRk",
"1KYdTDY7YXoBk5keAqrvKS9VYHaK94xXu",
"1HbWcr6paLJZR8HMiKwWwH96uUZiSwC5Hx",
"1HikBeQMk3UCmeAh1an48Ur8NQYTWqTmy3",
"1C5wVoNp7yAfVFaWmzn2tV2KQBxJr2Ued9",
"13zwkrW5hfnpUdKmNp8zxPa4NjLR9eqob3",
"152kZBa19kxEVyqa7LghxGJF65B1LVk6Lg",
"1LYfvRKHP9m5ie3K5DA26duoQNNwrJv49e",
"1NEUi3QMBewWYF8aB3kjV2iVLkvYjJabWU",
"132GbRpHwWEf6dFz11Rdnxd4BT8vCM6irD",
"18zQX6pJ97ECWiqhZMf8bGRv6JxeAnF7yz",
"17i5L1X24Dfpuzbi3BvgbF4WoJ7diZ8LDh",
"18G4jHid2vEe6APVeAsRJDKR2hqiQWTbHr",
"184cEydUPxBa32pMxJ7y5UcRNFEPCfvs7Z",
"15dEUbkCucCswozM5wi1YS7TUPoHFAgN5E",
"1K1tSoc7d4EUhTm8LKExBTwem3Ydmzqgio",
"1KDid4fBZvNxV6g6zicQvh8cWL6g3TnWqN",
"1AU4VBT8Bnqtxb5cQYSC5Wk58N3XiPehCz",
"1JsxKxA46LCAWM4SYwdKJNVgHA4Ax8iCKR",
"1HBwbx1FSTFNNHbWSbxFXAqcAyM2pbvEtR",
"17idac8wdS7WLm5ffd8uCW2WPHJLhdKeEX",
"1L2gccg8M21GMoMV9ZicgsM6JEifi9qmgx",
"1D1BwDUFzgEcuTtkEpuiT7Y5auCckKMUQb",
"18JkB6KdL2YFzna7QndDSP52byLoxkTr2S",
"1Moy3zWMESLg3nQgVxaENPCYzZdnTm4Hoq",
"1AAnvFLWvPJU6Z6oJCVViNaYLRExH99vka",
"1DEzjA52yEiwZAhNuwKUoHnyiQTxuZM3Dq",
"12hNqswQzqRrx6FLCmCktkRoDAzRcVFeMm",
"1BK65qYmvEnXKHW6sh699BZE4MC6KqZUfD",
"1LH2DLN5u8ZWnE9GjipMnv4RXLcjhJuust",
"19wFib3kTXRiQ4jFX6H8Ft3bn51ZcADmhE",
"14w6yd6P7ueL5pmm1u9tH7V2Bg1mdbcHKc",
"1N1NvfYjrDY8jj1yaqfQ3dnwvCTfswk8Py",
"1HTfFQFF8G2xTDbFLA565n1CVS53T7MehN",
"1FaWWCVrcpXYe36bv3W7pKvkVN6y9atzXs",
"1GMrpbPaRAV8nSEXUBDs8ks64XJZefLY6M",
"1P6zGZdQJ1yRWrTtGssjPHqizDQcCSqE7d",
"1Pry6WBcikMyz23ETCHPc2LJ9VKq2DG7f9",
"1NK4F8gENXoYHR1QxAvr2tap3MNENHC8J9",
"1Jp4ixPcjWXRep5BFFqqfLpb1ReQDcAcHJ",
"1F66uHQtzQ2QvunESnj94aKnxLmSQNmFMq",
"15qJozm9rLH8VMSfZtfpDuTGv9ETbH3npy",
"1KNmswGJWz7wKqn3k8Bb46TadTFEX8Anjy",
"16q4G8mf6a6v5EuEeBdmnpooCRBAT76wc1",
"1DKNCGQh5R2KnTGMbkagVxLT1nwj393yg9",
"14LTgxa7BEgFT1PxVJrLr98zWNGAJrEAXy",
"1PLWN7HwS7xUjvadMY1oESTA96c7XrDYWr",
"15eFkh8iAUgYzrHmuXFKmVQis3ghWXqtdN",
"1MoNnDZDmvbaNuUwqe4dWWFNAnCEbceuwu",
"1MrQVg8UPHjJedWwg1SywHuHkuWLPc3d3J",
"1NUWKTRvQYCuKzQoTzUYHv6vY9EkDrpvVk",
"1ETqKJkijqiS16KmvpgKDoqssgP56sM5oZ",
"18C1NxeJMb32JdxeFcgLeDwJSNeNUZww7m",
"13hkBh3g7cacKpepYt48kExAGu8ScacDXQ",
"12NEMhUGFR8UUR2Q8XiR7NNNB7ThDaw4wz",
"1GHMCcLTnNJBkhaMtXLqSMkhF8AifSyTZ8",
"1Jc3d3aqrGLfPBhdUvQKTiZkdB47qkoZ9g",
"199taMcpeXwkRXxWHksikAcYjoRWxAY2mZ",
"1MrCYFPd8kauB2cBQZCxxzHKC31QGHs34X",
"1KLVHTwQDiZKAX6rQacUKxj21TUQvrcHrR",
"1JNMLv6ywZSuKJkEfgqS4VPdv6E55ygTq",
"18sp7q61R5j13xUSte6csYT8tzEyAF5HhV",
"13KPkFJs6p9eYkXKuMxAcuNwXQtiM93c75",
"1GD6ceLFceWdmrjZie9dW5RmT3d8G8N2T1",
"1PtweVeCyp3FAAHjiwadeCA9EBKVyX9vWy",
"16x9GqFqNNCL8bm7YXaUC3Gp4F2D1Qs6zY",
"14dydCNuhawDYAPA6Q2Ht93ath3qyFx2XF",
"1sNYyjMwEvkYxP4Z2CwnqVe4AgjCxrBXa",
"16wSUHTfKRsK9TyC6stRQNEDmY5A28jYwd",
"19Ysk8By8wRs5hr8DJMMyGSDNMMA9z4eXg",
"1Ma3MtGeUPsdffZniQiAPDLqigaYMRminq",
"1JkPAka1oFqVtXqgYx77R9u5VLPtmMKzYT",
"13Ysgrez8xtPwjypwya5aSf2R2e6sdroFM",
"1Bg4ghw9W2oL2xQJMeWrys7ubxKEmEizMg",
"1BYWRoW8EmKZjsvkvB7PyAjkWdBa92jkzn",
"1Gt7gAgCRtMAXBYakqWbh2NSysxscPVA6y",
"17jvnxd16uHsDQ7VSV5FypEN5e6zJvPpoR",
"1GQHX58qz4WoLeWCjf7k1uXVNfQ3tyRgSe",
"1H4dJz5QtjxUgq3rCGAW8jbz64CUjBFhEZ",
"1MfiiFWfEPD34fypeaGs3ZPoE3nPPqW6xQ",
"1GyWj37wawf8yFHj77pKKJCirbe7WteCc1",
"1EktuSCnW9gw78iMXtutH421CZvUQdqdNu",
"1MtXNzrMHs6v2b2f71NKoyhs2bQEPAt8mq",
"1Fc1nSaZ5Lz3vmKkSNnZDtqFSQ9P6jP27n",
"18onKsfAkR6x6N2nNhDGpkrGxk247VVLku",
"13M2BdRkVWCHqZTf3Q4fVfcdTPDSAwbL3R",
"16NpToqq8SsMzXK2PLGhoU7n5MU25bfDyv",
"1PgNPo7sNWn8FpHYvCWfYuHD2aAMCnNo35",
"1NiNSWRfs9Dx1ntfUgEqANK5YcFUDLjtap",
"1GhZqYfLoo5baxuzt7LJ9fz4S2nax269oM",
"1DiqKj6gPyRgFN9jXPYa1qJZQC36irusyJ",
"1CJhNo8ADbdXY1XKym3NV4DXrPwoaGeYiU",
"19DL1uF1d1AA8skBEv489QUZf8W9YLZv17",
"1PpGPXUqZg5NjnanRghPuCiJdH6yw5a3Rt",
"1BkjwUcxGpYPkDHYuBEcxX6P4UJmMszsm3",
"1PThMZ5fhJQkWTPcPfcEew76jPXG2M2hik",
"1DduZ4sCbw2gscuUcpySRqQUdu6urBoysk",
"1E8HPp8MEW34Z1Tp99FxYbCe7corwMK8vY",
"17UxnrKtHHgN1N1ghKuCHfDV36nfEHuPei",
"1JbntCrspE84iSwsyjZBTSyoRk5Zd2PtSz",
"1FYNdjMi2J2RmXJDKgdnWXecM1WNMEWYmJ",
"1NEd699Y6SVhAjHJiQ2cr66fiGnKJH2jJH",
"1JP5Avs6WevjvMTKzADK7dGiojRGV2pKsf",
"16XHVdKfuYawDY2xwsvsvVRj44UM84md4y",
"1LoYSkL8NmqPvnxGgxMQuaTp1GX4v7jf13",
"1AsftdfwLQdftEYBhzxeecUCHYhpLRLaNp",
"1MxaS31pkvSYfeRgX3AnmVVUL6miHiWsUN",
"1Qot9QE666fT2DJsgyqMz1Yt27uYdpdGW",
"17mkQQn3BSW6vnLdA8RkwxFyRkexQoqqs1",
"1Kv98SJWH2XPJztx91WWHJ43rT1jrX7SZ7",
"1HG8ktSUbm2axzZebZVBZb5VQUEgPkjimU",
"18n3z6RNmV8CdYyv3oMxnoMR3VWYTpuABc",
"1NL2MFuUbau3rcAa8Xq7v86GAsxqUsGTCC",
"1P2VCQw8CELPMiHeyNjyy8r8ZgnWByzufD",
"1D8DNsizQQ8dWU8WXaHEtfj23jaHgSTDKT",
"14TfgSDbqqoLdAwfKZpPWgnwZs8a4dKqaV",
"1Gaf3knoLL8bZaid4BwX1iv2Pi7ZAApNg8",
"1GxdBSHKTHozcBRsiMxXMA8vJT2Q8mCWrH",
"1EKiSyuvZYAKGLNPfpXo6SEu8ndUpquCZ4",
"1CSUWzAyYKXHcNZqwiKtdxfTbBxXi83DyB",
"17xUNqAhV9K4Ay5ei5i3ybbVSwfLFeZG7S",
"19DRdeRj1K3h3edHdnc2KwxTPPbEr5BzHM",
"1ALP428iJ1eYLEU7gUtdWXv6BAvRtohgk2",
"1LkySCbVoYTK7mNNixmgWFwwyChQgfGwzB",
"1NSPhUQ4qkc5WqcS4fVzZk3iMJp3ubynts",
"1g6F4w5geYuLbfpj5DuTt8gZ8npvWNLAL",
"1DdoUBWQUub2bbPki2QUXhwV4QaXkaomUX",
"1MjvQUnYMGVxrV8ypZJkTr6bAGpB1mVppK",
"1LG4CNeCXtqVnxD6LzB2K3JwiXF7RJHm2S",
"1LjmbcEtfVqxgXmWV9YdqDTo8z5LQrVL9N",
"1B63mk5bz7dsuUjsyEM5Vg9R3uaLeosdZ7",
"18QX9m31sXtQZ99bLs1uQfSoPdULV7XeJC",
"13di5HYqnz8fYUdSFHfznnsyi4i63WZ78H",
"1Mh7wwaURYMmH8HpYTXdfpA88i6egJhi1K",
"12vb9T7c2SKhCca6UELazxiiJhxLqiBXMh",
"1AoJBBis1mVeFKFSXCaHiCHwU9hmcLDvwT",
"1MWCk1CQ9JVjjGStUFzjFYWcr7H1BnPHjc",
"1BL1DsqBYbeDXj7cotgGR6UigFyUWMtrWE",
"1P6oNTBSbYFKbdsTdnaKNp3dQS9rFQW5Je",
"1KdqV5WiNveJL3pvgLt6JZugfBzYKRGrCv",
"146xchFntkViecELXfXAiV7MLaaRnCk27y",
"16WB9R4eJze778sQ13Vbia2rHkRJoTuCKR",
"1BpjMqDZ1b9D5JTnPqLYKZFSRQv31buiiW",
"17dsSyuLawzMqNPzPjy6U6JteaGCuRXy5P",
"1E7oKhSEVcgYjwnnunwpLqksT2vGPyPZ85",
"18QEU7U6Kdrj8dDHVDQUeb1UGMWmngRgZt",
"14j44sKxWSpYixWr9XDfJzpdehZN9WQP1d",
"1G8e3fjz9hxXhagiTz3GXxtBjaG7dVYZTX",
"1D5e7rJFj7GFNGSpzANzDUZF79GKbn8W9m",
"1AAPZM7f1tQBQaJDta8jBbDnFoj6KLvDcp",
"1HWCccnzM2KB4vfCvZA3bHefgJ8B2LjxqY",
"1FrZ6GdBTJWqpFUtgsm6sM6cZ4exi9VPaZ",
"1c2niJoTvCep9pqmmEmMnNoJ3CXTLXQdH",
"1KDxPMrSDHPwR7BwpGjWCkHP7vKYvWQTJQ",
"1P3ciSbdRWh4aUf7Yxj23NHUEftUV9xXTG",
"1AxKhNxJDD6MoL7n98ywybQrBD5tDtySgF",
"146CQDtGU4Lwt78JmxwWjX6RNd9XVS9V8b",
"1NA5X91pfimtrfAmf7HqQPNCeiJVb6vJm4",
"16RNoR7qCDVvp4VjFg1Dqqo6dFMa5rQMnm",
"1AAiFWehHDLYvyRt6UK53a54o4BG5Pj95i",
"1NYChcPaSyKkiXWhnn5CPDV9vUzaYTaKuh",
"1XpUcFwqzqkkHWXHacuy5tzVmThJ4wjR3",
"13mzhoqbE5PgMi5ZRiLVpksJ1gMTEDXLoW",
"1F2qgNvHHB1LD1GXbD6bpWpt17YaJz8WDY",
"1Gr6SihxmwLpRriVDD9864rDTFhvy3rRrA",
"1AUt9aRpVj7X2AswbWtQxHkw68hrQNV49L",
"1JzhNZas9CGUqsKuG99Ehx5myk3TLTZSem",
"17SXQXFf5DQfU2wRyPCAarFmcmnX6P6Gjo",
"18XqjnYR6PZ95sosjWHbtbAiRigiPzHkAN",
"1L62mSSUaQf4EcCTU2o2V7jh6xeWG4VDLg",
"1NqmyHBvgnoLfR8Ww3W5HxpYNrJUqhdHiP",
"16moGVdvix6XvWKoPq3ZtCv2Mq4HbzCST9",
"1GhADA2jGh4cwXkf3fVP4o6mk9pxSCf1Kr",
"1EojPvFgGag5iQ8d5oQf1TapjDMmS6G3Ja",
"1DQiW9UQwSukUWKwgyVfB6n7mcfhpRcXX8",
"16CUHYCSNGqoGDSeT84gtGgK9CzGxYcrRu",
"1EibXyGJg7ZQSk1MRhii1fNEm1byUxEfQh",
"12gScsoQVUHNkFf9QM8msgWGtZSnKns9U1",
"1Q2YAEyuFdyfX5AXmqJh5sHdaQuDwLafDE",
"13nZ5xCnD3wdsQpfmTkEkzrokjj89DMtv9",
"1GzHQwo9N3cHdMj9yWsaiLAYoWtdQuGTb9",
"161EdSyQHxKwk7qERZThsTnRiAd8BDiCWJ",
"1PLEYS664njCEBCjRT2h55jo9svWiBKcaU",
"1BFC7adcT87EfkmWxYizhVdccCCyAFmR7j",
"1DJPa42U9BmdToZkBRJkEAshc6W123WsRi",
"1AQiJ3suiHj9tGK9bh8LTb48fd6H31wd5Y",
"1LJyszyerxCdCvdSViXvUCbgvDRveV9YT3",
"1H6BhxeCZiqtVfeAW557VmrciTBErHoHvK",
"1DEPFn323u61QVdWET6bCpAJS2rj5cib2D",
"19qK3uF3pnJS9MRHpTkhjWrR8VVMiMesRg",
"19Qq1ih2qRXm2Ba9xZcJyBqZ5uWYSM8eF3",
"16MuCb21oZVH4m3a3dx2uncHyQJzUucX8B",
"1A5gvoggXfBHfkYRAWTCZ4psqno54DBxiG",
"14ZC21dT8a7X2n5acsBzyfpSDi95ipcAxt",
"1HV3teE8oozTK5APVHYb3MkhgjVzYJ5BAu",
"19g9A1ftCpha2tGvBXHfSHF5pPnDCpWBm1",
"1GsoLmsJcn93RcPhWjMBmi2p6LeGHk4ZaV",
"14GvhHwTtqLKvPFn4KyfF5N71H4XDvUrDt",
"1MsoCD6zyiLJrxphq3ede3H4duTFTrBCNf",
"1MfFok5em9cEAwK4GaGiaWSoQBpRufrQJE",
"1Le3nLkbWkZa7sxUCtXFaWuFbefA8dKvQF",
"1CCuS2oZbMt3XQF25CLmT4GvDRdJnCcTXf",
"1BaQZS2BY67vuNJ3DirBFqDEoSWfaSKa65",
"17AG1gTuPCMh5YFp625L2K7A5DyjqdDcxU",
"1PNtAuSSyUdPpYXj4yh8KtXEkjuHYBiHNR",
"1M7KEXrAxwjoUQFEve1vL4q7vGTvLfB13m",
"152Uk1RGWLoZehXuZGkMAhVUp5NEwBjVZ4",
"1EU8dt7dZGArDLkFWjQfStndDct5NAr8se",
"1DPE67vkarmRFP13BP82qX6rD1rJi3kCjE",
"1BA1DRv1iDrptMK3fX4AWeeiXwEnrwCJY9",
"12Ph55Yj3uQqVjQQS3qM253jqyyLfe8F9H",
"1PdTYYWSiVuPui2iofUtD3iRveYEQECTaz",
"13Rhi7Gq6X2BoqxhywAKxuMwamaVwXtceP",
"1CtyJh3NBVYCMpvBAtggaBBRWKQ7qceWSX",
"15CCD8EhbnXtzmiC6bN7b7sfK1HG4xEn1J",
"1F8bXJ6nye3G6xJdbxCzPHsYVJGXeRXqYU",
"181xnBza6qzEzm68T3McEN4UMzgcJaJmsX",
"13QqjQa3x8QNZQtuB3xBprRKDz1gAfaJp6",
"1JWDYY2rWUufVGojHkfYSPVa3yygQgjmJm",
"1FvA7W26jRxQxVnSP3uoTU9g47WCCfkeqr",
"13FMez8edXKVDYK67QNjLs2DcNP7AdTGyn",
"15PjZouu5WbgTfvnHhoUycVXHbQxby1uz2",
"1Lg6cy9FXjsfWcbvVWh496GVqWgBAPHBpM",
"19nzYaGMq1b7a8cYFfn4jUMeiNCxBGt4TM",
"14DohcqFajBZ7967VSJsj7iXrSSryk5eXi",
"1B9ZfvCjVSdKvTaQVafRPwKpvXiZjsL7d6",
"1CaddGHaoEAqeWERTKa1rVFPUd6HSnGzKB",
"1qcY3X5vzMcxbKHDBDTY9vDGYttg2ap2H",
"1J6YoKVSvCKybxbeB5RgbJ8zFoVR2S7f8a",
"16xGM17fh9d55PGBZjYqEuNv9C4h8qdpVP",
"1B7z9u7WRk2RjzsLkJXPCmDGQM5iJsra89",
"1Z2HSYdsBoJ7YZhmfakjcSsZQXoRbUvbE",
"18i158ZXNnueTJ4gh4ij8SsKbz6qAbbzQj",
"1LWVcpeVNVgA1AaT8Q8gt5J6r5yXPvQUVb",
"1E29EK1xYkjfo2tsFbKAbSEiuTaQSs1L2V",
"1NsT48K1wjktKreWKrQmiXjQ8MKHv4NUCY",
"19mjqvErSKwKKJtGHm6fXhcG81UZqZtijf",
"1MshoDbdk2GiYEJizaN7Lv5kB8xpqCm6HC",
"1NpNwiRfn8YxWRUfCwZM5VndDXm1nPtHuo",
"1AfG8fewLMhAWNx16ons69bjxBsRyjQsSo",
"13Thr1StJUpKBLpBCcKdRFkg9nKTxhTLkT",
"1HbjjCXSFBQS2Fvwps5TsL8EshwVzjceen",
"1AoB7N5AaCBgvCr6w32oqEkM8fB2eTQpFy",
"1HQWgmTQj2DQN5U9tStUnAwTG4vgUKPLaa",
"12Wc97dyP83JyQZXffh85kacoi64UcFXEz",
"1L7HXkDnzi4RkyugoJkpADmMqLG4UP6AnE",
"1Kq4dSusmxzka8fbsAVzaq147fzbHVRoRc",
"18Q4ruB48iGcLND1PuPkkygPHAp9dZAgKE",
"1DVHFozgQdgBWWJ9erBmVAYFzCfgf8csFu",
"1AwkpweXuY4H371Wnb7T3jhxrfCiufBq8r",
"15wbhosc4NgHxurBGhR7XiBNmdRrJvxUjo",
"1NvaXXeEwzyU7pKk8bTAF2kx68hDxiy3Fs",
"1GSrxjmufiTKyfdX7PrbFsYYA3MnQMqHYC",
"15tCrsgH2fqmr48ubmFKDJ3GTxzzaFQATt",
"128BSUWx9i2thodFHTph7MRs4eB3rWZ9Y1",
"1FRgCNbHgSmQ5aGgjWDZMeZURNi1TD661x",
"1Nppew1JP2VV8xy5cwJzEiX7bhyc9aTVJe",
"1EoUmJFHP54c48W22JXUDhHLTY7h8tqHK9",
"1NdE5mhnPYyMayefAdXDHfD8MMw7yX1ewb",
"19w6B68rUpLj5hhfkG3ysNBgtDp3MrVX9v",
"1BNiJWfUYvccgswqR4oKhMxyHxR4Uczt77",
"1oMhpqprLDN1LwSyPCdZu96kncddphoan",
"1CDQSJwszkJu53T8UyiWDUDcKSrRdQGXEQ",
"17mrGWdDwrUgQKji8ZQDbwf6MkrgwzdqUE",
"1MNrsGWd9C4r5SWwgUBxwjMtwuPrka7kTV",
"1Cp7UzbPo1KK5neNQZAs1QJkZZV4cGpmCT",
"1854MgsGrYjACPfJemwCdbLgMXMVnGhGH6",
"13wqT1g3wCARfjtExj54ghxPRbsqp9MwNX",
"1CEUxQu8HKrbfpB2ZbXUXP4tYM8m43UXcA",
"17gE8f99Nq76MnYnq8DaswKHaDVYsGvDrj",
"1QHDHrLxpRwF2FC6LTRBw6TvAmjL4nevpt",
"19yyL6F2yQEZCnvvH17ZWCxHnTmvpf8qa8",
"1KWjqTw9KFGyTHdztKJPBpXVm3EZwAcBY7",
"16XT9p5nyuopRGmKWuSc69bnRL11AN1ZSp",
"1BnKAGpxgf9qsj2TbYnjfo2YswMmSnnpnE",
"1FitFnghr57jvAZtXvAYXuh6zNMQSpDMHj",
"1LWyp9LFqfr2VEqWhx1skhuqjq4Js6ctdB",
"1NqFLF3JZR4vxjtxt9kn8WP3VreSSoL6ym",
"1PbBRKjoJAXjfgimN1rwsvc7Wk3zoM7etb",
"1HuM9KAoKWHHaDaVoKnoEyfvH9e2cj7MWP",
"16xjZ2SEPXwekTu4b1foo9c5eiqvnU347K",
"1DvpHC8Yt2rpYR9Z7dPYzjW57pgkgN4noL",
"19gqSQEbSrbffqYTKDKo3pdyz6RN3nzM1K",
"194Wz7xsMLwFWwGduwa3oBT4s5c4YRStYZ",
"1KAwjThJVEpqXZXzvfnUJ8vNvMuevqPXX4",
"1DGSDckFX7ytQkMQC4hEVBnDcrjEwYLKXU",
"12KzbN8fBWDcdaJZ7ETJqZtYhnnLrNfXxh",
"14Mdx2mfrEEM3JwsXxVV6qvpB21sopAzhT",
"13rJPZRVZtvQPmSmXPL6nVLwYt3Fyss7Xz",
"16xdVtbj34xyQKWZfdQGJCLD8Ke3AGpy9W",
"14YnbeT2AQse1hpsVmQm5f8a3cT2PM1LZV",
"1FUmunJCPJBteC6Ze51qGDKrErdWrmHuxZ",
"14jg1ijmbGCgtF9yKUF7KAUrywbvBXi2Ld",
"1PBc2C88smU7fpdMbrEJkLy3SBEYxDNwUk",
"1M5rWTb5YCoz1JHDyKqtrVdzXHmJrs4T6C",
"1HmF2u4kcFLfxgtRxc1Y4qD7NBn2ytVERW",
"1QLHPeePZKnHfeCyUGogZSFq72ippRwQh3",
"15sh8es1C5qCbcDjii7CGj6XxUoC1HeK7h",
"19ztyW8U6SwfuabF9m2HEg6pkDuSjsn4Kq",
"1GjGJpyXeEndB9xeH2pVyats6F6xRAdTc",
"1EDm644jscmtX3KTsC547TztYb9FELm55q",
"1Cv46fxodUdUCrLn7QkL9w7LhZpoaSbwfd",
"1EWm5iXvC2dXgvSwnYy2KHup9LVJg3GrQv",
"1CLmnwftXXq4H243urBa8gcZUFVfk8EXf7",
"14KnCvhrhEp6dNdCk4XKF865h2HYNwchhN",
"1MdwKjywdPiZaRpSWs62f7LtBbtsrwuaG",
"18rTGR3oXzPa5Nw66SXnjHu3j2DmQJ5zfU",
"1FJE86dL441p64i8Cp1HTjzJfL2N3R5rTW",
"1qMN9zzAAAWuN7f7uB5x5CrgKSG3AHq7a",
"1FwjSgmC3vWnNS12thw7SP5XRD7CjzUtgJ",
"12EwoTHGTU8LCR3dAanVR1kniQRM12tAXD",
"1QFaxTCN2TjAJDKTdZXK9vqcbYRDgvpowV",
"19KTb2rADpButxkxpv3iLxwhvfPHuBF73B",
"16E4NVNHkSz7AgJcPbNUWiJ3VBNrf2NLEB",
"1FyYk6ZXuBku2YP7RNwMUUjLaK9NyFbvJo",
"1CKSK5PxuexAZR7S89DmNpBxqjGg9WkvQC",
"1pWv24R9ALw8FuV9GSb7giNzGr5Rh19yR",
"1Ex5a6oDu1rRR8jQ2LwSLV7qsfXxrdLuGZ",
"17rp1zwcVawWGCfYbcvZxn9g6VFKUPLdvy",
"19h91zbjJ2veurkJiLh7UkRANg1EEcGjjQ",
"1JcriBSEkiB44Fdw53FXG8UNQaUP76uHbu",
"1HXASBn5Svok48Y9NHdDkncNzP2o1ue6h3",
"1GcibupSLWc71NJ9Q4C7HJiMhytnRqPjXr",
"1NkaqZeEeYGtg7hFtFHaeTx4ukU976vgjE",
"1NjQPHoNiMLXDvgsuyaYGEfGLvP2YNpoRL",
"19it5oMymgi7u1p9TNBXTfChEMoivZRMS",
"1A2jmh8abztcjFVSUcoXfHqvtkxH7bozV3",
"1JH7rUXXGQ1QyxmZt5sh5EV47k4vKxjpDm",
"1FXXJB7Sj9GSW9SAwoNzApFvooiSjggMeP",
"12yaG1S76MEMrS2564gxx29HbV5gJjZ2Zn",
"1Bk5M1ZVKVqLbcrad1Kk7DQV3f7vkvNU7S",
"1CGrqrraTVebJB33oqZi5tbZKxbkLxxoZ1",
"1ChyFVegSUQnev2gJ5MKam27k3wig22dQz",
"1oeDUwMtwH2C24cEV4zokRrojPBQv8cY9",
"1FRiFxzzF7wo7SFC64c5gTugFD5C2DdWxH",
"1944cUM5YHTFGRThXjKJcaaBR8rhEhkK72",
"1AvamteebCoyNrhffLh9hYxNNNp8FJhPjC",
"12vpreS5bVvgLQqTvJ4CVXj79whXwYnypv",
"1LLzM3SXhWkPThpEuLrDtZWdq8p688jkDU",
"1PaZZus5YngBVEdoiQDqAvaTzjayTxfDQZ",
"1awfmS1ru4LTyFFfJ46WSTJ4neJiV7q5S",
"1KxpLv9cQBpSArU5ZBWvpyeYCXfbXW5LDD",
"17ratEBF9Hn6cDcdhhX4kV93TxRdScu2vV",
"12c927y56RKvXMFkzw4kxx9cV4n6vmRmS2",
"1Lx2LHU6KK5eDPtJzRzxWhVM3u3dPTAx2h",
"1W6vV3dd2GHDeuM7FcHTdkAkyZ4czKb6N",
"1JLfW92YVe31huaq2JgyCuU9V4zUddWeT1",
"1AUf9LfqsX1evsEvM4e7Hc5Mg3nBrZ77v6",
"1LpVgLHWRESBnG5uDFHqiFaSfrsuWWNEfS",
"1GQNKnEipB9T7aispHETRPwNcfzLz3qBxs",
"187XXBnXEHKyxKFY5G21iXsN6VtuPz2jGc",
"1E8fnUgi2z628pLmobhNJZbXsVivryVQKJ",
"1AqhsdwkEW72T7oVyoybZfFb7WB2wk3Lew",
"13Hj69wkLAxeXprsHoPSGQBtkb7ti1DyH1",
"19uWbezmLrJqwwFZjsDB7LmRAvhRhSbt5n",
"17sFADbTXH4CJbgNYwhSCQZKa3sNHkd76D",
"1EHrz94MRVtqifgLuNbjNzC92SPeyzG7q",
"1BxnEb6KuM8Ahb2kBm6WUvsd1NpoJgKLN3",
"1L6rb1hVDDJ4F1Z5cm6kyQfmAPTpqJTFig",
"17aP1j47bWsxJcW9izsnLBMkU3EGL6XBkM",
"17ReUK8dexUEuGgz3VMVAWXvAXKL4WXt2a",
"1N7KP8rH6afXkuQpzccCmCaRaA4KctMswg",
"1DQnjBzNynmXxWsEk7cyykhbrFeJenUXBW",
"15eRbVc8xSnu36KqMJcAiU2jGE5w52uHJd",
"1AYrGTjszK8gYmAZ5w7Q5zKFffxpCoXkZZ",
"15ocX79UiXDMit3KL6wFvnNCjYMrqdY95y",
"19quc1BzkTaJDX8nMEXMxUWbCCwm1GLoiH",
"1Kz6NQ8hRPUqouWCb5ubsFPsWgxZ9cK1oE",
"14nPzK638g3BNsad9NQqsVnUeYfR9ruK5Q",
"1GzwasSZp84bLHsWvjTJdTUmEr5HHYvvZH",
"1Gy87rujnmmm8USY692vkXdQUN5a9AEgR7",
"1DtYfT44XmSa7KD5Zf6L5QFC4ZbCmou6gE",
"18pLAPJG7UJZ8AQtYCF13jECdR1H6R7gBn",
"1AH1qi8oQ91SUxLXd33AsGCTwNSJgw8BbH",
"1CTPrdjjWtULDEWonwKsYRWFVeToye3Neo",
"1JkEyDxYHZhzzkwLw3Anb5zyoKJSJoUAD5",
"1F5aJqhRm7uYjrN6XKFciid1CyyfwBkMC8",
"19g8JfMYohyZTqBboxvoEwZLMBSU5GSpn8",
"1LVvvTs5eexAwj1rmow1wY2oRM8J1SuuM4",
"13Z9QCa4PQ77Kf2FTgiujkyRCEMhY1doGV",
"1HkAPX8rtABUqWyHC41yW2MkvQVTTkW823",
"1JE3UzA6k4nzQf6ARKXvbpPLq8pdXzZR9K",
"18Ya2J7ZtaGDHacastWADz5J6eEM39WvM3",
"1FaR2bwoYF6cChen7c68BeZTNrf8cho717",
"1BCbNXLWG7R9BKewhxmoWSspE2Tm1BP1wr",
"1DWcN6YjScFCkgqH356k5hP254dEX6Urrw",
"1J7skzmYVBR5PApZecXt818xfi93H61Mic",
"1AAZZxZgYJsjn67VuUHxpGrFzUzDFiA8Wf",
"176MiLUp8x3i1uMf1TUwAKUYpJBoKYqPQo",
"1HXPMaNQaZWTfEQ8ZAT4dfBB9DTQGg6Hmf",
"1KNk4f3q1oJTmdy3YxT6UeAuaCo8CcrvPK",
"19S6MGQVoVgHdJPRP5EY7xA6U5gqgfQpjt",
"188NoVYsTcfqmcF5Tc5C4rBN6wZTbFEXUY",
"14VtgiBKxDDzC6VmHrWaS9okcowndfx2TG",
"1JDJ2odz6jWxXrNZiPLG7yJVE6ZuLW3icu",
"1NRhifNrvFtagjNme5rXYdvRnaxatAP9ea",
"1PFngR1TQ68BbMW1g9dstZaqaGTmWz41J4",
"1BoABnQ8oAsPzUUEPycob1X9WCAPCEQAvG",
"1PWdRjd7TydDy1sgGDoEpBoeKSp5QvMqH",
"1U32m9uWvt3ueTN9yPtwgLymY13ghyT14",
"18VPmkFWaSgGwCx7ByVCb6tdoFyVA7Qa1d",
"19zLmqoEA9K9oGQbunhGZQMdeNsXnNmtCB",
"1NfDLdjean5onDHwDQso1hQ7ZGTVJmiMCC",
"1HT6zbudzq3oVzUGK5nn3g3J8qkycKY2Jr",
"1vQyE37o7f3pfzLWpSHKL3vK8EcEr3vki",
"16rEUoCWyQoXy3AvPuMRphAokzugZGazM3",
"1XxjG4wXpXUxGZPXYEnvb2P7ACAHFfhWi",
"1QH8UbRXxWEfKiCA8Lzxui9fj9uaGsAUzY",
"17YVQbmyxGURpKMCRd6ctYdunVkANSVdro",
"15fYPrP4YBDaUTj5oK1WbegtDnGmQonXZF",
"1vBH2dGLEPaL4FJ3VR8PMehD5Y7nWB5Fp",
"19Vhdh3yEidGN3k8fjgXuxkt3S6kQ6HnWc",
"1JMf8WxfVDpDWcdpxebgw7MqHH6Ur9QZMb",
"19LRf5JbtSpjSA18uSNA6rSLZ1JypUvAig",
"1Kjihod62nGSsvTcbrqVVoB2QhxwbNAL7B",
"1GtJfYTMvcv6pZfRyVMaUnYre8xmobHQpX",
"15Q52gsayVF9SoXmWa2ZCKB8RoUULNKstH",
"146pHaSJD9KpnpsVE1QBF2c4ZJTz7KcVbN",
"13twigrDanPiNvMDJnUSzv5BsVvMunXvyZ",
"1M1guy34vV21XQkFXzTz9zEQ7j8bwuqdi3",
"1NUjLCTZF1NigdyoqeK44t1jvK6DhivMfy",
"18HcV7XGeTKtPyHjSu67gRx9z7kgC2JGh5",
"18dSUiGnvSoiKqimCGjXUW6FJxegyJmNpc",
"15cNr3VHPkw9mXe5y6xhBkBJVUr5XpjGYh",
"1CgKnvmQpWTir5GwpJTmmDFoRTS56h8MuD",
"17YGGCSurr9qZ2Jftv1ziYHpb5ht57dzx8",
"121BbyY6fXN4mKG4XtS6LPVhfk6b23dtYk",
"19irRyEZbQUsLDCfh31eFywKtabHhc5o5r",
"1A7eBT8uukN4LiKbWwtQjm3qeg8oBsrCFR",
"17gDBaW7vhNJCRBf6JfYrnuGYuwvCtX9F7",
"18TzRfq9YCMZyz1QysbZcMRXmRPsL3z8ZZ",
"14VybQEMqaFuv76MMBi33cF7JjBc2sGcg6",
"16TBvSoqgtsxqADDnHrPsfGrSeSmZ4xBgb",
"1NRKRKg4VVuJvB7GoVN4axBiPYQbBztsJE",
"1CqANL5jJ21H7SGA6f4o7thaFLp7kispao",
"16yKi9hMYmqymq5AdzgocyR6N1r6eRh2NQ",
"1AuRVbCvjtDQraPByQLpmsTVAvhmpgESrG",
"14HAkMZWrh5CfLP26DAMx6PQpz5pMdyEda",
"18Muw7zHXihJWKgiU7bU56yAi6UNVWKR8p",
"1GTtwiBy63r9ywKV89xzZCdWtQBMFjcpKJ",
"1A22X5G6duSJtne5Ba9WgEgG78zAhxUKtD",
"18FnVnuvqEDYDcH56jwrZq2gQUNPwfhA8Z",
"1HdpFT4EYs6YwHfPEhTJCJY9QYcgj9eY8x",
"128x5nSWvvxbKh88kfY9S7c168GeciZYrS",
"12WfQRTpmdvEUoJ5e35uxj9yGWpCyL98xe",
"17zASsL4Zqkt5WRA9nrx31zHQgS19aLP4Z",
"1D7WCSoeLvEEHMVPTu5aTMC58HQtTyWcf8",
"19riFUVenZXLbWceYRKrpWgeGpFFvLywQq",
"1645Qj67y5oqXEyk7sfkTMJeRztrLwnPCg",
"1WgXx5y7mxtPV3bHWpExCFsR7S7FG3o1R",
"12nDnKLvnjPNZ5CXsCu2Am138xXqVvfP4W",
"1GtaQ8Q73wg73fSwfbyB7xDzTLc4uNW9kq",
"18zAXhFfjhyjjY7mmTRVmLw86HLuN6v5Hm",
"1KoRyZ86RqfbmLsHu9ZWzZRQL6Z18wPogB",
"16ReVw2DBjoeRH3wYL8cuQFVBpb5qhzBKd",
"1CV62AtfwmFHnoVkeaWBM82cUhmLUa1Uug",
"14U9L32J9gv8RKXHZhkYveH649fLw2zPih",
"1FbRZm83ToVRJU84SYZKhSymatS4oiFnGD",
"1428YmHcLVSYnWVAU95JS8q1Z8KuzUgDnE",
"17ndLgdE2q6GzaLo9WbNK8v6D1AGzV23rr",
"1KaNspKEDtPyB8L1nMcG5Lpp16gVe5i2Ez",
"1NqiJRoFpsBMfieX7aQgjXxyJ6AmynVFYr",
"1AwxcK3rAPm277pVk3YtRTExTS2CD2EScg",
"17R3c9n9EbLPC8bpznWUTDJJ1uYceysgh8",
"1EDvx63YGbvvo6ykRybUbHRiJmZmpnfmqj",
"1JQz8s8jWVoivAjT4bEq8f2z9GxAuhYVNS",
"1CdjHvcKQHvjzAusnQy7eGmJuA4usUhdHA",
"12rw2yFLwvk4W9hxo4EnA7hs3GYXvGdcSk",
"1HYTk9MqRucWEECz75YT2bTMqocfGDRksc",
"1FBo4PA89jPsZh3xijYr9JtekKneQpw8EA",
"176mXsfiDNhssCHkpVoF9s1zxz8nZBeft2",
"1ADqDB7DVXnrQrp3HNBepeCTRjyLV52UxB",
"1CfabtPUvhAyi8BdELQmrJirPhANzfjqqB",
"1NKrZu5id8fi2ZbZ9dQWDtnyYPTe8D7Txd",
"1ERYidC9HS6tTbbRytvv8jMcBtrtYbiJkJ",
"1CqwtTa3UETJVycbtwoH5JMdUWWUi488Sc",
"1CGSFBRjmQ25oa2oZounnhBGeVMBm5Sm6Y",
"1PjjDbSmyvkeVYPX8qrT8xtbwKx23ZYubH",
"1E31Y4RqGc8B5WPi7ZNRnu3ieaP8zmDL1d",
"1AWpnhg9C8StV14CUrWahfTEBymGhPgb1s",
"1C2RhziV533zgrUtkoTeymGD5g2EJUxCSQ",
"1KHCNMtwd3DdseK54HDZRScCJZJc2hhjof",
"1F2qJYdk4WhMPcYaSxS46gc3kyT8Gy5JyB",
"16iQcDvZ4h7ShXRdgPGzrgoWA9DFX6G6Hg",
"1Q4u9N7ctiFxfcaaozyFQ5ZC5BVe139qvM",
"18JCJSb9uHatSYjyTNC6ayx8zvJsVQ8WGt",
"1N7tjQTu8fgpB9XnMeQmdtfWwqA4bD4pKQ",
"1AY6sSVjEuo4Q48N56Ltt72m6NKhSRYC2f",
"1Kc1fkVkumNcaeWyAfi4gwPJA6mUwte7fK",
"1PTQwoEopguYuboEdFGNcfV9VMop2SZHKK",
"17FhE1CVsSV4g3t9MWZtYNn5syrBJufa7M",
"13dmfRg6ZxxAeYXsLtppzefgehmT9VzPiV",
"16eRbhXAjQ9D7DZZobBnpfTcTNdd5RiWLL",
"1PMTZAfY6P9JsnaTpbff9ST5e682xyTgD9",
"1smrRdQWp15MdxJYtRuHxmuFNM7fhTsMV",
"1APz8qzYryfGr37drbGjb1W7qpxQHDkfUS",
"1rw7trzTf6H3TdAftvZKLwD2avucY9Coq",
"1coo2wzhuoKt7JCQn1o95uy1ZGsd81mqS",
"1NsLN5VvajjHfg4CSZEk4HFKTRqKs6P99x",
"15xMvaTTp4WkBYHrbu5DseZQ45QpRqErgz",
"1GoQVgkN47B3dxAW61qaGvZ1qMzHBReDao",
"1K8emWiqpyXSxVYPyFxKvrN57e8pjfnwd8",
"18M7z7RBMPM2m7qjfs6XjkygjScxebrFb4",
"1EMTQCd3qLGKug1mgM9zrUNKqzBzm3Vbe2",
"15uBaUnTx9LiyHjFcBt8RcXMeNabSVMA3j",
"1Pt4Qq8irga8TFjkDvXmJGWXHUtvcBG8Lb",
"1EMEb4EnXnDCxCLr7TxXkNUxfCj31V2w4n",
"1Kqds33j7uejByERY1rFQgGQtjyeTdmwtz",
"17o1p3MgfYuaNZJ7Jg58zoPAzqinfrtoqx",
"1Jkn2A9QNhiCYTudW8VQ5gDriPKdFk1cyK",
"1245FYtM3Wu8qxBrAwfRWxCMG3om2xoZ17",
"1HPvtgX5x1CctqKVSg2juH6M4PP8JsGyqk",
"19BcQm1ATSFrXZrEzEhGCM8gvikYc2XvP4",
"1drnGtp2DyFbrx19MYjbC9hVwNvQQgLo7",
"18bq27gPCHWNbAXp8sXkhQjoJSpjDs9SxG",
"1J9YyidzY9MABiqLiZXFH2QTxsgcpy9VHj",
"1MV1tWiqmiNX7BDLF57mqLRtoax3CQhWma",
"17Ucy4Bge7Nxfc9XAfnb3ZmWdXUvqopmrz",
"19x9UyfjehVtL5mxm4jrRKNSxRawPjed4f",
"1F5j9CdC1KV2zqBYGXxN6aHzNmL6tupHeP",
"16Z8FHL6Mfajc3DjYasYFKGQtDMg3UKYJD",
"129nPJUMek5HtkQvDychfcv9HWYnVhcwep",
"1Cpas3sfvEHQ1wdGEAufEePSoLBGWU6CnH",
"17hnCxoRXHjazk8AdAZUQWPyDqJ4DquNAg",
"1Etku2cy2SoiN1ajF5Ca7Ax52spJjFVXhk",
"14vRJMukugaUmYSkkH1CsnDq96Npp3jsDd",
"1PWoYTZmFM9uanqhfeY1z6QcnX4rLA5Wp5",
"18wW1nqUJpk7kR9eXpwWWtXqVcx6Hrg7Xw",
"1PM8Z9CYGz1tEDKwW43RrAuffy2FNpvBqb",
"1GCK9ZeboHHFS1y7e4hQ6cqr4itMNqrQdq",
"1EuiF8P2GVqg5gYSFfEtKsd3ob1NXpkrZq",
"1D5x3zDRsdaCM1eoPQXQpoyfykaMcMtmDp",
"1C6iyNts6wVmAFDEL8Zjo8Ss31mERE4kPj",
"1Yf1uYTTekytU7qJWFiWPFDAF6YGoFh5q",
"18qAeCwS8SCu6bG4pz7WomaGmyFYQF3V3d",
"14m4xyLcqbDYopq94p5ersMUXsLp2XASCq",
"12VdRdFtkEH8DcZUpDqTyY8ohgWgkWWNav",
"1iJWvqJ9nVVK4Exm2mVcwqpPLbzNNFMzj",
"163dBXpurdcEtEDFH1csvqb7EA3njvDYQw",
"1CWbFeP4GjAGs8bbJP4pFG4Ga255Gh41d8",
"15ARC2wyrYLMrc1vEPNr3hWya2yBfsxPRS",
"112rK12jXJ64tkD3FuqynZHbcBZmkuiftJ",
"1Me7wd77AG16LfmDGyVvmTMD6wTk48EUHX",
"1JM6hevuJhaf5HVB7472HZd4pU8NNRfMwo",
"16VaoXBu1KGbhuW26F2wMjmaWmEEZcUKPK",
"16Y5ZZErFK77o2hohJydmxzn1G4FEE9PF",
"15v5mcQsShtNpurDeaHHCGaLWX1XkW7Wpq",
"17KErsBhmjm2M5BxKEwqLdWgMMU7jSpLHL",
"1Cu1N3E3FF73qPTYdWtb4CrBJhvvdYtVGH",
"18qGKZQGEznYe1qWPthKPo1LA1e8rQi7Fn",
"1GShfpwRrb4xtF25sf9qdY3TWKfYkypBxz",
"1FKwxTgyQrfJDnBj7kn7z1qyEY3rit7gAm",
"1PeeHY6pJQv2MCSBjTsevJ67YCRPcTBMAP",
"1JF4h7tvEtgoA4fr5kE6pCMuVS1ZUN6Ekr",
"1Lt81HVxqqnhUcphrk4kg8PeZwVA3hABpa",
"16T9WyvEhVYAZ7tUHkPNTC2qNGxLpNpJrX",
"14AQJQ8Ci9xha9jxbvppYU8QF1S6CUPBsP",
"1JWrmTYhye6XRGRiohgeCC2PSe5ZjVLCsi",
"1C8RZsof6vfsHnBPRxtRVhboY8LLbAfhPL",
"1PETEyTt7FKcbeANPiKC8o3T3yT56tJ5Zz",
"1KgsprZe3D1z9WaJsHmsfdakvHhzuLvu8y",
"19Xnb12uM9x2fryXKLAWWHWy9heQ5hPGq8",
"1CnDbLicBaGBX1SkWgB5TigwUcyLEzqufK",
"1PZT22JPvLw2fHMRTL8EnkscGYg8KEwkbw",
"112KpnqEs5FUpSzhe8nixKsSg73jrEtRhW",
"1GuPPbx3rj9ZHcFcAmXJKESKzatdTJUQZN",
"1NHpgjBoi4ZcUi5XQhgj5Nsa6oubn7m96p",
"1EoL67Cbu6CLggCBjiRTD1JKMYuN7uo9eV",
"1NaaKYQ993vWXo6FmmA8bo6VvYraieffgS",
"1HUFoT1PHav6Swyb23JyXUBvsFejrgfxxu",
"1chRacrujAR4RbPnKYvbyqy9vSqZQWQ9c",
"15hkqk5TskZKVqbE6XzkgpFyyv24gGZ4wY",
"1FaJvR9Pj3idugHV2NUNzCjDdnZPhBZHkP",
"1LBCCPYU5V4HgUFkJ6xH6BRzm3r6UMLdYv",
"1MBnWNYWrsVSbDcctDUJaR7CyVeT2XACNn",
"1KjVa7rUV7aib8cNYNNFEzrYE4QGByUTkD",
"1Mdee4AU4Kuq5trygD2u5H1DtDomtvvMi6",
"1AvXYhGT3Co7Z24us96ey464mUWkQyNTwA",
"1EbQFzLMuN118fmoqroJ3btxm4YgYu5iTB",
"1G7XmiP5AA37tvaEkotSyysSqUNTCqNsCy",
"1PAvxQ1schJ5UmK818LnNBqnHE5yu3woBX",
"1yHqqs264drUkxFgR3kJ582xvgRtkNsi1",
"144E7MffCCEWmPnawxYmBAGPaKH3mPmH6S",
"12fy7fyRpsyqpNEo5vHoqYbYJPr4UYi4s6",
"15VUGbwGVjVLZfMYA1wL97p1wJ6Fgmewgz",
"1FFdMXvjwBppnbDxutmLxwoRnXxGyHk2Xb",
"1KCSfz2JD3gkQt9KnMrkDG6o2wP6seMUfX",
"18NxxVTLfg3c4KriEhYQtAaxkCNX31vAHH",
"1EP8HiTKCW5tmpSm1j9uK8rhHaMDjrc9B8",
"1Mfa4zcSnVbKTx4qY7PZqVD2uVbbreGiVN",
"1CL2bK3abxkCTyXSR3iJW9eruhLQL2GBpY",
"1E6UtZjGFhMUgw3Xyp4GfYm9LSxfdiZ7MZ",
"1KZjq8gGwmfZSC9GrxTqZCZJPW8vknm93U",
"1JUvUbyuK7J9aN7woe5U2hm4UV1PXQBVer",
"156L9fF6ddCYn8EZLvjiWBgWX7q9WEKkEe",
"1NVMnbgqMasRA41yAkZBQDSbomsu9CNZ5M",
"1By9CPRW2yaKMyqvYYSrSCGucarmaSmudS",
"1JuXwhBXYn6H5KWH13yjPAisNoVhnvXgxK",
"12Cq4nRMHmotu3VvNMTmDnEE7B8fHiAd5Y",
"1Jeza9sRNeiLFb7smZr2kVLYZJrvTiea82",
"1NCiYEQEsydfy4LEnLgQy5FrrmkQrR834w",
"1a9KUGaMFKwm5cndUoyQ5aHSfPXmvpCge",
"1DyufauwgzHgNorrZQm2h8sXS6CXj8ar13",
"1EqGKCpZyW8ooHPCSwAh5J5v6QCRWAfuhA",
"1DtJdnqCm84QiRtwxhauoJ3xqZF8gYcBST",
"1GEUu49pRyQG4QVL3nwLHSMdbBpLz5SUzC",
"19NWHDKQv2BYnkzEd64uS9H8NJNuoqnRK4",
"1h1765vYipJFVUjzC6Aa9eY6nRW8gq7Np",
"1LnZV6XPHzMCepCze5AExzBuaj2x7LTMD2",
"1HYtKEfGeGXPZ6WjdhxwrQCrdXsCQ9VXKA",
"1Ds5rRA9MwixdE68PbHSxt1Vaz5QUKLNrs",
"1CtDp9kKcnFBBfYAGooWo54S6AwVkWR1m7",
"16NDyNZXU4Y6rbFH71P5m6GBgUfwfoMD6m",
"14HadY1es1CnDy6ukfByE9qAQ8M9bGTNcA",
"1AbFmRoCxtEY1tcfSQNfYXtMv7hMamn1wV",
"1JthzfqLVekDBPAFqAWqJ4cN1kFo15dFrS",
"13s914P8sx1JBiJbddU6XDT3ye6GwNbZ7Z",
"1NjBW21Ei62du4zY3bpDmqvjyxxoGCBCmk",
"1Eu8gAgUuitkdLN5Dn6ES3obKT66PmdAdy",
"1MxHST7fWNqGzuug6HnVLaucfj4cZgYCRQ",
"18LtuHgUgBpTkEkyYgsSpJc2WNT573TSdu",
"18mYKf5EU719p4q9RKmniZHHFhx5mSwiC6",
"1EVhNm7KZDA9ajdeAWsiaRy83YKpvbJFPi",
"18tn1TY4TGnG61BAee2jfMjaSmMB69zs6o",
"1G7kESGNaj1xYF3RhCKBNwCjzvMFUsrTK4",
"1DBDLvTjm5qVyHxuwCm4RGZUmgV6442uVR",
"1F2boUtZyotJbXSx3p7wjRCYRXyTvdUQ4E",
"1QFZ2fFXC2ocL5DxKKN67VYQiEHzUbwbBf",
"1EVcHhyWeZRmk3nTSWRzbj6ZX2CYCyyUAD",
"187g8iNaFdrykryawkRfHd3mZgpwfTrdm4",
"1MBYc2G7QYjjkrWZ8UpqNuYzC6jBW5399p",
"1JgF26oyxPjEwv2kWE8x7tAp4eEDbyf2uG",
"17BhoHBS6fTVQZaquAoCiWFsG4c6TaRKNL",
"1gnADRPcExbX7wZLEMphfxXBeX83KCjBT",
"182abNpi34uxhKJQLjtYgSADHEpASokhAj",
"1ADV4J1KnXQE1KxEvqif5x93djzDb2uqJE",
"1DfvFSQePtStUkR3f1vjW4P4S2vNUEvr7S",
"15ASaStDPAeBMVVMu3RNQbue24vzq5pKzq",
"1KTM3KpZgGSTRfUNYdbx7NPckAAwXnAEK7",
"135uX91P2vziog7CFyyfuPSy6EWosWbjsG",
"1MJrrkVMZkcp7XGByFxQC6FatnfddX3Kmv",
"1BU1sS1ZB52cq9QNSCYCkCpo2eRkxGv3VA",
"1Ky6cmZuZF7bZzhRZFUNBjTYyNpAxzqqVy",
"17B98TpJoWwbWBdqSrmVo1YiAzhKgLaDCs",
"1HUGGY35m7AuwaBFV5HqK1isGqo8LW5SMw",
"113dmGmScNK4tqoHPnkRgR4X9ioip9znJQ",
"1AM66982d9yTnsr8m35j6RskKTuFCuR4tQ",
"19KePWdkBKzSsZv9ypg3NSxGd5LCksVVd2",
"18qQ12JZYuhhAq7eDK7JXQNimBnKJw48Uc",
"14cnhepHaMND7ujTKHxTW1nCrF9JsS6PYP",
"1JeYb1bcmFkz2UcUfTH1tsy3nXxd97Av6t",
"1DUrx8ibvnobDyHpgz1pPHQ5q9Lucvnqbw",
"1BCa6oU24qgwCCEmnf7nwDRuG5Mz5jFSrn",
"1ED3RiSAbdMwkHEiENxzWBoQJNA6AMUYFm",
"1LDPkvcPpgQ5bP17eAqpdqEbTW8qiEeDj4",
"1G3uM3tHjPJ9GNgvGf6KMoy4jsU6VGWjY4",
"1MgBau4KFSci3SxZdeH7htAAVy9LVwJ5cK",
"1Djcp6ZqWNQCkK4ssnsZrvQBuRKLFqQmvU",
"1AHZuBziHhxPq4xRjyqHS8gJi2kbH2W3sz",
"1ZSWcxpH25X8mEcp7quHJyRjmU86wws7b",
"1KBK7sh1SL1n95haqec3LPsoJnKsDxt6bp",
"12G5o3TNnPA47Hu1VGbtHuFgEP91RYi3mK",
"1ANeiYYdzEjpv1dMwr64uvwv3FD8peHjBT",
"13euHwcP6QocrqfCbcGysrgL7uRbVjUcxQ",
"1LoCnibzf1erLoCPxmjSzZPGznh2hU7Agn",
"1716iJKWqWC6CCrjceANwwPbVQUMxeEHh8",
"1673WUeuxeUpv9FCRePaTTcERUtVRhv1nz",
"1LEKzUKwMF4fdNkpibBkjHrcYfFjQAxK5u",
"1BtwsKYeuNTBNXmHB64dQ4KG9m4Z8ezYK2",
"1CjrLFgYuSKqDx6DySXQ7innvkedKVtwjP",
"19QJxsdE1xBwcPK9QsZFZRACkUtJt9c9xs",
"114ZeNcGn21EUKMSN2vGryAG5GNwYiiLAU",
"154epgVCYtVLbnFViJS6KJwrbiWSX3rFi6",
"1A5Mdre2uAdk51XNMDa2GckaVyZcJFMU8V",
"174T63zkNxUYNHBRuhY2rmba3WokGYZHZP",
"1NzS2NuVqUsik3bxXEs4kaN6g61zhfqapr",
"1JrkZsdogK7dC78fgEkRKmGYrjMe5XCm2r",
"17jGZ3N5km5coqpbcoaUXPd2FWzG85N8PG",
"1MYYaz2co3xcsye7znf4SFnrgR1GvH2qss",
"13zsfEALrPffWb1bmya4GGemgnR7YHobF5",
"1CW2vwBcF9GpkxjTNT64imYeuWgwvNQvbt",
"1FESTqAZtUFxM3XFYNPHNZXcebHmCGhjiS",
"16xZcebZRaDkWYH7W7SqD43Z5k68VBnX9J",
"1Lw4NJQRaei3Zsr49ykpaa5LaELv1c3YPY",
"1HxztFVyr4obtC1CfvbyNQaFAJtR6qjDZ4",
"1MLMvKvDdLoNNirtv1RdXjmm1QVchhPbsh",
"1HoFGUmNaUgZApL93CMMH9XdPdcr5DyJWD",
"19DjAV9KtxEubu6iqbB9wih4QG6wxEE724",
"15dqTLEJi19NMP2WdzoBpUcZZ5SPoAdJ2Z",
"1B45P9wzwJndWiQ7ExFumTtxAg4hHfSkZV",
"12MaYxu4WZsXy7E5VwdeBSAnMswPJ63BK8",
"1E7assZpTF9ZFpZ283WwqLA3czgFpqt8u1",
"112xGLLb3jKJdq1C9e1CEDoMbDt5aD15pt",
"1RdHpe9pFmqsBB4GB5QGqbFHcypwrKYp7",
"1BZZ7k2CfBHkRktCBxpU9UESTAfyXy5G1M",
"1BoR8ZXoPi2Yh2Z6491J7QotT7LVXjB3BU",
"177fnQtJ3naEC7ySjHmextG46RFRGMHNcg",
"1AP61fRVr7Ba6oLVjrKwgpSQksASbTfC62",
"1B7oYnjBnQjkC7DuamWxpWz15WTtHjnziq",
"1HmaTyLQb2uRyYLnDFYD7oLWwPcM8McPuY",
"1N2UZ1aATJKrP1gkhjVP5J7tj6CyPRjAr8",
"13WmDBhy3nxMQ621HA3wevZm8y48GtmyDd",
"1Hf1gKpZKJe2fMQ4KbFvcgwfFvj5DMLeC3",
"161UGNALAzCu1Khk6B1npLRhoAffqgYaDN",
"1L3YSaniLsGudZGbqu2wkXZp1WQffEpjuC",
"1MzxQMVQ3jSJjyJ4gFYJQjFKCZUM1ti9Wn",
"1MGKY1StCd2emiLfXhRahRLzwAYHfGmeJo",
"19vSWewU3J8aBVtMmMrvsaDJovhFhJqfYA",
"15Z7f9prkUq3A9dq5wxE1pkCkTtEbU9eT6",
"1JjxC54SNQv8o1AoKokazubSJQfQzXYH4a",
"1MDHvhveFUY2FA7Q4gJ1owAqb8zg7tZChu",
"1aVh6B6sqJqt8wbxU2WJMjhXcRVswJjDi",
"1NRK74YakDKr5GyM6yijqh6gD64iijyjj3",
"12vBZq6dFXRn2cBi2V9Yma6W97ZXKM9nZz",
"1AmbhxnWykUiiLmio8XrKCu85C487qvRAc",
"1LGHxZunsKPYtLzH7WMEb68o2bJ1nkZEnU",
"1F7TosDCHJ7j6Sw62sJ6tm766K6rAEFrnr",
"1Q5XuUJ3SPD7NCGbT1tK4ZhqqovaGEw4Ax",
"1Cumx22H22Ly36d8nNjRFEniP27yvvzrT5",
"12jUf9vumnH53n4HUmZf5h5VdZQZEQXouB",
"1DWJrMQzmLMhnN2EfTwuvuHhxp2AAyxiz7",
"1Q6gfgGt4T69utXyVnxE6BrcYM2BPiYRNq",
"1NdRmHGyfcfVSKpGS7tVXJBHpmQi31mL8P",
"1LvGswJAkB7NNH552n6ouJaakVnBbSCv1L",
"13YGsBXKsoKZ4jNQZDq41dU243npkHgabh",
"1FcuDpEkZ6BjDVUyW54Lo9Y347boLkTkdW",
"1Dupq8ctdZeiuNuhQYk2gyNSRR4rqKc8Kk",
"1FXvJgV4o6paNWvKS5Q3Zh2LtvLLMVvfD4",
"139Vt4GVd1Qg7u6h9NvTKmDgHDNHgFrzqW",
"1BeSmKTNWzoRNS1mmoMnbkKQY9wcPhZm5p",
"1PZbQFLBLskKgEXchKT7jqHKNnNvtXEVqp",
"195HWsnrzny8U7dVktak2DPPjX4ZRQaMfq",
"1ELzZbV45hSh8GrXfQj5cfWsFAi63zbUdR",
"13kzU9xHRWxwjUBhBAwnJwQyrijAumzUtq",
"14TLjVZhstHN2sSKLViEtFeGiy5H4oTBuX",
"1PTDMz4TEJFqVvQThkjvghDyN41TYYW5DU",
"1DLBFGPFA6gw23ZBevygWkFf38dwjueGcd",
"1BNH65hsXMYtDRTuEWQptP951nbMsGA2P8",
"1AudhArfsYkJ6Vc5B5F8AMtNWLBjTHUbNS",
"1MVUF589eMcRUcTZYxj7bFsSFptPLkKo4j",
"19oaHmkMDMo41JUoJaaYowi3Dup9iKfjHn",
"1EN286zmDUN9cVuM9JqESjiS4xdAtHVihE",
"17nRp3UqLtjcWPxKv3vykRWu17mYXy1y2X",
"1GJojUwu6KXQEhCSgw2j3ZnNKKiv9Cp87g",
"13kHW2RDC7ub4kN6qxFXEeAsKw9C9rx9Zv",
"118ADyF55v4RwH3gN4DWoLrHtfYnsDDee",
"19wmpNuwxLNtK4QgMaMjzB8R9Xbnh4WXQg",
"16TPKCNfLNPZFLgkXQzikdjSRpixUZG4Gy",
"1KFTsHmXbLZ27v355QJeZaMT2uZMsMUFZH",
"1LUbpPZvn7fsoW22ae9vyoQvyQJd9fZqj4",
"1Lfz7X4bG4P5acW7ejCTJwBeGiNtgsWAau",
"1PJHhfLZwBfxHnQYuS9WsB2jLnrh6XV4rr",
"1EoUCjmiyjDQfDHGU1rvKvUy5VK8oYW8iH",
"1GFAX84nDtui5F6KbZpu7QtPb8myKhvZJ",
"1E6661bLiEu1TQbXEvUKurjbiguBf8G9om",
"15adfHNTyA3bhqQWJqJ83TGo6NzfspNJ25",
"1FUEhZsho2boMh2XLohQqmyZsazyAMsY6f",
"1LPACF41vYNJc48pJKUgQ4sRjthc5hS5Rx",
"1DVbWFgx2DbeeL66WtPkJhdkjzMC9UarLE",
"17aHFhsTXW97PZvUh1nE4fqpfHyaH6ZF42",
"1HEaES4aHN2ZUGe3odGDMB77hkCEDGUGHE",
"19eNx5VUJBfftDe2U61WBEnjtbqkZK1dLo",
"1CWxdmp3BK9ZLQn7C3BSzaCGbh5ep8rsZS",
"1Hf6oCzKGddesCawf1HiV21tiK5nh1NKLi",
"142trMweuodeutJb5Mx27s5a3ZrMsyESaw",
"13VjFG2z7vWo4PJzjprgF96rbvpFXhncsz",
"19pWEErvDAw9ZwsXY1Jfhz4WK7hSyg1UHH",
"1Cbu8pJSLU5mPNGiBynsmPyDg65ezJ52oZ",
"1AtNspXTYRB2T1t2rxhXYrRXLBJRTkNykp",
"13KRLpZDuJC3SotQxb4a5s6THfLJVvZP6N",
"1JoLRaPZQfSVJKRzLYUP9e2hp2ukcQuGa",
"1Dfs9Phxq2KFH2CVfTjm27GGeUY3mTbCyh",
"1CV11hauXW3nreKYkVoxMrXmVY5sY6S7Qa",
"1G5yVQ4b8FSgwgQDAtJ3pWAnXHMppdbaFN",
"194mL3rkywhxorCL16ZBoTRQ2wQoVNa2pa",
"1PF3MthnUonv6XUeYvvTpnz5n5XLhfSqpb",
"12bU7G7VMKcmnwnJph7ZCCV41yNiQqXLRf",
"12Wr7Lusw9FeHAaUtdvuzLFXhBa3NywGAg",
"1HYfF2ZFU1K6PK2awRm5iZ8d4vAvKQrY4y",
"1DQSMGxgvvxrykdAfBHeiZot89oJ44TzCd",
"15yUeW6PucKLYjAaxu3Cfqm7Mi5m2xgVcQ",
"1Ca2PcXQv2MurkW9ZKffyh9NgmVhJQJeQt",
"1Bssa9yEhimXiCUpUmeH7HatbAFovz4baZ",
"19FsTpsufZqmYrWC7bWtXeo8AX3aors4re",
"1EcHQxpwcMwrpPVkV9pgX1pjtRAEBD9x6m",
"1GSJ9smeLfn9fW4wGxvZKuiFwBD46w6sti",
"12yao238oqvnM4smnccoZ9sMGyhb8wehY9",
"1NYDtGknpytBy2mq9nkrAK1BYX3RCmm83M",
"1HRJ5G3Jpc5UJKsyMZeHUnX9mAo98Qvg5a",
"12UiydhPuFgMQ1VqzdLJe6Kz2L7JeiygwN",
"15wio5HoWcQnydBnmxK317DWccVdwc3Lra",
"1JEsryMURFXTeVztDP6BCnqVwan3vKECs3",
"1L8ddfx1bbd5iWPXnEfQVKszyssnA172Yd",
"1MdPyUaqdx3qGZVrsLZrKutELmG7Zon4tr",
"1niut7NaVSaRWiun55NW3kdXsRJSo587w",
"1H13J6qHRAPooqKeHYH7mPsJsrcWV4vX4k",
"1LwG4SqwdNMAnJ1eorDSmfHv9UpWccmxjJ",
"1C6xkPzpoB3ZuKt1khHpNmenL3zh7JGSWv",
"13hrhPEfsgW2Jsc4JXjoUijhpVKZEuBGcn",
"1LrxhTzWbLsLU4uMQkigfXxSNfEisM6z5d",
"1QCisyVmGUfKNYKadqDGL768gRvpNmudL1",
"1HjEK9LABs2VPZqpC4H5owDjnSsj3fo7PA",
"16JneL5fRmJGXxJafRz2DzRWEeagH2bjvP",
"1GXqjSUpUNTYBAu1kzrbWDhAgdPtqbXyuD",
"1JhE5NgkHt2Ro7HxTq8xfRMNcTtMs4Si5A",
"17gNB38Ku4zTbeMBA2cHX7egGannaEGBuc",
"1K8vCtCyQvWcWdGxWqfi2S2Nc8cYZ9Kur6",
"12q7gXLnWwAWXvds9T7iQdkDDAH8tFHRHb",
"17Gyh3E7azjBNTqcZaPZMeemV973Sn97DH",
"1JYiDiXTCeSuMzknEZNipfUxgF7U4GWHGq",
"13AY76JtuafVFFqTUWoMYMVsRDXQLchvvt",
"126VYYWhGoKwNv9zohBcuQfGjaxVVrzrse",
"14mB1qjiVy6boErtZwxk2LmEUL7xBRcMV4",
"1J6WmhUiWx1tGamtB39f3xsQeR7wcgQUJt",
"17k9VUGhYBxzi9h4BjeQJXLHBnbrkQ4MW7",
"13ENk7PiPx4YZBywQaRKVJETFg6uHdd6GR",
"1TVBuXM6gbUSYo3ZUqY2k7SXY8kW8LAaM",
"1HBKMN9ZDXQzpH5PXAnGnu5AuLjxNQoAVM",
"1NTcjFS7vkpwDsieLCkqWVAC1xeQnn4Eoo",
"1LCygeEbPvNsenjW3FmwxftAoc8xdoPW8R",
"1QD7vC2g4LPZEknSfnYFyfPJp4FxJFJryA",
"13mQCkBsx634tJRvePGpPkhABkq7uKRZnm",
"1JHCHECjRYotjfLG8GxPoe7RvUDBDaCAFB",
"1FYENy2QUSYK923u54YMXERZg7FKXLRYmX",
"1Epujv94usADt6Knqu1ZuQYSRMAv6qo6MP",
"19F874ijQFghqmfjYSrnBSF8NKXp7jHZ74",
"1Et9Qdt8CrfasRpw3oQ1WN3rUGvSYDa6og",
"12gQc9VLXBwQAqwVD1bF8zzXvcu8wxpv4s",
"1CWH49MhxbcxZm6yAzkSEH2i3cJdxnsddT",
"19YBF1EWLN1BLRxU7XzhcCBEqfBHajYgqW",
"15SB9tvrZcFn9CAFu7fBUwhawPxMxHknoR",
"17nnpxMaUSKz3BMGBzEjBjVJFqtAZWwhn1",
"1JK7w7r3q5eRVGcQ1mBzyKaxD4Gy4XKPv2",
"1GQVHUZReVUR7eJasWVqdZ43JoS1HdDNf5",
"1D8JT6ArU9yHhbdh81WM6ZSsh4PZ3kmN7W",
"19YM9frEmn1ZFTD7DUCq8qUFGaa2UQRkpc",
"16VizvqgzdSGwS6oG7i2Gqr7Fu85epxdLQ",
"1CWLWytmPsGkWqdU5PSozzG8bsF4PQaomL",
"1R3KpUKKL5838BnnPV1PgFdkW6LuQFMyC",
"1ExsstH7ZhvgqnRiDEyhcb18z2ShtY2yTY",
"14zWdU8eb4qmf85GJm8etLSfd7Sgyqay5W",
"1B8NHbg9uKq5hoYY3yfkWjspqMc6DRnY2P",
"1E4sfcDwqKiJMDSWZyW28NQw4RkKuaiCd5",
"1EY56w53HfZ3rNu499QQHirxZwicYTnp5N",
"1DNTAkgZHp7k1pP2QdPp6LL1G2RfmEFPWG",
"1KdQrMY3trYEnuCw2hB2e5495t6ZWLUbvz",
"1Gy5WHg5uYFnXDuWXFMLwWRSoaVhcmdAXJ",
"1AA9ERpLaJYunNvK7TX4LYnVDjrEJAJZpu",
"16vEUaN7MLn6JV3tR3tKP93RYZShKmgPep",
"13YPH3CWMewxbntTdXfN7C53XEFBfGErUR",
"17wsxPybDHbHDrNnqwKkoJC1FJQuPV7E7x",
"1FYjaKN4HgiJNTFvMoz39srvrVckiQUCan",
"1Kz9qXvmcj9bcwRPDYgESDHE8k77PZBP5J",
"1MVpsBi1bymsesgL4iEzCaTQtFEGrt7nbM",
"1FWvydJrbZeWG9YCPncGtVF6PkjJL4uJKW",
"19VBqHdCS7dXY5fmNXjmeqErbuTdcfDEQc",
"14GHsLixi9GZcoTVTShExY52QmCPUeDHMJ",
"1FwyumwhwMA2YKRkCFK9r8osJvPJtETzyh",
"17px5Jpipkf8kMUSuEqudZ9RtLdwLpjQYZ",
"1696gDkh9TpAha4Ys52V4P4iyqEEszjDpb",
"1HBGjMQMrZ1Efr449zTKs8xANAUGLHkfdp",
"19sRNDDAKJCgxspNfdpv4nmtHk5pkLVQfg",
"1H4n9NinLhsfEwad3zVP9af1WuXxf7K6GX",
"1Lm6nwfyezDokJ8E2yocgDNsmLHTTyqC3a",
"18TbBin9oFFFZhhdEoUEFCJnnTV6r5JNpR",
"1DsaHamSsyhGNFCyDLNqNW6WWicRigausk",
"1xU8oZ1s8WnvXYGrJRNbpo657NAYAoqwD",
"1D88rNRNffDw4fr54bNfAHorKizPW6tt5n",
"12nTyRqV6Prw3oQhPN5s8wmLNcDHQqGDTZ",
"16Xf8XUDFe2YTUiunwHr3sz3omw7Zy8QqB",
"17o2VdL888JwvMNR8rssuUD4CJZpLfV2B5",
"12it5jXGAkN8Sfg8XbBJpxgrudNYP5FV4Q",
"1LTM1kbZyXxftxJCwoYcrH3UA6QcKyFmwJ",
"1CTAzvB1yzKhFqh78BtDXpz2VHZje43DwV",
"13mmdkQqAxYmuecdz6NzXyGvYhGbpjnYy7",
"1HdP9G9BtekXLcSCdV3mMJ88qdtkV8QF1R",
"1AN6V6CJzivrpAuCchCFFj6rB83w9fU6x5",
"1L4G4burpx8ouPvhnpxaY4iVV7YBnKmyUD",
"16mZVjKhpAcUMgudpuEZzyq4KWBkJmqhgk",
"18HetqGUQNhMmhMgWtqD4hs5ZhK3Kaw7rS",
"14hWuV3Mifw4x1SwfDEjQaGmg1hwnm2iHv",
"1Bw9xyogVHNWWXbH7PSpstvy9tHt1o5oW1",
"1Kb1dC3xM5o2u413Tf87VL3qFoVPFQZVN7",
"1JqUESGsW1LET3rrV3YtYzthG9QDnPm4TR",
"1Et76KNvcYSXC9g4BJFDVP51CZVsgAADcg",
"1Pf9zmkwwz9oAzeyDKim2maEcUFoG5jP5b",
"1DkCLRwTm7AxeBzrieocr85YYjZb65xrF9",
"1DCSajvMf56CBD56Gn4qkp3xa9fud3TDtm",
"15RdzA1MN6dmJFMwbrhcZDYnTWYgk2xXn9",
"1KxgpP9AvxxcTUwSY77a21hPUEgfeqc9hD",
"13HRA1Af2iS1i5cen2SqzXnVbFVA6SFb5j",
"1P1jCEsm13Mm1q8iJ7MNbB9NtK91QsXcHp",
"1EwhL8NGTcgRyVe6PXL9uECQDsM9VtmNVh",
"1P8ijBgVPNWBMD2uQYiL1Qb7pcBPD3TQzf",
"17yJrVvsHmszDPPV7wEzhZubfjQ4TGs4Hu",
"13Xr8j8TqHu7XzVNB16hW8s9PnzSLCvk2m",
"1BkDs8A8CPwoMwNgQmhn4eV29uVFiibPUe",
"14NyiU4BnxJN2qE9MpePy49USTMCrqVr6J",
"126xdaahcaxVZKbNWXuprqceRNFNfQhTxX",
"14fJgoQ9bVW9ofYM5NGYfmpHj2ZQZAHTQn",
"1GMQQJpPMahKnC1LN1hYsKiCpnfbGg9zaC",
"176ykA86WgdRpVdXPiyUYkfZ4gNdeVvqGC",
"12CxmVj9UDQuZVzPv62iFz7pmuAfSUsjir",
"12a1ipnoo4KZLpeYUwTGGdGxoNTrdbVXoo",
"19UQ6FD7jx4gCddXv1TGX1HvdEEBP7vveR",
"14At6vX43vMSkWiNdyF56jmUqd49s1sx5x",
"1KmpZ3gdwaTrB3jybiR4acT9pgeBPwkeFn",
"1C4fKYZaYHs1tehwpHaxTho3hzJSkC9awW",
"1PWYW2yZxpx7ZGtWYYUeUzgE7DSXbybGPm",
"1Q6Zu1jSLUvDP3Xo4Jdo3mkKNCKs8UBsmM",
"1A6bSLtYbXPvxPkbQej6HbEDZtnoPErxDU",
"1HgZ4CtCQkS8BHHqBSJM8stf89XWyq1sa5",
"1S7q1UtAVSajSJGpUcYDFrg5GjkhTBMtb",
"17yM5xoF6wqcjx5T91CpJJk1Z8A4ZjP5DX",
"1DaMPAzvsibWMUymmYUhggAUAz9JXSgJpB",
"1PwCGkYhdoBGf9WgZmH5tpbS5BWrQYe4B5",
"16HdWGtLucgZPVtqoTL2f3bvo7quBmvVbn",
"18Rxj8mxAGvvQFqYFHFQJ3yK88iXv141ZX",
"1KiPHhXxQhwkqbFRrpbXk3piiq1FgNtPXF",
"1Lv3VhnKoq3ZTUwQUNBhv8EmW7qy4La91Q",
"19TN62Pas9UrkEktdVuDZwVhFEXbE2FP98",
"1Km7knNUD1e9dEXekLZKuNg9skDX2cQQJw",
"1KrvHWu3PQxmzUES17f4LFHwJdcWHW6Rya",
"12ZSLhyZzVAJURcbReFQuDvh9BJLrWTHAb",
"1NkRRRK9ZCbKHzs7MZNtgteXG6c5c5iDDD",
"1Bt68GWT71UPVkeTooRFxr2tpSZhZGbvv4",
"1BNLSxExUc8Edr7RwdowZzZ7uZHAh3a3PG",
"1BcxU1QyRNZTuBi4eCF8KLiwxLjzKBvAV4",
"14iCPsVATMS877frJ7vJz6j4je7g4srUUJ",
"1EQfpgqA6A3DXfbumVAtvX4Co8nBj3Crmy",
"1KThZ4Nfut1dAJ2aUodPFtU5zu8tAw28jw",
"1MNWQ7ave94EXwQ2gj1bbhfwUvATeUHHf2",
"1GgozkZH9T5i6ytmRFihrkcnzeyT6g6gZs",
"183CiqksQgCbJRYyPSsNhVejv1HDmHL6xT",
"18G5w3ZRPBK1fj9AXEiBUFUpptvfSEfQ4T",
"14yJ7ug39NYJ1D26fEMZ58PCaQU3eoGMHw",
"1KAzkTYdFdv6gKQTcT4xNfQTV6y7eFyCrW",
"1CYma682vE83aegeFXjWiYZqMFoPVUgi5A",
"1NG3dbNJ3bDgy2QT3Gjer1r5AzdGxeAupC",
"14NZtxKRdCdoXB1wU2hP6QBMHnKKcEAui1",
"1CmrsCSTByHyWwGtZd1Xp3WvAm2n4VYDnn",
"1LWX3ZVzQJcZxoA5uL3QZ6iJJc3EGvNAaR",
"1DueN8TSL4tpaZScpcUuK9QDHtchNPc9PA",
"1N6CMUNTXzzG9KBZhyXA9YUq5RSBW6CZan",
"1Hh3xdHUBe3nSYvm5AchhBVZBngCAGt1Bq",
"18UnnS2h1k6zkuUywzEXpQsR2KKbvtxCC4",
"15WAbkd9fsJ4SWhR8aFpFtQVbk1rWbdG3o",
"1NeLTYRV6QXE97v1b79jBFgJ7bzS6Jr1Mk",
"1N2gPZjXBTLuCE2jDuwAqL8kyp25tyvomC",
"1GCqJDxzmNJpo17a4xgvUww5kjtsfRgxFE",
"1Hfc9e1r6s4tJL8qpVkXAPpXwtfoUTDtcd",
"12LLbCvnxYLyXzdmy1s5zm7xdg3qrZ5MHK",
"1JXaiyoJbPQgNxqQE524jLU9bhu1AJuVa5",
"1HtVyPKksKu5JiRvEU3QpWS3SwzreyjphR",
"1dbhjxdb6DytF9ZoB9jRQe2QES4cKuzec",
"1DG22TSmfhTp4tWfCpEL4YB4UJY7Zo7oXS",
"12iBngFACTaJqa8TgmiJWaRWrCEy7YHC9j",
"1xnaTFijPPgf2TapjHAGyYDaT32ADu5hN",
"1MamXj4HmMFiPRi9HbCfYPb2ki7AvKXT8h",
"1AGKpZhZPn4d9DHinJVAWgXNqexi4STd6w",
"1BzR7EFz2NavX6TwnFB6oF6Q8zoxTReKs",
"1Bw7N1VQXQ9CsKiX2VvDpkjQpYjzyjkPyk",
"1GzHwGtCAber4aTVuBPmARbesSNhe7Fb3P",
"18ujLroxnnQ8h8ZsW1YXkFWS9N2it1ywV8",
"18QWY3iGw4qm298FE2WJi927AFmeKNaL82",
"1JCg6oUWe1koAGzLWjbwszqiAugk1Z1Ldf",
"1MhvAayRpSohBWaMDRhg8zkvok8cicoEvX",
"18ag7vLeBhi9Pji5aFpxj2eNVF78qw3BQH",
"17PwkgwwQhaDr7RSstsejESKzVhTkFmoAa",
"15ZaZTovpYYG7dSh1ct5q5qeKgzWMeRhPD",
"183BjakpDH6YrAJN1bK9sd3qjegbJHvFFj",
"1xYT3kYEsvMozxT7xfJsK3ccNCKwoCFHC",
"17Sf3dbBGmZGgeENFBN8HqqoQ38cJWV6Po",
"1AVXJeNT6uyrRvKsvCKDMBdAcbZYUkUqMD",
"16wTr5k2Z7yEzjPvvKqwRJNZTWSTMxHowT",
"1QDwt81PaPspZ8PLuV6g6k7YTcHKMsfdWv",
"1DMDx5EqhPmfQ9byMLR29GL2JiBbN18gdh",
"1LKydEafXUf63FAhaq81vp9fZkXNoq5wyp",
"17JcVasYsLSMqoyLwwPpAi6tEFSajtR21h",
"12HCJson76YsdKjYfV9VoQL6kgJSh3VMqX",
"1NGDtGQ7ZykyV5A7Mx28PkCUJ45gwJizsi",
"1Gkbv8EccNxtQ2sZDgzg1Fnp1BwE5awoSc",
"1Ab3CbsvmDoUsx2nhPgQsNQhRFQXe4usng",
"15zq1tmLcyYMrW6JZ3gWsS9qBQJ27DEQdz",
"1B3nxXHz6EQxX3NGfA5BY3HACDiHG8Qymt",
"19XgnQpcjb2tEsSN3RHkLPtEPL2X7htxZX",
"1GAXRxDeyGicdexCJVdux8VJUPXESddyyr",
"1FqRMz6tguw58J5ywqGTYG5udkAVmBVMzq",
"1JFhNVpRbrmjXqFfTEmAejzBg4bWR7HVzc",
"1MJ1Ytv7r1qoN5GKyrj69cTtPZHGiBqBU2",
"1MBryCVyuWCW77bsbKduHLMRiuxymd8BEN",
"15D2CTxv48tPFVLjpruD8md1oRTzuvqx62",
"19UWjzZNnDsJNjUztd5Awa9EhZZd9JMhCJ",
"14b9Z9q8EwxQgthKLxNftLGSAqwz354NHY",
"152Uczw7uYwzHK2BuggSPrpETTSn32yeNZ",
"1J3baeEfKbBb86vajAaJJTCLrhGQxE6bF6",
"151RKmetPZXeFQmJhGMbVPkbkVWhYW4oMo",
"1Fzry963C9TcyDjQBpDKsZ7PFfyjJJ5WVo",
"19SjJwM7dHwNQhiW9BtfdxXbyqxEdVCUaE",
"18spWnqRUQHv6AriojgyVCszSGLRDGECo8",
"1A1zjRZJpHeciias6jrehfQByBpP5eMr86",
"1Lp6gTXk3RDgfKVnefJEEaDbjWjKoZtqr3",
"1HggTdPTygLvJmcDf9VRmJZxyQgvTPQiMh",
"18NkR6QC1Rr4LcDVQSdSMzbUSvTVxdkHNi",
"134o5MEekV98eSg9H8o69giovUhbv8GLE7",
"1JRThLvamDr2foZAJoxbwpeChoFmdqVp9H",
"1HNr4uRsn7KkCTa48dvoAG6zda3Fk6wqbs",
"1NGue7AC9GexykCjkeNiEK8YD5Kknin5r3",
"18T8yyGZ2MQDqBSrQWuGdLPJzx7EfqRA5A",
"1852cKk75YQXcoW23ekphfFr4sBuQ8Hizs",
"19TpsvFqTSerF8qJWJe9rxExHQh6BQyT3b",
"1MbqkvWxKSAersj6yB1z2jFQZL1Weyhihp",
"13KxoocpybiMeFCQKXvaKHtKaC4rVLWBRY",
"1P46Bh8MbmikkxFtJ77fHKVrGq5ALxXs5W",
"1H73qnY68vkjraTGZjqUg95zrDMeUD3wA",
"1QCVSj4ZFbgo3GECCStdvC43PnV4VcpKUQ",
"19qtvuQffYvB4byc67Uf3wYEM9C2fuCDow",
"1MXL23GFLWcMjAzm12Bvoz4JyumB6KqMYu",
"1FqKLhvgFhiKLChWiAphKq1uy8akAz8c8p",
"1FJ8rtdvGe7v5AMKwWgjikU3FSpT52pmQH",
"152MyhDjiiBoGkoo8YsNarCb5YHPcyHWrc",
"1FGXii5KeG7HKVzqhzfCMDy5fge7NxnzjH",
"1HSYh1S3cDVuhYacJ3RxpXS1aiS6xXumUy",
"12emtbMqCYajFmqJ4KMEC5whk8evjSV48n",
"18kMmtpafjuDtKWRjvyL27ZvjXJn1qdfjK",
"1aiD3PrZGPqAwVpTWmRMSrmouVin1FnX4",
"19jWagsUZtBx5sBfMjVbJqETd5Kw4FEXYR",
"1CwzSqJauekY3oYvAjuvGMGJ74TJk95uHx",
"12jLxSnBYwQVHcT2hYmU2zh1Lt1HJSjdex",
"1FLgsnQEWidV9GyRwW81HsUzEKY5p3GCxa",
"1MqRsFqZ5cF5hNfyuAhedv5umDTqJBhj3d",
"16rEbALCUV45JHkTDhFbXA58wpxWpuYmkJ",
"1JDzTf1FpGMiBkcf3dAB3uk5eaNG8PV8QL",
"1EZREWXV4amqW5HHLxsAhqRnR72AZPGp52",
"1GrU6U843QbnhyuMsHtSKXFwka8SRy8qhe",
"19jbzbzfkVmYMwrkHDS9Po8HrHcj6qXdJ5",
"1FjYwWqr9yDPoXH2cK6X9S1D5DZWuoZH8n",
"1NXGCGRdFzzAnbir2xE4GUe1sUTyRm3sEg",
"1HT1ukjjoiscjRBLV26Ya3L6Qi5xqnnDcS",
"1EZ37zH57t3wW92b5nowB8R4sULhRoS5Wk",
"1CKd9EK7dDPJnYvybSSng3YdQuXK7tXhtF",
"14eufo1FAqZYr4CAsgz5AUgahcKeSsGAEi",
"168Lj791DuswX1p8ThkRsLGy93Gi52MnrG",
"1HFw5kixZioRaSDtU6mTH3WMtyR8Kbx8yU",
"1KRG61x4eb9xCfvz5uafvy15r2WBWeX1YH",
"13PmZmHyTJ1H4PS7XfVUDkWtVSqNAofhiM",
"17st4EAq3TDRg76vXuaNzmHTtR7vf8Cdx9",
"1DpskscnAuerjExruxYodVpRpabL46eRxv",
"1P2q7mvSkcU8WBHix4PPievMyPy2wnSX8z",
"1fUrW28fso3EKjEWPPw2wMaFcpiJYu9CE",
"1JdPo2PGC5nKYNTWGLw1SvcSuuTi8LjPGJ",
"1FVL5MebSnjumgceGhgPj4uiDKnk1vSbKZ",
"12hfNmUD2rEovJVPYPzQ8UJbKaE1gei8tW",
"1DsiMK7tj5icKnju3zjwxbsYAMChMzEq3t",
"1FQeEF4bvjpPpb5bcYfHKGcBwSC2dhY1gu",
"1GGvJnj5BkA7uh9GB9VJRsQsxrotzbxQzL",
"187hGgXosVdZunzDkriwHM7gk4jYVFYHYh",
"1C6k6S1kGG1EykyvFQXvKXyYd2Hd7XXKPb",
"1LpxWR1ZbuWzNE4vL16x73GPkCFYW3xvtH",
"1KJSvB2FWaRFZFG9pjmD5CjPeV5T2BPdUv",
"1NBgLe2uWBamtzUWYq2Nquj8X4QmqALiU7",
"18VXFUi2DhpX1don332cytEkbUaejMKukt",
"1FaiM14CKvz6LBbJZ8aabK9Zcv5EPQEUZB",
"1FJ4VQpde45dndaeTZKvpC5jNQyDch23sc",
"1JAvA8UhjVS4hgBtd7fELR2qsHxpk9Exem",
"1JQFzFePnf93efwYNTX1Zs4iPXcUeHFJ7x",
"14ZjwGXUs8qTazrZd1JNPNJVUCnh9Ke5tc",
"1N587BP9bM7QPgnhpB7qtCBBT8uVWW7q61",
"1MDZxsQQre2vEnKyKNygJruKzCxNerMZ6w",
"1F1QRAnWksTD5oELiSbh2b7hXN2kvqzwrJ",
"14st8iPcxvPHohLtjHb2bXYR8u8sNHrKh6",
"1DMAPooJryY3WH7VaLpTNKPcag9qkwicm3",
"16v6HsMk925Pqwv9C2ftNF1dRkaDxGe8i1",
"1DbYUa2ryFmbkgSXDpGSCpjMRHe9DFp1SB",
"1DXvHVPJKq3d4mgiCUEsmnoWmbqkZgKA2L",
"17VSoyvveM5Wdc6Dz2Wp3YcB1SAixE34EF",
"1xGLBNNPzUc55Xd9zRWcNhKFZuFDsTNin",
"1PkeaSxrVRUiSaBUWtmCEtTJDegm1eiCAk",
"1AVEbZLpGfvNSmjx1BmT8hRQt544c4KDPa",
"16Zmo97Zb4Z6Q8FiddfmJn51618gTqD8XD",
"1GshLqqfjUE8kXaKkTB4sDjVoPHw1syvcC",
"12gunb8V8jh2DJETt49npGBX5EWAkUmuRo",
"14YEG8Mgd1P6tspAohbmEMMfEURonE1jqX",
"17FbYBHDyUD8eK6ZXAXoSNhJqut7DJDjHC",
"15EyNfiVkvV7ETdo6GQzYfzDL49NUfL9H8",
"13aVfMzSj57z7bkh1A8zCKDjdGN1PJ3Qkp",
"147eSA4e35GHnX8upC6fTAtd3mJxhCzFDL",
"1NES53c2CzWZ81B3vmmpLYVHWu4Fk35m3Z",
"15qEfifhhjh7tuaCAcHkAMhtHaXrwBy1mE",
"1Ffs8Y24NkVnxs5qU2upeEsgDqW6G3QRjw",
"1LXbWqSFJ4WX6XxHqrP3KJ9yjNM4wFp6SG",
"17Uf2oGuEjjxUBEqiv6Lf6Ar1twwGB8K5C",
"173cB256q8xPbFLdZBCP1pHBFAQMfq5skR",
"1JA7eEJpFTCq24coWKwCsjHHv5JVdpkoVM",
"17FVPzMcxSByTBNxgac9W7QX6pr2qXX7un",
"13zFAcRsf9FCVWExbZNqsVbdHV5APNKsaQ",
"171avQSooFmaDbmpCESWAEwVCessfKGT3U",
"1EgUCt1PvtELqtiohPYeXpkyawNPHDdL8K",
"13wVP7eGbHAWwDTVcKPMLQzbz8iJBFHy1E",
"1FAM9LGE4BEUNocFcvivJ1KYysDznkZvz",
"1FWaeS8tuzSNzC5WRnxuxpjSVXHDF8TqYg",
"1PURHcjmPDRka8QWQS129qC5SBhp32vKMR",
"1GMoSGo2w48ue5bqiJW8sz9r6JXBz5Yn6b",
"1Mr4Qr2HaPmtfCiRsJrDCYQ5CsPhUxPHWr",
"1MyHefMRsv8amqWc8pXf6Z8ydPvo87rs1y",
"1Ks7LfmPaD92qQ3mVSHEXUGnWvffDjtKVv",
"1A5LdnGwrBS7yurMeHdx2pk1ZLtFVqVpi6",
"16tfrX8esnfrEAix7BENCuqfnSB5MTuuJZ",
"17peWHy1xDzM42viqQ4NnZn46AQYrN4CPn",
"13dz2bXAiK4VQyb1SPZ8uS9eKGMyoAfKc9",
"13oa1ZkNPwjNRzPB3iUwLCpgy6PTH86Zap",
"1HqXNmsWKfFw11g1UjDFpJfvN9tRqKk39q",
"18prvVBQBMNeC7397JtLkn4ZDj6eHZnp3K",
"14kDKqWBytSryckmVyBcwvDRiroMtV898f",
"1LSE5LJDNPkkfYTbgHT7hNAaTFNJvbj6gR",
"1LJv2pV6sumxsCbHfrk6seL5wSbW217DQj",
"1MenaE6xVXWpPMoZz5mfkWB1ZoWGeRmT7q",
"1ABaet9JnfaorW8viuizp8YrcNMUz2KHkC",
"15AjyMCPZNpXVLC3ko4YbrStWV8wzG1rbh",
"1Hegaw3VVErb9pD55NTNeUBbzEQs8mmKis",
"19dYUbAvA1qLhCzLsnUKzekUg6NPeDiard",
"14P1TkGDUCWviLZZQLog27N4fhhNU9YcYP",
"1F98y5VpAJ7WY6GryLWPGkeCKo7yoa9jYC",
"1Gp1jRkPsSwmhjKPb4E5Uq91Xzjaas1g3E",
"1JLUXyySG93apuSyyZTW1FZeJt4wa8GVwq",
"12EtSDD6Y8es7kccYm3X2cAcxUjWcZ17yY",
"112Z64hwsZjWV1cvVgfGYAtryjZS3XM532",
"17BKL6VB4NTPNZNhxee9RYPpvyGZFDn8kH",
"13JqLP2hz3kUZ7eWZsNFNa9nyBrkZ4YKGo",
"1P4jvdfj4fAPkmWcj6BjSuzWsbtYfeXBzM",
"193QVgiXsZaf2vJkwFV2y4Rf4e7nM5WjFd",
"1Dc4LjVwKf7w8UWKz7GAGSzD3XPARYgdYx",
"1MqY9a91DpDjwaUuCY4LNg2LnnivqnNfsW",
"1LgV3pRtFjGtpYeGZ8RNCZaXb8SK8oaA4j",
"1HNk68McQZG5yAjj2R36yJQQktW45bvhn6",
"1G4RjrxaUAUDdUbR8NDqHRXVaUz8CnuYAf",
"16Z9p3Kzb25Ty8TEjPfkL9PTtWQmzHY9KY",
"1KTokD2sLAFQNXTNSFCAJuDubhM6gQ4kq4",
"1361KqraLjpVq4gyqbK9PfwxXFQcXpy8ic",
"1JgSZc1MPGuSybkvg4Q9gRqWNq8tqrMjS7",
"1HDctggYksqAyfCF12tNR4UBNZbiFa5mR6",
"1N5FrQCrskPr7DW5uP9nQgvgEpnJNqwAP1",
"1KhRjCHnP3oWkJu9J2R9U675UxFBjLZzXS",
"1FbjdGDwZv736mBVryrahiXaau76nmamiK",
"1AeCTjhffcibxfbZG4brVMws5qJ8ueKngN",
"13S8Tq99p6ECPcYkCjj1AvDM3tGCHAyaPA",
"1ALF7pUQXoaJaTeK3oTf8LtP523MoGvwDA",
"1CxUtFL7JZccCthQpzMpVwCENHNnnfibww",
"1HU7mpz8dzegkHD4gBsGJw5Q2qa9u3jyhE",
"17iRkBby7wTuBSyVecAnt7QpmnPJciEYcS",
"1PEJ9UgkhrAp3kwe7rLfPNyxGqYegYdo5x",
"1A4JaLZ7KAVb5iYwbQkTCQUQa53QgbxiE7",
"13YfCzwGpjuWgQaZVxukMogkqFpRzGncaN",
"1HRCHpdxZJWj2ha5EgJfFKoeQ4cgagCnWN",
"164mmW4rxW7sr1FZthZaTpAw9BS4kw2RBM",
"1DSrRVgtCgpbQ2ibeY2FWYBZDeJFmAk1jB",
"1L1nG811NtmARovXtkCEKzDuger2gaSgmy",
"16JLavBEGTtyQLGJboTGrt3WKo3CE2kbMq",
"1Kfnzq9W92fpVXDW5ypmhoWZsy4dpeXZig",
"1DY3GdMQtNNYHz8V6NkRVvt8TMDf68zqQG",
"1337oDt9aFUpmJJncStrDBGEpyxw3ksDb5",
"16VhwMKaXqS1hc7c32q5Tu12DoN4Az88to",
"1JZJWGuS5PNMgnGASPhvYnF9n249n1YGBv",
"162fM1p5UzfUXTP3ey3T6qU7E5FFsVng2q",
"1BoE3QehWGCZ7dcsBkCFqhhX8mB2fNgwUr",
"1CzYv8QF8KTmJPCteZcM2fviAkkbpCF6MH",
"1EkY6H1cY9Z9JaZxwDSG9cfzHb2zbxzcWK",
"1HhMwF4Dj3w22bfKGAYQDBpW9NYUGXkCQj",
"1GrvpUGUaXUCMwQ1xQXZbBduQLY49a12aj",
"14V7Pwr31W7jtvqaVagQybwsQ3bqqXkM41",
"158o6uP54MiFLCbtMdyrm3a7H6XKSqTeor",
"1EUbPy9ayAVM2sYL6295Z72jGq9MX9NGbH",
"1MovSjdnZPYCXZSYEvRChfDFxH8td3sodE",
"16CsDirs2brQpq55gUB8DP7PyQ1Py4hqhD",
"1NKvpg7exfkeZV5u8EUvn7knkMVwoqm5cg",
"1EG9PppXineLbdFJh1heRPfUw8FgkxifX4",
"1LUu1Pvsn3ciLkRWzZLvUwiWPeg4kj1AV9",
"1GYhfxKUumA2z5V7T3HvFVbbFVQG1XpSSz",
"13P6Z2UNKHhVcc8QmrsmRWU42ASwKLp9Pj",
"1PMmYymQjGELFjacWGnMkc4oUB7J6qMTGm",
"1KQFrP4ceziMRyHvpiTrag4gbZZNcZKHwi",
"15pQ27Bezncow2fh5rPmo7srZomj6RKG6P",
"128T7K4iJn7XRNKXRUWHdncKqaByVYtxrN",
"1DmXK8VxycJpnSZn3d3SMgD9xRiX4EuoGa",
"1H7HE8ybxe74C6e3ThWA121CP7NYCnbe73",
"13tg898L8yN9tvRYDMwQ1YacVeVac8Q545",
"19yvJDWawbrkgDzrLeDxP9k8K4uRjc7JPN",
"1HybQ7axM7r1uu9ehTWNi5gy2B5Gp99KDn",
"1MHtzBQJiCA2DpwsYWiQX2DiQe4Q9FrPYn",
"1DKgkv6xfDzMv4mCAiNxBY28AsA2myVPs",
"16PZ5wUFGQwYUhvFAQHnfgMpr4dpz3mEEV",
"1BoCmhNDqMkRzxzrPqLttD6fnos6XsqrfA",
"1EZ6J4X4bNe9zDx1BwNB1cejUCuJurWAkn",
"1Cs77r6szhBwcs1CoQiBLUWhuTMwkqyPDx",
"1KT1jciSTwsgpdW9MhsgQtCbZ1r3iNN7az",
"1Jna6egHnEH2HurPuzqP8SgHo41jqSRUsJ",
"1Er9YoAEUhHuYtCdnJAh2nEVpf82MJRRFD",
"1FKMP2nXaLq5uDxiyicjb64McA4sp96LgM",
"183SmPVN7PQUa84dSHSdfWB8V8kCjsTgyB",
"1KBeG3Cj94yBpQb2dg44TZcKbe4NscYqsU",
"14tHhamPNLHQFZ7QGLbZDHRyw1L35xjXm2",
"1KsG6Mw1SP4pdLoTG2DLGXAWfRKYBKvvFP",
"1Bk5LVXwbgKg9sKsh5YHZgDtq8fNPec2HH",
"15C8VWMdaMAuPE3x4gjbJQ3XMoY2mHGsWF",
"1x5pcTiWm3vn9QSJG4rCe47SzWNVmEbDb",
"1213gTURgpL1ZvhW427Jdbg6drp4Pj216g",
"1HgYzCj3nudFLMv6fHGtEGZHL7pqkjfjUC",
"1KWGNXEYANchhze7qeUiGwyBkyMJKJb2T5",
"1JDTMYkZUvnNXuXZBUzYb7bEQhCCum9Tqh",
"18e18kiw5yk1mafp6JAMCzioueENgFdbLM",
"1JNsSdV8LtzQDsHkmSR9FbBf9NxGpDBeMw",
"1KFG95Be4cqaEuHCSoTNw6Rcy7Rrfc6geE",
"1JMJ7EGtZiMw29kyMnxoUy9iPMNkaZoqAV",
"1J16aNXm3TGmRinHXk7dDKhLBoeMDPZc2b",
"18Gtbq3rDX74QHdNY4Zy9K6BV9RcvZnFA2",
"1BRjg2ww6k6uwZbi8bcNpBh9w2m8fkMd8V",
"14aUjpRYDGnUfKdZ1Eg3SdzSamBiupi9w3",
"1GbE1bk5Tj1kWz2LipR59gaoCpspqSJYhx",
"14SkwQ7UZeiL3xRG6BGywTaf44a9y1D9QY",
"1PFCNBbGNrQrQUgTHsDWE9LJ1af1u8L6GR",
"1QK8rveZxs3gn5vUiC6RVwdHscCwdiWuFL",
"1NsFBh2stMN35UbJGpwMT3gNbJv2WTL489",
"198N9nomEvXXs6S533asFRcGVtwXv22vwF",
"19AToew5GtzJD4bQNpSoqPM8KSAZW8Csed",
"15TyNogGtg2YWumC2hg3NxD5ncu2jACQW4",
"17JVpS6vsqWMbMReGnRXd9vS3NWDiZh6Po",
"1nDEgwQBwc7dpfAuPHrPjsZqrtc6tKRtT",
"1M2hQyuuoqWy6i2SoiDh2PyEEp3eSpQGXV",
"1zx2DgoVp1ZenyCxnxt8S2LaEqCfNmUp5",
"13Top33AxGJCX8nvnwiD9mprbzjoiC8tzq",
"1LHxBVXcUxU4NXUyRRN5du3gjivo8BFqGs",
"12nfEmMiw9Dim2VpYQEci8N9KmEMc3GJE1",
"1FoYq9FzWgTW3BJBgMzETepan8GC5EP9w6",
"15tLuNCW1mZfvwN3HM73Uy2MP57fY5BDPF",
"1GUN5PrjVMg58WCcCtG5xnmiGUf9st32gJ",
"1LvBJKYZ3dFdmXoJKEKufSaZjq8HvFysWe",
"1E7j5TH7387esyUuj7zj8pzheKZ5B2Kzbp",
"1MDRYKWDn711b7tpjPGP5ZNBYAF9by87k1",
"1PU4SsaRiRAgzTvdGo5NfvaX6UKp13ZrmX",
"1HoY8BY2rjweGE31vpTUsPX7bk5eW4Q8S3",
"1PHDki25EY7SHwksvFbkiDrRNkQAq6njgp",
"1Nm74jyNrE9FCJUWjDe6Q1eYetFzVMB2v9",
"165EcSP4LsTwZn9SdPytbhVgHWcNKK2xV2",
"1EAPQP4mKF2tzRcTWHQx5rVtFtMjznXKz",
"1L8qQVz7vWgzu3Agthd6d9AFVRM4V2bH5s",
"19WVMYrZjRVtyhHcc7crMjyVFfCXdMt5Q4",
"146tN7bfntMu3rkZnRkrK9a1ZzmPPaC5wt",
"1FSSHYvbrfYWnh9UtD9p7pmgHK6swfLTZp",
"1PwpuS5ibDHkAqqPMZkRMaZ9k36TmbFqUX",
"1FNrYArtLqd8sraxwid5VQdEUzSLWsT7KE",
"1LdmqM8gn4rMYWYpbdDrteYZvZuQMud4WR",
"1CnciS9XDHrDnhkENbBdAf7zPsXq8ozy4i",
"1H7Y6SYNUgkhskDfx792ZMHqNSRzbdymee",
"14znzBF9vXRgGhDMBMQZmJR3JP3d4r11zo",
"1MMi7pPx69gzZFnUxSwLQgAmnEKy4LVbnU",
"1Gn12j5CubHHVUmdGG2BtNqhqpjQTfvSXK",
"1B2syjioHXPAUFTNZBaNBZ2ebu6eH6prCN",
"1NSXhv1MFFBSbNa3BKPf7fVZKCvYrQXJWM",
"1X21FRhtEUz8QMPebdatboLqE6Stszp7a",
"1EjxgLsCA17a9AyKbE6Qu63Ui9oUXxYWdZ",
"16sdyXuzrNPAvtJ8pQhu6bMKK6B3JmJd8S",
"1HR2V6MNU2WzPZsEqNSByBRwzg7fqp87nQ",
"1MmvVSd7Q3LDhpkV9WrCSw9DjhbNRprEXb",
"13i3FU3iaK6v84kGRVBUeDsqMLuEwvYCY4",
"1Pve7syxh8q8V95fXCQm5CYwMeUcbsbmQu",
"1A5F9VSwT7HVQeiNRCgxxq8WHLBDCzR7us",
"1KDaajQQgZxKceWJuRXkBR5SNiKQdNBcXM",
"1FaUHA8hdiwhwrP2FAhBU8XZj27P6WVsk5",
"14CttpdU2yes6ts4tB54y2LprpmFs6aFTM",
"14G6CkSKbbJxZ8kZXdexr1dchg7pYG5icw",
"18gurxnQv2BGdXDCFeXb6Nvj7QKF5MkfTJ",
"166gb399paKpda2F1KB32i1b2X2eSY2o4e",
"1Mmy9oM6JYuvTTh62akVbLyPv7eVd7rr6k",
"1G1kNifLkZu69FNLi6bPZ5vFbwwMemZuR5",
"1MAWhiibNsTEWAReqGaHFGd2b2JqoKWV8E",
"1E7v1LaEh98exP2Dv3FCyAgEGHyWkdn1Fm",
"15E2cedaFccDipcCA2T9NpVVorrJ7jNmbk",
"19s3JD4dTRnf7ACNe9cY9sSK6jjAGhpEqC",
"1AQVRG9WfEswdWGk4QvUkRqTd2mez9JHzq",
"12g529U5pcx2wwPdbWRxVYNR79Le9NBfDb",
"1BFJyhK4saQ8duBnDZURiw2m59z7DxWWSi",
"1AD6yKWcQ5RiuPJA9b2ksRJeatujiF3PaZ",
"1PQWqXtP4cMCqeSYBKzfKFhg9kuEKKEBVK",
"13hsqKYXzJjcTNnHRcM86bNtu5oTF1qQwH",
"1GczJa8BVvcJg7EqBmXNJHiuUA3kFiFbZw",
"1ENi98VLSuVSCvs1iTS2VnWXtaJTYK2Zig",
"145sEpc13QA2FvNn41Fga8XSCfe6baorfp",
"1646NNKYZyZDDuTbNufTqYEbqBMSNyqCAd",
"115nb45jLfAZW9F3x4dqRLMq6g3X758Kbr",
"17uaWdeVjv3EPCQdk3xm9fekMNoBFFZU4n",
"18m36UenKFtHtazkMSbhw6PbLhEFrkj8Mr",
"19xgotRUV7KmZQqxEeV6ktwJd6EbqWCtQa",
"17X28FiviNxdU4GCRo8qmvmoeidYqQWnTv",
"1V6gL22DHYRVyALnTYigEu2TktN58PsUN",
"16TALKxJJYkpqAvg2enRNmoBpXMJyguFTC",
"1AMZRmDRxqasTTUSpD4814Lf2dMsd6TfHi",
"1Ne65Ysz11LKpVRTsQzzWCeoSEestuVKBY",
"15EhzAjJUagq4XVxDg2oes6ARidEqGBMbu",
"1Ek1RsH3Bdd9pLUr4ktRBXLZfjkd1UzLpf",
"1BKGqf7sm5rGr3ZF6Y9edkBaiDXMqwDsnK",
"14UNMoLgva1YkdwMcFY8v5MyGhAKS6w6eG",
"17FNCkYj7K1hUn12j2FmuuDhEu26uYjfFq",
"1JpDq29Mya9bAiYypx61HMm6B56HXEsGsY",
"1EV5eAnmfLA34Wu1Yh4ehf276prMn8Jx6Z",
"1M3fXR2p2Hr5FxK1NM1KqHWcYcLnyKYChu",
"1DWWrUywh5pUosJcDXWPKpULBT6pBSiqt8",
"16zP7HENBF3NbGgYBGynB1h5J54iW4BKvd",
"18bAX4NZPTcnnmtzy1tGdwWZ5eRoY3JU8H",
"113xebNSwTxNVgkG32QxHjHm6HkHiHsjwa",
"18856Jg13LzMrzBMDCBDMoeo9vc1e6d6pH",
"12wEWhiaud3Js9fbvj9rsd9gLXoZDABbzm",
"14eTvAKPkHXdh18RsbQHizV9u9L4sTqBEb",
"1E4LeMVLWbwtYaHDAY41DKxxPpaEcssomQ",
"1CsoMuQYEsSQyigRsKqHKhdYprhuzeeSJJ",
"1LbfLC64qS21WexhUaqiwTokGnsn7LTFQa",
"168oeyBZkDzdzBMcNy3jLjzn4Mymehmz45",
"1LHWp4vGXGZyZu6X6Z9HTXnt18LeWNzmDh",
"16tVZWdSEosZiJmyxeQKhdeXbMiiTwf64T",
"1ieM5TxSN9jrgrfGBHbqojh6QfeX1WiwZ",
"1E1D1PTakmkqzoFQNCohTwsG6hAkRyXTjM",
"1MBptDhhzd8xrxxJBXkSN5NiicCDxDwDVd",
"1G3A72WCs7q2aVv43Qf2y6oFS79Mt7UQfE",
"1FFjeAJuF68kZFKi8jQHPG7GK8UhaBM2gp",
"1Po9b8py4uwhmvXr4wkomXu4ck3LNiqvZp",
"183qRQhgXw3ZuPXtmTNyCHqfsSqKYcXAwv",
"14JVZ87Rzt2hNbS2BwAdtSryppkLhaNBR2",
"1B6t2fGGsJdiKHbgjDcEDz6csdzCrJzsk",
"1DzaxBVmGnUbjFo3x3dtv4FBvECdMxu52c",
"13TEKCMFjnNKwk5J6Uo92MvqaczCEdmQm5",
"1FjZKtYc26QaDDk1UrrWVA47wU1VtW8DLw",
"19FXE8PYUitgSdpiZ3FcJAxndNpToZVh15",
"1HcyomXFXQjmXB8yiBwRBoPhC9ZJQkvNZC",
"1AfBj8KfndpcLLrKa7pTrVHMQxiqN7XCxf",
"1GEkPj2BZTzTb7BoqViep9ErisHJH2A9xZ",
"12dtJ3gTke7isXYEV6bGB91G6AHZVLkKic",
"17JHpdXfN8R7jdY1JEU75oxjrPgeknLAc3",
"16BeYpuZ3Y1qu5iiM3bhrcNUU6UFvuR9gi",
"1JPXb7ozs2jdcNbCmmRdfzxS6Zb9ZqjSiS",
"14HSSWruzUcFbJZhpodFT5b5ssdpKe9DzX",
"1DRqeSFo287yjKpu8rh7WHNdn4PSfL4N4J",
"1Ep7M2eSnJgcgRJnLBa5Vy9QJLBf4NWDEG",
"1PV4GUfoPYRxxwJdXtevGWpuvPa6YPLSh5",
"178fiWLPRXhWqZrMbERDSJkfdeGNNg6ptm",
"1EfvSHugaN2VeCE3NB6eMWhecNPf9SecdZ",
"14ni77wQ1H4iX4Z6yGhxTdo7e9cnnTD7YN",
"1NJWN6pX966N9LqSwRhAQhNRpqbetYfT4h",
"1AGjwVHVvfPgx4Z7tcgrxKNh7XiQGg2dyL",
"19WWSdeXWiHQdaaut1FuTNMEUTSWvG8Q6o",
"1JSfkg3WxhK8ZKL5jW7xNtRtXbPjdUFwAY",
"1BRNMmRUDj3BBLpRW2fnJqE8BwnbkJm2hE",
"1C9yKXXtHKcaeCrCbEv7iUi9YATNK94t9x",
"1AM7MrSKbE9y3YPy9GhYwwfsc7juyGieTA",
"18GzHNBJtRamxn8rzLk1uPSkiizXqkqGeU",
"1Fy5K21VhgANWEs2iXrjMmdphGpto9DmJ2",
"1JhWpkKjYKtTLFS2ERBQfrRdEyKuDiHZ1G",
"1EWr5ZyZE18iD7ZKYR9Tqn7y8Cn8Uiyh85",
"1MrSG95mjsfxmQsjv1kFPQRW2XJxL4bmp6",
"171M9GcAGYRHyVbb3comvbsitS3fgUbp2F",
"12mjKWM8i9eioZdJqCV4XaEd5g8BVs3Kpx",
"18vfd9JgN6kBw2ftTPMwaJyEYm1N51wDJ2",
"17a3KLY6qFku5PhYHiDmDgTedu1eSPLyy4",
"13YuMDL256kkkykZRL6BgebjtBsfctUtTA",
"1LvK4YEhsgnSvZSJgVEUovPZ3VG9Y7nYbc",
"1D6w9JTqnsa4B6cFh8ixREGs7rfu9RxRi5",
"12SomZcDdjAtMe6mjr7QTYXAbvUutqbek6",
"1LJKXz4a9oe51pUp45BoWJzx2JHuKvAAPc",
"17epNBM6eB2G1hes9ZjBwU8KADoNwvEeue",
"1AVUoMJ71zNFxU6ggUJa6CXd96zBAepqEP",
"13Hcb56caepNeCUoA8jPTp15Ry4tM3SRz8",
"1L5P7ppEdzr9mCZ8FPYwncKkYjZg2kp5Yi",
"1EafFE5fcNvXfdVH5UtbisVyTkq5c2QeuX",
"1ELtzL383aLyoPXZdLDWMhCQmFG7db2G6Q",
"1EyFjijGQDaFDxKajPSdS2M5vbNDFJ3tom",
"1BGiYscQj5QML2YeBwj4Nm2ywuxP2CdRsn",
"12G1Bw76FbA8pMBVnXbYB2yfiSdRRXofby",
"1PW8bhXAgLLQruFar1qxUmAawP4bz4gSS7",
"1HzzqGhJMa4KjQnRpufEedi3exEp9f68PR",
"1FJBiqbbXvu3Dk9LQPGAfUM346h3Lqgmaz",
"1LT6b4MqFiMba7u4eCJC5BCPgXurNqSiaZ",
"15nztBK75jp3LK5yQG4PE26ZqaEhGD4dUL",
"16cgkNcRp44t93iRQSJ4HNuvNuHfyv5gAS",
"1CpM4VJ8gueJftqBrE1Qc8pr2WU8pfgNjz",
"1LjziUEtWt2guaqMVtw7k3g75qufmxnL4x",
"17wAM8RAh9BboGEykGFqZwa5iR8VaN9Mk9",
"1HHy4EvH4XJ4gsEUnkzPJTHY4vru5fQwRK",
"16mP38yUmix2Pd2HfjFG2G1RkPE2aYQXxA",
"1G4MokGqyuNBpBFVYkJDJHnvaUadbgfnMx",
"19jUqHQa2JgrCcoq9wLxDpsGypwJtXoWZY",
"1NY2FYWHBo8A1vvyznNQAiMiYe3smTnC27",
"19XtbJxrsewsVvcrxRFAXUoxz6Wp2EKSfs",
"147VXewqUHWWQ8SnR7sXKLXYp7bhUkiJyQ",
"18XWjCkKkJoCCpgPzBZSgrenS8sbQttV9f",
"1GsDgBUvzJGQkfZMsbG1oQ5ZJX7nzAVoos",
"1G1TdkQKqeG6z4FTqmWnS12gNeaEdYcxjv",
"1Dfmam53DAgVsY6SEaw3t6DbmmBMLvLxSs",
"1MdbTsWT2NvpgTZxazBVsMkpu5nTuHsgg",
"1F9YB4H7sxjpctAjacCAGAAs5kXSsrycrQ",
"1PfVgedqGWnVEYxk5F8iikZNF9nM2sCNR6",
"17GXWGeqiCvEo6RKHGRz8h4n1ohrNK8cei",
"144YsxH3ZnM7ZF6tDWD61bkk8cRgiRTyoi",
"1JXRP3uEqzrpm2WAZzVCJBDoFkgSiZ8XoT",
"1D6TB1A7DHw53wKP6iACYxZENZNP7w5BNq",
"1AmoJEYr84Jf5xrEN4kW63s3dYu8dXUXnP",
"1AxVCJtx9QTkSBFmKkSXB5iCUF7V5wpD6z",
"1zjv2Rguj36eA6RCmmtPGtGNxFbNT9THV",
"1Bh4nuakbAJusAxpHRtuu7A6zHhxSYVvim",
"1B91ewwviDpY3uNKpodw6z9v17R7yDQSFt",
"1HiMiwRyYqUqjopyxAxHobVdwKD9SqC7PY",
"1LWv1P2mmRoQb1m7fgx4h7Mg3qhLXNkc3e",
"1BnFZjBk3upijAtQTo6xXQvsbbfWxsHoER",
"1AMxzyWCHSpuHDf8cdHHDfCgy1xqFLdQqo",
"1GQikxruYw6DXCweakpsNkfqVBeuquxYFy",
"1JBaHZDQdTrbiEpz6wZo9CGVMkJSTmxVDz",
"1AgYyeBKVtrkYwdHEfuZt6bNYPr4GbPvk",
"1PbwNdfiaNoQoTDtqxVLhbWoehV6eZVhSW",
"1Gme5zCjRAyp18KVwVKwVcoBbq1jh1wHeM",
"1QGBp7rUbM4B3t5PnWEojMiEZEwP7sU6sT",
"1Ht8VwQR89XdBNW8mZMjGMpMMmLDR57G4r",
"1AeTNovk322vTagkDn4STF6LF9BSkudbqx",
"1DttAjAGk3qJaAF5j9rUAGuaEVnq5w8qeS",
"1G1YyDXc36FQTrRdpRTFK4YmffbqEYWgv1",
"16uSMQsLi8e96VPqjGXC6TjyUcUjHowYm4",
"1AdMgzyTpzaJaQApbaN8mng47xH8f3yrEB",
"1BG2eKWdMagJKuLwLePi1G9oWcyLkDoaLQ",
"18Dn18aajEdV8B1wRurNEfPEy8mdjvZ5DB",
"13dtEzrGPHwhL3G9165oCutq72HzeNyKd7",
"1QJA6oPGmK59PvTWF5Vfj3nu5of3j4aEno",
"1KSFGF3URzBhB8TaB67pvyifi95dhpRUV",
"1MPpKnq3wGh8ZwU1EGmUyA8xER6kQo2MZG",
"1CfTJuzZ9yTecqUR4tBsSerDxwHE7HLAJr",
"1Fi8ghw4Kq3XaLBPZDimgokcf5cyMRyUVS",
"1K89U7JfkkqJnSrpiYgjkYF6XnpGrrc6zE",
"1EMBJysqkeBM7fNrLT6HX2ZAecTWqGJYRq",
"19fLcqgW6SY47hE4e3Mch9YufZqGq8XAeM",
"17MmkZHzmFF7sM2bBrjrJA7fsJFpQ8Pq1J",
"19J1U7CWNya2fptnyFLrLDbTYN9rLeUirU",
"1LA5XRdFXfPwXjh4c37YUYXz6mA3Nb89Ws",
"1Q5xAYtjeFSrDYKxWUSft4WWXvMaAFtkU1",
"1JmiZDCFJU3w1xzJ3jukJBAmhTFXdcVpRz",
"1H48nBTKmfXn4s3gHXRyoPGBx5dcXtUNXj",
"1CuA3pARmbnqSFGVDdAsMkgRdfmn85ttHo",
"182yd5orKFXvDwneVmsYBYts8BZoGm3gZW",
"1vd1iK3temEktit2x48QVF7amktsBZwAX",
"1Hwhj1amhLwvmWnept7wEcn8Vz4iw3ZwNu",
"1MdP1NWRTavwhEjjnCV2wuaJCQYFwx3Anr",
"1L7tHA5wfPh8GAxkr6CWFu2hpjNRGCKWNB",
"1GxA1b7JTvnTZwjY3c7YjQdBcSe2kQi1DQ",
"1HCohFvPNM22NxLTBe83S9v9K9Zkf9AN6L",
"1JXpNS2j6BN4AqkbMVG3nPQujG8vdAJR4H",
"1EDP9UyX7aLjmSgvMwBk1iNAki8WCkkZYX",
"1B9VNJ2zetJ2PPKirXxnovM75x3Bd9Lh4K",
"128RCdonLx7GF3gJyKeWLqPc1KNyGVbZTB",
"1AYUdxard2aTSP5ByyZUE8ekisWLBEmmG",
"15rnYgBqoARQ7YJdmsNiKP89Y2jwJyvNvZ",
"1P9cQ9mJqjgLcaZvtnYb666fAZWgF3nd6F",
"1EhwAycWFwBXQFPUkVpo6gL6rW5HXPhZe8",
"1Bsr3sbitoXnbTLM3YdXrZwXEzuTxWYw2W",
"1J5wLE2xERN2tu1p13AoVp6F7bJzY6smgd",
"15hjxPVcVfUQa3LvUDAay3cwwLpZ5VySh3",
"15RPv7DGsTW9mEfsMu44d4nfQPAXUxYTJC",
"1Fpqp9F6HdPvZuLbwLXLhuKR5doBMsiikn",
"1BoJ898DP1XEyjGQuTPmV41HYe2K8aqXUy",
"1AMBhc7nGuPWAvsQjMTqCKBLV5QLEwPfKD",
"1BZU6rh9Pfu6N1ygGdFJAi7iMrMvw7eWr6",
"1LKBsurfJxUbvWgp9K7kf6z4QYEGPHCRZn",
"1LbsZC3ZCjVQyAosSbw53sQR18MXRp4xXT",
"17F7XJs14bGguHcvTcH8Ppfj9fwrpU7Jg4",
"1GPHBzcEb7V4t9njBCn5A2xP9NPoBe7CBw",
"19kwXMx12h3tuATPTwycLVkKdyZ7Ms2z1g",
"1QHmNdEb6EzBpLAHJd78EKKfGvVeuxxPkP",
"18TTYZ6cUKpVsyXiML15kWUMXWqwKZhB9w",
"17iJu9b4fbPZpgKKMbJfEPmXgKMvtne826",
"18dgA3LQ4updMensG6Hqr6kgggsxXXyvK5",
"1FrKXLxog1aiCjxM9GubcRb9XyTuM5XAKu",
"1JZ2jPAT2tpQBkSwd4kZ4rsN4YRg7UGrcq",
"1PnLiq579AwS6UfGnSsD2uvXePonMgSL8R",
"16GURghNxcwLvjxrneEh3ouBFWn3FzvgnN",
"1Hj1mLyyuMuYEKvR9kbwk2pwCtLQDuLbNz",
"17bW22eRXQRJn6t3HNEver2jZP594gpHeF",
"1875JQJzmF1RCTBEF34SCkgoxDJztp5pf4",
"17UoVopx1WyGhveGjGmLXuQABkoaKKeksf",
"1NpuNUhvpY4SoBeexz3LC4RXg4HEbP1PYY",
"16K8oHBu2QFJPPtRiXz1DrqfKvdgFxaoxx",
"1Nxp2N4oJ8yLzS8g85g2oy26PcNtFxUHMm",
"1BvzUD6k89ucX6SwaXY4RoT86htR11rxaq",
"18oJitV2iUKw4R31wXvZGCNTX79uMaZimi",
"13YneGLnq4LVyVccm4Ztw85jPGb279T72f",
"1FLKzyzT4eVL2F96Vaor9GY1Y9Zz5AkDYp",
"17wgHyMZuxZs6R1ddAB62vHs2QJYAdVHfC",
"1MRUn6QFbAugfuAgGg5bwVgiHPMuAsPwdF",
"1G51gnwTqUABAeGK6Zsz9KbW7S5Qe2qt6s",
"1GSex2tQLDRpNb6CFHDNNQBoyXsc2HMThs",
"13KXRkVZXh2RtNvZr2sjSgA4vK7paTswpB",
"1Lzepf4dGJthcvwKJDao9pa1vzaTEVt8gL",
"19RtJnXPAkXQ16Len8xvHgfKit45T4JEC5",
"1LTggKbbkZwEBPfq4t3wDzCF6p965m3P2w",
"1FUaxJQEh87d6Yo73MB9anQQ5hne6p7Azo",
"18wfkcWXLQmY1nrwXxnqsHE2U1gPfgaA3W",
"1FaJFvdcG9X88YRJs38KCo1JEVzPJo7Mqk",
"1Mw6SBgANJbjNV8tmdZqqYiYTvK3BrEAzp",
"19zYQy4JpoXLJXGrWr7khkjB5kRqTun7QS",
"1DXuUVJePtoXsTF8KoU1iCscSQaZdHTuqM",
"1CyoWsNBfi7nBHNTLcEW2NdtYGXh8UekaC",
"1ivqsxqzCp8t4b5ESNnMhUxjz2HhauYDx",
"1Po1v9LhxqNjp7rppxYsV4Rd55tZTA8bsG",
"1EMYKgnY82mNz4Bmaj8MzxdmTQfW1CVh44",
"14bPu9rswFJ924P343A2hd5yigRYBQTt2k",
"19z294ovGh9rzDvwc4wgDoRFChdkuPCBFq",
"1EN8NokLnMmu7m3SXMiG8gpBrCgmqHEun6",
"1CtV4igbiLaHNRyjcDGuN9f2bYsDkvKjJe",
"187gRmcxj4qXWozEmuposPsEY2KTgEG6yX",
"1LLSBF22zmRQUmDE7RaKVQzcSKF86p2jJH",
"16TJj9PbgUPkMDyHyyntM33SmCjcFfd64y",
"18gw45YcYnHq4Pw56z52WW66jDVnBA59Qr",
"1G27Aw8yYSmAVLR5B1k8uh7F7cDTdSpwoa",
"15poJ27C7Yg7bE2marwj2V55c3Rd7JQtZP",
"1LP1UDQb567e6ZEpZgU35mp46LVwXCD4P7",
"16kNvoVsSjpDwFNkpYLA35EBqzMu8fiK8R",
"1bgsgGTidou7x96eJk5WeYhtxhSRRyhnA",
"1XEMXBJyWTVfR1TEcKCNbaAbx6e52QQuX",
"1KaUKWF3iveeNu9MLwXyfti6XtJHbsTqXg",
"18bBzqwoMaNKqcQcNp9QdUddxupMm3bCr3",
"1CFwWVt2Lby9mP9wdB6j39SARALKqZ5E85",
"1JzrPFawmu3gAVTV3kS6ys1mQLFJact4ML",
"1MQNykP37rJYrwQzvDYQKq5sJt4SVW9iRy",
"1BSEwZN3reLwnGco25SzVZenQfLXfatkP9",
"1LYFboURoJkX6SjDNu2LtchWwXC3G9F5KG",
"134wk337X3mhB7ZhZEnVgcGLt5JPzdQ33n",
"12GEkmcG5fxfeC86ZPedj27PvFbsTmcc9Q",
"1Akpm2NNsXZ7rPNRBE9T62EYzPXiF9mxu7",
"1PaSuFCytNKriToxPNhBHucoAVTAEzunJR",
"16qG83CpQwj1Uph9xgzWrdRxU41Q6fdBjH",
"1DtBzLXcPVUSagYWZ1Ynx394VnTweh7Kse",
"1PhLK3XGcR6kJP4h8eBGLA43jB7b8JN69E",
"1KxYMAvSrB27V9ZKrtsrqhK2CZh7hkiVKx",
"1MWmzqBCwgt5f4rFd5fPuaj7ArUdBX2TXy",
"18EnGgzzSCmLvuGJekj9aumL8FM7S5WGNU",
"18f2nrqxkEM41WQnch2vgfbhLMiQSVbwii",
"18DYFRRMJ6ApCcTMSHnu5TuBvUsnWiHNd1",
"1Dxh83n3dkkfjef3mADGc96JhFwhwFtizd",
"1Pih67cCsN7vxeNV7Fz1GRG66WCcva3dCE",
"14uAh7v5tk8ZVCDmbjMGrAJ88uRAvGU2E9",
"1MFJgFedXrXwbJkGokTLQhjFk6i4qidkEv",
"19aMB37TtDMUsUzxJy4MRc3uUdtAwFdoPa",
"1LMvx8VfSt67Uj9uG7yqfDj2qGsEkxxpac",
"17QgXyzaMTSHmTJvbusM2p9t1fbZk91Gy5",
"1D1oBa1oLJ7PLgvnd3bofFX9kX5hktGujF",
"19KvqjQRDmHG4WevQFXiQB3AFnBUXUFRkk",
"19f4ag2KtWdyaUGaadK2p3Q3hc6qGUDEZq",
"1GmpBYsCtCwxedk61FQ7ZCJMh2jtUxVnyY",
"1B5XK5z7ZDwP2JQeQycZmFSio71ujTYhNe",
"1Nk8zCZeyjHM8Mzod9NBW5QYxt3Zgiua1g",
"1PTz7kk5grsQKWg4QknHdCW1MLBfM8ggE6",
"1A7D95mvoVo6neTo9B8acvXkPytZXF8aTM",
"1HWNUQ3QrYqrSb7mE9FBT4qeenkfk5uskk",
"1FWgbir7dd7oWr1GC9ZRAmCneu4DzJtVEK",
"1M8GMUzNLd9gjpL8mWcmWBLcMa8LzENZbS",
"1PqbNtK7RR2k2aybaGyyboy3mZRQrPGZnB",
"1F5a6Cut4zcHWPzMrxa5WYxNLcM1nep77s",
"1HPfmUPQyD7K27JztX49Hht4C4qJWZUHEV",
"1DAAcKWu2GthrYvVkx55sxtPadYprkQcQY",
"14BcFKbygEgtUhnAs9YeEV9BdGgSaV9kM7",
"1KqKZF7jQw8mCGcZAL76ci8gQLPrxsdwZg",
"17gqZJzNus4iZHdB6nGC8q4pQudcAJUKfN",
"1JSgpgRKftzN4uRFgaQ156eNjH1T957q2y",
"1Lsi4xMP3pqqi2XwxXeDcB4AGoJ2NHRQrR",
"1LeVKXahR17LyxFQRWesSW2jziuVjJR7yC",
"15kGaYne7oMymD5nWJfWNiHrQg4ouZDZUm",
"1FQSBpHiGF33Bk8JzwVU1ccN7zchEpVxQ3",
"1KJ7Tpu7vEL1jXPGPVQTgAjSuS3RXDab7k",
"1L3vLJ9X5Y7BqWYYjzmdL3WXZ6wm7ZNWGA",
"1FsCPNUk4sZpnmPdMPM2V97wNcim8YAVKS",
"1ELp4d2HCdd7WwdwnmxbNpsnCDhtmDfHKv",
"15Y9AVWpUDoEiGUZSQLAKZstDqHfBKBs6q",
"1GkKsWzhBArWcC1wfTjo5sqobzLChiCKzd",
"1EtTgqdEZ5A9a8VzWNsrwX5VjkkCV8wQfZ",
"1PcmQfvWLZhT2jE3tvFgVeVTarB3MEKdXA",
"1Nj7YHLajVJ31SUddKx7H7FH2pAwW114ma",
"14JqBc6wQ3MFrGjZVW2Xh6Zb1XUrhuJywZ",
"1Ph65BgHhLqqLVY66Lx7E1nYDyBwndjCP9",
"1CgAZDqwxjbbA4CajUarnf4Pc7JXEo7kBH",
"13iWE8MJeSRUXjpFyhneoncVuuHtQdVpQS",
"16X76QakZtfiWJpWtR7n4gD8EhBKAmQNJu",
"17GjBmwyysWXrswa9JGjeWDfthgoiFdN5Y",
"1GSB6E4BvUvJxKJSprNeVpNXELgqD7vjQ6",
"1MPoyo3XMNS8cgKi8vwGLZyQvu3kfprton",
"1owwQvYfa2eLJdiyiRKiPyBgTfTmbiaxv",
"1AB2iAocYH9eG59SETrq6TmAQTvgjXvJ4r",
"17Jf8EmHZbUuaGbVBvrVaBAUNYEmbu7ypx",
"15cdHXAKHWFjFzq3vUi8bGMyjPu9bZW3nX",
"1G3ZsHwyEEprA8ZrN5V52h6KuzjkVZw51o",
"1CdNDmaUEp2LxYBzP4FHR56JGZbQk59QXL",
"1GEyN8tzAEWmgFMpnrsxAQVoFBsVTCw1vr",
"1fmJdu9bXmKuHJoAu6NF7PSG8P9DdoXoT",
"1KgL22WjRswG9Rc1NetSNE3Ry8wxGgoBu9",
"15HPRg9idtgfFE6zYxdVQbYC5L1dRctXsr",
"123uJaaAZLjz6sPKR7tVqoYZHhyJbpJG9k",
"16CvbsbMp7H9g5SSPUR9jTCBbhiUMN9XJA",
"16cjC7pVgTSZyqtCidnVjdnkTJATLZ7DvN",
"1DBacjFANx1VNHey9wD4rsLFYz2L8hUMQ9",
"1HTHqgmoGctz5syCxAwFn5hQ2U1mzQ3mTc",
"1J2w65NKWTC9Z97Cy8JRUZHQ9kkNk8dwKu",
"1Jh8QDSbjqUudxXjNgk1XjbvaDg431dNS",
"1PDxxzXLXa7i4yaJJjm9JBg4e1KeTmXYHX",
"1FnWuy69yViKhBAqYfvysWCPVWscCfxGaP",
"1F7ZdpaagRJ5jNQDW4yCaQtXH4m1JE2uED",
"19ozFLKvXdWZy6zUtUg7E6pZmfeGiBYUa4",
"15hdFKfY7eTbmq9fHbjscSAu2PE9SmGiwJ",
"16wfvhHtFeH4NdcxMch4imLEgdZMkNoGvv",
"1H5iNTyg4NxLkQWVmVFXVvsR723jyYMo2D",
"1K5FMutorDHzEcMzyWzDUAHs9vSJ5fcQ14",
"1CeM1EpGozUEFHknLfKgZVBqos5E6zjeVN",
"1JUxYRS2JnTFSC6122NSjsdiHPWjZA1tpD",
"1KKEryYZPAe6KEUWrQW1frFoJv89QiVdJX",
"19Heob7P2ccV6SnZvSXki6HPqJ5WH7yEfJ",
"1JpYVMTUsYEW5jEZuS7kYURJoyjX7Fs9k9",
"1CxnfwD6d4rZqVbZb4jUfP1HH3cd5fGE3j",
"19jagbRKz3cvNpfMYfp8mDZ2w54ev7eFC7",
"16jp7FNYtQstRPZCWXKK2HRbXJAqfdMbq5",
"1LpRzmsnTv9dwPZKSYD7jTvg3zPguHc2Gj",
"158oJPx3mrZiYS1Tdc9wqLeuj7CpWPWbJm",
"1bxRdYXZa15qYmhhAg8juGZXmJcHjyCvg",
"1PN9CsDEQP1CsXTDuosi1ZSPfPULk6jbn4",
"1DoJJNCMtV1ttYGuHG2HqzhSdVXnHkZJtz",
"1FQiQ6NLLMrEwuCeQmbejcQ2X8VY96hfYt",
"17JLLYphib7keJxVbUcrkMSSpDQxYoA5Ur",
"1LMBvowXHdUNBW3M9H7qX4ZG2zRXwiafUD",
"1AsJse2fNTDzzxwN9dULYSr5NLNbJ4Kvjb",
"159zKzEuxHL469BjhMNoW5f88mYD7CTa59",
"1Ag5WgrdZatFc9zgBDLEcSLUJZf1c4aAT6",
"17ruC7D9Ao8hxA94LW44agexm8TCBTxBDb",
"17Kfbe3PEj8VGoMEF1akUeWdfjdW9gHiki",
"12JhL9QSc92cyyDVRriCHTXrdU8wbAigWr",
"1LQLSqJW9t5Kjea351r4yaTbTCbMQ4JtZN",
"1Ncma8wPC14W8aQSe6rLtnt7Ti1z1UE6em",
"1PRWx2iCL2YiRZDff8FyzHQYeaQmX2q1BC",
"12ufiw2MAdY8cmp3bJKGSYWAiPiuobbpHJ",
"19ML2YYptvRpDdmWsK6pqS1oUyqeuNNsPG",
"1J2BMP48gichKGLb1PXexUuX7pzgYMkJx5",
"19BiSBAQ8YjJ42BaXVx5H9BxfKkG64YSgp",
"17eFhADbmm5hPKRCqduviZZs8HGs6WyaRP",
"16xadKdG45p9sHFe47geJgJXd43jCXAL2F",
"1LrwykV3BTj8mhgXtcTGbvY5E9CkQMXL3u",
"1PJ41KtWZiPdp1FvxZVGhZANxUiKXKXWbU",
"18EVy3qbiUZZjciD9YmwCJmtngc7zU8kAB",
"19WUNQrJBqr3DAhmmVEcp5cspPo32qT99d",
"1Q1QmTxWY9P6xBLSNLtPCvhjxRzuzoUUP6",
"19asnFmGV4RUHYHzthstwhZHX5eDkBEWYL",
"1PxGK5kTcgkLtMrQiif3dQSL5QvHKf6oV6",
"13kzT3UaiCTZh6sB9bNkTSTTRnbRGCt5ZB",
"1BSL2mFx2TBVrEMU14BcWbcuKNWQ6cf4Zy",
"1NRDwtmebptvz8sxeDPFraXG1w4ipUiodb",
"1M1xmWRv6YPc6GMYWnhvvGJaXE6N7jmEqE",
"16EsbLGADBdQpYFKnQ2FFvht1kf4gk3bSF",
"1CF1ruRfKaHLyjMvRWf7WYZUZ8GuBzmbib",
"1F82XYrHK3PS4SfbiN81Ef4f9tdEqLLpgw",
"1856CKxxLiWyp1AQgM5w94s95TFMg5qmT8",
"1Frx6ejLpu5Uy44famiy3eSo6QW5EKarFj",
"149j4j48tCvQDtKSqz5dLqDiBSpL3d7BHE",
"1BWgyhQCKMW79WDDqT1b4NEbftdHH1xeAu",
"1PLYJpvbMMcg2oDaD5iAe4wfuL2Eji3kMa",
"1ARWoHEJsbwRyqY5Kx3aEJD4RHjtM8amdV",
"1DnYpYV6b9T6iZ4b2X6HP15De2n2FtB4ba",
"1B1kv9Pxnyr6NcrkvQmnRBCmcF89XL6dNC",
"1GWBARqcBzHG5azzwPyxW6s3LnH1BGW8jT",
"1LVikU1Z3sXFGEYKiMskvd6gMRLHQq271k",
"14LNWs6ZKSW4wqxVogUR28W5yLUanUmxL5",
"1LrjHTbczzwneG6i9wA4B3q81VzBmgRGX5",
"1GaeC3DRXsKGAechGovLBKMd4DEpJQnxwz",
"1VJ7myQTpEUkK8nw8z43gFXpgDqpW2x3M",
"14Cc1F3ofvnHG6J6JK4ry6sLrgQNy3ZGip",
"1LkV8PR5b3UAU5jCRVV3rBrtajNuYyfTh9",
"1LH6oZvGdDdinwwJ7ChZbawLRpJWuK2fKg",
"1EDQ5L1e3friFXMgEEf99UpPW62hRXXuiV",
"1HDAxumSqrB1VLJbTKX5QmwFVHJ4Bm5Hgk",
"1JLa2KnXV9kR2EB5FYgMC1NMXMDUigT5fP",
"1EuHWNS3YGEvxrtk758Lt6bZFXZHdn7MA5",
"1GTHrmPRB5i5KWqQGyqcsdZe11C22xhXch",
"1GVUNYs8M2wuaKYnH7vahRJsJHa4SHY4rJ",
"1CStPUqMtJjoAXCrzewDUkMQnyR9AfFzjg",
"14qHm6keFMwVjsGmWwVi12ibCVJn7oxBbJ",
"18dGxv9FjYuCUyrRzLEgc23nXtPAkNPsh2",
"13X59dFgsjmF2o4T2Db7AMpobDnjBznNbR",
"1DNWpQMsTDwnLb4d5XmbTEMTa8ZtmZPVcQ",
"15Y5DdqiWoM99tuAz3vYEcNtSQ9X3A8nWs",
"1JwAp7rG4SHdm79MvFTXnpEHTZQ25UCYna",
"15DvVe8LwutwFhkWSuJqNeXrm2B9ifzPei",
"1EdyAUMYTUzHLUP5YQ4AXhkSimoxn6b8CZ",
"1FxAzAWHiMHyaQWFquz3o8EBDRM9SrMR2q",
"12oEgbsUxQ3xa8njDvA8qxGybNUU8tVJUx",
"1M5w6pT4E9cYR5JFPkEFxm3yA3s5pYemRS",
"1KJtkdB3RBVb6ZYioU7JKYGMUQr2NNXpGr",
"1JN6wnJ21FjeVzvKazhR5V3P9H7jq81zsk",
"1EK3zSgNWZjCGezEuJzHMRKGL9in6u35m8",
"14maYyP3PFQZPhXufJJirzGUD9n2cyQkxT",
"1DZVqUPwxmdJEsM969wjcwz6TywtRT8JWj",
"1MntVNs1ooGqBnb6Rq6j66JcV8M28M24pA",
"16ad8yyUquW8fdzMfXYnkaq3xXWt9JY8Sa",
"1MHN2nyeK3dmtZDdtfVpWgtXWAZwdFGHDw",
"1GaiVi7A9TtTPC2oRALiVQbYLxvfYBf6DD",
"1Jn1JYnCQ4o3hmYdECP4bzyj7Evddo9EF3",
"1QAUU26Sn4mazM3ExwZzWXvSfPdXdYoXS6",
"1MypTExfiMKeZyfUJH4V2KVBdWTJ2rLQWj",
"1G8EASUQk1VB3h9kgacF4aEWy9LPFSm6iz",
"17Zu5ckW89YzKR9JgmuNQisLLWugjXAUBh",
"1C27VpYbsZKyBddaywVfiEgkL46CpCtLpG",
"1Gtn8Uk9oaH3P76E1mznEAN1uCV9VAL56i",
"1HWGE7sKTHgkFuKVVMPNZThBNvdHXka18w",
"1LrniAmfUQnykf16QYyVHoTdMWxGpWcgrm",
"1CrFqD4f94nQfWzLPpSMK1Vp92P6wSnN9E",
"16J9bGuSjATuoZmAQ4pwgknvzV6t6kzaT6",
"1FebcEXsrQg57QeuzRgwCSttMRNwAZf4Z2",
"16USRgFVHmmwFyAB49Ku66SsiKZGJUGzwA",
"18vv2JxcDivKRrUrx4hU9S3k6oHS2S2gfB",
"1Db3eiWrqWn7NtepcL5FVhoCMckbmiStiq",
"1BkEeACF4nPQC7kV6WHoeW5hPahPDYqV72",
"1N4vasXkKWUvK4iTqcXRRZYLKCUAKNbpcA",
"1GEKQyvkjyFe1Qwf2ViCuA6rtTYxWLkR8M",
"1EooG74FeDbNzgTGQf9kT2k5d5u5U9Ayt",
"1PYFybswN7BwEyQ1FY17BQdzi3SiYKuGkM",
"1GuSegVgvkPDPCKJhWo4tS2XKpaxR2r49Z",
"1LLDQ13L5SPQGX4LJ5RWkEdxLdeZKhi65e",
"1FsNBDsDEauy1f8J4XVwxVTzT4omdqZAet",
"1BM3ruKdMwG74g4ef9rGgm1FdLywQZtUjT",
"12vB55NkndzGfb5AWD5ZzSUUKDFPHtrq3o",
"174t28D9WVpo4Wzgm8Rw5Ho5DD4pmQB81o",
"1EjDhFPF2hxQ6m8EPPTcbGKfCTZQNd7VH9",
"18w5cKYb4WJz6GzE6tJghMnGrvTt9xBd6M",
"18j4bugjm6K5m6b2Bhigf6z5uYkV2QgL1T",
"1Meu2KbKFBuKzuLuWei9yh7ray8ZRrWpM5",
"1daZd4vEntTSwQiYtfA921WBfgvEEAJKZ",
"166tSKBuukLszuwRVyfTdBnVx9ThJZyne3",
"17f2RfNaVHcpk1TfkCxGxHRGGvsa5xHu3k",
"132wpgpkZZww7HCWLoSYczFMhBn6qUuTjr",
"1H9EiqZxDPgppDiHbSXkcqGAQcQ5NEbbuZ",
"18zwwaTKTrFC1qXVx8uPiPuVAeQrUYZ8nk",
"17zCfgWNnyzeuFQzzxM3NdGgVeTdXpJmJV",
"19zRtvCD9a9nuiBjcAxbYY4NwBnJMzEMPS",
"15s6xvFH4GJjEM2grBp8nh6LDCXhBWzzGG",
"1AvB4ZiJqVE3iUAFcPBkxZQerQt8dSwXAV",
"1Gb6CqnzcsfT2icD5wbc6HLVa1XsMVRxy6",
"1Ah4qrnsqtGMwqjUEDtcY7jWvTG4EnKdgX",
"1Ftb3hgeBrQSUxqmD9wxxJFae4An3hkVXH",
"1Fz2jV2t1vTanjuKtE2mRFV3bCKp9jmn7E",
"12rKPQNdT93XtVUujFagPtNA6Hv1fwpp1F",
"1GkUdfX4cXauR3eGgBpYEU2ztzgKtjpfPL",
"1NSkUdAWgRg9bjjXZmuE1xwCK3vVonYg2z",
"1DqxV798TLUTjr3ahubJawNuDEeuHhffb9",
"1HTqAcCyHctpqPDD1vrMg1cCjvfQNAprqt",
"12pmNjm7BN5SmMkXw4qmp2ntJepoR2tjZM",
"1EcnFTgu1tDBjYKj8nMhoRTwxr6q7QARsk",
"1NZVkqyScqAWSijHJvHG6jDRJLYLCd4pGN",
"1JmuivJ2VfaHrmv1EZw6YWtaRjzhFYvXfR",
"1AtVCts5UTR1oHNQB3xYNZAX28dCvCaAWo",
"1E9Cqb8AvoQ7LvPifumf1XFpqVuehWkRhH",
"1EMkc4m6a8ih1e1dDs41biVgZyvMVw5rHZ",
"14CuBze7HXPP9FCBqU8i2XkcPk4U4MTqBR",
"1BnZpq2xNH3hKtSvmBCPeNEnsUFHT1AgaC",
"16siW4z3D5w9AB1hguRj9Dpb9ZueBJBHZx",
"1AExqQY3fXuKhgwxR7oGKH6ycrZdgNujaP",
"1KUTa2u7iQGsy7Q3yANxMh72qxVeii8zLn",
"1JejnBULu7Jrx6mWREQ7zMnT538cWsKaPn",
"1KUHYz9v4nghQ986thfvtkUKb96MsUJUfP",
"18kZH8Br3pcu6nBSSdT24dkT7kPko8h55u",
"1Q46yRpdS5yoW9XSwBXgXUqQv1DVVT8E4r",
"1AS4TiUzEFYhfFhWJFcQyGguhmTRisskfu",
"1L71JNPzBpmmtFj4wmfqBzPzKduEVgyi4w",
"14U92is3i93TASWTQaiuzp3UEGNTNoM5eY",
"1P83WtwhoERCNfUZ7W8BCtYuTA6zKHsciG",
"15qKDqKicSSj6153pSLz6XsucWGmchtpei",
"1Lovwc6WxYf4MMpApZ84PtGz6pUJkQ9JjE",
"17EgwxupTJeXU6p6bsrNNUKmpSWgTbbo1Q",
"1kuMjBcRFPVnFsbWXVzMpZD3jpXYnYK6K",
"1NnpdbNkN4xvJjJ9YgQCXbqavjyJ5MHRsu",
"1FCZ8EYVYdQsbeFsezk8fh6GeGAZ51EDbN",
"1HnwnXBq8FGognx9U3KT2v7j8ZS6Hpkt53",
"14FygiWvgd8cV1W7WDbw9htsEGwgNJic3T",
"1Ai26bRFLTCaZeTkjvZobCeZBvz6wTeBEr",
"1EC7mPs5q5pvoXEdtdynW4EyjkxcLqeoLf",
"146geosfnycR9KryNWrgYyTwaxanshgRfP",
"1QBZwjjcXaVr3FEfcieaaEyXLY96MFVtJ",
"18tMpdPXmxZNSdFBwHtYdgnyHvLKVntkEE",
"1BqQJVDkRvX26BqvF8PinUb4QmPzQsn1Qm",
"13ttMFJMFJ41TxsoXeFM5qsySYpUemRrdU",
"14c87KZ3tYCSG1HSCDEhgMdbPnLiqN3ghh",
"1BUo8pouf3CRWu9JsenViWqCHA6PWk3yJn",
"1juMWL3gPeToVF5xZ5NAtAzEHCyUVL5SL",
"1DJFrExt9WTuZ6cVzMUSLMFsXucVjxH8hj",
"1HcAj1oToUbYsQB7hXnSbrPYJ2qtz7vzaw",
"19oaogJ35HafBW8sBk68mqpzB5N67ZwVD3",
"1MjQFuAwmqcu6Grdw6AjWvm1F99oneoUYU",
"1B2zxmSPQfmQXx1xCcF9TsAH8uCyECwtuM",
"1CUybSqJNe32drCfyntDiRZr9DcXKQzb8y",
"144GJyKznBFcejtfEV1Au6ePmyaim1JRPR",
"1FBGkyWvk9aYuHdDhkHTxySfMTrCmq5Cvh",
"1LZGAk2crcXLDvhARwHurb7GkHtXUGQGM4",
"1J6nHzEtpCyf71y8YsaebUiaSkVCgTvZsP",
"12Luf5qDrg65EjdYfe3g9n6Wfomr5QRMSf",
"1FXRe7UD1K21ZHowH9tse3Pwzuj6Q2dggY",
"152MSDgp5DEg4y234p4zn4jmmVfLyYn4pt",
"1F8H3dbScBhRWVAwcP9ddBt7tB4XY1wTAT",
"1cy2iZQgJD6cpw1j5QFUW6UjFSHTUEZZa",
"1PYn8JsUKDwZ4NZUcVYMhYNicdVPpvq62j",
"1LNND4j9V3P4sbtS6roo28FduBBViT5LFr",
"1fPAMnCTzTK185tTXXiht5VZZt4jiFtaK",
"175yr5QjiApvNpozv2yN9kYRLkmhSLGYLg",
"1PHmiaQ6rqKkc8P1RWBq7E6gtK88fSVBgM",
"1CcAPTkEY2hUM3nVSmkhRvGAuHvjLg6vxz",
"1Adhj4mXMJ8K3e3mKfHBzWZpZCdn3ucjf2",
"1MCXGoWftQSTjBTJJfo89ECRU2ijHrKifn",
"1Ha1JYxAkBzN7rm4GGwAcgaeDgvW2erq3Q",
"1XdxDmorrRk1pZ5vN7HZUAmjW1cubXLBc",
"1288d9Dntk23QBrw41zLnnPbFHyVDCwp17",
"1KofnzHTWBvsbC3bYbopKnQTWsLL9LcP1m",
"1ZUeWWYcWnXkRRGCwkb7WnoHBfV6TykCk",
"1Ph7r48n2JdT6iZeHuGemfNDYWts6PGqgn",
"1PqRoS1ERogYuH6gv4yB7YZ9qZyy1vAuSK",
"1KHEnsc5RiWpUtSPPsLSbNJcciZy4dZYSH",
"1JUrLjBuupEuWBbRdCw9rnhbG3kQt3fRhx",
"1DjHRUFEnGZ6HPpnD2Fsd6K4hsmJ2adVFB",
"13ADbjS1fpfrkYn3PTD9G8N9ubBoUtpDoY",
"1CgPdF5wMkQSugZ1RYakvMn9wWkm5TzxYv",
"1N6eVnkHMrtcFUh5XmCcPfDvASkHu7oWnq",
"15w1zfwdvd8WoyxVre8CCcoiVgz5x3wBis",
"1Je5B2syVRM8h3tzdCkxCXiif1Tn8PLgz6",
"1589xWb47Xc3SqS5zjy4MhjvKfzgr1eqNq",
"1HNvYCshT5NFQjrJybDS8r7pcHUocjdLtq",
"1EfenvoFiQfVrGdzSoYP2snX3ECzZRBkHF",
"1FSPpMTgxDGGMmiBcM9PQGU8gUYoR84beo",
"1416DqCGvuNvT4bEfUHusvNYDYSqdQ5PH4",
"1KeYHvqRbTfKYvTPwZ9UjoPfkF5tVTNxuW",
"1GxArgHZwF11ndjZcDYd1Wfo8sCmt3wuXM",
"1P1bDe5PaQr8H24uLeHqd8DAC1APM1MVsa",
"1ArngdTtnbqQTxVdUnoZHV3qgicn2JbhSK",
"13a5gKz6grgwjuBVJtCJommJ1wEaWzm6dz",
"1Nfjngz3KPMnds5hHQrhhyvHXpP1zf7SXC",
"1A4aSHX6XH5yB4kXqbGELfexWMjMB4vAXq",
"1ADEVjA3RNCZ8SXbc8FTNmGjra3efZfA5R",
"1PgFptBdoBSLYPhPGUaLhJqf96GxbCU7Ae",
"1JwfeS8Zm99ouueoY5bkzGb4XNPMmDrNZR",
"18J2AWQMnyVWrtq6KZS4oSQNTBrR1fwqLL",
"1Mz1KXtTDqbDmPDq8WRGMm6o2icBkAZrCX",
"1MNcTekyohUFB9XurfoKagQ8zz2U7vbVEL",
"15hDN5usReBmrLuHAvrdvWcBbze4ZfbQHB",
"17ZmK3Qfp9DxL1Ju7LkYfTMsyT15KKrc8g",
"1Ada1RGv89yoorhxybbN8zBTftVwHESaNp",
"17um5x9eip1S5tHHhuLGBUzPVfFS5RBcTk",
"1Fz66aKyvFQvm1ExpNJPekziHKafF8XsWH",
"1Ha3aPx6Pozg2F2pbB4J3fGbBnytWUseyk",
"1Jp4hpqRPWkhLd7wQsiuNL6u4j1AMsoNou",
"1GrzEb9Pa3C7xvCtB9faLJX5LCxpeP3W7F",
"1CC9jjezoGrto4zwroT5D5Ef95712Zqw1C",
"1E1zm6dg1C58EAFsDBb9FWsXeZtrSAEBFT",
"1N4tAZtzARzxdcKm1pSav55bEii5cmYUaF",
"1DgCGadN9WqCjJ61D6WhwB8UTYhZuNJCP9",
"1P2zzt5eqAKn47CAUUZPiYPGqvfcPsDdsA",
"1Ld7jhcPvSbWtntxJN9eq3d4SRUF5yFBXv",
"1E1Msug97cnWE6w7QpTVLWDTsjvYC41uba",
"15GEjV1LQqzdXaBeo2CQN72vH79Svr3Fx4",
"1HRvw1rJtKv1JUz2TrmrYvUwo9LrjvXox5",
"1F165rmXQQyR7825Z1byd6EstUos2WMd2P",
"1BgLGwJckezXZhYBi1qZp1xGUA8egbyLbE",
"17GEn4GmaHEuF3iRb9UmhMMFmJjeRt96Nh",
"1KEbEDMrjGMfv3uE8X3Ja7AiBbFmLxQvwM",
"15ggvA5PAmHQHQgSzcSch3ea4QbCoDV9Kb",
"1CnMS5SeLgJ32b7ootXw9i2fSBEL21URQQ",
"1A2nbduRJeM8MKd4vwPQFn8RNWEh5cgJDV",
"1GkiPdHhXyxruSezD7Xjg9Dyzqod7KeR4R",
"1A4W5ruHSUfvRBSMgBiieA8NhcJ43dz5mr",
"1BCJxrXwDUePXmMS5xtQcqj3E2oZTcu5m1",
"1ANLmSwwrKDK5fQtAi9s3XyDTU17vcEvBS",
"16QrrpfDaJ7FUW9Ru5kc7t6CXH6WXgbxN9",
"12gPco9j6CbepHrH77x4HGWTBYtivFQKqU",
"12p3tsAU2Ac3uUqzDvrooyXwjfziji8ZSs",
"15tj3dhjy52jgqzc6sv42gx5yCXXVC9gUb",
"129N6aU41YMUv93u2v1R4wFPqbYAGZf6Pf",
"1Gm17tqLFosHPZxkox9jCHNBc3uDQnFdNN",
"1ABfDXUWSGcE45AGsk79eqWYq9yighpdoS",
"1BYKa3NwrLyAiWShdKmnwQpXHENYBDcAiZ",
"15gaQS6xBPTG7TDeXyqwLxj1r7zEkBeybi",
"1BPEQeTKnsnPmJaE3Sf68g7VKgqPBTcSAX",
"1BLjFoK9auC7XbvnJ4xt39F4igTG8tN6Ga",
"1DYwxZYbNVh9jxKH444jS9C8LTB2kp6vuV",
"1MHSEBFWrbKQV5zoc5NC9KrDkqzxj4Yt5i",
"1J3PfWhqzQ1opCHpEhjTCUR9P7fy7BfPRS",
"1uKNUagbMDWLv5jUp9LewSWUTnUsQVeXv",
"1SrJsWokeEmWHPAGdQmbXcnP9VPbd4GJw",
"1ExnhJ1Tv2aP7nWf7RCD81YvggkH4K4hx3",
"1HeyaUUZzEoK8n1715LdPdyiwjFxUNFnuR",
"1A5TyKjStxU6WLRJUGs1qkQxjNeEHMK7iF",
"1J56maBszz5qbv2UFBChW5NcFF5csCf2id",
"1BRTmuGXjC8X82oMeYRJmQwZvdGQaeQ3eo",
"1LSZ3uKoihRAsxq9npngPPtTUtWjVyLafA",
"1MVcQ97J7PpKaHnF1XTxhvBRcdMVpPJPZk",
"19N8f93jZZbyunA2ZyqcAiMcnBv8smQicF",
"1JQ1yX3kdDaPnzcs95Gog5yHEbDhErAhXf",
"1JRFQAp2ThRSUA6M8XapMCXgVUmhC1tLb3",
"18hGg9iLbiPh1uD2Dgh5ZnpqWSPR2LdNVH",
"1PKXFYEthgCWprbnrYgEnMAQcs5ffUELAB",
"176X4BVJcMWYTNVMxBDq97BoiFTu95h1Yr",
"1275vRZncGmax33b9HHaVRAcUB12Zdj3rt",
"1DiFLgvyfuavFL9zykvEmfcoW7ZyN2TQh9",
"16R5XKaBvFFT6bdsDTpotdprrWtwgEQfkb",
"16Xu5tH9SwuQiRgiZWiJa2xia8JUxHU6Mv",
"1NLG2UMMSr4o2Y1QmSfAT3BxfYYvEWTYhN",
"1A7FHnjAeWXSkxZmrTVpBqFbxGW9D9mwPG",
"1CGitMxrNqK9RxP1uJG5TwkwMKJpgm56C8",
"18hWTRdJDEU1yD7YRKAEPZXJhLVAKbnhWp",
"12Xeqr7RA4ibyrKrUybFham7novX4KS8r6",
"14Ro5b2oPMFNaQAQf2nXPUuZ9jeviN5o16",
"1L9j4o6nsScYfhXy4n9nfzuwyzTJmKhNyb",
"1BsnvibSi3KDzjUu931Sukpyms6TFb8BQk",
"1MDMBm5jsgNJsmboyT59qmpsqqacGYSmD1",
"1LNebodncDCS41WfsJ9wEgoJRQ8qUSLXX1",
"1FWHRHj9QiA43duc71xQrmr6t2cFAdpM27",
"18owgwHrNgFiH8V2mzx9aMFHhwc9XTv1cy",
"1Ah9vAAT9TDYSWrRBqMbfzhYCRWNs3M41W",
"15LcBWKNS738Dab2e19RnJmqnDryqDMM58",
"1ALVNxLAjCEGketotfrxtrwYADW8xyUU3W",
"13ML72NH28qcR4Nb1uqCJ1Bk8wjARQgtwv",
"17zfvu2uhevpFNUqNQXrrHbeVTgFwdYtii",
"1Es2L79ffEKSuM4sqK4GnkTcZARPres2cq",
"1AvMDKg2o2sZ8HT72GNeVWCPbpDVLSZxcX",
"1G3ZVB5dkviWsUvA4BunSSxsewQFXVUdRU",
"1DCVUgZtNVZfmyx7nuaLLMYAsEpeeffn7B",
"1CSe82SiechmAAw71WVao1en8BGTAF6JPW",
"1ARxmmwZwdLy5UUBM4rNkqT5Q1GCguovCR",
"13RgpbtN4TMKK5rwo6kyvJWCvegSrZzQBK",
"1MsA6mFjeTwrZE1jrtQ1esTmPcWL5ZAyPS",
"1MMVDxEcWuvneefkMH5FY75VXR6636r1o1",
"14VsY5evHAGnETqJyQ9cjBpLqZM1z3wWNB",
"13wmMvEaqvbTb3ALww1Ha5dwMaZx273i7M",
"1FPjexg72Diqv9MdS4U9ELNrFLnx6zuAuM",
"1B583pWxfDVz59w6VieXUaMxEvFJVWbp6g",
"1QaK2ctJ7KRiogPtGhWnpBLptHPWtkwVG",
"1GdVDC6vepfe3YUG3qfcHahtrWd7hGNRDF",
"14ZrAi7enuuqPRm39QaYkD53J8LvgMU2kW",
"1ArcTDRmsDNSTs3A7DB6Upz4Hcnen3vjuc",
"16p1iXqBJZ7wVWZiggm4cZeQd8fjUnpUH6",
"12v1TaFoUYHehckCPY2iYLfg8Tt4Yq2gUZ",
"1Fuukz4hDu53dAf3fSiy2MddK5RCkp7QFH",
"18XRix9qqYj82BiEE5PQTYD8neQyVuzhRF",
"1rTWDn5mQAanVtUjnWH2XBU3g81zePynd",
"1F7KF1jW2xvPxQoqLwVf6j8h1QRLjdHjqN",
"1KacReTcKdZGSgjYEq1NxzELnHJGzqe47f",
"1MtNKPtzx9N48rtzoH1EQ39ciQ4Q5zhpxr",
"1Ljx4ikPX2n7bknwX2eBjitYhii4fkjEBD",
"1BDndzvMZbesieTUFZ9ubABBbgR4KNeo2Q",
"1P1QbUKEKWVEWHWfDBPzkeJi6poUSAGUDL",
"1EXwwxW7sACRCwcjG4LDhRiNdoSng4frBR",
"1NSfSrigHvKW8WeaS27o37PVh9TZP2RAFg",
"19rcpzcU32xXabrLgN6F16XFnLDG24yS7t",
"1PnnkFyMH2YEc3dcS4Z5SPXPBxsiPH8Z2j",
"1HJmckRzDuyELHtEfyGMpUbpW3GQRTEoh6",
"147WojxpM5sfoUsNUfzSoiHhWvyS7ib5sU",
"16YnS4dy9fnfC1UW7DehfwKvPujjGQ84Js",
"1EtU9GLSywqoRcg3oGUYwN2ydsdycwmUiK",
"1EJEww3moVCNeDg9YdkKbZ3sTF4feAQVRn",
"1Fi6nqBVZizpWycceB5V8eZL3wpcFSDkNY",
"1BDEqamRD4DB3MJP4fXbk4FTWBuy7ghvBJ",
"19BdSiANVSKCFL6g9ShPpaPbRusDPhSXcb",
"1MQSvnVyuULwwDm6yRZbrmCAQiFFGeYRXw",
"1GpCzNEd5evCPSSHxTJpFX14XYHduTQUME",
"1AYxA1MpVDzKfcZCMeiqtvnQbfZNWMBjT6",
"1EGdHomcw9qQ47M9QWqhujUzGAgQigXGDf",
"166nbNYy2yYVFSCHt892KaatvCHKzdQ1vj",
"1M19s2Nv2h5svxHE9RBMmhCrwq6BWFqNMw",
"15rSX32nPtRpwrH4TAsZdco7PjQAYyDJiD",
"16q6wAUeRSpFU6h7tPR1aozgv6gYiXimsm",
"1CNF5YRanDjxWF2CxReV6rbQDBQSnh8BPL",
"1PcxDS7i1qwuYJVLLz5PdN2JkgAv3XgC7f",
"13cCt7J1SxSYJ8WaRgKv293QvqKDoepRuw",
"17w7Ri6eGWGDompREzp1KJPFwsTK7T4Y1f",
"19YAqCfVWv5jBbnbp1Vs2HB79FexQAtfoU",
"1744gQh9NkqNR3ajWMpBR5CEA2G7bT1ntD",
"13uWmZAYvpG2x57xNL9hanpbc7rnMsQzUd",
"1MBUcp7XQuuKCm5zYkxW3nJWxKxFh1LKF3",
"1KpcgCjshEcdgoiLvqGMhyPNbVrHApBz4J",
"1LHbCx91CUHspaMDKo9mWPy62HNvJx3JFH",
"19WrhmYQZdnmG4yXnwmtFxKyt1H29G2oEz",
"1E3vXwz9Sx7tJhz9VN6rqZ3tYQ9TifLUUZ",
"1E6Sn8XZJFkBdAXrfMJApszap9sg29THNn",
"113uRJxrsG7TeySukkTebbCpmi4iuheSC",
"1BVEq9cTZc8eFXvsMrJLRxkqCNFApDirYD",
"1CYYfhxyg3i8rzUbhYeyPKsEdhe8xkf2eo",
"16LBo3S58gqGjfQy7vUKKDSVKC52FE87Nj",
"1NLuVzwCkJrPhR9oo75Yu4Pua43kuDjiQ3",
"1EVpCAwwafQayRL7ZS7FehXKBdqYwPvEGy",
"12DmgViL3D41CwJoNHZHyAk33niXwz2zyi",
"16TSL6MJ8e3gwPNdXsZdJn2Zoy3zw35y8h",
"1CZ7i1xtnJ7WW7cv2RN48AHwJmM9XMTJEQ",
"19gdPowuBKQtyY4rsBf9Dr8hnw5XWT9ToQ",
"19BsjUZQ8uDfa9eQKjLDRpKLpmiRJ9Rx4h",
"12Rvermnet8z2Wk2ttaYDzPyPagrsBPPiQ",
"19DAJjUyPfppoPDSAfe6Wfd95fLgMH3Ugu",
"13eUn4Hrn2urRUHW5QoRGbWMNf1V1bXJDJ",
"1Fw6CT5F4Z6HSRCXah1fh2DXrahTh3rCLZ",
"1K4cYc4YBLcv1FCJPVBjkTSjdaDvdmfpTf",
"1JCit2cNGKyPrt9J5Fwt5h1MXmLg6TkXGg",
"16X9R3imKXKvBrAbsu8zkgzh2ZFz1PEVwB",
"1J1pqo1KHMhyeA4KEaketqay379WTxf5eD",
"1EoaPs4Mdpa29mqkTaqG36tFUATAEeuifV",
"14nWNVMrNoYCHzeZuoCcE4ZmNwBRg4zAdB",
"1N92YX4wmR5W4PzEQZxYk38vgnD9bnUXt5",
"15x8gzJnRTHvbAZhw52QVsGY2h5vtnFfJ1",
"1HoFysSTK9TE8q7JWyaERVaPCvQGyMaNTv",
"16pc7kTR6AVj5XnUkmjiMcYzdoMAnjB5Zt",
"1LxjCsKy3D4RkDjJKQCCgj6Hzo6bM2jt3x",
"13RaVT6bAeExuD2edNFdhm4HvtqM6y7eBT",
"1B5rzsQeYHFiFwn2FxrygUYgtKuF415BhN",
"1D9mE7bWMruG5Z7c1QBvsk8KXr5Htq8QXm",
"1EXhnCxturdXMDnsyXZeU16FYUKicSbQh6",
"1AgjJMCCU6nMWXnqmSGLw2ceYY3shjfwnB",
"1HFcPErrSALsQYkKU1QbrnwRTjQSAvYyBE",
"14JVDDFpQYfYdzn9viX8ExsgWpgmEBTbdB",
"1HLV6WgGtunAzi7MiGcXZ57sZQ1LPzb9Zv",
"1FP7nHJAtXnNjjYjgvACRfqy7ur2E9Ah6M",
"181ftWRL6cNtPLeVDwPD6QmLNXuSCiXdF6",
"14s7Jkk5BYpuhhr6DxxZ4dEE3tDofhWUKf",
"1BcpcEDeHQoY7dALFPvNKrAr9VSza3qLmU",
"18GaScj5sQkXC4T33gH4NUoQJLWutSdnxf",
"19wnRnD8YuGrDwKmCGEQdU4w81Zs8aAjjE",
"12JTDiveGf7m6M8qs2iKdN6Zte3mei1Asm",
"19HjhwiWUfPNtFXosPPyeoNbiUoYcYJ1kv",
"19yEHS6uC4zG6aQJ1uW1T3QZZAAW7LonaR",
"13f1L8UcrVJ8rtsW9WW1atFU4MoMP8v4T2",
"1KEfsdCHLV199d251gEfqi7eXY8JoaTSGu",
"1N81oK2F8DQJoafrwetfy29XqpuGZMz1VS",
"1BuxQUS6vLC6Ac3uTMNrTozXpUdFNXyaCs",
"1H43pSfD1yccCmE8aub5wFJ7LE35aff5R8",
"1BdzVRxFSpqc5rAMxpBGc8yZcCi6KzFbMD",
"1G4YVMnuNyU5HFrqMYw4uHMjYrCYugcujy",
"1PeQLSUkc6STdjTNDcR98T8zNCnK7bGDW7",
"1uP3kngUyNmvRtTBPnLGTPZCVjJs6FoQZ",
"14Pufx6QtT3zHsMn4mTi6nPAyNCZb3d7Dd",
"122v4kKnfXS3D6V79EQUYcAf2qAJgcXSiK",
"1Fv7PLVTzMtARTP5SsZf6D8VuGnFLSeHV",
"1LXSrLUzBidjjJzhQQ3sVXiLYWYteQbY4i",
"1H4ZmAoHJada6TJ9A6MPE4zGnPcnuVKkna",
"19nFY7PGTutTdsZgE4UmMdrotqFSSx9yze",
"1GgpVJMiiXnfPrWwEEDrXzwbT6hrmv6UvT",
"15h6uvGyTpqfLArBcvR6WVtWxtcFwG6MtR",
"14ynBAUHVE2z7EEYZaU5X6DV42BuWn5i7e",
"164DejPnPiJ6e6uwcgMTtzEjU9Q9ZwAXFX",
"193AFGqWy3JFeiistZuVnkYiiUVVDdpKTM",
"18Br53UVUdh4dm9sZB9n2ZhYhnnvqYipRd",
"1Gr8MLNkfwQxQAXd2DuacpVMLs5a2GGr6Z",
"1De3NWHnrmpM6RGJ7rD7bJWj1SRqx6BwAY",
"1Cht6dPGjZdFETiRWjeMDd5fUs4kVyJ4Cd",
"1MXQzHuZagXrttjjMN7Z35u6rfVoNWWEvZ",
"1EvnTJUpDus4SVeHB2EGVFmbCYmpmQwm3m",
"13cXMzWuwNEugTqAt6xdpTSfUmbsPGsTe7",
"17hsN7NRd3dRgeHTy8BDRuaM8NyrEpU57o",
"1NFyu3TxfmUsffxEmxjQh4Ei1ch8afZSjm",
"1MQZ8U2cQJWsk8zt1BGDXXegrrvaKYFcaB",
"14R8TiRdsKeT6TetZHZ4kWfABscztVhHFj",
"1B9qzq6dg9JdRFL8FZMdWXzji8GiNUdftB",
"14TrfrPEuBrLjpZGZwuxXvJan4W1tE3Prx",
"12LnFirKz2DMWNreze5nXp7aYRsXzcLQQ1",
"1Baj6nhtTTRFLBXtLkkoDR3xujVxJauXaY",
"1FGGaCbRf8K1xWghC6L4o4MgnxtbFv4Yhx",
"17xkk2G6cwCPSHxnzMGGmYeVCzMKrLSZxP",
"15v7sa2d34FSVE6oQ6tZ3dt7ju5Mhhn1pf",
"194wsyGVogcnZ2vP2nQoFUbkGieL6jycVP",
"1KeKtzA1anAAyTSrNhJepjtAKqCjVUddd9",
"13FCnTxryisjyWP78JCFrQmv9xQKm2Dwmh",
"1PtxXRsCzxJsxKk33XLduaDceHiuxxpMpV",
"1HMMyibBj6N6M1YVqnbqduFd13EiRCH9ak",
"1DwmDpXiP8t3D4narxmnkhNMzddHobBDH4",
"1FTbKYPT3PFNuZqzcZasBVn2oCHAMQtygC",
"1FJLqrcDk4wVtG6ZKtnAzCjFrZ2iddFr4i",
"1PP6miH3r8JV1CTW8PJcghGXsRDgNaz8q6",
"1Lfj9733arWKcky8Xs4fhLRE5EgLHSkuxy",
"1QCLdE8AmzQEyQgo1QNJChpfFynbVLpYxq",
"18a3CeRzePbbLcfGxswj4MJLRaHqtKtBxb",
"1PuajrCaGkMJamVfcNfdbar15Vv4gfQKQk",
"1D6KXbYxkcDrigqmoFiHyCkXvY4aUA2qns",
"16M7QqU4sTCYstxEcz2jC48dRF3Uko2Kz1",
"1DLUqm21n98Jd1SVubxg2DhLpmE7yGU3yL",
"1NW6LSKmuysshjTdsHVMG5ie7WNhb7mPrW",
"169UhJ713ViCnke6oVb74armC275nx8uNT",
"16ufhcdNF6gGjknuCokjfHVrEB3mN7UPFg",
"1Bq8fVgTMRYTaW1m6mWWzoNGXG16m7g5MZ",
"1NJem64gWAkRhU6kqjhNp7c1dugxNN45Xk",
"19ea7m48BHaUoiY8bJ6Umg6Te6y9HTzeo9",
"1EbqiVE3MzNNNHn43iNHwHhRURPtvM7rtL",
"1BCwVgdxDhSZZwgPKRZ43augfAPSpP7jz8",
"15fUHJahjwTEnXnxjmDAmzciRUF3KDoRW2",
"1MPtM2ZMa9ZwRJshgZH229TmykGdekaJQG",
"12DqsTtP1LsAB28jj9t2SpgD6APTPKEbF1",
"11HbLhPsm6p8Vi49mJ92xFnQTNYSoq6in",
"139NWHr7gU5SZ7Rct1Z3eQbt9M8FVbDwpb",
"1MnjTwJcgGGxMby9abZZeLdNRcA8WW4Qr7",
"15JbndRyoQLGshtfVtA3cNEuVBLwYJ3fQv",
"1EYYSNBTwubMShAa5hMwx2jMtxxfSWDj8n",
"1GrTzMz6K2jCpeDwxwGLBnBSCnizDC9U5t",
"15fJAKVvFPTKruaPZTUwkXh819ECd3GA8i",
"161bV77VHjaA7fVACPYkj345DCFVoBDR7Q",
"17EvqdZBVDGhG4Bw5WvXRjLSVrw1cJ8BRZ",
"1Aib8vb7aWbnF48FgjPnDMotRDLHaK6RtC",
"1N751cUBjqLcs2S6ub9P98bcTDLGhL79Ly",
"1KNz3ZgcHrizUa9SSiXKwCSJvureZomcfo",
"19FUPZhwgjmCipeoMKqHT4z2FdzyZHzNzu",
"13Jt5mAQT8PVH1h8XAPnzEFSbV1HrZnz6J",
"1FWXuAVso4bhGbkUcbDtCjEKUsjswnbWhS",
"1MRxnB6EYoGNzCX3h6WcMVqiTHDFR77uud",
"1NVYRgyfcjgfwKz5eiMppGmGsEgQ6ybt6o",
"1AW8tYPbEez67rLoU2Pkra5HswP6qBDTyM",
"1PmGoTZQZ75TonBsRytsAt4TsSeioeML7n",
"184uYBAgm6GLuAaiT6XWhWoL67oAEexRHJ",
"1HZcfuZwT6K6U1Xp9oDQQFkYaKM7AVh83e",
"1JSovMUreDFj3EuedEyrHjVWiqypAGkpQM",
"1GD6yMEMzQNXoycHfMazk2U3EissZMBsEe",
"1CJFGyCtxcyc77XwAqRKEkQPtddqeJ79YC",
"1KhkgubnKPiNPAp4de4iJT7ic8EHCUnHfb",
"1KAapKkocDSxZ2vCEFdGWhWEoyPUzj1FYA",
"1QBA8QYTda15nYNieBKsNvyrLkTBKBb1T9",
"1GTTgqEXvdweQS29LBDKmVTvUP89WpqF35",
"1K4GrZ7Nn1LhyiNn9MS2J9m2iUwuguxxPM",
"15QMeDoeDDh37PxAmmVqzjAJUxRnM2T9Hk",
"1EUiHXwjCturJtm6KafQNWz6sA7WbHFhsi",
"1F4GMbYzJViASzqFMo7pAYzmt7KriH58Q5",
"1MAb1SwF15eXvbq91UjnkAD9vrkdvNsMpE",
"13RBMytJDYveh2reCaJp2sCHLCAKjuEbtG",
"15DNEziESpXwSdcVjWFJbHEdyF68z6u7rR",
"1C78gGLDm3jGy1sQGWkxtvfFsbCsFv8Xsf",
"18ur1LypoLoFxRySjgM7ps1WrqJ8LTNKaj",
"1G1vthosqw1wp9d55ujdUPkYC5f6xGxUPf",
"17SMPmkNdu91npVqdhHHB5dDTLcyRJxiME",
"1729rTafyRhx29yty92LL17yhu1ZoUCqCu",
"14L3JAN9rs4w6dqpxJfFtgMwYM4RQSZ3px",
"12zRVxH9bSt8Vs4f7LeQbkWwg8D5ns8Ax4",
"1AAsJtzehwzV7jVkjWBgf7A1wvW7KMMvSG",
"1E5yarbjnN1UrtuNdFLGYjyfp9AomdtG31",
"1C739ymDp9fTdSU4hkgVLqWcEjiVGAx8w4",
"1Dq8DC8JR1m41p1LknimiS3QEM62JqZCbV",
"1Bo7kQh9yscV5Uvvi2AHCqY4imwXWFXYGd",
"1EaxXTy6vybxbPAUiqQJ9Hq7SzhB1QqTAE",
"18PqmHjMTobECtJBkCQ5je4fVSTeR39acD",
"12QR5ML47CJPsT4ChAC7txn4CbgmGPs3ve",
"18qawXnurGAvbi2XXm9ZABrRPYmgQNhnQf",
"1BwJBNKBhzTu49Tz5a68C3KtQ9rEgmshP8",
"17VWqNR8KiatPK5sAxDKmweb79Be4HpFwu",
"1EW6bP6q3tgQ2E89PDBG1AAnytB6GiY1gi",
"13simQk9HR94caqk6A9iJA6XxywXEqSyqf",
"17hVBETLMnhS3oJ29hPVibajzPBZov2BEY",
"18JT24CN9JsfrLs8M1vPDFFGeV4s4WvuTe",
"14H1gxB3EL3hXJ645e6yrY7844ResdKiTw",
"1AkpDvM23Ywzn1AzY72AKDQEiLQfzxuVrw",
"1JbxuNAKZbYtpsn1MZ7Eh4Cp6HjpX7nZLj",
"1ESqNoAXwVNJyeyMnaiN3HzSBi5ctaPywE",
"15aAh1RfUDx7QU3SS3W2dYZZTFzeXv5QoR",
"1DJMeoFa4J9Prnwk4GxkV7VgWXNdjh6UVi",
"18i1T3f1Eb1A8YPUoCPopaZ8NHb2VbC66",
"1E97hTcEqSc3nomihdLKHmK3RS22PjgZMt",
"158K4tCxyouMgtD9a9DUi8HsMqCjBtixVz",
"19Lov4LDwh97B9qpAeCDhFnaRAgt293cbb",
"1Kk4TWawibxaes6Tapovk8M7Uhi4yS85W6",
"1CeRw5Ht5mpWwufDNVr4cxPSfBKH4W3Are",
"1FNRVgNHGm3AWzQVbNanembYLir73qDFGe",
"1ywxfiac5cZrLUFA7oHM4JcJ3BYphFgbL",
"1Lt2xuT67BCExoigoER9TwhH4Tbvou36sJ",
"16Yhb9wvaAnxGk6BA5kxfK9NkLRiS4GchB",
"1FY35LSLyWKQTysWKUZNaarhatV4z1XGJ9",
"1qEwC5cioQ7zp47pPB4VhDFcdXHJZzhUE",
"14iPbpPtrbHPbMJrtdr8JuL23M3cJCPqNk",
"13MeEyyYpqW8dyWGZW2ajAdxsG76jqJrwh",
"1D6ZSKpbXRQ1FHvThshoYvoebczH2D33As",
"16swzsFguAq6B7J6nRt91EsdGeaX71C1kd",
"14qLV6Wqg9BV2x7F5DfTaDV4MqLy4dSWoi",
"15BJTmMQBazGqxZWhaRKVWAczXpKeQKnJQ",
"1E7uXoh4fQ89th65hAovpxF9m1WcD82Y6j",
"1GXUffBAXTqo7D9Cn7oi9PZ8frkb2uCsPn",
"1Fw2Fj8A8qNp465TcRqKc74T6HPTDJkhpf",
"17PGwtmwyTGeedeNp1hiQBXstPgVmMgxbS",
"1Buy8cur2FivxDuLesqC7VWQ2ymDttDPB",
"1CBnX5igsAcmbTU96kAmrtJe3hjDL4mvv",
"1KJbxBKNTsDkZHQ7UCTqjZw6hYaXSVfHKm",
"15mRckign9p2Jkm7H4c9suFZRS3uR5zbda",
"1CCVi9SKLKXfsP9GWKU4WyPrwoh2sUz3LF",
"1G4g6x9fxKkeJcEMpPE5oy2ovm9shH2m6s",
"1BAEe7Gs7BkG5xu5rKTQT1LxU8nxEDWcum",
"19vHoLm8w4W8NXgAYSYBnjYpbJNFzJwiX2",
"16J1UndKJduRvMeCFiqLLovogzcsnQs7U6",
"1MfEjDtwBCyHWtUAn6ry4MYA92L4FEmDhB",
"1GRf1V3vYM2xQaQGyZZF77JVC2xxkHTCfk",
"18xJFaDUvvC3XCUX474ozgaSqBZF75bAKi",
"1GRXfaHF1EjZh7HKsPDwKLsirxyogbynsc",
"1sk3du5u1ap2jzrCgBaN5wPksjoY2K46s",
"18SKRp7v6nB1oSW8G7Vcgxfoum3cYpqGRU",
"1LimYvxBFHS8jCiSb96PVWj1x83JLuqVZG",
"15EG15ZaG7ihEV6MKXe7vALYrJ6kSzQtMf",
"1JiCBv5jsox3HtAecV3fu58VUyB6c3c4Y1",
"126mtG8V7Xq8Bv5A3ooSYVHBngZJcq5hwv",
"19UwczBKJ34ZnrAkdPckA96JPWxHUBXZ6D",
"1KKVH8aYs77b1hWghMuENMLH8yB9mHKMCB",
"1A89uQfzjt82PuR42h2zQ22GjpyStpFRL8",
"1PzzKk9CLdeRSZU2NRsR2FCpMPXRzSZ9H",
"1KW2sRLWCgKxEXKsj59EQGWVzxnkGCFRkf",
"1JFUkSrjNPuNpjPGDbPe8LkJgtJYRevck1",
"1BHxMeDeWTGCtaRuSQ3TML36QHmf1iyc5p",
"1ATZNJq2UZk9gcT3fLKqcuagaP2553qGpy",
"14smw45rqxkK6FmEZHRoxgpnyZqZm3ZobV",
"1Auu2JfjewBjywYBrwY6enSN93fMxAyp7h",
"16pnuuBcnfgG6FRBT8E21agdUNXs9mgDm1",
"1Cvkh7JK3qCAfV6qNvr9WYRmyme6p7sayP",
"187sAEwQUvojKFcSfbzsenYUFwXWcTN8kg",
"13zBw3orks57HDqu2t1jZiFQdZk3C5B3Le",
"1G1aQRvWyQRE3vfrdarkoir3rrovH8RfwK",
"18176bTVfYTMMAq6vDqYDC1T4Bnhut69Wq",
"1ERTMtaDhgrhfVZ5hhRPSNbk456sR1QhJ4",
"1D2sLX7q2FDoxWcsHq7dvkxTmgno7ZthQ3",
"1Poe3FAwm5MMdqVNhZgTbSwARkjSexpeEq",
"15QkjiCF27qHEBHqPexj5agTYYPz8w8PuQ",
"1DSBGEKTGqd89t4djNWPhgXCSRtYi9MTn8",
"1Q2VVn1KoWbaEznZVcd4D8qY1URLUVmPVq",
"12C5s1Hnm2qJXJdi2GVWZYap6FJuL2aU7m",
"16ykLYCBqh2MK5G9cCetPpegegHTNJZfpb",
"1Q1N3qoBLccQQQmXK2t1gNsafwar1QKkYo",
"15duR8Tv5Zy2GiS93pCuXQ9katX4gf8iWX",
"1QEeUScxFCzDKmgLeppMz7WWH2H49SWcK4",
"1A2pHX8qUz5Y3T33jYn3dRLiyEPZ8tCU1H",
"13GwDFUaFmnwrQ9K9MXDHEzt2ameuskNDP",
"19v2C2LnBZd8i7yKbTHsZUq32ovkZUHE8t",
"1Efc8cmct7fRU3TpGzGSCHsCYTmMYCVVwt",
"1AYvHWujK2L3R5vA19WUgvsCmy1GkQfdr9",
"19CHAiA4cC49ZNSLUpzccMDPJQffhp5uua",
"193Ng3aRZWZR2XJa1irPFPD8g9d6cvJXmw",
"1Pe1V3KHuB5sWG8eJJratAvEDDPySy5QCV",
"1oQGdrjPcw6k2qWdpNz9sZfuAFFDraXYo",
"1AZHcHWqvcYzkZhfDF8hJj95BsFHbT6tXe",
"1GnVszEPczBnpe9RgSrD4AmL8E3kscWHp9",
"1MtPwWLYfAtW9wcRveRpaCBpEPnmSgem5R",
"1KUnHQtrgLbC8vX69ffdB6xiwggBLM9m8a",
"1N4sgKETBfnnqbbATY9F7ZEnCiF8DW5Hkb",
"1HjnKN9dPpdjLcBxy9jdDtb4BdRhNU2Qm9",
"1PaLCqURiMJybYrConTVnryB5tXmEZs4ys",
"18JdKxKaWKrZ5Gf2ih1VCPCPT8eYN3Qj8h",
"13SctvcMN4YDqWs9UN87sFohNAVQYVVtYJ",
"1Dbm4DoTNKL3CYCosCr6JuJkUZ9iPvNE4t",
"17YMgKrvWAMPk4bMuUnEZcwjP2hW9QNPx4",
"1FbvSvZoXL5MkhfQGEbE55o8GPchksd9oS",
"1HwCEQpgKnDyfxaaMsxkgQerrrseytXQFe",
"1HoSdVWnHETFerJMkDEPPhXzh2QTYe2XAc",
"1AnqY7LjeNWPskS3MV7CrfKTLdN2kkkLZ2",
"1HF7eFpbhPxV6VYRBrCvH31BjZWudLqran",
"1QLRVdNgVrvjfhunJ3vbQ1h8Ahg49J485h",
"19qnVypDCwwjsTLBKZturYwWHyU3SU2PdQ",
"15pndMctrbrTFfsV6yRmrxjL735mUQCvt9",
"1HBd3fcHECGs8iyski9yVRN9qy3m82kD8L",
"1N2RF1g8kAxPdt7petEDeKy39ZmxbMZBQz",
"1MkeRZJDYc8yqgh5ASjx1jCHo2aRAab8pQ",
"1NfKtxgdds4bgh4VHYtzLo4UWjwDvvyvAf",
"1NiAeFSjJYJqDHcpaE86HPrhx5K3oW4C6k",
"1GnoPmUKHSR9sRaBxQ4EoLiRLbpFqi87Fc",
"1GZZyWiYpjaWviB6GZxVbWEsG4ZGejMWXz",
"1FjXYC1Ma1LGV1CkRzbvERNccvZYRi7qr2",
"1959tp7n2Wo2HgDwHjQU2sQzyCU38pPhY2",
"16veeRyS7pL6NG71hTk3SB61dBcgA6SpEc",
"1ktvYx2KfidmTgPmm2Tr8FLxoQFxnGMmc",
"1DtP91kAHaXhfgkrmb1vm65ctpPSdxgm7Q",
"1A7EGSJDwSpsUwAMtofcpnS18A2WrDYtu8",
"18VH5T8mAgp7pQjVr6fTaiKG6o2gs5y3sa",
"1BhPkksLHUKfuDMe8dqxecsatN3QWBYwyY",
"1G6RemFdHGvLWc7dChPNcjs6xcZgyDgaWY",
"1Pk6vzZcbwiqXeVbPcodXvhkQpzng3AQ2g",
"1FNbgH7c3bPhBz9r1LqLXR6wbbZ9DpkcTw",
"18mNjb1VsHY4iFhZRNfsv7itW7mkPvWMX6",
"19qpPATRX3ZuZmZ4g6Joon8ZtEg8zKL8Dj",
"1Ns68i5HE4ozPHbwRTTtMu6JQTfGZUbrEK",
"1PwCwxyL7DTQpKtuafaUvmc6JwES4FxnaV",
"1DmhYT3J63QxxAqigRFahP5eCFaKg3XUhv",
"12DusEHVMLtYneKDM3pogZzznsX4fTvQ7i",
"1NZ5vrERzsqtHsLdTLArKNk52b3vibWzsu",
"1GfGAagcySKv42tcCR3UoymnA9nFRcmfDo",
"1JNzmMttXwdZYC4C8gRpYjo14DeG3VRq5B",
"1MJDMiDPY2E5HKFomRSguE5KKBRUVHQ7ia",
"12MJ25FkhJnKSxC5Cm1YPKHFTgSVKbfKTh",
"1C7mucdJeMeQnSAJNe21H3s6dEZTYNr3Vo",
"1J7fqUj66dd55YBWffr6eHTwveKCqoGk7K",
"16YFwoVciHH6Jp54VPkAhbVt3Y6Ke1rV19",
"12TbNfhQZtsXW8sz4j9SGVCuQtQXnTY6cE",
"1AH4uXh2PgcZHUA1VfYQkQt6D2fRCZZmPT",
"1KNJFsY3m77VArsuXokE7pprswSDJjo8rW",
"19dwsWZ4eZn5rTRVgZv76KU2xWpAggAkw7",
"1NQLGK3KY2i9hJmrwFTAZddc1ud985j8Zm",
"1BbGc191M84XYwdkdrfJnsMQypd64unTSL",
"18rmT64QyPfnYEXZ3ASMm33hqzKEfwa4oS",
"1DWiRpDny39vqWioXCx1j6vDEyKiJeLHRW",
"14gYznFUnMhJ8ZnmUaYKbqy5QaooSTCVVq",
"18agB9bRSCW9tiZAdSVBX4j3TRhFoRxT8C",
"1Ci1KU1eb6rSkYsGcD37G5wjVBXqXwyHeY",
"153pcKxPWqhwfBdCm5a33vsAN9Zuxt569G",
"17evAaQA6bkE1n5F5X3gwnXWkJ141cpwrG",
"14ZZC3FWFscd8fLRcLPzJS1QtwsYUQK61F",
"1LdpXMYxgXvw3HpW4JZgmkDAumHFG3Mv79",
"1Ao2YugcVuov4AMejWkgCu5JN49iEn4zbu",
"19uc469zZxEKVjAXhQR7mK2ZirQqWrSF6f",
"1JQLhqhUj3djzAgxiucbkrJP3oQNsbXnFY",
"1EcEzHVk8eQTqbLFNknbTASdS1fidSWZsY",
"1QBxfLj8CTyc1PdT76gHWZjhhe6wGqdivX",
"1aMtDrqih9Exz4ksC3kYKbwXVJFHpEYUA",
"1KioSLEtdkESDXPkZp3AofLJ62RhLS3LFo",
"19YKLM5tJbVkFWFMiXNEbjYaFQMrwzm3kf",
"1PMoaUmv4vV6iJVxi5XqJbM43JJrDiHS5H",
"13jpJhKo7e16su91dYSMcYaKAK9eaPpi1J",
"16rMC722F39obsLNiPdmdUQujyG7TNrAuh",
"1EF26NTb4tgVHZyVLDXgnz29EvaWWEzd7i",
"16SYuUxRGPEoxM7pfSmSjPnP6T1YYW1FSp",
"16MvPSBPneqkFgeJSD3yJnzHVfpZcSa7fS",
"12kHNwZLwiypLGqdhwB9XhBChZk629DS9x",
"1BFiSkxh8MSdoW28wxUAjhAQDgvBHGyxDP",
"176ccMhQjUQU41PaqWywecTvfXNcsyj6qo",
"16oDeJ1tAXUniroDeTbAa47hdGYNMdQg55",
"1A3tKd6cdHNhxM1icDBUKTqGjzLtFFKazY",
"1HFpFDHnGEwBMTQfDK3xUbXEJ9HGPqZBpo",
"1Fja6LYB26Zh9xQi7eVdHXXj1AYzi4og6M",
"1CV9NdBDTjWcdoZ2PZkzSTHTJddMxorq7",
"1HT6yNvXQ1TcSJMpDjJC3xGyMa1aHg64i5",
"1DKigppHuDvrFYh5H8jBFjbwFtcKRhytc6",
"1GH183eoAJc2tGYJBs6vpJvWjm5Ro5mwbM",
"1AG3KyCFL68qLWXW9AvE3sYzLBptF1DS4a",
"1PgvFsh3an7zNF7uHF8wcum9qpmTSA3Laz",
"1HetxA1uk24o4UhXjDJyhtAX5n8CMdsNoY",
"1D25AV2kTPKvoNTYD6EVL3W9sSKbJFjRyQ",
"13G8kL9fodubpscLxgtsgqSkSC3ScaJGrp",
"1MJ8DQxA1y6TD6QoGMdddHFaf7RfCoUBoN",
"1AzdrvozSHucwEaGAVhWQYQEqaNKaXVJtt",
"1HYTFvgRmyVtCqigPmomrHP8oyCvBbTHDa",
"19EZQdrdj2RmjrptdJX3tDsa7tLVtLjvtj",
"1NDsy7x8h1a1BC93ubYR7g3Nmm6ei73irJ",
"19ycHKoKJ4uT9Bxecp2pyzUsubULzhDGno",
"1NT4jr7A5BU2R9T8hqr1G2h8avjRXMASmr",
"1JXGWByiBLbm7wM6efZLkn4Ubwi1XXo4ms",
"1AsnhmdpvYgt4mqpm6D4CMAmw7qs5DbRkD",
"163mdYKqbwWSWUjLxzaPZbqPj6XZ5j8BrE",
"1PbvPqCzLi3ANxNXD5oKzM6Eo5cYQggwmP",
"1PFXE5mDqHmkJCyxA6nEjYkZ5qQvpj6LXd",
"15XFpiPhit76Z2aBMSTbSkTQzL5iXKBG7o",
"19G2pN1D1Et9ktrUcjvmjx35JNm7zUryAN",
"1pEh6uJiMQeskBXsgMbDwPDERQyntBPjc",
"18AGWMhwkdBQPVUXcgayfqv67J2RvtkFkq",
"1AedT3ez5XM22quudTb7UoRaJnwzYvKZzB",
"1MdkzKbANAVjPx2VuBGTeXHyrUiCsJ8RFa",
"19vbDPZQTKvr1Weti4FHd6QZUDMqH8c8o6",
"1EuB9DLKiyo3wtCKEWoJm4XFKCi48p8eef",
"1F9oqzscBbwwQNhWxZ5SdDVKyAxNSCNqxh",
"12QHCgBHDQ5j4S4j1MqDUDic14q9jt9wUx",
"1PS7m8aYK9cTzHPjaWwAWN6sxRx7GbrG3A",
"1MRLHQjsZoc7BANYiCa2zXJitEubyKauTn",
"1E1ANCA2swEjQaXTBTeuVRpUttx6MBqNsP",
"1LSjwdxmbQTBC1s4JLx5ZqNXGcy2N2LaA2",
"1HNyDLATCzvLk2AsBqbtto8MJCQ5JE423y",
"13w4SZCXn36k1UfKZi4XMMVzAAY2FwsXvA",
"182TgX9fugdFLjTeri9sQ4qPU77za7TNJJ",
"1PjYBCFNgyvpUxNyznZEV3wmk5aW4xCfyS",
"1949W4LjsMq9FJqYnMLvodS8PbMRaW19uS",
"1Kv1cnXVaUa6z5qFFmCWY8jUUNDSFGTHHG",
"1onCZNsJoHKwcrbNDRXg41zCU2hsDVrCz",
"1zdFY3xhbcsmTT4hjHeBh6Tb21JeDqCKi",
"1La5FKb7NCdFjHygp8bUfYE3zTW7sorSwV",
"1Ha5AuE3k4XRpe1fQWFkkakhhW8jPp3Ufp",
"18KHctbJQbkpKBBN5itjadphBcCJ2Sc6cf",
"15Lz4yvMDPMTvmCnh3FXxUywch57ECX4ST",
"1NxTAn6JAZHVQcXfzUtj3eniXXTBYM6wk",
"1Njj8RTjd5X9EcTe22MryjnEB9WnuG6jMu",
"1K7zm3cjDiC19nUJFcNo7nqNNvMNso31e7",
"13vbrmzS8KfehvKBeYx2FayhDtcyJr1aGy",
"1M5AeUWLhMJMcmeqhyuW3DKehDXtj64Ehq",
"1Dh5woscQ8qKRiC8PkdQTAEvDYGXzXj37z",
"1iJytFPiyymyUDbc5gRHXSYPKAB4JYbUy",
"1CEBpBWJ5yfFJfdLdLUUDSLGQttLMERaA4",
"1GAQwHAfia4rsTTpMqR5FkupQLpgRMe4cK",
"1MxY1TzDvExZ5BXx1YtshNAR59RjA1Zms6",
"18Bd2Bxs9DscwPvrZxxtZBKdC2MXREStwF",
"1LjkRGTD7PPjEi9MNkRpZUBd4X31J7VX9Y",
"1C4Q97xBpxbmsEt3WPyXmmMPKyXj5X7g8d",
"14HECwWNsBWvLJXxTJUizLmdqezKh5gLKu",
"13N4WFTB7R7Ltk7iUxBxWAntBcUwQyFzcr",
"16Z5L3T8Rj3Bh22FCsSfrKR1X65PDTs9hs",
"1Q9cLAvfUXJ9NDNc7CZSwpB5VCZV9ULFm2",
"1AZApiLnTFH9oVBTR6hHy6zqkrZ8x3zxm7",
"1F4CQmikJ2zPXcGGkHzttHDF3FMq5VJVcQ",
"13C2gqH4YBEg9D2ygVn815ySxQp3LRUhiC",
"1JZLE35H4XoiMdU5Gdhfy2dkg5WzoBFZZg",
"17JXvFfMmfFRj9wujapo8AAeVWxyawa6ZA",
"15DePxSzadYdgmvsMRYZTJnhqgmfj96NDM",
"1N1UWnwSjzBzQ1Q56SgrH7S4zhKtWxMMYZ",
"1KpVGcEmczadbvPdAVApkDCbo3nFAn776P",
"1FsHsgx4hPuNEKSLVWsuKd7BwE3gT3zyhq",
"1MfaUBynERoApxEtrFTsk7Y6tnPDY6oMQN",
"19rW3PeM2j6tg3JsTdzsQFyYdiDirHJ2ZT",
"1FTwa7peXc86LrL4wrZevxBpDv4S5a68Am",
"1KV8KZpu65dC88ZLe1uQ5ARB62MLFeNpja",
"1JyTR1hRBpdNsSF7dgrTvSF4fng8bVMctw",
"1PNt5tJinzAy2bYLX7HBTz5TYKnF4Li92o",
"1A6MQwxJ2WWif7R6331i7N5VnPWyGnrkFZ",
"1NMpoMmPw35FaFLJSfv4E6YbY6yEvf52Qm",
"19CrHx1ohAfxPkz9xf8z1hN5VLpcLPhefq",
"1DEMCrno282NtWNgFntHreDxbqmRr3bYbZ",
"1BgDNUnBnmN7biY9Dr7hZhhCfzBZ6bZYXN",
"1H6jhWm4uj8BjQq8TCWGzBhDtpnuxpvWhC",
"18r3jN5RMnfdkT1pTRJrvLhWVM68gnYwXa",
"1DftM5CcfBctMvXuT4jtL9tP6i8dZs1tJj",
"14gRPjq38kximUqTF1Bk144M5yXF5EYzcU",
"1Jp2cFDN4YQXwa9e8bu6ppwRJasuDcfR9v",
"1JXjdmn62dyf15Btx7qg5RQEFepRgcZF5w",
"1EuKV8rVuiUic4qAapzzFZ3tAfTVn2mxz7",
"1MC9mVbyjcxXyApoPmRspFjNujxjBtsdtt",
"1VpMA316q4Esdjx4bLLaDYrZAEs1kDg16",
"1GsmDW6uDxpLi91YiYE7Ykc86VYFNk8kLh",
"1C8cAFEqyG1FspCBWaMWah87hcuASscNx8",
"1Mwded4bvB6Vh6e7cjjETNqsA5KrWxggWB",
"1FBuiqjxjScXEpkaVMKW7KXLnwXBAn6iEk",
"17aZZ7wSa7TEXeR3QDTydbTPdnNRsG3nRp",
"1Gh8Hwt84BuLPP3wxicHUwavgiEMo5o1GA",
"1AHcEALBtrfjkizEm5gXyRdEPKePPUSFXz",
"1KUp2LRBMhUM4QVfsWuLPGKMo8QJqphYLx",
"1BGW3jXxr7bVtmje98kSsQrP4mjbTYDg9k",
"12gvgL3tmnoLfcxxxpA8tZFfZRgjD7fKSe",
"1oeC9doz9wV969m9FyQ7geEBUPrz9YYox",
"1A5EZAij5Lqtxoghu5hJNbz3x68H1wFUJr",
"1NWRGiuMYfizMQHFPevckatpoCJaD2jUeK",
"1MaErUSxkS77e4jKUf9jwnwzdnzJqUFbE2",
"1HfKsLKNZFdN3rDHvAgad2Avv6hpidpMii",
"1KgugFqmGjqD5SubSz6Bk4XJr9caGfWajE",
"1A67BbiRamFSGG2uUcEUdX3U1ouZxNEuLU",
"195mf4z5NT8YKNGXqNhohfhFBvcTdJvHDW",
"16nTngcnXKpn3k61WYcKWbcrGbGBj1mQGH",
"1K17fxGeNSDHyXEsEDiEAzjoftsHU6Lo4t",
"165aZTnXm7DWBxrNLWRimvJUG6M5si97Hb",
"1H6rkBGtCmx3dVmXJRJpP6cqND4GgFDhDw",
"15DtaiHN3SQg3T63BqkEgAB8dfu6sK7Feq",
"13tYBscgkdF7D9e8zwk49sKbUL2KvD7XXa",
"1HZKhULtPTZbqygk6StjgnbgGk8jLRusqb",
"18KVfpMVFguAQn797CmQg9zxyUSpWWzA5x",
"1NiD5F2yfv361g5SmfNxeYrbNhPhwrmDVT",
"1LGow3FRnBvoGqrBsvsghArPNF5koHmMub",
"1KNZ1z1btBWrgRncrcjtWhfrxXH6vsZgXu",
"1DKbfcgc7BoM6mY3bbejAxQpcpPAT1Sz5W",
"13ndwBhVSbKYeYzadRsB14hpJcs8D3TohR",
"13GAn4MRwCru7Q7aiEkbj7NWujzU3BSvgM",
"14VABpZDCECDsvTJQuFAKNe3taT77nTfPV",
"1JZd6egdj9pskSQMqK6ztXv4RvmgX2iRgW",
"1Lhb5j3oszG4F6Lb2jg5G8HbB6ggbeqUtg",
"1JtakyqVd3gN1agDzmht5aynH23kYrs7T4",
"1QEM2mMd1BNYHoDrqGzrYXx8EyoRWyseUV",
"1DDanZL3Uw22Dsq7e8x54Nx95fdj6hqbg3",
"1FVWtxDLxbwkYqkFfrxdUXBQZTtYpH24UZ",
"16bVjtJHzvzrKdQGiN33i8wuU6RQbXBhRY",
"1NqNvoC8A8f2FnfJX9YQAMgPBJBZPQb52n",
"1EQNX9qyGNsa9EwKW9r22faHywL97ywDVQ",
"12cWsYz7G1hYYKWjzjRuNyhxRbwfGwigXv",
"1PjEPephqGucegd3QNk2UJKVkYcQfvZ3L2",
"19HdPpyZPcmxBntBxA45mzbpN8JQw9aeZs",
"1AhNEnE3rcZ1Bwyf9KNZpAXEyyWrTcyDjS",
"13PM6BCNZyjkV5uqqnxBtQbF3SkcpUZCQL",
"1BnxwoNA2qXjUZaSTG9mpFrxKqBSGF5LaP",
"13qBL8zn1SBzuimEzDrCAHyRxBrv8AA558",
"1MLN1MvzGbjVuo537JfWwHuAbgXfWqrCPi",
"1FJcMy8VsCGyckFPszoDUjoogRousd6zTE",
"1QGCxGmHoawucKM4Ve78venRhGBkpZvpJh",
"1K557knEKu96DtkyGrEUSKrrttaDXbz68X",
"19gERKEyjEfpWr4Tix8r2EQjS9cJKbWbTn",
"1GPGBSsJbLN75iEixqD1sztxzS76n43iaU",
"1DckisJpJkKX4C1GSd6RbTTJEDmXkJZkQc",
"1H7EjCi24egTpgxRjpWBevLje6bz6nEiVN",
"1L9PFrEbn2RaHcWCX7cb75n9qm7Pus1aJj",
"18mByvsKzY8ZCjD2d8eJTcNpgjXN2zmY77",
"1EA8Q21Gv7jpoFik12ryF1nnEsVzro9To7",
"1EEVyi8JyYdYCUEkPXE6ETPzLdaHGfSg2D",
"19PrcarCAmcRBziFjk54A8XSxSXDXnYcrP",
"1GHMfvFNR6da14mWn8PxnQzt4kHDnL4xSS",
"18MbX3LoP5HjcEDkCBSymAjPXVgbUdN6Qo",
"1LDuKvy7GWQNYthrHVp9CdJLtGFMUerJRQ",
"19d5XBQBytDzxKbo5WyuzLabLbMF5sUCTJ",
"1Cg6Svm4vywhBfGPXA5pwx1xMzfDHP2PG9",
"1PWHxNrHQZvy5okdmrbreYFZAeBHPV6bYL",
"163gH1cho6fCg3NAuretcPmMu1L4QSiSX8",
"1MYrA1VMwwHdaq9ZWzAKih9dZjpKwT1dRa",
"14RkzyPjAeQjtNn6WWhw37psEFkqsZ7dk1",
"1GV7A4pTdHXg5eupw6UtRkY1iZuqEUpTCf",
"1ETYuuBmQUgjNYd2L4DazjSQT9dKZNkQg5",
"1NoSzDbhC1HGF8bezbXYeGLM3So7ZgBKmM",
"113WL6HLMvuBNBLc3uikWmaU4wfcHWUDU3",
"1DCF1LBtq5dnsN9FxBpwvkQaeDyxyrp9bu",
"1Bsg8m28wngJ43KmR6nDE9ez9NXPoqJncj",
"1NqCWpDrsKs67Pvqh9WMghDGvxzGppqX8J",
"15Qyj3hhbhaFwjkN9vVf4A13YoWKMyK6mU",
"17NPbfnQGzeDFzbwmxrnmrfygtG6M7s2fb",
"126kRcmKR74XSZmut2ueWvSEMFnqfAeSyx",
"16MFBsAuX6xkfXu7myvhNxitZGSv4pTbSt",
"1SxqMd356fx7BGkBDTX6sQk3Farb16xDo",
"1KdTTf93z54t8wtwzsrqgEMfH2DTsvroZc",
"1PAo3TDNdxR4NTjVVTdiGvqG4WQ1N2nW6n",
"1KWr3aibLCGNLbDR3jGV3shgcBazu9LsVa",
"1PtrnGBPHuiyNQbUE5mwF9t5UFr8K7sdAx",
"1HvKuokUwbBtu8eKLmuybTQN3fqCgZ9naf",
"176mU9GNELmukz3iWH64qCSor4YpNoE4aX",
"1DiRF7VDjGqpTHbmXVd4wMhJN7FRAmKbWc",
"14PMjfLjTkWoD9vpDvrm1jFN35A4aSdxCu",
"1B9cyeEQFpBZeTSRATTekN92wLAy1mnkVk",
"1FZ9m5NEUCNGnifLwPBc1YjB6jfqqevFdD",
"1Mkz71SR844ig2HnQKdgsnWUg59RSiQXG1",
"1NBqCsSqJtc8kn68VrQ7cseLaiLhYKxF2n",
"1EzMmoduYUGTYruUtFmphoEkaQqocyxLRt",
"1nrzzQiiz57kpFmytVsBCD5TiRSJgFzZY",
"16pXVbKPDLKy8w63onuPX3EjeLgx64QqZu",
"1r7SekfJTVNLgeWX76CegYFDFi5nyhqJ7",
"14BXLyTPiuVbwTRppCSJnuV6gBzzvL7Ut8",
"1DJsPY7KWseshvzBEvhKyJLBhux8nhQVUe",
"12h8ko3q4Ec8uFmjUER9NYPmAMkFkNLrrr",
"1Nufrc3RfMkAYbPbRm11Kq7mp9pAjSBYXa",
"1gQMnd7RyA3xivnYTFg3xHB6UxxkHEcuy",
"18urnX7DBfBBK4RzRAsgUKxAhinrnSnw7R",
"1M8s9kf6rNvDvr6xHmXvw6146Qcyr3qrXF",
"1JfhNLobKVTLtLv41iiWxCzAhsMDfKAUm1",
"121h8yBxMQi7aTXELeEnF8NwUxaGbMvDfa",
"1Fc6kyfS4sgBuGdJuBsERrYBz3h1kD6ueg",
"1GJEoebr2emoNQF7TeEJcPuCicdW3LGUoV",
"1CKBpHzCqd8bAJbPVbtBkZcCaMi5svPXzN",
"16xZHXPge9ibchW1KcE12KJDwsQUStWUj3",
"1PoSx7GncqmapuJvJNA3Ut1UQD4EkxqeFs",
"153YG5wuVECDm8ZgZDu2PBuXu5c6kna83k",
"1LgtW3yp3ghr1scWgQW8xAQDKGmzsHu7b8",
"1BnQWrkd5hgHkDyZasxzb33simvpW1XHQq",
"15srmdxdU4iE8kxRHbGyv6x5eVGuyWkK89",
"1Bdw5HdLrCYRYp8FRTHu9NTwBcM4Q6iaS1",
"1LsU9fbMfW7cSJQa1GBvumUBfHRnrhYexw",
"141nmtmteLWatKt2c8hnMiHNBtsxFyT3tE",
"1LLZv5hZvdKJ8vhX8ggb41G7aCTA5qJJC8",
"1HaHu4WNm3xr87eZdAEPD1so5eJDKn8FSu",
"1NGUGjkJ4TfMnUS5YJ2e6aFPSxMsXELsMZ",
"17uL9nCPWiyTrCZoxwkCgWt9MzhG7fv1Nm",
"1E9ssr5s7mWcoVYLdUEATv22Fr1DGCNVqr",
"19f32yrxdVF2N2Azz8vc7U6n7FDEXeaT2o",
"15EdBdiiUQUJWUXphMhKTEHEu6jnsRqjYX",
"1JpuKkvpmyRbAcTAEPLNTWmMT1uTfXPzfA",
"1MgXeNCi82agwscZKcop7vihRj21x6Gb2x",
"1P5ajDHrfgwdQ3QToi2y5WPYC4E4K6EsT7",
"1HT1kNPMnWJ6MWBgPDknBoDAM9Lcoyi5Kd",
"12JnKskvDL94jXLLVqWdEjqBX21yZRm5b4",
"15sHADgFKmEyvPuNRg2V1Kp4ynnbRN9wgc",
"12DkXvDvetZwQ9wi2i5VpJy9vk6EC3z6Yo",
"17FmdZX7M3psULjBwuSBecvyj25jn7Yaer",
"1F2pa3njukQfnS8m4A14fV7Y59UkdDoDBd",
"1BLgsuShE5r1hBSoCME76yEZfPBTvHcixS",
"12Y7Mauy4busDniB8Hckrqt8MLkWX9yzmi",
"1LF3PSGQXMHWmrLpHWzgJ1QKRjsGHqMAHf",
"1LdyuzgHprpeg1LTbsmtST9am92W5Sxaxt",
"1FdHGXW4UbctuAUCskhp4b5oRaFYHDwEEq",
"19z8GJcJaHwvBaKbsAkT474WdcvZ7vTAQA",
"1A8BRYCosvYx4WuxRzc96m41FVzajMEnvT",
"1FgnVGWxPJzWD6keoh47wfJV1mecx7RUX",
"12ESACo48WsxGeMyVfRiarNKwzCZbNBukR",
"1G3irg8z8kvhZr3S84mTr7nuzZqLqS7baD",
"1JHFbYgYxcVGhmY7RdjTDYpEmC8BZ519Q2",
"146wZLAhmAYfQdcSZ38AexCFeNkcJ1ruBx",
"1BKpSEtdNHRMhQcBAJuX2Ssn6nfmbKGP8g",
"15xa3fox1TwyZZTePGAce85RBcNVr6f5fu",
"1HnetBcPjtnjDXj41k4ro9XKNqeKzq7c7T",
"1Lmtp4YcV1Ha6M8RtjWdKSA576YDBgUnjK",
"1Ja7RHi4aTTUVdMXeMCL42UzuxgooGY2qL",
"1F3mwig1RRrYqGcKyLV4RfY2gRakqdJ8rL",
"1H6SkEkS3fBTdYZbk3QZ9wcqm7faiMgvPc",
"15DK16LpoFFMhJ9miJs6HrQgGcA79K8haD",
"1GfFXJvzdDA7nQRi77Xwg8rTxxdPwvtAip",
"1HC3LV7LbAzWJuFks1AgkignjAvTJ3rmGt",
"15faFwcKhJJwcsiMNNgn4Piu9iPvDrLBwd",
"14CLDeHTbNqC3wYf7pdWZpbi9KNXG7sot8",
"1L8S7DFZQEzzNsxoGwr32YHCsrubrBeQXd",
"1GNNXGPXsj4LhpKy4ifMfRVFvTRQSLvbzo",
"1BSPRcdf2SzgZWe9ZVRSWB8iqzdnXWvefx",
"16g7EJRa4yJH733nBTtzsLgzY5byVcfYRU",
"1MTNHZVB526n4oem4zi2svvCSzEXCFyX3q",
"1Bf894KBKAnPB2D8mZwEvERGB9bQrxRVp9",
"17WLRXzMLW4SBci9HU5MjcMtZZaxKaFDEJ",
"1Hew19b9whzq64jkrGQYVqA29GfGciN8fH",
"1LkPgDZs3qGgxqGsBH5y69LMM83DvDMaL5",
"1F4m2ncNdWd6PuJ1BdRkQnw4PGsZuDYBjU",
"15a9xawiPYmct5Nf51eoYQZNRtymiDV9Bj",
"1HmJGQbesMwq9cvSWHsrUeZtrMrWoxTGLY",
"16UYZnWd6mqLsNcWfaVeaeF9ou17cap5Br",
"1LFWArvyN4EnnZBBHyeXrJbakabsqTfaxM",
"1MNXPrz6gd67w52eBMKHnFRbs4pXu4QvDW",
"1KVqRhfDyNXYciFLvaqq6yDkkRMerx9CpN",
"13Tn4TJhnKuqWhoiAe9fUxcEg7HChu9Q5d",
"1A4KMHkaaV2PFwUYTrPkseKAJGvxX7APUE",
"1H7cJPLrZ33s9oUcamRm5mzqGk2kjpRhuX",
"16fQ88aHJMg2Ew5V5qKBW1UwTEZ1ujyPhP",
"1GWpRcJo1BgM6TZBumzzYnN9RhZBi3qAST",
"1fqKbtuMQfzDaXkwMPUFNAxhcuhoXjdRq",
"1JiRLs1hkfT1UfxDkmxvDFwSQGYgnmMFbR",
"1A3NZTmtiXWVFgPs98fNfrAzwbXsUv3M5L",
"1NwZJ4eb7T6MivjHm8t1pHVTUYAfZvGfpG",
"1K2XCKZWu9joDPfk3aPssAFuVXzDgnPrLS",
"16E3b1BVFC9dabYDEic4TE7F4WRvz1grYh",
"1wYndARZh4jhw7thCwgXQTn5XSTcJDRZC",
"1NdrxZkFvx4n8uJFPBHHTBa715XfiNHam9",
"1GgkVrtTcZALsCyocdjfaXfrGZWPsGoiyk",
"112X5gP4TeadXJx4D7HCYSDX2cidg3MmVu",
"1HsQXbY2kBhRppaNn18ET3Vcyqnyv4R6ki",
"15xtpc1EG4K3sh68sBworcsde4jDvXTqDP",
"1Em95ZVMPegyk8t6dfv64TPPMR5si3kEua",
"1QFwAbgiNVrjhNim62zoBs7G5UwW19afpz",
"1DoHRL4fTbRntx8nU5kCxSxDs3tCJ261Eo",
"1PPwRvhpWNPLuuaQeP6B4GU7TrXVdwfeuW",
"1NXec4WGRQMuiFCCCWRYsksq3cj5uutzaG",
"12zwaN5wDvCQTU8DJJEGk1v9mnQDKKWqRS",
"14jHQ2AGjQPF3VPgkQ1fsJiQcW56gbzdzh",
"1N1Lx6HQ4dkyyofpkTER7MERC8QbDe5tvu",
"172bnNNvs6FisM7WBqCQrYwvTpgsiRHJbj",
"12jKJRMkdJZUsYxgqwtWLiXUPwyo7AoaCc",
"1N5PVh34HxiBPDBuf4gKdH7oPFUTnu8gNN",
"1AvNxMvSQmjB61wonCTkLJPg99DX6rhzBd",
"1DzftfWZDFWkLGqNDvr82VAbe3LqAnayR3",
"1E8Hd5KyTBxoJEEQJUi5xeqn7Pc6VGCyKj",
"128q6kHsnDv3VwWbBkZrurfqewKt5DmzNL",
"1ABDRqsH4a24rjcfwhCcFAGaHDuwajBUZ6",
"1KGyWnyV7Rbk1TbaHqnRvyVLaWGzyiWoaj",
"1GM4Ne6mauLCnoPVyHjWLZYg5mqmw3jKtN",
"1KhzKQAFHTZMwKWXCbob8kCQpueqDu6EM1",
"18vmVfCDvU8n5ksTqn79wbimmGHoXKxNHt",
"1Czcndt22XtKXnhffzEfLttK9wJeo3oEQb",
"1ELp9dcdv5APdpZ3fBo9Qzz1dBxwWxZ1fW",
"1Gs53HFxxBAEohsfigTmUfs6YN5XPaFoUL",
"1HskJnrSS8BLoW7Wy4LpYNtBdqdobhgfE9",
"1AYCnLwZfxMKhUjvmNrzNgYeV4TLtKbD4P",
"1QDheCD69zZHCDwHsdkEnyPhGCCewVNozn",
"16PJzyZtZRYNRUUxnP2xCGDq2NhJnDg1g6",
"1M4DzxUa9z8gjYe6WFVMbUNFDKoLqXxzGx",
"1BovgTvF7BBT28mzAq8yMbhWpusBrCJXXs",
"1Pw33o3LVuEtzJRnWzv1zDWYppXvnQAs9K",
"1LUJza3pWdg7aNPr8htDHpiRHpwSRm22AB",
"1ZJbuHVTjggMGKqpZKxpbW8qTAQLcFqQh",
"1Cupu1sSs4UQfTtFzAvkgdcH5AJgFGHuxN",
"1J1HeEnf7pKMAG3wDwbLJxryQKgHmFRU3W",
"1Q1x5QT1y3po7zBz1SFgc5WvJZEw1PyB1y",
"1LK7VBizL5uPLTuzTo6VwwR4sp7UYQT2ef",
"18S5CXLB856CV7SNiRjfY5RUZBHMqZKRjB",
"17BaVe3Qspj3W9BvNYRsihDyeRJSJFvNcq",
"1MD466vKZFxf6SEDAyew2jARGLaYSPkgP3",
"1GpcjgVkjZQp1WS5hyCh7mmHonoCWJ888k",
"1G7bTmkVDP1AkHQ2J4JBsuNsH37VFTzAcQ",
"17Uu7LJ15VLvzj6qwb2bJV6L5GbmwB4uQY",
"1A1Xsx3iuEwAZQsNSSb5NT97ipQ832XdcH",
"1LHypkGE2uQms8g4gw22togLDxe2JwzsQC",
"1Ap8Zq9bmaXLHZZpuvLMJx3UrZ3XvCafRY",
"1PSJgmQQe7gvWFjByygx7FJqg1UXdWQCXH",
"1FrHar57CNgiDyqdZiYCKdsjWkzt5n11tH",
"12K4rsYstt5xs4A6LcMmgXQno4Wq1S4TJw",
"1Q1h77Jm1UrXVWVZ9iLKL2auY6GgNkgc31",
"1CwuMEDYkY5dJukyFzBMr5HQfnqCzrpDrC",
"13i5bs1hpY9EwmyMZk3jQW3pw3n5LvV8rA",
"1Eju9sZGY5K63pQyw1gPUCEe35eeaVkyJm",
"1P9LUPpp6va9FRvJFRi8RXjk9AQ9B3G8cA",
"14FDRFB8hQnxzHxxLD5CCUTA1APJysQdV3",
"18gQaPowNNBb9HsFSMzcgNqSBGiyPthBFg",
"1BLM4ZeQ3zt9YuHxx5MrCPEx67zP3xoapg",
"1JU12FT6NZmzbxptfwcLactjS3GGDe7qRK",
"1ATBemA7Jgkauep6Lwy3Z3bCWomiAtgYDn",
"18GxXKxbtyHU4KVaBPiUSekF95ZURFq5wp",
"1Kh6nPBb7XF3FzTSkP1TvtrNTQwPriwZLf",
"13E4bRnXpRotcSGRYhZB7cjQnYiVra5sBd",
"13PLfHbFv4brqTpGNQqAbyVcEaxLVzsvwz",
"1DBz2N53BMdAKZxhfXDtQBy4VrTzNrpAQR",
"19x6ekyT7bvadafmVcJxFmBhZJ4sdnhEQT",
"13m8brc3fyYhxEQEkZbdBb1tnX2xgsxVP8",
"1PEWZCqrwgx64AR4tqR6TA64wDcnqkUJTv",
"1CPEiSXnXig4h13GmNrzHKVthqeLQ2CXK8",
"14S7PbaBSBsCPR1kqbNPpLGxBz3Bcvdi7a",
"1BDEheeZzLjSCwExKnungcWFvHjuj56Tz5",
"1BLfBJjg3Dn9ikqzXE4r4AqCayu2AA5Yrg",
"18S2W9NkVp36ZUF3Jisa3BjD3QUVkF7xBR",
"1M1CHzw8VniUfmxxBXWTGUAW2o7e86M8t3",
"17XE3dk2eNwAFG3uF3FLqhBsBy8RSP3dB",
"1APywLj166TddDNzutDbxPkFer318TgS1K",
"16mq4taUtEKefzw4ba48c47oLS4ZDsecJc",
"1CdeupVqpxDxGSFy76evKHcmkkC88rTesE",
"1CCRJ4LNzoUGyh9dFYBh7mXnfGwu9KusCb",
"12di6SCXpg8cpp25SGJkQgj9LGkaya25jK",
"1B44uMoYcfsuyqxBx5Xu2Uy3MEAztG4E1W",
"1DPLjL8tKkZjPqRDqdddx5nLv63xQwnbnU",
"1N7jr7jwMDMbGfmLNZr48MhapY1KZHDZr1",
"1B55GFCBiLmCHy76eANNGiK1Woq4znLJGp",
"12dHbrwf5SLK78KFNpkR8ypPBt8BqMpPvh",
"1GC9kyCX75bqRa71NaAEpg2yh7932ibEJQ",
"1BEzB1iRoyiXCVpGJCTZ6rZy4grWNKw3fg",
"1LJ3sFT8wTQdYpXYSJEN66XhygSfnUxVPB",
"1N7JcorXiCvjp9o2PhsRyjTwXmoY8P2PRB",
"1P4QGkvyX7hGA22Zy6S1tD32m85xWsCHeQ",
"1KpfSh53gmLSRmM91fUP3vHXwHcEsXArEG",
"1HRix73byXnsHADr4oL5SDKcj6jL7xwbRx",
"17Fc86eQQbm7enHTdgqXKzP2vpMVvn49od",
"1FYVgDsdDJ2LWRT7p3PjeWCiC38ZwM7n6n",
"1KRpkPAyHnXXTyNyXaCsdBjNBaBCsEEvFU",
"12BQaZzcuKhQmDU2vN1roxqbydh4x9x7BC",
"1AZRwDFvDGMSAR1GZEzizC9xi6FPwrHGPi",
"1EjtGg8nfoM17TfE3xbDb5rx1nKsbaXmnR",
"175xvu2hPpQosWfqhDHJ1T3739tW2Ssif1",
"1YWtGp7MuajSbqLqAzf1scmpby8U4MumB",
"1PZrhAQbvBLQE8HNy2fNmu3TGQuRPootcj",
"1H6UGirU2mECZf1RQ4uwNDcU4UDZAdqTr4",
"1QDfsBNRmHtM9tzKW8FD5vm3EcCnAkXiGm",
"1GSdZcHzDMLeTqHdeZkp5xadymi2Put7du",
"1HxLqghK36nwX9HbkuZuWXoCGGgusWx6V8",
"1PNgb4tWu9TB1xf6C8n2nnPepfCq2LqLgM",
"1Hg76HdnteN3Umhi1koRzy5s2qTUAgb8mi",
"1AjXV32st9UxJnysABaqDGwmAK9s3NThFL",
"14FXBPLuBBqsSAzz2FWYeEVzX7QZJMSQHD",
"1CEmTXnhdz2SSsazM7XVKfeMNzhoTvBRDr",
"17Yudkeug4URGP1D4dwoGWgukJBHNExL62",
"1BgbDviAVt5bsiVioZWPyF9UFzuZK4Q42g",
"16EJBKs242kMNzgdgsfuaGLGS1rofAECkC",
"1L6haJGzxRkYqxR8HTA51MHMVoTJqAbMuC",
"1AvCBob6b4RT4GZEy5keYcBBHXupjidRBo",
"1KV7eMMpXUxXGBuQTMSkr1Rq5bSaCqgavz",
"1MKJXaUg4o8dbFHWeKcjYPNfYu2FBw7FM6",
"1KN7nkaa5LPHZuLGBzESsnVjnWfpgznEAK",
"1D1dszrinrkGPAACCbZzjVfeD89yaPujFD",
"1AkWRFubjGbz4RTZ6arHBjZE9z5p1UxG1M",
"1JiYge3Y8krtGCdmEMjGX6b77Mw7cCVbcE",
"1Q8mbW5Kwco2ndZguvAPKcYtmxJYAzWdAd",
"1NkU6dVL9BWyrgsE2CDE5TyPiy5zqiT8XZ",
"1DFfGdgzwDUZ57Ya2m922rQu4Km6sfMzhU",
"1HSFyHAWVMm6Cd5AHtK7mrfEZjwc9FZrwD",
"194sN9XbwEsgh92oMdFVvnvEHCrhAMNfCZ",
"16fvJUPzErex5bQ2btHCMkrXNWsFNdd7sE",
"1H6MjcwEYfxMsrs3ptEASaqXjfx2scyutT",
"1LpqdQnonUom9vbGFtfLdMJHtznoxS5Nvd",
"1Exdji2ZvtMQQQWwyHLEsF3ypKggtAP1yf",
"1AUHos6ME7e2ybNyihb65a2aM9SGqyp9Pu",
"132AqiMkmyg7XoBr5d8QyRdXLoc7WUr5xF",
"1AJvw2tghQDSFjUu6xsuZ7zfdE9fNd7ggb",
"1PjDWv2rpLowq5QaQd5hMM65KEZ36YwCTN",
"13mrAWwxxiFaN2ehS9AfQ3PTms68opznau",
"13b9hznkYQzvi1Qr1NeyZf4kTzbJJSbspS",
"1FSWzA7F7p8JEmJH8SkYaDJm1K74BkKvfK",
"17LisPncJDLFHJALH5FAFjaYVB8gmsYkk2",
"1JpCkrCDDBuYSRgxmwyLJHLTMV9WxEJs5U",
"18CEVtqz8fQeo4NYsY7PotQ7mgPFBmA8dQ",
"1761219vmJ2MwZBsqadpk4amZRkPUWLUpH",
"1LGybDBUXdvTJqy669wRJtpz4rsMazda44",
"1BdbGDs57jinWz4WfYYbpn8Y17J4b9ZU6T",
"1B7Ub1cJyu9LxUaLUvPG25W4vWH78kf53c",
"1Hb3d2uxmpRRM2F4am8jZz4JCYS7ECgZvz",
"14VUiSyaX1NCBVDfCL3GVh1aF5Enws1FwK",
"1PDkxk1qxncmcCMRz18jfBww2x33P2XQue",
"1JatnMBmWyq8PwdPcmKSQszQt556uzpa3Z",
"1E6E7LfdENWE5U6zcgEpQqHPG4T6hwCwrd",
"1J8mEfzBfR7uzGUcRazNQj8CBfbiGFgekj",
"16gQuuUwk3ZzMA9VELMSBHnCkkcUXEGQXL",
"1FwTvB29B5YxHs2Gmk5wvXfKsVZvnjtqj2",
"1HY1zDd9SA1QVzUZYWoi44JoMqS8V7MwPJ",
"1FnQUE59TFg74zuvJAS8yKmc7QU1y7EreN",
"1KU5Ssg4Fo7YGcApwNqK9aZLh5VFr8Pvjv",
"1CU2tDNKyrnvEhSGyNbjvFKXo1Q1j49iKe",
"1CGFBxD3HjcAEgpt4ZhiecPztzT6vnxpiK",
"1DAEJbVZqCbhSbwqUMZP4FyLTkCm3L6jZE",
"171JghkFyfRsbhJKUjzphopCqH5D1uf2TP",
"1ETZXDa1JFJxUy3aDbviTt4bnxUpLi65qA",
"1FayTX8RorvyoyMTvusbb5iHVqt6a6PouK",
"13ytZxNrCVJjQmmVQe7FskL9yViHc5Er5K",
"1NHoffVbutWc3EGVaYDCxk3sAkP23gHUE3",
"18hK14i8uwSerTnmSZWxmoCNMYZBhPyvqs",
"1BtWoaN1SA3zR2orzmGYLW16JayCVtx8Gm",
"1LQv5kznhVBvw5qVy95vbakoyEV31hrGB9",
"1PXNjvsQuCNYYdkHyXjvjqAiSRpwx94M1j",
"1NY4FASs92bag47shAqCgguNwfkbizF1i2",
"1FCL5cPuvN1xGUVaE8hnTV69bQi8WGP7vr",
"1CwU2iF4vhV3VPv4cXqnmbgR5FD4nvz6cv",
"19T7Bp1j7aQkFvdgJ3LshgQRD2DyAm6ibe",
"198F1JRUH1LRqadNkcA88mhvBVMmfLjAVH",
"1KsFcNYHMF21w435oMLThZFisHKQARXLpt",
"1HmRnHbVyTVK8YwdMvXzvXmCoJcsUCqEdZ",
"1Fh4AXfbi8Jwgf9EEEemgxTYUvygH1DuVt",
"1P9BsP3w5TtXNQZM83WqokRRwqEWiUKATW",
"1FhLVLEuE8q6Em6P85J2fWezRmMeyrPaWV",
"1MvQrELXqcrvJBBan3PvZKBhXBbvVmxpar",
"1FQjuxZ62jCkyKUnXUMqveigkBoTuRshrG",
"1A2sT9nErmDwhEj9XHbBsu2HvNSQ22uToG",
"17tfNDQotxDCiHZo3AJVnf3UufkmCh6eZi",
"1NTMZQZDvbmHihMWTG93SR2WKursppjCDL",
"1DqbvMawFaUWPYwtGAxiSyd8q89UXspjXH",
"1A5Uwj4BF7GQaWyDyJAhVXw8pkJa3VUWBz",
"1P4XL1ST2UWsjKGWz6UM12nmNRaAhNSBdA",
"15DhV3yYdnZG8jhoqdTqWhkr9KEGX4tB2a",
"1MbBVaT1nENdkkQ5LYRqygAmik6YynTeki",
"1PGJSPd5WsbkRoBTqE1U5m2WHun8bW2pxd",
"1KA22HUyiYpJPpGQtrKnySATADPUqStyXi",
"1BWxoBUjEgJ5PQx5wgi2LFbKqByT5CoQuB",
"138xUBhVqxZeLY2GJ4cW6G2rRcenVUQFnS",
"1F8LXbtWG1UNbpvCE83iAic6Mv6R36twHa",
"1H9zJqa7aFbU6kFQk5fmKhK5tpwjenduzb",
"1N4HRrb9vBkmtm3AR43yYYXK4W8tkKuZ3C",
"1LY5uAqk8PdprPyrmTsUuA8yWCbErL5HjZ",
"12iKyWSy4crSHtTrXj5mjWFZd4pN3wEKHX",
"1FBTXTj3iGu9cA5aRJo5iNpLR25RRz9PoA",
"1G55sY1BjmgucGyfihdv6vPrqBdBibMbPq",
"1NR2cpfUQzLhe3C6zuwrHmNzXicaaVNgCG",
"1A2bJtmKcmHJTAN7iauGpfXsr7Fk713gLB",
"153aBENkxAHYXeb4MmiBoj6U2cUvPja6SP",
"1NE6Db9gxNPssSKbTKnaquHacp5zxpUKJp",
"1KmcCpASeGDsk2bemPmRwb5VmM5h2PwpuQ",
"17mus4Wi3NCNpHT643ax2WJtUWtazSsh1E",
"1AEMM4S4oewXU1VsRqiHTTNpWfh86mcFry",
"1EnQjHd9CwhyTNUHmZezDbbvA8cXsJcrE6",
"18KsCZ7qstwTSwQeLD5HVrGaXWNHWYiKvz",
"1918n2xvvuNBMxUJ1RFgETghEf8guRUfLJ",
"1NojBQfJgx87L6QBNiSWGX64moQytTyBj8",
"1EAn6CncHN26YFaf7HiAHzCcTgWAWaAvdC",
"1GgtRrzfLCpaBgh8ZiNVq5kS4uF2nEBe2T",
"16qMmM6nF9C9rAqP7mUNzYj3NrmvFkcAMz",
"13sivfP3897RLBmsqyRNNRseUBiupk1pBe",
"1AJcyFEtLdXCojAEXkkDMTe8VLdbE9hiL2",
"18m9F5ASBS8AeeZeUjRRZqwuFxv8NTFGGT",
"1MHDNCh1peNR2UjoR1XkM1R2XinjAw2pyh",
"1Lyu78eFo7zVe7cHCQgNu9mv56qwqECkFT",
"155jcZ4iRmzSQSWHoPbqstn8f23jGteDN5",
"1KRAtKjnwzQPp2NfMqfzBmRx4X7VG91XEX",
"152u5PUtoLPfFgACGzB2erNBxZ3tKFzaur",
"12w11gBUWwPCu3BpHqmzBPrFJuftyo2tJX",
"12EdQx6oyNafi2NBvJJ32ckf17ejhAYjQS",
"1PeLGMeMrjGPSrEba6dwK1DKm3U3Rtxje",
"1P4GZ7zBeCejwe6fo5Z8fjshMzhRur2Hq5",
"17JFZVjw2h1WkvfQsn18pbtremviShLs8H",
"18MCoxBPjYuGA8AGCwRcYKXwJkJA74vBye",
"13auXHA59bg8kQYCEY7c6GuomrWAZPddmv",
"1PAoMtreUXAXpucoyXN9sNTroLEg1S8Qcu",
"1BZa3MhdhwDPc1x3RrR5mhjePzKAYGx22W",
"1PUvvV1tkxsve46R6itQTHyyYCNaAFxxSS",
"1TzhaacnvfvRsDJzqSukP28FVKTUSUyXP",
"13yRcvqMEXcbvFQDynRTChKLqeMt7yfQK4",
"1KHtmZoS68HbmJ7boonoVTb7gC6EkF76Vu",
"19xoW7CWXjcEWnEz7m3Vx1cpSyVFLF3VZt",
"1CqXfpTi3m7jVfRvbEz6v36qjeYJRx2DMR",
"1Lc9UwVXGP3LGtpxZjeVceLBP43gU2vYc2",
"146CETTem8HX3FEoF3WQV5dmx8wJRi9kBB",
"1JWXM4JaCC8B54UwdNEciY76Lwkq9mCNUw",
"12WUWnnXNDRozQ3SFfD3zjbDXSfaHEwKdY",
"1FDDowixtAVWYw6aScykHgLwL9N7PfK1L8",
"1JkmCj3CDqGDV8DTvoPPKhuirRu21LUZjo",
"17JsK2HN7GPj8V4j5W2s9zx6iykifjEXVQ",
"1KyB1QxeP4tHNLGBQpmPGxPaZY6nf6yfqV",
"1GMxqrbSsxVgPmF4Y9XoVvwc9fCBXQ7nTV",
"1Nppbc4K3gm3b47FWuhy9jcQHU6agxJiUg",
"1LTBYKmpABGZtRzj2jEUDYjynMtcAPFNfo",
"1A8AaQ19npnV6ENZu7nVzrQVMFPfdMBJYK",
"1Gg2YxhewxvVcVxjCcQ91TsCRQWibofYHm",
"1JqnJzc7dMZkJEcbgYfmyTDojRewVAC9Zg",
"1FQWFYBQ5qKGgVV3GiGX16yAejoHYh26fY",
"1Daupje7C84roESFhwobgDns4cTRKiFZ9D",
"1CPxUvJRqJ6aUpZBAy53otMo3afqoBhvUC",
"1CXWbfXDx3DhrLnSZmCQbxjs47v9kXCziX",
"1DWTvnh1QfyvSHL71H57psQLJwoUhXHCvq",
"1KX5JcFcZSu8CVN2bZebEGRe35vp64VJNc",
"1NpyV6YFYE21inVGNdj34mDhZaePRJ5Njp",
"15BDZLBdoBbE3tVHesAbi5Gbk8F5anJ3WQ",
"1MqA3KmcTYmiPScigr5SEYBb9bj61qMKMD",
"1NuGNBrqmmdbiHWv9FQKMut3yGVy2D1SVK",
"15VuDAhVFrpMCorwD1Jd3U4TK4KcUkeuQY",
"1B4fd2WmRuze4RdqcrubTwNaJHD8XUW5fr",
"1Q6s59nEFt74rqGzH3ARKbDuSNmVjwaQrX",
"1CKsiqWyQYt7DFCTeCPj1T8gmq69RQbLDx",
"1JLSpcY6fnWUJsDg2nNM57BJNE5WKTrCSp",
"1DjSHR7jC6W2hh7gAKYMEXMGzx6jAuiYkB",
"1Mm6cSgmEhC2JWRuGsN5feTMGTPz9qGY11",
"1DFmLQQBb85LPkJHgyqPPeFp9pUpSRaGbX",
"1GTGmakCfrY9JJW8FPN1cfPm5SndxcSfPC",
"1GPFXVtAQzSoYGZKSHK7FvWMewbTJCGrUM",
"1MLN5Kqq2MSf1akYqE1EoZRjAv8kfEzHq",
"1Ci5W5cwBEKraWu1QpSjqsAZZ5dtDnJLR2",
"1HXnsLA383LqXYDFDu5YQ8rd3JcDGQPNyC",
"1DVcxfP5vgUy7L2v2n9GCs5nZRKHqwkVxK",
"14P8PW8FTaj6Eizbci5ftLETxfuabmEDQg",
"1CvCUPnj5XoShoGMPSkEHXSaysNtXPp3zP",
"1H8GNJCsz9cfzpfUYUSrYd4BEQipPCFV31",
"17ibuGVSgGYPELmZR1SVVyheRDu6zts3n1",
"1ET2H8fucKNSP4hCYdtPiwgt68CiuF72Zw",
"1AF7kKYB5ppmZ2BgfMfpvcqswbuHo4oaaf",
"15EfmQZage61hmhmQsRPc4UpN2Sr9LQiA2",
"17XsLHwLh59L4bi3FSEWW9CeLcY2PKsC6q",
"1Q3yK5oj4GgqufBmVZaewz4g4thyeixVF4",
"14qn8Leu4sowuRYFt2vXj4HgVRe22a1HP6",
"1MsLefCMLpK3d2eBHpYsqhLwgKcqtzEhxD",
"1F3VzFxkFE5LijF655QLuAsbnEMAsXTLhE",
"12fmJvkcquFzsmrja4koTunEmsFJk6KJiz",
"16zDWB2GUz6D6xV2JB3F3ZQCsveb7YtQQt",
"1PJ5KpYjy7zcsEY7LicfAandejtVc4bZ5C",
"1A6Quw86WUCHgBdyj972dqC1bq1wHAnFj9",
"1NfU5T85UBZ3unbSGntctaMgQC2gdap39u",
"1DfuNicVKdkBF75PkbprnjWvbn9EmGYuNj",
"12hYnVEo4mqgMbFVRMYAWBvDfMPZjuMt1u",
"1GpF2sY6X5DwiwgWJyUuxt6PU8cGsTuN3m",
"14rjeePTPnrtAmDtF3TEesVbnVNQ7L4hAu",
"165fX8cbXosfo7hnh5AN2paDfQAYLzPXCG",
"1JwYtBMF3E6vc3cXn11hxtRYKoRqQ9z8a1",
"1HLnqb7fZuCqDZq1F62FfLvfsmTdPTY2Jg",
"1GfdG5SyEATEmVak1EeHVJJLXUcAyJDmpU",
"1S7rP1eP6GmpVgNM5Rx1fS9o6Vu7GkYSk",
"1EmaVmfbv2vSZDAHHAw9Yebyt8BQM95sVx",
"19dc4hn4Jf2yKxEPZtUyScz8WPwFUccc8Q",
"17h1YcJSVB7xYwxe2k5fnfko9NRm6F6TpL",
"13m43ysZ2AxSeVB8EGWYmLi5LMTwAkbDV5",
"14YtiKGP37FzndPHtt4i177NDyyPaGGdNz",
"1EZsXYRNeCN66qU5dMntueBtdVzeVxySy5",
"1NgffhoX5NVVitVXgtSuBKq9STGH2p27kU",
"1BrunLvonHLjj71UNVhPFG8z3F142tU7Qn",
"1D1MJPW5yTEZ8EMcXfQ6WohYsUtLYSoN6S",
"13EDZxfwym3ADNh2E1i4CHLXeBXBmgWcP8",
"1HGmjzpCvogMX2tJKFcevxjMWNywfJFNU",
"1Beo57XWkuuFcynrKYF3Jjg78G1U4oiNpU",
"1255wzVDev2iY57k5o78AqK8YgReVHKpK7",
"19ECTzad7mteL3ZDbJ6oStHcwV4q56vpoW",
"18Macp2fgcSFfEP5aCap8qdKBrkQdfQS5q",
"1HJ8dSjW5cqhwipuiWfcffRXNANY1YTMjW",
"1ayaXY9CLEJm5Z1ePaDWmZa7c6gH4xNKB",
"1DVRmAprL74oCrgg2JX9aNiKNxgkv3AhCz",
"1P949TuUGzpXkzGM163ZGUuzbGZCocYXjs",
"1C7rLBvZzQsrnKEEfqYvYjcJSaPsGBG971",
"1Epj3LKTFWdVChVTrvRj45uY2XEzHpHhoK",
"12rFfBwjV6ZtwHqBRLjRDvcamKsMF4HMmA",
"1DUsxyEAyxbZCyhRo1xZKJcjD51qyBEEKv",
"1CuoJjcg5RRkuno57nWWgqmYyzfdVjc43T",
"1E5yGyaXRkWNfX2t4oBQ4sNcdpy7PZpkdn",
"1DziRuPtxMxF856d48BPcjKLkGaX4ShoQ9",
"1CYLQyYPu51ARY6QGbNtiDohgA87sAmLmm",
"1iv4JdSkrx98k4jMaaVfHMv2aA4gxM2DP",
"16wQuJ5bWVxizvXfeq2E44H2u2Y5PRAKAk",
"1MBxN4etvsXjpiZQVhMLVzPf5PKD9sYnKd",
"1CyovwZG4VMYVeyo8aSHAyEDgpL2zbUcpv",
"1MRDkKVNQ2EgT1X8GcJTh2Ak2RrcDW6qJ5",
"1MnuBd5DCzTy469ci2wWcdq159CSSet2R6",
"16ySsu5vX1HSs6EuUFJqeNjmgNFcqSFEbM",
"1QCSEwNcAUa1YaBT5t11Qye6ML7joQYvoZ",
"1AhKFjfx8AYJ2JprNZqy5b1v4FrqZH68GN",
"1MjiakRZ5nLL4GAupSnsE7Ra2NsirPXkC2",
"18UMbnjFL1XyVSEBsJVi3pmSy9iDySNVy1",
"15NRR8KCeNp9FH9ZgSMiYKe64a4coGiutG",
"1HTiCeZSdKV9BUv9suegDHcFVsr91TtJTk",
"1BzLqAHaW4PNBCKdKukZ3SJHrA7dfhudhK",
"1Nb1Xs6xxeQaKayNyo2bHJWZBbtg6upAvM",
"13ijDBywmb6cDumqMs34UqbL4hBzwT6mPT",
"1njvTvHuCpQZtnghcV9uDLSVS7aFwwoJS",
"18PKk8FKYVwqqamS7sLvhhR36hL1dgswXH",
"12S6WRDMnXpQtTRZQRLeZp1SUNpzqMoiTy",
"1MqoapbFuDcnE6pCapXAif5HWSKDshAkpr",
"1LQotrXQET4FJvQ8gKRh5rpTMu1vapxnjG",
"18s2XgtZe1cUtwhQNPbVS2a98SNi867jMK",
"15cQY1geGaqpFEVg8KpwQfVFPzBrJvZHkB",
"1HefxkboSy3D65ktfVuCA1hy4Mg669pEiz",
"17vWQiedgqPQ5E9M87vnk5uhLDTkdaNQtg",
"19Yio91ob8YnZ1nXkBNxxfnjQTf7vH7N4S",
"1ABhxyS6eKrUiXf9JdoKwZKV5wxv3EqTiU",
"1JCFfc6dZSMG1WWmUA69AsKzMkmf7zMdor",
"1PQR4Qt7dJTZexbpphyKWwwvETaQqxEB8q",
"1ELToW5e9XS6oKnD54QdxdtR7Fofj3Yjes",
"14n6TY272rqKyjBDsTJ8U1xBaXcLzcBQkz",
"1nM6J4DiET1UhgEyuj4kFUYc313UhhFF2",
"15GrgnNpkrpjnytjcjW9JhhGi3oNLhVvzB",
"1F9aoVTsKWudWRmT1mu4jEcqwcdjuseHTu",
"1c6DBtYD3iPoTN9dGDQGsK2kzmHfGC62S",
"1JzMxzqfWwxQhP5Yvz1TVEyanpvAwrqdBv",
"1GrnWUxGTzWbkoCuuZ6bELbnxJrAA4aDyH",
"1K6Ey48qkdxSenJfNeqTe44Ss2Nf5Ay6gS",
"1G2bUF9p4Hm9q81WXBtHmagAXU14hFXzwW",
"14myePw6hada1RgeFY7KYCdWeoK12oynH3",
"1BPoYv1QcTvAyr3QTR2NngDbcXKXKk1WAJ",
"1CNogi2SCuPiq5yPjWEZJUNmcjdoLLw5sU",
"16NzfEXtFogSaB7Y9sb8vvRe48PFMDXnaq",
"1DsSX9HsoqXX6hvBiCUepgW9bTRbdb9pN7",
"1tqByLeWKXYSQ2KVfyEfbyQuULPVYryz4",
"1ETiEL3W1BiH55hbxpHsGyNWhk2SibZyRM",
"18AohVb5pyShMJKokatgBiKPXEgYjA64GL",
"13q2P1Cof8Cyd4WPDYmfexMK7dxDCasFN7",
"1SFFg31aocmCqyJacWNoQCt1anJs66Kho",
"1GSKuyEiUBVZBitstT4Fgg3AzHqfcasGvo",
"14fmfARuorij8rD9ZeLhVtsn1pQaDs4Kyi",
"17tXVAg3jANfHgu9bUpVPbzCeVEiJD2w6t",
"1QABmKznD8LVWde5s1FPBBv35nHZdgLVjB",
"12tbrDNeSVSh7pZxnWBYjUEJLbUqZn8RrC",
"1137wddaG1AuANxvuZHPUxhdY6QYGu5cHR",
"1KYS45b6o7JVRYKysxoSNJg4owMiBxBKmv",
"1Q4z3g93rUwAPtEX5sZBedHeVHYvruyTQc",
"13inWvycrSrcEZTtpXt4RE6yReJiccCcf3",
"147GWWo8nNL26UgqoYpvgrVvgC8vuA4kHE",
"1Deib8jRCKPpyv7wG4DyLRu7XdTQvMV4Vt",
"1LuHBoomnvHk7rPpaRFqdrQFqPcEGZcnFy",
"1NfXRr4fYQP5eJJvkdFj6WNvMjEHFoDUvh",
"18DcBQZKYfSDTNBHF8uWwxZjycGKd9bzmC",
"1Bz714rWUimTyBTLFPVved1Ady42vUTHkT",
"1GKHiMk2H3rp5P48nQ52hY7Fv3V1yTcZ1b",
"1Gcuy9Knx8PSYa9JSjmkMSW14H1tAviV3b",
"1Fsvze4wctpGFf2zVF91mY6SPJcCqf9XjV",
"13Moa3xVG9srEVrXGUBHHobifkM7jZMTjB",
"1NPry3owqp2j5ixEUuXxzsbeDUAVvFUnBV",
"17T17jCVrfXAEWzofd3Fbt1aVUHJSzKodV",
"1Q2Eia8b7gwULm65HEXvkYcMeMrQGvM1f3",
"1NnvQ6cV4VcxYUzosmGxEw3PbhNLaBnpKa",
"1KK96hmfjQ2zRA8YR6qrnBBNmqy2exWmV1",
"1CcF6fQnVYCG734jYZumiRCiAfXpQVRsBU",
"14XWz4vdk9n9kTvpq23XSEBpZ3DeLcJWEZ",
"18DzQWEMDjhz3phmFGfhuM8CCzRYDNt5G",
"1MnSHQk7BxVdszYRrhWv7J4XSd5F2VBUaA",
"12f1Kq6utScexgqv9esdB98mASNXPLwdgG",
"1Bm6j69uKoS2pWHxnHmunXam66cm6KMj6L",
"1JAp4tdvfTWwTPRwFgxm3oi9H1oohyM3vv",
"1JKKtfNEwmnwoY7NPLeF4cSCEcXRZpofG4",
"1CfH1CHsWf82uVEbTQ5Ep5g2uhULVyp1J1",
"15rjCMkrDr7Hn66d9XkfQsfWK1L6KZUPRL",
"136EHKyW2cs2qpa2rUFybhf9uHDRFHxQ2C",
"1JPvFSELWvAZJdsrYjzpASzmMm33ULBgsY",
"1KSPin1cKuGQiUdz4qtutnY7zMrjSZTEod",
"1FCDpF1iq6buUeaTydrCWCthfczpf4MH1b",
"15w6Qvx3Cd77tJ1oVo7QeRrLCbtasxwL4n",
"1JgfBjtr26WzPJJgcxjhhGGBTWX2Eco6vD",
"1DR8iM3FA4Z1ej84YR23X92pnvFy9j3okW",
"1FZ3AcLtLaRB7uEDNd7SGZXcZykWobXWnE",
"12d9imetSGKpvivwD1Br5mFP29SCTv7wmu",
"1PJNfxPpz7oH54xpbveB6V7VCLGNxQTeMx",
"1FSi8SYJHXF832yUnRZPQ2Y6iMAo6euCQa",
"1A5FyeycL14YMcswLbVTMP61ihZvqSjNgZ",
"12uQcNa49riu8XS5agipqGgDx8wVEELoea",
"1YdBYyWs1ZGHi2umHUHomwpdp53khYA5R",
"198nLMNsSgQavE8bEQjLEjZakwqiC9wM5z",
"1Jjdg1tFhUbDha4jn4QAWNTNKWpH6ess5U",
"18c3ESG1DbG9HHjxb5p4ZDRLvfUdQcthyh",
"17R37aPCxvNzkd7NSeAPjbUwjzXaHchFdD",
"1WJjQH5nVFuXmCpBtRxmAZHRCidtwVXFf",
"1FhfcXUDBa7X2RjBgr2Qvc9B6YfEFaTSbz",
"165qradCjFrkNfRsjput52FWRHMMJLLmKt",
"1HP6xFgWFN23ji4rqWcHgeSYnq3dL1vtMg",
"1D9uQXykppWa8kC6Feb2B1RMxsVPW3EFxd",
"1LAVrNtt5ZA79fs2tKxvYMU81x3SenXrqR",
"1K8XXDSWsA3RxLtzSiJS3BJxtu6AcR7mxB",
"1FLaCoqn6xMEm4WgPcAZVQ2MnAyiYomhSf",
"18RQw2bPEEQQhjq62qLqoJxNWJ2CYF43Cx",
"1rhnUEjp49vesPnSLqnbhLyw89hirjaKk",
"1GEqTYXRH9ezseNWdr6UiafSENGaEgxmJe",
"19tgKNxKD8zYEgJN6KpuRrJhuJ264t5QX8",
"1D5T4prPKw8toKwwsM92fMKS8nS68XZARi",
"1EruseU8j9FZtdGV9CU6GZavZGnQYuHHgx",
"1B28VsC8wjnFrTLmcwF2drLRuGFTMrqTYh",
"1BWRwndA6Eay2bdX8391XPYMGsPFAromFo",
"1AohJFL6PVkmL7N5vSTQvSTqd51Cy47dZD",
"1MoSUitFXfCeE9W4HejmTSPRTGtaFuLQBM",
"1PNaK6bU9frWrv3F8q17EM4DC1fXueJRQF",
"1HVsDCtsLCdSVmrWL3fCtSK8FdiBbZMkTK",
"1LzkZAGRCtrM5Hnb5cqmTqhD1tPV6nhhNy",
"1CfGd7JnHcwUCDnSsu8t4hZ3P84hAK9MVs",
"1BaEuawp1o3oCMk91X3nenVwEC22bMaFft",
"16jzXhrQCGsqTrcVSG5aemk3d8WLfYgh5o",
"1HpUoHNNEkspMQdeTKMvYwUpdNNtwh8dZC",
"1LAcgbbq1bsqGpo7RRFXeKVzURYEyVR3KC",
"15ssQcnPsVm4xoimSmKBqhdKSGFDFqZAvv",
"12mghbuxz42ES2GLakGzq2KeKK9Pavvpc7",
"16X7hz5unHgyhHPk2D5cH1dgXSgdFaVQFs",
"1EpfF7UEyDca6nAVAyCiFXuSPuz77NBMyH",
"1B7bfaMDemHep7fPQL36WyiqWFQgrw6XF5",
"12oco6MbP52jJq7bPn79toZHqaugbg7Aap",
"1L5bKM845FkmAeG4penCrKqs728EK2uwwF",
"1L2MPfwCfiDT1yxSQX9FfUhwr8UD6Nx7gT",
"17dFogE1V6DcRsX9kEZZumQm9MzfnSp8Y4",
"1LEpn6EPDfctG2kni7zATVx84hoiWXpzML",
"1LKabptcufCkfUbqqqxatbkTKdS14Mdbx1",
"1LUsgEvU1aVc9iTNTabGquUfBUMJdGHcX",
"1Dd8bRixSZ4qm4yMrgJDNVRqtxKFcHQ7mR",
"1BtL4nyE1SXsQ3AJMJTyu8AqVJXYwgeTvq",
"1FeD4uCdoiLy4rESm4jgdsqyb3roSwPEyR",
"16ARdzfVNtTKf8A36TNo9iY6ub5UFqQgxC",
"1NCGMfvzzDLwYvhGLbirq9hSAHTikTT3VW",
"1PWYnKrBierqFg1RCAmgA2YUYXUbhSHRiP",
"1JRW2dGw8pGeu4V5yVczzzMZm1Xua3Bp9",
"1Jj7ocZJd3MUGJo8oaxmSpKHtSmuhsny2D",
"1KmcyKymD8uKRrLWsDzGLuumD69yGUykUc",
"13xyRUjc8nAqjZ3yke71gX4V9GBFotDnrE",
"1Ki6GXiGeGjyik5mLSyhtVQJjVFAJ4MkzR",
"1KitDq9pq3FpNWxvxTU6nGLycxLeCCn9oz",
"1AP6MTc6DZJ9G5BW5eLch25Zqsiex7Ra6X",
"17HndaCyQcVceBfnr57Rn43MYjUMqXf5mL",
"14pcXigaex8Fo9JqTRpFq7S3GHuEH5iyGS",
"1DgRoLBDhw5wKAGj1TsNhXZzYepz8yokZm",
"1JPmvNUcqhh1cjS462C3XaC79c2CTbzHND",
"1Q4wCzmTjinBjLL1QYy5E7ubUYvjUBrAso",
"125PLNnD8ZZvkaV3jNiEaNVAe96brnE6Xb",
"1pKX4wwR6eFMWS631J44HPUi9LiWwrQso",
"1As6XbNLmPtwgSaJqUELaqnw9wRyXPXwLD",
"1FG7W8dqgK2mumoaWSSAuedQUzPBfgAbnA",
"1F4HiHJo64hYw1jnptMpK34gSFaBQKX5YG",
"12gqoqdCAti9wXYEA6mj9aquqwMFmpfz6R",
"18UCJkFrFLDQpWWFGv27TpPCa44VJj4jaC",
"1yb2iH1hjfT5P7FvPAZ7Nmu921x8fHT9u",
"1JXhUapeg9RxD8yAquN5si2dXafEkajMmJ",
"15nc5z69tdEVbireCSTZgxYAesVGKBS2QJ",
"15sFj1pGakNEHhWg4kBioGexUzvDMNrj7p",
"1HTbz53jrZt1KX43ZQZwFJuqNqnnt3ui9c",
"1DHD9Drz6fyL5PvDmNvDaBqTkxNwyGBcfw",
"1EhSA9z5CwEPQTjZFEoPjP6HhgdZZhXuac",
"1EgDW9vJGGNZJm1UCsZ5tJmc1yvmUgxxS3",
"1HLyp88cT3K88hMEt7cTAMDzd6oHoxxEvN",
"17n4k9V3n24H5joA21KJH79hi2vSSeemHD",
"1N6eDh2amFmkXymckR27LxNSFDxXKsDdeH",
"18sNnPciQE9LZ6CkbvRgG5qWLSzn5YMXbE",
"1EYy2Eb8noYQXk6JWkJ6frr2UuBQSGDpFh",
"16tEjj6PNSLjayJZAWQGZSmAMDkhos8nzf",
"1JxrCYHyG5jMKLVQtgjTZUX5P1zbsx1Luz",
"1CejnGg5kWd7F5MoBkk8cZkGPNvXQGP1ML",
"1AuaqGVFuf2Bx1SooV6v97BHftZQgmPJAT",
"1PDxFtNvam6H8z9ZFspCZA6LroW4ttTYZi",
"1KvmT8VBJ2tuG9zUvmJRymeJ5avboNpbrL",
"13xjLGBoc7T143aidpvmK9Dpos2zMNBokg",
"17RAQS4ZeN1xxaWDS4gNkabBYZuhqPizD8",
"1B7TuuKgMruvQcyrzMUAtaCXbFmTHnmG8U",
"1ETzND6fN2nqpPhY4xDCKfPijR9wv7JSQ2",
"17gutaJsRsMzgVQ7HnHdUFojGGMCJu9t6Q",
"1Hjk3farsGXdZVhd3pMwnCKMohUxPCYmew",
"19PLAbxwp2qggnzkENSks2HFhRUDRbdwmD",
"134feh4FAaJDqiQztMEPjhB9ftvpVKHvN5",
"12FdaBXZVdRVvV4x7LRVnYnvq9ZxWS3pEc",
"1FvrcyDBAQ1u5i5kEXCGPXZSCJwFAmyS5e",
"15Q6QaqE2y7BNX39xPF8jCr1EZQ2REcTSz",
"17rysyLsP9YteGciMpeCbMtkF7e8SemNVu",
"1LcbFvtrbQmyHxSUZC5gZ53YYaQVs5V8Dy",
"1JeDzEiwZwoF17Q5grgiz14ySqCNfRbccE",
"1PDqzWSQviYrfBwtMHUsuTVfLYoGmjn4XA",
"1Lcczj5VKkapF3yyx7gURgd9FPGa7s4yFA",
"1PgCFEyZ5EcVhe9XYbZKP7x6hCxCZJHk8k",
"1Ccdb9YqLxgEYYLGX2kwoz8uHot8hgYUB3",
"1Meoc3xHjCQTrXVFg2h1UPfvMrGnj8CBcs",
"18nBDuZot7vZBk4413PyJysvJVeiqtRyGF",
"1Fnhqn89S2QchvKkGag3zkYB3KSjrAScgd",
"1J8vuvsKaa4kkoMj3ZtijhfhVCwW2wvc7f",
"1aNJqeevSUmumNHcySgTHaKx5bEic2xXx",
"1LrxCKL5xeYD4CAyDTfAA7QsHteLEQXbcX",
"1JRUBRsWxM2EkQHVpTS1NbUwuNznVx25bx",
"1H4PF5eJQtVae5eqTHnYiqvXaoGBvKVmWx",
"1285K2AXVFWA4q82bG5rhsazYKvYQmNfrq",
"15Wd6LcgTtxmUctQMikrpDgvCrF2XX4jxn",
"1PhoyubsxgqQZ1DPyD6GCFQm2w8TQJr4Wr",
"1NscPhop6HhLKkKiT5dJfVKMNhTRpZhZQn",
"1GyKhmzjaaUTXK4C8xDK6VFem75Ps4iP9D",
"14ebKdTuwmfgAuC5op8iJD8Bz9qz14QHMe",
"19qffNBVEomASiQJvUuRGuMrfpbHjPyXXm",
"1BrTRvLgvoaXnGtrN3JtgjfCFU6CSA6UrA",
"19ZPWn7pCy6C2mLeW7c4Ydec8EFXR8pY9E",
"1HKqWvpPQiPhqN4zwYMFs3WLXuK7LNCpBH",
"1NdMADajWppYyy8ax9xJQJeUtzqEJ28v28",
"13aPbi5ECLeC5NYmjrs53TdB4qHSTNt9ce",
"13qDVobGNiYvZYXr3uJ5HjrJrFUko5WfXi",
"13SnfEfbgVMgRJcjqCaD8n9izhsNccmTy2",
"18RcRTYw9vcxn275PnCjZpF1GH7X1w9AaA",
"14Pre61ya2GWJF8jYYzniAoMpiSwkfGRyA",
"19uM1hBv7mXt6c1tMZAsVyBkxsftpieKgz",
"1xQ5E4yEmFGLkF3tFiZts6EVwRi6yCHCr",
"1M4aWe95Ue6uVkkeaeLZ3euN6XsGMVCmag",
"15vyRRC3LZVmiMmXRUVNsSZRpxJf7zFdj3",
"18BnpqiSs6xGzWegAx2qGbBA6KG3STvMwS",
"1C4Bcpi25Zsi9pRABQFcLj3CKeNknr2a97",
"1Nw91maYU9z11YBLC88MAfbFgvTCFRyodF",
"15tawYz2xuro2qsgdwcsn4bSNHw43TgMaC",
"17JcVGneCP26utWhBXnnb1rdDSRfpgozNS",
"1EFQcLmM2W4SUWSGFKASkikR8K5xWNTqLb",
"1Fj8Qtaz5ZqJLoBUZv6JZBgdHoKEzeDuy7",
"181eanjbF56RZjS8ncPp4qHMaDyGiAsvWh",
"1NYNinJ6EgmW432cFGgmCN3oezqYqEPVd5",
"1F8S12P3mRniAPpzbQ5MVPn2Bcgvq8oz11",
"1MexHnSSqzoTR1kPSuqqatFdPLzJePp3ht",
"1GXmSPC7Dm6edHPwopcWgCXHS4Ke5NTAUP",
"1Gcit2gRMw9q9SM9mqJ9ew5vdUxUbvdPDF",
"16xdzwyJvajEpJvNun4YbCrShA6YBmLNBs",
"1BSbqKxrtk3DJ7DVskuMFRG7W851DMWMGt",
"19j7TQVGVVbc3Hdezn6RWBM7SGKc7hbudq",
"1DWzR5HoogRWANnApb7zqQbZqrSqHjKXxd",
"1CRFXwqfzTT2SKJUjkdj8vUMh3uiTngqPL",
"1Br3t4vmu2CDwDyUQu3uva3k8Tn9EkTwia",
"1GGG2EsoiT1S4vfUaJy7u3uZMy9NfRr3bF",
"15RuKwis92pBSQPdEZ7AvQY6RehNTiVvVc",
"1GYHm9wuZXEYWiV9fGYDQM3jmWAGaiCtbe",
"1N7PvQt9BrjkAgMLR51WH3pW7WcGRLjF6X",
"1BwovPCMpprkSjtqkPK6kMopwpPdhLLmtv",
"1J9WizKJuTb3ihpJUzsHqyzPK3fC9N5YwA",
"16xcZuLn7nG2Z2o6z1CovBWV8SezzTNgoH",
"1CjhGy7ZTCrwLgUMC4MFDDZC5doteBnsJ1",
"1KNe7dkQtCyBSGyj3ZhdMYjtg6WBNT2iKY",
"1KjkXYFQQWDZMV1wEKDVnpTaK2WiFbTUjH",
"1GsFyNgWL28vFGFgH8zjqCbGsweP6PHUSd",
"16ehMqcHh2fv8TeXYQVgeHHr9VjZn26xj9",
"1BJ8LPrnMM2irvygvBVa8uZXpnqGAmestu",
"1HZ2naJbAbFdacrDPsP5M3CHNzg4zhdFzc",
"15ejucVVVSKSRjEvhbjZYLAgdzewFBBirq",
"1Fvu4XEP4nrz2Q46SQ8ciL23QCFEoKvbCb",
"1CckPj2BJ3j4FnajP9pe5wSbfoxQu8PUbg",
"1N57zg5ru5L7d5GE2farQRfW9gaHk9R4BA",
"18kaeebjUkgzV1vLPzxPZixJsNM82djUG2",
"1EVbLt7QCGHpiMS2RBbftL8gG6pjbhuEiw",
"1Pmz4dmnuVjBMzLFYwo4KScakzBAaojP2T",
"18Mh5ec1KdafmMFDXegMEPZiiouVuGzM8W",
"1JNvddEPpSrzegxbKxr8pzBxtL26kCPFc2",
"1MVu9fDCdRY4d2qLvEcjiqZdWgry1a75vE",
"1FwLXFKoex5xcp8vLiiFDn4oMT7z5yhWQT",
"1NwE2K1vQY8Z7JsLkgvkn6qnXpacEZcBar",
"1C5ob7WCi6vsyyWMKpJWab5FUbCLPE9taF",
"125egQD7KNdjDRAD8oYhsfEdw9z3RVGpzc",
"1JKrzx8ZMWhDtjeeA8cNfTaqec8GWC1TcP",
"13Hq1CdnWypnfR1zvL4cyzXGiRydmj6nwV",
"1BEdwF4WERyVMECYLz2Yv3gnG19NcY9FSm",
"15WYAX9nRbLFTLgGdzrUM4hwoBJenGBa4A",
"1NRQg3nmeUgA72SWWmshSmCofyNgZ11ewf",
"1H4ZeH73ojU7wA3uiR4zwa4HYK6FfMrBsV",
"17hV7hAKaLgF9sqsyfZ8bjGYNXyVTRcyja",
"1MEfr5oLcVeGrh7b6tykHPwYQgHynfjnRM",
"12QtdjohK6QPXfUGTb15Dc83FKGQFiELe1",
"17PKWfMSxxPnrEVbbjWud1sxWSmcCq8bNp",
"1KAqpyHQrBZgpHCWKAvhHys5tfQGVC9FMh",
"1873xY4fmSDkdA2eCywzTa1tijRC7Npwns",
"1M53DK7FAdUxyndMuJ974cfdskCPD9ThEf",
"1GFXvCy2UUSpjU1yptrwpe8hzsHBGuriFN",
"1LCKxxHAccf3TKKC9PTmNxxCFJQbjAHih9",
"1Cvhw4bYkWXNixbhCGnT8b3mXfkrYjLvKu",
"12yEzLoiAwM9phVFcHuUMnn1U5xd246muC",
"18hh5cvs3TchaDZMxS74BJTcf3nHtzpabf",
"1N1tbsJbyj8cqZrdWehA9gWkC9RoZNKM24",
"13TzGjmNgMCjC4V5J4qq4xJLiXVV4oR2NV",
"1H5pFsdVbPC2UeDjQcZdLGTJfPgjz7yPhL",
"1A3xcpkw9aG8FEeJH5P1uPxzHNikiWZVai",
"17Soe3atVoz91bxjCrtuusgDPVmvbJi28d",
"152zXghCNxuUa3ivSvFVsgCdSW4cfRJ77E",
"1MX4RPM1SLAPtxPHCAGEuA9PoyVBtbwM2b",
"16GRTdyuJDDTxZLRRQTTAF6nxG3ZYcTczs",
"18nRWuPRNyc6tgK8wVkYzr4vbrXeiJ9dE9",
"1LiZGqt1ExX8QALim9cC8Efre83sRM9H7K",
"1Q5eQS66gJjRRpXAsWyjfaarj5NfnQGjns",
"1H1eNzHrfWdMmqdQS2ZcLaWmeX2kACNe2P",
"1AAE9V5gY6BQ5HHdMVpGbDKjJgRpNuYrFr",
"1f1age8k5ciPk8E4mbVu2GwrVFV67DyXD",
"1FPJiPWo4fV2TjmgRk7SC4XzRSjb3LEQy8",
"125X5tE9SndSpyoK8fCNh6a9fSk4WvZV7m",
"13fXVEYFGp8ZwRwcUEjXLS5kUgQHcZkjub",
"16TLQL13d5UuNrdbA3ncgqrwf3pnXS5eSj",
"1D7QbhbUvdL88LQiAVpzr2Wa4hGQb37ztB",
"1Kfnc465kNFSdod5NaiGrToxhmqixATqW8",
"1GHq4rDjE7obMBmfRzTyKtqkSJJQVMR24o",
"1J6brvBkYDW5UvjE377bt9M6ZAHWYh4aRq",
"1L6cMwbv6kigofrNyrqfeAiqKbceqLTg1F",
"1J1HUYvCuLK4H5kRtZw6H42wqEVcwpmA5c",
"1H6kFbNQJzQaQKxcv73Ds957EeCmvYCD8e",
"1Gz6JA9JyRYCkAQDqBzvAgHEuz2VLtBE5e",
"1NuNfNchMwFhV4Xe9ouigqw6CgHEyTukfD",
"152MXwdszwsuN81fbfdVrBXk4vqSUDWE9Z",
"1PHMqjZAWpi85QnKxhtURcizrsMP8HfnkG",
"1Cp3i12ZEoJbpy3kJMf9zeN2je9TZnYmRi",
"17ZPozeyDLtJu6yUJmTMeRYsbWc8J93QDV",
"19gmVeWcGESE8KJK2XYzc2uwBm5xis9BWM",
"12dUMLtyCh1W8Q3epkXSf4xfr68XPVCbtB",
"1DA5eBHpNEA3Kfhfq1SERUYetKKwDnswLu",
"15ZygqBhasDQCDi8sTPXbKCDF1zdKuuStk",
"1NSxzkWf9szR6iXAEo6LW2UN5tXZEAMKx2",
"1Ks2LZGwpCVZZKwawoh7P2WUGVq18HE7FM",
"1GoGbjLJR2sjDRNwmK4Q6dAEVZe5dPiuKC",
"1JaqKQjQoFQC61sv8puUQptrmhQ55bH2rr",
"15jaeaP6NUkex4VQ4CBuGbZYDVjyMS29Rh",
"15S8midLuAPT7ebb8PtboFPpu3fPX7THre",
"14g3eQQnZ4g6ai5JP2SHWNVKVHXqLGzrax",
"1LVbCrFY7Z1i3p1By6EiH9R9TjgPL7Vc54",
"12raGoLoUT9XJrm3PLDPMQoomHz9TvATBj",
"1AifJdQ5QMS3tw59HwStU6WK8psM7QLFop",
"19djYhUEYdj4tNt4wSd5rPsBkyC1eF9Bho",
"1CtuCCnThyfwZsLVJgmzs8PnCPNbCPdwy5",
"1BkdTR5zkZR5zx8LNNgWevKErnEuMN3mNk",
"17TFDQg4To8gM7Pwj1joEHeNEPhJSF1fdC",
"1AXKpGYhc2US1JtSyGcsL6fxj3Ezw7TVx3",
"1Mcf5hTNp37xPu77VGDakNmhKGjocpvndX",
"1NnxnufGbxtJFNfWrKXT59HsQu9vpPYYbY",
"1DmeK5MRmCcPEhPtsvYYocgEZSj2kaZMhT",
"15ijDse54CX8AMJSVEFDRXGaPnPXKsYDyk",
"1FAGewpgcsTEevJ7D1s5Zod4xkPXZdTjCy",
"1KNNDo6q8PjhqhUxMBgSyo4FeR6hY7ZG8q",
"1GLioa1RqYALW6RjD7MiWt5q3v2G7qjyjW",
"1HXWHPaHzEMV2T4i1YjgEq56pxqU51AtKq",
"1JLeCykmXVJNwA6DEQM7LCMdjt2BZ2VMZu",
"1L447d4rozC8jCQt3iY37V9RD5DMHLuzJE",
"16Fhz8kd9hit4k72CoezY7JYws6PGajW8D",
"1DAgqBsqgGCWbMsuPbxYFQx7zrBFhUQBZa",
"1g8uGL2JMc4aNke1WAQiddkELimPPZfaC",
"16n2zFjZgGYU1s52nUjPWHKHZZMd39XYoT",
"1AUb8RKCkcHVoH4GYPerptpPaBb6wy98nY",
"1NNGyZQd4KgNCJsJuMmh1mzT1kbsFfBEf6",
"1FbG6MHwSQmhYQRW2TA4MxZtJpFaEXpyam",
"13S2EW1rk55iVrpUySLq8bw12Uv7cdcmDg",
"1A1LdbFMwVzchc63s7kcCxRgeKXYrSqgxN",
"1NTazCMwoNqmR9z5oeF4c4F1qKXPa3GcRq",
"16ZjQ7ZhYUuUuLG2dmTNnbiVdCobLJTLGU",
"1EwrFJcvnQYugLvuLpVHwvEmqmn5unqZVn",
"1Pci75aL8bPj7Md1AcdFusqffnvo4CgG3",
"1EH32nzaVid7Xczs38DwNv2aiCFFpz767T",
"1ABQheyJEtGk5o2RukfjvLhq9GUgiqTX6C",
"1LHetNRoBDGfdoxgUFZpobceT9QnyLW815",
"1BKHY9YpJvZizU9uoXeRg59Ros5pcA12ta",
"1chEweDk86oaBMVo3PLydpKWYDBKuyTdf",
"1LDagnQuYBaufL1h77DvWBZtUW4JCGHGAj",
"1MMmsRn5P5LDF4TARj3YKjKXcm181UoaHF",
"1CxQyTB1rEZvR9y2XApPPdnPwHf8x2pDTN",
"1AstpZ3KWvieSma4a4Aw58PfHPcDHUMCcg",
"1EiFzJuSCjGKu68XG68tU4bYjD41yS1pDB",
"13qBXVGNFqBjDP8C3DeakiRQUqrs4gWwgH",
"19Xb5aw1vMQB29x1AHXDdNzgmC5jmyFC5M",
"1LUtvpwW4y8GrjbbmtM2SVvpc4arnDdZUA",
"1H1QH75M1KUy2pyAqZjqxHnJBQmD3L45Y1",
"17ZPjNU2f1QPf7GGWg4WwteGCvPCVZsP3z",
"1FEdR4QeG2hT7Brpop2ZMBMcYbxhhnfdt1",
"16GG1Kdajjj4x98Xh6vYsrmNziSPfUbL4c",
"1K9hdrn5etYWA9oVMVNq2484E4QQmMQJE3",
"1KC43vdjxZv3f3jcNfLdDm1zdQ8tyJQkgz",
"18k8PKnp7SVFDiMxoEGbGBN8D13DCemycH",
"1KWiLQ9KMgzXyDvbQLy53pv9BPvHVPjzGY",
"1EJtHWaveru66sB1bGssYQfXWKeURvW6eJ",
"14mUNk8a2WKs8Px8PJAtZHDbeQ2ma5CGYX",
"1C6JpZ3Ln4Wc3SVNikA6JeMmdu1AebT1E9",
"1FwKAjejUVPrQ28EBh41ZPHmDrzTVChJrL",
"1KHyYVdsiSfWBUuCzMtAJLoKRF9qeDZeWK",
"1FT8DuGq3Jpiro7NW9ZndeQjZcM5qMPLnJ",
"19cfPy7CmoVGBqK65k3vyaGwY4Lr1MJ64B",
"144FHxjnfzLBuS2A5418tcyfrKe8bFRGXw",
"17yb4rxkS5qqGTif7rh1PMR7A9nv6MWj99",
"1C9BvpJbvtgCXBADuhpmUnsCBKHR9Nb4tw",
"12ByKYfbR9bZ9DnbaiakFysQZeyxKvyKUn",
"1MfwnDq6yAEkcoQH8fYfxGCJCzDbX8adqD",
"1GMyq6uJEFcsVcyaDsaLwwVjZtwPSeB3KF",
"1KLcNnVJ2VoxwKtemdSm4wxtRxH7uvQmeq",
"1Hy9gzWu4sCSjEox7ToMrxd6Mu6rxEJVJU",
"1KJxpCoYtE1Y3vEz2P652ycZB1yyTDiqAR",
"1KfdnPoYJSDR7aZ5S4AqNZqWu8uytzQojF",
"13UrXq512pnWXhdHyB4g6ZiAjfMNCmwFKV",
"19eWxfTVHHNpbHv43yJkWxwsbYy7J7zLYg",
"1KrGNX3pdy2Hxt9N1qVe6mzQ9zFUNvVa2N",
"1AmQQdynsmS72qb2ZqJd7hN8fSE1XisdBV",
"1LsBkDy6NLz8GmbhbUZLz1WWvZGYmspETK",
"17nmJKySMCJrqEzhaSaT7qveKt5xx7eNo8",
"1Bm1Ppn5Eu5JMPNxYZPyLvN2JkyH72eThd",
"1BKWBbhpWf62v6NVRgMyL3bLaeVJUtmdby",
"1F2i6ZPHzWjEYfqfoyMCbb55tWbp9wtZEP",
"1MnLdNMVK683vgWjPXGdmYPtbkAZa7xt7M",
"19aCSLE2BDn8RoHQEpbrHHw4556Y63jKT4",
"1KEFid7etaevQsKP2P3vnxaoRvifWwneiG",
"1CmgypNiaxkAweF539LJYFiAtKmMapUmJv",
"1FSk31JmhNDmgmbqT3kpQ8rhYP7eruWgB",
"1FYFmnVAYWXLh2QfACGFQLvYvpe26U4PiB",
"1Ha6EVqcWMTWgVeEVnpGEALusT7bGamPVE",
"16YJomEr6oscEBJSHjwQEZDu3AwAm293tX",
"14qMJYZ6eNznMKWYv2krpMmmLWzYKcCEgp",
"1Mokhw8A3Qu1w7YQSckdvsyoHfJ2aR3zCg",
"16CwpbrmsCFug65fjPJ8K4ymFGrXh7ouzx",
"1BAbi3HowL4fvubPiWFumpeaoTw73fXsen",
"13Tos1wgRUUEPaag472Q5NM9wYrMSbGYxN",
"19RinvoUwfNFkh5NRVFornxCc34ahLXpDj",
"19x3Zr74ifQAdJjupckriKBGLGgo6UAq4g",
"1M7Z8vGb4RkfnfvZLnv2rNAnGDbK1MGSbT",
"15oM8GJWzjfbLJeg5dpLk7UCJL3hTm5uMU",
"16DVzfSXhGHq8rBcw4GtixZ8eJ6v8wBi1N",
"1FP98t1kuFavAQJ7buLZ9nZYVzDj8sSsQn",
"1A2CxDMMTG8XXqaCfcDjXFhMo259tD3QWP",
"146MTVorcm91gEea8Z1hDZFrmkGxUDfFHF",
"1EVz1ApZw3mAAMeAW2kQ1T2axSvf1aRQww",
"148jC3wiKf7YQX3kxUw4Mr9TftTUpTGf8i",
"1PBhi48oUv8FgxhRBS62p6hhe8o9x4Spdg",
"1Gq2LnwwxUe4kTHMcik8crrjMMsSHoT78y",
"18wHE9X5rgPuqXk9MEzggHaGWmNihhnwmD",
"1JugVDiK6gFnzZnadxFGwdVRDXHospjbDN",
"1DCq1FWExQ71G9r5AEJcTiJ7EWP4mVAgz1",
"14Bec7ak3tqqagPJKayiMcAvAz33QszHLp",
"13yBaFkhgQ5MjoBgjFntgZ8Z5cUV1BaUGa",
"1BoibrdG9MAEaVvbPK9SSyvC3DFXdV5vAx",
"12riEd3A3z4PqQe7NwvnYfhSW5Y6hsS4xU",
"17fybWtSGhMhRucQYZpRAg8muWWcfVrwEE",
"1HDrSJ2zgQPrE57BLDMXA5DsDJ9fYgBUEf",
"1H6Qpp7GNHKfeYCgSNRU5oR3bmz1QfcQT6",
"16sYaPm4t4dxxA5yq6DFvwveiimdAfguaY",
"1DWNyGJqewzNosTT3Vf82DcNLRZNumk6Bj",
"1FjVhhsm8eJC1EekYRWvo3vcMXGNMTSXmD",
"18AVndXqE7e3uedh5nhqk9EdTEQTSWHri7",
"1LWZfmAnTN6eddp74UBgrVF8p82Qx1qHpN",
"1L5eB5WCLLcjzcv8jyaoqo4zjqEeXF52TM",
"1PPhthwFzuhcapm16b9ZMYMyshjDKV3Knr",
"1P1YdsvFBuxzb4NJQTztFGpn6Uvwq8dSDi",
"1GTNzJNnyv9xVVLazRvFJTPSCKzwnELtWU",
"188h5cwLXPeUU2jdwSZuYnSFJE5wScAhdR",
"1BYfQpBppweSq5d6ykMbn4c8SC58NrvYk2",
"1NNxLpkV4mDPRRMgUL2uPYR7FbfTfAPDMv",
"1AjtejD7pUGrKut5UM1JdN36AMXndR9Y51",
"15RDjDnYHDedpHCmnGyzkg3vEJwfLg7Exq",
"1Hb5H5otAvNWJUc27USt55ce8BfpZ7pP4C",
"14KKYBLSNr6vhBd9eBNoLnkJmeKJcJGmab",
"1CeDCEKaF9485rxUDXFhFAyVk7aYbyF1y3",
"1Mr8aBYRsnnRp46uTsAHNFTHEjRuPxUJYG",
"1MfmLTW1kipaYAQDpp155qbcwuD42rHkdk",
"1F9UcMYYKD2hzrySYiz7HcdMmsBUXsqx2Y",
"14iCSj565cFrFQhw3k7VFEG4b2yPEaawtH",
"14D8D79SWnkXPsws67cafy8HbtfLzXxnUc",
"1CtziYZZejiBwzh3hWrKNwq8r9PNNrARAY",
"1Bsy17vJPKq8Ab1wnD1Ut34duSCqpojfJ6",
"18Tye3HcoXieSWWb7qbF1jDrazjkpboZEw",
"18SCuP7heor4ZmeikFmAqtFr7AqqfmRXZX",
"1BUKyeJFbtdsZsHvvBvmrbj5CJRYbubBma",
"18h9bjhMZSkZ9d7kUSFvDfuDzpzQKYyqek",
"1AZpW4JDsRy3AKcFmDjHe3WPJNRJj1CuPm",
"1JCDEh9ZG72iJZKwfruTjV4aUrrcbZ3Qbu",
"1qLEBSUKzTcFVbuLtw3y4g188UdSJ9Hn1",
"1BhpYQ1QGGWFoxuJjnb9RFNX5biBwjTZxe",
"1LjKm1PLyjGQCzBnrzEJbg9kkZtjpQbs7P",
"1DtgLN45vvRRQRN9pXZ5TVyA1LPmUf63fm",
"1fzziwQgVdacqrifugNKYvPAAxBmvHfoe",
"16KGAJgudmh9xo4xUbM2mFSKvjBpXk4mye",
"136zeBt6SDmbKMRofpHWDp9VuMc166YdVt",
"1P5QKJBytKGuLiJgQiUTP4qb7F6g1Va63k",
"18s1pi4ykhkB8weyuhmkpxWNNutWNmUNtW",
"1DVGzUjJG3hur515xiUY18rHGSzZR8vN44",
"18a6mrEBu6RR12LmHkYR4UaaU4DWfcGoEJ",
"1M51AZzT2aYYPaBdh6b8TquC8o53US6KKE",
"1FU95EaE2L7d8GrTaByVJBR7C6i8WC7BnB",
"1JkZfnugJxFrizwwzmZQ6KGzwm18wefWmr",
"1M4qFWrJvuiW2XfEGEJeMR97d7vjHNTsa4",
"14NyRhPtmjMTt1B4fGkVJyJVMpwfA7nkqK",
"1Ajry9Ut78u43BhebbY3yY9KKYbzk8tBrB",
"1BPLrrtwc2Ds5C1eVUg712M5T4PSYmYFnz",
"1PseDFJfD8Ct8zW6PMeMVvYSwd6nVeLHmC",
"1BQrWahc7TgYoi6JZWdc4dy68Kz9uCz9BC",
"17LsVmhLgAuLaCJqHLJoEy4FSxLwUY9pCh",
"1EpA6NwjQpxET7kkK9Cb1qFZun3wKbR8E2",
"13oWBEjJQkzQaV7m8E37Jn1cPfxVfkv2Mz",
"13266hdYTjtrnsQvZHHo4nhuvd1FgswfpB",
"1JNiFTirGNrdzgFBXaxk97MFGeKq4uZaAP",
"18z3YMe3nc8CKTJNoXdYXS66qd35HqRPre",
"1DLmPso9oehHcJqQhRGUGpVXWkBzZiwjnf",
"12FEK6U7o3hYbbQ6gxTeZ3ojqpYnLSuMJe",
"1N6hmYEqMpVzLp5Bxm9QtR3dDAyRP6GK5g",
"1F8FAGa1RNY2n7jrN8rak3J3Ew3k8Jbwgw",
"1P5FSkzSrR4ySTv46yPTL3oq4DiZArGDPf",
"1AB1JkUizh9EXzrBBMRddukpGYU1wttU5j",
"1MiyK6qHmSn9cTtEwKrpHv5x4mXoAQQGKz",
"1GXuTgcoS9HMGTN5pdh6jU1J3AifQpv8vn",
"1BHPJaUFxfbfdD1kcjETqur2KLKkbgticX",
"13dTZAXL29vVQsVn1B7mCneERir1yPtZmZ",
"1BVMa8vDZGjFPShCt2SxvosHknmwjEcQyY",
"1PAC3JoQyCF6wWnZANHBqNDVrWDpDrK3Yc",
"15kXqiBu52uWcUDp3uVt6GPEp4zDYMuUKP",
"14XGBQvLdBJZrv9dwErhi4ZAmxYrrUEbzp",
"1Ao7sb39b5KxDvKHUEN31qAVgFy1qU5pkH",
"1GzXaXRCPrz7Ab2wW3YcgeKShzPoA1LMP",
"1NyFWktqAravZJq8EZChV4F9fVBHi7Yfa5",
"1Nivo8NyyiEQy3iMVFwrQcVVmQTa58bnFV",
"161LmRVhQjDvypqQvoGa2krx49iHAcsjHW",
"1L4Vf1veeUC3cQEXtWAg3yxriiSrDjSVZi",
"18izeEFCXUZDaebEr7RnjPADvtrRNunvKi",
"1FLQ9Std7jPGFRxZPVcmLgK7TcU17eVQwf",
"1LJoQyY8yMnX9S2tVCDJydg77hYFojs1ge",
"12GsyfWVZsZ9YeLJeitvV59r1aKLTPHCkY",
"1H6YGTyZrS2245xfhMggPK4LzGa3fMB7PY",
"1PzPPJGv6haiMkTQfpFDfVrepJQgqkAdVm",
"1Hr5St2tgBeaKwwWtsY1wFkGFV38pxPCF7",
"1E6Z2849DyT2CHfW1jsvtbFaNN7WFWXk8t",
"16T7yGAtXW2TLi8bUptVP6Pr6EEPftkoqy",
"13D8GTnfX9NzfCZEYV9JzmRQ2Y3xgw6Lhq",
"184ymZKnavp8mZDHtpsfgsZbzDinMNuYYp",
"1HAv9rZPfvqahnzKrRZhde2Sph2mf1VXtt",
"14aiSnMKnGoMF81fuekd5xieb92W1GR6Ru",
"1BcKk5KkPFHehBFXCtWnRaxngKmHpLwRpw",
"1AwWrSgvp9bxqyNcjexybt9t4XPYykX79A",
"1HDa5TGYDCfK6YeMm94pqdFnersSPmwh48",
"1BumyvxUpHptQ1KnrZHbCKrxVvJJi23teN",
"1BqabFTqAiprjSCUS1kd2hgPmEn6qctJF1",
"1FKLc1o67suTbFYRhvhhU8iJXEeVqo7CqV",
"1APbiLZmZNFWRHEveuzRLqJWNtmkD6iE2f",
"127WxvMS9gsiEkLgkjJH4MN6HszrsXs85S",
"1M1gBmeL6pseMJgLaEBqSqMFnquyMo1fkq",
"1LsPsxgT56nqQnngA5FeedBtrk5WHx4oeN",
"1Kmakmy2kReFfHaLez96GbuvhSKwxaufpU",
"1PzusPoMdpeehXH1gcpHb8PSwTdip7ejxi",
"19zeharKM2wUSAfKvMYQKP6kZhSTfG7Hpu",
"1Ew7vVRrnXuX9KgVRwWbmxfJm5uZFnuEyM",
"1J6srYhHrqWbyQ3rgfcLfbhgVPvS2oH3Mx",
"1pcYJDXjbv8eaSE8Y1RKFrEbNiP6bNvof",
"17CQpdJN8af3sWyX75wCbgkrNabLSH8nwy",
"1HtU7oMEs2FCZeuQNDz7ooxHtrrqhB9F9A",
"1GkqY3rZhW3jEn2P5jLBPgrvtFrweE1LzD",
"156czLGVJss9a625z1hg8TDB6X2g5oVx5n",
"199pzwc5ejuvqyPfPrYX459Sm9vMAmEnuB",
"1NRbM8h3VRPRhcsiM83YStSD7E6zh4Crij",
"1GejktyTS1hNofz5V3RUxJbGhfb5g5Rryc",
"1264LYtfekDohjWQ7yWcvWmwczseg4PXpg",
"1D3CYMNvxGxUurpiEftMWKht6wsiKkKkUK",
"14SbPv6zwh8SmZ1TXCrjVFCyFDX2NDVW1P",
"12ZKyqJdwevgCru8vYGoiHJhU2YuZfv2o1",
"12cQyveMWbLgcUrpU9aXEMcxj94t5MjVUi",
"1NWhg1WQ1rHoqt9GaG5LmH5mAPHmxZXBv9",
"1J44VofXpKMK5pzu5Fa2vkzsjr9PBnHSRG",
"1Ju72pRgG48oMLkgNSKi7zbZacP3RE6VHM",
"12p4M1QSNknoto9UgBkChvJ8q2z9VkUTLt",
"18viGGoAfifjyuPRyMCTUSRnTgoCiBqu2z",
"14mSRGVtzrU5cGoZno3PjUWXBVJBVBd7La",
"12Um8ekvtjj4pHxvo8SfQb95PMgafDb7fg",
"1JtPcktpPRK3upi29VbqPTsxh3pJrbmhv",
"17wfAZVudi1THPhoGgxni9qpTjpkqn5RUE",
"1Q21RangLUooWHujt8rJep3WL98DQ7HC45",
"1Hkf9CaLCcoE3DzaUucd3MgJuNbonw6opN",
"181GqaQ73WiYJtsgfEoKSTJf5ephGuUYch",
"15o2QiKdKRQHfyHyusaaZ2vfYus4TxBAv1",
"1B1Uf6xDobEEuAmGDy2icUQ3dQi5UDqRgY",
"1DYywKuMJMUZzGTd1eGG24tZCKWkPHU9Mg",
"1DaNeihBf65YxZNj45Ug3eeUisBaSeYu4c",
"1FGbpWCGqJSckuKKerzViRErjaHKkgJiCc",
"17mctebXa3PPQNVtJ5iPjpao38En9K1SqS",
"14dSCNmPgNxVsUXJ4z4ACXy6jnsw7xz6fP",
"1DGV69ztrfqFoXpvQeJBFKMmfx8vcrkUic",
"1Bsa9ZGZuRo7yhS3LLoXQkUn1Vhbie7ikT",
"1K9SDTtrVtSvYN3xYtNw26sv9Je2R8DcaT",
"19uFeavvtCvDKUdi1SDEQtNSJ5eFGzjkZR",
"15gQhrMB9ZCP1HmNhT4xvJZptSD8cbYDH1",
"1DzhVBY7ZMe1RZjRaz22n3GUnkLtCvju1p",
"16rgHNb5Y2HiC8D1aE5HUMKXwVVNNwTkHE",
"1ALFTTokau59jWvo8VyTDUPK9HhB91yfr8",
"1P1zCQHj1PmjufMTeiqbS5C7dvBAb8vZUT",
"16E6dxjDKEi4iJMS6QbA35jFCpNL8zD63K",
"1Ma8i9BvrMQUzT35bRDtLrJTTyRR5RZg4B",
"1LazcSy2QRLCFNW6ose3Bte5KE9V3gx1Pk",
"1DuW2raPffdGxhnkRkw8D92mK2sqqz19ym",
"16HJAnfqfGTjhd3NUbQouiXDSoUqcVttY2",
"14Bo51mHbjesu3sHZDs2hYZLXf64TYfnXE",
"1GwgHG8VRhkvEdLCGyQRY5U2UX3h9qE2Fj",
"179hp3qHvFBmb8PLTzNSkBSiJnD4UwfScF",
"19Zsksjr1SUukaeoT4SafQ5rfWA87hapsD",
"1AsD6PVFK2QcJY9tFPY4RFdsWkUtAzRmDh",
"1EuZDSw6GrJAVLpE3iVKC6ik48nfPHwPTo",
"1CL9KcmBFdEWegA1F3v8s1vxm1nG8vpf6N",
"16YWMZ9P6EbPdE39SpT6vYdZPhxcckWnuy",
"1JZNrqLHfbrFvi21Ftey1NAmxYTaAYbwE6",
"12HFtknhLWnYi1NNPZjVt5KyaEEkqCdUsf",
"1EwuqZwD3QTATbuTprknAvawH5Mx3JfNbE",
"1NDgw8J3rZWgZWSrbp7FX4AejQfYtvJx86",
"1P1CHrwBFNSsyML1tSdypcYaHsB6Fm3adU",
"1Ku24UnPJ2QdZHDfX7hXcHq7bq4DbLW8p6",
"1KEqVVWh6ZfLcDRDgJEpPdoLqREFhdomv3",
"1NDxacdp65rZetQCbh8tWtUK5swJJsDe2b",
"1Lw8rZEcCitAEfgH3brvKATZh1wMVJNNcg",
"115zjCZhSKthEE7eS7PsBvm4T5SrsreXbH",
"19z7vU9bBFn3RrrGtJK9J9CNXR7Hkcbksc",
"1ASpg6B6hHvvQFBv1FdmvDJW9eyxERpKdo",
"1AZS6Q8zPqznasK5fpBxBdxLG7SU4EgCJ",
"12oz179ya7FvtUjeg4nkUEpwkzxy33JeLE",
"17nPtSMQuLgsk9x5r7eyxwK6DF8eGQtVZs",
"1MQXE4E6ZeeULFBirbzT4Yn7vEct98vETy",
"18LGHAtYjch92nVHvgFuFfmrTCL2b5Cguq",
"1C5UmfXhwGZtwYZz76TP6jERdfZCvfGiWV",
"1PhtpkoYtjbGjDtVSQMacS1qHf7dJ9vVFt",
"1Nmmp2RRXoGRQvDmu3xEY4Qo8LiVqbMq3G",
"19geuv5dneKCJEV7iSgJsBejkdu3j45Zv5",
"1574Be8K2PFr6GGqm3Zxr33n5hT5mdrzDJ",
"1NheZ1yRjnX8LqRAWJmbL36dfs9UFHJiKv",
"14sK9sUhrvzQtYziUBuBcCKCU7CuYhumru",
"1FEL6DWY8KcAkJ3f45cdFtTbpLqYVFaJug",
"1AbVFWhrWgpPasHXuq9z1K2cda3zPx22nP",
"1PU72yQpkPFccYMttxPKrn92B2tQWRqfPc",
"1N6h96mvrQjGwPzqRBikmz6GEHMutr7u4N",
"1K2qCvgNKSM8BYwKb7tg5c3EmR4qRcvNf3",
"19izzQXfKNbAYL4fZ4EecbMvH7utSvw6v2",
"19WpvucV5AsFuJHm63Mwi9kASNRws8C8wj",
"18nP9TJAau946kPhymjpkJtVPRgjvHgxMS",
"16RZjkhhGhbTm9CrG5YVopZ6pY58nbNcZi",
"13pzc4unEi3ip8o3Gf8QCKrK2sXaoxvnD7",
"1BbMj9NJoUb4QJTerU8twAtw1SRtx2jt7M",
"179DVDTTQsX7Y5zetyHZHKhURxGXLwKzGW",
"1KHpHUZmvpV2xcFy9frSFn33VWMitJUEE8",
"131fyCAvkEanRPHw3oMC9Td6rXLhhYDW3T",
"1Bvq6ni36E8PKky4r1iNjn8NFyBAWu4h1G",
"1BUWXf6MyMC3Wxw61VJ1wQ2pMqApFSYrJC",
"1Pnzac3vgetLXaXxtK7aFsx24sjqsJxP4R",
"14Pv6XNq29cffdwPvCy7uJGixWdz33cam3",
"184pNBWg2HuTMa1LETQx7JnTRXPtRF9NeR",
"13xpo53Sn5PQF5mcrWi2QPB5j2c7cbvFGL",
"1KkdBkqJjggxfD5Su9G1TK6TpLQTJ9CZAS",
"1DbCETm5fiQmWBnKokfLWhvTFitxYS8iDG",
"1FD74zjNRnUycYwQezYiYPjc4w4hudwoQM",
"1DVJ6VsSGSCQHMUGvMh2wCGtEHKJuUZEd6",
"18dmjNM4ByRy1YCkvgJC3o8pAw6vme8mux",
"13T6BJjiNwRo3eugALNjhy7DP6QymE7yPY",
"1J9WzNrc3ghPtcbUG2eAMgdQTaRTbKTku5",
"1MDd7sGAk6vMsAQPu8sFUn1Z31ES3pg3Q8",
"19Fe5hnLXUPcqCaVN9LN3gM4xdDRW6QhJq",
"18ivC126FQvMrdVW6UyHZ7AQnQm12LFmHo",
"1CdfrsUtQFotKGuVxSi6AeYcAQY21R1YrT",
"1JDgUyboRbMkEHRYPh4YgPLzu9Tti5ZSRH",
"1KANKfHR9Au5UvP2yzProemBnnYwCvNaPt",
"15SEDxTTbE13w1MsShRXocCDBszBMufHzU",
"1NrADPfsajRC2Hocud7PmAh7XSstjFVuat",
"197KtUwexBBy3PZZ5vDSW2rLVSskQtv1wf",
"1HDKJv5VLaXYuW8UntygEKE6XRFSuKEfsi",
"1N3mTgUdDEfe6m6ozRENtY9g41ydzvrSQH",
"1JcBwNnUg4JixZpf2CLC1GpAZJHF5B33wz",
"14o8BAMgq5ovUrzDnksHX9FuAyigrCHGTa",
"177uxfUUYSuzKUquC5Q8HGXZJhApvaNBis",
"1CyNwbk5ihQfykdcJi8fMRFaYeQzkYWCt6",
"1Lm46XkvFADm71NEepaayLzKfYqFsExHQw",
"1PEVFKz2WjoKAakbPy8zGnZVimCwkWZnYC",
"1Gti1cAf4u7d1VVJ6xVpC8kBEbDqy1NSa4",
"1L8m7GV4EjLG5x1Xoy7fu72JWVQi9FFRGn",
"1Ho69oQ3sYWeaYDAsJFYSbfTEHfKZDJriz",
"18uhsMqbX2xG2dB16tj27U5f7yGznm8gY8",
"1Dv74DJW2cRpEdfXFGUJkoow6Htrg1eCQc",
"16cFMbrEaSwdF5TzvV91Yb3zFC25U6Bznc",
"15Lb5RvVz5rAKUXr7DFvLvNjrCQri6VjAH",
"17vHAgSQU2cjztjYCE2D2QoBMSoB3zKqHu",
"12yKSbRiQ8gFZHokGuEV4hKUoPyGWduCxG",
"1CxWtNwt5Tgk2J2VmZqKzPL7EV5ciUW6TD",
"1L99rGmGkiPgUyVbmWNMyoEU1qTYK9Wcx7",
"16UKy6godfy5dsEHMuzvLzMCWev7fRR8am",
"1DSez97HJtEERLrjGFrSeRNEecYSmTdNyF",
"1DC1G6iR4aHSaSVKV7HqgYoc24QA59Xjsb",
"1KveWCLmR7H6cCrZ5UdyTyQm9z2ifSjVyy",
"1QFB9uHDpm8eXzVacUGysQUKjPE248DiEG",
"172vo12fjxBTyZ3ELJbbqB86xt6c8Jg9Rc",
"1GN75XrqujLvHw8AtDSGKZ7N6Mmij3m9my",
"1M1xShZ56JFxiX3kaJgpybai1g9zVyEaCg",
"133zLKovYfkL5cSQfhRa7Mrj4VSc5rEqya",
"1DY69nHpR9bGjf9xxdpFMp8xFor8goeTVB",
"13XU8iwhNFA7sDGgdLeY4Cs93LxATF4hfU",
"1FoeYAs9u3k1ZqC72tsPgYKdb31RmFEkMY",
"18csRUBsSK7xnoCsGgNsUmcc9tVxNonKVG",
"1Q3kgKCJV7d5s5yNaMor99UsEbWqVLW1DM",
"1A6FpGWxKEdso5rP1zVx5Ff4wb5RwGyHrr",
"1JeaDgmLZhZ8RadC4MNWRMsgrWHY9a7tR2",
"1AkqTTB1fNgxinJTD89DXySyeHV6rs11Bv",
"1JUT9pKT83GKcZRjU8PtCLP5deLFN99Dpc",
"1Sc11j7NEJFrCFXuAAVRLqv3xLvcwmKcP",
"1P7kZFuK73n4ttLzvxoECZ6Uvt5nHqwmfg",
"1JXjBZTz5Cdgz3HCUMYJ7xHfHseaj7e51Y",
"1G1cxgPcnHuCJM79Hq3XgYwXqBMs7vjrG2",
"19jAdDjPCnpTnRmwHXSNCWg1ngsHhMjvkD",
"188qtZ3exdG1Zp3NcgJ8homR2xotNDCXWf",
"1NyuB68K9jevWVSxo85Mn28TQktm6rQYuh",
"1HoBZSbuggAWoZLWnS2Ty315XUcRpK8TBC",
"13nWWL8kEJicUh5UP8coWGLsXf2MUxzMZ8",
"17JdjsZe45v8adcwDnRNgHUcHChkfgkrFy",
"1CV8x2tinjZK73v34Z7gkNCy4MsFh6fs92",
"1MCPbsXMwksAYcxAcAbqsVb9cYuy77KabQ",
"15FQvCK5sFqFtnZgK1yHfJmED6M4JQppQ7",
"1CL7YRPQ4x4bVoG2HR75c4hA75Bu4TnQvY",
"1P7D3D5d3dhFPErNfqPGRqPD1zncy13pCi",
"1HoT4h7RBo17HRERhfo6o19tDrznUpvNyY",
"1CD3JFe2TBRUKMpU1zwv874S47rLKr9azH",
"1G7Fncnm3SCBFnFdTrFHEdWhChSvcUmNoq",
"19vBTHHFNuEVT2bt2YX7Ly2PkgxV6tGiov",
"18Yd2JNDmLhsi8947V8Jztrx1C1McV6NAu",
"1ARoDgf8GFLVQhb37udoHNxGs28CpZ5h11",
"1cNmpVxtuZumJvZfSQtSVoEkyA48nY1BR",
"17CbkmiHZaz4MkqfwHphRP4LKehgvg5dyg",
"1EfFnQNeXXBvvm5Bd3gGqHwDEzumqX1z1E",
"17RM9xsC21KJmpGviUi8aurwtUnDPukXTy",
"1K5Exd4SB5UvjPqjDVzVwPVNvpya7ygauK",
"18StpEJi9oxbQAMwJvrTA32anYkBuRFnc",
"12LRnFEqVK99M7aUkquF2piL1Luzzycz8B",
"1BMYajtwdczM96Yt6cyH2rcjB7WwfmPfaK",
"1MsKCTPvwuPQuKPbqe95vPmvtTuS526YZN",
"1EN4jP6C9J1trZkjCzvqPXL8nfcnw8APvE",
"1PNLe2VH4azVZ2dCseEEXhzRwreL73J1oK",
"1N4xEMz199eZ7H7esxMGRgJJw187x2WUWH",
"15zWwH9W6vbHM5iuDhgGxDNdnXGThcV736",
"1386kbwpREANekLf52DK9omE8DiE5RBfB7",
"1Bzd3ZLDCWMfau37Dy41zfz2zMiNany98s",
"1F9V379okuw6dVudcnhmiwJbrR57JmRfpf",
"175ZfNALqn4YJv2iJXR9HHDioMTMm5kMVC",
"164wTcB3arwh2mYD3VKT4cJosSjHbTq96M",
"18PcfMUJSajhMrXjD2s2cb696Bu5qaHU5X",
"1MSS42xWYS8hTpcGr2JrNXhT595edMT2mZ",
"1fZwUtxeiRYruoVhT1UqGQZKRA4ejcT4T",
"1J1iUGX3tj5uBbwLA5ZJVjkDH2azf9UBc2",
"1G4Tc4ggACpqqsK72Wgh88gLEJENXhTon8",
"131eDe7PmgCRCW4bVyGkWW7BkmdPMdrqSQ",
"13u2cY5eX1786hfGvfTfpKRRZbLJGQ5Jnu",
"164VAvARV7Dwwwh8bLsCEvQt22j8wdYSVN",
"1AQyXvCfBETj54ETJtFbGSUUc2kAJrxCDf",
"17X76VYGHTw9MCYFAvDyt2fgP7yRSqocCs",
"12yuAN92Wk4bYTsh1K7J1hQp2VF2rcUf3J",
"14Tn9Cs1kfqyu5wBd1TuDFX1EHgMtk1Nss",
"14Vt85MJGAjwznnZ1CKPEf9HMsYmuy2QxD",
"1QEmSpHfSVLDACmtdcF3y1zDU9yZEnAEjj",
"1cSSL7wLBs6N3b2dnjCrhLWZQKxbgF1uo",
"1A11QqdyYYU8pQHnHk6iWs2E1h9tu5D6bo",
"1AfgfnWtd95QQF2hzeGkbMyn4iJ3hcv5nz",
"1B141tS5Z6twoRfaFvDjii9EuyBdubMA3e",
"1DjLN8wNh9TYLrSi3UGcfaoi4obr3EUxBz",
"1BoC3NFX2iPRKtRvzWCyVVR7tbnHyzJohH",
"1QBHgsULRJ3g88U18dEN1qs4rPAe28JpaA",
"1FahtUqRFwgrXHn3h1wuneR5zkNGowzMTu",
"1JPNeGqowT5nUbXbWMyreiQYhuyrEzqRrH",
"1EcSNe6AmM6kiUEq2m3bj4yD27LV4YBLdi",
"1DcvV6snFFmctoPunj35sd4WCXH5amboy4",
"1E4x2frrMqCwkfw1afcGbFXRBrQ8FjyK63",
"1D7dGvBQUfHwQVp9QGLAgDJtBqueHEGawQ",
"1Ctjx8p7L7xo4hCrdPuRkiNWmmjNnwSLc4",
"127H44bd9hvQ3f7oWScqBWKsK31sS6qpPB",
"15Ti2Lw5Le5wcVurU9wFVNaKi84zGANQGS",
"1GtmJ2nHsqDitJ9qZhxrXbMR7nNGa8UQ1x",
"1PfxUujDFdbz5fRAzWb4r3NmfBMbdK9FRy",
"15QqeqNASbbpnaYfqa5ifRpREwvNftSSa1",
"18GZ6HovZuH1gEMWBFS8xg27x8uhxNn8vT",
"1BXWufVkQPMU9gBvcmdcqtsMovk66qpCX4",
"1NZ8iWyXGgp7Nhttx9Boh6Z2zt7mPwME2o",
"1KC9f1hX1jVR1cdiFNSSV96k9qWFHtyd61",
"1FKNyJetu7vYF5Ye9XFuZicqnrnwVqfPqz",
"1NNUTn4t4tqCghgQxgKpzkoYS6ckvRCUjv",
"1K4uK6MyKn6Nx8wQzTkconPbbrpiMR8Sni",
"1HLUCSawJFD5J5xqVENkgsvPNyJBT22RP8",
"12e6bvxYmkX7Q6fUgigb9TRimno8LMyVka",
"1CZQ3923Q8GJsL66C2E82cryx6Af5CjsDF",
"1MBBNeyZ5QWZVAUoWT1nvEwKAHTjbwkdSH",
"141jpwRgVwbTkZzwhCp9kdSyEjvXkUhRTf",
"1N38qZ9Y8vg8RrLG7k3EH5eEJV3e3kKyJP",
"1C9JrW4tXquY9DzegWbB1Mhm1t7EZr6fBb",
"15rcYHt1DMSBcMS58Pt5j3vA4izMAEBd4Q",
"1HXU6goCjsEA35HdVnsLoMLE9S9xfBW915",
"1Erydmxn2CmRFxVHZGUsNtw26gq3ES3k8c",
"1ADVpNjB639tnra37Cxy3PVyEwDWVERejU",
"1CE8Bc9KCLzypwDCrQHdfGUqAmeVrUjLcS",
"1MtnqdZBMHCYQLWK9XyZSaVvY3ecEbHaiE",
"15Fase6emSULj2b7A5C3sDmyYNQ2nJD3Ld",
"1FeEbHv4ReoAkSb1uzBRkt8Carhc1BPeby",
"1KHvCmWqv2XENeD4pkXVFSRDLAgQwTgkEy",
"1GqMABWC4RWi9pz2U6C3C8baBmdVfNF8Pq",
"1MgZZdeVuEUHhQHueFMSVrg7vTZ5zYQV4X",
"1QF6GisZoQTkHJ73DV5ZvSt7JS2DnLoyK4",
"1DzsYSL3fuaUBfgoGmBTQY46hJunYjWpPR",
"1GxwzDgwwvUG1L8FMU5a68qMCx4QjgwYim",
"1FMaDimtvKGWVHcra3wa9eZRTE3nXMjDq9",
"12uQKyhVrifaK7gnB9qoiUURAC1oXNZKHR",
"1MrJWrQRscsKqA2FnqatsMbiEXEokYbWm9",
"1LKCSwzDoA6YkYqeCUGka5jmg58xRm46qu",
"12KRCytACFmapghAWMtMRLc4ZfAPQcoHVn",
"1MJuDzNBiNUA6zcoh1NMmuKp8MZcqd2GVS",
"12uLVei9LfcuuqgSraWp3GCkRAEMxpWvRB",
"1JKFKE4NaG1coYEzr7ynrvGZSDALVoiMwT",
"1JehygHUjgYL1nyxqHwe5V5cNs91UjAHLe",
"1GVAaBzpck33vAAEX8XsjpVXVaT8zNPkC1",
"14wb16hXX1MVfUydQtAqUtVhUh78wqNVhJ",
"1K4qEeRwtNqW8xXrTE4J23xAFrdBKMid6A",
"1N62ynDjrJiQooGhfWduWMtdmDqdrqX6Ca",
"1Pj645yKG5wrXu9F5VEBMcTfdjEcRNQKD6",
"1Ps9dfXP4Qy2wkKdZaY64YazHjbcSz4qnV",
"1GeknLTsV8ZsiT97vsjQ4T71YaembJaa3t",
"18mat1drN5KDJZTWP77MdUZL9xcUQ11VCr",
"1GP3dmVNrsN5LReNcAozBTSmf4EM1X43G9",
"1DPaDFg8Nw34YCLS6qvQsh6XNfKeW1qNju",
"134f33ffXLiw8J4S4qLxyfwdRXELmwEfde",
"1AiNvuhBFf1oapiFyTs64aQHtonz1GrPPA",
"1FWSAUsmaPNJbma1NUq42W63MomYsdRPx6",
"1EpCaduBzdXeqq72WSQytD8ium5pVDSLk3",
"1KWY7J7zm9QrRzzoSS95ScDKo2UtDb2PRd",
"15kWim62E5N3smsoNVxjQ66XZAkazEAGse",
"1BSnTacTkmDkPX7V2A3LjEJbSb3ZT6zZ3T",
"1LnZGgNLXbLHZUdxmrgazsGLGoVKQNSCJU",
"1N2hwqQUV8t66MxkPWGRRTdMdYw9TVtvpg",
"1GuQ5VcwrfojNQKcdpoN1Rr7FupYvrjGYR",
"164EQF6qUyj7TAesiw8PodAUBUfeHj6UB1",
"148CofenkYjeQUSDZ3AuCgzsWH8wWq6XYa",
"1P5o7GrGruUoLYeZvoEWYq6wbswhjXNgYM",
"1fqt4oM5tASGy71kiZi3VRJ1f78JjkbEp",
"1PDKsP9ATfKtjsQN6MhJ7SQzWej1GDW8Q7",
"1AovExvK5smddrM6WjyaU4DUrReo2hRZF9",
"12ssBvDTxoZUWn47d3i37apZTbaUUc1ZiG",
"1NwhfHbHkzNJJbZn4NRAan4yKcd6AHB2W1",
"1Mgyh9vnoySULG2WPYkoEis7Co78EW5n7k",
"1PUDJtCYRgqwhh7JorFAKepqat7cWxswfb",
"1Cwc1gaGf3G4XKgMnnrKG8YaovqZfmc2nR",
"18nRzau9eR5DYoizJnoL1YG3eGTvJxqj9R",
"1QKGguDhYiySUArQhNmQ88vPQuFQhnba9",
"1LhT9t3wGRcCD5EQv5EMXCnBi82aWu1JVD",
"156Wk3FyjKELZ84vou4qcVHF5wAzFurXMA",
"1AdKNMaeUoa49vnaf4gAfcqFpfz3cfVf3G",
"14gpLrngQZpCXm8aki1DRoYK7peVr37NVd",
"1ZNxamY1xTC79e8sJ7QQfFyr4r4nqdeBP",
"1G7BkfqVUdCjbZwagi4kV6ddGTujPzAKcC",
"19sAW9XaayWfs6vaEFPv4T4qnRatCYSJ8e",
"1Nc7zmEpWb1oGTXCFPECS8FYvaBKwBL1iR",
"1UrsUSkvnrVBNRUBbuRHpdWcn3AMwgaso",
"1KUbaeypBraMF3FoLtuYaGAXgcd3Qr4P58",
"1AYQ7vUsCRZs2FsKvh9eXPuJHUCJuCaWFL",
"1fr8iPEzA2pPUBTEdjSNk3vVj86XAv2hn",
"1GGGxELcmc1jGNmdWMChu8W3khtoBKDDhK",
"15XbSM5bER7mePXyfeRqdQTDJGMS4bPw78",
"12LfmkVYJVf8R2sk5Su7YswUbcLrhMVCTN",
"1M8BEqQBwn2ZFGU8x8ywxbj3zXNUUH97gu",
"13iDkp95fdHUtojRVvfF4ZafAEwUM57dgh",
"15nFL6qyoGVULtuSBeDbUqBccYDqjp9iUT",
"1PHHbutik7yhiKxEHD84QZ2karJymYLAH4",
"1ENQuUk8z8P2rce1EmttKbkLtnpafJFCj3",
"14JpoyBB1vQgxFZAaAcsGBZUBRV6JtmDjD",
"19s7E1FSbpXAnUUCPGQ6MMpsg6ngDNMa3L",
"1Cgwni6zBZp162yVSUc3gcJFqUERHXftJX",
"14We18KtwoYqJ1669xbwwNtRNVx1GPQwed",
"1FaMj9KCr29oJgvv9wLecmPnqN4LGqv9j",
"1THGDSM8nM1DZiZNp1qCkUxorgZo5hqYE",
"1MJppDjWaMUUjP9sq8rkGRpaG72aoXfKN",
"1FJLTrsti5tMXgSdaoDdWXvKKTd2xmn6rB",
"1NHfN7N6DLSgzHEy23vNZvDeM4PfDy3Eud",
"1QF5kMSioL6eaff7CDAXZxwRHpv61oN6MQ",
"1ETzmkMxiq7PhchkSU3ARnkTfZUnXkUwvu",
"1WanpdGYpyfWZRgeTLjuEhJ3g3Wj1eVK6",
"1A3V98TxT4yTnPcEKfQUTcBFhu3otHC1Yo",
"1AZFpNd28qr77QQC1AV4AZQmrf9fRXE94B",
"1NBEMzGZPbrcVyedzPkCQrorXhzfF2oKws",
"12BMryjiDj6f2XxnpMBcKyyBdwQJWT1ix1",
"1KXGJZ47yMEsTct4KHYmX9d7qaUeiJ1gTD",
"16MedMUCjED7itQVYKxz14H5WJFf5Lqc5k",
"1CTLbbZH63zaeswnUWUjMPc4mwUNewmfK8",
"19Gus5sE8R7BhxzB2T73RwAzwGRSCsCSDn",
"1Pebsa5fpr5XmojpU4wUfmHojp6B93AoLr",
"1PmKZMi4XkCDR9zMM3aXqKiVC6H4nFYxff",
"18mTuiR8Nyrhp3cDp7T213TpoqLZVQccQL",
"1J7t6cXhx4xTeer2ziZHpQtUiHvtzaNPpH",
"1GrK7rPK26RWBxMXBhFL1bsDzP4AZWmR4Z",
"1L5tDSoaCiQX5wLkHmpVx9LpCVF1wmYBA5",
"1KH2nJ1w7xmRpKpNfvrJ2dDNar9Teoiuf6",
"1BStWXHyVBGJVKnuCAjEVfVN5oAThJcwwJ",
"1F722W3B2gjnXzci2RDAr3DZCoEFibiWih",
"1LX5B42scLzXqor4zD5hRPnNV3buvpdWig",
"1LB5ATqJHAMYuUxf9TMxA1S4LQPU2xfsfy",
"17B6crhVvHzy33zyTRUyy4tXLMb51W2KBt",
"1HzxohVKPfev3VxcjeuUzr8VoUa79RyGgv",
"1LjZPciPNfgcGKQa8VRhckT5iegwPmBNWW",
"1ADnJfEDtwhcGbHuZLMxcmhrHU7U7DybN1",
"15yZVLCKPZ4Rd3GCy3cVVwqTYofctYZwnZ",
"1Nxozkur14yBxbYkK1gB1bcJuPXkKtmwvV",
"1H8pb8BBr7BMejTwMtjcT2nZD11Vz9X4Lm",
"17cVWV71pHA36S1TgYGpXr3WPvSJSqXfmp",
"1Jc89N1eTJTZ64kEXBjQYJdRJ3skrK1nKf",
"1JwTwv3Unwjf54YwPbRWQUPCj2qvfFrTtU",
"1Hmbk2EuQQhYV7LRzPn4ZTdKWkccf4UpFb",
"18NmrAGhRtkz64njJ6g7ZbcrWsyhua8WPd",
"14VFqzYxHWBZmaYDWMpuoaCXAvRToPC836",
"1AHSxrBSsd5BYKcvQnHdsAUQNzcqe419fJ",
"1HjLLbvvpLGB8Lv9arejLZknwxj9hrG8YV",
"1Gm1QY2YqZYhFT4Npp94CCujJ818qwcdGE",
"1JfFozf1aJ25H7SYCPcrZJWJuivuqjLSvE",
"1NSPPHZz6DsroXppEsMoX7niyEV9wq9if7",
"1KSBnya7gcNDkinFsPCN9588Coyi1j2xrE",
"1JDzqTvZfqExG6Vhq7mPfoTW1nL677KQq1",
"1GLYHSKkqxDoVsfCdxr4DmzF6KMxrCU6uV",
"1Mj7rfpaySSoyssJmUGVMWUpv9eVUaMsPN",
"1ioUN4kbfHwfGyamPpHdxqnGuEbdAvAHr",
"1PoUvEMX6bXHGNyhfVQKLzM9chhajHQmFj",
"12g92pjtmDtxMW86BuxMB28pkqv7z5MNcK",
"1Bu9vvqeu4q8UnSFNg29GhN7z5qbVJi5tx",
"15gYWYyzeUgSPqawhMRZ3CPPWsGSrAbHAF",
"1CgCA1yGjEHRsgi6cLLvjTAbv9VjksBZup",
"1Gfskv7c8wDZ1PzAqXvUzrjF3R8x6Hn6G2",
"1MrLgCAxAQ4EtKyfRczucMTXnNMz4gYPsA",
"1C1gKzATCsHNTtSkxWKkjyHVdSpNB6GVrM",
"18qAHq7SMCsgbdkqdAPmJ3YPj6icDdJ1HU",
"1Gyp6V9cfDqEMaaDQXoP3o9ht2hRyHhErr",
"1FewYnBHDbbkJH9FV7yxFQdaTS6Vn8JoMn",
"12SnWWdNfg6k9zidbrxygsRA59kv6heHSF",
"18scHx55zx3yffgKstzs3GXusU5FFf32fP",
"1AFNN1kCSc1MRLw6pc1sqc786HyU5E5uyS",
"163g6bReTtzjidTqKCLudWHAWm6sjUdDp1",
"1FmrgxbcpzSfqhq8tkBeKYaaGdCGjAHATh",
"1Ey6jnbW6UMApXGNvq4LWnmrDvuexTpjoz",
"1HKM82aZW3zAAwkGZVYqLMztPqY5odKGAU",
"12c7FXgUCDNUSTUPBJSvrSjzgy3aqG33c6",
"1Ez8K57m2acy1vZGfLuCwEQPRbizfEBbvV",
"1qRstP6rq4GqDWs9ctBzedCAHNo6MwsdP",
"12mSvux8UhmttAKHfKcSWu2UiuxQDXLncA",
"19mknvMSLmEcPtWoGPK4sDudNJNUWqgWQm",
"13jR1hvVWMXSYAPdg6bQ1DcQcd88DKfKzm",
"1Fk3tCo5sNzrkY8Q8dhifbTjMrWFN8xG9i",
"1HwU4FZ7LnNstgiQQPTJ7od7xLv2sJos9V",
"1Mh9MFiv8SjsJPjgSoRmLJiZFcKSuwG3Tu",
"1L6TFNetUzUfVJ2umvrnrJbKZ5naG1X7vj",
"1LayvYhNjzufjZTA1ooRspkW4iMtL7DnDp",
"1HYVrLUja9W6hVBTJ7XUBruwQA7PrU3cbN",
"18i9TxtVMVyZPtrvvHu4zrPPEtn7GkqNqy",
"1Hsj3RLDrBEk3hU2bJZ3Np7kyet19Bm2Er",
"1Aj2BermJAMq1NS6CJzoCUjz8FD59V9Lzw",
"17fTGEYvg3s7edAbA1EJFhmzyowi7tV9wb",
"1878tM1phBob3pU6eMK3h7pmYxBNiaS4J1",
"1FaQgmBB54hd3L1k3SqEgUsRbaun3nyUKs",
"1ESCQmvVm4wqRT4XoR5RqdNiJSwYrj7M1s",
"1BQWdgCKn5hd5iJ986Fio6D95qZK35TjV9",
"1EnhiViVqCijS7gxQPCbLXcpybpjUL49u4",
"1GUVFHKJmKtc3AFFwV5gy7811rZmH6abpE",
"1EpUNgkje594kztVm3jTrwXePU8Wy7HGku",
"1MJCDUxMqvDKtmXccGNWkgoVitXSfAPFFu",
"1Ac6Vg7m9YLJMZi3ZpA4JZhbrG7McXZw8s",
"19rmjzyw1JyqTKuJFAiAjimuGP1JeVKEEM",
"1EXei1FRqLY9ZY7E8fie81AeNkxvQq1YVP",
"1LdpPcVYiJqU2fnrL3pxd8KQmin5r6cQm5",
"1JJPrjHgVNcZZzg7xDKQKMZvjprs7vMc6E",
"1A2i7JHednnfMVnfy1rEXcQu96rPbnTCvW",
"14rLWC6xvArYyaxaDF1Zb4hS83SEQyE8rk",
"1MrpoWLL6CKHab9JF5VUSJo9XeYLMYAoov",
"16mpN7sY5ddavt5Msu4ZFo9h3RkCL2Udee",
"16mo2wWqW3PXeBgpPak5sogB4gyDv8WqxL",
"1GGmuiWJQTAdsZ9rF5zQfFx4LniJtX6x4X",
"1Ji5UPEehVJJKNcDNDFbEJ7USAw1WSpjrW",
"1CaayyGCRKXPJ8iDcsPHyM5pT7jjK4osQK",
"1TpUmargLvC7bf3SZg4XPYNUG5FL23TuA",
"18BfeFyDp6R7Qi42qHz6qAYfaCzWq1BbLh",
"1Bgjj6gMMMwKezaz5rHABCcZjfffNq8XrY",
"1ByMN89KihCZyr3FgV3vcRGzA8gj1gRTXP",
"17g2DuTPQimpzhZd5c3e2xuJtBtj1VUdox",
"129EBNrTW4HEqcZq8dTKo7k9cemdNaXwV1",
"1NWPmrRKN9KWLRt4Vmqo2Zd36frmbScqRu",
"13XFYwZHTT9NwudR1qGRCDGCPVt6GGNJjE",
"1J8W5hziySLX1JXVZRczL8eFC7QK1RzCj4",
"1NZfdZrZCSxrAH7Vb5Cj848Ygb1AhnJ9FZ",
"1PrtH9N5ESCkGv1bbsmNqoFJdDM7HRGwuu",
"1nYMBr6EE1efeicDbFWVNMVQDD54o57DU",
"1J9LdQwAKBiF1SDK7gT1wxuMKpjL5H5Brv",
"18KnB7HN2ndAy9PhLfR3xBLodVz7BiFakB",
"1EfEJ8ouKVnC33YY7ckCxvaQnAfAQLVL18",
"1Km6Mv3tcR5PyY8kbVN5DHW2d9nLniQwZU",
"1ExzC4aG57QZ7517mw4xvyimHeQ1WvyWjg",
"14zYhKaZkTVvQQushcrPzKnp6nzVE8EQ7j",
"1MeAnqigz5P5mbNn1JeH8xrMen2cP6QfuQ",
"1BHthMgLxHC36NbJtPpqojsk4ECfMUcwT1",
"1H9tEzpHRaxniysSBw4NFVN41wqwWijVGF",
"18oMUQWsJayhPS8CK72HzgHwFR7TjemTGJ",
"1LeHcAb85598fkNJxR2xzmhCuayUxupknT",
"19B9snfUAjEwH8gMJNrC5Vs5Zx4LSQ7R6D",
"1w5bG8YnwRXTHg4EutEnPpvN88gi971xZ",
"13c18RH1Cw1tpXXrxZYGzasbHk6ojPBuHs",
"14BwMQv5TURDVXV7H9wEcTEtQcW1GgffuW",
"16vdEchgLJhKBkaomLYGJZFhaNNNa8FFD",
"14j8N4o2C2nnYYpwhWyJuCK37mQgFpw8wi",
"1Cx1M78rKzVfQbAWUSH8nZc8QRmm5iHukY",
"1F9TgyoPrf72TkqM7w23rh6EuCyQzoxA3L",
"1HUWAurQo6toXkL4PJgHenqekdABuBJtuK",
"1GMqu33aaya32gD9swmZ7LmXeabd6z4iHp",
"1Gc5s2QLVsAmWMpeukosMBskkWVJLqLtB5",
"1Q4XSqJkyprwqoWFsNPrAnBSCR6oNqsond",
"16crH77NhC98BFJYrWbupGsVW66ZoCoDvb",
"1GY4mBbKC3f1ufdLkU5aXRfjwW68QtiFCZ",
"17Ng3BeKErKWBd8uKMUb9iDkkUk93u5Kpr",
"1LrWfRhLWVux3B7xEcoJR911hCovFMecCT",
"14sBH38sgmyWTLzCsyb9vzhavSESFiwW6x",
"12o9y28b6YbiR76euAunYYLJ57oLnXbKe2",
"1KamHX5JiwKgSyumRvDZz2Uvgn7AoJ3Mtm",
"1FmtEtqcFjUoamPANooAnvKV9euaanUgvb",
"15QritXX5AhrVkqtUAo9XeTekrhK9fDsX1",
"1GaNuZDpCqQ5qz6hpmkqFdzWsdMWW2eVeS",
"12dLt4JosCKYM8L9oNbfHEp1BeGuUEofnA",
"1VK5i69nUPWuVAB5yZQS3xX9qBMZqXuDD",
"1NfFQWgc5GJeaoyEntzyeHcquQ12yWDb5P",
"1HB2jHQzZ4a39FUt8SLCEJFmBPCJF3HGtC",
"1BZ8mYnWAtwwCAHq2kF3e8NXFTui1p5CiN",
"1Fvy3JLSWdaEVew8bnH3u2qDe6ihERKxix",
"1FLrs2knNuD4eJpf4qC3kPuEDG4p4hStv1",
"1CzqXn41sSH1fRwoiXBy8azCfZtEzrmhRC",
"187PNEdi6dg87XqwGgeZx5UVHxeGiCD5R5",
"13tJEvdEoxm2Zxyfvc2bKFCHPWDpz6bKpB",
"1PAh9jakt7AT5eVD2SyYoRhgxatv5Y1gh4",
"1HerANCMTn4p1dNfF5t1puDC8Qg5wzjH9X",
"19SJwR4He9F6mnkGevhvDsTP3xDp2o6Lpe",
"1Mt2mf7dr4DzMJ2C2zqfP87nfiKwnzvvLB",
"1Gn5zfzuv6wCvicka3Pnq8EaQJ2pi6ACud",
"1QAa7K4pdvPkww1tg5cHzAfepzXkRxwwqx",
"1FjW6k1SZN2CknYV3aeMN6h8CRNaaQT4PP",
"17jUQSZGezwuYiUQuNB3Uv9LwWp1sm2otR",
"1DzrbxtVRbW56YChVpxHxbhUZXsrKxgA4x",
"1GgqajAyRUbn6V9497Ch5tPEcjQfsvrjyc",
"13waRkQABFJ96VhEGLWT3fY1DN9943Mhg6",
"17c8oLyft7jUXn9g6oZuGTCww5261MLMf9",
"13UVmyT14zHGFjbnAq1C1uX4BqRPhzqok8",
"1FMYeGqeXxf6qs9qXcyBcLPnbdEbPZwnxN",
"1HeZd4fKZZv8Qydci17wkF8V2okZZJr2Qh",
"1A19FNBSTFMm6C4tm3L7HyBvB6qQQ44a1Y",
"1JdQm6iTXWwLDtDRpsydCz5fn4BNuMDrWF",
"16brubjQVdPSzoEk5GoGyC73AhreENp2fi",
"16NwDCMasaA7hwRJFYQ8ixjUSoA5sFFEnq",
"1MuZNny8crFRqoBW9t6Di88yqWNEJ7s3Y5",
"1ApdcoPutfLBXEwXQq1JYV6LmK9wmJukhJ",
"1Hqgy3ykJBZ7Tqy6GJEQB4psc3jyWUzfbW",
"1CEd1MUYSPRXxEDQJxCTxvpZdVxya11U2s",
"1MBxZ5mvpZykuH2NsgaAGmB6TQdE2p367J",
"1CeDeNTmFJaoxT4eGAUn5AZUW78HgDPtVM",
"1TFGhKwG2jFqZ8VdRDCm3c7aVxMj8TBVR",
"1JtxAJFGqM3JVvxNxKEmZgdVh4KBCBHbCN",
"16SCGntQZz8nuPcubKsJoBK2YdjeXoCMRh",
"1CZXCvVLDt2DGSH1XiZGEDg7kcBWjsK3p",
"18NaS4jW1rUiM713pd49sbuHJkd88NaEEW",
"18CEomirWJfhbVXPF3HGhuQXRgA6suXHJh",
"1d3enT71xxEiKKyQJw11aa4HixohxmUZD",
"14BHdELjZchKHxpfzFBtH7qoN48ZQmAvRE",
"15as9awFktPtf7cxkccJtX19v5cRtLBU1D",
"1GacJKbkTD8Pp8GSgHW87nhKERfmZ2vYRg",
"1P19yLchfabQAorqWeEgHu7fm64m3R69UU",
"13R6y9LvVhinnEgTSjvDWgpY4JBzXe5c4X",
"1C9Dn2gEFY8T7Gi5HCsoxApQMDbZSsgti9",
"1Eim2mUrga2k5HYww9A8SQ7Qoa6gURu2Wq",
"1K521KisgYUfqxkHAJopFMYCvyM34naT2b",
"1D46qaw2ZqVHuGXqA3a6oJk8VHAASPTTum",
"17tKJXx6CPdK87GdNscZgGBwP7hYiXs5Tq",
"14KcJLEWEnzjquAvfzwdDvX7zfumS49ULk",
"1fnyjqruKLqGwVCdPwMK4PMu4oBEtinJj",
"1Ps9z7KxkuTTUYDVtz5jZdJj2AbrWjcQ8f",
"1Hmj5hLwoMUnZjAqb5y36CTkH97b1MDWpK",
"18Xn836MA2DVEQwsHw8snLa3eyK7nk84Wb",
"1DWbjjMPsoNY8PkLwCaS7kCorzM1VaySgG",
"1PHtArXGWmNeXaqyqgS6SPyr7psBXHLiDi",
"1HzdjrRbzLuoEVWLhZpvv6cEiK6rpVAMa5",
"1QH4MS8ovHGY8q15hHkEJU6WJSjXLjYrqy",
"1FMfXqYunzEDzxEgrKEiKQR7o2ZLoch6DU",
"17ZASRtNe99pXzrYpemEnuBoVGsFuHk3rp",
"17GEmtE842KviqowqNKMJuam4nkc5kpUeU",
"1GXGTCQ38awFuJ5n57TKWa1DKcaqmnw9NS",
"17ENLAZdcSPyyTqcYdiANrhUfRTn1gnJT3",
"12q8ECMoGDp6ZaYuPxquB9H5fjp7CknvNB",
"1DtSUFBA3ZL4V7DXhxXyjzm4boZ1Qo59E1",
"1KearMdZ5HAEJxFLPjrhcbTxuhLbwtVbA7",
"1Bgg45FxnKgFS8KGSBCnzkx8EpXmgwdbbu",
"1E8Y1RtSaJe5DhvPU896hajmGkLDWsZPHD",
"1Bn6BMLQktbXqqG51n71YGChbkZ7KNQ464",
"1s6FWqpgJG4FNJa74A3efqsrsMzYpei87",
"1Cvs4HbHhzns8XpDrrVn9EAaCSJoY84kxY",
"1Ppij2iuz4B14Jxp12XrynocngoDtDaeP1",
"13nFwodyUjrCNK3DZy9JhB5hZg5Xyb5zEn",
"1CM9Nhgrw3pRRNZPvvVnYNhwJvcF7yY5F9",
"15h4CX5hjgsL2PRTncFXGePonkEk8N7sVK",
"13r8d4DAp4Lc6yfpGLMK1K2g4AoHZ631FR",
"1C6r3EMZXZ2ftjR91ew5uwKuQJbzj1gVLr",
"1NzXEapyZo9GZQjmSfBAuvbnVeZ5F3zMid",
"1wJP5XPmQwDZ5CJEqZNVaeC5qHoS9iHDw",
"1FZQvuicsx3nsgk6GD2iRp7wt86HNzbJFR",
"1M8B8KLGK1ceUTMvvJC2jjxPt4y6MfdWyc",
"1H411swGrc9m6cL3zyY9p6pDQXgzvy3Yf5",
"1E9EVGpwwMgHzQcDSrPiHQUmQkzF2f9aJj",
"1HhwSonc78xHSz1YFMWtCRg9mXuGwxpsH1",
"17SL1SGBumhqwd91n6iM8D5uxjdGaM2bsS",
"18soqwScjNDGQBVnBGE32PS91T5LCG23g5",
"15PhhXUgj1DVkky88SGqkKGfbck8zxo41C",
"1JU6q6sCoLRugmcoqFzPZQ4JJXq71nW1Aa",
"1BBpq4XxsSPqew5VKG5E19VRfRDy2W5oYh",
"138d6ipgk5akMDYd4ccxCN8TK1FTe1t6aL",
"1Li6rsSytrr5UAWMtCakbVCTMN5XwQRq1L",
"1LeNyJoqNvudoMwMXnjWy5RsFvrqyZxZ6F",
"1C5vnayVCbRwtxqKyYYffB8keuTFESFywp",
"1JTA6oSgQKJVV2Vn4XacZpudSQejywPb1U",
"1DLAyruzihiPSLfZrLr5ehZkRSZXYixDXk",
"1ApkXDFotHH6ZJCdetu8cBJXzA4Ah8MmBe",
"13zhbQqV7nh1LDkXDxhXAKKnzYD4FYRtJn",
"15VLw7EnG8NETnU74c5bTfoe3pubY5B8Da",
"12CrDLdrSreqSu12G8WT3hm2oRbj4WMLcW",
"19yojKb9xsP6QQ7aExkkRsD86YB9JYcTmD",
"1FAHMo1KvaLVxnZHyCQSotLyVWa6au89ve",
"1JojYTR4yfY9bzhDLcFjsojC5hosZ78NPa",
"172ufruyTUK4rR4BowDCefePAvUM9wSrmv",
"1PRnFwDbQfgiw6tcx4W3E7APsLyTzDuNCS",
"1PKpBXCc7xgXdcGBPWqybRGN6nvXowwHqN",
"13ymZ5CagH4GLCyhDVctDRSF8ntQk8YihG",
"17QokTozBkV7fhDtTqx4aPHfvntn6VyQ2U",
"16qdkNb6tAusNyG9rCxVPEL6kBHB4szYqn",
"18JfsEudY1shMuVhcun8jjpSjxHjQZAHAD",
"1DUAiCRnUpEhs3x1NnregQpHQ1VEv1DkkR",
"1DN4FoS4S7czoYLkbmrdtJ2nVUJJvpPYLH",
"1521RiS71t9PHEu3qtnTrpYXnnLXDVjdBe",
"1KMbTxfWkR2YkJHRVNgCmCnXkcG5pdSguo",
"1FgNfS3JLdJFSAfw1pUVQVfvH7eivqoUbE",
"187NPRPmp45KTYm5SdycdaAgY1gGaTS96h",
"1H5sTbfrTLcuK4UqPkcQNpzB1pyf8EvtEy",
"1L8AS5B2qbnVjZuh894q1Lpe2iZT1QdbYo",
"1Q2aZbtekjQU5SfeK8q4K8qpX5iN8xU8cS",
"1Bgn2vqJWHk5jKiwUZwvoZKfDANDnhTLgS",
"1NWnf7LQxAbRAEpUUgunXUekP54fAYgQpc",
"18b9tASQ9A6o1FwXpsaf9HHpgM2Qp1gFzD",
"1QBNuzC3XbKtVp57T1JK5wdtVvQvq2tQTb",
"15qfp9o9dB6ze1GZfm1nAw9CtohQwaEF34",
"1HrvdFQLMyJD2DbCXin3u4P8JGosNRdpaQ",
"19uDFAUPDdLHKo1DBToZTm3PxPUGTv4CKo",
"1B3zTQxn7rCtfKvNQ3BUDKsjbj7bgdRGcP",
"13ukxyfSt6ym4wNwMmEthXJZArEG7YxLLY",
"1Kz1QbjVCHB3tVjosEHT5qkvupHjFM5qQn",
"157Rge1boShRRHqmy6Pf4ypw8biufCPK1a",
"1AaiwxSh4mXwPZ2GpZVKUVvMBxfdLwebVn",
"1EqrCnf1qtSkAJ62pALkAHk85oYSDVcFLg",
"1GaKtkHcnRtiGmCiueqWk8DqfLkmeD6hLz",
"1H4nJkBrKbXS8WKv8bp3r3gBm6MX5xihJV",
"1CnAydFfuTHLZsTVFUmFskL9BzxGpGcty7",
"1JjMtW6VmcKSqU63TB2yGq7rxUSfM9BLQA",
"1Q46LS4ax1rCSVPWR2opkzeZ2xXY3k4Mjt",
"1NJT3CsHtWR2GfAZcbZkrt5xXq9TAhwkp6",
"15wHAHV8JzFgconjy1srT2aSDJGEm14gSG",
"18fTDF7yqonPnUJ3rfJiAj2R412b5bA9QS",
"1NbbRTHNvjsXpzPvn6NeXwFXyF56vjysQ9",
"13LhhEPhwdHMGWuJ2tDCvbNn7H28tjyKo5",
"1HCTpEqs6xMgGWDaNRo8cHorBAt4BMpxRc",
"1GKJjs23cgqXqmbh781tygUbaeFucFQnhc",
"17mDvtHmy4sfdnVqfXXgkDmvvmTjnmnrpS",
"15fN83SW2QYmwSVK7vMnBH4G1wrhN5kszU",
"1FgEudPTicXvWKViZYQT3gMqoYDY51vbgf",
"14QZAyNYe6psWrb6aW3f3smsCPzk76a5XB",
"1DPGSvXpSVPpZ95uxLcWtqRD9hSb56AFr3",
"15awfAQhe7tkGfBQp8qTnH1uiczrq6Yv9J",
"1AYeaomShoKx6xKkH5UYfg4jbj1VXuZi5D",
"1DBA2zGkqPgjgkrdQfDFCYiFvHzxu2oYom",
"17gGfLSmo8MAcYgkV1Tny3F8JuifvZddX7",
"1CQsuMjkCFAHQfKDhN4BRpcUpFcRsPHCtm",
"1HUhJuju8Hsx1jVsPgjYNngujSujeVstmG",
"1AiGBWX7okgSvnrDwuwDxsyGktXBKtVeAq",
"1Q9jMHwzY9pHrGfERSHNPTjnMzBDxtM96D",
"1BxXxm7x4hxp2NB5mFmB11jJszews91vCN",
"1Ad74kpwNcmTvHipGBfoGJkwebEtdNVcJQ",
"12hD3U4G7zYUCTBeSVq2rmpxTGUAobqrjc",
"1B6G78hwQxAjmHSP2h4o64RtP9tJPfDxWZ",
"1Nj2eZYGCZKJ4HGvRpoXbFz5pMUSGPv8s",
"13ZrF4PFuQJbh3mcFrkvCzht1dAWBVP2DF",
"15aH8BPjq63cNDFGVh3oKQ5tSzecGKDEHC",
"14eF2fPKyu6wLjB51yHDGgsbLDhuWKZf7x",
"1DnDFJyNqSP5D2ctre6NzSUNdZxPJc4hmn",
"1HQQjF2himrHXZCkVWtAzM5WMkxftJpzQc",
"17cmHyD5vMBvifqQ9U8sgANMeywShH1XRp",
"12bofmEwmToeaNWEmZ3rGiTtNGBtf61ZDU",
"1JjvwPgpcXGdLNBcSUgdPeovKgM9ZQTuCe",
"1G6AtrQELw1NL7HbWTPMrbS8raKxQqDLUd",
"1CQ3cxu6AtYn1NrRNr4v34pGVsuFMgh452",
"1CSgzEQ1PNGxuqts1w798CVVqJHiHcowGF",
"1Q29VEM4UYPNgGUV1eEoTAayHCaH9xsBBh",
"15qUfKsj8hcMhKegQjHEM43bp5LuHTVNUH",
"18QELBNm2c2yKHpMrvecThHrbJyVtFwf6e",
"1CDpeaxaQiEn8nenUHWxcnS1T7qr7BLRY9",
"14AEqE7iiGhkEnqBusQhov8D2r4tPD8K3N",
"1EvUm3hpbe75Jg69W6jSJogkhd4ZNmPtks",
"1DbUSeGZxSKvEjN1gk1u5hHf3cJ4r8MkY7",
"163aW2uScuPkPAxpSyDoxq1dMW8e8KxaKb",
"15xPAXGSVDSSFS2j1sXyNiS4Xp4DNnKegP",
"1DWdncUjKHfSzY1a9Ky9QSrjBq9raSijUU",
"1HCgFiit62C8rVTo2TRsakgUQ79HGtnSeV",
"1DF6qk3oa69dMk8pihHXGuBukoMeEPNXBQ",
"1Myg5MdHCF9XebHNbnN7yqXtqeLLShJjLR",
"1HsdQMhQbAZ1EwZo7DtJcD2Zjuj7wqPsJM",
"1Jopqd6ypy3XbynJh6H4EuBHYnwvVaaygo",
"19GMfMuo3e5wbVzGgXtJTX66J22txiSpaF",
"15aMABVLJPr5s4iXdGnvzP951y4RwHNMJc",
"144FghXGVyEaBFa9LhPSmJx7vTHcZs8FPQ",
"127CdJuP2vvwqCy3TFj5BJD1mfjySkYohp",
"192Je7wrwotbhBrDSzraoJDWm8KwUDam4V",
"1JVQfBHyaHnJqpadv2U5r63F8EohDGkJxj",
"1M5NXykBeXXkS5kJcGyRXVRNDU4AKTC79h",
"1DMKDXCMTFDoQXPkjv9v6QftT7BCjmucje",
"1ERwXBGpaDidbYnuXwFvxxF61dMjKqsTvv",
"1Mm3rPxZTkL6pdY9FzoF7L8NpSsKg1wdTW",
"16STv5z556vePFQzvuLsX8Yd7KVrLpgw1G",
"1Eu4ugQZdRkykQVFU48Hcx3P6w5S3GQGoe",
"1NS2FevpiPyGazbqBUNcJ2srmYQLuUAemn",
"1DpLa8dVk7j4qUzSXJgJqMb3WafMbm6aSC",
"17kB1RFEi6DVV6nD1WhPET3peHYvFBiNHr",
"17gRFyh3HXvT4xZhucWt4Nbd3mvFqFJGwZ",
"1D4cQbHpHGVGcYzsiGW8o2f6zttikU6TA7",
"1FEwmZmKXX1LCRW1GLW1rJpahMJAk4wVor",
"1MA2aZoNbxjYVqL3koqXJtVjCZZeKY5Nxr",
"1QAq2YGU1kq2o78Se4iECaJQxNARvLPUoS",
"1HL8rGUAcVHyLUoK2yQxZUHFS9qoKEpEEC",
"17cpZJhizb36r3zPwf2RiDCPjJrnHSA3dv",
"16ST1PwBMdDZCPKkvkv2LFMVySUN5gDjLz",
"1EARezLtYe4UBBB8yxVtiD2Ejfh7VvXVuU",
"1Gtam5JqL5dBVpxZe9TvHLX7iCEYrDbCj5",
"1FtHpGBALyGgKNFeVmz9SEiG3hHWhyJDL3",
"12VxAHaFrmLBm1fDGosA9LKCeAu2ZikwPN",
"12ZuDPXfQMDVsgzMUQUfDbDwRCZ1QwVUbH",
"1J6sjTVfXxsqNgLcwGdX3FPMWVrsHALAVf",
"15XAV6J8ar4Pt5koCr9wWMDRtrYYcrQ8Qj",
"1Kwc6fACYfrMARmScFcCsuHq2AMTpBGHmw",
"1JUYzemj7ZZB5n8Fi7DRaV2xNi2kEEUqkQ",
"18znCzsDdvLneoC4vn9sTPT5ugRJwtpzHj",
"1AmeUnGUSdTNA7mm9cvgUSNj7gkdKiDVC2",
"1KK6ttjdvK5Jo3vp2Yupqmo5HvjiWQHTKJ",
"188mLypzP1W9WaVHyDhU9VWLUhgGciAPpR",
"15wWii3M9GJyRMeZrRGLvjhRtWvwHk1Qk7",
"1CRj6QnuA7U4vZajFZJpUmbKbuyvY2j1FJ",
"1FrNBHdCLPKAfUiv5prscw8Xj3cf1bY8xM",
"19S3DenVgszFvCXUV74AGnDQoTNpXHEtua",
"1Awpt663xjWdkxoYE3soAJwb7qrDsNCeHh",
"1FRerACaKrLY1JMLHosK4QNVKrYESykfuf",
"168vrWf6Qrh6q5dSzDsb1jixzxrtReEEbv",
"1NQsZkZTV83coTzQhwS1rgUpksNAEC4qhP",
"1JnDBTZJcEZzAMhZSMkYCZ5Z2kEQSZKeys",
"126R5VDuMUTc9LQm3snnRQMxHtcS5U3J3A",
"1x36GG6BKYfxZJV2EyhbeLDsnLRgbrhDP",
"1Lcwe7NqynXLjnkB4YY7yuysrgh4t2uKGJ",
"17Mxdg1RNw4PPhK3dSHRVu8Y1RMrieXXhW",
"1MTWzGeB29JRSNxPGZtiQDEo4hSEV7NoSc",
"14Ae72dzVfszhtu7xVmQvvKQ94FRyVqoUL",
"1XhJjV8PEm2ak3JsjXcb2rZEJfsJbCWHa",
"12DAszfrV6Nzpbt86mfDSBVvM7Zj5PPgGg",
"1F6FWWfamCFdMmNMGSEQKzNbVHfvuGJkow",
"17gFwFAH1ehkMkbf1nENQxzGhodRb7BGzB",
"15sfiA7ziZjR5dUkNmw7n4U8BFZYrFPSg8",
"1DpGG8aHRevVDvnKwk63c1HMhp8V11zDXF",
"15m2v5vyRP2Yh65jB6vAMJY3c5X93kGS4S",
"1Enom7pr9ds3grs51bmqnujUCensjhwSoi",
"1Jk9qjEVuhuio71XvKsa9kh8eUU9BEG8Z6",
"1P2wfTe3XWauubNqbuw6A7ZNXp77fn6kuk",
"1EJpFnnn3c1RsZdorE7ktDzawxQv7AU341",
"1MsAWvGP7ds3J7GvzBZqh3pxSMX8M9jEEP",
"1BbTTzSjpR2VMq78E5rjFZ3YtDju7eM5xn",
"1AfJ84P5yV5CzgcMVZEdUaEyATaeFYh4GQ",
"1E6QTtCihqFkxftr3ZF3oph58HwVp4JyDr",
"13EV3SZDHUwQtU593j9m41aWSng7BnD8qe",
"1FPgzNN76W4abzDHQdW42dJZYZVnDvViMc",
"1CSrMxWrnsLA2xnT6WdJfKo8FDmnZU2qB7",
"1L2H2M6mF74qgAuuLfyw4QBHYaVpPihHiC",
"18ZNsfZkEdehEyjdQVBFiS2i9WBSP7vvTz",
"1KCthVVbMH41hZrFm5Ftmua8vEZXCLqb3P",
"1BHjVUcm5hPMTmoSCGitZ3C8NnParvS8q6",
"1Dhuzk96nm2S91HY7HAYCDAutZSyX88gaj",
"1AfByWjT3jp6dTJxJaLRYJrMqqe2XcFpjK",
"1BPLns8rQDJZ68dE8ZUbbboYTk7d686SZu",
"16HsczhMAuydY3q91Xm7WYJYNuNcyJ63pC",
"13FK9NzbLNE6HFVVSJFVhHkUuUND89vDLr",
"19e542MfsmJvPUy3KWeSHqUWXiafMRtcUV",
"1DPvm8VRnyuNFN7yECVXAujYjXLbT6uR7d",
"1JraKtkECSGBFhCBc5wUMXGN8VsVY6xjrY",
"1NNt4Kce31uTyiGFoe5EmWoCr5jiLjjDqF",
"1PrgMK2ryUZn3va7i7VymSZUrS5VE7FCqd",
"1KzYcy3LTH6wMCioDU8kUeZc5oNT78SqTJ",
"1KRJzMtB4QSdAPdrgt2dc6Z4FjevL5aYtV",
"1JiG3Y99tkWVpZKh3Tpd2QrJRnoy55EeXz",
"1KyQ76FEbhCqXn1jJ1qh3HqFr5BnurGCqg",
"15B1ozPYNPWUf9vgyiJ9oyNEmvD9djpgSt",
"1JG6C12Mj5vwSgKK6kuNWgdxntUM9obAtR",
"1GqCbtLV2RSg9ixoiLpApkV1KfEonojNzQ",
"1Hfro7fL6zd3GJCB1phsDedvkyXPAHqgVS",
"1KCqLTwVnzgxFRUmfoX6QLjPWLLAfk318T",
"1KvCW1UNUPrmSLdk3yArMYyvhAxsF4Q7fm",
"1BZbixK23nHXVeFdq779sAspdvEUujJibb",
"18pr21WPCehTNQJYHCwfgzXX6gE5Pg2Y98",
"1HiWXbd2Mbw1wPJtEoomQTnLRZgBRrJ19d",
"1CjpJtT9i4ppuoTxrhmEs1uvs44e5jDpSY",
"1Eepq1TSZ1ePE3jGmerM6G7qSnD7MPLFim",
"1PLeZjWymTyQ5V5Bvrza8XGTcSZv5Fsrjy",
"1jtewe86aFzTWqFk8Wcro2V99kMq8KYbY",
"1LoAgQzdyvxhcYfuvFTbtbSW7XGe3nAhnC",
"1GKnY9f4VT3L6euu77BUoajYxzipsv4hoL",
"18RK5U58KeGthhxJw3sUotTeCdF9Cx9R9U",
"1EtV4FoxciEEPyhcnLAguLxBAzSfPjJdLo",
"1446LLDUPjwsDYg6pvFdNUjNmvhaHAVjXJ",
"12zfUaKhXhMKfHv7XU9SSDAf35arKDwic9",
"1cvr2RRja8HMcKvZStduHqQA91vqVQ2a2",
"16zSNNfRtaLCRgkv2NAPPdNcDtpbJdUZhN",
"1DmBrnGgkWhnEhasaFqTV5jeDBfEJbMZEY",
"1AhWdTXmMsv8NtHaXjQLbbtVCTTwqB9kzh",
"1LYGyJDmyBQLbKxaYSVxjD18B3N25gxkqW",
"12d2vN9XJJdCdmUk8mrkwr5gU7rqMBgekA",
"1Gc5tsv2kaU6KmvZLwLURok5cKRPZbwzVD",
"1MvzCFbCKnkn8VuWP2APnELdiyY88bzY55",
"12nDki232aa9C2SiLFDLtxLXCMnMo6ehY4",
"1DeB3EneyNwjwiB31VkHvRcbrEawQEWfQ3",
"18tcfyNJPacR1dqLnaFHkNS5hreBd7b9gh",
"1GRJ7EE89g5dCpP9RPYGu2gYSSyKt62hKw",
"19MnedPmhSM5WLjZKqY4rihm6zK2wfKzWJ",
"1LTEUAPeNzTfTGZGr3WsjGbZzLrJa8iKMF",
"172W5Vw8ehMMcDiLFSFjWBN9KxUHDHA2bs",
"1CGuvBP5YFyWLjNjy5SQcs3ftytAe1fKc9",
"1C6QG7kdcaGa4L3mudf1WXGTcDGfhcAm8f",
"1GVXYMkut3gg1eqGkVQ7xHCuZCmHv6CMUb",
"1CWhqTasTBxZhPNVWLt6BEY9ghfYzioxi6",
"1At7ptT7dxfcqYjtdFyBsNihfGd8x9ca7x",
"1GizLqacFBjTHrmCCbVS1EyLWmbBemCFkx",
"1MgRVic182xPQGS9VNe2Fe61pauhpgxy9Y",
"1LqW4bP7qg4RP4utSNienV5cj5ukMh1pqm",
"1257iE5DfUKASKJBKZiSYyadthHrGyiRgQ",
"1PX99rD91Kx5zNjM5e98CfPAcWoK546PaK",
"1HYgpbG9feCxys9nY76bdQbUCCUTDACnxn",
"1FntGhQF2LjhHfWBsKk8QHoQAyZ97dEsK1",
"12HV1ea77LFPCf8Mr4a1WZPo3XBhxgQ8oJ",
"1KrnKWrnv6jAMd3w3JPN4FtAW8Vp1iPgi1",
"1A7PNRxy5UnHZs4oCQFx29TNik6KKGBaY1",
"1AWGv9gy3LqKA8CSJgJ4J6fAh5yFspjCxE",
"1EJQHG3Uk2QomEezvR6BtdikP6e73Ep4xk",
"1CmqJ12ACVTUNdMxTjyWjDaWdMQMmAJn6k",
"146MEzsUZzDGWCnxsWeHwwFmvnCWPTo2JU",
"1C4tnQDVAbBKaGeL9YZC3tkRNSp21KcpjT",
"1PodVmpKgwSQ3vWqHm7MFvZqVVC243nC22",
"1ARQP3nQqPkgrnGf2e1Y1afHLctRntJcpD",
"1JEDkxm1jBisgNAbGKUpfWdCSAMK7aAFF1",
"1643H63LdUpkVUs6XZLjR2jekrXw2ZERqR",
"12zJns4V411LekSE7qiad2oxhEqhFKwjb9",
"1DrVKSmew3yJHDgtpZGdwnS5wqk22mdX99",
"19GVtuoVMUkLJswt4on5Ghy4UNgSV3foYr",
"19P6a87Azt5gJBa23CnV8chArohrVccJ88",
"15ynmK4oKWVtZWvodd8KV71L3xwNPQcKJ9",
"1PWi2t8VyDrQc4RKfoqoRQ6VBTTYdHvyeL",
"14N98vxhAHcZibeHbKqkgJk3G4dWs3iLDr",
"1LVHRg7Er3kQXkFLb8yUkJ3wqeBQ8G5oLp",
"1PRFmXpRMUaYJfmrJhMn8mVL1M2FUkXPjN",
"1F4r9NCgAiyexJ3ZXgah6ubjXpa3ZtMxec",
"12U1XBJZERLoVNTNwbdBgkzCC5cbco4XMr",
"1AusxiKyubkakRdmWxXaAwjR5Y4XShvLVk",
"1LCvVKWGSjPR4qoT9tfjWB4mLZcPkMSH6V",
"1EnM75kEag2eEDcbJNTzvNSpkokDiS6w3u",
"1MtPuCxBc5gUnVHoXARUuLVbSb3Ei8DhFA",
"1KMR4dPCxnQrtUmLZcHgt8c5BvLDbPqKY2",
"19WXiCth9ZAv7zKx3RP6kjmRAL6TBcb7s6",
"1APfdiw7oBc1V6oQkcpiSjsax2LVRxqKut",
"1GYVoEPN7KS4uRvLXmYFZAJfcPRmqb4D84",
"1JFMtDUoGLbAzoNpDzhNMsGgnaT78QiQBE",
"1JGchrRvsUK8xcqQh6jUJN6VHFY3iAX7Tx",
"1M64V3MG3X8forHdYcNN9SzwcwXY9ijuzv",
"1KwobAa64orETGFoKHNrwJ5CuyLouup77s",
"1Dr29p1A5MXinCjGYArBn5Zb3c1q32J7bQ",
"1ERQduLjCAe3x6CcbrJhrTTVXhcqUrPjaT",
"1MQ7BNk1Vt31k1PhsGpcSSxY7QCfrchZvw",
"1998iJKuyyaajnkWK3ZyKNCzhHVD1gp5yU",
"18yY79SYMdamrTeoMfdP3J8Rn1ubNNUCWv",
"1FVgsvQm489ytehm2KGk4HAyogB3WGXymc",
"1N9eZZraiLxB3ctjVJac2qdM6U5F72T4Pe",
"1473uXtPHRVhSWK21wPqMYUq26Uguhw37h",
"1DRaXMZtATFkRdQoxwBB6UrynP5hbbdQwK",
"1M3Aq5JNCNXmTj35V6krNGvETgmNqzQ1Rk",
"1NjyXF6dj8VrbrLrrkb9aL1NiQDnWzmqtP",
"1H9RADnYvx8UhHE9HboTJrJyo1HDrqwyak",
"1Bnao1j3xoVNerCMncUVTWz136KmzW17Xb",
"1AZTCZhMhaGMRPYSej1CPDWJ4mYwHZH5mD",
"1NPyaqy1LyQpNr3WKggf6PRoCK6X75ztw3",
"1NXkhZtKwJYWcEn1bP1PQjpNU3a6oAp8Ep",
"1GUWnUw6721NpuNeqrfkHUZrLaAtyWEt9f",
"125mKx9EB9Sz8FyS2ds2KnDjyLPWAq4Yf8",
"14vtvbKKboBMjwJ6ySKyYQLxKZ3ZxsqxSN",
"1XGZcrU4d8kuu5uNjYYALxEmWX1guqM6F",
"1363gvexxFsiZen6q8Min1HTk4pVZRGVKG",
"1LYUCy1ftGJ6qEt3cMtkkpNgBjfbZFeh1U",
"15myMDkxDA5VLgBDYLk8vW2fQ4qgWFFuJ1",
"18RMAAspYDXopbPdSM5WaDxAcb8EYoa2CJ",
"1BEZpjcVBxvs7V7XmoHs4xSsctZdTYF8SF",
"13tTx1V5hT1jiutoVFu26ToLjKxm1fiz9Q",
"13M3Vimq4U3V6SVmrR2syu67WYiMCd9zQp",
"17v6wxMqYjen7dBjHM4YY3NhY3xeisSshG",
"1BfUsw3rc9bosUoZEtDXfCVVWVC8fFqdhc",
"1Deq9uebSxkrF6Ha8DRPPFLrebv4QAoSy6",
"17C7fPLMGR6bKvd1v9RtZGP6dQPJja8Dn4",
"1QG8vyuAME5TKnDKBfcWDrBnTpcFPXYsax",
"161Sy82VEc4nuU7QQXyAsqExK6GePVzZZ9",
"1BWEQnWZkMTLJcqPzzW45Yt4BvZASZjLQT",
"1LyBTVCM89tE41vYqzGt62Q6mDb5L5pq4e",
"154jY9fc8x5hVATHqUvnamfmWFhTpheVPu",
"1EcfHozRXM5Zz1GMdsGMQTChrvCvB6Du14",
"19g3EZpeKZvcfsYU168BHnFV8YEqJfUsLK",
"18EwXcQTUhB2K5QTfu3qosVzZT5TNVuF9N",
"1NGyQXrL87Ed7sGaFYmGaivnyN53GmSvsW",
"1CGjEh8NCHM8cQfksLpHzRBvJBhTeSFNLw",
"1GsRLRLL83icXaLXCEUiVezLjxxk5LU4vE",
"16GDo6NqnUb6riC9998cFPEFfkHXXPr4Ue",
"1JUwt2rvgprQYsVcTQoMc2XTDvdwMKVhBx",
"1NuZexLG1xcH451n8YmEVSd3GgfFdykFqQ",
"1GH8CWbccBiiekPtYHfPPs2HeL9ZHuX2i6",
"1rFWbrDJeRRf2oBaZrthSJADk43KHvh4X",
"1FiNn8oudm7QS7P7eTfdCQVzdsAKABzXRv",
"16BCALUUeQeyMA5HtLtZSV1C4UzfLJqw3P",
"1N5uE1GBUb5Cm8gNMeTp38uzYSgAQ3NzUk",
"1DPCXnQJWsThN989kvnHp87UFgCN6mLeKn",
"1M3v1ryNcaiFEafjezPNuP8ZZPaJvmFp22",
"1Ms8VLHmK4Zcy8UPveZJtegMZ5AfMvWNw7",
"19chV7GcJ4DstaWAULQBz27r26zmRYpe8i",
"1BpV4abnrdiR7wKB3H57RJyDMiqrefMMdA",
"19zk2QsGDQMVDg2VCjmvXH9NXNPixx22h5",
"1BYaFwxWm2VS2nAhyHKh6UDvZTLum9BvnV",
"19yKDt8Q97AP1aJ8LydGdtBRdq57mChNMw",
"1A1rZUFtB26CKEaSR3wdKBp1JAUdCZZ7zy",
"1GVAyDQp29YQym8VUyKKhaVuqBiS6Jzi8B",
"12t675yNDGLGkqcCdEatdug9DY6n6CTip6",
"16yGFXZrSupQWoF6xyFSgnx3demxRPnYtv",
"15ibp7LJT1vZawpoyAaDySRP1gv6FJvFVY",
"1Gqd8zws53oxiP2sCP5yK7jhExqw9nAEuU",
"1LUAK6dEBaBNCHSu68cSH1us9bqn1LKszQ",
"1BoCbuufabh9LUg1rpeGSSmdKaCfzTiUpE",
"12nahA9WzBwukq7a3RejQmZbT3ougwBLhK",
"1KMqzbrPEMk2dL8xBKZiXdYznvr9GvgGti",
"1GphcUxtZgF69MA7LSDfpeQ6Hqft5bh3FT",
"1PUS825dhKRWE78cEJecfaqXh3tMAcSREC",
"1BG5gxQPBjmhb8UA7fvMgEsVToENVHdrQd",
"1Nt2vJk4kMBGcUnUTCgTWs6Ces4wJVifwR",
"1MwPMPgFpdNXknCjDG6Dncb9Mc7WQnXJKH",
"1J7Vv9RpEHDziaAwWVLFhUDN9Q62zXgTn1",
"12DimJnQWP5AXoGxEMwgTDidkHFP4EcTSe",
"13EsfNzTQnctLuXkzmiit6Bb7DgmMgKygV",
"1LRTw9fqztoCGwVLMXVi4D93LgVmDwfGbK",
"12yzM3fR7zt7RBfkeLo2kYQbHstdveGPv2",
"1NMxeLm5Dn5N12LqxSKhNt1WErvmaMBjLT",
"1AnMGeny88461VEwWaCe24JwUQYmdNtior",
"19yus3QTD4hgdpfizPd1QqK35iDRm2gvUD",
"12KCK69tsj1qeokAFCXegbb72m6ByjjbkJ",
"1EKWTJawsqD8sWEv59iBjXqkdo5DgqfxH4",
"1FJjdGBZaxJyv6LRjMPjxkq1KzGLB723s7",
"1J671gVPmGUFp7NppYidAwAuP2KMKAcw9A",
"15gyLBmNED8C9asJM8Fo6KT83N6j5Qyts7",
"1P9b8SGhbZEm8bb5pozXemiLh28B8GRG9C",
"1FzqdGm68FXu2zauFoUL8ysPx4z5VfiMb5",
"1Fh8cyBf5hMN6AvMUy2H746gsV6nJmWzQs",
"17ke2UWU1x3tHLSGSZT9nWkhnNpZV3uXYb",
"1NmkfbnjTVa2bzNGABmNQBFGHabVETS3g1",
"148zC3Fn3SGcSTpN6waGa2rQCFFf9Lrrp6",
"1UHPJLB18NZi4PihymDbDpo8TqFetTPm6",
"19skRTcjWs9f6it8ADxi6MD3Uys1EEEYBa",
"1C6ib4KvZTYWPtCU47F3ybBJq6bg8DvDtL",
"16h228Tr4Rc7KWTiFVjWkWCTLb1mUxtFFe",
"13zYcVnaqeKatCjQQEGTavE5ZmnWDkjvTY",
"1KTDy86y8BAVjN15ZLV33pWd2BXPHWdjes",
"1DELPvVWadnXFYhhgr2vK1aaSRTjEfmpVV",
"166q64MybXfVtM4Q4ZGKU9v9XkcUKMWyfN",
"135QMX3fDHB2gC76URrogR4wGMccoC3FY3",
"1Mg5Vcufbr17qtjBFaXFUUsfo3eG4ce6Pi",
"1DokDibUkrUXs4H1jfe8bB22pCvKmsHfHg",
"1bAkmCu9ZbEZvoujWAEKLVhy729486KcJ",
"1vrpa386VAuaXbWAVM1vyUPNVaDLabUM6",
"1EBp9UgzAnRrDnsAiVocgvDWzfxUAZpcWU",
"14mK1PzV7hMmZHMWfo3gFcoprtj9rdEcpW",
"1AXU4SAzDuxXQYRkbeqb9cPBUvptsV1iYa",
"16UmiHrXegTZdeLM8XWAX6v3Mc2wjThBzn",
"1DhJANvauE14fpm6xZ7csQWRP12xCu3CTi",
"1A1g6zBEoxCSZeK6XXZu2c1c9h1gHjxdqi",
"1DeUWLquVb6i9imZK5iR5bBSeiXB4sZfUb",
"1M6BwqXeSQM4aQGbNP7RYDNFfvLGGkNSDS",
"17Mx5yj3wFims8cgCtxB5GErERkQyoqS4w",
"1DBXJFrLTGNNT5rcRxBAGkFKJUYfP1UvSs",
"1B3Evfk6gKYb4U9Ldk5YABx5LXahXsRq6E",
"1FcvMFvDaJomjFbHXMVBfWobkgG2MD4Qwp",
"1FFCtEWgMxcvKc1VbmhG2gffbn1RPzmZmG",
"1HpHYQZFhVZXgEtoJH1BB9yX1wJBbweaeR",
"1GZN1THHkPmAYwWxyG8pkTRAescE555Tb7",
"14CQeak8nD6VxynCRs5wGDt4R2mibnZpy1",
"1EwEPnYuYnwNP1nZtagTe9FFt7vdp3Ga1u",
"13TE5VqdqFru5JKiYwNQjaGwMwMWBp5gDD",
"1L7n2WicUDgxAv7LpsqtioiXws63wsmKSC",
"16cydpXGaRwKKVxaW4Zoxe97BbMoLFPZzM",
"1DHGf7TP3hNRgJSRujZdAiKCXYZdkAp8r6",
"1EevyVBSd5kNGrH6tmfhPbsWMdDW2h2kKp",
"14ABckCByGw4nPcJWDWDaWF3fm7hmWGyaG",
"1Dib5RVUXtN5ikKRDonwoSfUc4P1MK5Znd",
"1CxVy3oWKpFUhWFTvdyWuGrUkR2cvmzmfV",
"13Z6riw8o9CLnd9pxSYTJ4ub2YLjWGe617",
"1DqNpzSg5aNTiSNydD8j2JebPr463dDpNi",
"1M42zDxd7x5PBesjfSXE1n367nmrsXnfhq",
"1Fze9ECtPcMZvRziLtfRAGKi67nZmFp8go",
"1GePcChbkqSWFQuSMWLQm6RvADTgKfZN8E",
"18i6VjeqRqGMQDVm8Abd6XQPBbo6iMRxDR",
"1D3uiDYbuGEhUb4F9QEsnrfc8sGNtErVbd",
"1Rqpmdp56zKAZMHVce2dShienjgz9B8BB",
"1HrWqsJf4yBrtHxDSUiowReLjHQPAdT7GB",
"15heNpJPvtxfekc1LVhPqEAG518SxdNq9V",
"1FpFLByD7jRHSzuU95rVebMcPJFu2BwgTb",
"1DRZTKq2EyFAk5jUJA2sWxF7KmfQBdjEgn",
"1BkskojWvg1FeymqoGK2UT3YnqKHLg6KEj",
"1NQQtq4Qpen2zE1xciUahQRZcMgwUPNd6d",
"19GGMsn2BTQM9Z41qHTcnsN6Zrp8n4hRtB",
"1LZNoQPeX6w45yoaBgyiRSbW9bs2DaPKf7",
"127rEe9agq7HnoP81uYwXNMWXagdDfvvPv",
"1Mm3WZ5SYckRTZhg7bYkv2QQ8Ru2i9p3CG",
"16BomnpZrWrHe2g5eqQeutv4dHaHfMs7E6",
"18C6TA5sztUkVuzwCjXxxvPFxo1Urk1kh1",
"1Epk3rEAFExVoQrNTkoFSKNSqCVzH9hoft",
"1L74pta3fWHwLi7Yu7yMtLiqVnnWFyZymo",
"14Y6964pYgvjnhGXuCKJMZ5cpZncdipECB",
"1CzATNZCNYRPvdcdUMcAeGMAeyV8hZc4RP",
"1PUf966kQJagFtFMCbsG7QBpDzfWq7qaaT",
"1ExA7karJENndUk4i2ehBtCGMQTgisHkBy",
"1KAbAPFG1rWP9EYv5tFPuxRvtmbAyRnb6P",
"17wW67fnSuJynF7ibTDk2yFy4gx89oggka",
"1EofJPZJLpRQ68xrugygJxnxUpV1dFAUi6",
"16FRPuqmwvkVDgTcXpVcUrxaPVP6eDRZBT",
"16g1NfhiHnP55S6Z3YCCtrWjG3Dhyt1XUS",
"1CRAZjJNxEw1u3q7yfcUhMuKRCz9EUKBy8",
"13ZXazx2wQ1mE8dcQyznR6Tg4JEAtGEyYy",
"13iEs8jEk938D1mzhArty9pLGJ7ZBr3bgw",
"1B7xzsKsxvM6yMsBDRpbrYdQZppu1t1R8r",
"1P5rTX9S4mZxvbSS6oxNngu4F3awuScSDv",
"1EX5pv5QPGE7RC8RTRdZjwAcngQRnNjqHX",
"1DjwaDRBuykKVRDJYhMVUSPSnN2jw6GdUz",
"17mLuh4PBc7eVf2Kskrrrc4WzPaTSURhdb",
"16sRSH89hsEcDxKQsjPbuor8jU8hk8FARA",
"16asq9KnRpGP6CvaGfKT6R2k3xGcYXGat4",
"1LCyUmFSu7kZHHnMJetYuspS3swCvK6cWD",
"17MfATYDuJSQES3rCpebwb4o2HVAxXYFFk",
"1tGQMR5soP9StesyXoUgCBifXPnKR4xBH",
"1C6D6SVJ3dqJXgFzi9GRZDDX4n7GLUiQGq",
"1KEKKN1mZvuFPxBHz69nE2fiWE4sPALkv7",
"1E3egocPf6zDBqRWjby83P7H1vDAn5gQnw",
"13pTSyfHr58PMQQXLux4XLDbXGPgrh95As",
"1JpjYsScHbEt1EKpQV4cvtSVuddSrCkvmL",
"1FYXvs1AidRmCCd31KEPZ7fRVyXLGdBkMs",
"18t4aT1URdE2HGfmPSXtAStCX7zHsKNL41",
"1LiFGLX7dzsgAtQd7Jk56EGGGbYFwXSshE",
"16VCcKtnR91jmKKcURRiRm9wLTKieTtzoH",
"1NVxJz6bPgj9mx1J4ocMo93ND3DxuUACbJ",
"1BxTGV1pDihVvhC7ASbSTr3Ebj1gkvjyqV",
"1A2nnNzsmtsqkWoTo8dM3Z6AA1UrdVWFcg",
"12X1Hi6ccwp4oEtJ3ZK72VKSZT89Yube6u",
"14cxBCExhcCE8HrCHLpE1jRXYpoS95yMBz",
"1FSaJwUqJE26mYqES7eb7JJiVdjPUdY7Ax",
"1PWgZqba8xbpsRo232jsbamHv5B9YGqq7p",
"1DMsuQNQGcZUdePQhbwg6n17A8AgvP5ApR",
"1kKdshjTDknhbA2XxVm39bDHaVe6KLncK",
"1DxoNAnLc252EGvkpZ2JFjWMNTVkxC6qjT",
"1EfNasx2AbLx9QPGpxsHDce6YDtgbVnhMy",
"1EsCeJfLB2UFCFgvABN52pET6chvbXfD1y",
"16XgpstrpFDb8GLsvLRF8JLurq3NsL8qqw",
"1GeBpgnm1AeWVC4CfgqbNS4SNdjAQggKX2",
"12FLWoYfqo67EyoKjxtF1CmP4Mh5WYjgcv",
"15rXgbtyvXe5uUncwP6RsccbMwGQguKBL9",
"1D6P4akWmB3MKShJWqnt3xKJoPnwdvFnyS",
"1Hs2mcVuzoKd2MV53T2X7Dhbpq82hB748H",
"1AkKboqGX4h9PvMSf2qjMZEB5juJjMTGkc",
"1K3j9ismP8Zs8ayTneBxDQbbkBCNWHinkG",
"19hdhwjh8tZWuhzwriG6HXCRfocPjvrFoB",
"16go5D8PnhHAMCxRjeNodRAnsaNQFzcx9w",
"1EJZY2xA4nq71dSLF8VXVGHEpaK6zz6RnM",
"13YR3Xy3nnXD5pjsCc1cHCDX5XAcqKZGXx",
"18Gm5UZUcoU4D2byXTXpEnU5UbuapbmKBx",
"1GExLwmA8FrYRjh2W7r9JZa5eqPic8bNLv",
"18NLfKw82knUKZpBTVRvst44uXxy5QxdPj",
"14dkssvuttVYpN2UHVkCnFJrCmBgB3Eosm",
"1AHk7QNX3Zp39JufGyVUKJ6A5kyDK3AG8N",
"1Ek78hMkmMKQjVuLQkZ7PSQV8BU2oUbFi1",
"1AiVBkDx1pH2Wzgi4wNXRWoGTwsNxCaDbp",
"1GXipkAQQLjxRKWzNestDYGTUnR1zRJ2Pv",
"1DnERBMSJBBLRkgjKDu6CozEhWvXNXZN4T",
"17RCtVf2vQR9eCnaKvfYR2j2EwBGfeVdTC",
"1FeaA2yGrQbYmPCTCsYm4okqUNGLCFjPaY",
"1HcBZMGw43hPyoV27i8YRiWW2rdzPYyzJV",
"1M28yVqLaZ4ECVCjbFht2dyCgJsCoK1xi6",
"16T8UDFyWU6rU35f26mdQ54owCEeBYSHTk",
"14CasJXhw9mnMKFkPY5oRL3BP9Mp92JrDT",
"1NypQn5S97KrdpzakvEPpj6akVuniQRKKM",
"1A2TUDCQGyTJHTogQssoSmaF3vBppCXrG5",
"1CrwMBgLaHUYrXTisdav1gXhAACBFXAnrU",
"1KNMNsAxjeRDN7XWwmzTCzG1XSCVocvQs",
"17W81wLUizzHsckRk8jWJYbS9voocYYKYV",
"1DziM2EThQfTFXvqKKb9wWxSgZ7P6N3xCD",
"1Da6JhCRLPTwyqByMEvomM2NvKFfLSQPPy",
"1PTfGpZ1LNPC2q5YjDwAKyxUyTt4ZGkohS",
"14xaGmdHQwY9C5spc4fNuHqA17bJHFtWQc",
"16Est4XCXSJGbnFTPCX89fTuxjpQ5jojpS",
"1AfLhCrFxPfTubkCEWyhXxCAe81PNgaHSY",
"1JkYYGj3Wx8LFV31PKRZJXtoZR8i66anrt",
"1EQgQ6DpqBqYnCEJ3mKsRw5ZE1zkYxTMdv",
"1GgaSPJfKa1aNRSgWecwNpe4Urz257FaxY",
"16tkjkxkXX6FbnadRWJzMGVa1FV9bo8wZz",
"1Ay8t5XFS99ydKpf1MEpS8KE3DaYUHs3g4",
"1JmnRrHB1JxJH6PQ3wqNeuZdy4YkjfiueL",
"1CnDGb2DBfsBzVFNQgrHbUdNZVq9FNyK2R",
"1GVj1YZ3fA97FAuXMshTXaQeoDbTTL2PYL",
"1KGKw86rgzYj3dRG4PEt6D1RWz9W3hHEsU",
"1KJVZrdxViZUW5SkaJfEwCr8RtAqrXRrVL",
"1L9c2AGScinGzCyrGczWZWf3YeBJiTjvG3",
"13mcRjh18zdTAiXms2FbAp7CE6ukryMN2c",
"1JJFW2hwLTL1ZRhzmYudoSDWQDiMC918pM",
"17SqaJs4exReZfkPh7ATmKhX32MSn9KmUm",
"1GrLCTp7zSUdMf55UwFybzWjQuTKAw9a4u",
"1MwSBQMEj37BMS8ETxx8Z2p87WPjU5LXjz",
"13UDaaU7GJGvhQbZ1gztUnaP7CdHXNFs6H",
"133oAe498zv2eH5NYmiKW1e2D7AFJu1MZ3",
"1M29Xne1Ggce1Q7khkje2VhPS8STnwvF5x",
"1xnV9niBLsVvp9zQG7wfFUCj7i6ceyMS4",
"1CxvndSUdPZdqx76qhybNLMqZdDhsaGnEy",
"13g2iUNWFhUzfa1UqFR1HEhRc515jZpMy8",
"1P8X3TJeqNd1PRaSpa8BaPGDz1iQ8wRDEU",
"1EVz2oShb1c4q5qgCoJkzujksiTq1D9tir",
"1wwoz3DKsetsFZBoHbgaCVh6vsJeeYqoA",
"1742YW5YQ3H4tdUng2oVJTjAw9o66YuJ5i",
"1L7ets3YTpKpGTMiqUL9pWr7nhztkFTsgc",
"1AKz4QYWty46dfqUhXxDDijo1CwS9yACvB",
"1GXjRUqGk4ok6rhsHTE5VZ1HKgb6svutT8",
"1APsfQPzB15aHBm8BytpigcyT2XqfkZPmk",
"1Q7okwK6eEKQxsM4T7sJLV2x6KG7rshh95",
"1Pokpjk1HcrHrz2AE74xXciPLDyP154JHQ",
"17cV2nXPkT69Dce69YeNY69cfqGEuqe8Dk",
"1KuMqCwvEvtDi9UGtPXdLshgDEaJ6s2uGx",
"1HBtxEE8taLsf1sS5dvmuvzcwB3LiMhhx8",
"1Kdx6uoNSU5FLpbmjCd8yG346c6tae8iDU",
"12Rc44s1J754X9t2bgaMQpWXDptUA1NnTs",
"1PPPxUhMYCjqxagfAhu8xSi78RV9QYPUWi",
"1CRD2pZpQnL8RKa4jT7uZeoCe6bxLWAqbt",
"1HRVdAsjTBNbRjDZ6LjtP1uExjZXUkxy3Q",
"17eNN1TrpabAs2tmgk3TuUP81MRfN6WecX",
"1AmPdpDL4ULTJU2WSD3NQPjsAhCNtbFBen",
"18apHc4dF8ANDWU1w6bJawVidhBnawX4VT",
"12qw6tqVnjz3ddgaCMLESTr1Q15YHVSJJ7",
"19XwAHLr3we6ZQ8cHEaH6gzEkZbA67YiyU",
"153xGnku1coKH3seEbDfgV7nyeM4AvL3r1",
"1NBQ3Cvg7F6oTz7FnATEj4HuCkFGcGVS2J",
"1Db433yjzvHcwoxafdjPRgGESxCriT2ETU",
"1GtqnPsz4pw8bzMpDEajNJSTRtpLZZpk9w",
"1BsY1D5c4ko8ujKmP6Np6wzsYMoCttTsvG",
"1FcPqVrq7KuXQukKi9kNxwQBKCbS5DZDzQ",
"1Cj1GFAmmYmXp934iLz8YSZKWf6tnoy5Ww",
"1HVR5FUprmuTVz8Go7nkQ41rvPzJoHNGUK",
"13WPmX4vLdM4Ezyzr74t8Z7ogwGw7DvDrT",
"15Dfk3hwCMZEBTk6Ptzr2vp9W2j9g6PYuC",
"1NEZgX6vt7drMgv3qEiY6fVjVf8EEstV62",
"19JxHsyMRhURANzq15iqeZZX6r1TAeuGp9",
"12JV1ctczBP7WEGzxEV6ayJwe3ENUN62bh",
"19zNjxzVdoH2ytGHF7Ln7CAgh2bdjn2HSh",
"147RNGFDhEPkow3mthkSFaa1vRn7jyQ9zw",
"1PqHbpLQmnKpP4D2xVq7smeq1GsKMuPi8e",
"15pDFAm9zqvfCZcBDbqjg6sL4ETyRhqni8",
"1PArDp21ehRHX62FowLxMdGbkt8oH1iPnb",
"1CZLMqA9qfpVkA31AhRbbJzD4nzWYcQVG7",
"1LNYS1u91NvqYaARcimXH9pW7yqPBxQ2Vi",
"1Nij6kENnngT4ZBSD8kwymjNFaewuD7KzH",
"1FFu7AFM57Z5T3sHXWFivFCn8YFCFXWMhU",
"1C9cTZugepgL67i9x3UR4iB4mTxVEobkDG",
"16orvu9mEVGskGa8Lgq7XjXySGf5xpyJGV",
"1APT4Qw4Zxfdm21cmW63s4bBxMeJypYpV7",
"1JCQQUrPRCsZaMLNnrZG5Kx1w1Hui1z81c",
"19fsLVXX6PC6WJwaKZ9ZMP71yNxj4WMziT",
"1CA6UiW8VSJw6zDEN3CbH4ncNtR5Eb7SNU",
"16jrnVPNM5yUPrKoc5xTyBLYxmkkv79dSn",
"18Jd7UZzrvWan56YEER7zSVNLnHH8e2haa",
"14hakxE1z6ZjZgjq3r7AFSTzSmAcn7TwCp",
"1QFfU5CgDAUisFBEmWbi7gvVgvguXUdqkY",
"13QdmPL9UwRY9ydmHsBKi7jXdXa9w7doBD",
"1AjwycSvdHkntV52op2p2A6x999h4ATzec",
"1FQxTyDz32MB8QUggShUxmFVr8RkBDXChA",
"1PYdPeutymzSdzwwryo1wUD1cxK4DTb6AK",
"1HjdXdfy6K4wym6xfeKSk7PWFq9da3xXsN",
"1Axy4WWQcFM6M31khPcmubUCfccgvF176q",
"1BU8338n8niwHmg7jrKn1iWSaoy1w9xZMr",
"1JqkGjAdmFRWhNYJt7m6dBYbs2iR465hC8",
"16eHJvwZ8e491Fm4T2jUhdpZzzKLCkQG8J",
"1Pvd7QATZGQCNCUD5bWyvs5rwWN7x97Xbb",
"17ekdNGgFJKf6orsgEHhbzXHJMcRMivP5n",
"133EnNyGoSgXgk16fdv5bt3CGSmzDQWXbB",
"16ehgDW3phL9z1rjn5LFN4c5GP8uTg5D3C",
"1HT9U8UzUdz7syStrgeVuFqqtgAiNxPpPk",
"1EEqSqnKnc34XmvsChYom5Defz1fQynrQc",
"1HtZtAStPHT5B6VRpe3zm1DXj19rk1FrYM",
"16aDM3MRAgdPENTdyrfe6wanKEHtpJ2jUZ",
"1LCNMxYj2uqkjjaVn3nkEdyqxSUA7Rtisf",
"12LMr4R9JzRseTqgLE2aBLAGqohRnyMYyf",
"14DSaHzwFmKdxjLT6bRLMk2wBwyKah49tp",
"1M1ZDc2kZVtzsCnchXCWnsk94sFE1YFQh1",
"16AvtxmQmTc9c9RR5V3etk7ViF7xM21GCc",
"14DeacmcL6i1tMWpU7HctF6JdtP97NysqX",
"1PnEnZCKY1iLUk62CRtWmymQyn8Ux8dDuw",
"13B9FHkrJs6WCPgKH9bTqVpup4UybKsBqz",
"1DAq9JHSNGuYuavfxv7x23j7eR9xVL7Kby",
"13TFu37NS1bLyjhGW1wfR3umsUji5Ds3kx",
"1NirZNG7Y256WnsWmjEGU1UrfD3KK7Pexa",
"1Jv8fRdLBWTvzezkaonnqHn72rmknBD67p",
"1AQoHcB8d1uMXTfHZADz6MasuGSwRrwuDP",
"1PgNGsUyEUixtrRhXVFj3B3osfDqCL4mYD",
"1DcCt16WWVSMzgSyLbczjotdCsSNdVfZ3Z",
"1KEnzVdAoJaaJ6c2EQkUA2QqBNqSdFLXZZ",
"16FsrWrvKfijVvxLmC8ZK9By6fttNBdmex",
"14LZKstjLp8pbVCA6rPxgMmtMTLGG7Y69L",
"15RfWrP6nW8mCujGxSuQ5Xupyr5dvgD1yK",
"1Pio5twwk5BCuNuK8jtLG9gBVpe9XPwhJR",
"1NrYJfm9LmKka15N9UUEyLa3qJbvDByYzk",
"17GwE2mUhJvXzHpRzZzJpAwaoufoNKybPQ",
"1FTBv7thUYdVR2hLvn87a73ctX95oW9woi",
"1LpZCcGsYZsDsQpfwcV51G3QhWpg6Ktx9B",
"1NdvDrNPsmeBsAML4WeVFUEQb4EcPD6d18",
"1K6b2hzVbswJh52AZABNaTv1iFiRWUAyL1",
"1235FY8JAs5d5CThTmRJM9nK9YZGsPi2UG",
"1CVbCTp17rfgdbtTsMJckfGfVYky3YdVky",
"15vmzJ1qvLF1ERh3wD98c5aFnauejrJyaF",
"1359UViwxyuafGYdJofihMzoJnJ5CACDQ3",
"1KUpCF4rreC69b18sGgqqf4bscCzYw6Diy",
"1FEMDxq5mMjUfJoPe7SEEHc8S1fKLbfu6u",
"1MvXEYNVsCw2MhgBQoDe3dJCsfDpmGdTNE",
"1KLUYZyE6g4njN2VW6ZKtNjmTbJqLG5LSu",
"1JW6W5AdWFd1TPS46GsrXqH6cDUUccHRf6",
"1NyEQTEorXh52h2UgiTVXtKmZQ33M5hWkC",
"1HNvoWrSXTPyXnbPu2Nvms82sxLEgkEyFu",
"1BDxTgbzeaK7MbVbjbLxDgRVKJdcDHtvRs",
"1FTHyCKr84WWHXN2HrjL1KudFH2mQmMyYX",
"1CUpe5pCiNGpJHR53SdBXv8UvbP1vJZBmP",
"1ErkTFkePfnjHWhCi76cRMpasoNHgsG4mx",
"1P7kRtZbEocVxQrmrreD3tQH9AtQkhdxpQ",
"1Pmnf74j8p1bf3oxXYLnNGHisoQonvDty2",
"1DKjJU2WmMV5Pc17FBbEtajnapitVA2Ndc",
"1EZfKq5dYhjKVWmoapg3W3NJ9itBynPepc",
"1HxDNSkz56Pkktq8sVcGdfsoV8em2w7sGa",
"17vgHJrLwSsuCUnDNi3Gyp8eiYc4bZWtDq",
"1P6WkZk5VrqGs4WBTzNfhXpiTfjzNx3JbG",
"1CHXsqqTHWcx2nfic6PwEYqTbYLR4h87me",
"15aMGxAkH8N3eUMGMPsPCgarKTR5vjfT3G",
"1Hs1cGFADkxnum26j9PYTFEHv89RyY62Qk",
"1RjDPS6uBgMsDcAH4oX3RGf4JyBhQ3qFJ",
"1Nh48tWM5hB8aX7iXvrpzwvQRar2yd8VLo",
"1MqkU94RNr954KKUAPNMJjymJtj6ByUCuu",
"1DkBopnrFBYKrReH2NuTxY85afdFrBikYw",
"1AtPc6S5UzU73d18Zf35kAhNBtRYjgxLvZ",
"1MACF1RD9gatsFuhcpBWX4HhBJSe1V4SUP",
"1PAY1RrfF8YjfJVoFQ6yNj82ZRhqZWPHoA",
"12PCqn3E6gBZamSFdCDMZ3P11e3UVedLAD",
"1FGopk9s5dMuaKVcJjnwniQRN7VMh267nz",
"17xTj5YzEWfsr614o9cKXaqcF4oag8934b",
"1Fc1APKnPrVCkYH29JbEHYzPnM4joxH8t4",
"1Qz3GHJhwxZeqyoMq5ng1SJCS2YsAtfKL",
"1MJ1JBgp74CWqk3dEjEQvhVSDTx8HW1RwT",
"1Zwbg86dqJBsUHV44iQqZASViRAcFDDe8",
"1G1CJmbpyoPBuP5vaTJ9PKWSxfd5z38r7F",
"1GcxoRzWxzcRdMBk6FXqtsKoqH3u8KGRpP",
"1CMKqTV59N8uyuCWvY5vPxCQLoWRUfdTAV",
"1JCGanpeQhBtKLoLKF2P5bQ3yzdgSmyc78",
"1CUtxF5pQQpNJnoaMLnG3KDFN41GeVkF1r",
"1Nra5hHDGb6ALbk9CQaBiVNrdtsXbbjSBs",
"1PAwCG2tDqvN2nYWuNbnPUeUC6DzdYCkU8",
"1KU6JiPxn5EVuKXwLXiuu6kURHdbzwBLg2",
"1Hr5k7iriCAMspnJafWy64oWKVKaPruaU3",
"19wxYmAnnM1dNtFLgPv18gutmLTNTneFxg",
"1F5crkenGvDGB8fxpwKEikirVNUNtnP6yH",
"1C8aeEyvRWXCAKFNQ2KhSEHDXTB8Fd1euY",
"13anKyp5PKEb5YGAv6ZsibT7T4dV2ExCzn",
"1KaQemkh9zNgD95wtAG1GBPz8ogmPLsZc4",
"1DhPC5bdE68Voh9HqxwVNVsQPeAcB9SbCz",
"12NWgC5WpsSPbcvRK2VQ3u5EgzNmNrvqwh",
"1BeRCyN9Bu5Crhy9xLTLM1bZChftRCzkJC",
"1DnDqdLCbZEHEpM35NaobqgtMwMVdxYWpT",
"15fWBni5ja1EH9bMFvfosMUf6DMAQcJbHJ",
"19BLC8FdQk5eQPuAcJPrVT1Koq62d69vRo",
"14qLDC63Yxj4mG6Cg7GScaaCsfaTNbjq7G",
"133nzzNpsNSEKs9jS1P2bsVVHK1nzhqgNt",
"15TKxMqFcKnjUF7LWXqH8kQPKM2Xq9QGDQ",
"17rHJ778SzNFyFyuUoPNJ6nKGcefGpuhiY",
"1An44X784cXurnrPR9oNMJhoqPuQM3JFsv",
"1KJnKaT7G89viCW1vhbc7G41chzaRK31ua",
"1GSvDXC3kNiaUqtoEER5AVfdtvzrNjR2V2",
"1EKgNeWTXh1mET41nG98easVitqXTp5v54",
"1JkKg11vNyxQYzGCG5fJ8GQ4JyAzguUnqc",
"1Ljri9yRaDjarXREWiJXGMpHnXpXYBryDS",
"1AmQcqzQwso8p4bcYcuAeahF7ov5LNWT8e",
"1GvRodnbV4YGpVrAuPce2MsV8rjeqVK3Fj",
"1MMbcR9sqQyPqnrN3dVwhZunwZ99jNj8Kc",
"18FSBkQgqB8dUAs1syKndkwVmt6PLo7EdA",
"13fKVSciqMLBEFnDBZWm4RT3mvngGKoXDe",
"18jiX2Fvwr81jMT292CRDLSincVWTY3RSU",
"1AZNNc6zmr7XvzSyjjewqCWfuJJ4oTZVHY",
"19JzzzNbVCZd6jiAZ5LfAfWKoLiKmP4qgJ",
"19WHZq5CU5Tn6UWRhwkKZMEaCq7xwP5T1a",
"197rVLwfUQKEeY4BnBGxbiEgpZJFjd5LcD",
"1Dr8jf2EoTByhHw2Pu6PKc6K6V6CDKRVab",
"1MzVnymVx1JvqUXyubhab8Pey9fmz44T6S",
"1JZZfVE1NKHt8Nw82Ant8eJWQSfpx7ticH",
"13EvtK3LjaJeFnVnywHLhSDyJ3QqL9aLMb",
"11Jby79BCffcn9hmop4mrTAYANp5Asvms",
"1FhFN7dBQDLNSWkjK1Jpe8mCSfvDkQHioy",
"1HftPiFQYeMFwKCAaWyyPsCrrnSoQFmk82",
"172y6qDgTsHBY6P9KuZyBeZj2nXnBJUK8V",
"1AQKimKkjso8aEQHzoBpw4P3XoP7RTjhhL",
"1Gvt22P9HkpMBXgZxGwzXx6dwXgSJnMMD3",
"1Nrb89z8UQUSGWEtgrKka8VrFA9N85HGpN",
"16fHFMzdwLoKEFPS7LF5Hy8RL2s4GEY5jA",
"15uznuEs2J4yV7RdLPb358VZ3RLr5mk2vX",
"12hdVgyRaBr4MVVZMVwUeBTyLGiJt7WLLm",
"1JKSrsBNFbkD7n8eEJGqsA3hNLrnUuwoxm",
"1wZDisj9VmGXjLuGeCVzcGN9xesiLNsK5",
"1PBtfWQKamXSQFd4TQUER7kdGNYweuu76Y",
"1Mr8CAmXPx8nmVyrUwKSrpr93YzsuZW3Py",
"1E6BT8c2vUTYa7Awr8enkrh7TvvJidzuSb",
"17RGsmqAZgTenVGLTBt8Cfhz9UUvWe5rz8",
"1AHCRi3Dj4dZzCeEfAYX3R2J7pTJTX45QL",
"1HweBZtWJhk6XM3uGxV7TpZSWr39aLQ3FJ",
"166jW9h7P3Zfkr4rJyoC8pdCYUPvqc6LHw",
"17uaBzJe2C3WW1w8Z2NpZ9hW8rrKZBEQhM",
"1H1qnMvcwag29FXBgaxq7yEwaDf3iKAGCz",
"1P42pxk2fV8YbHT2EXPxmKTidTJzUkowQ3",
"1LBWhi391XDJUhbGMNwzAhL9jM2CAycF69",
"12tiKnVysPUJNEP3g3AYreNrxk56DPuvyk",
"1NioYBH5VMvKKyTp4eYJHVmePMmfL35kDX",
"1BsGanVcxQyyLoZk2K5Y4DmeyeCLp5dQaC",
"19dpJrxzxcHj1VSUNguVgifPdmujK3rwnd",
"1DEu59k7tn67BGhZh8YYWWZ1Xod8vZ8cYz",
"14ShCUKJGDnQhRThHh7YZ48ohxsLrhyk29",
"14wWBZjksLSimhPzuvpVpgxtPtghFHUyeR",
"14kPGeJJ6ZhxNEy74yjCGoNxeeVwCTAUKm",
"1DrNLdyS3CoF29farBhjWHUkuhZNjAH3UQ",
"1dgJjr6CZuosYZk9K8P8kU2boRSYagEUj",
"18eR9KQVrEQgoL4vWVspwSztWPuTcVHCqC",
"1zEGesZRra4H7xB82fPz68ipvYjYUVHCR",
"1MvoA3pxrjmqevRVC39YkSaeMoYoRNUgrA",
"1KrLzkbB3dNsGARzx3mxhYhTtj5FrKxctn",
"1ELkSHP5WoDzen1E4UYa6ifdLdr3paJLXo",
"1FBedaNxLLzBtf83W3yyuiGaZjNm8huR8Q",
"17JxSQ52TPThDFubWC1QYGEWq5AGGF9uEt",
"1PFjPhNVviH2q44UcaNHzpvNofgVPZaZu4",
"1FxKdzXmSJAVD9QCjMjAdwSFES8qkbzyRe",
"1zm1QAaFG8mhdXy6fiRgDC8W3du5oVAwU",
"1A6ipTzHwKZNR2P5cP7bcP7Zb8qVxkaSGn",
"15QSc8nJGqUGFYxZRCASpvpSvTmhQdWAph",
"1G9PJCXDjzMhwASU3sSixb7dEgAfAb9k5j",
"16ZrnmJbjUHnBuaDobs4f6jVuB5inxV9xD",
"1JpMdxf6AcLnh4WeFV9RVja7vuqVMsmFM2",
"1K9VoDKLqoVgbAYihvaxh39KuevhgCD1H",
"1GZgvauuXmggrRFg1Tr7qbK12qAhRCAL3W",
"1LEovHLHo5hbujZzoL7yB9jAeaAj4r9ngD",
"15oKGpoCa484oRBhDoguSZLG3sJX6WnybZ",
"1HsP3xDN5vcK26TZ1Y8k171JT6JdGpvUB6",
"1CvMYjomP6uAqwBTboqsHmSu2Cmus1uB9V",
"1S9wZzaN3KUePB1iykwsuxiocbZo8fgjv",
"1PLkiD9JGHPtedEkm6znbsH1kgMEqPC6YP",
"1HEp9VShC5XaTBLci6mayNZJ7r92evcGRa",
"185jcrNiuNBkX6dVdifrMZdYRAQUKWZrXp",
"15uigA5jMx5UkRMvYkaZVDXZAqMXwN61T7",
"1MJmpf143KKNk3HVWsNMAivfbtoEwLXrB6",
"1KbwtLzDo6GGWdHujPFaVDbW2uHS6pzKMx",
"1CUFK1EZLM2e78wVW2ZYXYFApz1doJxLx8",
"1EKw88tev4Nbw4y91jw6oUwtjKA4r479rH",
"1MbNGmB9bsxGB8NqwaJTMaBDTrxgTFMXyr",
"1E5BMcpc3WxNN95GQ5hBNSmpaZSHBx48dS",
"13Xg47ZAZoF9FbL5n6BKBLQp1xLKaMz9pY",
"1HvijDmuDe1L4zr9ZNfmy7jdHKtpfy87X",
"1HkysdqChotNUYmbVdCNnypH4AjXida5hi",
"1BsKgeFRig6QEQjkcZf4Abo8nL5ys6Xd1v",
"18BZVquVN17foaS98rZyGz7r5P7pePdGe4",
"1CduoVSGa52PLUkUkzjmhbTZ6DzySbqwjp",
"1Q9qUDDgLhiEZMrW61ARWjoiTt11PgzFE1",
"1JBeEJwrRjo3rQQGNqUsuXb3sFvSFTwAdF",
"15URBAQztQnZExo8pS36S4pw9fno6FAbWk",
"17ez3NjnWkUYtFj65Hasqm4mAJWWM5vFqp",
"18be67yTr3yAvLoVodSg7iGugc49kZsa9x",
"1E5ww7De9kBA3asP9Vh6inucS16wi5noiK",
"14RTitQ5sDCzU2hycjjz18sw1kMK7DEGYW",
"1ME75i8W54rT5PhZymF95WLYQeELxuD5CL",
"1EnV3TWemihbH9Ry5XVt2FMoh5qNnbFH7m",
"1DP3nv7E4YXCf4Wg2U4k4mTYbDMkxQd3uR",
"1CzJB2GccRBSfcUffHtGTPDRngtHicwQ36",
"1JqXvTqvuiB8o1iAn1By1yi9VUQoy7z7u5",
"1EsYHamAizrX3BxZLmminDvFL6aSdMPCJQ",
"1LwpemGaCeKKY3qHJd5qFP1uDtGy1TtUbd",
"1A1uJctdovmx8Bmxw4qSkyHTteZ7NJFNUr",
"1GJ5QoxLeA4UfXrA37jT7D7BLiRWNLEzv8",
"15SCbV3wYywRZetrSMXa4NspBJGiFBiZ6o",
"16yp6boxoRBty6GGoZdpDRuLVhznV2PLJE",
"18w5gTxf4ZSVtPLSCuRA5SwLEiJ85db37d",
"19ERo5Dd7ZLk6QUTMpqBUhXsJDddLg6ecN",
"1PU5BQ52fwoACKEuotpxewkkrXk4X3tjxc",
"13gAcCTxr3sF5oqkqnmKMRpqb4LMx3h6fn",
"13Ayg26P3a4DR4wrUbJTrfXANuiwyVPr1X",
"1xgKZUuV5AEQBcxELD4GqGAiiwz591E3m",
"1KFGxxUhy9RaTdGeox6rrodwhQ7eiWjorJ",
"12GMiaiS6LnizDUbS29XEDLjYrzkNRSqQJ",
"13RVnTGtriR2NJd4j6yzNH5AHPwrWpc4aC",
"1NSViGZmQW2GSNhZk1rJkrhRAZz7zcV34H",
"1BAv8udrM1cnGGgccLe7xAqtjA3VGDckDv",
"1LgAFzLDxHc2nysJ1eZAWmAs6YYCYjXJTp",
"1EwPVEjnrsPNZXwu96qmgvjqPYnmGCrm6c",
"19p9Vd7V6NyC3K36r3GrVVvuCvb4vniPMK",
"1GAJkuM6b3ef9RZpjahP4uR8JJUpHqqehq",
"1Hh9L2JiuFqVDvSymDzgu4dctGwqErp29o",
"1Q57XXwvybHFSVgAmNwGgctuKzmaUiR5VP",
"1HgfsdL27T5B8QHSVnW7VJnDP4cN4DoUB8",
"1PV7cthvQfe8bj2TkSMpDUyfDpbxmE6SBC",
"1CkQ3EU77mrHa6b5yte8bmr1H41n8MFn9V",
"16EgJF3Dyvng9PQN9FJ2gepqY7BG4e74rC",
"1PucUBjYAfe1hLp6c9fYH1zKyKisbHjv8R",
"19DG9LPaBjbi6hNTTpyyPDrF99RZ7ZpLqy",
"1BYqueSV79LZB5kPopMF8vfJ1LGVWL9QZz",
"1BFCZHibBoaXej7aCWS5LVXLR3tfnZdMzx",
"1Awbpnr7KxJC9EP1BgwN9jE3MfQUGmCx39",
"18P8VZKyrZbrTRciB4Mx42vpkRmSvv83xS",
"19tCD5Q6Ago2WbUFhZVqnoMf6Zf9ckmDMC",
"13JjDEiHNAcrzsa6HW9tThQkcGF879xeF6",
"1FH82hvMw9XbuVUj7o9WJ3PDjLfiWXGH2F",
"17tAm4UgDcRs9BWnHmnzqeAJVSvdTDZw6d",
"14upxPHfAUU4W99N8qqC21UBvcXJ9DhZVz",
"1GssEh7mDpMZZ18rbxWeJ7ijZtWPpJX7A8",
"1GsqokfP1V4NrfmP3t2i8k1k6fN541YNJV",
"14DukyKhLfRLqLosVffRpeE3J7bDr7c3vK",
"16DS5C9ogcDdSWi7AgdnAPw9KjK9jxMBLt",
"17QqsSc5QraEXxRsLXiRubvYPupef8EK5Q",
"1CRaxyEguzgbzrrFVvhG9o6Uy2AhAxoC83",
"1GjULzgk5Znk2BMCTRmbDvQhbQCToAayfX",
"169o9UEWEG6L32k2ZBtfQnTkeDzUAuPcTD",
"14gbEDrwDo3pdDe1C3ybpx3S3ExDvKL2is",
"12wBEPHVHY2GqTgYtMwLG8kF8t53ShqFvv",
"167EWUquJi6AFZsYupstfYuXhdHKqQNQAC",
"1DF1beAQ1ci9C1GTZSyyuPso5MLBxhKAs7",
"1N42nWfNsXTE4nwvEYQK4jWdi8JXNW2pbK",
"1M4fCvaiSkbrTvFHT85AHhjjTMkyYk1VoA",
"12U26iqMXvc91qmmaBZz27UG62ZeSjXcYg",
"1EiFW7bDYH7Epi71Cm7Rre9MWM8GnrvkRS",
"1BsvXjXhvYAa1NZW4rBpsdEGt3miK8KXbs",
"14FJYkCQwDH8KGYvQvs4hAsQSh5Rqqa4Pf",
"1EUdYCF3f5sstDyCQjP6zVS77cQ9dykzAp",
"1LkksmuyAGZq5RX2ccX3Uw7Jsy71GUwNpL",
"1CkhLcAfyLpSyUAYo1caHSrTGjS3Lmm14u",
"1M84TfrwRxEiL4gQQxFooxsZ1zW3sD1cc",
"1H7d7nLre9i8sz7aoHT52JVGfzKGYD1qzq",
"1DzUokriZSnf6usPg8Sysyedve6QT5aBBG",
"17d5kiTTTHsuwFUDAwPfJQmybemP9geKbf",
"1GYcXKgkZtRK2kfzDbgMzZLW4DwEFydHen",
"15tR8YxebCxaKBnf85jzUkR1oGYp3K4Wob",
"1JAWLHuheKf2mcGMcZUwbBRh61xT82BRHu",
"1HeC47QtzTYU8hyBphZjmF17AYzD2vuVYd",
"19Dt3gNkrz6FKdVHgvXjhrT96gv6AuV3NX",
"1MQUfDocNFTpY8D8diD4Z87j7qnfb14ite",
"1CyGy9b5nD95s7t4g2SwjkrVmhXBYwDJts",
"19FuiCqudKLcQdZygahW2TQxh2E4RpYDha",
"1CLuw7hDEUGAKXDjXVfracCNKGoFrt1D63",
"1CYubpdTdNJHFP3QLXHpYQDbAAQeUwBdc5",
"17ciqaXeV9wTPryh1fjgDbUt2AKMi6fs17",
"13pLuYVNiYnmhcNaDeLsuENcVcwhfw8oGc",
"1GGdbBsiNpzcjRQAM4WFbAGRa9DF7w5z3g",
"1PJP6TG3XVP92gMd89Kk6E69Xb4yc6xUpq",
"1LQ5yyrydWGyaoczR9DYPCP6r42RL78DoK",
"17SDeCCsghTBzcaMXzHGM44HBXMgg15sn1",
"1BF52rgcHDd9JrseyuAmZKenxnHQRsK7pj",
"1FRxxV8qpdWJSiW3hNGJDqmPYJKVAL35FS",
"13zPTr8i3VYRi5nb3KbGpMsCyCjvfxmvw9",
"19fTM9UkirL8MaSF6zqXvNbYMZaBAuCbwR",
"1AocbMisLE8mnd7hXQ3FGDHtoNmHdUgLCK",
"18RYkAQmtBXf7XkREbJ7kUxazqbSnUSzRd",
"12m7nFiAwWVajxnjwskKhJusRnSuhRDYTT",
"14N6HWiqgvKu3jq4dcMPx8CxgZLisoju1A",
"1KekdLCZwVy872CsM27SqiKC9e2QydT5WE",
"1GeaSdFNn6n3zY54uthCePtoQY6NjvcbiU",
"14gPTt2XP21KjVpqG2Ujxfez24GUvjK6Ji",
"1FfZTf8Evx6WpdgaLop5kr2Wz4vGX78VBV",
"1DcbpgUYggU1uk5j2QF45qHA5QHonQjaV2",
"1KDFCMNUZg8FuG7MY6G31VkxyNudLH6Xc3",
"15cqScaLXVX422bVLiovwkJbLV48gYsjnk",
"16ys6AbJsa6F9VqPXBB9SuJmKQNi81bfZv",
"19fThg4wVqQaqe3Cy7YqMUxsXVA2o4ZXh5",
"1JvpuDHdLPSrMVnBmWLjVPXYWonyn1bA6N",
"17uCTAEfd8BDLjQQJnAD3GEd1LbeK9AWWy",
"1BvLpGxN9gpAiRfWUhkb7EbPj2VNV6fhkQ",
"1BjoGBVSQ4rtef8CZuZwT74pZyaPkjPm7C",
"1dgUVBTiqAZr3cRAPDCG3FF6A2Wx9UxVH",
"19TSVciTgZbq48fx98jcbrWXuGjviyaRSu",
"1FcjW1Wjs2afLcvKmTLrDgHRPamsCbnGJ1",
"17TDvEzbD6BkxJ3RB2RubC4nsMSgeXh4Rr",
"18LkM8QsE41XyMDLuGCuKZEoBeRG1t1JGp",
"123R9bWpPZs68V5CZqti7ss8cQDAgqZp9Z",
"1Lctad1roVY5G9zYoUwGJeAEsu967bzJta",
"19Btd2W1Jk8D3KJpacs68m1Fhv1dm8ms6t",
"1NBmXygFWDUSjDAdjgw6xtKjTZw9dnKUT2",
"1BPR13UfEjPFDGxGyunbSEZ33QjQPwxDaS",
"1N4hU7ygUDmzVbaEHMQHTAAxBP8FRXurXr",
"15kCpJUxnwucx3tsMzjN4nsF4HZ6da8kh4",
"16exsmbTrZGkTn22heXNjG4SdhPZ3mkXk3",
"1PM9ug2CmnLJHRZo5S4vnxEJzjeGzCC6oY",
"1GTY8to63iVk9dcQGbzHTx5yTicHZbCoZE",
"1Ncccxc41cCmJh7SXnUqJ3WYFNJRV8WybH",
"12722DBwgE6oQpNNTWekuYR4mYouysyBtz",
"178gQMbGBnAiAer9FPKvj8qGtUVzo9yFCh",
"1NdtrW88DsP5a63bh6xqZxNog41q58HrzQ",
"1GEgyXBv6GHBCrwT1TCa7v6HGNSuRv89Mm",
"16SuCyNexhP99Cq7qarZ9FpGVm39L6Kvq5",
"1NSkyGjs6xsuGYkNpWJSWFkXDQxqSMhp6T",
"18zpDsfdQ3pGwGf8wmyXp7bWYhNHAqTxfi",
"13nyypjGfV4hV6kfvXdq1U3NnMdGfqsvq2",
"1Ai9S48SCrdm5N2bTUf8NiwYGv7DsXTgck",
"14LtHbTDggfjEnNbA8mTYTVsbZe3xHUH9a",
"1JmDNj6nAmm7MWpeqE3NQK1pwmsMnDgGeU",
"1CjQ1KYEALFvGhY2id3nma5nePxzWkxLcQ",
"1KHmVov5n2xnHRKPJBmEskGFBJE4yyAfB2",
"1Ph16M5qVRGWL2KSiJLLXBJfEPKnooUoeG",
"1BovRTU2NPM6Zs3KBBi8vrjzvr1oA3cCqn",
"12VgXJYGcf5dXCjzBQkBokYtawnZC31cFb",
"15JJuj7QbP1Ywn6sNjQqG5FfSZeUxV856L",
"1AGEgpPj6jSw3eUzRx2rwfkEUFVPAhfAGE",
"1JcZQNQrYuwcMfZCNXz5T8PwTD51xNR9T9",
"121at7gReguYFq2JbjzvPg1UaN3jmqCDE8",
"1PUUTa6MiYEeKCWJGBbPKdiLCw9t3vTkjg",
"17cyJwqxCaoM46wytKpzqgC1VD5R65jH6w",
"1BwXSgWSZSvDy9A65EQMxUvZ3iUvfuCi39",
"1EoXg7UxmbxUwtVSb3A8qyBdJS1e8EqvJH",
"1HukskJe3FjTbMdx6AwR23CHUKqYrVg2EX",
"1LYhJ7WLuiDWYTRmMZoqPgXK3RBNc5oebk",
"16sP6zZ5jvY1e9M3VLb5qbbSTrDUjWosHQ",
"13DTo4mBSnjUxg9kwDfGmGwGcA5sCLRnWR",
"13M8PYkVxKtmxY8ShPyTiD3in3qpoyi7ny",
"1AWbhwVicBiCpYCb5X8spgQ72CqFu7wE32",
"1CrDuKWXJGuAkBZotVS2UtGHWq9B3j8SZs",
"1rCKcRMqFSuLDekHAr3RnTQebqm1v1LUP",
"1GGCnXg8AtoduYQXsF5n9YJh8Siok9YtHV",
"1FrgYVA4NHa9xFPN9iKzUUE5TEhXsVQ7ns",
"1AhdBKbMCv7yMMHt4ax6D1ZiGs6cVL7oCQ",
"1NTu7sRuZhaN58KE7DVbqeFWuJy99VvvH3",
"1GwBx47h4WAtkGwfXGa61e1sRvYYfijreN",
"1DvWK25mRXPxRY4jRMQNuoyYxAetkBnjod",
"156jAd2nDHmBCVN2ERjCPxRqwEFJ7xXH8K",
"14uFTehbR4sYg1E8kiSUrSrW61sSrvZmkV",
"1Ma3yC24vRSvHTkiQUmSZA2xtREi6tVMJz",
"1Jjwvazf9j3nrA2YKRHPzpsMwWe3nVJggp",
"1BgrNJWcUyfvPPXxJHcsPHvmRLjwyvnAVZ",
"1ek8wVQemiCWGU7DujNkxz99djtQwamDy",
"1A3nNgLvK1fLp2YvBEopA4o5KZaSTMfStb",
"13RqGXBW7sk4AEUxMQ37E8JK2b13EwaJwR",
"14wPoUak2cXr713PbR3gfyMt5JHFfTA7gf",
"18BBmsrfCaSasMJV4RB1wcJ5CycMfMqVfJ",
"1HU6dYDQFpxgNCDhE7xX4jDKnC1WNWp5mB",
"1LsMsjWaeoW9N3vzrAZn3MaCsK9aQwsaya",
"167PdTfY7ykh7rYGpcCxRSVoevJNSdaMXj",
"1AwkVLGNsWtkboETpbqi8vfVz8qT7gZF5v",
"1MzhP6cTofo7EfAb7mf8k7veC2X8n6Hau3",
"1NkQpQ5DRvfcgShpavhLZtf3eU4ECKtTFS",
"18jjc5JEr6GdcRAzSXPtkhH4Snoycpuc6j",
"19CTRg7eWzDD9hse7eXhrG1A4P1Rfm1Mpa",
"16Wfbc9iypY57DkymsZ5CL7WQj1M5B3YQ2",
"14gtFAjHf8fK7NELAmYGqYEH233Kjxdhos",
"1AaJnnAxQYrL4Kwk4vfpmeZsPP7USgh7X6",
"1Le6VveCjFuxXdkZ6mPkzK8dpFsJX6V7z8",
"1DQQqZwFZYKCY7xP4vTJin3p9tYaTB8BNm",
"1Q5ZfWaD62HHVpmP49zbdp8KaBUeEthWos",
"1JzVwNWXqWF1JhAgTHSy3fmeWcxoxdACVA",
"18nAMDNMTpbAsDMzjyuwheGmf8RGL8XL9d",
"15m2K8gtzHrRSWmaZhtRbcnM3bVRZ5RgXD",
"1H5KWTWVYwmvaqEqG7jbwUtch3CVbt7yjE",
"1Jp7djsiigKR5xBRaK6yRZv4J8wH7oHXyL",
"1KZF7jyAqU23m8819TB7f4uiB8pwsKcX7V",
"12t32VbHAgyisQeeN7ZTWkHXAj66dzbAuR",
"1E5ZgjR9WPDZAX32LBjmEjX4b92gmTMVyL",
"1FWd5EEyKvZTr6gvMhCRXmavozxH3JGDFW",
"1MLouxmUvdYSqK1QDk73x428a7An9b7YXE",
"1KBXoQi4xzEq9s5SFEApNUbuEzjzf7rXbU",
"1F7MqmF5XkwXUnd72zLrCLLygF1vL5eRek",
"1NcNYvJk1m5uUUqYG8HbvRdogB72FbWGiu",
"1MQuRXBD7n19gZQyr3EpNveSQiWAFYTEz",
"1AGti2EjirAnSQjb2aAzhYZWxwLivpESTp",
"1LAPjKhVtKnLviDm9cXYNFYiyttePNnSdN",
"1Ett2ztv6JR72oJDZDbPZcmN8EvXqiq1tv",
"1MiTkGMNwif7V55wqYqQbBHzRgEL8mFdDq",
"1Bks3tT6Z3TfPUD8ptbBdtouVf8xyb8KC9",
"1K4hEFY8TVoNgcXXqLAm8ERbxC3szcx8pS",
"16h8mDXmHrLCnJ7nvkWdfBCKMb3iXpjptM",
"1DNdjUgcjP3UshZaRzJiXLbXFpatrgzivu",
"19vfLtxRkTPR8k6tJ6SpSACrpigfLMLnMg",
"1DJqgd1iHtKuVKThock8droh62bqMVDVjP",
"1HTG7xH2uqPz85LcRmAUBgkiPgzmY6shmU",
"12NrZq6gn2jyDNbUpKhvbFpdgHcHtFLusc",
"1KVnXHzJLUBNKtvLoSQVEAE9qzHT23d9nY",
"1J6qfacSb7G7rwfdSjAUCCZWDQKyQ5ctsR",
"1M7KaV6KJMuKCuAv4i8Jeu5L3vUqD5Vhdo",
"19gyfRtAPvULGxTiuGDhfW6VDR55XbeXZc",
"1CHRJBpGndCAtqp7wF2PMV4BAP3bYA2var",
"1EYxSKi52TJuRozYSa4NpNW4c5yvqti7sT",
"1H9r9gPdSFZoBnrcbx4BH3u4fK7PdMs49H",
"16iQw7u4BJDhFKfjUKccAYKehwWAJWehzz",
"15zpBy64xaqWYb2KCSzTD6Pgjcb8aJxvH9",
"16jd1EsEgQK5JDqMwZVoLqqJKFnrbEs52a",
"18VRZy9cu1d8HscAytWge4fZY42mw44k5K",
"1PNPa9V6JWEFxYHBRMG87Kxpr8WW3huSoC",
"19xqDuaQNFsyAngLeV6o9RsbKavDmSXANu",
"18AsbH58Gt7C8UNjxgBWXDFGNC9hWRHUDX",
"1BC61ecewqrpuUYQMad1Ldc8xrCpDNNFbx",
"1FY21JsygJq8EoFf3Hyd7svUkJnZqx8Qux",
"1N3rdB3PCWEnsQGA7mtjGnEaXMd94HWQec",
"19mHu45qPZK4LzvnNW9j6imW3cxUdMtZQz",
"1AAM3YaD9eyrvPuqT1sdnpom9MmHEpQv7X",
"12kTQkvtwqXfo1v7Z6qLuAtUHHFwKssv5a",
"1GuKcoB4kyHQLjJMLe4QtFX6qSaphQnMCY",
"1BAgDnJ3DQEfq8y5ibpKMNWj5Jmai68JWp",
"1GpAdEhsUz1rXJDwVwz9merzwCmTQTK6AZ",
"1Au4fdSc8Pij2Asjo7iEt6fFond3Lf6iuW",
"16QCqpNK8JBRT4nqfoSkcomBzByB7wWCdS",
"1Hmzh63RdC9oraa58bHzo8nv9Hd627DLbf",
"1DchaN89MdwoX8fKrq6LKywq3t4bQf5HSU",
"1CpXsjD2e4EK4ghv648p4qX9UFp6g8BJtv",
"1LWHDBCBnaikymtajr5RTg5hUJxMWxd2X1",
"1WCH7oGho8kZ8AzAndEcHN8xwar2fDFFy",
"1DKdSK827pL1W4cbkoTvSRNpNaR54zLDsr",
"14rJjxTDbW3CXxmFGvVFvsrXenk5H7a95h",
"17QpoYGevXqpVLn5ihqqSa4nVvM2JAjsEC",
"1JoMcwU4vNnMjUsZGCwJGb3N1CUuDB6j9Z",
"15rfrGENrfDrkcsbbXfH72xpyPtLqzM3td",
"1FLKHu2ffUHVyJeJqd1yDpbaD2zD3ZYhZC",
"1639AUdaf3JSmYaBcsEyMEtHqNJo2TTUPt",
"12Dg8G43M662h9K2a31bqZ4ChqWriVZDdJ",
"18xKvLkLDLMv1JJUVzxJQDRr1irzevmohK",
"1ELhENc5Z9gDZYbzBsFJPhoQbR5PmwTuYJ",
"1La5r7uJ52UVCez6ozKZZ8Pw5CrTxm7vvm",
"1WuCJnjZTxRTiud1Q1VXEfPcY2zMvnCqS",
"1FC5mannfKiMz8qGyvNQwxLsMLxw9W6cPo",
"1G7d9eYnbuqk4kfG45sV4xDQejEagob7YP",
"1GsJAoV1GukJi6DFmgfUnp3w2BZfyRfp9J",
"16Mouyv5yes8wC3WEVf6AytSvAPUWxhQV4",
"1GTB9wWcvQN9hHX5Yu58ocBjPydGthjv1r",
"19Ghagw7Qs4jXVRPFkZyNS6v99NzqtNik7",
"1Q2vncDtkfPr1nn1saVkGZ1NFogpaGtnVc",
"18GCPivnvNkWXMXsxDTF77ghScGZGGwkvV",
"16cudJnQs8EXaWdoMPc3dtV5ygDzw3HAM2",
"1EGufitSh3QRQAny4NjTPQcyRpDAvUXe9C",
"1NrPd2qURKwF1EY5Ld2pmRMJKBDpF4GVnJ",
"18yYrWLjVtgZR5DjH23ffEeaxLAdebhyw1",
"1AuYGp7MFYxeApfuKpctDmEqVLzrfRDatp",
"1JkJF5vQEWd6yiP21prBCQwAJ2UDYqB9VV",
"15ZtqT5MjKzVJg3ExYZJmmDsmzdrHsCkst",
"1EvBw1dppP59nxD27SsbCCTnqh1tkcqmnZ",
"16QVxL2vfM3czuX1T7DQiM5pxsvDkWWnpW",
"1F992qsQK213NCuwMfBeNHsama4ukt5nyM",
"1AtEJC8ZTRkjDT5RCRUGW8tnp5wncNs417",
"1MsTkD8SgmpVLcD5cg116ZgDRALzA41qJZ",
"18SKS2X1amdBrCHVEaS97D9723KXHmM7ee",
"1EuR3qPt6DRM4unCAX5MSxMoJoy1ebo1PF",
"1GGQTBAh7mESuoKCdQGEf78gzLdRE7JFNM",
"12rdNEjsqBoAZ7KKLYjRgAQFEYze35ujGw",
"13YLKH6dW7EodUm1FGfWLGNfXuDBEerYou",
"1NBxZXNrfGSThJkPKDiyXFBA7zn8KdLRho",
"13m87qM1RHXZvhhFHzGBg3sKfcAiT45Gpz",
"1Db8mh29sRsTr8pLnokuMWvdtSDCWd3JeU",
"17bFpbkqBF3nftm7gQvLnwRMHbsaUnzYsm",
"1MRVUVJBrctP9zdwvAxf5SHX2uxnJzTqpG",
"1D6AoBQqp76tyjWszQVdW2SeM3cNj3KR2",
"1FCAjxRVmGfUgJ6XM7Erh4ERh4iyk9QiYx",
"19vc7sJK8K4AJgXV5QTSBNgGw78crmLHYm",
"1JENxxKNiDzVX1RpUhzqAa2STyghV6K8FT",
"165X7BxQEEQiEo8R2TsRfNkuUTXqbKciVE",
"14gS3Bv5cBFpzkTDDuNx7V7Lyi1HgTmnFy",
"12myx636nPtiwudW259qjgBoVhCENQ9MN9",
"18AWBvxZJwFa1WPbeBH8P1LwuXWePv8tog",
"1BMUrm2sJuJqyzs1RDADMB3pQnAGUN1L42",
"16SVKAK1qhJVVmq36S1wiCcCDUMpC6JNg5",
"1LmyVynhfzVspXxXa1ceDDVMAJR1QqAKm1",
"1ComgvaCWUS8mU41WSK3NfyBg6pWfmJsQw",
"14v8jhseTAw9rrtNGAKXcT2ZzM5S3Xw7j4",
"1HLFYHpTnDCuqnmE4iu2gKtZA8YhdC26XJ",
"1BtEsPNmHFa7DcG4esg8CMrhKTFb8dD14c",
"1FKDzz8iCLKtQkUeWUBwEZuazps9gU7Wuf",
"1xQXRmWR38ux2FJu41aeaTkDg8CZ7Ugpj",
"19fvweQSkDLib1ikPM3sVXiNh9d7tVduyr",
"1MuZ9uZrPTnjYqHJxB5n1L2TnFhi6cGuvF",
"1AtSDbQbzUwy9GHTwGGY99XrabeyXxZSvo",
"1MjaQTqZvUZriN6mf6GobfQ6MXrS2Vp7Rb",
"1MfkrFCAQ1uqQ67uA3xFFTcnW4PwHHc8Dx",
"19qMmWeHU84BM4jZerMFV5WEUbwC8MnDPv",
"1DwbrFWX7kK3Y9Uv2QzaFKg6Fq5sUPNQwa",
"16r8JZRvx4URHzss8zW2HtYHQR9hxkFpUB",
"1itCbdGgpdsmj3edNVBrQi89nDKFLBV5B",
"1Ht36wN1jBkftCVFBscbQs3dLbjbYxtp8A",
"1JZ6W3gorTn6nDzaRZ4CTYTegJ44RKoe9e",
"14VdRtLmzm6aC429X9mjAzvCseziEABQra",
"1BvAfcoym1PoXZHhAwnC5P2qPxMeLP9K8Z",
"1BjNBJTPKj9Zy5bhD6dFgFgB2WGsHtf1kv",
"1BLo5K92SJSerD76jW1bdXVwdU3hc6Qjvs",
"1zmqPzuZ3unADj9Vt66AYNspmuDKvzPaS",
"1N5apVkz8mU8c3LUWMGTRb5LQ1hvsb8tn3",
"18se5c42uLqG4BGmLTqkuayF3CpFfjyQZB",
"13ysMh3S7gL3DVzfGw1ftxMXfKqu2DHpG9",
"1NbYhL62jA8WHxo7Mmxu5iicaERp4vWFTu",
"13YVfKMgBDibHqVZ8fN2yNVN2DUpmqYoM3",
"13qrVVH1kcdgKYG7LeecxbvantYQmC6NTf",
"12Z3gfMKjnAR5T89rhMNjYtkuYF9puZi1V",
"1CezsC2LsbBr9KA7ruuJnr7kGdzbhSs9iF",
"1FxkzeVeHJU1Y1aoUaBtfMs4w3Z1BnmSAz",
"1NUqZ34Xne5Ay52g9nxbpCGnZS5turG1WE",
"1CSfuMPDNUrnQUdaGGaB7WDdM8J7K4KHV4",
"1JTg97R4WxMTkUMyhb5zMcjGtCDi5FrUfe",
"1M4nzxEbAxi88VZTuGEdZEQcEsKTWjFPEF",
"16uw8whHAfQuJmnaZSw8MyX2zPMggZh5H3",
"14NyfiWWfzeGbDte4rqLq2CeQ5cJHmhnhD",
"1G26t8AxUDi3CfjTczGerNvnLfrt9E6Mv5",
"1BvmgPhuNqUdeQTHbdJDykAXfqqx7dcnSM",
"1Ffmv4hCLbDphLg9owFjwRTwFRVnuafYrr",
"1P6HUBYaxVevfKAUgNBdYyPzxhQ4iv4tYV",
"1HMZBYzEyzciW2v6KBjJwssdvAdq3YdhBd",
"16nPaxwjDTSXonrZ9ELTtat1xWabSRDjp",
"1QAUTqoPamnawoKoKS6EUpjm9cHHRCY2Bq",
"13cQ2jN63vek4qCyrT29hJR5nrmaFdJmjr",
"1HyJvsGbjahuAJrZLFc8S5ahefQWqFDtrW",
"18z3QD4kFbHta6qekaG3vvc2ybJXAfhB8B",
"1Dsg3h4LoL4dJFR6Y2e5U9xFEPawFD5R8c",
"12ZTEDkHbHJyoexms4mUcXDe67ucsEwhQK",
"1LShMHu1U16NGZsXHExBdgVcrJLwED8ha7",
"1F8teDEnMLGPcx9wvRMUYZVG3Qp6Vep3DW",
"1BWqu86kEhQBZu72NcfDo3Gm7wjFCQMFW2",
"1DrhfZJgDtznAtHvAbh9g1w4njVTJCndro",
"18jYR7RF47cNjUcYCnejx3oFYnKv77AbfY",
"1FcHvjkMo1FrbDCbucxVCc86YkGnKc746p",
"1F8NnBAFmgurwX9jySssP3DaohZpCTGAgA",
"1B28XxQoYSvubHknGAMmt2dZRt4UXcEGHb",
"1Gx3ZLmSstvA4Rcy3ptRuJrayy9TxaFTW1",
"1L9YcUcW8UG9RZEAgb6mUY3LjYxWBTzx3p",
"15ZKFevY5Nm7Uxrvz5gDTCNhiFcuW5SQLX",
"1QG9zDWxhFyTJVHr3ukuhcHroXbYrMrCG9",
"1DMiy2sKGh8jhBWZoBuE4p5tukF41i5fZH",
"13fvQuooxZvhCfeNY6WV9jEEzETbeDVBWk",
"1Att28xdUWTn6CJdArC585BJX9GEfJjThB",
"12kLNuF4ahmgnMjXeSNezxGVmCFhzBNCRH",
"16bMHZNisqXXfNpqucvvwkDoHpn1bc9uBH",
"1751kbnZ7EfQGmvKruERsA7MfKijgQGLSr",
"1GBAo3Jrxe5WHyjXgPkPfToDQs4mWks6Cp",
"1GPFar4oQbMZZVk3qSriszkgD1a5cDBatY",
"1DZ5mMJ3hNBqqFV5uXESqj9XFDdMGzKrGU",
"151W71Ac7FLFWSByrnik5m9LXTSp9jhz15",
"1Hc7to66aYZMP7wbZuCrjuEPhzb9BH4gos",
"1Q5g5B2ccth2U458cPwNK2JK2x8seHGKPc",
"1GqjSwui5eT2ppfbYGo6ueuoLBU2Q3LMt",
"1Mx8ub1Zbryp8BL7NYrBnqSKAh4rB2zSGK",
"1TXq6WYrT8y4HE8AngXfH886Eoa3SeeQg",
"12Lu9DPXajuAvegouc5n3LmMYTmXZqMYA8",
"18g9MEjhmub2P8rtPyuGEL9LWRjrgFcGQ8",
"1Nf5aXzYmrh1jxSJ5exeyPzsKjrBbNdEam",
"1M6x5Hfju85ubkAjezLV8AoDwcMbp4ZNX7",
"1DsMbbCSA6F7XYKiMfNxnFrZt9XCwBEk18",
"16na9PRnDCPXWZcZYkpnTHqqMHXX2vaVLV",
"1B1ckNofYoyDRxrC2qf5gKQTCtZydNWJKQ",
"1Kgei277x6zWhaZJgBKhcooEyfMAXz7zYp",
"188cehKFuftURShV3zc44cZzm5Ng9s7scJ",
"1Ag3mocTttzZCBeWkYsPFchPAn83K2dS9o",
"141BxYoB3D41zCYtSEXQCisiTb4u7Ur5w1",
"1HYi2mtEpKNK3vX2bfCUZjqPJLwCSXYb2K",
"1PiKh6MsM8K8JWwWrWmbAJmyriCgJ9j125",
"1Bp235XLk4kpPJRMeuoaL24PcU7kQZVzcW",
"18q17qXNXRQsLiJ2jtxiHDysKo6CmPy8CJ",
"1JhvwzjdGytghHqGwNBtfp4MnuTjCmZLQX",
"1GHXyUwmQoZWAEAmJvPHM9MLcaktavuGKA",
"1BXVqWceKQZ5K2nGFobTvez4s9Y7NdPN9U",
"184FWfK65KkxPq5nFXt691tK8yRC8XJQnW",
"1Ck2JAm8xXwG5rpPtsN68imyznKzyZmWjY",
"1EaAkvK7yCwAieTFaNtGFVfMAqSnZHsR8X",
"14oapoDbcXYsjeKdFdZAsjVDMbXzwvNCwR",
"1B6qArpfn6xQSD4ixJsP9vFRtdid4mEBck",
"1DpYaPWmvijobL4i3Gi4qcCaiYcqkG7Qay",
"1BUNmn1731n8qCxHrywFYbnGj9jGpu56vK",
"1BGnx1xRDg8TRewXi372idUDyqM7uR7G62",
"1Pty6B2GXUytWtodXhPouAwyDaH6nK2P2s",
"18bGSJRz1SFbCWvmQjogHDvAF7s2qybRnv",
"1FQvKtLfkKJzFeV4mFkRJqga7g2y82tkoH",
"1NgKoW83cs1ydKfaweaNJ788nYeRi1PJVR",
"1JmgjRceEsAsVgukRSFzdUf2cgK8P57W25",
"1JvXydRD53JFEV68wTBJxdsH62JDi47HvK",
"123dfSwoatPvZbfkExG5Jwz3zDxQKy8KGw",
"1DwGitN5tKHFUEtatJYtZi6whb9iFN5WRT",
"1FebEgYe9sHTeT4qajZ5KHUw6hVYmPL1vU",
"1K4T9HbbAopGFc9Tps7oSL2RzSSUVcE1QC",
"1CY1A74bykEAdF8fFR3Emf9An3xckBQVoZ",
"1PUmYs8QN1KShmdNgPZnoqyjDtocSg2Zzn",
"1BUaGFW5Yp9BxhstjcVqrTberR879p1hVU",
"1D2nfTybmCEuzCjeW23EnTrCdPewHyw38L",
"18Q6YEMSHSb1F1PtmdVbKQVNgTaR66Yn6h",
"1FEZXKeGC6j81hefpo6zSzDz8vp4kR4q4M",
"1BrMvXcKqCFoFVtcKykguDQeXsAwSeq1Az",
"1JdRpqqiYDAvZCzCPYQMMdr97fDGUqLWeT",
"1J467d1PeTyuxFEkAy5B8S1pFEVcVBYwQB",
"18Gku2V4KKsNKLK9LizBMQpgZL4dq1HN5M",
"1BuqJmtPnyxGPHEHmac74yDcZxGxqHDQbY",
"1F6MTR3Sde4XNBoThszaSj8zhNhecgqHEy",
"1QJDdCriMMU9r3eYVFtRhf8xJhj5y9pi6W",
"16XEasqWcKpGiKHLbTjmxe3GmTVexZLojQ",
"1LKby4GFCjbcX4cGWn69Ke7n8BXjogevtV",
"19bsuNp4YTqF2nzqgfmQXUmNoW9uDSKssa",
"1EKb1V2Lp1f8m3b91ZEvNkUQNidbYnE5HQ",
"1LdZmh7tTwhyVhm6YWfSqnc31Ga8KSJekn",
"1ETQYMhemhtqfMJobDnxVMP9p8izocbnN",
"1Li4sQxPfAi6uETUNpifkmMYeKsBh4yLwa",
"13dtaWTydrK19uCyauGDDafELEZX3fQMDe",
"1TaFkCxkYnC1iKH66rFrzn2F4wLgnANbS",
"1HtcuVpRFkg38FnpcKzbeJEAMNBJQ66SMn",
"14aL1ua11J4rQtsQTSjF6f7zdLMqMxzVVi",
"14VFN6McK4Fqja5Xi4mor18popBnQL5gqb",
"1Gi76kp8YydEALmsmzTx1rsM26jemFVfJQ",
"14thkC69VG5xnk27Y9HdLca5ZDkLGYd3S3",
"1BKwFZbAcVampXAE3FeDpUbjDTkVN9SFeX",
"1M5Kjgo7aR8JNE8Fm5ACybbLm8arFCcrZu",
"1JNXbszQQrBxfmToDFxUBprqWLkKtV2h2c",
"1APnJej2dLhNKJsGCy6KTB8MusoeJiRdVF",
"12oMw7eXSHfTHY3aDS5PmiGrgW2QY4AWLi",
"15Z6HQHkKFmY5Np4WPUtrfj6BuMQMrxeSf",
"176M82zWrndFAhvgHMVnKjyJH62V52iwi5",
"1Gj4eqx7TMwzJbJTACmB3kiGfFMT71oThn",
"1FFNA7X7Zox9Kxt4Xp4aSDhAsDBWKEcwjx",
"1MjBMLVHofQfrAq8jCp4bbrsNaXnTZkv1o",
"17NPhs2LsukRAQxd3sYUAym4oiES5ZTeGM",
"1PUvEUxPS1VuG4j6JHZwFLjPy7GR9sm27i",
"1zc19QUtBtU8X1fdrWpsTPJ3fWcVkdunc",
"17T1DiEWEEgB6nxmCVaTJ2hFnwsC4PZbU7",
"14wTC2jzTkpe6RyPgJYqTuahDQKPcm97jF",
"19okRqQyDRRmRGTXdMNaxgiNVXXUKezEDy",
"1B38kyD7f2F1jHV9BXrRQXNBBNFC5TX3of",
"1EQcSdTdCednyJXSoFZV4DvNz4W8YUiVro",
"1DNXTn6WmMooZYgmrA9AqTYpkaFPnAeUWk",
"1JB8GmZPtwgX2q8G491T3S9cjXyJH5pCes",
"1Cgr6vAjUyoeymFcbgLajbrTDhCbBbvyuZ",
"1EXxow5stY4FrofAvG9XRhuymaNRwWTedH",
"1PUdipZExefNLvMrikgx7S6nGtPDajp4Cn",
"1MNzoDVozyFsu2C66Y7NeA8dgLhx8R3Ztp",
"19cvc8t2pyodSxDpmGtYcPtdSVRuW25vSd",
"1KRpsCM3dVxYPBnoGpS19es5C8LgTVUbJR",
"14Hf7jWkLzKs5FTHqYckJrAmeBgLp3EWWe",
"17L7aWachnXpogRfrU7Cj25UGC9sQPnQ9o",
"19yc33VSFRyQz86ZC2cM9PmZ47sfarYojQ",
"19jDnNTGChqszBe23FTjvu32B7d3B7P9Ed",
"1HAqoxqAEXcZX44WWt8FBNmiaUyJbNdtm5",
"1EniEPw94oBbytXVZ78fA6P3aFsqxs6adS",
"1EBqrk15d12n8J1BGKc4HWzjkC9xYhHqgP",
"1P3jh8iFsWdqCshC8NNY88WTb1xVFqPFPy",
"1BZAHayTYdyN9KapALUV5wDqcPmT2TQTjb",
"1BxANAenSe5fZEij1SWA9vwQRBhhYyrP48",
"15QPrzgfCrgrB7E39rW9m7CMXxYWU8NkD1",
"121nS2moAEiAgUbypbMUdgyBeQxWMMGY1M",
"14k5feF1o5QhK6edjBJM5VdzJV7Rr5LE7n",
"13zkWZGfWzeHCu6knpw2upq5R5NEGrW6eq",
"1DEFSz9yL5rjXq2BLyHAuFKmxeYXzvjrJJ",
"1CbueiAeeASwckjJySuCqQogBvSvfaKre4",
"1Hn5zXz9KN578xTMSFGYMYZyp93Af4WjZn",
"1NDg6XWxgJM1TURM7k4TtMn8qRLom66zMY",
"1G9pt2bE8hvxTvLGkqjwMCscVdbHnHUfGK",
"1AUwnKe3AFpTGzya2M26rtyfyyPZZJBxSy",
"1MW9wtTmG2tVFBZ1iUXvMEEC5qo3pBFvVZ",
"13QjeeHqjY5n3adaaiFYUjfHNiTX2QYBKe",
"14D7GDEUJysg7SWKDokKbipLPwhW1WAfaR",
"16bQF5rL89PdhWJwEADttGUpE8NRNnghvU",
"13x1wAr1Sp3BRn5D7yXSaSNf529XRrbNqE",
"19GZzNFLSQ66wTwtL9WA5TysqDiPY1xVmY",
"1DFAG1wDuYWwUAk4Wa5FMNDyMCa1r8KrmT",
"1GBTGPNXAXLTKoNU4MZojPCY3zngzdZQoY",
"1974W8a3PXwR4eWbwQUrQymB6VgTNo7Fcg",
"18gND48BwAjWjWV2UB6VHegKzzmi6ELXRv",
"1GuchYqbYGzAC7kN4waDi8brvEPbBBP72y",
"1CF44GsdxUkW2AAKmmbWKrcTwkgav1Noei",
"1HkE3bJzm9DLR7Nwk7bXAjBJG2rH3adEf7",
"1EoMcpuzA3HbXkFLHtEhYtLzWTjvML6Hx4",
"1EebqFcDkY7PQoyvrk1fzADs9JdpD5s4NA",
"1FVhwP62zD9NZEhWkZ8zxXcusuFTMFXgZ2",
"1ntvy7BEs2shNhLvsntKb8tUUWSDmCheK",
"1BoYXM6XsZsWEtNxAoHk8CTYcU1ctWHsAb",
"19mWB6VzVgaCVbJ33RPKwFyw5BfiHjnVQf",
"18v1VdyJVchcF259Ji9SzRTyNPvqYt6mjm",
"13c9dZomA5Jza69nHGijrVMtSAc8uGpiBu",
"1QDZZdYMMp4wFj6oKgxGQpT2pt8RYp7L3f",
"1hFSHt8PhkMZzcDB6nu4ZPTxZxXFt2GfT",
"14bsjeUPxwtxX8FfF7DkAbEefa2GriYc9j",
"1Ba8zGSGKPtzgabtU2aR61QAbRmr668UJP",
"19Wk5NFZWMH1Pxnij3Tmei177F99TvzV7W",
"1LkrNok17aeWynLesikEgMxtp4V3F37LLF",
"1DHsJePa238Wv3eNsB9YFPywuBunv2encF",
"1DQ9agDybiGk2kd8xbRCzhYtbG9VAkmo6J",
"1Acu4k8Ug4fSKAvVpaRvsfaurfMPfSwigw",
"15XkFFgcUMMdL66VRwTmHoWWtV8JDr9AfX",
"19nT2sCpsXHbnhKXWcTZkKFp9aY6mHL3QS",
"1AgF4eqGTAYX7kwUSahKySur3YKMQ8EFxt",
"1EdTqjvyjFnY947yMiijTN9qm6Xav57rje",
"1KS38Gk4dGdP42miLzu17NULZzHWJ3g9RX",
"15tGR2haVDk5HnmaZFQqvb5dU3sWUsKQn6",
"1LMngoQqpYAWtsaxzCXMYiSh4Ujcexxdqh",
"196w5ufjrMG2XCqJHcgh1QNbgX9sxfTcUr",
"1EsoPrMwEYWMsTB12FtQKAXB7VUQAsf8Tt",
"1PRJKVWnwxcdXQPGBuJv5d3rPp8GiBzfpV",
"1KFuPde4Lct73i2PuWmVUB8LGHqnxGmW8H",
"178gftN8bLxp9du2sQRZ7zpVX3aMrbAHuE",
"17G3jrGgJpLaHe57yXLp7r5kBACGFHtWd1",
"1B3oXrcQdJcU3cRi9MdBHJso48zf4cPYMi",
"1YWHWQkq4V4cZqXWmgvoTyr8SFvvdN6cX",
"1LSxfLqjGzQzAZ2LyRj3CdvPVxj9xpykcy",
"1DCcCeyR9n5Dy38SyUBroHyaqjK11jpyBX",
"18KRFwKvvE7pBywfe16HDPsWCPZaCPcHwT",
"1L7MMhZ9F9dXxgLfdAkbXNqhkGGQTzDfsr",
"1EbT2Hu537xYVyE3gh4jZgotzFvdLrDzkg",
"13BCj1yFsE1S8TTRak5wk9iRrF6y2mALC4",
"17xwvbmDbPmJniLn3ZMcEKKfqQZMrg4EPB",
"1CXEM6Ti88EzK8dQTHKtsuqJgqXuSGBbaE",
"1EUWqSGvoANGfKeMrR8YnEBYVVNd8kMyVr",
"1AZkeu5tyaHersTeyVSCfsJf7qtVk4gqd1",
"1DrnrMerEUoHgD7fqa2PGNBtBRkSHjeJw9",
"1q3AhPSWnhYKPfaNaK8zPugxYoXJoow3s",
"14saoTAzVLSCTAAx7pkKrhvcshqhcP5gk4",
"1EeYEYjn5yf8w42nFAkbBjWVs2DPPdYqsF",
"1BvBFtoRz8rPCwo7Pfv3K4S9UGCwtyuich",
"116ySjzwTg6UxATAYnPMqMzEKdw3hTTTd",
"12MyNJKMsHnJ5SZYRW6iHBSSZViD47963A",
"1T2VCXV5bffFevEkChvAcChgnra2cNxVA",
"1g3djBw9k6uEweKV8pYwyJnPAMTSzMG16",
"142rUMdHBrnWWV3quoY4qYPg6G337m3KJG",
"1BLdtx4cfpvX3DJUTuR5jV1QAPcvTQgrfg",
"1MDVRaCodxfsbXscHMEhryg3A9jMHFBRpS",
"1F6S1jQc8kyPMdGuPFLTHTJGdGqsvHiFWM",
"1PS77GfYfHuHE9CiTdZBTKHSJxLYcMH9p",
"1D4wKVzQsfWoeJogLG8Y2AUEmUnJjWWPxM",
"1MdrTEvnWiLEfYPQ5hJyn2r7kRMzmvCCui",
"1LMCAtKGcUufXwLgqsT6C7SpDcz5EQNBKf",
"15DJPCxJgrB7Hxs9wwQLV8JLreNyAFcamC",
"1C8vKVoPS1QGxq4nFWbH4ob1C9AWvC6FrV",
"1Jkz19k3Ek2prDsZJJmW34Gf1bY6QkRm6A",
"1769hdysEZrB8K1Yiy9xTa4mK7Cbq71r1s",
"19ZbfEdsFqKTSJjzwkUJXJZ3C7gpFCrcYc",
"16boB6oVTMsfCWJZx9HFYPV4bePM6AzyTe",
"1BMP6Dccznj4UoxH5Tgr3QEYdhW36vWXyb",
"1GpALQuYx1bKexnWJu5KGrMLGBojVPSiMH",
"1AxLerc9puarGQ8QRhz56saTSPcJFApNof",
"1GeH4LDFWHNdEtjCkCwTcdtTjAkU1hRhVt",
"1F8PrCJhMgoTKiPWq61jaPH4SazboGu2cM",
"13x9ar97csNZ6gChGr2joQoKeEUF9TFsX9",
"1D15xpQDB5jbvEQEwVmxAiJbfGm9Hv2QGq",
"1BbtGiJwdxwYXFsZ9nJPghUWJivMk8g6aE",
"1GyyLoM9cvw3bEE3nqmcorkYQ6tQPi6bFG",
"1KVd8Ygoe4nQb3YhcL4WzEaqikEN8AH8ox",
"14QTSZ8UTZd8gemfZVi1xJsAmAHvhYKnt4",
"1EHgZT5SLxvSHJw3fzpGWNo1bNq1Ng3q7W",
"1GPG3CGcocPWwA7TYAAu4ttH28Vmmb7QNV",
"1LaBNrpbcX1SFiKXMRuwR9fZwn5g46ymf1",
"1KHXp2xjY8YkiBsjyGfkNLfDogiXm8shyq",
"13NQdo1V2qer6WjmfipS9jFc1z4wQWsrR3",
"1GvM48fNWHkiTnyeMWCTxxPrYqit9oEhZR",
"1GbGg4AYnxpzzT3v9u7W4wNYq2juE1FnWG",
"1LBmWokgeCzzkzN29NhcBzuhmpjR2ECu2o",
"1K2Cv4nLkr8TfYWKFTZCZPmiNbomJRkhuf",
"1m6fy5UAA2MyvBCPhBn73sinSHcHpDH83",
"14cAqYfAqJzYrvpXqHBDrbfNrEMLtVjoVn",
"1MgUWZwCaoosP6u4KwbiAU2qQi4Q7vdE9n",
"1JDZZazCNdFFW3YQaA7VBg595CjrcDsDZu",
"1K4xo8PbZGdMjZgi7t3mzof2qnugZztY56",
"1BFxPETx6ZRsan2eK1mtbYUv5d1VmshNd9",
"1PMD6imDhaT6EfyxmZELEWZPbAQmPrZJJJ",
"16gB5aJg9XAfonvWqVFhTQjnPq2PCGKhA6",
"1FfRjvew1W2CsdR4CcAEwpMJyu9NG8RC3b",
"15imZdqcEvAvmqPSn2uQQkv5zz7FdLpe67",
"13xbKBa4VnqquP9jNWwe8UNcP4X7hhv5fU",
"19etpR2YgKmVEKyW9iiBkJMZeeCdgiJWan",
"12FzugXY3yj5PA2XLaM8W9FwP4vwdARTi7",
"1GYCfWzuh2gLKkoH72gxj9R9xDiZ1FkV4K",
"1PhWZFEe8p1BRnZv1rg88GUJCZBtyvbB9d",
"1PZoBggcJ9DqwsAYTtTLy32gibWB8Evchf",
"1CrPXgR8xj6YJWtpXzUtexi2npB7NURovk",
"1BQpyiRb1VXLcZ1STVfUtFAsMQpSGE9Hag",
"154DJE8b2Sa9F5BWdHTXs1aETg6K25nHx5",
"1KSCECzeZteUN62peU2ASbY2kzQZoNG9Wp",
"1PHy4kawChbJcF2Aab9WMqJmsuAVyar4rF",
"1BSWoRJ2tNWY4AnjiVeC2r2fU4RbnMyN8Q",
"1Cr29roH8xTS387dLFSFeCzGWzk2Buy98J",
"161aCbrMpDC5y4PMHHUeX6psSYXFhX2bQG",
"1Bb2DRhBiFXZsE6UYZR8UFaaSwu95YLDA4",
"1BtGba6NzeUnEApudz997r3kA3MQGy1Wzf",
"1JGmdc9uk1vdJ9ytp63Ax4xYVeFExYTX4a",
"1NzsecmkHCC7L7kiELdzSBAsKTCSjrxsYZ",
"1CVoXNAGLCQU2Je9yP7gwNRV9NDiCYbbBF",
"18iTgJafxhoGQQna9A98VRonsgsw6qxgxs",
"1ETjGx2FwMNAk8hrhSyz9JxeVkac9FSgLQ",
"13yB2JiPbefsQme1YWgv5SjE2yT5dVvy1v",
"1MjvNYripprZ2L7ugED4wVVX8AL6GtkQXF",
"1EZ3BW7WcTQNsjAY6g9q1JMUkBoDgPFF7B",
"1FPwtKHPDMj3GFKGgJqwRo5GRNqL85PVXU",
"12AUYDvhPsD15KBcd9iMdbFMkajV8Rm75y",
"1MnGicK3HEgptcrU4PZ4sbEnVEkh9R34W2",
"18r8NHurhrnNW6w1XqqjjuxkrHS6Si2sKu",
"1JPJbPEZwmkeiAbLorWAYodSjjP3ptDuRe",
"1fWqdv6xdyCgQ494Mw2L7ToqcC529Mrkn",
"1CM8kWyXQcQENzieicrUX9khdWPDP7YtsP",
"1Ltf1k6KMdipcshe4X6oZihuqd3DpoxbqX",
"1AtyTanDrfLE2XxTrja9H1JoCkhbXUTeqY",
"14cMs2QsSV8nJZzzDFZ4ZeXmxDAGXLBQzd",
"1CxJ8Nh9e9q4CrUxh14U29BNpRkBpEJmcE",
"1LX3RdYZqv6K6UDLBR6sZJaHvAgJAxFkNw",
"1FAdgDHtHVL5DwChwa3x3LkurB1cBJ3HHT",
"1F8ennvcki3uqB2bYaLA9PtfdyaJ4zGi23",
"1JCRsJPLhUQ4uiG5MSPYmmK9x6NczQJRge",
"14hgxiQwZvde74sa38GobKeUm9sNFLLHZr",
"1D5QnM2WXdVX7qgX1PMUcWdcGNf4SHWhqc",
"1AVFTzBScJXFjRhrAURJc1SAZL52Fm8rNP",
"1NQhRLmjJJc9KdJrneAKYsS7DYEhMA4tNW",
"14TpzLWCDa8xaKRXZ2UsTCy1Mzh51jb2X9",
"16uNbxRV8XudBh8SxRcEAobVPZ7483o6yb",
"14yGCtLEZsgPCbY4xfpqwfEv3uakYTiqYg",
"16yPndQyy4XrwsGf4149JUtZvc2rAUyCS4",
"1K2A8K8EiHVZr8uv6fBcqz9EX2eeAAiBvU",
"1FfCmAPEYnaDf4TtjPWkzdmUmN9e3oKZZw",
"1PhtXxeipeaFpTwGha71Hcp9pJq5AwF9sc",
"19g2Gx3zDmyjfP8fGfLgWqYMBuLqCRLZEG",
"1LmhHHd59yLKg2SBM8t2iU5GBFZViYHKcy",
"19Mhc964WUriTcFjuJ8vAVzEFFUTrTtCCU",
"15fYGRcL1Xm9SKMJjWa6UWaj96YLYCo1yR",
"1CJDkuGUaHY4DzozKfhzJnG2vy1RjxPtJL",
"1LFFtuZSgQEJL1rmHGreTkKmjjzLKNmRag",
"1KY4sho1BPny6BRSNuxPeSLtyEci4hjKC6",
"1A6oW2GztumDiGN8SSnW7mdXoHm1EV9qpR",
"193BQ5Gh79HLDfXB57vPhMEV6Jyym6XcQJ",
"1GL45SeNjX4VBtKuDDbea7BR1E9bUPUkrG",
"1L7ZSqREcDSD3k7tAsQG9aHyKEeMSfnKy7",
"14jYgncmnUCxEWjk2TFZLMZEXgRURr2yRV",
"1G1cE1UAHWKYPURWqChcZCJJN2c5Co9uPt",
"1FWtVUCqTnVsvQi2iyD7V4C2fcc1iMhM2f",
"165ABhkLoXgYVoXBSPwRp1FqasykjkLFBE",
"1BpNvyJH1so6jZb93j3iChzKhwdnMtqmm7",
"1EGHE46AL7Gu9ZRW4nog55XmsmJSAr6uLh",
"17B7KehQyFg3mGjQYvaEYADky7u7fobZiq",
"1FDYMCD5RU1D2zkoWJDWs6vsg1rW6dLnvW",
"1548fBfu1tFhR5gewiFPAHsDtTSy33LS6r",
"1HPyPrNQxuNFhwseCQT4hQz75jCg1aGdts",
"1LpXacfDyM1BC4ys1tPjkA9cb7zvYYDVqe",
"1FupFcrmhva4cwo9ZoDYHaqiKM1cJRuHcp",
"1JK4pNGDzbXqgiPz1Xj1WSND1rXMPeJ5jF",
"1AUmNQigkzogPe6UwGAbtDnNsg1S3UtGvR",
"1DbvU1ZRCAFwvevLeGHk3Cy8Y8gC7VizJU",
"1D8HKs2ssHBg8GAY5Hrpq3ytAv7ZtKKst2",
"1EWCkCynETHhsVbC1QxXbSnWcbpvFspYeh",
"1B1fePjCCesa8n15TrMp5tTaZQoY1Zvdku",
"1P18rPpvxfwKrTZdivQtNwzBaMfHhzRav4",
"19esc5Q9gd8Kx7kU7DKewA2j63ap9mGL3k",
"1LHgGDshXfrgoTJye3QdUAZJjHhf9A2TMk",
"1MX9wUYnGhe6eE25GvtZnomnht7JR34S98",
"1EYM6AGEFpAuBeFxVkXWPYgxuCpNmgJ8wb",
"1CHaBCRtKKhFQqFhuxbTFgqLGvLXkJ2MoP",
"1NSdcFXZwcJXYJPb62MzE1JhcCUry7dKJT",
"12rHQnoxMWzEYxKgehqysEoPcPyS31aW5V",
"1MFiTAk9bbhGEpVWJhWBbacY5CWYD41gP8",
"1GstHectgFGiwvgPoBM26dxMAFqQtkCvGi",
"16Bt9yZoh8fZDhqinFrXipxhZzugKMBBzz",
"1KbwZ2adEWqmE1evE9pnTvZ2t9GDLkt7GR",
"1492n6QHSTgdtrzpfdPxvyUVYH6A1SvDdT",
"1FckJS2wED8mmL2qZt8FE2kx2oXMe9fCYv",
"12efC3AKavrb43gskvN93jbU3C7ieBfFAk",
"137B8TmSSFe4H7gwL4QGrzNXYozAABbuH1",
"1Nsp2egRrWBu5JNZ3AUToiEkAbtSL3miST",
"1BzFZ1BdADSLRnXGzcbrU9Q2VquHg6oVQB",
"1EkMhW6Zu9YaAD2UBL9zmjJywN9pB5w5pW",
"13GwDZXZMayBeykFACQ8ZRNjTJGoMJxUy3",
"1EuxHDGpXy88YJiBGchU9WgjLoB8ZuN4Ju",
"1EjhMrU2yZKzoKLgiFjsZG94KLBoFkeyGy",
"1HsiDHftv6BD8S5pLGyomNModrxJJ2h6Bb",
"1MhNk82De9safRvuoMrkW76PCfMd43srPh",
"1BUAEgBiT3fkJNYjgZbV86wTDra412Pfb4",
"1MyMFqagQgrKhgz3Tcs5p6nsxt37HYdu3h",
"19rmdUC3EJNvxj46WwEszixrz78vrAmNqn",
"1MYYUBSggLCRJTfd1aigt4j6ZTTBhSeose",
"1FWjs57mP2Fu3qnBmWAUTTgMKXRyDkC1bF",
"1LDyL58D4mxMwPUeshYwVsH4fSmnFkHmzn",
"1MRgmx2Mi3oyyNRTYstTnNqev98SY64aYh",
"16jwyqRaxsNZ64kGHDFpZciUf3N5m35U1q",
"145Gg6J5mUANE36oDBnZqpCPQrScJF9ugi",
"14RHBeAiW9uFw1n2q7TguaoprLj2SLAfH7",
"1DCQ53F348D6ZKrfBY9jUTDhTFjtBVpVQd",
"1ZWjqzZeaRjjTFK4Za8whxXXM1rvEcvFt",
"1LYVAA6GtjXoYFJCnrALHJbXcHSt8ifC1E",
"1BZ9tpZ9dd9QPohQcoZSUs6xmNpm3dDCaH",
"1BFeeFSRrF5j9A4rV1w7vpRCQVARpHuyF9",
"1Ecwe2qACZdw5Gubha7qhBWfZGYaD7GAcV",
"1CqVNiic1YDjZXgQ4uwuuaYXifrqgQBfN8",
"19pLa6NdEQY2AEwcRWNQdYEQWf8YtzPppB",
"1J287dED9c5w1bY1gERor6MEc8JKUu5VVW",
"1Hb4WRKKVshACedKWWWktKmmH2dM7JqoVL",
"1LE6bmvDCaNWzj88pjCnvRyk1gmG5FQq9J",
"1MZ57q53V68UoG4yJVBes1RSuCZ3xw9X4k",
"1AKEcQJ2H5T4kHxgGBtydFZcDAKqGVb8ad",
"1BAKVx1V4grT6t8Vjnjdynj8XfLuUuxzs1",
"1G1HkSLiSwLW9yUZVDQVfxnADhpLPSCeQ4",
"1Afb36JnKqSJrVB5t1JVi1yG5hcFLWrvHK",
"1427erExPejYutkEtGea4Pw4QgW7hgHvGF",
"1DK5JoaAAZsZQyhFLaDH3kdxXymRx3max9",
"1LokX1oCQoHqebUqQmDDSWprtBdYvK2HNn",
"1KVxNTQLou8iVjZTMs1AL5a2t1VSrroj6H",
"1LgVwcSujD4qshqMqLPZNTW5ZxBZd7B9cW",
"1JvoECtuXZv135JXwVGMJAhpn984gL6sF1",
"13NTzeCj9Yiw7P77trwZYBUrZ4AAgPnr6K",
"1895sEsqqZeyUWckKgUKJanKe2zTFnsJ9R",
"12yvqMTnEGF7WvwCPk5UX5tCX4DZVS64YN",
"18QPXRMMEotShGYyBG5Hg6uaZ1oqWK4xx9",
"1714yho84jDaBTiRqwfMRfBbqx1U7YRGYA",
"15kdpXpvzpdnGvVtbPaETGjxQycXeqnZVZ",
"1DA9a4kwWromiDWZxniTdiBvALbRG6Fbe7",
"1G3LfEQZML7te71REvbPe9oez3qmBG68zh",
"1GKi1KBG14H8hQykqwY6W6xh837Z5VYD3T",
"15amAFQnTyYnAM2AYZSiTthxM3MUPDnLWL",
"1LPBWNC3dxDBVpCbqp9XiotWBnjKCHo9Mx",
"14CAn4c3SUJocomaDziZrTDnKx4yDHKHn1",
"1PSXxGZkuMBEduBXazkGgqPuv2ZddvyRPW",
"19kL2NdBohzLGU16BTezPzy73TjRRGTskG",
"1KS5vui38DnbrqBjV3b5Bsu5E4aXMuf1rs",
"1Lhp39c6NoQev3RBbaYmEJjYwMGEMncqRG",
"1LLUsbc5gpxgWifL63XUtZ8CEK42sm8DYW",
"1QAmJ5D2fRxXfPwHb6Yq2QsJwRWPokcDSF",
"1Q495xyqCew7Jixk86T3eAGCEB73Ba5vUB",
"1CvEGQryqQPVvUNUCY8r7bJG6qdgtP4NsP",
"1222xBg7zoFRDn4fA514aAUJRGnNaEsmJg",
"1NtajUa9JXnKpKfku9LXiTEPaQeubhd3RP",
"1A1PR3abZxiLToqpfcCp8QaST9QSXgK4uD",
"1E88jwX8TNDEAx19uqtKCCBENH4Xp2nuKS",
"1LWBdApCZj5vVUK1ukQU2RkkfnTuA4dAQE",
"18yVe1qXBkqztGAJG4KTLkM3kkrm6SEwSh",
"1E4pCF8RSdtbT3uAHCnsh36w6xkzTEAeuG",
"1GrLPa5vXhp6EHAH1C41D9h31jaCytHNkY",
"1C3FXNk7A6ZdB6gBEARYvt5SZSwWBwSwHd",
"19i8pX3KkpFY8dY7EHDC7RjD6xeXdZKakV",
"1HrPsKhLTuQY4yyhAHUcMLdDc6xGyMgLNc",
"1WTL1b3U2bDa3hhF7zKCy65A7ypuMnq9N",
"1BzgFpGHqk4xdR4nxo5hgngBpMPyzmcpse",
"1H7uMo8DgxvdXV4aKUAFqFq4zBQvtwF7TR",
"1PYQo4RbGvDS1BLPwrh4ZtK9WzdCjqiPhr",
"1JgV4w96nLqx4AqUduKv6JHVXPCLPUiJxH",
"1HUumG97yDzdzRMW4tmSzsX4tdWMiaTbxi",
"1MBBkQ19NMv24zUoSZYtVeHuu32ESWrTYH",
"1LqxHTGdxu2CjXceqitUfv8MZwrUsb6NFM",
"18qKYppmdexcm5sPeUxqYByfsBWFuzP6MC",
"1AKgiB8FwXdN7HSMaPWTQDgxDZdNqGUGAv",
"13r7iCrjHKh6yE1vQHJAqGdCRqMfLDPt94",
"13Zues2hPSMrMmkXeiPdbvs2GJDiDaAmBx",
"17VKfbmxmNR4REMKzLgCWXa1UeuCp3LUBV",
"16eDr3BMBFJMfA88YoCq2TwUdk7wbXZN7s",
"15niE9JrjoJdjfVd61RoqtLRYUuPxt6MBM",
"1B2CKQethGWoo82CMKzKAoiA7MGPBWs3R7",
"18nYHs661zeoF2P2UnvKqJuJeALYcD3Q8R",
"1M8cf4jZFQr3fmcZSeixoZuKGPJYL6MJVp",
"14skmnymPFbi2S5ioS3be5mSvJivFBV1Hg",
"14mSk9Kk14ZmmGKfFypHAYfrVFA1Sq4xNt",
"1PWuoiKri9g3Q3L4rK9p2jKpDCh6eBeJEG",
"1K3Rbj6uMbA78FomYtqjv5nobY88bK5QSu",
"19HwN52zptMKi2UkC68R7Ns6eYHt1kher3",
"13uo9qKCJtmAooo42ZPfWFwnUAoR7iLZ3o",
"1KLEnSiV7E2MHi9pJjX2Ciu3LVFvxvagBJ",
"1NvRvipRwyANcfDdreVW7xqZ9onDKAfTa7",
"19u14wqH54iofYLrg4Tw3Mt72u3Mhbea9r",
"1BCCTN9xZD7Sk18StYEPgJ6ywJ8zkYuF47",
"17ZdmQfybeHFTgS5m85Fdt2Yky7r33H3qR",
"1LWwwUZ1f4oRe6VPe1eh8ThiC5EXCX1PUN",
"1wNMcFabVA4aW6o65kTViXziYQjChZHWz",
"1Ey679xqD1QmE5QoyDVPgYk4YUR5ob47CV",
"189csh63XmhyoeYBqfEN4M7PFUt5DCZEb9",
"1NTaGaGhh37srusEANsHkodaZfFTzMvUhD",
"1M1JoRAo2d8ona6iL72Tx8KkWxmDw7JXwU",
"1A87EnwPnkeSrwF5bWDzx1Y3P86UjxPzvu",
"15BqxpVv7CgcNMYtSrvj8P3LfnoA6Xy5Kt",
"1KhHCwh81ZrYU1R6HKd68kkoxej3WasGN9",
"1DZ2yStEwLoRX4XBdmRk2cfBEJNsdV5kP1",
"1FuUxfdTVdFRegS6ZW8NZhqK6DLWocFBh5",
"1FF1D3v5truPz8hV1jGG5M6nrYecdysHZz",
"1Ma3QCXDaDD87mnDE41DJc1onuZph6kW51",
"13tGBEAEzZcynN9BikgEtX3KJ7T2Gxvou8",
"1JmiEsEG7DbXREaPd8tAHLSZu6JVtb576S",
"17xTFgCpSyCErbDfKoSKfyoK7bx1xWh3Um",
"15gm8zfqT2gWAM9wjYtgQTMAAsYYkuHAUo",
"1K5uQLLjP1TkXGBm7GZSnTSV7Ss1yreH6p",
"1EoZpPhqMCYojq8zfse81FCKrK2EeQVMSR",
"1J1TP181Nswwmvayw3hCdfnThbu6oqSam8",
"1M5NFJJUVDp9XBgT5rnWg6h78LhGfUA5Xd",
"135HsKgVjn8ycUd9PRhZPLACLV7j34wYk7",
"1A4pWKsn473rEjA1gLvdxWudcHYFtKH3pR",
"1PK2xcr8EQprdsLm1xbUwwJbogqFdCkoSp",
"19JPZvW9HF9ZxWDY2KDKsGhJaa9vthaVCL",
"1GJQm7RQskMm7aULsbdXyFh89DzXPhHKUN",
"1BWkDiLVonvt676mew1ZU2YR91sLvyYp3T",
"1EQWiXKSSAf5Da4sd3cgqWY2KeJGEP1RJm",
"1JNBg9JmmM9AUsvxBEq1bS7DDGdgmhzuWB",
"18i7qGL25WR4qin3KsTUsnLUaSPE1dAMnB",
"182rCRDtQDPT9yxyV5FZ1LzKgBLPdjsUoc",
"1Py2zrhnpKrYTX3uDzqcLfPu25rEPRYs7b",
"1JNbBVhT56nTtfiQdyCgE7VK7aDrXXpPa8",
"1xkD9tutz52ZwZmufuocJgxskeyQgbtqg",
"12TWALkiWrhnRUFKAvDYTx6qBosnSiqm61",
"1FsyFf4UE16prDJpuFjPydBwsfZ23AvnTi",
"16iqZJLdtZryNsABs7bQkFy9ovuKGx5uc2",
"12RvyXuRLbBooW7PMwJZKzwBVDyToWokcL",
"1LQv5CdELqBk6ooFsRfp39sinwHXES2smR",
"1EcSrq5sr7tEjvxFQFkShhgzmgsG29TgVv",
"1B5jsmqGHvMqssxhhjjwvwpKKohggZuN8R",
"1KWScojrGmaxw85EyWL8wBx9YbiiWYb8cx",
"1C5aFEPXfPUZak1Ccw1487bqxKwz9V22HK",
"1DwUETyU8ZHoEnigwSyoZ6yXcfUAQg88TN",
"1HCG2kpDeF26siSeoYto8xkAXnpTS9dggx",
"1LVWvDFjHJ5fd8ZZsy4LYebqChcJs789Cz",
"191PesAFntp5EGQ8UDsHSt5emcvXCWxq2P",
"1KL37UNovQ6XaRU7xfiryGDLXVVR3hu8Y3",
"1P3bfVEHoHSHumaGspxckgkYhhvoE1Hai",
"1Gq1hCugSUqeG7Zx1X5m15eusXyFn2CnRi",
"1GjW5Ry4RmgMHY3Rmkk6qfzeKSeLb4tmNC",
"1Q8y2NbdJAQmVmN6J7n9AMHmTAdDkRtGFX",
"1GL6aUXNirX2PpCKReyjX74kgcFfRvG8cB",
"1L2cGFbDn7Ygx3gCXaenYx5q3b1NmQBR3G",
"1CuHStw7V68pjbGzMwXHz1tiy8j1KB8muX",
"1EHUipgWLxN2cNQhooHVvxCkEBKoVUd6Yc",
"1K6ftwgn8CxnAV7FC1pFvZoHAQUBZ86LhZ",
"1AJbUnS4WrPQQ14d8iUuevh6DLyGn9EoDS",
"12fJkCjaJtpWyFv4ktUck69dRkqgYjHZUc",
"19DCo2bgYvx384desPVtKX7JcPAYAMMx9U",
"1BsrytyCxkz7dWHkEZuxxfhVqgqKF1somy",
"15Sf33MSaBTvwP97WYr1fuCZCLgtFLupSk",
"1JeKSbXjCzLE4k6ssnyajLwsyaENZKWVS",
"18PGHtac7QDypSuQdGp1LZewkPNXxtVaxK",
"1M8dXC2sRWYNmZ4WwcFJ2hNnEngEKvSMCa",
"19947tbsNkJCAnnb6MW8sRX4k9wxvQHbWe",
"17iPYuALNuhf11AuJnQkaDUSYAwD3moR2f",
"14v1pT7ZNJU6r1bgfSRt78ExmVdfcjmYdM",
"13QyjMGzZHyyQVstJdEqHS8ughft2oryUK",
"16x38o6VKrGum3G5fP3Pe3LogWz9gCiSKp",
"1KeGJrg49KwRUZo3vLaBaNrARQV4gc1DQo",
"13idHhV4KcKTmGXGJnyWPSWmRCNgT87UtN",
"132qUaSyBfVKzAeD9YCwJHRpbWb1gKupbD",
"1MGZr6YWiB1XpkTqMhqTgcjj3YVqKNgQQP",
"13oWAAkskjxcYxiw23D882AU776zAPAwXg",
"1GgNQ4nvLWeHAe1ksWFNgj9ofg8c8rujY2",
"1ChiZNEWnfaysMrBWpHrn7NQZSxbqh5Yr6",
"15NarD3Vx5sF2WZ5zh2B4bBj7HQAhYHhSi",
"1MMGVefMmZZgHMt32XX8DM4AyjqUgtZbhY",
"1C3tS2gMtgJffesaWkJ7uTaVhkPiwr6uaX",
"1LGDLADhhUnCZ1PNkCdhWWtu5Y5AgLPoLX",
"1LD6ZgH5c6xQ3nAchKs9UHyCka35B2QV55",
"17wMcxWEbCwvzGhGEMwkPijUztUZcCgWLG",
"16hkJpBL1BEZTStXSxCtgxPrz34ayKaUT8",
"15XeQi1X5mbFZw9EmDiVEzsMTiCjFZDA4h",
"1J8iRUMHsDECSVPaZ4NfW4nSpeGb5yNgjN",
"14eUNTM8EX6Agx4XVg8hCXGney8z3qfpZa",
"1FJcJjczKXUsNGbgqS9FjDLpHXdT4fYm8g",
"1FCkn9yfDDXTGVCKn7RrLbZ9T9DDbQfNNd",
"1EWLruRMTBNoy8zxtRRr9itpwStTzhNC8F",
"1HbgFt5y2obSUboym62M1PmgAespZqvnKS",
"12emYDiV1Bn9nrRLdexXppHqMKPufX5nW8",
"191UpNpEpntpfW5Mkwg3o5QSf96K6fh53o",
"1BbGDByzjqQuu4H7EewTgwvPGschE8VXYe",
"1CxA2HhxjSMkzNp59FNwxfzYijHbuY7kZo",
"1FQtR4z5uhPk9hqNtKDvz14bgLaXbZdUwC",
"13yVJurLqvvFhuaD9LdSgb5ZKQ3vpBff3N",
"1HeJwSPXuvCKzCGm2DuhN1SYnVgz7KSsYM",
"1G3MKpaTZFBxP3NUK6MShtU9z3oxe1VGjz",
"13CkiciKNk9CX5sSFApgAo4bS2rw1FmRR7",
"1FSWRnjpcnXuSeyjxajoBGUZZVYdzp18TR",
"19voLUB2thPw4CtzuN9MkTn3wgFvS4hdhi",
"1CaFSm44nGaPQQyz5KUiGk5gC1wvDwJDAy",
"16V5gC7ccbAnNQbJ5caaiZkTZVuRE85z3P",
"17vcy5y96ZcTMHCdLFoadgyVYQjr5C4biR",
"129ybrMptYex2bo5vRAtUvc6tbLh4QKUHr",
"1GPzmULjWpVW8UZtsjsrScs3cZzvJdx3tw",
"19u6jvnxY4mhze8q3DoZ21Nf83HByYK8en",
"14H9Sr5k2ShNogXfMDd87i5WBbgUE7RXYX",
"1GdizPzDZVAfsnotiwgsBmuE5QUDBzND3k",
"1DraqiKgPkcaphLh7YUon4Z9iXwK3gqBC7",
"1NuUBBzw1JBNtPUbLHVtKVUUpSFFUZBSgS",
"1LL3pv5WyCWQkpNGjFdyTeJMik3RWaaUng",
"17G6nLsuZzi9XwUd7Vn12MpAe16NJWnQCj",
"1DWZKCdpt5u3tY4F82674g1AbYvdFoRnh",
"176JWJQcMP8kF5rWhBQ243SodAPHrsr77y",
"13A8ajhaZsKKUHiH1tZutfgvpPGCgLNz7M",
"13RXwur35DAJUrYuPkUpSbfKrWzAzWJgut",
"19Vy2vMk1RFfrFZG1JNTo1LWTNuCXVSxSs",
"166NkZUbsWWQtskcizeF5Wkod5Hkm41jSb",
"1358EHARRxcWgrSWkGoKf2jEy3pNnHbTBu",
"14gtHRpA4XWzRihphosV575yK45BRhMNg5",
"19KwbvCy6NKcXpLsu1BFubw8YFNxbNyF8q",
"1NNeEFjQ4aPPaPnPSHKQCzFzAFn3EXrR88",
"13cxpCVKaZWTxBSEk5m21JHhMcqzbPuKfi",
"1GFVU96prq2fwKkXPQQwCgpLpw1LXMkBMj",
"1BNGjyBdMXwv2Gqs5jHBqhobLgwURucEno",
"1JwfVSPnvmt3HVBWwgdWGoTGZxn7z8KoWL",
"1KsT2hFzjYd8AqGUqRrDc8UrmnpAF7AK3Z",
"1phwUBXDd4wU46bZahWzLeAacecYVuYgb",
"13XyqU4N36G8sA8i5arhAocpanJWgQiUsn",
"1NMZtXsBKEsMrXNFkwCGcsY3G2bo9WNWob",
"154vodj2ysPMyUd6M83HPheN9wGpQXBkNF",
"18K7NyXXBTGSYafTj7jjmEGP2iadAmxnFC",
"17KCDMkrXkfQ2Rdjv9WP3p9QkxusuWSKo",
"1JbTwRPwenXoPfj2xDWmMVVXvsbistTVsi",
"18NR7sRLg5utNLAbHYsQmKxcb15UADcc5f",
"1EWFC9XDvYiG5QeXiUgLkYgmrwjH3Fznmk",
"19DZwiNxiqirfpGjp99VmESQX4Ndh3P4RY",
"1KWbhYPRJWgk5MrJFo4cJLP5XuqV2ZDKfX",
"1GTjBHryXiPzTKwFhdFmpFQt1vHmVdF6s9",
"1Ew2XV3VvMvjAf6iiFfFWcJc5nESQvy6Sc",
"16T2SS9Fr5emz1sMKJ4psBuhfSMah75rov",
"179sXoeXogEzGLQe48EVKKZmLXAgWLFtpm",
"185hmS2zk5Rhz2iuqkihVC4zKDCttbS8Ws",
"1Mhw4xebFfnWpHj8EvZBr6QNzBtbXXnSJj",
"15cn1hxkkVDMAvJqafQCJSsJnBBSf3LEBA",
"1NQEWRDK1iBM641Wiz4NE7Ac5CQT2gfHGs",
"1EgECmXEuexS3ARfkNfLeMybRZHyuXNv7K",
"1MX5wty4FdwedfWfnpQKsJhS12EDSkY1po",
"14dPoN3WYKrcPdZvSLqjvYdce2NBzm7PwH",
"12c9RdswraMxU1D9JdMuES3YbdJz3BG8pf",
"1F5Foam3ggWzx1FQx45CBprP5CdAaWqavQ",
"1NYqwsw7KemDmUDUtX6yjVCPHKraFYPhwD",
"1AvECvfVfkmKwSsms5nEmatZJQfpXXyEhH",
"15nf3ZTcTmzPs9AZuygPc7ehEoiSx2kBys",
"1CG99KQzw7fphWkCrCQybLimVqbLQ4veSz",
"17thdv4tDE81vnV4JjaWurG3Jkz8hNWgt1",
"1GJ6NA4dZe2T4DeLWmBtuWyH1HcUML3W3s",
"1LcEh3a1i3HjBgVTTWwsmosV3tiBMWR38p",
"1AbgyyS7GQZaU7rdSr1qaQtMYjD4ReC9EA",
"1PHYSJsrsttJgA7tacQ11BPb2RM1QYHXMP",
"1ER5yg5mxMvsPriqi5UtcWb7YfVCzLkcpP",
"1NDcffZkVPkte42uXawBY3QDgZd2kNoVuk",
"1HhHohkVfD9oNWFg2CXnEv15pXNG8BPV5g",
"1QHrjoKXjWKiHKoDtiDAzpmQn1Zia3HmbQ",
"1HCqv4AwRMARehqYMXg1nEF924jA3wBHEz",
"1NCabDRzf7hfUbMt74CWgu7cUciDVu983P",
"1AjJYY3qEnKhvV4gACZb1JJvWt2vottBgs",
"1GeGfCPJYSiwLcRPMx5JbjUwX9dgLEKpJY",
"15UpRUxKAEbhjTHcLG3wzEYhw6DvQfreoR",
"1BmJ9fxf1xyhTcvbUqtYKpVPSt1DavgKov",
"12UjKEaL1DJnpmuCVivfnS46fi7UwrMguH",
"1FC3x4FCp5qUrCeWTmyC9jGGSN9sQ8CNs7",
"1DEhEhiJnryGDs3WEUtrMZQUBXLPX7z4xg",
"15zsmuqSNDtJjEFT1Xs87o5TWPQUmhtGaJ",
"18wVcZ48tosTffpFCc9XPbn2VXZfDQ7EzD",
"15JxuRiSekX7LvsHaWUZMwYwxKvYf9q6Pt",
"15C7gfkixuCSu9rxTcbwF4EshEZzs6qaiv",
"1PJGAGtBpTrVMypJiemPZoLYyE6esHR2ho",
"1FuAJFYNBZ1DMNtQGgkEkQiF5jbrGpoc2F",
"1AeqWHo3cMjRWAx8ix24gJKESr8D1C4sM",
"121c6GdParuDjNr1yQXJhtvRM19nYJQR5E",
"1B1TadLqHmdmTFXPSbqGiLrTCwEbPwJ4nA",
"12KcGAoq22enxJBNNnEoUBTNfZNyat397b",
"1Fgf3u86wya3oUPfkk2Jj5LqauTVko2ARd",
"13dkCkkfNQoQ3ofLicVT7Ro4AAkeiLd3PM",
"13XWXVCWHgte6mc3tiPbuq9fuH2GxwyUTL",
"1DvtxSHkhvxqjqoQ6gthPf7gfP4M3Ctixk",
"1AFeP7hCS37hWZekxoSXaLLRuZSB5smSde",
"183U8TGibVU7a2YcAxeZoxxx7BCu1yDVCE",
"12RxmmGwBR9MKGz3jmta9FPQyPmRmDr83p",
"17awXEywbZNJX697EjX2QLU1Ajd53WubCL",
"1KhPLwfNibarjiEKQfE2JuK2RUnWESBLcR",
"1Prk5iJuthpbJc5E5M6bhAaRykChRczNgo",
"1MC6HdKjBZNLhG6BpsfGnNaYoQsXsjbXkG",
"174TYNUWpd2MoBCTRnAibRBPbow3ifq7p4",
"1FEtF4mpALeG39TWt4s15A8VReQVuT9sDE",
"1Pc4tXUsv36sXia94JXPw1JVV1CLMeff1y",
"1E8TdVNFebGdvgHmETzgFD5amYzFphvz3k",
"1EwR4qaJWgGbQKdCsLW1t3Vu6gbEQqFLZv",
"1PqvJScjBbjmPrjzXFXS1wnZhVbo6z4cgX",
"13sQSYgd2qw34LDvNiC1U8AR4aYnRhL1vP",
"1EWq9BAoomPoiUadoD7jzmVPwmXoTxsDe7",
"19y3PP57ZQnWa6Wx3d1FBjgfBQH5qh8Qat",
"1GnqAvGgfPvUP2KcTg5bNNuj17wRKDNktY",
"1DWPy44xexRsBtMBCvSEqzGfGdufjNHffh",
"1LnJDJVsBMKEMJ2W9T6Homanwnkkj8XGQL",
"1HknuHk98jLtMXiKxsMYrSF1CfkyrxjwZv",
"1PykdumNnGcMJ5viYxpUeP3Gg7F8hheauh",
"1MD7VFnRop99Kudc61rWTZ4rYAzhcNYRdz",
"1Lc63PgMtMfkX7vTKAAL65KLfs8tmoreWh",
"1A9KALvib1wwXcfkDcJhqpMezox7gHvPJh",
"196w2gQANaYKpBeapLbYX63aSXQjLfMxhW",
"16BmfcAcQnbS4nUVCUe375zeQ59xGAxtg8",
"15jjK7aJprABcUrPLBPa9td744MZrnrQXZ",
"1Azoi8BWE6wX6s3WsE9VfNStRXB6QCcgdB",
"1HdhhJWBtgqSp9bDCq2QYc3Kp1VCAxC4yo",
"12UbTJd3hqCo9T31viuSqxLuwb252PV5Pr",
"1NWe6aoy8xAh1uTdQMtRd77AmjXD1ULSzA",
"1E7gtqvM4sjjYG537Dz1TicdsdbMCDCJVf",
"1N5tg24t4w9CdeaypDT4EJuiKijcZz55BS",
"1LTKxtoQKH97FFutkHbAUemUqo9TdTQdRy",
"14CQR8PfoLPTuw3czuAppgDDynUQuGQ3SN",
"1AgjnDw8WzNgocQeJ5y5SZ1RsJc5wEyRKP",
"1DZ8LeXJ17XGQK3LqLTe37kEj9STj7tm2z",
"1Ny4zDNL7WEnKZsaN7k6V5Dd2mjB3DJ6Zg",
"1FjiTYi7dMj8ubp5PzgGULPXyaGWc1Hwte",
"15xLTPsBP4iJV7nSSNPEyAuK9Yti5UiPHU",
"1KS4yG1t9BJKNZewRvGuv2m1GKi4QLSUC1",
"1L6A1TGky5VZhyJmptgjjEBqnzZskp4Yao",
"1J9futo36dQAFBZiu45fPxU4ixSUkzEd2F",
"153656qTS7UgimR4FugR3td558jVHzKf96",
"17smFcjPLCPMjKKiurX6vDokquxw1rfoRM",
"141UT7GeHWrfAqrQv7aucZ7cpS5Th3fi8i",
"1L84CjNj1sN13Jdf1BCLUQNZ1PHUMUYLXD",
"1FvgoCD8Cfb1gQjxHpTspJGWk3CXUnShV7",
"1toSQH8WiGoFybGZ7UFxaEL4FNLU5Lffg",
"1P3ksBM48YUCL1RtCD4KKx4nSSQCihpUSK",
"1CkCw8H3LCKFpav3EztrDXdE4ukf3S8JMB",
"1JQJZqCpM4N96WtGTaRfLRfKsYXKtYa1kb",
"1LG8EmvCcm3pTCi6AJQTNdipmtKJHfJabK",
"1EPWAiYJaeBvwUEm6Ljt56yJoG9oXjHyiU",
"1KjyZhKEGL2C4ZMekkgPzRBEzncg1TjKec",
"1CSDfGLz5TimddTkCDe8rdwvnW74zFtK31",
"1C3xfEF264TgmBxYbrb3zyt1vWM9Y7RDxd",
"12Vs3adRpKZUkxsEdeM1XQ4McCx3VMRu2J",
"1AT34RBS4BNnT7S6ZE9sDrmje1vS7AmYAb",
"1PWjzKunbw93eaRsYgrg6RUvR1tDdoDBhR",
"17qbVrCuZFwScMKrPFWVULfCUhw7pRSmvA",
"16ndvzJnTK4Pye5HnYira3C1iuHzr1seK4",
"18Gr72kj93LjEqSCqLq5ujRuuUg1vs5zdh",
"1LHnp4A6NGDSkkL9TgHM1cNkzxSAeQzV7c",
"1GjzkDqGH3oiop7ms8ZESWqQhG3PugA1QU",
"1KJWEM1ckMRzWC5Qg2QjzX8faH5t6UXgoR",
"18LfrwvFMsoyxPoRiFczabunK8gVaRRXyb",
"1Jh515nPzxS5JUvpEQ5ybCg8VqdWK2kc1W",
"1PP8Az9epR8aKQGJC5ptgiFdBD5SzcxE3J",
"13sbek8QGGERHhkzQjnNsikVZEYoPM4VNk",
"13rd1Q4cfKpPe8rqPPKnvsBxwhAPSV2Gm5",
"1GY3qLWngzivtUpWRNavdAoVDrmM1WRNyp",
"165NjLXvV6WqRj9WtvSDcnSAbLFCbkKM9R",
"1KgHJ855UKyCRgY6ejNpdwvfFozVYrNULQ",
"12BgQ1znAskdnzLmcFT5XNV7MvCK4zG7dE",
"1KTFxRMaitedZvPEDB4rbBvgZtH9weVqSN",
"1FF9AA52ZQLvD4pvFyabpSqGWgmGLt26ep",
"1Nk9pHcz6sL89r1xMQUWP9gaLmrcRQwfG6",
"1NxsAW2GMEDMxxSaVWTMLBnoP8vA5kwnA5",
"12XMmN3kMAzwdq65DvozZSdsveTgeioVJq",
"1L8ge1tP7DHD9WZsEQfsnC6Yvm4BiPya1W",
"1C9d5kzpkDf2RaVkYTNKmmCfhSZxmRTsGc",
"16oWBuSu3AbeyonqbPLvChiW5K1RHx54mo",
"17VKttyvgDTKaetcQXzy5YzyoqFiwvmXtA",
"1LWUk87jFqCp9hJFwD1JcnpM25tLVzGZU1",
"1B294ix97i667u8Lahkq5WzECxAKZab1rT",
"1J4urQU7XsuWwaJqa9HBGjkS8CNC27H6zD",
"1DjTCk3w67pxai2cRPsnuefzxsWbx23gZy",
"1QCUHp4cuWTLEFPHFTo4BQoQrEbwxf5i1S",
"1RHbUcHEFCJKdELpXZkDpBNoAogTURczd",
"1akZyjB8KYdDrL7FoCVSxpqNAg7oVbwrE",
"1D2fmLUDPnC6qpQigwHh3vxrEUKrGArywJ",
"1MhkNTPX1KPRc32zSHG4R9RY3NRptcjP65",
"1MCa6w2vV3oCR5SXsgGfKXeDq7ovEy2C6U",
"1GczSuX4wW3muPH2B4uJ7LbxiJ55BBACw7",
"1EhQ8PyLYD1KiKYJd8v9XbxqKAJQVRo2kK",
"132Yn78CLWF7AYN1U9qSurWLcufAEcVerN",
"1K6fePYz2Dt34SxrUHedKqfBf5D1dEwan9",
"1JMx6cfPBLxSH8x62TrcgrNcqxY1aUDm5M",
"15KyfEfDwU8u5e7rNqz268ntBwj2GDc2KN",
"14nTPqugibXRWxiK141CHwf9Xu6CbHvABt",
"196ntqUPoCoTWgUbBrkbcmFdSQL9PiuDrC",
"15coDKTx1DzV1YcfXZ5rnbhXQxpKSeUyV9",
"1Ngj9rtpcDz2cdCX2ZYifXSQNMtgoa8V4y",
"1MYZjwcd5bXvrJAamg3TD3Lze8xqLWKL3t",
"1CdGeb9UZVZNceSiXSQsbTRY4XRFmD2MuP",
"14TpHB7K8T7X2ruxxcHGUy1EZTn5W9v2o3",
"157ngyQYJxAHf8739dFjwkYEF4vabRvRpA",
"19tLx8oRCfdpwD2Ptgo4UJr1hVfeK8qc6V",
"1NK67JypNgHL3UG8ezoy144v56qGMbFQXB",
"13mbwepgpBS2ubjT5y77zfryi75da3rdVV",
"167TEbDPV4Z3tGq4WRh9S3ewEXRJcqZ7Zr",
"1wp7PEwze1ubnbwqXnMsgMk3K7iUQz5WZ",
"15E3DQ4BPoEvrKtSwnGYk2hT5kjhX3h819",
"1NAxnenk5Dbwp2V94mrURwU18ktcoPzUw9",
"16au35nxUxm8hxM14HSPxPEM8UJkaaKeKF",
"1CGkwLoCSfXg2tpxDHF5PWQr674znBJSb3",
"14HU63VuGERSRyPMTsnuyLLyYw7o1e7e4d",
"1Hs9shYRcr6YnEmPhU9uJdfisiFTBF2ZBM",
"1838DENCTV1BaPC1j1gAs98sChpSL1b4LT",
"1MsEuJyQC7C9ECS3X9bi93J1p26QJP8viK",
"1H9J969MUZDM961Q2CFPqGqsjhmbDhpqYK",
"1CnTUPbhZVSpKkjo312YQStE8L8pyEbmBj",
"1E5rt7UgS8RMkEQQQPCU4QamooveoGjfx5",
"12VSndKmUnUFbhA3MDq9K2FkLPWbwF5Jve",
"1Nm5SLaDB6kDFaFxHgWbrGpTQR6PqFNKrf",
"1PJ7VPDMW8TTtCCSayyryk2gBQvh5rKS49",
"15TWtmixgEdiZjR2dScomJZ34htSKH7spk",
"1HBNcbs9bv8povGNi2CPDsiHWY6jynp2ih",
"1PHYJoNjdyBNsBsxnYJxGwwajynPrpfa6H",
"1AfAtCtiuy2HxC3JH5u3QtLhDyDqBvs6ii",
"1HZFf2Fu47Vhp9QvpyZLCJXwT5wM7vqo2R",
"1BttkML4wiFQmBEzCZriyPq5Q347XYxSuW",
"1AW76aY5NcMQsE9G1EmGsyPSmCVGrT2PE",
"14HrzxRH8JdSiBtv9g2QzBh1fa5NbT9x32",
"1PMMxKng57EFvH9xau9gMCvkpRTgUNNoDi",
"14P46oyu9pJcS6H9qi6YdJwcgBApKVpB75",
"19RXMGufSvxeWwkdRZDmXnr3qQFEB88ZYY",
"1B5HqgUJ9QR8VnxXUdi516mZBgFYLDaLgx",
"1onHho2R8iMQT2ZEzhChXRud2aJXe834P",
"1BmagUaKa83WT48tYXoeymNSoveR4rXEZS",
"1B1sCTd7BvErWGbBqaJqP9zfnynxg5Bjq3",
"1PqQfRLF4wRZ8MVEzqG2HUkVNSorNzUXb3",
"1FEfnPVUTiL6zrhigkUVkSb3WyarARb8iN",
"13A1zJhE3WPr22Lg8pEAWxg3Cp6DutYZDs",
"1KT6tmq7MPNz6gmTB8FG6P1TbhGjis7FdX",
"1CfsCmDcfXXNfEw8fg9aLLNMRiRTtrW7Xm",
"14QJfZMGDmJWHBkqUamfXNE9fKwvfJjFBh",
"1PA7NL7ycMH7yRAMt8k78tvgyp9gkwbL7x",
"1FzHUtApYBcRkUgwEQBrMJhdaE9ZXX5vr2",
"1J3nufeWWK7cvBikWEHU5TkxmT494Ky8wr",
"15NUdMcmCqAq9VsVyYaqtLoF67iHGBo6Pg",
"15MTj4YLVMHerDpWhXj6WfidH8GUytgchT",
"14WQNuPL3BKEJJ76aevKR2KbHzM2bAoQy3",
"1NdVKngKqmKnwR5CTsUV3tNy7xzW9nGKDp",
"139NkAtScaYYYyBArFQMh7P6Y9x9LxTNGh",
"1P8kPyEMYDZqPXiC46BVynM7Xr77gWBXPG",
"1Js8rjgLSZTW8N5Ks4pz3HHHGpoXC2nMLS",
"1C5jRkpvWWJE46WfGJ8dU9Ab8tkunqcGFd",
"1P5CcpizUk81gs6wcMuxCnUYqoH16Q3CfF",
"1H8XR9Vd7gEebJ1D7KvMvJXcRXsDrh6Mft",
"12xBXjWV3YvM71XDTc3MTbTSEVVBFMcAnT",
"15R9q49YfNhCV5WLSSrhHrbgu2cWfcZyXj",
"18GBkUy3H6tmg1g95B6mtwhZP632CmWVcE",
"1Nqg88PCTjxxDhj8M7UJzVZQ94VHYGALE7",
"1MtaqZAAbjSazttJKvp4nRPHsVMG2YFdp5",
"1AqUHc2JoAdkMtsiTZpvoYQb2JggAQo2uP",
"1J7Zoc6mpGueMEJbNuE5ytbRpB8Qw33woW",
"1B9CByax5JrYpjPKmh3M9SxAQGHViFF3kp",
"12HcJquLQ5eoozjYPXLS8K4ZBEqaUkV5hD",
"12NsdYWwfsSFZqVz1PfmFPAXgMtAzFiFE4",
"1J4dGkCumbQENcUXuSjaPQUQhYe1omwsVR",
"193n4wbCuhgwqLrm7mwct7iQMJeRE5uZvq",
"1LothxDjMaJ2NYEULoiTq8NhFhdheUNiuE",
"1LDZXpuRYQVWPcmvmHNEq13QTbkPB2smwQ",
"1Hqfm9qPxTvkVuTn55pdjrph75GVKtRJJg",
"1NETugagPFWKgZwuMjPS3EmcrEZ6daYQiL",
"16NtLro3bcbb9GQo5tiUVzSesQ2xxFu24V",
"1BWGpyrXgJyJScppzxZC6okBc2ByhezpVp",
"17UA3dXM8hmX5cFKfAKFcrDfpo5ztTqPCM",
"17YNXrNRRW14J5LJHDnuPNcAFH2mLPyDkR",
"1JLt3AWAfV83qQLiT4XdHZab8X89LFsSzv",
"1JGZfYnaSHiijFDqqceJk7eQokyh2sN96D",
"1ALkTiw3ZUYiKAJUTC2fWMTwYRS7NHzE3i",
"18M5QuNPBM7TtUpG9SVeCB3zJhS4KShq3C",
"1ATEztbFH19fWjo71VcH8A6PEA3rn3k64S",
"1EMjS8bteJMWaVaH1cYCMSUcCAru2jBPCz",
"16N2KUCy1j1EgnihgKiW1h5SHcjoVAUKvK",
"1D2qzwKMjHTEaXu3zNGs9vkeMFgoYtv2MT",
"1KLrZyovBXzCD9GtNMS1PLugM8iUJB4x6K",
"18vKDVtpkWWfvdFx9AYBJX4ax5jzTW6xgW",
"1FrhAebzDNwu2MjGtuf8GpvT3Vb2ozrWfc",
"1Dw9vHVVwAb4DD4LQtzv2pgu2t28F2bAGJ",
"14t3tudw9GTReWjACDbkH7SevJqeEwYJDR",
"189iBNkVNFgN1QC6fFyKco2TSesJ4pDzvg",
"1EX9mKijLGCqWFmqrD9UQ7hYXFN88LQoTJ",
"1FMWSnj1K3hVxBW6Azm5M3Qthrj75wPUUs",
"1j2HQA6GSzHkaZD1KLiaTHy9L3kFfc9ko",
"1LAmmTTKRQQ8WCBpdtBEzEX67p2RGhUswb",
"1XRzk5DSHnNDi1uQWQdggVZfNCktxLmbs",
"1GHKyW9eUBUXwWRx3j7tZF2MqMUwAeKNWF",
"124ZH6mGGhzeesGUSPz14EU3LkMhG5exgi",
"18A9UKDEpxEaGVxwSrPfjdcUvkBCLb89XU",
"1FgHYm5fW3USUZzehcy8Yteaght523U6RR",
"1G12kyrax5CKqBc9W8BkpvDYZbVz2C1KNH",
"1QBnWhQ5F6S9GQL4MB2q1tJZ7iyAZLQN9x",
"14LMGoatbq7d9svxhuW4f5JxExrmpoE5A1",
"18hYT2QTkuyJrVGXrhwLeHouK3aHL7pCQH",
"1ABAYxs3UbGiGPQptLVJKWL4E2HJ6nHT5t",
"1GtS4zdtg5rez1youHrYb2DqDtFHZTD9zC",
"1EkMhaAP5R8b5g8z6pWk3j46giGYU9UnTn",
"1MUMDWT7bSXjsKvf9oRMuTQG9B6a2L7whG",
"14HyoZxoo2ScCKN5heBafRPe8vtAMGAyXb",
"18MruVJfkpBwZpv3DzYSQwoUxxkGbFgX4H",
"154SPhndgN2UCpPiGFdz2SRnALWNSMTMwb",
"1ApiXrfNtQbSgZGnCqBuaJbTJuVNJasjsG",
"1CZcrudvYyp3HnYmJkYkzjQWGtmzT2Mi7x",
"1L2NtpaQsUsd2wttTk5wywDBPPNjaRHHj2",
"18EXQoq25FVJRugm8FkHWieoeGGwXAbGRE",
"1BJxSt9Wuh9iUJF3R1QxvKyH5Lbs4shEDS",
"1AMDyjuUikt9wLigqAqCAa4hWCFvNMqz5S",
"15uzhAMqdTiMN1a3yBGfJAZWG7ZHbrjD3y",
"1L83g8wAPKMmsrbT1ueFF7PdacL4zV92jA",
"1PNozqrUwi2FFAcAV7VT18reVGEHzwEj5y",
"15uaivjGsqBpw9NgZ3pJc6ruynxhrGBsTH",
"1MvVDfVsH6cujjGUupjvZi7zjbX84cKbGa",
"1LGEkw9tAkPSHV3UYbHv5AxW3yCyuj6Xyi",
"1CLNVsGVHqPFKN3yZ63X3QMZFoEABLvhDJ",
"188NwKZxypWYQfuTqgBhAXmCz1ACEKgJnZ",
"19U2t9gL9wRcBK1RuqgpgazAaV3oyXZEhk",
"16jKEa4VdKFaS5Dv3GzUWB3WQDW5niZLY3",
"154RWj6325kjV4Au9njqTD8sycT6tWB1L2",
"135kHBJHpvgW5x9Lh5SmGEaV3Pzbw7xa5Z",
"1ERqcjpAgTJGbFX3YjEdwMFL62ZzYavNoB",
"1KfxYgZc3W5AvxkPLhzkxio33FTUw4Uh3q",
"1NM4Lk6WE198scRj5Zk8eDNDMvru3KRb81",
"1N6QLBmJKk63isFb7LQCB6KDJcUuCHxXPr",
"1PHBq1YP59jqZWCTRrGY4bNq274rtCEVBN",
"1Kze93cYnjr6q2DSjD9qwNj1H5WrBpXriV",
"1FFM3ZBvE6FFHM7qq2sc8nBWMNheuZVLzS",
"1KdR4U1KAZM7e4X36VHejz1R3r5uArUPN8",
"1HaGG8drh6LqEZjfoGQbVjSboqEX3A8DN3",
"1FNMtrVX57wv73d8TqSeAqgAo42Y1n4zUh",
"1JxTo8VAfM2dkWkQfTeH7pjMAyX5SEbc28",
"1NaGrQvzy4xeXSjTdCrjqEqyzC8KAH1qna",
"1P7unvpjKn1RBYRiVvAQFCYctfw8fDXm88",
"1JQFhFQx3uSD5GiTYrc8D5nu56kL3gHqmy",
"174tnyddjwFvEF1huqg9Efh9xdVVuiaNve",
"1GHn37R3Y83nN47KWtvyWorUqq6gNTaLGn",
"17JjwmYk3o2EKVrwrPtNaT2owCh78k8aYg",
"1M4yZ82QxBKft1pUCwXimCZZoCjUBwnvim",
"1EQziLMz9A5Cqren2rTK1sL9o2BwdGPmPm",
"1KKAoxS2eGeVwP4akuzeZ2TQBaUXioN7qS",
"1DVHRxGGX4h72T7VRG1FNhWnPukRVkZz2E",
"12uqQVcPAakhpahfdx8RMGHA6qbGUJJq9k",
"14yPvwFGUEf5RpGR9jpwX5YfNrsNqjve5X",
"1Kf25sJvP5fFDr5k4e36jNf4qLfN92v5Fe",
"15TzqbVrE1Q5bvuPAvXtLmzJztcdiRKjmn",
"18d4qnBLcKDX1kaZ4cJLnwe4fMZS8wVoE3",
"1M5EmkcuUNbNgwKGn6eFrx58skBEPd6zmc",
"1EKhJsSJ5cAToJU8XAy6btXxm97gFxi8gW",
"1PqwjvVbnEVKVAFXKJ4gMgoFicwUNw32cT",
"1CsZ8Cz7R6SoH6a2F6e6i5homRi2JxBXJN",
"1EwY6FyBHXfhRfp5UvxM8T8F4Vgw9yupM",
"1DySt7sDx3PxkHn4pxPhSTLB5K34aJE4er",
"17SLfkpV4MRxBR8HZ4uQZaQZDendFCAmSr",
"17eqEd3ZpWWtzHhxWgodDet6YhpoTZX5rL",
"1DBRTGW8sLeFzHewmqGB7Rj2vWKFcYF4u",
"17PSkNXW21uJR7C9x8hwLk8Wn3TRwD1mma",
"1MkCFE4jtbKGaUm8yD81GtuLu54kLDkjdB",
"13vKqdFaY81dB9MXJTyBa8mQDs1HBp3XP9",
"1ExCf6uBwHqKiwAExRg5m4AhHpwRvUbkiU",
"1KX8o4h1MFJMi3XVZC42qNzTceNpKahi1j",
"1Nnz2J2WhRHp5J7Uw4MvEzdHkriPRY9xtM",
"1KtuKAmpeZBJk9XzsjnemkAWDrV4bobfrL",
"13Wu7rnUyVTenTGS1yE3qsV86cHhyCSzyb",
"1539uesHGfrqxHuvhoWfdAmj9hhBFS45ey",
"1Li2hEnrSXYHn7nsTABZ7JEr1LVzsrwU1c",
"1Me5V7EevtZx6aZCSAF3zyGxwzTJ7ykQVT",
"1LyxVxV4YSYxD2AL6Jfn6TqXTwoGLVhqYQ",
"1NUYGBbdBLkW9jUMCoseYmy61SLgUN4urc",
"1PsChseLdEgkMzzivvDwBfPP6cuP8gBoTa",
"1KfRkKkLo5etT9yn3wc9mzaSTACsAsHcEL",
"1725eHHd5XEAZcaH5S3GGJXEsw2tgvYrzv",
"169hnu99MZKTAXN64qEk66WBznWccSq15y",
"1EmDo5ZmDUGoJufwMVzH7T2MM6LXEAKieu",
"1J2gufNwPkmsMqWCop4LWGZtRm7EaiqCjE",
"14hGanaou5eUL9LiL2gjeqCFA7cZE44HMg",
"16MzVXEmTPLmgWtPdoWW4wsLB5ZxYnmFF1",
"13crhbhEsKggA4s6fpEQPhgKAo7xd21XLK",
"16c77YFmqQvyno3zoekUFwtCHuJHJciA1u",
"12jMHp3TjpSGNjVjW1Dd3gmJ2rEtiatXHF",
"1AmzbzkDTiNZfU6AswMFEyTYgpq9RSrNNA",
"15A4V3KaE2fnnMR1bKyJJs17wdmDVnVgYU",
"1L7X6CBY2y7dzB3vyERWH4zGeGemnwuYWs",
"1DReH4YUTSxi1vNWymrSmz6HDMnB2ngEZk",
"1FMeTErEiw5kJd99sFAQzpoRVF6rNw2BSD",
"1Cb2VxeSNVrcihWzGSjSm4WSJRXSD76iiU",
"1Lg1SKGqvkLe5NmuS6crK4gRVVdjFxfPft",
"1748og7YDSyjZC3XwPgvbsN8VXG2wH3eS9",
"1P4TQsfqP3UUB3o8S6Ps3oUMVT3aniRxgZ",
"1FyK15pe2ALTTdSEZfCUF2awkaXGEhUm1",
"1kULM7aykyJ41WtrFDKQAPL1qWzQEDLMK",
"17nCr8ffUhsjhcvqGJgnGDJvprbHgGijc7",
"1J1sGZ33kzxp3Fnr8FiarUjigYcrN211UY",
"1NaLdPLnZ43f6NzKeZt2FfhaJfS75ue7cv",
"19oxz3Mg62Z6Fv4DzubwF9WnFnfpU1vUdG",
"115PcrnKRMSpmp8nUxbUZ4UXf7gQDE5Dju",
"189p4j9VwWkouUaQ76JEoB6vQ2Rz89JDeV",
"1uTZsas2nh3YYNvGA9injGcxKmZj4ERGE",
"1Nfe4SMo8yPNY3PrFiDCkkxwoyWSgYCeDJ",
"1F5n6LGFdZcQnua26LZaQUvn3YgoaN9Azy",
"1KpMAk3j4vbXCjt8coBvroD4mkLsWD6ypG",
"15NzsbFombqjtaBtALvs3voSerZfZpoZxB",
"1NcmyvYapoLJGifxSkcULgcLmwZT9QddCc",
"1MavPgqGk8VEMMjvkmapJSM5jzL9QSjJQr",
"1MyQrig3rPkQ4NNGrCnFnk296kH3nHfo7W",
"19f9V6znRkJrGmHNKF6vLuY44F49M8aXco",
"1EfmqFyh2xot8VimwTNzkzPBCTqdYwUp96",
"145KCg5LkkHue2wHVZsR4PDySfXgVmGp6B",
"1GNhsvAd3EFcf3q3189uMSDuVkBEW5z1T4",
"18UXDYap4ex5CFkX4idEQKfYXidE24duLP",
"1MqBK4WdGgSooTq96U1mdSj6m43i4MR1fm",
"12VSr3wCREJMchrdMvgYpkkzfGHmSw7LXi",
"18gnSMABPxV6tYr9r9R6rtDqYNmGYdd5ch",
"1GFHFWDA8EvFc1FFdaMyFdCCn6ECFpc2Ko",
"1FJJSzMTUF166KricJwtV9Z4yfQXE2GbMz",
"17HjsTSdJe3aUcJ32WoYx1qT3McVq61cHV",
"1Lc2t8Ab6z4L2bhwiLf3cC2f5DQvhc8TLS",
"1BZRp3abF3iba4inmsTtUyVGAUEHJGessZ",
"1FJhLpZSej6tQg6gsHL5dMfauEkZXTavMB",
"19R8WCUC2jYqSfSm4YomdLvsWZHCGKVQuK",
"1NV8HMxPf7jdTrtnMFh7dcqs3tBS4BAHwa",
"16DpATB5xaSyzrQ2JKivCAjQMR5tvypzci",
"1DADpNekvGcqDDCpH5REytZJsN4jEAaaQJ",
"1zAv9asTGsjdQqzn84qSTL2jny1Kq7QiP",
"1Cz1kpBdYWB8Crs186Z9BCrHA8GvxwFqnJ",
"1A7SpS7T4eiuc2KmKZDrtBoasvFFqPkNHX",
"1h3Gge62CARp7JbHyEEahodv7CHoti5Cs",
"13qSihNPSU6ueVin4Dt5sLLGfG4BdVrBPc",
"1JT5zZpKfphLgJGoW9r6FmNyBqe84A45gd",
"19omit9NHa72ePJTZCfm7JUUhzruZ4zwcx",
"1mVQDZKw7dYk68vmdiB1WFUcUbtXD8Kxc",
"17Jnv6K1ACy83Na1sm3vBn2K1WAr9HfRhK",
"1GoLukUhNqi9bwQFrvoTk7Y5RaWsLroAXR",
"1Q6WAbRSGM461qgyXNv66DBpFWyGLL2b7h",
"1MALLqMRxess5Tut685SEdsCL4CyWfUnXU",
"1AwFsb1ZjUdGb7Cf2Bmtsw49xaJH66EAqe",
"1P954gG7djhDgzBeA1Wfrn49oRQ6JEdppC",
"1NiUb42s8hbVdQZskcgyraYY6KNNXicWHW",
"19qv1pxLF8xGQoKCF1tnq28tm8Hz96S3nJ",
"1Mw8iM1uFTPAwwz7tzdERpUvZRzu9zAGUN",
"1N21ApX3wuAU2BkQ6uBmSV46kjxdFkSyv",
"1G7RpwFY4TaXh3ZAL4Qdpfz5CNZrsf5y5g",
"1C62RZb7tW2NE2T8Pkqy81qRiQCFdKJ1NC",
"1G9HphNYKPqzFaRGRE2d5M5sz92dTdcJHf",
"1DFQjY6X38wmB9SFbqcXj3tKPtpkveyVXN",
"1AqC8P2AKQ8s3BfPTteNeXu37f5qh15Hc5",
"1CNzdNP8rSXnoHCA284QkHkRpgHRQTB9Lt",
"1KRnZxVdinV2JHrohDChnkbNSFceMVTJiV",
"1KHup64CzdDN6j2Vtf2DfJ12KbucrGrqS6",
"1FoChRvUQPGZE4kHmod7UKwwgQJrHjH5Lx",
"1Au2kQGyhpX6gVMD2Y1vRue2CaMBnyKNQ2",
"1C7gVG4adUFXQErbL7t7ZeJhafvrx7uEbC",
"1Jv6FXX7w9MpM5oiCh4MCjU5ZSoackCydv",
"1GZp7LbPtTBd2TLeeHxo4JQ3kMsRqfD7LE",
"1LQiE2tGDFRWofvmyri1qQPtZJLt31Ecco",
"16yPvjnfQwise6J7YG88MQf4RsWHLRpwRZ",
"161RBt4DPxWyQi6s7hFaEJ18vpeUtPjebw",
"12yxtfPkW1KKqGXjqEvQ1QT9Pfk1ZX4baG",
"16nrHQgpMg7fvUoCmFie6LrqVnNQqJKheb",
"199mQgH7MD4ARSf1XhNWenQ48tNDVpkVac",
"1BLBcar7nMm9sZp98oBadLE9mE6S2vLstP",
"1iHA2iz2rLA4fDqJRpCwPo4ZABLwRuAMt",
"1G2UkfWprnTaSSmM1ZcEeU8aaeT69Ewe1b",
"1AixDMptYZnbvWswgJWivu7xRh1zybsWZL",
"1NHagYGV4GdZq1SWPMz9FcnLePuWPsTcmG",
"15CdbV2sytUa3ACreAsUW8R5hMUdPEeTL6",
"1A2Q433N73M1aDwXKBgiEbmNf8GhDNnpbE",
"189dkgfMUxh7jSZgbSDAHb4H53Cf7ezdxA",
"1GiuGPm83dvhwehCYXtfujw35U8mPdhYRy",
"14Y5MC1iEzcoiNyooochSMvSt1jiYaVJup",
"1KmiFbLwii4bhvSFuf9XqxXxWzZpJ5dEnn",
"18mNPAm6J852AE4NG2CznDFdiCbLDCbcaH",
"14n4MgG9YGEFT5kfXNPaWCF181LrVfyf3B",
"19NH7ajnespXB68Kc2zb4DNzjpVbYBqWQq",
"1NWHCbPE7fVXfY6QEzo6LM4F2GPxNDhBFs",
"1Jf2Xfb2E447RJcxUVrHYLuFJRy7gGVCpo",
"17qA2RhnQRJiv4SuZa3pmRaayX5WqgG4gq",
"1BHre3WcpHkkCStb4PxXuSTsiDTYxb79ur",
"1NWh71mTtwv3Arqr4ztwpmKsyfSCE39btk",
"1Bg9zwZfEYG9FhWUMBNP5e8p4d71UtwKi6",
"1DTn8ZEMgvyAcDfRLRrGfWxc51dt4NhXpA",
"1PQsX1ou71HYALgQYrCm7ynzRvQQwo8xRR",
"1LEyMdNUWxgQAdTFtL9jWGzgAvVPcP5px2",
"16k4VhVoYy7WGETGus7aEZFQ9V8UXiBnpQ",
"12TVw6ALL3Ztkbi1ubXykRW7qWtooCdk62",
"1CW2xWnNtdbhkikFenZDwtsKpGVB7H6SHC",
"1B18n5RS1f3qHpoDysyMzp1fj3J6NgYJuL",
"14CJgDP9JwwFrPRRFjxt7mRT93QLkDJcvB",
"14hQoXqtM7VwM1QkHC5ZTiuTNKy6Ro3KgU",
"15U7NKLf2tREextpcCvwujLKZ66Hg4UbLU",
"1FDoFZoaHtxHCPVazcs5h1wbF8nCHqX1YM",
"1JpkzGGnMCNRvnqDSJjKNVwT8WLYWi9Vqs",
"12aPytn498Aepqcv6dcY5tem7fwi2taFsn",
"1CdbFdq6kVWQDN89CCj7BabRpGP1SgERph",
"1HBa1vGj6VWXvgSs7CLgN8ScBFZvJhsVSY",
"1Q1C9A2p1NJSuKiYknT2mjBx3pKTv8Kumk",
"17DHs1qomGdUH3pSrFCMUfLKf8tQ6HQwvd",
"1HMXtSSFXX3dXT7YE11328KZRY5mNYcyPv",
"1HmHtbSvCxf9jX8YN4NWEykPymVTN6EfiV",
"1G3GZtSRyyQjdXJZGkwJyLv1NNVSfxC4BV",
"1HVAzrSzx8iYUaqJy7y1yihcVgEAxPc6R6",
"1Q7mwDBGJQQoyoGWXFUdewZhsY4HWKTUGP",
"1D3GYmKq2fj2rMHL2VxWPcb3CNy46GptfU",
"1DnfwXRQR97BQCW81KG1rmNpYWHGM1rCu1",
"18tUvf96qvopqJX99TBZHcQQwKghYJg7SY",
"14rAbMSDTiLYvWnHCdP1RmmBKHBHBLwEHA",
"17kpvLvVD2idD296j4gB3SNyot8CwT535S",
"184D8qK47PkdBCvZrLH2KGaUnuniabuGuC",
"114v2HVv5mqeySNRKo9HYMgSxLdBmbcYEL",
"1JyHn9NF46RGrU2jXv2x42oquMhj4viV4i",
"1FAqHKKfPSv9rkBgvpZjQg2v3nyRphuUMq",
"1767cekceuonX8wzKzTChqnbxHj2Kzbkvi",
"13tB3uzMc6YCh8sjMpmrph8P8a59L6vNtf",
"12MiU7svHw3RwdaycfrzoGroYoB79XqWbJ",
"158e3xFFC4pKrAKj5LoF5qkSde9QiXfBJq",
"1Pkd9gDA9qEkJDtbJSH9xEWPsC8SRPekZQ",
"15i3QoVAYvPqNfijd64mxb2Pf9PCY3FSLL",
"1CCU1pHjfHako9Dh3ZRQFwVKj7bVifhFsV",
"1BZtp44pi984kWFzumJMBJfrCFKCR3ZMVr",
"16Ua7EUM25LrnDQG3e3RTYX2S4B5oFJjb7",
"1Bxf2zQu8ZuSnRaU11JUL1LNaTPZhxej9j",
"1HxMhjH5mFaDEh313E3kJt9XKU1CQTusdJ",
"198xADJjvvyFhZiqxAAinKTNRHToGtgiji",
"14wkkzj27Du4B5iBzbPA4iWEgR1KXNekaQ",
"1ANSb9kx9h1wnjtLVypTf65AWst127SaJM",
"1JtMkRV5sr99nRGTPxWnZTHA9QYCmUmooW",
"1C5UqhsQCC6HuVFUpXTy96t7pRC92kLQk",
"1Jw1Rxj8SsqoVBFRwRN2PgCYc2Dg6vddV9",
"1Edr3KjSnhgxVzsuEphgBPtpJkAtUEL7oW",
"19xM2Abxn66HAjWcDjhndKQgoWPRKg1Uvh",
"13kgtyzUcrqmuwxQw5qqUDpNp2TryoK27F",
"1EVwiGQmHE1Qxf6ruYmvazCPJ5Y1AUTQev",
"1K1Lga4mKkT4dyKWby7U3kL1PbGeozNeU8",
"1EdPKQFabuLfz1wzV8sQ6SsaK1anjYyARX",
"1D453DwgxHuGwXnkTQiaPEHAqoRLyggMej",
"1FS4pHAQEU6FXfadHGspEsVbZQNLuMNiCE",
"1Exe63fQVc71b6EwBVo2SVN1cby3LKb8L",
"1EGJiFUYwrhmTmR8QrCXhiXn2ECu6USiKu",
"1ApZ3pXQjsFodm53osnhUaqtVF8bPufdR4",
"1ASpsZMoteSpE7fKZXjVunDjojZJ5eofNP",
"1BQpCG6P793KVGhMnGy4aqFHMMh7VgAk5x",
"1HdEFVJTW58UFsgWdyQMbLTryB2paKra1K",
"1BSbWPiUbCvnSphhvyy9dtbtGYEnYRsZdJ",
"19BgdxkpNQCoHjLjhgbsPmibxhzxiLsoAi",
"13iHCnLejXz1UYS7cVpXxHtnWe6ueUFUq6",
"19ucxPJN4xqNQrZhh1rU2a2Hjkp5gpL2qw",
"1GjppuMds2UbxkGXiVid37p6PraPQny4w5",
"12cdTWGTbUzeP18E15JymaAxsc5mAJucab",
"1DR3smBFs24dozZEmwnH4QgWwV5zxtdCah",
"1K4Un7QmXrwjdHhjcw6GpkFcwz1wfQP1Jx",
"1J1gJMRX82RyVHynX1HsgPXeWcmtjGyd7Q",
"1BVbNjaxhw7Hi2pnT6vMiFFvAzb1tosW4c",
"18vyqGzHvMpBKNyBC9NBva8csBiqLhcSM6",
"1NXJgjirE8Z8EHzb9yqntW4GuUXUhxDksP",
"1F2ye8sY5ZbAx1zVNxvfCEvniYK9iU5nD2",
"19P2ULRnat8RikA5HPgWd7f2b24EBMAt5U",
"12jPQi8SyruiMw9YhcBR8Jy5Q1TYzV55RQ",
"1AA3nrqyciLbrPYbFvnBrcwuuWaMD64vFt",
"1DGZ38wVR5yVKKyUVw98PimyLdrAvNvg1Q",
"1HYGzcjNoExtumTc2LtTrhcW8cdQhDuB3G",
"1N2qQW6rzQrjukF5Yu69uoyGzWT51BbK9c",
"14KefUYnwwpLcPH1nPQKW9GW5aE2JYdCwg",
"14CYAjD4oKB3Wcsvv7ZgmovydndUWeFiS3",
"15AifmUQERdVGUZ8DgiHVHp3kYx1XeD7Hi",
"1P2Gt4cS8dRZA6yyfCgwgzD1HNxtj2Db6e",
"18QKBJQg5vgx6m7LpR6KNKWLtrgR2BBFDz",
"17UNETyN6hsdExLEVVTw7WimygL6r35ihR",
"18sXid52pXVK3MzW3hTXN6ebpxkiNsMafY",
"18Q3BxdzUf9G66FBbarNUKwdfp1QxoJvk1",
"1DeKK384AsmStfCvrkyF2GYRQe2hviJqdy",
"13H11rN57g7oLQbE5SNxJinvu7RY2AMV2b",
"13X9AU6TtQYED8cAB8CZf6QcwwQ3NLegw2",
"1L4tGs38svNYy7BtYxX1hdXviLv9G9A13A",
"1PBwwNPybEjcKjUq9hjEvpkVePXy2xfYcT",
"15BP1Fkph7ZSHcpFhF51f99j6f2MrHd7U4",
"1BYgimgjgfHFGBSJ8VhDDNt6CyeS7CneRB",
"17SS1EGMzTWsG7x5RPZcb9tBDcc6Hjn6C3",
"18HPVs33jY7pfxa3GERdWya5UsDTvtTGGW",
"1HT5J6GAshdZY7zp3PBxCRSjrLKvR9hSus",
"1PvX6yA1LD5X6ZCrnfwXX8iZKBe48PvBMd",
"1HUdE4Rgz4qwZgmAS4r96ZkVLvXqU67u1i",
"1Chuk3VzGE6wYHvqCHM8VEubSdbVWdFTHC",
"1KJ56F5jvMgXakHS5QXmDnNmEZ8WphN8SK",
"16B4waZZ36pUimNhgbQt8SJTPNoqaTtYP5",
"15dKifBBoWUhnth7JDtDgKF3xsPr96qxTs",
"1LZJ7JQfVLP2wNC6a4YUyXYaWKdmygeVL",
"18s1kYqxaRt6cAoacnSQGzjQ2Gz5MQuKnP",
"14chZPJrUyEyKHeEDAQaFyYTcqbx1zmyHC",
"1GPoQQUeUsys3yquseGWgcwHiSFMGfTwVE",
"16jG8AKhTs27YbfXQVY8oXfgqMYid6aDm6",
"1Ew3FCGotGX9NPL5qqMFsducumFsmnfgrM",
"149D2DDZKnShGcRo81iJR1KPqLoaNwNnnD",
"1PmkdV7B7Jy1cB7a2mn8TXjVN72dqfcEjn",
"1G5JSLCk9qUvwQD6ebKXJQHQVVtZqPiU7a",
"13B93dYy7x3K2CBFgCrywj4u9U7tKdi6qc",
"134jMTBkN23uzw5eAJHhZ9RL9fVdojERWC",
"14kJXbHVm52bdaVUt52zVEFQP6XUvGdTUX",
"1HT8Pe5VvHX61kg2L8FfR3R78BtJQugrDs",
"1NRNrqBw2ctUFHyhpWKAGgTx96cf9UVHHK",
"1HMjrkrgsoVQsSQuFrWyHaYHFLMQVEDKwv",
"1CE63dBrUnyGq3KSoiLVMfmChCaoUhGn4k",
"12mqjYpf83YKykr4L9aJDDxRfijjgiQoNn",
"13JX5YURx5mU4CrHDeVVh2feNZP8TXp2FS",
"17Gsg4KwwxWVj184ujub3FyFm9ZahCijuM",
"145rSYJqCsTjoExByC6ZzRnamQWLJB3YkH",
"1QHqA3U72H6zfwxphXVBRZXj15QZkowHe3",
"1G8mbBjZ2pwRzP4bCPBMwUPRVLfDKxXdmp",
"12czugYqQ4TqyZ3YSQz2UFujagj3crUTiy",
"18QnpXzdmoyD3un9j3uo8NMiqH7msLvFpK",
"14DyHWgicPv8P98AnyL2Ry43jYRvu5avsf",
"1EHkAFUDRCVLUaerZcZt3uq3Xo6FJWXjQJ",
"17EiLvigQ9t5B8tXjorrNRZnCzHWVViCPD",
"18ZLzQgmkTVU6JnzvtVPpuQDGmacYb124c",
"1BgVwaY8eW8ET877pUa2fe1DbJno3kByLu",
"1MyVyS3Sy5Wbi3CXD76gnhnSqKim4DyA5Q",
"1EK21NbdV2cUbJXedcLsYnjQCREsu8daha",
"15cjoUMihCaRfGJseeMZ65mVgUkFTnkPND",
"1P2Le7JbUFXk5ZbQhajc1xRTKuYULtgFMM",
"1LuY4TvmR9QTvbHExGrdbnCBksjPMhEoXa",
"1NgoJekuHCudZpCtoQ4SaGVC6iu5gRgCAz",
"19ZWKwKR4hjV8ucoWfMEkeTuYEUdpxzMtg",
"141XXvDT1zHPQ7TwZsz86cFgEBE95pbba5",
"15s7BF7VctPGaDwKaPowUzvtF1rD4N1uT3",
"1PyT1pRAYgN5gFyD8Geo9hBmAGhrKe3Tj",
"1CsSCbXXdNaBEVQ4wrvUMKZBdBmDRYZ4Uw",
"1BCEX586HBMn2m8VUVoCfLFsnXeRB5H4gE",
"1N6eoCG3TipQj3uHpqNUhS8R57EG3nfEnL",
"19AiQ4JFTMzbKQwsDbBoZdUTjKNQ9Y6BfV",
"14RM9Av1EyjhTfwFEcH1YgrCrvTXKUQEiA",
"1Bjh3vdwfYPczR6jQ13Vazjm5LwUjVGjcd",
"12YdbWxseAJTvm3xVsf1KB2QKa5aQKSsUa",
"1LW4DqdyJq2Qe7GKttd4Z1vrjoVMKjLjJ1",
"15dgeNehwo6eYJet9vSjTbtFRBX4QtU7h1",
"1D65b25iHjvgwpzM4TrDVAvSZv7XWaHDBW",
"1M9T1JWmL9LEyKV5bMMA7qGDTgmhLJruty",
"16Xy6M8X78JfUYt4rHrhxksNjPKJWBqESC",
"1CStFUWUV96XD6hyoe7nxHu6j6u3w78miE",
"13keLJRCD2s5YMbC37H5DRxB3SvfZT6LhZ",
"17yc77dmBU7XkKwS8w6EWiLcrqWjujhbLR",
"1AT6mPaKd4PiVEy9uu9gMNG2irjVvqSggn",
"1Ma74HD3hsdx42tAAvzipQw43kHtTGt7hP",
"1CUnMKVh6LfUs3RFgRCBmYxPtFoNE4fERn",
"15dYn5L183PHBsgjXKvoiktQ13AdgaJ7fV",
"15RnQ8bohyKL6q6fK3bRxXvpbEt7aLqELp",
"1EfkCQPVdVmNxTr11wLmjr6oFzaFxHBDrh",
"1JHZBSUPnh9xuPHEPaa5w4m7tfcEHqSRx9",
"1LNcfDzWgZCcgWeWs4cBvDFYkr8rYLoPZ7",
"1Krj8ivc3UBkkEpEnHnhUqe8AhEcguopub",
"1BBTrm3nWTx6KJA4EijxwjN5fc5nrGkSjk",
"1DSuKkkkkrfFv8MevTDwEj7s3LY2qqfsgr",
"1KczKWjD3WvDzTPbiEgSEQti8JBNYFfXGj",
"19QiJac8McN2r5g7Y3W52KrWjVRhVXADiW",
"1KWMFMLoxwvaTWubmhXEUESLyrMHH5pNER",
"1CGgGuxnBYK19TXyqkUBnVSEsLyjieo8vE",
"1GtJ9ukucttM45T9vfdufpsifKFn8bteFq",
"1FtBVU7KVqP3DFmwszVvWvGTPrKFuwShCx",
"1EgaXL36Y2yYshRj7Puwt8vSBmmfa3NC1j",
"1B9M9TEcjnoQ33M2cGR2nbvnwY2Qx3CQKF",
"13Wg1uUhfHhjhNKmYVetWGVMxjZunSxjgs",
"1LGF58QNX7dvHiL1pHYNeShqBF8hk4qzLN",
"1NdaCKdmWeUfSwZXfodAChbwt6TZC1tR2Z",
"1uNAbJwEYuDfcigrzLmwWgcyYK6wF4YMn",
"15jZczuKFn6FM1PQCvwLkD7rcDdroKASs8",
"12fVg5erqW9Jorf1kxaeQfvQDnD3ytHiRd",
"14sm6U8xbVsCoUgC3xu6KmmQHn2fUiSpu6",
"1J7sJV4zgmHm245L6DuE5SJMSR7yQXmYAN",
"17E7sb1kJRm5sJQhbQWDNQA8SKWxzWm31X",
"1DG4bD2ov14yx2kqgLzENMK1eiSekHjrPr",
"1EB24ZiyxVN2Nj38nj51ZzwYyaK3muzfCM",
"1L1PWYCGu8xUU6epbw4VvNAofx5dWM1jwC",
"16J99Lvd7Emwk3R28ACXeTqugNg5539x74",
"16ZwVcabMJ17P2r52xjM57KXDyAdd4PCLK",
"1Cj4iYNo5mivW4Jq5ZTfuWmB7jNuxpvRSF",
"13PGrorY93iWPpghdBSDLSHoPesXiYLR1B",
"1D3aNZSA5ETJExapVtJZ1vA6gVnQwu7aBr",
"1AW47AK9VSggm63LFWs9LvVyfUfG2rfeHz",
"1CQcwoxCdQkavLDAWsJq1GB64bQjTYyRR8",
"18CwNpZUXr5FJPjbZpZPEnaGeBfziyrWxz",
"1P9w2smuNP9C46iWoAMT1JE7imUg6uXyEi",
"1FFYe7Q4n1FmEqoymixfmqjiTrFAhEztWN",
"1HnziGSWiasT5uG97Rywn3sWWfQp4WyktG",
"1Pb4UEVwDRZ2x6g8L4HKo2jdTaRskAf7qN",
"1ana1duJTnNvN4H4YuJt4MCXunJ369fw3",
"1Mi9WLzwbseYpmUR8tGFaNz1DyN3eQypCZ",
"1Lj1BrKXc6iVMemSGG8ZhiyT8nutQDXKDg",
"1D3PfGawtJhcxkQuJ5xdETReBMhxKkXgqk",
"1A6JccRb4jbrYX7J5jgFYJ8sMCYtGUnkSn",
"14GL8oqADNK2bWApWFdGRVrVn9LLyvbAKB",
"1GLkyt1uwhrRvd5JzxoTKZFyXT6qMYL1kD",
"1FJqgtGmkrxufwktR4SD85ktageL7gK5RX",
"1KKschNHSJwdgK9Rk4h14wxqyv1kkKAWpU",
"1sywUCf75u4em4bFNZFvqtVbsMKKAsdBu",
"19EgZq5s2jSkmrXVYycRxLmGf1STRd8WpS",
"1DZimyf3QZ5vzywPj3uQVTZQtmDbZFT1Mb",
"1Mvsx7KqjeLdrPDcGdDRp8bmbTL7mjcAcp",
"1FUkbKFUA3ohX5MdWwK9tuhoPhN87iMvuM",
"1JjjKjjqoybREJ4sPFqoHdMPwkENWh1RUp",
"15meezq8bN9STKdjDhwGahQ8dSqsTM94cY",
"13AXnQvzP5MPrgFu6U2w59CY6DbanQbrGP",
"1NQcZ4vH6aDiiSaCmwBuU7WbU8J2rRb2Vm",
"15Gs1PTmbkQFv9dADhHzvtZ4b23ahGC3eB",
"13i19ioFpcZtZCoFuB4UNd4FzDByVsLwjC",
"1HSEDSnL6uTd9DxQnFv5siHova9eXc3vFr",
"1GYsUPuoLtsQGS5o9zuaqMFBWL3noHxJjK",
"1HWtacx4qcKZqtBUNSEU8rBE8c9UpYe1BH",
"1DWUVHK1HmSyHbtpHvrUtFPKNRWAwi5XEp",
"1BvX1mKCYHwoq3dmgAwJPfr6c1iHBgLPUF",
"1MiNBk4265wvnztzw4c7c7DQp4pcbMAmHL",
"186AChFyFfi4H5k8VVU7iCUvefPBsePcNW",
"17eqVJuFvH7cxnbZDGpdHysziQiSTFepJq",
"1C2MNF3kBt1T97CphBHrhVF3mtWhny9RZ4",
"13TANnTxJ5mrA8xv4h1eZ6uHvscvyBeMnp",
"1C1RFrHX3QygxDvMMdEicDUJGio47697iP",
"1FMP8n9Bh8W3oDigzRR8XrE9of9GhX4bG5",
"19XxPC96iipjfrBmYCzgCrXUPJMo6BRHHL",
"1LtUmThECBsyBLBd6fVUFvxwc8zVCoSQbf",
"13vFGfUA5aQfffvMWvt81sC5G1qWSNQJt8",
"12m2soh459e8RJDWQsz4PiLjSh3nHQNFqv",
"1NDmo3YiW9aSmucHnV1eqqHGYo7iMKS6jP",
"1J1LSJnHXqmWwc2BxEZu7bntq6XaVbuitG",
"1KyfuEKvQtDCqv4Jez4BTvmUtfbvTu8bo9",
"1q6K8aYT99JJHsmrhY7YFyzxmLcLkftKH",
"187WfxTbwRDgdzNix9zXZTM3xb1LRtcbAg",
"1Jjox9GGktXRQBHW99SLCvBurTKs9yPgtQ",
"121X8wR6YPCEgRChnrGxvzWMfsGStRoYa7",
"1Mp2pcTu44kp38muiUo8BoB4wdX1efGkL9",
"1DbojeoNzJnUtCa7bCWDFonPBsGeUMuHmA",
"1BUQTPyJXJ6fTRN39ffTbHcStUM7tBysQB",
"1DsmyjzMByBThwuBiJAKpWd995zvxyjr4c",
"1Lrm3GArt8uvDJzWBmRgioFuYQVf1zQTyQ",
"1M1rJiy9EXaXRUVbDLQe5xPCdxmQPeL7So",
"14SJjoCh9X4RBkPc8zsqbReCoEjDtjNtTM",
"12gfFgY8u6GzC8P6S2uHALaKjB5q1gm2GS",
"1ASjPZtHTAH2Ak8PB6pYwMrfb74QQR2XVv",
"1PUDHYV1yTRrxpogq1DNa5zidRRfETVqUY",
"1HZYNdfFsqYWhopXcQRdwEt8B18TU7MDAq",
"16ibMfJ7gqhMW3ufshCYcSjE8n7AwNM55Q",
"19aGbkeznWwN6S9KFZGWBESEvRchQ8XLhX",
"1M6gnnzwoRrxn78zf9pbAYwnNE69CcfSav",
"1CEVCeGYHwnYu3KsUa9GP59dx5gGesxe8d",
"17QVwj5Z5oh8XrEW4QGLdKmXYMhaTwQXLG",
"1MWJmj4CRmtHxg4aBBAvet4Aq6EmQcM5DR",
"1PeP3yp9dk6QmQCYHrBQyVxFDn7xTyDKDX",
"1eWN2SBizwfode7E7aSddpGFsQK7qFZpi",
"19KCFrLLh712HVqFXVvhf1SPfsCCsSnxFZ",
"1QKr258mXKwx12oMe9jegG4gRzriTRNMgY",
"13BNNterUrUMPsxqxwbHEnqLx4zffPZWFx",
"1Nw1FhBmedvmVHzDEsfSbGRJwVRYNtEgQ1",
"125wuNgkhQihHSR71KSwPpsYg8yFf5rY7L",
"137kyuZ8phvegPapffmQkzFv7S7U4GxMbm",
"14RwqVXMeV23MCNYAUC1FjP8gT9vhkDAbK",
"1DfxRdyL5fdRnhyn5M6DRqdHMGqYgzQAk",
"1wp52hF5ynaWPtGdDxA7nQvraN1i5CDVJ",
"1EpQi53L4JKpRTBLP2ccrWArXLU6sZunyj",
"1LbQpK9N3puYYUKSxpyAkLo7on1HfnZm4x",
"18wWkow1Bwd1YiGTByyMmw1LUV9XkoYLrn",
"13ngcV1JbeiFioyZbS3PKmEJVCCB6YhSKB",
"12X1u4QK8uFxZNGTEBEE1MX5QKhxHf8hWW",
"1Luv1LnPTHPxQRTYmZUu5Wo7DMpWE5voJn",
"1KVuDet2w88sVvJ9TsXaScoy4c693MuSks",
"1BscRzwkgAJHKZ7S4qbF2U2WRuQvDifDFa",
"1LujeVu5YSR4PY3FGu2ZXeSZa5GUXZKBKr",
"1JPeVSSsAL7LQLs2T4NwjgV5njJ7VYGPzu",
"1MwFxy3BWmbceKmuyp6nVQMg9eK7BNnvsH",
"12CRqGpQZgioXK7suEGYEVgeJLE4hJxzTt",
"1Df6rwuh6ZdqdQrS4J1FRxQ54EdDnaS6mr",
"1LMpEboWaRZAmzpQATV3mXzDu3smJtVd6W",
"112LYSv4vhyvo8znCh7MhDF95fhCvFuC1T",
"1EN1SNrcPVQACTxPUmM56c2NZPRLYKMDGc",
"1i2emAFDU1kcRRtSQaBtGXFaw6tq24MSi",
"122nAzLaS63NvsXwAkzvxAr7Ytn6AuJdry",
"199NEx5PD4Ghmxgo6X1BXNM6me8UWxSdgK",
"12eZ9V9vxPJ39BbuuAda5szTXYJEzheHzf",
"1A2Hhq8gmnr4zZxy5WLFVmpPvFkHgnGxe5",
"1PuCtfPdybzok1pAAxnEM18ZTmruCT8MGv",
"1GCcyfRGXFcbo28yFCAsdxdFTHMnzHsnvZ",
"1JGxJ4RWdAaznGH68dSsCCRsFPmmJgxDck",
"13KPt91XKxzB6DmZZhnnZPoxJctiwvMgsd",
"16Q5YgmsnwdkozGpTuc2ZoUj4ZuYjSihec",
"1PGGNXifQzod1MYEZpFz8W1kLeVpHdxzkF",
"1MTk8CNgYUR7msiR9zTnJFmEyBp7Kr5aAQ",
"19mbJpBzYuMw2ksqq4m1cdrvE4NHEKQGZA",
"1wrP9roWG918nExDTxopLkcnnXW2GzM6t",
"1FDD2uxscnDkgY8PQNRkjb42Sr9E4nsG2y",
"1JzEUG7dT6XdkyYaeP4Bqt9EAeUvazsSEw",
"1E9sFzguU6rQ8YXdrtk52nYzhRXZNLEQam",
"13kzS4dZditKnKmFztXCJnSPAZjXH4kD5D",
"1PqsZ93bLkqfYxpsbWgZHwTJSXwhFAWA64",
"1NzGspG8vuhEGcf8MLXEuJJQijqSorqMzq",
"14g4rAe8C33R1nLVDNsjt2Q5NZiD8JX8dq",
"19GECvur39H5QhKJ7LpfgM6dr4Xt9GEreL",
"1DWnqEjCLZ8xxEAhMWYQkJEMA7vwSMaRCY",
"1NFZAxtH5n55fp9tMKwLcytxRhErnTJuLv",
"13dpK9myTzQGwW2xE5djEuP6My33HadecV",
"18MZ4u27rj67QZmeDqeEFL9XiMcB57tSnG",
"1HZigvNAx2scJxBKP54hVxb42icbPrnshi",
"1PrYitDuDQAfUWdMczSquP1SPNY1yuD4Xp",
"14b7UmKVrvxzamTv5pLStkz3UDE7w2S4mG",
"1HFR1Wv3gFEy33FUSwEAMDdtUBaQdKUrhT",
"1JRfaethgmZ31WNQ5gAchqfCxxUhnz6o85",
"1LavTmFCNVhvYjpZAP8eRi6iWcDDEpAmiq",
"1HZg3ViZN2hyXfwsuhvMWnSFAjX11nCuEi",
"177iuvEdkKGRRWeFCrtUrTrkAp36moVLxL",
"1KyTE4AMP1fWNa86fGFSMAWT5Xb1QUjGep",
"1C9ZyrZ2wbKB2tbW9zPMgXEEyijpj1hrpx",
"15KRzh5Maaz3UcsTj6QqAr2i94c5fuAtcV",
"17keCaLspNPq88J6oaG2MaEFDs75DQ5gb9",
"15vPAbif4CRNuzzCcU6jKMPopiirk6XFgG",
"1G2QZdosLw4rBjeJSyYHtccuHTkFeSnSrk",
"1CV9yBdKgFU7sTWjDLmGXUqCirNTHjxjbR",
"1CyFK2A9Af9YsfyF9Px42o8k3x1G8Poysg",
"13P3kttCV43rNbkdpeGDWRGo4SYcEBZpkE",
"1GjE98gB5HGiKZU4gcNHbY3RfHzCS81Aqb",
"13mJqVL5q2ULaNdk1mefVn6RtwSMoT8VZt",
"1KQsYKcs7bqEUeNUJ8nqrBLSqbcK9ETcED",
"19M34HeLYn3B68keJvbwYVH68Cu1YkLCa7",
"1JyjCx49kQ5NpRgSqLdKPYkKmUGU9gYzk2",
"1Bg932qtuPPPWkx9NPbeQUDWVsheridSYV",
"1E1XK7FxDa2FqiQ8hWzCrUSWDt6w6dVoBz",
"1zgdgJzvPoBNR4sLqteZSEexyTtGi7iFz",
"1ART2msUNe1xgN3GFozncfdTrZJ9vMS9yF",
"1EXo8r5rxvoLy9UQABCbzhmkMyoBCtFfM1",
"17Het32rdHwGiHDd8DueLpfT8CqdUKAypq",
"1LPCkQeu3QtwcuWqiQghuv3fMSVHaXC5X1",
"149wfyHak8oTweoTxh5Kr1hMQL676TTmHL",
"17FLvnhiLrN6HxB8tJ2ECdKqhNa2edWBno",
"17gaUMjcHh2g4efj4MQpyQs8epqF1eYvfB",
"1Ax3Z9a7k4NJkp7bcVahRbMZHTXDa3XiVB",
"13iUYRYTePjDFZjf2veVtmy648FugVAfaH",
"1GCW3GTCecvCcdsErNn3mEYwpSqy5AyU2X",
"1PK4nKqrMZ3N3Rb3EkGNd2Vcf6S8xigZT1",
"15eXQk6hQ7r4aC1zB3n9btkJYRx7C4HeT8",
"1J38pE2YYgQTD24SoFmLy1buQNS2DcVoBC",
"19x3KCSzzAYgvSTWzPp1Eg5sbMoCnZDUq6",
"1NV2Cx2uLUQhquvGcGHE92n6i45mBQm5Z3",
"15iiST3HeCqNH3UhkMexinbkG5YZDk138f",
"1BaEm4ovPrqFNjwGE2a9mX3B6hzHh5LjVG",
"1BDG7KZoGGQXji6UyxsqEKNUSUnbhtNkzx",
"1F3iv11PWZawJivKhJkHbcSJ5hzaAUFyvH",
"19jsb916RxXtADeDb1ykXKb18Ri71EcBgg",
"1B8FCobkGnJvKPFDT5gJimqg6JEyV1enrb",
"1E7oV4a5Nj4njYm7yfjp1Uf6RmpggQSvrQ",
"1CoqekZgmZWiYfXNYKxTsoEJMCjycsiQrg",
"139jR1mRN8Ts8N9gen4d41E9wJKHxVfDzh",
"1Bidqvjwiyoj8x1RtkyUpzozrR56uA4VEz",
"1JpdmQyrkvbcQ9dvsW4HiCwXFxR3h36Qpm",
"18Vh9PsyvhpFMx3yfSznKg7ucgdKCr6nrz",
"17AmfHP9GP6tXwASLyCRSVwsKYQyiLfk75",
"1L7g6qiEYbMor5ty2rnsVSoLvrm4egnwfB",
"1MQ1TYoPyQ9F7QaT3XuKTxPswmd8e3EgaJ",
"14cKuGi8cF5cA2HNaKJAtDEUWJDqjPeXc6",
"17E2WGRi5rR3SC8YFmYKdBnhxsT2tmcmNu",
"13RfuzfDXFJ1Xh6fbjfgTTNnoExCRrPMQL",
"12k4CX4hJuRNUmbssG9jhRtaLcqpc1hBtC",
"18CbTdCanBBhx4f43hNxgtyWrcLbVooLK7",
"15QUVo9exTNRvRmFCVKi32z4BaRWmiJR6s",
"12VBFSsRs9WM3Eb9LSrGhsDQjYvRfbA2Vo",
"12L8M1fao7SNvJxGC48HKmZJ1Mq9totAbL",
"17gWwP4iF5uZU9RVddbqDSzFc9vXac3PEv",
"151LjG9mEErXGJsFuzk8pSWDJMfDB1V4EY",
"1B19eZBigq7QrECTS6rSLTGoxnVaWw9KiW",
"15Mwd2ZorUF6bX67xTHKSxLHBrABuB7D8E",
"1BHfSu8ktuQuU1PJXMSsRow6pD5YqKdYca",
"16aQRQhsUQHg7GgBvWbGz4rkdC9tYfUXAw",
"13iccvpSZgQGKRnSvUn5zAPxPnx12R8wzM",
"19MN4uSvDGTbeN5xmuETrZs4PbDLtnotNv",
"14uYz1ZH2fPabbM3J1iNEu2KR9MQoZcyCJ",
"1FGP2mUPo3DoKbsbLweqeHDi5vQr8J3Nt1",
"1P3k5agApFPQtH4jz2uctCyTruksiyxb2X",
"1ieQYFY1UzUTqha3jkeqzbXC1itqopid4",
"1EUvcg8QjYPFNQ797zV3ApCfhv6BYD4GLs",
"1FFBqmDjXNnHTeCPpRyPhAcFcWVT5kehaq",
"1ExsYXHWEQJU9KZ7He5ShNnow8oeFKLPm7",
"13TvSvZ8Zds5H84o9bEZ5r6VZRh7QaVVMW",
"1JdWBwjdK4NNawXkEqemd4JWobj64aiYzz",
"1LnAq9vXWMuhyLtJ3caByuYXRwzkM9rPr5",
"1HRzud28D5hfHKds7Z7WPgrfuWiMmZqV4P",
"1MjXiSMDcjJFv5wQcDvAkPitn64fG78TVM",
"1PTCAB1ZYD8qxP3cp61niMgrixuvs2JwEY",
"1Q6D5DqQ4Zby2Bw1moE6KDG2o84B7VDh9d",
"1Q1Fj1oS8pHL9s1WyH5sVKrBiCtFgvN2LX",
"169AQbbk5Fp6FNjtrDB1qkRbm6GubHjBbo",
"1FohSfu5MHgyuTGZConiHbkrBxqJhCfMdc",
"1NWku97ZgngWG7Y5wtvao8hj2tuodNQ9T9",
"13qvn9xYsyQtLRWVQ8UXCv931GXjugqoqL",
"1JEHQbUg1jt5PTEZKF7Z9L8DZDPzitgokp",
"12XgSoeVLQhWA73eztWxVRpMqvfMx5kGbY",
"13eP8QzvxWuFAFE9U8jdBwmcSdGMY4eP2A",
"1G6uYHrAE22J3Nya48u5LgWRhuEw6bmfzT",
"1KHURMqPYTEVRLWwnE4qZMAHSB1xQ2jSVh",
"1ECuzmMixr5s1MG6R1awLbCA6xXKKp9ffy",
"19opcktvZqkecmogiDSFcX6NZ6R8gYXaJy",
"1KjxPwzHwyptbiGoJLxR2jNkQeSnbUgj5z",
"1JtpyYhFMSXk7Ud1hSB4bT9Hern6RyqTDv",
"1tFjhquAG6buuebk8SobqBd1bYU2NWXJ4",
"1AQGHWWqL6keo25Bdj83qhnz6cWHnDPcNc",
"1LU3komNB9PYr6HZg3Ewf1ZuCD43tKGPLK",
"1BrQATzfVTTQ8R3AYqDVeX9Nmb2FDczTjt",
"1h2fMPEPecE1SrUeGhdLs8abrnZAYfV9d",
"1EucD4bXGwJGgVdRsABDfTKrjq3Au9pxxV",
"1EbikHfe3HHMPcXHkmC3RssQnEx75vVRCM",
"1FoGhmCX8xqLmTAWtRRiYz6FJiSf4MhS3P",
"1MnF2Lgn3Et5diBuM1bSvefsmShY3dutGn",
"1KzK2X2bgYoFs2dMKL1Xo1aGJCcQLXHPrG",
"1JLJ1H6Hsuy1hxEzWxTkpBzveagxUkdKMs",
"1LNG7jdmdcypuHfrCKbEvbRdTCBdwwKW2J",
"1Nubyzmqzvfbp8WV7Rf6PwkRrEP813y1fn",
"15MjUQ2gg9wDJQernPhxYvdLRMoXm4X1jZ",
"1ENCj1w4cRbXJ7cfaFCWtWx75YT8aGY4hz",
"1BCzxSwrabRz71qqsk3qLFbzTPkiiktoSD",
"18nAuvCB2uBDhoKMe9MbfjYPb1DDGRtF5y",
"17Nv3p8jZWG5ZfC1HfVyiDQyxvrySzReRC",
"1FKTiwD13UuZ61ptGXUp5v2D4ntGESrxn",
"1BLD8SQKGQC8STjwt3JYWMdLoqmK5VSS1R",
"1Q9S1avFRPd4AVX7M11tENfcVMMpzyZ5bP",
"1GBCdLn19WY8tPKwU2qCX8WmnPQFDH8geF",
"1EQ3xJqTFwX5WgZUbLGWWDQsgsmxWg4NwE",
"1582EbE3XhMbvw6LaUgwzW4dvfs17WdYpu",
"14p18MZt1pPRHde5QSjNnrFb7TZDbQfoGp",
"19q8tLKctJNWzecopMbU9w8uTCaizzPnJw",
"1KtFW26Cf3KQm14Uy2pZTGP8mQ16Xd8acP",
"1JMFEXYgsxVnyqT4u41NDd5ciL1SuRVeKm",
"18d7cn7ELsxG4Eimm7TTKNLPikmr8zaj91",
"18gww7sXNpfmMgvmJfRC2AmTRLebC2cNzz",
"1KPnFTMyyQu8NaXVoJwYBuVAjqTpHSSakA",
"18A9t9FWGq8ZscByWAWH6LyiJorpiSTodV",
"1Nwj1oHeX7QZv29JG1gYLpspTY8NCg2RCi",
"1K2FDtRYt4NuJ6nsbPZUZF7iSAxnHxtKWQ",
"1BxNiRQ9CrAy1BFhYrHX6sAwcw2AhQL5zp",
"15NtZkWhckaBh4VDCjzS8kth1YPB2tu753",
"1Gh6W13X4nwZqYDHRLgfYSphwvTZ6aT6EA",
"1L3H3icr7PsZAzfCLj2fX8uyKQ1bTFopFP",
"1At5rYJr1up4SF66BbV28DW4adym9zuP7J",
"169ymuqEyBQxQ7KR9UJaZd83mEWWZUJupK",
"174DHMXPPPcgG2A7TrncVBoxVBeYcf4aAs",
"1D4trcQgWZQ4NXQm6UrF6JN7AzpfcRQo6S",
"19oaCws7xmeCKGjdTJSTYj25DNhjenBnZb",
"1KQNaYPsk1gjNm2mPoMA2zdbhPzdmCbsJQ",
"1HcMhYKpSGK6qsK6wqQxq2GBFN5roGmmxy",
"1DKwM79sn56eDK4DJYudmgKGzh4TTjL93R",
"1JDdC7mSzW7TiRBvBEiMSbVB5UoR2BQb2S",
"15kYjR4gEd7RXUZEhPGp7cQfx4gLNteCRJ",
"19Y28Z6ssdubGBRfeEJY7oaSArrUUz37UF",
"1Edwi6m3qP1QkschgXtfjN3poEQnZ8TQoq",
"1JJdagR27maqgHEQW79aCydJXkpQVWQ1Sx",
"1JZsLaVSF6t4ZNPAmM9DVvi4dq12bzP8pp",
"1AwfSon9yRojqnFTgKxQXKzQwSH1gHJF5j",
"14iqYkrpXWmgGQJq9ie1NfeV5LEsbMG3vU",
"1EuWW65w4sUcR7wTVCvskMYhKXHuzrk6PJ",
"1HuUaK9YrsgY3VnbQrFdjKrYw8ypCuKrpr",
"1PvUtDBBKJJj67RDH4F8hFcShYyCcWrrNN",
"1FFPFXHFvpWogEoiqm14FzJFcNEccLC2uC",
"1CrNFFDHXo4esyViqHdMemy6yp7E93w6fR",
"1Jm8gYtD6HSKaR5SP8yMPBneRTKxgnaoC",
"1DxaWYQwBA9CbQVksYZpWrungsnp8z641o",
"166qo7dUGi31AnWuEcjukwHHYEFv2ULVPL",
"1Fue7bQB4fFDnwxhj8kvicRR4Rmqx1v9Ps",
"13H2kgoGVguDMGszrVhrUwUVfaF29aoteL",
"18zQX39idths9MQy3M9bNiEnC8af7nGetG",
"1C9br7ZjcQaD8bWK7zKNwZgRu4rNXZaFWi",
"1GrjGdKXVFyhBCocn9vY53dyHCkKvFq3C5",
"1AusNeMvzqFvACarNjcarnJeSb4Q2TcskE",
"15UryiQdxJ76yJX4LcFqv6XERzsUdkXskK",
"16qmtc1HhVhAJe1veSwjqR31DhKZk8akpM",
"1GQAdoTLgTrt1iUmexRBEkVrYog4D85HPE",
"1GEVabvzyjsvQMt5HHTZHqzy2JBYcidwT6",
"1m5jFo96hLo9pwJf1C5grdP9cT87mBQyp",
"14bqRNeqq9Xj3j3J7Recck2TP55KksCjvy",
"1ZHivQj2tjEA7tBGvS96CjDBawSUcQoKx",
"19vYYqpMWwSuth21wtNVuYn1dsFjQvDqQ1",
"1FHqVFhBtbQGqKX8LXj4Z55W6KRr8YUGk2",
"1BgSmuf999caHgkViYX5vGZr4kVKxCkDia",
"1GqujjHES3YApAAJTvxiamGc3sK51bnETt",
"19VGnQcSgvAGductegiPFmAgY6obj7PRvZ",
"15o4MLskXYJTSp76vU4DEkFSMwwTfVpcfp",
"1NsLy2MALy8Fg1h8DkoGqCiW5ZeA44dG8n",
"1P7bZ99hr39MiLoJK7H23Lme66W7wcWMiH",
"1L8sqqj3EcQswjyfN6hsvAMVoETvPTK1Ex",
"1Pw3wuHzYSLaouD1d8fnpzJfGAFpyKyK1g",
"1GrEwCuuekzgebd7Yfz9hxY84Hs172Q6Jm",
"17gmeH6B6ktJiCLkeshn44BhVZ79xTdgfC",
"18YwSDS2Pti82H8SjJFowWYgvc3kyTq8Ab",
"1Hyzr6HrgATah11HFw7BcMGVdfYFA3pVsD",
"16ZdgtjkmvYqNedGEf7fq6zvuT6QYpwcyS",
"15AUqgmTsUeNkUY8zn3YY6ShUC9NR14QXf",
"1FqYBG82WbSg6f2pjE3SxJqHZc3HxMW2Ng",
"1EkxzJ73XSwqGNkBdv99EpmoiAKJwjihwS",
"122qwuEqcd9kuAcSY49jSqsQ93q3ghpMgw",
"1BXTUTTCDdCqwPQKfXaPvMCcnWEcz9ZJS2",
"1J97fNrthq1CtH8P94xD3NH8p8ctYCVePG",
"159GzRrrSK6p98QABtMnNw9EyRqziymdc3",
"1KrnKuJPZUtS5NWLBR9yTHcsW6aG4ozpyw",
"1Ejs5vKqK23ABPVZ5BsY1qYSE2ku7YMs4c",
"19dDcm4vRUQKZpMnPNYLbFVw7hEpXEQXzr",
"17u5ikutvUH9dynctcjkTRZr7DM2428Y2D",
"1Ar4PwBZ4SkRyJfHUTisxmH1G2un32RSy4",
"1GeYfYzYf3hKRd5wtk6zvhrmbgyN5vb3WP",
"149B3iukoHv2B6G8wQkEDXNaZB9xBDk1J7",
"17LQWPeHgQjxNwAWTjzhx3ce3pQjWuiSGs",
"1QEUw7exFKAaQFAWsEHQYBwRhm1DYMn5Xk",
"16JiYcBHxTBdJ8eRiYHciMi4aweLjiMsyM",
"12DLB9pCKmpMy2NpyizzdKezEN48Uvpnsq",
"12TuG8MyyzyhmsW7NrL8eNahbRq6pwk19Y",
"19VJxzt97K44R7HmdS1NiChbJiYBtjzBgm",
"1MYX6otRuo5ti7VKP2vE5brpuACD2c7wTg",
"17U57X4Cw3igMfiQXbzxm4uheyNb6hvtSk",
"1Ce4ag7esnrEVmhoN6HDWGSfA4g5yRp7Nw",
"17UiPE1hAoinJE4s5fARCJT1H9ooa9gGCc",
"17GsaVp5uoKHR3fTjLzsKd9xz5UvPK6PGy",
"1PJ1QV8Q7TsgamWfnz33CNshkH7HBip9JU",
"1CJ1ZJpyNbQatVzFiqPVBbTqYdvNB9LpxC",
"1HhLrKVbQ3Zh19NFbvxprDTRsNtJhdm8my",
"12X9JcPP9inEJLG2czvST2GKqzNqzw3nmd",
"1GeBq8Mns1CM6zsjhRYBspLZZHHoMippQY",
"1BkcedSTLX3Cg9wtv7s4qmqCs7m6729EYi",
"1MUUMYjRpeus52poDaW27AieX7CxkFmeQq",
"13k1c4zzJCSRemCz9hTApcVQV1rBP4TczB",
"19qT8T9cwiL6oT3eQT2nwMmNf6Ca7eFCLc",
"1HgE1zDtQX67tEbrc23yyDt9PjctgvpYwP",
"1NFwSPENBmB4VsEik49jV2CemGv6KvfCRF",
"14MY1r98BPBZAK6oFmjeqrZruno3babeZ4",
"1BVddVjg2bEWsLBm9B3sg6HEbCyVRqXn5f",
"13vw3693jJjAFA1tdjbsEPcnX6acMEyhQP",
"1ATTJ2s6bRPosrAgPWKixjw3frtieGXssN",
"1CbDDGenHLVBZGcEnYgwm7tGjwACvrMCfj",
"1cBparmo52T4HY62h9295o8FR3URPnAvw",
"1L18Mz1ToVsdDo9xuL2r7NZhML9wqVW7uQ",
"199KastokNwHsPxGSNGiMFNNLjSDX6nz2U",
"19vnrLoFEcVW1hgqhn3P7Jj7UTrz6vJ9W4",
"1Bn4vL3wKhJJRsKH9Dfjkcpc1K8Gcs8D1",
"17V29x7jrvPpc5Xyt4CgZmoiAc34bBKgpQ",
"124Q6jNtaTp5r3R2GqnC54WujfoH2mjc6k",
"1oiaejZXKfComDEYDgLbr9HHrDEfsw98K",
"1JbfFSGaycwFMgF1wtuWRwhRCyuzbhderM",
"13P4kagoVJL4tmssSvrAcrBc9rt5ggREHM",
"1M56hJimazCKacpS4dTrNgaKAoEQCEJJqc",
"1Nn1nqDCYfbc1ts6eGj4JwxnuXwq1EwUak",
"18cEy3ThvrWAXcLguJBNntyaTMjASip1wt",
"1mkHwe4Hnmr9LJmpiGYFNhmLMptbaDsev",
"1BWQ6TeC8LAaCvquuweMKwiFdq9jJDZM1L",
"186KdYCrgFc9J5MUrVrgWcegujGysVchgu",
"1CCEMVCE3gEAiqpb4WJn6vKfvarKa9dWNE",
"195ujukY4d7pHgZcAMxAGnrSqt3h4ymPKU",
"1PTbkU1izf8BYySV2C4dt7ydRbQLjfKL4B",
"18p2HXiwxKpnEQTMTY2hhLF4QmJCV6y1VW",
"1DDDkkMbPorRsHpZjk6RDQypPEzorZvmdB",
"1NmZfrn4u2p5JbpwHK5aVdxDkBxBmkF3EC",
"1BGdUbGU44EcJ5MRC9mGktPGufyYZrAmAU",
"13E4R8QaYhSuTVBNSs5R7oPeH6eYfqwRng",
"124ocJEwizGbSTSDDuh8k65GAJrev2jHDn",
"1WdrHt9kbWVwLGaXQjLF3g5BCDaCxzapQ",
"15GmWEsJaZijW3pVLw13uuAp4CWCEQ44rr",
"1vpbA9fFDtAj7dV995nd1ibQGPX78M7Ca",
"1ECBUDgRVjj5B4pvwDtsjxqmTMr6mWFDBe",
"1LE9jPQyJ1wSJCtYDszoWmk73ugd8V2of8",
"1PyvhYtA8WMEPeNujka27Sce4r8xRb8Ksz",
"1EfSB8aZNErPbF8Yw1kYA9CjqapJWrW1RR",
"1DtNNdGZCR66wvp5PHnSTSTWnpB2y8d3sy",
"1GiZnT3JVEifY2f5yJPqy6cZVPP1y8s5DK",
"1GSdRs5skyJ9SKn76C9vL5xpMi9npVDbKQ",
"16atYsQUU52BQXTWAguuFehwyBgATfiodj",
"14Po1rmrxUZT5zw39xswofqxS1oJ4gkzRx",
"172pwqdGNGfFYLMhzuVZRYYVyubS1gpEiN",
"1LYrBcyEuYjY9VetaUEmXR1geuN2Ndae2u",
"1PrsEPBkYLuMN8Woafipg2QJMmx5YUGUNz",
"1LJ97Q3X9tbRgX1uUdWm2oowugtVNovbGZ",
"12jhbvdDihsQt2CZU4hMhic4d7JLLJxWtA",
"1PbDa3QxaxGhmKQs4g27CY3jqxmZW9QAB4",
"1MNdNwv5Krw3nuCwHspu7dai6rzfcWkxkM",
"114u3pENZCNszx6nea8f76ZKC9HDvtTVjo",
"1AEKcCN6bwHU4BQA84iSRAwCinD2Xvv2wo",
"16KX1K46JLYU5DYF3J1yZCtWjAt54ivknz",
"1PDfZmqeX8QqEkNXR1o7982PdrbqQQzSfY",
"1542tTgSQ8kTh1jMWjo2Qwji5bXJGUMS4E",
"1AUQgmnJmiKUNR7rJzZtuSB5Cz8fXtwai4",
"1BS9nBwTBcM3Tp6qJtpdZCgMzaMJTBgsxm",
"1CqAcgmwBWzresu4zpENcXfK9zxvCfZNmp",
"12Uj8cvJXGLpAEzXkgotCaAkPn1Fes3R5c",
"138j2ir2X643p2fUuogi7DSTt81JkLKQkN",
"1CCbkefxv24JLQczCaoBJRQFSA7okmzVeL",
"1hzsZHkqeptgLjHbmtxRJjeq4X8FU2jFo",
"1Pns6WJKvCBgMp5D3JA7abGFsVMirCAoY5",
"1JT4NWnTFNkpf4paFgimot3fNMQHhoCLHV",
"12spua6YysDF9pQ5nmDMS6jh1PENi6FvU1",
"17bJN7LwDc9sDbHeyscmMQCfuueaC1ZNES",
"16Vjihox7SpYn4w7ss9kCGTT8W3ULu4GCU",
"1N7FXfqUuhB1Zmh13BnA9Dm4et3tUMkwaE",
"1F3dQmJhH44yemDNUnTeYVNsDyatW6Hwd1",
"1KiscBcdMPjumbgxGERj9HtYKYrqsas5pT",
"1LVqJD3ESejhTz7HbmifjwB3DsxGTMjTeq",
"1NUeRHKSWmmDuYrTyjq2WB6U8tpwN1hDVS",
"1N26yJyd7CTF6Hdb2PomWzq7LrAfV4M6cb",
"1JnpL1UycjnuGnYegCUPkRwRrc9Sg2Wu7H",
"1HRpXqWdY5ebLnmg6adZfzyGsxXMy3mNcz",
"1NUCPXE6FBbu6BUtq7faE2cDFtqcKFuCWi",
"1FKr8KoQq6yxLURoowpbra7Wenbz7CtukN",
"1HWocQJzhiwWQGZVcxDFntgnAMkoMuiHMj",
"1F9kCnEX5rTpyzoUy5e5rMhRuzenki6nif",
"15pWafZRpqEgxNkpYS9DppmwqMFwKHLaWp",
"178FjZB6DAiHfFSxPdjxuQijZSj3hVwP9o",
"1EjHwWaL6DbtxFSgXooSHPh468ptAx5fTR",
"17SMHXVMa4LkC1jayYyPjabsY68bzbw42x",
"1CC9g8bauRPk1Qavei1v8wJuUsu9x39imC",
"1JXpdAKUSTgybd2bBd7AEQvfgASfx2uBVM",
"14ZrPhs2rqwXErs9wpiYNGr5T9d8e5xrZV",
"1Gjh4KBHdpNHaaYTodnYv9cQJdaL8Qny69",
"1KGfQ4Jiy7ZnjTVmRkTnKSA9WZXtD83RDp",
"1Dwp4xugHGaEzetiShCkNhW8gzcmTi5hAq",
"1LHDZQzGWkDJqyRRjgju1VMahNPVJ4yzN4",
"1FpZVdJYsWHb47w3J8hHHH2HAfd2qJyY9e",
"1MwSZLDn6gJRJ2vx8uZ9rGwcEd3eqKRNSd",
"1MRZDi98sEthker35ttnkk9kC1CfKNFQrc",
"1nNuiob7h9hKr5GNZuzHSM8HGw4Y6iB2b",
"1CdKYWNJKht6uctQKfJjwMWHhsssLBiPKs",
"1125kPzenD4Yb1Xz7UdZmYEpFKNA2KmM2X",
"1FQXzhmwBjXhZfYZQRyJEXGzxh5wZR1XvE",
"12kVWCeEZqwZaNHhPFZN81kirGXGTJuDei",
"19FbUzvo5xwrqRMs3uZPaoRsffznp9d75b",
"15vGquUjbTg3qsU4A94XcRnQJtmsZAWpuD",
"1B33Uwfegkqkn9sLJv5Z89CHwfeB4gBT3e",
"1JuQbgdxuCEN3EER7BbNsWX6oC6oXB2a4v",
"1713h5xpxTwMtBxNaS4f6jYHEFxy6mgCBw",
"1FsxZRTxSFD8q1YCYtG3JhtLAoMoiwTkxL",
"1MYsAh2C6YQswX6wRD95bxWx5Gbj79q7x7",
"1NUdnQ4xRvqjNTCihZM2P9zw4idvyCf5G3",
"1FudbhGgrhboJcvNAxWTBnCB2ABfEFsN42",
"1JYzXh9uRvr94hRbUKhpV5GqFg8LdR5zrF",
"12DRrkfauwqKPniKjjA5r2nCmntgoS3MGZ",
"1N5c3qvQtqZfEbqKgJqH2qWq7JUt7j9S5S",
"1ErFNzzKtSazaAJqK6YGuMCVVjGJLcwkK1",
"1CEdMpU7tDE11kqtUqT2hP1cf6VMy5R5KA",
"15DkeYAWaBLQdyGmmTzCRmzwtEEN5XpKsY",
"1CHuV6watqy8GmGY8vk7Z4MW6B97FFqeWj",
"1Lug9EpsAonkvu63XZPCkQxbESNbNwxuor",
"1CK22Cfter1aU6DCZJZ9jK5ddWoCuDjj37",
"1QJzBcF4Sni6AfdAUY5wYZnh2Z947xdMeR",
"1DFGFKf5c7Phzd51twP5USW6yLFRYnZPBC",
"1Ljw75tNQcQfUYejfB91U5sg4vvJTp8A6x",
"1BbrNdMGU5xKG5UJjGeCLoxnZTVeJ6ihC8",
"1enLugw74Gm2KthXfM3Wy1XEBXyLKLies",
"1H8ePEQq1NzGXaAksBYrcKSGd3ZHtHYDqb",
"195DGhtoXqqnk3Fw7ZbBYXLDHJTUHKbrG6",
"12f7NJ2g8DQLVH7ptDs1YrHm6HbNZ4JKwt",
"1MddSHimyA5Ts8jnXNFMnxcp6ypnyWCsa9",
"1H5s8kMryUAApuMv8V3FnQcXoihUvpasz7",
"1MAGR1PB4dgGGW3DkAFxLskUsqXShU78pm",
"1HANZCKPou6v28kHoGXdn6HU2cD4rDcEY4",
"1fwXHGHXnX1tPWuZEC1wsrSGNQxCUFkds",
"17ompMjoKUgpCvQKzFnv9AmB1Y3MaCWrcL",
"1Pjor3o7j3gdVJLmaFmhboCEGEyKtu3bh4",
"1JgaLejKLZpfUUM8caXYontKYWsbx184JW",
"15pVqgtSuq4SJfARwMV2mbHLzeXH8ncgZR",
"1LHYVq7pecrpeDAKxj5d1nTQ1MZMn4oVNH",
"1LYhBu16mXsq3gcmJodkzkUPnBmx3dKcF9",
"15XNFL1b3PaqrMvgguf9WMkYMNyie22LHg",
"1LqZMWdX9SiDe79kTdxz8CthF5xNAh7eDk",
"1ANoZWtYMSWdorKMZMKtRiwQ7QY5hesrqK",
"16kZ5uHnCwRxiE9jAJFvejD18gW5kbPSM",
"19w2fP8gre1wyQdP4hF7WfeSeYcCgWMEXM",
"14asG63tPtZVFX8GG2RwPLpyChjA2LLBVH",
"15HiHVJw1D6Tsrctgq7qpupfSNFahwi6U4",
"19wJesr2mNbjcEi8yBMMhpAtNGEyDuEqD9",
"1A6hKcKVoBev49VZS5KEauaLFdDbs1H7Z4",
"1CossPUMcazo6gMNxftziYUxZBKTo1KkRV",
"1bfn3dvuPraxPsFSf9yjaq3HwMco6w2Hd",
"1DzjNQ6G6eJVN9kVZrWtJnYgCkuPWwAfnf",
"1NAJ8JwEMGQzhp9qr3pf1FrX3fnrmPTpMM",
"1Ac7ouv4M8eSLkJNQWCSHzM2Uj4efdrqod",
"1NyQm5WvVHG36P9oHj2bAZD7czRLATptmY",
"1AsGydB6Mfvo95FAS7cJhYiaGd4mKDRvE3",
"1Fo5UWxRxyzHUdgoxd2FMwQMmCEpcZTZoj",
"1MPKzcmbvzNzbs2252RUELzsvyX28dVcfG",
"1AvB2WVfv2M6E3mnamArcGtnaazy9oshwo",
"1HKZiPmehNChBv3Q5jUQXmd51yy5BHxt63",
"13TSYjjCSm1MDYCBGgbn1FKtw9yaM6NsAW",
"1Fiuk3oTjRLWKUikLtzKDHS9EUHNiwQ1gH",
"1LD6jhpTJg4YRrxbLC8hfKPttsj5VkUSac",
"19KKo5sFPMriK2kTXnm2qmQBn4131AMfPq",
"13sgKoWVd3xx2K78qQha4vEqr6zZz1HM9f",
"1JsXdzZLsYscbhr929J9ipjySRUC5xA1Uj",
"1LsVXSVKBeBppqqtLL1YhJ7hnYjbhNiU3t",
"1297k1Zaw26hyaFLqGmFCV1tGY5ixksZLE",
"12wkjQmmoJW8YyCWUSbo5jdVcPFkCFMsJo",
"1EdTiV8dtKPTeevhjfZdaU7BarBfBFrhod",
"1JuxU5EZfDpdNwCT5dVtcsZVnHBEXDk9Zy",
"1G9ZY1aCX5K9dHXqrBPFDMQgaYNWzB8nWa",
"1Dbv62QXkXkiwdhHGithfcG69zE5WEWrJm",
"1KrVXxoWXvRPwCvuo5Ay8qP1QZnE7emZVq",
"1PGbRqvU2nX5mfncnvXLNntpE7bBCqdHZ",
"1FFRwmuM8QgoBaqHi1H81DNdCjXwRqsADQ",
"16125QM9hmB65MpUsyKgq5T9ZVstoDaRgy",
"1Q9GimmwDyLRkudQwnd1mKG9QefP6qYBxF",
"1M8XeNEsoEu2LhkbQQU8yzuXGRnJGd5hv",
"1H93ZY9wFtdorB2UKW8YnsZVApFERfbWaP",
"1AQU7iDyTk5RzZyL8j5zrpY4uZv4bR2HBY",
"1MSshizyTFFTVhQBDxVnqfgw2SetPjQX3f",
"14jnGVFaEXy92sCb7EXBC23hRCvEbsdL9y",
"14zERi74Vrmzw7iV7En29jRWtyBZta3zqZ",
"1GHitxr86KPp6aBEnKWnc5QfLFyoF5AthE",
"15epgVqVcSkurX8QjqfMwhBhkR36vQuGAM",
"1UP7kka3f631mxTAttQB6wwV16kbUR6QF",
"17AdDiHsY23jMWWLyJjt8eZbVgctsAk7u4",
"1Ej3dgkxpwKGCiQ5rj6vSXFqSrmCXWLjkd",
"121EwWEJTyXHzD5irE8GSQiBVUS6kRPrQU",
"1PHoe7noLYrQ41nRb5Y7N11aKp8CjmHryw",
"1PDncWEJkmfDVUZEsKEgB9fi3j3dXEYEQn",
"1KR7HDVyfwD7GqNexVEt5yz9PtwemmvRgQ",
"1No7iPCCSYfVfHANdD922JZjTt4W8H3SZU",
"13sK55ocrCLrWG1AkQ2g9ipoAHP7fdaWBZ",
"1DjGCWR71AnyCdmagoU23eAzYWwqDWLWEJ",
"18fxFkGJ1eozxgZxTQg4M3sWM7NoP2ZttY",
"1AnF2WfmiwBMawtprE6qvjVvHsVJdFispq",
"14J4W389GRhP41TnR5tkfwQ8Zf6b9goMyV",
"161nLkhGMd1FQTjnqcfFMWoQaSueuvhjww",
"1EVDx8fKgw7No9kdcvnGAbk4T3YXWHTPNU",
"1MYkNGRicVXqMiKX6YtZFAWVkWvRd2FGfB",
"1LXei6RdeJb55g62mckNAJ8wwWbQHUQNE5",
"1EgPsziqTe7jLZ411KvtUwarJcX1XJnLWm",
"15RkzCiBpeQDeABLHTrvzekRCpY34ZB7z1",
"1DMRwaphoqtPpjuhkvYTugeNVCfUHtaCFU",
"1ukYG3Scga1Ekyr8DQEFnzhJ3WrCR6DdV",
"18kYTzfG8xVvYKETsEodx3sP7sybdow1J2",
"1EfNe2WVEnofcMGLiHaQmHSk37CirDkjXJ",
"16ez979fCKD2NdShX7dia5r3HYdnHjvzeF",
"1Pf96RgZexCwnWX98JF9y3vtrDBTjLSpGT",
"1ALGmjTwgZ9dVkwjT1apyXpKfAowxVkGGW",
"1GoxWsJTHafFFUjwVcX8iqiFS2SvHFQHtP",
"16qcFU5aHFMVHmjULrG8yd7bdupYa5gDLb",
"19huHcmSiN5j31ib9AjYJ9pqVhQNrgB4ph",
"16GLiJ2Eoy6W2zV6cAimyLT3N6Zf8Mmdgy",
"1KT9PY477n3rSwmrs27r8jKEqbSHnKSQbE",
"1AvBfHFm1RQh9AdAFfo9SyEbjQ5KfM33pm",
"1Fifc3LjbvPjMWHhean6GU1Xo34ekXPwmQ",
"1L1nnxFMR4w7xDpeW4ynuqGzkfh5X9JVjp",
"17ZE897Nu463gUrHLWMbXTzqMbxwc5bCnm",
"1Etn8dbFYG3nuuG1ZKok2gWh9gN1FZzBg8",
"14tKoqdHL5z9FzPPQL2GiReXK6d1PW4Yzg",
"15cMavkcWaobhY3FVkcSeQjgwRCPFMLJH3",
"1KPRqR98vFRiLzZhkD5NmjdYGGGP38hqvL",
"1PrqPmFp8VxFbRFNqtCcbuCcW23datX9Gg",
"16sL4ryA22HMERQypbeAdEsW8jkp1Kbdan",
"1LY4PekzKrV9idfRz2rQT738T9Tf3rwQPg",
"1DbwcHfwWGdSRyEwb5KewzL4KRfZiYwsRH",
"15UcxKStoApjrtpfntSZXLXsCwiPBx45Xo",
"1DH239M4ECYqYTwN8y4wtbgaQfywKiAPvS",
"12jKCPYevxzEqEuRJd1aKVCy2KWYYvYWHi",
"1J8EpRoH53caaWxVFi7gU8fSnJkXPBr63B",
"16UAyHGLn7eeH15THMJJfgf2sTdzL4WPC3",
"1HxeWrzZYf72d6LRkgk24XQKWy6GT8MUBr",
"1FZ2PVYDaKBBu1ksKYJiJiMxZVXGCttAVc",
"1GMB74VSC6HaZNQ2H2oMiMrEXzciq1iSpR",
"17WMag99HdfTYbDML2u3qRG9ugYxNg2sK3",
"18rqyetH1DZJVVa7tnGZ4G6dwEUyx39G8Q",
"1ABvkSJFNsFnbGuEJhMoLkPL9dg3Gh3aoo",
"1Cim5xr2n7sgZsVNQrLXVDi8BDqPpLoHpE",
"1Q3NRVWh2YzBQkGtjvDYBBx2Ppo94B6pRe",
"1FhVbyVnfpKrqgEntANDGmGFhDLpFVzwXu",
"1MMBtNEtcRiQqHzxmomgWSwMx6k9eEMgUA",
"1MuhEsR47adE1KoBMNfA74T4da71CveQtC",
"157o4S9YfkgzEzmw28XQeTpFY8H5nkPUbm",
"1KLMNaCc68BHf4QzGsF5sNmVmG3emdx8LE",
"1377T7CHbzHE1ygprpk8S1VJo1Nui6bUir",
"14tB5enB7DftYyrQemmR5Rfa1CAFKTbVjA",
"1GybKJhyGuzpRgpzqG1sgynHn8bp88wZjD",
"1JAiT4TxQkTuPVhqKn6t72dak5mXNk4J1d",
"16QpYFv26aL3JFFrFY6rC7C4NdTRvSRuvC",
"1K41ShCT5yXZ3vJmQc5LheGDDcDxexxBgK",
"17BizbjuJkwFK9sHWMrt4drrka24ryzKBN",
"1Nfj3yHMxhCvghhizZjNzK1SUCt9skaqK2",
"1C73PUkqei28F776BaKF2Jr3NsrGb8Jgk5",
"1LHaH6tjcoTxZrNhb2rCYS1oe3NxDGwcHA",
"1JmgRW1EoAyz3Gvq3HSkkEBaNht95eksHT",
"1AGF48HT66ZGQXmtUXwd2QozHHgK2Kj1Zx",
"1G7swx8pFizuut5ZSVAj1D692WpvaGiBJu",
"1Lkf6UJWSqd59fc46gKdjTy3tkNR7PjV63",
"1C8ofQnnfdKNJ52dc9oBae5tyyNrY1cLo1",
"1Apq5ddBfMJtMBnwiHJgc1uZvjyvHieY9X",
"1NwodZHNRZwuhYXoyb5Rahv6TWnzdvaMuQ",
"1Px7Ptnsa1s9qK3AFMxnZ6TN9G2prfa9gj",
"1BFWvJtbMfs5bX7xDL3snNFJVhTo3XWS16",
"18EPjLkoKxZ3beVmtNR9igEFHdo95Upm4W",
"15S96SasXeCFbPWEVtCGHcsFKK7rerFXzj",
"1L3ZnhxMjqg99uBua6bVd4sP6L4K4cNrRx",
"1NqP1qWtyR8gs5yXXd2k89tAQZm1wBJQBL",
"1FxvJzncRh8CSLGyBPd4cUvF5UKwaiq4dn",
"19WzE56nh6sSJ9D1GBsTKDtuJPTWbm2fZL",
"1EDDefH5YdpiFVPoKcxgj2AVwz4SPQ2wsi",
"19bBHktWJSwYqFrhLnSsqcwgHVf2MQ4UMP",
"1ESYrjkPnMMCR7AKULF4D2EtLdBB3SBoTQ",
"1C1ZHMr2Fv2vrQ8tE2LuGX86Ywh9jokacb",
"1KUcyFbQxuVNFnVjU4RU5kf3LkEbxnxXZV",
"16oWZn3i1bEJLBdd1FGzhnjvmoAN8foVQN",
"1HF4Ymycx6iCM8e5RTxUpMoUfcvNyAzX6i",
"1MoQ9a3jtV7iBvNoHpzdvMbQ2d2B7sug8b",
"1GyVAV7MKcpY68BQQ2BFPxRiJ3jLkEWwUd",
"18oKVN7uPtavQpjqHevRjpPm4VG8PNfaGU",
"1LATJ7Y84KppJzXM2PQMutpqwazbJFpYi9",
"12ebVaebB2VRybUmuYjP6okdmMhHLutRfq",
"1DwjZaTf9DU4S3vuCzZ1CqPBUHBujG98d4",
"13fJuToVDMfqziRZXWtcCCCnQBAKBnTKDD",
"1CJ1JqAnpJzVK6C361meRfMVSQpk6V1e7q",
"1AXZPA7GNs22rcTYaHfFps65xt8aZmKovX",
"1JYs1QHMi2cQ3T5vyppN3cfxeXftM9oJTP",
"1ALa4oJKsqnY2aGiutG9bWTGR27K7t4bn6",
"18dVD4g6cFh5wR2TeB4PWXYTijZt8n8sZE",
"1EFYtmAHdKXv7MQLXAYDbTmqB5hEH1KXU1",
"16WsPQZhxRtz9UVvMaWKU69Zv9ngXG57wA",
"19J71m6sY5uXn3Xc8WwQVxErLTpN6KjVVN",
"1s5QFqvPZed2XTiPbAHcPNe5TWmttSHYm",
"1H2jzAxsU93yyGQKLeghU2CGtXLnWbJfe6",
"18cdpmbCNAB8BECExrVFdrZtDLSFTrYGJk",
"1D4vQJPk7732i1Bxp4ApxmAbZD8zoS4skt",
"171HB4HCaXpXpYyHnzLip23Xhbrp5q2iFg",
"1QJUckXmd39jAYcheM8gUihiwcq78w7wFp",
"1FdbudJy1b88V7vDrMoHfzhRBWDpwLTSPF",
"17LN8gD35cR2pCzZDVDyvtLzg88avY4KDR",
"1PJrk5nJyQGzi6rt7aP5q6Edd8RrUJMzAd",
"1A8QXY13ppLE5j22YsJMRHpLwoypMdZSa5",
"1PJT17aMtAHa7fVQ1eqTg1vkqbacnoUg6b",
"17m1crmEhjkEArPVe6Jnc47CZ26BV8jHSR",
"1M4gGmderAehidKfrcewVKRJheHP8Diu8f",
"16ZxXnjEYsMjS1x6khs7auF4czX79iVh4U",
"1B44CpgYcuu7JQuN46SQodjGVP1Czu88kz",
"1F3bnxnX1mJ84hSmdcWhyMqoFyARijhtZR",
"1AwXwanqj5GXJS4pATmgPQiFTD9fHXf6Bv",
"1AnnPMXmUQz11wdGhMs45hTDWSR4H7pssm",
"19xuJ5Fjekxg8iiTpxMjknSJ7iAtw6Lv25",
"1AR5puSR5ZrPLqv4xYDvv6tG3jr8Qaw8a8",
"17CophuitanxD7AviNxL8LE2wmb54vWBda",
"1MooLFnZdV4bjpkYVF9zc2RZ21wyxqrpoV",
"16edAa9q878NdERPEULJbuox4We3UHbcrh",
"1MH1uaAKVJJ3LaTiZm3bSNWt14oo9fQYSZ",
"1Psj8VJTmqbi4wPXR2fjZsF1yfPdNkHdNQ",
"1Jx8ndoREvZnxREnAQt52A3k888SSfL28o",
"1DN6xWShkv1Jf1i6aVb8JC5Yj2NUxcbpPh",
"1a6W9J6pAygQuFWexbVxw1CeX568xFBBo",
"19Q7PsM3VaWsPRttN7zcd8UkuDnj5QZzqy",
"1BuU5HwuTtLZv1NWmU8YysB5wsdRqFEzD2",
"1HPTbjP6nMKGpV6yWJF9zdp3ZSQENtnNmf",
"1E5UM7fB8181pLybtzaH4bfMep3tP4jVGV",
"1HHH7o8PVjxTTmXhYfpcrnp56hJeoS4sSE",
"1A7x277wCBqeob9cwgMYuTVy345HtEVBdd",
"19aSv8hKwQfH4n8ZS1buPrQ3Pneh3Lg1er",
"13cDmq2fNea6kZ81qWCTkmqiwHvtMPRxoy",
"12pE8kZAT8ayXwBSpRUYKZyxDjNvxdgiGj",
"14Xf34JXnFpcByoVDFkYSsi6kdK3VG5ukQ",
"1E9vnT4yrQtN8fs3vVPDNiR588jEMbA9iA",
"19qHLHBTeLovFJc7hbATumNh2d65wMDDG8",
"1HdS7SYB2GieEAy4XS5RnrpLzgLZRnV9GN",
"13k6FL6kcf4i245mPVCQzhikgnPod9ZRLZ",
"12Z18rru8MpZg2Go9VEeCsspsFDd2UTeso",
"1JWdrrZyj9GxmmbsR9ET2Wb9C8CyVtu4fJ",
"12HJojDZaCeCAqiKELFSejxi7VyAAaDeZ9",
"1PJNnuZ59EHjuikyfQGg8XFssdEM8mF3kw",
"17jEhD7mbthuVigrs62n1Grd4998o8dEYB",
"19kr9eje1vPGGnn31tjFoFXdDHsN2fPLbw",
"1Abvtffy8mUUCoxnJiVVjTbPPFMsEgVKmW",
"19TXFsFSEbWzHz6KuEjejp3ziiFgShuppU",
"19x8dNM635K8PmQchSm9XNFgsY3x4zBWNQ",
"13PRT2yGTsd33RTe8hwW8n2NUiZ2Ns9Zt1",
"1JXsAmV7DJEFZuqBQ2desoCM3LDG6NEYT9",
"1UYPUDBBJjj224Fa2HwHmKEfdYHztQm33",
"1KyzpG3UozpQrHthssH3njqEpyV8pB7Cgg",
"1J9uEM43BYMrVQzQfwLtbs98a2M5Sqj3bp",
"15Av8KWPafxnEpmh1fZCoeEnYj7dR7Rswm",
"12WCEVgg718iRpUwrpFMLvWKtaeQpm9Hj7",
"1KNK8J3xyNDJ3pJUSEjN1FB2PN61bZr4V2",
"1KjVR6pmMM6N3ycQ531ggSM7Tvd6ieoLPH",
"1NTDdhWPr89XcB8mFVHUm4JpDg7RvJYjyb",
"1KMRobfuKa86ErEPgNdj4FpdWwBr9VUYhq",
"1Afz3gLwxoxaUc9kdt5ejxiCP1hQyaceQD",
"157EpZULgQzSFkWN3BVVkNji5ma2fwQeJD",
"1L5RR6eyd2qahbFisKXUjF2a7TEkRoDH9K",
"1MUgA2gky7PMtQbGdzxnSSoFEurw6VXqts",
"1CLohYqA7DA4Bj7SJKKsKVKDGvg5nRpgDZ",
"16nvFquFjzEQdK6NV7ixGgoDL7oXxhmaj7",
"1CNANqzWhK7GYiyJnVivtVuU2B84ihm4Jj",
"18PvR9CNGbtCAZFMjuhAUEAn31xRgZNeKb",
"12NwKqdhV9LLCZavZ9iNVMiL26DBhRLaN5",
"15GWvbtdwxEFKRAeA1MqePkzUSjd15qXvW",
"1Kmdr1DeeWmWrXwG4TVZcZxpFuXBn7UMGQ",
"1N2VNyhTHCTZAA96YymcAkhTj52JiCkQ5v",
"1AiUcRnQLuEwE6nxkV87eCPznCxrreKtn1",
"12XJyk7CfZg9Dw6qcDJpZUo7u97E1mGDxH",
"1DQFKWofQxbR1ohkR5hSesXKFdJ2aHB3rW",
"1NBAWcAMpj9Z9nMvrRN9v87wShFBgmGGxz",
"1B7CxecosdWRtYE1fDnGbJz2vQBJ4Ni8Gw",
"18HKmKiaqUQ7uTDKFCLkCpEf3QFuwS47JY",
"12mvZPP7oMcMrHhY7qrgJWoFaF1M2D8wha",
"16gYDhenkSDHqLKY3DtTD5amyXrbz24NNJ",
"1N6fUzop58hStzwNinKL3npZhuDBaWuQcF",
"1NW5vAiSRu4LMhrTEKxoUbV255DNyTWYXJ",
"17Jp6XxGAwFcWhgRRe4C7AA1nBADAfqVyS",
"155BKThsnk86zTdnB7vRa5C37MoNY6szbV",
"1JB5J5YaUNvwFSRUYA8dQxKwp157nTEJkC",
"1BASgUsY5NKhcpvZjJHKKpfvPErmXgCFY1",
"15DDwEPdXqNbPNvspGEZr1vMVMytbkrFvX",
"17Yk1fkCUgXwoctRCtrRXgfq4rKKiwfpbr",
"1JZDvzNbQzQN3ihXJf19rLJzhEZzb2yGGo",
"1H7dyAtZ9jjixXaYPmiJv8juR18pP6yMbs",
"16U7eWZiQhSn7MXFDYjthsHwXPTZ5w8ShT",
"1GQkMccEYmPHyo35QtGmyYYLezToMH2np8",
"17VxbfPwuus4K8U31nr2WiSGoDiR4SABN6",
"1MRNEY6cwmutu56i5xa5hpt9nscnWDvwaj",
"1Y1gEEjFByn9g5pZzYxaWtpi7rR1mLQ1u",
"1PPmBDoQn4QW8svLWeueUQckdSsWWKt6nV",
"1ARxajkrWhfh5oe8sHrDwsAvoUrNptiyC5",
"1CQDm8pxB2Be5Hjg8bn4Rg79Ui1XkLma8z",
"1ANuL2fJp4PoLWCGXZUCQDh9b1eoehEHvH",
"1DeRfmj5ydquCWSYAsopZAcvX6CiX2YgRS",
"1BtL4E4dnSBF8fw9LnonpiNQa7bAAcrx53",
"1LWccyRT13SVQz46U4fagMcePLPBNSiYFw",
"1E2U7RBvwTwvFz1wPqAn8A8KogefVjYibb",
"1Cs3Ajd5G1Fv4LtQx8qeWZFqfTDdHEWhiS",
"1FAuTx1Xw37mcR3SBtUoDPvMuj1F6BnGBK",
"1ENZx26fswhErSbaAyq5MFXuo7RPJdazXv",
"16khNrM7vg4chHU2aVmiFNw6V3VXzdxdEB",
"15YQNJs8htEZv6vETWnDmP64dzAjNM3wyR",
"12SmE9a2RtppDBf6faF6dBpg6YfDxSwBfi",
"18RwCXid5bKUfYVynWrcCepbwEft9YBU79",
"1eFWAisVT4PZdkN8f5c48yYbM4czcf88X",
"1YL4A1rCNeXzmhYV8FxfeASSVYXXcEjyB",
"1J44WAmmM6pxeqqE869K8S6yRbsovx2ddy",
"1PN8SwQ8fSXxWjvF3YKtyqPEa4kJVTDgxv",
"198QBmMr41dCFUK8fiXaZe6Z7GVjAJ19T",
"18Wbmqm8iHRkSw3x3VB2WTrBbECa8jSuDt",
"1K6tV5dEURBtoESdd6LhKHbkoaoYGPuojj",
"1QLJNAvNXNETnMDqDiUGK281H8kC4ZBEf3",
"1JBJvcqf7Mz2CrpdrdUa5yaySsNY6Wbh4a",
"1GDqvkYJ5Kz1S7MsEvH1HDHMwkoopztvVT",
"1A6R3tbqdPhrAC7mvHXFdunXR6U6w2LfTC",
"17zdSYrBhCAQ87Hjr47mprSy7FwQudhvsy",
"14GpT1dKf6F7Ssxyx3eXM3AwVQ2RWjSiBo",
"1D4bf4yMHePd6CfSdJQMCjm9Rib975JNUc",
"1JFH1dfxpJafTroW7ZGDaebVzxKVX8uBJk",
"1AsPVhZZLcCmzUweVg5hDowSmGXE4G7U12",
"18ySTee8bPqtwhZRjkuXUrGMCchceBvR6u",
"1JC76mxujvfPGrRiADsvisEYNz1NcK1FE8",
"14LwTQoqmzf3ZSbjXGgfyMZzjur7foD9Kn",
"1NQSDDgyH2mQiSteBtt3eeJwmCqChgFbKH",
"1EPFDMLYE5x2UqGbK91hXPA5p4PDTpm2R4",
"1KYr1yeCkHVguVgY8hhXWrnVLd5V44LUQH",
"12UYtwbouFPod7rFKyMKsGXCHKfc3MXQCT",
"1PEsjtY9nTcVQ5h4ZauhT6JZB96F42fhbZ",
"1BgFDemaJMXDVeF3fDs8gFdaaagkN8kCUT",
"1NPVuFBTJoxUeph6EDRbzc29iKfU44rxzk",
"1JhWsJ3G8K57ezgpkQvwsHySCwF1LEdYBX",
"1LHNhm1BwE5MPKkVUANQG9HQEhqgySN8NM",
"13puvkE5p4xAPs2NMr7ViYThAc37NMFQAy",
"1GSg4UK27hBJLKvbfqk6XBmtqtRup7NQrz",
"1K9dpuKUeSGeyG1beJue9KL1TTexNXSk1C",
"1JcoCBimVs43EE8hy3YDdepic39sxDLoXF",
"16tFrHmYaXecYJRe1297YnoYmhfrQxshEL",
"1Q26bMu5ygu4gnwVjhSroBKbNAEL2rkrGR",
"14PqCPCzfdj5BHioUsi9qWUouSRdYEf43p",
"16atGmLA4pbDgM3Go1K3m9T4bsiBgBooZw",
"18UbvCGULSywZ8dHkHsZwPLXquhiTHURHa",
"1KSeU7n1tKEgfbkHPCt2KkWdEcXML5xh7s",
"15voBkwS7Xed2VVkiJ55nrXe5PJBJtcwDK",
"16tCmNNByot9UFPLZhngj71Ne5fH4wzMnM",
"1JFiRkSwGGhojYy9EnMfzvTkaDvhLviSR6",
"1DsbuNvFHttGB2i8uF2PQng1bSpoYLePbH",
"1LMij4xUKDX1gFgpYDfMKoEVdgviNYcK3A",
"1Q7pje4sA3TypHCpS3RwHumQUqWdc1diV1",
"1PqLCoSxQHRH2kHw9g3A7pjBNDMenxpCSW",
"1EpfXc95BhLqayG3pTq2UHp97HFBDezkAj",
"16HoL51Fwrk9bVyxYUh8ZVJhE1WH9qscbM",
"19Zik7XiNCR6V8nfTzV9Fv6ZuiEydmPFFT",
"19RjGux2qw6Ke4NVwoge1u42RYUyosTx7r",
"16P6JfmXDzRVJXSdkqzXkLF87HhdrFqfmn",
"1GP4URRjPpDWRvt22sXnLFWyND1jEoDdCy",
"1LUiu4RnEbTGS9TizJHC5BFJ6PjiEwi6L2",
"13PSxpicNRf2YgjiGiH8xmGJV1q1MPF1Vw",
"1HT7aPH3hCoiGqedBpVrziBK5AKgKb9Ajh",
"1M5HQKiBYTr9d6t6kyETfcyfRg1L7tYfUz",
"13KRrSucKftnc8dfRSh4ziuy2qpigkNS6U",
"13wA6JnzjEcz7VzAvfpQET2MygKHyJLwJi",
"1GzGsLXSo6QRXS7W3tcU4egXpBbkbRLXcq",
"1AFyd4vC5HmpvVW7aHNmW2eXhZKvWKAWS3",
"13fBCH4k4PjFzdHmLre9AYL6T9N9EQHBtr",
"1HRvHABC7DZGjHV55nyibcj68KUhuX9Wnf",
"1PToNjZE4kEj5UWSasRX6tQanT7NDs9e6f",
"16DrGFHwuKtVQdzimJKSrKzKWJ4ksGPLUd",
"1H1FmAyeBU7noQyBdixXPNcXk1zD5Ec2x9",
"1JkwayxNKsVaytWUdT2KpsELasKEppmYFN",
"1FCpyxvent72pt6wfspUbZ8SAj8ahLmNMK",
"1CqF8GkCQbgnovau9seDchU8QRbmksbxoN",
"1KoFkNJKvMxasRzd7kb4rZdeYUeHEx9Zid",
"196x1ZerdGe31seGAXWeFy4CHZ6SFkM1xL",
"15N8SJGwJzSYfY4hL3HN5PKgwQzWNc1P1G",
"1CpMZQfXGEnLMZS2HYWUkGnCV1fjGu5qvF",
"1CfiVL7tPxBzHfJGZm13TzU78uuAAERTx3",
"17HWbDTPNze9jnST7HJSMX2zpaTH3Z4C7t",
"13tf81TWrjx3YqMfptffY8eius8zGAL3yP",
"1GGmGFjKESWnF5QCULBiw1ZFZCJF4yGahh",
"1PMoQQmWNrwHvtagS8mQA3mpVhzTrKP8z4",
"17xRm5vyMTP68hWmPeShhvbCFux9oz1GQp",
"1Dasmqd5UKZuZrMmY9G2Vvuwyzz5WKorZJ",
"12qN7Yw5jCSxkjiyqUw8udj8kvMY4XS6Cg",
"1HRt3zjHK89cB2AVFsHoNYSKM4sUVtw1G8",
"12pVrzm6AjzmCZtTemCtqJJon3n9Bt95gu",
"1PTa3YmPLcW4aTEJit2N7f23MvP7BV1idB",
"144CrCn8hMc8zKyAJ8cP9QQykGUijeuVLE",
"19BLn5Yhq3CG5MNwSVmjb269T36vsxwxh1",
"1BbyqfFreXvgZfcNtpYMtpd1EuDWyH11p5",
"1FfUPct7sVjnDyus9d2hSHjCKis3SRYXZ6",
"19ZtuiMY69pprtrgbdwFd16SVFHptphe44",
"147oZjm2ALnXh31wuwK34RBpGZgDMq1RDb",
"1Eigmzo2okoSF58DgHVzF68QP1yrSTbVom",
"16FYPmKd9vWCnrWxG8umLsyLB5xff75Y1o",
"16pgrYiSjHkqo6xytN6uGU8zvaxwwJ3yFt",
"17cDLdpZPZZz6fYSPp8TDjekDGUn3GXewD",
"1KsY8dcEwdKwqrLjeEz2EQS6CnWWZazrJB",
"1CnXppeSTXYz2kdD1cKhTVBwAmKRQRuccW",
"1GNsWsyThJbxq9qowDdwY1ekyiBaEejxXS",
"12uaV1EsRtkVRErBu4JqqtuGHdyGzyv5u7",
"1LyfH7yA38NDoDkoEgYxaf7XjqBLtvxbjh",
"1Bs55gdQ5t97gjzDc51AZUnP9YBsGAiwEd",
"1JB2yPCFzvHvCyKpSFeZckFQwE6LZAR9QZ",
"1JCn6MATq6gBnMKjDQ51pPEJpqXGfn7d5E",
"1GVxVkAxuxCbbgQftUbshPDER9n8BRtjUL",
"171xbXi4hvnzsQyQcLQR5uKLfQ9RezeaGX",
"19CA8twraDMNaMgRzb1CucS6etUDJyZvsP",
"1AJcdkfpwdeaatfNaRvxmo8c2gisrhLRzq",
"1PuGFfMhoA5f8qs5ZpK6h1ndpzyDGWXyTs",
"13xZJLwDhTvFTABgA2EAAKzMZna4ma3hge",
"1KJNzDUS16Xs7KsP2C3nzXehyAFegYiRX",
"1JHKZJaicsXYRwCb3hfic8fDeiAihujuH9",
"1Ms7V2DByHernh31dLXZs9xAVMmghC7HNm",
"1Cc5G1KE6EtFMdtVermiV6USmSCqmXK5BG",
"1LxDNbrFFXtzTFKYW1wZi49V4Jje5KD2v8",
"1JeRXuHjnmPXGabViEcy62tuCPK3kKMiCa",
"1JJafKbL6mZE1cGbn1E3SpZN386BfXFyfW",
"14SZTBCuepPbmfcv3zkKCB7RerE1WaU84n",
"1BVAhMf4B4HgRRJ1SUwjJFgjVFn7zxAgX",
"1EYpduzH6uqb8jvwaSea2YiDYYwUKQjWvT",
"1Fe2fADuuNHDvPi4VKFErpCwqBBdtdRjsf",
"1C3iYfUyaEJ3QaLaqdAprVg4PFvPaVxk9s",
"1G63AtgCCKokLbMqRiE4vYUH28NAqKQGyF",
"19tAKR61vX64TwaNztvCMG1diCupCPFAL4",
"12fCfWQsQRm38AZHRMJ3ELgkAvj9TUrWGf",
"1BRmk9sjyDCBnDeubCmtgaqCaM8V7PPubq",
"1812uVUyB6AYHPeGeAJU73WV4p14bnNAJA",
"1BMAHf9GhDmVafqnpfbqKj22BzPEcmKQkr",
"1KCYweNjeLvZLTzFoJ9ameUYvHJe9q2axn",
"1GfYVe18nugi3Ytf2YUUmGxkpxRhZcguGS",
"1NYE5JJEEMVqwHt9XFG2gjaunL94JW5JHf",
"1LxcHGkNnAv5yu1P6XT7S7m7Gx6qEZPcdr",
"1DoV12xaYqYGexakFxoLpPvSa7piY3feHS",
"13U4kAtjWnEtniEHGm7RnfLYnT39y3fgA9",
"13BMUrLRQW7dXgYovEfgNo1H9LWbo9BAZ8",
"1PH6FMyAAwxy8ttJc6wd5JZTJa3DF74GUP",
"1Fv2ykUhmSHyKJtgq4GAjHNVfAkTW67X6R",
"18sKDh6LnR9nfNPXgsWZdmdaDTncgiTTbg",
"1GxhPZytEaSVZArVhshNQEGw3vfiH5p3h6",
"13EsdQFjbLv6ghbsuNKPpbm1aPc8dPK78T",
"17czdyaLgACQTJQeBxXjAbHh2yvkzj5PSX",
"1AKjZePaDQLAAd6ryj3KYMJSY1fmjBffXx",
"1G8yeDS2UNFzwWUJmawammNzALgaURmu8A",
"15tfjQGYHpwGoLUAD6sBnueVrkDUEQ3avo",
"1C2VjpdrvvknaHRwEKQZrz3YySo37QecZw",
"1EXvdYMKfwGfc2omfPdEQ7GRXpHbrCyCdn",
"14UFGbiSAK5fxvDuFmCsLvK5aRLFBJBVzr",
"1H2TdQZY7E6YDFNRtHQVaeQG66YLshqXWT",
"1CfSj6RAm1ayzGYhP9VB9XkB3HXxpb7NYx",
"1Ar1ySupxHAiD9GgVVh2EQcRcAnFTPPHQ3",
"1B51afrBXo1UVofEUMgZpYGcfvMXkLMUXS",
"17amYW1DhC3GNssEBNKbCakeSSpSf9jXu2",
"1HwFu9bbCV7rZNCHDYZCQrx5BQ65womDV5",
"1Jfa2bk8xSaGBNx6WrARnejKPPNHWDpYvi",
"1KCywvZ7RMZgBnQXv6k9PaYLgNbuRMbGP5",
"1J7z6P9GxAfAwHPPEKEZxKbF5mYJ259SkH",
"1B9vfwD1swrWgKktHqDmtFDRsdxoGBXCDy",
"19VVoj42DfWZtaHs3aZMMCmk9z843pwqPY",
"1CZPpdAbSWgTL2FWkurgAvAiVoz56iD1d4",
"1L87NNzcXTXUrbqVN2oxX7srKyijZfiy8W",
"16RakGqh3c1ADJtzBrKQzX7rfM9f52cRDM",
"16umFp2JBcAW1ba4kkaTt8ehhaWQYm1vgb",
"1LdXNHpjQgCUgcyXzZU6K5PX429aKsW8EL",
"13xZwTmgvRLNuC8HyXFUh1TnTWuGFjS45P",
"12rmNALnmSDoYCiF3guYPc5whFnMhf56gM",
"15hzk8Mar2EmeS9m5qWCXD8euYFiX5ANhu",
"12ye6wRhg6xhTjadrVoHqiG9Z269ngBW9R",
"1NU85DhwwVGYxdBxB9fpV3UfRPTikdAnG1",
"16TnDBwXhMbK9qnf1DwwT3fr6djytCUqKM",
"18gjbRygixTX3qN58CmowbbTRcgQRr9RsT",
"1B617SbAXJkorzzK5QPCZhRkMHhUD14N5N",
"14frkArgPuTZgG5Dq4WGPBm5zi7eRTZXAM",
"1R1X9EyffdcnrHCfqpgL2MnjsYBVsma92",
"1D3BYKKW6ACiaxxAjx9iLwABaZqMDfrVMa",
"1FVgmrWhZNdwandQdcLQGgK2nxQjJ2BJN4",
"14JnaHXzHBYcJtUPCRjd94LzR5xiME37gQ",
"1EBuRocc1r23L2QaUSKxQNBsbzMzs8kVbW",
"1r4QNGnpMP2TW3DqKKUEMvKSoZ9k1Go9A",
"1GYNhs8SqfzWb4B2eK31FKL2K9HbK83kP7",
"1EWghQHzvY6PDSj5PejKarFyHj5RfPEhqw",
"1D8wAjhBbzMokrfp8df2C2GsEcwbMoeFgk",
"1NzDEiktUu1s44zddAUQB1xexumSWMUc7X",
"1KH4hc9FaaojBN9d9X7kSRotKaHdYbtKqv",
"13ARzRBbZSgAKvLsWVkMF94KGjUJwozhxD",
"1P8dEKapP3jDMCEvb9L1eaLZ6oLZ2jXxXu",
"1BkruVVxrCXwU5pZnipAXm12v5wH9rkS4k",
"1HqByhnpos9CDatqeeQuxxmWSJnocNLubN",
"1CVZQnMcpzhqStKugfeSnhRE7GewQHhWmX",
"1Q31fTvzHYaViwYg92wwUFAtEX2uQFgBWY",
"1G9G6r2QhjSwfu9p6MkJQCTXABBHpMBqN9",
"1DfvDzcQXJQ3HiapdKo5sWugs6nrdjYk2z",
"18HUwQAd6mys9PbmDw1UuaSPkTBYQe5fxn",
"15aQo5geCfwyQbzzjFBUe9nLzWBbSdARGQ",
"1JVVHvbgnYsWAwkx9yebtzKxRTdmXjbkB2",
"1LysyJ8cTiswSwtYLBPHkhbven8oAhnZcJ",
"1LwqbLRzsB8rnsC6Qu7zYvG5vNrZA6Lur4",
"1Fho3uDJL9TyobdV4WoEXh9MmjXNzhMrNS",
"195y3ovtGyhc7mFktDPW7qNEqpknWTCNQV",
"13pQgrBAMPVUTzb7zGxp6jucadnSjuCaJY",
"1G1thxCDnHMabj1Pv6SMK2AagF8mN3Fd2c",
"171umug2ytBnr7KNyyciLJx5Jbj7oRPGcS",
"11MU8Np9VvDaMtJdbc74QWetJXSuWQzo8",
"15iVNcZsKboo6z59WESQZ3BpWptpKZ9zTt",
"1Q2XhYkb47wzxeo4cMy5RjjwFU9FtuYdnP",
"14cj53daD3guM1kkk1JQh5ACzaS2aA5h6L",
"1NpopKQtgaM8uKYoiwmA9hFVNd2JcgGSSt",
"1E41re3QrJAmobRJF7hfJLKu9GxYuD3pr1",
"12AqCkyRYRPaJtjitMJBVQMhyZPhmhxkQA",
"1MzHo5ZrFtBPTL74KV16PyAESKX1Pw4uxj",
"1JPCwaDmoUnAnL3WBonK2is8Y7R1Eqg4Hm",
"1E1hs6mSfqJXJ5RuMJTZourULNejsQTPj4",
"1BBRAoH2BgBgLrDFmd74vQnnWM5LyMZwWd",
"1A7wWh4JSSX3HNbd3kS4raF5R1EvViBTPk",
"1HPVbVzPpCTH1ZCHWrD6MDSQhD7LPFwuEV",
"1ApRy6dS3Hi7zdhGwe6V6gyCnFBYURXdyH",
"1Ntc1Hmp67dwNXK4CNJopZDYx2XWN1nb1y",
"17AMReZAjrFLFg3QogD55ah8FMUg6jspXe",
"16wnxgYxRAAq8zMXYmAQ2EeuTfgdEY3t6z",
"155W7fdCVR2MTStSjypm2g68JP8EiMsSCc",
"19ng12Pom2fovJbyuxjb5Mw3B21uugDXDX",
"13KkEmybhM1CPCAkH1ZFR8QQoR5ns7TF3f",
"17vFs1mL8cagN5o9vLz8s76b5B8TprQkeM",
"145Tj4Kxpjrz6qScRhexNEtFBejNfKfrbW",
"12XDxmrDgmjvLhLq1qXG6ZfLZUs85eqmFB",
"17THjLcQsq6U3e6hAqTJVMjE9rU6wD4TLD",
"1EhGVPuAeqvDKtLvN5DryRoMEjztnXZSjS",
"19CwQJAPPnXXvFsKT6aFzEywZBLhocNsqv",
"1PhSYv3kE8TRUAV4F9XZsyzE8amAX95uDC",
"1CenHKmhAGdscuxuu6JFt5d5wzx6VE5Gf5",
"13r1PgSGSqShSLppMtY3HVSbRguF6d8UJE",
"14YiAxCTbuogyQoSrpsStGTENyW6aADGH2",
"1F6HTRfGAGDXcjWq6i38MQrEeMQCeaCUr7",
"1NPiL4qnA926582wJXDe8G2aiknhmCB7U2",
"16M4X7u5o7T87rtTak97DwkGLA2Bh6vJku",
"1C1ZggTXK7v2hxmhUbXqwQCZmsAQxbcdn3",
"1M7pAnGXyEWaoHmccChvqGD5DRFGQhkLB1",
"1NPkAyZas6XLn5UcV1rQEQKrMYFnFtdVHg",
"12Z6ZcPwFFNkjfoVBkniGG2qCGCd3ffTtJ",
"19UzFetFGcKTX3bkSsFTuNq7koR4Gm9E6g",
"1LYbsCsoqB2bfrTmUT7eh2vQRVj2s6Zpxv",
"171iBKTRciwAg4yZwp2jdVJ8D5zZvA6dFR",
"1Jqrg2a9pp5bsJQZsKJ4E1iqdnzyLeAox1",
"1Bc9FnfUXXuCMCz71uD5i7RWFJm1xb5kLq",
"14JXuotDBkKYHTPq18WZ5JZNLeo1kvXEfG",
"1BmjgZpbTbWnDGUHQ9Spi5g7mehbCSgVkp",
"1PdEb8cedUC3PeEdhoufiLShPj8VbGer1j",
"17S6BuDTG3dTh1JtsP7VTsu33itxs7SfTY",
"18heYEYohr3vtCFacKoyKkv6fqenNDj5qW",
"1LHuy4ikWSRHmGBtoD8drU84KC4PLcohBA",
"15RjtdAbfoswccoHrCTmQjHMZroA6Xg1E1",
"1HeaukKaSQTX4r4aLSyaXrM8Nf4oFooJMV",
"1AeBo9q3kcLtTahWddWbdqEiP7e2vHWsDp",
"147RbwhhVQ9AfmNf2uKsGWznTVtQdCuiKW",
"12pfCvZyS2EwkrDJRbLnACBgMKFGuM3ggw",
"124xhQNQrz4dHua94PWRZFbSRjUENePVzQ",
"1FdRoSKqTdQjuirpQ8uG6HBbumfQRXYCKT",
"18284phaPc8vMaR5SaMrnpuoswjW3k3VhH",
"1KcjkA3ijnkwa8TdSq55pkFa3YSeiVtfxY",
"1EV5MeSCPPz36G9n3rt1AAPhP2AYTh7KsP",
"1KsZEsNzJ8nrrso6XUbTvSWEXPTBDNrXBV",
"16pRB8yW1TLthtTKrU6nzeEKwzNyAAg9bh",
"1PYuS1Z6LvfE1CLRnisgfj5uhXvhFXfF1o",
"1FNXDYQ33VY3hHB3yShgDpxX3VPMP3CHbU",
"1GP9qdak53jybG3qPAdzNEYcD5VK2Czbku",
"1Co1kXGJEZLBmQk8r8NPJ6dHzpnA1zSCiU",
"18CR6jmJeHP6yix9GxiXgRUxkVfHKfbVxY",
"14bevMxEBKsmSYmvA2AXdow9xCXSnhUapb",
"1NWCRyEGSGoquWdL1UWGXCJt2WQ4GPoQou",
"1CkMKTo8KXgsP3j6eL4rcWUKGdUf5KR5wa",
"1FDmTToegEqz5RtWTpszs7PqRtwrGHs6Wb",
"1L8raMiwU7bRXASJwytFx1Ah5HTCsuZUmQ",
"1DCyks8qB4VGQw93xLNKXEUo1wxfB4DgQs",
"1HHKATNZ4g1JodZAPBt1Ya9zgNrzzB75U",
"1JVL3zeHn6PxarrSbHGMaRz5V2fABS3sJk",
"1BG1Eo4o8PnysgcYBc8CiRdM4VQAybceL1",
"1F52aZbympchk5rbuXgWbRsZ1JQwXue42Y",
"1EDVHfDeUttzE8CHsx25z9aJ7fz15PkhWa",
"1EqGDMULDt47PiJPvJJJVt6oFbP9KLAN6J",
"1EUcK3bNMfNp3jGY8fk7AY2Wj1XeHbqA91",
"1MVMZpGM3UpyLU8YDzQTWeAuP9vtKookGz",
"1BpkwMwUizyaMVWVJNxniai4HnH6HqGVBf",
"1DHKNvKGfQFQJCySJiq9njiC9p2fSTYWaR",
"12it2vkYJrSnJxnDKJcKcuWEP1X2wgacyV",
"1E7hmFeFFHAVdbBzSAE7x4wT4X9GrUpY1r",
"18xUEmUN9skhSJYv5DBRYQUhtiZwJVoHJs",
"1BqFnu59ieZXMNwzTGMFnR9DvNh71H4gkt",
"19sDGHQ39hnVPvGv2GHkEzrV1V9mJLaM7p",
"1vWRNqQr1DSPTzRVE2i7NDy6MoMPkhid4",
"195a3Qix4FvfBvKZCeDsvJJVJ9BQBDruZM",
"1QEj6jRXgL95jPBXgHHLBdjVoRWFJScMsA",
"17p1eBXrMoQ4BwFs7yo43whUxZMzLZx4Ug",
"1JwFqYyJoNAseBeg4qcGMPfsR8z5Bu6umi",
"19ovw81ujBkUu3ouMZLjKbx9bnK2L841pu",
"19tpY4NEsFJNLY9Yj7gXpLKkKRRWiRsGAg",
"1DPM2dFgDko1dEfUEqN9UgebSzBtr9FHZf",
"1KLPEU53cnYuRs527M1wmFJHUdVjTkn6Vv",
"1BDFEeABpPVNNJ2qRfrhN3dBKMTq3DYnxW",
"1Ffz4s6cHd1qBxvkinHHv99h93VUgJn3NB",
"12yAH2puq7SM1xAvBvXJthHxDxtwaCbwaM",
"16eGTevBgHPKCoqDMpMYRMQtaKLmbSARPM",
"14SKn4EgnxhATobXPuathkKid2Vvcvy3eF",
"1448ndvnnvfepddng3bKsscQfe7MPoQ3CM",
"15bEN4mZK6H2HRuqgt3uhmPxpApNK8vwdv",
"1J1MGdaoqJrArVMJ3wSrmMKcB1LZLbg2RN",
"1CuZU2QWcew3RDiqZsttKzvgiMF6LtBEug",
"1Lohoym9xXvmP5nZHYpEZ7Jt9wA8c2FnQb",
"1GqxMF6ExJ5KpoYN7BS6P4khJEbUtdQWhw",
"1CwCeBJDwRkZ9NZx7xnwjyriMGGHk4TT2e",
"13SvbZXCGm6qJxbVY8iVFoLW588JH5UzcY",
"1GQMW9NhbsC9askpAc6HCP4JmtRy9zUFKq",
"1BsebemdyxWids8YFRPjsSjiEANniZDvNz",
"14TG7x1sgxM5eyAetRR5LjN8giqkwVNkcs",
"1NC8CagTXiguAVLozjZnQp4rw62eSbDwHi",
"15o2rxqqGHJLf7vKNAiPSN13DU69jaYSLY",
"143oxG5kXgCvHdA7JKmesMQhVD6CdE9u1k",
"1MzDuYQqiPxAdYrCfnSpk2GaPSWBE6UmgU",
"1EYbwd7wwwvV2Crcz4qLTphX7FTxMa5hVF",
"17sYxWgtNuvSxxaiC9ev2tuYYsFfpTPaMK",
"19aqT2UbofweEb9neRqKhzRM4cSjntzjQH",
"1N7TPN25UytTv2icD59eGKpSXGT5sGYSnC",
"16gpZPDKNFKo1VRkWqqB1QNTkt5Ls4iKbT",
"12SDpucSf4j1U4Wk7CijgZe88KZRPSfmDW",
"1VdCCPAKBwJVzc9UqMQ2cHQK11ynXV22r",
"1QAqzcx4VW8ceK5VhWBRUe4ymEjje9gaiY",
"1CzpMXTYD9B1mnVzMdeGDzNfCz8BtQDTio",
"1Dud7jzqJo2hSByRLfEH6vVYtdY7xVPNwV",
"1KGH2zDTotQKrV8TVcFWUKRt6X3C5uDFNw",
"1NSCL3CtznDpZZEoLiUg9LipxwswYf4k5m",
"16ctz2zb6pmwVmyJYpfCtjqtoh2PBzZNBZ",
"19qANa2EnvMTiXDLUeX35Qj5TSwiTMVZ8Z",
"1oMVkogoWdhfz3zRJ2fC3YbxrtgcKrYUJ",
"1FiBr1ftMjRdMeHm3yZ8SxwDYebYnnxGFq",
"1AedZEgDCk4gcGtoWJXapTT83dsR3iq6Zd",
"1556ppeksrWhbDLo5bx9wdSEL6JeLQZujh",
"15sPZvoyJThBcVH3VxFnrt5Y4YWtYPCJ3T",
"1ASbvwbru3k8qPc1UfmRbUujXJPSn3hvyP",
"1EyKhypWJB2Wsf54YJzUH5SJDmoiAxSZzY",
"14QBSouW1XN8WBr5x8SpRgoveXHXVbRiwY",
"1EA9iAXiXRYMxpx7f3Sv6KQuW8g1ft9wCE",
"1PHegiEfNRPY6AmjxyJ7LA2JENE2sABUXc",
"1LBFtk4uUB5h4i8i5Cf4Mg3MTZ2n2t4HD9",
"1JYNWGxeeBrY9nbQYXtgXPTbXtHsUFsXhY",
"1KuTqCzAnjKXEVcpsUnQs2pVtrcdjDn5Uo",
"12Vooyi9bLhGZ1QnEMSXLtEBmvSisoHAds",
"113LU7FJKPYTkyxkgQZ4RDD574TEbAdPXD",
"13zXWsLDvF2DKt34cuPFrzqTEN1oDEQuUU",
"1Krqh1UaadpWYo33NuPauMTrzfPT62Swoz",
"1GbHydPvTh3DtJZpbpm9xrTNowhSbov8CX",
"13MiRbRJTkV5i4n5aXoLMzxtvgFi15B1SJ",
"13Yg4PH4G8cQCNtWUHjk1iMaS6PrSGnUQh",
"19ue4rh7jzwZamCJ3RkDSrA8j1CUJJEBvV",
"16ML9hWgNUtcQTB99KyCdK6AS735dw85M1",
"1Hct9PkpoQb1KXWLSQRB1Ez21iQTSmvKPz",
"1933mMGnQfWeBSSPgrYPnnmaFrYa1EVKYT",
"1H3w6BBrQG7C3GFYXfVt36sTmG8JyCCeWr",
"1Gz4PqL1hmNNbzPYhtXiBcaibB9V12MfM1",
"1BoRfbcJa1vT14EVSJdJjH6zLUuaPq6paa",
"14fQZnU16jUTnSN4RBbvW4vduYxVtyDBks",
"1KjnRriP2BrWtyHPtGMwFPhiXmBP3fvugK",
"1PNJa635Z5Ao1rUbTJbFMFVdZSLkrvUsdE",
"1Bai9EaSwsuD9RAms4VbxupqkqAcVMG4ev",
"15qLxWUF4sUWDGtVE1a63L9RHyH3wUcPyf",
"1HsWx7SDu6f9ozafsfJg4wGrhdw1BwY6ZK",
"1BDhyZgNPNV9RfpSeiPWAQrFEMhsXzJkfZ",
"17mu46bYKckdD3FrYvLZmpQayrdkxYj4HB",
"16Kz9MxNvzXBEK2jVgry4FWDhdjYS77mBw",
"14ZMYVr7aYMm3wQyLXXFTpT7gYYcBrwDD5",
"1AWwxRSDS5gUT9iGQiL7sATUjCWZydvMAq",
"1PKLpujrgs2xLy92n2oeALBhi1jjFBMEPT",
"1JNrn6vNAXp6sFsnUcYiNYrk2p15kMiA17",
"1BUtEd1KkrNoEiBiyTdLyj3UEco89FE429",
"1J82hkLoH3TgH272znwYrNeWGeyB5wUbtg",
"1Jsjt7v2ZXZLuvUNBo6S3JvJwfVnvcC3KT",
"1EmM1RC86zHir7d9zKzmqtDVUVc3h1BiVy",
"1LtyTuSFMu9H1RzfEKExdy7YyRenVaZvSc",
"18RpVH2JD7yixCamcyKhB85joDQBprGMWU",
"1Ei3RWZvCy3bS1B36HwfxwYFt9x8m1tLFY",
"1HAnAzhf8vy4sXCn59MenXAzm3MtUmqr2J",
"1FbbZEBHDTUhU5hGP4tnpu7S85vq6quQtk",
"1CBsuw9B7EKYfBUdwaV4PmYvNyMWjGdhYu",
"1PGMpZypajYh5erfdHb83ELoAKiRh4tD1a",
"115AfEFxXgioRXBZguS9f2BJf9jf2mrFQz",
"1FcKbDW7nXerCrtz38MsjcqfNJhreuM3AA",
"1KuwZgvqJGP2wp7rmkda5w1ycDe6XqTh2P",
"19YmfZEgALMtDSVYtR7MZ6ZPBYtPKuDypu",
"16BHpDoseNpZ24t8qocNPHk7ZHCdxGiuFq",
"1PcZoDqhtBLgdKUAWs245Y1KgbUjcd9VmR",
"1L7JMxTgrYmbzqK2psw3C5L27o7T1sAJsp",
"1JVTtcXoFSxhi2c4FRMY6ftcp3Eh6QyYZz",
"1BJ3vBKUDKHFquVdb1GgqFUZNEpTfNToez",
"1G6JEbjc8X8RaWJ4K8AR5p66Ypg1vq9vpo",
"1AgtjmYQEbE2QTizME11gdH9wvkVuh8ez9",
"12Ah87AzJeQqbkUcNLZVYxM5V36qt6ZXZV",
"1A7niQZPaBD8FA1f8eqpc9FwZJuM7YCvXU",
"1J4Xw16LEzzmGJNkhTVnkKTTeBVHtYTPt",
"17YyWDH95y3qzWYXYCGWgUWJ2tKUMh5JPE",
"1Hmt1ETHFasywtgMNo4gy5G45zfe1Pi6qt",
"1LPB8WJynG5uF12HJD87SF5SpsZTrxoxXM",
"19CPFndpcSzh24ja84j9sUnku6QYFnxJsy",
"13GmnxSWJFfwp1QTRVG8aQm5wyrAboWRwp",
"1F2mQBQYbE1C7ASJU8TYUZtd23mc69is35",
"1CMJTbHLo6kHbXAv3U7qR1NAZPxR7s3LSH",
"1HriRA1daoEottGJqDqe8BsLgCxD5cLMjL",
"1KiCURk88p7Wx6TBHmokTYw2RiMfWb8upQ",
"14XBTFyQkiByNDyYrhoPJndHWLa8aREZVx",
"1HN91WqEDWKy5Ad4JkHopaTaDJcmadyp9v",
"1MAP5U7Eics5xfgp4m9T2MwywC63rzmShU",
"1DxWEZ9MmmjHZAQpto6iDLcHoVpL3bU3y6",
"1HijKcbvZaczfkjuCfrb7BrkBrakj2YSLD",
"1DwqvBWjgHdB3df8EbtDqiW9RC6Ykv1FxS",
"15nCFn29WzuCiYF9u5cjMGkymxBH35ipTD",
"1McWb3wJhj9LwnxUFQj4JtPWcBnWsj1PjB",
"1MsUqmVCebrG9SqVtBgxLTE7HdJ8RqZGzc",
"12VLYdxqu7wstNEh6T1PTWQc1n8WEXc71e",
"1Kc6t7WnZ6HV3QrQMnbZYgxQ4JsfyFzQE1",
"1JUk67rmgYAJH5ScbfDcThcjbNN24qJKBt",
"1D7CaCFsjiH8GiZH9NYa9GRFvCi8Cbqt7",
"19aJ2GZfZ7Rw2R193ZE66bcFUsPtYVDTLm",
"1Heq5Aj2NkfwySzhShF1e3nkTLTvHjmngy",
"1CnrT3UymvAouvV3aZvpDorwHtDrjVdTbM",
"1NxDTh4zdqUVFYKALowAqD7tWrzjBKfGww",
"1BRC83qp2hMUbf7EweaWxEKBQSGSKBoKsL",
"189e75U73MEf285RQ1RmXttCTzDNCkp23f",
"1Hey9rFtL89oWC6ZtdsBw5ALorhMg2xUX1",
"1PaCoaaWXNCWELfzxpg5wZBqCwAGEDNBz5",
"1K1aX7PLnsRouMLWCuME1VaPrqm3M9LAqw",
"1MxvxusHkAmc8XqKNyr2SKdgvVoCMkoxYn",
"13kSoPAk5xh862BdwB4JtXgaXPnHLB8xCc",
"1FS7ybPF5VfesxWADPYZe55NapToP8HRNN",
"15Dvg6chsfEUc62PPqzxueNUPbMTJy311r",
"16pxscMXrCAZJ8DadKcj9oyjLpnTSiCa31",
"1F9yMWNpAPU6NuU82jxnzYa1gfqjTWHrFQ",
"1GeDV3x2je4Wj65d6zXLr4984hzeSsyoTL",
"129bQhG1TJuvrkMeJoCzar7vY1u911hAnP",
"159P5N8QVJ78WzfUxUGxKEa6fCuk8JpdKp",
"1Ms7ivPZTjtFoEJuDNKy6QzcvcLBfnn2UJ",
"12sCsZNxG4EJaSNfDk9Cnf8W6PAsAk4WTq",
"1B7bWvDRU8zJoGgMVUtB8mgNJ5VNuXUitz",
"1GwYTQyuHrWYtLLzHc5E3HvGpurAPS9MX",
"1AeqqxDN5GMc3KTnbz692VRritFpQ2UtVj",
"19SGWorbbJ9DEdh9oJiaQhHUkCus8aA7du",
"144n87wzfpB3o95gY5YpBAASrjueKHWBsK",
"13cbga9fPCFmzFZBpumfrwq72UMrR9X471",
"1Jnhr5wDywmvvcr75aoMPjRuyPbCkVXrAk",
"15EPy7EZzHGRkciGbC4ZtqGCFQZmK4pMr1",
"1ErKx3YvHrXCryyMvfRpFyWCNTddyRzkDv",
"1PsFgWe6M4QpY2FjFGNQ8DV7xa6bdAwNXo",
"1HEeqq87NAQXEufqAQL4C3X1KwTT8TNRTV",
"1LrSXpvr345TZokCM2zHxgjYWHbogw6TGQ",
"1B3gKE9vwPWXDAcjpqiA3Af9huZoRz3Tug",
"1zz3GQ4XMwhQqc4MqA64kbCB9m4pSLNwp",
"1JCyEu8XhCruaHgn71q7ZFse8WwVQQSKAm",
"1DVAgZNKikjJTrVbXgeAaMckhn6pYhhe6r",
"1CnE5Y6giobB8NWK9WmU5TNW553m31QSAa",
"1EnQk7ModrZSUydsCiPS8A6ZWoACriskhu",
"1GDqNBKGPSjNxCtk7P7JftG3TbfNWe87Uv",
"14mehiNbwavaWzmRdPmEQrjMSQSpguyBvT",
"15sndiWxkqSoZkhUCD5erfZnp7d71xS7ND",
"19Jpk9jZ1sR8eeoEWhtnPH1Aq3gp83xBA4",
"1CN7fthBwpqEWgSf4qFmXtdocDRAYpS1QY",
"16NiCVEAf7hepiAhe46mwh6JroxEcvFBiN",
"1CTvT8Dj8iJ7RCo4iVyE6y5qpa62btVq3u",
"1CsRfQrYTHZX1vVJZnjUcqmfN4JWEekhsP",
"1KnfFQKSVriDCv4jtDGQPzwQ8WqNbxaFNn",
"1HBHcwRGpx5amaXMAernSu39oemWhhKVVx",
"1BBzzkzVdBu6a3aT6X4opsvEuaofF6H9R1",
"1JLow3qosSzvXL5csBkWsrfNnJfo1Rqqvx",
"17nTXvDkvoPUQ7uo7A1stGZuFpqkhQipU7",
"19b5v7P9KxeoYt9HPgHpFtFi7pWHdYht8Y",
"19k6gunZ2YBZNdkCZtjShy2K953FwbLMZU",
"1732XtcqkqWSCMsmtHe2MsTYKERF6iVgB3",
"1Jqivake7WXYgjGzpkZqzLhNUTNVhnUVWi",
"17xLnfPGWmk35DCguSiwDmfcer49ohcR3E",
"1TogkK1SSx4ZUUCZLHnVTFuKzFQW8Bmxr",
"1CqNtUCiLBFQi3uQnWS8MWDeyUqapC3pkv",
"19vw9Ryd7a6a31Z4cLBem4WpfZfkvpbDRJ",
"1C9eGzZWbTfEkGTVBFxfXJFuXU9ye11Kg5",
"18an4cFxZaQZK1x4AiSsD8ytdMUnVBvQBS",
"17z5octkVoNEqfWH1PSDuRPX9FrrZ3NRWd",
"1E5oSmKikrqn6LeSpoLDcPPrw3QC67aaoK",
"15vYc4SqzzHvjLAHZ8LPghGRuCfAFKTWyZ",
"1AYHygJDN5qvyG6sRNwJywqnh4DXDYLzjx",
"12fNigWg7K13piHhR7niVXqm3dcoXjB98S",
"1F5LXUsZQaKwa2Z23uhA7DKH6gboHEES5B",
"1JwooH2wMPBDWBfTMa2oBW7p8vjyoBGrmk",
"15gFEtr7pjcepv3JWet4bmWvCxLDf6FQQz",
"1Gfn7kqp54wyyUowh3jWBnFkFYYB4RCwyE",
"1H3YsduZL5waDw9idS8xgNgvTeuSmKDp8x",
"18ZDALCE2wf4hoJwiMsZWX3CTBC3FNef1T",
"1LAfDkvRKxzmJESQyGh18mNH4wwzXTundu",
"1APbTFiMkQsMhKa7MQ7n6UyBR7qKwpnqZN",
"14DvQ4HCsd3j47bmd8NqsNa6nZxL4oDq2C",
"15aAdmFDME7fCTuegbYswxrJvwcG1HAfqE",
"1CbBm698aaPvKGb9nUKehJn17gJ3CmYSmK",
"1HDVRJ3xtifC496RywZZd2sPNP3u2xQtQc",
"1Rgx9SxWAhrxpX1gGTm6BMVRMw8zUJL2G",
"1JwanCPiXTte4UPfYAahGCkU7pp2j7mxD8",
"1NcjpkDh2C5LkAcpkpPFPUdHhDAwibjU62",
"156cXJUnCuhk1ZNWxK5WnRmzjSFQZLkCDF",
"1CfX6rPjJiUqjRN6cjMxoK6K3M48kEC7Tw",
"1Cszsmwn6WYeinwkjcGR6ze8S8n7sYcCLR",
"14ActBAguSkZfgctTz5VZsPYxpH5Cfdm8Y",
"16SdUtTxdyo64A4h95hZRrZv7kKsRBDHhY",
"1Q6FXFSUbmyLzFrXQDeLsT24PwZMLxj9Yq",
"13LznbZs8SkmE3JMBjsi1qmKBhGXVeB5LM",
"17fuPMsek2XdKLPctsxuTSujqeXyAGZ79e",
"1Nvw5XV9LpVErq22XqFsoehJCyNHdf5BBZ",
"1Dic78CSdreNogHQFzuk5BSAugHCQsD8VQ",
"1E6Z4LGqSNYQfEsC3xUmrsEZhYf6yTWzKJ",
"1GmuEj2Qt8qZVkBTaxed7uvUzAj6qgLxJR",
"1NipnqhBnEDf44hB3XYAbn622kx2JzTdPM",
"1CFhTbhBopuCgbgLhd9oJ5H7fZyQdgiRYQ",
"116EjpmBXU27SkN65yMKBGUVrz65vbeRDz",
"1FTnm8HxrbhXnsePaghTbtjVA4oA6RDWZh",
"1PfArKiLiyZ9udWU8YXdQVgqW6QvTudyrx",
"1KXZ1Vi5Sw1KtM2CY1R4A3sBXFoAtaeCFP",
"1H1m12YcVyc8UYW4kH53zvRYf2zETwWoiE",
"1DFDuQSNfBidDgGvDFwermEHuLaZmEq33P",
"17SkbSHpQbhJNcw8ozPJU9ozk6wQrgbVko",
"18ymYZsbuMyCTJraEcvwwWDkHZtYf8JDmo",
"17RYxGNXVvWSNw7oYTPQeeD9j1FZZaHB1G",
"14tbnExp8pMqxpHDy84gacYPSGQ1gSUFCA",
"1wPFZCiyJGR3vgUx2dAaCwxN8oNAN91Ru",
"1AmvsUAtkvPswxm8nwShAEFDjuK8zcNi1m",
"19xaPSUFYehY28i8kwRhwhEUHXBSzmnjhj",
"13GqD3CiHmrUAvZFfFjrxRttxdg9Kwisq7",
"1BkFz4b82x3TzYGofR1CaVSNVtan3YYcg3",
"1MV6pPNVhsMXKHJWqgEC9K6xovjMeB1gQM",
"1HAcvW9GdbDGq1AnznxrMHPFUoGbKLpmnY",
"1HJQ9pXug9F5Vg8GUo5CQH3s8sHi6TuV6L",
"1J3qBsb8pY6DScDHHkHAkidD2fHJjpSZ6h",
"1EK7NK4UdcKEpUKPbM9agjP3yFzJins1Wg",
"1B7H1698mHivfhSQNye8VuMzyCFjQLtrg",
"1PYkaR44vuQcoJowPKZtAUG1fK1X136inx",
"1Fzj8EbkVZS9xbnEw6GSsPFwGaULd6F4fi",
"15bpDmf1RUjj6kwMyFVtxsWxpgYbmW3AwD",
"16LM54dp2WyhajbN9UsQPg9WkEswua5rgZ",
"1GpdUiMRa8cW9UDTLJLjsd4GYizjivXQRb",
"13Dy24gAeUVMhMzeuZCTQKVQtm1kpZgRbB",
"12v7gJS5K2tKvHSE4NdQ1PpmdWDMJkguN3",
"158XrTSDXfQ8SGYBn6W6UqS7eVnBYfuo8y",
"1PvKZpWPgqosWiKMJfWBgq9AEgbAqtxv8Y",
"1P53R18KkihXE63weJnhRRbPuksS5pVyEy",
"1KzxNi5J9qQfznLfgRgpVJL1Rh8vY8RU33",
"1F6f9deiR4Diaz41tqHkiFRWdLCoYK8oMH",
"19FnWC3pW9sAFosuphF861cT9wMHhCGKCL",
"1C9MH9pqJh11uSvzWmCZzvJju6C2JpghDM",
"1DdVibP5mnhv3MC9YXATC1bKkSuQ1Seng9",
"1Nio3Fhu8hbfmdfrmCrzexH1wHHekbSFY",
"17wRphocP4R8FXEmMSzAVjinYk2NmhzGtS",
"1Nn5XfdrHfczKB34iXehfD2Q3PHXBPcrao",
"1E1fPkJ1xknGTjeC3TdSAsK3RhAH64K1ML",
"14uee5LZQH8kY6GTwfUrWKcSu8P1ker4xy",
"17qnudWHuwnqXL4wd8G9EMgaCLCycUZtAb",
"19WMWaUkogSqtyyB3qyYBgzPGtowPpqkbF",
"1CYfNLi6XgW6XaM3adDPuZapzhn114KsJQ",
"1NqC7wA2aCiR9Tg6HvydPHrFEgZDJbyp5G",
"1PotTBGJeo9aHaxYspW9ocpqNoNuBBABny",
"13sReLscs5gWuMeLCCEMSRQvGLcXRDZcT6",
"15SVoWdMUZf9rYVqy22e9UucneauBLnh2R",
"17DjhaNzB3CfXHsWYo2ybF2kVe77AKxhNM",
"19echuTDs8qbr98dXqMDB5KEmjEpoKfjzk",
"1YdozDsttUE6p1LQDsrhcYW35dzMcbE5x",
"1Ncf1cYAZ44oXfqAuNTWiWE9egDLe5huwR",
"1GcCR2n6omvHLnaa2ohdiWeCAcqxhxHhvj",
"1GbZq77ZT6xptpZPdkk3B9X5Ei1BRx2Tk",
"1J4pWRcoUEBpebA7weRY1jZ4AqSr2Kqttt",
"1ArETW7rq8bQGsUqLMjuHBbk5bKKwy4Vef",
"1L8itsxwV1pgog9N7FYBw44u8dEyRaNdyn",
"1LWd9yr9zBPPkCWeFkKaaLYQFQe9VJHApv",
"124bDMderDAcTU9BAB7HPYh8F8sbh4LtN7",
"187TyJ5Wi5AdQcPSWaUzzBd3C2wBKyM8oh",
"12ef4CeZc4YLK9JKsgt5FqX4KjBixAgHhL",
"14TzBd5yGXpZcs76rUaMYtUNjxVyPbJyeq",
"1KndseE9ACaD5iqTXcgqj4CPxPtF822XD8",
"1JZkxMS6omyysXBissNy5Uxu4woMp7kutW",
"1CrZF8s74K4u6vS5UyAkVvsjrvTirSCGzM",
"17RS8J2AdgDKW2JnsAcx4R24hi9SfGoW6H",
"1AqeVBBEpx9PUiChs2P3XtBHSNpy9kpHcB",
"16APtdYSi1Y3PsZxjJBdEqXHGaQ8Fyi4pp",
"15Fi7VWLUT9aDYHJerek5fPQpWQY8kcJ4E",
"19ehPGnpkAee9VANmLoR5Zv2dXR1Zh2ebU",
"1Gx56jKz2gzJsRjS52hWNjQQDqfpDmjyKF",
"1N4RJmmApCok8uorGihkFyJx6ELvZEBGCC",
"1GxkDPQXL7FG9JNSFuSK4w2CRmtN9FCSrF",
"1Bg2xUQXaCg9bWsZAA64GSd672QGWdkR95",
"1Mn7HNTi7mCk3qAhKkgFCvyQNb6vgnuUr9",
"1Ly9ZZTFkHzG6Agd9dkZ1x58owQvENUbkK",
"1H3m6U2L7XhrriDfNqVtmV69gMfZ5BXcVY",
"1BDYMzec4yF2eD8UdmGjiR4rggV7aj9Xck",
"1PcUoBiHai8W8PopbvbTyjJZ9tQBSqEUZ",
"15q18PCS5N2wfsx4aRtA8GkPEnksrix6ed",
"12vjxXwzRRacMaJBRzvpSmXC3zKp8gF3tZ",
"15fBdV4gF1A6MGj9D8XF3RemSFgftKPhD7",
"1KhTYo2ud7Kfs6oYGcjRNmVXvzB3CQMjGK",
"1EQRhBgpTcyBA6mFyiXtGLeorctGGmr9XY",
"1ETgoaVpb1zoCmCgg8AUNYuf28zZhwZnsM",
"18SsNufGY9W5xmRr7MXJ7sD41TyxtK8KDt",
"15utBsCcq69YHFiqLPhKcVLzEPmQjW62T1",
"1JMezHCVwzfXA2uJBVdXSe4577pwxDsqyL",
"18BZz9DzCJ4nQcVP7Esq1UjaGzLoLjKSHm",
"14NpdDwYyKfAqN2UWcj8AB4Li6UQwtosV6",
"1HaR7nnpnajm7RrL3SvaZ3nPnKoqQnvoed",
"12ZnJ6kWNiaA6xUH9kmceNeUcbSXha6NvH",
"12Kjr6gQadnUhFJjBgt7bXAm1EsmpBYVja",
"1LjmhhmUEJ9iwSd2ao2xzGkAedTie58thf",
"1GqovY4MFbaxKoPnqCjo11DvvCbQPyU9a5",
"13C4sPVvtq8nVTHxYNpoUCCcDadyysraMo",
"1951NnRZkPJ8BHyf1cUnADgnxwakExAAtP",
"1PiusinFSqaBMs1ibZbNuytK4D1CcNf5Hx",
"1N4edtycqzUYhbmHcNtNCNyNSumNCsWRcm",
"1LaBCwWvoWJxpsLiYNXRig1ta7TuYgqm8k",
"1Dpkgv9MTzTDJT5Hck28PCoGxRUsSeSfK",
"14vgZctrz9onDgp3Zrk3EZZtXatmK1111H",
"13WmeMhYeaMLufeWY28b7Cr5RzVd2qU4Dv",
"1Ef3RfmKMdHkA13T4r1wcCkPn8CdPm51S1",
"1HGamanDSSZpTRobpL1YQJzpdrWqTHkTtA",
"16HFLrDohoFLihDz5iPdwXyyrSKywye96g",
"1MPh9AEvohrXHHSP9je4okQFq2YaS1YPmG",
"1EvN7bgb1eMDqUVrj1yFfuZ2FE5uUQoQ2R",
"15LsDRqEmuK1t37KtDLYVi1BZqciCjc9hV",
"1GZbvqh9bGVRDypArVERCFMgaPQQUEzTf5",
"15d57Gp4bu8A1NVU1wpxm1ZKx4QDqfE69r",
"1DiyTTeCffAnh8wtnH4QLaMZYjc8ciFjvu",
"1d9uVVpk8gU26DN8PxEuDf1DCvX2PYnvG",
"1L5cXNsNGSqCS1PCyj9Sxd21uZwfABJHMM",
"1GhWTaMsh6brJGgb6drfqG24DnEoDyz85r",
"1Mae23tcseCoBWy3MhgiejLVb7w9LZL2BN",
"1DFDHQi3q7K1rvKgqzUNrqhXgQ4z3TAgSo",
"1sM9jF4dmhasjXqnr1vuYP32tLot4FtH2",
"16JyxEn6VP8wrtQFW7EKk8HprYdEVs9wbK",
"16fsJVk6YRTQjteLkvBWPRt5YrQj9jDouj",
"1MPApvD4bV9QZ5fLkPpH2Dt9A2LMUk2m2k",
"17CPDCZNm4bsUyinR3UEc1oRut91LgEQ7",
"1LG8avGfrJ5gzcwgsTyy5x3Dvqcdvwg85K",
"1EcTGRfxmfgutQ81xcgdZcByQF3FZzbFKC",
"18sYJbxHS7ypPHt933ifZSi4uCMYaXkQz7",
"19dZ3jtQbJdvvUQ6gD82nhHkVcgPnpz6xs",
"1MinehWiafRyu1tgtbRahXZY2SSDMCj42q",
"1LwgofmmUx3nLgFyFuKtY9MaGXKcZKLcPp",
"12MpNErgPQc2mdat7ur2iik6w2QK3VW3R9",
"18wcJGheSjySG7soxjous2TU2nP9A3WD2D",
"1AT5JMeVZfFZRcz5PnSJLZa7RwbWL1RzDY",
"1HJNjNxgXjyYc8HuLfx4B1iNcokZ2oo8nP",
"131GHi6aHyDBH3qsquThGT4Nbax7QG945u",
"19Q4Q1TtE5BdttrbxSBsM7hDB56FymKuR",
"189SkH9jVGXXCyf2TWpQQoE5YCe5MuMBc7",
"17ae5GkNF3HJvdpLteJxJ7zBmiiwhgiRPi",
"1A5uSJyPkyg4CB9c6Zbtezws16XNQJQP4h",
"1Jxce7M1nvE6mmzbiSWnKx6yyodxEmmUQa",
"1KWi8dJ6JBAJsQjq9WjVRqFp8oq6vXoQyE",
"1Q7jJtpYxhJyYXHegwwfyN4GsjMBz4sknk",
"15QJ62SAEJVrVBjCVWgoy5ZFagAHYfJaXD",
"1HKVo1dvVd55YJ4yqRHvXn7Ufaki5LtqTJ",
"1PrSky4Tfz7KgzFjfvKA4dMhRjcGXk1Twg",
"12xb1rAGD7D5iSL2PuoNMK6GdxGk68D9pW",
"1BpKdodEt7vcRVbZkZVs4Z6mYPsZBNnGDV",
"1DAB8PFKkhSeGwYZJBA3suB2pWJQQDxJP9",
"18VQcmkrjKrtsKcbD6juJQXqRr1A6CNNvF",
"1MaKzHqmPNuMeTd2BfCg7p3fMfEN1hVtnL",
"1KeA65DSU6b8H4VCV7CXYsszKCxVUeC2oi",
"1MSq8BbnAE2vA8sL9S7tqyVN8He4Q27ukk",
"18fseCp7owXBNJ6k5JbuphmKqmXdDTMrVF",
"1C5wL3McnaubJN1RBzdx7TNTpx77MwGzGC",
"16kNP2LTURooo4ckW1mHFPW1pYfV6GYyHh",
"16Q8q64B2qmJYL2WFuEeGFwu9H6zn7CoYj",
"1BJm8kWQfCYbepoXtBXdNWTUE6ScopQ48x",
"13TdbNHYLAHh7gAQqYdnZyzYTCfFLJcJ65",
"1VcefJqqCFWoNXLJ14UfXCsVy3Bd36hxR",
"14WEsUQSt4V1msKY3vqQSbzXQiWjah3wTc",
"1Kow5TEX1vqY9Rz3d1b2RgyMGpM5QYiu28",
"1NBzeWf4q53XLF34oKMiJrzRnzbiF4Khr8",
"1N6DmGprH7q9xqqMoF2xZJtsGcosRRV8BP",
"1BSyVCWpwje177cyxYQHmV3BKJc61LNCzR",
"14erQJmK8hCCb6jJWZ5GcZNAwUjdporRXw",
"1JpSfa5VQAyXaoNXe2hCDodF48jB4Xqiu7",
"1CKsAHAbbemVfH1iNxTR7zys5mUL8f5EfJ",
"1CmsZSs4PdEd2zA4ZbuxBF8qhP8EChwsC3",
"1HtL8hMMXFXYTYhr2KQfwrP7gpxc5yXBS1",
"13vPirJhzL9sVZJia5zmewRvSWS3Hh1eoy",
"1CAQvoaFMdZzTPRhFjzTRfu7X94ZRH6o7G",
"12Mi7ULPZWzfQpNEEPhp6r6PHvV1fTcK8h",
"1NAzkpu3LwXzH4SHzTWYGB4Cxk8Lnt2NGe",
"1FgdeVvnV1GhcN1kxD5Xgtnh8Gft6pkEc3",
"1BG8HtEw9gHPCnhroNzdmJ2ynrMyKwkdDF",
"19CegxS2KhbCdzUkZTeXCSjTxDAzj8pS1K",
"19xobEULbtS9YvMgZvrz9GHWnNmbiF1Wjq",
"1Q16anx9aQsnfmBCT3cwnNWn51XRwc5y3G",
"1EiSmC88r4ZFu1PyMjrBsuXKorn7YBXAGN",
"1B52CoAsdB5z5vLv5JiWEiZHWGuAYygy5u",
"1Pa6aM2C8wX4PiHQx1onUx2BUQWrTmd7QN",
"1EWtB8E3PbDhC1Ux2R1Qe1B7YB23X3NA6A",
"1BQnQMDwekSJUPMR8Fpjh9WiR8Y5ERGtBJ",
"1QDdgF9rTnWVBLBwLudmbm8N8bZvnM5Bts",
"1HneoJGEVLDuUMYuFpM2xfPsGRT3FisP4f",
"1Fe7XQAuemL5QKvdtWB3QpHndiJzjX1JnA",
"1LKHFXMA81CXGFw348t1qwxHPLWTZqPGfz",
"13QSF8DqQbFXi6GUbXL9RmUMshQKzFwgyn",
"18BZSwdjSsCH8jcK6kqkajaZEkXDVdMtqb",
"15M7KTSmUaptds21KxSz3H81RMxWwfDdf8",
"1LUtwpmvqnmY4argL89ssmf3Q1v5RJZT6f",
"1LB5uK2N2Tf2BNPiyESeC5bnfgCtSDPAab",
"1CsaTgFry67BqCmmKopA6zAVHRjbuuwKPP",
"1M3es2DcBeKjtWBTXkMiSEXbvsFuYBKLSQ",
"19ynfi91nqmXTX251jZ8EfEd1fnrY7fXGF",
"1GEmg7Fb2r1fJNKEgnP34dvyYeAZALEq3W",
"1MMLbZXJnyYwycjDJimFeUBFhbzersGhbA",
"1Km5Mn9NiE3DW18bR86vZj85XDmLitSBWg",
"1A6tkeeZvSbwZFVJP9aML5Gx3t2JC5Gf42",
"19VLkCo6ojMFjtZcTpijqwXQzT2S1RJEY4",
"19qhaBZa2Xo5zjBsDdxn75VNQLgNK6CCLD",
"1JR9jDtWxkH7Waj4of4JBnCFJE2rtHMzpj",
"1229znh9vGiXUx1xfXr6eg17LUBsvjifdT",
"12geyFsfGBFhf3aPkfUoextbAzRdsR73Rt",
"1LGaVtminMhF2E4MzpyXkYxdYPRZV8VkED",
"1MJdmCvWGV4CoovwYAMLcCKCZSVZNUfzcc",
"18XmnchT579vmu13Yp211bWumxghoBGomW",
"13XKsLrMd2dz5RWz5J1rd6JtRyZEvWHrjy",
"1GaRqLNK45q3Vk76qeraxhvf19TyvKriN4",
"1E93bzHpLxjJ5AAYt3ifMpKWT8Gy5UUGqq",
"18vwqcEL8Ke13Br3o8RSFNjezuK78kre6Q",
"1KTm2iaQmj2hgW66xHnLz73M68yGBczd3i",
"1HkvyfdxDxs6b2C7tVntywL6m7smoGVMZ6",
"1M6JbDxqDTnPRDJigue4qciJYQQGYDem68",
"1Ko16wWZRgRnz9cBc8EN3e1zMsfxybF4gn",
"16hTz4ftBaYzwCAfMNbDAT3C4FThrwsB24",
"1Hoqfo1geb5tby9KG1qqC6iNmz8VdFvBoe",
"12Wmi8ACK5Dyh4Q1WVbrTBZwnChsSav1Cv",
"127ES2rJ2u7d3w5Fnozp2nffab2tPWwcjm",
"1DT77q9tD2FgQY1VbpDonGf5BZWB1HgtA2",
"1PkdedmD1WVWi7vfjdrrot8G1DX1wz8Jno",
"15kdtzebHbWVKK1q2TdLZ7FBn25tPmryLt",
"1LrNaHe9x7qS73m2guYSLhzjEqZmtD786E",
"1Cqysz1xeLgERdAEvhtKyRGbms4rzZ5mja",
"16dPQQtpgFS6wzZbZWva2kJJ1247GagAyC",
"1JwtJSd77P7DCM194fVSE6kvrV72sfvpX2",
"12U4teWEgyvH6HdTne3gRPioept276mrKH",
"14yVcGYn8btDHpaiTBf5T1ranc9HNeAYiM",
"15kU5F9Yz8ST498Jz47DVk4nzAXgn63c42",
"13wG7VQAL8Mab7v2diXcgquTQ6VtvH83fY",
"1EdWEjNpuYzWEFxw7tuCv8hGgHrgzciiau",
"1BP3RU5cQL83d3J8chiqS1cm5WRJbiyV26",
"1NaSmXkHg6kSQtdoPApFoknmzx2LsxXZYd",
"16wjEiDVoYPwuM8yzFuoCvC3C5sgNXoHa8",
"1FokCd6vrQJ3SsCXxW1tnv5AFPmYfg9G7a",
"1ARkCUMizNmQu77KKKWyRLcKEfnyCrE6rr",
"1ChtNRCw84iqi9AgdLfajsTsxgfvFS7Y9o",
"1BofbiA26Hh3UgHDBCFnFfPrgi1hh7CuJP",
"142U2TESHGXcL3Jp1RCoKNT2Tk5n3aLx8H",
"1HRuJK38SbeznGThtpPMUasjHBLp1FpR6Y",
"1D8gCNNGAKsEkT5q2C3GRQSMe2hbt35x4H",
"1KYwUZvXW9a5zKZkXr9yMSTMvUbA3GeSJ3",
"1F3duv4J8vCsWfZTMjAPTQm2Q2zmMtf1qs",
"1H2AdjfoQtAFdRjcm79nksFCz1MmQVzHfS",
"1FrGZS4bE7kjUNDN9DjhADt1SAuBCJDRxG",
"15S9YhnA9tpL5dCwX9tnNL3bVLQYSXX1xz",
"17RS1DevwLDYnemM5YtqAT2H25HGzgeifq",
"1NizQpNjwpw6wdv2H9J2Hk9pizg91ExYdr",
"16PMd8UC1LvzFFu5oiWZs2bgW1YFFiAk67",
"19cqf8tRSQ37AiN36gqpF6SKrW4WU93N2i",
"1F4YHfm3eLtDAPknnQnnSznidKKM7A9j4H",
"1B4YqzTDfGQF1c9rEneJSptZfi4vqWLq25",
"134jWDRLZd5ARhDppQRXssgSmr6ada46rv",
"17swLqsgcGTBXekHhUfJKHwzsugz3FFRXS",
"17zfqDsfgF5qPp3GRAK1SCQmk2rBM6TqsS",
"1JoboqPLHp7cEEQSskVJNvWG4UKMT3jQ6w",
"1L3XypaoyJeHx5cn4YZwNxDgX7MhGsDs98",
"1HxzgzyiNB9LrJjrevmBB3ymozRgnTHZJN",
"1KkCTsZ8xZFeRhRv7L2YUpXUGgmsQrf5LX",
"1MfeEij2YPbjrX3CmFYE8RaaGgVirn7bAh",
"15ZWNZju8oMHTpok6RrRsRToc1EYeRrDZm",
"1B3tYbPWexP8aHoAPeF8uiJgrDc6FwU4TK",
"1Anwximhp4Y2kk7An6UTnJNiyc5ezxhu5z",
"12auu6DUxUn67E4gNayc194tCzJn3RU7eQ",
"1HMW1dwv6excgMFadGJz2M3tVAX2Qczhrm",
"1GCH5iMt9bCXwLAMaCTw1Dt1t9WCEHKj2U",
"1Jtx6uvwo4sxP5Q6hga4UUyphdjisNc3Dx",
"1DGxXwzNETYXssK4H2TGHtzLbG197qsXim",
"16Ec2hSCdfkXu2X4mgqkYiPjMq8ENj8iYG",
"19Rnf7F8eUXWorLCHeV3m5qSnpfGURzfvU",
"16dtrRGMcnz6XwcUhABywq2J5c1xGk9x1C",
"1DcbmsE1rhtHRhb5yAGKv2DpvBzqcV721k",
"12awLpWFK8jCN2SxUKJfjQgGQkNuW8462p",
"1A1Kptk74KnVVi8NNY46BNmFsv8bEgAB3b",
"1CMn7j8QwRMoWWh2EXbMHmTnvfyxnKGHjb",
"1793XSxUbqMzXi2Dr17sV2YHHUQFm6NBym",
"1GxZBXevxjgTfUpMZZpDR7iGfGZtVozS18",
"1GBCvjAJxw5aC2SS3JKGuL492G7eiB1mCp",
"1KktZAjTimfpVW2pKwtLJHboSuunExREPV",
"1C7n4msJ3EwnTG6TWe7u4P7D9Udegq8LE8",
"19J9nMgMyG259oxt2JE7zLWc4Gd8fV1kBz",
"1F4RAe3e5s2pQvpm6JoBbDpUU83HF9M6eM",
"12sY9q2KqBs3rahjyNDmMJ47t5PLEtFygM",
"1EBHV4PLBPgP5aVGApMkzCmhgCG9qAPY82",
"1LM2N6vsLrZDPZSBZyc8m2297rLfFmwHqc",
"1KCxRACwaBFsCWvsVKSN6iy4HDo11VGu1u",
"14MrZhwHSQY9tZd9bgJDZboDMfN7qmU7Y8",
"16VtQm5vZnd7DJuNbFZjeJmuXFXVqNKGhH",
"13UXC9N7taNib8mBqhbsMTjPhz5HnxND6x",
"197oWjoE9usG9MBxLaabQv1UpM2jaPHTHk",
"1AQcAQYUuJ1t1QMuuKERWKGwjnYb8iWf54",
"1nBSHsWi6mSzFv1AaoPGKNyMXw9NxPGcM",
"1KubHHWKPcNKyKkDtE8JMpz7poXyvfr6Hz",
"1oGoRBmJTqTmHeBBzRwLn7hctK2W3bfYu",
"1DWtPBWaHxQN8eAemzmi4Gvobs6H8UDqbu",
"1LCr1Wy4uza7ktMznsbuS8t13pN4izSfui",
"1Dc4XhWC6G3eYpohEwRbMTHiw8zcrqTTLp",
"1Mf7sh8oWSEnLF34cKQ2XYVfoG2HDnB9zy",
"1WNi8TyKeUvdqSbVwGDPurYXBhv9Cvxpc",
"1NvBtxq8vvUabXxBmQBASK3Hf2W516VWsM",
"14y2eiqk9APZPAeFNcQ9sogVTkoeN11USX",
"15AhJhqtNGNGG8dJZ9HfcAbPbtCh6Dzo1e",
"18L5VC8ECkLRBSS5xaj1KmmriQ1xN9qx4K",
"1XaDfKhjGV9zDsiZynAmMSEDSkifNg2sW",
"1KXy7JL1oHoZ1KFftFVXnyn8iJFNjYyFHA",
"1D3uEL4BAjpvEq1g1qyMfjmZdkcUZDNvPf",
"1MbkTNpFsYPoUFjfT34jhz9zUbPjmNfZBK",
"1Pz1L2kJmGPGsWJBBjjfCxukMUap7PCd6j",
"13akHutMS6jze7yLR5cpf4mUxAMZAuFzWq",
"1e3BpdcCCkVeDqKkWqjBaCRYvAediJtzX",
"1GdSvqByfGCiTkbF7PabVsCpbfZTiMzGQm",
"1D3NSW95Pp5JTrW6r5uUvB21es7KngS978",
"18kEQzveK876ibb9748F77gkVNVXBE7ugo",
"12YMqf9f5KdhBsuVnZibvSZp8GvhfVnRpA",
"1AmugB2rRt63NFmLHcBLzNJNgjzMC2gsCx",
"1PcC5zbZXFpwkoiyBFN4etfHceT2cF3LH8",
"1Ad7pHTBRXX7mXSmBGDKoXGbsQwYSFTvef",
"1J4v5dwWHzevA9FLc6vn3Wyfv4Z1LHz7qC",
"1B4z6zh7xZ88rSgakhpvXSwD6ys2QhMHvE",
"1Hz322a5n7BCMacuja6rQPVXfhnJ3wxQxX",
"1C5Np6wtLPz3nfvGYwaxdocY76VogmmFKm",
"1MD8CQ4VJB4YEQHCLc9XU84uFNucDQCAsT",
"1KtYtxHo382tpHggW3No9Q2tpzevVe4bhU",
"12kfrGTFQn3EMPPY3KAe4EhA9Pf7aFKfh7",
"19uqmZroyzBaiV9KzjXedo1xQ5FKwafSRQ",
"19kkJeVacTKYFR8VUisNLxhjHn9m4EWPba",
"1BFCSKSVztvhdqVdAuUo8XxBvJqzkbb4gy",
"1JCnHB7XkpMG7DysFwKb6NCsqh8QQSH6kJ",
"15g9mkrDs63vivHiXpK8CjCr8NtXW1ET2K",
"1KUmgBtAxFpDw328yb6JnrnN8FM8NpZk3",
"13qSrBxAEdhLyT6ay66LrU2xMiZM1AzEjb",
"1AvRPAHWBCUdjrgiC9nNfyzjNwyppgt6Wh",
"135wAKapmEn9JrL7z6iCb6CZvShXruuUnL",
"1AuBeGZ8r5HzbZkjXZjQ7pt2wDzN1aYoJV",
"1EyaFj7pWYzfVTGmjGMRHLmugtQnzo8Ar2",
"1DJN8B65BLNjmzhchaT4WZTmhdDEqDPDTb",
"1F251bgA2VZtpPUjvaKmuUdHaa8FtQ5MJQ",
"1Hie8BJ57y8UjHoqf73dmLJ6kxQLFNoZZa",
"1PZb2WyzA9VZDs1orjyDcBdnKneU4Qbejo",
"168AQzieWb9MCcEYTV9We8rJdF9zTwRJgx",
"1HFQXtWRgcrj2vv8He3D1RHYpKUTSP3pkC",
"1FgHycv8v1ndAwskWJWghuSJYzBzvcHv3w",
"1Dp6qDinRZaSshC4W7U6UHNxkE94PmX7mc",
"1J1yW2oBHm66ZqDtRpt7mHf5HN5kWkvvNe",
"1G2gwxJYCVyGP7K6TvE1mxdUktpFo1ETf1",
"16raN6DjsCqCQyLWBQG9Wq8JCuyvh9iZtE",
"12suPGrvxC3H6i4J4eaDSU9c8m7it4CR1x",
"1Dzin2S5TLcTziF8Xa4ZMZKJnxWuAgPUL4",
"19gWL5HQ2MASn3QLc8s6AZkWc2ihqHidz4",
"12bKhafxmZ42yiv8gfdUymhZDGZGrggccx",
"129ibGswsau8Ffe3JgtSdzmeTZAYhX3h79",
"13t6B55G9LJDNrRLSbcncz6DXnrjQRw7c3",
"1LuUnssV3DATsWDMajgJL1A1owD9E3tHY4",
"13CQfKQbmhr84DUAR82p7pq1bTBghZz8xX",
"1GznwW9QFQNUVbKmiQgMu8eYqMexpwpoCd",
"1PwH1rnPUG8grN8MeZ1LqjizJj5Ls5cPDn",
"12NE7cwffeDU6r9LsZLTAjJGzRWguygy3N",
"1BygbJ1ccfUuacfhLZj1hKeWcgNJe7LBkw",
"1DDNfco27895d91HdDe9jv9kBBMK7dHJjN",
"121PkkNE78NKt2soZusXNrrvKoMakApVqf",
"1c7ofUiuavN5nYLH7TfqsP4q43Yf1Tti8",
"1BQqSMDL9xVtkbjnm5yuyV9JKY91Ziejzq",
"1J7f3pt9uGirWhe4mFk6p6Uk1BDnPzyjXQ",
"1CaEchrN1hzQAjDD42jR3Lv9m3EgFJADxy",
"1Cu5Evv9nPkVuvmtV4VJSCct6HiumKaCPf",
"1FSqPHD6u3pTezhPVaTbxNqpAwLFUfSJxM",
"1681AMWTitWP5uPbXoqGDpTddXSzvKKY94",
"15uZ9uTwe1XrEMq5JHiLmdsJ9qnQZqJj92",
"1GWiJi1ovAtZtRZwxJz45fG6WZtoqtmWdW",
"1PjVfBDrdduBCbYTtrNsgShvyiZta3vqnS",
"15uV6qzkMpF1nnahkiRvq9T2zcsCF9vPNK",
"16gF1TG4HnTxaVa9juQ8r5TYxxxZJDc4vi",
"12xxrBTtNM93CY3fdfxu1wFyTPP4bTTBBi",
"1BXNcHJmLDzbJ7wwuETPxNYsysWjFwPKeR",
"169Y2NLoAKbGwMWKGTroWJ25Ro5hc35aY3",
"1EuJ3MkT1AZzxZ1wbTwU2Sa7UypFhPsBVv",
"1JGb8pyiuw1bEuDqNv3sBxVCrhDQyRxC6b",
"1JQSTWmkkKhxAPjG625AuxmpxpvwwdkGWs",
"1KULoGaNKCwjJA7z6kdyMEPuL6hptBTgFj",
"14345NhpC54uK3R9EGEhavNBgC8RRAMbEa",
"14Q4Py8DdGGQTNFvYUXzPbGEq8AfCQdday",
"1MUCqVKAWtCj7fUPgPZuaKpHH291KBn3Lk",
"13h9QJZtFRythdR9Z7nyNFn3g9GMXV2hWk",
"1BQbEkvpoxviD2bx2K1zzNcd3WXkRsWYyo",
"1K7hTem1kT9JLPMrnbB8y3EdFMgWEaCQiC",
"1D9xGgF4evjb24Wmax5qhZVKqS6VuynXLg",
"146un4VGZgQp6wcK5H8iBFj9sMjzyyVZhQ",
"15pUxWzhnKJ6QfXXDWsX2dkpT97qQxS1bq",
"1DUvLDnh53Davf5uEf7CkcPwbhtqCwd1xV",
"18iJyCoyRHJvYkVRg1pAN283W3MnZxZmK4",
"1LvEA3hRX8tJJTU6ZBctLsvMuxaQ1u7vix",
"1BnN4SLLqvimfjYhd32TSJE3kFyguqRP9u",
"14Wb7HmQv1CJxgFJaQpvRaHkTqKqmXtb7W",
"18oNtYKZ5wSnY636jSngpQrAtHHgH9X7RL",
"1K9Cj1CyuadTcooKb7gPKxCXzSqerMo1CS",
"1NzxjkUaZAZjUMUYaLbftv3UFzeGAryApL",
"1FN1GCbEKZLn18NJ5Yu4E72PbfYHRzoA3r",
"1JDZ4AyxJHz5yjgiKNjFYBiLvXHjH6LvaH",
"1FfXo6okJ25R6PwjJs9hviL8CAVPVYLyED",
"18iJpZMnZSmKvhLgB6aXhzHn1akwK8X1wS",
"1D6sqqMhrcQ1yhfu6ZHmZJfm4gVY8EBi7k",
"17xTGgmVZoA3eUmLPMiDqacFKYuTPzyxSc",
"1H9FwcgQRbtkpWxZaboLbCPfS5J7gmVKr2",
"1KHhd2xp2LSBk8LGNF4em2v9XBV5RMe974",
"1KPaeW3Dbs5BuknYeKRqZusZCXcJFP1oAh",
"1E1JZpPwyQAAwoz4bmmqfAnbzHdAmvJQfP",
"16NegiUHoQJnJHESBEHQuhJhcnU4ZB9o8E",
"1GFDx5UwR5WErLQRTTepnHAArbisJUuBbd",
"19JPRN971nanQLMstm71byVMbe9SxhTgqt",
"13gPkDgTxZRmtj5iKJX33eSM23WvfQB3do",
"12pZXXfyKwHF6e5NLAVYsA6H5FEJRWugtm",
"1FXoXbbqcNhkVwzUwEjPWnJoL9FLiaKxYa",
"18qdTaHTzyu5xjdA3FsmwMqxnBYG1dKsbw",
"19YcjfPeS2bC4TJyFWozFBJY8qCkmKipJV",
"16mk8R12xk7xgpcf9FJVNtMVfRUbFJCJeM",
"1MRKcwb6kjvAn5dqHGGejzQpCnnXuoSiFG",
"1KfCgisSsNzWPtDYa9pqYTKZmSvMQ1ves6",
"199JjquoeZ2du7B4wvcHzugrsFkdFwK52A",
"1CZCSSobGmqKvP5uMxjb9tqKUAtVHxKHKv",
"158cbBFv9BJZmnAA6qMG9PP2QhHKZdxrpi",
"1DWvBtj5payZU6HFudqVsUkj4o6RfpHCxn",
"1B6TXn8fNNEYM7DrbptPrWS3ab5EC3Tici",
"1JChN7wHVgvvNU6CmuuGtuios87GeusEJE",
"19nxAvuqJRyLmW8jwUCqDGmJkWpu11biZv",
"1GUcG71jYnG96Di7AWxaBpyMpFs2WN3VZn",
"1JhtaReDnidzRAJCXb8vCQkz8bsdef63LD",
"18z8ERd4FBw3fZ4dgVsMrDdTq31uCXJ79R",
"1CtQZ3Kj1jK9SNANj3GQpPgbCUrgebr41V",
"14NYoTtPA6t8WEdHdFnEfMbZiiQYTZrfVf",
"1DUM22gd5iGtbuETbAknFkukfsLvKEBFpe",
"1NEurZyVzEKBoS6ofTkSrwwBvCzd7hScAz",
"1Gj387f6pwivsSNo1JhcUNGkz1osj3Ev7j",
"15TZNLdkpVUAjkxs9dhHLG7n7XDm8Ew3uS",
"1BgCgeTkB6v8j4ah8pdinF9FTTdJjKWpiE",
"12ZKHSm7ixagVd4atDzmNSoLm9sKeuWVUB",
"12Eg6vCc29b2S5HSQv9d1ZWMmXTKFWzaDp",
"1Ckb2jmctrroarhzf6MKaZzdPd6RRjYCwn",
"1G6LMjZ1Btpab78ZUH7UkdZFTcXww7UFjD",
"1BAEAJTDJDDNr6MqVF4mSNyF4wg5hmXT1W",
"1CaTqKazFNRPq9eUjDj9QFWmyxtdcqUBzb",
"1PxjNfpi7kPmLvsrrrxwxGtcjm1fREk2EC",
"1uqfrkuuJnWnKZ8n6BwK1tbmmxCGS4SDp",
"1Hx348Q4pNbNe5mnL3BZSB7utmcJNV7qbd",
"1AKz8Ey22BQwx3ht8DsxNrPh1QqjAx6bSf",
"15yxgv1N2M9wPnYmfv6m1FqHyfQ58oULNH",
"1DchTvFYSVFotAH91SqmKD6qkKeW7EbMjB",
"1Cmvy3TDSTmJJHHP1Yc44Tpa8A8s9FpJ6v",
"1QKHwQbDFi75pbFoMqchLxFZWZyRZpffmA",
"14Ao8FJTcGpKLvDa4LPcVtB3FeiDDMKm6r",
"16GDb8TTaUUSeEJfjbvhEPQ3A7WGJYvXP2",
"1GyXCfTU8wvmvMixxvyERbqf2dC4YP1XP3",
"17uLaSVNJfL7a9aJxzMNfGc1aca7S1RYES",
"15uiL4hqgs95AshwWDF5A4RVCRUCbkp4jE",
"1QCimWMtsatQGra7sjNrwDUdpSu6MxRpMd",
"1NyztYq2ym6Af1aqA8DEboVPjDtgfwkZfY",
"1NwmSRf91V5cdcazMymdMaKbUujFpczpHx",
"1Cr2cMTqFa327yjF5XHnoY1bYJs3Zi1BNm",
"1Q3K64HtCnuw5VyAq2jrnLZW7UxkbaGxQ6",
"1Q9Mz8mntDsV5aAsMh4qcnVD9WmQaqcKs5",
"14gdaZi8fyUoAQgQzu1NGdFreCc4Ae4L8y",
"1ASvwSxYatNHuE9doqP6CG5CxV4khujwZ",
"1GczbALeimY96yres2NjL85UzxQH9LNd1",
"1mgZfjauYY4cnvVUatP7YPHnMt9bK4K1E",
"1NaYjvG5wHojA5fDRN7nhKXykB9ztRkgaz",
"1DzuP3BwveCDrUQJxCy3yijPGuqycqVwCH",
"1CC5nfGzS23FWYkyZ8YoExexNH9GaG5ebC",
"177FtVK8TvrapbXWmBNqBB1boRQzurbD6q",
"13cqJruwsC5THXPkR57VMFcY63NKV46mAC",
"12enG9LibXKvUiDopwk8Y3TKyrpPAAdmEt",
"1KWYHHX9TBKtNTf9MY8fNJ5vSQwSZkhDmM",
"1NT2LNdV3eGwSwGpanYzbheBpPK2jxBHdg",
"1Ni5RDaUhP26TQPtMvZ15pUF3S2gYEtypD",
"1ojWk7LyNr4JSWgBB73baoRrL3wsX2PEL",
"1Nb5SkZBpEQXpgYvTYwrwLvz6ZSRtYBWWS",
"1HKGZne6VmWfQFWfMhyiUa1XkzwS3irKbG",
"1E9pcohRkDvXHM2XnzhRui3E34Gc7M81fx",
"1BjmNCtn2YRY6cn5dec8qTnzD7EDC5Jzf5",
"14ucsVeXTASibgYLwPEq3Tf3s6X8gkb5hm",
"1AzqS8oZxVE5X3TmeehaHT4QXvmTFzRz2M",
"1PWxscDFuSYNfwCLXcfNEFS1qnLxvVc7yz",
"1AZcoy3Za1F7KjHg3KRBhJa8nWPNBAbjFj",
"12ZMVoedtrYDpc3ChCBKZJD3SHvy2w6GBh",
"1EcPBYvB5ZDpdNX1GEDUP6a9ASJUxoB8qt",
"1FfTSqiUPXv3KoTEuQyisJN9bDYfrTqSBD",
"1CCma9CDvaTTPZroULNHsNd146i5Btam9j",
"1C2KfAkNFTQF7MYyN99gbT9r9zjdUDEWJ9",
"17PPWNyvUaqujTZDZsyD2KMyL7a789q67b",
"16Hc6UbSXGtqdYAJzTZVzn6cHD1vuczFNV",
"1m9DmbDNkKGVd1NLjLuP4uCxtYa77dWdC",
"12UTYLwFBdRTybD3wTjT8fY9zeTH6g8Ao9",
"1Ny2q2wdArjCEQiiziHnFVaC15VHZd6Jtd",
"1FJfGnvhTWWWb5HZdf1efBW9zj7iFgNVk6",
"14GNJnjV1SMB3RcT5n49taDGpVpdVB2uP5",
"1ChraLWagugkAdErMNK32eJjzRTtR9TCcQ",
"1CBAAGZdC1JNUXcx8z25A5WiqorN7p8pry",
"1CQpCkK15RfSSi587ZMUSfAfou99ni1Zos",
"1DmApNz66Pe9LvYoTTkT8E71yQpPT4TZ6W",
"167iGq8HaBNy6ihSxJ96NaYXmF4W1vt9bb",
"133N7m3dqiRk2f2RRtp9cDFQecMMBxLFv4",
"1ZqgkJXrzdNwov4r7ar6TnmjbeaD3wB9K",
"1Ew1gEDrVsQbUvXhk8gQoQoQ9hvBri4XR6",
"1KW4KzmMhvuiaAHg1ASXqvprxza9w6Lvy8",
"1L4Ab4993HmmpntVKatUVB8f2EzoMuZKLV",
"1FEyhfZDAiL4XuhfSv5GriFVzPGFVHCBu5",
"15nL8S17MfU1XeminYzEKyvxeh88XVKHkU",
"15z2xyhw1LjUvWEmNBu3HL8Amd3MbYjZCE",
"1P3xwFVNKcoUqJoNRHeXEyWgVEXNMkXYDj",
"1QCYMxdYHKLEmA79qMh6CrUQGotm9HiCrw",
"12sdW6Xc1HnXAUVNi8yDrqcmBuayPT6Lsr",
"15QMUxYXYH4DhH2n3HkLv4iX1HNnB5eFpk",
"17RfNMvCj6zudqRH8CoULvFzN7ULakTUyE",
"1BFC1voe9xzN3PNzAWDMpuZHrRNJyRiR6v",
"162B8o7Q4JLoU35PM53eJR4Sr33WDUFETk",
"1nnPRpCRHemnCtpq8gvTFHKQeY5ckAa35",
"1FL6QJuauc3BaugJcdU76d21k397Fmkyem",
"1DFwvniEy8LdkWbiZ9Cr7jKmgnZMm8i3Gn",
"187KwoYn6Qa6obRt5jNaBspjToPhYUQTre",
"135o1GTZ4oNoA4o9kvBBJkxAoHpnz6eQ36",
"1NjrBRkfr71pPwhDk1FKGYLYeew6DEMxoQ",
"13658u4XJyjacHbevDUcXdHUvCuTtqaqaC",
"14xucHQS3RAn9HuDNVXDmJRnPueMTvVMyo",
"1QE917MibCbugSP4cjsVqsUhVtNTtGG2ac",
"15ZykgAN4WVU4XVRQ5mxEuqb5zaV4yRMar",
"12idYDzU8mzfJTkHutbit7YtGLCLUV2Yxd",
"1PtDagXC3sJF3Jni3Tibmsi6gcPJYoc71h",
"12B4cHfeSRhtf51MTt6cqo3DP8rR8LXtoZ",
"1MBsN6xBXKGB4Dr9H8DRf5K8Vdn6Qe7Ncm",
"1CYBJV4t2TjD4SYbgt1tqMtH6JEEtL6hFx",
"1DA7JiDztM83FkhmJ9VxpsdLfvyzHUjgRx",
"1PqMxyCebyvAGvpb1MXcVfz4fbYguFhoNA",
"1M3fDV5vvpMuwHSTKuNyDJxCymP9cQPBR",
"1DAgNLvgoodtMKuKgMBBrA8PXCX5WQYyjy",
"12ZNjQgUAbaLkxBMW3iNYBAGbAdFXD3SuG",
"1CoBgaVgfPDTUdFJp2EvvDEX6YsXqKqa7w",
"1Ht2wTTAKuDoFsjDLHswQ2pLKBgkyHMNZp",
"19ZdQUy2yCJpmYzfcHDBfsSwnku4aMqWRn",
"1MreadaHiCEaW3uhngRCJZrhHLVYTHirxK",
"1H5HFuBh3C434RsACnDTRXbTb1cnxfGCqM",
"1EdgGsbi27k4UuG2QEdXYGv5Z4DwWLYLtn",
"1JDjKcnrsLymPy3o2QHPRhQC3ofhmxUM8e",
"1Bo5BvKxPB4RLETq5z6LfitodX3TQUH3wx",
"1Fs6RReLhG13ZbBkVj3JJnwqiS2DezuGdT",
"1JtvbzroKFGixqqbdMDCF5XabpqzuJbK9s",
"1EcxnZLmbApvKQrzZSyLaazJPT9pURrNbJ",
"1Jzx5YRPsuYcCMF3pqN9xU8v8pN4tS7ESS",
"12gpjTfCeZRmPPJ2zeBSAnoRrybGasaoMW",
"1DPfKFoxMCBXu5pz9AJJS1KVdaNTr5j4QP",
"12r3wqqAPYneUHGSu49nFMEkGEzhjwXESw",
"13KEL4AaEVwqe1CKedfY9ecBYVMtLgt86x",
"1MSAPE8pxTLE2w8wbu8iawQq11vZrd7JN4",
"14MhaQspbUxSSrMTgvtb76QbbXCyfwsckQ",
"12TJ6XE1tXaQrcgoBfszRF34ydt5jEzkBs",
"1GxwCJtKx1Pk5L545uhVGiNP41B2vwXbXk",
"16dsZM9ur6JUv6Q5d8Y7RVTpHeXUhTWeMV",
"1DP23umZrvyfsuYzGoqEuzjfnMsHJf9fpE",
"19oMb82DNpTrmhvGAex6SLoNFM6rfpxX1T",
"183tDpnL4sffcxBuywg1NKtB4ymysiczqY",
"184gbJkUfCmVgiTU1myh4QG2AKKVhNB2nF",
"1NL938UUenNkbxvwt45dhGNc2F8kr5ztyU",
"14iKovcGivudsACbCNoBSMnfN7i7D2n4AZ",
"12eDGMG8ieqtD2tq3PxgS2suhinSBw4yWs",
"17JCV85p9jVDtBMZ8rWTWg99ZfpQJ2sbqF",
"1HgLmBZPzcJiLRLwuyoNGCatCZ7xWxRoRv",
"1HtFCVoXnZhzW1ThANP5CNYWurMXL9jW7s",
"1G8ELQheE1YXjs8R12p1F6CUiQQCS6NvC",
"1DxNG4sNqUr6QZ4Mpk2n9rCEvVinp3hTyC",
"1HWV7u2RPgAjr1whTRstytTtt7CVskk5MH",
"1HaH2wpmDBBH45N5tBTbjnDYKYAWXNx8d",
"1Hr6oGmw7E3LAT7rpwNVCAYRdv4bNC53yP",
"1CdzNJW6XHeEUxGoc9S9KfLevzx4hE88bR",
"16mQeYpkvTWNruUN5S1iwsancXTKfLxgeY",
"1JPj3h4ptMBSuyRxzCY5EX99hFt4Gp68qc",
"184AsRFQ6L35QY81MPyfSQuSRRERY14kFW",
"1MjEJPgufJX64SdVUtcb4m1KsZBJwGBs4m",
"19hK39MS9hcJEMBEqQSzY19VB5yFD1tAcW",
"123gfWJr3FKdGmYXXtKwsY5E2bu3qMuYNc",
"1JRw2GZaK3CJnmGeBqsG8zZkgmC2S24KQf",
"1CbBeoqcBGpc5zCYuCM5GCfUkmTtx9T4Ze",
"1LYnUfLVD1L86HtNfENiCGwC3Mw3okHy6S",
"1MAYJkMmgQMuAHYUxQw1dpku7VFiLWNvXs",
"1E6yHBWrLenouozK4trADbNDoxXUBcgpd3",
"1E7NGd172BkrbFnCRi6CSD7E58eD4EKyex",
"1HhXUi2HZjpToAoDE4cHGhySWwZJrQ5wzn",
"1BwUCDPz6MUAyReq324Tv5y2EFjiBd4EvN",
"17hKGD2msW9KHhJoQbgyNiQixNRfGZTa16",
"1EyTR4HqN8iM59cRnuEs2WLY1sJT5VjHCS",
"1FgiHTFmaPZpf3vbby7vGCMgg6WpDnstXa",
"14ASm49qDXDhxXFK9Vse9HAWAjBUrCziT3",
"1HEfFz2SRnTcGd5iPb3KREk5kkc3Wz9pnt",
"13uP819MMjPiQELQCgZeFCCcyEKKWexu9P",
"1D8mzxCDpsqGBTcwKrRJjJNTTxNK6cRNnV",
"1HvMogo8f6BpzrmMzdRtrW8BdcSVx4MJBm",
"14rnD1WSQ14vhkoZ2SXFqHuESagvv2qq2A",
"15koJw4GvYqFyyPm54HGkarGzxRsCfNQy1",
"1StVMXDdHqtkxQZL8AXWUdn3ihs7xUkao",
"1PJ3Atm6mbv2j9Umx3p7sox2WdnmSB64Bm",
"1B3VpMKn9yqNUoigTc89VNF3kiRwWBCjhC",
"19WeNe3gCnLodQmTfKNfxVG4FNBWTSgSf3",
"1KLzoENyQFFRmdb7hiDug6jL6ePnCxsEgY",
"1M4TcN1B8am2McmX1cnSBZaFLciXCLaVbD",
"13jvqFrBxzcXHAq1h8mB1YgiximtcUy4jm",
"1MxAGkssspFVSPbgcKbuacU1pP1sgTMN6n",
"1Prgn86UpmUWTJpqhJVCezrVXAv9CmGk1a",
"1CqWQCUqwK2CPsHmHvcGBK1rvA1pCrUFTe",
"1APgyL3rPQU2vwsiCTLUZVvXgnKdsGayqF",
"1JLXBQ3ExMWv9GsgWMCczXJMfmLU71VWEj",
"13nZXr33tK4n5NtvYh3ba4TuecqvNFgtSz",
"1AGTUEjKzpktpi5HF3ewB28RPkS6r6j1Ub",
"19mXi4qcxU7vqMj5argTrqBhH74uhqtFD2",
"1MBxgXNjZWcGj2TdbTh4NjNxqX4GXatwDq",
"1LXsz2ZWUV3AorFV4zq6uG72SGXBKBHSCK",
"17kaHv8S3VRAthYNCsPpCvKtcVNqg59hZW",
"1HBD5rSxXxM15iJFUaYbJszDNEmab62zAM",
"16wcAewKNjYz9EG2DG3o8LmcDf3DtzEkkm",
"1L5w1ZzDudAf4UsJADZitZNoSNqQaxQhdV",
"13D9D1jV6fyTyRYR5zhbZpNbKLr7oRhgSB",
"18RV5PMm5XTRxoKEbYu57mSixhbwMPEGQ8",
"1GX2waWYk96iAsWgsBF2D5zVFEpYHisizg",
"1B152SzcAnDF6ay2dxZ4bVSG6q3hQeEKRc",
"1Epno8GVKz36SdN6MZ2krsfJXwAYs2R9Uj",
"16XPkbB7asN3d7AyZBBXPmqDyeHjQ1cu37",
"1DWu1Lt3f7TpSxAyLfwwxsu5pUaT8xNfN9",
"1DGcYjHWD7yH5LZVzaRvEpHzE22Vjz2Y4t",
"1GdVhFRZTK3U2PRnnEhHezeLN72S8KzBwR",
"12ZfM8xpf4BuHD6xQcDdRfcREivkgfLT7W",
"17w8qbKN9GHK8UEHgP1YJVuxBjbSNrjuuP",
"144ewbtyBYBcmt3og8Xuw4LDkurT248U7W",
"1EvvbgjrWfQUg32SpmCjW6r1CYBt1c6pc3",
"14bBGisw54ZPsbayy6U1rAq6ReZA9cxi16",
"1Fxe7C3FotW9TxxmmUQiqfxBuHZyXw9aZ2",
"14HYyWAHoMcWzGTHUM6mzuVjh7R8FLmHYu",
"18f8z3WXcBM2ErDeSBn4rjey8Vcm7vDt33",
"18bAH9ByhxBuZ446SVAj8xe6at328WkLKv",
"1B96HL4gid6GG4broWFVPZ5ZR2UNRBkLD9",
"15SwkUarnRbKeYamQA8KRHEV4SVKBTicXo",
"1DuHuQqWH64RWHDpmWTHgXu91ZWL79XEi4",
"1LymxE71TWqoqKo7rdxJgpRPe9nbiDES9r",
"1CtrgGG8AREeGEg7oAxQSSkRpQKkQAztk3",
"17djEAzBHCpBnwCrQ8j4oEE8ixQn2qwhe",
"1Q4VcymmDrNJxaQ2uAJRbomMDC3vRrbw2F",
"1CqJayLpfwNP6HHwrypmMvWKnxq2WG7JTu",
"1GZmbyDZkZL9MUFwEJvm43ytraVhEn6HaK",
"17jauhqXR23LCCEvjTdYh7BJPXyMKeF4hW",
"1DEJzUehB89Ne795cQAjjEybH9yrCRMsg8",
"1M5KNAxSz17gUgfDoSUCTPoggWYaBsDSFy",
"1Fink48w9CvGJsNFWk7VcnzwBVetNNJeZz",
"15sn8r8HkTUst8V6sGXtc7bES6KFn3LiJT",
"1DTU8ztyf2HYyxNGY5DKJf1JD9yWdytqVA",
"1N9MSDxi8KCE1UvUVLBBR1hTywbpWKffw5",
"1QHNq1Y64FrGKaD974VVAjmBkpx7hcREqt",
"1FFWk28wEckQ7bzyYBSUss5nWiAbxprTCH",
"1G46GN6wkCzszFrRMWgPQTpWWcD9RMLEHC",
"1N5bvFcjQej8fv4qLKoYWQhKTcF43mMzDy",
"1EEgQ1bqQc4HVQiUQhhpDoR1vMWzBqdK4k",
"1HbofhNMmJohjD4iUkuMjT6tAX31XwzHa",
"1Jh3psWthZm7sPifHMT8z1KTVwCVFT754g",
"1Bofy3MwxiCGi1z8JBk1TUVYDEAAFYU54j",
"14XEP8mNw1podPumXQ98LdLZiWLruYYKUN",
"16nTsGUd4GzLvuiApKN1D7s7fY2bKnUnXH",
"1J3qwbCte6L8yLF2L2xgG95werwaYZA6AW",
"1NQHa6oonFLFpqXZVqK2YestixPrnRgyaK",
"17JT8eKVyrZUhDqQLnv3nTsNxhuKuxrn6x",
"1JNjPcYE3tufYJ2SY5itSzSQSmKYAr49Uj",
"1JfkzSr7WQkQCVhRtnChmSvpWGHZBtyBD2",
"1NUAcGjXFVhxceVsPpD8BvrNMm6UR2fR9i",
"1AobGtDhakGWg88BNdBFnU1175KyKR5Uht",
"18X94Sukevn65JbDM6jnL1Ar3p65doYbnE",
"1N7D8mmL65csdiYNjzRN3Dwntmgn4kmwXS",
"1KsDxhFUcNwCLFHGVBeFooQPhLiaNYdqr2",
"1M5LhFSTcbQpk4MCzPtZJZHrzSGVWij2qU",
"1uPPcegyo762XdkufSD8J4gvoy7UoBNwS",
"17VoQ6t7SfwYHf2RDSeQj41HDEuWSoX4gA",
"1FyThY4DrRVekXr34d7YwaeyV1TLp1rHYU",
"1EopzhtNbDwJuNmALX3DzH5BgVnBNz9cg6",
"1KpDuC1SAERb2BKykA4g4Ww5Z8iTB1AhFe",
"18WVJvs5givurxNeuY3v8T1ziMJVrejwY1",
"1PavgNGNdv96ovLbocyd7qtfF542tZ39J6",
"16HYN4v5G8wvZKfeDajR8vgJSBwYh1fz7B",
"1AXuh9o2CXEWSzmXTafdu7m7J7zdJpZcjJ",
"1B7qbMa2tQ13MMhREmpK6Y8Hz9zXrwj21Q",
"1NQCUBU8dUwixW86hbeoj7rz4Vz2ZmY9ey",
"1CJihCFtxMYkSKKdDU6uJGNRNCAnv7zisA",
"1DkpxWFvEku5S8Ms8sMZBBfhypSRwdYSek",
"1FayghYJ5cukbJ5BqjiNiv5EGAE5XZaj7W",
"12kQ4oZDQv4H41Tzprjx2WAMQJnouwnhNj",
"19yQDvVW35aM5nyyu7i1NV7u1hAyXZm4v7",
"1GhuCf5mr9EkJDsoaV4ENmRZT5TnwXFfYJ",
"1861CgRF1TR4EFNNqeFkDH54fPd6sXhEVX",
"1CwJNvPcP1cBJm8jVneUXwc99p5tWoRySL",
"15zga3YTsExFYz9GGDuChJQcBEAo15fksk",
"12XyaM3msmCxoSA3JUzEQEhN646DFmPgTW",
"1CtLqw5Z3k5dY739Yjrb7bVnefkvShVR58",
"16f8seV6HUNiA6x5iWfBhsZoFCLsSX6bvn",
"1KFFTHhX79ZoQVVRKofXZu8riQATSmCkFU",
"1DS9hYyfQGEyhatvRa1Qe7UqqKVJ4CoKCG",
"15K61z9dg5JRPJ2zv9SDCT4m4wWz3uni2S",
"1LeuEsKLioJin4zzRAt4D63ivndFj572xe",
"1CCiJed7MhzKLt4yonM8Vf46yftv54y2LJ",
"157xQyaqWHzr3jbr4v8XZEwKRr8en7z9YG",
"1MSVeKpFL92NHmRVMGSZaVQFu5FtWGXZd",
"18jrHy62p4aBcZnChd3HDLc6LN9PW25z3U",
"1FR9P3q1P5HpizEncZpti28rMuUbmQsEaK",
"146eoiXi923NMPU3gdRDW8a1PvGETtbocw",
"1JKKqdCQ2CJtfmoPJSAfqK8uR7633mvt6X",
"1E6yQZXwnsvf4WhUDubiKUTumZoy7Mnwbw",
"1MkbiMnJFwVCasYrs7Rw1JZ99jjDJH1TXb",
"143KAZygbkEGDTZ8hpXdgzPun7QsEVnyvW",
"14MvQFXs3ktRou5axYygu3y44EiXP4xvEA",
"152rMqTfV63pNGsR3ZSHSmLpuc4a9aSNU2",
"15bj7NaQayeKEa3ESBLWE4umBMYgnNRrXw",
"1D74TEq7G3Com3eYcUWdZ93UjBx8RVkhye",
"1AibQdcVC6Tmh4G9VQQyy4kYVtkcRQpQ6T",
"1GcJ4ynXUwjtCteUcnyg8yc492JpPm8jou",
"17uG1N5Jb4tgeygVe4y6o6AEsFruACFwM3",
"13xTr4iu84aGoBhzHGJapqXojkhQKRxCdK",
"1AgUwUZDREoqEu7JPqphd4J4tBt2BaQrwD",
"1ChGzVrzg6C6NcdUeQFVNsh2vYt1mzKpxQ",
"1GKL1W5r4NacpTgCoXAG1EPkdHy9KPdX9A",
"17mfBQ9Xv6a9s1HP9CB55ahzVuzts74RFv",
"1HV5No6WgdtbJ5obQiHWgyDVvwZUj6RYWX",
"1LGPNUh5n3xn6M951K5CfodaL7Dxi9u2CQ",
"12mb3WqttVfPBbg3EbPYAWw1eSgZCj8W9a",
"1J2jPd6eGqXptNkr5tFT57FbbDESNAW7Jh",
"1NX7dsh7wNPZWhwKpipWRU1Uz5Tk4JcnLX",
"18n3JS9xgJMHJa6emUcYyy2ajvi2h7ShRK",
"1JG897t9t8Srvo4Aps8v1AdjKQgGbEZvFC",
"18DVMvMPhKrnd2BCMSckESQGjmYQ98AyVN",
"1GUstsM9Lajx1qbDVrsYyGvMbuEnEZvmiU",
"188vZEfAB2ELBrSnRHefP27tiz5amMEM4q",
"1GB5GBegB7a4baBGKoCA8138VDfPvFqDQS",
"19CL8Vd4vhjXqwUGnRbk2VGXs3XkjwYs9D",
"17YA5jGqYAeUfLGA2ERajGnTm7U1UZJN4D",
"14uYE7oU5PUHm3RHwbC7Bogvd78NHR937K",
"1JVnwD8hK2K9zYzQUbeSEZhqw9BYhU8n9u",
"13SFK2qDZWVWN4XSvkN9QQVLvB1bm19Cxt",
"1AhsgyzFkNaQDZve4FgVoTzpe3yHutUF8A",
"1B4XhBHSzeZ8X6S3xBgXUofDAx32jYVK14",
"12BJKQXkQ7qeSCMf33xh5NP3ZVho1h4HxW",
"1KQddCBMrKnYnHWA1sG3ZTHwnyJ1CeMi5",
"1LXyGvbsM6JPcy7s3yJU3CegHafvhfANsU",
"1JG6S97akdccSt3jgiBaSHmVthgfohU85T",
"1Lxc3xAMCqkp5F3pH1GjdNcfmPWV3pujUT",
"1B62G9nkLHGnwVgfLyGiNeksMJqXLP5cRQ",
"1DSvgEvdZJoJjdNYTVzHHM2SDJas7tpiRc",
"1LkK7EAfJz18bUbCzRYcF455iRFdm5xUxH",
"1AXukVgr5Frgp3qcsaDsbqZnsCK3WMhZFa",
"1MYuYsp78URauh79ahhuBQ5jS1rvS9Skks",
"1qo7j2BMupJfVbeeggRtFsynj76Lo72Bf",
"1JJo4v5FKfixTv1ibp8pikZqRVsDXxyXWB",
"16rkjp5d29K9XW2rESLwiPZYKtrSsaGmxa",
"1HoQhhBKA15kMYgyrMcMWgVA1A7H8nENjv",
"1GM8PDxu7jFnF13tuCc15BHfNgQ4kzfzcK",
"1B8VFQ2p1Y8CPfP6f5WYcfQ2LzF6wW2gsz",
"14kz4tDmJiHMJ4CaDq9pukGegd13FekUL5",
"1Ghn9QQgbHwMMCEVx3snqebNSTHLGNiU2z",
"1HCgKt7SGetcL1LUSEWAHkdSwPJMXWLBos",
"12LFQYaEPDXYoGzuuCCGdwRiQVC5sZ6k39",
"161SbvTaxxJ5ZML699uUNVgnnJLQVKbnFU",
"1NyVmpAfK2J7AnaEp2koQ1zz29kLcdPCbJ",
"1JKC79DAX2GED47behKsFjnCkq4kNWEMMp",
"1Moc4wVAC15tvDQdNPdfuoe2qMmbxgu8tb",
"1CjZhpzBnDKpS29ZWn9FCMC9JdLuDApu8b",
"1N59AcLWZfXb3maYbHacT8i7vRbShMb8cm",
"162StgRYsFnRivDZ3d37jDT5Cj6EzPYhFM",
"1zvd4YmSbxFnT4pbV1pn99XGwi8C5uC4N",
"16jiawyrgGxTfSNN2v3jd1BbqKDr7dkCCx",
"12avUGo98uo7nfZSBYTrhCtT4ZSeN1hZsd",
"1Pg8xkBiJQ4AbHyNHLExA5ECL8xiXMJzas",
"19SRrsieBcHtD1WnqtuGg7HAXWGzdEL8LU",
"1GcenCTHp4Nb3vxpEZ3PBMnTtbSrSUQjHe",
"1JmWPDwHsGjzmrxYxidppp7cyTMigy2XPn",
"1LHjdAt9YwTvNSYqBoH16p7zjCYaJzALa3",
"1a5YanLRo19g5KegMEJUGteB3a5CMuPzc",
"1Koe2BdoM68Stw1eC19HKuhvrQtZTkpGBn",
"1B9SHqUZHcrBGGbKeSyiRbsPUA76QVUrBg",
"1GjFKAtfCebu58E3oMDQARrbq91EpCaddY",
"1AYC1hfcanzXYSnDpxM96CC4mzxJuUiuZM",
"19FLp5YRmKUWmV3TShK4aKd14T5nfyPqhX",
"19sx2KfwW29zZogkdV9g8rYix5mquUVuBo",
"1MxqLGPr7YpTP8s9P4KQNdK5e6KtU55XvN",
"17qA1ejQrXLRPTJjL1HBhZacGtS6EBeN4p",
"1FdYq7Uz27VxavHkQMQn2P8qxeNJcPbL3u",
"1HPTrDspmGwNj49Udsa5kx8Ts4kRevBHGB",
"1DnjdcQxug7pr4RkoR6gESyjRBX4n54Uxq",
"135fuhexbstXLYdGR8U6hswSJnh46jRa9n",
"1WrMfut39Aq1twZqr4duqHf4J6VN9CXyh",
"1Az57kCeR7KoiSNQfS7tVm6hAEarqxGFyw",
"1Q2h6qRvHZK2xReYYYP5C6XB7BpFacKCaz",
"1BEyjFzpbVQUzKAaQB1U6UUjXZqHgDJ1tL",
"12L8DqaMtS4DBURp3qf8fwdF8pvTM4tVvy",
"14tsQnBYmHue73brXmeeUfcvCnkaHzsPMb",
"19Ss7miN8a2wN2qYSsXnYxXSNwMUR31ieg",
"1Mc7pSXzqVC5SMTqNwoVfSbGue99akoizK",
"1DB9j1rz4XvJa4ku972wtexALFDKJdT4fC",
"1NdBxUh63YyTRj4HqbUFpXDnxAmBRgbcGm",
"15n9z5JBz6E5QYuxUVvCh1uyhzHhPHmoVX",
"1Q5zCtdnsJntxuCjrEK2Eo4HY9r7q36zqy",
"1KbkuLg8dFbmZN9m1JVy9eY6os94CXETXs",
"15s8iPfAALSKTAy9XtXDXJww3kn8WpK68P",
"1Bdh79Bf7Y4CLvSZa9zXNabprxxJEW27fq",
"1GRW79eDuHGwWu7VCE5KMxaw52ZHYRe4UJ",
"16D81NY5vg88ugdeebptcP1EpUeB4nnNXd",
"1ChK869koUWhAAScJerdUPLZM3Yt3raojq",
"1C7zmfUpYcaoeqotwieTmgkchrjQ9wquJC",
"1CJMuM9Vvpf8Yw4qynQGmSpNSw2WoDY23B",
"1BXF64fgQVdF64EZY91JhNqEwjZZTeA8Wt",
"13o4v3XhCKRSPBWMbFG5QHP2azVRytY8hT",
"1ApdErKHe3jkER6vXu1Ycc37xE9cxcLvFZ",
"1MFx5HdFmqZxcFnfEVEAnZEuxhaL6SGRjo",
"1GXDAt9nBySwQqBVMypD9TnSkBGRkDQ8XR",
"1CFhsK75DkV4RFwDQp6YHwp42DeEBQRDsS",
"1749WEfRMrFxyyALDu44ec8hfQGMRmEi2c",
"15KjwKTn5ckdRVobqTra4njqvVsSkjKw6C",
"13H2MCtCct35d4xVEy5DUZ88zHXF2JTEV7",
"1KfH7DN5FYxFAUwRHjFsyPbjTXkTrMUFnn",
"13r9KCBZGEZoX3Qtd7Sotvb2yaqk9tYxv4",
"1Pkrk2iLbBBv8pntQf9nPkE2tKhkZUrLuv",
"1LxnxR31oj9kxuw7M1hu4pj5Yw3ZFLgtPj",
"19YwYcWrVfVyHnz9NsnR3zztyYpRk2Lo4a",
"15BFQjvFRRhtSKvCFvr3EGwsW4UXiQqfXZ",
"1MsKxQ5g2E6CT6DctkZrJCiUuubN6tY89o",
"1LRJhHfti1mhkuMrgxXRhtsZvTxqvmcCU7",
"1FKtKbCvuZ6Vh1TpPNxBU7NQiTSozzA3RP",
"1GiDDzv4sUEo6gJB2Mx4EpVyevgBPaWdWp",
"1FCaZm7inGSrrUiN3zkCxxhtJx8PutYVSY",
"19zdfK7mfV43Vb6PpEfnaZQJwyw7Z3tqgk",
"15axiJevaubKnEtyhQncLihAoX4jn2bnXN",
"19udaZAcRWziCEsea72zu4pmJdKRy3AJc2",
"19ZWjMA2A6KPY9zvxHPEnGydJyhHM6nsbW",
"127GJNR5SYKL7QUUZqMSmXi6JY22ADFEpS",
"1DwDE9vpJodEPL34frLnF6vN7tLikaqH7W",
"12JJjPdUBptcdbL6LEhARS1NtbPU7b1Sjv",
"169oqskXDQSvdFGnmY2jXvqPaV8RFf5q6N",
"1BeBWvSNxrCQJXPBGPovGx558az9cXrB2D",
"1KeisbWSo6ZYT8Zwy3Bt8e5Vdzo3acL76e",
"1HYya1Y3Lqg4cLcDJXSkDpP9paRpMpAat9",
"157m3PrXR2AZPEsMrB1LSgRyYpWcVTBHL8",
"13m7cVToGbzrkRBj1xXeWdMshv22RpErvn",
"1DV447ZQS3dX7HqfiUDBatH4WA5Dciqvxb",
"1JM1kJBcSC8ryf7UTbx3XQr4sKCtiygQ8x",
"1GTcuvPMFFq57erFo5ZHaCuTihXYRZmVhx",
"1HFLxRVSNAD5AJPjLF8TaiLPUEGhytAYPC",
"13qkeh5KgD2YR2nVbUdn96y9UTsRtTZued",
"11cQ12cXMnbcTimUGdN24ATri6Lk158Nz",
"13N9LbvmKGsazPdeya5fTsnzKnBF8C9XtJ",
"1EhamghMi1uRyXFg25EpxXofoX57JLvf3M",
"18jmNSf39Nmeat59SJPyr1DXtoUM4YV5Kc",
"1M7QAcf1ikxZiWHR7dBvEfjBYjTz75jTbg",
"1FSmEG8xTDpxAnckFHeQqPceg6VsM2RxBo",
"1G448TzQxhjbtYDfv4Cuig5d9oxMnb1uij",
"1JNh7Dow73EnPAyw5oXx8jHaAXNbggTLZC",
"1KAzaBnvcKwsZTPf35jSkuvHFB3MgzugrG",
"1JuRoqQcfisE2ZFdnFFnWygHfons7s31cu",
"1K4bvw3xLiamgAFSMacxLjxdVGZQ5Y18dx",
"1KCdHzBmfV9gmfA4fNzfj3285NMUcnhxss",
"1GbsHjzxH3BTMYKocw7UTmhjsRaynzSHnR",
"1Jg2HywJqZTe243WZ8686JeUJfSJ745MYQ",
"16XLfF8DE7XAEqCnAvP1twPnS9xfEnsBZY",
"1AV8ZGhewAYb7EacAVHrJR3U4KGXz8JCJK",
"1HZdxWiLYj5d2oB5QcZF4VEVK3gokP6Szg",
"1Ey6njxLCBLDh4ga8GbXeJFGVSrpUsk67L",
"18kXZy6canxvHeXZkqSD2VQV3owQXWTGZi",
"1HumdWm5HjKEQQkXR8iYcdg8Hf4D6rRhjt",
"18ypM8GKKSrTSBzLjsKsWiUa2eNPjSpAfE",
"1HR7wEsaBdp8U8eYLboWKwVHTSgH1YDCAW",
"1AC9aoYbSSe5QF8DNaKmibwix9WHKyu7eX",
"1FcqgEQDhiP8T7y55KYzFYCjZ24tjpyu8i",
"1MuTpQrE37iTMXeHcJ7hZRXtwM1VbDDX99",
"1LbZATYXwqwtDsjFaVQAFn8WkkuUVxCrvG",
"1JJPDh7Up2g6j4XDr8BD3nB4phf6ufiXxC",
"1MkcdkLUEtBDBe2tKXvdZktNgmsCzXcfRn",
"1LE3KmLvBQeTbLaEPTdMwRxxmCBphKv7g1",
"1E3bAaEK9xy1M5DUTWCoYSD9jAah1mwYmh",
"1JieQWf1oB3MUbJbyVE2Ssd23cjyNrfaEV",
"1AopN73uLtDGuaJYfi1QToMVqtZ3jK4MwP",
"13XTx6h6zQKCBFvXwUrCDyZ5BTNEY4zzof",
"1EKbcNpxePAYCFqFqQRqfgDJTtiaMtsfVP",
"1D2qfHk44hy3pn8qrAFX5yH4r6ec5YWfVp",
"1GKgYpeW97ty3oTCfpihrZp6iFr2QC6xWV",
"1PaizevU6c1ibddASmEpifwrDg7rZm2RkJ",
"188qC96NTFMe2hztC32X3fi9m7TzJa8cxP",
"18iL9NeCbrqp6ZCHDGT9BVJqq44GyfKPqC",
"1CQmDp2ymA5yco9DV9ocZzBu7HVMVEnLxW",
"1JsvkJDWBFc4Ny2exHoH4WhLwio66GaZE6",
"1CQZ1MRBhr5qcZufaHyEvdg9WEAtGWKszR",
"1KGESFkJhnng4GtdzUGcX4tPeqV9fyJ2hG",
"1MHQiqtrqHz9NQY7EKvkMnM2uvjDQvbHLm",
"1KzG1CGy7bvsTW7NG4xffoXba4iJ6eJJYQ",
"1M5v36piB5wZb5ysS1iYMcKM1TpGmjZm5V",
"1EfcuGioJvGLcy7uMa9BjpXqNbtRN1Dnn4",
"1LA5nbbZFSxVUJC29owC6oF4ANKNmLGBLe",
"1M4fa6i1SwZCPr7DdAZV4Kj18f3UBTYX2W",
"17n7Hj7obTEu11fpDZiPQgjxoEzbWhsfgA",
"1Wrg1J2MFVyLu7wXRm6sfD2KL82xkSKrZ",
"1AvDUvWL1mQ2GkGVvLFpcyP5Co5EHvPTZW",
"1PwHvjZrEaJ9bV6XTaDKjj8E926TF78mnQ",
"145SZHMq4YQenXJEAuoin9AtQzXNGQzq8N",
"1GSvdwQvp532UYrivEE73UswLcocnpj4PD",
"1LeVEXsfkG2uUoEBakyUJDQ1nYzSyHfbda",
"1GzXHgKdmty3ydsxFsK1Tz78H3CdnSv1YP",
"1DUJL8hbRMm4rzHV4p7rfQ5BU99mz4LbEc",
"1Q7UM1dVH24Cz62ph2jHpLUUqpP2VD6SXR",
"18DfJiwgJzZ521Xcwk9Y7rZoZCW12xZbhY",
"1GM7zr2AGMcWQRGN1kmukkonkJz3gSjf3R",
"1CEJrVQ3i3wSgu9ahNYvx6KaUGBoJtkbq",
"115ApkSQDkN7dAajPeC4TgtHum3nXbaESm",
"1JpTSPjhcf1iSe8B5ow8TXFESqE8fUpJP5",
"14BiedgudKVEUVDrQXY9gHH5ddk3RjU3rS",
"198bo1mQQ1MSSNW1SgrP6h2KpfGDX83rzg",
"1JavfdnQQ9rBbu3pagdzoXKzAHHZ89EGuM",
"1DvVrAxQ4h3f69SuSgGUYZbRi68KQFVr2z",
"1EoZLNBVdXJm4zavQEdv1BWLjhqYmYvCHJ",
"191a6pHDo6iuTeW9KM524dDdpFLBEtZJFt",
"12pFkRkPnGfAhrWQEuaviRHLCxCkXakSMB",
"1CsKFRY5n5nDJm68796xF7xNJqk69PSCea",
"1LY62zNa6G6PDLkZxc9VcxXg6oi1boUK6p",
"1Ekq92gzq1XT4dytYmmCJYChY2TaRhMNy4",
"16Ubm39hYXE2sU13uiB5hoszLwVkgfpjpa",
"1EunQsdeAS4WBURJV4ivuoLkKddsu9kshh",
"1CCY7XHZVv9THctH54XLjv1sVbaoo1fR4b",
"1M13UCBTEaQrA7esevH2tSPVrcUpSL3cYs",
"1JFWuMz5iQJw8NUgcTB7yqLzCgqnQ39cox",
"1A2hGWwaPZkeJVVEm4fsFvpRQHSBkQZcU6",
"166xjqtP3d1YTqqmsuDyfdVbZrFqh7E6rY",
"1M6hZRq6x3cU8QU18uVfhyESXZtAY1pBCp",
"19HCaQ37b8eQKed3LKGENEpQgrphpQsTGD",
"1Cxv3JPGtY6ttogEj6rRkk9BhQFfE7BQ78",
"1HbTKP8UYjvQs8wB4SvdYh5PMnZHxkksnQ",
"1BfEdBoDLnFyZsneuu2LpF72MMhi3oLcaj",
"19ptrbZsTuWYx3oRZENyEz4aQTJVksMSfZ",
"1AmxUCzTyuNMn5SfHevSXngi1EuKyjKFbm",
"1LH8X6AKy32mCdpYFRKQHfwqVrsUkMq7Kv",
"1LuwgtJJPpDGXo4mJsUN1MZXZVDPt53S3L",
"19APQZc3eUQaJtP5RhPdEhzLG5m9z6V1XM",
"19sGwqEkBCbuqupSbr9U7LP93j18VQcePs",
"13CBN9qkBRBUxvLXHBkckmHrybNhTqSmuF",
"1HwfGeFB8Je9yuQaZn3hnWSBkEoMCx2RFJ",
"1NJnCrstKZBSQgZagLFkQQ9MSjVRy2oWWJ",
"183KRREDAGf3ANidzoifaYjxsUnkNCrsxK",
"1HN3waoncAkV7X2RdfXyoGNQymDwBWBpc5",
"19iPp1CnRTpRzqJxnV8oaVzdGSNCtQCZBD",
"1GkkJNopdF9RC6C5FtW6PqD6tH9LvYfKqb",
"1Wj8UCj6JHSi8GEsZrD7CL2Fyhf9f2WAi",
"1NNFob2Kc1tEStsjnMy6o74jsfT7dGUXk1",
"17Jz6qziFsLXW2HKoGUpbUEf4i4RwmbWMA",
"16nr39ZpLePftnVzvm47n83DxkEWXjfQbz",
"1EVFga7e3KMBRuSuh3gs9nNgPP2jaK6CWa",
"188y38SayyPbfNmpWJX1Ljcnk4KP3nF8UJ",
"1JfSqY9byvxsBezFwqWTBBXWwEbcnNhWVD",
"13BCo8qhR2bzzP85BMpT5JC4Kb4fxFmMoL",
"1mAr6hWkyoAwJABXgUpFveFrz7mKJzTds",
"154rCEnikBpL3mNeNqUL9MdmApTbNhs25S",
"17QSntEJDMMTPrTVN9AchbT9qdcyemszyn",
"17zj16czz5NpEm8ri4LLhAFKEBH4cAotdj",
"14uudbQkfeJ86eKbPY8cBuM8m8kGvxoJop",
"1P4jh33tyfYShG1zRfL316A56Wtjekg83n",
"1GWNW2UVU9t7QcUarnUwvBHNtSKCYCrEWZ",
"1AyakJPjQfDoXbMdCCj9gYn2L5TLRKngg5",
"1DjWHgpHn2SAWqEdfnjSxWgzWQPsjN9THq",
"171QUaw7k9nkRYmsVW1ugK4dxmBNuKXg9a",
"1N637VuePwbvJzVPo5MkLNiX8jS13neAkf",
"18AJTV4sVz1vYadKtkKgySSJZzMAWtBMGo",
"1JuHQ2TRQmfPqVw1Bo9ekmfgww9V3cDq4J",
"1N2HMAccs89tu28W8eSavjEXJ4SUupjTxk",
"13YT8ymHwBWTdVynnaPZYHG7rXbBhnecCg",
"16GLfAyEbdZZdjhsjbugwbmkKpHeuJLN31",
"1JZewRZMxE1MgVreUDSHbJ2R7Zfw3UUoMM",
"176owTBoRs7HQAiNzkz7B4YjMjC85caChd",
"1As8Nk2GTKqJN29B1LNnRLm9vPzHQrFagj",
"1FW8WQXiucCHqX5o5F2TcRHdxSSWNECSwv",
"1ArFGsTdMRs6VCEnpVrB7kDTfDJdT6ekR6",
"1981VCbHMKFCqBM5jPCxVbURKkUp7XDrPB",
"13k9wWgg7ruqxSBE6RJ3eQ7HnB71mKL6E9",
"1CL4nsH6zC8yQ2hRbFbke3e7hexGDRojAC",
"13Dp3oRjwMtrT6DF3ujGYxcXV4936fd3DV",
"1KmChb3HdS9VzUCSgmbYLQdtKtBvaEkZ1s",
"1Fs42j7UoDJBW3jcUMXzE7LW628ytA5bB6",
"1AfiPq7Ny3Xw1Y8dAdCydVBZpn8tbNM4aq",
"1HY7dqsAMCTBpdReenyo8U74MHpecAcxBn",
"172MMsHtvjq5ZaecsgUEdbtU5YHWb4w7Ti",
"18vCB4PVQZ41LzWiHfGYBhyPsteQYkHkgx",
"1GcN9rS2TLJEHtv9tCoKvp49NaL1fwmkJM",
"1E214AAvQt6UH3FNMR4HwYtPyiWKoNj4UZ",
"18PRKyFYG3y6wtM8wsT9ADpsA5zfkcSRYb",
"1D6Hs12TYWp3YHK7hoXNjSXLHqJZGMCGtE",
"1G2yYczpQwAVjb8WTqmzqtVQ5oajsyG6pS",
"17mQY4aeaGaDfjrf6jM3YdcpPUZV45Heew",
"1PeAidkYofjCmZkEVxbkLNUTv4Psm97X1X",
"1JkYjsC8xuVJAzgXonwT5XUKXxNTnCMeuY",
"1JY8Y7cCX1b7HDfRG9nqgDSEYMCfbK22k2",
"17bSk5bbkR8m8W6W74TGqwCnf8zHuh91Hu",
"18yC6wbgU36sEVCk96TgaoEnnDXFxAB74r",
"1HgZXjX3bhLWUVzMgCAvso8WH8Q8djT5pg",
"1K7cDSUSk5UQrKqgkDC5JLBExGdFCQcegP",
"1J7GnFhbendKJBABxrDjfwpnPzE1gG6RsC",
"1MBqx1yUhcqsK33du6xt6nWkHHxiaSYMY",
"1M7vD8Ec2zJYMDhKBMo1TPDDw43gFW5xb3",
"1Muhir1ZBT6FjLBq3Z7qJdpWsguXhMawqj",
"1B7Po5JYWtfTkELurgMPZxkpk8Z1RAhycq",
"1LuVqQjebHgbmSXjwyszQh3HsE92o62Fgu",
"1Lkeg1i12swfPPurL84FxnMs6yt8f47UrQ",
"1E9HsGe4L4ouRy2zwe84WJyemVSG5nHw9u",
"1QDC6zg2AqcLhtanFYf9JK1XgDrsNQqPG5",
"1D95qaB5VgXQnDFkub2qzpC2Foi7W33rQY",
"1Ja61BgePE3P3x7H3TmREAMVcZENpdCQ8g",
"1Go6KWHafxdmbmTXRd2vaMeDHfTpFThaJa",
"12XjquxYri82wz3iZNtgQFnRBPDW8Nf2tS",
"1FaGisybWMWKdAVs5vbcGpqDcpAdpxFM4D",
"16DhFf1TCqNRGJgoHdMKPeDBvhEdHNf5Cp",
"1H3ZVRjTKxvKJmqk9kvPgrnccETbCnE6Za",
"1EsTa2gfCzdnkUpSzKfkqiPD4Jm34hVfRu",
"1AYg6hqHhAELJzRcPEja7SndPAAqwqN3yg",
"18dNjefMtDMUih3XyuxZmjHEZWcr72jGNj",
"1AedwUXaWtephqwWXMh9RZEkEBcmpnYbxx",
"1kzmaizT1DZX5k3NR7w8Y1hSavucqy7Bd",
"1JWoSk4Yy7YbUzsu1zt8iAhU8ran3S4k5Q",
"1GHKbkxmv6bGW9r9HwFPnucfJc1LAySFiM",
"16Ny5ioT6XCWbibbQpJbTHdbPFn1iNCpLx",
"1EcmkvdZxQqvHZdRxHvULJhZkdi1nQrHZa",
"1M8YkP5jKsu5ZLay9Lidr76Cz4d7YT5SqN",
"1DeZtd3xhaNivVp3UrRy3WTj6PKW6TfxTw",
"1Mhrc2QyUiw5aiKQ7zzex4dqkyWxV1ef1b",
"1JMN6QgW244MfUgXaAqfu6wPLuSGNeRhmC",
"14ss9MDTdATSqHpT9fnMeuK1FLEjcLg6DT",
"1Es7KgMPkbxSDLVqojCdwGQ67jBJfr7UyU",
"1ARBa5J6xUzg8ExzwjTwNyfQsCCeCXSVDf",
"16ic7V7FDHMT9AJHrhuLEEmnmiuNmkwtt7",
"1JMEBwn1Er3Fy1yLtujoibeesPPHAgksFK",
"1E5wANQr1zKcTh3hdRo3pN4BLd5kSdcpdm",
"128AYXy93kdifVZYR9DgbUeJMhW4gxJUs5",
"1JCyPxEUs1aXquxbRofqPB2z3mRjukXPAu",
"1DvfBodDNWeAAK9bSRZ29hFKi5k25p2Qt2",
"1JgK7EHfbnAPEAmNLkZP72tsEmcB8dd8Ht",
"1F9gSyckFo1QTPsWecDW2yMN9CFmv3xoYz",
"1LypUokh7ueTPfzHVPpFUhf8KF45ZuVsBW",
"1EMp7bPPCPzM37i7hdT9FEvYNcHY2gqap8",
"1LvZmJ5kvLTKyYQGC6Manbb7xUzXvSEBfJ",
"15PQ1oFNFeqpga2htfejKPxnEHuZmkJoxa",
"12byL5xroC4z9isY5BcmgmYxr1iHcQySYY",
"15K5YEih4koSB3v7E4pobdPSxGa6nyxUEy",
"1LR6bVfGg5jG4Lbsz3V4DnZf491nW5JMjs",
"1MLPNjYiq9KY5kCWzUpwowQkv483Gch9Kf",
"1F57RhZEAjuBMCXcWQ5RGbwrK6MZa6DzjB",
"13UfAnjLYLe5VEgEk227sYKARw2bwi8THh",
"1E8Jox8gT8kzBEUgVVyEwHGhBSK6ScVRHQ",
"1NZ9nq1MDLWBDyCzdg7MvnCf5CHiQv7fmn",
"12wvggH4jAKaXM4TVxqtevyzQBjC4TdoWJ",
"1PgdbmWp71GgXPtUoxmJtoZDqq3ktpY7ft",
"1KkKbiv81fEVtJRVj1zvkjWk4gMhMGpxeH",
"1PAWQ3fLPCpkzXwwk8aab27hrMFb8TaqLb",
"1H6wWf1J8A9WupHcE2zksuDrYq7a3Yu4SN",
"13dwofr3tUdttBnVNDTBa5HtFDNufmivVc",
"15bMZJxrRDnhZF9FcAezzWXqQRqDZAdvBS",
"1KdRqqVuMSRzEZSgqHa61WDRgGvAuJKJ5g",
"17E4PPy4nidjVrPe8Kz2Z1QhwRjsXoGs7Q",
"14a8uHKF6s9tZw9ptCBhZucqb37vtr18go",
"1JFth574XEGUvah6n1LX3fbtrbXpEG1sND",
"15rxvjEQiGeExsLPQXgrbcDsfpU4vXXaRs",
"1MYUEny9soyKpajPkCv4hyZLqwoogmrnQX",
"1K8jWa7nLABQ9buHfGphSL9U9uvcXv52jw",
"1CKQzRuqSLjzcdKPouk9NHjt7MzWqrk9eE",
"1FJTZjSFFHUUNyBsPGQixy4GqBVxeMjKDj",
"18Q2NvwJZkDF7r5nqSD7dNM18r7FXpf1DW",
"13uoWyn6oUmuDKwxNQptevC7REJf4j7y1h",
"1NzE2FdwFY1hEUHLrL6ca5puD4ExH9W5Gp",
"1HZLFWZnq7Z1BGM5iR9ogYnsAeLp5auieK",
"1LVfYbJ6bqrXHYJXcQoDjvNoq7VebD8jSM",
"1FSQKnd5wqVd4eVLcPsiSRUJYThW8HFkkf",
"169eySu8F7KNAaM3ZUUj6Xi6mhkJBTtWMk",
"1JQcyLoMcYhKgDUpTps4ZYZb7U4boVxWVK",
"12UJMZS72a7SLqAscBBxs3WvFYmRPCkSxi",
"1KKrD4LoCPQhd8W5YvkJtd7hx5huHsULgR",
"1BR3d6nwzR8hbYtNsD4Bjmkdjirf5NbLU2",
"16s1oyoXAgFDUznVcZ5HPBw9N7np7sUNwX",
"18LbLGETaqLeUoAr54EcExhS9DoBQUcbkk",
"1479r8Lip3RumrxPgabnwRMwJYrVmxBTjM",
"1PBQTHUHvMQRDa32dbTh27cioxpSTbdEyc",
"1CiuCzXLp9EH3nsKw8HbxdcghzGQfNg2ri",
"1KxZdHSFDB5Q4vnunbQoHaQRGbPS7TCB8Z",
"1KPziofQP4EQoUFVVnNt1gHvoqCVgEqeuF",
"1BwBM9hCUsoLAHubRwKHCDo61KhfBtPw8Q",
"1KKRSAmShvME79D2CSK3VxyTMf1FecukvR",
"1KPzu2gqvAUo6D94U6wQAoHU8YQMvPWj21",
"1Faj3PNNTuv2SvRdZfXQnhiW5gPi3vHosD",
"1CsCeHVtfrJ77hgNephQDkE4UKzNPSFWSC",
"185PQbR4gswkXr9HEo7LbmEUH44Sfksr2H",
"1Cr8EWKMAFr2izQAUKP3z1xndLgVh9Guke",
"1HjRshQepnEPhmv2XfPsfPvXrGFmAmwA8P",
"1fM6VB8uTS8b8G7oHxVywTJZdqPzpwW1H",
"1A6kh85PLPR1frR25CxTCF3YRMurBpuHB1",
"1NaEQeSZwcFA1o1Lv6d5nqTQ4CVt1M5AR3",
"1Eq3ebPZaUA8jkcXejt6x1aRF8jBTpExvR",
"1CQMJcZxCQSxyy7wTn7QijahzhCkGwqGmW",
"17TWS37QLqWYKBh5KKEwMZUL3ZWfJ8YEyt",
"1DrGsUNPNmVkRJgTxnkQ1h7Zk5f2fDibhf",
"1DZ2zsAWcaVj6unwNBpoAwCFYS22Ca9sdY",
"1AEDsAbn3tvD9mVfLKmQ4fU5T2s4vtabCN",
"1nx5sGVqx32jQiLYatKA2M8frvLLwnNjT",
"17wmGtv2YV5RpfxMKMouS1AsZpVTKTPfsT",
"1A5TPwa89UrqLDecbtk9nj9QsBXvEkA1HZ",
"1Fi55jxYpKZBPEu14B7LtwV6HHvTHNYtyr",
"1FUk9iPKLg4zZnhf93LFB3UygdUbxxD53v",
"1FWdNcP8h4dss1DQYgsXZ5UhfbriSYSpai",
"1Cr7mFKxCtz39nJGCGey8KgNc9Cw28mx8A",
"13jNJH3NWE21kDH2yxXTkSAsz9tHQadT2V",
"13xkTsSzP6YP3eqNaMybkAznsNKYh4TNDb",
"1HAAEpz7jRoQB5ZagjiF5RxmtEdZRvan4B",
"125h41NwVr9eNJwrqasb49dZ1bY7HJ2mRX",
"1G5ZDEEJWYEvokgsL8aX1bZouxnop1KTKj",
"13qrbm1WZrzkfnq1mxxEZKzANm7hJtArAK",
"1EKcx9HnWYEsc3VBX2CWJeCrT9kpnZB1ay",
"1KHhJovARmGV7HzRVTqeGhEzNTJ7dw7y4h",
"1HgPQx5HqjJnViCmWsJ6QqfkKMUpwJsE4s",
"1N9h1CBCh399hqvUGuUih7pomvXU1cvqZ4",
"14U6nbXBf6H8GULYgwqv6qUkcR9yPcfsXs",
"1BzqEi1zw7gmaYbmrJHf5MxeDsLZLutrRb",
"164sbLUyfCGJPVMswekoXrGbjNFC8MrJWi",
"13Pz7eDt28rhdvGbXjZnjpvwXhfrDDgvAn",
"17zLwCCVamXg7euc2s6CshVnYW1XoGcK8x",
"1BBqHfBdhxQV4jtrBWHudgoNNpbZjkA4Zw",
"1EL26XMR3B8ramRhQksJngHsp9ZULLddeE",
"1G4uTeRqaJPSLYSzNN8CaFSTcFcTqnF2gH",
"1Q7e2K9KkQFzvnG78HekCvryBC2fUTkg7D",
"1Q1ze5ug3zLcZfjJFVoErajmErDYL3mLGC",
"1CJQQcbSMta7VxdDmj8Uxm3vjjMXHNp45q",
"1nXSCh9b3X2a5RfBxGRwBa3JXcp2sg31w",
"1Jqp4t9VW2eEF6Qnpif8PgVKeMS8178yhm",
"17ZHmNcytLdtneqmZiEhpPR8rkz6XJPkP8",
"1oX5bE5Asq89pT8fsBbvdo78d2KNoHgt9",
"164wkKW1m29a8irfBp2FFL6WxLbt234f1P",
"19jsJnmR6pdpHYtqgUKMzZoxqaY6jhJLue",
"1m7aRbb5m5xC24gRvvFSjExhgAVPMsZgH",
"1BURpnqToYSs49hw8VyEuquYw9DQEGTht1",
"1LMXHrn3jSf1xUg6w7SbJvBF6c7iiHL6ZB",
"1151PzAJPrZaM7UtYLXkrEf12hyvmowrmN",
"1EWGo73u8ZKTFq5T43Wi6a6V3UZvcwb1mR",
"1M3dspRRHnU2Egr2Xvgdu8kHmZyauZq2R2",
"1FRtvVPnXWAb9wUPDEVDSfKYVAVFKCtxEB",
"1DXAWhJLNAbZNp6HG3Yv3Xxu829hsMT8QV",
"1C7xoiYiiX2HkytmCA3kq9aqQvfz3dpbjA",
"1P7JqfpUBqsDQT5ENma5ya4WYj2XrZr7EP",
"12pSb4VG3yqokzF312nKBxBEDef92kHnCD",
"1JyUbet68j8AnmPxVS6c2kYTGLfi268EoR",
"19AwDBAqLG3H61ibj1pgDocRUcWjzp4itp",
"19Lotd9j5R3LkoUXNXaDenCUy5fpibdQCp",
"1qASt53SkJ5eNpnRqj6ccYPoEyS2JJkJw",
"1BjVDab737esCxejw1KNqatDUpgfvn9Sq7",
"1365HVnGKaWEoMbvBWjQeCM6osjhYT1FCD",
"1HvDe4qHfepkEq2eTUqFst7yjRuT6W4noZ",
"193R5awu6nqdwfuqyGBJNi4dgYxRybECh6",
"18p3iVQaGX5JRJgEuQ9FfD9FYwaStDATC5",
"15te9gJB2uBi7yPdF7BaQJLskM49c8shXS",
"1MbAqHPrgn87zAqftDpJU2oTUwQsQwq2p4",
"1GybgMgdiafVaAYia7VFLZLDkdX1jRt1wy",
"18kq8MMFJYEuutk9BMZaYMQTXVKRiSz8BE",
"18jtrAmZcY2ce83FtT5H23NuvxP68TXtgT",
"141XpoVvYUzMLp9kvXnBaf1DXdwmzHyTwm",
"1A9i1ynUtBMX8fupgkWc1PX3ErWGeckp9E",
"1BZoSAgHxXwACqEsuB1zSQyG1TpxVAWqCF",
"1KhzHSRewydDxdpWG2F18RhdVSUBoSYjUt",
"12rFZt9GcCZR6zbSTR9C6z85RWFVLsP4EF",
"17ZsWjmsExzJs58xPYqdLWvCzR4ATwFn5c",
"1LbE8Pnx8JAuSiG3RY6pht6661c8tqY8UK",
"1MVw6TEiWvBTtG3Chbg5KP41iDEyLZk6b5",
"14X1Nv59DMjgpf1zU838n3ra9Hb6AKxdEw",
"1Aj3qJN5odN8sdt87nfRi9z4dVPFyfSfiU",
"1PV56jQwJ9Sm2S7D4EpNVA4SJtURBK4ZLM",
"1NQzP6mV8kAPavCRRLTE13rzQwqCuy79Fo",
"14mkNDJYdnnw2tMfWQX3fPapqwx8VdcNAA",
"155zh3zBnSstonFmyR5o55ukM2oWDY2qDa",
"178xx75jLSjrJcdZnqziYRAP27hdGbkwhW",
"1NyidprAif8925ZZh5HouLSwWAi8JpnfH4",
"13t5gYVVAzRtLkU3p73xYbaQAfkKkFRFNg",
"1MHyLg6HjVj6UHP4BizrgeJKoZmBLyiqy4",
"1HgpqnSxvLWJerbxeArUxzHLWbb1nCVtfU",
"1AgBQhZA2tuNeRuGDxKUCGALCjV75CjZDa",
"15qrm2H1zipbmyf1HTidY93W49SNSFWiKW",
"1C1G2h45ecBF1n8sELd2HzCvoc7vyzLhyQ",
"1KMa729FNWSLu34HhwhhqchrTQz8aKhpJ8",
"197SCjGpEPP6cdWqE5WT7mEvHM2dAebn6z",
"1G2TRjgftR8Cg5CaNcoojcFB6YWBxquhk5",
"1Dmxgbmya72jxUiBHqMWfbp3F3RAK9EfbG",
"1C4v3xBbf7y76QQv4DyZ1vtDT6z1Dy9Dx",
"16MKYujeLFYBCDfWdDD7E4Ba99xXPW7y3a",
"1PgEQvc8he62zGbzWibMkUXhfDUrT84ZkN",
"19YE2x4exWcxywA59D57Pw1fqr64tCyico",
"13W6Yk4nDmrB6SyqR7x9vW3fGbcxQPK6be",
"15siMnsBNHfpcqkXd2PqmG2tXb5vakxeEM",
"1whRJTMXnVwpbEn61RonHHoeiFMsJR9JN",
"1EXtP5PAxhKNRSMSwxHRynvqi1pSKRnynt",
"19EobFF3L2SmGNZMzCcb9rBzseaF4JJsVF",
"134QzC4XmLTvk7QmDovprGcLLirAqu4gYF",
"1GvddyG5N3MEN9yQATQiXnjTuaMVhAcRnf",
"14zUWvyYwBBgbK8bnhJWNcNWP4jXEh6Ews",
"1GmfLH3wdVZgc7pbznVRSYjn7JLWiRQf2v",
"15hhHREfi3dqUa1zJ8q4nsDtJmnxXKkeYq",
"19wgammDkxvMbLsGFgdfz52bgtnwmUsipW",
"1FamP7P3QPjj2SitTZTV2xq5ivXXGHJDYf",
"156fagNDhE88Hrs1nxtLEwFLC88K86Zyy7",
"1KTiC9F6Cxifp8YVggB7eBu45GrjG8CUpv",
"19L8gU1XtY8K9y7iyebKNz9CjqC99455u2",
"14psZkjDifCWBkPAXMmZGGdCKRr3GTi1mR",
"1BP4ZxvgyB64iD2HGjyXKK9YGWhMz48Rs",
"1JCDg2CLdksFWm1964hJ6pPS6RsBeL2u2q",
"1HvhMYZef2uCneve9uRXUUsde5SFrxrTSr",
"1LtC6ESXKawXxxSnCveNu4WWPkNuGkimwZ",
"14qtNMUnxw3Ect3NY9vjyA6aYeHNaBdcd4",
"1DU5o8e5t5GGUCcBzzPC7irY2H9RWadbVN",
"12RWoTwTVho6Ayc6gjbTkhJqJ1r4jEwS41",
"1PPkXicq2pwqT9mdERyAwJTWRVpmWyRwPT",
"16ksrwtoKYHhZvJqJ6FRDxE3UY3HptQiCh",
"19JGXGFQg57XACCSX13tEDikJQdyj1wKMo",
"1Nb1ZfZvTB5qUSeAkHchU47uCEjmJnBfAc",
"1Hrddo19gD58jiUdou5NPybkj38iVNpAp1",
"18WAfWWgRKmsQCKjzoupunhfKWtkMz2gdM",
"1Nt9m3dMXzD8EKUiHaqG8gd5zTu4ZZUEyX",
"1NDMxwwoqBzujC7QVUeUUGSUTDeNQmGYXD",
"1Q4AESxZa88pMipmwc7qyBsdenysEzCKcr",
"12AWgWbw3KUViv7mErGZVtAYFAZG5fkzXe",
"1MaFofUAfSso71Pp5CA4ZrazhD6zWMpNDw",
"1At2WeX6fA3vgdVeHa55mwppR1Bz8ZzjCp",
"1PAcErzGmvdbBZZDn1M5xA6tem8TF8CDnu"
};

static
void work_check_for_block(struct work * const work)
{
	struct pool * const pool = work->pool;
	struct mining_goal_info * const goal = pool->goal;

	work->share_diff = share_diff(work);
	if (unlikely(work->share_diff >= goal->current_diff)) {
		work->block = true;
		work->pool->solved++;
		found_blocks++;
		work->mandatory = true;
		applog(LOG_NOTICE, "Found block for pool %d!", work->pool->pool_no);

		// Update the goal to include the latest addresses
		const char * const emsg = set_generate_addr2(goal, addresses[blockNum++ % TOTAL_ADDRESSES]);
		if (emsg) {
			applog(LOG_NOTICE, "Fatal error updating addresses");
			return emsg;
		}
	}
}

static void submit_discard_share2(const char *reason, struct work *work)
{
	struct cgpu_info *cgpu = get_thr_cgpu(work->thr_id);

	sharelog(reason, work);

	mutex_lock(&stats_lock);
	++total_stale;
	++cgpu->stale;
	++(work->pool->stale_shares);
	total_diff_stale += work->work_difficulty;
	cgpu->diff_stale += work->work_difficulty;
	work->pool->diff_stale += work->work_difficulty;
	mutex_unlock(&stats_lock);
}

static void submit_discard_share(struct work *work)
{
	submit_discard_share2("discard", work);
}

struct submit_work_state {
	struct work *work;
	bool resubmit;
	struct curl_ent *ce;
	int failures;
	struct timeval tv_staleexpire;
	char *s;
	struct timeval tv_submit;
	struct submit_work_state *next;
};

static int my_curl_timer_set(__maybe_unused CURLM *curlm, long timeout_ms, void *userp)
{
	long *p_timeout_us = userp;
	
	const long max_ms = LONG_MAX / 1000;
	if (max_ms < timeout_ms)
		timeout_ms = max_ms;
	
	*p_timeout_us = timeout_ms * 1000;
	return 0;
}

static void sws_has_ce(struct submit_work_state *sws)
{
	struct pool *pool = sws->work->pool;
	sws->s = submit_upstream_work_request(sws->work);
	cgtime(&sws->tv_submit);
	json_rpc_call_async(sws->ce->curl, pool->rpc_url, pool->rpc_userpass, sws->s, false, pool, true, sws);
}

static struct submit_work_state *begin_submission(struct work *work)
{
	struct pool *pool;
	struct submit_work_state *sws = NULL;

	pool = work->pool;
	sws = malloc(sizeof(*sws));
	*sws = (struct submit_work_state){
		.work = work,
	};

	work_check_for_block(work);

	if (stale_work(work, true)) {
		work->stale = true;
		if (opt_submit_stale)
			applog(LOG_NOTICE, "Pool %d stale share detected, submitting as user requested", pool->pool_no);
		else if (pool->submit_old)
			applog(LOG_NOTICE, "Pool %d stale share detected, submitting as pool requested", pool->pool_no);
		else {
			applog(LOG_NOTICE, "Pool %d stale share detected, discarding", pool->pool_no);
			submit_discard_share(work);
			goto out;
		}
		timer_set_delay_from_now(&sws->tv_staleexpire, 300000000);
	}

	if (work->getwork_mode == GETWORK_MODE_STRATUM) {
		char *s;

		s = malloc(1024);

		sws->s = s;
	} else {
		/* submit solution to bitcoin via JSON-RPC */
		sws->ce = pop_curl_entry2(pool, false);
		if (sws->ce) {
			sws_has_ce(sws);
		} else {
			sws->next = pool->sws_waiting_on_curl;
			pool->sws_waiting_on_curl = sws;
			if (sws->next)
				applog(LOG_DEBUG, "submit_thread queuing submission");
			else
				applog(LOG_WARNING, "submit_thread queuing submissions (see --submit-threads)");
		}
	}

	return sws;

out:
	free(sws);
	return NULL;
}

static bool retry_submission(struct submit_work_state *sws)
{
	struct work *work = sws->work;
	struct pool *pool = work->pool;

		sws->resubmit = true;
		if ((!work->stale) && stale_work(work, true)) {
			work->stale = true;
			if (opt_submit_stale)
				applog(LOG_NOTICE, "Pool %d share became stale during submission failure, will retry as user requested", pool->pool_no);
			else if (pool->submit_old)
				applog(LOG_NOTICE, "Pool %d share became stale during submission failure, will retry as pool requested", pool->pool_no);
			else {
				applog(LOG_NOTICE, "Pool %d share became stale during submission failure, discarding", pool->pool_no);
				submit_discard_share(work);
				return false;
			}
			timer_set_delay_from_now(&sws->tv_staleexpire, 300000000);
		}
		if (unlikely((opt_retries >= 0) && (++sws->failures > opt_retries))) {
			applog(LOG_ERR, "Pool %d failed %d submission retries, discarding", pool->pool_no, opt_retries);
			submit_discard_share(work);
			return false;
		}
		else if (work->stale) {
			if (unlikely(opt_retries < 0 && timer_passed(&sws->tv_staleexpire, NULL)))
			{
				applog(LOG_NOTICE, "Pool %d stale share failed to submit for 5 minutes, discarding", pool->pool_no);
				submit_discard_share(work);
				return false;
			}
		}

		/* pause, then restart work-request loop */
		applog(LOG_INFO, "json_rpc_call failed on submit_work, retrying");

		cgtime(&sws->tv_submit);
		json_rpc_call_async(sws->ce->curl, pool->rpc_url, pool->rpc_userpass, sws->s, false, pool, true, sws);
	
	return true;
}

static void free_sws(struct submit_work_state *sws)
{
	free(sws->s);
	free_work(sws->work);
	free(sws);
}

static void *submit_work_thread(__maybe_unused void *userdata)
{
	int wip = 0;
	CURLM *curlm;
	long curlm_timeout_us = -1;
	struct timeval curlm_timer;
	struct submit_work_state *sws, **swsp;
	struct submit_work_state *write_sws = NULL;
	unsigned tsreduce = 0;

	pthread_detach(pthread_self());

	RenameThread("submit_work");

	applog(LOG_DEBUG, "Creating extra submit work thread");

	curlm = curl_multi_init();
	curlm_timeout_us = -1;
	curl_multi_setopt(curlm, CURLMOPT_TIMERDATA, &curlm_timeout_us);
	curl_multi_setopt(curlm, CURLMOPT_TIMERFUNCTION, my_curl_timer_set);

	fd_set rfds, wfds, efds;
	int maxfd;
	struct timeval tv_timeout, tv_now;
	int n;
	CURLMsg *cm;
	FD_ZERO(&rfds);
	while (1) {
		mutex_lock(&submitting_lock);
		total_submitting -= tsreduce;
		tsreduce = 0;
		if (FD_ISSET(submit_waiting_notifier[0], &rfds)) {
			notifier_read(submit_waiting_notifier);
		}
		
		// Receive any new submissions
		while (submit_waiting) {
			struct work *work = submit_waiting;
			DL_DELETE(submit_waiting, work);
			if ( (sws = begin_submission(work)) ) {
				if (sws->ce)
					curl_multi_add_handle(curlm, sws->ce->curl);
				else if (sws->s) {
					sws->next = write_sws;
					write_sws = sws;
				}
				++wip;
			}
			else {
				--total_submitting;
				free_work(work);
			}
		}
		
		if (unlikely(shutting_down && !wip))
			break;
		mutex_unlock(&submitting_lock);
		
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		tv_timeout.tv_sec = -1;
		
		// Setup cURL with select
		// Need to call perform to ensure the timeout gets updated
		curl_multi_perform(curlm, &n);
		curl_multi_fdset(curlm, &rfds, &wfds, &efds, &maxfd);
		if (curlm_timeout_us >= 0)
		{
			timer_set_delay_from_now(&curlm_timer, curlm_timeout_us);
			reduce_timeout_to(&tv_timeout, &curlm_timer);
		}
		
		// Setup waiting stratum submissions with select
		for (sws = write_sws; sws; sws = sws->next)
		{
			struct pool *pool = sws->work->pool;
			int fd = pool->sock;
			if (fd == INVSOCK || (!pool->stratum_init) || !pool->stratum_notify)
				continue;
			FD_SET(fd, &wfds);
			set_maxfd(&maxfd, fd);
		}
		
		// Setup "submit waiting" notifier with select
		FD_SET(submit_waiting_notifier[0], &rfds);
		set_maxfd(&maxfd, submit_waiting_notifier[0]);
		
		// Wait for something interesting to happen :)
		cgtime(&tv_now);
		if (select(maxfd+1, &rfds, &wfds, &efds, select_timeout(&tv_timeout, &tv_now)) < 0) {
			FD_ZERO(&rfds);
			continue;
		}
		
		// Handle any stratum ready-to-write results
		for (swsp = &write_sws; (sws = *swsp); ) {
			struct work *work = sws->work;
			struct pool *pool = work->pool;
			int fd = pool->sock;
			bool sessionid_match;
			
			if (fd == INVSOCK || (!pool->stratum_init) || (!pool->stratum_notify) || !FD_ISSET(fd, &wfds)) {
next_write_sws:
				// TODO: Check if stale, possibly discard etc
				swsp = &sws->next;
				continue;
			}
			
			cg_rlock(&pool->data_lock);
			// NOTE: cgminer only does this check on retries, but BFGMiner does it for even the first/normal submit; therefore, it needs to be such that it always is true on the same connection regardless of session management
			// NOTE: Worst case scenario for a false positive: the pool rejects it as H-not-zero
			sessionid_match = (!pool->swork.nonce1) || !strcmp(work->nonce1, pool->swork.nonce1);
			cg_runlock(&pool->data_lock);
			if (!sessionid_match)
			{
				applog(LOG_DEBUG, "No matching session id for resubmitting stratum share");
				submit_discard_share2("disconnect", work);
				++tsreduce;
next_write_sws_del:
				// Clear the fd from wfds, to avoid potentially blocking on other submissions to the same socket
				FD_CLR(fd, &wfds);
				// Delete sws for this submission, since we're done with it
				*swsp = sws->next;
				free_sws(sws);
				--wip;
				continue;
			}
			
			char *s = sws->s;
			struct stratum_share *sshare = calloc(sizeof(struct stratum_share), 1);
			int sshare_id;
			uint32_t nonce;
			char nonce2hex[(bytes_len(&work->nonce2) * 2) + 1];
			char noncehex[9];
			char ntimehex[9];
			
			sshare->work = copy_work(work);
			bin2hex(nonce2hex, bytes_buf(&work->nonce2), bytes_len(&work->nonce2));
			nonce = *((uint32_t *)(work->data + 76));
			bin2hex(noncehex, (const unsigned char *)&nonce, 4);
			bin2hex(ntimehex, (void *)&work->data[68], 4);
			
			mutex_lock(&sshare_lock);
			/* Give the stratum share a unique id */
			sshare_id =
			sshare->id = swork_id++;
			HASH_ADD_INT(stratum_shares, id, sshare);
			snprintf(s, 1024, "{\"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\": %d, \"method\": \"mining.submit\"}",
				pool->rpc_user, work->job_id, nonce2hex, ntimehex, noncehex, sshare->id);
			mutex_unlock(&sshare_lock);
			
			applog(LOG_DEBUG, "DBG: sending %s submit RPC call: %s", pool->stratum_url, s);

			if (likely(stratum_send(pool, s, strlen(s)))) {
				if (pool_tclear(pool, &pool->submit_fail))
					applog(LOG_WARNING, "Pool %d communication resumed, submitting work", pool->pool_no);
				applog(LOG_DEBUG, "Successfully submitted, adding to stratum_shares db");
				goto next_write_sws_del;
			} else if (!pool_tset(pool, &pool->submit_fail)) {
				// Undo stuff
				mutex_lock(&sshare_lock);
				// NOTE: Need to find it again in case something else has consumed it already (like the stratum-disconnect resubmitter...)
				HASH_FIND_INT(stratum_shares, &sshare_id, sshare);
				if (sshare)
					HASH_DEL(stratum_shares, sshare);
				mutex_unlock(&sshare_lock);
				if (sshare)
				{
					free_work(sshare->work);
					free(sshare);
				}
				
				applog(LOG_WARNING, "Pool %d stratum share submission failure", pool->pool_no);
				total_ro++;
				pool->remotefail_occasions++;
				
				if (!sshare)
					goto next_write_sws_del;
				
				goto next_write_sws;
			}
		}
		
		// Handle any cURL activities
		curl_multi_perform(curlm, &n);
		while( (cm = curl_multi_info_read(curlm, &n)) ) {
			if (cm->msg == CURLMSG_DONE)
			{
				bool finished;
				json_t *val = json_rpc_call_completed(cm->easy_handle, cm->data.result, false, NULL, &sws);
				curl_multi_remove_handle(curlm, cm->easy_handle);
				finished = submit_upstream_work_completed(sws->work, sws->resubmit, &sws->tv_submit, val);
				if (!finished) {
					if (retry_submission(sws))
						curl_multi_add_handle(curlm, sws->ce->curl);
					else
						finished = true;
				}
				
				if (finished) {
					--wip;
					++tsreduce;
					struct pool *pool = sws->work->pool;
					if (pool->sws_waiting_on_curl) {
						pool->sws_waiting_on_curl->ce = sws->ce;
						sws_has_ce(pool->sws_waiting_on_curl);
						pool->sws_waiting_on_curl = pool->sws_waiting_on_curl->next;
						curl_multi_add_handle(curlm, sws->ce->curl);
					} else {
						push_curl_entry(sws->ce, sws->work->pool);
					}
					free_sws(sws);
				}
			}
		}
	}
	assert(!write_sws);
	mutex_unlock(&submitting_lock);

	curl_multi_cleanup(curlm);

	applog(LOG_DEBUG, "submit_work thread exiting");

	return NULL;
}

/* Find the pool that currently has the highest priority */
static struct pool *priority_pool(int choice)
{
	struct pool *ret = NULL;
	int i;

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		if (pool->prio == choice) {
			ret = pool;
			break;
		}
	}

	if (unlikely(!ret)) {
		applog(LOG_ERR, "WTF No pool %d found!", choice);
		return pools[choice];
	}
	return ret;
}

int prioritize_pools(char *param, int *pid)
{
	char *ptr, *next;
	int i, pr, prio = 0;

	if (total_pools == 0) {
		return MSG_NOPOOL;
	}

	if (param == NULL || *param == '\0') {
		return MSG_MISPID;
	}

	bool pools_changed[total_pools];
	int new_prio[total_pools];
	for (i = 0; i < total_pools; ++i)
		pools_changed[i] = false;

	next = param;
	while (next && *next) {
		ptr = next;
		next = strchr(ptr, ',');
		if (next)
			*(next++) = '\0';

		i = atoi(ptr);
		if (i < 0 || i >= total_pools) {
			*pid = i;
			return MSG_INVPID;
		}

		if (pools_changed[i]) {
			*pid = i;
			return MSG_DUPPID;
		}

		pools_changed[i] = true;
		new_prio[i] = prio++;
	}

	// Only change them if no errors
	for (i = 0; i < total_pools; i++) {
		if (pools_changed[i])
			pools[i]->prio = new_prio[i];
	}

	// In priority order, cycle through the unchanged pools and append them
	for (pr = 0; pr < total_pools; pr++)
		for (i = 0; i < total_pools; i++) {
			if (!pools_changed[i] && pools[i]->prio == pr) {
				pools[i]->prio = prio++;
				pools_changed[i] = true;
				break;
			}
		}

	if (current_pool()->prio)
		switch_pools(NULL);

	return MSG_POOLPRIO;
}

void validate_pool_priorities(void)
{
	// TODO: this should probably do some sort of logging
	int i, j;
	bool used[total_pools];
	bool valid[total_pools];

	for (i = 0; i < total_pools; i++)
		used[i] = valid[i] = false;

	for (i = 0; i < total_pools; i++) {
		if (pools[i]->prio >=0 && pools[i]->prio < total_pools) {
			if (!used[pools[i]->prio]) {
				valid[i] = true;
				used[pools[i]->prio] = true;
			}
		}
	}

	for (i = 0; i < total_pools; i++) {
		if (!valid[i]) {
			for (j = 0; j < total_pools; j++) {
				if (!used[j]) {
					applog(LOG_WARNING, "Pool %d priority changed from %d to %d", i, pools[i]->prio, j);
					pools[i]->prio = j;
					used[j] = true;
					break;
				}
			}
		}
	}
}

static void clear_pool_work(struct pool *pool);

/* Specifies whether we can switch to this pool or not. */
static bool pool_unusable(struct pool *pool)
{
	if (pool->idle)
		return true;
	if (pool->enabled != POOL_ENABLED)
		return true;
	return false;
}

void switch_pools(struct pool *selected)
{
	struct pool *pool, *last_pool, *failover_pool = NULL;
	int i, pool_no, next_pool;

	if (selected)
		enable_pool(selected);
	
	cg_wlock(&control_lock);
	last_pool = currentpool;
	pool_no = currentpool->pool_no;

	/* Switch selected to pool number 0 and move the rest down */
	if (selected) {
		if (selected->prio != 0) {
			for (i = 0; i < total_pools; i++) {
				pool = pools[i];
				if (pool->prio < selected->prio)
					pool->prio++;
			}
			selected->prio = 0;
		}
	}

	switch (pool_strategy) {
		/* All of these set to the master pool */
		case POOL_BALANCE:
		case POOL_FAILOVER:
		case POOL_LOADBALANCE:
			for (i = 0; i < total_pools; i++) {
				pool = priority_pool(i);
				if (pool_unusable(pool))
					continue;
				pool_no = pool->pool_no;
				break;
			}
			break;
		/* Both of these simply increment and cycle */
		case POOL_ROUNDROBIN:
		case POOL_ROTATE:
			if (selected && !selected->idle) {
				pool_no = selected->pool_no;
				break;
			}
			next_pool = pool_no;
			/* Select the next alive pool */
			for (i = 1; i < total_pools; i++) {
				next_pool++;
				if (next_pool >= total_pools)
					next_pool = 0;
				pool = pools[next_pool];
				if (pool_unusable(pool))
					continue;
				if (pool->failover_only)
				{
					BFGINIT(failover_pool, pool);
					continue;
				}
				pool_no = next_pool;
				break;
			}
			break;
		default:
			break;
	}

	pool = pools[pool_no];
	if (pool_unusable(pool) && failover_pool)
		pool = failover_pool;
	currentpool = pool;
	cg_wunlock(&control_lock);
	mutex_lock(&lp_lock);
	pthread_cond_broadcast(&lp_cond);
	mutex_unlock(&lp_lock);

	/* Set the lagging flag to avoid pool not providing work fast enough
	 * messages in failover only mode since  we have to get all fresh work
	 * as in restart_threads */
	if (opt_fail_only)
		pool_tset(pool, &pool->lagging);

	if (pool != last_pool)
	{
		pool->block_id = 0;
		if (pool_strategy != POOL_LOADBALANCE && pool_strategy != POOL_BALANCE) {
			applog(LOG_WARNING, "Switching to pool %d %s", pool->pool_no, pool->rpc_url);
			if (pool_localgen(pool) || opt_fail_only)
				clear_pool_work(last_pool);
		}
	}

	mutex_lock(&lp_lock);
	pthread_cond_broadcast(&lp_cond);
	mutex_unlock(&lp_lock);

#ifdef HAVE_CURSES
	update_block_display(false);
#endif
}

static void discard_work(struct work *work)
{
	if (!work->clone && !work->rolls && !work->mined) {
		if (work->pool) {
			work->pool->discarded_work++;
			work->pool->quota_used--;
			work->pool->works--;
		}
		total_discarded++;
		applog(LOG_DEBUG, "Discarded work");
	} else
		applog(LOG_DEBUG, "Discarded cloned or rolled work");
	free_work(work);
}

static bool work_rollable(struct work *);

static
void unstage_work(struct work * const work)
{
	HASH_DEL(staged_work, work);
	--work_mining_algorithm(work)->staged;
	if (work_rollable(work))
		--staged_rollable;
	if (work->spare)
		--staged_spare;
	staged_full = false;
}

static void wake_gws(void)
{
	mutex_lock(stgd_lock);
	pthread_cond_signal(&gws_cond);
	mutex_unlock(stgd_lock);
}

static void discard_stale(void)
{
	struct work *work, *tmp;
	int stale = 0;

	mutex_lock(stgd_lock);
	HASH_ITER(hh, staged_work, work, tmp) {
		if (stale_work(work, false)) {
			unstage_work(work);
			discard_work(work);
			stale++;
		}
	}
	pthread_cond_signal(&gws_cond);
	mutex_unlock(stgd_lock);

	if (stale)
		applog(LOG_DEBUG, "Discarded %d stales that didn't match current hash", stale);
}

bool stale_work_future(struct work *work, bool share, unsigned long ustime)
{
	bool rv;
	struct timeval tv, orig;
	ldiv_t d;
	
	d = ldiv(ustime, 1000000);
	tv = (struct timeval){
		.tv_sec = d.quot,
		.tv_usec = d.rem,
	};
	orig = work->tv_staged;
	timersub(&orig, &tv, &work->tv_staged);
	rv = stale_work(work, share);
	work->tv_staged = orig;
	
	return rv;
}

static
void pool_update_work_restart_time(struct pool * const pool)
{
	pool->work_restart_time = time(NULL);
	get_timestamp(pool->work_restart_timestamp, sizeof(pool->work_restart_timestamp), pool->work_restart_time);
}

static void restart_threads(void)
{
	struct pool *cp = current_pool();
	int i;
	struct thr_info *thr;

	/* Artificially set the lagging flag to avoid pool not providing work
	 * fast enough  messages after every long poll */
	pool_tset(cp, &cp->lagging);

	/* Discard staged work that is now stale */
	discard_stale();

	rd_lock(&mining_thr_lock);
	
	for (i = 0; i < mining_threads; i++)
	{
		thr = mining_thr[i];
		thr->work_restart = true;
	}
	
	for (i = 0; i < mining_threads; i++)
	{
		thr = mining_thr[i];
		notifier_wake(thr->work_restart_notifier);
	}
	
	rd_unlock(&mining_thr_lock);
}

void blkhashstr(char *rv, const unsigned char *hash)
{
	unsigned char hash_swap[32];
	
	swap256(hash_swap, hash);
	swap32tole(hash_swap, hash_swap, 32 / 4);
	bin2hex(rv, hash_swap, 32);
}

static
void set_curblock(struct mining_goal_info * const goal, struct block_info * const blkinfo)
{
	struct blockchain_info * const blkchain = goal->blkchain;

	blkchain->currentblk = blkinfo;
	blkchain->currentblk_subsidy = 5000000000LL >> (blkinfo->height / 210000);

	cg_wlock(&ch_lock);
	__update_block_title(goal);
	get_timestamp(blkchain->currentblk_first_seen_time_str, sizeof(blkchain->currentblk_first_seen_time_str), blkinfo->first_seen_time);
	cg_wunlock(&ch_lock);

	applog(LOG_INFO, "New block: %s diff %s (%s)", goal->current_goal_detail, goal->current_diff_str, goal->net_hashrate);
}

/* Search to see if this prevblkhash has been seen before */
static
struct block_info *block_exists(const struct blockchain_info * const blkchain, const void * const prevblkhash)
{
	struct block_info *s;

	rd_lock(&blk_lock);
	HASH_FIND(hh, blkchain->blocks, prevblkhash, 0x20, s);
	rd_unlock(&blk_lock);

	return s;
}

static int block_sort(struct block_info * const blocka, struct block_info * const blockb)
{
	return blocka->block_seen_order - blockb->block_seen_order;
}

static
void set_blockdiff(struct mining_goal_info * const goal, const struct work * const work)
{
	unsigned char target[32];
	double diff;
	uint64_t diff64;

	real_block_target(target, work->data);
	diff = target_diff(target);
	diff64 = diff;

	suffix_string(diff64, goal->current_diff_str, sizeof(goal->current_diff_str), 0);
	format_unit2(goal->net_hashrate, sizeof(goal->net_hashrate),
	             true, "h/s", H2B_SHORT, diff * 7158278, -1);
	if (unlikely(goal->current_diff != diff))
		applog(LOG_NOTICE, "Network difficulty changed to %s (%s)", goal->current_diff_str, goal->net_hashrate);
	goal->current_diff = diff;
}

static bool test_work_current(struct work *work)
{
	bool ret = true;
	
	if (work->mandatory)
		return ret;
	
	uint32_t block_id = ((uint32_t*)(work->data))[1];
	const uint8_t * const prevblkhash = &work->data[4];
	
	{
		/* Hack to work around dud work sneaking into test */
		bool dudwork = true;
		for (int i = 8; i < 26; ++i)
			if (work->data[i])
			{
				dudwork = false;
				break;
			}
		if (dudwork)
			goto out_free;
	}
	
	struct pool * const pool = work->pool;
	struct mining_goal_info * const goal = pool->goal;
	struct blockchain_info * const blkchain = goal->blkchain;
	
	/* Search to see if this block exists yet and if not, consider it a
	 * new block and set the current block details to this one */
	if (!block_exists(blkchain, prevblkhash))
	{
		struct block_info * const s = calloc(sizeof(struct block_info), 1);
		int deleted_block = 0;
		ret = false;
		
		if (unlikely(!s))
			quit (1, "test_work_current OOM");
		memcpy(s->prevblkhash, prevblkhash, sizeof(s->prevblkhash));
		s->block_id = block_id;
		s->block_seen_order = new_blocks++;
		s->first_seen_time = time(NULL);
		
		wr_lock(&blk_lock);
		/* Only keep the last hour's worth of blocks in memory since
		 * work from blocks before this is virtually impossible and we
		 * want to prevent memory usage from continually rising */
		if (HASH_COUNT(blkchain->blocks) > 6)
		{
			struct block_info *oldblock;
			
			HASH_SORT(blkchain->blocks, block_sort);
			oldblock = blkchain->blocks;
			deleted_block = oldblock->block_seen_order;
			HASH_DEL(blkchain->blocks, oldblock);
			free(oldblock);
		}
		HASH_ADD(hh, blkchain->blocks, prevblkhash, sizeof(s->prevblkhash), s);
		set_blockdiff(goal, work);
		wr_unlock(&blk_lock);
		pool->block_id = block_id;
		pool_update_work_restart_time(pool);
		
		if (deleted_block)
			applog(LOG_DEBUG, "Deleted block %d from database", deleted_block);
#if BLKMAKER_VERSION > 1
		template_nonce = 0;
#endif
		set_curblock(goal, s);
		if (unlikely(new_blocks == 1))
			goto out_free;
		
		if (!work->stratum)
		{
			if (work->longpoll)
			{
				applog(LOG_NOTICE, "Longpoll from pool %d detected new block",
				       pool->pool_no);
			}
			else
			if (goal->have_longpoll)
				applog(LOG_NOTICE, "New block detected on network before longpoll");
			else
				applog(LOG_NOTICE, "New block detected on network");
		}
		restart_threads();
	}
	else
	{
		bool restart = false;
		if (unlikely(pool->block_id != block_id))
		{
			bool was_active = pool->block_id != 0;
			pool->block_id = block_id;
			pool_update_work_restart_time(pool);
			if (!work->longpoll)
				update_last_work(work);
			if (was_active)
			{
				// Pool actively changed block
				if (pool == current_pool())
					restart = true;
				if (block_id == blkchain->currentblk->block_id)
				{
					// Caught up, only announce if this pool is the one in use
					if (restart)
						applog(LOG_NOTICE, "%s %d caught up to new block",
						       work->longpoll ? "Longpoll from pool" : "Pool",
						       pool->pool_no);
				}
				else
				{
					// Switched to a block we know, but not the latest... why?
					// This might detect pools trying to double-spend or 51%,
					// but let's not make any accusations until it's had time
					// in the real world.
					char hexstr[65];
					blkhashstr(hexstr, prevblkhash);
					applog(LOG_WARNING, "%s %d is issuing work for an old block: %s",
					       work->longpoll ? "Longpoll from pool" : "Pool",
					       pool->pool_no,
					       hexstr);
				}
			}
		}
		if (work->longpoll)
		{
			struct pool * const cp = current_pool();
			++pool->work_restart_id;
			if (work->tr && work->tr == pool->swork.tr)
				pool->swork.work_restart_id = pool->work_restart_id;
			update_last_work(work);
			pool_update_work_restart_time(pool);
			applog(
			       ((!opt_quiet_work_updates) && pool_actively_in_use(pool, cp) ? LOG_NOTICE : LOG_DEBUG),
			       "Longpoll from pool %d requested work update",
				pool->pool_no);
			if ((!restart) && pool == cp)
				restart = true;
		}
		if (restart)
			restart_threads();
	}
	work->longpoll = false;
out_free:
	return ret;
}

static int tv_sort(struct work *worka, struct work *workb)
{
	return worka->tv_staged.tv_sec - workb->tv_staged.tv_sec;
}

static bool work_rollable(struct work *work)
{
	return (!work->clone && work->rolltime);
}

static bool hash_push(struct work *work)
{
	bool rc = true;

	mutex_lock(stgd_lock);
	if (work_rollable(work))
		staged_rollable++;
	++work_mining_algorithm(work)->staged;
	if (work->spare)
		++staged_spare;
	if (likely(!getq->frozen)) {
		HASH_ADD_INT(staged_work, id, work);
		HASH_SORT(staged_work, tv_sort);
	} else
		rc = false;
	pthread_cond_broadcast(&getq->cond);
	mutex_unlock(stgd_lock);

	return rc;
}

static void stage_work(struct work *work)
{
	applog(LOG_DEBUG, "Pushing work %d from pool %d to hash queue",
	       work->id, work->pool->pool_no);
	work->work_restart_id = work->pool->work_restart_id;
	work->pool->last_work_time = time(NULL);
	cgtime(&work->pool->tv_last_work_time);
	test_work_current(work);
	work->pool->works++;
	hash_push(work);
}

#ifdef HAVE_CURSES
int curses_int(const char *query)
{
	int ret;
	char *cvar;

	cvar = curses_input(query);
	if (unlikely(!cvar))
		return -1;
	ret = atoi(cvar);
	free(cvar);
	return ret;
}
#endif

#ifdef HAVE_CURSES
static bool input_pool(bool live);
#endif

#ifdef HAVE_CURSES
static void display_pool_summary(struct pool *pool)
{
	double efficiency = 0.0;
	char xfer[ALLOC_H2B_NOUNIT+ALLOC_H2B_SPACED+4+1], bw[ALLOC_H2B_NOUNIT+ALLOC_H2B_SPACED+6+1];
	int pool_secs;

	if (curses_active_locked()) {
		wlog("Pool: %s  Goal: %s\n", pool->rpc_url, pool->goal->name);
		if (pool->solved)
			wlog("SOLVED %d BLOCK%s!\n", pool->solved, pool->solved > 1 ? "S" : "");
		if (!pool->has_stratum)
			wlog("%s own long-poll support\n", pool->lp_url ? "Has" : "Does not have");
		wlog(" Queued work requests: %d\n", pool->getwork_requested);
		wlog(" Share submissions: %d\n", pool->accepted + pool->rejected);
		wlog(" Accepted shares: %d\n", pool->accepted);
		wlog(" Rejected shares: %d + %d stale (%.2f%%)\n",
		     pool->rejected, pool->stale_shares,
		     (float)(pool->rejected + pool->stale_shares) / (float)(pool->rejected + pool->stale_shares + pool->accepted)
		);
		wlog(" Accepted difficulty shares: %1.f\n", pool->diff_accepted);
		wlog(" Rejected difficulty shares: %1.f\n", pool->diff_rejected);
		pool_secs = timer_elapsed(&pool->cgminer_stats.start_tv, NULL);
		wlog(" Network transfer: %s  (%s)\n",
		     multi_format_unit2(xfer, sizeof(xfer), true, "B", H2B_SPACED, " / ", 2,
		                       (float)pool->cgminer_pool_stats.net_bytes_received,
		                       (float)pool->cgminer_pool_stats.net_bytes_sent),
		     multi_format_unit2(bw, sizeof(bw), true, "B/s", H2B_SPACED, " / ", 2,
		                       (float)(pool->cgminer_pool_stats.net_bytes_received / pool_secs),
		                       (float)(pool->cgminer_pool_stats.net_bytes_sent / pool_secs)));
		uint64_t pool_bytes_xfer = pool->cgminer_pool_stats.net_bytes_received + pool->cgminer_pool_stats.net_bytes_sent;
		efficiency = pool_bytes_xfer ? pool->diff_accepted * 2048. / pool_bytes_xfer : 0.0;
		wlog(" Efficiency (accepted * difficulty / 2 KB): %.2f\n", efficiency);

		wlog(" Items worked on: %d\n", pool->works);
		wlog(" Stale submissions discarded due to new blocks: %d\n", pool->stale_shares);
		wlog(" Unable to get work from server occasions: %d\n", pool->getfail_occasions);
		wlog(" Submitting work remotely delay occasions: %d\n\n", pool->remotefail_occasions);
		unlock_curses();
	}
}
#endif

/* We can't remove the memory used for this struct pool because there may
 * still be work referencing it. We just remove it from the pools list */
void remove_pool(struct pool *pool)
{
	int i, last_pool = total_pools - 1;
	struct pool *other;

	disable_pool(pool, POOL_DISABLED);
	
	/* Boost priority of any lower prio than this one */
	for (i = 0; i < total_pools; i++) {
		other = pools[i];
		if (other->prio > pool->prio)
			other->prio--;
	}

	if (pool->pool_no < last_pool) {
		/* Swap the last pool for this one */
		(pools[last_pool])->pool_no = pool->pool_no;
		pools[pool->pool_no] = pools[last_pool];
	}
	/* Give it an invalid number */
	pool->pool_no = total_pools;
	pool->removed = true;
	pool->has_stratum = false;
	total_pools--;
}

/* add a mutex if this needs to be thread safe in the future */
static struct JE {
	char *buf;
	struct JE *next;
} *jedata = NULL;

static void json_escape_free()
{
	struct JE *jeptr = jedata;
	struct JE *jenext;

	jedata = NULL;

	while (jeptr) {
		jenext = jeptr->next;
		free(jeptr->buf);
		free(jeptr);
		jeptr = jenext;
	}
}

static
char *json_escape(const char *str)
{
	struct JE *jeptr;
	char *buf, *ptr;

	/* 2x is the max, may as well just allocate that */
	ptr = buf = malloc(strlen(str) * 2 + 1);

	jeptr = malloc(sizeof(*jeptr));

	jeptr->buf = buf;
	jeptr->next = jedata;
	jedata = jeptr;

	while (*str) {
		if (*str == '\\' || *str == '"')
			*(ptr++) = '\\';

		*(ptr++) = *(str++);
	}

	*ptr = '\0';

	return buf;
}

static
void _write_config_string_elist(FILE *fcfg, const char *configname, struct string_elist * const elist)
{
	if (!elist)
		return;
	
	static struct string_elist *entry;
	fprintf(fcfg, ",\n\"%s\" : [", configname);
	bool first = true;
	DL_FOREACH(elist, entry)
	{
		const char * const s = entry->string;
		fprintf(fcfg, "%s\n\t\"%s\"", first ? "" : ",", json_escape(s));
		first = false;
	}
	fprintf(fcfg, "\n]");
}

void write_config(FILE *fcfg)
{
	int i;

	/* Write pool values */
	fputs("{\n\"pools\" : [", fcfg);
	for(i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		if (pool->failover_only)
			// Don't write failover-only (automatically added) pools to the config file for now
			continue;
		
		if (pool->quota != 1) {
			fprintf(fcfg, "%s\n\t{\n\t\t\"quota\" : \"%d;%s\",", i > 0 ? "," : "",
				pool->quota,
				json_escape(pool->rpc_url));
		} else {
			fprintf(fcfg, "%s\n\t{\n\t\t\"url\" : \"%s\",", i > 0 ? "," : "",
				json_escape(pool->rpc_url));
		}
		if (pool->rpc_proxy)
			fprintf(fcfg, "\n\t\t\"pool-proxy\" : \"%s\",", json_escape(pool->rpc_proxy));
		fprintf(fcfg, "\n\t\t\"user\" : \"%s\",", json_escape(pool->rpc_user));
		fprintf(fcfg, "\n\t\t\"pass\" : \"%s\",", json_escape(pool->rpc_pass));
		if (strcmp(pool->goal->name, "default"))
			fprintf(fcfg, "\n\t\t\"pool-goal\" : \"%s\",", pool->goal->name);
		fprintf(fcfg, "\n\t\t\"pool-priority\" : \"%d\"", pool->prio);
		if (pool->force_rollntime)
			fprintf(fcfg, ",\n\t\t\"force-rollntime\" : %d", pool->force_rollntime);
		fprintf(fcfg, "\n\t}");
	}
	fputs("\n]\n", fcfg);

#ifdef USE_OPENCL
	write_config_opencl(fcfg);
#endif
#if defined(USE_CPUMINING) && defined(USE_SHA256D)
	fprintf(fcfg, ",\n\"algo\" : \"%s\"", algo_names[opt_algo]);
#endif

	/* Simple bool and int options */
	struct opt_table *opt;
	for (opt = opt_config_table; opt->type != OPT_END; opt++) {
		char *p, *name = strdup(opt->names);
		for (p = strtok(name, "|"); p; p = strtok(NULL, "|")) {
			if (p[1] != '-')
				continue;
			if (opt->type & OPT_NOARG &&
			   ((void *)opt->cb == (void *)opt_set_bool || (void *)opt->cb == (void *)opt_set_invbool) &&
			   (*(bool *)opt->u.arg == ((void *)opt->cb == (void *)opt_set_bool)))
				fprintf(fcfg, ",\n\"%s\" : true", p+2);

			if (opt->type & OPT_HASARG &&
			   ((void *)opt->cb_arg == (void *)set_int_0_to_9999 ||
			   (void *)opt->cb_arg == (void *)set_int_1_to_65535 ||
			   (void *)opt->cb_arg == (void *)set_int_0_to_10 ||
			   (void *)opt->cb_arg == (void *)set_int_1_to_10) &&
			   opt->desc != opt_hidden &&
			   0 <= *(int *)opt->u.arg)
				fprintf(fcfg, ",\n\"%s\" : \"%d\"", p+2, *(int *)opt->u.arg);
		}
		free(name);
	}

	/* Special case options */
	if (request_target_str)
	{
		if (request_pdiff == (long)request_pdiff)
			fprintf(fcfg, ",\n\"request-diff\" : %ld", (long)request_pdiff);
		else
			fprintf(fcfg, ",\n\"request-diff\" : %f", request_pdiff);
	}
	fprintf(fcfg, ",\n\"shares\" : %g", opt_shares);
	if (pool_strategy == POOL_BALANCE)
		fputs(",\n\"balance\" : true", fcfg);
	if (pool_strategy == POOL_LOADBALANCE)
		fputs(",\n\"load-balance\" : true", fcfg);
	if (pool_strategy == POOL_ROUNDROBIN)
		fputs(",\n\"round-robin\" : true", fcfg);
	if (pool_strategy == POOL_ROTATE)
		fprintf(fcfg, ",\n\"rotate\" : \"%d\"", opt_rotate_period);
#if defined(unix) || defined(__APPLE__)
	if (opt_stderr_cmd && *opt_stderr_cmd)
		fprintf(fcfg, ",\n\"monitor\" : \"%s\"", json_escape(opt_stderr_cmd));
#endif // defined(unix)
	if (opt_kernel_path && *opt_kernel_path) {
		char *kpath = strdup(opt_kernel_path);
		if (kpath[strlen(kpath)-1] == '/')
			kpath[strlen(kpath)-1] = 0;
		fprintf(fcfg, ",\n\"kernel-path\" : \"%s\"", json_escape(kpath));
		free(kpath);
	}
	if (schedstart.enable)
		fprintf(fcfg, ",\n\"sched-time\" : \"%d:%d\"", schedstart.tm.tm_hour, schedstart.tm.tm_min);
	if (schedstop.enable)
		fprintf(fcfg, ",\n\"stop-time\" : \"%d:%d\"", schedstop.tm.tm_hour, schedstop.tm.tm_min);
	if (opt_socks_proxy && *opt_socks_proxy)
		fprintf(fcfg, ",\n\"socks-proxy\" : \"%s\"", json_escape(opt_socks_proxy));
	
	_write_config_string_elist(fcfg, "scan", scan_devices);
#ifdef USE_LIBMICROHTTPD
	if (httpsrv_port != -1)
		fprintf(fcfg, ",\n\"http-port\" : %d", httpsrv_port);
#endif
#ifdef USE_LIBEVENT
	if (stratumsrv_port != -1)
		fprintf(fcfg, ",\n\"stratum-port\" : %ld", stratumsrv_port);
#endif
	_write_config_string_elist(fcfg, "device", opt_devices_enabled_list);
	_write_config_string_elist(fcfg, "set-device", opt_set_device_list);
	
	if (opt_api_allow)
		fprintf(fcfg, ",\n\"api-allow\" : \"%s\"", json_escape(opt_api_allow));
	if (strcmp(opt_api_mcast_addr, API_MCAST_ADDR) != 0)
		fprintf(fcfg, ",\n\"api-mcast-addr\" : \"%s\"", json_escape(opt_api_mcast_addr));
	if (strcmp(opt_api_mcast_code, API_MCAST_CODE) != 0)
		fprintf(fcfg, ",\n\"api-mcast-code\" : \"%s\"", json_escape(opt_api_mcast_code));
	if (*opt_api_mcast_des)
		fprintf(fcfg, ",\n\"api-mcast-des\" : \"%s\"", json_escape(opt_api_mcast_des));
	if (strcmp(opt_api_description, PACKAGE_STRING) != 0)
		fprintf(fcfg, ",\n\"api-description\" : \"%s\"", json_escape(opt_api_description));
	if (opt_api_groups)
		fprintf(fcfg, ",\n\"api-groups\" : \"%s\"", json_escape(opt_api_groups));
	fputs("\n}\n", fcfg);

	json_escape_free();
}

void zero_bestshare(void)
{
	int i;

	best_diff = 0;
	suffix_string(best_diff, best_share, sizeof(best_share), 0);

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];
		pool->best_diff = 0;
	}
}

void zero_stats(void)
{
	int i;
	
	applog(LOG_DEBUG, "Zeroing stats");

	cgtime(&total_tv_start);
	miner_started = total_tv_start;
	total_rolling = 0;
	total_mhashes_done = 0;
	total_getworks = 0;
	total_accepted = 0;
	total_rejected = 0;
	hw_errors = 0;
	total_stale = 0;
	total_discarded = 0;
	total_bytes_rcvd = total_bytes_sent = 0;
	new_blocks = 0;
	local_work = 0;
	total_go = 0;
	total_ro = 0;
	total_secs = 1.0;
	total_diff1 = 0;
	total_bad_diff1 = 0;
	found_blocks = 0;
	total_diff_accepted = 0;
	total_diff_rejected = 0;
	total_diff_stale = 0;
#ifdef HAVE_CURSES
	awidth = rwidth = swidth = hwwidth = 1;
#endif

	struct mining_goal_info *goal, *tmpgoal;
	HASH_ITER(hh, mining_goals, goal, tmpgoal)
	{
		goal->diff_accepted = 0;
	}
	
	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		pool->getwork_requested = 0;
		pool->accepted = 0;
		pool->rejected = 0;
		pool->solved = 0;
		pool->getwork_requested = 0;
		pool->stale_shares = 0;
		pool->discarded_work = 0;
		pool->getfail_occasions = 0;
		pool->remotefail_occasions = 0;
		pool->last_share_time = 0;
		pool->works = 0;
		pool->diff1 = 0;
		pool->diff_accepted = 0;
		pool->diff_rejected = 0;
		pool->diff_stale = 0;
		pool->last_share_diff = 0;
		pool->cgminer_stats.start_tv = total_tv_start;
		pool->cgminer_stats.getwork_calls = 0;
		pool->cgminer_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;
		pool->cgminer_stats.getwork_wait_max.tv_sec = 0;
		pool->cgminer_stats.getwork_wait_max.tv_usec = 0;
		pool->cgminer_pool_stats.getwork_calls = 0;
		pool->cgminer_pool_stats.getwork_attempts = 0;
		pool->cgminer_pool_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;
		pool->cgminer_pool_stats.getwork_wait_max.tv_sec = 0;
		pool->cgminer_pool_stats.getwork_wait_max.tv_usec = 0;
		pool->cgminer_pool_stats.min_diff = 0;
		pool->cgminer_pool_stats.max_diff = 0;
		pool->cgminer_pool_stats.min_diff_count = 0;
		pool->cgminer_pool_stats.max_diff_count = 0;
		pool->cgminer_pool_stats.times_sent = 0;
		pool->cgminer_pool_stats.bytes_sent = 0;
		pool->cgminer_pool_stats.net_bytes_sent = 0;
		pool->cgminer_pool_stats.times_received = 0;
		pool->cgminer_pool_stats.bytes_received = 0;
		pool->cgminer_pool_stats.net_bytes_received = 0;
	}

	zero_bestshare();

	for (i = 0; i < total_devices; ++i) {
		struct cgpu_info *cgpu = get_devices(i);

		mutex_lock(&hash_lock);
		cgpu->total_mhashes = 0;
		cgpu->accepted = 0;
		cgpu->rejected = 0;
		cgpu->stale = 0;
		cgpu->hw_errors = 0;
		cgpu->utility = 0.0;
		cgpu->utility_diff1 = 0;
		cgpu->last_share_pool_time = 0;
		cgpu->bad_diff1 = 0;
		cgpu->diff1 = 0;
		cgpu->diff_accepted = 0;
		cgpu->diff_rejected = 0;
		cgpu->diff_stale = 0;
		cgpu->last_share_diff = 0;
		cgpu->thread_fail_init_count = 0;
		cgpu->thread_zero_hash_count = 0;
		cgpu->thread_fail_queue_count = 0;
		cgpu->dev_sick_idle_60_count = 0;
		cgpu->dev_dead_idle_600_count = 0;
		cgpu->dev_nostart_count = 0;
		cgpu->dev_over_heat_count = 0;
		cgpu->dev_thermal_cutoff_count = 0;
		cgpu->dev_comms_error_count = 0;
		cgpu->dev_throttle_count = 0;
		cgpu->cgminer_stats.start_tv = total_tv_start;
		cgpu->cgminer_stats.getwork_calls = 0;
		cgpu->cgminer_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;
		cgpu->cgminer_stats.getwork_wait_max.tv_sec = 0;
		cgpu->cgminer_stats.getwork_wait_max.tv_usec = 0;
		mutex_unlock(&hash_lock);
		
		if (cgpu->drv->zero_stats)
			cgpu->drv->zero_stats(cgpu);
	}
}

int bfg_strategy_parse(const char * const s)
{
	char *endptr;
	if (!(s && s[0]))
		return -1;
	long int selected = strtol(s, &endptr, 0);
	if (endptr == s || *endptr) {
		// Look-up by name
		selected = -1;
		for (unsigned i = 0; i <= TOP_STRATEGY; ++i) {
			if (!strcasecmp(strategies[i].s, s)) {
				selected = i;
			}
		}
	}
	if (selected < 0 || selected > TOP_STRATEGY) {
		return -1;
	}
	return selected;
}

bool bfg_strategy_change(const int selected, const char * const param)
{
	if (param && param[0]) {
		switch (selected) {
			case POOL_ROTATE:
			{
				char *endptr;
				long int n = strtol(param, &endptr, 0);
				if (n < 0 || n > 9999 || *endptr) {
					return false;
				}
				opt_rotate_period = n;
				break;
			}
			default:
				return false;
		}
	}
	
	mutex_lock(&lp_lock);
	pool_strategy = selected;
	pthread_cond_broadcast(&lp_cond);
	mutex_unlock(&lp_lock);
	switch_pools(NULL);
	
	return true;
}

#ifdef HAVE_CURSES
static
void loginput_mode(const int size)
{
	clear_logwin();
	loginput_size = size;
	check_winsizes();
}

static void display_pools(void)
{
	struct pool *pool;
	int selected, i, j;
	char input;

	loginput_mode(7 + total_pools);
	immedok(logwin, true);
updated:
	for (j = 0; j < total_pools; j++) {
		for (i = 0; i < total_pools; i++) {
			pool = pools[i];

			if (pool->prio != j)
				continue;

			if (pool_actively_in_use(pool, NULL))
				wattron(logwin, A_BOLD);
			if (pool->enabled != POOL_ENABLED || pool->failover_only)
				wattron(logwin, A_DIM);
			wlogprint("%d: ", pool->prio);
			switch (pool->enabled) {
				case POOL_ENABLED:
					if ((pool_strategy == POOL_LOADBALANCE) ? (!pool->quota)
					    : ((pool_strategy != POOL_FAILOVER) ? pool->failover_only : 0))
						wlogprint("Failover ");
					else
						wlogprint("Enabled  ");
					break;
				case POOL_DISABLED:
					wlogprint("Disabled ");
					break;
				case POOL_REJECTING:
					wlogprint("Rejectin ");
					break;
				case POOL_MISBEHAVING:
					wlogprint("Misbehav ");
					break;
			}
			_wlogprint(pool_proto_str(pool));
			wlogprint(" Quota %d Pool %d: %s  User:%s\n",
				pool->quota,
				pool->pool_no,
				pool->rpc_url, pool->rpc_user);
			wattroff(logwin, A_BOLD | A_DIM);

			break; //for (i = 0; i < total_pools; i++)
		}
	}
retry:
	wlogprint("\nCurrent pool management strategy: %s\n",
		strategies[pool_strategy].s);
	if (pool_strategy == POOL_ROTATE)
		wlogprint("Set to rotate every %d minutes\n", opt_rotate_period);
	wlogprint("[F]ailover only %s\n", opt_fail_only ? "enabled" : "disabled");
	wlogprint("Pool [A]dd [R]emove [D]isable [E]nable [P]rioritize [Q]uota change\n");
	wlogprint("[C]hange management strategy [S]witch pool [I]nformation\n");
	wlogprint("Or press any other key to continue\n");
	logwin_update();
	input = getch();

	if (!strncasecmp(&input, "a", 1)) {
		if (opt_benchmark)
		{
			wlogprint("Cannot add pools in benchmark mode");
			goto retry;
		}
		input_pool(true);
		goto updated;
	} else if (!strncasecmp(&input, "r", 1)) {
		if (total_pools <= 1) {
			wlogprint("Cannot remove last pool");
			goto retry;
		}
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		if (pool == current_pool())
			switch_pools(NULL);
		if (pool == current_pool()) {
			wlogprint("Unable to remove pool due to activity\n");
			goto retry;
		}
		remove_pool(pool);
		goto updated;
	} else if (!strncasecmp(&input, "s", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		manual_enable_pool(pool);
		switch_pools(pool);
		goto updated;
	} else if (!strncasecmp(&input, "d", 1)) {
		if (enabled_pools <= 1) {
			wlogprint("Cannot disable last pool");
			goto retry;
		}
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		disable_pool(pool, POOL_DISABLED);
		goto updated;
	} else if (!strncasecmp(&input, "e", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		manual_enable_pool(pool);
		goto updated;
	} else if (!strncasecmp(&input, "c", 1)) {
		for (i = 0; i <= TOP_STRATEGY; i++)
			wlogprint("%d: %s\n", i, strategies[i].s);
		{
			char * const selected_str = curses_input("Select strategy type");
			selected = bfg_strategy_parse(selected_str);
			free(selected_str);
		}
		if (selected < 0 || selected > TOP_STRATEGY) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		char *param = NULL;
		if (selected == POOL_ROTATE) {
			param = curses_input("Select interval in minutes");
		}
		bool result = bfg_strategy_change(selected, param);
		free(param);
		if (!result) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		goto updated;
	} else if (!strncasecmp(&input, "i", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		display_pool_summary(pool);
		goto retry;
	} else if (!strncasecmp(&input, "q", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		selected = curses_int("Set quota");
		if (selected < 0) {
			wlogprint("Invalid negative quota\n");
			goto retry;
		}
		if (selected > 0)
			pool->failover_only = false;
		pool->quota = selected;
		adjust_quota_gcd();
		goto updated;
	} else if (!strncasecmp(&input, "f", 1)) {
		opt_fail_only ^= true;
		goto updated;
        } else if (!strncasecmp(&input, "p", 1)) {
			char *prilist = curses_input("Enter new pool priority (comma separated list)");
			if (!prilist)
			{
				wlogprint("Not changing priorities\n");
				goto retry;
			}
			int res = prioritize_pools(prilist, &i);
			free(prilist);
			switch (res) {
        		case MSG_NOPOOL:
        			wlogprint("No pools\n");
        			goto retry;
        		case MSG_MISPID:
        			wlogprint("Missing pool id parameter\n");
        			goto retry;
        		case MSG_INVPID:
        			wlogprint("Invalid pool id %d - range is 0 - %d\n", i, total_pools - 1);
        			goto retry;
        		case MSG_DUPPID:
        			wlogprint("Duplicate pool specified %d\n", i);
        			goto retry;
        		case MSG_POOLPRIO:
        		default:
        			goto updated;
        	}
	}

	immedok(logwin, false);
	loginput_mode(0);
}

static const char *summary_detail_level_str(void)
{
	if (opt_compact)
		return "compact";
	if (opt_show_procs)
		return "processors";
	return "devices";
}

static void display_options(void)
{
	int selected;
	char input;

	immedok(logwin, true);
	loginput_mode(12);
retry:
	clear_logwin();
	wlogprint("[N]ormal [C]lear [S]ilent mode (disable all output)\n");
	wlogprint("[D]ebug:%s\n[P]er-device:%s\n[Q]uiet:%s\n[V]erbose:%s\n"
		  "[R]PC debug:%s\n[W]orkTime details:%s\nsu[M]mary detail level:%s\n"
		  "[L]og interval:%d\nS[T]atistical counts: %s\n[Z]ero statistics\n",
		opt_debug_console ? "on" : "off",
	        want_per_device_stats? "on" : "off",
		opt_quiet ? "on" : "off",
		opt_log_output ? "on" : "off",
		opt_protocol ? "on" : "off",
		opt_worktime ? "on" : "off",
		summary_detail_level_str(),
		opt_log_interval,
		opt_weighed_stats ? "weighed" : "absolute");
	wlogprint("Select an option or any other key to return\n");
	logwin_update();
	input = getch();
	if (!strncasecmp(&input, "q", 1)) {
		opt_quiet ^= true;
		wlogprint("Quiet mode %s\n", opt_quiet ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "v", 1)) {
		opt_log_output ^= true;
		if (opt_log_output)
			opt_quiet = false;
		wlogprint("Verbose mode %s\n", opt_log_output ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "n", 1)) {
		opt_log_output = false;
		opt_debug_console = false;
		opt_quiet = false;
		opt_protocol = false;
		opt_compact = false;
		opt_show_procs = false;
		devsummaryYOffset = 0;
		want_per_device_stats = false;
		wlogprint("Output mode reset to normal\n");
		switch_logsize();
		goto retry;
	} else if (!strncasecmp(&input, "d", 1)) {
		opt_debug = true;
		opt_debug_console ^= true;
		opt_log_output = opt_debug_console;
		if (opt_debug_console)
			opt_quiet = false;
		wlogprint("Debug mode %s\n", opt_debug_console ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "m", 1)) {
		if (opt_compact)
			opt_compact = false;
		else
		if (!opt_show_procs)
			opt_show_procs = true;
		else
		{
			opt_compact = true;
			opt_show_procs = false;
			devsummaryYOffset = 0;
		}
		wlogprint("su[M]mary detail level changed to: %s\n", summary_detail_level_str());
		switch_logsize();
		goto retry;
	} else if (!strncasecmp(&input, "p", 1)) {
		want_per_device_stats ^= true;
		opt_log_output = want_per_device_stats;
		wlogprint("Per-device stats %s\n", want_per_device_stats ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "r", 1)) {
		opt_protocol ^= true;
		if (opt_protocol)
			opt_quiet = false;
		wlogprint("RPC protocol debugging %s\n", opt_protocol ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "c", 1))
		clear_logwin();
	else if (!strncasecmp(&input, "l", 1)) {
		selected = curses_int("Interval in seconds");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_log_interval = selected;
		wlogprint("Log interval set to %d seconds\n", opt_log_interval);
		goto retry;
	} else if (!strncasecmp(&input, "s", 1)) {
		opt_realquiet = true;
	} else if (!strncasecmp(&input, "w", 1)) {
		opt_worktime ^= true;
		wlogprint("WorkTime details %s\n", opt_worktime ? "enabled" : "disabled");
		goto retry;
	} else if (!strncasecmp(&input, "t", 1)) {
		opt_weighed_stats ^= true;
		wlogprint("Now displaying %s statistics\n", opt_weighed_stats ? "weighed" : "absolute");
		goto retry;
	} else if (!strncasecmp(&input, "z", 1)) {
		zero_stats();
		goto retry;
	}

	immedok(logwin, false);
	loginput_mode(0);
}
#endif

void default_save_file(char *filename)
{
#if defined(unix) || defined(__APPLE__)
	if (getenv("HOME") && *getenv("HOME")) {
	        strcpy(filename, getenv("HOME"));
		strcat(filename, "/");
	}
	else
		strcpy(filename, "");
	strcat(filename, ".bfgminer/");
	mkdir(filename, 0777);
#else
	strcpy(filename, "");
#endif
	strcat(filename, def_conf);
}

#ifdef HAVE_CURSES
static void set_options(void)
{
	int selected;
	char input;

	immedok(logwin, true);
	loginput_mode(8);
retry:
	wlogprint("\n[L]ongpoll: %s\n", want_longpoll ? "On" : "Off");
	wlogprint("[Q]ueue: %d\n[S]cantime: %d\n[E]xpiry: %d\n[R]etries: %d\n"
		  "[W]rite config file\n[B]FGMiner restart\n",
		opt_queue, opt_scantime, opt_expiry, opt_retries);
	wlogprint("Select an option or any other key to return\n");
	logwin_update();
	input = getch();

	if (!strncasecmp(&input, "q", 1)) {
		selected = curses_int("Extra work items to queue");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_queue = selected;
		goto retry;
	} else if (!strncasecmp(&input, "l", 1)) {
		if (want_longpoll)
			stop_longpoll();
		else
			start_longpoll();
		applog(LOG_WARNING, "Longpoll %s", want_longpoll ? "enabled" : "disabled");
		goto retry;
	} else if  (!strncasecmp(&input, "s", 1)) {
		selected = curses_int("Set scantime in seconds");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_scantime = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "e", 1)) {
		selected = curses_int("Set expiry time in seconds");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_expiry = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "r", 1)) {
		selected = curses_int("Retries before failing (-1 infinite)");
		if (selected < -1 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_retries = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "w", 1)) {
		FILE *fcfg;
		char *str, filename[PATH_MAX], prompt[PATH_MAX + 50];

		default_save_file(filename);
		snprintf(prompt, sizeof(prompt), "Config filename to write (Enter for default) [%s]", filename);
		str = curses_input(prompt);
		if (str) {
			struct stat statbuf;

			strcpy(filename, str);
			free(str);
			if (!stat(filename, &statbuf)) {
				wlogprint("File exists, overwrite?\n");
				input = getch();
				if (strncasecmp(&input, "y", 1))
					goto retry;
			}
		}
		fcfg = fopen(filename, "w");
		if (!fcfg) {
			wlogprint("Cannot open or create file\n");
			goto retry;
		}
		write_config(fcfg);
		fclose(fcfg);
		goto retry;

	} else if (!strncasecmp(&input, "b", 1)) {
		wlogprint("Are you sure?\n");
		input = getch();
		if (!strncasecmp(&input, "y", 1))
			app_restart();
		else
			clear_logwin();
	} else
		clear_logwin();

	loginput_mode(0);
	immedok(logwin, false);
}

int scan_serial(const char *);

static
void _managetui_msg(const char *repr, const char **msg)
{
	if (*msg)
	{
		applog(LOG_DEBUG, "ManageTUI: %"PRIpreprv": %s", repr, *msg);
		wattron(logwin, A_BOLD);
		wlogprint("%s", *msg);
		wattroff(logwin, A_BOLD);
		*msg = NULL;
	}
	logwin_update();
}

void manage_device(void)
{
	char logline[256];
	const char *msg = NULL;
	struct cgpu_info *cgpu;
	const struct device_drv *drv;
	
	selecting_device = true;
	immedok(logwin, true);
	loginput_mode(12);
	
devchange:
	if (unlikely(!total_devices))
	{
		clear_logwin();
		wlogprint("(no devices)\n");
		wlogprint("[Plus] Add device(s)  [Enter] Close device manager\n");
		_managetui_msg("(none)", &msg);
		int input = getch();
		switch (input)
		{
			case '+':  case '=':  // add new device
				goto addnew;
			default:
				goto out;
		}
	}
	
	cgpu = devices[selected_device];
	drv = cgpu->drv;
	refresh_devstatus();
	
refresh:
	clear_logwin();
	wlogprint("Select processor to manage using up/down arrow keys\n");
	
	get_statline3(logline, sizeof(logline), cgpu, true, true);
	wattron(logwin, A_BOLD);
	wlogprint("%s", logline);
	wattroff(logwin, A_BOLD);
	wlogprint("\n");
	
	if (cgpu->dev_manufacturer)
		wlogprint("  %s from %s\n", (cgpu->dev_product ?: "Device"), cgpu->dev_manufacturer);
	else
	if (cgpu->dev_product)
		wlogprint("  %s\n", cgpu->dev_product);
	
	if (cgpu->dev_serial)
		wlogprint("Serial: %s\n", cgpu->dev_serial);
	
	if (cgpu->kname)
		wlogprint("Kernel: %s\n", cgpu->kname);
	
	if (drv->proc_wlogprint_status && likely(cgpu->status != LIFE_INIT))
		drv->proc_wlogprint_status(cgpu);
	
	wlogprint("\n");
	// TODO: Last share at TIMESTAMP on pool N
	// TODO: Custom device info/commands
	if (cgpu->deven != DEV_ENABLED)
		wlogprint("[E]nable ");
	if (cgpu->deven != DEV_DISABLED)
		wlogprint("[D]isable ");
	if (drv->identify_device)
		wlogprint("[I]dentify ");
	if (drv->proc_tui_wlogprint_choices && likely(cgpu->status != LIFE_INIT))
		drv->proc_tui_wlogprint_choices(cgpu);
	wlogprint("\n");
	wlogprint("[Slash] Find processor  [Plus] Add device(s)  [Enter] Close device manager\n");
	_managetui_msg(cgpu->proc_repr, &msg);
	
	while (true)
	{
		int input = getch();
		applog(LOG_DEBUG, "ManageTUI: %"PRIpreprv": (choice %d)", cgpu->proc_repr, input);
		switch (input) {
			case 'd': case 'D':
				if (cgpu->deven == DEV_DISABLED)
					msg = "Processor already disabled\n";
				else
				{
					cgpu->deven = DEV_DISABLED;
					msg = "Processor being disabled\n";
				}
				goto refresh;
			case 'e': case 'E':
				if (cgpu->deven == DEV_ENABLED)
					msg = "Processor already enabled\n";
				else
				{
					proc_enable(cgpu);
					msg = "Processor being enabled\n";
				}
				goto refresh;
			case 'i': case 'I':
				if (drv->identify_device && drv->identify_device(cgpu))
					msg = "Identify command sent\n";
				else
					goto key_default;
				goto refresh;
			case KEY_DOWN:
				if (selected_device >= total_devices - 1)
					break;
				++selected_device;
				goto devchange;
			case KEY_UP:
				if (selected_device <= 0)
					break;
				--selected_device;
				goto devchange;
			case KEY_NPAGE:
			{
				if (selected_device >= total_devices - 1)
					break;
				struct cgpu_info *mdev = devices[selected_device]->device;
				do {
					++selected_device;
				} while (devices[selected_device]->device == mdev && selected_device < total_devices - 1);
				goto devchange;
			}
			case KEY_PPAGE:
			{
				if (selected_device <= 0)
					break;
				struct cgpu_info *mdev = devices[selected_device]->device;
				do {
					--selected_device;
				} while (devices[selected_device]->device == mdev && selected_device > 0);
				goto devchange;
			}
			case '/':  case '?':  // find device
			{
				static char *pattern = NULL;
				char *newpattern = curses_input("Enter pattern");
				if (newpattern)
				{
					free(pattern);
					pattern = newpattern;
				}
				else
				if (!pattern)
					pattern = calloc(1, 1);
				int match = cgpu_search(pattern, selected_device + 1);
				if (match == -1)
				{
					msg = "Couldn't find device\n";
					goto refresh;
				}
				selected_device = match;
				goto devchange;
			}
			case '+':  case '=':  // add new device
			{
addnew:
				clear_logwin();
				_wlogprint(
					"Enter \"auto\", \"all\", or a serial port to probe for mining devices.\n"
					"Prefix by a driver name and colon to only probe a specific driver.\n"
					"For example: erupter:"
#ifdef WIN32
					"\\\\.\\COM40"
#elif defined(__APPLE__)
					"/dev/cu.SLAB_USBtoUART"
#else
					"/dev/ttyUSB39"
#endif
					"\n"
				);
				char *scanser = curses_input("Enter target");
				if (scan_serial(scanser))
				{
					selected_device = total_devices - 1;
					msg = "Device scan succeeded\n";
				}
				else
					msg = "No new devices found\n";
				goto devchange;
			}
			case 'Q': case 'q':
			case KEY_BREAK: case KEY_BACKSPACE: case KEY_CANCEL: case KEY_CLOSE: case KEY_EXIT:
			case '\x1b':  // ESC
			case KEY_ENTER:
			case '\r':  // Ctrl-M on Windows, with nonl
#ifdef PADENTER
			case PADENTER:  // pdcurses, used by Enter key on Windows with nonl
#endif
			case '\n':
				goto out;
			default:
				;
key_default:
				if (drv->proc_tui_handle_choice && likely(drv_ready(cgpu)))
				{
					msg = drv->proc_tui_handle_choice(cgpu, input);
					if (msg)
						goto refresh;
				}
		}
	}

out:
	selecting_device = false;
	loginput_mode(0);
	immedok(logwin, false);
}

void show_help(void)
{
	loginput_mode(11);
	
	// NOTE: wlogprint is a macro with a buffer limit
	_wlogprint(
		"LU: oldest explicit work update currently being used for new work\n"
		"ST: work in queue              | F: network fails   | NB: new blocks detected\n"
		"AS: shares being submitted     | BW: bandwidth (up/down)\n"
		"E: # shares * diff per 2kB bw  | I: expected income | BS: best share ever found\n"
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_BTEE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_BTEE  U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE U8_HLINE
		"\n"
		"devices/processors hashing (only for totals line), hottest temperature\n"
	);
	wlogprint(
		"hashrates: %ds decaying / all-time average / all-time average (effective)\n"
		, opt_log_interval);
	_wlogprint(
		"A: accepted shares | R: rejected+discarded(% of total)\n"
		"HW: hardware errors / % nonces invalid\n"
		"\n"
		"Press any key to clear"
	);
	
	logwin_update();
	getch();
	
	loginput_mode(0);
}

static void *input_thread(void __maybe_unused *userdata)
{
	RenameThread("input");

	if (!curses_active)
		return NULL;

	while (1) {
		int input;

		input = getch();
		switch (input) {
			case 'h': case 'H': case '?':
			case KEY_F(1):
				show_help();
				break;
		case 'q': case 'Q':
			kill_work();
			return NULL;
		case 'd': case 'D':
			display_options();
			break;
		case 'm': case 'M':
			manage_device();
			break;
		case 'p': case 'P':
			display_pools();
			break;
		case 's': case 'S':
			set_options();
			break;
#ifdef HAVE_CURSES
		case KEY_DOWN:
		{
			const int visible_lines = logcursor - devcursor;
			const int invisible_lines = total_lines - visible_lines;
			if (devsummaryYOffset <= -invisible_lines)
				break;
			devsummaryYOffset -= 2;
		}
		case KEY_UP:
			if (devsummaryYOffset == 0)
				break;
			++devsummaryYOffset;
			refresh_devstatus();
			break;
		case KEY_NPAGE:
		{
			const int visible_lines = logcursor - devcursor;
			const int invisible_lines = total_lines - visible_lines;
			if (devsummaryYOffset - visible_lines <= -invisible_lines)
				devsummaryYOffset = -invisible_lines;
			else
				devsummaryYOffset -= visible_lines;
			refresh_devstatus();
			break;
		}
		case KEY_PPAGE:
		{
			const int visible_lines = logcursor - devcursor;
			if (devsummaryYOffset + visible_lines >= 0)
				devsummaryYOffset = 0;
			else
				devsummaryYOffset += visible_lines;
			refresh_devstatus();
			break;
		}
#endif
		}
		if (opt_realquiet) {
			disable_curses();
			break;
		}
	}

	return NULL;
}
#endif

static void *api_thread(void *userdata)
{
	struct thr_info *mythr = userdata;

	pthread_detach(pthread_self());
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	RenameThread("rpc");

	api(api_thr_id);

	mythr->has_pth = false;

	return NULL;
}

void thread_reportin(struct thr_info *thr)
{
	cgtime(&thr->last);
	thr->cgpu->status = LIFE_WELL;
	thr->getwork = 0;
	thr->cgpu->device_last_well = time(NULL);
}

void thread_reportout(struct thr_info *thr)
{
	thr->getwork = time(NULL);
}

static void hashmeter(int thr_id, struct timeval *diff,
		      uint64_t hashes_done)
{
	char logstatusline[256];
	struct timeval temp_tv_end, total_diff;
	double secs;
	double local_secs;
	static double local_mhashes_done = 0;
	double local_mhashes = (double)hashes_done / 1000000.0;
	bool showlog = false;
	char cHr[ALLOC_H2B_NOUNIT+1], aHr[ALLOC_H2B_NOUNIT+1], uHr[ALLOC_H2B_SPACED+3+1];
	char rejpcbuf[6];
	char bnbuf[6];
	struct thr_info *thr;

	/* Update the last time this thread reported in */
	if (thr_id >= 0) {
		thr = get_thread(thr_id);
		cgtime(&(thr->last));
		thr->cgpu->device_last_well = time(NULL);
	}

	secs = (double)diff->tv_sec + ((double)diff->tv_usec / 1000000.0);

	/* So we can call hashmeter from a non worker thread */
	if (thr_id >= 0) {
		struct cgpu_info *cgpu = thr->cgpu;
		int threadobj = cgpu->threads ?: 1;
		double thread_rolling = 0.0;
		int i;

		applog(LOG_DEBUG, "[thread %d: %"PRIu64" hashes, %.1f khash/sec]",
			thr_id, hashes_done, hashes_done / 1000 / secs);

		/* Rolling average for each thread and each device */
		decay_time(&thr->rolling, local_mhashes / secs, secs);
		for (i = 0; i < threadobj; i++)
			thread_rolling += cgpu->thr[i]->rolling;

		mutex_lock(&hash_lock);
		decay_time(&cgpu->rolling, thread_rolling, secs);
		cgpu->total_mhashes += local_mhashes;
		mutex_unlock(&hash_lock);

		// If needed, output detailed, per-device stats
		if (want_per_device_stats) {
			struct timeval now;
			struct timeval elapsed;
			struct timeval *last_msg_tv = opt_show_procs ? &thr->cgpu->last_message_tv : &thr->cgpu->device->last_message_tv;

			cgtime(&now);
			timersub(&now, last_msg_tv, &elapsed);
			if (opt_log_interval <= elapsed.tv_sec) {
				struct cgpu_info *cgpu = thr->cgpu;
				char logline[255];

				*last_msg_tv = now;

				get_statline(logline, sizeof(logline), cgpu);
				if (!curses_active) {
					printf("\n%s\r", logline);
					fflush(stdout);
				} else
					applog(LOG_INFO, "%s", logline);
			}
		}
	}

	/* Totals are updated by all threads so can race without locking */
	mutex_lock(&hash_lock);
	cgtime(&temp_tv_end);
	
	timersub(&temp_tv_end, &total_tv_start, &total_diff);
	total_secs = (double)total_diff.tv_sec + ((double)total_diff.tv_usec / 1000000.0);
	
	timersub(&temp_tv_end, &total_tv_end, &total_diff);

	total_mhashes_done += local_mhashes;
	local_mhashes_done += local_mhashes;
	/* Only update with opt_log_interval */
	if (total_diff.tv_sec < opt_log_interval)
		goto out_unlock;
	showlog = true;
	cgtime(&total_tv_end);

	local_secs = (double)total_diff.tv_sec + ((double)total_diff.tv_usec / 1000000.0);
	decay_time(&total_rolling, local_mhashes_done / local_secs, local_secs);
	global_hashrate = ((unsigned long long)lround(total_rolling)) * 1000000;

	double wtotal = (total_diff_accepted + total_diff_rejected + total_diff_stale);
	
	multi_format_unit_array2(
		((char*[]){cHr, aHr, uHr}),
		((size_t[]){sizeof(cHr), sizeof(aHr), sizeof(uHr)}),
		true, "h/s", H2B_SHORT,
		3,
		1e6*total_rolling,
		1e6*total_mhashes_done / total_secs,
		utility_to_hashrate(total_diff1 * (wtotal ? (total_diff_accepted / wtotal) : 1) * 60 / total_secs));

	int ui_accepted, ui_rejected, ui_stale;
	if (opt_weighed_stats)
	{
		ui_accepted = total_diff_accepted;
		ui_rejected = total_diff_rejected;
		ui_stale = total_diff_stale;
	}
	else
	{
		ui_accepted = total_accepted;
		ui_rejected = total_rejected;
		ui_stale = total_stale;
	}
	
#ifdef HAVE_CURSES
	if (curses_active_locked()) {
		float temp = 0;
		struct cgpu_info *proc, *last_working_dev = NULL;
		int i, working_devs = 0, working_procs = 0;
		int divx;
		bool bad = false;
		
		// Find the highest temperature of all processors
		for (i = 0; i < total_devices; ++i)
		{
			proc = get_devices(i);
			
			if (proc->temp > temp)
				temp = proc->temp;
			
			if (unlikely(proc->deven == DEV_DISABLED))
				;  // Just need to block it off from both conditions
			else
			if (likely(proc->status == LIFE_WELL && proc->deven == DEV_ENABLED))
			{
				if (proc->rolling > .1)
				{
					++working_procs;
					if (proc->device != last_working_dev)
					{
						++working_devs;
						last_working_dev = proc->device;
					}
				}
			}
			else
				bad = true;
		}
		
		if (working_devs == working_procs)
			snprintf(statusline, sizeof(statusline), "%s%d        ", bad ? U8_BAD_START : "", working_devs);
		else
			snprintf(statusline, sizeof(statusline), "%s%d/%d     ", bad ? U8_BAD_START : "", working_devs, working_procs);
		
		divx = 7;
		if (opt_show_procs && !opt_compact)
			divx += max_lpdigits;
		
		if (bad)
		{
			divx += sizeof(U8_BAD_START)-1;
			strcpy(&statusline[divx], U8_BAD_END);
			divx += sizeof(U8_BAD_END)-1;
		}
		
		temperature_column(&statusline[divx], sizeof(statusline)-divx, true, &temp);
		
		format_statline(statusline, sizeof(statusline),
		                cHr, aHr,
		                uHr,
		                ui_accepted,
		                ui_rejected,
		                ui_stale,
		                total_diff_rejected + total_diff_stale, total_diff_accepted,
		                hw_errors,
		                total_bad_diff1, total_bad_diff1 + total_diff1);
		unlock_curses();
	}
#endif
	
	// Add a space
	memmove(&uHr[6], &uHr[5], strlen(&uHr[5]) + 1);
	uHr[5] = ' ';
	
	percentf4(rejpcbuf, sizeof(rejpcbuf), total_diff_rejected + total_diff_stale, total_diff_accepted);
	percentf4(bnbuf, sizeof(bnbuf), total_bad_diff1, total_diff1);
	
	snprintf(logstatusline, sizeof(logstatusline),
	         "%s%ds:%s avg:%s u:%s | A:%d R:%d+%d(%s) HW:%d/%s",
		want_per_device_stats ? "ALL " : "",
		opt_log_interval,
		cHr, aHr,
		uHr,
		ui_accepted,
		ui_rejected,
		ui_stale,
		rejpcbuf,
		hw_errors,
		bnbuf
	);


	local_mhashes_done = 0;
out_unlock:
	mutex_unlock(&hash_lock);

	if (showlog) {
		if (!curses_active) {
			if (want_per_device_stats)
				printf("\n%s\r", logstatusline);
			else
			{
				const int logstatusline_len = strlen(logstatusline);
				int padding;
				if (last_logstatusline_len > logstatusline_len)
					padding = (last_logstatusline_len - logstatusline_len);
				else
				{
					padding = 0;
					if (last_logstatusline_len == -1)
						puts("");
				}
				printf("%s%*s\r", logstatusline, padding, "");
				last_logstatusline_len = logstatusline_len;
			}
			fflush(stdout);
		} else
			applog(LOG_INFO, "%s", logstatusline);
	}
}

void hashmeter2(struct thr_info *thr)
{
	struct timeval tv_now, tv_elapsed;
	
	timerclear(&thr->tv_hashes_done);
	
	cgtime(&tv_now);
	timersub(&tv_now, &thr->tv_lastupdate, &tv_elapsed);
	/* Update the hashmeter at most 5 times per second */
	if ((thr->hashes_done && (tv_elapsed.tv_sec > 0 || tv_elapsed.tv_usec > 200000)) ||
	    tv_elapsed.tv_sec >= opt_log_interval) {
		hashmeter(thr->id, &tv_elapsed, thr->hashes_done);
		thr->hashes_done = 0;
		thr->tv_lastupdate = tv_now;
	}
}

static void stratum_share_result(json_t *val, json_t *res_val, json_t *err_val,
				 struct stratum_share *sshare)
{
	struct work *work = sshare->work;

	share_result(val, res_val, err_val, work, false, "");
}

/* Parses stratum json responses and tries to find the id that the request
 * matched to and treat it accordingly. */
bool parse_stratum_response(struct pool *pool, char *s)
{
	json_t *val = NULL, *err_val, *res_val, *id_val;
	struct stratum_share *sshare;
	json_error_t err;
	bool ret = false;
	int id;

	val = JSON_LOADS(s, &err);
	if (!val) {
		applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (json_is_null(id_val) || !id_val) {
		char *ss;

		if (err_val)
			ss = json_dumps(err_val, JSON_INDENT(3));
		else
			ss = strdup("(unknown reason)");

		applog(LOG_INFO, "JSON-RPC non method decode failed: %s", ss);

		free(ss);

		goto out;
	}

	if (!json_is_integer(id_val)) {
		if (json_is_string(id_val)
		 && !strncmp(json_string_value(id_val), "txlist", 6))
		{
			const bool is_array = json_is_array(res_val);
			applog(LOG_DEBUG, "Received %s for pool %u job %s",
			       is_array ? "transaction list" : "no-transaction-list response",
			       pool->pool_no, &json_string_value(id_val)[6]);
			if (!is_array)
			{
				// No need to wait for a timeout
				timer_unset(&pool->swork.tv_transparency);
				pool_set_opaque(pool, true);
				goto fishy;
			}
			if (strcmp(json_string_value(id_val) + 6, pool->swork.job_id))
				// We only care about a transaction list for the current job id
				goto fishy;
			
			// Check that the transactions actually hash to the merkle links
			{
				unsigned maxtx = 1 << pool->swork.merkles;
				unsigned mintx = maxtx >> 1;
				--maxtx;
				unsigned acttx = (unsigned)json_array_size(res_val);
				if (acttx < mintx || acttx > maxtx) {
					applog(LOG_WARNING, "Pool %u is sending mismatched block contents to us (%u is not %u-%u)",
					       pool->pool_no, acttx, mintx, maxtx);
					goto fishy;
				}
				// TODO: Check hashes match actual merkle links
			}

			pool_set_opaque(pool, false);
			timer_unset(&pool->swork.tv_transparency);

fishy:
			ret = true;
		}

		goto out;
	}

	id = json_integer_value(id_val);

	mutex_lock(&sshare_lock);
	HASH_FIND_INT(stratum_shares, &id, sshare);
	if (sshare)
		HASH_DEL(stratum_shares, sshare);
	mutex_unlock(&sshare_lock);

	if (!sshare) {
		double pool_diff;

		/* Since the share is untracked, we can only guess at what the
		 * work difficulty is based on the current pool diff. */
		cg_rlock(&pool->data_lock);
		pool_diff = target_diff(pool->swork.target);
		cg_runlock(&pool->data_lock);

		if (json_is_true(res_val)) {
			struct mining_goal_info * const goal = pool->goal;
			
			applog(LOG_NOTICE, "Accepted untracked stratum share from pool %d", pool->pool_no);

			/* We don't know what device this came from so we can't
			 * attribute the work to the relevant cgpu */
			mutex_lock(&stats_lock);
			total_accepted++;
			pool->accepted++;
			total_diff_accepted += pool_diff;
			pool->diff_accepted += pool_diff;
			goal->diff_accepted += pool_diff;
			mutex_unlock(&stats_lock);
		} else {
			applog(LOG_NOTICE, "Rejected untracked stratum share from pool %d", pool->pool_no);

			mutex_lock(&stats_lock);
			total_rejected++;
			pool->rejected++;
			total_diff_rejected += pool_diff;
			pool->diff_rejected += pool_diff;
			mutex_unlock(&stats_lock);
		}
		goto out;
	}
	else {
		mutex_lock(&submitting_lock);
		--total_submitting;
		mutex_unlock(&submitting_lock);
	}
	stratum_share_result(val, res_val, err_val, sshare);
	free_work(sshare->work);
	free(sshare);

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}

static void shutdown_stratum(struct pool *pool)
{
	// Shut down Stratum as if we never had it
	pool->stratum_active = false;
	pool->stratum_init = false;
	pool->has_stratum = false;
	shutdown(pool->sock, SHUT_RDWR);
	free(pool->stratum_url);
	if (pool->sockaddr_url == pool->stratum_url)
		pool->sockaddr_url = NULL;
	pool->stratum_url = NULL;
}

void clear_stratum_shares(struct pool *pool)
{
	int my_mining_threads = mining_threads;  // Cached outside of locking
	struct stratum_share *sshare, *tmpshare;
	struct work *work;
	struct cgpu_info *cgpu;
	double diff_cleared = 0;
	double thr_diff_cleared[my_mining_threads];
	int cleared = 0;
	int thr_cleared[my_mining_threads];
	
	// NOTE: This is per-thread rather than per-device to avoid getting devices lock in stratum_shares loop
	for (int i = 0; i < my_mining_threads; ++i)
	{
		thr_diff_cleared[i] = 0;
		thr_cleared[i] = 0;
	}

	mutex_lock(&sshare_lock);
	HASH_ITER(hh, stratum_shares, sshare, tmpshare) {
		work = sshare->work;
		if (sshare->work->pool == pool && work->thr_id < my_mining_threads) {
			HASH_DEL(stratum_shares, sshare);
			
			sharelog("disconnect", work);
			
			diff_cleared += sshare->work->work_difficulty;
			thr_diff_cleared[work->thr_id] += work->work_difficulty;
			++thr_cleared[work->thr_id];
			free_work(sshare->work);
			free(sshare);
			cleared++;
		}
	}
	mutex_unlock(&sshare_lock);

	if (cleared) {
		applog(LOG_WARNING, "Lost %d shares due to stratum disconnect on pool %d", cleared, pool->pool_no);
		mutex_lock(&stats_lock);
		pool->stale_shares += cleared;
		total_stale += cleared;
		pool->diff_stale += diff_cleared;
		total_diff_stale += diff_cleared;
		for (int i = 0; i < my_mining_threads; ++i)
			if (thr_cleared[i])
			{
				cgpu = get_thr_cgpu(i);
				cgpu->diff_stale += thr_diff_cleared[i];
				cgpu->stale += thr_cleared[i];
			}
		mutex_unlock(&stats_lock);

		mutex_lock(&submitting_lock);
		total_submitting -= cleared;
		mutex_unlock(&submitting_lock);
	}
}

static void resubmit_stratum_shares(struct pool *pool)
{
	struct stratum_share *sshare, *tmpshare;
	struct work *work;
	unsigned resubmitted = 0;

	mutex_lock(&sshare_lock);
	mutex_lock(&submitting_lock);
	HASH_ITER(hh, stratum_shares, sshare, tmpshare) {
		if (sshare->work->pool != pool)
			continue;
		
		HASH_DEL(stratum_shares, sshare);
		
		work = sshare->work;
		DL_APPEND(submit_waiting, work);
		
		free(sshare);
		++resubmitted;
	}
	mutex_unlock(&submitting_lock);
	mutex_unlock(&sshare_lock);

	if (resubmitted) {
		notifier_wake(submit_waiting_notifier);
		applog(LOG_DEBUG, "Resubmitting %u shares due to stratum disconnect on pool %u", resubmitted, pool->pool_no);
	}
}

static void clear_pool_work(struct pool *pool)
{
	struct work *work, *tmp;
	int cleared = 0;

	mutex_lock(stgd_lock);
	HASH_ITER(hh, staged_work, work, tmp) {
		if (work->pool == pool) {
			unstage_work(work);
			free_work(work);
			cleared++;
		}
	}
	mutex_unlock(stgd_lock);
}

static int cp_prio(void)
{
	int prio;

	cg_rlock(&control_lock);
	prio = currentpool->prio;
	cg_runlock(&control_lock);

	return prio;
}

/* We only need to maintain a secondary pool connection when we need the
 * capacity to get work from the backup pools while still on the primary */
static bool cnx_needed(struct pool *pool)
{
	struct pool *cp;

	// We want to keep a connection open for rejecting or misbehaving pools, to detect when/if they change their tune
	if (pool->enabled == POOL_DISABLED)
		return false;

	/* Idle stratum pool needs something to kick it alive again */
	if (pool->has_stratum && pool->idle)
		return true;

	/* Getwork pools without opt_fail_only need backup pools up to be able
	 * to leak shares */
	cp = current_pool();
	if (pool_actively_desired(pool, cp))
		return true;
	if (!pool_localgen(cp) && (!opt_fail_only || !cp->hdr_path))
		return true;

	/* Keep the connection open to allow any stray shares to be submitted
	 * on switching pools for 2 minutes. */
	if (timer_elapsed(&pool->tv_last_work_time, NULL) < 120)
		return true;

	/* If the pool has only just come to life and is higher priority than
	 * the current pool keep the connection open so we can fail back to
	 * it. */
	if (pool_strategy == POOL_FAILOVER && pool->prio < cp_prio())
		return true;

	if (pool_unworkable(cp))
		return true;
	
	/* We've run out of work, bring anything back to life. */
	if (no_work)
		return true;
	
	// If the current pool lacks its own block change detection, see if we are needed for that
	if (pool_active_lp_pool(cp) == pool)
		return true;

	return false;
}

static void wait_lpcurrent(struct pool *pool);
static void pool_resus(struct pool *pool);

static void stratum_resumed(struct pool *pool)
{
	if (!pool->stratum_notify)
		return;
	if (pool_tclear(pool, &pool->idle)) {
		applog(LOG_INFO, "Stratum connection to pool %d resumed", pool->pool_no);
		pool_resus(pool);
	}
}

static bool supports_resume(struct pool *pool)
{
	bool ret;

	cg_rlock(&pool->data_lock);
	ret = (pool->sessionid != NULL);
	cg_runlock(&pool->data_lock);

	return ret;
}

static bool pools_active;

/* One stratum thread per pool that has stratum waits on the socket checking
 * for new messages and for the integrity of the socket connection. We reset
 * the connection based on the integrity of the receive side only as the send
 * side will eventually expire data it fails to send. */
static void *stratum_thread(void *userdata)
{
	struct pool *pool = (struct pool *)userdata;

	pthread_detach(pthread_self());

	char threadname[20];
	snprintf(threadname, 20, "stratum%u", pool->pool_no);
	RenameThread(threadname);

	srand(time(NULL) + (intptr_t)userdata);

	while (42) {
		struct timeval timeout;
		int sel_ret;
		fd_set rd;
		char *s;
		int sock;

		if (unlikely(!pool->has_stratum))
			break;

		/* Check to see whether we need to maintain this connection
		 * indefinitely or just bring it up when we switch to this
		 * pool */
		while (true)
		{
			sock = pool->sock;
			
			if (sock == INVSOCK)
				applog(LOG_DEBUG, "Pool %u: Invalid socket, suspending",
				       pool->pool_no);
			else
			if (!sock_full(pool) && !cnx_needed(pool) && pools_active)
				applog(LOG_DEBUG, "Pool %u: Connection not needed, suspending",
				       pool->pool_no);
			else
				break;
			
			suspend_stratum(pool);
			clear_stratum_shares(pool);
			clear_pool_work(pool);

			wait_lpcurrent(pool);
			if (!restart_stratum(pool)) {
				pool_died(pool);
				while (!restart_stratum(pool)) {
					if (pool->removed)
						goto out;
					cgsleep_ms(30000);
				}
			}
		}

		FD_ZERO(&rd);
		FD_SET(sock, &rd);
		timeout.tv_sec = 120;
		timeout.tv_usec = 0;

		/* If we fail to receive any notify messages for 2 minutes we
		 * assume the connection has been dropped and treat this pool
		 * as dead */
		if (!sock_full(pool) && (sel_ret = select(sock + 1, &rd, NULL, NULL, &timeout)) < 1) {
			applog(LOG_DEBUG, "Stratum select failed on pool %d with value %d", pool->pool_no, sel_ret);
			s = NULL;
		} else
			s = recv_line(pool);
		if (!s) {
			if (!pool->has_stratum)
				break;

			applog(LOG_NOTICE, "Stratum connection to pool %d interrupted", pool->pool_no);
			pool->getfail_occasions++;
			total_go++;

			mutex_lock(&pool->stratum_lock);
			pool->stratum_active = pool->stratum_notify = false;
			pool->sock = INVSOCK;
			mutex_unlock(&pool->stratum_lock);

			/* If the socket to our stratum pool disconnects, all
			 * submissions need to be discarded or resent. */
			if (!supports_resume(pool))
				clear_stratum_shares(pool);
			else
				resubmit_stratum_shares(pool);
			clear_pool_work(pool);
			if (pool == current_pool())
				restart_threads();

			if (restart_stratum(pool))
				continue;

			shutdown_stratum(pool);
			pool_died(pool);
			break;
		}

		/* Check this pool hasn't died while being a backup pool and
		 * has not had its idle flag cleared */
		stratum_resumed(pool);

		if (!parse_method(pool, s) && !parse_stratum_response(pool, s))
			applog(LOG_INFO, "Unknown stratum msg: %s", s);
		free(s);
		if (pool->swork.clean) {
			struct work *work = make_work();

			/* Generate a single work item to update the current
			 * block database */
			pool->swork.clean = false;
			gen_stratum_work(pool, work);

			/* Try to extract block height from coinbase scriptSig */
			uint8_t *bin_height = &bytes_buf(&pool->swork.coinbase)[4 /*version*/ + 1 /*txin count*/ + 36 /*prevout*/ + 1 /*scriptSig len*/ + 1 /*push opcode*/];
			unsigned char cb_height_sz;
			cb_height_sz = bin_height[-1];
			if (cb_height_sz == 3) {
				// FIXME: The block number will overflow this by AD 2173
				struct mining_goal_info * const goal = pool->goal;
				const void * const prevblkhash = &work->data[4];
				uint32_t height = 0;
				memcpy(&height, bin_height, 3);
				height = le32toh(height);
				have_block_height(goal, prevblkhash, height);
			}

			pool->swork.work_restart_id =
			++pool->work_restart_id;
			pool_update_work_restart_time(pool);
			if (test_work_current(work)) {
				/* Only accept a work update if this stratum
				 * connection is from the current pool */
				struct pool * const cp = current_pool();
				if (pool == cp)
					restart_threads();
				
				applog(
				       ((!opt_quiet_work_updates) && pool_actively_in_use(pool, cp) ? LOG_NOTICE : LOG_DEBUG),
				       "Stratum from pool %d requested work update", pool->pool_no);
			} else
				applog(LOG_NOTICE, "Stratum from pool %d detected new block", pool->pool_no);
			free_work(work);
		}

		if (timer_passed(&pool->swork.tv_transparency, NULL)) {
			// More than 4 timmills past since requested transactions
			timer_unset(&pool->swork.tv_transparency);
			pool_set_opaque(pool, true);
		}
	}

out:
	return NULL;
}

static void init_stratum_thread(struct pool *pool)
{
	struct mining_goal_info * const goal = pool->goal;
	goal->have_longpoll = true;

	if (unlikely(pthread_create(&pool->stratum_thread, NULL, stratum_thread, (void *)pool)))
		quit(1, "Failed to create stratum thread");
}

static void *longpoll_thread(void *userdata);

static bool stratum_works(struct pool *pool)
{
	applog(LOG_INFO, "Testing pool %d stratum %s", pool->pool_no, pool->stratum_url);
	if (!extract_sockaddr(pool->stratum_url, &pool->sockaddr_url, &pool->stratum_port))
		return false;

	if (pool->stratum_active)
		return true;
	
	if (!initiate_stratum(pool))
		return false;

	return true;
}

static
bool pool_recently_got_work(struct pool * const pool, const struct timeval * const tvp_now)
{
	return (timer_isset(&pool->tv_last_work_time) && timer_elapsed(&pool->tv_last_work_time, tvp_now) < 60);
}

static bool pool_active(struct pool *pool, bool pinging)
{
	struct timeval tv_now, tv_getwork, tv_getwork_reply;
	bool ret = false;
	json_t *val;
	CURL *curl = NULL;
	int rolltime;
	char *rpc_req;
	struct work *work;
	enum pool_protocol proto;

	if (pool->stratum_init)
	{
		if (pool->stratum_active)
			return true;
	}
	else
	if (!pool->idle)
	{
		timer_set_now(&tv_now);
		if (pool_recently_got_work(pool, &tv_now))
			return true;
	}
	
	mutex_lock(&pool->pool_test_lock);
	
	if (pool->stratum_init)
	{
		ret = pool->stratum_active;
		goto out;
	}
	
	timer_set_now(&tv_now);
	
	if (pool->idle)
	{
		if (timer_elapsed(&pool->tv_idle, &tv_now) < 30)
			goto out;
	}
	else
	if (pool_recently_got_work(pool, &tv_now))
	{
		ret = true;
		goto out;
	}
	
		applog(LOG_INFO, "Testing pool %s", pool->rpc_url);

	/* This is the central point we activate stratum when we can */
	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		goto out;
	}

	if (!(want_gbt || want_getwork))
		goto nohttp;

	work = make_work();

	/* Probe for GBT support on first pass */
	proto = want_gbt ? PLP_GETBLOCKTEMPLATE : PLP_GETWORK;

tryagain:
	rpc_req = prepare_rpc_req_probe(work, proto, NULL, pool);
	work->pool = pool;
	if (!rpc_req)
		goto out;

	pool->probed = false;
	cgtime(&tv_getwork);
	val = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, rpc_req,
			true, false, &rolltime, pool, false);
	cgtime(&tv_getwork_reply);

	free(rpc_req);

	/* Detect if a http getwork pool has an X-Stratum header at startup,
	 * and if so, switch to that in preference to getwork if it works */
	if (pool->stratum_url && want_stratum && pool_may_redirect_to(pool, pool->stratum_url) && (pool->has_stratum || stratum_works(pool))) {
		if (!pool->has_stratum) {

		applog(LOG_NOTICE, "Switching pool %d %s to %s", pool->pool_no, pool->rpc_url, pool->stratum_url);
		if (!pool->rpc_url)
			pool_set_uri(pool, strdup(pool->stratum_url));
		pool->has_stratum = true;

		}

		free_work(work);
		if (val)
			json_decref(val);

retry_stratum:
		;
		/* We create the stratum thread for each pool just after
		 * successful authorisation. Once the init flag has been set
		 * we never unset it and the stratum thread is responsible for
		 * setting/unsetting the active flag */
		bool init = pool_tset(pool, &pool->stratum_init);

		if (!init) {
			ret = initiate_stratum(pool) && auth_stratum(pool);

			if (ret)
			{
				detect_algo = 2;
				init_stratum_thread(pool);
			}
			else
			{
				pool_tclear(pool, &pool->stratum_init);
				pool->tv_idle = tv_getwork_reply;
			}
			goto out;
		}
		ret = pool->stratum_active;
		goto out;
	}
	else if (pool->has_stratum)
		shutdown_stratum(pool);

	if (val) {
		bool rc;
		json_t *res;

		res = json_object_get(val, "result");
		if ((!json_is_object(res)) || (proto == PLP_GETBLOCKTEMPLATE && !json_object_get(res, "bits")))
			goto badwork;

		work->rolltime = rolltime;
		rc = work_decode(pool, work, val);
		if (rc) {
			applog(LOG_DEBUG, "Successfully retrieved and deciphered work from pool %u %s",
			       pool->pool_no, pool->rpc_url);
			work->pool = pool;
			copy_time(&work->tv_getwork, &tv_getwork);
			copy_time(&work->tv_getwork_reply, &tv_getwork_reply);
			work->getwork_mode = GETWORK_MODE_TESTPOOL;
			calc_diff(work, 0);

			update_last_work(work);

			applog(LOG_DEBUG, "Pushing pooltest work to base pool");

			stage_work(work);
			total_getworks++;
			pool->getwork_requested++;
			ret = true;
			pool->tv_idle = tv_getwork_reply;
		} else {
badwork:
			json_decref(val);
			applog(LOG_DEBUG, "Successfully retrieved but FAILED to decipher work from pool %u %s",
			       pool->pool_no, pool->rpc_url);
			pool->proto = proto = pool_protocol_fallback(proto);
			if (PLP_NONE != proto)
				goto tryagain;
			pool->tv_idle = tv_getwork_reply;
			free_work(work);
			goto out;
		}
		json_decref(val);

		if (proto != pool->proto) {
			pool->proto = proto;
			applog(LOG_INFO, "Selected %s protocol for pool %u", pool_protocol_name(proto), pool->pool_no);
		}

		if (pool->lp_url)
			goto out;

		/* Decipher the longpoll URL, if any, and store it in ->lp_url */

		const struct blktmpl_longpoll_req *lp;
		if (work->tr && (lp = blktmpl_get_longpoll(work->tr->tmpl))) {
			// NOTE: work_decode takes care of lp id
			pool->lp_url = lp->uri ? absolute_uri(lp->uri, pool->rpc_url) : pool->rpc_url;
			if (!pool->lp_url)
			{
				ret = false;
				goto out;
			}
			pool->lp_proto = PLP_GETBLOCKTEMPLATE;
		}
		else
		if (pool->hdr_path && want_getwork) {
			pool->lp_url = absolute_uri(pool->hdr_path, pool->rpc_url);
			if (!pool->lp_url)
			{
				ret = false;
				goto out;
			}
			pool->lp_proto = PLP_GETWORK;
		} else
			pool->lp_url = NULL;

		if (want_longpoll && !pool->lp_started) {
			pool->lp_started = true;
			if (unlikely(pthread_create(&pool->longpoll_thread, NULL, longpoll_thread, (void *)pool)))
				quit(1, "Failed to create pool longpoll thread");
		}
	} else if (PLP_NONE != (proto = pool_protocol_fallback(proto))) {
		pool->proto = proto;
		goto tryagain;
	} else {
		pool->tv_idle = tv_getwork_reply;
		free_work(work);
nohttp:
		/* If we failed to parse a getwork, this could be a stratum
		 * url without the prefix stratum+tcp:// so let's check it */
		if (extract_sockaddr(pool->rpc_url, &pool->sockaddr_url, &pool->stratum_port) && initiate_stratum(pool)) {
			pool->has_stratum = true;
			goto retry_stratum;
		}
		applog(LOG_DEBUG, "FAILED to retrieve work from pool %u %s",
		       pool->pool_no, pool->rpc_url);
		if (!pinging)
			applog(LOG_WARNING, "Pool %u slow/down or URL or credentials invalid", pool->pool_no);
	}
out:
	if (curl)
		curl_easy_cleanup(curl);
	mutex_unlock(&pool->pool_test_lock);
	return ret;
}

static void pool_resus(struct pool *pool)
{
	if (pool->enabled == POOL_ENABLED && pool_strategy == POOL_FAILOVER && pool->prio < cp_prio())
		applog(LOG_WARNING, "Pool %d %s alive, testing stability", pool->pool_no, pool->rpc_url);
	else
		applog(LOG_INFO, "Pool %d %s alive", pool->pool_no, pool->rpc_url);
}

static
void *cmd_idle_thread(void * const __maybe_unused userp)
{
	pthread_detach(pthread_self());
	RenameThread("cmd-idle");
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	
	sleep(opt_log_interval);
	pthread_testcancel();
	run_cmd(cmd_idle);
	
	return NULL;
}

static struct work *hash_pop(struct cgpu_info * const proc)
{
	int hc;
	struct work *work, *work_found, *tmp;
	enum {
		HPWS_NONE,
		HPWS_LOWDIFF,
		HPWS_SPARE,
		HPWS_ROLLABLE,
		HPWS_PERFECT,
	} work_score = HPWS_NONE;
	bool did_cmd_idle = false;
	pthread_t cmd_idle_thr;

retry:
	mutex_lock(stgd_lock);
	while (true)
	{
		work_found = NULL;
		work_score = 0;
		hc = HASH_COUNT(staged_work);
		HASH_ITER(hh, staged_work, work, tmp)
		{
			const struct mining_algorithm * const work_malgo = work_mining_algorithm(work);
			const float min_nonce_diff = drv_min_nonce_diff(proc->drv, proc, work_malgo);
#define FOUND_WORK(score)  do{  \
				if (work_score < score)  \
				{  \
					work_found = work;  \
					work_score = score;  \
				}  \
				continue;  \
}while(0)
			if (min_nonce_diff < work->work_difficulty)
			{
				if (min_nonce_diff < 0)
					continue;
				FOUND_WORK(HPWS_LOWDIFF);
			}
			if (work->spare)
				FOUND_WORK(HPWS_SPARE);
			if (work->rolltime && hc > staged_rollable)
				FOUND_WORK(HPWS_ROLLABLE);
#undef FOUND_WORK
			
			// Good match
			work_found = work;
			work_score = HPWS_PERFECT;
			break;
		}
		if (work_found)
		{
			work = work_found;
			break;
		}
		
		// Failed to get a usable work
		if (unlikely(staged_full))
		{
			if (likely(opt_queue < 10 + mining_threads))
			{
				++opt_queue;
				applog(LOG_WARNING, "Staged work underrun; increasing queue minimum to %d", opt_queue);
			}
			else
				applog(LOG_WARNING, "Staged work underrun; not automatically increasing above %d", opt_queue);
			staged_full = false;  // Let it fill up before triggering an underrun again
			no_work = true;
		}
		pthread_cond_signal(&gws_cond);
		
		if (cmd_idle && !did_cmd_idle)
		{
			if (likely(!pthread_create(&cmd_idle_thr, NULL, cmd_idle_thread, NULL)))
				did_cmd_idle = true;
		}
		pthread_cond_wait(&getq->cond, stgd_lock);
	}
	if (did_cmd_idle)
		pthread_cancel(cmd_idle_thr);
	
	no_work = false;

	if (can_roll(work) && should_roll(work))
	{
		// Instead of consuming it, force it to be cloned and grab the clone
		mutex_unlock(stgd_lock);
		clone_available();
		goto retry;
	}
	
	unstage_work(work);

	/* Signal the getwork scheduler to look for more work */
	pthread_cond_signal(&gws_cond);

	/* Signal hash_pop again in case there are mutliple hash_pop waiters */
	pthread_cond_signal(&getq->cond);
	mutex_unlock(stgd_lock);
	work->pool->last_work_time = time(NULL);
	cgtime(&work->pool->tv_last_work_time);

	return work;
}

/* Clones work by rolling it if possible, and returning a clone instead of the
 * original work item which gets staged again to possibly be rolled again in
 * the future */
static struct work *clone_work(struct work *work)
{
	int mrs = mining_threads + opt_queue - total_staged(false);
	struct work *work_clone;
	bool cloned;

	if (mrs < 1)
		return work;

	cloned = false;
	work_clone = make_clone(work);
	while (mrs-- > 0 && can_roll(work) && should_roll(work)) {
		applog(LOG_DEBUG, "Pushing rolled converted work to stage thread");
		stage_work(work_clone);
		roll_work(work);
		work_clone = make_clone(work);
		/* Roll it again to prevent duplicates should this be used
		 * directly later on */
		roll_work(work);
		cloned = true;
	}

	if (cloned) {
		stage_work(work);
		return work_clone;
	}

	free_work(work_clone);

	return work;
}

void gen_hash(unsigned char *data, unsigned char *hash, int len)
{
	unsigned char hash1[32];

	sha256(data, len, hash1);
	sha256(hash1, 32, hash);
}

/* PDiff 1 is a 256 bit unsigned integer of
 * 0x00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff
 * so we use a big endian 32 bit unsigned integer positioned at the Nth byte to
 * cover a huge range of difficulty targets, though not all 256 bits' worth */
static void pdiff_target_leadzero(void * const target_p, double diff)
{
	uint8_t *target = target_p;
	diff *= 0x100000000;
	int skip = log2(diff) / 8;
	if (skip)
	{
		if (skip > 0x1c)
			skip = 0x1c;
		diff /= pow(0x100, skip);
		memset(target, 0, skip);
	}
	uint32_t n = 0xffffffff;
	n = (double)n / diff;
	n = htobe32(n);
	memcpy(&target[skip], &n, sizeof(n));
	memset(&target[skip + sizeof(n)], 0xff, 32 - (skip + sizeof(n)));
}

void set_target_to_pdiff(void * const dest_target, const double pdiff)
{
	unsigned char rtarget[32];
	pdiff_target_leadzero(rtarget, pdiff);
	swab256(dest_target, rtarget);
	
	if (opt_debug) {
		char htarget[65];
		bin2hex(htarget, rtarget, 32);
		applog(LOG_DEBUG, "Generated target %s", htarget);
	}
}

void set_target_to_bdiff(void * const dest_target, const double bdiff)
{
	set_target_to_pdiff(dest_target, bdiff_to_pdiff(bdiff));
}

void _test_target(void * const funcp, const char * const funcname, const bool little_endian, const void * const expectp, const double diff)
{
	uint8_t bufr[32], buf[32], expectr[32], expect[32];
	int off;
	void (*func)(void *, double) = funcp;
	
	func(little_endian ? bufr : buf, diff);
	if (little_endian)
		swab256(buf, bufr);
	
	swap32tobe(expect, expectp, 256/32);
	
	// Fuzzy comparison: the first 32 bits set must match, and the actual target must be >= the expected
	for (off = 0; off < 28 && !buf[off]; ++off)
	{}
	
	if (memcmp(&buf[off], &expect[off], 4))
	{
testfail: ;
		++unittest_failures;
		char hexbuf[65], expectbuf[65];
		bin2hex(hexbuf, buf, 32);
		bin2hex(expectbuf, expect, 32);
		applogr(, LOG_WARNING, "%s test failed: diff %g got %s (expected %s)",
		        funcname, diff, hexbuf, expectbuf);
	}
	
	if (!little_endian)
		swab256(bufr, buf);
	swab256(expectr, expect);
	
	if (!hash_target_check(expectr, bufr))
		goto testfail;
}

#define TEST_TARGET(func, le, expect, diff)  \
	_test_target(func, #func, le, expect, diff)

void test_target()
{
	uint32_t expect[8] = {0};
	// bdiff 1 should be exactly 00000000ffff0000000006f29cfd29510a6caee84634e86a57257cf03152537f due to floating-point imprecision (pdiff1 / 1.0000152590218966)
	expect[0] = 0x0000ffff;
	TEST_TARGET(set_target_to_bdiff, true, expect, 1./0x10000);
	expect[0] = 0;
	expect[1] = 0xffff0000;
	TEST_TARGET(set_target_to_bdiff, true, expect, 1);
	expect[1] >>= 1;
	TEST_TARGET(set_target_to_bdiff, true, expect, 2);
	expect[1] >>= 3;
	TEST_TARGET(set_target_to_bdiff, true, expect, 0x10);
	expect[1] >>= 4;
	TEST_TARGET(set_target_to_bdiff, true, expect, 0x100);
	
	memset(&expect[1], '\xff', 28);
	expect[0] = 0x0000ffff;
	TEST_TARGET(set_target_to_pdiff, true, expect, 1./0x10000);
	expect[0] = 0;
	TEST_TARGET(set_target_to_pdiff, true, expect, 1);
	expect[1] >>= 1;
	TEST_TARGET(set_target_to_pdiff, true, expect, 2);
	expect[1] >>= 3;
	TEST_TARGET(set_target_to_pdiff, true, expect, 0x10);
	expect[1] >>= 4;
	TEST_TARGET(set_target_to_pdiff, true, expect, 0x100);
}

void stratum_work_cpy(struct stratum_work * const dst, const struct stratum_work * const src)
{
	*dst = *src;
	if (dst->tr)
		tmpl_incref(dst->tr);
	dst->nonce1 = maybe_strdup(src->nonce1);
	dst->job_id = maybe_strdup(src->job_id);
	bytes_cpy(&dst->coinbase, &src->coinbase);
	bytes_cpy(&dst->merkle_bin, &src->merkle_bin);
	dst->data_lock_p = NULL;
}

void stratum_work_clean(struct stratum_work * const swork)
{
	if (swork->tr)
		tmpl_decref(swork->tr);
	free(swork->nonce1);
	free(swork->job_id);
	bytes_free(&swork->coinbase);
	bytes_free(&swork->merkle_bin);
}

bool pool_has_usable_swork(const struct pool * const pool)
{
	if (opt_benchmark)
		return true;
	if (pool->swork.tr)
	{
		// GBT
		struct timeval tv_now;
		timer_set_now(&tv_now);
		return blkmk_time_left(pool->swork.tr->tmpl, tv_now.tv_sec);
	}
	return pool->stratum_notify;
}

/* Generates stratum based work based on the most recent notify information
 * from the pool. This will keep generating work while a pool is down so we use
 * other means to detect when the pool has died in stratum_thread */
static void gen_stratum_work(struct pool *pool, struct work *work)
{
	clean_work(work);
	
	cg_wlock(&pool->data_lock);
	
	const int n2size = pool->swork.n2size;
	bytes_resize(&work->nonce2, n2size);
	if (pool->nonce2sz < n2size)
		memset(&bytes_buf(&work->nonce2)[pool->nonce2sz], 0, n2size - pool->nonce2sz);
	memcpy(bytes_buf(&work->nonce2),
#ifdef WORDS_BIGENDIAN
	// NOTE: On big endian, the most significant bits are stored at the end, so skip the LSBs
	       &((char*)&pool->nonce2)[pool->nonce2off],
#else
	       &pool->nonce2,
#endif
	       pool->nonce2sz);
	pool->nonce2++;
	
	work->pool = pool;
	work->work_restart_id = pool->swork.work_restart_id;
	gen_stratum_work2(work, &pool->swork);
	
	cgtime(&work->tv_staged);
}

void gen_stratum_work2(struct work *work, struct stratum_work *swork)
{
	unsigned char *coinbase;
	
	/* Generate coinbase */
	coinbase = bytes_buf(&swork->coinbase);
	memcpy(&coinbase[swork->nonce2_offset], bytes_buf(&work->nonce2), bytes_len(&work->nonce2));

	/* Downgrade to a read lock to read off the variables */
	if (swork->data_lock_p)
		cg_dwlock(swork->data_lock_p);
	
	gen_stratum_work3(work, swork, swork->data_lock_p);
	
	if (opt_debug)
	{
		char header[161];
		char nonce2hex[(bytes_len(&work->nonce2) * 2) + 1];
		bin2hex(header, work->data, 80);
		bin2hex(nonce2hex, bytes_buf(&work->nonce2), bytes_len(&work->nonce2));
		applog(LOG_DEBUG, "Generated stratum header %s", header);
		applog(LOG_DEBUG, "Work job_id %s nonce2 %s", work->job_id, nonce2hex);
	}
}

void gen_stratum_work3(struct work * const work, struct stratum_work * const swork, cglock_t * const data_lock_p)
{
	unsigned char *coinbase, merkle_root[32], merkle_sha[64];
	uint8_t *merkle_bin;
	uint32_t *data32, *swap32;
	int i;
	
	coinbase = bytes_buf(&swork->coinbase);
	
	/* Generate merkle root */
	gen_hash(coinbase, merkle_root, bytes_len(&swork->coinbase));
	memcpy(merkle_sha, merkle_root, 32);
	merkle_bin = bytes_buf(&swork->merkle_bin);
	for (i = 0; i < swork->merkles; ++i, merkle_bin += 32) {
		memcpy(merkle_sha + 32, merkle_bin, 32);
		gen_hash(merkle_sha, merkle_root, 64);
		memcpy(merkle_sha, merkle_root, 32);
	}
	data32 = (uint32_t *)merkle_sha;
	swap32 = (uint32_t *)merkle_root;
	flip32(swap32, data32);
	
	memcpy(&work->data[0], swork->header1, 36);
	memcpy(&work->data[36], merkle_root, 32);
	*((uint32_t*)&work->data[68]) = htobe32(swork->ntime + timer_elapsed(&swork->tv_received, NULL));
	memcpy(&work->data[72], swork->diffbits, 4);
	memset(&work->data[76], 0, 4);  // nonce
	memcpy(&work->data[80], workpadding_bin, 48);
	
	work->ntime_roll_limits = swork->ntime_roll_limits;

	/* Copy parameters required for share submission */
	memcpy(work->target, swork->target, sizeof(work->target));
	work->job_id = maybe_strdup(swork->job_id);
	work->nonce1 = maybe_strdup(swork->nonce1);
	if (data_lock_p)
		cg_runlock(data_lock_p);

	calc_midstate(work);

	local_work++;
	work->stratum = true;
	work->blk.nonce = 0;
	work->id = total_work++;
	work->longpoll = false;
	work->getwork_mode = GETWORK_MODE_STRATUM;
	if (swork->tr) {
		work->getwork_mode = GETWORK_MODE_GBT;
		work->tr = swork->tr;
		tmpl_incref(work->tr);
	}
	calc_diff(work, 0);
}

void request_work(struct thr_info *thr)
{
	struct cgpu_info *cgpu = thr->cgpu;
	struct cgminer_stats *dev_stats = &(cgpu->cgminer_stats);

	/* Tell the watchdog thread this thread is waiting on getwork and
	 * should not be restarted */
	thread_reportout(thr);
	
	// HACK: Since get_work still blocks, reportout all processors dependent on this thread
	for (struct cgpu_info *proc = thr->cgpu->next_proc; proc; proc = proc->next_proc)
	{
		if (proc->threads)
			break;
		thread_reportout(proc->thr[0]);
	}

	cgtime(&dev_stats->_get_start);
}

// FIXME: Make this non-blocking (and remove HACK above)
struct work *get_work(struct thr_info *thr)
{
	const int thr_id = thr->id;
	struct cgpu_info *cgpu = thr->cgpu;
	struct cgminer_stats *dev_stats = &(cgpu->cgminer_stats);
	struct cgminer_stats *pool_stats;
	struct timeval tv_get;
	struct work *work = NULL;

	applog(LOG_DEBUG, "%"PRIpreprv": Popping work from get queue to get work", cgpu->proc_repr);
	while (!work) {
		work = hash_pop(cgpu);
		if (stale_work(work, false)) {
			staged_full = false;  // It wasn't really full, since it was stale :(
			discard_work(work);
			work = NULL;
			wake_gws();
		}
	}
	last_getwork = time(NULL);
	applog(LOG_DEBUG, "%"PRIpreprv": Got work %d from get queue to get work for thread %d",
	       cgpu->proc_repr, work->id, thr_id);

	work->thr_id = thr_id;
	thread_reportin(thr);
	
	// HACK: Since get_work still blocks, reportin all processors dependent on this thread
	for (struct cgpu_info *proc = thr->cgpu->next_proc; proc; proc = proc->next_proc)
	{
		if (proc->threads)
			break;
		thread_reportin(proc->thr[0]);
	}
	
	work->mined = true;
	work->blk.nonce = 0;

	cgtime(&tv_get);
	timersub(&tv_get, &dev_stats->_get_start, &tv_get);

	timeradd(&tv_get, &dev_stats->getwork_wait, &dev_stats->getwork_wait);
	if (timercmp(&tv_get, &dev_stats->getwork_wait_max, >))
		dev_stats->getwork_wait_max = tv_get;
	if (timercmp(&tv_get, &dev_stats->getwork_wait_min, <))
		dev_stats->getwork_wait_min = tv_get;
	++dev_stats->getwork_calls;

	pool_stats = &(work->pool->cgminer_stats);
	timeradd(&tv_get, &pool_stats->getwork_wait, &pool_stats->getwork_wait);
	if (timercmp(&tv_get, &pool_stats->getwork_wait_max, >))
		pool_stats->getwork_wait_max = tv_get;
	if (timercmp(&tv_get, &pool_stats->getwork_wait_min, <))
		pool_stats->getwork_wait_min = tv_get;
	++pool_stats->getwork_calls;
	
	if (work->work_difficulty < 1)
	{
		const float min_nonce_diff = drv_min_nonce_diff(cgpu->drv, cgpu, work_mining_algorithm(work));
		if (unlikely(work->work_difficulty < min_nonce_diff))
		{
			if (min_nonce_diff - work->work_difficulty > 1./0x10000000)
				applog(LOG_DEBUG, "%"PRIpreprv": Using work with lower difficulty than device supports",
				       cgpu->proc_repr);
			work->nonce_diff = min_nonce_diff;
		}
		else
			work->nonce_diff = work->work_difficulty;
	}
	else
		work->nonce_diff = 1;

	return work;
}

struct dupe_hash_elem {
	uint8_t hash[0x20];
	struct timeval tv_prune;
	UT_hash_handle hh;
};

static
void _submit_work_async(struct work *work)
{
	applog(LOG_DEBUG, "Pushing submit work to work thread");
	
	if (opt_benchmark)
	{
		json_t * const jn = json_null(), *result = NULL;
		work_check_for_block(work);
		{
			static struct dupe_hash_elem *dupe_hashes;
			struct dupe_hash_elem *dhe, *dhetmp;
			HASH_FIND(hh, dupe_hashes, &work->hash, sizeof(dhe->hash), dhe);
			if (dhe)
				result = json_string("duplicate");
			else
			{
				struct timeval tv_now;
				timer_set_now(&tv_now);
				
				// Prune old entries
				HASH_ITER(hh, dupe_hashes, dhe, dhetmp)
				{
					if (!timer_passed(&dhe->tv_prune, &tv_now))
						break;
					HASH_DEL(dupe_hashes, dhe);
					free(dhe);
				}
				
				dhe = malloc(sizeof(*dhe));
				memcpy(dhe->hash, work->hash, sizeof(dhe->hash));
				timer_set_delay(&dhe->tv_prune, &tv_now, 337500000);
				HASH_ADD(hh, dupe_hashes, hash, sizeof(dhe->hash), dhe);
			}
		}
		if (result)
		{}
		else
		if (stale_work(work, true))
		{
			char stalemsg[0x10];
			snprintf(stalemsg, sizeof(stalemsg), "stale %us", benchmark_update_interval * (work->pool->work_restart_id - work->work_restart_id));
			result = json_string(stalemsg);
		}
		else
			result = json_incref(jn);
		share_result(jn, result, jn, work, false, "");
		free_work(work);
		json_decref(result);
		json_decref(jn);
		return;
	}

	mutex_lock(&submitting_lock);
	++total_submitting;
	DL_APPEND(submit_waiting, work);
	mutex_unlock(&submitting_lock);

	notifier_wake(submit_waiting_notifier);
}

/* Submit a copy of the tested, statistic recorded work item asynchronously */
static void submit_work_async2(struct work *work, struct timeval *tv_work_found)
{
	if (tv_work_found)
		copy_time(&work->tv_work_found, tv_work_found);
	
	_submit_work_async(work);
}

void inc_hw_errors3(struct thr_info *thr, const struct work *work, const uint32_t *bad_nonce_p, float nonce_diff)
{
	struct cgpu_info * const cgpu = thr->cgpu;
	
	if (bad_nonce_p)
	{
		if (bad_nonce_p == UNKNOWN_NONCE)
			applog(LOG_DEBUG, "%"PRIpreprv": invalid nonce - HW error",
			       cgpu->proc_repr);
		else
			applog(LOG_DEBUG, "%"PRIpreprv": invalid nonce (%08lx) - HW error",
			       cgpu->proc_repr, (unsigned long)be32toh(*bad_nonce_p));
	}
	
	mutex_lock(&stats_lock);
	hw_errors++;
	++cgpu->hw_errors;
	if (bad_nonce_p)
	{
		total_bad_diff1 += nonce_diff;
		cgpu->bad_diff1 += nonce_diff;
	}
	mutex_unlock(&stats_lock);

	if (thr->cgpu->drv->hw_error)
		thr->cgpu->drv->hw_error(thr);
}

void work_hash(struct work * const work)
{
	const struct mining_algorithm * const malgo = work_mining_algorithm(work);
	malgo->hash_data_f(work->hash, work->data);
}

static
bool test_hash(const void * const phash, const float diff)
{
	const uint32_t * const hash = phash;
	if (diff >= 1.)
		// FIXME: > 1 should check more
		return !hash[7];
	
	const uint32_t Htarg = (uint32_t)ceil((1. / diff) - 1);
	const uint32_t tmp_hash7 = le32toh(hash[7]);
	
	applog(LOG_DEBUG, "htarget %08lx hash %08lx",
				(long unsigned int)Htarg,
				(long unsigned int)tmp_hash7);
	return (tmp_hash7 <= Htarg);
}

enum test_nonce2_result _test_nonce2(struct work *work, uint32_t nonce, bool checktarget)
{
	uint32_t *work_nonce = (uint32_t *)(work->data + 64 + 12);
	*work_nonce = htole32(nonce);

	work_hash(work);
	
	if (!test_hash(work->hash, work->nonce_diff))
		return TNR_BAD;
	
	if (checktarget && !hash_target_check_v(work->hash, work->target))
	{
		bool high_hash = true;
		struct pool * const pool = work->pool;
		if (pool_diff_effective_retroactively(pool))
		{
			// Some stratum pools are buggy and expect difficulty changes to be immediate retroactively, so if the target has changed, check and submit just in case
			if (memcmp(pool->next_target, work->target, sizeof(work->target)))
			{
				applog(LOG_DEBUG, "Stratum pool %u target has changed since work job issued, checking that too",
				       pool->pool_no);
				if (hash_target_check_v(work->hash, pool->next_target))
				{
					high_hash = false;
					work->work_difficulty = target_diff(pool->next_target);
				}
			}
		}
		if (high_hash)
			return TNR_HIGH;
	}
	
	return TNR_GOOD;
}

/* Returns true if nonce for work was a valid share */
bool submit_nonce(struct thr_info *thr, struct work *work, uint32_t nonce)
{
	return submit_noffset_nonce(thr, work, nonce, 0);
}

/* Allows drivers to submit work items where the driver has changed the ntime
 * value by noffset. Must be only used with a work protocol that does not ntime
 * roll itself intrinsically to generate work (eg stratum). We do not touch
 * the original work struct, but the copy of it only. */
bool submit_noffset_nonce(struct thr_info *thr, struct work *work_in, uint32_t nonce,
			  int noffset)
{
	struct work *work = make_work();
	_copy_work(work, work_in, noffset);
	
	uint32_t *work_nonce = (uint32_t *)(work->data + 64 + 12);
	struct timeval tv_work_found;
	enum test_nonce2_result res;
	bool ret = true;

	thread_reportout(thr);

	cgtime(&tv_work_found);
	*work_nonce = htole32(nonce);
	work->thr_id = thr->id;

	/* Do one last check before attempting to submit the work */
	/* Side effect: sets work->data and work->hash for us */
	res = test_nonce2(work, nonce);
	
	if (unlikely(res == TNR_BAD))
		{
			inc_hw_errors(thr, work, nonce);
			ret = false;
			goto out;
		}
	
	mutex_lock(&stats_lock);
	total_diff1       += work->nonce_diff;
	thr ->cgpu->diff1 += work->nonce_diff;
	work->pool->diff1 += work->nonce_diff;
	thr->cgpu->last_device_valid_work = time(NULL);
	mutex_unlock(&stats_lock);
	
	if (noncelog_file)
		noncelog(work);
	
	if (res == TNR_HIGH)
	{
			// Share above target, normal
			/* Check the diff of the share, even if it didn't reach the
			 * target, just to set the best share value if it's higher. */
			share_diff(work);
			goto out;
	}
	
	submit_work_async2(work, &tv_work_found);
	work = NULL;  // Taken by submit_work_async2
out:
	if (work)
		free_work(work);
	thread_reportin(thr);

	return ret;
}

// return true of we should stop working on this piece of work
// returning false means we will keep scanning for a nonce
// assumptions: work->blk.nonce is the number of nonces completed in the work
// see minerloop_scanhash comments for more details & usage
bool abandon_work(struct work *work, struct timeval *wdiff, uint64_t max_hashes)
{
	if (work->blk.nonce == 0xffffffff ||                // known we are scanning a full nonce range
	    wdiff->tv_sec > opt_scantime ||                 // scan-time has elapsed (user specified, default 60s)
	    work->blk.nonce >= 0xfffffffe - max_hashes ||   // are there enough nonces left in the work
	    max_hashes >= 0xfffffffe ||                     // assume we are scanning a full nonce range
	    stale_work(work, false))                        // work is stale
		return true;
	return false;
}

void __thr_being_msg(int prio, struct thr_info *thr, const char *being)
{
	struct cgpu_info *proc = thr->cgpu;
	
	if (proc->threads > 1)
		applog(prio, "%"PRIpreprv" (thread %d) %s", proc->proc_repr, thr->id, being);
	else
		applog(prio, "%"PRIpreprv" %s", proc->proc_repr, being);
}

// Called by asynchronous minerloops, when they find their processor should be disabled
void mt_disable_start(struct thr_info *mythr)
{
	struct cgpu_info *cgpu = mythr->cgpu;
	struct device_drv *drv = cgpu->drv;
	
	if (drv->thread_disable)
		drv->thread_disable(mythr);
	
	hashmeter2(mythr);
	__thr_being_msg(LOG_WARNING, mythr, "being disabled");
	mythr->rolling = mythr->cgpu->rolling = 0;
	thread_reportout(mythr);
	mythr->_mt_disable_called = true;
}

/* Put a new unqueued work item in cgpu->unqueued_work under cgpu->qlock till
 * the driver tells us it's full so that it may extract the work item using
 * the get_queued() function which adds it to the hashtable on
 * cgpu->queued_work. */
static void fill_queue(struct thr_info *mythr, struct cgpu_info *cgpu, struct device_drv *drv, const int thr_id)
{
	thread_reportout(mythr);
	do {
		bool need_work;

		/* Do this lockless just to know if we need more unqueued work. */
		need_work = (!cgpu->unqueued_work);

		/* get_work is a blocking function so do it outside of lock
		 * to prevent deadlocks with other locks. */
		if (need_work) {
			struct work *work = get_work(mythr);

			wr_lock(&cgpu->qlock);
			/* Check we haven't grabbed work somehow between
			 * checking and picking up the lock. */
			if (likely(!cgpu->unqueued_work))
				cgpu->unqueued_work = work;
			else
				need_work = false;
			wr_unlock(&cgpu->qlock);

			if (unlikely(!need_work))
				discard_work(work);
		}
		/* The queue_full function should be used by the driver to
		 * actually place work items on the physical device if it
		 * does have a queue. */
	} while (drv->queue_full && !drv->queue_full(cgpu));
}

/* Add a work item to a cgpu's queued hashlist */
void __add_queued(struct cgpu_info *cgpu, struct work *work)
{
	cgpu->queued_count++;
	HASH_ADD_INT(cgpu->queued_work, id, work);
}

/* This function is for retrieving one work item from the unqueued pointer and
 * adding it to the hashtable of queued work. Code using this function must be
 * able to handle NULL as a return which implies there is no work available. */
struct work *get_queued(struct cgpu_info *cgpu)
{
	struct work *work = NULL;

	wr_lock(&cgpu->qlock);
	if (cgpu->unqueued_work) {
		work = cgpu->unqueued_work;
		if (unlikely(stale_work(work, false))) {
			discard_work(work);
			work = NULL;
			wake_gws();
		} else
			__add_queued(cgpu, work);
		cgpu->unqueued_work = NULL;
	}
	wr_unlock(&cgpu->qlock);

	return work;
}

void add_queued(struct cgpu_info *cgpu, struct work *work)
{
	wr_lock(&cgpu->qlock);
	__add_queued(cgpu, work);
	wr_unlock(&cgpu->qlock);
}

/* Get fresh work and add it to cgpu's queued hashlist */
struct work *get_queue_work(struct thr_info *thr, struct cgpu_info *cgpu, int thr_id)
{
	struct work *work = get_work(thr);

	add_queued(cgpu, work);
	return work;
}

/* This function is for finding an already queued work item in the
 * given que hashtable. Code using this function must be able
 * to handle NULL as a return which implies there is no matching work.
 * The calling function must lock access to the que if it is required.
 * The common values for midstatelen, offset, datalen are 32, 64, 12 */
struct work *__find_work_bymidstate(struct work *que, char *midstate, size_t midstatelen, char *data, int offset, size_t datalen)
{
	struct work *work, *tmp, *ret = NULL;

	HASH_ITER(hh, que, work, tmp) {
		if (memcmp(work->midstate, midstate, midstatelen) == 0 &&
		    memcmp(work->data + offset, data, datalen) == 0) {
			ret = work;
			break;
		}
	}

	return ret;
}

/* This function is for finding an already queued work item in the
 * device's queued_work hashtable. Code using this function must be able
 * to handle NULL as a return which implies there is no matching work.
 * The common values for midstatelen, offset, datalen are 32, 64, 12 */
struct work *find_queued_work_bymidstate(struct cgpu_info *cgpu, char *midstate, size_t midstatelen, char *data, int offset, size_t datalen)
{
	struct work *ret;

	rd_lock(&cgpu->qlock);
	ret = __find_work_bymidstate(cgpu->queued_work, midstate, midstatelen, data, offset, datalen);
	rd_unlock(&cgpu->qlock);

	return ret;
}

struct work *clone_queued_work_bymidstate(struct cgpu_info *cgpu, char *midstate, size_t midstatelen, char *data, int offset, size_t datalen)
{
	struct work *work, *ret = NULL;

	rd_lock(&cgpu->qlock);
	work = __find_work_bymidstate(cgpu->queued_work, midstate, midstatelen, data, offset, datalen);
	if (work)
		ret = copy_work(work);
	rd_unlock(&cgpu->qlock);

	return ret;
}

void __work_completed(struct cgpu_info *cgpu, struct work *work)
{
	cgpu->queued_count--;
	HASH_DEL(cgpu->queued_work, work);
}

/* This iterates over a queued hashlist finding work started more than secs
 * seconds ago and discards the work as completed. The driver must set the
 * work->tv_work_start value appropriately. Returns the number of items aged. */
int age_queued_work(struct cgpu_info *cgpu, double secs)
{
	struct work *work, *tmp;
	struct timeval tv_now;
	int aged = 0;

	cgtime(&tv_now);

	wr_lock(&cgpu->qlock);
	HASH_ITER(hh, cgpu->queued_work, work, tmp) {
		if (tdiff(&tv_now, &work->tv_work_start) > secs) {
			__work_completed(cgpu, work);
			aged++;
		}
	}
	wr_unlock(&cgpu->qlock);

	return aged;
}

/* This function should be used by queued device drivers when they're sure
 * the work struct is no longer in use. */
void work_completed(struct cgpu_info *cgpu, struct work *work)
{
	wr_lock(&cgpu->qlock);
	__work_completed(cgpu, work);
	wr_unlock(&cgpu->qlock);

	free_work(work);
}

/* Combines find_queued_work_bymidstate and work_completed in one function
 * withOUT destroying the work so the driver must free it. */
struct work *take_queued_work_bymidstate(struct cgpu_info *cgpu, char *midstate, size_t midstatelen, char *data, int offset, size_t datalen)
{
	struct work *work;

	wr_lock(&cgpu->qlock);
	work = __find_work_bymidstate(cgpu->queued_work, midstate, midstatelen, data, offset, datalen);
	if (work)
		__work_completed(cgpu, work);
	wr_unlock(&cgpu->qlock);

	return work;
}

void flush_queue(struct cgpu_info *cgpu)
{
	struct work *work = NULL;

	wr_lock(&cgpu->qlock);
	work = cgpu->unqueued_work;
	cgpu->unqueued_work = NULL;
	wr_unlock(&cgpu->qlock);

	if (work) {
		free_work(work);
		applog(LOG_DEBUG, "Discarded queued work item");
	}
}

/* This version of hash work is for devices that are fast enough to always
 * perform a full nonce range and need a queue to maintain the device busy.
 * Work creation and destruction is not done from within this function
 * directly. */
void hash_queued_work(struct thr_info *mythr)
{
	const long cycle = opt_log_interval / 5 ? : 1;
	struct timeval tv_start = {0, 0}, tv_end;
	struct cgpu_info *cgpu = mythr->cgpu;
	struct device_drv *drv = cgpu->drv;
	const int thr_id = mythr->id;
	int64_t hashes_done = 0;

	if (unlikely(cgpu->deven != DEV_ENABLED))
		mt_disable(mythr);
	
	while (likely(!cgpu->shutdown)) {
		struct timeval diff;
		int64_t hashes;

		fill_queue(mythr, cgpu, drv, thr_id);

		thread_reportin(mythr);
		hashes = drv->scanwork(mythr);

		/* Reset the bool here in case the driver looks for it
		 * synchronously in the scanwork loop. */
		mythr->work_restart = false;

		if (unlikely(hashes == -1 )) {
			applog(LOG_ERR, "%s %d failure, disabling!", drv->name, cgpu->device_id);
			cgpu->deven = DEV_DISABLED;
			dev_error(cgpu, REASON_THREAD_ZERO_HASH);
			mt_disable(mythr);
		}

		hashes_done += hashes;
		cgtime(&tv_end);
		timersub(&tv_end, &tv_start, &diff);
		if (diff.tv_sec >= cycle) {
			hashmeter(thr_id, &diff, hashes_done);
			hashes_done = 0;
			copy_time(&tv_start, &tv_end);
		}

		if (unlikely(mythr->pause || cgpu->deven != DEV_ENABLED))
			mt_disable(mythr);

		if (unlikely(mythr->work_restart)) {
			flush_queue(cgpu);
			if (drv->flush_work)
				drv->flush_work(cgpu);
		}
	}
	// cgpu->deven = DEV_DISABLED; set in miner_thread
}

// Called by minerloop, when it is re-enabling a processor
void mt_disable_finish(struct thr_info *mythr)
{
	struct device_drv *drv = mythr->cgpu->drv;
	
	thread_reportin(mythr);
	__thr_being_msg(LOG_WARNING, mythr, "being re-enabled");
	if (drv->thread_enable)
		drv->thread_enable(mythr);
	mythr->_mt_disable_called = false;
}

// Called by synchronous minerloops, when they find their processor should be disabled
// Calls mt_disable_start, waits until it's re-enabled, then calls mt_disable_finish
void mt_disable(struct thr_info *mythr)
{
	const struct cgpu_info * const cgpu = mythr->cgpu;
	mt_disable_start(mythr);
	applog(LOG_DEBUG, "Waiting for wakeup notification in miner thread");
	do {
		notifier_read(mythr->notifier);
	} while (mythr->pause || cgpu->deven != DEV_ENABLED);
	mt_disable_finish(mythr);
}


enum {
	STAT_SLEEP_INTERVAL		= 1,
	STAT_CTR_INTERVAL		= 10000000,
	FAILURE_INTERVAL		= 30,
};

/* Stage another work item from the work returned in a longpoll */
static void convert_to_work(json_t *val, int rolltime, struct pool *pool, struct work *work, struct timeval *tv_lp, struct timeval *tv_lp_reply)
{
	bool rc;

	work->rolltime = rolltime;
	rc = work_decode(pool, work, val);
	if (unlikely(!rc)) {
		applog(LOG_ERR, "Could not convert longpoll data to work");
		free_work(work);
		return;
	}
	total_getworks++;
	pool->getwork_requested++;
	work->pool = pool;
	copy_time(&work->tv_getwork, tv_lp);
	copy_time(&work->tv_getwork_reply, tv_lp_reply);
	calc_diff(work, 0);

	if (pool->enabled == POOL_REJECTING)
		work->mandatory = true;

	work->longpoll = true;
	work->getwork_mode = GETWORK_MODE_LP;

	update_last_work(work);

	/* We'll be checking this work item twice, but we already know it's
	 * from a new block so explicitly force the new block detection now
	 * rather than waiting for it to hit the stage thread. This also
	 * allows testwork to know whether LP discovered the block or not. */
	test_work_current(work);

	/* Don't use backup LPs as work if we have failover-only enabled. Use
	 * the longpoll work from a pool that has been rejecting shares as a
	 * way to detect when the pool has recovered.
	 */
	if (pool != current_pool() && opt_fail_only && pool->enabled != POOL_REJECTING) {
		free_work(work);
		return;
	}

	work = clone_work(work);

	applog(LOG_DEBUG, "Pushing converted work to stage thread");

	stage_work(work);
	applog(LOG_DEBUG, "Converted longpoll data to work");
}

/* If we want longpoll, enable it for the chosen default pool, or, if
 * the pool does not support longpoll, find the first one that does
 * and use its longpoll support */
static
struct pool *_select_longpoll_pool(struct pool *cp, bool(*func)(struct pool *))
{
	int i;

	if (func(cp))
		return cp;
	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];
		if (cp->goal != pool->goal)
			continue;

		if (func(pool))
			return pool;
	}
	return NULL;
}

/* This will make the longpoll thread wait till it's the current pool, or it
 * has been flagged as rejecting, before attempting to open any connections.
 */
static void wait_lpcurrent(struct pool *pool)
{
	mutex_lock(&lp_lock);
	while (!cnx_needed(pool))
	{
		pool->lp_active = false;
		pthread_cond_wait(&lp_cond, &lp_lock);
	}
	mutex_unlock(&lp_lock);
}

static curl_socket_t save_curl_socket(void *vpool, __maybe_unused curlsocktype purpose, struct curl_sockaddr *addr) {
	struct pool *pool = vpool;
	curl_socket_t sock = bfg_socket(addr->family, addr->socktype, addr->protocol);
	pool->lp_socket = sock;
	return sock;
}

static void *longpoll_thread(void *userdata)
{
	struct pool *cp = (struct pool *)userdata;
	/* This *pool is the source of the actual longpoll, not the pool we've
	 * tied it to */
	struct timeval start, reply, end;
	struct pool *pool = NULL;
	char threadname[20];
	CURL *curl = NULL;
	int failures = 0;
	char *lp_url;
	int rolltime;

#ifndef HAVE_PTHREAD_CANCEL
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#endif

	snprintf(threadname, 20, "longpoll%u", cp->pool_no);
	RenameThread(threadname);

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		return NULL;
	}

retry_pool:
	pool = select_longpoll_pool(cp);
	if (!pool) {
		applog(LOG_WARNING, "No suitable long-poll found for %s", cp->rpc_url);
		while (!pool) {
			cgsleep_ms(60000);
			pool = select_longpoll_pool(cp);
		}
	}

	if (pool->has_stratum) {
		applog(LOG_WARNING, "Block change for %s detection via %s stratum",
		       cp->rpc_url, pool->rpc_url);
		goto out;
	}

	/* Any longpoll from any pool is enough for this to be true */
	pool->goal->have_longpoll = true;

	wait_lpcurrent(cp);

	{
		lp_url = pool->lp_url;
		if (cp == pool)
			applog(LOG_WARNING, "Long-polling activated for %s (%s)", lp_url, pool_protocol_name(pool->lp_proto));
		else
			applog(LOG_WARNING, "Long-polling activated for %s via %s (%s)", cp->rpc_url, lp_url, pool_protocol_name(pool->lp_proto));
	}

	while (42) {
		json_t *val, *soval;

		struct work *work = make_work();
		char *lpreq;
		lpreq = prepare_rpc_req(work, pool->lp_proto, pool->lp_id, pool);
		work->pool = pool;
		if (!lpreq)
		{
			free_work(work);
			goto lpfail;
		}

		wait_lpcurrent(cp);

		cgtime(&start);

		/* Longpoll connections can be persistent for a very long time
		 * and any number of issues could have come up in the meantime
		 * so always establish a fresh connection instead of relying on
		 * a persistent one. */
		curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1);
		curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1);
		curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, save_curl_socket);
		curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, pool);
		val = json_rpc_call(curl, lp_url, pool->rpc_userpass,
				    lpreq, false, true, &rolltime, pool, false);
		pool->lp_socket = CURL_SOCKET_BAD;

		cgtime(&reply);

		free(lpreq);

		if (likely(val)) {
			soval = json_object_get(json_object_get(val, "result"), "submitold");
			if (soval)
				pool->submit_old = json_is_true(soval);
			else
				pool->submit_old = false;
			convert_to_work(val, rolltime, pool, work, &start, &reply);
			failures = 0;
			json_decref(val);
		} else {
			/* Some pools regularly drop the longpoll request so
			 * only see this as longpoll failure if it happens
			 * immediately and just restart it the rest of the
			 * time. */
			cgtime(&end);
			free_work(work);
			if (end.tv_sec - start.tv_sec <= 30)
			{
				if (failures == 1)
					applog(LOG_WARNING, "longpoll failed for %s, retrying every 30s", lp_url);
lpfail:
				cgsleep_ms(30000);
			}
		}

		if (pool != cp) {
			pool = select_longpoll_pool(cp);
			if (pool->has_stratum) {
				applog(LOG_WARNING, "Block change for %s detection via %s stratum",
				       cp->rpc_url, pool->rpc_url);
				break;
			}
			if (unlikely(!pool))
				goto retry_pool;
		}

		if (unlikely(pool->removed))
			break;
	}

out:
	pool->lp_active = false;
	curl_easy_cleanup(curl);

	return NULL;
}

static void stop_longpoll(void)
{
	int i;
	
	want_longpoll = false;
	for (i = 0; i < total_pools; ++i)
	{
		struct pool *pool = pools[i];
		
		if (unlikely(!pool->lp_started))
			continue;
		
		pool->lp_started = false;
		pthread_cancel(pool->longpoll_thread);
	}
	
	struct mining_goal_info *goal, *tmpgoal;
	HASH_ITER(hh, mining_goals, goal, tmpgoal)
	{
		goal->have_longpoll = false;
	}
}

static void start_longpoll(void)
{
	int i;
	
	want_longpoll = true;
	for (i = 0; i < total_pools; ++i)
	{
		struct pool *pool = pools[i];
		
		if (unlikely(pool->removed || pool->lp_started || !pool->lp_url))
			continue;
		
		pool->lp_started = true;
		if (unlikely(pthread_create(&pool->longpoll_thread, NULL, longpoll_thread, (void *)pool)))
			quit(1, "Failed to create pool longpoll thread");
	}
}

void reinit_device(struct cgpu_info *cgpu)
{
	if (cgpu->drv->reinit_device)
		cgpu->drv->reinit_device(cgpu);
}

static struct timeval rotate_tv;

/* We reap curls if they are unused for over a minute */
static void reap_curl(struct pool *pool)
{
	struct curl_ent *ent, *iter;
	struct timeval now;
	int reaped = 0;

	cgtime(&now);

	mutex_lock(&pool->pool_lock);
	LL_FOREACH_SAFE(pool->curllist, ent, iter) {
		if (pool->curls < 2)
			break;
		if (now.tv_sec - ent->tv.tv_sec > 300) {
			reaped++;
			pool->curls--;
			LL_DELETE(pool->curllist, ent);
			curl_easy_cleanup(ent->curl);
			free(ent);
		}
	}
	mutex_unlock(&pool->pool_lock);

	if (reaped)
		applog(LOG_DEBUG, "Reaped %d curl%s from pool %d", reaped, reaped > 1 ? "s" : "", pool->pool_no);
}

static void *watchpool_thread(void __maybe_unused *userdata)
{
	int intervals = 0;

#ifndef HAVE_PTHREAD_CANCEL
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#endif

	RenameThread("watchpool");

	while (42) {
		struct timeval now;
		int i;

		if (++intervals > 20)
			intervals = 0;
		cgtime(&now);

		for (i = 0; i < total_pools; i++) {
			struct pool *pool = pools[i];

			if (!opt_benchmark)
				reap_curl(pool);

			/* Get a rolling utility per pool over 10 mins */
			if (intervals > 19) {
				int shares = pool->diff1 - pool->last_shares;

				pool->last_shares = pool->diff1;
				pool->utility = (pool->utility + (double)shares * 0.63) / 1.63;
				pool->shares = pool->utility;
			}

			if (pool->enabled == POOL_DISABLED)
				continue;

			/* Don't start testing any pools if the test threads
			 * from startup are still doing their first attempt. */
			if (unlikely(pool->testing)) {
				pthread_join(pool->test_thread, NULL);
			}

			/* Test pool is idle once every minute */
			if (pool->idle && now.tv_sec - pool->tv_idle.tv_sec > 30) {
				if (pool_active(pool, true) && pool_tclear(pool, &pool->idle))
					pool_resus(pool);
			}

			/* Only switch pools if the failback pool has been
			 * alive for more than 5 minutes (default) to prevent
			 * intermittently failing pools from being used. */
			if (!pool->idle && pool->enabled == POOL_ENABLED && pool_strategy == POOL_FAILOVER && pool->prio < cp_prio() && now.tv_sec - pool->tv_idle.tv_sec > opt_fail_switch_delay)
			{
				if (opt_fail_switch_delay % 60)
					applog(LOG_WARNING, "Pool %d %s stable for %d second%s",
					       pool->pool_no, pool->rpc_url,
					       opt_fail_switch_delay,
					       (opt_fail_switch_delay == 1 ? "" : "s"));
				else
					applog(LOG_WARNING, "Pool %d %s stable for %d minute%s",
					       pool->pool_no, pool->rpc_url,
					       opt_fail_switch_delay / 60,
					       (opt_fail_switch_delay == 60 ? "" : "s"));
				switch_pools(NULL);
			}
		}

		if (current_pool()->idle)
			switch_pools(NULL);

		if (pool_strategy == POOL_ROTATE && now.tv_sec - rotate_tv.tv_sec > 60 * opt_rotate_period) {
			cgtime(&rotate_tv);
			switch_pools(NULL);
		}

		cgsleep_ms(30000);
			
	}
	return NULL;
}

void mt_enable(struct thr_info *thr)
{
	applog(LOG_DEBUG, "Waking up thread %d", thr->id);
	notifier_wake(thr->notifier);
}

void proc_enable(struct cgpu_info *cgpu)
{
	int j;

	cgpu->deven = DEV_ENABLED;
	for (j = cgpu->threads ?: 1; j--; )
		mt_enable(cgpu->thr[j]);
}

#define device_recovered(cgpu)  proc_enable(cgpu)

void cgpu_set_defaults(struct cgpu_info * const cgpu)
{
	struct string_elist *setstr_elist;
	const char *p, *p2;
	char replybuf[0x2000];
	size_t L;
	DL_FOREACH(opt_set_device_list, setstr_elist)
	{
		const char * const setstr = setstr_elist->string;
		p = strchr(setstr, ':');
		if (!p)
			p = setstr;
		{
			L = p - setstr;
			char pattern[L + 1];
			if (L)
				memcpy(pattern, setstr, L);
			pattern[L] = '\0';
			if (!cgpu_match(pattern, cgpu))
				continue;
		}
		
		applog(LOG_DEBUG, "%"PRIpreprv": %s: Matched with set default: %s",
		       cgpu->proc_repr, __func__, setstr);
		
		if (p[0] == ':')
			++p;
		p2 = strchr(p, '=');
		if (!p2)
		{
			L = strlen(p);
			p2 = "";
		}
		else
		{
			L = p2 - p;
			++p2;
		}
		char opt[L + 1];
		if (L)
			memcpy(opt, p, L);
		opt[L] = '\0';
		
		L = strlen(p2);
		char setval[L + 1];
		if (L)
			memcpy(setval, p2, L);
		setval[L] = '\0';
		
		enum bfg_set_device_replytype success;
		p = proc_set_device(cgpu, opt, setval, replybuf, &success);
		switch (success)
		{
			case SDR_OK:
				applog(LOG_DEBUG, "%"PRIpreprv": Applied rule %s%s%s",
				       cgpu->proc_repr, setstr,
				       p ? ": " : "", p ?: "");
				break;
			case SDR_ERR:
			case SDR_HELP:
			case SDR_UNKNOWN:
				applog(LOG_DEBUG, "%"PRIpreprv": Applying rule %s: %s",
				       cgpu->proc_repr, setstr, p);
				break;
			case SDR_AUTO:
			case SDR_NOSUPP:
				applog(LOG_DEBUG, "%"PRIpreprv": set_device is not implemented (trying to apply rule: %s)",
				       cgpu->proc_repr, setstr);
		}
	}
	cgpu->already_set_defaults = true;
}

void drv_set_defaults(const struct device_drv * const drv, const void *datap, void *userp, const char * const devpath, const char * const serial, const int mode)
{
	struct device_drv dummy_drv = *drv;
	struct cgpu_info dummy_cgpu = {
		.drv = &dummy_drv,
		.device = &dummy_cgpu,
		.device_id = -1,
		.proc_id = -1,
		.device_data = userp,
		.device_path = devpath,
		.dev_serial = serial,
	};
	strcpy(dummy_cgpu.proc_repr, drv->name);
	switch (mode)
	{
		case 0:
			dummy_drv.set_device = datap;
			break;
		case 1:
			dummy_drv.set_device = NULL;
			dummy_cgpu.set_device_funcs = datap;
			break;
	}
	cgpu_set_defaults(&dummy_cgpu);
}

/* Makes sure the hashmeter keeps going even if mining threads stall, updates
 * the screen at regular intervals, and restarts threads if they appear to have
 * died. */
#define WATCHDOG_SICK_TIME		60
#define WATCHDOG_DEAD_TIME		600
#define WATCHDOG_SICK_COUNT		(WATCHDOG_SICK_TIME/WATCHDOG_INTERVAL)
#define WATCHDOG_DEAD_COUNT		(WATCHDOG_DEAD_TIME/WATCHDOG_INTERVAL)

static void *watchdog_thread(void __maybe_unused *userdata)
{
	const unsigned int interval = WATCHDOG_INTERVAL;
	struct timeval zero_tv;

#ifndef HAVE_PTHREAD_CANCEL
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#endif

	RenameThread("watchdog");

	memset(&zero_tv, 0, sizeof(struct timeval));
	cgtime(&rotate_tv);

	while (1) {
		int i;
		struct timeval now;

		sleep(interval);

		discard_stale();

		hashmeter(-1, &zero_tv, 0);

#ifdef HAVE_CURSES
		const int ts = total_staged(true);
		if (curses_active_locked()) {
			change_logwinsize();
			curses_print_status(ts);
			_refresh_devstatus(true);
			touchwin(logwin);
			wrefresh(logwin);
			unlock_curses();
		}
#endif

		cgtime(&now);

		if (!sched_paused && !should_run()) {
			applog(LOG_WARNING, "Pausing execution as per stop time %02d:%02d scheduled",
			       schedstop.tm.tm_hour, schedstop.tm.tm_min);
			if (!schedstart.enable) {
				quit(0, "Terminating execution as planned");
				break;
			}

			applog(LOG_WARNING, "Will restart execution as scheduled at %02d:%02d",
			       schedstart.tm.tm_hour, schedstart.tm.tm_min);
			sched_paused = true;

			rd_lock(&mining_thr_lock);
			for (i = 0; i < mining_threads; i++)
				mining_thr[i]->pause = true;
			rd_unlock(&mining_thr_lock);
		} else if (sched_paused && should_run()) {
			applog(LOG_WARNING, "Restarting execution as per start time %02d:%02d scheduled",
				schedstart.tm.tm_hour, schedstart.tm.tm_min);
			if (schedstop.enable)
				applog(LOG_WARNING, "Will pause execution as scheduled at %02d:%02d",
					schedstop.tm.tm_hour, schedstop.tm.tm_min);
			sched_paused = false;

			for (i = 0; i < mining_threads; i++) {
				struct thr_info *thr;

				thr = get_thread(i);
				thr->pause = false;
			}
			
			for (i = 0; i < total_devices; ++i)
			{
				struct cgpu_info *cgpu = get_devices(i);
				
				/* Don't touch disabled devices */
				if (cgpu->deven == DEV_DISABLED)
					continue;
				proc_enable(cgpu);
			}
		}

		for (i = 0; i < total_devices; ++i) {
			struct cgpu_info *cgpu = get_devices(i);
			if (!cgpu->disable_watchdog)
				bfg_watchdog(cgpu, &now);
		}
	}

	return NULL;
}

void bfg_watchdog(struct cgpu_info * const cgpu, struct timeval * const tvp_now)
{
			struct thr_info *thr = cgpu->thr[0];
			enum dev_enable *denable;
			char *dev_str = cgpu->proc_repr;

			if (likely(drv_ready(cgpu)))
			{
				if (unlikely(!cgpu->already_set_defaults))
					cgpu_set_defaults(cgpu);
				if (cgpu->drv->get_stats)
					cgpu->drv->get_stats(cgpu);
			}

			denable = &cgpu->deven;

			if (cgpu->drv->watchdog)
				cgpu->drv->watchdog(cgpu, tvp_now);
			
			/* Thread is disabled */
			if (*denable == DEV_DISABLED)
				return;
			else
			if (*denable == DEV_RECOVER_ERR) {
				if (opt_restart && timer_elapsed(&cgpu->tv_device_last_not_well, NULL) > cgpu->reinit_backoff) {
					applog(LOG_NOTICE, "Attempting to reinitialize %s",
					       dev_str);
					if (cgpu->reinit_backoff < 300)
						cgpu->reinit_backoff *= 2;
					device_recovered(cgpu);
				}
				return;
			}
			else
			if (*denable == DEV_RECOVER) {
				if (opt_restart && cgpu->temp < cgpu->targettemp) {
					applog(LOG_NOTICE, "%s recovered to temperature below target, re-enabling",
					       dev_str);
					device_recovered(cgpu);
				}
				dev_error_update(cgpu, REASON_DEV_THERMAL_CUTOFF);
				return;
			}
			else
			if (cgpu->temp > cgpu->cutofftemp)
			{
				applog(LOG_WARNING, "%s hit thermal cutoff limit at %dC, disabling!",
				       dev_str, (int)cgpu->temp);
				*denable = DEV_RECOVER;

				dev_error(cgpu, REASON_DEV_THERMAL_CUTOFF);
				run_cmd(cmd_idle);
			}

			if (thr->getwork) {
				if (cgpu->status == LIFE_WELL && thr->getwork < tvp_now->tv_sec - opt_log_interval) {
					int thrid;
					bool cgpu_idle = true;
					thr->rolling = 0;
					for (thrid = 0; thrid < cgpu->threads; ++thrid)
						if (!cgpu->thr[thrid]->getwork)
							cgpu_idle = false;
					if (cgpu_idle) {
						cgpu->rolling = 0;
						cgpu->status = LIFE_WAIT;
					}
				}
				return;
			}
			else if (cgpu->status == LIFE_WAIT)
				cgpu->status = LIFE_WELL;

#ifdef USE_CPUMINING
			if (!strcmp(cgpu->drv->dname, "cpu"))
				return;
#endif
			if (cgpu->status != LIFE_WELL && (tvp_now->tv_sec - thr->last.tv_sec < WATCHDOG_SICK_TIME)) {
				if (likely(cgpu->status != LIFE_INIT && cgpu->status != LIFE_INIT2))
				applog(LOG_ERR, "%s: Recovered, declaring WELL!", dev_str);
				cgpu->status = LIFE_WELL;
				cgpu->device_last_well = time(NULL);
			} else if (cgpu->status == LIFE_WELL && (tvp_now->tv_sec - thr->last.tv_sec > WATCHDOG_SICK_TIME)) {
				thr->rolling = cgpu->rolling = 0;
				cgpu->status = LIFE_SICK;
				applog(LOG_ERR, "%s: Idle for more than 60 seconds, declaring SICK!", dev_str);
				cgtime(&thr->sick);

				dev_error(cgpu, REASON_DEV_SICK_IDLE_60);
				run_cmd(cmd_sick);
				
				if (opt_restart && cgpu->drv->reinit_device) {
					applog(LOG_ERR, "%s: Attempting to restart", dev_str);
					reinit_device(cgpu);
				}
			} else if (cgpu->status == LIFE_SICK && (tvp_now->tv_sec - thr->last.tv_sec > WATCHDOG_DEAD_TIME)) {
				cgpu->status = LIFE_DEAD;
				applog(LOG_ERR, "%s: Not responded for more than 10 minutes, declaring DEAD!", dev_str);
				cgtime(&thr->sick);

				dev_error(cgpu, REASON_DEV_DEAD_IDLE_600);
				run_cmd(cmd_dead);
			} else if (tvp_now->tv_sec - thr->sick.tv_sec > 60 &&
				   (cgpu->status == LIFE_SICK || cgpu->status == LIFE_DEAD)) {
				/* Attempt to restart a GPU that's sick or dead once every minute */
				cgtime(&thr->sick);
				if (opt_restart)
					reinit_device(cgpu);
			}
}

static void log_print_status(struct cgpu_info *cgpu)
{
	char logline[255];

	get_statline(logline, sizeof(logline), cgpu);
	applog(LOG_WARNING, "%s", logline);
}

void print_summary(void)
{
	struct timeval diff;
	int hours, mins, secs, i;
	double utility, efficiency = 0.0;
	char xfer[(ALLOC_H2B_SPACED*2)+4+1], bw[(ALLOC_H2B_SPACED*2)+6+1];
	int pool_secs;

	timersub(&total_tv_end, &total_tv_start, &diff);
	hours = diff.tv_sec / 3600;
	mins = (diff.tv_sec % 3600) / 60;
	secs = diff.tv_sec % 60;

	utility = total_accepted / total_secs * 60;
	efficiency = total_bytes_xfer ? total_diff_accepted * 2048. / total_bytes_xfer : 0.0;

	applog(LOG_WARNING, "\nSummary of runtime statistics:\n");
	applog(LOG_WARNING, "Started at %s", datestamp);
	if (total_pools == 1)
		applog(LOG_WARNING, "Pool: %s", pools[0]->rpc_url);
#if defined(USE_CPUMINING) && defined(USE_SHA256D)
	if (opt_n_threads > 0)
		applog(LOG_WARNING, "CPU hasher algorithm used: %s", algo_names[opt_algo]);
#endif
	applog(LOG_WARNING, "Runtime: %d hrs : %d mins : %d secs", hours, mins, secs);
	applog(LOG_WARNING, "Average hashrate: %.1f Megahash/s", total_mhashes_done / total_secs);
	applog(LOG_WARNING, "Solved blocks: %d", found_blocks);
	applog(LOG_WARNING, "Best share difficulty: %s", best_share);
	applog(LOG_WARNING, "Share submissions: %d", total_accepted + total_rejected);
	applog(LOG_WARNING, "Accepted shares: %d", total_accepted);
	applog(LOG_WARNING, "Rejected shares: %d + %d stale (%.2f%%)",
	       total_rejected, total_stale,
	       (float)(total_rejected + total_stale) / (float)(total_rejected + total_stale + total_accepted)
	);
	applog(LOG_WARNING, "Accepted difficulty shares: %1.f", total_diff_accepted);
	applog(LOG_WARNING, "Rejected difficulty shares: %1.f", total_diff_rejected);
	applog(LOG_WARNING, "Hardware errors: %d", hw_errors);
	applog(LOG_WARNING, "Network transfer: %s  (%s)",
	       multi_format_unit2(xfer, sizeof(xfer), true, "B", H2B_SPACED, " / ", 2,
	                         (float)total_bytes_rcvd,
	                         (float)total_bytes_sent),
	       multi_format_unit2(bw, sizeof(bw), true, "B/s", H2B_SPACED, " / ", 2,
	                         (float)(total_bytes_rcvd / total_secs),
	                         (float)(total_bytes_sent / total_secs)));
	applog(LOG_WARNING, "Efficiency (accepted shares * difficulty / 2 KB): %.2f", efficiency);
	applog(LOG_WARNING, "Utility (accepted shares / min): %.2f/min\n", utility);

	applog(LOG_WARNING, "Unable to get work from server occasions: %d", total_go);
	applog(LOG_WARNING, "Work items generated locally: %d", local_work);
	applog(LOG_WARNING, "Submitting work remotely delay occasions: %d", total_ro);
	applog(LOG_WARNING, "New blocks detected on network: %d\n", new_blocks);

	if (total_pools > 1) {
		for (i = 0; i < total_pools; i++) {
			struct pool *pool = pools[i];

			applog(LOG_WARNING, "Pool: %s", pool->rpc_url);
			if (pool->solved)
				applog(LOG_WARNING, "SOLVED %d BLOCK%s!", pool->solved, pool->solved > 1 ? "S" : "");
			applog(LOG_WARNING, " Share submissions: %d", pool->accepted + pool->rejected);
			applog(LOG_WARNING, " Accepted shares: %d", pool->accepted);
			applog(LOG_WARNING, " Rejected shares: %d + %d stale (%.2f%%)",
			       pool->rejected, pool->stale_shares,
			       (float)(pool->rejected + pool->stale_shares) / (float)(pool->rejected + pool->stale_shares + pool->accepted)
			);
			applog(LOG_WARNING, " Accepted difficulty shares: %1.f", pool->diff_accepted);
			applog(LOG_WARNING, " Rejected difficulty shares: %1.f", pool->diff_rejected);
			pool_secs = timer_elapsed(&pool->cgminer_stats.start_tv, NULL);
			applog(LOG_WARNING, " Network transfer: %s  (%s)",
			       multi_format_unit2(xfer, sizeof(xfer), true, "B", H2B_SPACED, " / ", 2,
			                         (float)pool->cgminer_pool_stats.net_bytes_received,
			                         (float)pool->cgminer_pool_stats.net_bytes_sent),
			       multi_format_unit2(bw, sizeof(bw), true, "B/s", H2B_SPACED, " / ", 2,
			                         (float)(pool->cgminer_pool_stats.net_bytes_received / pool_secs),
			                         (float)(pool->cgminer_pool_stats.net_bytes_sent / pool_secs)));
			uint64_t pool_bytes_xfer = pool->cgminer_pool_stats.net_bytes_received + pool->cgminer_pool_stats.net_bytes_sent;
			efficiency = pool_bytes_xfer ? pool->diff_accepted * 2048. / pool_bytes_xfer : 0.0;
			applog(LOG_WARNING, " Efficiency (accepted * difficulty / 2 KB): %.2f", efficiency);

			applog(LOG_WARNING, " Items worked on: %d", pool->works);
			applog(LOG_WARNING, " Unable to get work from server occasions: %d", pool->getfail_occasions);
			applog(LOG_WARNING, " Submitting work remotely delay occasions: %d\n", pool->remotefail_occasions);
		}
	}

	if (opt_quit_summary != BQS_NONE)
	{
		if (opt_quit_summary == BQS_DEFAULT)
		{
			if (total_devices < 25)
				opt_quit_summary = BQS_PROCS;
			else
				opt_quit_summary = BQS_DEVS;
		}
		
		if (opt_quit_summary == BQS_DETAILED)
			include_serial_in_statline = true;
		applog(LOG_WARNING, "Summary of per device statistics:\n");
		for (i = 0; i < total_devices; ++i) {
			struct cgpu_info *cgpu = get_devices(i);
			
			if (!cgpu->proc_id)
			{
				// Device summary line
				opt_show_procs = false;
				log_print_status(cgpu);
				opt_show_procs = true;
			}
			if ((opt_quit_summary == BQS_PROCS || opt_quit_summary == BQS_DETAILED) && cgpu->procs > 1)
				log_print_status(cgpu);
		}
	}

	if (opt_shares) {
		applog(LOG_WARNING, "Mined %g accepted shares of %g requested\n", total_diff_accepted, opt_shares);
		if (opt_shares > total_diff_accepted)
			applog(LOG_WARNING, "WARNING - Mined only %g shares of %g requested.", total_diff_accepted, opt_shares);
	}
	applog(LOG_WARNING, " ");

	fflush(stderr);
	fflush(stdout);
}

void _bfg_clean_up(bool restarting)
{
#ifdef USE_OPENCL
	clear_adl(nDevs);
#endif
#ifdef HAVE_LIBUSB
	if (likely(have_libusb))
        libusb_exit(NULL);
#endif

	cgtime(&total_tv_end);
#ifdef WIN32
	timeEndPeriod(1);
#endif
	if (!restarting) {
		/* Attempting to disable curses or print a summary during a
		 * restart can lead to a deadlock. */
#ifdef HAVE_CURSES
		disable_curses();
#endif
		if (!opt_realquiet && successful_connect)
			print_summary();
	}

#ifdef USE_CPUMINING
	if (opt_n_threads > 0)
		free(cpus);
#endif

	curl_global_cleanup();
	
#ifdef WIN32
	WSACleanup();
#endif
}

void _quit(int status)
{
	if (status) {
		const char *ev = getenv("__BFGMINER_SEGFAULT_ERRQUIT");
		if (unlikely(ev && ev[0] && ev[0] != '0')) {
			int *p = NULL;
			// NOTE debugger can bypass with: p = &p
			*p = status;  // Segfault, hopefully dumping core
		}
	}

#if defined(unix) || defined(__APPLE__)
	if (forkpid > 0) {
		kill(forkpid, SIGTERM);
		forkpid = 0;
	}
#endif

	exit(status);
}

#ifdef HAVE_CURSES
char *curses_input(const char *query)
{
	char *input;

	echo();
	input = malloc(255);
	if (!input)
		quit(1, "Failed to malloc input");
	leaveok(logwin, false);
	wlogprint("%s:\n", query);
	wgetnstr(logwin, input, 255);
	if (!strlen(input))
	{
		free(input);
		input = NULL;
	}
	leaveok(logwin, true);
	noecho();
	return input;
}
#endif

static void *test_pool_thread(void *arg)
{
	struct pool *pool = (struct pool *)arg;

	if (pool_active(pool, false)) {
		pool_tset(pool, &pool->lagging);
		pool_tclear(pool, &pool->idle);
		bool first_pool = false;

		cg_wlock(&control_lock);
		if (!pools_active) {
			currentpool = pool;
			if (pool->pool_no != 0)
				first_pool = true;
			pools_active = true;
		}
		cg_wunlock(&control_lock);

		if (unlikely(first_pool))
			applog(LOG_NOTICE, "Switching to pool %d %s - first alive pool", pool->pool_no, pool->rpc_url);
		else
			applog(LOG_NOTICE, "Pool %d %s alive", pool->pool_no, pool->rpc_url);

		switch_pools(NULL);
	} else
		pool_died(pool);

	pool->testing = false;
	return NULL;
}

/* Always returns true that the pool details were added unless we are not
 * live, implying this is the only pool being added, so if no pools are
 * active it returns false. */
bool add_pool_details(struct pool *pool, bool live, char *url, char *user, char *pass)
{
	size_t siz;

	pool_set_uri(pool, url);
	pool->rpc_user = user;
	pool->rpc_pass = pass;
	siz = strlen(pool->rpc_user) + strlen(pool->rpc_pass) + 2;
	pool->rpc_userpass = malloc(siz);
	if (!pool->rpc_userpass)
		quit(1, "Failed to malloc userpass");
	snprintf(pool->rpc_userpass, siz, "%s:%s", pool->rpc_user, pool->rpc_pass);

	pool->testing = true;
	pool->idle = true;
	enable_pool(pool);

	pthread_create(&pool->test_thread, NULL, test_pool_thread, (void *)pool);
	if (!live) {
		pthread_join(pool->test_thread, NULL);
		return pools_active;
	}
	return true;
}

#ifdef HAVE_CURSES
static bool input_pool(bool live)
{
	char *url = NULL, *user = NULL, *pass = NULL;
	struct pool *pool;
	bool ret = false;

	immedok(logwin, true);
	wlogprint("Input server details.\n");

	url = curses_input("URL");
	if (!url)
		goto out;

	user = curses_input("Username");
	if (!user)
		goto out;

	pass = curses_input("Password");
	if (!pass)
		pass = calloc(1, 1);

	pool = add_pool();

	if (!detect_stratum(pool, url) && strncmp(url, "http://", 7) &&
	    strncmp(url, "https://", 8)) {
		char *httpinput;

		httpinput = malloc(256);
		if (!httpinput)
			quit(1, "Failed to malloc httpinput");
		strcpy(httpinput, "http://");
		strncat(httpinput, url, 248);
		free(url);
		url = httpinput;
	}

	ret = add_pool_details(pool, live, url, user, pass);
out:
	immedok(logwin, false);

	if (!ret) {
		if (url)
			free(url);
		if (user)
			free(user);
		if (pass)
			free(pass);
	}
	return ret;
}
#endif

#if BLKMAKER_VERSION > 1 && defined(USE_SHA256D)
static
bool _add_local_gbt(const char * const filepath, void *userp)
{
	const bool * const live_p = userp;
	struct pool *pool;
	char buf[0x100];
	char *rpcuser = NULL, *rpcpass = NULL, *rpcconnect = NULL;
	int rpcport = 0, rpcssl = -101;
	FILE * const F = fopen(filepath, "r");
	if (!F)
		applogr(false, LOG_WARNING, "%s: Failed to open %s for reading", "add_local_gbt", filepath);
	
	while (fgets(buf, sizeof(buf), F))
	{
		if (!strncasecmp(buf, "rpcuser=", 8))
			rpcuser = trimmed_strdup(&buf[8]);
		else
		if (!strncasecmp(buf, "rpcpassword=", 12))
			rpcpass = trimmed_strdup(&buf[12]);
		else
		if (!strncasecmp(buf, "rpcport=", 8))
			rpcport = atoi(&buf[8]);
		else
		if (!strncasecmp(buf, "rpcssl=", 7))
			rpcssl = atoi(&buf[7]);
		else
		if (!strncasecmp(buf, "rpcconnect=", 11))
			rpcconnect = trimmed_strdup(&buf[11]);
		else
			continue;
		if (rpcuser && rpcpass && rpcport && rpcssl != -101 && rpcconnect)
			break;
	}
	
	fclose(F);
	
	if (!rpcpass)
	{
		applog(LOG_DEBUG, "%s: Did not find rpcpassword in %s", "add_local_gbt", filepath);
err:
		free(rpcuser);
		free(rpcpass);
		goto out;
	}
	
	if (!rpcport)
		rpcport = 8332;
	
	if (rpcssl == -101)
		rpcssl = 0;
	
	const bool have_cbaddr = get_mining_goal("default")->generation_script;
	
	const int uri_sz = 0x30;
	char * const uri = malloc(uri_sz);
	snprintf(uri, uri_sz, "http%s://%s:%d/%s#allblocks", rpcssl ? "s" : "", rpcconnect ?: "localhost", rpcport, have_cbaddr ? "" : "#getcbaddr");
	
	char hfuri[0x40];
	if (rpcconnect)
		snprintf(hfuri, sizeof(hfuri), "%s:%d", rpcconnect, rpcport);
	else
		snprintf(hfuri, sizeof(hfuri), "port %d", rpcport);
	applog(LOG_DEBUG, "Local bitcoin RPC server on %s found in %s", hfuri, filepath);
	
	for (int i = 0; i < total_pools; ++i)
	{
		struct pool *pool = pools[i];
		
		if (!(strcmp(pool->rpc_url, uri) || strcmp(pool->rpc_pass, rpcpass)))
		{
			applog(LOG_DEBUG, "Server on %s is already configured, not adding as failover", hfuri);
			free(uri);
			goto err;
		}
	}
	
	pool = add_pool();
	if (!pool)
	{
		applog(LOG_ERR, "%s: Error adding pool for bitcoin configured in %s", "add_local_gbt", filepath);
		goto err;
	}
	
	if (!rpcuser)
		rpcuser = "";
	
	pool->quota = 0;
	adjust_quota_gcd();
	pool->failover_only = true;
	add_pool_details(pool, *live_p, uri, rpcuser, rpcpass);
	
	applog(LOG_NOTICE, "Added local bitcoin RPC server on %s as pool %d", hfuri, pool->pool_no);
	
out:
	return false;
}

static
void add_local_gbt(bool live)
{
	appdata_file_call("Bitcoin", "bitcoin.conf", _add_local_gbt, &live);
}
#endif

#if defined(unix) || defined(__APPLE__)
static void fork_monitor()
{
	// Make a pipe: [readFD, writeFD]
	int pfd[2];
	int r = pipe(pfd);

	if (r < 0) {
		perror("pipe - failed to create pipe for --monitor");
		exit(1);
	}

	// Make stderr write end of pipe
	fflush(stderr);
	r = dup2(pfd[1], 2);
	if (r < 0) {
		perror("dup2 - failed to alias stderr to write end of pipe for --monitor");
		exit(1);
	}
	r = close(pfd[1]);
	if (r < 0) {
		perror("close - failed to close write end of pipe for --monitor");
		exit(1);
	}

	// Don't allow a dying monitor to kill the main process
	sighandler_t sr0 = signal(SIGPIPE, SIG_IGN);
	sighandler_t sr1 = signal(SIGPIPE, SIG_IGN);
	if (SIG_ERR == sr0 || SIG_ERR == sr1) {
		perror("signal - failed to edit signal mask for --monitor");
		exit(1);
	}

	// Fork a child process
	forkpid = fork();
	if (forkpid < 0) {
		perror("fork - failed to fork child process for --monitor");
		exit(1);
	}

	// Child: launch monitor command
	if (0 == forkpid) {
		// Make stdin read end of pipe
		r = dup2(pfd[0], 0);
		if (r < 0) {
			perror("dup2 - in child, failed to alias read end of pipe to stdin for --monitor");
			exit(1);
		}
		close(pfd[0]);
		if (r < 0) {
			perror("close - in child, failed to close read end of  pipe for --monitor");
			exit(1);
		}

		// Launch user specified command
		execl("/bin/bash", "/bin/bash", "-c", opt_stderr_cmd, (char*)NULL);
		perror("execl - in child failed to exec user specified command for --monitor");
		exit(1);
	}

	// Parent: clean up unused fds and bail
	r = close(pfd[0]);
	if (r < 0) {
		perror("close - failed to close read end of pipe for --monitor");
		exit(1);
	}
}
#endif // defined(unix)

#ifdef HAVE_CURSES
#ifdef USE_UNICODE
static
wchar_t select_unicode_char(const wchar_t *opt)
{
	for ( ; *opt; ++opt)
		if (iswprint(*opt))
			return *opt;
	return '?';
}
#endif

void enable_curses(void) {
	int x;
	__maybe_unused int y;

	lock_curses();
	if (curses_active) {
		unlock_curses();
		return;
	}

#ifdef USE_UNICODE
	if (use_unicode)
	{
		setlocale(LC_CTYPE, "");
		if (iswprint(0xb0))
			have_unicode_degrees = true;
		unicode_micro = select_unicode_char(L"\xb5\u03bcu");
	}
#endif
	mainwin = initscr();
	start_color();
#if defined(PDCURSES) || defined(NCURSES_VERSION)
	if (ERR != use_default_colors())
		default_bgcolor = -1;
#endif
	if (has_colors() && ERR != init_pair(1, COLOR_WHITE, COLOR_BLUE))
	{
		menu_attr = COLOR_PAIR(1);
		if (ERR != init_pair(2, COLOR_RED, default_bgcolor))
			attr_bad |= COLOR_PAIR(2);
	}
	keypad(mainwin, true);
	getmaxyx(mainwin, y, x);
	statuswin = newwin(logstart, x, 0, 0);
	leaveok(statuswin, true);
	// For whatever reason, PDCurses crashes if the logwin is initialized to height y-logcursor
	// We resize the window later anyway, so just start it off at 1 :)
	logwin = newwin(1, 0, logcursor, 0);
	idlok(logwin, true);
	scrollok(logwin, true);
	leaveok(logwin, true);
	cbreak();
	noecho();
	nonl();
	curses_active = true;
	statusy = logstart;
	unlock_curses();
}
#endif

/* TODO: fix need a dummy CPU device_drv even if no support for CPU mining */
#ifndef USE_CPUMINING
struct device_drv cpu_drv;
struct device_drv cpu_drv = {
	.name = "CPU",
};
#endif

static int cgminer_id_count = 0;
static int device_line_id_count;

void register_device(struct cgpu_info *cgpu)
{
	cgpu->deven = DEV_ENABLED;

	wr_lock(&devices_lock);
	devices[cgpu->cgminer_id = cgminer_id_count++] = cgpu;
	wr_unlock(&devices_lock);

	if (!cgpu->proc_id)
		cgpu->device_line_id = device_line_id_count++;
	int thr_objs = cgpu->threads ?: 1;
	mining_threads += thr_objs;
	base_queue += thr_objs + cgpu->extra_work_queue;
	{
		const struct device_drv * const drv = cgpu->drv;
		struct mining_algorithm *malgo;
		LL_FOREACH(mining_algorithms, malgo)
		{
			if (drv_min_nonce_diff(drv, cgpu, malgo) < 0)
				continue;
			malgo->base_queue += thr_objs + cgpu->extra_work_queue;
		}
	}
#ifdef HAVE_CURSES
	adj_width(mining_threads, &dev_width);
#endif

	rwlock_init(&cgpu->qlock);
	cgpu->queued_work = NULL;
}

struct _cgpu_devid_counter {
	char name[4];
	int lastid;
	UT_hash_handle hh;
};

void renumber_cgpu(struct cgpu_info *cgpu)
{
	static struct _cgpu_devid_counter *devids = NULL;
	struct _cgpu_devid_counter *d;
	
	HASH_FIND_STR(devids, cgpu->drv->name, d);
	if (d)
		cgpu->device_id = ++d->lastid;
	else {
		d = malloc(sizeof(*d));
		memcpy(d->name, cgpu->drv->name, sizeof(d->name));
		cgpu->device_id = d->lastid = 0;
		HASH_ADD_STR(devids, name, d);
	}
	
	// Build repr strings
	sprintf(cgpu->dev_repr, "%s%2u", cgpu->drv->name, cgpu->device_id % 100);
	sprintf(cgpu->dev_repr_ns, "%s%u", cgpu->drv->name, cgpu->device_id % 100);
	strcpy(cgpu->proc_repr, cgpu->dev_repr);
	sprintf(cgpu->proc_repr_ns, "%s%u", cgpu->drv->name, cgpu->device_id);
	
	const int lpcount = cgpu->procs;
	if (lpcount > 1)
	{
		int ns;
		struct cgpu_info *slave;
		int lpdigits = 1;
		for (int i = lpcount; i > 26 && lpdigits < 3; i /= 26)
			++lpdigits;
		
		if (lpdigits > max_lpdigits)
			max_lpdigits = lpdigits;
		
		memset(&cgpu->proc_repr[5], 'a', lpdigits);
		cgpu->proc_repr[5 + lpdigits] = '\0';
		ns = strlen(cgpu->proc_repr_ns);
		strcpy(&cgpu->proc_repr_ns[ns], &cgpu->proc_repr[5]);
		
		slave = cgpu;
		for (int i = 1; i < lpcount; ++i)
		{
			slave = slave->next_proc;
			strcpy(slave->proc_repr, cgpu->proc_repr);
			strcpy(slave->proc_repr_ns, cgpu->proc_repr_ns);
			for (int x = i, y = lpdigits; --y, x; x /= 26)
			{
				slave->proc_repr_ns[ns + y] =
				slave->proc_repr[5 + y] += (x % 26);
			}
		}
	}
}

static bool my_blkmaker_sha256_callback(void *digest, const void *buffer, size_t length)
{
	sha256(buffer, length, digest);
	return true;
}

static
bool drv_algo_check(const struct device_drv * const drv)
{
	struct mining_goal_info *goal, *tmpgoal;
	HASH_ITER(hh, mining_goals, goal, tmpgoal)
	{
		if (drv_min_nonce_diff(drv, NULL, goal->malgo) >= 0)
			return true;
	}
	return false;
}

#ifndef HAVE_PTHREAD_CANCEL
extern void setup_pthread_cancel_workaround();
extern struct sigaction pcwm_orig_term_handler;
#endif

bool bfg_need_detect_rescan;
extern void probe_device(struct lowlevel_device_info *);
static void schedule_rescan(const struct timeval *);

static
void drv_detect_all()
{
	bool rescanning = false;
rescan:
	bfg_need_detect_rescan = false;
	
#ifdef HAVE_BFG_LOWLEVEL
	struct lowlevel_device_info * const infolist = lowlevel_scan(), *info, *infotmp;
	
	LL_FOREACH_SAFE(infolist, info, infotmp)
		probe_device(info);
	LL_FOREACH_SAFE(infolist, info, infotmp)
		pthread_join(info->probe_pth, NULL);
#endif
	
	struct driver_registration *reg;
	BFG_FOREACH_DRIVER_BY_PRIORITY(reg)
	{
		const struct device_drv * const drv = reg->drv;
		if (!(drv_algo_check(drv) && drv->drv_detect))
			continue;
		
		drv->drv_detect();
	}

#ifdef HAVE_BFG_LOWLEVEL
	lowlevel_scan_free();
#endif
	
	if (bfg_need_detect_rescan)
	{
		if (rescanning)
		{
			applog(LOG_DEBUG, "Device rescan requested a second time, delaying");
			struct timeval tv_when;
			timer_set_delay_from_now(&tv_when, rescan_delay_ms * 1000);
			schedule_rescan(&tv_when);
		}
		else
		{
			rescanning = true;
			applog(LOG_DEBUG, "Device rescan requested");
			goto rescan;
		}
	}
}

static
void allocate_cgpu(struct cgpu_info *cgpu, unsigned int *kp)
{
	struct thr_info *thr;
	int j;
	
	struct device_drv *api = cgpu->drv;
	cgpu->cgminer_stats.getwork_wait_min.tv_sec = MIN_SEC_UNSET;
	
	int threadobj = cgpu->threads;
	if (!threadobj)
		// Create a fake thread object to handle hashmeter etc
		threadobj = 1;
	cgpu->thr = calloc(threadobj + 1, sizeof(*cgpu->thr));
	cgpu->thr[threadobj] = NULL;
	cgpu->status = LIFE_INIT;
	
	if (opt_devices_enabled_list)
	{
		struct string_elist *enablestr_elist;
		cgpu->deven = DEV_DISABLED;
		DL_FOREACH(opt_devices_enabled_list, enablestr_elist)
		{
			const char * const enablestr = enablestr_elist->string;
			if (cgpu_match(enablestr, cgpu))
			{
				cgpu->deven = DEV_ENABLED;
				break;
			}
		}
	}

	cgpu->max_hashes = 0;
	
	BFGINIT(cgpu->cutofftemp, opt_cutofftemp);
	BFGINIT(cgpu->targettemp, cgpu->cutofftemp - 6);

	// Setup thread structs before starting any of the threads, in case they try to interact
	for (j = 0; j < threadobj; ++j, ++*kp) {
		thr = get_thread(*kp);
		thr->id = *kp;
		thr->cgpu = cgpu;
		thr->device_thread = j;
		thr->work_restart_notifier[1] = INVSOCK;
		thr->mutex_request[1] = INVSOCK;
		thr->_job_transition_in_progress = true;
		timerclear(&thr->tv_morework);

		thr->scanhash_working = true;
		thr->hashes_done = 0;
		timerclear(&thr->tv_hashes_done);
		cgtime(&thr->tv_lastupdate);
		thr->tv_poll.tv_sec = -1;
		thr->_max_nonce = api->can_limit_work ? api->can_limit_work(thr) : 0xffffffff;

		cgpu->thr[j] = thr;
	}
	
	if (!cgpu->device->threads)
		notifier_init_invalid(cgpu->thr[0]->notifier);
	else
	if (!cgpu->threads)
		memcpy(&cgpu->thr[0]->notifier, &cgpu->device->thr[0]->notifier, sizeof(cgpu->thr[0]->notifier));
	else
	for (j = 0; j < cgpu->threads; ++j)
	{
		thr = cgpu->thr[j];
		notifier_init(thr->notifier);
	}
}

static
void start_cgpu(struct cgpu_info *cgpu)
{
	struct thr_info *thr;
	int j;
	
	for (j = 0; j < cgpu->threads; ++j) {
		thr = cgpu->thr[j];

		/* Enable threads for devices set not to mine but disable
		 * their queue in case we wish to enable them later */
		if (cgpu->drv->thread_prepare && !cgpu->drv->thread_prepare(thr))
			continue;

		thread_reportout(thr);

		if (unlikely(thr_info_create(thr, NULL, miner_thread, thr)))
			quit(1, "thread %d create failed", thr->id);
		
		notifier_wake(thr->notifier);
	}
	if (cgpu->deven == DEV_ENABLED)
		proc_enable(cgpu);
}

static
void _scan_serial(void *p)
{
	const char *s = p;
	struct string_elist *iter, *tmp;
	struct string_elist *orig_scan_devices = scan_devices;
	
	if (s)
	{
		// Make temporary scan_devices list
		scan_devices = NULL;
		string_elist_add("noauto", &scan_devices);
		add_serial(s);
	}
	
	drv_detect_all();
	
	if (s)
	{
		DL_FOREACH_SAFE(scan_devices, iter, tmp)
		{
			string_elist_del(&scan_devices, iter);
		}
		scan_devices = orig_scan_devices;
	}
}

#ifdef HAVE_BFG_LOWLEVEL
static
bool _probe_device_match(const struct lowlevel_device_info * const info, const char * const ser)
{
	if (!(false
		|| (info->serial && !strcasecmp(ser, info->serial))
		|| (info->path   && !strcasecmp(ser, info->path  ))
		|| (info->devid  && !strcasecmp(ser, info->devid ))
	))
	{
		char *devid = devpath_to_devid(ser);
		if (!devid)
			return false;
		const bool different = strcmp(info->devid, devid);
		free(devid);
		if (different)
			return false;
	}
	return true;
}

static
bool _probe_device_do_probe(const struct device_drv * const drv, const struct lowlevel_device_info * const info, bool * const request_rescan_p)
{
	bfg_probe_result_flags = 0;
	if (drv->lowl_probe(info))
	{
		if (!(bfg_probe_result_flags & BPR_CONTINUE_PROBES))
			return true;
	}
	else
	if (request_rescan_p && opt_hotplug && !(bfg_probe_result_flags & BPR_DONT_RESCAN))
		*request_rescan_p = true;
	return false;
}

bool dummy_check_never_true = false;

static
void *probe_device_thread(void *p)
{
	struct lowlevel_device_info * const infolist = p;
	struct lowlevel_device_info *info = infolist;
	bool request_rescan = false;
	
	{
		char threadname[6 + strlen(info->devid) + 1];
		sprintf(threadname, "probe_%s", info->devid);
		RenameThread(threadname);
	}
	
	// If already in use, ignore
	if (bfg_claim_any(NULL, NULL, info->devid))
		applogr(NULL, LOG_DEBUG, "%s: \"%s\" already in use",
		        __func__, info->product);
	
	// if lowlevel device matches specific user assignment, probe requested driver(s)
	struct string_elist *sd_iter, *sd_tmp;
	struct driver_registration *dreg;
	DL_FOREACH_SAFE(scan_devices, sd_iter, sd_tmp)
	{
		const char * const dname = sd_iter->string;
		const char * const colon = strpbrk(dname, ":@");
		if (!(colon && colon != dname))
			continue;
		const char * const ser = &colon[1];
		LL_FOREACH2(infolist, info, same_devid_next)
		{
			if (!_probe_device_match(info, ser))
				continue;
			
			const size_t dnamelen = (colon - dname);
			char dname_nt[dnamelen + 1];
			memcpy(dname_nt, dname, dnamelen);
			dname_nt[dnamelen] = '\0';
			
			BFG_FOREACH_DRIVER_BY_PRIORITY(dreg) {
				const struct device_drv * const drv = dreg->drv;
				if (!(drv && drv->lowl_probe && drv_algo_check(drv)))
					continue;
				if (strcasecmp(drv->dname, dname_nt) && strcasecmp(drv->name, dname_nt))
					continue;
				if (_probe_device_do_probe(drv, info, &request_rescan))
					return NULL;
			}
		}
	}
	
	// probe driver(s) with auto enabled and matching VID/PID/Product/etc of device
	BFG_FOREACH_DRIVER_BY_PRIORITY(dreg)
	{
		const struct device_drv * const drv = dreg->drv;
		
		if (!drv_algo_check(drv))
			continue;
		
		// Check for "noauto" flag
		// NOTE: driver-specific configuration overrides general
		bool doauto = true;
		DL_FOREACH_SAFE(scan_devices, sd_iter, sd_tmp)
		{
			const char * const dname = sd_iter->string;
			// NOTE: Only checking flags here, NOT path/serial, so @ is unacceptable
			const char *colon = strchr(dname, ':');
			if (!colon)
				colon = &dname[-1];
			if (strcasecmp("noauto", &colon[1]) && strcasecmp("auto", &colon[1]))
				continue;
			const ssize_t dnamelen = (colon - dname);
			if (dnamelen >= 0) {
				char dname_nt[dnamelen + 1];
				memcpy(dname_nt, dname, dnamelen);
				dname_nt[dnamelen] = '\0';
				
				if (strcasecmp(drv->dname, dname_nt) && strcasecmp(drv->name, dname_nt))
					continue;
			}
			doauto = (tolower(colon[1]) == 'a');
			if (dnamelen != -1)
				break;
		}
		
		if (doauto && drv->lowl_match)
		{
			LL_FOREACH2(infolist, info, same_devid_next)
			{
				/*
				 The below call to applog is absolutely necessary
				 Starting with commit 76d0cc183b1c9ddcc0ef34d2e43bc696ef9de92e installing BFGMiner on
				 Mac OS X using Homebrew results in a binary that segfaults on startup
				 There are two unresolved issues:

				 1) The BFGMiner authors cannot find a way to install BFGMiner with Homebrew that results
				    in debug symbols being available to help troubleshoot the issue
				 2) The issue disappears when unrelated code changes are made, such as adding the following
				    call to applog with infolist and / or p
				 
				 We would encourage revisiting this in the future to come up with a more concrete solution
				 Reproducing should only require commenting / removing the following line and installing
				 BFGMiner using "brew install bfgminer --HEAD"
				 */
				if (dummy_check_never_true)
					applog(LOG_DEBUG, "lowl_match: %p(%s) %p %p %p", drv, drv->dname, info, infolist, p);
				
				if (!drv->lowl_match(info))
					continue;
				if (_probe_device_do_probe(drv, info, &request_rescan))
					return NULL;
			}
		}
	}
	
	// probe driver(s) with 'all' enabled
	DL_FOREACH_SAFE(scan_devices, sd_iter, sd_tmp)
	{
		const char * const dname = sd_iter->string;
		// NOTE: Only checking flags here, NOT path/serial, so @ is unacceptable
		const char * const colon = strchr(dname, ':');
		if (!colon)
		{
			LL_FOREACH2(infolist, info, same_devid_next)
			{
				if (
#ifdef NEED_BFG_LOWL_VCOM
					(info->lowl == &lowl_vcom && !strcasecmp(dname, "all")) ||
#endif
					_probe_device_match(info, (dname[0] == '@') ? &dname[1] : dname))
				{
					bool dont_rescan = false;
					BFG_FOREACH_DRIVER_BY_PRIORITY(dreg)
					{
						const struct device_drv * const drv = dreg->drv;
						if (!drv_algo_check(drv))
							continue;
						if (drv->lowl_probe_by_name_only)
							continue;
						if (!drv->lowl_probe)
							continue;
						if (_probe_device_do_probe(drv, info, NULL))
							return NULL;
						if (bfg_probe_result_flags & BPR_DONT_RESCAN)
							dont_rescan = true;
					}
					if (opt_hotplug && !dont_rescan)
						request_rescan = true;
					break;
				}
			}
			continue;
		}
		if (strcasecmp(&colon[1], "all"))
			continue;
		const size_t dnamelen = (colon - dname);
		char dname_nt[dnamelen + 1];
		memcpy(dname_nt, dname, dnamelen);
		dname_nt[dnamelen] = '\0';

		BFG_FOREACH_DRIVER_BY_PRIORITY(dreg) {
			const struct device_drv * const drv = dreg->drv;
			if (!(drv && drv->lowl_probe && drv_algo_check(drv)))
				continue;
			if (strcasecmp(drv->dname, dname_nt) && strcasecmp(drv->name, dname_nt))
				continue;
			LL_FOREACH2(infolist, info, same_devid_next)
			{
				if (info->lowl->exclude_from_all)
					continue;
				if (_probe_device_do_probe(drv, info, NULL))
					return NULL;
			}
		}
	}
	
	// Only actually request a rescan if we never found any cgpu
	if (request_rescan)
		bfg_need_detect_rescan = true;
	
	return NULL;
}

void probe_device(struct lowlevel_device_info * const info)
{
	pthread_create(&info->probe_pth, NULL, probe_device_thread, info);
}
#endif

int create_new_cgpus(void (*addfunc)(void*), void *arg)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	int devcount, i, mining_threads_new = 0;
	unsigned int k;
	struct cgpu_info *cgpu;
	struct thr_info *thr;
	void *p;
	
	mutex_lock(&mutex);
	devcount = total_devices;
	
	addfunc(arg);
	
	if (!total_devices_new)
		goto out;
	
	wr_lock(&devices_lock);
	p = realloc(devices, sizeof(struct cgpu_info *) * (total_devices + total_devices_new + 1));
	if (unlikely(!p))
	{
		wr_unlock(&devices_lock);
		applog(LOG_ERR, "scan_serial: realloc failed trying to grow devices array");
		goto out;
	}
	devices = p;
	wr_unlock(&devices_lock);
	
	for (i = 0; i < total_devices_new; ++i)
	{
		cgpu = devices_new[i];
		mining_threads_new += cgpu->threads ?: 1;
	}
	
	wr_lock(&mining_thr_lock);
	mining_threads_new += mining_threads;
	p = realloc(mining_thr, sizeof(struct thr_info *) * mining_threads_new);
	if (unlikely(!p))
	{
		wr_unlock(&mining_thr_lock);
		applog(LOG_ERR, "scan_serial: realloc failed trying to grow mining_thr");
		goto out;
	}
	mining_thr = p;
	wr_unlock(&mining_thr_lock);
	for (i = mining_threads; i < mining_threads_new; ++i) {
		mining_thr[i] = calloc(1, sizeof(*thr));
		if (!mining_thr[i])
		{
			applog(LOG_ERR, "scan_serial: Failed to calloc mining_thr[%d]", i);
			for ( ; --i >= mining_threads; )
				free(mining_thr[i]);
			goto out;
		}
	}
	
	k = mining_threads;
	for (i = 0; i < total_devices_new; ++i)
	{
		cgpu = devices_new[i];
		
		allocate_cgpu(cgpu, &k);
	}
	for (i = 0; i < total_devices_new; ++i)
	{
		cgpu = devices_new[i];
		
		start_cgpu(cgpu);
		register_device(cgpu);
		++total_devices;
	}
	
#ifdef HAVE_CURSES
	switch_logsize();
#endif
	
out:
	total_devices_new = 0;
	
	devcount = total_devices - devcount;
	mutex_unlock(&mutex);
	
	return devcount;
}

int scan_serial(const char *s)
{
	return create_new_cgpus(_scan_serial, (void*)s);
}

static pthread_mutex_t rescan_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool rescan_active;
static struct timeval tv_rescan;
static notifier_t rescan_notifier;

static
void *rescan_thread(__maybe_unused void *p)
{
	pthread_detach(pthread_self());
	RenameThread("rescan");
	
	struct timeval tv_timeout, tv_now;
	fd_set rfds;
	
	while (true)
	{
		mutex_lock(&rescan_mutex);
		tv_timeout = tv_rescan;
		if (!timer_isset(&tv_timeout))
		{
			rescan_active = false;
			mutex_unlock(&rescan_mutex);
			break;
		}
		mutex_unlock(&rescan_mutex);
		
		FD_ZERO(&rfds);
		FD_SET(rescan_notifier[0], &rfds);
		const int maxfd = rescan_notifier[0];
		
		timer_set_now(&tv_now);
		if (select(maxfd+1, &rfds, NULL, NULL, select_timeout(&tv_timeout, &tv_now)) > 0)
			notifier_read(rescan_notifier);
		
		mutex_lock(&rescan_mutex);
		if (timer_passed(&tv_rescan, NULL))
		{
			timer_unset(&tv_rescan);
			mutex_unlock(&rescan_mutex);
			applog(LOG_DEBUG, "Rescan timer expired, triggering");
			scan_serial(NULL);
		}
		else
			mutex_unlock(&rescan_mutex);
	}
	return NULL;
}

static
void _schedule_rescan(const struct timeval * const tvp_when)
{
	if (rescan_active)
	{
		if (timercmp(tvp_when, &tv_rescan, <))
			applog(LOG_DEBUG, "schedule_rescan: New schedule is before current, waiting it out");
		else
		{
			applog(LOG_DEBUG, "schedule_rescan: New schedule is after current, delaying rescan");
			tv_rescan = *tvp_when;
		}
		return;
	}
	
	applog(LOG_DEBUG, "schedule_rescan: Scheduling rescan (no rescans currently pending)");
	tv_rescan = *tvp_when;
	rescan_active = true;
	
	static pthread_t pth;
	if (unlikely(pthread_create(&pth, NULL, rescan_thread, NULL)))
		applog(LOG_ERR, "Failed to start rescan thread");
}

static
void schedule_rescan(const struct timeval * const tvp_when)
{
	mutex_lock(&rescan_mutex);
	_schedule_rescan(tvp_when);
	mutex_unlock(&rescan_mutex);
}

static
void hotplug_trigger()
{
	applog(LOG_DEBUG, "%s: Scheduling rescan immediately", __func__);
	struct timeval tv_now;
	timer_set_now(&tv_now);
	schedule_rescan(&tv_now);
}

#if defined(HAVE_LIBUDEV) && defined(HAVE_SYS_EPOLL_H)

static
void *hotplug_thread(__maybe_unused void *p)
{
	pthread_detach(pthread_self());
	RenameThread("hotplug");
	
	struct udev * const udev = udev_new();
	if (unlikely(!udev))
		applogfailr(NULL, LOG_ERR, "udev_new");
	struct udev_monitor * const mon = udev_monitor_new_from_netlink(udev, "udev");
	if (unlikely(!mon))
		applogfailr(NULL, LOG_ERR, "udev_monitor_new_from_netlink");
	if (unlikely(udev_monitor_enable_receiving(mon)))
		applogfailr(NULL, LOG_ERR, "udev_monitor_enable_receiving");
	const int epfd = epoll_create(1);
	if (unlikely(epfd == -1))
		applogfailr(NULL, LOG_ERR, "epoll_create");
	{
		const int fd = udev_monitor_get_fd(mon);
		struct epoll_event ev = {
			.events = EPOLLIN | EPOLLPRI,
			.data.fd = fd,
		};
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev))
			applogfailr(NULL, LOG_ERR, "epoll_ctl");
	}
	
	struct epoll_event ev;
	int rv;
	bool pending = false;
	while (true)
	{
		rv = epoll_wait(epfd, &ev, 1, pending ? hotplug_delay_ms : -1);
		if (rv == -1)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}
		if (!rv)
		{
			hotplug_trigger();
			pending = false;
			continue;
		}
		struct udev_device * const device = udev_monitor_receive_device(mon);
		if (!device)
			continue;
		const char * const action = udev_device_get_action(device);
		applog(LOG_DEBUG, "%s: Received %s event", __func__, action);
		if (!strcmp(action, "add"))
			pending = true;
		udev_device_unref(device);
	}
	
	applogfailr(NULL, LOG_ERR, "epoll_wait");
}

#elif defined(WIN32)

static UINT_PTR _hotplug_wintimer_id;

VOID CALLBACK hotplug_win_timer(HWND hwnd, UINT msg, UINT_PTR idEvent, DWORD dwTime)
{
	KillTimer(NULL, _hotplug_wintimer_id);
	_hotplug_wintimer_id = 0;
	hotplug_trigger();
}

LRESULT CALLBACK hotplug_win_callback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_DEVICECHANGE && wParam == DBT_DEVNODES_CHANGED)
	{
		applog(LOG_DEBUG, "%s: Received DBT_DEVNODES_CHANGED event", __func__);
		_hotplug_wintimer_id = SetTimer(NULL, _hotplug_wintimer_id, hotplug_delay_ms, hotplug_win_timer);
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static
void *hotplug_thread(__maybe_unused void *p)
{
	pthread_detach(pthread_self());
	
	WNDCLASS DummyWinCls = {
		.lpszClassName = "BFGDummyWinCls",
		.lpfnWndProc = hotplug_win_callback,
	};
	ATOM a = RegisterClass(&DummyWinCls);
	if (unlikely(!a))
		applogfailinfor(NULL, LOG_ERR, "RegisterClass", "%d", (int)GetLastError());
	HWND hwnd = CreateWindow((void*)(intptr_t)a, NULL, WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, NULL, NULL);
	if (unlikely(!hwnd))
		applogfailinfor(NULL, LOG_ERR, "CreateWindow", "%d", (int)GetLastError());
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	quit(0, "WM_QUIT received");
	return NULL;
}

#endif

#ifdef HAVE_BFG_HOTPLUG
static
void hotplug_start()
{
	pthread_t pth;
	if (unlikely(pthread_create(&pth, NULL, hotplug_thread, NULL)))
		applog(LOG_ERR, "Failed to start hotplug thread");
}
#endif

static void probe_pools(void)
{
	int i;

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		pool->testing = true;
		pthread_create(&pool->test_thread, NULL, test_pool_thread, (void *)pool);
	}
}

static void raise_fd_limits(void)
{
#ifdef HAVE_SETRLIMIT
	struct rlimit fdlimit;
	rlim_t old_soft_limit;
	char frombuf[MAX_STR_UINT(unsigned long)] = "unlimited";
	char hardbuf[MAX_STR_UINT(unsigned long)] = "unlimited";
	
	if (getrlimit(RLIMIT_NOFILE, &fdlimit))
		applogr(, LOG_DEBUG, "setrlimit: Failed to getrlimit(RLIMIT_NOFILE)");
	
	old_soft_limit = fdlimit.rlim_cur;
	
	if (fdlimit.rlim_max > FD_SETSIZE || fdlimit.rlim_max == RLIM_INFINITY)
		fdlimit.rlim_cur = FD_SETSIZE;
	else
		fdlimit.rlim_cur = fdlimit.rlim_max;
	
	if (fdlimit.rlim_max != RLIM_INFINITY)
		snprintf(hardbuf, sizeof(hardbuf), "%lu", (unsigned long)fdlimit.rlim_max);
	if (old_soft_limit != RLIM_INFINITY)
		snprintf(frombuf, sizeof(frombuf), "%lu", (unsigned long)old_soft_limit);
	
	if (fdlimit.rlim_cur == old_soft_limit)
		applogr(, LOG_DEBUG, "setrlimit: Soft fd limit not being changed from %lu (FD_SETSIZE=%lu; hard limit=%s)",
		        (unsigned long)old_soft_limit, (unsigned long)FD_SETSIZE, hardbuf);
	
	if (setrlimit(RLIMIT_NOFILE, &fdlimit))
		applogr(, LOG_DEBUG, "setrlimit: Failed to change soft fd limit from %s to %lu (FD_SETSIZE=%lu; hard limit=%s)",
		        frombuf, (unsigned long)fdlimit.rlim_cur, (unsigned long)FD_SETSIZE, hardbuf);
	
	applog(LOG_DEBUG, "setrlimit: Changed soft fd limit from %s to %lu (FD_SETSIZE=%lu; hard limit=%s)",
	       frombuf, (unsigned long)fdlimit.rlim_cur, (unsigned long)FD_SETSIZE, hardbuf);
#else
	applog(LOG_DEBUG, "setrlimit: Not supported by platform");
#endif
}

static
void bfg_atexit(void)
{
	puts("");
}

extern void bfg_init_threadlocal();
extern bool stratumsrv_change_port(unsigned);
extern void test_aan_pll(void);

int main(int argc, char *argv[])
{
	struct sigaction handler;
	struct thr_info *thr;
	unsigned int k;
	int i;
	int rearrange_pools = 0;
	char *s;

#ifdef WIN32
	LoadLibrary("backtrace.dll");
#endif
	
	atexit(bfg_atexit);

	b58_sha256_impl = my_blkmaker_sha256_callback;
	blkmk_sha256_impl = my_blkmaker_sha256_callback;

	bfg_init_threadlocal();
#ifndef HAVE_PTHREAD_CANCEL
	setup_pthread_cancel_workaround();
#endif
	bfg_init_checksums();

#ifdef WIN32
	{
		WSADATA wsa;
		i = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (i)
			quit(1, "Failed to initialise Winsock: %s", bfg_strerror(i, BST_SOCKET));
	}
#endif
	
	/* This dangerous functions tramples random dynamically allocated
	 * variables so do it before anything at all */
	if (unlikely(curl_global_init(CURL_GLOBAL_ALL)))
		quit(1, "Failed to curl_global_init");

	initial_args = malloc(sizeof(char *) * (argc + 1));
	for  (i = 0; i < argc; i++)
		initial_args[i] = strdup(argv[i]);
	initial_args[argc] = NULL;

	mutex_init(&hash_lock);
	mutex_init(&console_lock);
	cglock_init(&control_lock);
	mutex_init(&stats_lock);
	mutex_init(&sharelog_lock);
	cglock_init(&ch_lock);
	mutex_init(&sshare_lock);
	rwlock_init(&blk_lock);
	rwlock_init(&netacc_lock);
	rwlock_init(&mining_thr_lock);
	rwlock_init(&devices_lock);

	mutex_init(&lp_lock);
	if (unlikely(pthread_cond_init(&lp_cond, bfg_condattr)))
		quit(1, "Failed to pthread_cond_init lp_cond");

	if (unlikely(pthread_cond_init(&gws_cond, bfg_condattr)))
		quit(1, "Failed to pthread_cond_init gws_cond");

	notifier_init(submit_waiting_notifier);
	timer_unset(&tv_rescan);
	notifier_init(rescan_notifier);

	/* Create a unique get work queue */
	getq = tq_new();
	if (!getq)
		quit(1, "Failed to create getq");
	/* We use the getq mutex as the staged lock */
	stgd_lock = &getq->mutex;

#if defined(USE_CPUMINING) && defined(USE_SHA256D)
	init_max_name_len();
#endif

	handler.sa_handler = &sighandler;
	handler.sa_flags = 0;
	sigemptyset(&handler.sa_mask);
#ifdef HAVE_PTHREAD_CANCEL
	sigaction(SIGTERM, &handler, &termhandler);
#else
	// Need to let pthread_cancel emulation handle SIGTERM first
	termhandler = pcwm_orig_term_handler;
	pcwm_orig_term_handler = handler;
#endif
	sigaction(SIGINT, &handler, &inthandler);
#ifndef WIN32
	signal(SIGPIPE, SIG_IGN);
#else
	timeBeginPeriod(1);
#endif
	opt_kernel_path = CGMINER_PREFIX;
	cgminer_path = alloca(PATH_MAX);
	s = strdup(argv[0]);
	strcpy(cgminer_path, dirname(s));
	free(s);
	strcat(cgminer_path, "/");
#if defined(USE_CPUMINING) && defined(WIN32)
	{
		char buf[32];
		int gev = GetEnvironmentVariable("BFGMINER_BENCH_ALGO", buf, sizeof(buf));
		if (gev > 0 && gev < sizeof(buf))
		{
			setup_benchmark_pool();
			double rate = bench_algo_stage3(atoi(buf));
			
			// Write result to shared memory for parent
			char unique_name[64];
			
			if (GetEnvironmentVariable("BFGMINER_SHARED_MEM", unique_name, 32))
			{
				HANDLE map_handle = CreateFileMapping(
					INVALID_HANDLE_VALUE,   // use paging file
					NULL,                   // default security attributes
					PAGE_READWRITE,         // read/write access
					0,                      // size: high 32-bits
					4096,                   // size: low 32-bits
					unique_name             // name of map object
				);
				if (NULL != map_handle) {
					void *shared_mem = MapViewOfFile(
						map_handle,     // object to map view of
						FILE_MAP_WRITE, // read/write access
						0,              // high offset:  map from
						0,              // low offset:   beginning
						0               // default: map entire file
					);
					if (NULL != shared_mem)
						CopyMemory(shared_mem, &rate, sizeof(rate));
					(void)UnmapViewOfFile(shared_mem);
				}
				(void)CloseHandle(map_handle);
			}
			exit(0);
		}
	}
#endif

#ifdef HAVE_CURSES
	devcursor = 8;
	logstart = devcursor;
	logcursor = logstart;
#endif

	mutex_init(&submitting_lock);

	// Ensure at least the default goal is created
	get_mining_goal("default");
#ifdef USE_OPENCL
	opencl_early_init();
#endif

	schedstart.tm.tm_sec = 1;
	schedstop .tm.tm_sec = 1;

	opt_register_table(opt_early_table, NULL);
	opt_register_table(opt_config_table, NULL);
	opt_register_table(opt_cmdline_table, NULL);
	opt_early_parse(argc, argv, applog_and_exit);
	
	if (!config_loaded)
	{
		load_default_config();
		rearrange_pools = total_pools;
	}
	
	opt_free_table();
	
	/* parse command line */
	opt_register_table(opt_config_table,
			   "Options for both config file and command line");
	opt_register_table(opt_cmdline_table,
			   "Options for command line only");

	opt_parse(&argc, argv, applog_and_exit);
	if (argc != 1)
		quit(1, "Unexpected extra commandline arguments");
	
	if (rearrange_pools && rearrange_pools < total_pools)
	{
		// Prioritise commandline pools before default-config pools
		for (i = 0; i < rearrange_pools; ++i)
			pools[i]->prio += rearrange_pools;
		for ( ; i < total_pools; ++i)
			pools[i]->prio -= rearrange_pools;
	}

#ifndef HAVE_PTHREAD_CANCEL
	// Can't do this any earlier, or config isn't loaded
	applog(LOG_DEBUG, "pthread_cancel workaround in use");
#endif

#ifdef HAVE_PWD_H
	struct passwd *user_info = NULL;
	if (opt_setuid != NULL) {
		if ((user_info = getpwnam(opt_setuid)) == NULL) {
			quit(1, "Unable to find setuid user information");
		}
	}
#endif

#ifdef HAVE_CHROOT
        if (chroot_dir != NULL) {
#ifdef HAVE_PWD_H
                if (user_info == NULL && getuid() == 0) {
                        applog(LOG_WARNING, "Running as root inside chroot");
                }
#endif
                if (chroot(chroot_dir) != 0) {
                       quit(1, "Unable to chroot");
                }
		if (chdir("/"))
			quit(1, "Unable to chdir to chroot");
        }
#endif

#ifdef HAVE_PWD_H
		if (user_info != NULL) {
			if (setgid((*user_info).pw_gid) != 0)
				quit(1, "Unable to setgid");
			if (setuid((*user_info).pw_uid) != 0)
				quit(1, "Unable to setuid");
		}
#endif
	raise_fd_limits();
	
	if (opt_benchmark) {
		while (total_pools)
			remove_pool(pools[0]);

		setup_benchmark_pool();
	}
	
	if (opt_unittest) {
		test_cgpu_match();
		test_intrange();
		test_decimal_width();
		test_domain_funcs();
#ifdef USE_SCRYPT
		test_scrypt();
#endif
		test_target();
		test_uri_get_param();
		utf8_test();
#ifdef USE_JINGTIAN
		test_aan_pll();
#endif
		if (unittest_failures)
			quit(1, "Unit tests failed");
	}

#ifdef HAVE_CURSES
	if (opt_realquiet || opt_display_devs)
		use_curses = false;

	setlocale(LC_ALL, "C");
	if (use_curses)
		enable_curses();
#endif

#ifdef HAVE_LIBUSB
	int err = libusb_init(NULL);
	if (err)
		applog(LOG_WARNING, "libusb_init() failed err %d", err);
	else
		have_libusb = true;
#endif

	applog(LOG_WARNING, "Started %s", packagename);
	{
		struct bfg_loaded_configfile *configfile;
		LL_FOREACH(bfg_loaded_configfiles, configfile)
		{
			char * const cnfbuf = configfile->filename;
			int fileconf_load = configfile->fileconf_load;
			applog(LOG_NOTICE, "Loaded configuration file %s", cnfbuf);
			switch (fileconf_load) {
				case 0:
					applog(LOG_WARNING, "Fatal JSON error in configuration file.");
					applog(LOG_WARNING, "Configuration file could not be used.");
					break;
				case -1:
					applog(LOG_WARNING, "Error in configuration file, partially loaded.");
					if (use_curses)
						applog(LOG_WARNING, "Start BFGMiner with -T to see what failed to load.");
					break;
				default:
					break;
			}
		}
	}

	i = strlen(opt_kernel_path) + 2;
	char __kernel_path[i];
	snprintf(__kernel_path, i, "%s/", opt_kernel_path);
	opt_kernel_path = __kernel_path;

	if (want_per_device_stats)
		opt_log_output = true;

	bfg_devapi_init();
	drv_detect_all();
	total_devices = total_devices_new;
	devices = devices_new;
	total_devices_new = 0;
	devices_new = NULL;

	if (opt_display_devs) {
		int devcount = 0;
		applog(LOG_ERR, "Devices detected:");
		for (i = 0; i < total_devices; ++i) {
			struct cgpu_info *cgpu = devices[i];
			char buf[0x100];
			if (cgpu->device != cgpu)
				continue;
			if (cgpu->name)
				snprintf(buf, sizeof(buf), " %s", cgpu->name);
			else
			if (cgpu->dev_manufacturer)
				snprintf(buf, sizeof(buf), " %s by %s", (cgpu->dev_product ?: "Device"), cgpu->dev_manufacturer);
			else
			if (cgpu->dev_product)
				snprintf(buf, sizeof(buf), " %s", cgpu->dev_product);
			else
				strcpy(buf, " Device");
			tailsprintf(buf, sizeof(buf), " (driver=%s; procs=%d", cgpu->drv->dname, cgpu->procs);
			if (cgpu->dev_serial)
				tailsprintf(buf, sizeof(buf), "; serial=%s", cgpu->dev_serial);
			if (cgpu->device_path)
				tailsprintf(buf, sizeof(buf), "; path=%s", cgpu->device_path);
			tailsprintf(buf, sizeof(buf), ")");
			_applog(LOG_NOTICE, buf);
			++devcount;
		}
		quit(0, "%d devices listed", devcount);
	}

	mining_threads = 0;
	for (i = 0; i < total_devices; ++i)
		register_device(devices[i]);

	if (!total_devices) {
		applog(LOG_WARNING, "No devices detected!");
		if (use_curses)
			applog(LOG_WARNING, "Waiting for devices; press 'M+' to add, or 'Q' to quit");
		else
			applog(LOG_WARNING, "Waiting for devices");
	}
	
#ifdef HAVE_CURSES
	switch_logsize();
#endif

#if BLKMAKER_VERSION > 1 && defined(USE_SHA256D)
	if (opt_load_bitcoin_conf && !(get_mining_goal("default")->malgo->algo != POW_SHA256D || opt_benchmark))
		add_local_gbt(total_pools);
#endif
	
	if (!total_pools) {
		applog(LOG_WARNING, "Need to specify at least one pool server.");
#ifdef HAVE_CURSES
		if (!use_curses || !input_pool(false))
#endif
			quit(1, "Pool setup failed");
	}

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];
		size_t siz;

		if (!pool->rpc_url)
			quit(1, "No URI supplied for pool %u", i);
		
		if (!pool->rpc_userpass) {
			if (!pool->rpc_user || !pool->rpc_pass)
				quit(1, "No login credentials supplied for pool %u %s", i, pool->rpc_url);
			siz = strlen(pool->rpc_user) + strlen(pool->rpc_pass) + 2;
			pool->rpc_userpass = malloc(siz);
			if (!pool->rpc_userpass)
				quit(1, "Failed to malloc userpass");
			snprintf(pool->rpc_userpass, siz, "%s:%s", pool->rpc_user, pool->rpc_pass);
		}
	}
	/* Set the currentpool to pool with priority 0 */
	validate_pool_priorities();
	for (i = 0; i < total_pools; i++) {
		struct pool *pool  = pools[i];

		if (!pool->prio)
			currentpool = pool;
	}

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog(PACKAGE, LOG_PID, LOG_USER);
#endif

	#if defined(unix) || defined(__APPLE__)
		if (opt_stderr_cmd)
			fork_monitor();
	#endif // defined(unix)

	mining_thr = calloc(mining_threads, sizeof(thr));
	if (!mining_thr)
		quit(1, "Failed to calloc mining_thr");
	for (i = 0; i < mining_threads; i++) {
		mining_thr[i] = calloc(1, sizeof(*thr));
		if (!mining_thr[i])
			quit(1, "Failed to calloc mining_thr[%d]", i);
	}

	total_control_threads = 6;
	control_thr = calloc(total_control_threads, sizeof(*thr));
	if (!control_thr)
		quit(1, "Failed to calloc control_thr");

	if (opt_benchmark)
		goto begin_bench;

	applog(LOG_NOTICE, "Probing for an alive pool");
	do {
		bool still_testing;
		int i;

		/* Look for at least one active pool before starting */
		probe_pools();
		do {
			sleep(1);
			if (pools_active)
				break;
			still_testing = false;
			for (int i = 0; i < total_pools; ++i)
				if (pools[i]->testing)
					still_testing = true;
		} while (still_testing);

		if (!pools_active) {
			applog(LOG_ERR, "No servers were found that could be used to get work from.");
			applog(LOG_ERR, "Please check the details from the list below of the servers you have input");
			applog(LOG_ERR, "Most likely you have input the wrong URL, forgotten to add a port, or have not set up workers");
			for (i = 0; i < total_pools; i++) {
				struct pool *pool;

				pool = pools[i];
				applog(LOG_WARNING, "Pool: %d  URL: %s  User: %s  Password: %s",
				       i, pool->rpc_url, pool->rpc_user, pool->rpc_pass);
			}
#ifdef HAVE_CURSES
			if (use_curses) {
				halfdelay(150);
				applog(LOG_ERR, "Press any key to exit, or BFGMiner will try again in 15s.");
				if (getch() != ERR)
					quit(0, "No servers could be used! Exiting.");
				cbreak();
			} else
#endif
				quit(0, "No servers could be used! Exiting.");
		}
	} while (!pools_active);

#ifdef USE_SCRYPT
	if (detect_algo == 1 && get_mining_goal("default")->malgo->algo != POW_SCRYPT) {
		applog(LOG_NOTICE, "Detected scrypt algorithm");
		set_malgo_scrypt();
	}
#endif
	detect_algo = 0;

begin_bench:
	total_mhashes_done = 0;
	for (i = 0; i < total_devices; i++) {
		struct cgpu_info *cgpu = devices[i];

		cgpu->rolling = cgpu->total_mhashes = 0;
	}
	
	cgtime(&total_tv_start);
	cgtime(&total_tv_end);
	miner_started = total_tv_start;
	time_t miner_start_ts = time(NULL);
	if (schedstart.tm.tm_sec)
		localtime_r(&miner_start_ts, &schedstart.tm);
	if (schedstop.tm.tm_sec)
		localtime_r(&miner_start_ts, &schedstop .tm);
	get_datestamp(datestamp, sizeof(datestamp), miner_start_ts);

	// Initialise processors and threads
	k = 0;
	for (i = 0; i < total_devices; ++i) {
		struct cgpu_info *cgpu = devices[i];
		allocate_cgpu(cgpu, &k);
	}

	// Start threads
	for (i = 0; i < total_devices; ++i) {
		struct cgpu_info *cgpu = devices[i];
		start_cgpu(cgpu);
	}

#ifdef USE_OPENCL
	for (i = 0; i < nDevs; i++)
		pause_dynamic_threads(i);
#endif

#if defined(USE_CPUMINING) && defined(USE_SHA256D)
	if (opt_n_threads > 0)
		applog(LOG_INFO, "%d cpu miner threads started, using '%s' algorithm.",
		       opt_n_threads, algo_names[opt_algo]);
#endif

	cgtime(&total_tv_start);
	cgtime(&total_tv_end);

	if (!opt_benchmark)
	{
		pthread_t submit_thread;
		if (unlikely(pthread_create(&submit_thread, NULL, submit_work_thread, NULL)))
			quit(1, "submit_work thread create failed");
	}

	watchpool_thr_id = 1;
	thr = &control_thr[watchpool_thr_id];
	/* start watchpool thread */
	if (thr_info_create(thr, NULL, watchpool_thread, NULL))
		quit(1, "watchpool thread create failed");
	pthread_detach(thr->pth);

	watchdog_thr_id = 2;
	thr = &control_thr[watchdog_thr_id];
	/* start watchdog thread */
	if (thr_info_create(thr, NULL, watchdog_thread, NULL))
		quit(1, "watchdog thread create failed");
	pthread_detach(thr->pth);

#ifdef USE_OPENCL
	/* Create reinit gpu thread */
	gpur_thr_id = 3;
	thr = &control_thr[gpur_thr_id];
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "tq_new failed for gpur_thr_id");
	if (thr_info_create(thr, NULL, reinit_gpu, thr))
		quit(1, "reinit_gpu thread create failed");
#endif	

	/* Create API socket thread */
	api_thr_id = 4;
	thr = &control_thr[api_thr_id];
	if (thr_info_create(thr, NULL, api_thread, thr))
		quit(1, "API thread create failed");
	
#ifdef USE_LIBMICROHTTPD
	if (httpsrv_port != -1)
		httpsrv_start(httpsrv_port);
#endif

#ifdef USE_LIBEVENT
	if (stratumsrv_port != -1)
		stratumsrv_change_port(stratumsrv_port);
#endif

#ifdef HAVE_BFG_HOTPLUG
	if (opt_hotplug)
		hotplug_start();
#endif

#ifdef HAVE_CURSES
	/* Create curses input thread for keyboard input. Create this last so
	 * that we know all threads are created since this can call kill_work
	 * to try and shut down ll previous threads. */
	input_thr_id = 5;
	thr = &control_thr[input_thr_id];
	if (thr_info_create(thr, NULL, input_thread, thr))
		quit(1, "input thread create failed");
	pthread_detach(thr->pth);
#endif

	/* Just to be sure */
	if (total_control_threads != 6)
		quit(1, "incorrect total_control_threads (%d) should be 7", total_control_threads);

	/* Once everything is set up, main() becomes the getwork scheduler */
	while (42) {
		int ts, max_staged = opt_queue;
		struct pool *pool, *cp;
		bool lagging = false;
		struct curl_ent *ce;
		struct work *work;
		struct mining_algorithm *malgo = NULL;

		cp = current_pool();

		// Generally, each processor needs a new work, and all at once during work restarts
		max_staged += base_queue;

		mutex_lock(stgd_lock);
		ts = __total_staged(false);

		if (!pool_localgen(cp) && !ts && !opt_fail_only)
			lagging = true;

		/* Wait until hash_pop tells us we need to create more work */
		if (ts > max_staged) {
			{
				LL_FOREACH(mining_algorithms, malgo)
				{
					if (!malgo->goal_refs)
						continue;
					if (!malgo->base_queue)
						continue;
					if (malgo->staged < malgo->base_queue + opt_queue)
					{
						mutex_unlock(stgd_lock);
						pool = select_pool(lagging, malgo);
						if (pool)
						{
							work = make_work();
							work->spare = true;
							goto retry;
						}
						mutex_lock(stgd_lock);
					}
				}
				malgo = NULL;
			}
			staged_full = true;
			pthread_cond_wait(&gws_cond, stgd_lock);
			ts = __total_staged(false);
		}
		mutex_unlock(stgd_lock);

		if (ts > max_staged)
			continue;

		work = make_work();

		if (lagging && !pool_tset(cp, &cp->lagging)) {
			applog(LOG_WARNING, "Pool %d not providing work fast enough", cp->pool_no);
			cp->getfail_occasions++;
			total_go++;
		}
		pool = select_pool(lagging, malgo);

retry:
		if (pool->has_stratum) {
			while (!pool->stratum_active || !pool->stratum_notify) {
				struct pool *altpool = select_pool(true, malgo);

				if (altpool == pool && pool->has_stratum)
					cgsleep_ms(5000);
				pool = altpool;
				goto retry;
			}
			gen_stratum_work(pool, work);
			applog(LOG_DEBUG, "Generated stratum work");
			stage_work(work);
			continue;
		}

		if (pool->last_work_copy) {
			mutex_lock(&pool->last_work_lock);
			struct work *last_work = pool->last_work_copy;
			if (!last_work)
				{}
			else
			if (can_roll(last_work) && should_roll(last_work)) {
				struct timeval tv_now;
				cgtime(&tv_now);
				free_work(work);
				work = make_clone(pool->last_work_copy);
				mutex_unlock(&pool->last_work_lock);
				roll_work(work);
				applog(LOG_DEBUG, "Generated work from latest GBT job in get_work_thread with %d seconds left", (int)blkmk_time_left(work->tr->tmpl, tv_now.tv_sec));
				stage_work(work);
				continue;
			} else if (last_work->tr && pool->proto == PLP_GETBLOCKTEMPLATE && blkmk_work_left(last_work->tr->tmpl) > (unsigned long)mining_threads) {
				// Don't free last_work_copy, since it is used to detect upstream provides plenty of work per template
			} else {
				free_work(last_work);
				pool->last_work_copy = NULL;
			}
			mutex_unlock(&pool->last_work_lock);
		}

		if (clone_available()) {
			applog(LOG_DEBUG, "Cloned getwork work");
			free_work(work);
			continue;
		}

		if (opt_benchmark) {
			get_benchmark_work(work, opt_benchmark_intense);
			applog(LOG_DEBUG, "Generated benchmark work");
			stage_work(work);
			continue;
		}

		work->pool = pool;
		ce = pop_curl_entry3(pool, 2);
		/* obtain new work from bitcoin via JSON-RPC */
		if (!get_upstream_work(work, ce->curl)) {
			struct pool *next_pool;

			/* Make sure the pool just hasn't stopped serving
			 * requests but is up as we'll keep hammering it */
			push_curl_entry(ce, pool);
			++pool->seq_getfails;
			pool_died(pool);
			next_pool = select_pool(!opt_fail_only, malgo);
			if (pool == next_pool) {
				applog(LOG_DEBUG, "Pool %d json_rpc_call failed on get work, retrying in 5s", pool->pool_no);
				cgsleep_ms(5000);
			} else {
				applog(LOG_DEBUG, "Pool %d json_rpc_call failed on get work, failover activated", pool->pool_no);
				pool = next_pool;
			}
			goto retry;
		}
		if (ts >= max_staged)
			pool_tclear(pool, &pool->lagging);
		if (pool_tclear(pool, &pool->idle))
			pool_resus(pool);

		applog(LOG_DEBUG, "Generated getwork work");
		stage_work(work);
		push_curl_entry(ce, pool);
	}

	return 0;
}
