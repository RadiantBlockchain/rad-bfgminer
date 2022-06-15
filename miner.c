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
"1EWLon4h3neosF5teMT61S9RBd9Kg7icLA",
"17mWRb9csYE1VF898MuhrMTA8PBr6G4qKi",
"1Ms5dcKUF9nvfGuiEQJvgNL5ex8C4wrdXi",
"1Le2Sdn66JviEjjhEdgqHG1nC8VuxMYade",
"1FRxhAs5ZD6wCTXxz8NF7dfRRv6FqFmm7y",
"1p1ES75wrrLn6ZBJvEzRK2XFi1BmUn3qD",
"1KQsaXpPM6ArMoUZpEYFmg1xFSK7UbTGy7",
"1PHSg1GxrqidzFoL5WrVFahhjmfxHb3s7F",
"1PSB9p5EuGYmeoC1ymKFfv6Lv7YTEJy8QJ",
"1KabW3deFFW6KsBsw79CRaN5fZPUvxrtvV",
"1JWyvKKdrvGRekZu7xW185giWKBd8mZ4xB",
"19BiQzNXznhw2yoEZ8yKUHPdMur3sWG795",
"17QH3VazmjRGkVvDocAuCpDWdF3DoSpAWm",
"1NMCwWhkofbEDe1Ja8eHLzUqypvFUXyhzw",
"12TtS5tEaSLMVZeSvStWMRkHoUkBwwXE3a",
"16Pa1RcQ4QFtMgzE1tfzYpVCj9jsJmE685",
"16qaBwujNmqCSLznbbrStoQ17zYeTA4BDS",
"1CBNrEtNVTk6kZDedny2M2sHbrUP2ACBwU",
"1PZ5b2CnAvqBqbBXaa2y2xvc1fs3SZw9oK",
"1GfLKbGve2mCyzwGE5zeTY6NQWefRcFVkf",
"1NzZNxv9L4EdavvmpihW6758hBKEPPe2yV",
"1CHw4rraAegu1hZsPCE9LAPu2BgmY9R5wE",
"17CUNcu42wWENhjW1z5mrTfqMgcNPuffrn",
"12PJXeNnc6ogMKmdBHdw2EuKgLwyc35qUZ",
"1FHYDxt4QsTQNT1ERENPY78pD5b7YomdZk",
"14gWYeWqrZtCjZfQdrK1LEDFwrHgHgJNPT",
"1F9SorrMizpcRr5b88K61FLgwcdR5TkEGt",
"19P5RrSZRzwazXggiX2Q8Jkd8gfUPapYVi",
"155W1mLUfxnmdDdkbw13Tw1bRijer6CVUx",
"16kpjvSDKXk2ZBBDghvHMfhqFSWrRK2NaG",
"1JBfQuKkQK1XhUDrXdaBFPStiX9NsQJsvK",
"1CqN6tTZAjEquYEnroro3BH54NouBfadbC",
"17T1Qtp2xCi2Bd5mddnsCJbYWqPC6yuPbc",
"1ho9y9Kpv8bTxsYppqcx5pGwKrqGWJuh9",
"1FBbyTb1WnGgCHe2SV6EZxP1kZcJZUbM8k",
"1NinioNR4mHxDNsNPYjgrrrU3kLKMd3Emn",
"1AmERjuKbEifrPiWTpmxzhUaC95d61MSRJ",
"1MT5nUAbvB1anKBsHtiuoHAxbsuX8ZjwWL",
"18xxdnudU8hfwsnaEBWnBUWneeQhMhdoL2",
"1CzeLsTC7yecxcE7nix4GJ152vD4B7KMkK",
"1F182nvKdGmPfFWU3ReBQZJ67YM69ZnmXg",
"164jVX2fEwzLvew9ZEXSv9wPYutjJW3bhS",
"1LHGy1cDBJ67dYCPS5PP71SKQTL5jsc6j1",
"1DTjqmEShQkkWpQGrKzWciMAty122DaGeA",
"1BW8ed4ts6YJTmzfR4NQHUsuRHzJASJvoL",
"1JgdbpV4X4sXjVFu3nEMkEyBP864jC2qFm",
"1QBoJZLyBES1ZEQSwDTRrDWwdQzSd7uZwF",
"14criRezDPA6oKiis8RtMgAPwg5Hyst5PK",
"1DyYuU6W3JNQtHWhVww82BnXBYvwBebxMT",
"1CaZwTGGp9AUsxBHdBaNuxwnb6sZSJjFZw",
"1Lu2MEc6MXbZYRrQBd8TK95LFcodtc6b2Y",
"1MqbXYhUxzYZJ73NpMEb9DaMYcUPEMA6Xk",
"1G6VuqjCB8w5HotnVrRqmofKvpzGCUxAk3",
"1B9StxEXtwNrat2QZWufLefeHoFF6Kb4Zp",
"1L6FnXQXXRD6tNiDr729Zzejwv5ZsEsZnb",
"18We2V4Yw3wZmFhfuARjr7sAtw1YRU7Wrx",
"1EzktdJ3ddAo91DTUkKQ7L9brt3wdGp8Fq",
"19Pf6EcNv6i4ntkFSwn9tA2NhbhbgMcvSh",
"1LgdGT1FtyWzPPbuPJ4qe2b5xZXyVXCGPi",
"1M92NY4pf8K3PDSPa1qGSS3KqEApM3h58t",
"18DcVwhww1cLXjQFAWFxCAWXc13coZwL1r",
"1PyH45YoTXqdX4SN2g5TZkpL9aYRoaHnvB",
"14WvtQiN5oiEpw2psXkDrkYB1bDysEULjz",
"15e7nG1whtxjHdskh3TxYoUqmX4T3P5Ez6",
"19Rr2hMzv21PCJPczz96GrjonhXY5YuU6d",
"1GpGvfTXr2HLU5DsUdXmwhbdfs7qjG7UcG",
"1F7DVrBL1bhkL7akCKoJyrgGQpwGRegp56",
"1Pymyy1TzjUYNNKzAx3Q3VtZb3X4quKLZo",
"1UUtzbMi4KJmUwbxshDRLNxf6u31jCn4q",
"12pU3dw1cCX4epa812zaQoCyT1mEK3q3jS",
"1Am1hpZVvuBQwDeA29h79ZBL4GXtb9yjMC",
"1BtxwZcMPdqFjUMsoHaqhyXyKg147coL7z",
"1xNnwT6agHER7eHg5tEeXSN8JvMC8WR6t",
"1G4tfxjBpCpnU3DnDBaxAsirATGQj8RYGf",
"1PCgGaByw8prodbrwLPfBbg5vkMTpj2rvk",
"1Jp6tUzmffXDV6ivausWZ3D18Nn28ST8xC",
"1Lu6B15DjiebQcAiY1EhGRzXoN7zgcVZ75",
"1GfD3TngXJAeLz8L8ojZ4DvYwzYKeyP8mo",
"1P9w1J3L4BgFaj2zEra5Sy7p13tjZTd4nh",
"13Awy2fdVtWXSQ8DKnSL28ksxkpSnavGX4",
"1Hw8ruJ1WY8yKmCuwm6gBDmRTDzvwmVKqq",
"19Bj7yZYvsHEyo63ZzhVSJVFrLptzak3bt",
"1LQ8NQbQ8gUn3LTfknTTe5AYjRa9SzVFQw",
"1KadHy7YFuzupGAq5tqABGTzbt5krCVLKM",
"1FqJMp6PhjmGLd9gVDppF1tPtbrE96YJvW",
"1JRpWivvbE2ESfHyQN1MNojkyrsAFSgMhD",
"1HirHumuwhf5sHzperGMV9axxfdYn5JQXZ",
"1rc2EEbhgSfvHXXKvBMwyFkR955cenhMr",
"1J9HifdqnTrktTmXs7rYVTkWrTUzLi87CM",
"1DykM9LwxC2AMwjd7Gi6oPVFwEgf2CRfLq",
"1CfPdy4RDXuRLuuv2dTBTzm7mcesw5RrKT",
"12XRf9ZtKjzo1i3cVkATfGRv9MV2cajWPN",
"19fJ5HFDcY68csPWSE4AMWjqkctjt2dRH2",
"1Az3CtiH9LbLstndYCtP8GmxWwXabVDgg5",
"1GtThA1oXNKkGKRTwo35i2DH4LvRbqvrTo",
"124E46ycLUEZkBg6MxdiYc2RqxsvSjR2tx",
"1LFgLeCpqVmvoXRTi1F9by86iebGJSkm8v",
"13EG4d7VPDtwzgc2zrYWZgW5UtPRqN6bLT",
"14K88bmf2sD2MJRC21AhLdLNTbV3J7Aha4",
"19uztLiGNvWgM5Uy65C7vZxkYk7DrXhboy",
"1HiR1CzFiisj5MLou2VJh58usPAV5GZhJ8",
"1FuQksyyhwYsG1NbmhJgwNQU9E1Q8jxuXe",
"1NvegBepZVhG7MCAA91f1AKrMwucHCMT62",
"1FtQUeBCa3pNevym2BYCtTxvnk2yBzEWah",
"1LyRosgTeMePr8x3hGTEmFAdEJgATr7n5c",
"19E3zLU8umGdninAHjmsiqnaXiC8k2Ec84",
"16SbnryRLZoguDaF4qMsZ1e6ycQKioCZRG",
"1bpJWKaHS6S3PFSgwqAsiN89qxNKvMbB2",
"1EZ2PLSn1HBAJYkvRKesQ593vLhNck8pYU",
"19LC8uyDRtxnafJPo2xzUfMRqRAvyYUjkN",
"16WHd9oqj2yJyBWSJ8dufTmdYzKLFPNfrZ",
"1KiyDVMRtKtTS53MbSfeGD5gTDwtbutGTr",
"1PaxAhooHYZ1m8AbuxappsjW4ejtuAzrQq",
"1EWAA8GcYjBeQxAE9SR2VuSswb28tidTu9",
"1JgyP5ozcYXxw9bmgH31TQwcnT1qTdSu1G",
"12BG5JauZygGUrkJnZCFFyZGry3euMW7p2",
"1QC5oWw1nuFMcd4952r368R9kuFbysZKrz",
"1CR2Vnfv5n1QCaE8kHw5yAnhtUdumVzzxH",
"17TwPvNqSUQ9EgG1e2WJb49AfmrAwie98k",
"1KYKgpjmQXp6jYNu9wkN3vkwWkB1DGYA8p",
"1GMESnVuij5FPrvFg9JXrGHgoi3ciY4B2N",
"125bjLfrwSWzNPUZeSV9ZWHGSiwtWKAspD",
"16MbNz7HAAKVoUBa8KF9yo6WEQAbTMXxuW",
"19NGpuneFGxXLyfp87wMksecn57rcG6WPV",
"1PevYJzrsGsCh29sWDoPk7fDXrUUjGpPJn",
"13rg86zeLtP38YNNJYRVAs3RCFurYfwYWZ",
"1EAtH5V97XEVyjX5SWaBn2Av9QGqSoTy86",
"1LWYLZH6ZtUHhoaeyqUA3aTgpkuKiVX6w4",
"1HgkvntACBfCoMx5bX5aP26xdqUc1Qgfzw",
"113owdy8t8ZjQQPLL36nxYHbhteT2SyEsb",
"1EujKK2X15TpTYi2dAgxFf8YfJV9Y6K2mV",
"113pjJ8huT9j7vnJ31wr9kFC6phE7QB69i",
"1KwaPxk9U4sgNxJZfV4u8piacbZxWWU3jP",
"1BKBNNxzogWjfDCwE64pfQTkAFtHRmASWA",
"1QL8Mbff5DQWmykFT7HgBP6STsjqdc9Bxr",
"1DPyTXY9tV43J5Doxz8Cb3fhWBR36Lx2J3",
"19KtdFNqCtDkojSmjfJKrgMwGocKxvpHeF",
"1HUm7qtCFHBRsrqoQoStMZv7RYdNQQrFaV",
"1SyRygCC5NzvFSDUdHDwdkf3KnGg7LZaL",
"1NzvPneHiDBBvUpa98iU1NXzEfose4f6yP",
"15S1334VBhC665i5ANyBwmdn697Ba48M8h",
"1HoK8hLdigaVkmXy9fz1AiBhbMpJi4B3yW",
"19QLG7De2hDYZ2GPhbqmwubgteWRMogMkU",
"1JdNy2sjFHzCTXvdiKDtZcrr9oqodQnStb",
"19mYzLgHfwyPF8XfXtWm75nHaEjXhJD7SW",
"1KfuXrCotVS5ia5ptXuXDRkGvGAP7eNJDd",
"19pimQrZLWb7jPyJHLpom5QgAU94na3htJ",
"1DBrRFXRLkAWwhY4nQsL4TAtFDz51Sq61t",
"1EJNM2cufX384vvPdHByxdsgTKApp88dt7",
"15AWEsR2aARLjZxYi5YXJewXRfNC9i2RCM",
"146Exp8tTc47UAxtyTHszAQfEJkQhVBCU8",
"1GBeMd5H6tSRXRiNXSTkVivu4FCbQzZLsv",
"1AojBjN8jjpEFEREhrxcgCnBoLCxfb92B1",
"16MQKZu65yhMQxE66HxnzBGYCqgBEKoGWN",
"1KPW9J6Hi6YiziRvUcVrGnac5NdMVKZpyX",
"15wxZU5HPMWxtmRdmPEaWR4XUHCmYRF2Sa",
"1LmV4Rxs44HFYxjooh9GXgv8NPbkpY7MFu",
"1Q1S9AXSfvxRbB3XitUZ6fEnPLiAZUZfJ6",
"1Fchi6VnducBWYEsvXKkV9fg9vRLwNdUFD",
"1BoNUupFLjHc38ZiTAsZHTvjCxap9djPt4",
"1LHhQsyrVLyFQRuqgtuYZbKQpBKERA5N3x",
"1HgaBi3RsQDE5Np9zGQRApz72N1Ruh71ny",
"1F4NnXQP3Q5VktRP1wtRBrFxZiVxVyg8T7",
"17Q9mkJiBsS1bdVsRn3P9DsjB3dPJhCAuC",
"1AqcxHpFuW4kuTD5jcNQzCP9ZuUysQrUhC",
"18m7h99NjRJ94jQzLet5RpVPziNeAXFcZf",
"1HffPz3qzwoU8dWCven6m7Hc4f5NuAptAb",
"1Fjd29PJMe3c9K9LJpBcM5sJoD34k7dHH3",
"1GiUSXFGcRu4HRtmbmF2QfYpMYBbBARSNS",
"14ZF4pB1DcSQHgcqQaskvum6PQNk2BmY9R",
"1FTfiRmmwPofPi4dutfSSfeBq3VyWsPi6Z",
"13kC9aBFqLhvGA4pNfZ8SXiBXg4UDEGxn4",
"16mYqhJf4QzFQK1oCNwH5Fi2JDetugojgf",
"1GxdkgCCoDqQcKNMCMDsvjBkA6uL7DEVcg",
"1FyR9UnB8DekFHfQPkMAHxX952iZYCQam9",
"1EhySMZixnwPiVL1GRZcAbFoDwreaJMESo",
"167v2EHQZUfneBp8n6N1Jhsud5YqEmAYqa",
"14jbJd4tVvNVBRQ2Skp7fmViDQ4sfZBUAp",
"18KypTFZDm6MVQGZWFP2K1EEQet8i2HCei",
"14q1S1sW6HF1VyLish53NvcJ1XxSYBsJgo",
"19qcRpwp2ooZ2HiXxd9FVotUBKDPmwq3Sr",
"1EpNUkstZ3LVQMaQzhgLSnMMTf9h3JAKFy",
"12yVdk56PoJFPNASWTSVogdiuBvAs1ejGT",
"19ypvU4vLd2tEgwhGnje656FrYyay25Pro",
"1HqWqjUtTbJnQ6QMc5eiqd8ZvktDGd3Bkd",
"17ToGFPrbnQzKBjeAiQkBRzRDmn6By875T",
"15gEi1coNLNgy4GDwMspvu2pAYrqU7hKnA",
"1823E35hCxGnruoFEXPju8Y1yrx5ig8nzU",
"1LY3onboThwwcwf92bmQVMDwmzZCkAB7VQ",
"18QYFwjivYtbYtqZ9SsMR5KofvgFJUCdcH",
"1FeimTwqQCCXHk9mPQf4oX5VK7A1FCk24z",
"14gz18Fd8J6dfGSJDxXNyaUhWTEKtuG4Ye",
"141juFJkknXeDneXxN8NV6JemoUkEmtm3W",
"16jBbiEfJViNFKBJzD54gJKQ1HwWEK57Sm",
"15hAs4r6ccdFqkFmt9v1i2skbKFtaJJj4u",
"1NGQNzD2FZuuGK2LxV9w8YNQYgJj8avQdk",
"1UHypcfTrH7UwBAwcJvLt6Sy9Vz12T6mt",
"179c9EXD3ehB6HePRJdBNRd2jmPjZTazd",
"19oXNP3zJyNvP8ZLrtURCGjBkqnkGBDiSZ",
"1FhDN4QVfttHhkHc7dWtpJEw7r1UAGZWUG",
"1JqvzWDkHiGz83ySs4tS7k5hWWnsfC3iE8",
"19Bs2h1J2ufyhkHZYzB9H2AWRUgjfYUg21",
"1Ct92H15A7aBhunucwTpP8YgaeGPMbr3Br",
"1F7QJ33rkGWMxphJYUvUpmuLB5jPocuuf6",
"1EKfuADLTDFtv3JqMWLb1oL3SviJa31Mai",
"1MkUTwmeQXNM7ZWzm2SRkxW8GdbHYpeyf",
"15F7iHW2LDrwHqZKkKP5w6ENeKoj6SqhyX",
"1QLX6oDwmDcaRskVFzUcMWwNNGi64xAMJ1",
"1FLkrKHRkqKP52fSbfimSuKQRuEyD5uFwK",
"16ArRsrHfpcJc3DAH4ketSt2kt6S1GnRWK",
"12VTTYiARDtBQfTiHecmAfJDpBAkixEYND",
"1FjvhuWZmqUhFuUq6i8ac8y1Lv67VnhYAJ",
"1N1ND9j8oCyEof71ekY36PNpZF71soBpuC",
"14CRuSUHWbgKFf7Wzcz41RNBCFpuzLZvTZ",
"12qmM9o1eBg7r3obFVzke4zYQLaEpt2r2L",
"1E71nwfmWzDBP1EMP8C5gDyRSpvRfW2b8Z",
"14Aq99bwKgkMYVyBhgqRFB9pjE1F5FQ8xz",
"18Ssk3oNgssCXKARrkBMWLUv9UR9MgUD8r",
"1BiGXtvYbD69fdewp66Ti7BXfUytgCg7qP",
"1Cpck3aCjeRbGoEaBqYntXLXQ4MSjZ7F3e",
"18fLqizq1yirKQdMkVymRvvZpnT8mLamuo",
"1FRQtKuNh5D5aCTdA8jrBfh9rQomdLfWcG",
"1BMpytGnuEpUj1ydrzDLZY4icnkmogrehd",
"1HdJGbLswozMZcjg6ccztaa5em2a6XKe5u",
"1JBt39DPXpQttKV7JxZLyxtK27AwJE36eT",
"19f1VhpNerVUwcPCkgDbyAmq4D7SC5sBGR",
"1DXHchpMfe86ZzVbKNx66ioXsYo96WbFmL",
"15fEYHcMxG4ENEXSSnEeXSyM8xapRy9JrZ",
"1DiSogcGShJZ3ETsxpXJQDoJ7pyxt1HnfW",
"14tRfthKBwAsmCX1xKpwRYs3qzhtpunaoT",
"1LCGqxAJZBcp3KRUp91J9XWHpLsrRnquzm",
"1P2C6xk9hN8cP6AHNuvNNzuo4dWQrRuve5",
"18FisnEWXu1zBsyyiec8rJGDKgfmtFL2b9",
"18heGGxoc3cb7ZteTz6HYQPd2py1jxxmUU",
"14HRLM6qweGEq6jBrQf9NU7jDt85hRCAz2",
"1JvkWYtDE9arQmj8NPmcB6PqBtao3YisfF",
"1GuG6ugF5mepzPeugBvnjERNFWGYtiD7kY",
"15V2MnEGGkyka88fWjivV7wamiUFTGaR9X",
"1ZzMtuzBe7uFrQvVwKeSHgc2WRtQvSM5N",
"1HGqmDNJr9s91kdywkt73KFL4qy5QsfvQp",
"1BcGXN75A5hxFGYgBhggpfN3vcDJKvXXsm",
"1NiWTCL5RkYz6LMyj3ZV9dgmBLCVpcmjK8",
"1KtAxdjncaT1MXHMz7rffcqNmkEL3tySd6",
"18SJXvJg41muxhrwKDcqPhZzzbAzeUqP43",
"12gDAfjrQsVwVufmXX7pZKzUvyJ4rukWDK",
"1EnwxkLcqkEeAipkztwJKKAHd3wCBVBBoQ",
"1KScRygoqTX1Vv1d9qS2kKWe3gWTDCKu2H",
"1Q9R9p39WAUeTCozq5RsmQjQoinRXHcWGz",
"16eXq4evoQ26jj9i4DFgBVNpmjHpRyjaY3",
"1LeNcG6GyqKsycjegp1S7gRfV1AkCd1RbL",
"1PHTg8pGN3r9iE3oV2qBcHa8WSd2Q3Urec",
"1G9uiFhNZJxsxK59P3xWv4ZBjFRF7J7Q7c",
"15F1Ut4CtRzoU2nQxgiJrXNhZ22E1fyHZ5",
"1P4wTbPLcEhg3iVTgYMBtuKKFrWwGAHMAf",
"152AWHgzZazotXjHgNJ5aFpMZ8WHjVqBFB",
"1LTQiwNmk9PQrErAoTZM2zq2yp6nM6Tqni",
"1FnqHCKbvGHzWRL3gDARV2iamdbqQYYSCL",
"1EKYdDX2DRmvmrEvD5GQc4x1orT2Xe9is3",
"1PUecU6StEYPcZeqi6W76g7essD5NkFKJV",
"1GkjK7SkGMPUhLcQnyRnSCHpyzMQxG6VgN",
"1DF9UxgGjxSnR4Qwv4vTK3ptL6mpSYa6si",
"1E6g8o5QcFnhSsMQKKaLngt5cs65A7HZdq",
"1KB2XayV74iChrJRxJk1pibHxEUcKtrokY",
"142egrmgX2jhYLXQq879WVbfg8nzpHSoGW",
"1A3J5UUsRLoWF1ABrfJiMADC8Rm1KXtv34",
"1M384rFU2KdQsMKg8zSCtQn6EuC9prQ85a",
"1FT1MnjWDSPyTx92Hyny5rJnT5h3Xtu3R5",
"16J727dR3QNcyu8dnqy9mKh17jBNcutHBX",
"1P5zwinLNNGW4z2S6kHvask85C2tUfNTEV",
"1HsnpktEiKX7zA2CZkQC1FN9qJvQjbvwwf",
"112j1Upu2EM47CPzccGEQYEvzyRHKjTF3D",
"13NvpyfsCQ17jdBpSnabNpDLtB6LGUFoQL",
"1LQoz2DG1xYonuEyhpC7cS9fHXqBHSxpim",
"12njML8scS7PXYr3FL1bRroeJTasNUNonE",
"17Fy5z5jZxtnEMJAZDmGPRp6qdSgvX5MyY",
"1EPaaV9YR8cpiAVC3XoqRxLKxJZ355ghFP",
"1N9SLmwUY9G6DacEqiaC6ntm9nQvFog7D8",
"144oAMFevAknnTTnKAnp7gcKsJLBWjZWWw",
"176frR1CmKChpix78ZHWQ5FN8iRVVjBj6L",
"1FQC4pKNMJqyV48epTpTEFaddjtmRbMfJk",
"1NyeiaCoKu1AqEf5HxQVzxRyznf9vsq9yY",
"18oyjkKtCEBMrWGQqSFa5V9bkmqa9iJi1f",
"12p4XSgPsSyYXsdaAbBb8MvVdP8rp5sRgw",
"1E7Vqqg46PrNy9jBdnSBqJJa9UTAg2wvi6",
"1KG22Q5BMrk6SXzaadRj8QHaTkTj9MXB5f",
"1DEDXU6c5a5RuZRbf9VqyhcanvQFn3EhZu",
"1K1UXvv1BorD9zSki11j71poiWdYLF65sg",
"148UhJcfgyYXnNxma8xkhDy1Wt7Z5h3Xy9",
"1PVK6G3jmm8Km6Barxa1gPg6CWJtMq16ij",
"1HsETyF8ZT56NuBxErknPj2m5PZADVxyLM",
"1FV63WEHkdT4W8fF72migaxKvvDuR3XBVw",
"17L8YkX4Dn8oTMnCvwVXQaLQdTeJiefLME",
"1JVbVtt3UrJHyfem1vqQjFxZGmq27t5egW",
"1Ba1k13xpJPMXN3sUZV8hn1vR7VMnmU7ZA",
"1PBgRG92vmV8AwWrHBg1isEmMWyocqgATH",
"1Sg7cukEtvb2SKSJgSwP8vQvEeNyi5vGP",
"15qBxNTktc5yE6TdAo1KcBKY3hRMEGYZr2",
"17CPZamkk14skTgToF8PUkMxH5yUtdJHLH",
"1BcuGB43UMSgLQpfHrtvBuKVXaXiTbX5bt",
"15i5PezzdQaxW6wEFBvpQKoVBC2XPEUF3S",
"1C1J3eqVozGTWUnQq44uit7MJYZz4AbmBA",
"18aA9msKK5vFShLdUQ2cGfRXcEgirCcBVQ",
"129vyNTf6AsMnYR1TBEx1bex47m7fw3Wz6",
"1JbwrBPnSJHyiET3uzwMAVeQYAtfabu9eh",
"1Jr1tZC4CwsxVjeNayzx9siNM3GFdDvCzE",
"1Gr8kXvai1k2JCf1Ytep3D3DY1JUHyY2Q2",
"1Ftgcxs6PEapZk6JVqDz2hF6etwX2EMtkh",
"1NnS7qwYJwpnssd2oPfvhEjVduxuMA2CP6",
"1A98kwt1b9cJcKEgapq97Xj6MmbkbB7292",
"18GXMNRSqnxrbBTU6PXtwggaKA4ckpgTr2",
"1BMjhMbj8qWVQKtuqqMGyuo44Jq1uERWno",
"1CujFJRU9kkQkqY3sgp4jTTJ9D1SBbbuBn",
"1KbG4uA6FUWZbGcWy5Ns94ibiZvkT3snxB",
"1BBqD97bqKpVPdPJAZWYw1ed42j9jBNfXq",
"1K4xPDg6VyTgzXZazaBdxB4Gu2XTnFbrE9",
"1NvcxqFr57tadDbAiqC6ektmy6hKHJ6WbL",
"17hefNQ86iYaW6JYoDdx25W9ZyPUhB29ik",
"1C6VmxSTUtq1mnqe2oNU3x7dFmWcUS5o5o",
"1GUAdizwu73GfQpNDUDpegHMBXcfKTqHX7",
"18F5aMZfKhCZHxngPW5ZU4jDF8ykfCATuJ",
"19ZJivWmmZoPvaGuyrApCHCJqrtmuwzbCz",
"17SSxXdP1xE8Rzca7UoFdGbCkjcjxAWRgH",
"1LG6Lu5iZqAx73sRZnEeqHpbv5jxFApd97",
"1GqfDw3jVCdWYcds7MiZLCwTNmx2gCQpv8",
"112HeHoaJwUgckKXCeWWqXVVod3Met8NvD",
"1N6npho8LqtLPksBjhPA56HEpfnuJpAC66",
"15J3pNoQDF2TrBJ1VkVQssWW9ZJCDQtura",
"1HxU4xiqoyajy9faUxBQTfjECGuXLYpRKh",
"12tz5Xu1p9khLfJAS6wNJ28kUsaEHiHSZh",
"15AxobesHJ9pKJHa9CqTcZ8f84CLFAWCmL",
"1LJx3bsbLifbWG1EB72FzRPz7TjXWoFnTC",
"12cqZScZyw87iSPKGhGVHVULVa3V1KE4Fz",
"16PM6Qayw6ke5kEYkXVJCuDQ1UzJBU8sQK",
"1HJaqxFdkh6g1FBDLfLWHcRAvs5czVztNg",
"1FT9phstXgjWnfribZEfyXVd8RxTx9YBJr",
"1Fq9asctAWABtMZmYw2HzmGStFNgqgoNQ7",
"158MBtFxE6QMz9DgT1T6LemhjtpXyGwtU2",
"1DxJsn3sWykzYjLFXg5vtg5jnqCfEbQTYE",
"17LWiTKmPRHSMKBTpE2rpSzgh8XorURxpX",
"1EbktRPZ38BAXvozxeh8f9L4Bj2ACQLALE",
"14bTwFC38cxg9epRjw9WyL6n29hT2AiSWg",
"1FjXZd7kqU1uusF3Saq8snC4Wbphmc2kat",
"1EkZNg3JuWFaJvE1Xt1DksmptoaXqqz9s6",
"17CoFXf2CSbG863uz6BnRvjV9zTySnMGLL",
"1HyawjoMyopE4EbhADMboZJrYJji3v6JwN",
"18jsstPcwmZk9hpmqHxogeevJK8owL3Jpn",
"17v2sFfhMtR4znvR7ta3c9JZZ7iQpzSq5H",
"15mGeQqRgLLQ9Zej5q6VKFQYykMUsC5RRV",
"18WCP5aLUcE6ZoR5cwQhWzGgcA67b2cbN2",
"1QD2tx1RKhvgWb5s4sRmbZNo5tfPoHvSxs",
"1EvNSqrbgGK54H4w3iUWT6y82NxTUc1jxP",
"1Q4euMuDbZohmFjwC1adB2kfDs2ChvVg7h",
"14TiWuGhX8jXcvsFyxR1CKceiVL7SgErqq",
"1BvNJdwmJUwSmqgkLDMyqwKsPjmCk5h46C",
"1GQaHH87PVA1nR51UQ38E6KwSJxEo5Fdcz",
"1BS4Su7YvjXVHazkKPgAVQYaL2WEHmVYZP",
"17QMmddTFuhiRXhBvG6pEYJmRYZ2D64pHH",
"1FPqWSshVHGqv7bTbg6wNcYehGfZtQV24F",
"15FLxU6k4K4f1pDj6MM2XbYKWjddsyYoHg",
"13FQjr9CCoFM344tEfUM4XC6R3pcaq38YQ",
"1AtDZYoJHYEoWw7WYjphKaXbsPaDuUyDAf",
"1B3fFE24QeYBKy1zcEgq1WKaBFVLS9jU4W",
"12XDMnde3Kv9mzEvUGuB6vgjEv4vJkTwV3",
"1D7aU5RczpJ1Ww5Y8tvat2C2nJHHKUnn7a",
"1KFtqxt9YwktBAmkT44PY8YEgbgCNzFVHR",
"1HMgZDmSwM2K1ogpQnoLRh7CLyZSqdmoBb",
"1NkwWXVQCk7k4gM3TVGAsyg5FBwkAmjXS2",
"1Q9wggCvAM1am6Lxaz1tQfiuVSHANZ9N28",
"1CBqDLNXzFTGT8F3PNe3xGrisiuASAcf2j",
"12ypk4pdpB3S5LiBpbBj7H2B9t8DWL3ohq",
"1KUJ8rJx1BGK9LnaxDKDz1Pq4W6gy6JEvY",
"149FRA9nat5f89xvGpsKqfmNKvCXuZUR2g",
"1Cdd9mouMDq8gEAhSbhuoTtY3vykTTNQgg",
"1QKQVhyYzQYDukKsGHUsbAPFWY5MxSfLD8",
"1DeXAognDwVfBqzmEbWqZPGSf2WF7QH7pJ",
"19tQdUrSR7msGjGP2TPAD1U9iQYGPKPXFk",
"19Udg7kjf4dJx8jvMWs4o5H1m5Cz7tBChb",
"1CP9vZykPwZBZBRMaofpoG2z6fKYYMY6gH",
"1KuJexpvR1w7vHNYVBuQWkDCz6sQyZYriu",
"15hfCmruCL52GiE7k389xHV8dmgAB1RMXN",
"1AgznPRqV4XWkkC47Mg2dDdQTYkT8JjWEJ",
"1G16S9UWzxA7QSBY8fp338YwiGY9T1q2SH",
"18XMd1ZtNwsnUyBmNV4qj2NDZY2d1zuFWi",
"16H2dQ8i2fzhZToSRz7Y7dff6VFMjeBSAZ",
"1JtubkZdsKTYK8sCS3ZdP2oZSkJgFaEUTd",
"16eW6DFjsnZB2tWdstMEjZBBLc74iUjq5y",
"1i3CP6iRjhRZGUukp2V1EqZ95ZvU4NNMi",
"1MC8s1uaBu8wtt2qkwKyTyZQjavdrFVunV",
"1Pxk3vUz118n2o7M8YD6dCtgxWVXvmj8Zb",
"1MuMXn9TaomdzeuMsRsyHf3kqufNJEsvwk",
"1EqLD7zSBXLQmzKiTPpTC2HWGpQSBoko9r",
"14Z3tD4hMpSRiFTAKrQRB2CsAqjm2WuffW",
"13Ah74nRjWFEdu4r9BHAycH7wf59FizH2R",
"1NjtF1fnSNUpcP2Y32knQQRQApU81nYLkX",
"14hqf8sDTepLktCVTYcuNBru37UJTSSJfA",
"1NoQL5gQVNHH4ZHTTeEkLGa4p5AJ3m52HX",
"1Ho8jGUSq9L6XpTKNzfEFUCvfq6TVbXzXZ",
"1EhduCm95MP2swuxHawuP6XBdqbxAGeEPH",
"1BeWKx9VrgsvFzvTeDQRxQmRy7Sgs83VYe",
"1E6E9nAEChTV1iKVmB2dAhaPmUbL1By9aM",
"1DNMEkUYeJZ7oNRVtcb8jGC24bPa31SyCK",
"13GHgmQ5uijryz7MP8Kr4bX6nkWESmGWyf",
"1BLuCsKbnQ6Cr1BoRBmE2Ju1Kme7YcaeFH",
"15QCgfBHqG1P8eF8mM4X9kUqRMGrjV4sK5",
"1BvQffqMmNkDxTV3yk8XkhnG76fNbKquXf",
"1AabyDS4m9c1VN7GPnvKrGMGmgMRn9pw97",
"1AkPpvUWr5nxnBk9s5JxVTFkTu6jdqEJYc",
"1LUa9y1TLZHE7Z5vrRQon2FJXUR1Qyjzrh",
"15Tqfs3Q4eBLhxGEg5W4gt4aDa7sCzXmCr",
"1Eb8nnLyN93yjRQPksT2t5kivNeqp7BSS5",
"13oyPM4UUeAYvSPnN9o8byxU92Qjb5ePrz",
"1Jphs2ghQKdqWoPSPjXdAQvLPCNfNyCMsu",
"1DZeuS14UPxKSu9UeM7p492fLoVuaBigPC",
"1AwUnuAWWjtxk18he1CXL8eBqaUGpMuqYP",
"17ii18WadPWmjbJBU9Z2CQiX5CLhs3aKZ2",
"1J3g5ndoaq8HoJJHqmC3P6zXTRgYJ93d13",
"1LEUPz5JrKGmZbENMed7aRfNWv9sTdky1v",
"1JiKFyFx3LFTXt2gDwVmu3nJ6Se6SpDoPD",
"1DkQ1jZEgG3WoaoUEJiJoRDpZS9BuzVkRm",
"1CxkhhLZ9qwXYMYF1Q3H9DZKA7L65QjmUJ",
"1G5R7mneuYU4ALjn6DFNjxsbEP9bCKooxa",
"13V947rR5NWUdCHYdEs2S3gqJsdTVpAufa",
"1FPXCpKCwo8U6LdKjsPfinf3PcYqk1uu8L",
"1DPedLJdEczkVhi9EdxDurTRgTMZwa1XmL",
"1AEFWZGTwcKA2EyAdz6H6Q1pAZoHrVG4BV",
"17bbmGHnD8d9iV3aesbUNtNymq8wxbyfpF",
"1CvM2rmXf15tzzmaick8LypE6HxWWxkNUi",
"15CG7cFd2TDdTfxCJaiJYaum4pnsWVxGzT",
"1Ny8KB8chmxdMrVjDn6KYXkvwQconvm2kx",
"1KESXVs3oq19RJYQPcDR87AnRpeSRa3J46",
"1FviUwkDAM7aNoEkSWtq3iAGD8d9ZUbJjb",
"1KVfTR4codUTDCwoAUNKmL65AwJtEMybpW",
"1LUTwai5SvfCHoFPMyKz4zfgEc53YrZUEH",
"1Joq9fbTgme2pMfSJvDiZsYdHkeYJaNCM2",
"19jQNs3H78pDoU2qvdiBkzTxYcsJKtvnR1",
"1MnfKrk4X9qLesdRjXBzLMY1Fkgm7HcjkH",
"16JmahKbmZDyTo1MSH5iWwbN4XxoLLZewT",
"1JtWkPdgB2o2YXaXQSSZ63CDyoAkv6cRHn",
"1DNqic7uEPR6aHECmBPk7Emnj3ykucJP8P",
"136tiVoc2AkQP1NYSnBUQA29XqbR897eZT",
"1Lms4YFdaRHwJrauVTi787jSGNkbMcfaYy",
"1HZ28rtBQWnEy9vmiFUhK2SZJKfkNP5Rwp",
"1642ab9waFaxaeuL4r25XyLZhjUes6i9oQ",
"15V3WeCcv1R11GmbFC9PrYU8rWvpu2vBLY",
"19RnF81P9C7DxATRncqjQ4JqBG2Mb8beip",
"1L6Z5xMcazw7G4HNxgHZEgodXbJo2FgGwu",
"1DQuxeaNgXniuXVGBHFYa72GssL3tFKdBw",
"1E8GLzgrtVv925MSRMfqe8yebJKFZegxLV",
"1BLhk9hUP78gd5cZzQKcnKiapxMrTy4BdP",
"1H2YSBYAgGabHdXwDZuMdDx7pUsMpSXxdZ",
"1PGoweGhu3K1rA8Rn5ERTTzFH1hSkPAyd3",
"1PyGLAqeCoxr9tH8g1eV6wST1gcQbvxTyL",
"1MxFh19oeeWtT1mUskgCM4uEv5nJhqsszE",
"1JH9cEW5MFS7bUjAz9jhU1kuPfe4Ahd3Jk",
"1FhURLi92FHY5cue23namqXJnbtjNvtrti",
"148FvKsENvrrcfCpH9R9KLE5MPY2se5xTT",
"18mVPPRGD1kwkGgn1J7DLaLnFgqcpG5hHC",
"146gMCc8e8dUQHiFLTs7TNJtJZ6PTu6b3P",
"1JzdEDv3swuPau45PuM672mmdLfSsQrqwT",
"1PTL9xt3bsSzogdKTrBMVbNKAWvZcZJGox",
"12dFtBHiSWoS52RTmSY8QsWpjTBZAbY5U9",
"17VDU8BYwL8teHv8XK3kcLsoGHPwzcGVFV",
"1ENvEvLPpfa73rFoi8ZfC7BTnWPAQGURg4",
"1Z9aNsqBBm8Y1b9w8ihFt7VeDH5xeK3dm",
"1EEAXDHmR5EG9hD8CeiN6r6McJmv5tNsgh",
"12QJrhqRTa1FQymg2rr21CD512vuiyCabE",
"1MgvAzPbe9a1pfWVhsbPyQQLUW5NstJopC",
"1DSrr5bU33M2Pvt9ZA3acjoqDUCBME39n4",
"1AQExdTegCCm5THXvsy6r4R6x8zUWW8aMP",
"1Mtn6VrAjuCsL37rJWhieGeKALAi7tN3Bp",
"1Kx23iCiPp2agNsqS9dRJgNb48C79B8nxX",
"15uGrEJUNYWzAZjtguNzDctPN8cFjNfoKE",
"1D5aqnwwxePmoVxQhc8BHrtqf99XXtVQYh",
"1AMjEzfJG9ocprLJA3HeDRowf7Agttn2J9",
"1G14t9ZDQDEbBHkNMk4xbN5nDbAxG7eX4z",
"1KtA5CDDfaTcNe4XU6zAQ2zh2C7GmDXo3b",
"1FAQyVvktUvgjpiD8AW3MNtYjf4z6T1kgA",
"1Nodtd3u46KpCyTqjBjvWNapy8bt1wckmb",
"18MkrnbZVakUFRfC55hyrLqd5ywSjiXpQt",
"1ELjTtsPnpkDZoY2wHWL9WUCUeVE3TwDqv",
"1H9nXkMqYwYo34MhyYp4iH1DUac34iCaV6",
"1K6aUgkyKJe3uSuBDDhtzRqD5PNNLsmuYR",
"1EYVd1pUkKh8Ky3LF1czNLPcQ3zT921KSv",
"15o8xqheFJDLVa7GEfVWo8emRhnmrDhrQL",
"154qg9riVSriTMwe1n6nGNa3kn7NWxrmrx",
"15coNSE2WDJcsXurxnWCp1XvaxsvGeYH2q",
"1N3giNSjaSqsvp8qvq2BCkjk75fG9aKv5M",
"16kixGVrnWof37YwT8Qi3ZBxiad51CSXj8",
"1F11XfZs9CW3WZ1FCNdVTzqUwCNq1bZS8s",
"1HZEZDycKNhczYQGtys7cVCNHibswEH3Mx",
"13Kb4p9QxLaEhSc6hER4VY7o4yWXo5zfZG",
"12EfjKbq1WFeFhhYpmozjtngUUBE7AYjpr",
"18cP5kG4cmq19rhbLENUqSfEibaKg9LZvv",
"153XoPChadZcBS3iJPbFXxvyvNzgxAvi4a",
"1H5VJcHXbVyX3XReGMDWFiKW6rPcrTtJ1r",
"19FwtAvTknoyoJ8FGEAhHNtZkD6Ma1xAxW",
"12FusWdCUNrp8DZ4p33h6vtLhiY57tuSe1",
"1PWvnhiuUBXa8NwbFeEVPf5GLW6kT1DsXc",
"1GaUqRnao2PvY2RBQJVVpzxwR7VEmHMqJJ",
"12DopERfszF6MNbfHQcuSsHSExj3wgMMjU",
"1FG7s6N9TDb56nSxpa6UEcW4nZ8WuBVsPV",
"1ACAC55DGt3UYNTP5tXCD2AgtiphEHRamv",
"1AAVfK3VsHVTvAwQBBdEqfrkmi6fG6Z3s8",
"1MXwb4bQzTU5QDxdvwV4kGi85Q9vskJkaj",
"1ADDLa5eBXULd7q51SKQwxd7TXnHLbr8N5",
"17yH3pGbgTHdthWprusQfJKDtyoHiMvqtv",
"1KNF741Kw8nfLEopWSk7XXszCFav4ryXfX",
"1JxwqGgnn5YVSAqkWquhADb1kQutN2UN9K",
"14ZK7Pqj4TRH6Vq6pMYARypDgc6CYPddsD",
"1HqGUmXez82eDaFVW4uS1Nd6ZfmwcqFnqK",
"1AR8E4GtzaDimhcmAwXhYgyhF5AWmKcKHU",
"1NoxwTt1KZhdcsGaMGMP6BrE6WKK28vPSi",
"1LigYAVWEQ1t1nerbyd83xqu7TLyZcGvbP",
"1AYYmVpWFMuaHEM6YSu8XYWpLkKoH2B4kN",
"1DMruQGnLc1aS3qiX8jfxBxLgFSAq7vHFN",
"12vUeQPXaQiL1C3NmmjqwHe4tq7H6DJypj",
"133ZaWKEM2rZG4WYXoPg5hKnKjoBvAizWq",
"1xKgSXJVmtQrBeaawFpXtUJg5uPuMaUcq",
"1K3SJRygLAU1Ceqd4honbMgRdp9wbuy7P9",
"1FmvsdpzptZUNwcP91jYFY1BpVNfLxG27D",
"14jdtMzRyfxBAcy65QD7X6boYYaziZh8d9",
"1gybZzFiXWmhjoD298vLC7vLxHrXVF7VU",
"1BeTuB9w6YL59FsfgmW62hMmdoznyr5yZf",
"1LQdm18jLDdr2Gbpgjb4JQXbU2wMHTZT1Z",
"1N2ZAvjtvZ3P7Qf8Er1dp8DTz1sSKALwux",
"1KhRQXqgZAvVPbnkdhYUm61X25Z7XhimGM",
"1JaDt8vin5VLJA4j3LX6Fji74aoUDJdxj4",
"1gXF3dJtWvL7EoXo3TfxK1rdi8W4D3qXF",
"14HDemCH4NmKnR5Li4srtxKmrnGWGP48uP",
"19xfPrEi4HownaLpVyknM2X2dDqXrk7Pqc",
"1HHbijqJtSWqgKBhgBhLeDvEQfsHAGF7mm",
"1LC3vZdA59bmuCJWhLMFouSfdz1PTt58km",
"14CAaBKLqzLKidws3gSTnKEBAC33bSCKoy",
"1AgrbWnmxSRGkHE6Y3BqpfSEVYdqWa1KhK",
"1AE1vuLdCY3cXeWD9PMCXpXamA1EgYsdsP",
"16ENMeiWcrjAyQswGeJCaCXkJAR6G2NbmM",
"1JbzU8mk92YXPM94PFtFPUEvrsNJwkQMY",
"1PA2taLUkzrf2bA84n1tEA8F2HnK95cpbJ",
"1HTLakkur2SVLpCeobA35nccrcMgXeDKPh",
"1DSYcewRis76bEB4B7X6KAczGeNzh3tUG5",
"1Jc6y33giaDfA5b3adDHmfJ824Reff339b",
"1AbGiz8dMH6aSPBubrxBHHKSQXGLPsMPnS",
"13acX1UtGrUX2x7NoSmfqAGgH1DctPn96F",
"182Ze8iiCypjaZJCtaGwR8fbbXmXEwZiAP",
"14xfh1WNjRxgSYKKh4eEzqgb5f2M9N6JVr",
"1PMgreiTTcTxcssZXrAt7gaNM3vA9wJ97p",
"19GcR1L8oSk5WDekTiX2hv3GtLwH1sDoHF",
"1FL3GCDiWnVHhRinubKpneMkEwQGmsacc3",
"1BHPCfKzs4g6U5DcGFEek3xbihikKFWLUf",
"1ARAU1SfEWw9RnTeAZPXpE5khr494WXjj7",
"1Gf1FqyuEg57iayiosfcirx9W2p2tvj2No",
"16jajsxZhe7ErxyrN2yPuPHaUD3uJ4Qqdm",
"13Ew8BGJnnMBaW5GcGwDfWvxuuaPsRFopJ",
"1ZpNm6Ci6bq8ppZQXt6JkDGggCZ5HV6F1",
"1HJwkFq5aayMUU1Xy3ckxbZJftwiPp6JWb",
"1KsvDgW6ur6MDBPQ4cAiwLE4tSVaUUAQ3V",
"1A4peYooybCYiGeS7LKDLSDRkby2StyBE8",
"1N1PRWVJnygHoQAu6KqaFQe39jvBcf4wKu",
"13dLeHeCK2AMheM2QtYJP7ACSo6s7Vvqb6",
"1N3XqX1DVPk7RTbJRHQxyma4P8Bx654wGu",
"1GQQyPdV917LSZTYGKBvHLZ46T7oYeeDv3",
"15DSWQENkWDnLYNZuhXPLWuyDWhNkzrGyy",
"19vJqw4xe3hWCYLKiSBg1RHTy2CFMVHWV1",
"1N2JGj4omsQ3YcD29AwT3ZqRaDfXHBSjbs",
"195fSrMis5uuqsFAn7CCYWv4F4EG4W8C3A",
"18djCeKsZUuqRDLR3AVK4m5buUhmL9GftB",
"14UfJaJs6b9B1t6qQZcJ6JMk7RLAy4NCAm",
"15VLg9KNCCyzxdr73Mih7QScp8i3GnqSSJ",
"187C8zGp6Eq7ZgD16TY8QoymHSU5VaUjgX",
"1LhFQomSCMq1PGqcPvzY71h86FVNfQ46ER",
"1FvCTRMU4BwkWuZMZetkCFfD64gj4cHqne",
"1N6aE8ACoNnjsS7tMGNdTYkc1VfbuaWxNB",
"1PPa6w8xQEPAVGJADo8712uztnQJY5goCj",
"1DWiJXndoKWvfYu7QSFYsyijXrkoWyRYH",
"1ACsa81B33noTNS5WKLdobmWGrbWyC1CXA",
"1F5SrACb4uYrCLcPVhCHaFH42tK66StXf6",
"12zBmy7sxGXY1F3XQMDvukNNk4UmjT3RJT",
"1Bcgb4xjgGv98v45oUnwWpBpFcHH5j3MVR",
"1MMr6iYjSKZDJbKbMrzsC6hhkLoHeyeqj5",
"1DpJ9if1gAUyDVmYwRmsQSPqDFN9W2UB8V",
"1KzzYawDunWBhAH1tf9EDt3dyg57kRYXaL",
"12nLiBP3XhSvih41dWs6gQFew4tcubdVaJ",
"1Bc46eDZJtVYoVsRCTR9mSu6URjopXvTwX",
"19sgpfnB74ei262kRrHj6B73TZ5BQLgSfL",
"12xEFFjqcKo5nepb3YsvKLTE9pFkTFVqwq",
"1A26f6aYosUvAzsDcENNVQD28xph5CVafW",
"1ASj533oErtzuHfJvtdhf63ftgjMyYzBtd",
"13BAf9XNrYJLJxgGVcaBnHdJNnC4xodtCB",
"1HFNgSnwS3PtP7pvonCFPQzX7hdYZYfmRV",
"15WJkASprZR21TEtrA3A7Sqsb2tkJfuohK",
"1AHv6qJBTQk1yNDzLtGTy47ZXsRVSa5hM5",
"1DvPeCcAK3tovLo5XzwcsAUTVFNW6wnygg",
"1FEE8wyUzArzzXNsQiSUmAzKUEWQDanRo4",
"1MdhePfMTcPs1tJwZP1H2eGos12rq5fkb5",
"1GziCb53wCzVG3f8ikUZaZddaCXcpczFvC",
"1BAncW8xD7X2bH57pLMC6BFGfjozxri7oc",
"1Nehk6dCar2kSpVeEmhyCHo19SvNc6y9cS",
"18YfdtveQaLqrDV1aCbvuHJ75ChY4w1WTa",
"19jCw6dBX5ZZJVicmZ9Rsx18LRafAgfGjE",
"1QBjuhppV5qTXJmZWxtUU46aRQL55kMs92",
"12FmQUyqZFSSpJHZW8zcNdp8sgfcQXvK5t",
"1MDdhP5e9RAU4D7Fin4cESkT2yFMN4Wkcc",
"13YT16FQeQivVwhP7eDmgzev1JMTC8pXZ1",
"1DKj4cJ1cVtotSrTyUobiMRVNHjftumQ7n",
"1KtinCiFyUYzi5KYUKRaNENsQnwUbzTk4F",
"1Hnoc93VsotBpCXeXTP9bCJJfMtWFPS4Rc",
"1FcrTq7VsSDmPNKmrEiYJhpYgZvv3w6Mjs",
"16PmaAainSffGABA5FLo21sv3BBzUa3oxk",
"1JVT8pwCw8714MeqvNTNM4Yrqn85JvGBzX",
"19PRsyWe7pHBXGLezjuezdZwDsYyJYpD5s",
"1EYf7nV3iLHeaDue2yCv61hwK8tTXwmRrp",
"1MQWeg7uCbdpWih8rr66wvDaFM2yJMP85z",
"16wrPvdKE27QReowfnQbiiNgS2PDR6AvE5",
"1KBVWmK8gJXHLSWbLnSvntkTBG4Yd5Ufsx",
"1AJuXraTnEdvn9Yk7iDgGLaZAvmwZMAnJV",
"1J72PiuknDPdYpMJ4ZEt64eiXm9UVCodjF",
"1Q2NqQ3tPejPyJb1GZM8MzPvLtmcP95BP3",
"1HH6Gamw8qNKtqRaR6anSQTiHb6vnUSrTS",
"1FwfophXrgpZqTaQs8TY2DUVqPKJaLkxMe",
"1CsDKWF8hZ2rGpuzjcrDC5k8SrEorG9zH7",
"14cQyYE6JZxFMBQ4kf8zx2JwsxBkiPCBe1",
"1GtVVPa57g1vV6t3ss9aaXFULMfg3Y3pdT",
"1P9WD8qNnvb4gjEFAM1gvkbJspC6VNNhMa",
"1B14TifRYwnCeVaqAXQmBDqDwmCqL43FV",
"1DNxbcwLV2sFYgPYQ4N4CsXTJvpJZidXNw",
"1KJp7iJ4u6cbqFoBX3Q3JW4o1sv7QVn5Zw",
"1K93ZHt1tCrM7JXJY1TJ44JPigADibA9qq",
"17idkfopXPKUDcBXd8CdJJCfz16ezYfrTY",
"1AqLqf6wHgs2cAvXdwEvmtcNwDdss1ejGk",
"14KRiYL1JCiqemcvesi1C29JRMnKyTQW4v",
"1P7Jf9fuwy4Dv1HoFrHEXQZnpszDj4dk8V",
"1AkhM1WZsg9yD3prEZH91UJSSyHG9kHaqF",
"1MNNAc2P7xy6TgNhMWHKMPAFvd9zp72aVx",
"17MFuwKXSRNu3df3a42zhM6Bk6rY1NLXxo",
"1AG9nuJkTXwUcymHG7ui8vdQWU6Y7NGqpc",
"1DbBTTKFtzjiKwvLvqBminEdHRMFxGuY9M",
"18GS7PwJSxrCTUi7FxaqfeC8T3Qnc1mosq",
"1GLH7bvRsHRP8GSCWpxkCf9NbpkKubPWeM",
"12kUWamdvjcCaDZUCd5JoCoKnx4JHguaHv",
"1JDSQp2ePXYRzzbYeDDxaXe4noD7ThrEjN",
"13Zr8fLgm1kqhnhY41UYEwUMVQtadpDf7X",
"194vc6t6aj3Xm2nhpPMRn7Pkxkmd5bRNtE",
"145MkhwDFtNhE2awQgU4nv9erQcy8yonWE",
"15ScpQbGzEStyoa7s1kdZhqzxCP44b9JcF",
"1K9KB3DkhEKhAF4ZZEUa9YVirzdfK6x6Mt",
"14Rop9qYh9yTCTF5sEYNXKjCUiBpHTaQRd",
"1Pqf8qAQmsinqPbVNRXutDTWvRYtahiarW",
"14NZ9CbeSspMNP6xu838zGTJhALVRix42L",
"1PcmFsopsHK3xXqh2Xi5o28tYt67jmQavc",
"1FvtVqeAxxpX4dmCMqsz48FmpKWG6neKsv",
"1CozvLFU1nagrKQeLeKWEQR24hAsnPGiLF",
"1HqZ1Nz6Y73uhVHLoZep3M5UESUjQrehKs",
"18fzh42ucpsxduQXyTTpUA6mZYw7xHYKWt",
"1DC6JifBKhyVootNguQmqjWZWeN6SyEjJy",
"13pESQ48fzVFiPDyL8My9MwaM8XC5CisQD",
"1PxkCfkeEEeSUENxifmzC5np9mBWNtNWxX",
"1DoCPD28M7YSCYm1KuxYWxb71FfvQrau1m",
"1KAZ5rYmApK7AGMYzHABe11z2pwopYcsfL",
"1NDNeWSPfvnzAM9Z9uNYK1gEwSBUL7EVYP",
"145LfhLNpDK3TXVj26Xob7T6DDgbYAxRdP",
"178HPjaegub1KBCm1avNrcMaNenQcnkajv",
"18MHy6ezJjWXp49UrgHqCAP4gkUDbW7BLo",
"129UeBeAFHYpeTDp7DhN2VhHDoA5oDMnbH",
"1J9s2bS32sBn7EnV2F4fPaMjKaY24KpGvw",
"1Bf6YiW5M6Xv4AdeAuWLwKjNPnB3XBLCLC",
"189JFi2FmYvusXSkTKZkwzBWBuUVHpikLe",
"1L6PTAJtxA2hgij5czGjKYtJvqHxhuimy6",
"1PVQkAvyEyhF2j4WWEQH6Uheahf2LJLZ9f",
"1Ftme2PNuCUMqmGA4BBFLzzNLiE8TwZUSd",
"1Pn5y1Q9iJJ1C2bdQJHjbem9NddvvUBRZF",
"1AzBnfFNbUQeFPkDjn9c6orPSW3uqBU9PY",
"1GSMxEa6b4AuEdKZChsEoqQU2ZLiBQumbD",
"1PzJ2D4a9Soed1MjUF2ap3YnfpJZd3zgZi",
"1JY6yPfvZfF6URFEP9e5psgtNad5st3ptb",
"1JU9ELD7UyYeUxgBANqk8cgMxAP4cpBqhK",
"1Cxq1o13ePohd1qxF2ZKnYZN8KFnPXfcCn",
"18FozCSvQjnMoG7HdxPebatQ5vbjvZVUFi",
"1LM37NwasP84TEhR88vqyWHRcBUGHFpc9W",
"1E2yDWwPQZMoWoABV7REHGGfERiRxtpM2d",
"1AkyJvZWWhZnySKujCRGnDnrQYtN6FA8gC",
"1GsDE5c7rQgbZjxjdU62FGvUsDqkGtaGCw",
"1AVf3tX6w6DqHhyvZL79UUxaN6jXiVpMy9",
"1Ek23jxTHV3Z3vGw2Z1QkBbrbNvWT5oHvn",
"1P6LHxTDi784GCxM1YkFaKT4pCn4S3suTr",
"13N1Hrac7pbK8eGzvrQeF1Dc7JjQGnnBvP",
"1AYi81dKgYUKVcEv2LGteKeMTNJ9hG7woY",
"1K2afhjo7UmVxNnEhNbKadVHBKTFC83Zqr",
"19ci7hd2ZmYMLYoAchNGPe7m1rhLSzEA4k",
"16QVHSAaF45eix4wGHaebxDx5rzBYZTqvX",
"1AmftNYJv1K3tbsfZh1tjFNC2pG7Rofqcj",
"1MDyTh6aMGY6uqjAqSzLGxW31QZK28w3yt",
"1AJBLdmuVEGtvAEFYN9m6ANSPzHC9vVhnE",
"1pxrEsxpCkN8ewZvNPEV4pFTzykCCzmaH",
"13LJgs86d6gGfhxrHivuko5HjpkJS5gPd3",
"1Hffj1h3dkvAxZWfEiae3PrCHtkQvSCE5",
"1AkAsiK6H7ifqobEJMFf3K2651aTh9JeRA",
"129j7HwMACQi56zPurVaerPxEejPAzXQ2Q",
"1Do8mtZ6KDvpzVz49eCE4Z4CZ7JDZ4ynEK",
"1E4HZuxegUj1xc3K2j3cCNBFa6feW3qy3m",
"1EtwM6j2JQPuxRt71Uh9uCw5WS7Qb3r3Po",
"1DqmyzJsBvrWAwRdsbdSJ1hjRhfERLmakk",
"13nfAPD3m9B2xykvfWZiTx51X8JpVEuHzF",
"1DUq9cmyLHSpPy4c7W4u1ipsCckZeX8YvF",
"14zwyGJ6VoG9niq63ToGsN9uvQ3MANfgFP",
"1Cjg5xnbcqmuWQJdGXDH7f4TRGQHobhS22",
"1EYc8TrHAsaWxUHRve5mf93XE1yM5h9N4A",
"1GtnbqjqvbdLnrPMcYGAS5WEPrGGMsXNum",
"1FnwWAUVXqm7TYqHyPYRPmaFC9F2kypxVf",
"14nGeDBN3oFAGNXmW5wFrzALMtwvKn3c47",
"16PrMcj7CfzQ9nJWpirDPAhXhFC7jAmGi8",
"1PsQrChfwCA8UWLBhV6o3NCjh5saiQbm6L",
"19nGtFXoZwZ5zywvHLMif1cmNRMrYNXbxV",
"1AQgUXyfAdgqkqen82LK5euFmJLvqfPXNr",
"1pJTJvqsqJcTF4zUDkMgWvJzRbBZiJ8Ap",
"1wGncLBHFaEc8XMGkzFYEcwQYLS3df4su",
"18gMbJYZFTLRXFXwCPJKMR4bF4aQeDYZ1A",
"1PNnfBSs2KrCd2jeKaMCBfb3t7BE2rteLA",
"1pi1ttTEuARELBGM5A8yRw7SqnUCaatFe",
"1TbZPtZCVWE2pK7U2J9hpE9BR3Qy91ymS",
"13vZsT3H128xgn7Efe2CPBTtCkBwxr1PnN",
"1C5JjgGDG8468yxa51fBhv8k6gy4ygVung",
"1EHSi8NpJdSTVqjVJry7wuut2Cjk5TqQ9i",
"17eGGAvtDAmrj1x5W9KyCUeEbbnCFL6Sbw",
"1PENdnwS1heqPr3EjFGubBukozvjwG7GSe",
"1N7wQFyJq6RcWWmyq8qwHEQHZQ9ZiynuQr",
"15HDTvTFVF2iGGHyHa9StRsCycam4tMK1a",
"1FJuhibCsP3VPx5GsLcexRLhMBboz99ZtK",
"142kkYK1ZuBx2QnjS2TtbWnze5JnUufjYJ",
"1G64AqpgHZkqX3o8mQ59HFrtsS6RaxHy37",
"112pmEQY8bSh21JWBpcMiHBETzFSrcwP9r",
"1KR4DHG4kDfkHuRG7X31qKHjF5jac9RSUg",
"13xKzeHNZEq5MoEYXaLMk3LKmNousi6ae6",
"1L1H5d5UyqJxUdjQa8X3vuTr2JSLfhnXfd",
"16RVU9eohgRkYKLcAKPPKrfe9kfPUmYwLs",
"18xkza14Z6ZTKatu95i9de2LFYWrwk1sSo",
"1A4o6nXfJ7jeW3mjX9j3ZuNs8SMMMgtood",
"1Pzs61envuMDtCJiLqA9BqJUzgAbVeDv2u",
"17ydbNhekrHEuLvMvQKx91nw3KV3Ejy1sx",
"1ELHe4CLbHorzGMfVdVC7wUUddJaMNEEMw",
"1LayijMpXikh464N9yrkkAUN1Vn4uapbbD",
"17bvdQhBdx8gGedhRvkzKXkcsP9fBqfzi",
"1NwApFr3HkM14HatZ6dUruyCg3Jnrctfbq",
"13YCtZi3i7zKNMUC4fALTutRPXwTsLwcSr",
"1FF2LVPZJFBLK9dF5SAVJYNHhgDbDJXb9n",
"1PxqHGLsskKgBmCUodogKkSxZfqUbksMjN",
"13LB3DnGqq87KdTa2HemTDHoQgT4vbXcJb",
"1MzDXUf7bzxQu1RTEXyVH881k4Hwib2Cng",
"1Eev1evLQ9SCCgbjZ6PKgPeCQ1QkPw8ANb",
"1BRU76p4hL968aik3nAuG1D2iLaWf8nn9K",
"1JCuhJRbjftJUDedsvjgpay7wN2WgLnh6w",
"1H7xLJXcLtaVjoM5v9nN2TE2ExVD9hf9sS",
"1DUGYEkkoeg8rYuFe9PJmTbc5nX7Uab8m3",
"15xSaREQM1NyxM3qPCy1tu4U16TpAcGYPi",
"13mGeYmkBPC8phkbF9mFbN8DbjvHLtCpZr",
"17LGoC9KZi2MWrzLm947c7KyV439r5bJi5",
"18uE9Pugvgkaea14ohzwUdXyAahkug6XqC",
"1KJBrAjHhVyCC5LqoLA5zaHu4BfgBrsnbX",
"12ptHSUKCVN2hteExVnqChvp3HiiDQRHEB",
"1HKBsBFLBAYEYnrmdsL6zuDxHh3jt7dFAe",
"1M49eQo7iygdCvaDraHWXgbpNvuicx7VtF",
"167WGyrbatDP69HkxjXZtSWYJ5Xhn7cYra",
"1KWZieVSFery3tAn3D1eHLDsBg1PMXGeKw",
"1QJ6Rn6LpJirALsGno6kKEUtJLtSdJFRyG",
"1PduLNMa8s3UyJ8SRQy1SNKjjrw3mYNcjP",
"1CLkFTFuqwMjq6ZLenazuFVPMcBnscUvqr",
"1GWKnKbsRAv4PKtN5Yq3HHQumWSr6ujoL7",
"1DvSjj7ej8WJ64RMwqLfyKkYLfSJ8CprfD",
"186VaGaUFnhMkokqGh9V5WG451SYkn9pRv",
"144uuLwfTX2SHtKyFdgzUypHX7d8RwebKi",
"1DRuuP2Jn8Az3685QBwVAz85zhuYwWS9Hq",
"1JzmMpDWC6bbCXUGaNtxjB9tNVwNhrChty",
"1DEPN3kRoB1ToiUCSdxNbUVY7viDjEDLT1",
"1Q9n2W1FLA8dsX4z1CwiFbdu4Gt6fbL9df",
"1AzXU8zRZgsA1N49DNCE3rjioWWGTYrAGu",
"1HzwJmtbdg3o1renmu7qGMaZA13caUoRDa",
"1DuiYQffP2A4111b6itXVZF4gnmf1X28v4",
"1B7uLiJDpeUeYctUHHzQcfqYmNft4F7K2P",
"1K9s7DXB2QRo7YyKscyC8kjuMz72v1Tgvz",
"1ASpfF4poYw4zPHD33Mhp6nKJVxWZG1ju8",
"19x5SLduKy4tzb1DuXaPu5kizoe9coGnpA",
"1xbC4eMpke3rAASdXbM9559Pb8hoR7BZa",
"1LC9wWweqmeeha2ebAtRU8876CH6D1tB9M",
"17fbPhK5C9WfYHCokxwJpPPbYW3S6oLwNg",
"1HdJ1z1weAWdFUW2EA4tRwi9St9E9TjZ6n",
"16NX7cihScvim4bVJjWndEQ8axzLPkNFGB",
"1JcAz81isFYzcLtRXULstYspFt8VYgUDeK",
"1NM2T8fFyPbNNFNy9WdLKMvQau6TYSvSQZ",
"1Q1e9WUXogzgXUmgtS7ChLbvH2C3JG8tk6",
"124vFbogKhrLjTki5U1kDwyzSrL1wmMbxQ",
"1BDLHSuRHEmJgmPKa1qDgChewAvfSNSkRQ",
"1CSUAF4qLsUKhn4WcjVrffpCFNEP8L6o6p",
"16BvkRrRSKTheJKHJAyqjTSCH8A6mG7jZN",
"19AkBmo3FBhfEMin2HqRBaRZaqmJN9fT9F",
"1Bke8x1UaednqtmUD5fPVK4oJ7RxBzgUc6",
"14bDohg3kspYp6erqDULD37DawF7b2sdjs",
"1Mo47xFmrZakp3Qq2LmTztS8PVSaWMvgAq",
"17qSmTdaLdDwqEfqConfyhnAnBYhdN63Rn",
"15zr6zPUV1MJMpCLeQj9jntcuEaY3B8Eer",
"1KgCD8WdfgNYCysMnGK8zWB6prSMUcHxV4",
"1KP1grYmV1oKm94swxr3roobqWLdDMftJ8",
"1MLhVdmYHo7YMqJ7vy25imudo5MeLAnVq",
"1B2BURoMLAdmErSqtJu15vzW8MUcWJRaKQ",
"1DocaJh67iV8QrgZdsU1UPPctQe1pBxLRP",
"1CFV4KXhGLBCCHrf2r6JgNs8ZFDJKQqrky",
"1Gm2SFcRFHoep7Sr2rgFoyQm1grFQswZMK",
"1PAro84mv6jeb18HWjdFQUgsjP1hQRgHpF",
"1CCvksE82qiz5tUnUghRP6oWK5EHeo6sjb",
"1GrnVqtqNVsM2qjVv7pkKZnfQNwLEa3DCn",
"1MWhtwYSwYXEdZFrco41xSRicoz72acSvS",
"1K8cnv1DXic5xCyCSYaCAA84NQByBpSwdV",
"1EhnMyAgzz4ta1v4NatPqCmNxjUgGKJRps",
"1Me7fmS3MxQ5VdF6C1sJveQcocdqT54zCS",
"13HskQDhw1Gy5eZCPfLqR5AuQBKVehQLEJ",
"1F3qAjYxmEwk8Y1dSyPYURh1R7yBEMXt4R",
"1PYg1DZzgAQwoeMPqf5hpgw2hMm3enwmw5",
"1CTmi5G94msBusQpo7iL7FQjm5WW92XCFM",
"1wMpLUqPb5LMuCD8YEENWXDVnnhbq1gne",
"15x9wuumKdV4oE38DcBjUmpGQBjrE1hXRf",
"171YS37BvgnZq4ecXxBfwcZ6aEjUgCB4jp",
"1JypMVnSXM22mZo1S5SZyGN8XvqguCA6tW",
"13EVirUy8thiLbgVBwpzqaZwXA57Kg3Pxs",
"17YNmNPk9TUpqA7oHQUvZSCzzXkdykwxwh",
"1CjvjyEPvNt28DyNuqR6mDzXspxvxHz9iZ",
"17QRVWSLJhjwK1gHRhFgBsdwtBS3VSsutN",
"1725FxWyv88q6FBujkRxwUmHvgHiUwrgz4",
"1Jk7nb4eUeEjwHFTw6fHA98RrWbAQBF8HV",
"19SEdZHa2wmVqkqSfqiXeNdbFdKhJL5zwL",
"1G5cGMg4hEmdXVshmgSbUihYbrm3C7zGTu",
"17GnodK4YUArjW5xNHDVuEihddDEWna59z",
"1E8yocoJhCcaRvKSUpAUVj2y3wsUi8Qv2W",
"1JBZVtzzG2WGcaUGgDgaE7mT4JykZQezEP",
"1G118vdVt3hfdK5ayM2PNUgWUSjazn8zsy",
"154wEJWkBR5jPGJt5PawWJMsLcQEk3FQs5",
"1SaHwhtb2jcj3UZ42F75dS2ywfLZh8JtD",
"16qSzvzQDNirRGsua5espsG75L2Jg7KwKK",
"1PYWKohzuN941TsuHA6Tpau5GnGPosB5Jy",
"182fBoSmL7dE13AbsfJ74BSE9mHKAgTpga",
"1VV3vMBnY9Ktw8Fp3JtbzJTCS3UWBXVxk",
"1MH2Hup6GNsZXr7o9FFEwDh6pPQ1F66RDQ",
"1GWPcDPNS5Mg5uWPFy9D1gy5xfBWPpXbJ8",
"1DDeG34AzLERm6vSd3DXBNg7byheRSisNd",
"1HKf2HHz7rdaUC2ToSDkxmAcRefEJG9vHM",
"1AiXDsTLQkPMV9Scg16R7rw9mzrLrLJGmZ",
"19iP3wZbs6HsKe1o5ucqWceq9qmSYiJWfp",
"15bcVCSheJG2FVMd6nDweyAkZy8gj4fHnz",
"1GLJsRnbLAA6jdx8pjBbNSLDCD4QngHTMk",
"1NHu5F5XQcYgUUXP91zTTma5ueCmuUJphB",
"1LFCqYiDAaf7oUbCDPWtG8PXA9T1GgK6WA",
"1Q6W3jZRe2R2rHqmHi9pVzs8BadNukcin5",
"185zfTUKMS4dPoNLL24NDEtj2dhwUb6DeF",
"1Pnu3GFxDWmBgfFpqKTwBr4AjbnPam4EoD",
"1LUChxY7RcYmDMPqZkjRTSPtgNVHvmSm3z",
"13Ww1NEQucRoNeYR6cSEWvQEftVCFVKJ7f",
"1LjMFVzYv7BGmw5Gk5Chw8LxXqVQnyX5fb",
"1KPEr1UcbPcszeRe66d6XhmK4UH5JdE2pm",
"1NLvzo1ij34XB1W1p7DJPhjfhFjTVKZnJm",
"1LqHAWXggTgrWhzuuuPrzi3p7WJajyA1fP",
"1AUg79wmrRrohgJsywg7sTuJgmTGsBWSu3",
"16cuahq7xuZs31fAVXF45hbMQi3LFtSp8A",
"1HeBv8KJD6jzESPqdnK28vCuArMr99N4MZ",
"18AFAy7xARUHU8pvGP5K3my3E7jTyZA2Vn",
"17dMd8b96BBxYWFP429WghHL7fqw3rbVsE",
"1h2dDWABE6mWcRfGT3XbTToDz5DxRNmtN",
"1Nuwo6NKPYfK4egBUrWpWexjYWTecsZiM6",
"12d7zR9hxQXLXa1PoEsmH88iDykXixuBRY",
"1KxmsCW4nVrVTa1pD3gAi2hpxWmX2VjnF9",
"1MiDksQRvQiNsSdokVv7kM78jj9XcrP1vu",
"1PWWqovjqiJeCZJByp8PRuB5PMV5i1C13c",
"1GPxzm66TJkZ8Lzs8Cr8BXAgzdn9k7CDDR",
"1GBgJQAtjd6uanHt3LEpix8EFzoiRQVZ6K",
"12JGK8n1Waj8JfwDqLDK8VkQ1K1N88pFwn",
"1PLyycvwwEt2nFajzmMQPtFrs8ZnckbTtv",
"1M8xPpKH44T1BrcWCsiYBQDwpdyjKdi33u",
"1Kqr8Y6bXj4VghxzG3tZiJZQU8UYATtku4",
"167RK4ipnvePMbDeonHxc3dahHGY99uET9",
"1Cko1XvHLdUsvC3bKyAaQ2E11cnraSFfFk",
"1BKtSytJ2Ju5wAgNJqLJfaG153UoUQCZ73",
"1CDiLBtNWbv99KLUzjPxuGX2hzF6cdswUb",
"1G77viMt3GYUxcGVyP3PcA4nSXfBn2qeLd",
"14p29fSeC5k9wTcLDLYaw68tLQkfea5Q8U",
"1HLeHazKYRUhjCKitW6fJL9TT5u4gGS2Hj",
"194T2uNrpKVmhogVfgqFjotkeHXQ8Nvdtu",
"18RPkrdVSdLSbPkBJ6gAncxzPmbjTu8yZ3",
"12EDY5rKeP3pJ4zG6f6rhXtDenXkiUHzzd",
"13Cp48YJEXdYZqaMiCfzpCsYZH9ozEPr9e",
"1Am6sJi1mASs6eGvqK4jYEueChknLxMAS7",
"15fpe8G49N6gdAhhwW7ZXpqPP6xJYh8ijV",
"17FAREGSz8Vn31Zozu4VppJ4f3xNskZLFN",
"1scwLnVQDDYu1z4gycJCA5pa4fXZ22kNg",
"1HhUwEd9opBFpV4HctrRL7eaLTqquT6rZE",
"1M5ACEJo6wZMHERmqUV84Pqs8acZ8XZa1k",
"1LNbY7Y3vYZceEBaYyVcQ9w8VvR1CpGMGy",
"14fXNkLcvDxrvibdbhGCbQxhcr6CYhjrvQ",
"14DCNrM4EepZtc1B2JhfR3tGD1FYL6xFGs",
"1AL39aRZbvqNKTctQtw6CNv5kbLjAUtPuL",
"1KU7RgdxDqNSax8MFtAEYctjem6oAWZUgY",
"1FeTsuKYsV1TkixCjq3QxHUHpeF7oxaDZD",
"1BG7HuKC4TCQL5q17DKyTc5ctFp5TznxMP",
"1F8GNCkCN2pguLaLeC9FMMNrEYKuh1JLCv",
"1BNTNnHT7b44wmjpyh9mo5bY73XHWTnBnC",
"1FXYyCLaVTGg1s4LpGBWuFqFKs8uwd5gAb",
"18LGCqXjPueMviv1FYtZMr6AnHQfJu8PrE",
"14xPf1kHq5jC26ZBHw8S4epZyPSMLKhXiG",
"1CNQmMSk9LJMrV7DVCbsV3881ERFVn22vg",
"1KQEU7CkYg4uU9b2hBQckt9LxBLFxHCyyA",
"1NFhFbv5XrQzDfw741EySsDFL3heawF5pn",
"1J5UBFjUHTxxshW4LDD8dWrcCxp96puonA",
"1JsZpd1EsU6rGh5cyT7hGa1Gt2tfpnhYgG",
"1BRWWKbz43zcVwq8sbFE7JKFtXCAk8bvHa",
"1An7MS61Q23Gq4LqE3DPckQWi3nS99Vtbm",
"1ByBTkSC8LKYHSQ3raTwt2G9Z1ERnS5s9w",
"1FbMpTZiMuGLPL8EYQkJDtze4Vct4v3B4c",
"1DGxhDZsHb18sq4heqDiWxksF2eHuxsSEZ",
"184UddUsMgE4dhzAfJe9WApezPukyvoiRG",
"1LDyY6GfVqrjHimrHXQwh88NndC3AknwoA",
"1AnhMtn4VGJiNpbLFehUnn1oHqT1ZGCco9",
"14CF76cXH5uhn8S5gYATg6b6xcHkwYi48K",
"1JvpatnG2QSNYCGhQCydaZPcK4oNCEC59K",
"1JPBbNj3vYXfp2BcqMFAsygT7bALp1QaPp",
"1KQZZ4pMxNYpJU4xCRneNZUS2w15jShf6x",
"15mZFW18WUynfswccfGSqdtGqGtsAza56S",
"1C1LCFEwswjd4ZsT4MbDqe3RC5AShu2BhG",
"16kSFxs8R2vdREPPvfzhnpA7JTmh6uB2j5",
"16CYyjJsCiPG73R5SuEfEDgk1Ka6nsNkRy",
"12dWmTjrmAanB9Ge6KZ3b4KWv9Csk73Tz8",
"1Bw89EPVmW1BrBn7unWM9CE5yeB48wAH3q",
"1F9aM8L3QemwoguYnytyFqVGwQt1ZnBFYk",
"1MDxkNs1vLHagF88xHiiMtcsmiv2GCX2B9",
"1GcRfJfAN7CrZsiohCovQsTkmusDe53HCy",
"159q5GSsyTSRyvQrUQ7MULREdFhfEj2qPU",
"139F9yHbpHk79o8prPcBZynwiFpmm8ht5B",
"1RPDpDTtMhq2vShJgKJCkUiRe3r4qAA8o",
"1MpMXHsDybUvNo7hHY95NHoTJFbHo8C2HH",
"1ACR9a5HxSeZSottjdf5u9apQzEuv4Xr69",
"16gjHSMSZCBHonbbbF9VSzGyvYpMFdo6jh",
"1LGTi9igjNmDPbaEijdpPdgHYieuTYLycK",
"1HxwJhJCEPxhwFboKjG1N7A7qqN4spiJKJ",
"1GK6eRqbU7rDG32J1wKwd3M9NFYoBXmpx",
"1hCZRtbGdcfhY1eVvnrQHhg6kGiHku8Nk",
"1LbAA4ggSEfk1TzsrybY77AnrBFnsQKu9C",
"167gFpErbDEMUHaTviivHiQXpnZ6Gg1fMe",
"1C54wNkGP68V9ecVsmhVJU1g2taZLHLBdR",
"1EdXJNrZMaYjZzifhKdCtgwGkumttChCN4",
"1B1ZF7gQKQmWRGGQtJqJaS2HZNX1yB5i1J",
"1P1eqWtWZrDTegPYbCMY6ubP8rwdbHqeU",
"1ELQdaPM1vvWjnBD7nxbSU1AeNX4Adi2zp",
"18bAyVFgCnqx7JUX6kT6CmM96XRiKo6Rs2",
"1NMK15dao4N1hSKJ2Fp5X7TC8QDNw8EAVY",
"1E7BvM4pFDYv5HMz7KY4y6ujT4h2FZzPjY",
"14BkFima3kpDivwdLN5unVxW965AZrqq4N",
"1Q2x3HFM7q42juCbdq1jse8SLjKFhmAy9w",
"14si6VHye1Sj2iT8XJtpvHnHih6ZvkPoR5",
"1669vvuJJ7SMbXrod8LqEZSXpv2ywfm9CB",
"1DzNN2TXUsoLfnt6qDzKF2x6a8WSrr6cB",
"14JDo9jJrEuqn9RGGcviLjQVs1ziMoLeD9",
"1EGtpJsyrfjupyYh1gernr925M72dSvqoV",
"14XX3R8xmjjSUdv6H4dGv8CAFFKkkFxdve",
"19uCBsXbjbgt8fbhX6gSJcV1qThvvTGE2w",
"1Bdjpho9z4JVcnC4omZhteugoWjQZadUEv",
"1Fzniro1t6aLVkJA2m2ksGDVUhSd8hSP5x",
"1Nb5ARxVxhup1FPL6is73Rjj2SvWb2FwVg",
"1BUKVyzEXWX7B1xBoxAfzwj2g3mQJFrs2s",
"18G9tJgVc7tLfoAyVs29wsCZSXodd6171V",
"1A143BHzwZASi8DPtfxCGQqUmBTN61wVDu",
"1AYhSV2BfrkLg8NESyp8UL1mPfpmnwTqph",
"1N9AkR7YxqGFDV7M38EmUjVDT1RnxXHYSq",
"1FFduUuQgYdNfJvcnRDdDCpZByskmZnEvK",
"18GqWxmhZYUX7owCtytwnK1ppV3NYsWRMZ",
"1NPtYuTy1d9RGP9sG1WPZogsvYcUqCPASR",
"17ESQ4mKS9fnTj355mLFJNDdRbJtFd8dsh",
"1AUnfqijGtkNLnCny3EtgydDadSUWbyGNv",
"1LMftEoAzxQVyENZnJtZVEmePVzJNx3tD1",
"12Z6C5n5kMUukRAJoijrkNBkpjtSTFVRpe",
"1GbkXFi9pfLmMnn6B9kDyuQG8dAS5ZPJdG",
"1Jp9mGmwSC5dkBjqW1PXtj6VUPx67jshbw",
"1CHWJ9sP56QFgwAh3FoTiLTPmhLb64Zorh",
"15nMcrRNtZmvCWbDBkku5JwjfDSLkBU9Vs",
"16xWsN4a6WJHHNMSHX2qKNM7mhyQvCS6Vq",
"13gTjs6keYFPJcL7xRno5hHXG5A64NNKcR",
"1AvGEpSrERAnJcRohpFwexKTby9DmVmP2h",
"1KV3jTFekHCfuoRN9k66W6trbPvpbkmFS1",
"1HgqAfE3yZXMbUWkMjBpNACvaBuC4PR78e",
"1CEwdTHZL4dMs1MMecCTXQLHBww6oCmG66",
"14DCtMiV5WQQQjHXp85bBEmf5bZUZ6ft2R",
"14pcxq1YG3AdvcR6QxTweY9A5hUnnh8ibU",
"1NVwfsnnA9Hi72mT3ThP9eFS1EqNJ8Xhb",
"1Ke7cTuMqr3PDVYbmtMgy5NsKR4ac9DGGX",
"1BFCEdu3Xq1C2JgWULfnhXrEQHEhFkcNEG",
"1LLp9hSoT8BtuFtmD3dADd3apxsmtKwSox",
"1Hgc842uXC1eDPy4ZGb2h9cHbBQLk4NGY6",
"15GjoHEb3BgJq6tRggdc3CZY84YLYFtN9a",
"1Mg7ZdiPTcTUZcpiQoihkX17uprgoQnXSc",
"12G7UHe9KqP3JGUVGq5d5XWasqgECtK82P",
"1GEWuCVACT6dvJGJCyzjxou8KLHajJ5h29",
"1NDZogHeaijq67Cs1FMkZyQSi4QZfq4d8C",
"1PXEujPVA7K7BZYvU6RuCY1Pdbk6rWmT9k",
"1DhF8YSNFcW289ANdBesMdwLX37qWZqoSW",
"155bDU6KmTx1qrukFj3qSYDuaimuSaT8d7",
"14QdvgZfYgohSzADgTBbKwQf4YdeeTu1Uh",
"1KSegNmjDbedvi7dvhw8ffzC854BgUmJLP",
"1MjT4r1GpxNEVgsP4onR1cvTUjvGBTU8dR",
"162sWj8NhWMJKy3JZ6uqoJoYYjpQntgbP2",
"1ADATBceMmusG3nKTYPwLYt4EwZAHpJtDo",
"1B1LTNYNEonBdUXwGmit3veZ8Dgqj9P4zK",
"1CsF4gUWrfJPaR9Mic6Ni6f6CWc4HLzRq4",
"14ad97MyeSst3tQQV5ePgEiCp6a9rD63Js",
"14K5pvTpp2rn5LoERQAPDn7PyARApFeFL5",
"1HSBmdTwqqf9zUv9FQDozioch9RaQdCv9k",
"1MULZ5KVDZt3BurkdDtVF82nnQBv6JYs19",
"13agHckey9URE6GoFEcYCvLuMX1YV2HjFG",
"1Xw6tShxcyDGYozTdCRTfTbg6smu7vkFz",
"1NMaUSj43zYFWmD3bi8BXayP89bebHrKWX",
"19U58HvnZmazDhA6cjubcqinVp3vARpkvz",
"1AJNeCdRorqAL5Hvj2oz9m4tcDEvik2iiS",
"14bz96oHdAdJewej73LuvDaZLC2MLWU9Ww",
"18e8yNcCRTKtHKXWuwaVAZmuBNuV1KNwaJ",
"1JZLS2LJqRWKDGm328wemCrGnSqsjKMMwv",
"1G2VmGQjTBSTw76opTpVBxb3xNa8ccLQBJ",
"1EbNFKKTAWBkPscUCjeCxPN9hHsRwZS8kS",
"157rS3vAJfKch2NEAijpNs5CYusMXBenfe",
"16Xrvn6FhFMTY12sXVaEHDKN1aunTZGC2t",
"1HLF7PtEQ5nCZmS5unMUj54JowhVJNuk3e",
"1H22BK9DCRukiq5iPNXLhog2ZS75iBEeeX",
"1HKu6AK4LUgrhqH5AHwQMVEz6F2SArDJoW",
"1AvETq36g4eA3JStN7PM79NVJVKsEyd7A9",
"16iqBi4kN6RARoBM9oL73ABagNA2F3cMBe",
"15QoNXwn5pGhJjFWMKryQGi75KYu49Yydo",
"18V2rge4yzZJh21q7c2w8kejjCPX7dtWAa",
"1HbEnS7HcVHjxgQ8aphr2SZt4CztpLUWHX",
"1G8WVHZVYz4w5R8DPjpmHjpXhYTPAC6EQ5",
"1CwAQtf3JNUVrzzhoe7XmeUCGamEDQrbmh",
"14Uktjbts3oNJCs7ipLV3VLR6rRB9E3Byy",
"1Czs97UWq5dw5tfPmpiKvBVsCPB3qdKwWf",
"1PaeJtSDvdGZ5fL3z9T8QXxZuYmPW1Hf2n",
"1KK5pfiJ4JDbMVNLVxQHUijCNMQhd4kDcR",
"13pL6RFpj9HZ33qAyyYRVukF3E3Urz1AV3",
"13dSjHSueCkL7ArEDLQZLBV8J6c5BWwsYd",
"1CyEkrbmGAhrdK2tJvFxtjgFy1d94Vefz1",
"17LASt81Vv1d4VaKYxhQFDvASokNJhxgoP",
"13QgxYvtCY8mZMrG2AJEgQg8crx6dbx8kB",
"17cZ9S9hqBJ5KdcJnN1dNQt73pkcCshWLC",
"1BQsaY4XDFcZxtPGd5LEHPTMJHJbTC2XgC",
"1F7KR2Ckn2D8fcMAukQhVCUjXc8NchJfue",
"13cpBHy8TpZ2rdtpcoATDe1etvsXm6K6gh",
"1E4eF1eGmvz4JUNboiYNsDDDMmpY5zvd9d",
"17XeTE8rHNNKoELgvpQeLRUFFcnAj5iPnu",
"19P1phWnjQJXvyBJK7nPTLRBCkFRxCyPnC",
"1NCCyAGgwKFMB44nQK1vKqkYRMSyzmDnUa",
"13p6JkaoyeKZAgnYup5ktdeaPRVj4Wjthe",
"17iveQ1vXCR445sdcvzm4yEwC7T3SXJ9K9",
"1DWi2vkjRLZq357T43bxwaXYUs8EiqVxYb",
"17HpEdkmT2qYbh5mCYYhABm2tWuKQhc3R9",
"1EwsEwN2KtjDumnerngoYYni8jLG51EZmU",
"17sG5szUq9nbiQNXqjkWQ5uWqPChdN8SSo",
"1DbSSVZ8tkYDAUvRPj3HLV7qvwYDZe3J8b",
"18mTbxQsNzdhbd9C5GBM1Bpb45pQBSqusA",
"15VWR9W7AVCRyUvHTssvehych2Nxavguss",
"1Hz9juRPS9euBvXDkQSg5S1w8M35Pr3bgS",
"18j4TnGSVfJEjAWGuviGafL3Fcsrqvj6PR",
"18TYUzobZseCAAr4WuaHjgoB9bwZrNd3FH",
"17heDRsRTxwtw6MMtkC7f9CzyYVYVN511k",
"1PabXix7EYZxHVHv2gpvgEikYz889MQTVa",
"1DqdDsatD2mmy8zDdS1z5YHZrNxhXvdSR",
"12cgrw9AEXsheGwY5kstnXqnMg1JSDYN4D",
"14KYZtbgX5qtBmAKgNkQfYDoyY7yjEFpNs",
"1H7hZchn6Zy6GxGcDBKB5fXv4L6vGTfJwg",
"16cMNc6UEL8rvbS2YR2iGwFt1kLnnQ3fJ6",
"1NhMx2KZLv4nxfguychcmj7etp8uXh4kWQ",
"122kATTXVoMQauN3fdNpkw3pZGPWkSUtPA",
"19zCJYUmWPAEZcD8SFT5vrLprEgDUdeRfM",
"138yaXeUHRYwtjcJJN15xJtkmHZHUsB6E3",
"1FdrU6mU6JwQMiJdmMJSbvpBv6r5t75vWH",
"12SxYwsZH6Atvqny6StQHg7UNEEuVrj7iA",
"1Q6j1v81AQGGvPXc5vrvZtp5YemtJUXMZx",
"16tsVykcNbq1zKKd9GKT8LED1NALtzDfWG",
"1QG2KVbXvCBe1gLKKg3dT6hx8FqVGS5i5b",
"1LuvJhXURyyPAHSiteKL41BnFdAg443wD6",
"1QBqUyKKMrp1sP2g66uiBLFgHGcKsBjuQc",
"13WhCa3mZ63StRCqQyKUjrVGDBC2rQdXSe",
"14MQqrsppLKLJcL169TcpoMyjw9EWpg1hV",
"1KrigBBFuoVjLw482n7QWx1YGiXNtJzNNr",
"1GsijdTFgbASNhKkg9cVmKoJeGR4MHMXj5",
"1FFWX4jMNtRwc4eYojw8KpR4xYP12yneqm",
"1NT18FgJDpSLdKksA5GY6gquB545Unt5y8",
"1E5Ldg3pkzKkMiStm7AQr3aamqeLdL19qd",
"1BjWjY1cHfmCnTgVgk4M3vR3uuARjH7WKN",
"1GVej6uiyt5rXn9MuQ44rw9Xe1uXBeRmyH",
"19ASi9RtBKp4BXCv59KpH5hbHUmpYieZwa",
"1G2y7mF9EAsvM2FEmyzitAXPNtAUr3VvpN",
"1Ljfq2ELXQT1LMKArw8jPjjGqce5BziEKN",
"1NbAPAjj2iahc3eq9jGueSuxzMtGAepuG6",
"1H69RDmUA8abv19i4kmDBy97Nv9zHwsjMD",
"13YX94BQBfsdjeLzM3awtySJdEqWZwdH3p",
"1nNEZpEoB5iwwEyjpqkFPtzpXCCKUintx",
"1JLKwveDB2LqvsFjjyD1W64dvw6xezymst",
"1KSKcrTRpNe2fDRTryV3n3NFyy19ZrQ3K3",
"1LXYob1Ced7poxMth8cutGz63PhVfNkk6s",
"1945RvrnduPzq5F8QBzeLsh6BRBHzdLWnc",
"1Mra9zG2FkMiJTEN5mJyN3tWSh1Mx86qbu",
"187uXQtU6HkykdGcESn3fbfLbNW8o7jhLu",
"1GS8j8LsX5AB7i4m7FKqSE6Cp7LvGH2CMA",
"1EjurWLnrXbG9Gw6iRMX28c2Vcdd6DheCr",
"1EaECFZpnPtb5nCsQagFVuREuREP3sB8u9",
"1J4EzsyAXFJ2eojTZEc5Qm89DJctdN66Fp",
"1AcU3P6pRmEcBii5Bg7AUXZJVtiMW5Q3TA",
"139Ca26gwTPwTyqv4ckH4jdx6Dct6giyXv",
"18Pa2kHUrDh5SFxtktjWg7pi42hzuXJTfk",
"16539CpWgHsnXfGpVFmz11w3pZRZZUYPCZ",
"1EBbjA2uNr8FiWXfkydPoAMgjLWdecvZ4b",
"1ASKxs43ZuJBUUVXhxTEJ5sSvtGHRny7fx",
"131521sDVQjAnHbSqXrr7sujyJugee15R2",
"1ApVrH55W2yrws2bjSUzZq66WdYpFcHvBw",
"19QZKBuoTcrN3dsVbpFiHs6kYpha8K3zyx",
"1McdBjVnG7kcQPH3QoPSxZ54UtVFM4AQUk",
"1BUPo3UPi7koTEADz2sH3Qw7taFtTwhovZ",
"1GtgF8geWwYjKybbELENE1EFFqiSiWFf1m",
"14q9YJEvAHqVFsxidy4fCQeibGunEirwvx",
"1CPhR2p1peGuy3LytXTS1xwS9426m6iJXX",
"1Mrzn83zPzyrxoEhLTzTyKjNeMnfQYAqje",
"13o3dHDKfsvs14TXtbpfhWxFUAKxf8aWSk",
"1C6EyXduBcdPfJ8UN65rgT9Q1npgPTsyLE",
"15qCWCbwoeEf3GNm62trHVcK8pVuoozV64",
"19LjYWdAQBWADwU2pAFAv6boLJ3ceGZb6Q",
"1FL25BvMMxsEuD5rFYpLe2fFqb2qSVXNQk",
"1G1RsekFFe39MCka5Z1U8HxgRrmDw9S248",
"1HR95KRK6kYshzS4iMLyzbZxx2XoieBczx",
"17ZPGPpwhaPApPMLBkZk64JLgMk8DiQAM8",
"17sN2PEQo9QrqN7WPgpbG11ZnUuPpbzX54",
"1HJR4rN1Efd3FDBtcCHWXWpcxSzCvVtHsD",
"1GBZZexCW5mKyQmgmKr1Jae3vix9gRLfeZ",
"1FTqwwUhW4VFFvN8FiKV2EDtrKR2giqYPz",
"17dZ9aBBYXyHbQ6k5JFasr6o3wCtvopuTo",
"1DrkH8Nzf1g3Ks1VB1ZUnyG35u5ctcZPti",
"1CMuZffpnSdC8HP4SrMSMXKSMpxrtyV7nD",
"1LrXB3h3p8ESSvUf4YCgdhhCqRmXEYitj8",
"13iiRRvaZe29pGMauHYBy7MhdGzTnK19fP",
"1EkNyDJ4Yaqr3qEY9iS3vFRqfCw763nngf",
"18gGVcHRWQkww2u8nxvHTk4w3LhD6mfiK8",
"1NJ6rN7qc9XVEtrGkGBGsHMEyW83pS5xpJ",
"1D7gLNSUhMMvwh7VoT31zuJgrdVLYUXFGQ",
"1PFSvGUjX5xkCjeqDKRU3gEJnHCJjVSKw5",
"1B7wNqGv6J613xiywkaKHDP9uBvfpj7con",
"1BrAw1Ujt3ggBxjhTingNtJGXjw5dHTeBw",
"1DSM7kYUNwk6K7zNG4hJ7bat9tUMq3P6YP",
"1LGuVvbNdGnsY6gVhR5sUA34i8yZXfcN6a",
"1SekA9YKdpM3qL64DPX5f4erwJ9DYNq63",
"159FQUaFHg15yXEhoPbdRkZtcrUxaMJivj",
"1CgeopRp1MG1AG7WqVPg6XQWBAWAyYaJVY",
"1DuH5uqdMErn8hizB3xCmmRjYz2JA7hZcU",
"1K4tqLbAosTi7C8WcjGLQ4YXBMEkFwGUVB",
"1NK6qdJ2uZTj1s13oAKeF2rm1YqQxfnCtM",
"18akBv2Q3Tmb26qzTMJLw8cYQ7M5fu7Dwz",
"12w6dYgtLBfR96ffRqpYsmQWUBRJjkrBJc",
"15LAPnNqKWp7quAFDYtRfa1uN9XiyBdUAd",
"1NsHiTcQEUz1tD9x8jp7ZTPh2z2z7UqD2t",
"1AEVzCVWQNzF9M97EbH7tZueLjFbhrvciS",
"1EriEprCP7VRwy25U2uHBH8abcDXgizVW7",
"1FS9JsgfFb7A7wJbwbN6qoUrZDuFCsjZPo",
"1JbUCE4nseirXYxvSfp5KBXjbkHgBtyDTA",
"1Pf86n7gFmPKz3QeFwxo4DE8HPohwKZ9JX",
"1GhZLZepGZSmeDUUDwbjLZEAV1eu3WGFfk",
"1uPyVGtiVytiadxjUW2MYUV1wPYZEDTbh",
"1CvzU8TV8kjzsdtEV3NYPnvpdBWZqbjXPT",
"1ALyvaayQjTRxRxVZLD4w94Fo4QSak1rHp",
"1F2CNdijL459JzZkX2iF2rroSjGF9RHXU5",
"1Fq86gB8rFB6m8uCPoA6HhuZUYgKZnXU3B",
"186VXDEyjSgQV23PtnqeHv67MewRLg1EJ8",
"1CET7wxAxh3ibSJPpBdTLWgb9qvh6rYvQt",
"15SKQrPn8NK1jKtFwVU2c1kn7CqCRv8K6J",
"17YcfYVoYtUQ7C5T47M1XaCX11mRDP2gHm",
"1GMHzzufqMokfHUkLVkXu2Bs4hWhrzMsHe",
"1DUe5Ue4a9gvKnKQdCuvoSWixah2p9rkwE",
"144bYmxybYWxTQyZ5FGbdqw77i9gMApdQH",
"16fgwEkF2WkypMy8eGh2g6N58wT913FnoV",
"1ANZF7rr33YvYdhz67dU8k25HfiYrPz9ec",
"1Q6A1Lh5HD5U8yDFoqCmwjR4KUmHiFYM8N",
"1B7PVF7NBUPcWbK3ERzdVjDbrv2X7ie9Sf",
"143KbBVqkjDbPdjNH3UPDMCXeFrP5TmVgY",
"141Ra6fEDnEmS2JoSVY9283FpMtNJ6ELVy",
"17n8TsnuH8NZVGvVj2Dc9ZphkoNPKsyYfQ",
"1DjzsvBMnf9MsjZ6N7uumjGeWcYnjaraAy",
"1Hjubq9e7CRozAWPRRWBQAJM6gyLiitn6y",
"18gHAubbFs5BwP1mKyh4PbNXrL7yxufRcK",
"1D9MwRpnGzL32NwHPeNq7oYrp2PGphQGAj",
"1MAA6f2d6FKiDSghyrexbnrGzM1Djt8Yqo",
"1MbTVPHXGEV1FJKCvxYnHfwnajVb3kwowT",
"1CVzoNHoNNdYa6HYhzKr9989inzDYaguTT",
"1JQXuG9WvxBbc1rXn7VShjorKWq8nhZBWK",
"14Q768URjMgKqiFmcDWgGwZeoittx7ewyp",
"1AToxVCrJCtXsAGxAu4B2dZUmtAqaLJN5X",
"1Q4aCeZNnfgsYdZU8zy2WuKnbBK4gvWPLM",
"1Ke28aVDVKYGShkZW5QMSJ7BL6nLYdRuR1",
"1HKcrNKzdY9Bc5cxpBxWFdMd3n7r8551qT",
"1MdBuiE2gio4ar8hNnhhh8LUp5yURzUcGH",
"1CvrCXcRWcGPnHgY3QKfyCWoTrzjeXMsLE",
"1QK1GeurpA9DWNtsFVJawePyXSyJPJKGq3",
"1NYZMZHqrwkh5KfjnMhYaeyFdcVEHZUKAA",
"1BSvsfqBRcrzXRawPueBzhWT5pY3xmzuMc",
"1PM6Y1Vec4qj7kXJX9T5hd5dwCyVrV8yci",
"12MztpzCoahES3P2QvQNSRmd6KhT1svZdW",
"1NAZcwksPyncXQK5hCUZr5HcSETuaKkqc4",
"1P8dEFcCvUSkvAp7yVGYfRN38mkgsZGgFm",
"1GJU9am2KSbTGrCW1i75BWb5upN81A697E",
"15skrZE2pVB5ZgtKQMCLWGQnLenBw4Meo6",
"1NPHd8SecxXGkbvc5wdK3qDpUT7gWTVD61",
"1KquugoicEvE3864uxUuFPiJxdPGf2xE2G",
"1MwxHwZKxuVDMqQk1gfT2oaRghPUqZoHE7",
"14zbb59Pd3qLuc839LuNh4trdn3RYXuAW3",
"1Aef13ZmxxaA4DF94hVfkReUj2pBwBqcQS",
"1CaH5MYaxUhU5erJFhGbCpaeJAnQqoYCvQ",
"1LfzVAZCagexGdWeqWaV3LoPqkst6RoxT5",
"1Hkdm71qqoXY8F2d8DefjrwfzJd9XVVcFU",
"1GhatUQqHqY2fgE5hjqV5R14GAmNXpxox9",
"17gJEA9mA4X4ziqvhENYaVPHgSLEGxacxh",
"14EeeXv3wttkZXU3XEqC89K8d6bVUXnozc",
"19tyufdw8pnSvG9BsirMpcjG5asB8HgK1T",
"12jwvZ5wXwH2eJZ3yLkCcDsVCF39o8S2Ly",
"1HFQRmeizoXQoSJE5CJVRCf8zwgpatYXvo",
"1C1t1Y6gFGkZstY1NsR77iEY61Jjq75hxq",
"1Ax2tkYYY4xEWzd82QYUZZ421nbEW7QvK7",
"1Af6RDaLxjih3bju7p9W28bCtbK291SkeZ",
"15e8zVFxeP2kFqVVEAsuALkP9CAn5Hn3ZW",
"14Vjdnc1BtWvFd4k8uAago8hVzxzs5qg9Z",
"1Bbu1Cy42xzMwcVPWA6iuDANN4gEYM5ozT",
"12JUFFfGNah8ErmwiDysMmaD64wcVjVAKH",
"1ExCuE2MRGzhA5PnsAXDdvJJJKJwU6XjKz",
"1MyWjir4SZJCU3p4Yau8gRPshSkxtLSrZ2",
"1KC8HbCw9Vwuz7HxTdSTWnKyZtgb5yaiBm",
"161ZjQaHt6vceWuNx83Jur2LkqrC2muoAo",
"1LAy6Tneix9pL1oHr47VVH8Hxp5yFhsJ86",
"1FpTZ6Lfv38sMUKx6aZiwKSGDtQM2XyKPs",
"1AnuCUpDtULiUnqyjA3tfDo2jscPzdw1vt",
"1GTerSvHJYqpVLnPSwd24amty7UZu8TnB7",
"1CdP4Q5ktbuRE6nan74LC8ivdryZr3SzS7",
"1C378QhGzpiPbFyvnJwn5MPtnwg57h9T6g",
"1EnpR4xexzWVuXQAji389kc8j8igR3afrQ",
"1NDA9Sb2ngoqVKvCtzdCcQNmfY11JquJ1L",
"1BdfZJDF1uNxg6ovMBxTdWgP6JnfNRbEBg",
"1LG8X1rQZaAwGvNTzC1EgYZoUSSp7N6wC3",
"19X4nA4uz2PiAo6aD8KPmcaTfMuD8igJue",
"1FdBDFQuBFYVns2Y2f9owbyb7WF8sqmPhG",
"1CgiigYqvAAdJqA5H3Aezs6waoZHZuT2og",
"1BCSQWUd6NxXGMuYnCNzanszi4Kz5TUsGy",
"12DqD4de8P12qUGZpYu3JopNf7h7TFeNKA",
"1Hms2KhrC2Uvkdkeysg8XZvcL7b3XwLErP",
"1KuhGqNS4vJw38ZHk6kV8K3bKRY3mZ7PM5",
"1KEdM1D2rweV3xuFKQBa7sakigUrAuHBFd",
"1PY1ZCPvBjWhhqkKixHvZVmEm1GX8EV6bV",
"12yRmsLvQGbjTEdeCVmtpMSH2BxjVHzTVv",
"1JGWUMSMWfgp3ZvL7NPtnYXLVmxG6Qh5NP",
"16k64BmedPubM1QamvzR3eUS5AxxJHUHh6",
"14LSF1XK8vi8AH5oRZVcLvcHDNSYnSq8X2",
"1FjbxoABKhkLdyRwLXr5bfSTBAnppjHfNF",
"1Pd8pqbUMQoD1qcG4bXEomDkaxTXGx6P7j",
"1NmV8BBvCMNgLk3uE4vi9EYdC7qRujsDu6",
"1HtkEkjgYsbDyeSrKQKWXfWaPtvSEmGBbc",
"17A6imAd2wWW6kEPy73fhbj64KdU4764Y9",
"1DMqNbW41Q8aRoX8ae1sDdwVXGR4DD3vf2",
"14T7etZBTbwAQxrLTcRMxF9f6f383ttfkL",
"1FVVXLiYVbZDNBKyzrUFNm5fNKgjiqcvaj",
"1Gk5jxT8Xfe2uHBdfaY5wMQR6X6bjGRdwH",
"1DtrHqgeDdDrS8bMAhkrDBsSZeaBMMG1ay",
"1GmGvkYfJrL57YtNyQW8zou9BU2JmjkGyd",
"1HMejKhzC7fErwbbqLRrQJ3NyzcYWQ4BMR",
"1FU7DQQvNimqaUFYGbqh65K6Gjn3vLpDPm",
"16SvVBv27yQ7KJAmeMfhq5uj1zJAZhZYPz",
"1C9gvUkn8J2Ei6sKbUtLy7d3ZrcCFTD51A",
"1Lr4tZ5kf1HcL8fEUqHfKW2J1peuoaNija",
"1DpGufHU3DpfmyfvBcs7AxssWRs4iSwwns",
"14HMG3fxbC55kjh4vLMnwgirTAUKZccUWZ",
"17mVW3seGPeaAT396MNkakXv6f3bmZhoCQ",
"114XeAvrVJN8LUohkspG2BNzydycjxohXA",
"15PepZ5VN3gAeydQgLPqSmSkgNWecnCcjs",
"1QAfXAqu9YWyxoMcyZVCMrgAKhapUrQToX",
"1A5GPC5fMDbZ1GpNCj8RKfg8BKM4ZshG25",
"1DAbPZvU8TaSCurDnsPpKaXiA8qHnd6ei5",
"1MA1DCeCqsDBraFaQFhDEcTeSQbtD6EEWP",
"16dgURhcWP12ZtGbhPASr9zYcHaNaBmpUe",
"1PzrfY6wyNpzLbz4esgYbfe197upJH4ofa",
"1BkCTT1vCJhZc6tNrrpe6Ed453GTsVCB13",
"1Nu9ZswV5uPdZthTvrZQeGGt5WAhXqnPd2",
"13TabDhqJnssdctrmu4atyGDcNYuc8UQWF",
"1GqARARkbx8o6x9kSXDyhdU1XvJbvhWoW2",
"1DhreYk8Cndj25SMAEnB1C7yYPwGdpttGu",
"13x7g4ukyc3FxJYxwj3gb3ZYn26DrwJcwK",
"1Efz7eAx85DxGmCWuPTcYHSwBJ4TCo2LNF",
"136KQajaDmuMVHxDTUqxUM2VdruqGwpBVD",
"1LpfSgVjr76EnqKDGadLzqDsTwLWiLY6gz",
"1MRCS1UP4NeJfN3D7yEqY4kLJUiv588SdA",
"1FJHx4JBcKfgBJShgDubpc4HHdpY8VR7mr",
"1LY3t4uVREcmDYe5c2x8RedDtPh1XBLzy8",
"197oMVXEewHsbJbjJSkHvb5BqGZ6nUaFpj",
"1BgYFTvAQSPpucL2gNKLsBfVvtCpjWWezL",
"1itr2ZAqekQUKy9zDjg8zcEnhdRvpJq3T",
"1Ddr5w1GkahzVEtZgidUKNdHWfMHAoWjkQ",
"1D7ABJ19MttBSs8SpiHTG7nBGBYHE2BagA",
"18LuS3rLdN7TmPxMELR78BLy9AGRb4fRjL",
"1CAnkr5oyVLUbarTcPBP2mp8Kpew28fbRf",
"1Kvq2GXf9roEkUfDLgGVx4s9j9CA63zSaJ",
"1LWK9yWWKieKWUg4nqgwVCDFqei1mPc2Zm",
"12eknEdpxTFPpZ9gdeVvSGJeFDoQYdf8Wq",
"1K2PWaKxksneRTdRBfU3rndh16BBnb8on6",
"1PJrBN4vBEFrGVsjK2jzkp4XXxZvm4Qvxp",
"17RhuBTMhCoaeaNuc47vQTSSNE9W8tZ7ca",
"15JsWAedqJdMDwEs6dYvSpg7fHM89xuaKJ",
"1NHHcmskM8eFpe6oRsZHBokQXbgi1cxJD4",
"15LwJx6cFgN8zjq64YJdp4C9poGgEnwoHL",
"14NsJupoXvxZumweAUk3qbSAKkSVJE18HE",
"1FMmvUjz33AU9tz1BSeWDGg6FRPJppUp5J",
"17w7YtSmyKUnN9zLnNRXpb7UGHtWEGGh1k",
"1AxCPvFBt3trMNULBpnbcHLFDKapEQxANM",
"1J9BtdEVJSkqxNBYrxFmTChHdgtXL2gWqm",
"15EjLAcNjSDaa6tzq9Vq2w4qkxspjAYiUh",
"1PdNCQFyapJX8kM8rRhhwRK6CyDa9WyARn",
"19JB8cL5ET9kSzSSGTyPT46ZXeMYM6WYyu",
"14PGDCccZ7hWNZAwkCUgR1Vw7a8KBvx7Wu",
"1HY4FF1tXAxjPctetimnNbReEBkfaQPArB",
"16yoJaE9MFyTbr2QTK6Bv9QEouWs37VuAi",
"15yBuU7mHNReHho9caoVUYVvfdkrUxYqvC",
"1HiQaR5jzMEvtn285T9GpfLyUr6GtcStUJ",
"1NLDf5dhhxAEnQybZjdBYFRSckmeEe5NHs",
"1AJVQnWUdignbNqxLjbbnxkrmj6Y2UQU4M",
"1qKQw7gf9unHZzorPDvoJJdq6BxCinGkp",
"1BipUvQkxowxqhUBXbmrjoY7VVdiCy8jDh",
"1DpRYkdPv3ViwVqTsYRnWmwqujrqGeMFFg",
"1HzfEBLA5JSLxv5dTVTgJKBiGN1HqozE7n",
"1FWgsLhvyFHY5Wb6jXTvZ1xQqwFKNfiaR9",
"13U1GBJyrzFpre2szKP7SboEsvAPWf3aWz",
"14CgrBCL84KSAJTHntAQTdjLNnuPKx8WRc",
"1Mor8Z2XTrDyMFzwwbvwpDx5RjEXE19KTk",
"18WuT6YPbTpVr8ic9eXjzqjsyTZS5HZeAc",
"19fzkKTzTtzm8qwLygJoF61G8NfEiK1qpA",
"1H9otkqc5U95e1rD7ic3fTfSZzG1SrF7kW",
"1HNM8QRYcp7z1aZf9HbmQrfzWgmTpae7MV",
"1JYzs3E5VA6t5TGZUvHJWRcbCEMjwn7K1m",
"1NNntNm2eVAoZzCBx1vKoC35LXoWN5mGhU",
"1CpVEYX8XLi4LAocMRYDP6wu4XggYpJ7t6",
"132999z4jA5e8TKAtmbg7J6dop4MSAnojZ",
"1McpjsqKzfW7GzQ66z1AZvZhC9zFFrTk1K",
"1Q5mbGqX8S4wgTSZX7haJH8GaKXdvVHpVK",
"1Js1eNxLo6EFKSkqaLqJkYziAaA7JLYtXo",
"1LDUYgC7Ed44v58miUcUH16UcgVGBBtNoc",
"1D1YxJjbv1EybbZBtGmR6MrBwjjY8grVW",
"1AHpu8HZQ8BZ11Rxpf2bviLLZJWAg8ixvp",
"1GaJ5J7DEyvgcibTu95CzHoPqqdicGxgen",
"18rSJG88LVd7Wo8pbuVAWGzx3ZEwfkAdfp",
"1Jx1E2AxnoFHgDAZJZuuF9whxrrGHUBQMf",
"1NavP5q9BnhuWgGRJC8YVCR9n9Jd1VPU8p",
"14KXa3GSwhUCNrmiag2XHi9MQ9rs8rp3fE",
"1AVUZXpsWV94PEfrF4y2wRwRijAZnDpBra",
"1PWEP9LPaDUJUqKReyxTYk5TA9dC8VQ5YD",
"1Mxckp9Qj6RyiW1rmputyihoRE5EZMRQ3j",
"15qEZPQZUwkhCkDS83LUJbKtUnxZwmD4eW",
"1JFjLMEgb6jC7PUYKjvCwQDNw4Vu35WvAd",
"12XPeaqZFyLbDGMkjsDzy7xUAgNvsXCeFZ",
"1BmmtmYpn6D5te8FibKf3RCD538ZXuPZNi",
"1LUJHftextkxVGMvN1YWejhUaqkzouvfhE",
"1Cxpbqs9LZse7GmjKCoubyrAk9WVm8uJMN",
"16dEtnDtxo91y5VV7yEc5HsUu37DC34avK",
"18dvgEmRYmizR15Nj2e9XjGqVk31x6nMLD",
"18UXrQ28WYnpmCiPjRBEnpxbMRfdjvE2kb",
"12Dxvy4FAgHMxPU4tafaEcvy9PNxwJwKZ8",
"121BzHUHwRusPB75a8F2kng73g8QWR3hG4",
"15m94VtyQ8Kvh9d6PnSWzvdopWBFekLnD5",
"1KH2UprSMvcJV4itKurAkmL3nnQmmjyyXG",
"19m67xFWGf71xVL8Szqy99wcexCXkcJzVn",
"1CKKqM98pefArzDt6Lf8JGJUDW4X7PwKDY",
"1KXVvrp4WKCbVZ3nMydJQ9EAtXstERywtE",
"1NuFEfe8iCh7zBpK2X63efbsi5mJp6Z9Et",
"169wS99xniV9nrXEYkfcZY6icBBFZe1rDj",
"1BQgUYW7tSha856gRsUuLCA2Q6eFMMm5wQ",
"1Mm67DutjpLdLfaMZgGepc313ZJccx3jSW",
"1JBhkRk4QBRHzj817gKkwm3SHcqZdFKLsQ",
"1GotWRxYLD9Xyc4MZYHkXxNaiWMVwwd1ax",
"13hfBX8ezQwfpGMdFuE2ciAaNBhTPpK2c2",
"194G9TXZFAe6yX6kh9nHia5CRAcQA4aHZT",
"1LhoEUMPwsy4je5LvntsaAPJqv96sJTyFT",
"114bAJWP8oe7YzVWY4mSfenFKu6gJTt9ve",
"1EAJDLsHSC7s7yDRraHX8s9CCGb6yZbrXQ",
"14FKjPko3pGvbV611hYcwNBVFTjGAW3Pxw",
"18KPWYFWRV1g3imdweLrZop9ZG24XuJdvd",
"145t2Ysz7y8MgnhSic3G96q3Aq3MB4CbXK",
"1PZGFXMn4bE2DMk7ku5pFEiM8wGoueGLD7",
"13jo7HjmMvfqmSSMCbHSMZsLXb4dhJrrtg",
"1CU9LgArERc2F6XE2giVL1y8sepcQUb42C",
"1PcA16tgcD9GpiN6DdBkvhsqkPYzRo8G2x",
"1KWLfAnDL2XQbxgjqctuv7tNMqPcVq2NRD",
"1JBoL5dc79CMfc3v1a8NJ4CTxjyKWaW7P7",
"16s4u6ktZHnEdmPR37q3w5AvJA32M9g2CT",
"1KDWQLG7iykuhovHrVMDPqYej8JVATHh9E",
"19p69NNXpZgyRz3xpfh4mRfrUYMNKuf19d",
"1Q8Y8u5xqDCFb6kE7J8gcbyUeWpokN99Bn",
"1MytYjtnLSL8Lyq78cUh8AwNheaMAFyCuL",
"1JQr7PzTtBrHZJ1aV9SiMu7SX4LktAhky7",
"12JkkzxDTLHBbtynhgsXFb4iNx3TLBiUTj",
"1BESxB4UD6TJ9iCi2qVMB88gXwbp7LHYPn",
"1E4zYiT3g5D5TEaiUiu5AfZ3Q8a6uiBv2j",
"1FeEbpx1qrCeHkRQsWN3Yisx6LDBzYfCXz",
"18TZMNikCMV8Z3CYKV9ksvgr3fHMapocxX",
"1ACbkCj5efnTPJRAEpNJP333XisVzQWqBi",
"17C7WDrUsF7csZ1gT8c4PxCvPVdENXjGeK",
"1Lvp5zDcsVc8Y74KxYjXfSzrQFXmhfVkXQ",
"1HeYKCNiAAZSMxAdNBcY1r1Dwcuu1t4GRh",
"13sy1fkKFuwMmhrPeuU2TsPXNm1bLrggY5",
"1KQmTg9oUKSMHjGoPfnCEe2JhVE9WTShLm",
"15p3e92vJcwVC33qQ8dCD5EY3SwT4HCbzE",
"1FzcT7wrtf4B1kvE1C8hdqqtpX7QHgz3Ed",
"1GfPQJg91qg4gMniaHkoXpfsDADz8v1SFr",
"13ynftR19pTgX9ymFZdwFMsXXg8Jw6ZkjW",
"1JcgsNaYfLT3LgRNLC3KdHZsfjDVYvsrqT",
"1CpCBbJMHPhKF3tP2jK7NseKW22UdE5PVm",
"1F9HKYn5J3f1MnxRobDU98y7QDTCoVvmDw",
"1CDyiBeSLCbgbsDL6GuwXgD5r1KtPNawZg",
"1EuyBgx6FXH47y1aLrw5fo2gRGXC2VfFMb",
"1NqwZXNjth1t51SVESBmzBgKynsW8f1c1a",
"18KmVL6EaHCYKtd3Ef8esDQ7RhccvQQv2",
"17wPDARBNfok4jZjEg5vbej7ypML2ABymp",
"1ETAHnJcHYfAmQcPNLj9MUtruagm4PtAUe",
"1H8syGhyZ6JBgLFsd2dsGSppHxLDiSVnYT",
"1EqeQ6jhS7zLpHhk6cwHg4TyMwjFmfdvF3",
"1D84pry7DmZkpfSxrTVrGjH2cfszDEXeVM",
"1F15k17WKsu4jXkijivWbzKmNgGDbMwsGs",
"1KQs7X5nd7sQxWbmiGQrHiacRj3syHDCHA",
"14AkFsD7v1snrQuVkdneWzp3myKcaMeoDp",
"1BWRwgiHYXunsU7AxSYsbyKe7KGN8g5aAp",
"18CG7iLjvPESDFuUuENK5cuZcVFh7rnaVF",
"12gQ747QLXR9xRx8xdpFBi5d9xuKiJpDkZ",
"1L5BEXH1fWzyPzvky5FB9db3ecmuKgT2tB",
"14WcUKegBHLNT8p4RQ2iRpcw5uiDmnfp4U",
"1FW2eLUuiD22Xwp9meNbfp4uHDekqWtojs",
"1MGuyaRvjJKA97Xi6Zjg6XJUA3XDF9Kuzp",
"1GucRRMHmVjdwqkHAzzB935biU9TecMfxX",
"1M7ceT9tbUUQM8zdQWXLUH2UdJEpNLN7m",
"1K6SKmhjhjxMRdL3VxM49286E37StCWkcL",
"1G39VH58hNr3hcbYrAp8K4cpuoKD8kfU1Q",
"1EXjSWo4Aqfxx7m3kzUUqBe6xx6ZoDL5jw",
"1PBuwNFYRgQa8Ecqa1bVBDNrfQbafV9cV7",
"1FZgAk3MKi76Stu1NE8cNw78v6dbcMXjGt",
"1AYG4F49iRfNiuEoDxD3FozSBMFEsGE9VE",
"1Dv7dQZZJubkWkXhQx6eYZV8tNdvt2JcyV",
"1FLu75vA48vcqAurNnBdwYJuvZCuCz3j6k",
"1GBZiDvSPkRW1YZh35vyHabPAzqTEPV9gn",
"1GmAekGmUjZg9P3k168qt3x4azq9juDT7W",
"12pLTnFRqeArNqc26h9jKKj2CuVYHHE3Hp",
"13ZQFFTWJjLSNr5WNS9rkYo25dxpofW1BQ",
"1bDqKW7yAPnJ7qDsU8F9oSJ15Ydj7yBKr",
"1H5FBsQtBChtEyb2xd5V98h8issye72WNG",
"1EHUcfBYu2C6S5RD9mLvvnGsx3rWswzmaP",
"1M692zTMtQCKPQK7VXAdnr2wsZaTU9Cg8C",
"1PoGPwATcGE1uW7uY5CvPxjWZJQMTwc2eZ",
"1LqbfaE4XP9As1jXDebu3TfiRqe5cRNVbq",
"1MzQVU6MkT3x8gswPNiwprBHJ1YbvGjQm2",
"16nEJDKm9m6FYAGuCv8NRLkhV4qwV9oyfq",
"1Kv1kBbALvcfVGjkUK1kL2Uvsf986eZc22",
"1PkVfcVXJjStTjnNieDxagk7woHuM125tr",
"1K3Lz5776iHxdtczbFH9eBc9b5YbYTqNkH",
"1CDo9YBJBtjWRB8CbywCUHbbUDs18CtW6k",
"1DDDZb2pJppjTgw9Bitbf5S9bnpr7FpjGm",
"1Ky5r8shuYWwyrLzWuJb3xpQcFMmvqiSFo",
"1GJd6aeeeE4gf7LRPH636Fns2XD9Qcbonw",
"1NqyZ7GE9RBhUj9UuoYb6e3y2eMnLuGB4K",
"1HvjsRusavCfkNsLZ5omme4dUciydVNgoh",
"1JG3jsPsu2nZLg2zMSqsNQHRSij3rs3mvt",
"1Lxwi1MqKzZphs4LRzB26wQ3jYuLPRriq9",
"18RNPagc54BHd6uvXbFdgfPtEWK1XXKCig",
"1NWfJ6ApbGT9nZtU7pvVCrEcqevr7GHb5p",
"16H6w3DXerf2uvWxJYFwvg9c536nWcwvhG",
"1D1fPFxX5GPBMa6MqQATpBzP52HzuYGoTL",
"1EBKbwZbguTqrZhMZgm7kFNH8Aw3pazKvo",
"19R5k29RByoAQyg5ozd43iHRraN618RfjF",
"1LQqroo8AThs2rVQHB54L3hcQkuCX3dBqp",
"17urCh9ZSqcf4oMTWhAJ7PgvaNdzVxcs1T",
"1GHQqQhUfLmcBqGRgrNjicXyjxgtXtBJmp",
"1ARAZtVVV6VbEzzVNvb5c4Yo47mGLsCPj1",
"1JxhFpyWs5YURwB3R8zSWCjuzGhN9MSXmw",
"178BKZhiqFtfd4tHX2ziDpwWrYd6kGhKwB",
"1Ec3Hcq7hN8BUR9KT9WtAsufNJPhUF3gdH",
"1GigaekT1LK1sL9A8VhpfjejGTSDpRmAE",
"1PXzBnpHmAWw68J3HHHfRhnb8sgdaNJK1k",
"1FrcFuQ7QaNi8P2HxxavTaJJ4dZkjSzzEA",
"16G5E3sa9GuQXwNSPkqNYZp7Ce2ie8gaZ6",
"1BduBHY7BapdTTnP8RvpYeaEy7WH1MCE9w",
"14KJh1aDcRwdZssiXXe924dUNjGtkr61Zi",
"1HmYanyiPXCrgdGBzFYx8WKJHNU2Nb8re5",
"1cyuHtjgC31Mxkev16Sp7ZVk9N9vb6p6k",
"1BWgsCuqPkt8cDrDVcgMFyWodC45fZZ4X8",
"1NGMT1ZkyUk6SSA6CtwxubAUzgS8UxbVEP",
"1MLncfxP1bSt9WY5wCJVGRw3in8to5ERiD",
"15B2bT3NbNXvVoZTnKBLsKetJ8usCBJs8b",
"1QBmnimwbsTaEFkV3eanVAcbjQjiNu81qh",
"14VzPdYcaJjzHKSCXpKppUtRVGgNv66Uiz",
"1Gpyz9ou7NxcEaPfA4uHBVpPmZbbYAkLEY",
"18ai5hgYGj3teWnxxJv5sJqBuNBdRHh5gD",
"1J2JPdDuoK6EYm6YxXkbQMHEYpmXyXUbJT",
"1PumKpSCyAJ6QhspWrn3V6VGKZTUFKd8BS",
"1HXoj1mRAnF4dEiQ5ibzuUECAG6n1xfoh5",
"1KMNXKqAoVqvju5ncQFrdvDUeFMRhTaNgT",
"1Bk1k9xzyu4b62YcJLDNTcQsXUkrY9LwZS",
"1HbdMYk4yFNFDqv4YoXNzZoh5c71Y9mbAQ",
"1K7qKCQb2MaAF7agDzs1pbHxGd4wsVhxAy",
"138i67p8GKYSZA5p1SjRQXFoWJLgUfiBHZ",
"1NDiXkVZYpzduA7nLCKwUHeZMktWTBgqwY",
"19KUjPqiBKZbcKzy7iDiCbNyhcDCE2x4bW",
"14JdLwceCa4NpkjddstGY2oE45H5SwRhb6",
"1GhvsZS5yNLejHJBASCNbF1NehyXGGya2J",
"1DhhoGo2NVcmEEQXrMivzLWnatVSUKQWgs",
"1K9cQcNVfD1phfcA5BGWCsZekyjfiMmLyQ",
"1MokjH7ZZeFpxtBtJhZHJq5oG3inLG6ajv",
"17n1EPLvn6dJqYMZ31EtGGzyyWGLyR76pT",
"17YhbhPpgoJ6eJ8wJiEzfGP77vF8N6Q1qC",
"153BYKCYaz6q4vDiawuGaeKakRavB3cD7g",
"1DEfAps5oKVtxRZ89Rq5dGf8shurYXtZYW",
"114qxGjzXnYhsYptc9kSMYYL2spRoNzAbx",
"19riKLDkikpaDdzf9YUH3EqDjv4G4wdksh",
"1A6BkwTFAsmvck38gsW9zZqDLkWRnx8AjS",
"1Bxs1MMKtaX2aySE4qGLPDdGn67pdmf57i",
"1Mz8WXrrxRHyhNkB3qACVb1vP6yYvyVsNC",
"1GSpfYSm6FpJ97EVwXGbXdEXWpmuCrEQ9c",
"1JKj4chBkGvWcn5yQJEkU2k3in17Wxk7Fv",
"1K13RBKStyFJFC6ZMbmDZRbZ6boXvQZEX1",
"18QoFb4M8qJdnSA5canM9vJprghCmmFRnA",
"1PU7fKmWu4arMbY7PJfzmGtdHXNN4yWwHq",
"1Fvop2GwaEDi2sHJZH5Fbp9dHZsUWMbmAo",
"1CTr8XHNGSSYvKPLSC9NvCcHZRSh3Peiyi",
"13wEtYYa2HNuZcdeM9UZn2e4hPPhhWSCQK",
"1KSZncXrw1z1q1GocxF7ks2tpfwuCrVQrv",
"1HQt9VzJgLYmq1M3F68vd7EEnyWWxF3Umc",
"1Mu1XTzojtUz3DxYfWQx7cfhQnVhBbCD7Z",
"18jkCb6o6XjABnVF1WqQXSqdZSKsCLxV4u",
"1MHJiNPs4NhSykTypwqdz9f15TTSnSXQTW",
"1785rRo254vvx9xYtWectn4QKv1apH2uV",
"14KcZSZyN9Gnx55VLpkjqi7Tj5D3kte4EY",
"1CRTWX9ijzcPnZreHnQNVw5oqyEe7AiLNc",
"1GpD7TuymftPFNjVrHTjfwznRcDiRKBSkU",
"19wDnd41H1KtPfSp3iMWsnFyZGrjWs62iv",
"12W8VxRGPWbacuau4wVjdtrVVve3mnzbxa",
"1GKtibLeUaUfmnkRz2LKswMZj6weuA4qYF",
"12zuUgGDfjRMi3MkJqbPmfWkSpEvQpkYLq",
"1Mm64ao4ZZJ3tzswHtKZxi2qy568gBqyz6",
"17aZeKs94iBesQM9pyi9AvLoKjbXZiYH7m",
"1NxS8cnuSrbmv6x2jYMNG7vZTJZCfRqokM",
"1GKErfdqMPQRSgmcQPUjt1c8HPivWxKpmy",
"1Mfi2X3DPJbW1EoPWkssBTo9q8PURPzf8A",
"1AX9qqmmB5iKPZd6oZVzvZRF5yehu22aSE",
"1Ph13EfTGhe3tD8mDvNeVf314jeYBrbuzP",
"1Nrq2HpVeGQFvJAbhpZhQCf8ZXUxNrncso",
"168PzQZLVHKad5MXQFiPrBt59cftDex7PQ",
"1QK2gxyhVuLoewxbdcdWboL9GahLzVen51",
"1NqWJe3pkAiRL6Y8ShA8ko52W4C8nEu4Ry",
"1KBM625KAdYsi7iJfgFUgc6BXdJhvQN8Fz",
"1J1By2oqxQNHpVxYJ6zhfu4DVg4JSRtUBQ",
"1Dcr3c1yrGjmGMZGoxGHxNu4DT7VikmGCn",
"1QFVmxCdx1iQpC3brrkic8n3d9socXCUfN",
"1Q35cG3HgYKy4eEcMT78AqT5wpWbCe4LTg",
"1BD3nyYBKeEhGrp5kqBhDo23dhoDkHPX4R",
"19z16hcVqqMksyoSaSvxyEH73ZPaNvCpXo",
"17UkGpscqAjjaQ9euHo3JiBBZ13kVozTPp",
"1DJCY9HBxPPTmJX13vDti91ZNoXB966xTE",
"14hTQfRLpJqJWzs3s9vox2pfcXLKZNCrz7",
"1MbmEWwpowEufjaEBbiYYfAGobdVzwKYk9",
"1LhSUz8fozt8ckqUe8RHfbeSCZxTBjjpRN",
"1Bpo5SFscrqKuYk7Wf4w3bNA2J9fGpEG9g",
"1JUvKkKqgvGgdHMgkn3MNVANMw5oR58GWh",
"1EeFFHZM4Eqj46sD2FZk1foU2C2asPkeZ5",
"1N7axDkLHQVvzwuimMr5Gk98cruAb2cJCh",
"1MKQRkjYX1ydgcE3UNjYwmK8CmsrqPeyif",
"1HPgy2sLgBJ4Bf9YDxKi7ACrtyoDBdAcJy",
"1NZodoAtSYvWLSf6Pujy225YQ63QsQnsjD",
"1L716BndKanBqGozE7GW1KiJcaqC7enEhZ",
"1A3TteWhFSLnqRzxhxWNHtq51N73gbMx8m",
"1D7zuh8GX5or6dxnjRkRiMkXBgoM2nCr25",
"1KVG7Btf9syxXtLyFncNdGa7p8AiCXbsB9",
"1224AoTphorTrS5aQMuwnv2GZhay3tkhNw",
"16ZX5Ucfu72dhL9Um2PwB6TmE81m15BexF",
"15gsht84fE6EqZxacrTNFXbvh5FPT5H5nY",
"1CzmMHHtoWEimkoTzphjbjeM9RC8r8SGGN",
"1KLkpZ9ofH5H717BHSFYnKfzx59Nn5rds7",
"1E8c5wx64aLApoeCRja26j3Yq4S4L81S9G",
"15hHBR1HrwZc2m2XZUPzuX5Wofb4gGBucD",
"1PqzQqt6MmACEY3SUG9cZnfLZggimsdBBD",
"17d6Xbh3hdyWzyvXWhv3Luv99ynXgEEzWG",
"12wdZcLHNRBBcyN8YxhVMo7JiN98pkgNdx",
"19cY81trGc5zNmZJmPPtrMp6skUNAerqqZ",
"1GwAAQdPU1qT4b73iyAjZ4qupzuG4mhYKA",
"1AjH4kf1GnMysXvEQ4utvHmLmDUttVXQan",
"19sSxgu3gvzRARLEC2phMaph5AvrjMKwyv",
"1GT4QHjgdXycxkLrPaibqM7PPcwoCi2CfZ",
"19h6NZKc47bupeAXwGpi4QW5Phxq6Hmhft",
"127VeQJFfafnGxkmw4U4b3QJXEsrQniVtb",
"18KohWT8mY7iMRLwtQJP3UTwdmVnBAU8Sd",
"1PKqZPcak82Jyfo5vKKwfg1P3Sx75zwiaJ",
"1JTN3gb2v2aLBWxmiVmFKJcE7QvHjRC3cH",
"1BBuGF8niwNgwxHBJh3CoJU4PjANeNdmG4",
"1AXQdkzMJdxZ9RhxphwHuA3xCUiwG6665b",
"1NGzH1YQtDytFphvFEraJ7Jb8SBx7NBSsT",
"1Nn2ahpHbN61UGHAhbA5mLKbWavRznNK4T",
"1PkjAZDvPRpD6DANb94X2TUx2oarPkyMpZ",
"1LtuPRrp49SK15opm7XQV4qsoKPHeNGjNz",
"1BaZfASwPpJt1Ks1mH2tgbfFSR8h2zMkMv",
"19ijDAKabzGoCXyhrzGJ2WGoVYSUiBEb2V",
"1Bke7UmJqSP3Tmqd56Unqn7Grv9mnfdnj7",
"1E3wiJvNFnH9TLLuaYBjmDFEmCvShuP2e1",
"159Em6n6hX3P1Z6hDi94Tt7BBLNFVap4tm",
"12nRXpz8q6Uwuyt6co6jYwkxJ8iZEFucCw",
"1DX2ACGzTNACdBCaU34wM1Y7F1BJPCrMmk",
"1MHf72rxvpYwP7eCJ5GiEW1TVP8tez4pfV",
"1GAw6r3XxD5NAGgjv8oACWXvMTEefFSefG",
"1NszpeXWfB4NYQViJkKDHZZVXLD8Jn4CKJ",
"1FnrPhzq9kXvtDF2ZuVgFLWL57KkkBK49V",
"1F9ufQbxSbFPCDn1yiK5h4bZkvZ2Xe8Pbj",
"18dq1Ur8QVLfdZoftBfqGH3PmjaqrKz67z",
"1Nu62WK4Wv9mcCj8XU4cK35mok2mRpuSrT",
"1FN4LuFgzeFR2kSZsVbRGmUvgeaE2NcH8S",
"18PtSa344ahxjktgFCSanadc2iriTCiRfm",
"14noFp12MvLVRqGg3o8s33u35fFTdYVc3H",
"1qEVt2tE6T2vJpjjsLGKZT939t9GnXvV3",
"1DUCuorLojwR2Pd8soiV1ahH4BdokD1ctH",
"1KfqTpxqyPtXAptXKCcVeS1VgxCXA9DBkT",
"1Gq8Q498Q6iwjwDx9pgwQcNBnmsGeuu8qW",
"1ATpo9ajK7zqYzcgR1435sJEbrUReWSYAi",
"12LnuxdB6GA6n4fVD6a32H8MFMse9YkrLx",
"1Cth7UT7uTSuK9ageYv4BCiYTzz4x5SpyC",
"1AReMMVCq8uKy78T6iLzCfcXw8QZbpSj2Z",
"1G4gzE6gjk1dwPhdvtTtHWwErKAxGkS8VK",
"16QjCRQbwUo6M4ZrUvSaaayrsZPgqnyQfy",
"175XBhtX7qqA68YMGcqx93XMxSMpk9k44m",
"12WJFSgWfMjmLajSwqsGCzG4dcki9b455m",
"1JffHf81wfw8UekJ7sRUCUbxHeVbE71d9N",
"18cMixtwUUzT3aBDTcpNTo7rhLesho7fck",
"1BsnknSXjnr7d5Wm96d3kBeXeWEQGHGHRS",
"1FfLjXCQxWLTGymm4eyK2cE2toyaxx9BHL",
"1Cy4dEudLzxpbCwEfnfmo9AHT5ZyPSfUCh",
"12hjKrPgrZ4vLqpQWKxwFxfWJi8DLXQNxC",
"1CMvmGTDJnVJXenCJc3wL612fHoU7YAKGy",
"1FkWR9PiHmQtJ1zTd37wV6eABZHLdRsF8x",
"12det4KEMkvsZd7PpnQXih3Sknh1txcPJF",
"129PCSou7vGWVfSyyrRFwz8PmiEtBLY1yK",
"1irqN4NdThDGiYiBr5DEwAXUGuppG8KvW",
"1NMGtXmTJhUGka8Bw1tMdG8yGUDDjAQ8Zr",
"1378u4mkdqjPqYiNNAmt8LJAnsBDhUr7xW",
"12EVQLdRYGw65nzWzPBsFM3ocuzypn45at",
"155hcGycL4mcU2brjo8hPk4rtHE5z6ZEpp",
"1MUKG4XyFmUdJ6f9iiRyvzxwDcpUdaofeR",
"1AQCigZ75uj9ziwvz6GnjyVt2uJkU7NPMR",
"1Lnmmb8kMhFDBXsGv98tP7SEVR5QShUM2M",
"1Q6GVRTwiuoFK3Ur7Cws6BpYDitVC8Ju3x",
"1DXzXRBkB8LcJ6Xqs8h6qCTzWFgQCpTDHp",
"1CkHzeU9UGP4LmVWGzQp61vNYeiGZaFbac",
"1M71x36dw37wbwGdVkcVF3JJiuXjLghWBp",
"1FXgVU71uvcWDnzgetXqtg8vSnUKbGu45j",
"1E9AubUzkw6Y82GVSqEzjaKjnecG3QinW1",
"14o29AgerfEFgTkYe4h1JRzRSYtA6pQPPS",
"12sZW6Vufd8eYpNG587gNHdARCHyZfnBFi",
"1AXbvqL2aAvY7BxGoQK4Qat8s4JXRHjGhW",
"1FXgd1kDBdyGj4NMANuurSszdoyCF4MSk5",
"1K4knXPsZhvd1F5z4QpJjPHHtUMM9EM5VV",
"1Fr6rTczdEPb56kCiwvxjsxBapqxYtccJ4",
"15PWE45Bih1qXdSCVXNojneLDrP8go5kCh",
"16ackQdVvfvaHQLKV94R6oDcuuFAY8RF6m",
"15AdEdLLXTLmLodsRretN9yWmBed1HEhde",
"1DVHDW6KHZWXUsNt2kiJXsb5h24jC3Si8j",
"13DtMSqcYWxyKmXmfqz8JnX9geGMizaKnd",
"1P8PLsbGkhumHzUetHH4wNta1j31qmbXuj",
"1Pi8D6Mpec7ipAqmekt4Ao6o6feY37h4J1",
"14CAzgMVvaQCeLKeJ7XXmguxhZQtcLvHp2",
"1NwG8d2Sz4kP5xEKnzErKqKPm6uxbJb9u8",
"17uMT2VrMzc2JuKa3DRjvN5QA6Wi6GjnH7",
"1Myj9iXRJksDJ4RUrVW2XLrajvbmnL8Lxw",
"1GmfookF3KjozBo2jkB94SstZbKhZK3EbQ",
"14gruQNEWYPMdCzPRW6U3d53Th9m3W87uk",
"12UuGXRH81HHc4v91nRh2xv45SbE6F7D9v",
"1AMr5AuKWf31hHNpvT2GkzkzMca2xQ1Cba",
"16mYF6VaSfxCkdNkFb7L7nZADBrnoVu3mc",
"18HQqXXXCVraSBV73CcTAuhDp7SY55PBG3",
"1HBeVyLdgqQhjUEc6kd5HP2zu7GDqn4Wsb",
"16pwSxuQYn3qFW7HeL2R69BGvXpMz4ZN9Z",
"1DW7DShHUMCdd1rjV4NczgTAkDLNXwzQpK",
"18TwrMet7DidYDRVC6dmeocA1gkyFSsFWj",
"1FQDUetnjoswR3wqZR64QNACBk5q8HQTjG",
"1JqzMGTh7jrmw5pdQ1WB3UXybuZAgXuryW",
"1JRXbZZLTKmndiYaaM8RrA7hdejNHwstjr",
"1K7cq1Feea4ZRjnGQmqGGWLefnSwFYTXv9",
"1HeYZcuVASLJKZPhizjBSbp4hUbZkjNmQR",
"1jvmx8pd22jePaQZK9TKQhy9iAb9LY23Z",
"1KLwd68GZXVvWppoHqGW8T8jARhF411T4G",
"1LB7Hj6BDZSaEgEsEx4JnZAyvXGMDhufCg",
"1HVBJuodxLVvxnAsCTZPUvipriTTrNbByU",
"1KnMd8e1mWozGsj9y7Hru4crh874NF7LY7",
"18RfBgDjNZ67KzK9FnU5sHRpHYEaKvVfkV",
"1BgGT7QHHH6GqkmFFsR4nY5XYEQbpNzTML",
"1BXDQvYEZjDyxWEMDsEed9m3eFVNH6XQcV",
"13KxBtZpWMKWmzsowSepJSHUkooMLgv4pK",
"1PMxVbusveXK7unfLywSYF1zpnDb3UPHkZ",
"12w3FdpwP1VKv6fqWAWGMUF9165L8QivmT",
"1GhB15bgkr3Hc8TQeiocdJSWNrBqVmqQPf",
"17WfqLXypMPDYjjSontgtMvd9ruiHDfDDQ",
"1Pt1e8Woe6VGcmXVt2GBs84FLNckMUpMrc",
"1Eab5qPw4LnX9Mjs9rwFeoDXhLNcjQFqu6",
"1kGwoYvrtJpfb1WsvhombyftasQZJshpp",
"16C8zJJkGEF2SLtcNo6Dzq5V4TgJDCL6n9",
"1AZZMKeAG7WAb1MfZxf742M2EtTBxE6igY",
"1LMWEoPxqkK6xEYFNq21Sj9p6KNpmgbmJr",
"1BL2RQNWsgnHaQ26Lv47KBSGngcQTAEqCa",
"18Lq2TdLaKnHZBzdxxeAmBA6APiv2KYdD",
"12rvmFk2zmtMKAhCXPzPYGcYLbSQdScxkg",
"1NGLz4r4zxJkvQ2MZYRY7mBPzuvwR9MPPa",
"12VcGB5wEy3XddJPcK3JCsTFz2oq5j3vLB",
"1Cq28mC1bqxmwkQz8dx5Kwkm5BgCT17MgA",
"1EMmT5CZf8PBUbEvtefisN5RsjzUe9ksgm",
"1G539n6cpSX68oqqxedZpskUZPZVTaTgAQ",
"14GviMiyCo6cXjXzBnhjuTSrR7TiHnL9Ch",
"1MVczrb7Cy6sTLJAeQptTFKmNuDh3mL753",
"13Fjyz6Ev6cexs5dybpqVvRe5DZiWDhZ94",
"14mWKxZzwnGdEzJtGMxR9N76kabrtxXLty",
"1KtZydgUBfFbWmSZqhAEvm8BwTbsYEJS5y",
"1466Tk6y95u8asF8VAuuDiZyDx2GttxSLr",
"19M9zKneQXwwwWLdBqKtvPaw536PaUSrtN",
"1NhYUUiDJZjg1zyc2yYSwrTLgSSE6itWgc",
"1K8VDhWQ5sSWZs63F4zYF9u23EHj5sv8N7",
"1EZ2Dito5wuX1fR15gLfeGWkfpRE6vG4dv",
"1HusBx2an7dv7E6sy7h4Rru8vkBorZkGft",
"1GmDqjGAgPEi2QRjoEMT8BZkgAtGyRBEn2",
"1FTAn8Mp3PD57GZ6vX3QeJQqhMZjdi5an2",
"18hFVaKSEc7MTwZ2wUwALxNhvc4TRgXWRV",
"13gTTW1aLUSTUq6Se9dFznvJqh7kePtGtL",
"1BbBG8JMXFxbVHVU8MCWbsB8xbeNRfymok",
"1KQyGnub3jv7sHtTBsboKqPVbZHZtoHeys",
"19uZg4c9FuqKnF2ADXrSkFqrTo5jiB4sYg",
"15ttpvfVAM2mCeqMyBdfPUxoqx7U79fQfE",
"188r7ezwvJYYYTkGw6Mfi8wYZrc71PXSjR",
"15ebYU8sFQJ9e4br1RHrJwxFrYJkwtopuL",
"1E9585qAeaPUnFSzaD4MudwZyjwE7eHMjD",
"19RZyPxCUT6jTutoM3GXzdgPBsYN1VRCJN",
"1HNzrxnw41NCb3YDKVj9gmdJRhCpawcrf9",
"18xm5GLuDor9XZSdRWmHL8dS637W2VxEvr",
"1LYbHuc3MzL566YsRNAE1MmU9417z3My4Z",
"128feg9ehfqyYqXtM6XSq8CSVPLiJmNUsm",
"12J7rdibCubK293KHQgyF9obHZ1nSVodzg",
"167fj6WTWNZ4pRKeyBNNP2L6i4V4Gpd6BT",
"1JAYp4uT35o5fzZJVRmg8U9VcFUEdu1RU",
"17jo534dCzqxusEKjxNiUFJ1zEqeh1Sjeu",
"1335LdcPQ1fvnjboNd2TQjBp5q5NrrXJ7o",
"14GhJjvtHhpQRzMUmZg5264Rh2Gurkquqw",
"12wttizY2ZqUrFjuEFqHUWxmY72CqpXqMy",
"1ANDAdPUd13brmFYk8GeRUr9ymchXaoKhv",
"1McwowGJFEnTRFrGHLEqWBDiNYWXwjpzmf",
"1PDNFX19z5UcrBQBAz7V8qP91VfQUBZ1Jz",
"1DoRNwhcXRRq7fEBeMyFFx7QVcFqzQYBCj",
"1GkBzidQwTxJ1YPX78TJyd24oQQq8rwPkr",
"1fFWYwTAEjM9vUpbyjNZBCmn1mSkDVLVT",
"1ESMrbiNqrYLA1VoiuZPupESMoLj5nNA2H",
"1FcBjFKM5Ft47tZ1YqyH3P1oUvZbkXKVvW",
"1F36GYWnhxiGSaXXQxtTBEAkRsesMgXVEf",
"1CwBUfGbtigB69VhgAkcztqCXisP2Jbmu9",
"13kUaEMjob8P3Si7DWaZQ8NTpXecPLVGDr",
"1GpyzXCbJ1YeaBEaMVZrXA6DNGy9NQU5bW",
"1MhN8c3JsnYUqZSFtp6W8VdUUymUJb1EEP",
"11cG7gP19KyQQWMWo4zGMr54ueVUJY8PP",
"1628xHbEjhjdciEd5TGkGcmF4iJhGCxYqS",
"1BFMeAS7FWW9M8W5FB4fDFGJedP8t6C6vo",
"1Dv5Y8M2NGNgErZsZoXmXHzSddfZ8g7CFd",
"1ANQkDqqPfNhX4fV2rsiHD758NtfLfcGFW",
"1CpN2FazJKV5Z6dNVbw2M62k8P6gxQNDWw",
"19YXZdVfKUQ118Wq8Q2nCnwEfadJoZUxoP",
"14kWuT6AS7zT8btw4939jDcQKg6PAH3jtn",
"13fPC5GChVo9dw7YEkEZyMs5eB4HpzVqVA",
"1NNBXhcRn2AdJ3iGebGREtH1AHUZtYp6v",
"1Aj1XcCsMUmQ9xqSiZL4Eg8E6vVLyPVxhE",
"1PP1XrZWD6Xy6Adnx6eiVhQScx1d2c5VnG",
"1Q4CPrZ3M649GC38nPqMR8TdLM2FGcsoLR",
"17cqYJsSzyK2Zb3vtpReyY8sdXVbmCLov8",
"14WYu7dETmjGNs1CJMXS7WUFgVbiSCMf7v",
"16KacNLpYteTNzt6Pmr916VGBygwW3VUir",
"1FRU4MeeY5hrHvfxcd3jXETTbetK5z2rCL",
"166kR8rZMMF8RKMgrdd7vUMDJWGAGMzUBK",
"157NVgYQ4NSY4VQPUZDmBAzUnQq7WdAQQR",
"1JzyiGyJ6vfbd51t3VsEHR8rGmRbUkVhsv",
"1B8o3dcWshnLWCBrYVSDKQfTwpNRBRuEnt",
"1DGmZ42HpDhTxbSmeEVS1JyLFCSK4VFsLB",
"1KCUQg7EkJG8Zpmh3LwXSRNRFu8VZqrXi4",
"1GMh6PNdipAVhYkMjXdFJMEhWrwBk1xG7Y",
"16mqGMKJCUN2dyjoqRhS3LoaPBNzQTQWjL",
"1EhbA5q8p1AmXbrdxrmQ3sSnvbPoXjdo1z",
"1LcjJLJEL7zBgXTU7Fook1598hcuWN4R9P",
"1NYrnpvLit9NSk4nuXriULJTzjeURKRhyR",
"16n4R5KmpzNRSkgw5hFFZ1nU9mLb5btpD1",
"1LtftLpqRapk5BuqRHY2ZypQejKW6WhwpR",
"1PYUdPy8VLTtzbEGqwFTcV9rPHagJnyZFt",
"16unFAM1Z7KWycUb2C1Uw9hCuYS2th1SGt",
"1ak42n8dmehKC3kLX9RCmCnnZujdk5G9Y",
"1BExHpUt5aaniHq2JvYE6166NsfFjQ5azQ",
"18FNx6cC74xjFHM2xuCW1hXA5AsV9i3BVo",
"1BE5qsQdKaEDYhPdrtZ8r6mSaNwukCX8bC",
"1G4G4eNL1kBGdRWWhTDy17tStoitw4DsEx",
"1M8iT2dUXFNUKio1rqyRFr54y8vT5SVtGZ",
"1Eietqg5zp6FgqZesqiK9wvrXtnAo7ZzET",
"1HWx33Abb3dUT2BwaG83DS2zpEU6gSM7wB",
"14Pm7cV1Vw4Gdzch5BQr52ahkKuCBhRD7h",
"1NSUQZD9pq37A7euLGWDSGrnp2xGgTZYCA",
"17VzxJTv649DMkdYLp3tEvWGUsYruhkHXn",
"13J5i73vdUKSrczapecDtfaAgzt5EZV13S",
"1BCkd5uacZQuS8WDHUFDJzeNjeqqNgxGsz",
"1EXStUZyKboDe7irHWNLCXENg9Y61DseCr",
"112L3jNdTpWXnX2yhpVFbDTfKanXZRPU8U",
"1Mmv7DxBM2ga6McchZyFNTXC81va9HDw9u",
"1My6jn3MbbNek8hbEGX8nvc4qgcy13CrmZ",
"1HbJhZNz4ucZLeVZASZaEJ7dssZvHJHZSS",
"1Q9T9pCjY8r1RzdtYJtCeK8btkCZNjUHkS",
"1BwKYWmkKzSYnhmJgUDVL4JLD827XqWpMk",
"17taFpQAjVmhs8czNvxETygYtYVMcYRQmU",
"1qt3N6t9mWt2AGB6DGjCCWQMvB1X4Fewu",
"1AhLAxrf9Qs8sjZCKMvPMDjGTs7ZgiC1Uv",
"146KhZsQXT4dfre3YmV9M4fHeqAFG84DvH",
"1GCJ38Zo8McEXtRNRsnScoccPnLjYqhTZC",
"1PjZTs4h99YDATqaHVmjqWLGM8gnbY1jLw",
"1JJbSF1DZ2By31ZYYLfNo5GjQPWqfXA7sQ",
"13EeyWZ2nhkrFzKpgZfMiDw4zwaZQZnkHD",
"1NXQYoBomKMvSv5xztT1uGb6RE57UEu9qt",
"1QGrNtbyE4ZNLog17tNsriWqgS4CRgfjRX",
"14X6W6UdUiVAsjjDrFzmGJexwxEcEmqZeF",
"1GzDsvUhwogGTuPxdtuAYVVWe5SZntcwGT",
"19buY3b9fT2PQ79pQs5agA6fn236eaYajk",
"19LrC22Uy51jn1NxNyHhqC2mnHe81aZnVX",
"19eNTo1s2CjGBHJeCdt4amv9xLG3CrHjzx",
"16BFvXrf2az6CzwAMmthdozbXi83jJmVx6",
"1KNyUoqcMZuUYXFEXX2Xasp9PcpuGSWmnR",
"12dDyCauV8723JoXKUEcnfVV9xW33y5brR",
"18HzqemAhMKzKBuvgq1NE6qywFeNZ5utDZ",
"1DvZbhGXD5FCQQed42Xf8AUQgGuZ2j9jJE",
"16jgVG9Xey49kZn3DmHbtxrFbo88BXQhd9",
"1BsCjpEWDrCQ1JcZUrxKmAYg1C3GckPb3N",
"1NAdbSPnWk6UX8aSwJ3ztvpadSiSmbEsth",
"134kie4tH5MxS9MY9u1EfSfThiEXJ7zNqe",
"1CWWQjnmaRTQpnf11gVhSwnN4MFpGZ4V48",
"1P92DGq4hgKMJFdYrY5DWn8TmJLUbKUPT5",
"1A3XekkLTgeEeXhCSNJgrvhkivF496r1zS",
"1Pb1wRc9DaPZda9MEPftcbu8tFXuXvJoCf",
"19JwAyChL4Te3HxLZWsPSQC3u3YaxEg27i",
"1tWdWWaZY2MfVNFV6wMK1FMLeJuhAvoxv",
"13E3d4xyyAb3LVoFgh32gBrcBpZY8R9DEG",
"154tduaiGLtKo2vZSri3bmHnCT1uYLyZHL",
"1Mwb7bd3RqMohDFQrtVcqVjL8gXSYaYVsv",
"17m3sBqh4V7yLVm2j2xuwf45qar7Tvma1y",
"1HxKwEJrce5gaKENH9uoHMEbyLYJ8tkfN5",
"14US9rYdbYaxNRKh4hFPePXVY147bwF7am",
"1GMc4HJfr5UHPNdfmTVgQauEfVbKRkNH8v",
"1CFMCx1wTo2ZwMv4hWkFGQmBXCKoETcnEs",
"15CGrmntCcC1gaZ8zBxGfTaz2PYscRc9rk",
"1BWm5uSsiReLfdX34utAPE3vZ3CCMKbaKW",
"1kwrqPT5cVaWY5nFmRUGsV5xv6vWn6WGc",
"1GeEcLo3n2mHG8iNYZZmbxa1erP5dZMxSM",
"12igE44h9wjb2V3Q6qw6Hqrp7dKCSA916e",
"1B8YuauBpQXC1bfW7EuMAL9bsd9KgtRefP",
"1FjbpaQGaCYrinrhrexYKhcfiV6Fs53qe2",
"17k1NuUSNGmsj2XeuUC6zrdZ5bDNFtsD1C",
"1BCrTpidid3e3WLEKRTctmssx8YRs1VEVv",
"12WzctG4yvwKSMGvVcwmcNXuGkVnz6nVzf",
"13sK4jF9VkiWpTmkoAAB7k3xmL4Q2bTYGj",
"1JcJq8HBpSBAVgXwrkHxLY6F1JQLVP9wt8",
"1LeAMRYojfFUgLj2ZtpoBVMAFcE9YqTeGv",
"159DqTc6Krtovd3gnMCwhW9P8P4cQXqFGn",
"1BoyTUTdHtwwvwgRsRunnGBKhbfVx4jahs",
"1Dw2SGtsH4ry8RwLqznAdaqoTYbSv4iSPg",
"15iCsvKuWMXtd7a1f7T3JN55871e5BUmwP",
"13E59ff7ZG5VjvakmNp57XY43G7k8ZtkbH",
"1PF1Ufu4AmqzBqcUhj9z61xM7bjVF5CrqL",
"1PwXE4j9THY2zkxiqiApH9WbRzx4YxAd8Q",
"1GdV9if91tuSYdmDr2vbS7nCtPWxtmvsAn",
"1F2FDCYEN1yqD8r2M84hkF8CEuNogaM6QU",
"1CrXVCgX1uR49fJ5gCVwNHUcuUQTznFbaj",
"1Lv1FynLtjiQJLYCCuz7Bheuiw66GNJD7S",
"1EphtKbDyCxY4PpNawhMY8C69aGWDyCGCr",
"1D2kz71tX4F15Z9VDJLQQesndEJnJLZLvL",
"13cq3ZrMCuPeBCFbFAGokXzGCn6WBhfx6T",
"14MB9bnieKz7xHxkKZfRrV4ZPffQ83bscb",
"1CbsAwMVUsXcLgGKVdD6jLNsZjrSxysD9Z",
"1M4GLBVoxpM5GtqnH6Fr5CWDno6XWtCv5J",
"1AQs37eeqQseJUFto93pcXewa6vUnkR9bZ",
"16GnazW4YYxV88jUzJAwD7HXddNdNA1ES2",
"13baNSsGqYEeDBTvusBHcPu8zNPCGNnHKN",
"1F8w7xMK5pVj7Lj8YNUe3sNZ9sZLqU4yVV",
"17g7zmPkdGxnPNjfis1jTGA2B8h1Lqc1mE",
"1GvPLxysesnJMt8wGHQ3x5YsdHBETyfVEG",
"1CfqRMUDXxPhUBtx8vt4EvkwSaS7GLbitR",
"1A6GudMTVWWMMF8eYCCew617T7VBcsCJBP",
"13SnRLKoV3EQEAzB2rAbCNBjCPpwR863iX",
"1N7Ct1UwLVU9JA8T9FtB9yNGVbWfxBPU4r",
"1PTTiLNNSnoGZsrNQH9EXT22BaS7XKG4X6",
"1MG2M9ead6pTDEQf4yMBTF2rgm7u2R1NVu",
"1sh25LNs7UkHXb2DRVt4CWqsSAZou41H6",
"1A5gSiQRrJHKGXQpQZVDbbJrpHbftJyRsm",
"16tVJeZAvtM1mQQKHbPZNAu56ML8YRkpC6",
"1CwVfxHxhTUB5gNxvjDmXddKTcKF5zEUf3",
"1GwAdS8X95rC5j66mRAfEVwZ5S81YfyeTn",
"16vYxUaBXxg1nqEDdwrPLYLUJ6y6jjH7NJ",
"14WRg9u35vVEiHFzxDTPMeDTZ8Eh4ebYEv",
"1P4Ym5icv9887K8n7aAEATs8t3XFs7tsQF",
"17iDT4S6r6xNEnCxz5CoYQ6h3MbZgF8thK",
"1MY8DBqUusBN4USZZxXw5geRJ8hXF2wHf9",
"1K8BBS47WThUeJDYcvMJ7KGoHGBUipdpsC",
"176zHE8feH9ss5vcyhv7Qt9ZBP2wk6SE9W",
"12RSzdeNpm4wQaMK3feG3wbnr55Ya9Uzpv",
"13Kw7RoMbD2veqaChurmJMwAtTmLJptP21",
"1MyjKVuTY9oCczEhvWk5RVQgMNPKHkirgc",
"12YYUXEDwaiM7NG2ZoCs8TMzzu8EjzStSj",
"1Nor54hCrUzXd1TxkRdVAs6raonoYzRNhH",
"1EL2b4KgV2VsUqJtAX1UhRxt5qHNzdebaP",
"1BUbxmkkCnZJYBeBU4fCVnH2Y6KaJtrthM",
"1E2g2knrzktcaW6V3YVGMnqJU9m6LL4rji",
"1J8GaWyFPen5G8Lxk622iqQCXAsnKNxssm",
"1LXQqofoftB9hexijApnWPj3yjd4oFB1VT",
"1LgNPfLkYJHhHwHhH4sKsNgPUDUaKTdBXQ",
"18VQxPbmFVyFxRcufzpAd3oYKiHqBveimr",
"1Pw82UtDrntjuBv6PZaGBnBSgfDWCHFgea",
"1Ht4v4bSNdi88NkS7BQ55AFcxmLNm9s8PL",
"15MeyLsD6kq5eP5x95SkcS8MVW4EuSjbVB",
"1G7oNvJ3145NEPnCnCksNYfEF9Yo6CCREJ",
"14ezYm3H3wjSQ2HNt1r2wSnKx7fB8xwZ2B",
"1EudRruuYhU9pLvxYZK9khnfwVS94TQv83",
"1HaVGNJhysLSSV2LgNfTEJxUXcC4ChKkeH",
"16Jxv6LRpLe8Po61tvA27KLymXdgFLAZfy",
"18f6DCx5okmF3ST4z7VLE6SQCz4iry3Jti",
"1G2iB6XzsRYmzVtUDnivNn5MBVrJ3cwzn7",
"1F1BkgCAgQyAXh9LGcPNtZiCnnao4KtR3g",
"1LsHhKc53BrmZrHJY9qqdmJQUwmnFwa43q",
"1A9cWYewG7D7JN3g9dWrFL1Q1vsjMokVc8",
"1Hymcc83h1qQbZz4oBGeuqeAx68RcBL2RC",
"1CTAcbo3LQfYLnKumechFubRt4m6gcznqx",
"1N4TMSJ8HJ1kpnJZqUbwiNxJ1wgTpvyqgy",
"1GycPfTPDLVRoVW9Nr5mrX2minX9tZNtqd",
"1CkNeFw9t1vWXJZT8xA9yTpRT5R4gDmz4s",
"1Ay7YZoFQeHvAAVddKwRgLREce9ADHypzG",
"14PmJDWJrcpmTWH8nRJDC1LjynjDjTAuLH",
"12NqcKLovDxaPBzLgp5t289hrg7CLwkiwT",
"1ELn5YW1hMQhuZFzG1MsCLSUnaX5tkq3cU",
"16LKtcxdthrGP7QiNvPqkrA2csJ1BR9Fhh",
"154iQWUVAHonFr9Gh9SjQurZAzZtacrYi5",
"1Jo2BNCvwgGiBMKp7WuotapaLurAC7scrQ",
"17f26RdDeCDe45GAcGbsYLX4VNx8N5dWZg",
"1Hw3jrfLgZAqoYtc4q5HokrSLFFwdqtJyL",
"1HGVsrZjmBSLuBGjRczBaxaWArvo3VHMjf",
"1MXTwfZrYsBcpxkoQxx6ZLTvbjJkRtdxQG",
"1LDAyZh7rdnqZTVagBKs6sKuoX1Tnu9L2b",
"1C2NS6mNqa6BdsRuvhwJPYx7FVVwBfEXsb",
"1JCsSxHATRNvhQYJDhi6csAyJMfraiB1Po",
"1ND7CZ8N4ww5tMg9hU37ihZDX9UubMmRDA",
"1NJVM3XqtCkHD8Y6atJdFVZnCjNDvQHRgH",
"1ESNBv7FCGd4PQtKdaY8LhGPquhTdU9KjK",
"1HgpwewdaFThvn6XKBg7N4ASEjS4o9L7sP",
"15ynUqGcjSY9fswZR9Z6U8Be8KFwk9dYPm",
"15eMRwNWwgzNQ7WEqRm7zoo8auakDNcADL",
"1NinjPgSbj3qCSgPLU7hnMxooa6fF8B8kt",
"1AYdZtv3PP5mTiN6EPrhu9kxFnJy9dXKtT",
"1HgcynNaLEkMmh4u7cvWW2dyp9P9eSDibc",
"1NKyChgUgYLGW3F8aj2V4akR9utYwaswrD",
"14AVjmQFWLtayCPuwJZzB5e9VJW2RN8TJh",
"1GMjBMHPNgkTGcaYCfAcwMwM6NGkKGNG7v",
"13XsLxiP5UmpU8METxWyJs61t3AbUPqzG1",
"1BbZWKAJcXMMaGpLj43f4cTTCoJ9wdLQG3",
"19eZnhJnWJgNsrNTjvN7BTrb1CCZb4uwdP",
"1LoiQjJHT6oLg1SbTbJ87MiftJBqFtg3W7",
"164R5cP3a1CB9nuWt7VA2m4qMJRE7eiHrN",
"1NHmdGZpnXp85mfQC2Gt6XBJuEwXxvavn",
"1Gu9NF3i3w4q8TznJFvakMsfS4b5u7v2uA",
"1GuDdGPGbY6rqRCpZ9RD9GKhzeyhkroCDv",
"13ZifiTc2mho3KFbEevXF3HhVkqEcj5Pgw",
"19aF3C9Ay8UmbPR86PDgxKUkJWzD1Vc1rH",
"15FX6H4QECLdGA2wJG55cE8CQdxuF1CQiS",
"15P53jtnEBJDphYi9pQt5FBumk6NJrECmA",
"17oFAtt8Xn6RHqDXqTaLEX68iMj5afQCaW",
"157YhFcnB2L2x7mmjLh6domECfA8Gz985v",
"1Fs7jFgPRwVj372bZiYuPgm5mz3xncFufi",
"15T4nkjyrxZ6xbfQP8hJoK6Md6ffc2WDu6",
"19eZek4tP1MKuGDXbgg2uffai8VM7bTacb",
"1KTwfUyfqCZkxUxzyEQeBT82MmRSE5dzaJ",
"1PwAzFFA7uVhC9MWPnGBBTbSbafzRF9MrC",
"1QHQHz8Tfkaz4YHa4BJSSdSN3svheSKZuo",
"1DHFfKVvV1PEmf1jTye7P5v2d9rFnUusq5",
"1N8jwJ4W7z4CsdTFyeXsL5T8WxA12qVcaq",
"1LYLSpEc3ytKsNHuuTTVFbBKXC9nGocm5v",
"1PAcBni3sffDn1jTwmA6pcqoAqLt1Lr1fV",
"1tThtPoDHmbckY3xwG97H6gY6jQJmgD4H",
"17c6dKSRht5yChxTESA6W8jkVvQbdkbU8V",
"15cP7EVHow2ZVMraKd9z1W3CtYN2d1ToqU",
"17Mxv2KtrpxRuGY8HXv6AodjPW4YLUsGQ8",
"15eENrNfVmRE56rseBZmHWVeMCTsRrvUHn",
"1MVVRnU1nmSdNCkV1SMuwXTRJtrkZzKMPx",
"1PGHJdt1xzLB8kFxvwwJAgCStm6JCttHc9",
"1Fibv5u1u5eA5ojGvQGokVaPRru3re7wNu",
"1GPnANiq89zePx6FZMomaC8YwU5TJiMjNu",
"19P9nEzBxdfDWmVVDdBgsfSULCCGzYCyga",
"18goPjCSpoV98R72P3NWJrztGwUE3cf4tB",
"1KAQtvZ6MRXSaoR8xzePtku8Ksen2KXwAP",
"1FypFM3Ny6yPrs4edvjUYBwAhUueVQNdxZ",
"1Fw8Cqf3xPqgiT7nxxRbCtRjhzpJXcNJND",
"1L1a4JA5cDj2yGVwNR3onw7qdLB1yuZdNF",
"1NCC3P2bs4d2j64X3UsCuST5Y2onScsCfY",
"12vjYPZkorPKns8yqPP3w6PhdWiRBfoPYk",
"1DYmcf47onNKYE5AR3HE1ZkmYCQSTzjWk9",
"153zU1jJZcEUS42geYfL2dsGtTJCQRK9gN",
"17P4pE9X8Vf8ek6QTeniLhUMEsq1bQks7B",
"1BhsiEwMYs9GkWrZtx4L5p33CGNPJ6BAJM",
"1KxMcv2cjq2yoBaaCodsG3AWUc2w74YVog",
"1LR6JdCA6855qSteFTziZPkR7MwWDGAwPQ",
"1Ls5pVCFVYZk9HJWbRv4axSBuJpWr3k47H",
"12q6Nqai1pmsjZvZM4iu4K7iMeGQT7woHo",
"1MvJm3J7TcCy7abdxFJoJRCCSQZZK1ViqN",
"1EYXoyBxky9Tc4QbGpYgKww2QnshDsn9tB",
"1KYaybKKYiTeh45rS4w7UtP78nCmj11S3M",
"1MZfPJZFa7c9o5JRak1CuEB2NNLuGkwMFq",
"1AvotQTUAwn4Ms1QPoZQ8ZKBKxU7qDuvP6",
"1Ahhf2HbSMxJ9P3aEJrJVuvsYowKWeKvhd",
"1CQhJoxn6mkVZ4bPNLQJMZb8VFuiGax39r",
"1BfYK8RpEZjHHCzuSyzc4nnuvTrf9hJjiY",
"1LvbTEdXTSH9QVw2Mr8vRL93TaVZjKTahw",
"13XFHDtdNYQTyyms4qi2c4STS6JsUtppBm",
"1CuXVEwsvgHBz2iS9VGibRnMkGzuCPTuEu",
"1Cv8bvSdq7j6VHGYQ6uM94SgYYSetx3Dzk",
"1Bv5jrfmoCikhjL4qH3DWQei85oWp7gjH5",
"1CqhwseuStFtjfsQ78DWHBqUMrHtN4TC4P",
"1BYsh8HdYog5gJrUjdvZoeeaWw4dHsG4YE",
"1HRNo9fjvpAygteasLUZqWerQUuyhupRJk",
"1N9BEFYmdvHhLjh1fwUbsRVm2pE4fF5tor",
"18kG3RuMgTZCAY1pZcovxU2XSg1Ky5rRMW",
"1KoccozXTT1U2GGMtGgpnWTjgBodt1PFmh",
"1PSQHvvR25cG4rt61JsoKgsnV1HAe8Ga3f",
"12y3WLN6jVZ8p7V9qUCqv8KnNCDx41dyQo",
"16q5RvkQ5NRjUSfajVZnwoBZ8Z1bF8mgc3",
"1MAqJ3MQX8FQFq8VQyhhnvzEr85UMK4fkU",
"1DGAwtyCVvyMg4bptYSmgdEPU6qPD2dzF3",
"17UiKP3atJZHEEYdssZ6aDpW9BYnMpb4qA",
"13UMpCVLrsaTnWcjDqeWn6gyACQwGDXGEM",
"1ByiFhnFhKdborX4x5c12cvDwGhX4GCVTp",
"1L77RhJHX9PGEnRTch49D2FBtwwkMacRUL",
"1KimrmKTX7jEsxun1JH3fWYtSYw8LzkrGS",
"1EZ68eqevXXtAQawJtcBmxFfKyTcs4S6QH",
"1AxvtJWW4jffyyP9B3NqzwQhzmnrV82GHx",
"17U8q3ieW7k27wbwDD5LWij6UWEr3177LN",
"1JFX4R4AwHCypyifeE6cNQwf4posLEfpfG",
"1BwC9oN2DGgx4KHii7usgBhKiFiKNFf9V5",
"1CC16Sp6xR5xH3r9bJVQWsMXDRAUEGjz3T",
"1NR12LgjzKiovEFFaWaik9dmCH5JVAnDdz",
"1GWuTSBobBRtQusYXEMoyLUL33WDHpXwEt",
"1AoYvCEmWUA7UoyuN5AuZajKWDQXGwbjdD",
"13KZjnaSHEpkLeto1SkcX2wBxMneJmxsha",
"1Gvo137JccXqM91e8vJprRZtxxSCrisj6E",
"1ELi6NZhgRsL7sJ6TUoReL5vxMyGGRNGk6",
"14cJDobxiw3rK7bbRiXxPYtLZ3yEDFb1EB",
"18EH7p1dUVDpuEyzVVEuWWaUzqByHQp2yY",
"1KKoBwSBCZe8mbPzcS36dtpeRwTpHFsQoy",
"1QCdJKScjMd38vgBuCujRb8nsQeLDpX5aN",
"1LLXK441f3ZKuvtZvYL95jAERKcRYYS4qB",
"12HBedVhUPvVzgX1hVu8hHyuB8cu87JuwU",
"1JZkDtXqq55QmubKrv1f1igwNm79qoZeVK",
"19sfznjJxXiXuGHNvCny7rvCqr3Aeo7JJD",
"1CLFq4aByJaxPEhgCBa4nBgE7Xkgy6ykA",
"157eMiAZpVimQXJx4Rnheeb3dw2UQhzdFt",
"1EsE6p9Zu1nWpD1967y6RoQGD3FMFdivsA",
"18pTA5NfH7JijR5YDyZjzeGnpnarrq5GmG",
"16k1FCDX8zPnWZUyk4LYtiDbmFww3NG9EW",
"1oi8DPHc2jZNNjnkwt82nwCAkNtzdhWLo",
"1KGSrWFSLgBPHA5dCRDwRv6Q6uVXpaXRBr",
"15BDGbvMJoewusMhLwj4yZu7JnNPNquZcj",
"1FuBzGAo92Ft2mpSuSGrtKniojs6uGmk5H",
"1ERcKMGSq6zpw64eXLfVF6KDxhpoKcT9p9",
"1BAXJD6AmmQN9cAknB24ahTbyJ6G3rgKtF",
"1cbQ3GbbSe7nJPa2JhkpoK2vM5axH4kTx",
"1QDV7fUov5zPrhBAuby7gsucKcvwJbFcSG",
"1BcJMecmkqbSACXKKteJsTDConcysPy4nC",
"14bAMdxeT5YqXiGfbaz9awF6Gg31LtkK6g",
"1NZ97Nnra5wVrTyWcZRzxDJSoMqyDHkWGv",
"1CwTHiugdGnKj8gJcLQPM9jTNg5xmRnDr6",
"1BjXitUEiVZMY6CCWRfU3rSVoNhq2n4K8h",
"1CiFKaqsw15kw6xEXG4AYvRDc5WckFwpAF",
"16yZVih6aaXXNCLiBV2nA1KxzU5mqPfKLr",
"1PrX3EHVSxq3ou8YtDV1iNsE2wqDfP1PQy",
"1L4EKHBf8qKPcQX4hSR7B1JhUYHnxrBF76",
"1GYrGsi5NVZuU8AxBQwz5eiSBc3TzKASLA",
"13gRQ5Dvq5Fhc4A7bYNse17CjWdLgxb56F",
"1GjYZqAwoFrGakNwNrHdfuuUe4SCYMn9xX",
"1UU5v8ep7f9DM59wxqvShoEKNgDmQrStQ",
"1DbRFyYRaEG3YsWChV1FMy21HbxR5qzUtQ",
"1P46zWyNZjjnavM1xAfA5zcPnWFNskJ89H",
"14Xv2NybfVetjgreFBnGZdbL5MukjX8QJQ",
"1EDQhrzGcL1RDs4wqhDUxzeeib8h9mbL5V",
"1XVHH9gmqpuqhgYd5KQvf5PAUVJoSh1yT",
"18QDVSQypZ4asedvzjE6hUfwQENmUoqjJk",
"1EskWQ266WGqoimecuSNyPeLiduxGpFESH",
"1D7caBSCxAjbhiGzv5frKNN6i5h6DkJeQo",
"16bBb2i47krZMYSokKxoYW73sxAHiMWSeo",
"1GjF1eHrvA1sj3bKU3XytBCk4pjZLgXT3c",
"1Nzh3Fb4yqthDvWKkfXYuwCXEDCQRsFSpg",
"1CJXZBsYEDynVZmAR71iwbmrCJQeQdJMhD",
"1ANg1PhsotL47fHx6xarHUpvtR34WQWWAV",
"188NiQc8treFDsvDNxd4i678QG4V8pL2cE",
"1N6DwfzustQhu41nyaRXoJNYocFzBmz6xy",
"12BdxRq8hE5Af5KbttG7nPgwCtPPapb2vo",
"1Csg9hRyxJZvP4TrEYqXNpZ2bLQ4tvBnmt",
"1sr6FEokVDXhzv7WcCeJSMu16k8nKqpcK",
"14x5udbsPxjd99W7VaygUK8dj2F2nT5Vgk",
"16Fjsbjukwe9EUvjFtWp6nRmjJLcbQV84G",
"12ASHR9NK2KgXcmHVHBc3QbrSf2Cp7KsgC",
"1AVmGoyAiKW1NQqqwyfHiV7RFuLNyiHRDh",
"1LpuoYU6AWufZLXhgXdvs4mRKd147J3oAq",
"1N6uHHXsr3uq7HeKzqGV8WLenQ3kYHn3q9",
"1CotvsGokqvkZ4hr7ASW7ptDxKRmBC8q6G",
"18P8Kg7HF7e2AgdAtpeSfPLEay7NWpRvTY",
"1CXuw4Dg9Drp9sMsj9cgcVBnnAFwXkdSFd",
"1BydETkmRKkUx5YVSHbz5qyBze2DqADoKy",
"1D1K5wFJ573FHUspq2QbUgSMhGYqdm8QTq",
"1917o8V9eGk5xwZYnkQ1iCrNWzd2QSo9UE",
"166LaTkZX7QdCEFFTLGoosYpFoH84aywTM",
"125UsXUb5YS9rddoTPZUycGEeGcgMrJ8m2",
"1KgLnFhGr1FwTsU9y1m8CyWUkKmrhGFyjZ",
"1LT4iDfnmxrqgi5snF8RwZAdAYrG1VJCps",
"1146XvKdpMdsS3BTpGdUvVUasBEn3FwC2b",
"1CNxngCcoUKBAubX4srhfFT5EwPxQA8xc7",
"1GeiYYSte1NJHf5TRNTwDRGp4QKxtY2dcX",
"19Pvxfw2kFobrwy65EHHrXeF4FeqJuYxw4",
"1Gm7YucLni2aJQM12RN5xukreAxqtreJdF",
"1MHuUHKd55GDbhbPnoPj9Zpm2o4WZ2ot7u",
"1MPTdH6oyibHBiBrHgqQet6Lmo2bmhsab6",
"1MzkrN8TiZV3Zg1CkBgbi5JaFK1xCQUb3t",
"12meTdNYsneiH4LMQVqUT4u5YWCPYkXkZF",
"1KbeUh5jryZBNyk9oW4rMb1HLzxZrSPtNF",
"1GM1BRXLZJvSUHQNPaEGpny72pbPB2bpku",
"1HBDFm67Y696LdUmsHZjYDXxnhXibAFKaW",
"1LJ4CTSHGyanGwjg6jyZcxtsvr3BvfGViK",
"1QCMETRHD3kM7iDRDmu1t6b4a6GYRcbH1Y",
"1N4vr26Er9e47m5BmpTRADKuGJacfGGZES",
"1NuSAoXMFoRygs6Lpc2CqqaPM7hCFTfmhw",
"1Js7VdZtaJhuLXfZ8U94UWVJJzBEZ533u5",
"1LJZbUz9cWGt4FdMZv71YTg5F8bZyfYqWd",
"1PZEDMsYzm46sJb6H1Dm4jqaj92zAkHkbD",
"16ZVGqaahYZZgheZTpwVzyou4mUpQmetEG",
"1PXzkDTmkuSQ7wri9uAVkFa1oocZYdRVLw",
"18kLCx1ssFPNJaUyqh2Hfxg93R5tnT4xDw",
"12g14KkD3pp44VTUkf4GufUkwscgQCqfyo",
"13QFpN1zKxmok68U7GPP5Y6uhhs9GvrJ5V",
"1LC5c7qnAGirqE2pj2zx8XsHRAmtX8EttH",
"19yRc2kdDrsm3KibVyg7QBZBo3U5Kjknku",
"1Pq5h8LL439RjdmnUNTCAKTT4i5KoNBNNG",
"1E4e6zDcFUsXUvwRUrfpqpDrFhXVDG8CTx",
"1FvBWdSFrURDK2EWdX4FkmKDdaoY625CGQ",
"17Q1Znu23kiXd4F3Rakp9QwZdwCSev6Z1M",
"1Hqr6DW34Ys26eoXm2MCdBzGaHB8P74oPS",
"1Jcoyx6y7ehW36Z2TsH5WSSsnMCrBJEKpL",
"14KfiQ2vnvr6P8nw1VCpwnNJbiXdndmWpZ",
"1Ja9hSTUESQx3zxWVh7aa5VxgLZhsFssz8",
"1A69U6CYnMkbJToTByh14gB6RoPeeMcau",
"1MAHH1MqAs6yyqCedC7rTFTsJjGtgPybog",
"1KTeyCNNKyC9ZQ59w8JZRgGEEvRwxK1fho",
"13fHhBazjuFtqt77tJQDXecuRMGCXEPiry",
"1GNVfa9HxeLo3UnEGzwiHRjH1Chf5WYnjt",
"13KBPHv55o3ZsPWxVvaNGTxxy6fDXngCXd",
"196dZr43EvdM9XMJnipkzJekPY2XHV876w",
"1H6kY4vyWWL7QiRTg1wRqFmbSJoDG7gwub",
"17tP4gmLKA7YNCb6G8SuZWvEBKycjU5t5w",
"1KGkkELXy41JECHXFXZbTNskVuVRJdJXEg",
"19UDvy2njYmfzo9YSiJqoTTaPEEEnJbR1t",
"14q3ndXpwtsEXm1DTzZVJwNp9p9MN2XEHy",
"1G66GrWVLG1zRpZQaAiypPNPgTYqJRvZoB",
"1CRn3bcsr9uvJSn4rFpxME9kh8Egs9Zvk6",
"17umr8GuY728ygYZRx77jvF28b9XGMVJs3",
"1G85hpqWYUQCP55JHPfVRGfjrShF6vk4VW",
"1NTmFVEnxvd4UkArdy4eRDb7nGtTzoNUZ3",
"19CzcN9Z47jyhSipD821SaYGsp3jR2156E",
"13rVPq4LSP2awB22Y9QR6H6XPV9hAQVWXm",
"1NPE6mZRixajSEeonVfzm4bBAtyVbMXyPk",
"1G4VhhfEWpkQAo9p6ur8NT6efKhBkUyx7r",
"1PFN7hEEgqDyvS4fxoqoMenDXNYPYjZsxS",
"1Eq2aKqstz2Zw2vSmJ4rKUgjfUFzD4db9B",
"1NJCpcaswAZAtHbhwi6Lt8pyCLRJ1AuiDf",
"1MJYozyL4AeuXXHD7zLFAQka3s64jFxn15",
"1PDsZnE9Axo48hWbKBQEesQK2Hu1CMhft2",
"19Er7Mj54n4RifH1xvxBADbfbXuTAHtkaZ",
"1M22CA9yjUXjjDAnTeoABWeLYrsRcQzSuh",
"1PQ2yPB1V3983UhwRdmmHoiWhEsnKuCcsZ",
"1EoWiCBCG5H8dEaLqLyCwUwBCjU7hs7tzM",
"1EfhEv84B8BWP8RfZ1dqNd7kQkmGySuyGd",
"1M5Gsa4RjEBKigVcxCNt6UoDFfdCAFoNMJ",
"1JeUKQPeJs1qQ2q7wqdnZ7RSxk3KiSQRjC",
"12o8Cu6M9eZQoq7PGqSE24uGgqg3wmid1H",
"1B4uyXUKsj7HpZYQ1MSFn5KJ6kpvoE8Gi1",
"1AcTeSTNRyR7k24MhRWiSs5pBVMNszesLS",
"1AenMvKerygdHu8eUoBKNuJw5jiMC2BFUv",
"176Q1pVPB6xtdtLNHQCkfa4dTWSEJ2dbwB",
"1Hs87oLbWUfSJCDdzPrxXa1W3JdPyG1fej",
"1ECPV876G2fp54EGf79tgdHBHyUnpXm6qG",
"1Dwes3XF8CLKuCBkhHXUAzmJL7cmUw29Kz",
"153E12wNrJWkn59i46LcEZhoGZ9JvFY8hX",
"187TjvFvfMLSKmUgKXtc4T57KdDWRVs5iU",
"16BdNwckhuPEjrpzdXbCmWYxwbEzk38Ek9",
"15NLCbBvE4aUEB4N8NfgAKg1oyjS9YjBJm",
"1HjFPy27F25y7HiVjPZKj72y6PX735yBcx",
"1HYY4Laez2cYFjuRcy35gAssgdb99bb3yH",
"1AXskDco9oycsrhx9iCRYFJpYCMfhpE2BF",
"12UxfJBJdE9enzY4xPpZ9tMXC2EAv2jdap",
"1Lxm2cMrkDD8FYWbgK748RWb4JJ7eSDsh",
"1NDTYbRH1XroCKGLGSuJXmpvW1r42XxgpN",
"1NYhQJLMo63aXoiNF39BRJxXf8myaariHE",
"197XGJe8FLRZtmrrRTJuZiv4PXi6DJDq7R",
"1GmW8BXmLdSgyLkyYohUSyHX6Gs5Pa2XcF",
"1FqiZ18JQ1TjGhbGgyyxnhZeqUvmLUUL21",
"16bdJ3pqZ3SL1E3v65ZF8njssSPqKWCGcG",
"1LUnuJ32PAWEW8Cns9YjwV4BcGeNDjxmVY",
"1HZmPW9Ci7qh3yD7Uny71GbscB5yg3Btnx",
"1CwHU4ubYFno4axjJ2F8gvnzhw6dXkBm9F",
"17EQ4iLvrAZwT8kBfDti31TAaSitUd8mf7",
"1FYRTMxBZAh9iQSBUywCtxzFFmvRdBhRkk",
"19kDwUSVfApufVtwoGiwLCeoW8NWTpseo4",
"17SwQSxFyUfhHszTFpQtd9kj9bBwXxcmFe",
"1Mds1ysmaBwgK4SsHBFydeXQNaWfmsEKcm",
"16GGBx4m78NBB52EyLgXncpZaUxpKjR6xG",
"1DFPWE27JucxqaD2Bhxaea59F6u7vyVGFm",
"1ECsnmHjXV9WAcgafwxhf4ar2B7qfP93LD",
"1HX1umfCxgPBFGrkyRE92nDffmSYJ2ACgT",
"1ESXvsz5Gvb3DYDHj2U7Xn2P4d84XmRzgn",
"1MFhy3ProAswiZnQWCjBapBMQRbvpzQGr4",
"19S3JWK8GaVN9TxDzcLggtZ1KdxMX2kWqG",
"19qSieGCwD8dnCF5fwrcJe1QUmwLb9y8AM",
"1NCxVsEu6zax9NCxYD7DxwmGkWYwNuuh53",
"1LQqk9ZiRtGMjbbVFDkTwVFEdUna4GXrpQ",
"1Bz38jFXbHq5FQSxrheT37NQudqz1ctnVN",
"1MHUddKEMMf36mpewpgfc2xBjANSc37Ru8",
"1HcpT2wna4ebbuvpYgnUwJBwZhGzFKPU73",
"134wCHEicv6xbYeXBWuh8FyufJQUY2WuVF",
"1KmFgjm2meEPb6aS158XB6extm3cUybjZd",
"1E9Z9BdKhfaLS5yfU5fCbZwvXJmJZBPF3i",
"1KgUo2FxrbEGjxYy2e3qxVZtMK1UaBKRoU",
"147HuWeJxQmK5svfL1rdSsmJ483ZDFHahp",
"1BAUEMsGRSHHCcwCPDMjJHT3mkCjfGWR9S",
"13BroA3UCbA2qwiMLC5rvTckvJWbaTxq7c",
"149KBzjgRQetDFijfy6fH1MKNVqe54Awxt",
"1EwPvbJ2S9kQbg9xvoG5JaAiZJ5jydecQX",
"1sUwmqREVz2euKXGJtKRbjTF4tGHtMS9n",
"1L3bumzz5St6Vv5FqovJmFUxancXnx6etj",
"1DxiNuhcPibgfW8qvBUQAk2dpKq7fwB8iU",
"1BG64fmC3FUfH64ZFZP6tQbcGaEHFHQUaw",
"1K3sU6RE7eTinBSkHk4wik2XpXDPkyt8wX",
"1EFpJLDfPJmSuaaLXwpAguUiKN345srDSs",
"1NpSNxwXL1wD9K83SDdQaTCDyyKYcixqVx",
"1CFVdyA3ChYTYYZTzaoSyiZGaXwgeD7Jwn",
"1MNdgC6dmqYtaktJYZiyPqRZB9wpJ31r2K",
"15wJNJvenAm55ELh4qUuKdxVDiXEtupWbS",
"1HBkVgcvf991HnU2nRDw86Gz1pJ1yJRZyo",
"14yMfVHbYQmsivFjbuETc8FxEdJ7w1gx4q",
"18Q5KWZMRzL7k9P3Efa9GQoiJBiMtCcMam",
"1JBM8Lc3UrjZxaGTKZk5gQsCkG2S1X95cr",
"1ESBLzbfVTvimbtHRA4ysVVrM1J28XtN4Z",
"15Go8mQrmPhQCZPXLQCJvgrSmbUGkEskq1",
"14Eks35etS4wXj8p4YoGhh2wvujm9xnoEq",
"1L8NDsYY3p8XSW4FVUswedMmxHKrSeTV2k",
"1PkWNgdYo1pQ5UaQcpZt41RFgWXk42RREZ",
"1C8pGAf1FLFJVpf8aDLn1TjHHfeRmKDa34",
"12aqdCCRYxAxhUs3TkTNmrpVAxMYebFVky",
"17F23BiTrrpErYFvKmDtmcWESvJgiVxcHj",
"16ihosUZtwqyteB3psRyv4NGgQ8JmwTQSQ",
"19PkUch2ena4bidnawHDqFnUHvBUEcN8At",
"13W8GxiSAvKTeAqm1YWE6RgoxpfhCkWs7K",
"1M8bFZj3isyzHwDqEo5Ex591oUFYUQU4jr",
"177b6PCKzCmWHbq3XuvjM363Gyr6qpR8Jw",
"19FGRZtBfAbsvSyxYSfDnSURMU1bhcNSSh",
"1MiG4X2hGYsKAm6vaECE1gxPTcpEUwkZQ2",
"1DxBN8rr6x9EHzvXZDutGdJmBgkfE3CuBs",
"1CHayn3h24C4cost9tzBfk6n1WyUqZdCno",
"1CuYT7kmrpdCaij4E3HW2mtkCK2owaPaP9",
"1F9C1iLbwpKFnSz5mTUjDvMFq5vrUJspwM",
"13HDSkF1TwGWZLCfWjfwDZp97VYYQ7LWAU",
"13rUzn6ZTdXWFMbNLArdBvGauYkBbkuHts",
"12jF1eFfywSd8XDCoConBeWZ6ZEffwFMQ4",
"1NqayvBwp2f3Az2P9c52HhQnEGSPvMsM7n",
"1LMjqM3ossNqTXJftSVqp5jpoKrVnhHEZb",
"1L7MuM1CmMFoeSSF7kptUpZwHbLeu1VmKz",
"1E6JJN69bU36d7Y9NjK69nmkEWRsG1XP6b",
"1FrbZYtYbZwpuKwX2Wio6LGrRoME5xfbq1",
"1EAkNBjaxRDkv8VW3PkvQ6WWahNgDyrAep",
"15zxaZ8fZbCQH1rELuzCzJTqo4uAheeLsV",
"1JrnmPoNH2KPVAWdzrbgV7Ma3wLk2uPpxM",
"1CGkxHRdEsFxi6MXNJX6bsNNqUUSGaw6nz",
"1CgPMxKw1By85Xi588jWfEsVq9ds3X8ZhH",
"18wwYcaCxcUMuZSn5Ed4KeYhvLGszdnjf2",
"1AkrvJ21VCpmnVQHGH16NUud1cw1fVyVvk",
"1PxNVvHmNdZX6nUMCyhQsekSKN3MAB4M6y",
"1P6RcmEbDo3zTAdxivTdGMRxWUeGi3tjuP",
"1NYTMeBvzAbNJoxZof3PJvzr1nWDqd9Zd6",
"17f9TaEuTsF6eeZPdYkhNmR7KE3ZyGXuSU",
"1GbtrpC66fYsGbajaHumag8JdBfFXRwoUu",
"1FYULJzvkf7SnT3PYfjn7YFoCBA4swka7j",
"1DExgWWSRudgwxPBSzmbk3VY3hh8CvHaS9",
"1FMV6YTCY9eBtUGcokyCpzK2mTsa73RLQu",
"19Gczdr5EJFakftKHSSDswzUKs5dxtAtVq",
"14f1atnTnUQAi2VGENDiXv1K3FBpx5gwcA",
"1F96fQb3LeAa2sp3jKa2dm4wnD5RNt35iF",
"16o9bDSe2M6Z73VYn144gLtj9bYnc4C2kN",
"12M7yM59Qn1ib6XNizFQ3ynd7awfNSDeG1",
"1Pvt43MdiRsigyQ8t6qM5x97gZWMagdZoR",
"12ZVSLSWkBdu2VpgPjr4cVdJnZbXR63baK",
"1FhqV71o1qbcDuXHkU5BEGJzsaWs84fzZL",
"15wQKcDdxECjdHZ4o7Qxtq6qh4uBkFWHFy",
"12eJn2KFnBVJQASiXaN3MdP3Yf7i446H5a",
"1Bur8SZ9vxzzndSrg2LSszosS7ahEDuxNe",
"1JZW2tENzgEtDSfu1q9SNwR12tjSb3EWfJ",
"19wqU62UfFi48d1ojqeTjviRMS5QHsMnit",
"1BKqxHKDNkVEJ1aaVPNY3dggngtxTTKfaX",
"1HnirphC8sYFbdWJJi4h8iwXMsqoQ8yed4",
"1Ny5p7Vw8xpB1Ju4mqA2eyVvycVWiJxPHG",
"13nrcci3EVQRmtGTphQRUoXkMRTnpfmXd1",
"18ehCTGCWm5YoDVefMBFWg2jeTKSbYM54R",
"195D4RmoAmpAc5DZGJx6gt3Pf7CeDdfMCL",
"1K1BgTrqwJpWBRuuZRHjEnH9zsjjEHViHX",
"1A7sP2gsZvEutAD5QxsVnZgPeWrxscWTDi",
"1Nx6kt3GyvMcunfXYgZiSkmXHYi7kdh9Ut",
"1QKn44DeW66S3ofqs17E29oggzcHdQt3i5",
"1JRK5qUePLdVfatNoLPt99WbxDbMx11PJc",
"1J1BZs2Rq1w1FoJCKL7nJ8i8LBZEr2MwY7",
"1P6SA3hqZ9LMFbdM51BJxVVeBYmTYBck5F",
"1DBQuGmqcW7xVy6fp7TLaAHHHNK2BjfkvY",
"1wp44i9CuTRo5dRHtEtZYqWpFNVzDD3fy",
"1DbmVzbwbnwdFjv1oGGbQDSUq9sz3jsGy3",
"1BHh7hM12xUfaZRcdBRsRgVZsJvAoE3X8z",
"1Fyg3Rg8xhME1g5Wuh7PANvRz2JuyyFx3d",
"19a4QuAGdYYVBwDMUSsW9PdyRheWCzXwp4",
"1LY71J8mvaBrPo7Zeigb78D3xXoLsZrmuN",
"1MMPNBPPY89LuA6z1UoTx1kXg2p7WGv4uy",
"1CcWdJG6HaFJ1zRb6tYqcyUE9jED2AaW2A",
"1D6ea61utyLpfcvxmVQicuEyn7bNX89zfi",
"1PFKTNyp6XKQcn2RQXo5exSY1hAYcS1KqK",
"13uoHfQcF9TLELqrTUgTyWuJfa1LJpFxWJ",
"1C7JSWacSmiaBw3XwvcXgimmXgozwv5VBp",
"1Eko2Jy3hea59AnPmwx354bHJQjfDaCQ3R",
"12oTvQjoXmTA6dkrrT28M3SV5DxkHHRLtu",
"1JKyMCGGQWGFBSiepkF2aAAz2TfNi9iZeY",
"15so743jtnUaoGZC3Aa2u74nAbmZu1hEr9",
"18y7tCzw1hCX1J258Y69nPMV4MFVsHJUdo",
"1Kh99yGm8zfc8kv5UdCnQukgaS91UQtiA5",
"187NQq6mu2M92q3kU195Vbba97NJEt4deb",
"14y76ZCGhZdUsv9aPDWLSdnzyLkeYWJRLN",
"16AD3v7L6F8yHsYCtEWXYAtayrJ9FXXszz",
"17xubqfHPbgHowjq7wu9SihSptCk1J9E5q",
"1JJC2ymVm2SZHjAD5rwojFTjPi7s28nMue",
"1HvrEsBBNy3QyuEcxm7J1NCmCNVgfv3ZQm",
"1DfJoMkm9EfE6DU6afTUbWupE3pq79HSUn",
"1EyX62JvgxbwnrNnkNwrX91Ym3L7zTpyR7",
"1G6Lu8jeyCwcs39QyZMAFzQmkWEaK2endr",
"19ARvGBhFwA58dg8riquVqtenDMcPDohZx",
"19Jn6UDr1dqnCN9SgMFg4PAQzpYAvNedFm",
"162majSwgD34kZ1fuMJB8GYREsnCTnvpk4",
"1Atyk5mXiccSpHMaSWxwfXz5sXM4DmtNJc",
"1JTiqxZ51LQ8WedMpQ1g1KrsbUb777puPp",
"1NQczXqvRURnKG3vuzDoTXcn2vqznsMM9",
"1LM82mixAvuZmzfx5Tbi9e35cNHijq246h",
"1GMgLU9rNCzcwVfUJdKz6Efue234xxrY56",
"12bPry4xy3aRSFBLhMMoGBGnZj44K7gTzc",
"14RVz6eJmuASEkF9bvLzzpazRnyLzqfDX5",
"163iJp2wqXZ3tZuur4Vwk3xc21nZvMrAP6",
"126hfA3WEP1z7uWp6V1hKGcEksG4yZ9ibh",
"1LsQpzdz8pocE13aCHCdES9r6a2Ma8GkKa",
"1P4MGvrPiffZUf6py2MZw8vtmxiKsfHv3o",
"1BijThaqeU242Nb1BQpSLPDKA6QL5QK53B",
"1Fo3W8MGvc8Dtvq59xnPqfRzCkk1aPTJ9Y",
"12V8bNCWS72crNFAZYZ2qaVdyy3a8c3p3R",
"1AR51vRZxinBYT4rqhpNmsiEvavuNYi15m",
"19dUvKiRdfc8NAbwyYnXBzd1dwy1k1exDL",
"1K8TgBi5hbPS3JNVThkpdwkFYZFzPyqRFj",
"146D6GwA7bsYNWGt8axY1x9xjSzQf3DaZf",
"14BdDCASF23J1nDbeLkRwEeA5AZwyZDAwC",
"1AgxkEdVVN4vhY7iQYu8eJvEv58r4wT5SV",
"15gApMiNGiFHyubLo4yK9bfQkiDhRr9PN1",
"1JnLuMY6iqA2Qh88rV8pA1t863HRDg8Fc",
"1MpG12iTvWcf98YnRdXjEvuhiyFGGRzivS",
"1Pgz5iwK1U98r4t4YqGpiaZVK1Hs2274Eb",
"1PTg2rN6ckT91pkY5WfZKpNRfQ2NsgaHac",
"182kkjJo6wLfmnS8478Vw1GhbCqwqbFC3i",
"1Hm2H7y13jo4NHzmeTCgsw99jvCN6r3crL",
"1A9GDQCSCveWBAUu5xXQxUR72zPDK6YyDU",
"1MYQWum62HmGX5FkR5MEfGnN8FGjeTxM5B",
"1Bg7w77HopcQ1ea5c41yo1Xhiy57UFDJVs",
"1G4Jd3D9L4HBg2eRCDmBd4iw2qbanAqmL",
"15yzQ6qaNwJP2e4tHB74PM1FxsBwPL834R",
"1JkBmS9a2jRKe9w71qJTtMwAi78UCPdYbc",
"1D6dLkSyx3WkrusDN6yyMY1AmVFqydqcRk",
"1NAyTM4C7PZYS4imN9Bj252Rt74TSPekS4",
"15JnrjctWtXdiQaa35WXf11DHxyqzw2aXj",
"1CqDqHLeHovkDEd2eZEvDxUXR8WiiAkexx",
"15hu33w4Zsi8nuG1AEBqYaGDuFSgGoFmX4",
"14sXZ37pSGPRrra6wn8JGuHafVPcgtEFd1",
"1NT3L8mGxFeyVPu4nz231PDHnVkcWMTUvP",
"17rU6JiaHDmPMq6UMwXeSCrk6HA9pr4UzN",
"1PkEEJXagMijAVuspLWbwMCMtmF7LTQsuz",
"1NsBHpfZGMcZ6AmRPcV4bdzcGSHmLJU1UN",
"16onVVubtsC7Eb9GuHnAzGTGmeSFH95JBq",
"19AQsW5qfJccsAfA2dAZNxubfYYvmunwYc",
"163Er52UYEWRwMCaXX8Nt5HW2fUL5rrR9z",
"14FZggmWZ8LNHgYycDjfvZDceFakBTLsFx",
"1CPNH3ebtB6EbFKqQCSJpwYKpGV2D8hAZG",
"1NRoHowJQrhmbVY6KKRdLb8jMd74DMBgSN",
"16fv9FDgH9aRjYvAuNxy3NSvRAgGFhZY6z",
"17cY6xumxjao4LGXRrQixbezszCoSXpoqi",
"1CfvZmaWB7NMNtN8eKtgPnYALYE5q5yZbR",
"1LYfqANszmTc2CmJnySgz3xvUX6fizKESM",
"1JVWnnTisQvZdwVwBaygn16ANehPZLeH44",
"1AhLp5xSa31nkxuSp1KQdS3mvytVTRE6jN",
"1GrnVFRYHNVFJQdvHUreZyYbq9cLEoekiT",
"1KgG3uiQ84rUruJqaLTUZgqeM5ZviAMhhZ",
"1GaxfPrSvAYmQXCMSx5fhYPaD75uUf5UsH",
"1F1XnYTBNKqsVkwHC8PZEG7EVTpbLjAthW",
"146VUTdWuW4UVTed1byjqKvH5FZMkXSxC7",
"1HwHeoaEMWLNAtUEhLLn5i7ZUcRycBNYML",
"1JCQyxZXsBo9YsCYrX6rhPLDeZhRrEgsnr",
"1MpBoagMaQrAa5s7PBSmiLWB896t4zR2nC",
"14YMAhhdmVFxjiskF7stYGHbGhnU4aRMX2",
"1FARDe3Zw7LEh5mwi8FEzYfBA3SqyfhorU",
"1PJfjsf9qhGBJfKbWVP5TgBgka3GdWDexs",
"1Jj91BG9TqeqEkPrHTqgo5W27cHRihsvrf",
"1BfFTe2xWKNdRc6dzNh2HjMryYd1Qwrr4B",
"1FEJYcy2RLLUfsaSUENMQ61eeSnPTxJEJk",
"1BuHjKfZDiEcFocRY1UQWPeBLuHNAfV5o4",
"16gg9YiRUN1aJyHq6FPkKwgrvFdE5jkefh",
"1LCd2qrjrVf78btAsAja7eWJSMRdnnhqnz",
"1FhLxn6ue5xSjvq3tS38XkzsADzqg837fJ",
"1661G7Y7Gf7AXdddqi2MFqbGki572ZBQ8n",
"1DA9vUk58w9mer5ptmMx4p9tiqY28rRzN7",
"1MVXwArTmb2HxhALy5A3zzqzC1CtnZUtYA",
"1Pm6skPdKwGdU3FApEeL8FMa6dS2jpvMy7",
"15GpRGb8kkX4DQTn7QJ8t1rWY915eLHCkZ",
"1NmTfTmcGat2oNefTs8pDokURqMWJdXMrm",
"15ZzSfsRUGBbwmy9TxVqfQ8gFD1yashiU6",
"13HqJ6nroSqjSkBfmvt7CbDaLg7HV2CYkW",
"12ZGiZWovYUpZAY4R3yQR1MsQj9FxkGcoF",
"14UBsJ32G8X8LmE27vk6RRn2VJUuZzRRBY",
"1JZtwLdHC472nAv97BpyCv2t5hoActNDvx",
"1Hj8jwwcRrHKcvj3xuEM4dfMib7iVjzW2h",
"1AuyQn8UUhbiuUdycyZuTMqA9cdxk14XVA",
"1Q7oMajjRfoi4uZPb8YyTsJrykCPPbWmrV",
"1CR5wbvrVx9mgkjvmiKDVJViW9WWAWR8hF",
"1Dcv6woG8UBBgSHtqwjs9zpM1PCGJMyn2n",
"16xrtPLixr9quKzGQxKnJKT4y7HWJF3Yo2",
"1FcxZpGZa7JXi4caLC8Ap4bkMAkp89XRS7",
"16C1zjQbofKw4g3M6kpw8oTBvYvQb8LeML",
"1BfRdTTGoVgsEFbkrr7DfKQ2wtDDmNCip8",
"1LizEJ9k3SyaJLthGQPNqedvsKuQqu6QYx",
"1DkZQY2E9fHr4VCxinMcGDheEJ4ZJJF3X7",
"1LL2Un9zeaDMdy4dNHngvAGNFArURu2Xwb",
"16Q4NMGvbkkx9TBMNvQfy9oUSBGpeWaQuM",
"13AMzuHmmeXE9C5FCpS4oe9PXidafeiVif",
"13fF3xpn1MhnS83MSK8fq1uodn44Ps3eQG",
"1HtAaytmRBfiNmgeGLJAShekGWkqi3cvkC",
"1Kjg27hXaDj7P3VdLWa3GsTfh5PYBvqk1f",
"15MVgCgmzhr6GnoGB1PVDHPrZpC2RCCfms",
"1Mp6CHei124HYPopC2Lfawgwg3UFTVaPpz",
"1MYsTcU5mMiQytkR2UvTrWUDcQiWvCPWDT",
"14ZFen713DRN1FJt3nX88UenMiW6FuKNBS",
"14A8B8LbXm47DSeiEtPXArG2Un3qHaiogE",
"171TX5mmDXNvHtwDQrJNN5FtrripVMJWJt",
"14HYy8dT9BAHU77b1XGX2Jw8SzFzwTHpz1",
"18drhXyExc8ditV8tJqEMzdArmiY6TfBA7",
"1FpQYr7qtMpgFBF6UzR2xWhhoH5BNvBXS2",
"1Nzbpdyd9t53dHecg621f1CktreQfFQMV3",
"13hbc4DcVrxj1qRaChSBvKFfNjZ2kjUUWV",
"1PNFmRAvhH2SN744VuTTeNkiBfS8TWTEGR",
"1MgZ3zRinZeiKSbKjfcYYxrBNpfeuMxkpu",
"1JF11B5VQFtVwjsnbzb4sa2mVHd1Xgi2CN",
"19Bo4VRAGmzSpBNFWHTxxxTL7dkAyDXeir",
"1KLxhEcNZy4XVUrgmRuxsN3NUUVHTePUu3",
"14bBcwALcMMNxcadYNitczq2x35aTZereB",
"1Lb5uFj1NBq4GKgqQiU5wqp8AdanTC3LUi",
"19kTD1MFyCmktbmDPhqBWNp14WL2w2Eipq",
"1Lk3GZEK9rvZaT5sCbxyEBVTVGfwoUzYTX",
"19ZGmSjWJXSevrjk9mZnvj3MEboBMPwSyu",
"12n5cn9mdnFfn6DqwzAoBbxCqqohuFpn7w",
"12bMLzZDUAqEp1KM2nNEDUAh4qHgmWrLtn",
"1PjLSvLrRAoHVpBjDytpzaRR5VaHUKHt53",
"162XziZqdz7ThDXfeyYRfCEpHPbGWCLQMo",
"1EF28X8FUEGGqoZTcKJyYiDxJx49yLn72c",
"1P7NQN5q8FH6NWRBh3J3Qow9sLAQyQbemP",
"16wqYHhg32UoC8uK53NLHrz28vWmnbHbA",
"17gvyYeZVFVgyRdKjuyMZtQVfxU1LJGcFV",
"17exH5RYoRnJzJwBq7nwnWVhVFaZiJxHK1",
"1E52vMmesVcAi2cewMmEk3pPSEKuXnge36",
"1Aun8WSeufNqG2qUd1sWSKWoLyomrVGiNY",
"1BKfka2nsC63y9MKP9VMj2UmMzKFC8Fhhq",
"19NBNurjiJr2TPCVJgEYaiRL2DfvpoBQjr",
"1AByCCwb3qYs1QgqRm4ApcdRcdRAZmtJ3i",
"1NUAiuB929Lbk5rSdTC7tgY2kZNbN1VtUL",
"1Gab46v8F2CuvFsKe4ADC68qDaGGknznyV",
"1G93B6tjLTsjncHE7jTzhG9ym4E8MksYJt",
"1BJetXGzjUGW81yVVCNLRu7LyzwJMmyfjM",
"13avkeLZruH2Tnh7LncGVH5z6NpKkMpMob",
"1HmtHtE9kUEM3bcGjeXz5bszMmbuJpzS5X",
"15JMg3SMRofGPHGUS41nSqwnXChN7cf3Ug",
"1CTicc49uGE95asBwhSrCM17Ery59tfvYN",
"1NSNkzdN75XV1KRBTT1GumZv6q8cP2TwqR",
"1AmHdT7Lo57GxS6oVvQDvZJukRV1RHkMcT",
"1PpwYVmjy1u5HWDQiARdu4H4FJanrRyz7C",
"17w6ST4uBcb78pKuu9aP5D1RrTLZUZK51i",
"19EiemkVzuVUq7xN546iJejyvnmSbkRRfC",
"185WR9sH2gYnRG8ktVkQjXbM1cRGLpQwEQ",
"1FihQKpWwyKd346GP42Z165jbXc3EGvey",
"1Eo44PPwPpxo1fcinBqm4Zh1sW7CWqzNUY",
"1F6xzPqzQjgTWy8nUjLrLSoWyjX6KEAYcd",
"1721akKnYMnNdcnGrgjCBNJw2nDTiyzX8b",
"1JnQkyjYLDsCd998Pfs48HLSBkmFKJ8Me5",
"1HSZvngEPfoJCnG38qnq8LZDvyZbU2aegs",
"1KrNvKeS8LXTeaDpVHFDbyZrCpo4GkzB1n",
"1u9ytWDPR7aP6qsp8WjtqHAGiSsXWqAMN",
"1G73exekzDuGowdYvbGA6jG46L41XiGqRP",
"1P4LaTzF2kN4Yf5Fgbx7MAqeEosU1QMorQ",
"1B6ktY8ZaPj9W9YMqfQqf8QFKtimtFBMjF",
"1MmGZrQ3VPqP5Lk3wnj8QeVBTXP7McLPQF",
"1PRj4WgkXroDJnutNJ1oPTxKieMW7Ws2Q3",
"1EtmhunLnuqGLrxtB6b5gYwQAjReCoj63Q",
"1KirkYf7cKnHF4DkMSF7RiUZDxHDMPdbt1",
"16r8G83y5gAr3192bMpr7ruoQEDbrpyJaz",
"1FyhDFBVMt62uA4UYUfQd1fMQZa17rSmAX",
"1EQYbbGaXAac5FQpmLF9hwjdvY8GkBZpmC",
"18MLjrSPoq7vdGCAHsQj7ZDhgLphQoPuD4",
"1Nr4WFkGC2878rXxEyxHb5R9scqqLWTJ5R",
"1GDtzra4ABeBTc5XaN2xi1DvyqF2Pxc84C",
"1E8LuP96FwuW8UBmWYsUcbk34uEvppkzU2",
"14rTVAyAmZ1oXzfGC8gj69fxb64ubZKwcQ",
"17tNE8AStXvDYsgUBXaCYu6TuPxBFhkrEb",
"186EgDXvE8JaLh7CbqNME19JYCoc2R45Zi",
"1KY4m1U3VekGosg7sr41nwLmVgZpY8unrW",
"1CdzrYs5UDAPik5Ak11D4xwQ4AcVFAzYq",
"15RUDf6sSLkA8rZt8y92AZcHJp9fZumtnK",
"1Bkxz1JheM8owBjPGW7iYc2zGe5me7B7Sf",
"17SVzZWotnXu6SsR29E8e76K2PaDP6EBg8",
"15ZiGTnR2X1Yvo9qmExpZj1aNrtwDF2tP9",
"182NjpTxAo6H4PYyoAWy6pU5SQdr2594jn",
"18oed7Kpid3wu2csi9S5as6y32UjJMq3gT",
"1BW6gQiRJ26JcyBSsVEkdamqRWoJVDJuNM",
"1DMMjie1xCH9dojr2eDnueT5wsCV7XPyGN",
"12kV7acxHdQMNahZFN37d8Kxy7NtTPPkoD",
"1BMR7Zd8fyhZprbZATZr7PuZ5W8EBYZtrD",
"1Ezxe46SRqVHVFqauKbe86w7x7G1KvJamk",
"1ENk9vrBFQKMoUjdkuau8JMuZnaTkJKshM",
"1E3Wp6Hjrw1c4sQjPL6sMEwemFx4AwQuBU",
"12JyiRJJz42K5FW9bRdWGWdhTGgmSdPTdo",
"1JkGXsY6syc6CTEsK4D2QL5LNiAYu6r29C",
"1CzMnakX8CTAsSjGa1CzAYLNutVh7iaHjt",
"12ZmdnLZyi2mMW6Cbvs5EnsB5gt938gf72",
"1JqM3Y5xCVjZWHMmerS3UKq26nVA3wMgiA",
"1NJCrrRg2AZ5roGVto9dSqaJDDioDwmz6e",
"1Ajg7biS6sKF7Vz2H1rpYKZWD1hJkgSTsR",
"1s17bwDaG4VXwbwoatooQNQukiCpcsdEc",
"1Up6WSihfFypNWocmXJE4kTsJx2iricAn",
"13B3QfDahYpAdCbB4oyVj1FNn6Nz2cghMU",
"1K73HFPzsiSzeBgYhk3XrsWfnp8Tuw25Y",
"1CdNQTf5g6AJawss7aTUvGMXJy6h7E7qq4",
"1GEr49ERq9fNdZMF2V6Zk2v8bnynnbPVFQ",
"1KprB4Nu7NetH3bpERQ5XMoVPKusEcAyGJ",
"1Cj5Ce7jaW9z7WL5MFt6abwfEKJ3WZQKeN",
"18Lfjv7SucBUbJF5zVCRnTrUt3ZAUeaBky",
"1GZCvPSA9WChFNvT6MjTqkMqj3thkBeRTC",
"1JTmYeyuHS2a8MaKB383xP756oTaNeSpCc",
"18BnPdGnyJv94f5XZyDDXvuq9mfJd4t4QL",
"16xxgmNFfZgdVH7QxpLCzcLcUCEeto3y3c",
"1ARF9775bPt8KjZtct9AivKug7qmzSKGG1",
"18AqwuUTeNSw69rmqtqPPtHxKc5m1h8Cuk",
"13fP1UkXgckbRAZLT5wuUj816YLkaygtL6",
"1N5zi5oekdTesLDTyAfAKKBP28jYVW1zLu",
"1L2ktXiV1iUbYo5vga3x3n1RUdYmtTdij2",
"194Bu4fgX5iNzZ4z6AehZrn7k8hbF2rRKQ",
"1FhV8vjPJyMZHaSHg7jWTvSV7qN8wmHPeg",
"12yCjAvNHd8njr3DEM1zc8TwHFW8eXc9PD",
"1E9PktoDNxbiwgCbboZeUdz3tNJwEKXUXH",
"1Gr44A3DDY9trTKbS6djFayx15a4jP2VfJ",
"1KCePTbsaz6rAdXy63A3FsTaGuLiMd1gby",
"19oDzgyySiXXXWehWCCHGRB8qTKpJ27LnX",
"1MoXmyv6xiicb728wocT2udmNrXBzZRiKA",
"16qj13Mf2EynViSNXqweZ9gqnngwfi9o3Q",
"1JgT1WSbLtx5AMpsWsRZCF3vsq9QHb6cSN",
"1HUcukdhxvEfy72NR5o89oSm85i8xpEzGP",
"1CJiRUbNSd6da8dedYewYCgydCV7wfVSFC",
"1FApYCkCxgQ5nipEyMUz5cw9hRD5T1ZL1P",
"1JZBreH53hRp5wELithaNYVqdfAPxTV5EL",
"13sx5B4x4LFH6v2nb8VUW3XxhJXwgqM9Zs",
"17j3kpYDJtCwFj2v6XYeT9hxjejgxioyqW",
"14CnadxWWymbKAFZfEVfk2mnFmbR9vrZ7C",
"17bjy491JQbFM9ETujgJuUS9iMA6Pte12h",
"1M1KWAQ27G1SvieF1nAxjUdNnsrYTxcpQi",
"1PbQP1A7an8wfupiSRCVfWGBGGLGX8t6NC",
"12ZEdpK1fcak2UaBL4kc3b5JXm1i4h13M2",
"1BBisH3eHqWpJaHhya2WjT9aMhHmr4qCyW",
"1LR8JAoT8nW88Rp1UjEBj2iWxCEJJiaDCm",
"19wDTsYRJVQQFJZhf9RwhPAep5M85Wizsw",
"1Q2CBWmmbvFYYPeX9dVFYTAP3i4wVByyXc",
"15FP1sC6dEyCqL5zLX9bTYL3T6FvzQxLZo",
"1CuSM7fA2kM8rK7TMN1NQ8Eu9dpFkSXwEa",
"1A8sHv5w1rreLPLvYV7A2H3CbSxs7qqmbw",
"12qcxwwFue1KCbRouFeh1rhbxyF1CYNfZy",
"1BdjkqDLPWr9qYi4VmobrbmT1sHKRuqRWv",
"1AfpZJPt4Fb2A5mxmHcUf8bamSZdFkLmzS",
"1EyS1p9Qrpg2BresRP7zKPhkPNtgLjDzG",
"1MmpCHppAnJ9jmsUfzHcRLVfWgbtvEJve4",
"18J5RDaNLi6KJsLw25bz7bcYdF3a6KYc6r",
"1F1H2Rz8zxxUK4ZeR3i7gATNbyt5TQpWER",
"1Hrxt2WYJhLUy7bXT7UweNrQmbHEY75Wfg",
"17WKUnsCdGP6A8B94CiW1VxA1Jwwvr2UpW",
"18jU3qCUjhVBuYV5n3R7TK5F8thCjhtYDj",
"18XWiiSdfB71nZP2iS97X1vCHrFfbmqveL",
"1NQ3bLqepHHsnyBx9FCG5RiTkNQps44ik7",
"1FzpycyupeaLMVKvGeJmoHqvSjvboLhAPo",
"15PCakg28vUzsZTVwdGWh6eF5dK1zfp3of",
"15wjkEBjJCTv91hRqBGDgyajnpP1RetzXg",
"1GSwbUUTyPP5reAHz5C2AnjnscQ1s1f5QZ",
"1GEZcLCbT4gK8yQnLb9NLRum8DA4Ds7tRL",
"1FFWPQRiXrTSpmVPVk57UjkF6gw78BaLwg",
"1CC1ZtN2Z7t7Bm3mfsWWjCwvZS3xbiKTg9",
"1PLouNEAF8NVUrcYbqtw4sXgGgB61gTFnu",
"1NHk8XeXXNQX6esQGFSsMzEibiuaYGiYa8",
"1N8XXga3aiPHnqfqZm3WvcRZ8JH7nqZVc5",
"15BeWZkX7nC7YQNgsEmcJwbPWdheeQSYE7",
"1JGQyaKETdgRPJZ5irkXrUKMm1auDvfLJE",
"1DJ45FZVrTZGH4FY9tJPAhiG4Tj6PkX5Qy",
"15keuZ3SEbQweFa3cHkJUjYBn4cK8VxmQV",
"1HqwAUPC8NeEecF8USDozea8nb66tuFAzz",
"1KnKVuuYskajdAYn2dntTLZeBeDsusnCJt",
"1Q7kZTvo1jaeo6fL6YFJtV1tmNtLANpzcz",
"18UXYGHRh4WoyHbm4nMaJzzTp7g4FBQY9S",
"1J1G31Fqnzr6qccVKXCvzg7jV9zxxELakL",
"1MByW7KqyJXpuuXiGBRGFS8tzrttim3U1M",
"19Z5mT8nKr6Qm3qRtTrFrsd1fuQuYz46kH",
"14WPVS9qxXaRDDjMfmxqyVbEm2bY8eTrTX",
"1NzdbRWDhNq8JiJDXaPcqZRau67DE9xehT",
"19hergHLC2uUe1xBDDXbDH83nYFgTJZiu2",
"17y5JCS7XtUrJg3wztbDf4HhHXA1WuBKmj",
"141YizaU9hn38tTyQUk4Ho4esWV2sFj8wo",
"14ynEAWTJRBgPwcXGM7B3Geb3wuHrbJkfC",
"1EUrEnG3Vnkd4NkjJN1XABV2ib9KYNyyVL",
"17CkEmrf7HTvJaKyLnW7Ni7V4RdcedPaWD",
"1C76qJecfQ3BkpQqcjaVYZixdHn5u5M5JJ",
"17b5SLot9T1MNLmt9WsxbUWouZC6krBK6q",
"1DVBKFUULCX7rNFHTawtwBgKYybAgKdyov",
"15tZtR7mV1FkX75dGnpau6WNapmo5cxuPT",
"1BJ1goJ32hF6jxQweh7xweyoWDVqmRGSYV",
"1Jx6E3WVyD8UboyXaFN5mnnbpSooRQuofH",
"1CcP9kWG699YwqM4tanoAoHpu6nptw7inV",
"1NXbTWf5VuZQSMFbbqTY2EEbK1qu3G3j1Q",
"1EV3gtnEpfDV7odgjZiMeiJRVNBCtRnba1",
"19gAfgnTKs3QVibu6riGZgPGFX7DqvdJmt",
"1C8y1tUfx2wwYJ3t24rfzfRFgk5iAz9of9",
"1AFMwcdwPcdtGvfdrfwpg8tzLeEjeFQDYA",
"16RJYhFxVqX6pUzzJAXYc6EHLapXWZ2T26",
"1JmjzMy2jhNab5YmfJ7wEWU28UXDPUg9kR",
"12f7FoR1uG89rVTGbZRG8d5YwvnjZXvs6F",
"14pqYVRqrP9Xq4oErKdPEr9gWiDtojJMef",
"1B1tSkfX1qhguFV3MxWqj5j3ccJf2LvnhZ",
"13NvvzE5VQodjpQugBnaWM88X5m69PkHaj",
"1PEo82ot3XYb9P4HHwUSeXcsNUUzGaQgUW",
"17aCUVj7Tm4zWirki4e7wYKPPKKz3Yp3rP",
"1E7Wm59vNCFRVHUPiDoUvA6hdAhuJBDKvC",
"1BUosJQAkNH7NF16Nrh7M61JdXg5DXYnd4",
"188jCvUxrAzbaXVeHgRHi8W1VyUDeTyy47",
"12VFLQeH4iXkVbBaGZV1Vj22XMZ74qRvTC",
"1PDLajrHGMDUwKMMMFAyhJP8dpbF82GMBx",
"16ZrMdYPLNdXF13ZE7VwfEFXYBTaaFnaLP",
"1MvKXxBSuo2foMVhbaTBX2W58W1xZQ8wPi",
"1JvG2Ho51xqCVd4bhzVdLmVtZXtoH3PMTa",
"1A1RrZUTtMMpVFCxMvcp4rVCdUMAVUWMnt",
"1QEvMeghhQykxotzWDspBJg2439T9uvR6L",
"14EBYj9XaroSDHCWxWJQo4DRW1Dfdeu18v",
"1C2zDEaw2r1oPDa35frHKrzWvud9znzatu",
"14AD5DpJq3DkDLPT98Zy86sc8EcQpraKb3",
"1M5dGjLnf1HcSEKMUHL4rJQQZV8MD2dQqW",
"1EN2f6qFQj5wmncTrNqgHfGYDwtf69WCbp",
"1GhwpqqNAJbveKHiviMFPT7E4W6m6MbRpL",
"1N7emCqeW7k5UrhsVUCAxuxJs8oXYq1jD9",
"14HVrHicKxyNpNotTB4R2LcwbMRJSdAefi",
"17Y8g6U46xShrECAPrUXRkYwxW3scjd6Fm",
"13SHEWnkj97pZDgoeFucJcxw3UaiSp1wj2",
"13zpfMyrNWdyiFCW9A9JZ6V7pCd41QXg3W",
"1ELf8NEmQJkLdh7WvxStNdJ6Y62ni1jLZq",
"1Mp6WwEDQEJqYM1vuVELJxkC5KgX7pim4M",
"1LgZ7Sj8wRorhoi589RPQj2WbPFe6ciLrQ",
"1GJRz1W2ZMKJsLCtVfXtiQFZp3kFNYkogC",
"1BJQpfsHoPEY8zjDnBw2mCrfzobVpjoiNk",
"13vW1aD2Cu2kcJnAJX6rC1YvqszVWcSJ1v",
"18iHaEz1HjMRpo6zee3dbj2baPHajsfPLV",
"18nsjhNamqDZgLNefjSpyPzfMsn3ehoH1a",
"1L3QPZx3QkvmQX6QVEdzrC3Ai5LxrXqWLX",
"17GiZnQiNwDGtsgRegqePGKwiuD6eh29R8",
"16VyvLuGL9dMVSjmnwHxcVC5GSxu8PXAvz",
"19BhysNUxoXkDVLQRhFHbudaonz3aAij25",
"1KjxewtKudBrCZgrH9Z638pbvrZXKSCwBK",
"13DC2MoS8zXaGQCAqtToecKaWhYgjangHU",
"1DskRxc34DNegAaxJFXTYWep39F3ig7m4A",
"1GTD1EUAeCmN8k3dYAxvsi1WqY92QbJfLi",
"1PNjYSKtz5kTwUWA8aRoGXankGkFh5PnC7",
"1B1J5JfctVGy3wv25wsPpYq9y1hTdbe2iQ",
"1G91KGF9AsJpANQLThiYeRqZGanov1hmdY",
"1GRPtU3bfZart2wYx81wY9RLgSC98AHR12",
"17S6LcGPxJmDuf787pmrSBgi8wU87bKN13",
"1QB6MD1o1K3wQ5Ex8aXdXNWpWTZWzqvzHU",
"1H6JaAaPFsbNz3oYtbSB7egfK2hiYocHGt",
"1NuhCf6c4mCJw7zR3JHrFkc8RVnTG1etQ9",
"1NwrzvyvpFV3LFnqhRUFVp8uVp6gyrSvtn",
"1Q2zFFjucEbznPtBDcJAzTMeDYVgyK7YcY",
"1Kv2N2b2Czcgk6d5Bw9pE3vfYBM8wENJhN",
"1KWfVsVNkcgid3vE6VJDdhW7ECQYNG8RaG",
"1MQHKS3N6hysUaCw9ZiyMpjw2TiR6Js4ZQ",
"16vDctFxhrmfonuk6muBrrTbNzEYxEr2Ug",
"1ApXsBapvrmWGkQ7n911Gxrf4ZXMk3DEqB",
"1HiwrVX1JmwWfTHdPzi7j9zhxFwfjtKKbS",
"1CYqaF3dZaN2wCqgpSpkfdKos45NHSe8ZL",
"1EsBG3g7MojZxSsaM7BvJ8tPn8D8sjd4Sg",
"1NL8z6hXj6pEqDW16XB91BWZcWJ8sDFgG3",
"1xKpTz4pc5GGtyxsjDE9hLmZTtiyHU1fE",
"1JePq76j3xWZjCgC4oAsBSNdkB5W13u93V",
"19PLEyqfHWSDTPZFdUNm8gDkD6XwN7QB56",
"1NtpYkH6DVAEAPoXgezdsGZavJ8FMmaQ2u",
"13jZiMzGNbWBf2mo95ACuq6NgHyiFGHVZG",
"1JRMQ8TK4cQTSwbdhEyP7DWCvSET9qtUin",
"15FLaiiwic8PBYDdqW2fAFghHjiyPaVLK4",
"19RPsKgVtUVjoXPt1ZnWuJUEJCngEmDigH",
"1MuRU51rXL8SvauaBDTJyEdvHk8sTf8Yay",
"1KGfBrzgiCRwyGFLNqUpuH4nLxsMXSsnoZ",
"1CHjAi9HCpPdcpU2R6BJVRqeN7t3iJbqkD",
"1AyTcJYpNgK2ibBY9yTmzn4rE8CFMo8CWX",
"1ApwNSaWxsgH9YoQZPqKx3XQ6p11k3GjnV",
"1Kj762jZicg9gX3mH7L1hgdRiagLHzBxXd",
"1NJfh6tKUE6P2VzFk4akioBo2jsjqStPwb",
"1DmtiSpNeuw7L8GCWeGorUnpNHDeGKZTVF",
"17539N1DxkL3Qz2eaEKRG5JPaHxsKtcKNe",
"1MJua9gUbuKkt8NXsKErL6BAXKVcm9iPgs",
"1463LXbwH25NibbALaxqnB7p1a99hhpiTn",
"1BXN116YoR1kX6v492gJSiunDnpGLUwdDX",
"1F6BhvqkuMuzoyxaR5Efn1jTL5XNMEETna",
"1GeLtzoBZCuDCSHYgf13aUkGecGCtkUFJG",
"19fWxsi8C2jaEtNq2HxDNkMayhEC7N9h6V",
"154FziRuvbUxGEsctEgHnaLAnMWUajBfyx",
"1EgNTiY3EKgKUmr65xuGDdGHrjEKHSboTQ",
"1Dhjqj5rYCj5u4UNkEv3oVoJTt9NBsU6WB",
"19gcZymcS11AGNdeCVc4ib2vEHdsyybk5D",
"1BHNeWuZewAb6A5R5c5G3WwLKCu4Pv6HjP",
"14opTN13vpp9ktDoUnjTs4NQoUjYLP2nbd",
"1D3rQvPAFT6cnmBnvuMXdDq8o4twM6wyRz",
"1Gt3i28YgiL6v15ExNkCVSwJSP1frJoi8g",
"1NjjjF9HKn7j25EZ7bpuaQHxy98RoUgxcM",
"14S4ZHu1ZpKL8K9JkDXM7LzbkTEHkSHWD4",
"1Ko66zCpaKawfaNwBFq3SPpeXwcPJf5DKM",
"1NErqJuCKReEvzFBnQ8UcHpcYgcq7xGzRS",
"1Mq9TwDJb1YirRU7nkm8o2oRtCRvJPrsq5",
"1G6vHw51bszL5RU1Wyi8WhYHzUdmdpHqSK",
"19uQmsMKjfLwS2fNzsQRKMpd7zfobRU5rD",
"1Fx3HFAS4ngZaMsHNBPbJi7aYwaX6Ug5Yf",
"1CFB58uuaa7GchMpgbjDCF7Hcx2sSXErvs",
"1PuXFXS5bT6v1Vc8oF6pyv3rwqQKS6sCa9",
"1H3GWbUKfLQPD3G36zUiLxa4x1VKYwWGA",
"1M5hoZ9j1a6D6zP4vVQhJnW1ED17nQFC6w",
"18Lzs1L4sY97xVh26Jm2FjFY98Cfiss4Sn",
"1FxHcSKsEzGBHu5e5HsrZ9vvtnvq2dzasp",
"19Fvh2wxamu1zad2DWjdVr8Mk7pzWkZmzq",
"15sVDYczmHQEen9QuMZYb3vNtNb1CvbdMm",
"1G4eDR8pv2ih4rq1dspKwsVMwo1F9Cepas",
"1GzRpDHJTW7V19Mxc89WQPCP1E5ahxq1fj",
"1Ge2NpDXfRurYQCwNuZ8ggqFU9N1ooeqJs",
"1u6HFqspXsCKnx1woHYpBhv4cgwZdJdA5",
"1PoQLyt1MJNukRMjpgt7vLGmcQzBt1T1R4",
"1JWAo3GPVndRZQmDDtSi1CM4Lsc1zY1XhG",
"12yvTtSWhYnfe4jqPHop9Qt1zF9iuyYs8x",
"16K8ndAyE1VTZ813o9UEjUQ2ZN2yTgxMdu",
"1HRtqXM5aGZobK5pg1UsDzGUcFyWtdrHaV",
"1N63Ki3CnARweLgfnSCMbPHyaV9toxhnuY",
"1AS57x8FrF5LgXPAJR5sC3Qt1pBUxgCziw",
"145ewbmapkDDtdG8FRU2DuBrkwaVhcT1rR",
"1DTWGKQy3os6yoE61J1KFQxq7AFKDeE5TL",
"1DU5uoHLMp4pJisfN7KWqYQrMxPQmZyf1j",
"1EE9JmnReJS69pjayTkRrZzVTbtM2sNpbN",
"1PrUKknieaASvm7kHWQWVf8rfMBRwXeTZQ",
"1GgB3fCpdi5uB9zXLMzzAvrAZBnRJhXzCm",
"1MMRmbzQ9DaxnLSnYRsQTFTy9rFNDZ9iRS",
"1NZjqKd194QJFCFeZxRv6wwT9ET5hpbgVN",
"1JEgaBDjkVMnd8csQ4HPnQaQr6k3gvAScu",
"1QL3TcMqSn8j39ZLbAwQ4trKtfKpyAqQHw",
"164arowi6ZHYZYWko2ksk3wUPoCRdhE7fP",
"13Fr8dp7NCSP2RSbbXEmTV2VCH8m9U9TSL",
"14nyeZeBybsJ2UHoSQXyt4mcZmtBkbL6Mh",
"1J2X2eWpJVCLWuGHj2MCf7x8a1X5J374iZ",
"1DUPMZu3toZYjCRsrYL5GwDV3279w7kqCB",
"1KNusWG3AwhjBCmFmBZAVLETeXFuATA5oC",
"1GuM9PAdLgKyVRjKvfaBrk7GjS1ujmPzGJ",
"1HL6bakopnenBpRQiKC5z6M4XnZfWRsQ79",
"1DYtM9NWwTqYpAJav65B2dKaqxPyJJ2ZWp",
"1BLLHpEiwdEJBvRqUoRyuhAQ6hdgVWQynR",
"138sHrHs8hXgxaBhhiUWCrKTfbvwUMeY5f",
"1Lz8uUAU77RV5S61ZzfjG5vnziqn2W3kWS",
"122bEktL1kH3Y8z1Z9MBv2agmMRnsVKoNX",
"1NAPP6zWVGyGXoH8yDQKKYGJFB9qCjZX2D",
"1M4qp3QfArz8WhtuFRodxFCsyDjt5BC3Uv",
"17wzfoJqHtt3YTMAW7Rbp1mZd6CRqcwRj3",
"1Ndg11m5pDzQHiWXcGums8Eq7BFUhctBxk",
"16CVDWSRgh2GNF34mx9g7zdEQUSpiY8iBm",
"1PULwqkZFAjRUUsgQ5VmhmeY7DUmJz3xoi",
"17kXCb8eWZG6Sqmqju2FDv49ZZ8fqXAuz4",
"16xq3xJh1dwzR8N5EfmjZXLam6oezEpQkW",
"1CC28ccujdqvWiNmsqW1Q8Rk23QBfpFPCg",
"1MpXbY6AoJGNDs4DVvmR1QzEF8irKbmR4Q",
"14rvYFVjjtMMirdFhmEtAqBg8KHH8aGUF9",
"1CwtTf76nXLHj48ugMJmFqLhAeroY4XSzz",
"1B1mpkC65GwDcndVhzNXtrWEKfCPh9cJcz",
"1BC33Ct4k1Qd68DRx64kPsw86DkVvd47WE",
"1DK4zgQntiWV61a1nG5hHGshtDMrdpMspo",
"15WB8rJXz6Go4NzUvmLbSnne381Z3tVp3n",
"1PppxuhMkZG58BZxDx6ZcNgDHX4aLrmuPS",
"12tgsZcJkgb7FrRPqC7oe32rKhFVBSXcmX",
"1Cy7PRWZ4ogBfFk2j2w7STSARZiHp3znnM",
"1CmprkoKdHEfUQqYSUuP1CwSwgNUJBcQq6",
"1MNPSchbaZXZNQymLG9jcnLnFQ1yeVCPQH",
"1QDEw9ttsx3TYaJWLv75CXdeBUxDyGXWrx",
"1NbLB5FE1fim7Ke8wPW2DRP43ZoQJhPN4e",
"16YQrXRwu3RbUxbzVs6iQqMBvi5DF6gLA2",
"1MqHZTxe3ieNGZjatLRKngVsvwQieZH5pB",
"1Pgj531pvHY8EPSPFaxB4aUh4SiSiL8hsT",
"18J5CX7QXHJudWicMZTKS8pYyK8UYyypQa",
"1AQj985ANEAiF5f7bUsdurpSoHreNAQF2h",
"1HhPMeBK4ercCyT51YE4JYUkMBGLg6hWzd",
"1H7MDVTtZpZWjNGBVBqaYbypdcefMApHzk",
"1Fkg6DJK9qxS2T7BhGgxUFyfe2ku4hLb7y",
"1G1pixoQyJGLheqkcSHXtVqwyGvQi2We7m",
"1LnduL2ocaWDSjXKKPb7H3Uxw6dDxExChm",
"1C6ea4VFMrxT84oEBbc6RVrEUFpd92QmCg",
"1ES2AoCdGTrVXpSJMurdWty2sH5VMFNXbt",
"149qimTDRrNBZC8AvEv3bGLyTKq2fASUTv",
"17fB1PrFFuNqatMEUWFjAWWQNbQs3Dr9mp",
"18JEtzPV9Vp2urgK9yxdweopKMACZyseW4",
"1MNF2qcp1Ro5Y4FKap8LkzQpUWSQVoDR42",
"12QWTHQUJNBvdsVNMtzAijk2Ni6we3PhAd",
"17jEjL1M8Y62XqLgudpHm9NXGBHB8LByj3",
"1K39ihGkuG6sXURQ5VjSvdxZmcMuwsQBm1",
"1KaYcwknWzHLZ4vw1wA14XjvNmVseFanzS",
"1MvzX6FCpoARjnfS5TEY8DYcfCvGiYLUhH",
"1CKeehHqnmEqLGsvsMez6WfNwav1yVgxvK",
"1BNhUtDB9C2QpTQwoSzKPeqZXYyk3xAJyf",
"1DzzaCHKwY7VndpzfPPPuhcjS5Z37uAZcx",
"1PcC6h9v5n6ZDzoM5tAzpFXm1tC1mEKhLV",
"19MNELEPJmtwsoPDmjFT2PF9hryAYNtzm1",
"1BnJDAPjpLRmCxJcdUVJMEohxp39i8CTQB",
"1PG4VrzMMskAiY1cxcDRV6BwM79dfoLGmG",
"1ALQutqq1eParr3gW6URQbEX9MfBivzKMg",
"14TkZEELn68gS23secFb7CvzRVjupY9nj1",
"1Auv4QaxGtMbRiGsPb6SDMwku9awX2uugD",
"19G29t3ZXtm8Fn2L3dEMxb2JJy4M3pARPf",
"1EhL2WSN2vHaqMY3XtWYQyDMjFLtu4Kk3x",
"1QJv811ZML5VyLHdZAa2PQ9eLy3EfCoEyB",
"13mLZji7UVfMVuhvMszFGf2iehbYKBJCKw",
"1KB1Gh2NNvCRjrrUW8VUXCX9j3semgMhbn",
"1ELxoHrAJLToqNGU4pAgjRKEPkuP1RdzPi",
"12LGPnK8dd8XkH5ZJzTTJNF9RcsPDYP8pP",
"1BfiEgNdW4U9LbvdU3SUW987co3Pf1gzEj",
"1EUoj92gky6Bpeb9Jb66AiVz6qQGJzTDfH",
"1ELkWKdpNbNKS1mM2mzHf31X4V1nqqGiuE",
"13YBYNmLRJ1gEDC5iAkv5Arc9a6L9XCHE4",
"1JEeGtBPsQ5WjBzWuemNoGQfeGC4SkvCB5",
"1LgdDHrpKA4PvLCQUnwmxj8nAMM2krxQ7F",
"1LrMjUsJcJvF1BUZbnV3xGiV7dYURgD2Hx",
"1Gb7AC94fty1m2Z7MQuFn5pgTkyEDX2t1S",
"185GeJEcwvw9GcYbPRuEXMzPLWzKJpiNJo",
"1KaMBXEczPKtYqZn2QexruEPYkSubvqoX4",
"13S8FACmVcTQTwmKFh5ALZYXGMQw5spspx",
"1BVyNRtCGtL6gyDUM6TypkyJaNreAoyc8N",
"1GKqx5BTuWKRM21VAQjPQNSF3rzQjMwieJ",
"1YJFwvdaKU5fi9AVNEBU7B19jX7bCYjBm",
"19WNkTHRbYBPQWJJsHXJwNN714B1UZcrsa",
"15dPSeR3p8scK7a1dtLjUvaK8qm9dWdPuM",
"1Nioz7mvkhQ5i6KYWuURqq6iMnpAUMoRiN",
"1DuLkuhdcRm71utCyk7tUKBczA54fr532A",
"16gauZw86AUpk6MoBymMpZxD2R56xr5SMD",
"1HEDLFSHeYHhZ866cgiSzWxSNMrz7Ymeps",
"1P9GzdeJAAp384KruAvxRh2agbaD8JvGSd",
"1D1pZ5KTGqGCVfKwxBoayZJKUDVG2sEfy3",
"14Q6rCgGtzngEhz6HPVhaWxQXsqjU3yYDu",
"1DRD4kShYgdg3FEnSr6wGUuvRYiGNg3C2v",
"12ZBTiVqJZqFR98oW8W2NpS6RpsoWm3xhL",
"1P7AbvHBYuy9fWyqWF86oaUKG4LspVxQBR",
"1JULPc5DvimmNHMSiDdBKCwFxftAeawceW",
"14PZBq7dC1cfuibxKrzHYZ5HWCwT6TDbtg",
"1M1XJFLYsAyJwa4MepJAXPBenzH4MwUAot",
"1FnfpeVJPYzVdTnc5DiuLbn9DGriMjqtCT",
"1FnCSF1JqMPavTUKDAcTK7BHpKRaLqk8LS",
"1M74uVqpsPs5aqFboZKnYeEXuHoW9ZaUAg",
"1PqYveYmexbXchNvEdFEoGZj7Hx9Vd3saF",
"1MgUj3Nh3fkQBnWP2AQzuStaUgLF1hmzvS",
"148QoyrQXw2ABCYEYefCc6oEHwssFJMZV2",
"1LYC5DhFVhpcNcrQkrRJ2kpd5wsmv87bHT",
"16dHZRkjvZtXRv9SJWHGUHL3ac7nYjjey9",
"1NgZQHawfjfe757r7QKBkD1CQ9M4bhKN5i",
"1AC1r7hYnArL3GAZr9vkkCN2FmvN3YY7c1",
"17uHMQVUnzCoiWikZ1C3TPoPt4h7Z16TSn",
"1LHHke4gCKfD1tJFoGkEDQPQEQTVoQfLsd",
"1ALPkEq9ZfYc4CGAVKRNQoCB1hqcXVW1tB",
"1BxG7eQzGqyZHNqYdHeFo5BiPvgZRN5FPA",
"14K7tgcn9KermTJGfdoYgjPLcSpyFxrckV",
"18SXv2LkzCQx9DPbzVSVKjitCjRVzfvXQ8",
"1JzpKjxjgx8KYeewmdXHu5woEKz6M5SvrM",
"1K2wYRZkyfYFUJmzUEV9BPbcaTY8ohpMsa",
"1MJhuAZoN2GuHrWDK8CWNXyM2GoLPxK5K6",
"1B9d6EPT22oSujDgx5D2VDSBh8VvDAY4Hm",
"1CvZKT6ycBMejUX6mLCgqnQV4wtdRaMKSy",
"1KCq8tvbkq5JwALLfgudMY2mWTp2F9GTNn",
"1DZwipBRM7aGyA7n1rDRcgexST23CEes7q",
"1C8HSbrADc4xjCD2a3GxKKRYkUfrCzh2os",
"1Enx48DWnPmSejKw6wvoaW4bmwQjPbW28v",
"1Lpz4gDfydJ6vDS51QbbK9PTWqTVepyLhu",
"1CiLpPFm1A282MuhHnrBxqDfVx9UaqE1q",
"1MvVNGSZkJPjWDAaVpHsxsmDAr7CEY5exP",
"1JTNVJNyviRJh5ZqfmPq8eSVouqPMum31N",
"1LRFCCmp4tnMpEsvyaprvbfFvYUgfYTp1U",
"187ay481aBBqA3ruVVUPyws167CVaKBbVG",
"1HhT47TaTvFVDh9KV3529y22WEJnAHVXRK",
"1NPA2idkkC6M8D2T3Nw2DAip4eXYsGWyCD",
"1153cHM8z3vrq6RPoQomsYHFDu9ZU8LnA1",
"1DzCYyxLxRawsGr7zVCv29JGFATKcZwbCW",
"1CG8JVz18ifwkH3FLEJBUkDqa8adh146tv",
"1HozgwVk99ZHPuFsSyUAnweqjjszd8Fa2Y",
"1H9VLbbjVrEk3aHCW5Pcv2hPCbCReF6huJ",
"1QAp4Xt8pXhY2Cvf7aq4u9zpmGXz4WmswJ",
"1Kuy9B3nDVhbBXyijAuH8b4mzYBsoTCZAs",
"1MxVU4QUnEL6itpJTjy2PyL6FVq65K2gJv",
"1G1SVdTG8phoeXaADFd7sXZXHcyL34vRec",
"15x1nJEAfe4uJ9M21yC8wXkydj1E4YLLbM",
"13pJBbdcPYpzjqZwjQqnQqtf7xZX4oHFny",
"1L3mRx6vHsj4Tbr7kJd1ZVTmgqt18qobxf",
"1JGZHs9o1vVZVQoMehcXV4nWYcpSi9Uw9X",
"13BH8DDHVbyK4dGWzYy3VhWvVybv9Gmpe9",
"1G6qT8nueyFGX5poxto3ksEwt2dWvB6ACS",
"1GM81HRBgKdr4iuuZCdhzzAwwxDZfvdSCP",
"1HDQfvWrbfWbYFFev9nuXfSDm978hRT1dF",
"16sqg8cBWS6T535DSzJPHbErN6cQh9YvUp",
"12xgAr5JKWUVv9oVy1L87NbYzcSRhc5Szk",
"1uiAKM8aPhqzHjFeKnEXFFRNRnjvBghqs",
"12sjFnAjsQLRVh17criU6yKhLsMwkMHGMu",
"1788isu4ABWpvXi1ML1QDYfBACq7xynQYk",
"12qdRt88SoLHKr66w4LUqzWpPRtA1a4kyu",
"1H2ZWmp76ZcbbDkVoTgNSLM5ouzH7QA61v",
"1X2PqVm1Um7Ck4JP6WntpEtiPdf5HDDpR",
"1QG1iRME5NMa9D8LZoqoGsAubZfqnpZy5X",
"1BZgxxv8rPyac59bsAKQqVLXYDh113rqiF",
"1MZt51vvenDCjEomwr7kccVd1urD9Ns4ab",
"1FahogPRBbqx6rqRD8K3wzJExpNJLmVKg3",
"1EahJopUoLKtz6gmcE8mdUEUn1r6acB7CT",
"17CfuucW7j9uJGooRF7RxMyK2BqKm3TY6w",
"1Fg5rxkiFKF5EjehHtQAsrPkPQqnJQG2FE",
"16WG2ZSRpeyqifrmsVQVjxLfFKV1UZ9ZWC",
"1KVyukCXkc59Ppo1R6cyRbixh1ZjrSzd7M",
"1DW4YZAj7Nb58SBbggRCvShiHuejDTnePk",
"12vLLod63hWjG5Nd7gmpWbMF6eDPz59hj2",
"14a4uXzShT9C4mTSdCq6M6uuxWTH7Df5tm",
"1FrWYE4yLxZLr3n31CaCdKTtLDSMuwp27n",
"14Tjp7iFRmNjjjRawMr3HztwAhqNbH73jG",
"1DkJdMyZzXX2h3edWB9Jw7bsgBEGTGfa5H",
"1B3d1xe4LZLnf6E7nRCVuCNzNqKNXC4Jy9",
"1BdrGgTvnYVsGPf9FD5p6yTHamN6UHkwhy",
"17XXHe64pzGTN115MJCGRtQdV5aH5AgmX1",
"19f8XdFeCq15w22uy4EnWdx44JEVzog2eF",
"13uzGjLthGqMuiiHbZPcPRiK6JB9dYo2BX",
"1D3cwBn7UZiqHd7paAGFXRPpPR9eLWEdz7",
"1JTheKZ56kdEbbi5YpDv6KPt9jSoptAL59",
"113ZAUx9yw7gJ7swPUeyfFLM3osuwH27Ea",
"1H14NzvyZmSNYeGFWHCmLtqLfBqCve48G",
"1QFS5mw2Vnb1auGKa54N9znZxr9Lutu8uJ",
"1B2jZsEtg86QhfmHcG3qSKUYsDTmPxLyfF",
"1JXLmFxAm1J5CX9g1t5UQiLygGTuJqfXnB",
"1CdNqTonVa6DL6kmRAjv1NHjYuF21qMsBQ",
"1MEurMx8wsnLaV5Vfay6eHx19sMcHgJ1mv",
"1BHUocZ6dUs7wmu35Pu1KMqNfziKhSePyK",
"114yGjPFen1kPGA3JsG5FmuFUD7rxXToBF",
"1Gpv7eWEsND1NKpAgPfsm7ArNySxkAZaAJ",
"1PZH1pt9wCDyNVwcHbCMa1gExXtS95ExfV",
"1BHfKsqZSe73PPUEc4G9BXUYMG3zbsHSuA",
"1AgH16BX6oHFvHKgPjoVuuR4nMsmeBEZqZ",
"1ESjHgzRZeNowiNvqzhevrX2522Vdo6H83",
"1A2YjW2sFnLNeSGhg96xX8a7LEMffZGS9F",
"14zhsYC8cKfdME33Q9mi7pu3fTGzxVK2pt",
"1DCdT9AWzxizb6f6F54pyrvQGZRnBoutQQ",
"15GooJ1Y4quqPVxpG5FTTxcwMNDiFHjEr7",
"17qrWreo2nMqF1GzSLV3cfFLWZ57eLNSUy",
"16EkHJU1AFQ2qyPvCmgaF5SR4jviwoNKya",
"16oNtxMVgV1Sd4vrrbuMAGt5UurTukYbWu",
"1F7VZnfF9FhDXQas3XFai56F6gvyWNe2um",
"1KCuev64AT9xnEmoHMoM3G3M8VwtQCtpaF",
"15Qn8diSoRKmYEGM2Wjn5nMJ7MU3So6XfW",
"17AaTPxHUv9oxyCddKcbxWgWayyTjCQ52h",
"1JxpS6paQyhFq8HwfqquztYWpo6p97L3qR",
"1Q47zkWV6xgSgDtJvjW4xBn8wfa9C4sLfn",
"18uKVTEn1M8BeMRCWD6KWQScor77CF655H",
"13cwR9GU1jaBxm7vCfPghywU6MJ317XTPa",
"1Mg1bH9WiKspdV7fYgV1R98rj4RbqSkYbw",
"1Dy8rVHjHa8iSX1pS5ZqZuJnp94bYHdjdm",
"1HY2NxG9o9pBVjBLeQ7idCGSHuWeja8Yor",
"15gqj4qgrSiRpRYghx2XYjKGLNesi6DdiD",
"19jKj4ZRuAcPCCkpH3S1ZEUPhpBV4KUG7c",
"1LorWR1K7itEZjb6VqLuyaPm9pcWKpucQd",
"19KNSUZzp3PXbeKfQLn415XVWskqP8WJ1g",
"16pZXSk1gwuyJkjoENXqRGHGgW7tQURiUo",
"1BzNxHdijqR3bV9DDVo878xq3bVGSH7dUn",
"1C5cQhbww5pjSoQs95kTSdKUiBukfruTEj",
"18piFNaqbKtA6WwoVDmWRnBBc4PCQ132U9",
"1F8t2V8kt5znTHXafVe3Xbvbz91cNuriH4",
"15dQ2nWB9tsu9eFoj4ojMKhc7f5Uj7vHVs",
"12QyDSRbC562FFrzkkA5o7Serr9ScdfFB9",
"1KScPxQdEHbWXp7Ex7EeDgsm9ejExfuM3W",
"1LLVJue5WjH8Rq7zHBDyzVjEV5PvBmsgVi",
"1E8jsWpPhJ6KU28Lq4Xg5hMCWg74yRboWs",
"1E7fJ3VjeDrShS6U3TrozfuhRJGVKoDdyu",
"1EutpodY2q9yTaxpvk8mP5gPWu1JuG38fz",
"1KUBGbYjcwY6VZoK8WiCcDRuC3gkpr6TLG",
"1Mr5Z75ECTKgSGdNjj4tt52BcTtRSFGHN",
"12W3VcAhVBevactc4MtFxn9hqZM4KZmZQ7",
"1MRs5Y6UiGsme6GR9U5eiLtXXGU8bf4LbB",
"16S21xowVHgtqA38QpSuQhoXm6x8vV9mAh",
"15MpJ6vmpKBEcaikrZsPn4wBBP8LBdzAqx",
"1Kk4oajZFgNRXRagwxGbH7ybu6o3pi7ffY",
"1LPPYzhTS9YdtYmJ8jo5HpMYfDmaCeqra3",
"1MqiyMFMZPrfLgRLUXNj3uJ6MvVVRuKTUC",
"172X8kzL6G5uZZ6pqxwX3vGmLPqcLSuiXJ",
"1GknUQoqge7LxefW7CGerKJa6d9jmEbxGk",
"16BgatTKiyKKNmaNU9e6tgofFs8ktiA7Cx",
"18hsotjW98h8LtzyFcTBg9wfcYbwATfgHD",
"12kjAEGP4Aq5qC4gC8K4UNfxLHrSWXRySw",
"1BGtdZAqRUfHTgETATP4Y4o3mjaFpBKges",
"1CTmDN6n7AcVRbbeucm13XjGuQ5LztntTA",
"1441c4G9RsjPH18UNyVQJNNJMV1BYWqTEH",
"163YowjHfD6zc8Fo4PKqanNydRvQdu1qtU",
"153UdmnSa7YrZp8HLEzwDbEix3qSD1Ao2w",
"17bmXeUbRpVMQTuUfu3pYKGf9EhujiLdre",
"1N8WpZk6geNqRFXJTjyv1qSmW2jV8SSvDF",
"1Q6qL1yLCzNVKuTqqBrCH5XUZFEBdum2dt",
"19tNqxEfCfwLBwSL6NfCWpLhYRyE49Niga",
"13KbQUjenRVkmY7TGJSYyJhQmAyyvnxjd3",
"16jDAnkDDHqxktgtz2T6nvqZJDdNhXHEo5",
"1Di2ZuZE8KDTEv44dGCFPYz5NGhXpjgcAJ",
"1Ha7uaMmj2zfWadEJKv3JRfgDA4m46SV1f",
"13X6udvHydDnxSKG2jj34ME1HGwgRajVhD",
"1KCsUv9c4UXmSYMaeiDkgAEuu3CdhisbkK",
"1HChNsWEFGJB1BQL7u5KgF4PkNuyuweZ2y",
"19aewJNM53DWRGF1mWgq96xEy2opqEb2PL",
"1Cdc6CkRfjbfo1EAByXWSkaJAkXW3fTKoz",
"1KFeckAioDCDDhsAUwyomqstQ3q7jz2ApG",
"17p36afQQrzKm7o8eP2hLzLqLVMK2YxVDo",
"1PFAoA9bKkntJxZzYKeWkkDABAAQsAzQdL",
"1EGj969HggT21epKA9iBsbqUwB2wmCWdw2",
"1JEm3XscDXyRSoJYGhJ6V11fzvWmMZ5A3V",
"1AKMDoXV3tZGSWsKwt731gXsv9jaHLxJKo",
"1BQS9cxyA33Ta4H3RQQo5VCdwApWKGKwNa",
"15eNaXyxhmnLq4T3rva7XBgt6WVMXmJ8jq",
"1Dwd2K72e72g49T172iBL3abzMLTe3bKeS",
"1G3LS2Ubxi6t8b61AQLCntKEasRzv4BPnP",
"19dae1HJVhe3yEC8uKHoqjFH41DP1DbCkC",
"1CYF8ynULfsEqh6bZzWg6yV8y8UcU8vVGp",
"1DjfoXjJkf6v84DFq1db8utGpwAhTKhsK3",
"1DSS7h28QsMk8ctQzQVrHT3J255MVjZ741",
"14qin6PMsi6uvzPLxUUPJ9VLPruCoPYAfp",
"1EeakYfgQLJF2GsJLoLc8hMwMEBCCfhEh5",
"1M2oPpnLc1Azuo2TDSAhDT4Vmvj19Wkh6G",
"1PLzehkowrk5bN2YoRpNGTcnicrjFo7U9A",
"1A7KaJ2AtwZ9UooMQp3e2ZPC8QYw6tcLw5",
"15s7Zw8CmLZHzXn6UHrsTt67PzmLMnFrya",
"1FqraDaAnmV79yzTSD4fZkHXANMfaxh3Fz",
"1CRJe9L4QC1X6nPU54KjSuymfDt1x83pW9",
"19Rm86A6cXMmCptGjyAWqSz4B65QgzJQfu",
"1Mveo3CcnpLtLvBMw1GVvLtAjY31UafxNj",
"19PRxA7CMea9vB9hMFEzFeyj3SZYW582mJ",
"1MrFssVsA6Q7KB1dMa4yczmGkuzvqs2Jig",
"1mDXvUdnaKpYETeZiKXg2W5mwfDGiQR9g",
"1AT9tJRjQEMJ2FXSFX9a3rgwR8rit3wYF6",
"1F65BVJtfnhWzaiPxqpd51MkMp4orq5r3U",
"1NSJNakU9HfAgCeDvL64iisj1sKprzFr3h",
"1CpM2kD9wV7pYUtNkPxprE46vWgbYptdJo",
"1EKaniK2XKuoXwpibMugynB5xuHJm8cnG9",
"1FqxDhAdDvE9G5mgHaxPKs6L4VePkczsjH",
"1FPuYAiwdspvLSuic1upbCxMNG88XsxpnT",
"14evXrje1WuUbfki2PCTi9iFeKPepjeS3V",
"1FCAdfLu1rojFAm5cwwt7oA1XEuUhi5xQA",
"1BVnvDgUTTHDGw1KpiHY2ai6pQdGoFD7SY",
"14F2QuqUTQ88XdWWdbTQ7KbGYztpgSjaHL",
"14UwH1yqbiTQL6LNG1isYEicYdw4tCKBbo",
"18PRsmVERiPUJRchTuvvmRT7XsVNsE9wvF",
"1BLTGLC1m3dEGxFoQzy93PckZLYbUDrRNJ",
"1Eke91nSMxTVGKvM3PhyAVHsJ1J1oveDFB",
"17Mw99dLLgbWRT7B7bADpiVM9bNb3MLpph",
"1Jb7SWKSdv2ZWe6GuLiq3UR9y6hyPDVriU",
"15KbBHDUGa374c5Fy5wZV5sWSdqzkBRJLG",
"1Cmsv3HT7aaSF6GizeiaNfyrgLFJWRY28g",
"1AXXpTZkA9qMYJZgekybUyTiiuk5kXh64f",
"1AXrmvRYZoujpHgp9CrR1oVDy933Uqk8fs",
"1DZC2ogb56NyT2ZDmtER4VDCwhcttRBpj1",
"199PeGaVgyht5veVqYE1KmysA1A3x4FptY",
"1EjjuGHDkgPN9fLY2pcQbK4QDjDZGJbfN8",
"1JPiCka8ebhQ7NqoAeYXKSxoeCxVisiAe1",
"12SkkWPDXFWiwJjT1SGAXfggukfRtih9Cc",
"18GivExPSKWqaczXvxz1sn11pb2ywwykFV",
"1HK5Bc41fRUs41aLZAVacRudDGNCNX9yC4",
"1FHkkYereRSMSsCG1p2aCBjNrMwfmQ6XhS",
"1FPzUMRkTueAeX6tQr2QTMT65wpcYPVM4X",
"141knA9uWpZkAcxgNBEGaG3ofMzYM2m1ud",
"1Q4Mh65M2aPnNN6KHvk93vEbvkhsdAiEEe",
"14jsBVHqhR1izbs9YqbH14MyfdMJTsJVAx",
"12WzWSPTmZTW6ZRxhiNwMauW1wLxf3NDSJ",
"16HsZWWKwCb7ezppmVqKnqRnxRiG9zQn1J",
"1C6NA9MTPAPqzJSF62mYJNbW5XLb232LKj",
"1J644arEVcbWHRhgFK1eno95puHAETNzMA",
"1ToUz9ymdnJXdrs6DwQWjPaRWGYBoKy7D",
"1LEChhL3Bh2nGxFE7YMLHif2J3VUQ688T1",
"13i85yQfPoK2cuNj6ZcxgBdd77baBrymT6",
"12xH8XAHyyDRueLQQXPxYHBu48H78hXUnr",
"178UezVT9SP5jZYf9zuf1n6hw7Pn54SmtJ",
"1KSZgmaNFVvWj9L6NxsAZRJVz7m6SqhsAc",
"1AFvJxofeMq2mQLUAyDSzRsaeZtTUZacyy",
"1BPp7Wh73TGngx3s9aybBbRg1wEcvCmq5Q",
"1FQ76eeexCKkQ4CPRenGbBR2aZEut3WaqK",
"1Exh7bsigcTJkf6uDK1DoocJMk8cAJM2rc",
"1PhDBpwtxnP5XKQaV2BLfXK1qWko8jv3Wm",
"1ARNYJmKj6uKWLGcbUwKRwDcTbvNYEKyiX",
"16w1QC35thmiGMV55XFv2Yzd3ZvpX8MV65",
"1r5PQpS8Drpq7GhMG8J5pGraCinDw7eDC",
"18s78MfS78kP1RsLy1QzqwDPxyGdJf1fBu",
"14nEeXiF6uMUWDKqTzB45Hk8XUrNSz7dDv",
"166qyFQRa7XyZppDsoWtGpz5q1poy17Nc3",
"1EWd5dzYrxwANU62Lr1LnsQDG4sjNgEC2d",
"1MKPa26dzD4rZY9KWiuFvLYfJs6FPAig35",
"18e3LUKhq2CnFUd3Ar27jVmRoi5NXZo9A9",
"12ZgSytaZcJxNhwnqH1iQvk58KBq2xv4yf",
"16viyHAFkyLX9B4maCEyJnNeYHuaArGKhw",
"1J3Uf53pzmTtwfCRVnzMnppkA7gCFagcPB",
"1GxZ7iBoPN3hTFgdtNYWQFiBLs7QxHpwzi",
"1JEErUA1YgwfDCkW9WBM8mDATPEpXGsEef",
"13EEBrcDMuAdRqGJPqUmtSYVTep1G9K79C",
"1PtWEX4pjMiHjDik6xfUtEq4bTH4GKYxJx",
"1MN6t7hHxtvJ3UkgSxMKLh3BZzZrsuda1D",
"1C4RR1cLUUb8JhCokh4Jbx4XRQmeH5FxJN",
"19Qqhc9gFcvSBwggp36V6aqPSuXV6GyLvH",
"153cdwc3X7tNQttgrDUXA9bJ5iLm1GA34C",
"1N1HEhGG7Sspy4C9kE9jfjShH3qsbzYic5",
"1NQfSK2TxTdV4WngaWTPHyJs3FuP5g9AVB",
"1DjbaqJ85h1bq7UereCBLdaVnqM9qNigsS",
"1D3zkWXuyabyWzckfTBysvj9mvYX6Pk1N9",
"1ETJvuzTfTZCZZfYxLy4bDuVq9JPksT2Vj",
"14xWrLTNifmr9oHwVQkQBLac7c8LEyAapZ",
"1QFr24179Eg9QeTiTFi9V53QEaV5ADNWsv",
"19rb7FzTvszVQQJtpm6SugwWkXvrapWFjR",
"1NaFWZPjcAZctj2XdpQLnkkCF1ytB3rBxg",
"14HL6VvDWdnbZnBm24LhXspFhAFB51912e",
"1KqMSJTSsBFE5WjRm7B6NyE9ZQNmfaDP5g",
"1BEWab9ncRqoUKj8GT7mNAGSjNnEn4f6fD",
"175EaPtwa6AyDSvx9dj6bsiH7huC3G4v7t",
"1QDppYw4uQciLDv4FoC43v3sfG5n6Ns6JZ",
"1Mu1BCmxkrcTif9ZFZBk8bL47Hs6eKhU5c",
"1BE77qYYXCxBWp5qt7JqCVea5PDyeFp4ro",
"1HmESC7F7E8LaTh6rP1PWjsXz4ArGG5ti2",
"1NW5QBDNYC98uJdEcovtbQt247NRw4CYKc",
"1LxPN3SR648ZpFDfwrXYLmtFUdWsSP5agB",
"1DtCJdRSEXE4P7UyAJZ7vQeEB3GmFT1UHj",
"1BRxREPdDANzsZVo2BzoeNa18wtr3SG4s3",
"18oh3hjBAqnLPdhbGMtmMAaP56BJAtzP4V",
"1JawzZo7Z8ruFpmCE972Ae6UcMZPfC1nzf",
"17NsC3AG53wJpMbeKSDHZ1XpXbgjjp2sdC",
"18s7jY4GFJ4ZD2cte2ZUxcbifLetid7kMc",
"1JULZZ7FKMKgJhP6hpT6jPzDQsAN8qrpwc",
"1AabRv1QRduv8ZP8vhNPRfL3FBwdiSm4ZH",
"126wsBtx2yUzPTQSe3WtMWCCyDLrLSoBrB",
"1E5mzsAMP2XegdtLKPcLUJMqxMbaghvddK",
"1Edp97mfvSf32HVzDxYbMkrD74NfsWfjDm",
"1JXNBgZThWW9FKD6SHYBTPKsejJQa4AEAw",
"1JsDz9HizRjQu2DCtJBGvBu2TjRVLEy2xJ",
"1Kua5vF22XkyByKwT7NwaRQyCemHmL91TX",
"17wFFJuaoaMKPTFX6w6AQaRHZmVPoMY2uz",
"1KkbNAj6FeRZA11W7LDmev5DLrvRnMhSp1",
"14vL2gzk3bXiCjoBgGoTSLvbrVUNugBz8x",
"1NhQ5FSRnCrUoVBNB7bzMMaDEfrTJp9cjn",
"12QJy8fCQYfVJsjWD7uGCpZ3Kt38HQfvhG",
"1KwK1uLfDZ6QNatJ9aYs89hLevV594dgHm",
"1F1EqcV8wf16kr2sk66gRVxrjRrPJVwF9n",
"1E8hZ9GMpTJGG9ZxVNKS1mucZXucYWMgCA",
"18NM8k5AkC8XgZmgRkUUFR8D8ZsdV9rfQP",
"1EGoQMWURJEQabypdAaKNSDiDRfedmfnj",
"18Wp5YtwbFnSTBBwZSNQzEcShH9ULpdf3",
"1Kf5pHJ6xH7SLVGg9agoPVA5QtxP7PQFz8",
"1NaxEnVDnNKZDrDR7GBu6xkiBjVB1SU2mz",
"12t3o1JhrKRSG46SVcjeRr37iBX4dXaFn5",
"14aQHzBHf5ozMXJ34Vai17v1YKTGHqKUS4",
"1MkL6xUgG5FCGFzMsdPC3cp9tcAtJzxhu7",
"16kz4QdeQTdo2kjckyKfWDqdeuJVbb8xJ",
"1GfRQ4WZRU81afwpGvsBYy3uJWN2BfqMQg",
"159JhF15Vf8NGU57Sy4ZtwiY1L9Thc6UGp",
"19Yq7w8wUNVncZUC4eZn76FzCF61PaXXyb",
"15ovvYDPy13DdpxM7PVQP8Px9u8U7F6cvQ",
"167jEQF3F6JaS7pkpjgNefkigtTQnQj9KE",
"1H2xFiNWCgLSfSDHi1obE44SXVotLuRbih",
"1Mk3eHDeGVR5NoFtmsrwhU66mXMPvSESrt",
"1AiD6SMTRD4FXQsj6Tz9G4zeX1Mm7jH2nL",
"17gukknBkPhVazZUv48bNmP96r58EhHneN",
"1B9Jy2uU54cXtoy1QLU5k57qErANu9Cz2o",
"1LSkp7SgHPKv1LwcMGv6L889x8f2ts7zKD",
"1F8bagGAwsMQD1NtsbRK15Sp92cJ6tW3kX",
"19qJSNGhkw6xb62P9NBoMWF7X4rPGYCrv9",
"17Fo3L6YBMPAV4KMefb2gFCL1XrxD4fdxM",
"1GLTwHM7b8v2C67zSY7DHw9uBjQsRVLXXx",
"17tR3xyg4tT5FLpQNRipXbmCf9ybNMeDcv",
"15Db3xZYhxzExrQy88yA8MAAmDN8QoRjJt",
"19WBLoRaKZLAfx1mkshEJzZtdxqHiJWBE9",
"1MUTyP1sMny7LNmUFqHhaKGpTp4BssZhps",
"12oq5maHPj5Ey4GVhNYFGpcKdnjvJzE2UY",
"18MbnYQVRcgYVnpN2EcphvsdPgsF2rJvGk",
"1NqHrtZoWXeEkPQ9wzydAViHoSeZBp7beQ",
"1HsCiCaaznheugsGJ69gLTcLuJKysMYLeC",
"18RWmGFr5PimEd5WPS81cPY9yUP7e16R9g",
"1DP2WoUAK5VveXzeGpzCWmGsEWjy6TxxX",
"1BMeVm9QZ1knVmYuhztgRUfQYeqVxxWDui",
"1CBUxJ812pqjEEGhrXakrRQ3Y16TALAyKW",
"1AiNS5MCBtscqgsZ3FCn8kakDBKH3n9h8v",
"13hF5ca6L5yjpKVz8Ex4MyihgETa7Pd4Fu",
"1Msgsq1GFcKaQNmeEQAeCZ4hHFHW2QJnYS",
"1Mz3ioPNMXoqjsz4veti9jnU7K46rKPzEQ",
"124iWPUR4ky8GMZgnntE6x4zLYLSbavNzk",
"16Ys7YE9BhT5HdMY3Ls9pP1fAtiVL3sLSD",
"1FBPhpixnr2HUknPUsUGicEADqJns161c9",
"1Lp2oWzsZ5v12HpEyKzexBaFVJLGvtEicR",
"15yxvC6t396px2fkttGLFTmpWxHhE3ZT8B",
"1kBntdr3ikgi1ELGAYTnAdVcMsPsNwJH2",
"1FZYHF79djWB42Y4yxscmhzibTEWtdf7Fw",
"1JqmTc9wV1ssd2Ve33sedhcXCZP2KKtiUx",
"1GvYJTeHp73rATxGn5Ci22Xm4sfTvJ7z4p",
"191LkMKu9deg1yg2hsjgmf4BA1EDVcZBge",
"1N1jvhvfjqYVpmLczX5rkgayMW358vHm5L",
"1GucbpBDK9kkaSnNYxfURdLzNP71D5uL9Q",
"1JwoZvMLoCdDzbMWrXK15R9WGbTvSCzyXT",
"17BE7oetstKJ7D67aszPLjrS5GtTDCdZi4",
"1GxSXZeanyCTS6SvJ9xEEZmZwYBs6MD2hY",
"121efL5LFHobiZB7sHECTkYkBi3VejmdrH",
"1NXm42iby4GWKgK5tiGq3aedBUDYnNFqg5",
"1F2C7vMhHF5uDGFHW2rmhaDfbmsKFBgi82",
"1EEvYNsCCKBeKv8umvxKBu7gSTR4QoMvZP",
"18qyMHgdxrjNotckcHNbyS1esTaBWbTeXM",
"14Nuj2A9ryDLQeyWeg9KciiSfmYj5cHMRs",
"1JXHqCDbK7RWZrTUbZEzisGZMM4TUpBx31",
"1LYVdu9TJBuA2bjwe7GF6qjhBLqtu32Cfk",
"1Bf53BGWbVMMd5tjGzXJ97VhUT2AuMqSXh",
"1LK2QNVD5gz4mvpfDL97zZvtwYjeebdjCV",
"1K2GMMisoRWoP2XnTVfc5EG6XJtGHPQyMy",
"1PXY49z4YKcg7FdEe6vVPybvrmywcFf6Di",
"1NgWJbPLo6zzshxcPvaVwNoGpfuCut297x",
"17VGwCMSfPSMzKrFNCGRkbKhVKVMukbu4v",
"1FycjSiHpCsTb3A1NfUg7dnM5macNbBdLh",
"18cxrDVys1G8DNvTmdwMLuEk9RvSB84wV4",
"19wrvGyejLog6DHJ4C6cEtxubjgELzFicZ",
"1EM2UPBwgspLwop3yetYTSXBBDVdJcwziQ",
"1KA1iwFcvfNZgWtvbHaz8GUGEHrkRv58Lm",
"1MXBxi6P2ZMTBBRGsyncYNS53bw2Dq4NC8",
"1PfRoZWRvdhdLLMo4FR1qa4yAPuSf3ftDT",
"1CnhbqadPMChUcypa7JgSZ9VgMQs3JvELR",
"1Nn9XYsEoKoiuRj5faW5CM1v42hZorsiey",
"14wcXxqgqudfeUQe1r6guArcTBAiRxwLrg",
"16Wd3DW4XvGGYWupFH8TRvSwmafiyWAJ3E",
"1PmyEZMvHzJYHq4yuDDeNYpxdn1UXG1RBM",
"1DaWnbXyiyzq9BTaHVY6xWeFntTgw65DNq",
"16Ka8uA2qQkcnnobCR7onL6nxQBwsBobs3",
"1QEny8cY3eFJ2ufRxgS3StHQBiA6cdeJ8L",
"1EFGphVDb4Whwh4FDc7Y8AuNz55AgTm87f",
"1T5xw3HBaeMxzSa2bodfKbXCnuzPZVLop",
"16gqBP3KPmJw3gZ2gesh1BWWJZdfRLJYRL",
"1MC4HeVZMgaoNk26Xigaz5CyykUXWMnBiw",
"16MDcbBL83xY6aPEa2h77znRy3PcMoSvLx",
"17tzYU7ypANZ6UVHKjpq4WfmKkYYGP4qWg",
"1J6keXi9XnEj114XhHFkvSWeZjgKq96oRm",
"1EPPTyGcy74EeomzFMoZnbLdzEkp6tWPeM",
"15AEjED5eM2uYT3uLeMTJWTMfK9TTRVm7b",
"1QCvq3Cd1Nv4G9o6ef93GqLFLUKHUYfGwh",
"1KYHy9GQEp9qpsPdZaPif7689ZZaN6QxLm",
"1DHbRWqUdznXCT52CujKPU5jQfo51yLeLB",
"1QJVr9sFy2LoKeeWw4rE4oRMkjPBt68b82",
"153T3vYAXLM8cUft4waCbapVvchbd4NheZ",
"1PxDMFL76KRQ9xtx3stgN1CETqjJMVHcXc",
"1KJAkkcXXBshbp8emKN7v2mhcEjKuo6tbX",
"1NqpszU8W4FxfFR1mriRhkmiUYTmJ5wSsj",
"1M4nMRn8pGwMTzLAEuGx6FqjKY7mU1dBX",
"1He94JjHbwbGet6Li1mjazenNV5ds8EbEC",
"1BTs7E6Fz285m6eFnan8WEKevNB3ZFR9Bt",
"1Do6aJyHPcCR6dCWRiRr23uSAG64pRDQas",
"12YmFRzLSP784UAbNAS7agUYQgGegnbY9G",
"12N6chL3ZvPWhiyVebcAVsTLUJPDyprZyD",
"1LDmdMdiXyMb4y81pJLWb7Z5ZpkfDr99cw",
"1FjG7LtPELqHWJjAKppjfSHacWxWiD5xZz",
"18ake8Q6odqeUQ8omwbDjWvXQ6Ln4ihNha",
"1Do5jzgyCEpVYRYn6q2TqNM5twBA52T5Ao",
"13CYUgBiDSM63dh15Q3QbcckAp1dU6NUPd",
"1EskfjdrfoRH8LC3XF1tzKhCRt5TVMQNoC",
"1QCEnE7HNGQ567AJqU4L1CiAN1Ny4zc2qz",
"1MXDBtiia14ZUjBCahmujLDjsP51G5ccoX",
"14zhB6niWweJuwVair4fbbR1GiMY34XTHw",
"1NvLNnd4kqAmNibdMe7L1BEVQHLxrAAFxA",
"16Piqq91HbNHzk29XMdDRVoVJ4JWNnVDpD",
"1GdwzUs8kjNJiz3Y3kAkWU8vgrm2EtSGtD",
"16CysNkrpnqFPKHFtw9pccbZWTtTsNNMZU",
"1FCQWgZxt5bRwxFDhRPC1v8Md42ZbMWP5J",
"13yV1xaUmFgj8ZRQKtU37w5aTsZfMHuYgZ",
"12phMmTgGoL3eBYqNVfhSWZgKfawZUm1kF",
"1An8G2FZwkByV5PBSfazGTryLzH5vvHoNA",
"1L5jXZvuXLBCYUFSxQtNuJM4jTbsKRFmPx",
"1DSBbZwJXY9qMzTE9LDfG4sbGpBKhpQ8ga",
"1E7HgNJmqvAHv1i5cX5W2dkAFY4gviCaTw",
"1TVENVVwoQwTSWuh1f6sfRruxxW8ViDTj",
"13TR3gpFRo12jVhzi67znWjwFig6RRq4rY",
"1GSfvbZes7W8nfJsTAj3FQx3P9f9Xn3wqr",
"17a3jwYZd7ibf7GhyJZ2rPApQ2EEXV4kw",
"1NBnh3RZ5bBKnmAcH4TQqhSmDYHynrzhbF",
"1AMk85sUrFfztJ2MeEj18o5mfjCFGSnbE3",
"1EZmyvPkcBsgr7M7kebpquqUciwx9K3vy1",
"1Hec9beGpYxE7j3qLWD4VB2Mb3jnyiMWVN",
"16xouh5o2Gy2yn8NtU92uug6wGZ3zu55z",
"1JS3wjwbHyrKE1jnujXx3473a1kdfXG9RQ",
"1PSf6iwLofrTgXkXQhqEzofJX4FQmj1nkc",
"1HQ8mp1Lm8GwnPVcTLkYy8aXVWFJrbe8uJ",
"1GCnk5ZNHS46HSzgBYF3tT622HCW92KKz3",
"1NxYnFET9zsUcfmkzGSdYNRRxaff34hPN6",
"1rntVYPxU2X3UBdu4ScupoxXC84jRkF6K",
"1CUShbTKK2auZq6WYugF3x5xR5eXCj1wNF",
"1KMUyehqTNK6nxiMy6PcmC91j2NVuQqDsc",
"1PNCyTBAUQt3tWWatRfeJQM7K2MfyxMhDa",
"1Fh1iMTajvEuyCqAJW5vzCYT58V1dNhavH",
"1CmH4GfTPB99eDwqPyDhDmQBPoJZHoCYHn",
"1Hq6b8AK649UdVDysDv1Rrbv5S97Hfc7vN",
"1Q1ZKxU9vRnYC813vt26YMkU2HmzqNcQfb",
"15TMFbTvVyXNzgkEjZG1FPbc5WthAEYvyP",
"1QJ62NcJ6phk3i3FjzsMvdZwitSVGhWgXS",
"14qVwomCPJfWRqBcC55mAGgBohR1XSUHK7",
"1KQbAayydBFhkaXNzfTqiqfoN7Js4F5yfq",
"176MMcsTqxgTRh419xJvxonbU6RaCcw1hW",
"1GT6DTeCEFcy663GGZqQyXb2mcbqxFGDc5",
"1C4uizDu8Rd1RVEqXPJPheByBXPkMRUogV",
"1L9JpXma32J9V12f9NGDR73AYHJbN9EdUZ",
"1GpAbdWH8qZiuWxuR4mfeyHPudrzh2AAwF",
"1BMiJiyswCQWAN93rToPDwS61bSoaxs2Hm",
"1Q7Nc8f7ghjj4h9CrShm7ixmPG65Kqak35",
"1AGuauix3f9NsfmMvX8ChEZLb5XmvH7tn8",
"1BiSGog3tkxVeGb69BnnzncwjF5dUWQwBs",
"1sn2fNJNw6NGfr2RTJmtbhDGUziwfL3uU",
"1979guHRdDJitHLUftJnsCHCt7NcLgoGAN",
"1wxLdbvjTXMGvyKv2JEokXRJjtuVheoZW",
"17fPqwigNmsWKik5AfAi265wQ1B3oCDbXm",
"12gwQZkun5GsbPLyHnNQckRqcDb1pgxFDe",
"1NpQye6iQc4GFWdZhdAFMAcY2N3YKewxTA",
"1AZJefRoypWTYQ2MxLRuzF66LUKU9PhqsC",
"19MGdNSJKe63KM42N99vrk7MgxX55H9Rz6",
"1JMVBp9ErC7iF2tFk2tWUffEBt8MnmNtzT",
"19KqjVWxR4J5NCdsdbC6J5BPFhAExVcbeJ",
"14RRoDbiQ7XPr1j4sJcstku6yV5ZZuM1qh",
"14jiF4riHzqqAuuNT7JM5Uomgtcy2orcpC",
"136SDNSgswwGN1BJKzYYstppp2FdRb5Buc",
"1Hc48MrKH63oPHJ4sv2r5DDMXjMQ2x9LLE",
"1FFGaBNTVafBUqMnJGNvZVwukXQad214k3",
"1MZyozLqkYWWFX4tXEgrLuvrL5vgcZG578",
"1Fr3phDs3BcRDGcAEEtkPKHWBTL4g3VDum",
"1BYf5ZKVjfPEgHdDGXyk3jAMwMQQhx39zk",
"1Fa9pFsHqBUxkf3c7keohtYa3AqKWkHRVA",
"1PnTnV8rY4aMjCgTcirA2SmM2sajNermXg",
"15Fy14AaVGepNSVfXYYucXE3TXUEm94zjJ",
"1P8wL7dB528BCHpPPboRbhqCLCLV1XMVeq",
"1Lp3XGms1wUNbJ9biXQCzcfniYt2qYBxGs",
"1DkgCW4xTAXiUhJ26PgwFv5E6dqp215kE1",
"19Mpv68pJcoUFA5Rba1G17UBoULiY9i9KS",
"1GWj4JLJ5hMQEqPn8HwKbpve9cX4RhVX15",
"1HvLvrGtk2uPP1MeFVsXSTrsnEkPhVEP2G",
"1BpB6tpw8KokoNmJm1jQHTBmuGBVNA4pZb",
"1BYYfM64yuH1wd63V6G3ug88cKtWs1zE4M",
"1HZ1jzwMSYyDAugfpyBfQFVLewqoPQTUxL",
"1EgnxXtqQBEgUCTktY2cUwM2yoi1eJiT1y",
"1oAFLbpLMG8bNdpFnYPpsevfTxe99gr2m",
"17PovJjMSpH5v6ap2m1q2tyb2c98wMaAPj",
"1PjbAFHBF2RNYbi2tpLGJHEJjxnBKZQUGh",
"1KJ5xKoEnQRwhXwL6jHBkpYty2yLpFve4V",
"1EKCptfQPqLc7b6LRQ8Y5SC9DTxSGKuyFu",
"17isJbdfgJyowKQmkXMFsPe5qSVPFYD7RL",
"15LNVrvnW4h6TsRwtHYfrvUo978yuqiFiz",
"19E6zfY6QAqTgHsryssSPSmaBSbVfSFC6w",
"164scauTqetoRT1DayLHREAemAbQoCcRo5",
"1Dhoowhdw67ndRyCgZi6pBnobMgdr143ii",
"1D3DRAEt8kPtg6f2mr2nGyxjWoDb19DMbA",
"1B6EBCL1DkiLXF6dWbFtneo8ZNYT8d8MBp",
"133McLb6kcvu3tud8e6TkRPjpotzX2cYht",
"1GHXnFBou1xfK6J7W6fxKteYuoTWTj9Ase",
"16beScspM3ihXyhVbP5mUsdrYZBt9TEvYb",
"1CqBvhD2Pcz9XgPVapPc7SSPuJ6RiwqGF5",
"1FYVV1b5F7cYCrUgg1s9crDDY9LARMnxEK",
"18yNvGY3hYU9aRYtk6zv4hdFLMZbQU2c5m",
"1CRuTT6nMr3amjAM93MCH7K8KPTfM1fdPg",
"14DFMCCYJyCNQ3k4ATNqquweuKTexDkEYm",
"17uBfWr11VLCBL1aRHVvJ4aKaD7iJaFEh2",
"16geXHTRHk8pqedgdPxHu4BLt9ChU84naL",
"15swr63U424mJotyWE8R72ZMZvGG1nZ29F",
"1PqR4TKK1PHhpb5JgxPPrAXwT1oRQr5BBL",
"1EoMU6hhxt3CHwBMhEC6y5Hm12gw7VAWvX",
"15CtXshWax84QxJnHgNxAK9JRkScJMwdm5",
"1DmmUBh2QjXfs3T6p67Sy8GqFGH53yit99",
"1HYkQCQuYmZwUK7ht9nbtiQ4FAXpAGC5T1",
"1AMJZcXtLJdNfsJ5ocnMNhvDgKnLPa9Rjj",
"1DexDu93Yr9ajuXySqEQ4Pw4KXPTSmKmpT",
"18wdhArrDTYs5dDWkNsEwA8RZyFTPBrySo",
"18pvVxArHjC4L7WcVHb87v8tnhg8M6nKsh",
"1JQJJWtkmzAmuKmuSGA7eFv6TvauB5mwj",
"1NvZ5yPE1eRHhsZVGPAjACiE8rjw9JSeSs",
"1FGhRNSeyw8CfahwxutbqoNjfcRVz6kf6X",
"13SY2NnphKjVW6TCF7XWRjsEcRzdvpzrA6",
"1PuBEocvh5sp15MqMg9GbtUD48xwCJxa4Y",
"1HHxYSsJ7cPTwggkvezdRaN4SDdQwyJ8B2",
"1YtwRcvBuVXA9giVVtedeqgjM5ufDnrN9",
"1MTao26DwMmdF5XRGANusuQyzLofeJsDn",
"1FSW85ccs2fYPReWXKhEq9tGQZnMhp6tuh",
"1EqATmFYLNi5oVGY2mAEUq45jdTmVJJvDT",
"13aENjbo2LeXyE8SpR7mCFLagtf9HE76Rd",
"1JncQJ3MJp8G8bSetCskpHqP3wVJ3g8U3Z",
"1Vp2HpPAa2RnXQsNzBpoaFKA75CxYCAxc",
"1DrYhnZBCKH9NVGKPudD8ynwtoLiqh1Jaa",
"1FEy5AiiNStNjh2AqPoVwNL5TkyMbYAP5E",
"1B5M6QPqz2aFGWFmpVaXijyus4W2J3gQ1G",
"1Db4kAkDQ81ToULFXYJbE8cDDQboPvABGU",
"1FKhJhNUwHfd8mfXjozW1rgc3u7C85yW4b",
"13FzrpYDX9AVQAJ6VxYKkBmREoAobSMEFE",
"12rS2hVUVMofhF1EUiahJj2QH7S6mRZ7uH",
"1EpHs2crWoeP49Mbvj5qH7RaJi1P28N9NU",
"1Bda5EMrdYiG75frNWCfhmh6EGoPsTs7WV",
"1JU5HnuavqaHmXGwzsLzgJ3hVezXYWdpry",
"18h6v1QuiHn4pVzgguzv4ASD46tqQTy6Gc",
"1K7Cv7G6KWuD61Y5gwtB8cAWuhZ75nAPhg",
"1QB8b8f7knuxFfVNm9JnZVW63icvV7zscR",
"1ErKGaUUsfV4s9eGhPCrxFTFy6e8oCyJob",
"1Q5HkBqCKKtZkYQU5pwT1c263QP5LAwaQ7",
"17XCxMBfpi87taum96ypK8MWhbvJW21y7f",
"1F9aCxkVqxJ5G2Lxkw534ce8yRCXrNckWQ",
"1P8MFu7T5Cx1Yz3GP1osti2VgFLwwLVRfL",
"15H2YeYcvFxPK95MmWDpsPhMHaTNNJ2VzS",
"1PZCVDHC5ib9vpoUbAhp1hvx9MPfTdtXTr",
"1Gy75y8gZPnRioWZxpJb3Bc4T16p4fttUK",
"1FHecK2iiXGppVv6jDtRQR2Tk2aUJnvVmG",
"1NCxvsQpso519UUdpR11X5JXGSeq49fjPz",
"14WxNTN1ME1uNxGgrzSPWA3zikJqwZf9Sw",
"1BZRms5EvmssnAVUUg1WwYWsuo8S2bXMDd",
"1As7xKBUU62uCeJmtHn8jjZ9U3iWDz9WS9",
"1QGsNMtV9Yavcxyh2iTE444t6wP62QEY48",
"1LPu1tHkd97pzuWMVtyLmhgrENTe77YNLd",
"16brocdmnz3bbtMcwKSawyLPuDFBLQWLRg",
"1JeRvsEcGmF8XB1DEgQz8V1tq7mL4NmY82",
"1BivvfMnF8hnagtPtK7ny2xkSpkpuod2cd",
"18pkVnyB4aLKd6or7oGoGeJgy3qFnLJcAi",
"1BqEpinMbJdq2WExcaKP7rF125ySdCrLrw",
"15drUjVrKw259qwLqS7pR2DraaoDK8gbCK",
"12dZHgVy9FngtvGjDKConpuvZAytcjE87v",
"1FbuKCYZRnMRfcnS91GxUHY6grx51YHtcz",
"18ki5WZAAzY7MEbyR3zgWThv4y5ZiPKraB",
"18uBKR4sv99Df1eXFu53YZ7pAGGZ36qpjG",
"1Ah1WpKVDdd5wDM1cRvmCMY2Zidb5S44Yd",
"1BXsrwTBRtjyUHMkXaq54qdgDNUJPYRziD",
"1DZPu1rREiDDRDAQxQg4w4beyYW8usxU7o",
"1CwNgLBscSKXx3ZMrmftXCFhaC5Q5W57Qu",
"1KbanWckfVpRF81Wvza2xAhJMDLo5YA9A7",
"1EAsBQxhc2LaePorUeSdWxBMo7FMHrQeSV",
"1KJD5fxWcdk2zWoU9YVTm6soRvCUKXfoRk",
"1bkcfH7hWsP8KY8DMe5ciLx2Fjj9NsPCn",
"1Mi56zF5MwRgcf9C4RBN8Ac715MJabSkcp",
"1C9nBiJwP3WeAQSK4Zxj8N5CnuEwukvdmP",
"14SPU93hVPybL4zHVhdeiGyZAhUaTQ7Ue8",
"1Cauc8xJNF4FrvW1KdPbQ3HPCpvn2Lzzrc",
"1HvnnfgpUk6KZXUiK6Xbxe21dtp91Rkfae",
"1BAvyPPWKaaG45PbgYiSu2Zb2v9Vt25RUB",
"19UeVYTeFxUV2cQ7thy1sc3omJ4Juo3c43",
"1NgFKgbMjeELsLTmEMv8G6RQghVKognZcE",
"12PByvXc3aPaHJLy5JMjS8xFKagbsL8awJ",
"1Jk9Am36tbnNbT4sLwjYWeqRkCfYsxdHqF",
"16V8crpHTy792Yb6R8pDQ1GfKosKSCUnPR",
"1FYvwPwmzBLhHjKqENVQnHjVESEwJQ1ewE",
"1F91cmWEg5G7C4A9C94NRVt8rehjietMkV",
"1CwypeopXGsSZJ5X3Ew21jBHnzCRVjzpmx",
"1Fbk2bKdjw5417GZRzQ7xgekyVKLmpxjsZ",
"15L6L2MF5CPH5U2YXqnxbkm5ihVBPLjX2K",
"18rRhhWaNvCY75te9mtM4E8xvMFuqiP95s",
"12gwar7HkZ8hKeWGZNVtnnCeAwAjYfNKiY",
"1FTAo4dJ1vA9jKwLZBXaB372s7wFnrbYR3",
"1HQcQCVqRoxAuqPYTVUGKr1sZDAi5VQxfc",
"1FxHGNsVUyefEJSt9YXSLpXhCT4LpiFrLz",
"1Dfnnv6VsFMBnFwQXEN8YkqQqrd3fFiEwF",
"14jcNFgDyLUVfqTH8Gkii2tTNg5TGZSfyS",
"1PbpgkWZR8Kj4zQZ2pvtfHmsZXpayKC7uY",
"1LPDg9N7JfCKXEYGe8jCSkagYRCRuJed61",
"1BNMkzqijmexdZCkrfyPGSsmBNJZomK51j",
"13PVvSL7EVSe2UUS64uGp5JR5j6d4Kg2Sa",
"19PpxamqgRLFnTUqUEVL9KNec45cTRw7DY",
"1Q4eA2AZ4WGYWY5iyoogtDsaamE2Sr9nzq",
"19z9AKnxvGZRtEQM8vJPQGePPzmi4XUtmL",
"13fBUK34SVzj9KxAibWkq8HThiX5LgydSr",
"1E7dpeV9upHRfCFwTvogsuue94RzE5WnDu",
"1Eez8wvtUjwQ5bX7QX9gb2URMsAfAHCycS",
"1KLJiXFidhm177tceSL2dyDtEukAwmBFtE",
"1LPSWDY9gZGpH2kF9WnUwsaYArt1VCCcii",
"1DoMWkeLHALr4W2rKCXpFmTpnM3MrqKw6w",
"1EDUHjEXYdjP7frgprCxEeY4funZ3YQpmz",
"1D6Hj4Yg139EaDwoFJQNWjroZHkZbnhtpw",
"14FS7LVp5PnUTN8WvLkUhmYnYjWxMNrzjF",
"18RapKLoXBgfSSibbSx5Ly8zEAbEzFLTYU",
"1J3pHoTheSMM754Gysg2oZ1U4ia3YtXPg3",
"1H6BuJj8RF6yiU1Dw3yWTSFXBCxWar1Mb4",
"176FTfVfhRDocdxDfCmTZx9UpfxuNcQvuV",
"1JKmvbYFcWtUwe3jFphXL2rrTPTmzXrUEv",
"18CR7VnouzdHaqfceKN66MQjG6p4RT21Px",
"1CRqFiaKVQBVnxRYkv8uWAZNYSFA6Wqz1S",
"18PeGtRur9CWoEujcwuTTKFJJ2BqnKQGhB",
"153aVbmx1UHReB98Hbq3gm6udRMCCMufhq",
"1FXpJSDCUjMDvZiZZC6DnSbxww2jToLoC6",
"1SY5ZVeRFriwdLCy3G9TsE9LKQBnjtW1Z",
"1HtsuMhM7mUKqFsMXvrdZgSYKYqE44wRPW",
"17cW4cs4XHGn6TAuX3NjfoGpU5tXdAbrFC",
"1ARCjwk4SJ6nfH2VFHREQtTEjN2jVC3Tsh",
"19jt6i5DN8k38As6yQGG3JyzvJKCPmhc7F",
"15jhWSbqjVf9jU39RtxJ6DP8DuiHFBio3r",
"12bc4AGVJyKbTNmF3A7ijbrEXxzwxEb58w",
"1N8J25CCAgbdm7tJVGkPgbSZ8fEAMJxyQQ",
"1BA1Tt3rD6uKnRd4xP1JBidimBzjt87sQH",
"1NsqzRMcdwaFWocQzquoHgacWweRRaqGpt",
"1KomfszTim53fzMugxhM6xAUGzfTcC8ZmR",
"13TbbbYEqHG9srRD1HsGf3igvGFwbDzqme",
"1MiNnNhqZvLmmVQRte3wC7X9F28af6Lt4r",
"1E4JgEm35Pf5T9qXB5miAZyaoZnC5MnFcr",
"1Gc46LsdhcsGWsAub6MBFiFaFfWq1Uz8dN",
"1NMd9D7uBgYzswJ2sfV3vBXwjrZpgderet",
"18SfZWh79mXezRu2XRDZcUMkjWigCrExs2",
"12Tk8WutoF8nDxXLiP9GqsqXFzFqHVW5JX",
"1F6hFqTo7eUaVzVzWhg2L8A1fXNohsegQ7",
"1KCUD9fL7mysX87e52b3yHRMsqVPAW46Bu",
"12wkibPnYVLbuJvWkJEUbRaYrWdgDnSui7",
"17afk5frWPNoREoZKgbjpaAzzjfA3stHqG",
"12i81es8SGXji4utyEbfym9GqoxTKvZewB",
"1KV9rwjyzVoMJYS5tPCPs9WyEAPJWS4BN",
"1JKceyGFACjgyGBnP9Lbve66pe8x9FgG26",
"17QFjMD3gXVuu1LVZUMEeEsseP6rvdyfHS",
"1HaK3zEKX7LTSTKNudnEmUfJAdenLxwPQ1",
"17bMRpuXUBLmEsmnxUykRW3ST15KS6Wq3C",
"1FYZymCqC5PJvS8qh5PC25rmnsiAYMYBnC",
"18e8y2SXCF7ADihC14C8hBVNpf4Gqhi17s",
"15kyxBSgJLYDi5gcDAgQb5rFnWFRZ5DMLs",
"1CADh21YDpJHk6B8yjZk3SZg3JYSD5NEB7",
"12kNTsUr7fKZo6AkiLaVu8G8fAkXGTJwH3",
"1FszHZoBVUJTQMyufk5Wo4UKK5jmWiwxow",
"1Lp8TbD8Qypn4hZyngeif21PLvaCMR4DAi",
"1MwkH6uTBVxPZdcDR8cLXxmR9FZCiaaDiU",
"1B98u3iHKUdmqWNj1kqXnY5PoASJxRU1Bp",
"1EodtheSgstiXBZ6W6LMogF9xKMxucQBtR",
"1HH3FnfzKQ1abWUhUqJKkSFaLC2U4nrD8e",
"1ENAfyMvgnhQiU1VaMDCWTY81gsiTDkyrQ",
"1HGR2owovjbNJ1UcSsfEJCXJHiWYvfZprn",
"1DxLHrHK9oEsDDAjXntcYhsK4cBukBm9p",
"1A7npMRvxvwZsGt5wF879z546umqkqhDvC",
"1PzYvWURN8m8icEp5PJKMuhQy9mL6oX6KQ",
"1BPco9bKg1fiPkciwskjGfxjfqSTBTcphq",
"14rQKLWUedPM446s2d3fTmjvg7DRQyTdvS",
"1GhbQQa7Pgq2VouE4DNU23rDT8xA24uDFd",
"1HpCqNkcxpKaUw9hZ4tsLHgX7hEEMjkFja",
"1HHtfTbwdF5fcRajNqc3jRdD8EFZKHE98N",
"14GAP4z9Ed3jViHhp2hqxEfRun742asniF",
"19vFfTxmHRL7BgjQrnLTcxcxmBGTrKDNpr",
"14qjzWfHLbJLtzECoPgeCsgYauqqDoz46Z",
"1YRM1xY6TZYuKj9qDtLwAomHp66zoCZiW",
"13Z34pY52iNAhTuJCP9vVxD9N48TSrDBVK",
"13SR9td592KQhQ5R8t4vrBZdLN2KCrBDgm",
"1KXFkeaxAEST2mt4rL1hKuka3Wi7kDjGpT",
"14fSiFh5QGJR3iFTBadWTHG4E8hm9D6Wsp",
"1PTHQ3MFgFiCKwteNzUDe9ztr3Een1EsEP",
"18wrU1LqVfYmyWF9YRZwweEVtVMptE9iRC",
"1JE1T2otRdeQLdnJguc3wv5EzzdoaTcMTe",
"1N1RzMs9h8SjqijSaUisBPFoFhNg18KUre",
"1AdkoP1jLe93x938YHpPmLz76yZ1p5M6UL",
"12hjCK99xqr1a244UkVVNcAzGoCPjBo2uC",
"1DcNnNN2X9LisRoYEhgBGXdjHwrRvW4ptV",
"13qTDwjSLvZ3FCwnmnNZ6Aaj21PMoQXkSb",
"1M7BCMPyuoaAySbUM7ptDnWtVqjVzBbZCR",
"16NYHHi3byB31eb4gB2RrcwUPwhQWJmayL",
"1PkEkEG4dh35CeXpPqVdJ9jAZXCi2VuP4e",
"1DHob8hdKXamvzw6TvMBv7rZuouyH1JAdC",
"1JNjfCpY7KTpMQzU9gxf73nBwXhkvfa4KS",
"1VbU92N1BbpNynUDWmHMpJmgoSUQMcETa",
"1NFV99t1Vdt8C87YEaxaW5bXghK7TpQnAY",
"13oSfAPnSwjwKi8ZYDjsVbV1tWuGB1jYSZ",
"16UAQcet5qJF1NkrHdhL8Y8gx47Zt1bTkK",
"16eAU4ghRHCTKrMU6Z6HjixvhnTTpWqJku",
"1JwTt5PNkNLGu1WLRkaxzMNq2CrfP53p7R",
"18erd6RFDCk9YNHc32nrj3Uk25V8wZ8ohw",
"127aHVTPqJtiy1PsyGUuizoNXQoziUiKUg",
"18ryNWaCRzH7F2gaaoGStx7HZEQMUn2hm5",
"1M19DAr1rBzjB7fpFRDLqZFqdq6nAzcBag",
"1JCKBDzknDNbqUtgEYdMr4CW1PC56B9nR6",
"1CswCjM4nMVRZuhLsQKndZ7yqKxRmFWPDk",
"18DdmradxU9i8fV6EAM8ycRmyXf7mkdiBi",
"1M9ke63dw2wqu173iJthKG5PfQQCKUucvd",
"16jDnA2xPPohwTAYP7LkmgcuyCvTKnB3hH",
"1EN8FHATAVC4bMtUm9EGhpxfoJ2gahPVhy",
"1BXxoCxhiJs2CcV5VzWVHB4E6M736nF5ZC",
"14qnKSPfmnQzEhX2XyGZoAEVtv9N1TdeVr",
"1C96WC5vQnkTR2DRNZQiNYyDUT6gZGwBgw",
"15T4bR5nMb4LnoA3U6T8YPKLFME5vU4Dt5",
"1CW9LmpUiLk7o7dhJ9vaP9wh3RLBXUXh18",
"1H4hyK8maDM5j4gT3gfjvQbzjXGjEDsTw7",
"13vTUfwkzMQERdbdgya94ujv8Kef5Xbv8p",
"13PdTESHw7px5eL8fxBLLjchozkd45keMp",
"152cS4VwqxG5sVGqTLF9oj52ctY6UbALf7",
"1NC58yRW5UfyoTrTTdz1GnBb5d4RAMQoJU",
"1dbZ7xFLMc3UN5nsUNL7wMaoyTiNrLk6p",
"1b7eTtaXyWRaGZKgtD2NCStxre9Rqh92s",
"1EWFBVrdTdEwRn7bDQ4VWiNCKLNepZrx3X",
"1LANfywiF4ZQsaXwn45UUBEJ9wxyTVgjWb",
"1xD7ABMMLXownqV6szJLzbK2VgZPq8ZG1",
"19iaPak1v68o6vFJBFub6fQe63CSPJv82N",
"1K8XZdGpAGmCQbEuEzpKta72AGe2FXbRNZ",
"1L2DYVjVntD5jaJkoBZSLkTFs1YHnHvn7B",
"1G5CAnw8aU1CGyxRmSarp6hSqPWa3Tq5qR",
"1GStvNBTHAu55tfLiMeBowohPvFbummjbf",
"1DuEyXUxQX46Nn6gCh17GziVrwPvFo5SZw",
"1Mwq28SoNP5ktgP3dD6hisGZ9bM7hNf6mX",
"17rdEN6LMUoDMnjTVCiJ8WrY8DQRjFXHpo",
"1215ykbcx49cFkpXuUwnWqtsFfZX7Te2C3",
"1C9SzkUrGGzrSsrAcZbKLmpgWhbxbi42DC",
"1PS5o6DCvMYrnovAQVLdebuLhTSzfdyKHk",
"15rofpwtpivfeWSkk3F1dhTRDViLppCDsc",
"16thJmrzK4yPVsAzzo7Bdf4cdFdcKqUGWE",
"1D6iAHhTPZ3KoxMKNTKLJK6uFiJRRyZZF1",
"1G63GRyuowKRXJJqNA2YeFg3KPB5ADDTdy",
"1N7VZGzKwGdMx3djzXjS7wwASRNRABZxF2",
"19AgUvkZuLAxpxZ6YRMNVCivnMpxWHAxXd",
"18DRkZbkHqmub9sNzTo6XHGwBZrbbWeNLU",
"1AxaJ25BkC8NMiqi7pfC33MLxGJtcNGN8",
"146NfVaGQXP2FZHCA5zk9ddbeJR6WN3AeH",
"1GPueCMBN21nMgqQnURMsguncMBAphPL4P",
"1MH6s78i5T78ChYmAKqEMWqrVFe3VY6WBX",
"1Kem9wg8udxvmMvWPuKu9vDq5ytkbh2mgk",
"17VMYqLv8KaPSkwqETKrVQJTMpGFgzc1h",
"19PH5LiVDSrhEfra23WKSPhQmNAs4hXXwG",
"1MhY9s61G5SFtuvvz1fzQqkkL7DdpzKbF3",
"19vifGJzx8F5mM7KskCUuGkecHN9SRHe7S",
"13gJgbYcVMDsvqwxzrBzS9vWenUkqPEE59",
"18yKKSox4o5NaJmJbhPrbXHzHw3DZoSfpX",
"1ABKgzDvugUnpkLij5JTYRzYb6Yms7hJ1n",
"1KLsuvkjtucEfjJstXQ66v5BocoQaujpu9",
"1G3oBDBHwWbDvs6ZsskCk1VAaQAwpJVQWM",
"1eFhkhj72U7c3GHzbD7PkHrjRnoEcPyUX",
"1H7rUcn5krxfFGEw9SXhPmXMt4HHtfn4Yn",
"1AHf8wgiMpaZxZzffFXbT9gKatBDzhMKRq",
"1QCBVynsHaCkHs8ZREjEo6D5FmPs2gQshm",
"1CvQxvfUEWUfvgbipneFBbgENKbTQp4hpB",
"1NAUDwQDtJGBARG2Cog6eTSnbkoa88NCCT",
"1CFbvrgWhYkQkgkEu9vgnoYgHdAuKmMvBd",
"13yemgcpv5dEEn72pQkW9f6qpekCqLokZX",
"1KYTWTXNYcbUuRfQ6ZdVjmpr1kNK9HvWyN",
"1Q69EMqw5H8yGGBzPPTmZFZvcB5hyY2VUr",
"1C2PELAdmCQBzXkaoo6nF4QPXJLH11dWXR",
"1G6JcuaXvnZhZ7v6VW2MHcNEYJPUJ4EhMS",
"145FDdJPKZVUcnK3SLdqDhKLE52hcNHmzr",
"1BvKMb5t1cBQFaXMRPbFQjJ5s2C2Bo4bQY",
"1DZ3PmruhxBAkCFDmjDDfztQm4zbbax4e2",
"1JVvjm2UQxPMRQ7vETk81DAaVGc6iNTPjY",
"1Rb8QZnho16BXgiAHXNabBxpKCyDyymwA",
"1AnG5Lo52nQ6dQVjhLn46kCQgCBk9hRTTT",
"1Ksg9791G2QG2KeJF3TpwwoHRNAZ4mLqAP",
"18RD1sv95iZJrCR1GPRyT6g86HA21nbzyX",
"1KG1M4ByVnKnUTNVXNqMWSQwGSxFYiFcvA",
"1rXSrWvE6aQ1pHK3sKFjfJv7RnWNXWDAz",
"1EWM8F3ajaaXX46BYzy6cYvazBydVmBqTd",
"1BJBufFUbWeF35pvX9vEsgAmsV9F887t4i",
"1HGrKueCu6FRxFdjpgZGwdYiSEXNkfLohm",
"173ZvLX2dzCNxoFmoMJygbasWBwu6s2caT",
"1765wJkKtFrfygkJu7LBf2g8h6byJiSAra",
"1LeDRwNbSdgzeXFCyrCrBtLR33SZ9dnC3e",
"15FoDDKMUBoNL5MK7xXMZkgzSD9eBDS8Nb",
"1ALSpce3HeNkxY7XGWBSeGane34yX2nteu",
"1PxCaaD6c1mzcweLngmvfov8toA5HARwJ7",
"1HahwVEWi2s18u3nbcRA3MPUQ3ENhGHAgD",
"1AUr7JNFzRZq6WszdVGaskSf7PqpkbkSBP",
"1EgitdtkY3JPvXTRL3JtAwFsUggHU4vXSn",
"1KtCiyeBs8CPHBmg7Ev3pbQuxMmLNw8jLN",
"16FJsRp9S892kHkKvC9YTbg2r4UxByJf41",
"1JvohFGxkLUEzF6iupi5EMKnbmpecqZuz5",
"1Ae6PdQsd9y1qfkdwcVwTZgxA4x8wVDhPM",
"1CQqp9zAyk8EyXSsQBxz3W5WcCrk7gGA4D",
"1CKQ2EC14FRVrGZsnauZ61UWWsUqwxtdQD",
"1JmHgiT13ZC3RxYv6xn97S4REXpp3rrLym",
"1MMjSh2tHkXfqMJ7hvNUVL1jc61txonBWS",
"1Q9z5EwUb1BYwiFCoAkStbQJD9cFpYy7h6",
"1KmeC1fqvVzBH7Jg5M46PEp91gMxbbf4Ma",
"1FPQ5Rurmyyr4PVJjbGwKS8FEQoqQsRBGF",
"1FPvw4m4aqPRUbF5An5NPoHF2R9ZnbLBpp",
"1HX83ckrnJqJ6SyLaXmcQmKN5jpx2KEwrv",
"1NfJDJchWhDL7DtJS7GdSjWL1aDCYcxHJU",
"1Ej3iR1rX7AvYCLdPnrn732UnUL22Q7ani",
"1ENDLiGnTJHRHR9kgozG3JSZtZJd2Wi4ZH",
"1GVQjYy2hcFvYxUgh7fpjz5kuKvzR6FHE9",
"1FdVjG6XZudNbxBEYKnDL4YK2SXy7rf76L",
"183rTK4kN64CEeGyQN59ydydJu41fRU3z2",
"191LkGNoSWi1pVXfRVyBn19vfDXqByVarg",
"1KzifKL9PmcSh3NB7eUakJWHC6rSugvWtU",
"1CEccUeKnGGowNskXodr1yN1EHhmTuKZ6s",
"1Btn7fQ1GwPaXkBUa9B62LkQGipDjH8F23",
"15NU349fvpqzdQCmaLPv7A6eWCojek8wRF",
"1CFvSDhbVvMU2iaviUFuNr1eVPFcTHiofS",
"1PmrZ25Z3atX2vCkPym5stwNY4JZjdcsUw",
"1DZg15RjVGLLncMmf4gPJzwMrViJD2WPSe",
"1Bv5dcZxHAimmWPLNsSeKGcyd6BhxWaC7m",
"16eHDGeDW3MdLe2kY5iS6CKhGU7sbH69XB",
"1HKoebRnLQbe2ZKov5d5NBuMwsQizR6Qyy",
"1MPt9GDuq1FNruHobA59C7tA4fGCWZb2t3",
"15eb3UxVTQ43SbQxk6sdHT5RNx8Fekz4gH",
"19bQbRRuUCXUkfdWXqJd7uWwwz7LNH6aJc",
"1tX9xZZY2fwWMcAxpGLyVKiwRcs5hJQL8",
"1Kt5ADtQkxJzgPCQXJKCGS2f1D9h6FMUmn",
"14wkQHKZsKjBX9tybuAkVi58tkbNReS3Qw",
"1L4kB2XmVL6PCPCA1uAU8cdDXUpSNKaqZT",
"13D6EP6f3Vwb2zgQPqxwGsU7rLBMnBbjzj",
"1AeAUasDWBDmAZgXcToY6RwNWdKmHQShcT",
"1BsZSAYExGzNvuBcZwXJ8MGYVEjedB5pMA",
"1LgkLznxxmNncg8anBY9gjMUnTTp7Ld2gA",
"1AZUxvmnQLb8H5f3x2BveQEVbc7a7LQXou",
"1CxdbhMcLomCE4YX2d8VzLte3vtMedmgXX",
"1KVbtcP6QkJwwViZPixW8SbsC4xAPuMeFg",
"1FoCfBRq9LdhSR89f1gAsjrcjxebtQzKDc",
"13b14FLJLPK1b4FRBVJcLsQTDVQr6qyRke",
"1PekYD7hRjrwyp8yq39GyhSqTkxNCrQxVL",
"1AbKAFw9HdZ5sSvBBq1wEwn9vjDa6uLKE2",
"1E42VSGo2JRVgQLzg6MmwS5u8Qk1hYF64N",
"18r5bMKXqPoipFQsMA5zNv4Jqacv6DMneX",
"1PkdFyDYLM9oSquRN1rbtQ8ndoD3tJp7yi",
"1AgcgGtm8fkMyy6VywU7fzFtZf8XRHCVqb",
"1JuYKznJf8vtDH4kWkrCFWb6eysEeKLBRU",
"16qbK6giT1CCDjBrcabBvRC2k5JdZdrHxM",
"1HfwWGV3UBpCnMGVuRzRWHBDjZDVWcm2pB",
"18cB992MdPJ3sAeZkW16iY1Qdr9NgM5dP4",
"1HQ25TekmpW76K7zcQPucQe2727Ssr63MK",
"16bbNt2ct1Z4pySo6Uq9nKJZ1BfCeVgjE5",
"14WVzYdftmdcrbZm68eFQo2Xrjs1Sq2pdW",
"1P2hBfVJheHmDMowMXthThctqvGPn23oSe",
"15MNvyAc2qAW6SxQphmPZCs3LfMtDNHvKs",
"1EbKEoCHTw3aoJH1ZXJdoE6X4KKUxvxfEB",
"1PzfUfdvjWLJ5DF7qAxJcMGojYiYkBKc4h",
"12naXfbgFEmG22oK5Um88GDiTenWXDqbYv",
"1A5v94Aqxe9rRNBt5ev2mD4cdE342T5p17",
"1J2fJC11ufxaHucur82XQ8YZ2vCqCLmmCM",
"1G3HeDN8jGvuSi3bg5Cqen65PKfbbH5KqS",
"1FRZiZWUBLt7pze4EcQHchJcAnd89DWS6H",
"1PwBPwVnPufEdLC4eFzKYQTJH9t7QPy4pK",
"1CJRykAE9oFP4Sh1yqKD4raKSRGrZDq3oz",
"13eU9iT7gS6Z4iataSk68DvZqGTgKQ94RN",
"1KxRjVsTGUJaugUQ56pXLa219nHkLz5LT8",
"1CMhkWTfT3fWCeNAEJj1UBRo1PYKi4aJZ",
"1NABhebPyok3DmTMcPBYJ2MmZnTZH1ZZek",
"1NzuFcD2cwQd66aYy7cXmBq1LF3KBRsgVs",
"1HitT7EgKMv1NJNfpwdiW3K5A59iQV22WU",
"1BNVbFbHZpxBmm3JsXPgp2mcrLJSZa4GiX",
"1PtRre4MscFnVstmvZTjbo79pTKbPtHYod",
"1ApmmT1rvrXWjbqinSgDzTweKpSL27NrLk",
"1CRuUVri3VMyuk5FAU8om5mChcFp6qNv5Q",
"12t3q4eMCa2Pt53rMCvmYS7kopo7QqfwWf",
"1FuVnj8nEM7iLXYQuT3YL7E4t6RLV93ysn",
"168GMDeJHutdiyjhfGtV5fBwtkMotLbSAV",
"1Mt8bJV8j2asqWkPrN8fMmfRMkMFDqVEMf",
"15FuGvQydLfR4azvQNjResPETPNzN5aobm",
"1ChmRjBxbEXFCv7dZCLxGAb2xYKZ7fB7J5",
"1BzWJHJvpijhbu869Jq6PYyS9wDLpK1CRT",
"1AyRpSrov5T1Z9tXNDkwMLbJafr6Q3kLBC",
"17AA9QA4tyL48SwoMRiV8d9ZAs5FB65o6C",
"1N5K87dKhSGDExRfto9Qpo3282p4LYWZ7o",
"1FjCUe6zrprVavN2c5VuWa6gtzqhxSv9Gv",
"19WwpubzFBdbkCAQzEdnwWfRc7DXYrQSZb",
"1Lwo9rKP2pkZmUtxAC1VDEYMsii8NMtEKR",
"1E8egrjMVBAUFga75XG1TCnNMjuR2j7oKV",
"12DbKMAHicdhAjEitiW4bgKJcNjGdFvbye",
"14cCUs6pt8hDUdDMqcPPtGxCiSAtE5Y2Me",
"18fpYm64v9d1mRdcENxfyU7haj6A5cigkJ",
"1QAFUWmnd631YRjzvFA7NUv5yQ5An7VohU",
"1AfgDNkQ7zcepmNrDULcH9qZNv4AWzeXWh",
"1LJxAbNzAwVdjNkidysWB2thYsXsS69vxx",
"17gkVZWU7iYw1GuLQgEEYebp954qyNhbnP",
"1AsRiXoWdPgUjpHBhmVmhJz2QBpRDbER5e",
"1Ae4rQz66AmBjZuYtx4EPF9efPrT4G3dVa",
"1LEjeCoU8tbDiMeMvwyE9R1nYN9gjuVrKz",
"1J6njhrdWUGMEdKGVFWtHxdtaybGaneXUf",
"1AsX3P6E2H5Xxhg4Lam9U3fAPQovVe9aKY",
"1NRPmYVyp2xCtnhiszbjb2fi9NE6ScuPFd",
"1BEvpwkjGDPmCwM6hiYgUhSegmwNwvtNLk",
"1BNd6RyczAo5RHrGgYBpwhXVmmvHsZ2cfo",
"1DfNpbcBhRwQJy7pdQj1FbZ2SCZ3pBaaU9",
"1JA9eWzkZ5N6cJUrWBZ6jz1xZjC4e1eKyC",
"16JiUcFnTiwsJnjKXXQWiaQu4b7zpr4RS7",
"1JjGMbiwMrLfuEYEV6vhoGsJzVjiJD1ALB",
"1EaRX6Vwdff1efeqQ9Vq2aB9bpPGJXt1eW",
"1HstMsXxVeAcXagA3USkCWZpCzsePwsJ67",
"1MnWr5ck8dD3H1cePE4UwTFX6a5h6J7uFD",
"1M6rtWEC7X8jsEfyWvPqyqMB5a53oP8Cut",
"1LLVRrkc7xNe23mPQ35vgnT8EYvMPTC6vv",
"1FtEM4ia1kse4Nu8RXBWBEioB5egdZfqH5",
"1EGfEqigAyBHTXg5TXrLPZJKiUUvrhg9XX",
"13uP88bCcgpYHguFFGjRckgNFFXq2DU8ei",
"1PBqAbHvUVKMsKeYZRjHE5a4gVtNbG9fFs",
"1D43i2HtY94x5u2VdwmFtxRr369RoYRBFE",
"1AYyZVd6qVtyGyswsv8WSnakQk222w5Jyg",
"15YcQdBQ2GihmkVJSTHBg3h12HESsSGKyP",
"1yiLa7FcrjeYgzN2Nx4Pa9eRYRmMhri6J",
"14zjd4Sue641sPF3PNk4HcVTQATtV1WjZD",
"1NhB35X46keUgmzNWtnTrJnqjtpshJN9Ku",
"1Has8LfTDv7y4pXpufLg4rbJyRDm7SiqsC",
"179hXPYqcKXVxo4GbFAtRkk3nodRszwk3i",
"1Hum3HAVHG6ZSzLYp1GVpk6GHB7tE3jgJz",
"1AFCw82ZCPWCY6KpHxszHKaRc5AMU2sd5L",
"1hyta984sLf4JdwbSv6karGy2i4GT39hN",
"14gfcFT6B618vmFEdQJBMULSQp9tkfesHC",
"1Kn12HAN4bqJhDKz2wAHZ96w4bbute5QP6",
"1JbyoKq5J7vFqTPEaebwsBgddLU8sxEibW",
"1GGH9GDPq7qVtpXg5QaCJY3tuCxNBCX25b",
"1PPuMr3bDDpQMxxchQz6icPEq6mQBu7tXg",
"13ttzijFHn6ApVXnSe3qjN5ss3Xix7zDbq",
"1ACngUqQU8HxATM22xC8NwXDLMTceUK8mE",
"14484XoaLo6hUV5U4Eutjp4zMdDYQboBdn",
"1Q3Yj1kWiRU1ALBvq1xqWXnXL1QeemfzMH",
"18evMngGkTGWax4S7dTv7Ph21wiaKA6mjm",
"13UhJwYdPWN6fRgvcmKt5Sgsm1Vrarq7TD",
"16qfjna9btoGrhEaH3EvzAtdZzdXEzQ4qz",
"1Li5Ks7tWDYTtEf9vNDgJHLAtoi4zTPpnU",
"1CTAao47yr37BGWW4nuF27JrCXpdVdWBK",
"1FdSfXnZQwAk6dGHKzPCFpiTVWcnZVgdVx",
"16taXJe8dPCWvqEycjXCukDnmUxyyMn6yN",
"1GomfJWRNMbFaoKKtCDtQ8c6LSJYFmcmZ5",
"17qLpMfUyBGisdznY433e28J8FbpBRiW6q",
"1E3MYtpBZNaCULc9tV4vyGSfm7fVu6peyu",
"1e2BbNoJbt9Mo1Xqxjv5aFJsvrZZvNN7a",
"1DGAH7EWKoVJyQNKxn49BmhFfo8XxRgQTx",
"17zL2WAtDgB2CxRDXUwCdc9TUWWavxsXob",
"1BwSxadB6LMDxiw5BLybDRh7p24i5ErVHP",
"1FwsUbAxCb3rcmfiXXicXL3J7qhev7dJaR",
"1J44nQGhoRGcKMjwFN5GeNbkEddDVWAr2X",
"16cAJ6cTV2AtZKe3RicZsYQeAN3gPvaURo",
"1DkCwS4qeqAPq7BUerLP1StWuRnVnuFKbi",
"1ETq8T741RZkfFkvtPzGfHL1sxCeUSrHbw",
"14AdizKZXQYRXQCdDkftG9r1W1uZQGVxcn",
"1NhfLGCC9QKq87uZATtwzxxb79tbQhzbLV",
"1BhpjJmSTdmorCnL2aNv1AnV2V83q5bX95",
"1MzTLhDFRXPjN6MLb2crvWZdRiEX1LRqd8",
"1Ka6YHhohUX6JCFiUDBYz38R75P7J7vXbx",
"1465qZnMAggsL3xgM4PcNBoBS7SDs7o6tn",
"1G6s31TU9b674LiWXMe51Qv1AKRwDhBsZj",
"19L1td6Pf8btb1wxToj1yXhUMxKJHBMo36",
"14yjV5uqpPgUpQ2gEBm6kGvhZ2ScVAh8jf",
"1EF6M4DXwgicGMbejfBZfuyqWqnBNBctWD",
"1EL1gisb9G961d7Cc2NkMHK2east4Hkf55",
"1NjUMW83ENimKCCsgoQ3Q5D8kpXyJW27Sz",
"12zqj56Ya1Zcb2KZL5Mf5U7TykUUA1pdHX",
"1A3Hmuzj5f4xPESCPtc1QCsxDsMr58o2qS",
"1D9fFe118VoNYF8U9aDTL4LThMmW6tyxod",
"1He7kbK9AQNRrj7mYMkeJgT6X64qrJHbSi",
"1PwxqmNW46fwukbvHi2RzBtRb8dcZM4Jje",
"1Mt15yajyAYGeAxvqNAHyU7GjCyhoRiuiM",
"16kScH7H1P2MQt9xpmWAjnJ1y2CjPMDnRG",
"1FdfgVsHWuLNU46z3BhJXjrDvLhGWqzTJe",
"17QxqQ6mx1RQ2V3jhrMZCgzu22ytnL33wj",
"1AF8De99mWmM9m75aEM6zo8JsCboaPjGuV",
"1JnPqPv8TEaL8kJXVCspqEoR4gfejf1vEo",
"1H6rw7cqucacqPf6EBN5UCuvHAVirCmRwr",
"1AXjmZnMV4hVHm9U7hiJaWfaoAFQJJzqFK",
"1HsZowjRGsXQrznJeCAg6LQGv4iaoDsqAw",
"1Q6Rb3n7Lj9xWgw7yDJS4eLgNCGteN3LWQ",
"1FHX8Ufe4n1JxKGpTvMtZXZkt9o7Fijihb",
"1ESaqVw2njuTnDcGL9njyQis4dgZ75pYyt",
"15NgruFcKrVhf613a1nLVRR7Z5FiQAsCuY",
"1KRLzHtXXThGb9oRFBTDqhGprw7LDpZnEx",
"12bbst3PCapF4sxTNWrEcxBu4XqqcjYQVz",
"1NHQJkXayJP2Li8unTZRfrd4Re2HzzNCJo",
"1HLjG2oVtSY3DU17dR44q9fwQ5g87d9KPC",
"1Fhzv2a7wXSxSYVYuFpCYUxpHdjoJeGrxk",
"14mVuUcdSmid37MgR1tY71n2HmfL89kqoe",
"1FrN9Sffei9sQKW9RUSQeRekErkHsT6qCY",
"13ZPbwv9d3ZLCfAw6C3zScg798cjnubQt7",
"1LGzJTUwmCx7mtpJfuTUqaJE3B3v2sfi3b",
"1JdxQMg84Xfhhi7gmaH9PRKp6NEXRcSBN4",
"1FapjoHdtiChdKRFpKdiF2xTg2fGNJBysS",
"13K8k6Ax73nDAhz2XbAq4R42GpeheXy7yZ",
"1GH5BzctmXPU6juB2ddKmRWCwMCprT1jUh",
"1Gf5Z5DGenBSJYDnJUVrBKaM4DVo79sRrp",
"13WV8AU5ahxvRbmNDoYtG7xsv6WUiCu1NT",
"1EW6L6JfVMPx4AEQEwEYyoyyTbyK3LARM3",
"1LdKsL9db4ZNN44RTaxPuu6vQanv1WjVim",
"1NyS9wFNgVb3Xr4ZYXfdHCD4ydvdiWgoUy",
"14byouVciUw9SzCZw9Ttoa13Wci7h8h4gW",
"1J695oGMf9MEBDLcmZAeBuG2r7C4FRd7Tw",
"17V6qtasaF1crpN726RiVhDunfH5YuWBHK",
"14S26wrXCjRLbrNhPFRhvJNdynGQPDYCfE",
"14z7MHJN6V4oSQ3sxZVRRdy9CF7r9agHJH",
"18sc5mUMFiJGzkGQrWF8w3aeKkwzgiZBjJ",
"1K9euQYfwYeStrAh984jM81K4BknGQXZzC",
"1K1YGNzFaDHmvtYebGsqk6sMGDum1nUUBP",
"1LJ1ruHraCmHRPQYELowC1DeBz4Jg4Cu45",
"1G6ks7q9kXEu3DKbT6TS8exXn6WEh5jX7j",
"1P8sdXmGNeJ1RSf3SatTsidSpxsAgp5pNJ",
"1NoCXGHueLEPqDyeBZBeSUDG2LD9nRsJ1Q",
"124bWy71dDjTaR62D3LN3v1duHafYKBw9u",
"18zzRa2rTVfHje56WUsMA5dJwibA5oYuQ",
"1AczHPj8kprSQV6osi5yNfQ1aRMze4udi8",
"1NDzXckMUdFqQdMSgtKcXwMHoLJVdjUAHE",
"1H7beZY84zPHJAbS1Sz4egFuY6LvhWBymv",
"18JKP5b97KQ6mMja1JAD5ENtBFpN84FtHJ",
"1PpqpZDRoBcqXT9VoxuSrD6kkJi7fFshjc",
"1MnRBwMAQiZ3omv6VycCmHLQkBmPiXgKau",
"1BWJumEc8LvvSL8Rw3ZMKbHrNpCK6eyEBu",
"1GHU3mwymcqjMaDuFkTdMVKu8FqptJo42F",
"1GzYDpttA4QUgcCrh1NqM8v8jcRRNRXuuj",
"17dxWjxwk9eByZV7s9V1W2cq1KBMPpw1av",
"1GD9wUai8kfu7QpABJrMkKTZjQjhAtJqKt",
"1LA5gu9svYn7MTWgsThmrAd5Kd45XDAUWk",
"18b7XX3t9x3ViAgD6fqyTxzyN4q7oBVyQ2",
"1HqHC5gcptSwzgdZc9mErzn17xHex6YGqT",
"1AeFDZTghVutoBCB4mkNn2pGpaYCoR7SbW",
"16VWMEa9MxB5z77gW4t2cMnmGMFVsDbWn",
"1Batmjcvan7K3hdoGjjTu5Z3inKtZizn9a",
"187u9VDUk4FVuwyN3EWV3hA82FCKEt7tEm",
"1KhD6uABemxqZPSn4rB4M1ukU7QuByAtYq",
"1G6SEhBVMdhYVLFDJrdCXvhcgSYWkrvyDg",
"1BxM4zt4z7qJCsT2K8xhiLrL4Y5DBcr5xp",
"14VFbAyoovoFwefU3hdXgRzcctz1MLDfx8",
"14JYKw2DCiVCoD74JKL4jeRim4m7HCTXK2",
"15mVCdNRczfNtX7eh5XcJDzHRwN78HAz9i",
"1CHZQAkjiqXkx7UpQya3xTT82A9UJpoVpo",
"1gzpy6RhFXmwMZsBByKX1kwwcbxUSF8Zu",
"1GsQRcQBAjKJRo6nWecMfAupWwDKbTy5rH",
"1H1gBUMEzBiKVgdaH1jzce7rvJnyyj4tuZ",
"1JDHLPEf7EKiaoQDpxDG9KgFgnSDwHSRsd",
"14QJGVD5WTEC39LpGoSbMZqVN5VHrcuDCp",
"1M65YvrVijbrdpUmTXS7oKfseMz7KjfKjQ",
"18t4Fu8na3HrzqoSxsu67p4cdoS7GeakAR",
"18X2gbTmA713oSnoy4RPvgy7XGRGJCah2Q",
"1MgFny5Nu4U7zbn7G1P2ZPRpT7eu4cZ35J",
"1KE2gRrXzG8q8ZbKe2LdwWd2ti5K7wEZnb",
"1NnNHgC6hpS1MV4v4fACUkt5nqoHKHhUF",
"14FdQSBdwMCTxLhSrXYppy7b7rjsyzGLAG",
"1Ppeca9wbHhfG62kxHHXjfmBRDcxJhLmkz",
"1AtQ8EsroBnW67UPDXVuC7x1uarJzqkjPR",
"1JRGjfmJHQuWp7jZ4nboEPxNCtJv9maCkn",
"1E3CKiXnJLffgBhaR3LJvJESgz6murAQ64",
"12SVqh3JdRr6jboUGc4vAyMEXaKzGm4oKn",
"1mbcdqsgPi8LhStfzzRdK3gchz1dQLMmQ",
"134KRXDivsrvQK7MAZphG9YuEmBBrQVA9N",
"1BibRs5gZSS6UKq5iXMhGYCJReFBLg4X5K",
"1JQKhjCWjDYuYxD4rZqihTuS8qhd8i1pS1",
"13qLUFU4BH6KPBY9b7Cg6UB6w1b3dxKJJZ",
"1MPRmoLKXNhEonCrAQdRwAj9f6WK1ENT9Y",
"15q9AtNvWLuZqKreLqTLLuVtSYMWNUEYta",
"145jmk2VKU8yCu8poFXghrYC1xpDTFPobz",
"1J2BY9mjwrMTztRmHuTz4QTHCugNiX7apx",
"1DEs9RKNuiXwZ5tKV7cdvZgkjV6rzAMD1h",
"14d6VTRiyep5oj2UiRA9ab4nfd8erdsb1f",
"1ENAhg7GwQMwtiRMgmo8R3S5CHBBPTWWdE",
"1PBgnARuMA3Pd3MeqYpX4toAcaH5QVaRUd",
"17cSymT3ipHeHZe4268ptvsMaf4PyC5YJT",
"16dWoofvQKDxq5W3jyx5aVJ6u42HuL9i1h",
"1QV2RZtZpQcAFVJ5qYeWTAXW7ptLaMbUw",
"1C1wAiCVNp6p87kHrbWTvvwMhTFfhX6Wa5",
"156s1zDvv8gV6xUshnwCgpiLADwVwCnvGw",
"16zDF4tAtNjjVyQqhR2yGvuyRzsZyWEJQo",
"1EioodpkHNdCtwPpDkwMBmsjaujjhnFpRT",
"1N8ooqMDvTehEwiQkuqbMfUU3ckYkfcQfk",
"18tbSLHh239Lq8ZZNSWqcEuruNVRi7dHf9",
"1GhpTzRDCixXjzZmiSqgQLgdHufePedBXF",
"1KcJ2aaQWVcSMi1Ldf1p7duMEvLxK69mL1",
"1PC53afZLtnbnRWQmjftdWzx9iwRvGYqiF",
"17Q8Xv8Xr8tzs4B6mHAJAFcCZyGibXwRX9",
"13sSFi5U4RpUp7ffv1qAGRXHFZVcCoJ9W4",
"15N5DSoEgYhdLTLZjnEtMbnvCY7jGe3ZMq",
"16yu4GcZtqUCr76hfpoc4rCAjai9cBkdWS",
"19HZ2xubGXoN3kcUB9a5pdkvuW6W9n5g5z",
"1FzZ6PB4AjwPNjmjzBadNn66tVimwaTXE8",
"19VEZqFSa9PXTVFDeJPHnNNCq3M5yi7y4P",
"1LWhqwCSmtrhDh3oQbYazAnGF7KFH1dwgK",
"1HY3QrZVPhUnXgDwnBxXiaSm7MnPjpWUt8",
"1FGHfu94KzvjXZQ8zZh1mUcnwecbuBzgnM",
"16vo15xWvKF9h5pSxMZW33NZeRnwChw7Lw",
"19YDrWReqctEjJCdPtXh6n117bAYHo8prE",
"146mgbB64VJN6EPmsq2H68RLPkum4oBSCR",
"1ADmrtZrf9jrhbtWvPAvXNT1HyAP5Si4qY",
"1Er2uHRzFLXHeJqyzJMBXkwVxEVFz5gVgZ",
"1CVKgULByTtpuKMFsQkd3NJ2hPuZnim7W8",
"1FmFxe999EALHxcG9JMgCaThmTRKuXtqGu",
"18fdRbyw3CoSnNcwXAgDbEB9wZz4twELDE",
"1FkLEBxhuS2LGQXjpyaAD3EEyqgnowPJsE",
"1BhjfXpCj34PF9c4jiUpbpbKkfafr9H4Sw",
"1LuFxorR1AWjGti1wwK6NbvvJB9NdjR95Z",
"1D5txVJVSk84zBZzH7vSHGfXSYMk3Q9urX",
"1GjViRBEFgKJt2b7RXeeGMVcyXb4wTbwh1",
"1H17VDb2rSXUdz2NkD8M73wdFiA4jHG3st",
"1Nk2o26LB6D6WvrAT7gdnoKydwba5E67vf",
"1Bp55sbdTdxbiWbRfk5bs9y4TxbxG9yNzd",
"19ex5EAnR2J6XthXETPxQ9Em3Kh7DMyyYk",
"1QEVLzTNSB2YA3m5w3pFu11bg9w1VXkBhZ",
"18N12bP7YaHCFqhTYB1Y72z1ciq7tG7Tkm",
"1DjCaiU5TaNZdTSJUZtaLyJuRRt78oSPjg",
"1AKAAABDJehMEspMkN1g4psbNsooERm1nm",
"17BLGFumghgsipkFDGkm9uwrsv4iLEGcZq",
"17zZTVFmtzgUtfs1Ri7qgwy2FcNRFFR52F",
"1EVKAZDsfxBXcbApW9w3kLvaPzTqXXcx3F",
"12wG8CtuR7bXeCwLyWwoPVSh7bfHeWYQ1C",
"1KH7r33WdpmJmxYrQwTSZmtr1GjKVY2KNd",
"1LNjit3DFr3cjQ8vRZJskLHv7xKUDPAmru",
"1HsQoksK6XKzgngxmNvLcuAXgWWfiwMYCM",
"1BPUddWPsiuVyTaSiDZcgwH3SuHH1xKpaZ",
"1Hq8KgLVRSkjqhMovRwAxctYbDAZ1FCbFG",
"16key8U9nG5a5wHFm5kdfYrnVvfrK6oz7o",
"1P4jkNaWYy64cp1jK9xboyGUiU4bnzuypa",
"1KFXvsLX4ZUsuxCPXWZocV9P63kzWjftfF",
"1H7eLeBrzdcFjkTLpCj94qosRnHUAercu8",
"1Tn9zX8n15x1o5AZXg3xHLzy4SGQAoXQP",
"1GAwPgU318jYE9MDKSqd62bpzqscdMsfar",
"1KxvRcLuXuhWwxfH5g9L9vjPKAv7P1VGgJ",
"1KU9s2qocrqssfRZDfrSYbifwixbXVebdC",
"15WCeA3jV29xsJnazYhC4DwAFLUWSSV45o",
"1JXzGH9wrtndSwYaQ3CsJmpD66Tmv1yHBM",
"1Ma1yrhtd55qmcyNpp2cEqQpQDttpnPsfE",
"1Nc7WJFMvRXCRQRCNAsVheYAYtw1GoefJe",
"1NkUMkDfoLb2ZpyKtePrWTCU9L9ay8fQZN",
"1Ne9acpD8uxWLjwajYUM1LgEAmyXDDu51o",
"1Cgkt7hSpXgTPrrVDabJdLPwAmKsRhtaLV",
"1K5pLtgZx8zuw8JHSaWr1oy6qQpew3n1Hh",
"1PmVXbF2gqHpZHxt6Bjx7uAucKPSab6jgL",
"1JVCswJotJysHA8uVkGHE3gnjHzrcver4t",
"1AbTktDBVZhcWMBXHtgaGwK11jr72C9VQD",
"1PNPHnTtZSPUQKij5PShhM5ACLzShkKbnT",
"1An5RSntDNRFUEnjvfRM3BsedvtEfvMKdR",
"1JWNwDgAEKmKBxmAb9djAzMRf5KiAhqfzf",
"1Jzom2wzaBeUGmMq9y3XXfkdvHyYqnCUJJ",
"15QYHYzppDpwJDUw6YVD1XiqHsTzcnieP6",
"14zJ9r57AujsYzkzNt3uWUCR1t1CVT3uNE",
"1GScaHfRHQUHC5Lm9pCZ6YVCaYNx7HKJJ8",
"1KmVsiaVUCbQwr2xRpSZcPyfRf5NCL1wFY",
"12Rfbe69XFEqS1cwhQZLxxcq6gSWTpH5Jc",
"1FoT1Ew1mYk3bL5XiHMuW347Z3tztiQffi",
"1D1FUzbArbC3ZttYbYm31YJH21T7uJsuDL",
"1MEMoqW73aWxUdUWW9hK83AihR3NrEtUU4",
"1BCfLUKpYtDkLQs2jW3AGr1YoLua6YUbYf",
"16JxnTHh3qppwYWuQgzhSPJUsfEVupfieC",
"1HSPyUbyP8sPAbbJP87hr7D7MkypgPrPPU",
"1Bt1ptefGTfMvzpRTSB2rLnY68hMZ6HcKG",
"19z53Cc81VuU1wsVrNaJ5VX4sRLX96dTRJ",
"18C2PHEAxnwdtV6y26TbANWgyfDCHQyXDA",
"1Hngy1Y5B5t5iSbRMZYWKJ8hudV5QbyUMG",
"19NhX7BH2AEd68oSngGLtW6K1y1ULhdVEY",
"1Dh33MhWzhZ6kgW4CvjLPr7UGbWLE9JHZS",
"1GR7kS3PDH4o5W7WTJdjVVtbM85uVmir1s",
"1Bk7tuRc4yrHu2FptZfKLTi7RJYVNFiMiq",
"1KEZgb7zcx6aQ2Y9pEPhwGkorcajLMBLNq",
"1EXQCKiKAzE57uRbyrwnGeAGZ5X3DvQYNN",
"1KhYQoKxgYvd9ZjqU54cEyDhSBgErNi9MC",
"1BYRyFU5MEnw9861jGnRQYyomQvBmy92gk",
"1HsxPHj1sMLw8FfxSgbmq5KnuTnYD7DQF",
"1HaWrFfq7HF5knFSpFpzbYgqkPoWRq92Jw",
"1F1DarDkLD8mDoz8cf4fEuGH2fDAT6ius2",
"132xzkKtKZ18KK8XYjTX3KZa3tNMWAqaQp",
"19ksDhFFBb4ze32CKrr6qGKbgxF5yspmcY",
"15KNuGLwX9tcSdQbRwgYqd3yK7qu3Zqkj4",
"1KZVu3TrkPu872eSJ1tYwwmVinsEnXzG2B",
"1GyLxcd6boNpBwP6hoS2htb9r8fKJEfSwr",
"159JhYHbimDa1B6mAKNpV2KUiYcjmB9vQX",
"1DxxQ7z5LxXNgNogHp4neH8u4oQcPiRdVp",
"1KNmYRTHRiaYwrjXg5TZqFQcrXeqMaR9vx",
"123NerckdFkg4riwapdpE88FjbgvNuKnqU",
"1NzsX1KY5wmRC1mEo2UViCar4HgsYptLpJ",
"1AGzu8rzwbCW1SQVayAS8qGKB9qV6Zw3JP",
"1PPSLEiGo5exyw35k7PB7dxSJzrLrActCA",
"1K117toSEKi9W1qi5c3nqLcyRVd8xpPsce",
"14tTDSp84okLwa2KfwfRm25AnAPSVnPM72",
"1Kr6xKx3LX7NCH67wyZPsVNDBnj9nbvjYr",
"1K1xZsCf2L11jY2aPNMaNq1uUkBaoSHtJp",
"19vKjWm6uXJ75xfEf2TrchQpTBGadvyTL",
"17s3vCSM2koS6hQmBi8YgrwsXDdWH7fKnF",
"176MkgCp4PYsXpijfMsjgtmNPoRtoZyw9Q",
"1HK8xpPHXn66XL7yWrEzG4oduRgGUvqyky",
"143HXSPFptYg4ZjBTgtiG7JkEiBsYdwmap",
"1BPf9Ts1aZ1mGtwwfLFevCPUeL9UEQWQ49",
"19ZLYALe3ZUX5WMsBZwspzb7QwqhtGNFS7",
"1KxJ7zL4Kx85zyfFxJYbdorV5mXk4ddbyn",
"1Jq2GNG9jwD4xt3x8ApukCnKRkTtGbpFrZ",
"1KUdLLL6f2frKtjaqaFkwiNby3HLXXze8n",
"1HHNFss3zEB3pPBFesfyHorKZvx2HYYium",
"1x6X2zc1AmExMVEiVq72dAeoyQguh1Y4c",
"1G3E5DR99xsS3DtkKicFE8n9GcWK156QvV",
"1KvmKhhhfGs4fTT49zBoRQvMok6aaSMaUb",
"1A9xDmf6et17qRAK4Wnt7272MeG8yn73ES",
"162WDMnQphowJCsQuJwx62rWSdJDV4So9g",
"1E1cJVdWe96GJi43NctP5GmBJ5JXsFdgXR",
"15dZURyFSvxwmQKBUFvhxhnhpSJVsKzkDF",
"15cRL9NPDo7VsFR7MtqyooZASZwTADaaQk",
"1Lx7i3skjin3KeFvLieEUYC4SE8AXTV1ut",
"1NKxshQ7nEvn5YqkrwWiDKTemEmeMXt6Zq",
"1MRx9QVERnpbC2GDViKqBYEK5NW2dNWew8",
"1J92xB9g2UAUX3iTtesgeN5Nw3g1yyMj6R",
"1HmyTq2iAnj4ndYkEydbq8z1qs8vMKSCAw",
"1KR3DsULjcEhfVmDkEmEbXGHSx22yDE5jc",
"1Gwd98tL7NLxcRFTxpuvwWsg7sHai5HUpG",
"18GDfshu4uLpwdXV151BpopjUd1CRG2KKt",
"18DD4UQNChFE7HyoWiDA4ENrmieayiUd8V",
"189WVfCoxcJfFtVsZYpfNXXKUajvKESk8t",
"1qow7cpU1mswNfmt9tgyKxvxx5fgrZR7s",
"18k6qZQryiTaFKVo32YwjBSbhxE24XCvL8",
"1Nwav5J123tFPLjYMAwWsgPm7YDJUSDj2H",
"1MhCYN9u4Uso7J1YxVu2GPSChd2VSHeB4T",
"1DEBD4kTrKuNk7zQytzZQoHF2Gvuuid6CT",
"1EL2fB3vacQZuUkybVgKphwXHLTaWapp1U",
"179kSPyDV7JGcMtvNH27KQVAV11osRWUmG",
"18TKxKSr8TWZNHoyFNtMHDWvvAMrAfCSG1",
"1Msnrf1ieynizvBgF2ARB71xLbqNcxywcz",
"16k5nSGnt1UECNSqm9pT9bXTqtCYaSbmwq",
"1Mdkqw4NtMbtJpwPbwKCKDv61fy83bpSS",
"1Bej6cCnyaWTH4v5VeKRXwCnW6vy8GEXs6",
"1DxzzQBDAKs8tvFTofUZnBAZuh1fhMoSkW",
"17py1H8xPJ4edP4cxC2KpBNR75BCjYEp9v",
"1CZnMq3DqbBdYhtB5kqjgTG9x7NFmLUVuR",
"18ATFFR1UwT8bsrYi5oajZNNn5SDi133YB",
"1BXXYAttWmkvWNbfq5SQiypu1D2z6wZEJG",
"1H1S94TE5BJ4STrmXMGc8HF73AQDiJ33ha",
"1PGTrQ5LowpVMy2Xro4en9VidPnQS69oTA",
"128HJ63fgYh424ovrd4mgsT1N6V6RBTmKN",
"1KgBZ5DNuJxvdTE68SpCRW86uythq7BoFj",
"1Cm4FzG9DE6PFZts16HRSWTZmxnf25UD6F",
"1LertHTiZWVE4G3iySNZmcC1RpzM8RhYiH",
"1MNtNL78egENRrwuxogcpvK6Q46aZtxL5R",
"14jn1gW8NNbjfiuMpZdg2mzX6WXoBZTBPa",
"14izmMjgehchmp8iya8LpvCGFDVKLaJMR7",
"1Dwp3UCb9Tpgv2HQ6QPQdgEsnJivrKjAzu",
"18ULvxi2ugp6JsR6TGncD5a5bYp4cUEzMn",
"1DEDawGm4wDZ1ZETPJidkbLDNX4HJs1KPW",
"1LB6mTdcWJACP63HJpA3tKeTbeYhfJU1Zz",
"1eyCCaEN5DB3sRPuqo9jq22qR7a7TAoh7",
"1mmVyrz5aQ7oPQv3tqncYaEK9fJy9BHzn",
"1EPmNacPmQacBZRgqGmJL1tWajVzcTTQUC",
"1Dfv7SgxMK5ePLESSH7gCB26QjnTpL2mX1",
"1GXs1wdfrTRGj1GMHenQiKv6ME9M7Ymq6U",
"17Hf5YkTNjcvqSz9gZQtZwEEjzD8G1dGuq",
"15CiTib4Y9eALH2QBUjhe6bP1qoSoPykcH",
"17P3eGwiYzCzq29K5m1xsDreexbNckgLMN",
"1Fw4wU34govRsGFH2WiYKisHyoUJbyix8N",
"1CszqudRZdE9MFw8jy7Ab83PHmCogqnchw",
"1BemcXGLbnDAi18VxBkmT6S8Q3mi9TSVQK",
"14GErrFfspCYBEiurpPfGE1GaNTR3sVvrS",
"1BzMWu2rUtDdz6KUMNWd2tBguzGnve2E1A",
"1LVV5Z1gdyhAB6trrKkLcLq4KvtxC4sDHZ",
"133Sy1cse5Krgf8zsBp2rDvSXCxhfWRag3",
"11fuu3Czd7jbVNLZ2TendRUC3h9nNsqsX",
"1Bsy3s6wyPwukmZz14zFkkXHto8NUQJUE8",
"14wY2ycwUYQJnvKruNfZT31c4U9Q8vBRuH",
"1Q7ZzdTWJwdnWhhWSNeJrPi2vkRfVjDXdo",
"175hkHCACPxZhCxxBJqpGE6DgjzMRQuxJx",
"1JQkBGJEYxhxGTxJMN4hm8FJXhFM7GjjDP",
"1Gr2ibeVYfQhsHWbx63Jr9kPaTtm5ZyAF7",
"1ETbsiH56DSiHfgpm7EQC81xwXFmJ1F8XE",
"12g1Dfko8ECqWZCZPFWo7aABqHpXsESiGP",
"1Mg6mTsr9g9Sgyrc8KcRptkMPXxEKuFpzK",
"1NXee52X2j4PCHGUnJBEdfSMEgjEpRuhFZ",
"16CjJ7esFr2KFBmSEfZu6MLFWc4ZVNQopA",
"16ji92j7ahNZLYEUPGSdBnTErKFCNyPGZh",
"1HPiZR4mYkev48rrUKgqgdxA7e1huRH9gY",
"17JgMyTSEwzsVr11Cfsit5wL8DWanTXJZ3",
"12zm5wi2bfFtwEbjM4aTjoVxSiXEr5sV3N",
"1T627uVasRv29Ex2u4fHQxeq4qiwmnAYz",
"1HTGnBzgE1kqeddC2kFumG42qyQCPw5j2q",
"1HCz6d9VnBnkhyfWTuKcAaMqovcS48WX8T",
"1NM7LTyE5duGBh1f42JzTpoJ9ULiANv4MV",
"1GWE49KTEpyyvoBMxxPsHVA2k7C9y5CqW",
"1CnRZTW6wmquJEnHKhiQ1nM5vEayq5u8AW",
"14MTjTrrrhSBoCniznx7SWp6ijrS93qo27",
"19BG8sVMrBQYwkQoSXBt4XUEzLb5jFhcf4",
"1KqnHuQt4EBzaxj7tv58X2EUmEWjzGngUr",
"14Z9ocbWJtKUxeAgLj1KGvbFTb61d1Znms",
"1KxnX5J8PekpAqkBo7BJZzcCwmZoGCHqMU",
"1KvuGXyWek4Dyv3utYh8auXgBtWzERNoka",
"1LZnZJevBVzSW33q7tTJEwr3WgNs4g83fv",
"1799V4VH9T4P1Gye7qco4fiXX2extveSgU",
"1RAhKpLhgYax73VXN63RWrtuh6MjpCNiJ",
"1JurFqX54fscLzgYnMw9bzsAd4BRZMS8bo",
"1Ng1KsqcUnuBh9LsFNTPXSQMJ4zjUYuFKz",
"1JW63qJnRpYU6KzAYhJgttkj4pxZBBNR7z",
"19uKfhTefTDwrKe9GvHbMkqpHbqYBNQ1sR",
"12dM3Eb3Zn9Dr4NCEzctSjSSjz4zVtpYZv",
"149dhmbZTu2qykuhYmRfdzZzQqxY5T8yNm",
"123RgGQyfbTdrUz29xGnhHQ3HbZJMRLBUq",
"1MKNEYh82s7oCnyD7FkGpojSBaWxCH4cDQ",
"1AaDtaiPZsYJbpFzgSC4n7pCsBqDiZzfbc",
"18YNHpYwvPbXHcrFTNzgiKcF38j1zHtYqN",
"1AhzPEpxmBFcUmundJnufGgsY6ph1VUyie",
"1DoeGRaLff1PLaKc8YbuaoBDZAiuab9EMz",
"13eMnXSVu3oAVePSqBXNG4tNMZ4SpiyEYC",
"17v8sm9LBecDcoroVojj9bu4AjDGc6JmNF",
"1AXXPbzympx6T7FVAUD3h1Qh3gKmyBXnKp",
"1BCkpeFQUu5CxFVJjpkhZUefqcZtyt1pGk",
"1AkiZF8usUyhQ21qBvWSgvg99f4iY99Ath",
"1MCPM7tcEUyL1gxmN4TsZ14uHK4xcX4eFi",
"1HrBpKZdY2UbyYecfHmYMZhRoVZa1zoQRT",
"13fzrwyw3aty1QYDAs3ZnAa9MFTSUA87gx",
"17rCWurPK4iPKCina6bKkjoN71zhSQQETV",
"148eMu9jKM7MLPA8yBeAvZiaZ8vtRsK3Mx",
"17gr6zyK5a7ugQHpWzvXRVb1HE6y9fdBHR",
"12tK7wkxcNqXBZjBCpjfgF6TUHpyVRgrRR",
"1evHXXD2FzfFv7ZcKBzmJgraMDkNgiBsR",
"16yWB1xeuEi5sSRq5tGjFBuyErz78voAzN",
"162FgdZKZ6KKL3vkxa7F7mQ1qjQuRnCqLc",
"17qKFc1gTHyEm37nTbxbDAu5FqGt2p7Ud1",
"15nvfqdJtySPwd1AHxbUccescE3XobsZAp",
"1EwSGwhRjvAdRtvmuAGsRknAA26VJbGsnS",
"1HeFdYturcJojnt22UUbtK6B4wWKGCyDi4",
"1EjgjR51mxXSpwhxiiy2MLaLnvm8HJQCXc",
"1PrTo3CfCfWxrxZ8ZLFV9cZ8qQfm9M9NKX",
"1Kx9tEd3Lt9DSbFriWWh6UBY7zcTo1JX5K",
"1B1zrFYdQPhqvNRQ5aeGU7NUjGaGZmEPds",
"14dBnVj9xepZj2eD6dSXEnKy3qmNB5LmwB",
"1BpjfjDejmVTuTUnrVgxfqvsoAB6TkWriS",
"1KD3XGRqzp3nrmT5RgisNh8TUDRtknUBC7",
"1FTBBnYka4w5b7ec3ErPx6mhx7xGRcKSj9",
"15JnHvLiQ65gEUP8REAqj8TeMQ5LVns2Pv",
"1LVS577BfDMmAKU13E95rjVyp73RDcChty",
"1GpFmGEfd3a4YeEy7wbEWypoJHSkePjAXy",
"1JQpDNNt7f3Ccht6yCMJJrZ778rPhxuFcw",
"1NAYxD13FNgtgUUw7F7T6n8RpjPUPy3eDX",
"1Dm4iKH2ARfRwsL3HmZqWVaMtmY6wyNJrb",
"1NBaWBdPVrHLHM3kjR9HdyuMhxojWgybmV",
"18JgFk1NDYohcV4dv5iL8me815hJ8PttpZ",
"1CDUiFdwUGHq9hcjgmrfNEMfhko5nBc4fp",
"19qatqznZQ7XJuDULfiJcaCjD3FdX83oW2",
"1LoDyPevE5T33U7e8kqG8BnGuZypUHzzhW",
"1HC6zeKNcT749yg9VSweBaLN7FZH8wEtSq",
"1GAtd3eVnFo2BArRxVoYKT3pNtqreTDZfJ",
"19io9dz3Bzeb8BkwitP6rm6cvjwZxXuhKS",
"18xGfBQQU8UkymX9juXTmzzwGZuTohwkdv",
"1EgznGrs6khHod1ZpjwK8MA7u5JLnGT25J",
"12rH2GPcod2ZpDuSakPp7tTMD6Y73rcML3",
"1EMAs4VztYPkmsq68NBuxWks9f23tV4ynJ",
"1K9u8wgtPTiD5W7RXcrYi8uvfyw1XjyEs5",
"1N5TPhNBkYJAXmg81FrP3cfSKAB7TUfHGq",
"17pVwFsAHBjEbb9AKqWNejmbgeGNtxMPx7",
"1JUuh7BJxVEuGFYmY2kqcVLFPVUxAJJ1iS",
"1Fuh2dkTomgrvZyD9Zt7BoaFKgAdWnbUFN",
"1EYYaBpeRmneb6BTcej6Kca1pTVp16fnJg",
"1HcQAsJJBkq9uZQL68EH4FryxUV8qUwV26",
"1Lk9EZhZnPjhLTgkSyUAsAfDRcoE9m9nUP",
"17wDgCLMfbzZ1sKjjTwLJiWcsCW5tUp9jx",
"1ocZpUqxMpGG2wgdR8WRE1JcGwbS5jLcp",
"14EiZMdwc6FPxd62yBoU3cLF4FmKsFa1eq",
"1Cv9rHJPMGAC69f4KSzURNWPPHSRh8EQ8k",
"18joNTPN1cjiKHan6jzeQx6ohFCGpvANHu",
"1KnVQAHiGA3PBYNn5AT7vDr4UruBxeGBs4",
"1AUzQhTxSHJZyb8RN2oknhVLBU9ueWNi8m",
"13e4BhCWbsqAUwGUmit9HRDihLwTBMHvPh",
"1FH33HEwU87ntQYmEjAaE88ed6wQspgmNn",
"1PSXjNpSBjcmDb4A1fFd9141MUtKrCj5vF",
"1U31f9pitnqVhaX8sqXZWgFXuXbfRMFt9",
"1Asg7WFVYse6CPxmNxaej1NaWvedac6BL9",
"1CNH9CxTcE2GUTipbxfZh848vmQaXQNqFa",
"1ATHHCr9gSSZSc2ZU8m4iP8ZErRcNtP7nw",
"17NVJrHVje1pxMZBgRMBK5AdTAm5oG1tEu",
"1DEctegeLDsQNFronebfCFoYoq48Kom9YP",
"1AUEm7ZgUd4vcToZi9J9NSNiuiuD4LAKpK",
"1HjeNXJQYScQzTeuZEDiuQga9gCMh1WWxT",
"1KhUAT4EdUeqpvAZLQ5MrrWajtQBAmABnX",
"1KETaqsocFPMxXsGgKF1B3TjCtJtF2u3WV",
"1QDMsJG5Tpt2N2pKYP41FTgEjLkQZTLRva",
"1LwUhfbtEzKh6pd8mRWGjB36QGEwX4qaYw",
"1GVe7qgZLbNLdQZsQ2gxjRru21daippRr",
"1Fr8j1RcpBgyGtdBANGJ4EuzdQiauwBKQ8",
"1HCmBrDgX4UaYx7PKRUXi1uPgCbwxQ2azF",
"16kmssKfD79BxN2dtgWXRXsrPzSAasrGki",
"13veJRJ3MvCvHEMLqaWS82ANfta4GSQ2YE",
"14nR7PVL5rKHZoPtbCKvZFV6MbqnYdF3Kx",
"1BVYZ34JzDaAEvvSJy3oDwvsBz1o9atGvC",
"1JVbp1jfEJp6JUJfWrDKPZrxtQbA2VXyT6",
"1Gu7fcMuMQEAXEiam68TEKrMVxjdrixiVK",
"13GS1eQtootbs4R5usafvn5Nci8azgAW9Q",
"19G9YNuokXoMpUcDx5kxPjz8PYfxGM7UtT",
"1G5FwtKC8pP8VFKLVG87dr47mZQmsotSy3",
"15Cvw7QxADJzu1GhXoAZ8mnGM9Q9eenzFM",
"1N7NWqKcQNbFpCemypp9ai8n1pBP13mpvV",
"1GKdbj5bfPajjHTQ61ab4MKeexHENUqMrV",
"1AYxqmxubYdj1niKSQF3UiAeCThTP17RjY",
"18kRKiZrnhmgBycSyZUr3BwCHnqL3oi5Wg",
"14PfYLpVviCkX3zmyVRqafKGKyapzXAX9k",
"17EzG4EHmgzNKoiBawCnYF14TgY4P1fmhn",
"1G3ngNVtwNXDhjqRGQHCvHq34ECPydA2Cx",
"19ZwdFdcAF26wdGY8x3nqnmHYUnNtSpMZ4",
"14QD4dysJFgqvQWjBHkBav9DRPiuNZmbBo",
"16J2hjFxDcFEVDukk4B6zHPR4PnxnnUSDu",
"1NKJ9egwBdgz6LwNZF6gLuur2xmE2ZbBoP",
"11WZrW1eEev5TAReEWXMRF4mvKiAh5QK1",
"1FtBNzds3HoYbY6Kd4Efj2QoDdsFJGiV5S",
"1PPv7Jp1ps1uUkuxEdWfVpcJYXYBrvixxT",
"1FgJTFxnzoSTxRvB8rp5a5oAiqngW2XMUT",
"1L3gxLxpEUsye8RtV2CEdmYfjT46MV4LZz",
"16Pq8rZ4aFDfM19SvAFi4oycs7UtFpCVD6",
"1GETgTbrpaSsoKT1KJ4vg3ysX97q9yLjpo",
"1FWTx1Zh7a5LVzs5FwVQpjyCsgfWS43yCm",
"1HgqFBPsrhjou1qw9tnaPo3gWqAu6eCiJR",
"14REGKGKqT5pmtBYo8k3mHgXZYsg5rWFi1",
"1ErRKEt8ce9KBoumWAA1D1U7uBHrmrxog2",
"1C5p2GQC3SeRpq8fqQUmh9yfA1fNv7kgFB",
"1EquCZRsZUEqTVBujAmM7UPamUcgEc7evH",
"1J9yE3yxgnWq7ULF17edQaxP37UkzzhYo6",
"1Hw3cimDYFoAzKFzKcBuFry38XRnEMZuab",
"15kWb7nf5GHUzqmfH23rcreeGuHHdjQ4Rx",
"1PUmHSgZP1Ns8YrqKtC33rTUFJdhDnD5gV",
"1J1ydkyEEexha3XBuQ39Cg89JMW1MS4jw9",
"199DJ45Wjhp2Wq2MeGtcevdZaqGmakw9ci",
"17yDxBYZyDb1aaRWrXKDw1PpBq1ADQhaAD",
"191WUMBzmV9uDNdEPqRXERiei2swYcYQT",
"1AaApMnHuw5VpfsDv1UG4Ac36nNGJ7qgKe",
"1AtUr63RohczSKFuc5XkBhjMieaGYY2uWk",
"185nkfZfxRTphjkpfHdLHEwCYxmC2SEadD",
"1EWEeBBUCC4hs6VRuiCeUZmLAyE9NxLVS4",
"1FCCA9VHy6V3p113xEahUhR97pq5uFZ5HN",
"1NZT3W6advHxH4TiYvzbLo4bZbTa8jvy85",
"1KX33B9qBqg9RLTBJjnM5DyeDdFHsyiQmQ",
"19i3b5WTvugb6q13wyXF3b7wKdzb3rLgC3",
"1CK98nkZzS8nwoUyWVfuw6iNjzvuVepJNs",
"1MnyTqSrhXDjVFqhue1Ev2tS9RwQpBmAWs",
"169hw8RJSK9JS93SgFZ2udzS4dtqxFWykX",
"183qQoN5WsJZdm7aqh9bRurBVzg38j2fmc",
"1MCBm5C5DWma4abqFY1uSJLMyDUL1KwZtP",
"1NRet3386skU1nzDUDVRK75cZP1HnkUCAz",
"1KuPHqfCmuaUbjMKGWRZAPRes7fg6TBUZd",
"1Bz4FcGXKvwZVJAtcxgSx5hKByPCuEzXU3",
"17gZsM1FdkreFfRwGAQs64YfeCeqLvpxXL",
"1LGL3fTViS55Yi8J36Tno2D77owZ88MeHx",
"1CfJp52gtBYKYq6Fsg3aawU9ZvZtfdR7bF",
"1F2Ss5LZUJBNGk3ZqYfChvvJ11j2cP1dre",
"12wXckMY7r2tnU6vMeWjp7YVYphMXp5R1s",
"1136sGTviDwFr9qZJUbs741YTMfFBfFKjo",
"1QCrpz9nXg2voXfa5k5au3t9rstDZA6RG3",
"17Dp1bxxByEeX3LBL5L7o9jGyEpwma9oKv",
"1Jadyyuf35thTU2YVRu2mLDv9s92iXnLJi",
"18uFFGGipJcoR4ypcsM5RGmoqg4gKbKfoM",
"1LBDrSGAEKsYq2TEp5zMgDibkuKNFZf31N",
"1Bvh3juFr93Tmd28wDpYudDoHMLZgQ2dmE",
"14YWGXUYgJgH5bqPkzqQhQjFoCeVt6oJBf",
"1FUHcUBEm5SJCY4XCR4AEiYC23C7Ga6kxM",
"1QJtvTAX6L8FDeGSzk6wUM6GBzJYZ4kCBN",
"1NTTrp2E94sfNyiCHabp9FAQjhTHxukNgK",
"18DV5NHWNwJVnvRdRvFhzkMibNPzo2hkmv",
"15aVRertz3xyZTWuVwdgfuGvAbgzKtWDYN",
"1Q3HDAfY7bPhfpyxxjiQcstUdm7ooSWr9t",
"1K9tin3BHmX53Hdm647HY5TMfTrrhkchkj",
"128KtnEzqvEVuq3254uMcJHJDdspx5Cqpg",
"1L2DXeMkKAF4Y4btoCmXfLEHapNfAWV78J",
"1JHRn23YiKn9NMrJiFTQpMxGZS9hVCvMTY",
"1aN7c2noH21F7eYqaLQvz2LKZxwZ1bvso",
"14HZgwWs3xZyab4SYGkgX7gfAP2ETVyKMQ",
"1BAfaWQcU4skGgLBE2WwT7Hva5UGVRCzb3",
"1Frwh1pYSfedD7e9FEhYjCYPPYtG2kSyCi",
"1EoZDfuPzcY5Uz1PFSRYEJaby5U7JBELYt",
"17dhbvXaacfB2JC7zMMGaCwuVho8CLxxa7",
"157b7Yy73TYw8Hk13THjNocS62DqqDpJHw",
"16zkZvG1mPcRHaJmrM919Xi4d1Kta5mGje",
"16bdk6TXsQcomSS7R4sbRSpAaNqd7GS5XP",
"1MwtpqqsEnJbRNVBhrsWP9KuMSZzyjjVwh",
"1CkYVrqwpDCyd1CEmxojnsrtSAZkWoTGS1",
"1CyZCHYVM3d7UUHwnNrbVqwXJv3281oDMu",
"12xwAsbCHUFzrP9dnxpGSwWCBpAfvbZkdq",
"1JsWpjX8XWgQo1kEn93wggN2E7EB7NcKy2",
"1Ax9UxztUP7PppmpjCjg9mEzHH3EPpyBgw",
"17cVZJ6XjyewjZ4JxMr35WLdY6sSgoHCap",
"17os1AboVDXUykzyxTxjw2xMkF4aETYiye",
"1Jt5a9DdKfdNtPs1wNYsDDEi6Tp7H2PyWq",
"1DVfPpEdAwWCdsULqPZWYdr1n3cnJ5xMX6",
"1HvZKWLxM7fH3XYD7v1dnZZ78TojRQ644G",
"1Gg762aWJ1SxHdYnmsxW2hCsVPmFgDa7ny",
"1FPrE6amrz98h3iD2Vu9EXqbNf8mW9bza6",
"1LipfjaDX79pxZbb4wNziNZ7uRc3SUwibB",
"1KRs153VNq2YU3EnvBhRjct4DbDh373u4J",
"1PfWjVQXxKo7KvTGce7zCum2Uaqe22MfuC",
"1D4oAQALo6r7iWfVtpeHcPzsp2Bgy5MRS3",
"15J519CndSkBmQLDnHv9vpSRiPqJ2BHtKT",
"1PmvMyLeZuKTJ7rjuS3Lz3Zzd8M7zvoeiZ",
"1NdbKJZ9K3AyaGrBn3L5kz4XwBbAKVA616",
"1DKdJjaTpiaWA3MxXppbXSjBrPVEsZMg3c",
"1H6pU2fsW3MzNdoU9N7n7xqQcDfK5bn4Xf",
"1KcQWdBu3bw3PoTdibAbupgZA6gTrsQevM",
"1HH2n5jKS7E9s88gn1xPtE3MtCWfB9Dfdj",
"1G46P7HxHqrEEP7FDbjYFj62mrJKmMjntX",
"12Lt1FSYqZgPgCmLJSWG4EBvDnJFciNnHT",
"14FGFgMjPrA7CSSyZMyC3Tk3DqEFbYyfCT",
"18SQr6LWYgf2WVmg5TdJftgyKhNUPwkSvX",
"1J5d9HkiKbyygSoXqayhhPSXuaHmW63ygx",
"1Ak61DkrakX6h1P9QwoNbmfpm2L1ppkTJ",
"1B6PwTbaSgrURmR3DqWsjHKK8fKWNrBLmr",
"1BHqyMDvYM6BxgB7Ei1bYUWauBufuHRH8A",
"1NFBAbuYSPnH6dFhDDnC9qXUSdCCLvpugc",
"1BQYqFMTcQdr2G9H5kMRojGEnDsbZnwKEz",
"1KGotQfEVV1BRi4YRHiwVQT2XwBWR9yACt",
"153AhLTbT574iaU1Yv31ep2xBkbMcQzy34",
"1B8g8zZKq2W3QYALBc5sBJkJ8sUCoPKXyJ",
"12Z4DQounid7ZfJrGu5tjQgrVbo7BEdNGQ",
"1NbUrF8dvsGb6vAC2dqCVh5rzW2332DNY7",
"14ksVqTo3Kv5uLn8GHen9yxteint69wHVj",
"17HxRAdntadUsUXhgWVPkEfNL4Fqjya5KU",
"1HQtUtDkbJyiWMguePYxFn14mSF4pD3TWw",
"1P7ciEvxayEb9hdA7RN51Yv5BbFj4uyTqU",
"1NBiXweUxvHHLW4FUCBCmCknzAxP7kfeFm",
"1NwBhPkUeZdos4tQZLXnZUsBag1E9ziJKj",
"1B37FVNCVpH7BfVSFuPsmXHd5nix3L5fJy",
"1NHU2Ac7MDF6jaLXzRd325hWCctJyWFSGh",
"1NgpMqKxqqEF8AXHpzDVEmgAgmgcrZ1F7D",
"1RtWMD6qLEkhe3QZEzEZKndY8wqdR8URQ",
"1JjKpoPLcaQXkHRQsvd7F3jRzhTPd8gDFq",
"1Lioo6DJoMLep2iGTbgPMQhAsu7KfwPhEw",
"1BuNk3Dm4DPLvMWDCPhUk7USEiz7V7qaQb",
"18TeC4HuC7wkJDNHt9eqxnfPcosCMSWNVW",
"1CYEN9gExYqCqknFLuV28oaF4V5AShvcwN",
"1JXU7zi1A4xdRqGCDHmwjxqrtm26vYb3Hh",
"1ZbWkHtDuBikf9fh2AM5XMbG7496iHhwc",
"14Gtw6e4iVjoo1J6FqaVwSLCxe3SkvywTA",
"1PdNdNokWqEUjThjPjaSrZzC8W1YJSMRWW",
"1PcboTCjq4hBLUGpx4agZEUBjCZfDdfQhK",
"1E1mXXrc4NTHZ1ppKowJy11pZyyZPBsQHi",
"1FVTXR6Wi6Qmn57fFWJ8sNTTaWQriJZMCM",
"17uuRoXrqGV7doCHPwEno8F8gCMPqJnFug",
"1CW1oXSaKN7woVdQCKv7SAxjLTh2MwkkLq",
"1N7iS41y4Q4Q37NTD7uCvdXc3PcBn5rsKm",
"18XonjBudKfbLSEH8bdJd4TQ5JgwhapWH5",
"16S39h9qgKPXFWBumPNPW8PLBsPLW5T2EC",
"1Bx9sL5aioHNncZeMn1yUTLzAbmxnbwkZ6",
"1KRXmquW7f26RfrwGPr6dAytvLNNeBvuN5",
"1Ph34UnJz3QRxWHcFYUEMWJssiegb8tDzh",
"17VXqmNA7Rfo6opAcTA5vA9ikdsckS7Wvo",
"16h9QmTffHTfupCFjJ29fgWdZhjsbxTeSS",
"12XJA8DMEWGyP6xh8r2r1JGohxFbEfi6kK",
"1J6Nv1E26pEu9qJn8o52jWtmPoGhDmNdXE",
"15KPsCmQ1eYr8G8XZJAbV2gEQzkw83oa4Z",
"1BByVdJB2xFrZmAEysjgx2qUPbR2ydPQz3",
"1Jf5YxAkrpz2zmyGEZHDuaSiNdtTkNsLTL",
"1MAb7BwogJLPFEUSCkjVfky7tvy5wYZJ3n",
"12dSEPJtm7dweREtduTXu9ULreMJV2uMMB",
"13c8sSqxBNpeom2u2G2kxkce6qST2gCHWU",
"15AKmo9QuvRrKvpVk9d8YnFP1gcgGKdaWo",
"1473oAK19yZVue4LrqwV45GULbK27t25ur",
"1LTu4kaNaW7X7C4TARyw1XqELJ7m8EJBsK",
"12q6jGb3cn2j6XbmUPtZ5SegshCKvbWYmn",
"17LMjch3pPbFgKesewEHEzGvZhufgztAsm",
"1LygmMtCNvxnDqcGbwM6W2scRXh6XgWxTG",
"1FDgNzYD89hqLJ3zdmAHCa8UsV6Ut46xJ6",
"1NqNLJCYBgoaN9MUkQV1qh56FqwC6PYjY4",
"1Pw899KwC4GVqCyWnQxFFMKbxF1sVxNpir",
"1C6woGFciFggqbSo5VnGju94jLYregr5aw",
"19TYKt6jiiHgrtzg5WbKHrUay6qDB776g3",
"1HMHcuG3Rues9YXQEtZLcjXQjjT2Axh9pZ",
"1MNt7ti1gSr1x6dJKFtipkvz3527ZYEG4E",
"1PykdQp1rJgsG2aCtRaZc4vUiRndEkp31r",
"1NteFvhuZuMXfKnBacEkfiwUr7oru7EEAH",
"14g3j6RAYKreYKSmrZ2MNkyojPkkXRnEwD",
"1BrXbp53anDPihA9Dy3msxvi9aU8y2orZT",
"1EHixgaow3gaiQy8UCReVT3JoUruxu9P1g",
"19UUt2jP8pSQ7AAKTkUmgnRaCcrDS8Rryo",
"1QBbcgHfQ6XaRrTTHSwSfAhjcX6zrVwGNG",
"1Ld5ddgZJBCxvsSLg3GDhtJSr4e2zZM4HW",
"1K4FGod4UWdQPusiRdPYjkxYRyjR5QNQTF",
"1HSjZitM8afrL4ksykUcKREkHpEstBcXWm",
"1EfYnYC6Y5htDv3e85dBD5VeXufbJKu4KW",
"14m6sDmyfAwvNTRQgM2d5apaZfXSqpAonE",
"16XpQpHgZCAASgK6TJFSG5oLSC6xfS6ykW",
"12jwCY9pSAiL3R89aDcLFdiXD3LRmYfk4B",
"1MdXUgWTq8aTSPG7mQ3J56K63BDBQQ9XS4",
"1GtPAYTZeLdcCpEbiG98RKX1HYkehJMutr",
"122oawSVarAm27pk38q4EGfmzama6ekHAd",
"1CVrN1ApGf8apkguM3vBVYMW8Z8ppqQRL3",
"16YLqFJiF8y89VpB4VnctAVaJKPYh1ArR6",
"1AJg5daQyaiuM15B1Hp5ecd4MUcQmJjkLB",
"1BhwgvJn3n7PVxWQ2bvwuLk3w3NrUPfTTc",
"1GBVLMS8pzbG3N8KdNWACdQijdKTviTpVe",
"1LV8jH8Tots7N5WpprVNVKSmAj8Roq2aFL",
"1MaZUg31U5HYFFNW4okw2jQgRnjb3d8oFF",
"1MzQuyTaj9kXrxq236xbJEPPbWYrRkd8Kf",
"12fHyFmieFw4dYbhAS2aTJfaESKNQUEpqL",
"168HZba6LsjUbtVB1iU5ZLCdVuezMWrfVJ",
"1Ey3dyocxEAhtK26SRamtLoveEAbTzJDLp",
"1KeCXUeBp6cgWx9FMw4Jr2t2dLVW2PmVdj",
"1LHuYBzmyYv4gJNDBamFAB4PvrBoiYqozQ",
"14bnezrEmatfJuvwwPe2fecbG9nRWBaLht",
"1M7AMVydT84dCsUASDaX9Coeo1H4EhgigS",
"17fhvkHWmCtMeG3WyPHa4yuzqN4hqkKVSo",
"19HrN6CWDZQ4cBd5CHwdHh4EanQyu97ugF",
"13u7fbH9FcLBkYq5ii9UtpLnBd28A2Kowu",
"1JBPmd8uq2sJcUMLEE6Mmcck6hvQcoNxyH",
"1NgYZYavaCShscbTX9sh3NF7ZALvKBs2AS",
"1LGhfgKugicm6E222xz3aFwf7cSNWLRagV",
"1HhBahfZ2eSJv9cDd1143rQ9Seg2itGFXP",
"1AxV5U54JDiuCqbddrAnWwNAruLYUv1rL1",
"1BPJqCp7ABzzkjTZUFWwx3YUoznFTrK1dX",
"1EdJ2yLm1jC7cMcnuWw9ZYYwPb4Mk1dmcr",
"1Gk9XdtbsswP5XYay1AYy93m1gL1kseXiB",
"1Gzd3EkvaKHahqBnqff31RJFwZCkDw128M",
"17U4vfkr33tR1KYJwDKBuoDjBzSNqnmsrM",
"18qSzcoEVFSV1tp7tGyBbyg8QnogFMqeDc",
"1DZ2ruCm7epxcjqawbpusYWdXp4eyFTXhV",
"1FuSpmb1UHXTKoKnLTxm4ardtPxSA7T9Qg",
"18g2PxDwjnuf5aFwSENmXJMxe1x3H26Dkb",
"1LJ9YcwspRE4Jq9rMbe3rqeaP1iVP4KGMi",
"1GPTjrPf5rj59rkA8mrjz9SRMBzvA5LXjc",
"1DL3RjCdMLy2gLnjefo33FZucHsV9kYTD7",
"1N43icB6vmP5TyPoyrWXrYAWB4x5WcNNBz",
"1FohMmpc13XNAmqsxzHf2AXLebVBBbJ6F8",
"18vGp35PbCiiN7szttqX2kav4f5k7ncJyF",
"1NGhXCz38ytjZDAXQ6nccVWkrxKJcXvYio",
"1Byn3szAJAToALtgnjXtKYhh57i9WDwM3D",
"1ASm7hynW7bjZmRtAGC3wV8wsHjcGTbjKu",
"1EKib4MwWw5HJiXLaxtfEhJEv9B5ucoGaH",
"12Kk5f9JSTgZ8jitEroSE9UstYq5wmCcJQ",
"1JWjAJkvfjRqiyjK3PLJHhaYE6gkZuiQ1n",
"14SMdw97GL9Y1dFJver5QKezwZbz1512kC",
"1BjaDNtUpYVzuSvy1b91zjA8fbskgSntKZ",
"153nnFShLcpx1G7LobXuUdryBRLZj2hJyn",
"1J2wi4dqY5QQSnB9nT1Sv9z8bEq9NTqZgF",
"16WuGtuq1GfM5XPqCxFA2Y5YaecH6VKxAQ",
"1MXAXqTXvvQnxRs2uwjsEvpyXYVUQkpsQN",
"14g3XL3FWis1GxYGDhFLTAh5CAa7m43C4n",
"15dwK2nHwNJsnwtcBg67KjtvgYPrG66Faw",
"1L4V9Qe1rV4RGnGs5NBED4k1xzaK2t6TSn",
"1MN9NKrcr5aVhnEdBQFnzdcewXzFBJkqWC",
"1w6CD3pMoVGufoETnFm5mXuGZKjqwdLVF",
"12AiHfkUpPEP48DFNeZxQo3UG6JcqvQ4qr",
"13Fz7u5Gfkh6opf8U9nG1AjAYhnFMNf5rS",
"1BcLRTyCW2JWGRHtvjGyBWdNpB7umiHnsu",
"1AohNqQsLCKCqRMeJGFM96a7agfnKjGarZ",
"1E5TKzxv3bV8NMvQZ3CdSHry2kYjCyGree",
"13dGwkwgqFYmmPB3P2VPQ628LVmZyxfxkf",
"1ApwFpBn8f9n26i3mbwKTCE4LLkCdMEX5A",
"1PYbRXPyt3HawYT7m411Ap2NnuVMPteWsX",
"1KfpMEwmdGNBmv9zuGGDHidpPvZaKy1TjJ",
"16ajVJ7QFZ88V9bYQb76U1muLSfjJWA78B",
"113UVx7Gp8DgzQkNDs5fYBjHzfLv7FxJk",
"14J4YdCTccTYL2o6bzu53NUp2GnLH4LXY8",
"1ZMDUCkEny1RizmYtBHbW5wgv9D4goQQa",
"1CfYXM42NUASYWfekewFcUmJj3aZN7dbgb",
"1FvmtuzC5WJNi1rezToNYTfGxFj7YQVCp2",
"14GHTbQe3RXQrresTNJFtiGfUmSLP8zsUD",
"14PgBYkDTtpqXHbyMpcBirq8a6HrCmcLiN",
"18w3Y3UbvvX2BaAzw6W4ysJbaDw7jPHFZ7",
"1H2oNAcFxSkc7wu2pB2Q58gT6MGa1F55Jx",
"1AiCZpioWvqq4k5GGWZ8L9Gu3e4bdY29kp",
"1CRRD4YkNbsKJ3tPKStaKNEU4qFxacMii4",
"1K9FaM389J8eSQjQBdh9odiWhvuhdCee89",
"1AEfnstjnE4BVDgEsQ7FjNtJcc6GMg9BU3",
"1K4ehMpvpcNLT28oJR5vXgmN2kohixZU1K",
"1L3ytAeyVJEmgCLgbaQHVrwmFUsEFfHKUB",
"1ArsMRR7QcJT9MBZWuSyziYgaRtEmSEHj4",
"16A64oQAM3vtXRfZ7J542C9APq28DU1LEn",
"1kT7sFFjcYYce2rbj711Yy8VR6igExowM",
"1Jc65fyahjswXVvsJvzvXQvNnERoGhX5px",
"18sNNLALHsjK3ofcYxwpBoPgEN27Yd7mAS",
"1AruTtFSkTkjSd8LLcxGMux8BrmM4ugDKP",
"1M7MgqiXQfq2oEC4aSmg9ZdNVyhcQ3VASn",
"198qKcmCn4EwaSg7oLLJfA4w43JZDdTX4J",
"19uPgghyphBCw5HiJwUwJw5VTmioECZpx6",
"1PpTufQDi4weh5VgcD7DXYrQXxjsLhK1yC",
"1GqXQAREsHuLZqH1ee6zeGshZDk14chQZ9",
"1LQgKmNxMYonYptuyESX1ZH4MCcatf3zVW",
"1MkM2hRayzV5fq49LaK1J424CqawxvzAia",
"1KA5ZawSRajHYBH6tcYk6tv3Wdi7UZwyPn",
"1LpxXpiKw4b2P8dZcua8XHccoGYaY2NDGw",
"15NLJLMLo9HCNQNfk61dxJnaaGXSR9g12L",
"1Pb2yop69oLaEUGGg2eHeP3zPT11V3QaNr",
"12Yog5Yw5kLV4KW46Nz3g5pWAVpy4QqzZX",
"19pQBpUuqFoKbV5B8kKLLFdVyqbPvPN8F5",
"13Mm9pnXPctcxe3aceLzQUkqbxpSb8KGCg",
"1JJsx6tFns1V9XgDdeyNW3ocXvRRReqKCz",
"1PVM7VERe3e5j9PLHAgytDqFZxadhetbqz",
"1QKdyZ3caXFpwHvpcS58JZ8pRmeFPLcvBf",
"1Prrv45m9ZbPWGG6zX3adJScbLNkzPb7GR",
"1GMpNGVxfDwyMVGrCnooqEi2wt2eyYZPn6",
"14zbfMgU8PWebRyXhkCUVG2P2JC65juf8j",
"1Hfkw8ZEdGXiiUun6ZCEhfGUYMoyMYY2ho",
"1GfkR7vto5SmsL6VUN7BYDQZECtzoEqyuA",
"1KLJBtmHG6YPP2py3w9UttssGaL6Lp1F4q",
"1Eiu4gCQazSgW8mEriwgPN99LJScRSH4Ek",
"13C1eEUuyQPGDYQBVrse7KRdMnAnquosBG",
"12bmrjpBFENvHwCokip7kF5mTXzpNGwdAu",
"1NCME3gx3aHRYNP8F5uG9qKLsNZYjXvV2f",
"1EDpABDoDv9CdnGjbY7FYngKqqyAAoKVsf",
"15m1v4oeHWzSq2xvVJAZWnwCyJzZhfnH6N",
"1D9jrGhStpWDQYAoMES3tB1GGFALHwHkt7",
"1JxaXTb3vra6HFWCmWe4BJLpRyUXf2B9K3",
"1M5k3mcX61E3wijTQ5tiDNpS6U4MDMffFD",
"1EFYBzQoyQjrPd2mMqj2AEcdoKn2mQMhXU",
"1GGVkhAnPvouFQgUnKSPpv7ercfaZDMQqK",
"1C8RaMjzyfkBKgFtmoSMCruQ84mYHkmNUB",
"1MYaf9JSFQNEdQHPE3XLwSqF46XVoXQP23",
"1Eg2q9SybGL4tmEEcBPwEENb8sDZLbrNzd",
"1H1SNwgscQu7QH6taDwAteoFdJNWhCzijc",
"1JVYFNLCp6XfZ6RsJbic7wmEu1LvnMwWyW",
"1Bbj4GyEfCbpUd5bbdV7G48zG6ka8AmSCj",
"13nUcM5P3nTfG8Xs2PbT6tMvSJWSnnnabk",
"1CntZUjrPpUH7xq6TPHeLSNxQscXpirZ3e",
"1FvC3U6vocfmHYo5A8YbF8X68deDAKTzSg",
"19JsN3oMCUKrKXdkgp5LpUodojPs77u95o",
"1NuCA7JaC9Kq3XD5JmuLa3nBzCoXp3ZtJy",
"1Ggyev46FRrNfp9bFCt27X2XDc9kd2FocA",
"1LeM2cdXZcQFiGZfqNHDsTFaKjGENe5Rzd",
"1PZgj3Pg2cdc22tfTASComE4aTQ6wc7Zyb",
"14Kf8UddvWwMcSeuwVfEVWXTf2THRhQ4Jd",
"18PVDX7aYkos2kVDMn6YsKEKPg14rYrB7k",
"1FpRNhFQnfnbh5NJGp7SHqL4vQvGDGAQpw",
"19JUjDTJXA4kQKYSYMsQnZ6R3wbj8rwcDP",
"15iAZUEp1VMfaAM1EHdhndxJT2Y5zAU6s4",
"164BvNMZtaJdj7AH7QtGfpybGN62aihtyB",
"12tH7ZEQh5YQG6YZgn6NYMq7Qa4byeaNQG",
"1EoySR6tkiDPjxgwnDN7fCwth5ujUsJVYw",
"1EHGHEDFqfHqsdTxcejtBDc68cuJwkXUho",
"1CxyEeFWu21CW4rgxEc3zWtueWx64pYfbt",
"1LZ6fJmbgTAuNEUCdbJi3tDy7F2JMuCFjK",
"17F2T6sL4uAtCKkWFXLvh1tWUpYv6XetHB",
"1NgfBPhaGAJitwn8ahxi6X2kFTrBrwEdjZ",
"1AEoZvmZYiESRPAz2ZoKtpw5ydqRrSQxYU",
"1HG1KkCV4uvTesgwBmZoLX3N4DPuzDqmy9",
"1CkyU4QgaG42M4y5aZJa55iecWEx3r6pQL",
"1DPk3VbcZgWWsDUGGMg7SZSnjP2j9Fds5b",
"1BexXpz1uwaBQR6PdaamwphNeDp15AphsS",
"1NLa4vtD8RnNgeV9r3LZ2q17YtjXL1Rx6E",
"1A39Fq41dpgEz6Ceap1cv1G5LH4LnLBxuf",
"1FyY6JxsyAVBZKVjaWMGms6i2JAcxi5bUv",
"1FePYanS5wwAjTAg1ndZzP8ZPs4HRNd5zC",
"12CU8kzAAaBLt4fh98iAm4puGhjmGA6FF2",
"15U77S7F25q8qgod9ED5ettBsUgYChcU2q",
"1HoqnEufpFxGD8Kn7GYh122Z9Y478AVpCU",
"19YmLy3NHmSLbJAxB4ubNmMaDm6fjYWJ8h",
"1B2Gfq5zDz9G9cg5svvGbZckkeVDR7qHsv",
"17jimJzP3JqLucPAydBjyvHP9ZoRxhSBdM",
"19N4RtbjLKUT2mWqS5XdhtazSfBvRYsbxA",
"1C7bSsWuGXsGH4xFN1AYgE1fg76RXcCAUb",
"15KXyK29vwa9Ahipkx2FMRG1yLWiPF4GMd",
"1DCTCeefhq13M58gqxmPhsac8GvEauwbCU",
"1LngMDq8znziXXdA9MVxShMii95wezmgqj",
"15wCgryY76mxnZMwQ4gyNcVQkGJi2NCUo3",
"1AG86f73aUzatnCibHhXVvHRSVT34Sj9YM",
"19gWN3QqPj5BwZYGDVu7nT6uNAAgNjevSK",
"1Ne2NsNXMZPAdtsgcE95qQGYJZU1mDbf1S",
"1C7pwDVpkdfCDsFXfv2KW67fqsVUZ3bXi6",
"1HeiqfnstZexnXAQw2S95dREyK765b45Qd",
"1GPJnaRFB2EG8DUfFuK3K9D1xMNgSsAnnL",
"1GqGhfG182pjKRFiXEaikgD8cBarrkjb2Z",
"1KTC9yx7j4Tf6tfr2wGQKYmFiFqEy2Q6c9",
"18M7UJDK4x9skLFYnFjgrNsJyhsayCbuh8",
"13csEedsC81K6EwjZaNWYvX9JJK5Jgoey9",
"1Cs5AULA6pgAjxWe6vdeyou7r5xDnrc6FY",
"19Fh36exeDHHzButwJ4jHEbeQpCBeMrQ11",
"1G976x6Zd3dtQ5boKYLLEQ12oc9neaj7CK",
"15VqrzRxp4JU136Vib2XiYtvi6vLZKNdVa",
"1NZimU6tkLmTS86hGeunM4t23XSHKsTvXm",
"152KPBSiiVpaGxN1cLpXDCiazVfvMNufSC",
"16adsZLdy1vQAYiECRWci5T1qz5rKUqSNK",
"19KFrSa1p3osmdYVEHMPebLNjRjsDyTyiA",
"1ES5Vxd8raetWjJ27htLfcPxiUCRz7jraF",
"1BSphYkhK17zDTV4oQBCBPoxC4jDQUzLVW",
"12ft9hZZTNvKLB5TE2UFG8FasGLwdETNAr",
"1Jr2n91YbGyGggNjcUTVJRQjPxfMXs71EA",
"1Dq22enazW9mG2u6PL77LXrMNq7BHKsEBw",
"13GkmBJV2NhH4d6oBgE7yw9gZRkDspgQd9",
"1HZ2ALBJqeTdFpZmNh3p96YXA1xn2j3cez",
"1ERR9b3faRrGzpx7tGLdJ8rnBvPMN6Qcks",
"1GMk8rbC3adaWPxmEbmMaGueSS1a6YCUfh",
"1EDhKgKme3vQqumyCMXeH3iGuy3ukyVAbQ",
"15aTvnFvxsnkbcKt64tgBVrELiMPz14B9H",
"1GqBtanjuqxAd9cha6tTk9sEV2pr7fNhZH",
"1LkZY6DdZyFJmVBxN4zF2ktRsujrEeCiL2",
"17aJx7MA2X9k9JJEsaVQwacCy9xC1oTSET",
"1AWhiEYX4PDqJ1cZtrSRphAdTfD5FNbBii",
"18muZG4dXJRPqdHnwLnbsX9UBQ3TQAUqtj",
"14W3Efrg9K8sw3zi8k46DEjXwXuPou8pe6",
"1AjmBmzvdbiMAS25832S68AxzHPU9hh276",
"18fcxERYLvM7z9dxCKUgavjRvLCACtqJkZ",
"12reNeLXSmgmg2gJ2VsqJpRiLSCxjPFXcr",
"17xgASmJDH8KSmS8EWEPnhBi9hC8v9DxPK",
"1NiArKgKkbXxiDNieWHw7JR3sNE4osuL8D",
"1K6atXvzJ8jXZDa6FmPiR71LM2sn9JLoRR",
"14n2jXhBm6b3uKAMFtDdFdkJqPM7axKX35",
"18cVXxncfTwzQ9iH3cyetj2AgRZL7QvS6c",
"1CcXB6uEXWowTCzR1SbiPorFB6CggK7vbw",
"1PPssKjNoJeEV6mUh3zeGomSJCwfZwLEzT",
"13KLdN55vCsePj74MdXZX3322JNA1RQasM",
"17q4iVXFYaab6EWXisQo7EHBJduPQrhNoU",
"126N3Pyutq59SENUHQmrHMcwaBGq2FSrtc",
"1E5bPAs3zRAe8NVLNYyJx5xAAg1c9cBtg9",
"1EJx28TPUqNyxAXWLc4qirjs8bgnAF81y9",
"1PboFC5Y7f4nwXJ3QE3wtzHXp9KxoG8MSB",
"1QJ9hmFJYucSBH66xXsgafk7NmCyitkSWL",
"1PkjiCooBgM35H4zHUPJURnQiMrHJtQpbh",
"1Luv3gpWnLdSS8dXf5TScQLYsntcpN2PXG",
"136XTuAoFXof2PZbqgevEBWzAbtbccpgRf",
"1FrvjZiCsVtYZHoSmxTaWQRoTmRwX8LTyx",
"13GS1mGo8ZAx44CfHRXdR76RJg9xyRaMXE",
"15TJ59rDNgvc6k2DKA1UJGZQrm3ymfkhZR",
"1AL3iFdedbu4EDiDY2TvvVVXPExuPmhkR4",
"1Bd1MZfpcZpRMJ6r9VEzQRW1SZnPWuWisa",
"1LmLu4GouhXJ2kVGejsqrwPNkAZrhbAGBG",
"1DXoCtLWnLRuzk6XAB1AdRYN19DDeaMxAn",
"1MYoCffJ2nSAahjtff5W3YdutwroJreTQq",
"12wAF8HtqxKYQyqY6WZtnNE44briJXCe92",
"1pLh4BbLonSGMLX8i7r5sSzmZWza16iNX",
"1Ns2ri2B5n2kCU6gNyo9MnDqg4wYoHVLUh",
"1G9xML3njQ8ufoXTJPvXh2pZbeirPdtYZC",
"1JjYHLPL6oW3RQmGMtYm4rsgyadibKrJG3",
"1NXG6oewwvMFCp3NdTiPYcr8qy9dq6YgrD",
"1BZ5G2HFJqXmMoDgaL1FcmQpH7ACBrv3na",
"1GdXws4opSEw9F7avroWH7u6JuEUKDHVcH",
"1Ljfi4xngrmnHUKSj9swr7jjAsnRB86a1y",
"1NxhRS4pbFPc3US29Rv1r4Zoay6SyEAP5s",
"19DLtME4wfZusZnSBPg9DgH4zSFvPpT4DU",
"18nrpGRWATAVmuwQxMr8ktgtW46jmXkK4k",
"14t8JzPSQmXEhuHSouqhJRdWuQeYTjSJUC",
"1LttkPbH2DMEEFJQHXkVesKMSUmwHyzfra",
"1KGTvyB8j7MVzFMfZi9AkgxrpraAQZuLjP",
"17TaAjvmDEe3pJAUWmG8mSiyMM6Gf2Aj8k",
"14WhYcvqZMRyynP46imNLYAMHPg6PeWovB",
"1P6f7tK1Ub7TcwK718iHHqxvZEq6FE9vq4",
"1FPqEUckQRAqMAHui7hZdtPJZjDXeHRt9u",
"1Msto1HNMukVUAqTdQHv3qTJ3eaBtEzhAs",
"1HNtUGF3aZDHFfYJ8JxxSVWi4amBvNg9qA",
"1Bva6co73VLGVKfTEJYCHJRujeCtmc8A6x",
"1NBNtjWdvsrKzr7r96HsNkqdjrFn5wFUHi",
"1C6EaDUpMA92eJ7c5urpm3np3RVjrfkmMp",
"1JPoboVjhDdsexgiUdUKU41cbCkH22zs5r",
"172h4QnnFgnSGgyph3pd8RDYKggWj9ta54",
"1NVtZbZTo4BDeQwBirz2iiZUB6XaM4if2Q",
"1A1XJzFZNiC75GWo196TcFi2zJnPUc3kGD",
"1Jk8ebgxKE2nTgemUvU93A6FU74fNwmFL2",
"17NG5yE6Q1Pkq9eWDJCFGgnyXPZKse5eKd",
"1SRHhA2vPMnug3i8XCMNNL9FSr1JJHGyw",
"1ARwnN3L1tWY98FV1FBiqGeXqXr7kbW6Hh",
"1ChaYZFNb456JpgRxfj9VXExzrMYb4GWvV",
"1NCKfsgizDrev3jEH5wSwxz5HywLShBRby",
"1PKpoZ4Xu7Jv3ssoZJHH6hwfceYq22vnEk",
"1HwM3TwGiyR87mdWBvwHuEm9AC5CUCubyW",
"1ATfFCQDQ998s18WoTC4SkE7Mh3s2B8qcd",
"1AqhTCQjYw6rG1DFKQffNnjqav7E4iGTwr",
"1DyijHPBMpgPHYTnDLcs1HqXQjEDJwF1pr",
"192dQTTNfsSdVQLWR4Co95CLDMSfMYhq3z",
"1DBVZdh4HepHnLrBYjyB51HPnaSAGvMhZF",
"14uY1H5Miyoaey273peMEdwrd5wF325tpf",
"1KkXYcYMKRkNBkCfDfNsiiuYm6aiPjiHVV",
"1NKvv3Ed2NGT1gDTSmF5x7iyeDL6qkBQPn",
"1HgF5KYRuv5QkMPvAXq5LJQKkuVXNcNQ1o",
"16jyCsgYfHnaX7ystatj6jaXraEApwtrWe",
"1H9vrmnLNM3iyqeBwXA8SfLyDikPHKgXVc",
"1RvXvHrXLXhGwYjG7oPjsWSKn6wMZY9TB",
"16C3Z6jzH6TwaRjNjbDM5w5WtPGUgmDP3V",
"1EmLgQqNHuCFCeiP9xw22DdpEsMF5gr1pB",
"1HAgpvKJYKppaqsnnJiDymTx6WVFP9XoVg",
"1LK7LUwUq3jtyAUrfdSNjBZWqwdMCQQnAX",
"19zgf8B2CmriN5ru9nuGoR3rgjbAUCLdES",
"18FY7qpDK6vsXtMfJSbEFVwHGQkURavAp4",
"13jY7Bh5Jkxkd5fW34AagsG4Hn7HbbVMuD",
"18NKYDvJCqYAi1hRbVgU8C88AYmRde2BCQ",
"1423XVFCxZiuskPo5hu26KHX1NQKUaExLV",
"1td7AVFFBmiqoUy7ucv7P4TijMUQ9q9VQ",
"17svVjxMd2p3zzecUY4KtqyaCp2Gh2qXoA",
"1GbfFbb82YpqZLW3UN3pDtuc4Qbz525xhY",
"1NmqwcBNTyBWYwasUJj7svugwmtxafTrJj",
"1LNqk8ZwCwh5b3phtbH5cyaQoy9DUJoCzp",
"1CrW7MB4iwt1HiCNRRPdNNdG9B4ziUkYCr",
"1MsgMByCsjZ4dBWhaMD1FuAodymWrVA5Bm",
"16pa9karbEzD8CveUzhQVy5KcZhQH78trp",
"1Me6jFvMfqYBtNf4bHQmJSDo6wVDPRusVV",
"1GjrCUHxeiMZeHWi1T7e5zAoaGbTdrJXn4",
"18HqQ3n9bG7X6qXGz3yd2DRR6hyE375JvV",
"16C8kZdGJw5wApiCf3u2F5KnSnTdM3koEi",
"13B2S97DbsMCdcnpbkYtqZCKKvdiedbdiv",
"15yBom9D6TF9tzwj7L2bPV4bamg6MDByFc",
"1MXsspwUMYUiWHSjMf7mHxSqjDSXMwBpLD",
"1KRuKGydGsUEt6xqn8jZMX6VzT5ZDSgKRt",
"1CPE8ZREt9NwCfUJx1EW4BaLtqi6kzbXNN",
"161cuRrbwTz8MfWgCX8xWspJgAG1BmdZiZ",
"1GrCasCDwFwAe7UUhsbEN493SqWDRyHLVf",
"141NV4e15egUXrhA4A74uSTcaskYXy1bt6",
"144KNA9hVKq3Mq8c8pHhkEc4TWxvs4Vwm2",
"15UrPhzppodtDVpX6pskMQqvPhCRAjYmsw",
"1KiGUKWfVPBMd7oKHvoumBP4ThyhsJa1Ns",
"1HZuVLvAa61Gh3xH23q8X5gz8J5zQXtAjb",
"15R3hinknKoSktUscfPqPZdBh55XXyAcc3",
"16pv6Ev5QtNVqFyKVLUDZs7SvD5Fdb9sQg",
"1EgB1TUUogzB9Q4XNeBSYHxjnJVWqbtZnn",
"19GRKuRkc17mBKCzcCN4ARAnQhjSxTfBdB",
"1QBDnXyQC8J7RCwshGPPCV22wJTobS1pkR",
"1MGCATEKfsUFpeiebhVCWa6N4XzuQc9KCf",
"1LxyTUF7uyMiL1YcmKMc58tr7Z9PL7NUY2",
"14cFwVbXdjVmBWXx67AkG3Y5xDPxTrs6ik",
"1G6GXzAgEq4tsRoqhLDn9PYJ2kSe1mnLx4",
"1LETMS2swQJ2V3XEGCGiZm1NyL5UEft7Vi",
"1HnbVxBSj43tdZSXkW8ZY9AV3P3UPVxdLY",
"1MKDFSj7fnvsb4pvfuJCRtrhuD4VVD36gC",
"1AP1wyLTNvzwdY5YrexUZAKhoBGDXagt5c",
"1M6PtSnQT8r1apoJM6AAWaVK3ms2HRD9LX",
"1CgQXj7Unkr7J2LdNujzbqWUZeMf5NBMrH",
"152Ex72v3FtyQzcHY2hcQSUN6sWBZ8VxKm",
"1QLTjqurhBuhor6hwsYnjqWg6bqRYPjsFP",
"1AVJ5cy6UvcFft7wqkvbjf7smzbykSYyUT",
"12fQdq35yf1pF5uAtQBGL1NAvj8pYyi5aj",
"1PPVGQ2BsLtLvwdr1B71ZKa3XZrnT2CEqP",
"1LLW2VdgiNyLb1gHzAgPz4jWTKeK9eSH2s",
"19fF6BT3dsQ83pCQt3ZnTQBuWHup2HRJGG",
"1BzQDuTvT6MVck2pocXUid8yZuwF198sqU",
"1P2pyk9DNfch6v11yFLmpLiQiCG2Xj2yz9",
"14FveLmLKUEQo1hzyMPNwn13Pj9Mu6dhMf",
"1BiTAWhjRsGjK1iowcWjucGeEghTU2NQ2B",
"16Ks8HQXL2qvGWk6ZUfAaH6EMcgw4n74rm",
"1H8uDPF4p5LR4P5GdErYtY7fuBWR9wny57",
"1GAEbPscrRSNvnKM2ML2C4pgmRCHSxy3nM",
"19F8qeFAZZwCnysRkWHqdZ3ucj6gXsFqEH",
"1Q7RxczUFrAAtHsu6qWFP5cTdnqVodNqmh",
"1EFENo5EZ4kfrV4QyDtCUTQQ38cGXLUcE8",
"1F8EaoE2gCW64ggy44uKUGh3wmoW9U64Um",
"19rs6hyxyqMt5as5a9ifQM5BKJvmXfAoh6",
"1Gqfz3FKRveaMc94cYxoHZkaTkrJZuFYR3",
"1Q9sZ5FGe2VHGnyJtadnFgjV3gpJgDirM6",
"1AvaYJdpPK11B7kibRN9gXycZUXZfpidhR",
"1PFkedbcYnj3VXYifnJsWemrcEboXfm4UG",
"1Cpn6oKx2mnjwjkPUzt8my58EjYfXVmfTj",
"168yzuo9q3d7AHzabdWRDEaRvF336dtAUg",
"13exgNokmexuHHaXCjcYGEkByLvJGcq9Fe",
"1CxGWRMLQjXN8mCr3WstjoNfQcXLgu9BEa",
"14gDZERm6HSHxUZaNmqQLbx5R2YLFvHDJ8",
"14us7e2qKg22V4rzQYNawMZC3G7YsEP3iv",
"1DMR4Q2QMio63YrAPM77gKcfzyp6xUG1Be",
"14AES6UHcxsL3UAxavR5ryFr7Q3wtAH3y5",
"17hArdNpGBhg2NBh4CRVr4jjEfuNVSejGa",
"1CXQCBwa5qJCJSQU1gmSZyY6f75WDzejyE",
"1PgcLvaAgt2XrVyq8MUiMXRxCDURfjvYNw",
"1DNS1u8e7VjJAhKV17YQMP2Ngeaz1txZ1E",
"1GjNFsRhmnBHRnsttpkXuA8GyQ468HNCf1",
"1AS6yPFeuxocyYv1XBket8tKCZpXgkaQA5",
"1iP9cwFydnTYntnHdn9n8wPtsRm7pjq9D",
"186j84J3eGgJBQoFMYFAVjGpxzFG7urRmp",
"17ANKyQhqgG7hsUVKdnrLckT4boqQ8qNmQ",
"1PmqyzWvC52vgjwR66jEcdc1mDKxrCBWpp",
"1FyTPE9jYptoPLNTi3mtyEhdNZz63jmPyq",
"16EiPPabHpN6dhySyizcs5syNpgzqy9Pa1",
"1LyYPt8SG6Fk6XfJG14sPjAuoYsxHTXtzH",
"1CmxQXUSS7SHMpbCrum4AZqV9nnrJpYYVw",
"1MEaheeCWTm2qbskRLhc5wqU2ZP3UfGWqa",
"1GZ4qKgF9i6nYUDF8RVQDVV52TVY1BcL2C",
"1HKgSvy6FSbGJyfBN8XEmsEKWm31N4H3h",
"1AiPSExx88WQqLkg2MY5i3JNQ8mxryBCWb",
"1HZRWCDEBUVz5TxiFdYkGGoSduaHHzGDaU",
"124SMwbqXoMw5YjHem74AuQWX9DPM5C55H",
"1Np7FRtVpg4jwNGqMrH7iQJMxxu2QRKKks",
"1A3dkhXy1DUXh3J56QoMZZCkBNooxNmM4R",
"1DNEUHNyYETneY7WeDMK72jVcxdzHpQJAx",
"1Ghu6Etf3hPrGMhCD2jheMBYUNpe7X8hW5",
"1Lw3H6Bnu7zbMGa5YnXs4H2qcxLZz6Z1Uc",
"19r8qQgp4E8TuXXKM4MmWKbkP78eXrPq6",
"1qn11k71b7CJ6XBsFUBZAHTpR67fuoDcX",
"1F7ch2wbtUanjxfEdiRqjXVyJE7crMWus8",
"1DGKFkF1f6TtGt16HW4DeADuzSXYUmVHGB",
"18qzfMnxKfpFsRTX8HBbNSqZgnrGmg6Ehy",
"19KhpEZUFBAiRerwT6RAwVVKjNfmao7z3e",
"1ELFMgvubYqP4S12o3JEpjXfXBA67EB3zi",
"1N6xMfi3muLLZny6LSsUbp1bwPZdZpFdFg",
"1EdgYigtCSqgmcMS9P4vWF25HMRvscfg7w",
"1MHZiBGMVHKg8WnTomx1jsHtaajThcCkmj",
"1FpBUR7BVViF5PoSXeHs5U9DXXKZMDazC2",
"13fbZD8U1rHJj591tBhXut99JR1ZUeuBSp",
"1Dw4ULkSQixcsUCAZFoK77vPT9JZM4VKVW",
"19sxLirbVgde7uGwNTaMuYWUF2Kb8dyDG3",
"1MfZC53qYbu8PGMJuAe7MkBpMZBjhk4RFy",
"1659uqUq5cCprbNSrqW7Sps8vvNqhdtXZu",
"1HfBwrWMsusfWEkQND8gfnWQHztCr44sYt",
"1HSUHh4EtKVeVwPCyL1BWXEYU4jEDWZZVK",
"1QC3azQh8FXvwmWLXcJGmymQDpHMQyg27r",
"1LLq6p4MxCNgc7vyQAY5133tm5hx7wq9ai",
"12ZMcq9kp8hGot3zXx39G3jWryeRnKMRcT",
"15ymzbARxQsPqyqLYNoPVeLsond6PSYNDA",
"1E5odviYFFvUrZyjHuEZF77WUvAWwrhZV4",
"1ccziBifSKis3jjuStDExWQPcobvDDnZF",
"1Ep6gY58FKVoVXoTroKM59J5QmKCSs5z3L",
"1FENyZ2dGaisVmkyJbYyRW8GkmvFjSn939",
"19SQrKGY41jCtNUGHCgAXBu5gSs31rFW78",
"17h7xPCBGFgzQpuYiKrLVZXThVEA2U16QR",
"1PQpzCm9bXZmLvZQST9xoiqaGW6PkUvSLv",
"12zz4aVx19j4w7yAwPTk8HkWMDEEtUxJkp",
"16F9vUkDY1e9LSvJ2sriAeLkB2MUWf4KXE",
"1CPWH6RZjVgPzQkQinAxQqNXTt9rRe4p8V",
"1D6Q2fL7k8mVquUjmCPuRaukh4tVXLFDdw",
"1FUg1GvSaSNissZ2gnXYvTG1jn3npVa74M",
"1AQ8GkdN27mCpzGFugMyjCJuoPWDcbayiB",
"1EYVXLyxq35fDULUDeD1eXKtmbD4C7zrjM",
"14z31re3oNH5Z2j1ZySxkAz6gsUHbL2QAC",
"13Ppo9ZwUWgvvKsQx9tAscxXK5TKfGV31W",
"1MPrww1y3ucNSeJ1MWAjxezUCbC4BCaTvJ",
"1BZhLVNnsoWPXT9Zikzxi1nXEVoGKauk7B",
"1LiTJ57F8diikKcoSKhZq9SMjLEtSjXR6o",
"1MtttE9fXCZnH16yZs9ECmvG54HaqBPvbc",
"17Tbjp429vmTGmBhNTUMTjWVGVynyVtJ69",
"1CbM2z1eJHL8W3ySkjntint1xKtQ2SqkJ3",
"1ER1GJCBxZcAMifTB9vKxnfybZmzDnhX6H",
"1KkBGK2yvDY1Y7ARC5QpyidR948cpDBD5W",
"1BiiSGmMurdkSp6PhExyDHfmxFNCyocXGo",
"159EQg3yTsnLAyFb78NuC3fNdJ9crAaKTf",
"1DXNGBxrQeZb2m5EKmspBQd531ruzYLzEc",
"1Bj4xhjwNJuQ9JfxmZFZUViRvswWhBPXoe",
"19dcKG93q2zWyogHSbQLb1M34v5EkLnq99",
"1X5vjhXBLSAJGWBbA2exwvWHFjrRsEWGW",
"1fb3uJtYaPJiyvEtmdHkaUoESpDStb2G2",
"15mb7LkLbeU3mnGoegit7PFnBnbeYXcvxF",
"16abs2xtcuyo1qg5THjYzmQYXScjCzUHXp",
"1FsKWJjcCm7qb1VHN2BpbSy2M8vGeRMzG4",
"1QHsjY77mZFqZqBmoicKuLMShd8z1zcxND",
"1M15srdZantQcsfupZzu6ioy5R8NK2ZXiS",
"1ARLgb9N6iLL3Vh1b53DjZvNM5cm6Hp1yo",
"18PNZuRaLcbQMtQp4jJBC1KxmvLq4Ti3SR",
"1BJxhDCM7NwKbG1Q5GSjBqaXuz7ay8pRf9",
"16tGkxqkTKaGG2sfjA5W9gQeCCX9BXd58o",
"1MA96ytAqudR8vmjeFJdRDxEtGxv3zdWV1",
"1L1DAaBZSE1Gj6uBRhq1LBAY6pLVWgarvG",
"1DPmsgWpURimrbGfeHaZgjnycyTapPrUFo",
"1EWwLe4MBV21N3ve4WWXQSG4LUvS9RTwo3",
"1Lo469AuzkjXWjd4FnxrVjHkuRcj1gpoTg",
"16zTAaCPfbC5dgev37LRhd7WX6Qck5Lv7D",
"12bA8QCSSih5x5ixdHjrVpRNvQiKvhHvzm",
"189SL6NYHqxoMxa8gx5iXKXpppnEw4qxrH",
"1LhyF7TzWEh6jQyMYFW7JNz43zwrb3dqWj",
"1ETvK7T1hMhBM9KcW8DA6s11abqt9pAgh5",
"1CgVXPXtk5cAHjBixqK2cKoUDcB9makjjZ",
"1CeSmdGKDP1HRgP9d2xcaG9K2iAK9dhmWT",
"1JbnAVVQu5kc63jDcRaVaLeXDQzR7U9eww",
"1CVhPQCFZ7zbunEypZYRhuwzDT4G3DaTAg",
"12ornAmKR42E6ow4bajRsJcLSemLoRdwqJ",
"1AaWFrMMee8GX5TPPR1agMkwuuUHLpbCDS",
"19wUsMXu6hwmYoonzcxo8UQRgNXMK6tVZx",
"1BNBZhV8MuuGJWeSA6bcrWcaAuwmcTaDE9",
"1MUz1tWyD3HdKeNcm3WYH1TjJWubSPycjh",
"1CFET2SYMtxvgLaVvqzQwvvhG7kaRt37Z5",
"1N8tqKNsmAqEB5TwzKhAoHeYSxvy5GzmdM",
"1DdnY51jEV6eBjhBXBCJcbdtH6gdLAkbVr",
"1AxCfsPMTtDpjCfjD81pjGNUYrfcnMSXkP",
"1DpXv8pipsg6KABbVkh3XirAehcJ14QFqd",
"1Jk8eWpgxyxxeeQBeeqkeBPXzHeVExwuRf",
"1J6kk1GpjQSYUVQ1pZ5D9TV7XWRGYVMr19",
"12Jym4EMQWwSbiGopGPgHf8nkDyj9du1sF",
"1FWc5LppxD6Pnf2abKQeaL1gMsBpaCvreY",
"1JdK1DVPjMUdaPPiaA8esW4gBWAaS7ob84",
"1Nz9Fz69ZPCzuw4sWMKZrmpuGcTahcYzih",
"1KTeSso79ougBpb4hfwVgPfaURXZyFHMXE",
"18FKd9g9NmpBmHVUCoq3dKwAyCpniQgEhV",
"1E1LWGTf4b3p5brJfdBVyszuVG1vsGunZD",
"17SJy6nuqUZDqpvdA6mhje9qHgYniryHwF",
"1DxxUVno2bKYtWeB7Y9RdubgMfM7GcCd1B",
"12kKEmDCV8eHJFnpdB5HpUM3nGtoBHwUP1",
"1QqDadnWCRyiNRC1rrwCRxna23arHwucx",
"16jgDwQJQuFxdCFCshv4VnHhSaCRdaJXo5",
"18EibNLfJSf5YprTbknA7kCF8i8g4LQwY",
"16kTGN3dPJEF8Jr96q1rrxEYBmVyeUMGLK",
"1Md6XWGU96amKWtuGRZq7p5jRiYTfxYCNK",
"1MzfpiS49ErzEUwgbXe4XyZjrCuy5tcNxU",
"18gY2LYAJUSVxGXKfhFhXVEogdy3mcxfzC",
"16VqR44NxaqTtE8yBhxHjREE7tk5GjKCKx",
"1Go4YPLoGKRMEKstz4y1ekniZ8QaFaosVe",
"1CmoK9N5PbKLjApiHgkUXX5yGXjDQB6nUA",
"1AoNhLWjL4Zzq2XM29ytbbzrrtKWDei7hE",
"1GqZ7Gc5sDCynrQY7fygHPtfUgZPerZKk2",
"1ABYCYbXPNFQwsNhJ57BqNwbduEKb3yqAc",
"1KEqhCjKyDJvdgHfYg2FkGUvk3jDmUGpqK",
"1Jei6zMSTAwcFSNZ7dMPA8W6B3UZTGdKg9",
"1Lp4re8EWQ2Ti82wdf6Mnj2cSsgv5rM1E1",
"1LPekPiyfa4dDh6CuAfBiyrHKojuER2U8d",
"1DMQu3LYUaNMcGEtBufrNKzMEXdtKumKt4",
"13mK9KngSTjVBBq4H2RThvwEuwx2SE38SX",
"1FQ2eL5wP6paZyoYmHwSJf6XVj39cKMkw6",
"1PwrfoiZPiZP7ZpG8Mr1ox8KMT241FDPGd",
"1GCYjmwfPHGsor3zbFjQFG7x3hcqQBXzuW",
"1Fsf8L3LKF8hBRuhVD4A6Zg3zJ2aJgyu6C",
"1HoJnPwnhM7CwvnRc4CCpmP3qjDNRWHZ6u",
"179Hy1oaNR7Etr1r6427nqYbNJC4ReBPYo",
"1JSqohhuFfcJZ83mMnyi4zwRzbfqF54q3b",
"1BjFCWNjnQvCQFiGCErjYpSmthrdiTWj2t",
"1CFNQc4JCfsdcZ3ABHC9WmA7ymsj1YzWrb",
"1812VCx8RoHxwEEbqRjcxhHqaKZtz4kond",
"1Ap5dR7Kts3WoZ9xSMBFPhWXyZZqSuTGZt",
"1DUZ1SGpemCZUUwR3S59gygQdKaqR9hL26",
"1NdwKJEPM6irnnGPFLq4hARuNPS7H7nNa2",
"1NhcTKrLyHm9pPPNHE9YW6cG2PHjHJBPxu",
"1HnherCtKzhkDh8VMys2arUyS4DMyTmptH",
"17e8SZhdMU9C4GhHHEdcAuXzaRKfzgYzDm",
"1e8pbXDYMCEnGvTt28SGhabXx1ASYALmy",
"1BTYLPZ8e2cxzPjXt1KdtsS7X6TnujCz3",
"1KeQTKh2SsGTcbTM2FJ7ZB1ZNwmFaxsuCx",
"1G6G7goRFJCPaPwvykcgKS2XoEgY71GNTq",
"1ASULWaMBVTpMFKttL9cntzq4YQQnBfq57",
"1GqQKnq7FHEWto9SLGbprkcET1Y87Gz3KE",
"13LEoBf2BB5R3y9czrnwC8Y3GdW431xRuE",
"1HbkF5f3pRUZ3aPewoYFyRAuDwvvCpyvvg",
"1AvJ72TYz1HtcnJv3Ngc4MCMhNeyXB1jqh",
"1LaiD2ZUxgP3TmsDaJZxTZiH4w2VkpdPH8",
"1GChRTqH58HiZFgsiSJ1csbg6V8ebvGata",
"19NxSvFnnYisxcCSQHepBPzseqsmAvjxUF",
"1BBejcSWt5uFGE8CbyEq4Fc2zhRcJsW1pm",
"15t7mvdAPYa7FQBqkq5owbr3rwTf5yFdVK",
"1Mbbdnor2bpzcKtkkbos4cUXaSBHBPbPLq",
"18BQMMDzVv6Sm1r8SMZP94yboYogxU5bJa",
"19fy4Toq42a2n3hPz21eE93PB5P7Tu5DC5",
"13GDnni4wxMS6BhqiWEQLY3334PSzYbqUU",
"1EkMA7Gtj1a6H2tdz28RvoLW7Sefi5aJNc",
"1BKC1smkZuuAuqeeKzSbv4LXvLcy1yaVvU",
"1MSedqzPyaK8bmZc8kn7rVxeXZCrPwUJSe",
"192dk24XoJwrTCPT7k6iUqK4SkrPUzdHMt",
"17bDmiaJjzcn48BM8F864av6CB2rojSp4N",
"1L2tTYQWq11hyxAARgu9p9dU3mfk6Ts6V4",
"1C9tqVr1tMpPFq6feeW4v1ByoDrEcHuhjV",
"1EPBbkHcsBuk2SFVrWdjR3NqyigjWS6Pub",
"18tT5wGj3Yq3wdusRuv285rwgMB7HRf2Ew",
"1MLJKYkzxDN6o22UQjUZxAWVwmtP3J64t2",
"1QHUQMsU96CvCmbEF8di3SvasCsnmSfVFh",
"16e6XdeyYTeiSYFiAc7QZfpgyhgVRSeRH8",
"1L8aJqMorZ2tBPM2RNQjwqS3EFJdEXFfvQ",
"1CcHTTryUvTvQjrzjDCj1h7w62nJUhCxbm",
"1LneeJKQ1ALzofX7ARunZ7rSRtVf4KTq98",
"17kkqukQUaYepfdtCw2zmLCarNEMKmADRR",
"1EeCW165FLcLNTAxYV8SXarDpFBkJkYYL5",
"15BoYkm1HqQ8nM9kyTWamHhZXpERDg355w",
"1MfNKEqW8byobgdhpZ7WPUDzz19AY3Eydm",
"1N6KSJBko22GJLxTV414gqSmvaTNFqHGG3",
"1N4YTCSa1pk1HoGgCK1LkpSXwqKyHaEDUS",
"1M5SttDcfDh83Fu52VAsFe9CwKMvojTNcE",
"14WompUFk2Sv48NtFhpXDmWyDgA3wwYr2E",
"1LVhvefS1gFTXgqqdZ5ez8Myaec2cDjZty",
"1CnoeTEW2yN6L3vePoe9y7J6kcB93VPdRJ",
"1HisD3uwChcXZTH8AZzddSN6VimNQsp2Cc",
"115aG5kCyAhAuhyMSvjkuhxpRERhyLKf6E",
"1KUr1g496W5eca6RwpBET5X84dv1Lhxjn5",
"1NCMmZC6g8nifkGSe1mHL5MNnqzXWuevSp",
"1LgvqtzeexDiVcWczmLbxS9kmoWKPiAfKa",
"1NxL7pwSNrWpVgAvB2v2WoKH8q6z1UHPwh",
"1AaxZtSmu2VMXzrYKF54HhzSoPLfgfRJdc",
"114mLKBGm65SGfutdKu7z95jQgDbqkRwhK",
"1MLCuuvGt2yzoaD5D72mKuESNP2LgHpo3e",
"1JxG8m3ByTyTbtGdWrQUH1u8qvT9zTP8EL",
"1CZK8nAarbjxVsCbRBpjfoLMnx4mMY4wjZ",
"1C68mpMEA4pdmZ9KN1pphrpU8Z8eMQypQu",
"1CzZ3t1qwkFipkSe24zptNztP3PPTbgT76",
"167C3BthaAHxP8TViNGgoFkbLSAyhETH9Q",
"1DV7MSdzdoNHHbMvrsC4KNgHnUpdqeMCkm",
"16RAEyRHiSJNBD9g4v7eTHeBn3CYn4RvGD",
"1BCQz4wAjydGhWTBNT5HiQ6LMVzmwY9L1k",
"17Eoe1sVqKQqC9TEEowrNq9pTx6Qvr9vQQ",
"1Fn7nSHnFGSoQkkVAtEPAEuqpCLwsEyAaj",
"14nVZQRwUtU3wtDouMH9FdHnRUwYA6boa7",
"16GrBgzLeqMkU3mf8d428zxuyftvG8UspG",
"1LXYRHy46ip8ZoNZ4JjiwcL4AtKifhC2cG",
"14Nm3PLE4QyAzeGejyo97M6NMnApLSaR7L",
"1f99eSRVqzhMypKmWuoc7eQpW2Ha5ypeZ",
"1PeLrYckPSegFT4xjjDMz3BSWb1CGGR3s6",
"1CxpskRTsQMUqEMKe41hs2h6zL3JBjsjrg",
"1BrojrgTGhjNFSqkBRvro8bXJyhH5MvSwi",
"1DQy6Y5BmimXY93nYZmSPVWiG7RZB7AMED",
"18Wd2k9iJnFRkrfLztVXYAXM1JB4SQq9A6",
"1Epi5QmWbddKNULWnQdyDLSEKDvhQdchg7",
"172wSPcjGmFNVtzGACYNrVEeU1hDSVUQV1",
"17ZGT5zeDpvkMLDSoJH7MdD7TmvBt2pnA7",
"1CSrsNsXREe17VTXE5KjC565Nq27RArfQM",
"16Gmzwu2ByrWTbPXaYkQmTszB5XXYmZqzz",
"1Lk95iiEnWBuPE76YTBThtAKe31xMry51p",
"16uCQNbn9m7KqCiyFzvaBTNWTzhAJpMZVW",
"1GPTWpqup38CZvHBxWaRzqgxM2NVEwoEhk",
"18x3DL8rzW2qu5py8cSDp64NaUWsmqVnB9",
"1FnYPCWHi9Z8S9oHr9bt9MqrBwXKv9myKo",
"17ALakoo74WD6nVXifRMNL5khPcH2Jptgq",
"19vouNruGS2HbstDstr982Kcor9VZPZJx1",
"16RBAnXYVG4UitGE6iaQQ7BZQVoDHexmZv",
"1KE4wibDwAzvTms7sZ9aJevUeKEtwVjh41",
"14swDvg4JJRrxyDCiwfNu53bsYw8xpFLCJ",
"1Bf9s8vAqPxwLifMeZknZsHsYPiZBmpZdy",
"1MtQ5PMowwY3VVeoFfwxxCSo9zxuUoeQLC",
"16CjxDMTJhkwzWcUENrcVsgCMr2PVqKykW",
"1DPZjcDqkPs6G6gVTS9pZwFMmrM9nibXKk",
"1JBEE7MyfQo7MFyDmFCJMUHqR5ppW8Z5i7",
"1MXM1jEgoju4NMBvxr3gmrgxsmdowVPYfG",
"1C46rqGTYxz5FAHWB17DNkw3WPGVPBCGZr",
"17CsxL5xF8nYpUB5p3GWZfT7fUjJwNUqGU",
"1HBmbmS6vxEGRJE6MPbxVgoJu38TuwFsUc",
"1EX3ebbFTnxiTjSD7bU3uJf2SZVSCV3uJ",
"1AznmyCGYnsb8t8hxtYJ2vkgSzJh1VW6bE",
"1KxBkq99PqXwnyQdYj73VTNUCf6BZhhRp",
"1NCYt1dmr6sUkxaFq7j6FCe6JDiTQFfGtf",
"1DfmLmP5ZMkBRe58AHs62eEeb8A9gvKxpV",
"16YwLqhDR5RxRZQ9xShSrVv1RjdC2JDu5w",
"1WUptjwZsUZUJGR3Y5toGXqTgUXzWaEZM",
"1GPfgM5gneoRMGZPq5qSNzZzViKyWWgQds",
"18XtYa5dMymEdWXYrZQR5cBgYPxduPjgHE",
"1Pc8qPmuA3dWuh3cbg1tNVXRNP4etximKh",
"1KNVusf2vMexQ2X3SaZW9XtGWrHxKJKvER",
"1Bazo24pyfexGednvCvxDxcpsrBF4NAmUN",
"1Q8Two4FwgxmNWwYMGTwcpE5C2aEWX8s6D",
"1C6tGsjcdKUtJDgKkXdCSRcpGRDdwocTXy",
"1KLgTC19Hb2Y4vS74Jy9xYTtU5s4BH73gq",
"14NTwEgoaoC9Y8txzDYaJxyS9c189rcBtK",
"1F6CCyVwEE9fKEbxZvDcGJ1pDdk8iQn4sN",
"1GQEmY2oxCpexZDeT8JgU97oPSfXfSKUVQ",
"1NHEyfzqR35FkauFcKDfMsoLULQNhxfKBe",
"188BsU6RDXp9JarmoWJi7rYTeQuL3qz9kL",
"18D59nYKpWu8NaBaWwVWLJfAaKpuHokfHY",
"12uWQPhjg7tbVwJiVgLfEijJQWrDdz89Vr",
"1NhJdVy76KuJdKiJdjqR6gL6vP3TLpDZAk",
"1DQofV1iQ6HsZBLGFiW9hxYn3kqAMLJs7F",
"1xeYmmYNTF8TjWY7a24D3e1V4Qj6CtTRK",
"17NN3CcTpwvLPZzbyTKVK312j9YQvCZKc9",
"18KGisX53DaFcLFb3UeyEmTbW3jRVrorVj",
"1GX9WHo72cHxVruS8bksmSqQB5D9mkCD2H",
"1DanjWxqijGT5BqqrbcKiyukjNhwjYKrod",
"1HkzHKc5S2mvXRP24nMHbFLANm5NLaEe1d",
"1H6XGFF9jgnyqWXHpYzkFvtXBH9Lya6LNe",
"1PikkPwLt86w5MVWrpfyZfMVGbTxBN55Sw",
"13myKXvYC2DQk8mgniQZGE5aiicySMPAbN",
"1NjPdKD1RVNqgKqd2PWJTgCNjo9LaBzrtM",
"18gsjazh6acSN9Pzq5A3wRLuYe3Ks9Ref7",
"1QaxMRiJy6WKcVg6HBgdFEDtYCzoswFde",
"1EzzH2jocL4ZzQFukyxiqjnkxJQFUsjDz2",
"1LrrgkYpst62PbCE4cn9ygHwNk5LgVJ8yo",
"1FQ6g9zcZTfLcsR2SugLzfne6sYkK7DvMq",
"1GxXGRy5YjSSsdWtfXVGhEiQMnWQ3fWdsW",
"1JJCyMACqBV2LVPJDHZvRTjSsDGEEwk2Wg",
"1HYjSmB5pqf3wYqrcHQjmUiDFKUS1aZNL",
"15mnGjTyHE7s5PcSP5qREjiuJHfDgabbBf",
"17sFQD1RR8uGKvhQm5jeeY4xkQmxdtxvo6",
"123Si55wp1dLCNt54AELzB9rRFh5uqwEob",
"1PuenfBjPks7V9pSzq6v4ywNhDT7QvTbUj",
"1PjzK4DdDR1WoHapygxGnPPd7oMZFrPCyx",
"19sGprgK8D4Rq6au1D4QxSnv8vDhG56snR",
"1C9puir4EqovRa9JVBFEXoAoPi8ZKajvBs",
"1QGPkkFDtJ3fFb4boPZhNeNJjmcX5gE99X",
"1PqKULdPCp1dWHonHct496PX4ysFjEkQGi",
"17MG6X5yLdfafJremKHrTeZQUsCEvuLwGn",
"18pDroQRmjJL81ockrQa99yx8KiPQMZNDA",
"1DvAHufRnMZ1KUuP8n3s1VSYWduNSRrgUS",
"12FjFsKYQ85QdSeX2ethiX1tEvRxkAdcct",
"144K3CNqE3ED3i6xBHPBkdbzTJMfY5fASs",
"1JaE7u4hzUSfimoap4stqoGriCrEcoDULe",
"19wbtDMhyNN2NnEaVmEQ8ZV8QgsKeqV6CD",
"1KYnKPpgz6nMKuYaqaSCvM1Y8YWdyT7Y7d",
"1NyeoNpD4rqKtqsSaBRrfTyGKHZRxvio2d",
"1G3ghgBUMxAi5ygpHpFLEXTNW4P9A41MtZ",
"1G2jV1o8aM11q2nLRszr1Xiwj9tbJcS32X",
"17kBJphuCvwxvoyfDoZY4cEvHasdiZxsNb",
"186jWgLowUxu1FcxdySUbH83USfncBAehZ",
"18nxV87E1toY7XQNNSh8x6zoUNxJ7GXbv9",
"14vYxaoWdit6hbwLgRDF9yVvdh8xXakVy7",
"1EEd2b1WqXVe7hQ3654ZF9pB9iDCzhSUYz",
"1PoHw8REoBtLboWj1G2yq8QdMMKpR7RKZq",
"1M97f3jEPCYSiQdhexJxAYZSy79L3NKSfH",
"1ta8AkbsiRRh1c3dj2UDcZdsYo7DxF7Xp",
"1JpmQxLYEzVoJWmaxqpcRWLENghheo2b1R",
"19MpK8oxgySUy5gCf17bFbwA5dCgmXTRqt",
"1FAoFyKvf66zEvnscRHRXEa6fX84vK2CQf",
"15FLuRkKBtWn3AnXdmdZgDDTFYFW7eS7Nn",
"1KnR4RjBB1Zq12LZwrEAdSUejZQwajmoLL",
"18mmTpv4DWby36wJowjPeJM9XT1CqMnVkf",
"1KfP3mzBVPSmrKqyyT2fKpN9jY5FxM57ob",
"1CngUDG5k8R8nwQZ4Af2G2Geh7so5qe9gf",
"1Q6YiirGik6RAA67iwSusYAiYF1i7eL9t9",
"16ChnzVzFbVD3f8PrxvLChvc8juPYskduK",
"134qWVm1rU2cqoRjp1vXy7nv7w6Z5jjYY2",
"1GP7p8U2tJMmgJCuYEuhX2PKiKGcGvuTC3",
"14q1W6ChubCKBPirYEvifSuMB1z8nCshRd",
"1LpV9R1yCB3uqxZDmRy82Qu8rmJbBkEWnV",
"1MG6LvvAY9Znh9yFPLGDFyMzh6omSQgtKM",
"13wqotrbjmtzrbK6NF6LVPRbBHCWye6mCp",
"1M88xDthgg4mjQdPNiVVK3E2jBTgAoXjnG",
"12Kr96qh5rYWMZuS2vhGoJpu3yskn9ftqM",
"1DYBHdnB9g1zhd63aFoxzfqM89wYEXyXCb",
"1z6UsXg6bJYqXvuA8aa1rNhBvdqpeQMwf",
"1Jrj5rkbkUCGrWHwTdbmUCdhymHmu3zHMA",
"1LgVGfStv56LE2ZRpQEgxkHt9KdatNqwnX",
"19k91stqJqMq4GAPkTt68sCDHCTiYtSAvw",
"1AaHKTdYfUDgheX21HfyvJKKjoomnRSmfK",
"12VAkYxQPdyHaT4emwGD5JkuNixucjJCzf",
"12HJamKfTLpHCAdVoespr2AsJ8F3ih1hpb",
"1DpUeRuDND7ZVD2a5w8iurSxk2o6mTXLG8",
"1PRgaa8gGCsRZgyQtLdqVX7e52nHZBKmMr",
"19bS19Cu323K8DjzhjoZqAJuV7xgMa11n4",
"1HQ3Ls8jH8Fh5X91A2DvCK1d4AFehYavZU",
"1LcfD6MM3y41NxVyjC6nHHUqhHEWb59MWa",
"1AtEKJ22CeyWnmBzr7itQ2S5ExjVzSPCjp",
"1DcXbfSGueBYVELpw81ebm5CDdNKQbFp9Y",
"1HGyVXYgXD4Xs7yoSk95xee2dE9AKyQRnq",
"1HcwSBa5rKPVfjeU19LxX11v237tZrk5u",
"1HvPJECkgaL1kfNeNeAgicptyNXC8HNuZR",
"115HdUcNTqniapsiFxp8yNay4hNgwmN1ov",
"13bWavAXgWsrovCFUkSW43BidEN7EKk9MA",
"19tzVFsvgyoc4XxALURPgLL3DqeqR9iNKn",
"1F7u5MfURpUcTqjo7JPWL2YeKErx7Wp6ZE",
"1DFWhG8FYcN6gNgY4vUHbsx2PFGKXvHt11",
"1BjRgMmNdKwfa8sQ7P5KcYiENzrjoJ9BZv",
"1Poj18zqLBd5wvMptmVtcfQib4JTJn3MvC",
"1KMqagngyE38TrFwex5Z38tp2Jt8BEtwFo",
"167kKQY4ieyQZtQHpe2uJyWqgXRXKqtMcF",
"187BMUQ4HiNE1Un8teHGGHPcRxH9cSyubW",
"1328D5TVrs6gMJbJ37ALM7g8G7Ld4v1WcE",
"1PryXvzy1QSpjwyiackk6DhkngXtgrnXxp",
"1Kk4ymoXXwyq1Dsxe7UEdiCMeH7fMg48xe",
"1Jehb2ZbYMB9KCbFjMv4GNLjrWQJhiffgq",
"16S54catk5Wjrvs3Tnd4kGa1hFqTfFmnZr",
"14ntnHCpt4A9yys4vgm4RoWetGMFeBtWeS",
"1CV57LtLP7HNWP2GWWQLoDW2WnjpWiWfzJ",
"1LmmZYqczWkSYpDFPqdZN6N7symAFocP6C",
"1KXMfSzHWJvHvMcwAsyjGUaZQaZ4YiSikT",
"1EsnKiVhWkw9FhqaoHppBor8Rs1ybcBhVU",
"17ZoWqPfLcvD1JsTPFquKjjmP9AxTng1Vk",
"1Lo3Sqv7yR6LoS7uuZU1ZYuDSdnrSqENzp",
"1JbCUyTtkSrWo2uakhP1EfZYPs2LXqAdBB",
"1AyECUU5XdyRzUj1C8RV3nYCCyELJkgEGc",
"1N91rGn2ms7pH1bbrXrNinJ5a9TFqgkLvP",
"144gB32nRQJSZNACTZFcUgehka7FvYERQw",
"1N7pv1TGYc3E1eTEPKHoqa2N9EcR81A2YR",
"1FSYb6bPCsq5TAkDBLWshRLdaHNmLSD1jC",
"193yJTjhX7J4qH3gVLJzfZn9rJmdVzcNNT",
"14K5m5pRhPLRbiauMMjAyJyP8UquTmi5g8",
"183kpfUnzfiVs4fpTQ4uZ2BWDivPZSYxiZ",
"1MSgRUNHPABYLCvdQBJxjj8KaCYnAznPrX",
"15oiybdchdhwEKVt55z2FGouGcL35cqB5y",
"12CmbBGKupum4k5MN5Ko3zbn4Z1xLV1k1e",
"12FAhUBB97ey99eVkWsBbk8qCLsof34S48",
"13oCAGf3ehVb2si1M98evmienjCDSoEESv",
"13fhwsosH8xtcpbij8s3p1MrXEpFqZghmf",
"1Ky3ApC1QGZSqN66VQk54gGUDohE6vjNU5",
"1GoKJZUhKQBEfxHM7njLoADA97ULg32doT",
"1F3SRws6dCYiLWmartRAiKiRwmaFmP4zVa",
"1KcioZG6faNGyK7zEkxGd547wKuRk11743",
"1By7zeRv2wY6XgxTjHVwrh7zhdmHpDiTzd",
"149E9DrQTy85xYbLr9nECAjmauJrNf3KLm",
"15xyRHNHKUPs2x66Ki3gT67D3t2udok9Bt",
"1KujdRKzYvoi1MJAN9hggdai91bCH3qKNc",
"19sM6izgo6RPWKtP6nuzGX4ooXWmQD5AnU",
"19P61uhixrkEc4r79BwgvWoQLtsRhCYtkV",
"1MfE9gvcvgnLyVBLsFBLovz7CURiXLodpc",
"1FTnzq4stR2w3vKQgofC1iTLxzq3QHjorZ",
"1LaU6bcz812JM9L84kHs2BkJ4HrBcVSw7t",
"1F1mNhvcPow2gfYuHiceDAsD5Ky6UCJv13",
"12mMEQWjm6wif6qNbGwT7R77ypiaRuQooX",
"1FK2XM6Cm1oFKQFDuSTeXaq3HEWckArkVs",
"1PzBUsuJLzvCnGg6eQbRmDM9VJnazPUFaN",
"178R6AdQhUfiMEWYvNegN5h4iYhqoMSXQC",
"17LVcfobKodo57RbajPkw46GzHMthV8ygq",
"13SixHTYKsdbE6BzoUfYr3zZxYjPHegnrk",
"1H11SqVKkXvCFM1rd1JT26QosT7AnNorGR",
"1E4nY3Wnbx6mU9SqVkpqYkezoxTBftBj6M",
"17UzwoVc1f2vgzCas6Xwxsun2BokfsrZB7",
"12gqrR8pYJ1NrV5i3nx9UzwSSN7tANbyFU",
"1CmFttNJtb9kNDxrgV5sWRrCuR1NgXXeTY",
"1EdnK4usJzai9sGsSrJh97g1uuxka5Foc",
"1DbFUkiLX2iui4JyQ3xKG9c9bqmPMwriya",
"1KkhqyH9b2Um67CeFQsiziL7DKu141fw3S",
"189ct3PgcA7aWg6Zvt2db4okhz511T7F9H",
"1HVVdCtV7juKaPgtDZCxpNSBrVe1BHM3xt",
"1D4DYvqCiiNgUAFHaZV1fN1LzBgLqaCuAy",
"1N9w99BYZSZMDx2i4PxyWrKkGVLGC6BgFQ",
"1BZJYv87Rh2jNfrxZj4AHRVPDkMyyi6eVd",
"18p7T2DBBSX6X8c1jGMWXVVT3Uk4rHjM1g",
"1PMDHrQtvmV3Qn8KTFkmkWN4QZ26xvcc6a",
"1Cz8HRCDppFRuJLop1r15as5jHz9zXCG99",
"1JGTqtLKcqkSd2izXGDUDTeb8V4bMxRRp8",
"13vHd5X8DosDti6MsaTs5KxXf6AznF8ik4",
"19qVUoxCrwgiFEx24wbUv3uiynDyFp8TeU",
"1FtNz9P9Ait2Vuwgm5s6667Ac4BUaQZDSu",
"1KsUSefvttGu2CTnzacKVXio8rZnAgWxWB",
"1QARerzgrzTABZrVJdwsTqigt2jrBkcakr",
"1D6NRosoxRThKtBn3aEat9S7YCa5CTqDpb",
"1Ad6QF3xKVKinbNnEsYLs6JyN18UtA41go",
"13JkfWXtgVHiefKeddZzkwLG1CCVSMEpxG",
"1FaA7gWmuKKmfaGMfRxd96q6R1KSCAuBxL",
"1NWAa3iM6TfxwC8AJJuwbEAyarBpLkjkYG",
"1D5EFrVoG1ajuQNxUTZbJLZp9qGhanQJpr",
"1CYinVZFuuvfnnRET9u7S3z712EXeRbHk7",
"1Nkaa97dEFpdt48q69gGJKFq7eoq3G3YMe",
"163jR1RttKdFANfbPC3GnpCk4EcNhF69aN",
"1N5eHg95qvR9WJifJVmedAocQsfR4uWLhu",
"148bh8Tdw8gASoJrbypZVaXcDggUiUXHvb",
"1BR2NiJDeXrwdqgiaX3samZT6YjhStTUgf",
"133Yy85uXsNNn3fdNNGcZxg2GuBcrhbE3G",
"1PnGhN6NZiJP5enirXi9VmtPwG4VThEJrZ",
"1DiC4iatrXms9MHavPUNo3WCGHE6rxGt2q",
"12xZhgxDTFHN8zN18uZCbqYdLTJkbTNsj8",
"1NYes6anHyvVLHypCcBJpBhhJEXDjKqWjy",
"1FMUx2Td86JqR3yYcJdafhMb1tXaxVZHjv",
"1EZuFJR3wX3bJjbLYksNeyhSBvVvtp3xFX",
"15E1gdbRv17WhG13iCNQ1yAnhfJseo6Z6N",
"1HTAQYxmb2iZMhAytUF1KxLdxccVBMEcCc",
"1JoZTFc2SHfjn34y7jJQrJTckrZQvRGCMc",
"1FaAA2jiwkt6SV3H85dgbomfccRFLRStJj",
"18fxGBGNpynETNoEWgQnXmEMcUahuNZFV6",
"1GM5hmL39sKmiEMTZkpvJPvMKksEeNdEm2",
"1Ew7sBHhJ8TP3sZWc2WLhY4W2q6BjCAjUR",
"14WCvb7LfLq27nevQ9ptQCgV8u4DYVYEuq",
"1B8i64dnUgoXfRKkYr6Qng2sLdwQE4Wcyx",
"1CYHuVowZxaUYu6Kq3973V8qJnByyMGEbz",
"1FKCzVbAP8gcDTdbaNK4qKQVrnsK9vARSx",
"1M9LPdmaGg3x5KNjes1Y3Cu9wkGPgUd3ty",
"15eHSa5wVCuYLshcmXf4ftXTmZduH93cNt",
"18z9wqJPe9BTkr3nmLKVTQeVKopoNKT3yW",
"1N3ATQxP8b96gWuzhfBe6fwXG1ijBoyGRf",
"15xfYnP19NRfEKw6MyRyfUvYxV5cSTcGCr",
"1LUzxexQ9vz85TZ91hax1fo3ckxxs7YeQS",
"1LwLagfEySBovkMW8QQyfWGSyPpohn9FsK",
"17vpbf8bxFNQEQJyhU34qtHxdbew2svKGu",
"1EXXXnZGE1TXWaNaSzMRBWMdWar5NfL89M",
"1AU1DbkGdQX9XsWRpny1wUKqNcNwDqLvPf",
"168d5Rb5QgxUyXEgGA2o245JHXkp9am8et",
"1Q4bu3iug6EjixHKkmZdcgmstYdCwuyyMy",
"1KKfmXzuMXEpZqa3hUNbPG7pPym7fB3D6U",
"1FcjE8HLySaweEwLE3yzMm3P5yJWzALqZJ",
"1GRcRyZW15envfiCtgpmu1yPpFhKhjmiGg",
"12GQGP9ECxRyoFMVJrFJoTUwDSJC3AarkV",
"1CByir4APttSPg1nfQ6vfExV6sbWhKjTvk",
"1HevbuP6gsbg6Xe6ougHbqedFrga47PTCD",
"16pSF6wQabweSGiwVxfTHa1duAFXiQfmFW",
"17bPL2VUbYyRygrRrbbxtSzeFm52E5x1jd",
"14YrFGZGtB8sociLwEe19VP6dnpkXX4np6",
"18gbXW6mjWzSz9TonMLozytqmso11CgT6Z",
"13kh1VutFUiBhNAJ7skPa9uApozfg7PrFL",
"1JFynykFEBYBTPJaBfCC4kmG4vqn8SdtcC",
"17BjbfiVaQF3prGLtNkbFvSdTZjpgGmR8K",
"19X3jZbi3kij1bwe9en6VKPGQf9KeRdCNH",
"16sR8MFutYJe8Pm4DSbaAvp5JhDmzrFhBv",
"1PieiJQGSqnMa5pnyQ3sQJJo657LL9ozgT",
"1ADKqEWjf7kQ45sKRqiGhCciEUsRKbWSzQ",
"1DdQpWQ1Q2uHrTbUJ8JvNnKZMtCoqtfpYL",
"1BGk82rgMGjLtUUnrbaMjDYyeqj6cbDZtV",
"1NbFN6EBSbpaC6G72hkatUXn58QAa5iqVQ",
"1aR2BV62PYUQBUosYmC6Ue4fa9YGij7Wm",
"12ffjJPfbQYX8TPjJMjzEuBUkSExSaALkN",
"16hGJFmpuBCYJJYqfXVTsdWBFEZwhGMGfc",
"1LBSpBqKS44jF35rXXnRmWXSsCEui6jadN",
"19FJPe3LksADDLK2oHWeeyuBm2NVkYKU4a",
"18Rbaz4PBD9au8pXyW26fgQp4Mc5AejaME",
"1JAnfrZw2W5bHxuYNp9gGQcy6NBh4mFX37",
"1MS4LztRZ7hLRQy86XUHoC5qXhvCweTxyh",
"16y8WkXZPEERDbZPsf72HS2vg2Dq6p4xZK",
"13PQn2stMM9DjgsVwgGsXLnUWScR9vQwUj",
"1Eg5niuH2dSQ6XMee1Aq3gUiYfPQTXKnUL",
"17TArzdsSJR3yjubo6SjojMSNhB6WqsRuc",
"1spRQgHh1CLmnZH8SyJ9jdZqFPDygcRBb",
"1DgJwek6ejc8S8vCyYipiTxqTC3NFSXJ2j",
"14cgfmb2NEh6X1mDkrwunKWtdEjTSMzAZn",
"12Ti3ujd3e6r4wC4ycj1f5uKmXEfi1qMKW",
"1DzVdqM9NV813DEUB1XU21xryc9SeRNWpe",
"1E7m5zBji5exaVhcwNj9FqMHygVAvUUNSM",
"16mXURhN3wkj5JV2jcQRxysUWteGiQECNH",
"1KtjssAbv55CxJdB8ca6YU5BS8jtKUciWA",
"1NqHFGzfmW1pePPRJ1EVX1pbur7R5K7S6b",
"1Jq6wwSxX7zvSZFgFFeyGy6nRMEc4r3Uh1",
"14P2SqTQZDeDroRNurgVW5Kkhzcbanjz7K",
"13GEuYBzjukdsUD5VcS9WPjWjcWzesHoAu",
"14xPyjpnqSRMqckSkDxrtsqhvWDR8fTzSk",
"11pr64Unj1XXfvBAceRxGJPyVkuEYbzPZ",
"1ABdzCfFCvYGyHchVo4DPKKstkUbDs6qFK",
"1Ay6GPEmrDAGTLFiyd2GXwwdtBLHdJogjL",
"1Gzi1ThsCx56uLEwn5mo9n4gALN2rj6M5c",
"16yX255Uuq6vzhwXMyXdz3n9gy5TznASPV",
"1JWc5q38sf9M8raTRcr4WuXKkhxGMccaWn",
"1CiuNGA4GHY1X9NXj9uWL7CmRL6nFbjXbH",
"18DANZzM3GeRvjhxGXc43P9gsngbE6yQgD",
"19rkEuYpxBJU7FE5BRa1FQaBeuKAY7xp8e",
"1C89tf6q8wbUHLguBUEZsnEMjCzGei2txJ",
"16ksPvFbeaa3PkMRKMY1TWNaGct719xkXE",
"1CgwMbk7vUCsbUa5LSrJ5YbgVvg3dcLJRz",
"1MmXu3YaGxckb3nh4qTgVaozMSZaehRz6D",
"1PAtaqMXjTWS6USwrEzmFV6DW8CJ8QNhqJ",
"1K92s6B1tsGZ4iuUT4Q6udJYS8dWmtWeL7",
"1BeqsMsUwkCAQch9n6m9pMDMBaH9FW96bB",
"14keyv5Qb6DrWaD3S61YJQ71dAsCyTuqaN",
"1GHuqwge6fJTjKhncYMPWrrDVukTzhFGW9",
"1C9zFVmYimK3FC948hWe6hMAm4Attobmr8",
"16ccxMh8gwzVTsKPPVW1nLs2nGERyVZPmD",
"1J1ujueZZpVY2Hj2G7d8CaWpsuZ8ZWcvYe",
"1DB3uSTL7czV54C5GLV97hDptdx2GrnMxv",
"1DtjWAAz9d2jEZgMenaTdkmKFfUV8XLagy",
"1HYWU3n9iyUUHmwXXMXxnZ67Awf2kBYNy2",
"1GzYbCUJsVHMHfUdQ7QeYEizqFxPsQtaDb",
"1NQNhxMBhgNX9cRwY3ySaHq4go7N7pRGwe",
"1KYTyBwpF3MXHezpuHMnzgsdmhninJsiH",
"1LDso1oihBpw9W8Kuo6BAuYNV87jY5R1kx",
"183uAxPdi3ygPbVmhoeaFiHUGhGnvakcb",
"15JSnbKXSR5NgoU9mn8mvCSxsHxC6uurgs",
"1NcBZvX2JrVH231KVeeyTjRxzwkTj3MjNP",
"121Mmk5qaWLwuVCfrwirmcobCZweAm1zky",
"1D3KVJNbAHR4E7aEvYKjFM8xjEKLEjUtqA",
"1LGBzo4raQ4eFk8Aryutg99cCKryhU6Exi",
"17Acz6tKp76t4PEEqssTs9P8WoYM3xmPEm",
"1C8CkFfMUWYeXWVHF2GdmCDRyUick2pSns",
"1QBuTQWXJWBgLXWk7nn1yeKjBRygv8FmGi",
"16fuaGfsBru7Jgt4GqU6zv8WH8PKrp9DjQ",
"17i79w8E8kUz3FRhtfPnpVH6nNp92z5Uv",
"17G685pUujiS4DfLaiQ5Kg6MXdw2KbQVBX",
"1LnAjQgVtF7g41c4GztsqKqaFQM3QMSnib",
"13zyRwzBQLyCYQJDmi1X6hokYaQg5cM5p8",
"1ANfaPECQRBNG9ScyhKqXpiBs5FNqqFpcy",
"14Zvwz1JFaTjqApjSyZG5wSzJKggZNX4Ag",
"18mUScFMYtayc43TJby2wTZGCbpjhAiV6d",
"1H8GtEUNt75z7AVLamxQkm9VHREoLHKadr",
"188nf63wqH1fWM6YNDuiin1WCnK3vLGTBe",
"1CMmEQKHGuuyDq7rvQvkFUyCZJVcDg8sWd",
"1AetwNWbKHaLPBxvGdPXpjTkfuP6NCud5v",
"1ED4uqAAt9Wm6socHYBdGXR3g7yeENYRRk",
"1LmyL5KQGERMHEobUThuXgBX6sUiXfiyyE",
"1KJ1xJxbnceDQhvD9s3jxUwr9AjFoQdxzY",
"1E7wmzJuq9kAXjuSZDce7C16SLfVw9yL6G",
"1Jm2oxc3xGLRYjUrUJn7G2awS9eynR2v2o",
"1PgN7Uxm7h68tGhKHsK3ZDsJGrBDoMq5vp",
"1E3mtHZCijbrBKzf9BF3pP1uxc52kewnZy",
"1voG2UzoHFAGWFAyG9sautvmjqT91TQWf",
"1FhMeJgquhuRAQmDsonTSZtQHRhKmMDVSW",
"1PcNwQLfkVrwEHujxE7RezhQ51QxHmdmwG",
"12gCrBwj7Lo4oZ2yBKVaQPZXFnnXqqUyxx",
"156RRzqPTncjXHuUMYBePG1Tk8p2zsTV47",
"14bmbkBNhM9B1zM26Wcv9xJuBty9V3iAvT",
"19NMCSKF2xrRPhgJG6XPdr7y7KKctEyLqR",
"1AYwtwjX7C8Di6J8uKhrQV6vfsFJWD6dKh",
"1G4gZGD2K5719ScvtRnUXSvqk1rsTABXMV",
"19QYUvpPYmpGT64V9AU7dQ33XyeDh16Aji",
"15ui3AXC1MvHoiuhQ5uTFXVUq7GybWdfXJ",
"1H3Qj1uet6faHZrTLvikq4iZn6Fe7EDJjE",
"1HyFMcY2FLhuYU21uFkEfmTqXoNoNSfrmn",
"16R9c3yhfGHMnUiKSm7Ua7M1cepkNduzif",
"16s3hxdAwuq16D8gbA7k7Fc6rZNmaoEwbV",
"19yCvm5EKygeFo7riJHKAFveZ2T3Rt3gST",
"12Ld2TK4vXZutx7ZP9WmuCio2Rn6r9vChp",
"113arMkjmAueV1MWbqL3npHNkEQudw5B3R",
"13fWKhzco7S45QWtjobfeydYMitw6DPsaR",
"1KuSNVwetg71tRU6SP615Zx29jQ31KMDST",
"1Fxv6Lm5X8gNRcAD4YMp9cJ8wZVfnSzZYe",
"16tqaVrJD5hNwEAmEGerF4LPVNkF9nQw3C",
"1277PDNMMzksw7bkhQgTeD6W5AcwcKNz2f",
"13538F3XA4wakStz2WYWhVFaYYJe8WdNDw",
"1MJ6CHR5MkHmREcjGy6wjM9ohSjvfcjAH8",
"1KZvahYmqXgsv29QH2msBsBiCV7TbJ29wD",
"1N29LaEuCqbfZ4Y83Qo7uj3Zuimi62t8am",
"1NX35T38p3qhkBNqYrofnDLU22XxAaiaLb",
"163wHvuR6x1wU2PP22FkyZzPxjaaMakBZe",
"1CGQk2cmW5wqPusYGjyd6PTi3wadknfX5b",
"1CYcdimWBjVPZB1NBUX4npdeBnBtVTKzEo",
"16NnFVU1RYsyosGwVBptDU8u9v2ofV1qEs",
"16pJetunBaysFptCacrVdV9mo5N6gct1AD",
"12vnYdvkdTWFJ45z66bTZEABsWmrvk53MW",
"1Lg6mmSKFTMjuPawTsTpKANDJj9Ez7TkQ5",
"18CVU51gXuUuvVvvziYJ19wndLBjx7KLic",
"1JvmrJztd6cdNUSxiZRqca7rgruibujkkD",
"1EwQsAgDNY8wfaTnZGG2WhH91ydVUCjMbf",
"1CRSAWnywgjAhc1Hf8iXAtEq647JCMm9HM",
"1J7d91awx5oCYbcu8UJLusFTeoqCdneJbZ",
"1N8TREkm7uRA7TuMBTCG2r8RZjo2Yoooh1",
"15zsccaoUmGqhLjZrG5jymGHDdHt2EYqbR",
"14LYjjBhFw545EizRgUqWhBgCH2pvaYEkZ",
"1QCy8kUg8qraoJ54XPhQ52CztGSDFbVfSq",
"1P77EngNehw5KmqPyAiqCRaZsdgZ4WXhkQ",
"1GJCCRRt9Y6b1jfYyt5gptFQkoswK9WLYi",
"1HKwkG5456DHwSamRXg1KZ49ruT3dJcJB7",
"1C132h8MfPzUto5KMwBeS1FcptPTWY7rQP",
"1KLCqEGC5Uz1UPhFSUHN8hydoKm3JHAmGU",
"1NajPje6FkiC7gi6A6urxTf3FimdasfxDn",
"18MR1jmV5iHdBzrt8uA2xHToXxKGUTf7J9",
"1MDn5PXRt8CBrQQAYBqaWmzZsfBF2ZreNe",
"1D4aLJ3jvuzR5vsjA2MN7QSoxXbr8cK3rK",
"1KZBb1qsBXhuK4VRsoFMko8L6L6wnGuhHg",
"18Jn6hnEopyG4xV3raayfCBEzksXTcPiAy",
"15d7bbjjwo3C8ZRiv246oPskk2J64JVdcD",
"15MvbRzn14DEsPiQWJdk1AeXj3WRNpKJSt",
"19AGCQdEFTmzuP5RxyCK4oN7En4g4rGjue",
"1GYNn6mRzYGienKPVx1iyBe6BfZPbabny3",
"1NKUHxtWRGKRLQ8oGPSr3frwfYxLVgUsui",
"14qbEZmqTM5wdxb5JjiqkC2yUohTZuH9tn",
"1GRa2q7sghya32CfoDqv2prrBVmbbLXpfG",
"1JQFcP3gomtPBCpzxoD3kzdCsQawJMGECe",
"15DREnmusHw9sBpz5avutjyfCmvVxy7tFy",
"1Aon8Ettya2TmqZ7KM3gDUuqThwgvYeUTH",
"13KjDC96HGyCzoeH4su3zSXs5C4YcJoHdF",
"1LMGNxwZ5Z28gAgRQWBEo6iGYuEgT6SWYS",
"1D2wJYmsyuDuofXJDMC9UCUAkHYAE2Zpeh",
"185i2fg8SPNU82YJ52rcqkeKPgmMvED3c4",
"1KuTpbwK3wZFwEkz5KtCdtvsepkjhvcHhz",
"1DnmjX1Xpdr6WAjsijwwVHft9MfouxxoBJ",
"1J6NmgHEqkaeYFYcFuTcN5AsFWKmEp9czz",
"1B9uroWdYvtYhVJ2XQnm6eqz5GrnmDscmC",
"1ME8zSVCPamrnEM9WjHPwSzJHqRCbnXSAU",
"1DesApCw8ELv4A43MZL3FeNqBhHeDPtse5",
"18QbHWFzcXztoWgixWfpeFDnShc8a5hitf",
"122qsjLVb6ax3MxfccN15S8RCxKb5xQKYa",
"17cHukdbQqJXuaFfVbwaMcXBHkENYpGKYh",
"17VUdxov8AvvXBvkQZcsurD5YggMmag3Yj",
"14V57GDactogix1LLuUWtBXaL8s4jZmkJg",
"18qdxxgH3np9fqzwPf3qhMp3M8oZQvW7Js",
"1GPYnMfHyQHX3nv3fNMGvCfD76PK53FdVG",
"1PpLKoJrnt3HhEhKxiXtDea2fsrKCyb8oq",
"1PZAQwArzH5JQ4xwbs5c4QEqvU7FTfhVoY",
"15X4Y7gE8LG5zrULaA4B7SjRk9adNZ37Dw",
"19hPxCZhaoVvhi59pSbZcvuxbd4AM42Jvd",
"1ANpS9kggdgqn9UZWiQfUR6gvrAmyt4jwY",
"161xrdc8rb787iZnzefLXLKGpZbXpSqpd2",
"1M8dzmrYqy28tb21XQavi7h9vHnugTRw5k",
"1DgJLiZP8KAxW27ZxsZdTSiLR3iLNNxnKu",
"1EaAMJ9aisaKNLCpyRgXTpEVnasRgGoAWE",
"18R2AW8nwFRdi2Zrjwy1r2jxBuzpNfijWb",
"1EYG7vfhqUwqrV7SwoxJQbXbsy6aN6DdZz",
"1FnjVdkPAZTkJmnmfdJrs1tevnRn1tc4QR",
"13efhd58mZtatUSma3aGreP7j9xk8ngEub",
"1D7K5sFYmyChSshU7W9CxBp965i66gVfey",
"1AjwG997FjcKnLGeX4T9M58ux51cWjjQD",
"1HP58cHAqj1CmTD34VD6zCk1q8TLmegwk2",
"1PeEb3UgRdPZEhxNBeYMrhbRovP1XeAZde",
"17QDghQ2dQLcnvRvEdCie3UEPRWE65qNAe",
"1CGHoBnEUSiirNv7Pug5EgZJAssPrC6tYG",
"196MgpA2Yqhq3ZUSByKhbQ8rNHWYkVdk6r",
"1XgsgDCeaCgkHwFvx3TuenmM7SwyMh2x6",
"1jt5QZfsQi99kNGD3xbDMgSoZs5czyJ8a",
"1CJNXW7Yx1PaontKFhGN6AQhwPrGgz912R",
"15eNAaAKx2sEGzt3C7qjiy2cd9ecvhs7Ls",
"14dKx3iU9rq1uydGLvTtqpjkryDrtjcqYU",
"1CFhzfi8o3ChWYAyFLEjsaRf5xPTCY9foF",
"1AeSmp1Y22jGXUhX9t53pbLUTCygHJhFCd",
"1PB6nX3Bu4EAH34P1QMQcgaXSYAd6c3LPN",
"1Mxdoi6YqS6EEhHKMP2sW6DXWGzsrbqzjr",
"1Db8zP3gS79baaFt4uiUKZgfiLZm1ib521",
"1H6CTwT58uTibE1rjA1jPQP8Ff5WfnEHy7",
"1QJ98P4RnBC9rfTh2iBjgYni85QkkYwEq9",
"1BWS7PFw4yVcQHQ845hS2ksdUxpvhrfVBG",
"1HwcJ8yScwEL33nFmacMxguzaWukvsMb5y",
"179hSMS77H71gPzWUY6Tjz9Vyct6XURwtK",
"172dLHqB3kc6pPsajECfFfejzURkrDkvA6",
"1KoXfmQJKskMuTLasYUjibeZ7KjRC7jhce",
"1MepUq9mkxz6Qt9WjqzPs7eysDKznZ2sM4",
"1LpYrXL6khR7hLMxoaTbRPZkUQqzSYoD9o",
"17YFuq9iv2CrSfa9rQtTRafUSK2DPLfaQV",
"1Cfi1sp8WY5opkTnRYd5gEAZGLo8FkTz2",
"19xsKqV5f2guqWqaApkjjeNzf7DP3PubWN",
"14sTEFfA3p94AUTLekbKNmFYrrhFQmp5Zp",
"15MJky51tG7VkF4yR8HvXqEAmx6UtshFTP",
"1MbaAiCWm3J14DVvvjf62s2zLRG9Ng4MpN",
"12AUUpqQGQYbfYNqcQd4srpq8ZwngpxzWH",
"1EJtnfCPBUwPE5H2YPpkdxcG8cdces4xGT",
"1KBMnwnRKayHMjjybssCdozkhKRFReRCPw",
"1BuCjkhAtxqUKTXRXVNpbKaqgkpAyfFTkm",
"17C48ECwZhUi5phavdVmfUt2Ua7RrfCw6s",
"12mmpR5RAuq18o8SQK3WdYzYoBtQAgcBkW",
"1HGx8XYhNVJE9Bn8f97TFjSCbWmV9a9i26",
"1KWcv43NEuXaEpU65Yqx3mZmmKgMUu4EXT",
"18tYRGpJ5yqY5DboQ8UymkYDCbCujUD6Uo",
"1KjrR5PR2ZXuZt2Nv3i9XgeyaQwopXajRG",
"16SaNyjXTMGdcCmqEvNuu1AmbwqsfHP6WN",
"14Dz6bHBhrTMeuo6FGJi7B5SbHvmg8ji9k",
"16XXA9LLdBDPMRNnkiKMAJDUNGX3XB7KNr",
"1EsJKsEUo6KmQHDKB7L5DBv4ZM4cXq9Yhy",
"1PkzJeRaEcwAyv41F9xBDEJGQTuwEM3iYp",
"1Jm9FjmCC3uL3Yf3zqEd8sy8ZTXmexrG9s",
"18jZMKiL4GFaLpNn2eBFhEmczvkF7NXNhS",
"12JnPUFZZJAVZuX6fXFtQiPakktvgoc2zE",
"1J36e1Xbhm1WpRX8d9xLtyLC1PRzpvnQCE",
"1P8exetMPeDQ44DYrhV1jFgDJoVQky9FyK",
"17cjUykoA5yH2i6mEdXyrvpY7ZAzgAzQc",
"1C2DiGXRH17H1jz1EqF5ui15upZBCMZeYj",
"1JC7X8UNzXJoYjaDNitDaxS7mYgF6vbnQJ",
"1J5Ma8HZESsmRtFeh6WzYEYkMLBtEAMbd8",
"1DGWPrKUzBNrxYdknRMYrRmDZxnXD6houh",
"1Q3cBVFz3rjoGa7ng9Cz26dvLJPDnLLaeB",
"19DXfEJbhiM8fag6mCnAa1ZsDtExZ6K26v",
"13EKRchSUzZMFY2umC4wNGxpePHNUtE4Wx",
"1JuX92xybufu6qaJTaaeG5Q2BF4VQCjzCg",
"17Ax79ESG1yL99GVUsKxPPbraEPXBQUw52",
"1NpbmhcGHCQeJVt6ioaXBCqVo8TvVNk7UR",
"1N56piJboTNvN74u5TDrhYzSkhPjxWqGjj",
"1FeF12QF9Bdzf9D9ztNSewK9UMtupdCUC9",
"17mUwTpcuHWPEi3wW2GVnARajru3UmEJXe",
"1Mny3JWVvyQwAVTnBXXbrtfqBtjvp6AgSB",
"141v2CjgnQopvq3hYFEmuJ6rYvY97gXtmc",
"1PxAMtWKcUYrpEydpXe8sQ7cUPvJSGgSK1",
"13m3wA6RmoEar36nBnhAy3rNN9T8YZntBC",
"16EgLc2soVL3NCZo5d6CcqfFHMY94bc4V5",
"1N7AhUyRaNvCjZ2ckpCYsuGATsnFiKw8Jm",
"1GAf3NamaCRbwzhnrH4ZTwat6XEuYDX3y4",
"134uBHGp6zbJYSELgyzjQbLH6r9BhMZiQZ",
"13dNR8xUsiMkCucj2rfzxkFsmagPCjBKsJ",
"1Pgzo5PrxZB76WgFeg28JU7C8sFpKYuk6H",
"1LHvaghG3RTjDnS2Xv2kHwNGyGDoR9dUNp",
"18VffsnLWLv2KD9wqkGwgNwEzNPANoDyzh",
"1NQcTTJaAzcoQyPR1BbLz2g4YkVux4whHn",
"1FuERAMjEHC3o4YciqmzmHz5fzxoZDQyD9",
"1JT5mC5rnh2dhFwva5VemEQGB9k8bBhm4P",
"12LkZ1cUpky8pwxEVGTJ4QznFPJTuTH1Rk",
"1GGqZBnBvsRBcAKnJpnfKaUcsXeWX1Ho38",
"18JuXtiZz5wEK3ka65yM4g1iFTkteXAMac",
"1CU5U9VZvjqz4K35PdBmjNRocxfhyfNGMT",
"1EmBPdze98F7PamLvq4hKyYj8YkcNrssdS",
"1Nemq4p4Swq5fBbcMHfPBq5H47HyzhhCgb",
"12Lm2Zew9h4RqCfD1G11q8myYAAkm8TPLi",
"1MZd5siPTUbkytn7mAURasKeXZ8Mg79s8i",
"15pGUhGkxJCVsrQfUaDyyaiQyXrqpiDApY",
"18sRR6fjPHJM8J4fCgB8g6U9GWtSkLcdBg",
"12h4wpZUcGQmP1AkF77MhHiJCysnn82CC1",
"1ZCRMxyaNiqWPgv1xSVUNyhgYCnmEfN2q",
"15dosXfwsx7y5ha3Kiv8rWJagZ3WebVFbh",
"1LBWkq9baxqSXifxu76jvFLmLFXQgaoakZ",
"1CeqmCy6nvZQx4V4GyPvL31FHFeD1tNoXM",
"15JxQc48qsh7En3VNVAMdrGm1RdxBt83z7",
"1LModjuAvguLdpiW65SnvernUAFE8tkrMn",
"1HoCNQdURj8zhuoVSdoBCtJFgtghJyjR6C",
"1Lnp6pT9TTe9GEu9DZnyPe5cjY5gcg79hA",
"13w6tuQsroiMepjCSj18Qbg4KsmjJzsE7k",
"12j6APSx949ZhZr857ndD63tD9AdST7uY8",
"16921PKD7VHEEQwHZ8M3ept3DaKFu9yhDM",
"1HCH3bTwJ5G3rQuxWMPt3HDH2BycvJXTkM",
"1PrafXj7mqrnDLN3q7ZZvvYg44XW9ws4kF",
"16utXZT39CBThVyjnNujSxg87NLMv2YsVq",
"16eZTsNiiZ2uF3Dg5NSG2Aa7EpX77KgCoP",
"1KqFgaR2DRndCK7JJf4K2WpYwANEGAfYLx",
"1QH4XiseDMPPqPJFSGdbiKQ57QWy4yHvvn",
"1FjGiaEjrUXWiUYBHzKPLpJpGpDzEiawjg",
"17vZ6oLekmY6GEBc7Gi9dpaGQbwjYenhVM",
"1Haupc7LM4LgeV1xAAXC7R91U38UfQVsCi",
"16BDNNuZ2kLaqQE8xVx9iNi9PUR1q4ndgJ",
"1Q5Dbrt1Z1uBrhNxHmZ9Jt7d8tbzT7s3vu",
"1C7YKmtHWW2hBZYYxWMGJYhytZperVymQG",
"1JpY7hH1xwhq7ev4zcQWYjaaKrCKn6JrdM",
"15mNeAKCKZEf37JYg1nkLQJRNgrumB1fv7",
"1KTGxfE7eVFtWsbQgtFZMtNZjz4oTg1oVD",
"1DGCL33JRVeM8zWHxohVTMiF5VjAdRMStP",
"16w4d3cEQRLKYxpqB5kgxW3t7WnWYc9ndq",
"14MD6cjQDpVVpxn88NbCKEKxFVRD7tpwTS",
"1Cai7GCbMHxHkW9yKe6vYre5n444cL9wFs",
"17YwmgYzx1je7jreuH548Z8GxffmpBRMmn",
"1FGG6Qb4kMh2s7XfQpW5Qz1y4eLTTNgPzk",
"18xPWe6qKyFsszEPVyHVjAki9U3etvTUsa",
"1JVcSHfZ9gxDoZvp9aXDbFdfRZUYkSf9QH",
"12p8zdx6SDFHkyjGHb4GbSxM1JSD62s5FT",
"12TYR8MP5uUda2KQ71c8o4YvyLm9Uz1zZ2",
"1MZrdcwDBbKtvn2vgz1UsLDLyi94S52j6k",
"1KT1qjrzrvwgFXDKtJU9j4JfuUnQcZx1MP",
"1HJxCQHMuh9QBe8BbVCmUyTZWqwARMjNCS",
"1LpfovczzumgRSMPXsFGih66raLEMiuwLM",
"17jSEnbeo1yngxNQtr7BhRd4NvTtrqTigt",
"1BVfrY4rMkPjjrqH7dGKyoUX43TLUj3ZVY",
"1GWrLCX5oem5K6w4oKNAKXMD73kFN6n85t",
"13K89JUnjTAyugW3YemCFsJ95Jzb6MuFEU",
"1CZWCJ8cyadf6oSpKQ6r2pi35uUTpyproJ",
"1M5hUYn1DJQQF8Jip3KEj3Lmxx4sYawZkx",
"1LyBfGeeRGi7KWuzXYtY5nf7MQCbGRzujC",
"1Kq1KcCAG6sKkYxLFxGoZs4m5eshPQkL41",
"1KznsW7tA3uG19E4amSkTJMc1LCqoopg6F",
"13wG6VTQN9AYiHU6BsDKJYGJC8ZuZjfwcq",
"13cQ3bYD6sipg8rCEcHuUg5WknQ7meWuD7",
"1EGok1RhkHFp11yGvfQbovUsopRcU2LkoM",
"1KsvBtJEgSCSF8AnqdC4KCBatFf7xxhDNV",
"1AbngtFpzTjTcDVUNy6uiwHjUgwfLYStjt",
"1CxtVQ1EffG2c4Rovpj76SmCCeHU9jHaTy",
"1Fd58MmSYmEcExc1e8jNSrBuFAdp7KzjxF",
"1EXcTqGo4Y6vEQKWcNA5ncVDqaSoohobdd",
"1HD4JJftjU2uHB6Kip13oLi6w2up2555Hp",
"1B7M9eShrGCTP9Hw2BjP9n1HhbBVBt2zsX",
"1FJeT5AWJWCkgLgCsaBcUTeMxMciiXtkQ5",
"1DKmg9pCYfgE3opXhxjGLGbvS9sU1ofHYJ",
"1PwExhKDJLB2o13aULTmYw4dAXNR1qvV3c",
"1MoVMwkAaoUbouu9pN7SyGAior1M4mU3Yc",
"15hdZY7NpiBxCXzms8pRyeLWA38a3GP5D8",
"1EmgyvvuFzoJqwrhjx755zXon4S3jbqrLK",
"1CQxgfdcYAUkBJgjMDkLuLXM8ErpFqaF1z",
"1CPupcjMwAZLq8VAoLZ4BpAFRXydqM3Jdc",
"1KWPqQPXag8YJ4JhZgvpidbLUAjxvA9mcV",
"1zZSVsCeHsghmxrsw8kuoMpaPoutKZimb",
"114XmNHkZffa2BUMddYf613FNzx6B2qzyD",
"16eMRmFz3sj7m3Pn3J3h45xVd7eBDUDinV",
"1PLueHCV1haZpK9KZpBavJCcwQ51FbJWQf",
"12fq9uVXgzZgDQ3CgJTYPgATgL5vk3wuNZ",
"1PVbKDrR2yzNBgBBaF8AwXT4ZmNds1Tkod",
"18g4rPFxYrZtbeZZhEzCH2H9mmc6zmauuZ",
"1HJdP6MmAEcZ4rFYWcLmGqMv3WRcq8P7Z",
"12ZLSLYxEFswYy8EcAM1bs9vkkn2rxLAnt",
"1DfmBxsVN5r8gwpq35fABiUqDMYvuPg6bx",
"1HHmBCmzMv7PLCNRoB6rE27QymM6eJ8uTU",
"1ALwb3bZZwv4zXS72MdFYjP83WAw5rKSsD",
"17Xymyduz6W4itrVHtHcDSJkjz2rKDPXng",
"13sep1CfSHqE2quhhkQza5pJ1cKGbw1ymb",
"1Hxm35PShcdegaf63Sxrj8QERqezQNDY44",
"1JAWkSZPR5TszEqDscjCmGAN3M4Y2qzZn6",
"1PaBZ5drexLC3x48297EiVfRU1ghVBF1yy",
"1M53o1qErY4MMMu1zWLHqDn3DpWk6g4Yu",
"162t3wdxAXWWLhpKZtCb84kntB8w9TWDnq",
"18ZRfwFNdwzYYs82Jt3tUVpahXDWqUoxk8",
"17j3V95T6ZH8dnEW1qA4VQ6fQCRwGZ22nG",
"12Km7w2jDdPmksf7NBmEiMrDwMjNZkMLsB",
"19rdxkJitK3QmLT6ZujqhZxBDdfTZ9nTzU",
"1EHkcvnKBDHGZtKqav1PCU5SDZU3x2GiT6",
"13fD2QTChf9XFCj2DR15pXiarreZjrwJKE",
"18172WZSLvtVY7xfgSxLdxEZAPgKfSHapF",
"1HN9Mj5L1wGtEgNmdvh9zWCKoLjhRxq4Aq",
"1AqCMSBvjya2pvxVJjvjb2uUHJACTSH4Ta",
"16tV6RrnkKafKCgp4tb7jXMRzupFVKBTV6",
"1KgXio76gKVhhvaZcqRzc7KdXMPdzhAgRi",
"19yx8zsvkf5VuAEZa17bdC8epfTPKFu2uv",
"14SzstuC9CLcHV3zcuhRRjMyDqSUxmzBss",
"1Phv5rrNr2iRzDKBraUF5CmcpinccH8RJB",
"19ejY8Kz1d6ABk8hFqFKWcZruPwfnGmt1n",
"1FC9BS67ANXKtg6n5Ept9pBG9dbwXFL2yx",
"1GnUFctUJuKPT9CYy1V65X8MdXXhghdyQ4",
"1PXN77B9546auqMegrPjZzjiU3cAeMnZcd",
"1GZHoQMTkGKkmN4vn2CJFwvMJ4qBwTuygW",
"1FZCqk9eiPwyiPcz5eg4FFKipvVb9XHrEW",
"1BpchcHD93tXJvR8Ldjgwq98pb4nxtcdKZ",
"1FfGuZVXFkHMnh1MYCHiUixF3s9qYhLaXj",
"1Cv9z46yT4VSNEXC7sZbKyNbFoW3XxhGGV",
"1FmUc848ttyg9YMTzZ1gvfUNRbtoLCQ7sP",
"1JazzVyMCRGoVGqvfVK3JSBXaAQ9aQNxX",
"1HRi1qcKyBtkz2DZkav47n83yFFCeFSjG",
"1JztuMa7147iFLt8GdrP4p4R15po3N9zjB",
"1E1N8rmT34cKzykuZCkHLqYaj5BXQGHEoN",
"1JfyVWjTRGLZE7sw3WsGhNBRnRDbzxuCR3",
"1Joox7bN7AZVP8GwZeFdRXZze14V1cAs4p",
"1DCHVPkw52uDcg5JCrggx5aVponFMKrPQc",
"147WkUADoaAV8iEaS16YCfDFhT77AD1CK5",
"1Q4h5rgJGPytQQqvsff45gx2P5ufp5YZVH",
"1BncvH5DTvZ4Eh9sV4hofvwNp8DG6MJM6z",
"1GvkYRXww17fMqXuymMYeSXtMaBhDDrvQa",
"16xM27EiTm7EGob7ktAFgDYKst2KtrEDyu",
"1856kL9fLvQhb8FNpcVby6V2tsz5uhTDaT",
"1BKvWBTE9v5A3qncpjDHiBZqWPR1wLx6xT",
"1HVG46HC7vcWZbrrfJzNK9NwYs2i3scafb",
"1H7Ssf9XXjsgVXm2GFZj8kdEeedwy3PArG",
"1KG3DV7UnsiPLhDRvvVGQRLkhRX5EEL4Tx",
"18csU283mmP52S7SCQ9SRj1FuHZeaGjoVd",
"1JteKMgnkew6R98mxcfoCgV7FgzDKzLqP4",
"1C9oqBuB5TD6DFgLMtDokLJL8rz41Y88E1",
"1DqLP2JqWEDyw7AXftB9FjRzJyaY7iXp3P",
"159iskEnXWJuyxhAqYLJMMgBsNEuZfexbA",
"17rfovMJ8zSzU8nbjxbZeCFYdKphRnKCxc",
"1C8ZmkoC7va6mAimtN5xRNphkMEhfXuDBB",
"189cxQFZmuj5scet7rENjbTwzRmk9G34ms",
"1Nb6vSGbewwP3KWrrpNj1NQD4XgQxixrNv",
"1Fe9HqEgany2T3NXkSougX3NNmAAuroAQL",
"1DMeHJ95cHXWNoybMVVYCppX7kK2oWjcwf",
"1AtvWXUdebaazeiCNmVCF5mywgdRCHsuUg",
"1HaZtVyLsZp62C9dat4mF6jA3XyzT6d2PW",
"1Ptp8RZAnKSr8PMhuYCzfuwNMZti8yfggq",
"1DQR3JdK2UB5gQFptaVRX6eVmBc8XSUyEx",
"1cyYZWQFnUTLAFF7b5SkmUsLBcCXMEp4R",
"1MsDcomrYkUcqp5eRvaaUEGCybfpwFbwmv",
"15yxWV7fLRdvQz6hxEGii5MLZbSaif6ddM",
"122AkLxTx9mMKP4hnFn2oijJnuyACb3pUz",
"1CkUb4mkXANB3DwoZDA1NCNpf1AdAmZfjd",
"1156ULwwkzEmuVzrYNJqqeDTjDRMsDJjXh",
"1C99mz6k62zC3ENxX384Zub6w4Q5tsfDD3",
"1NeHAGF4ZnWQ4fi3GJ9RV89Wtuvz5r8JCx",
"1F9J2wMsiTubzEAxiL6Sh7xWp7jkAmKGj6",
"1743is45ir3w8m5wYBDzNEezEdSCNrEJnn",
"1151MLtfoUP9XPVgRoyR3WWnwFMbLZtZUt",
"1Cmk5Leg6DKeMnRsj5oauiU98cWXUNcGoQ",
"143HHqowxu8UgvTftrt38dbYgdUKqZFLRW",
"151v6Jj7AE84KJpsqZ5WMYNfKrghgbJodw",
"1KY1rH7cUmNq3CF5bpjpbLuXGNqwuWPx29",
"1MUotX39EAWo2vrXTyDkuHxDfsmrB3NQY1",
"1AGK3zVbCuvMdJBsD8grovqNCED1y62dQn",
"1N6BfYfBXUv5fVm3Qz9EHEXcxcJoguGt4u",
"1F4Uw3ZGjvyo6qqoEeesrNAR8zzksNmUNi",
"17Zs1TvFS8Ue1cL7HuR4XV2yKNWPkGaur7",
"13gUouxmompoJvGnAvNPmM4QwsfRGbEqhW",
"19GqnBZkeRmA1GHDrnM8mVgWvWLMfc4Mjg",
"1GCKMvJ5VYcpvk3afYppR41At1oHZRqWtc",
"1CfaW4y77CRpMiYGGSq4GR77ZPKHT9rrMd",
"1JtNP6SgxSpAFSTrvMPdL23VfuZWnRwha2",
"15Bkyum3YLVbjgpFEZncEP7GdYNWG25MNP",
"1BUn6tKrcCLHJnH2q7tAnMAXU4BHoHZxSA",
"1B4Tyjt94PPPpZfG26yByENLxFA1vCLdxG",
"1LStCvV3H7QVfREmV4mE7gRPExwbsupQCM",
"1BzsbTnhmJtHghTAW3m7q2QgskDsgzNvdF",
"1DVKPMGWZM6uexGX9q294zY6s6Yrf4ZdrD",
"1DWs4jUYZzCeEUMiQkv8VVmQHhcaJ3ZUNP",
"12ELJsXyi7EmFLMCs8nRubDhxzrWcruwac",
"161zg8JW3LbkvmYtgnbwg11dsfHjuniFSS",
"1bn6pZ1PGWjWuezUBBZs25DHXPb9uEjpU",
"14Gp9anUPKqU2VbbaqEfLi9nXa3UZ53uCg",
"17x6zXYsKoUycGEb4wq4Kw4YFvX5VAetEm",
"1J1amJxXjXrJ747GHeBVVebxaCaioaDxUX",
"1DN9818cGYtSXTMvr4QfDnUCSe3ANJgmH8",
"1Gt4yb25TJmnfgmmRLmnEjYPendrKsas5H",
"1MuW8hDXdgB9pCgzxXaxZ7BzwfJRx7mxGo",
"1GbZFwEncH9r8gmcw6bQhVeaS3ioDhF4dn",
"1QLS8hTJcrtBVYp8bezq8BU3WdJPXasA3D",
"1PuFvvXLyX8bfWZ2i4Py7tZwENxWM8Cd7Y",
"1K1ksta4E8zbMZgnUS6u9GmsJZvgTdVuge",
"1MR5R1JYdGxfFzjmtomyegDcBcJWSgh3JH",
"1DLPXzeiuP2To26FPZ12D7p9RpTRkrAaXB",
"1GQKqTG4u9SNQpcoA8VJzYBcKwJk9x9xG7",
"1NobS3ReQBMyyZeDzi4718YF3S9YDx6cLd",
"1DYc3VkmXi3c87qMXwQBPDrJxYQWQ1QV9k",
"14oWLi9x1ZCN1XHRtF6zadHvkAqt7oogzv",
"1NKoipfzz6WM8WoK6tgA9nqTSEQ4vh6m3s",
"1FTZyFHXbzzxCckK9wBJ1bbT9NRZJCaAnX",
"1CqLaga9wfMQ2WbhaKos112tw5DUgbH7ke",
"1LxZC3fmSMp4akyZAu5MgKhy23DhjtGeuX",
"159DeRkqw2aTCF4vusCD1Mjco8Bq1vFdbb",
"1F6iNoge1gm7uY9sxe1xmaS8nPy1WbM74G",
"12AxbKe3UharHNXPFGgyw6jT37GkPEj81Q",
"19UFNrUHbaVKQWUeiBEPwBzHCRGNw9egmf",
"1KVPUvPzrj52Mcy72cp8tR7nmR6z7wXTeF",
"139FQrY6uswtuzVxCGza6hAiXq128LjCFm",
"1JxmDVbnSqYAMHWpPx8EJyDEmEdozJruis",
"19PUaePGB2RVLSwFHjk3VVdWacW842B8Ef",
"1CLgkFh8qRMFqDXu7VALLLqR58e7vi21Xa",
"17dFqoqNktnwQkxUpwUrbNp5gknpHPgK84",
"1HfQQJ2be1PFHBFRVTVn6KZdVJpJGXYrWL",
"1GZsUYi3sYbyiF9tzEeitFZzrNqLPMEUis",
"1CD9haR4zJzq2ow853kKhTHKtdbQC9HeCT",
"161MeMfpKoTGhQHiw2BiWAQW1VXF7r91vS",
"1fNDD4t1NNusfnfNCHiTiCGKAxKWnxA8D",
"1b2dGX6Jvy21KwhQ4fmTrtK2FRPvdZNkr",
"1CXTmFXSejyNNE3eS35eAbDNDH3KnEyLgA",
"1NMLasfJCkPTMFb7HzuBwY3gWGhcUbvFBJ",
"1PMiJJAmwphdiqSaGDyw8bHjF2C1mkvAbq",
"1QKhMjaCKnyBrKRds7iQnVc7NaMLFDifth",
"1KiU6ER7oqFi2Six8xNUna2kaUX8xbvrbM",
"1KYQsAF25vZd2Yrv879dgyb367PivVxpnT",
"17TKPS1fSsm3zu3D4jC1ovt6hCuBNhFgJP",
"1LNpCSwM7ZdH9oU26j1gpnXYRy1cShpBgH",
"1LCK3x9NVk9ubCdMftaZpELFde8HyHXWKW",
"1FnSJG69PGghjBkzEYYDTacBUr9h4RKvbQ",
"1rMxFt5e7sUGPhRhD2PCPKWSqA1bjgzQB",
"1FSuxDisgG7zGuLzSzmYFa2NDqPoztQV5Z",
"164w7nC42smW9WnNNfVJMN9cZ7LLsSGwCB",
"1KqucF6K5s1XfunMYnnpRYHeD87Sp38VbT",
"1As86oN9fdP1HmNEQJuBQgdvJT3nseLWph",
"17TxHFR5Q3RAFWLohMi5UHuGMk9WDumtPs",
"1JXeeHP1uGvxKobUvm3iQoVi5aVZwbEVF6",
"1MnubzEQhornxa9a6QwwJaHJgw8W5FqeYZ",
"1HeTigCFmWbymcPmTo7UjWsuNTvo3G7cNP",
"1Li3eNnKxbxrRVJ1D6MwjrKXS1cgZAp57D",
"1135svZv6doFZ9Gi5jpDcsURtCZAh8SbLo",
"1Pj8HsnDrT7hzjVtybcrccRcTppPJVVPgA",
"1Et6kjPhdTs6WiTfVuRXQ5SL6YNsotwW7U",
"1MyDfc4nycXB4HKVAjuDNSvitcnyZUHCND",
"15AK7NhJ65ByGdgL7M2Uvmr44L9g9tjv2z",
"1735fup79f6CruNtH1DyL6kciDXrYDVuNs",
"1CwnAY1w7SXYyGPawGk8jEcdgryukt5XQ3",
"1PLhTvPx8mc3JhudtQegxzH75otgipV5Qz",
"1GeQc9mUj2KCAMNZCVToRofC19CCVmrhRD",
"1588XPvLFAmNYEFmZ5qfqHEgnkGXKasQVt",
"1ArDEDKnwR6DLERwQTcZdt2jUNQGBibFBY",
"1Dh2s14Hj9Wcmv7ycSAw7g4UE1UHAuwB78",
"1Lg2Rs1UMPP6WKvAPcVZBCy4KGtvUjThEZ",
"13ukVkZzHjFTwZfwiDD14jc4ZVbZJ5eZiZ",
"15VoXEx9esEdDEysizjN8wdmwDnzriVQZo",
"1DejmBcSXuAhbZ3kpWBHtbRV3VyXN5fQVr",
"1GsK6FLJYUdi5RCbibt1kH7k2GcDweaahk",
"1LUJMgcFRHwiZZNPDyy65J5SNR6ytag1fz",
"1DCLyydzY53dy5BoVbLsF1bgjmDWVbR7b7",
"1CMxjTfJ7qqrsKcBvgMiLKbkQ16wuNikHm",
"1GFfLsFD632349M4AGGQKoC6UiKwUuM9Si",
"1DyLDoPw4yQb8JKhsSnHLvC32zEnxCepSQ",
"1AQnaP51e9xdb88R39dDuK8yDuJP33u5rY",
"13ENNQ83TqHNq59eESNePo62a8Ha3pr8cE",
"1k4Lxh4XD5zuJRpVropXPJeGDSXpZFpLN",
"1ApHDGufpSmcCzVMKTQ6Uwpxc6LhZkr7nr",
"1BkrAzjPLnTQMxYbvb3iz9a2h8CgCLgPZQ",
"1NSc1sFSdSKZ5P2bp12uCdysCrBfQhSnxi",
"1KD3V8eyPvMY9pKpGtFVb1nRFZk8TrjRSr",
"1LzK5sjJmFfim3Ffw1B1UaALWrhvg1macj",
"1EaD2UKd8VivSo5BpgxSYGVU3AharbBVMy",
"1A53Ur9VPYwjDo8nUGrbyTh4SDZr7ucc31",
"14PNFFDvdjZdDyjYJxJ6NggMbreLA9vNws",
"15yR6CVPMEsrCds6f3GkoxXN9pZMQk14Sx",
"1BAjarzD4Ne81GgsieMADYr2hBwopbiRVc",
"1FjchbLx9cBz6QhZZCf9QoUjagq6WPw9C7",
"1LdYP24p4mfr5btyUdbK1fVPtsKgBQbc4c",
"12KFYtJY5dWJ9V5YgSERyXh7kG9f4R7Eyn",
"1Hhex4Fwz3Dypd6uG6PUa1iuhVBDKVRmH4",
"1hbKq2B5PwHUWtbfRJKNUf8X4KfjBvPEm",
"1C4GZEpqxyS9bWNMw7kki5PQ79mafcnJD5",
"1MmLPG3VzipCHxCKbczotqGvD2QJijcFSC",
"1NCnWaTfcpWCwhbMUu1PM7s8NXoP1NRrCE",
"1mwUCiZatfdP1xsGW9Rz9B9zERwnk1eN6",
"1EYD9y4VBVmSNTY42g1KCMvRCRgwwPyNZQ",
"1FUxeXCAJYALtJgb2apfmwB6zT5SJFxAre",
"15ZAZoDKvaQ4Xo6KtZKA4dPp9HqSrcBoQR",
"1AtKFQsRuLaPb4ahjJUqX6FqBUPU5BD15s",
"13rbM3g5SAVnPRLUiXkXLyhwotJym5qTgy",
"1Fdcm5iij6zfEjYyMBACwb5KaQD8DxAGgF",
"1KupKKb5eGTcBysgHhC8xet4ttNVkhyt4j",
"18bgpxszc6fvckzZpmxaZUUqJxgF33cXfR",
"17KvY9XcB59QfJFZ4hMEMtEakdCg6GMrr1",
"12AkT3n4rutrHj3kUgmujQrw457RGmKkdP",
"1GXzCU5s5m3Yz3t5AKdkJD3H1abe5E9Sw1",
"12rQyND9WkaTPFf28s6NpgMseTyfJSx69e",
"1A7wH5fqLrizrrQNYFC6Qc3hAV58s6JkoE",
"1Jw6jQrJ3ebMzhxgmXrrGtYyzLzVTyAZ8H",
"18h3yWJ7YUUqr2YLtusGZMbFiBi9x7Fu7U",
"1PXc6MCZo42aR2U4nWhAYPmSaN5Pvf5x9o",
"19WVnM2ZuueUMFTeH3zwdsdhuzDUuU9xwC",
"18BAbWfBAP9bYkWqtvewRCZywH9pwjdsLn",
"1E97S1RDXNJZhN8afKHUTwbTYrCaZDtX6q",
"1DSWJqP35y33P9X3ikKnsUcQDBrNdADZ31",
"1E3vMp9G7hmRyF9ZPHwMvew655neZni9kV",
"13F6AMLmV36ewhCkc96eA5gxyw52BWsAKk",
"1AWscwEYVD3Qa4kAEYrrnD223SwLTdzDVa",
"1JYgHVjnpB4zx7CTtUGo56dJxCVWkDehkz",
"1H1wummeqMG7GLA4uGj4pDATimKB2cidZG",
"15jHqpGTVzbeXsCLxMndkCcR5gp5DMZuzy",
"1GXJSMr2LNXYWdLaFnbnmiiGP99G8r3Nny",
"1HiC7FoC5L8x1913i5HDmMNKX7AN24ypw6",
"12R6RHHJWGJWMgJDR7arkwqufxJgo8kwVk",
"1JosbHwusWRiQaL5HMUzc3WUuTuhH3vGAW",
"12pwaBxxUSmoG1LJ2dbQCZp3yuGQeqaFoW",
"1N2NTJ9SDRCPmvbVmbzDNzVt5CXQNAyZFf",
"17vu4kV1sQnxqHXJvTSjWrY5BD5bEemTFZ",
"1MttFbmpoj9dure9Jb3uS3cZidCeUUYvnW",
"1HCGs9GwQLAFeJqLwLpiJvPEsTmdYtJXaJ",
"19BmmCqTU8YXW7rhMf8jvEmJNQ9CAce2HA",
"1Dqeu4Rs7qaPdWrv5VyTvoC7o269qQYsi4",
"1K4Lh9foreKkm5VsqimHERoMfKV5zGTy18",
"13byN11FoS8jnKwjBpW9LRmT6ptouaz1q3",
"15n3mLX4ud94iXwG7X35ojnFtkPB8f9ULE",
"1Q6acqgcjnqbkqBrgxJBtVCHbXpQcSkpcV",
"13Sh386KS9ViTf4gvxLoKyASqHMHYQmfKy",
"1FrxpqTtqibj4b48PNHDmzNNBFDzL26Css",
"13JoChsBWk95DiEvAuo6nohxKH86kxFXkM",
"1Bu3r46VKS7yfmDUzUjSWehMeDe7visHoa",
"1D5w31trb2nFna1RR2YaNE9rQfgTKM4Gyu",
"19jn5zBgMyJwEZfPeoLi3wLzV7azzRZqAc",
"1Q5QkhcLh3BUTv7cvoG3ZxrtfTG3hQgSxs",
"1DFYs5UtdaNjASjUDHzVBeKNuXTeXvwYsS",
"1DuTsfj37F368ivP43ZjAvwkcCUAUu7aF9",
"1N3PWFjhuDraCmiKyg5GyzkaaxUUnrYHyC",
"1LvjZRckcbuQCscQUjmeg31Sumi22EgwH",
"1Dtvx746hcgJiQaodbM4KWavXCVqrLA21w",
"1Cd1K1bgsavfWPjHQVPn7gzzK8HhxWwaaw",
"1DndjQx6RQdQBSYDXfN6upRQ7Rodoc7ean",
"1HbYqDx65NqBHGtsXkpyb7yDacHNRaKe45",
"1FAsWNiiPcCxqXCu2r6MDyZmd7sSbujK5s",
"1JNANYz8iFLSG3EJgb4HoVjA9JmamwNk9Z",
"1Q65tLbZDiTVgycxBCB3VS2MDzkzWTNV8m",
"1NLvTYDZkwu8LmcKFwakAg7Wz48spag3wZ",
"1AacMphbUjKaae5ZfVwkVWp6vi7vG7UR1x",
"1MTHaCGsmU6F8HJZ9wBvSXJUzBN3QsS5KJ",
"15q5pExqVdZnv4MgWHXoHmYQwvjT2nNEp4",
"1FXv1z6XrpEvKiBtwirsLtZZFj7zNRpcvH",
"1KCujwo5tHQYL3Nr9Jvzp8N6F5swNsa4JB",
"1EZAKtnTmhnGs7nQDcFFs8YCJofzLpdUj8",
"1EQ2x8spU77ZAQrGCDq3q1DangUwjk4pD2",
"1KqA4gum47tMhQJp4wCXeidParkDY9EKQG",
"1GqaR5DeKhMvFP6wxm8SvrPCmLuAH7x2gL",
"17SxDNDUKSzjiRVAdWebx7X4vsaiFXzgPq",
"1KYW8MZRF16WB6SVB1EXLnLu6ewbdvjWnx",
"147cEqhoxZvm56GAckzixf2PcojVRzggVr",
"1KfSTbhTmHqXvFrscdWegCBhUV6eJiDBk6",
"1DMrf96jpjMQEL8YBh8doHQ96M5VZcrRXg",
"19PVPnAS41ChnRZz4UzGFUPxEFtQAJTVcx",
"1ySSA4oKn5kU28MbBDHds9f65MyaEUXQV",
"1F9Eh5Jf7iuX9D9yyPzxypFuHefBuUqsC6",
"17FxJrqsPSknyA3V7cXuiJYqTH24FhWjDC",
"1A7Y6HjdLYXMHaNjcVxG3EeRY3Vq2o2UAN",
"1ExLKGFvcvgtwYcnyouMLaU379Kt75VWoe",
"1MHhbDbojW5Tojtj9sx2ECr4erHfSYLAYn",
"1oMarxxZghi5u9QQtJpUmEkuyXMHh5phg",
"15b5JmFA7Mw8mJLPN9ZqHcudjTFHjqg4Bq",
"1L7JSZQKmnmiKuPneyxtkeWFtWeM8bDp4H",
"1NRQqKBHEynpna4UWtzWT6WYP1CdeMg116",
"19EKimqfx1Bj7oHuKBZP8pF4WBT6K4ammh",
"1HXpV4b3L7NDDsBZh61nKGe55rvZUembJh",
"1BqG3ETZHya8EYSWKoQFMFbQBP16Ah7AJX",
"1DZW2FzSf7xQnNr9cbR4h7GGtXwRZFUPCn",
"1Cnoz9k9iUpCTqcCZ7xhpPkpNTuUCEHjWp",
"1FZzG66RWxNuwjsgBxPtMwNxSfhcbaZ1s",
"185AZzjBqHf3a95eo1AMxzpavZcZ9TL2zH",
"1FS48nQbMJzfQGQQ5LXikAQGxCnwwUgq8E",
"12eHBPfd9mbdquxNQnsRzuHgvEgdf9gMs6",
"1MDeZBVMhaiUCtMhWyafpd9zTPjDZvmppY",
"13sAqwT2RiskrLYH2P8KZymfd4W5opnZ8Q",
"1wcvieNJ4pGiubP9NvmqipYUHasmXNLXR",
"1EAXPnb8ZkpBZTMdBnvcWsQyYt3XLrxA9U",
"1E7d79nf7RdBba2TrmACk3VoPJCs3NRRfJ",
"1CxwXhDvK56fVc1sA6XAcMo4x3feMtZTKf",
"1JWcA7pnfBN8929KcWmzBQccAHL4kjZukP",
"1HTbuk84BKChS3apk2KS2SozMb5sNrJovZ",
"1E9omEzyV1jPHkzVdfF1xZafLYkjFUQHYK",
"1KUykUGoh4eadCX3PpTTJJz6wPBQTuCWqi",
"1MGNrvyxEL7AvpmM8qxzePU9MBcQ5yP5Cb",
"1NSFpf2V4vwihyPZKyLMDPomDkdTVXSyvt",
"1AUC67HZz5dxSoBuczi7S5dazBXvjhe2te",
"17HjkvFPJoRVSuEjnJoJ3CuS4nxCT4YZWB",
"1CgTRV3bQfWVQjUefpRLm5uU6svMnhDKBu",
"1NJCi4je6nMxXSR2V5ut7EpMBpWXUEPBDN",
"1DLHJfvSaL4ZqTtY2RmFiXKjnDQngoSkAD",
"15WEuVNQKndicPyY7t3isUsoPrBrCa5PDC",
"1Pvfgu3BWyqr7NoocAZw4ucm3361J3KKEf",
"1K5bCuhWSCN9JSNk5YJeAoguSTfQB6KikE",
"14qDeMx3SYk7EMzcbSwxg6VjoLs6D8R48y",
"1K75GRAbJTmJDZ14sHPYCxantpuP5FQnBY",
"132bAuyimy1QnNSaMAYrVTAPLAXgXqK6rm",
"1vjwsgKvZrH7KdSGcEpiysAjp1PokjBRZ",
"1FTVfB5WUtQPQe31HKfDCsi2mwehWWET5B",
"1FxF7PdZ5HrUVuXmaFKyBJrjpvDo74TqmL",
"1PF8vUw3EzhttbKjwfj8nvNRydK5CPLY5y",
"1229gnWbRiuZEKDd6XYdPsKu91JxPiRtZP",
"1N7G7j1TPAcLVGCb9q1teM1inDD77PALfJ",
"14uwn16R2EFq9YkPFP5kiXtZr7F2YzkDRR",
"18KAEMWrLacgBMhBC6iSnCWeWd1GuUojfU",
"1Dmg6NTv4paCjoz144AA6Xuc4pv5vLSaWQ",
"1NnowLjUFTuMh7XLpUTBYVrffVWAav5CYG",
"1KYpxStQWLiFPbRjoWkS18pHyP8NBtsqrL",
"1N7CQFkmQw7vszwcboHtuLnGa4qJhcogen",
"18HRNu95B2pE2bjo2pND3rVQcSMz8DCs8d",
"1EDdUQTFktMfx9jA3i3cJY8YZ3GwqL7JU",
"167wfceyufTn72CK5JBzaPnkfK1e5pJ5c8",
"1CmSpj1vjifKWw9WQuaJLe8xtG59gKuRnc",
"17RW3ExkGkN7pqanBn4X6QwaAUPcRF96Zf",
"13mJPmvqPTAKp3H1Z56rtKDtENNbdNojw9",
"13pxCuBvwVmm57BkG6HmefoEC3riisgbwR",
"1DZ6bFCrYEEDXck86vs9CxWxPk3oXGXWt3",
"1HV8rSVpizhJZVzUUYrKFGKJ3NgErQrKuu",
"1DhNBfXw4oMAibz49novc3m6crzrHshhBV",
"1NYgR3WYhiyW2kz9x1r8LjgxqY4JNKsMPW",
"13t7pYNeT9d3ctjFydJzASVSMH75SkJgvN",
"1MEcHf21mQxExntbAFCamvtYxpxFEsjeAd",
"1CVRvH8vZFj5DwiFSf3kHZTERCvH8vpx5J",
"1GfX2xcuh9yKDEQautWE8iu3AvsdHsuGJ6",
"1CesbSQ5EyFrZVJfbn8zE7jtQmLhH4vZ66",
"1PYzoz8eMgf3BBSmSktWKiZ56Mwcpq548",
"1A5xL5bo3rGDmXhF722cH9xgJ4zBfnMdDR",
"1oT9J1aQaS5aStgAC6j44hCYkkESwtxoy",
"1BgJVm1BMsVkRdEYLDvJF4XnDW5e9YVYMW",
"16jDrurywFS37w9cijj551uSEsG1a2JbQj",
"1KfwobvfSKrVoQBewyCgQZJFrtJtM3srNt",
"1JSd6Ttkg113ttgfvgtrpUkiApGuU7SUx5",
"1K4pWkaXDSoEPkAUNJ3wQAj1DZ4dd2wTU2",
"1LroqYzdZt5zRQn8LYXaZ2mrGCpFMJmqE7",
"13jBvgfnRf9H3JemN592wQJ7CMXiC7g4fH",
"1C5rjeXmNVKvxPK9Vv17mekexkQPodPkmo",
"1FAAFJNa6BjkiMsBVwPoZHC7myJk6wAMic",
"139yPF6sKPf2BUkiewFJZ3oN6MdxtTVAug",
"1MKTjZxA4C9er64mQ6e7CHVCp5Pk5Gfoyz",
"1KNNk8hF54ayZvxgd89rsJqPuQpSiWQgpT",
"1LAxeJaDVMnLkDUGaGJNE5do8wENSP8CKM",
"1Fwf2PfzBVU8gHogQjm41exAAmduxDT5qC",
"1KiP4nZv1Qe9Q3HDGbaaRpCxP5sJab2LNm",
"1GFaRCE4J6xdMEA6pJsPvhMvafB8wTCtv",
"1Bmy65NtLGsASZoCedKd6oB3Ajcf2823g7",
"12XCLvGdRrCYKiSS6d8CeK9FJUSnn3XG3d",
"1GyXGRPnwoJ1fjip49oDoC5vg7yKC9ZQv5",
"18bEwhST33x5MEAwsnbmVkq4CTDZBcStAc",
"1MLbqhkLBpD814tdbjqWdeCy29ihLNLSNs",
"16nwQ6GEFCkSvttq9v7XuCa7dhreLLw3cn",
"1FzUvMyZz6qTYLokop83EJtMCE3dYHmoTf",
"1HvB3xD8fgMTnoDAbb4SuZGzPAzy8KJoWn",
"1FdnPyfc4GhdU7df8xZWyvSZLuuJk1ihr",
"1Djdk82uadAUV4HVtqQpSA7y81knViygof",
"1AkDAg6ZmEEgePa8EtZgnubGMc7pQ7HmDC",
"1LqdG3Y3uiAMirfesHEYqJgwNksBAZUCS5",
"1PKFET2fFHQ25G8difYsDL37zxx4NvPyXn",
"16ZxrSucYr4JDCahZB2Gkq93gQVzpUAnWE",
"1PhPpLZyaXQfpPHMVQorVaN9EjEqEevSqC",
"19j36gpN5o8NyrYyTCghVXvByBw28mLpJp",
"1J7XDmS1NZ2p6PzUZGee7g9RdtiUrXGZfc",
"1LhZ1rHjs6Bojt1GDAu8VDhVr68AyTa4MG",
"1GrgKpo3oyRPAWK6ahHFGEj2skRa24WwxL",
"1P6Go69CBU2PjaD1pkbCY1gtxezUZWQrAk",
"1LiUEojtYuLxHqStr8QPLCxe6brN6TdWJ7",
"14GmnbfNScARUknED493gAjppBtw3mC6vq",
"1FUjHZrXsKHcJH7qfHA96jsZjcJftLtoRW",
"1FADYh2DjrfEdB28ZyEjUNmT4faB1bqH1y",
"1FKbf2dmu3NnAHHRyb6vEPdXD5BiRvJ5tQ",
"14SaBMJp3R2sp5NTPcS2FadYqnP8eJeKUu",
"1NKnLzpXHeknUFipCMFExfNf96UaCAMuPm",
"1EBRFnmBgMVwSvXyRBzV33u41tE6RBfEcC",
"1HKWp6Qc4YqaYzo1Du9jDZN9X4Z65EE5hn",
"1KXQb4HkRP9uTdvdjYB3z5brgkjnt8YBa6",
"1Q26N8NKSSSEbNJ6BdjtgzJ1d9ohszBfXs",
"18Qm7rYQTkpE4wo3Aqqe26qqWah3FxbGig",
"1CpPb44wA6pJV6WbzmioiffWaJWiZYJyHs",
"1P5ktdTFFPH7e9SutoTPwVJrjeLFWMBnRA",
"1NGqDeRCRJ4hFoVSxtmxapdkH1dB2X7PAP",
"1JUo1rVAkQiMztyV1aQCEAM7D7b2wvt8iv",
"15hzjd7fKXFf5AdB3SSR7QQJAA4BQZwxUx",
"1DCqWpTPLKTbcBLCYp5kNQ2Xix2Gd6ZQR1",
"1XJRVtGUrqLnBbjBsx7XMjuE8mZN9oXnK",
"1KTWnYRKQmovVp5gMAonoVAhT5T67YiSdp",
"1Jj6kR2YCgHYzuW2duoZoQVygi2nNAhVK3",
"1JcDR9GLPQKmT4diCw6394pbF5GCuLshGa",
"17B91RMaNvH8eDaiTyXk3UHW9uiQW181xD",
"1Eqw62Yx9rPb3jr6m95hjG24swRTPEbvWh",
"15AQfnjGkG5DoBP3t1HkbbTQcQeWxjmWmm",
"1E8WgPS7Ut7cCBYW23YzxziBnZE5eoezNk",
"1M7NU9cx5GSszXv8p2Tjctdrw91MhMjCBM",
"1DvPgZoRfgSkAosEonvJ5gHe4SQJdh7pQm",
"1ChZai8RxjB4K9aqp5fu1up5D66LcznP3i",
"15LuVZhNkM2Zdqc8HbTtqS8pR6jT9aQGfb",
"1Jovpgovi16y3fmGmMzrZ4zSYVSCQy4Ww1",
"1Lk3P3RghsX8YsUSif1CvsSZF1u3hmo4fV",
"15QxdauRTKGJ6c27qeZhmbMQJHQUAQycWm",
"1KmD3XgigLFKicxbn9EcJQp7ytdo8XPieF",
"1AGgb3K6Unbpui4S44NMzGY6eucUynMa3K",
"1PLzmtdyWsnftrLzqmPP6fDVspXhuekPGD",
"1JtKE9Hxsax4wbzAC6WWHwcxQSscmRkbGc",
"17et35K7MADSkMqtSAZwppdL3jP8X8tQgj",
"18HeT8TKaZXCmQVPNZnhdjjGQwym6y7pFb",
"1E5oVhmx41PtCqoiEmJSRbFUkVqXmqQLh4",
"14UGeDqVQe8o784t9QoKX5E4ZRYQjH6sTM",
"16ETeMNmsbGPp4UcfZYe4aRgUzWLWDSdDq",
"1N4pofZ8Fq8cdcHofCxm7gxMFTAPBbARY6",
"1742iJxoG5ZGiLwhuzqVE2F2JqrNAYSae7",
"1DnBKkwc1KVNRkpZ6ap1nM2P9QZ56Xujhj",
"1C7E9uEDiDhXbUKgFaK3apkBpmBFBvTDaz",
"17zBjNEnL39VShv533Mptk1E7HU4wMCQXm",
"18g69fVPhtEcejESU6f4EHk3ntmeAnpmgK",
"15WEX8x5LWNbntz8hddP9xpa1jZ4HwDJiP",
"1MhSh2ftiNJQxWaBQUVvi8uobK457PEp6Z",
"1BygXrGiFJgchHyaRtZkhpjyP5qDAnUFmb",
"1AAW6HxMmP42ouRxYqm2RwSP3yiXDbCnoN",
"1NGkFWZ3tQudGYceUs5oEsii5BXi6Xm1oe",
"1JhZL2DTtdYwjpGhBiLwDbdnWnxnviaF71",
"1F6tCw81hiifCLGTwoda9AMkD8ihgb26jQ",
"1QFSCfcN5o2SDhyokubmzycebikHrezT7x",
"1EKLmt3fYNyMnhM4qqLYH1uEcBADJgt85P",
"15MFYtEPTyX5CdyM7NnrWQZ8jyaZqsmBeW",
"15goTMMyVF7m6pUTt7Vys4gb6eQ8MTuuqx",
"1BgXQjWRFCPVevBHxAUCRVJAaV4FGihNRK",
"1AmcP5vxezYDxs5Jd7PdmQMVPuXTipQVVB",
"1JnTGBmh41AAnCEqvAt5EukBzwtU1SxVdV",
"18hKiGdzHSQYYkWgVgF1JH9uQBtdPjmEg7",
"18F7u6HuLDHopafJbTCBXEVJmzqmEY1Wwo",
"1T3WELu8Gsx8HYUBVbuBoUFF5ywKruB55",
"1KBwtT7dCkZTresbEjiWUuszhA1dqGxkrj",
"1D67JaanYZGRtAeF7ZkGkK2cswucjJscoR",
"14h4mbAwtUn65ctyv6RkQ6TdBH9jTM1fys",
"16yC6P4NAtfSWnPv2yDxQDSMkp3C4Uex7n",
"1PhdWbiaGsNaQCyYWuJUbjVFVDVGZ8ts14",
"12gRhASRiNfKtCunzXUAEKs3ex6FGHy5P3",
"1NTsdjsKTUVUUaxGfAo8dHpD8AZzfEK2vm",
"17JEFx2gxLWoKYJkTVuSAfNC1rnB5TgTfb",
"17mifRZ4RjBDM6HtEojgigAybT2fsEoWPv",
"1MTeYmiCZMf4LUb9vWskbmTLUPQMUUW1Hs",
"16nZC9dq2NjdHi4aBw2yRDTuP1XpFxiusL",
"1FDBEfirxaH1KRACsRDxQZ6wEoyxAcRnG2",
"12NUBpPmriMX51cEr5aXS9J55gNHHcuLou",
"13fkXHQU2ngu8BSHon3zZge8CfectHzwhW",
"1Q3bmCGb267cG6cBCq33AJpegLGS4ZZ2Rj",
"1EDLGMqNHmJY7g25xGSnbNmDLxrvhh3NUC",
"18yc6CcKq8HoXM7CXukY73Q3yjhigNveTR",
"1LQ2gJbdxFV5CWGJ55g4YW5tEo3m2gDoEJ",
"1FLm5EZk1wN4V61WBKBxg8zFcYN9SYsTPT",
"1A7XV936H9xbRtthkKybwCdpa5yBuMrdNP",
"1G3JCeqf5CCJ7cfWe5ELFQK7z89iDkEJyn",
"189NGLwv4wXXfaJdeKLanhUJCQVMMxQrUK",
"1iv63Pmg4i5a7LkgTXZUgH5Rru1MKc5aS",
"16nA21cEzknku89czzz6CpqgFyRu7VpVhf",
"18kbbnZNGBZhUdUcRRNGmkghbK2rH2kZc2",
"15cyZtfL1c4pq2AgzpuaK8ySTpNQoqunJc",
"15mhcaxVLohSXEAkYnKABmLyguhjZkfU2X",
"18fHfw1MQK9n7T1nCQGcyoo95qypdSwoHP",
"1AXFZMZ33Dtyh17mrf7jmzBez7kiy3rjMN",
"1F4sH1FhGNVfctFkfpLXGAuujwLViRBsns",
"1N7PZ1dEWMCA24hjUJrYx4VUR7ehU2ANf6",
"1GVqirbqnta5Am1BgwT2Dv4bu2ow4aqqsv",
"1KZjdWMbTL9X9JnrBHpXSRiPRakrRhbRar",
"1EYCbbHxu8FGUQ6x7F1jSQ2QWnSmHwAT6M",
"1FBRWFnkmpuoyYsofiT4P8DkeLb4hu37U7",
"14MVaJzDT8BGN6C3fVnDtdHRCpGQ9impeu",
"154hVAx1M7uMGGstbjwhKBRUFFhp9UApZ2",
"1N9romZxi7whoWXbngMxJfzTMfHvPBEtsT",
"157qqgt7tw2djZX72EdfZmisfDw6pAJ3Ho",
"1LQdpKegy8ja65kiZQD6q9WvrVJErmBP2P",
"1Kps2nAqVRtwDk7dEfGSQF24X6ysae7bJf",
"191WfPzFrWavbdDT2JgL4RymkvT8drsDLj",
"1G3SWn3DJnb1je34DRaZSVMQFGdAmfQpK7",
"1Co77RvfLSz4EK2Y2Bjo3NLSMA4feYxY4G",
"1QD6H4pUJ6PAc748PUVJTDmZpmJnQu9VPe",
"1HCAqADZLfpXX43vWqGeJCDctQu33m3267",
"16YAgwhkBRdcePBrS5mUNwQTfPqVoDuvRi",
"188Pvz7L42gXc374fr8jUdgyfjz4kzfBrH",
"1PzhxU1eA51BvQS3kpq2DW9ZgMgYJE6AaH",
"15N9czAvWUVrQZ2tmvDqhv5u24HF2ZTgz5",
"18Hk4f3kuLSrXUCXihxKxgMm3YC8SnJukg",
"18A2uZ36orHVPaNgLSUTho8oYhF37nNg9G",
"198jvr3fCq97CxtPdrfS4rjzQCQRbgPfZk",
"1xQNWAVMLZegyZcFkAz2GVaktX2hNR5eG",
"18Yitni9cH9M36qj6H6AvZRFvw1aQkh85Q",
"16wVhvBkR5Q43miSfi3pm9qM7wQPR3ZHYP",
"1AfsEYmsGCvvy8VutAKQXkabnryqUD69E8",
"1AVL5VQXwRpFW1PTdpdKbePHUMBU8nizzt",
"1C4puAfr1KdJA1F4B865gBu1NPf9LyghDo",
"14z2GfjwCvzkS9wBspSbAeUxNXVTANUXAi",
"1PPtf4thCvgCZuWiFKSaWSUetTJLRz1dCc",
"1N3hcu2hLPHsryy3Airx7f2F6eSa7emYkD",
"1PvWkhmwa3u4LegyU1ERfrphWaksJbP7qU",
"1HGwXhwvxPwv3T3DgBYJ4hRhehqHHADTuQ",
"1ok5FLxXB6uH6ENfw3pDKVohE4G3aerek",
"1qgzSJWMdGNBk2HesZ6UL5t8FEaFWTytC",
"13fjxnUmJQqnk4o9qoExMxY692JxWXJ56C",
"1N8j19fvwt7GgHqt7N2WiGNNiiUjTbfS2U",
"19M5DynQwviAqbK4JR9c24eVdhGPpQRT7K",
"16eTM6EzoHb2ov7GLY3PnSExsX3HeDCte5",
"1DhyKYfgv7KETobDrwEnVnwAeb2UWSw6iD",
"1H8mg7jaku12r6QdwuMaYbprf1DNwhr7tT",
"15kH767gXjiAnYX5RCWTEurnT7CfkuFVuK",
"13wksCXNL85XY2zCwm9hs22y1Y2mpPjgkm",
"16gXyYYkKxMceAwiq4wRPZ7Sfb6dT95MU7",
"1NkrUe9nX3N9cD4nLCbkEqQTrzghRG1ou6",
"1AJGtd8kqqMyo8qfAkaSyR42G6MMnjP2MZ",
"1Npkvzf158rJFF6jGWDDhNrSKkyHXzVJvS",
"1GJpeDAM84uvkfRvg1DzNKZSawpPJWy8VT",
"16F33f8eXSCAa8jxvPAPkaMFnpFHJ3msZc",
"1FD8jZwcM2BAhM6WJUqZsRdXzEdfibQhwf",
"19jTxSfCA89B3s7DPnqqdjzZVmDYvWNvr9",
"13mUioUfHpTaj59Us7WwkLQqX2zPSnGuvC",
"174ySqZEivZPNJMkpMWAD93ugpR2VmeTBD",
"1FvtcHGVW8HT1RhAfe4L38VrLagKf3DMxu",
"1MZncrmZ4oWp5wBLguGEc6so5FB4WMmKrL",
"1L5MKjg9mSzwe1CNJCMztw7tfrmwr2kx9z",
"19gL5hGba1k1SrxLDp5x19MDPaSADog3tZ",
"1CbDtiF6tq7SytF48wXiTqRm7wex1cKCPy",
"1JNz3mrNAR3x48XESm4T7p2GuYz2EgqqxA",
"173KxGnhh4cEbfJiKdy5Twhqgu6ZQFBrEb",
"1CWCrUR7Gg1GgPe5KiApSnAYqaTP3WmdL2",
"1PRrHg2MVUYNVysiAhiqdVg8g8tU7gJZwP",
"1NcrLtdW8zQTMBtx5BP3vbRSjLE98vsGfw",
"1mWvRyRJV17nQiZRXwQQQGFXdMgmwbi2M",
"16N3SqHKBKKzohHXPejeb18aYwCFXB24cz",
"1NSDHJ7ssrYrTDApZMtK5ka1M8e8teaQ1j",
"1BmRGdyFvmxuXfARDF8VDUd8XztVvANyGe",
"1M3aQmnYwa5rTEsR7g2SPPWEeG9bZbmtY3",
"1MV7Ksc464YhBGub5tb7hxSZWJdViBDicC",
"13NWLxdLQyC2w53Duxs1QanW6UdzUiZJZd",
"1MPreFS54UABkJEqpeTRxVnvihakd6Wqig",
"1CmU6Ss6Gdrk1ikTjbtPfeFX2WWgDZf69j",
"143LVeTKYSoVUfdkxBEvyNAZ9Bxb3P2cpH",
"16t8o2JNT3Q3Di2YWaLV4KmFG7QXEviz4p",
"1MMx7tzjoKdmw59EZe24xBobYxpejvVU8D",
"1AnwXqEzVmo7MmN5UEWJ2K5EJ51kF9aqCs",
"1FePPBjSwxB8SYrJacUGa341iprYNK4Fzu",
"1JYt1RTGdVnHPixyAZNaQh3Svfkoe1Lp7C",
"1DEKzDXuj9u7dFG8FqMhXQboMujYfgdCNw",
"1NFCLo6PU24Dy5YrXCzTSvwUzWAp9remh8",
"1FE6H1oP3WfWo2UZXE9QFJE1cKKyWb5Frr",
"1DsdBuvjYZ7QUDHaDUxdUmQh8wCViFZs3a",
"1KA2jDNkYH2MzHSjy3cirhuPKq9JxVgLGv",
"1JFB7Ri453HsG1QMu9ASoDqxsjyT4qywzj",
"19btE5mJC8xCqVS6HcDcnxnQqJGmJSQdTJ",
"1CddZc7ryGfCHFATg16vWkUqA3u7Gf7hJQ",
"17Aq91c89uu5e5aEg3bmoZcachdLHx822Q",
"1DtsgXkvMHbgCeuevhW2P3LkrpKT9cNPYW",
"158kTJJCrLCoMZJA8NtYSH7a5Z6oE4aRMA",
"17962866frdEZhD2dTD1Di8cCTh5nBS9Ue",
"186jXuT9Gt2izPJHDvrNrFFBgxodu8pRdi",
"1AbFfMRMkV1zLgV5W4HSp6Y2rReZZrgQjm",
"1M8SDrKUzWDy93rEfLCcWZnip9JH6QoAaQ",
"18djdCZ6Hrw7dLNkGMcfEe5oV3A3xkvMhd",
"1sgfb8XniBUBgcopgHUQRuxuDz3vFDaLX",
"1ATtXJ2k3jnXmo2zsJR6zGi6p7uk9akVRq",
"15mKaqDd8AWwMapBFciJenQRNLhYxDFQbS",
"1EBPY8aHBoowGksWKXikqUCVvfd4AXJvJV",
"1EWdoxJzo9g6WLfkryhAh6w2HRqrUWb1LW",
"1QA78SfxTzHJK91781AB7Ukao5B2GqH12F",
"1CW8UJUdsdn3mrmJfR7ZMfobTTtYB5rtvo",
"1FPtbDeCU6Cv9imPzyM5qiUcaUicd6ooLg",
"1GtdVHaNF7orLNXhdEf1e5jCKhttwqarWt",
"12qnZzcHdWvuUMK7c71CqkNsmwKRWQngyG",
"1Q3ohhxV5CNsGhcYPdQP4fnJKbqf9JonmK",
"18vDMgQm18e1ggo2pWb1PQ1tizG1x3Hggg",
"12MXUbejeTbKF8cDSqQRbwNgMXxasTcF9x",
"1LG8YXj3c2xJVbXGXQWQgYjjUq8s2Ciaqd",
"1KbZUgzwuJ1S9dEwSabbZkgmS8yvbxTcaD",
"14iscUtAfALREcVzHij5RiNAqqojYzSpZQ",
"1CX9XUF2PD1UJSrzoHArCxQsgeKfaF2MQa",
"15xbAn8XRMQJ6EjyEVBM94RuyWXsuTHFfQ",
"192Biw1VPg2KcqmmFwHh2FhCPo7DXyF7J9",
"1N3tiat4oJv7XsyUnCgywC7zJbgBom5uoU",
"17vq32gnkz9PspKtNUCnTUcaVoXREkKexP",
"1N6Jz17c7a2ALydLfropgmmKMPYesXuT73",
"1Cp7AjhUdU4sH9LKHV13rRkghHsE4vsXAy",
"16oSN2ZCyYsoFX5oGAcrn4xsGP3khiy7yr",
"1CDKAt6EejMfAYuQ3QZSZmoG6zFkduVkV3",
"1KU9cGZhRj7XZvvGYx4s5kbsTrkZzCfKqb",
"1FbuPETBesZ8CHunzKLDTFgeAxskjCi79w",
"12opNtXqMJW1SDAPTx49EocXMoxYy5SjjC",
"1Jsam1dRYri3aKcKbUCR3wfS3HA5w7DKQh",
"1J97Ea6W8wj37KqSRu3FGD42QKL4wF43i2",
"1MTtone16a4nnFp6hBuRVbKnnnEpwEDq1A",
"1Dp4AUYWfcEBqTt7m2CFYJnyxq59uYnhEB",
"194WA2CkioVNBKEx8yMDQHtYEteZgmMqmu",
"1HpbR1hyyz6kocmEjp2NTz9rRtcTANjDjz",
"1DNbSaNtwhN5ppBfXMr58kUJVq3kfsq5CU",
"1PYrkPv2RJpzZEGsq2W5KWYsZ7s1wQS1Jd",
"1NMYsW6E8QS1mpwYymxtoNxHzm6taqcb2y",
"1HFN2UPgbY3sddNoQNqiAAX5NtpJFvCEWE",
"149HLGVirdk4egJ67q6Shvok2Qq7Wb9ouF",
"1K7kfDm14r6EcHLG24nCcAZ3z1qA2oV6Pf",
"1FCM5xd2FEzHuPBF2uCYKUvoB4N8gL5XtL",
"1FGHJr8aZZ7N7VENijJRDbey6KffCVwiGx",
"1JFbxhwqv9DZ7oTa76hHyX1jfyRfSpJmBx",
"1FXshondZ23VQtwDJv9rNoRntnVJGT5Fpx",
"18PpjjoazfT7qNyDg85kKUVGmqgdUjaWpg",
"17NkY5z4L7iaDK621aERUnmh5EsEN1vwDt",
"19FpEPiyf8yFnJr7dYMkzwovWtH6KBUzxp",
"1LWedw1N82S69exNbqFrUcyJnRwJhBef13",
"1FCG4GEjZ3Gs5VVbZ8xwGLPttVFEpPbR6K",
"13JvEE9eZk12WxDqr3Tdn1TuU4FywoTxj8",
"1CvPy16iH8dmPZ4aMKzZDG8Bx6GFTCbRiN",
"165zCyE1V9NH48ybbFu34NpUkqbKEsrk1V",
"1FDmDcFFenT1bApPJk1hxCTKczpgYi2426",
"16JURcpHqnUjeLZ5GPf71VQz6VXZhsjqdz",
"1L17autiBHbxVLNXrFTFZpvharzmGdBgAT",
"13pucRkxMMMGDMhrsNUdzLPKx7A8soinyx",
"1PKUCFXvowsDfjUQTUDsArsfZex7BqsbLC",
"16Bak5DziQX8sdSsETS6jt8q5CBViVpwW3",
"1Wn9LKngpSoJzf3QFCv9EWR6pJmZQuW7L",
"15ceimxe7DqYFS4YdwXpcFAS3CPiTfZoQ4",
"1FunAopvaU97GAP92wzcfUrwPetifkUpiG",
"1E5UpumAwVmLiRC6LsDsky5eaVf3JzhXGW",
"1KudSrbTWsKKBVUsGzzsisv9HN1w2iX4pP",
"1GbuccEKhLRUFB8YvEYvVZpKhPNTY1SNoG",
"15n7kwjqHbCgjq2SabzgKp3CjP5jwCRnY3",
"19wS4TbdXE3kbLTi7a6QYoLiQ7TS9GJ31P",
"19bubD9GXXGwLw7ssMBPNVU5yR6GVW7wgg",
"1NQiAuPyJMiosGDSjhzS5NX8MnpfKLABsT",
"1EZkummojGMXoGLRoHhVjh3SpvUYMWE6SD",
"15NwnTTH8iyBW9BDfKZnPja3pAy5ZiVz9w",
"1AXBcnR5qJbtJNzya1WuYJdfXhCt3kZ9Eu",
"1JRaaoHjrKbFSQZ5eYwKjAR7o67FTg4JVn",
"13f3kyeHcaA6NMz76ubf7V5aSUbWGwc3Eu",
"1CRsL6QGfvJGrHCf32Fze1UrrN8iiDgacx",
"1NkvWVyQYCWwErjcgKKR1LGFB9Vi8dhLLr",
"1FryJ6FmAEABYcxh33tx7UtucCKWeD1TGb",
"1CfpVysybEUszd5irQyCWb1FGtf9KJeTZM",
"19Fn7R7icsFmoPJa3FzrtJi5YxVqh4moTv",
"1DAQPM9hyBkfSTXkW3Dd8SEC553LeJ6sCA",
"179bhWzPwacvVqsur4pB9GzJXCf4EXjL8y",
"12kDMDdNSuPEqheeCfdydKRfw1NLkNowpT",
"1FJ3yPH5FuTaX1k75eQALp4fvoS8ETWuce",
"1J7zKLvs8ZG3v5iG7f398iMqmxJMvffs77",
"1EerBsHNugorKNJPU8iPF4rDm1GRANfkRL",
"1MSuk4gz4zLwGrWTTphXRY5D9NR6zzXu4g",
"14V1C5GgrXpBhXkgiiLcXg49JgEHNKENwF",
"16ix2DDGMD29kD1cizNotFsF3LDkRNb4Gu",
"1LpLt61aKhXuxExZrtpK7kCwRs7KxocgL1",
"1LxeNAYCira4JaZfuSpecoyrHGdjpjjWct",
"181jnpLXqZLNtmpzQPnM2CaLu43hQnMytr",
"18r9T72g13JpL5dP5m4aDHVCMhUjVbLZkj",
"1QDQVJN4xMWWaSr8AdFS3JwU3g2xMNoJXz",
"13Dog4R4WQoVhapqnzMF4c7MWUWXf3MqeE",
"1HbEmorsfEwkihJdbsfcGghSZVv6Z9aJUR",
"12d3uzaaFKbEstWanAjwDRjiCiMBFWeJaT",
"12RjrTge4twg84VZjbou5UHR2bZHqJ6m6Q",
"14ihiXwqtRacWFG5syvcEyvfmi7enVmDpL",
"1NwCEvngvDqEzdCcSKZ3U3uYHQ3CPBaPcY",
"1L2dhBTaAQx4KyApkL7aBkAbBj2abCtk3n",
"1CXJSPEhLhhdTHi6VTS19Us1CcK1Acce3p",
"16ZUgDCx4VKUHtyjDZu5GfmcYhyFtbdxDp",
"1HEtdwiuvCdR7tWNCeFEnFH9PVXZskCcUF",
"1L5RzyC9vBY4bHPCDbkkEaVHxf32mySh6S",
"1LQzGvrVcNz5mmsFFJ93C4rMvr6Ceah3fa",
"1LG4DTe7Y9cM2rWEJKHm8pqc6rLiiDxJy6",
"1E8fZ7rPb4V1WMzuu89CVUzTk2YNBYoGfH",
"16u4eFzcVLgD4V3t4u4abMCFSgnF4rsicS",
"1Eoiz8G8gndAEV5TXbW8o4ScfvWFta39DF",
"1E5rCEJ3JcaKXvV6fHmjJ8h9YVZb6bABXR",
"13qaJCTBx4apu6HEgMuy7kFrG6E95uCvh7",
"15dk4vxe1FVwmEKBNn5ok1WhMyDpMx9UVi",
"14wD2WosbqMeYRLpub9Nh6DH7rBuPWPZjb",
"1M72Ti7Sur3uScJ9PQtsNXzrazbpiBpGb6",
"1FHo7rnpWC4TgmptjWF3uoyM2wf6LDMQG7",
"1Adz7G8bfKcD6TMN8QmQ5Wc4oUNg1wbZYX",
"1KVBUsLTkLqsbKAFMaodwT96uMTCGHf6vR",
"1FyjcfLwwmYymQ1wQsjR7cDpdXQZeZyHVH",
"18dpmpTVSkZqx5wHAos68htkFQnB8Rhso1",
"1Dwz32H1WcUs5HMbJu3i2YWQvku3kYkZ2F",
"15c3mWS91qquJzjGZKz5kVMvFnn3uKbVVn",
"17gEMuRpY6JEb2Nfk2UjzMTBdt25aEDVtR",
"1JRpw5gQ3ezbM3N7ctGZVGvRsvRZXungiE",
"16vPAb3XzZe73nAWRY4zKuNAvsiZSwBdT9",
"1Cjg6Yt1UFHZDoURb6iTHPm2ET2gLLfj3d",
"1LuVtaC6hbVje27UGcrhhnHjzf6kg3bS2A",
"1JkmjaKWHgLQuZ2FdvS56mcxa2pjzaBeGx",
"18ViVtKqteuS12RcTcS6hk9zrDYquThZji",
"1KTctWEmJv8ry8EBhjHU8K43joPyeGoQTn",
"1ACYzAaQmfAuV8hTZigfK1ukEz7hp73bYE",
"1NKCpcwztVdqAC3eBXLQeyHjvvpgVWsJ6D",
"19758ofKLmbSJ6NMmQVCmZfZSJM4FM5hZD",
"1BGj3EzMpDpBBVyMxL95m7ujmc73rvTCKD",
"1PYCHe53wvYvHUDYfZgNs3cH7u9eSgXuaC",
"15ZUkZPCj5R6nLZmRMigr7EJo4Qwgivzvk",
"19a73JjgPKQgwQBAZ8mWGnStQPX6gLoQBt",
"1NFr7SuuAiR9ga2JWaDXNzt874sHK5kYGn",
"1NL6Pz2DyJgxMGEodegzZPi9375mU2akJ8",
"1Lm6xyYahHU8MrfoZhxEFurPK25rcT7fvo",
"1PqfS92C9hgpWpaosSVQRsBgn8dnMViPuK",
"16SeNz37iNnWHYPECGFDBWqkZqU4UdgHP4",
"183TCr8h9MmiucsJ1L6ZCBXoirpcXMR5zC",
"1374BMb9qVZ8jG9WratRusPMXtYJ57Hzjf",
"17PbjfmoRLndhvvvQsrQwrTSKyBpeDEKFv",
"1EjGSQvBSKEU7xM2GN8R8EF2xgGWgMowW4",
"1KLf2nxLocLKEz5EpvZnn2ipY7mzJ65qu3",
"1BtZUJnpdikariG7h9iKEgTcFpaLdmaQfS",
"1CFvVQqPKSuwpN3BpSRrJUmSyURK5xGskG",
"15mHdLsxRpFUMQe2QSbvvcuWcrBqUuNRju",
"1KqEK6oCS36RZnRYeyj6EgesPZQy4zEnpW",
"1QBkYvUJrtsWCDjX61mDM4hZRuyqs88nkm",
"1D6VvNSZZMZoMYKWovBMGeR9eHU88TEpra",
"1AZs4DbndwAoJJK4A8YpBpdTizREiSqsqS",
"1Q4TXj65Z4FFdxfvgTTFNu2AqV1ZmxQd8H",
"16H1jtezMXWeSkzU9yqFGcE83bNbWsc8Zh",
"1LHmi7FPh7dCCoU2eSRbkmzacKqeshszTW",
"194CNnFHsUH6H8SqjMi311aQByUrVbhanY",
"13VBZKUixbndvafxFoVxnZLcsabY6jwkuk",
"1A8tLozWbpiAWRhB2JCZ3nVtkJ5mLP9fzC",
"1FSfqMi2PBFiKWbN8zhDzmKv8ib63ETqpN",
"12CptDp9ChpJgPtjXqNnqokksDEvrDLZvH",
"1HYvLniLTwQiqtLCUsQyDQajELjH6PhZV9",
"1ADTY23uUHqDiJMVSMTLfTHTmuV48a4WkQ",
"1EA6DqsiC4UTyK5YD438SP7XoCF2PoZ1o6",
"1GCLUn3SJxsNYr63ub3uR1idsxyAEWhejG",
"13cgk3w8A3Hh6WcSX7yM8YfxLiErB7A3xw",
"14P6GZ8494oW4Qmcdv13wN6t5MXyMS6ZuU",
"1FsUdWsVyBFc6qx8SXStdCN4biMqfKBEg7",
"12YkJvDMoe7aQVZ5fxkVLZL8XuCPwL94Vo",
"1H7e79eoeVQPc1wXxtFjaRmeMLFKMQQ4zg",
"18ogTnbwRi1ei3w2EEFAyNbYzqDY7zj8o8",
"12z5VPB8myoY2sa6Xnz7DexkacZBA66udy",
"15x4zKsHfoszCyYRodhuTzZ7Atntb2XjbG",
"12f1dhjvsJgT6fkGzbXAowK2NTbvWCqtpe",
"16EEbgvjEuuHUVgikQ93ocazAAULER8qjE",
"1AG3HoSCF3iHDn7xDFWMhUXtQhViVd7k5y",
"186rHELtpPW8zDN5doaNoFscZnQkwergYS",
"1DxsV4hiEqMsU7tzJmSd2upHeMYrr3DrUC",
"1o187QNJ2gZEDC6EyAwxEHwyotHKcEftw",
"1JBSX688FT1JgKMZzaRfQRg1FEeU9yY5A1",
"17H3SYTqATKQEGZdYsh4D1qQPNuXvDh6Kh",
"1AYknAvoBUFyxvTS5LPhRZjr9fwyGY4DBx",
"1CEiSmWGpe9S8VD3JTDbL1ZNWoZ3UDaNCP",
"1Fbbi4DsyoGDYFZmi9hLdNCcxj7hHsbKTh",
"16hZCCHM5MKmM95zAiWqvDresBy6RN6Wzg",
"1Hp37WuQhiZcDLpyFYEdhL5USNzXaMWrMw",
"1PFzgq9XCaTPRpbGKcYnzg7nKcKdyxnKQi",
"1BvWR9BGz3VcLnk66vKJHAfJLD1xsssrcz",
"1BPFBEbsjarVNLT13LjmAk2C6GS6h8fNFL",
"1CbjsZjfAoN5Xbb4J24R2nWyF9siCXF48t",
"1GF1D3daFxfTPuANvcMiNSVEifnrob7ymd",
"1M5g7f6x6sDYxQiyaE7nhGi9ZpKmMYwe46",
"1N7QVspudMrsMJ3RgppXN1QhiyEysgVWLX",
"1LALHg8Z62jeSDdrNSEpVMxotUyPfDPpiH",
"196Ro2LiFKFjxgEa8ZxJnnCqJH9peJNsAD",
"1D51yUzRXd55vF6bLEDdNP3dqqbCHMTZZV",
"1DQruaqnBmcA615mm42fezbWWS7H7nCywm",
"1DVLJLmDiSrmLDMaMyZv1L32VjTu1RYwUY",
"1A4x6F4o8z7V2aAEFKDCqi2192mAedE4eS",
"1N8rzFcAJpGHKo61UwYfUPPAbndiAFDKiL",
"1SH4NyMgUYUBVkB1rWMHdbnBiykKgwBxy",
"15vgdCX343WbUdJD6VMky7WXNGVSkPdeMT",
"1NYt2HpTnePmaGvwWAnj2FAvQ3szpo9VtB",
"1LULLyDgQzvWYnvXEXufTyyuhi3XhZbdTs",
"1QETk9FmvBbFwhc6nQECJWjQKtgbx9qU2q",
"17NfSAhuyU4krKKRBLCY52B2oKFZ4RiSZy",
"17xZKHNuoY3Da85gjTh1yq2hDU6RJjNVLS",
"12NjoLLRJCHsBfZzWghNzjZfhEAsYXL4rK",
"1MtCyVkhsGutrhfpxjfoDCG2Z32XKpHpHG",
"1JcTCvxvdNMiPMBMoZ2WVeUhNeJRZ7mDF9",
"1FwfZrqrp4mqMsv9EgVBVk2tBuC3Hp76sZ",
"1Gdwa4guKtr2UQxkS9FKsBJMJbgBRqYeef",
"1PVcY6uBB4Ewf5TkAEZ54aNcQQwrM1jf9H",
"1KLgwf6Hu5Zp4ZbDG6mrRwog3ew5Zr9ZP2",
"13BKmoAZNMmuJcjjcS2PyDqhkBQ4uWAaMt",
"12zZadBp4KKRMtA79GA238VF5HPoQu6oGe",
"17YoFkZ6zGBJmJpK5yA1FYRkfCjjooSmif",
"1KCXpBMt4dP6DbVL9zV2EjMT93C9fVFTP7",
"1JRq9aspYXUcaSRwZhBJ1VLdhqJiQbzugy",
"12MivZmwr37rXusVrNFvLwPPZRyKUiLKSY",
"1vUDB7bxKJ1xXE5MK18TfLzQqBXL4vHbU",
"1HzY8J3q7mjQJ1UZnpEuDYHS9H1cKBwuvv",
"13JTtfeBbyjRGe9RZ2XHo7vLRV8VWk1uxq",
"1MaxXG7Ka7mgtgEKdJR8Vox6RMnsAGo7od",
"1C47KV43SMa2X4yev9bzfHVsQyYC36cCsa",
"1PmvJxdyacS53RRuWgeKyPDWyKQVvoAJR",
"1HBS5xhR1R4D4JPFAgeHY3aSonDKbtPikA",
"1GErRvwMCpmY9GVyytZhCSCdw2zWP5bbAx",
"1PMyusHzBktJTeZURfU8w2n6y36MJUfFui",
"1Q9ac3UupcLZ5FgDj6DoPPnSkToYTuLEqM",
"16vnKSkGtp7bbcRKcdDLyP1TzFqcx52N8Z",
"1NyCe4C7QsC3SJfjYGRZMSUPMLMLUdyajH",
"1PWxtiWzkQkouqAt8NXssB5mE9XHgGnqDR",
"19LUzau25q2FccofafjnLLm88oHc6jbHM",
"1AZY5bPSYma8nMSqKXS3gfwhvbPxatcL74",
"1Dpqz7GttCEdfhThYLvhZutzeKNSvwdPgz",
"17xCiSqFaqNZ8f87vssAAoytnB4iej5yLE",
"1E72JtwLCc4BmEGrmBQjBPaMdnhkn8LnJU",
"1PKutvtGNNp3a3Q8qZJbCgisuoCva1g4pe",
"1DvvdEtxzWnS1WCb8dz37kfA5Yq7jeeqyj",
"1JkV8TQ4n1hZxBvexWyidrCeXrp3UWYNhP",
"15waNXyWuW374u5T41wiPeztWdHwUTo2mc",
"1J7RN6eAoMmTXetz5FayNf4q2gp2SJikig",
"1Gi1bt5icsysTqZ7qtwuiG1GzxgVJTZax1",
"1BT2vQXyQ8gcjdqrWzUx5wxir4Vh59Zrev",
"1MGHdHdmEx1XHsNx8DbkZuJNmiqfAXMBS5",
"1EsG21EZQ47UuEB5txNv2nre895RMwQmpQ",
"1P1oNAsLUKhAxHL3ZRjbBSLs361mMrRC1b",
"131u8NXSLHhhdM2T65GMusdgXLDBs31buA",
"19CzsEPWUUJRCDduxCi6te2w9pPVTRgKMa",
"1Pic2GGQqDLV47eZCZfdmfpmvrCUKTEBBq",
"1DmU4xt6ijDbs2zA2QM2t6gvsMSanD3UN3",
"1L3KXHZmrce7jCvReZ4JzKFohgYqWnkodX",
"1FWGPd3JkNWsFtJhdCYAZ2fRUzdRBUsQwh",
"1JSUVvZHJBcPDur2fXDUsEXNE6STyFDSUj",
"1NRmjuMKVBardij54F7t9RmmfS77FtF2yS",
"1FN3oNJoVjkCEVnUU7JdYKcVAGXhuKrvLd",
"1N3KQiSHyj6SuAsyBjgkXcBuw7jLLhey3g",
"1A4WhYktVNZsfQiNkXxPD8uD2vjCWZdDCH",
"1KQ83ZhSg9LxQWh59yetdUJjmxbAJVJzRK",
"1Bj2XqudyxGCNp5xdTmGEi1Jjrmgrw2YdG",
"1HDcXvpWpJddWnfSxB3gpJmjQQfctfJJz6",
"1DHXNkG29r8UtiBHh6DNcLTXkJsBGN8sgN",
"15cUJ3M5C2kdYYhU5VcuAffnMfUntkjfJ5",
"1FWDAzDuXE5AjcmCPi64qB2qJozzm6LzFq",
"1GLpzLuNo3Czvsk4AK4iyp8SbWztC164aX",
"14Vssa99PvjehBGmfLJhMGsJvQEfkyE1Qg",
"1HGFUHiFdgEe74TXzdVzkVjajCbxtkzh1x",
"18JHuHfsiuhDLNXEMPnkbVqQsrAufvTAAF",
"1PfxmegnNqz55XfaqsUpFdHSHQrHHkuBKT",
"1Mnucdhy5N7Q5TP51jv8o6ThwEK6eeDwdz",
"1NocmAbJwPW9ESdDphzb96PM7gB4Fsg9cE",
"1HWJCWRC8m8NP2jF6iFiWBdr2kJo5GZiy2",
"1JEc9wWtqu5H4Tj7CNym3CbNoDdsGYRvg9",
"15Ktg9RkP1igCAMkasrznMMcGcguQ3FcL6",
"1HaJ4DSr6ZorzyJ4a97nJ8FV2RWfERStc3",
"1NQ9CZ3qskNHUFUe1ygkZxnpG4uPm32Nyf",
"1NzNhbLakBNqWBxWVX9vi8J7wgo9jXD5We",
"1ARoyq7VhZBfsVqYxPuboApmjC7CUeofXU",
"1N8Q8Qi6X2ee1JHLBhYQDeZpDDa1DQt1mA",
"1NsAhjhaM2qXEpig95bh7Tt9Xdnx6nKfUa",
"17v8e5btUq34Sae4GbHaTXibKPgD3wJxQi",
"19pvWdvzHAJdfYP78aWw3uXosGrdBF5j76",
"1DS1usAR7NuSLZRfTZJpJh1Jn1n9ujU1YA",
"1GtyTwxmFAFezboeVhxr5eXQeLYsBSRB2G",
"1E2KPR39os2j3TGw1aUNuA8KsFwNA985ZC",
"1Jdf5ypZxazASVmSoxUVrcdaMpsck7bsU9",
"13zxPKwdEsUac3eugmbqrWf2UFJU14jvNL",
"17T1m7tVtXFpTEeTtejttXYAoKcBktcAVS",
"1P5DjNsJ4PdKibTPFFmCRwjBNxqn5b3ZiV",
"16vAnQ9kJ1ZbRcJmB8NMK2nBP8bAuhXqs",
"19D5BU8yREWxqenCaULbMwBvC68sFt1d3F",
"1KLXUjnoYQvTs8GJBhNN9hoj2qeDfN1KDR",
"1GgGmtYKwaiLz2NiLLBrxTHCCdG9MfwvsV",
"1CattXwj4rPqu3ieRAjmM82oXogwua73xX",
"181KtHN4S5FsVh3QEkZCK7YCtBiucnUAX5",
"1rfmP5Y86Bzbiqpt4rU8d1UsUwYZ1J5Au",
"1L53T4j8TNnKQxhpi4TivmZi8XoYEHDKJo",
"1EY9DccGVYC1DNZeo7K9EnCuvZ3mfgYVDe",
"1NZUgKoYjwJWnD7p7JMAD8n5L6FjLz1Ppz",
"13auYaYVpV9LGp5zPe2pyJB6Mr7RmJqDBK",
"1FA54fgnrW3nAdktneycQq7aj6XzKLNAYf",
"175i5CpdbXrbTG9cTnncwAJf4ha4uYxKmG",
"19NWmkpQH4tUU7gS1a6Zkfk4h89BkP8M7F",
"1PjcERA2qo81xvKYdQF4KDH9RiiXa1kDp8",
"1LZy7QmNngjwFqaKm4iC5oMB7ZxJgh7np3",
"1Nu2Q8rqSifCGSBQ9kRJgwAaDojRG7RowU",
"1JXFrLtNSYUty3spb3ki55UBTbVeRGJ8oW",
"13BBXFPpMxrb12EZA42QutVGRPxsGmcPFm",
"18DDTHuvHUnfSNBHZnDW2mcGvFCnKvr2yf",
"13AF7RvdNX8bu4CgBynPN7byieQLVnFURj",
"13a7eFTxCfrPA6PSVocVM3BNAy8soFf4H5",
"1CQK3Fv2CAyNoiSbxgBkwKGgFQL2ZYKjbF",
"19SFA5QFPYpm7jz6s2SVkQRYufpiafFwiC",
"14KoKx7ESZCxLZrkmwPiQMJV8oHpeWf59b",
"1KiEjuA2gU6zuVKxi4jHBNsVqo5LGEixCT",
"1K8sX1PcKxj3m7whFkPeyuzW49BwsDEoim",
"12d3MehXSztC5gZPujsPnxq3FJLxEi6MpD",
"17Up68vBm5X1HGnpVWEUhQq32iJvco3eHc",
"157Ff2tPhaMfvBhjt7X8vnkVtwT2BAyrr4",
"17fPaMmnohjdoKSL8UFN87hCGwrHctQpT",
"1HRCk7vnLR1AktE8f8cHSkajTD9k6FgE5c",
"1ETwhNxmrTVdQFe3vtBXCiQ3Z9NPs7TgC2",
"184jjbWeE76mAcekKpn5wwJoSGy6hYWfqQ",
"15rNNkxi4fMsF6gYrDvN8iPRpxaBHHdYuN",
"1LMScNaZNNgYyTDW4G8N8dEyZQhycryyZ9",
"1CPhbuVdwyVYYqpMBFo8oukSR1mRm4uPUk",
"1Dj95W1HjFAfgQCcayKVNa5opY2F9DV6EY",
"1JFXGgZX9yWuaYTvnRksNYZ4NFr1wtNKQB",
"1DKJD88DCU89RNVaqRTEAGqhUpR11E3XPb",
"1DTn4UD19kDiQLKQ3kJPyibkyoGhn6hLzg",
"1BSuzQ4NHtAy3DhTZyFoyUcGdHKy1XE2xi",
"1PAc8HHTYumwpLfkP8XMQQSDjUZoQbytoM",
"1Ms7dDD5SuYqj9u99fRuT2pxEuA3La6PeS",
"1P1eDq3jEVzyJKrTR5eSS3Aiv8tkftLpgp",
"1LWMzFGf3cqNMW7DH8eeTtpZtX5LqCfydB",
"12e6FLrE41eYopeBhwjK3RhTwHER4j1kBx",
"1BpLrq8LF4ecEjg4EAJFpN43wwGRkjgtPo",
"1N3hipj7asA6ABri8xLw9gRuipwh9vM4y8",
"19PahLJy2FFtHrrbL94cDJFYRRwuWQ6AbM",
"1G7HQsfWeL11ouPvmvD8VhAmTxSMZg7LXK",
"1xBVuFvFin5FCE8J2wjaPN7jXJy5jHZUV",
"1NkEWDui5BRLBvynJ33cUKAZSeKYPjxwAq",
"1CMHaFgPASewye8QBabWghhjbA43kGzbei",
"1M3r91tCFgz2LB2CvERFEsn1pT2L6zWs8s",
"1MDBfZf2oRxYwvFrx7wZLrkDwGrH1RYCBD",
"18xhBMgEDGfBqP6aXg8itkdA18cA6bTfsa",
"17y2dkpsocWAc9bi8hfN8gAWdnAHJzZcgu",
"1HQMaFXqduaP6XXXG51dFwsrzLUQe4415f",
"16ENHTH69jyagkyA3tL6oAnnCPnhYmNjXh",
"1EXSTHoWPbPNgnCQi1Psbuey9JXZcR9kwb",
"1m2rTfSY8tcSdixR1JWWFNL11hL9jEDFp",
"1FcHA4B9sKDbWhHJVAqaPhMBZJkUEDkqwc",
"1FRshPBk3SutasMkh3UBy9WXS6WasBhg1o",
"1LQtLagYt6ij13j9TYL1myDijg8JpFqkDL",
"1Lygzsd8eTn8rxEgzE7UEyMasQAhTyoDBT",
"1KLuoBSPvB2rjbgPNJaxcTWqTxkawwGGU7",
"1LshGKdF6yMNwPD99njkCK7w1HoXMMyGTL",
"14rnyt19bkPadBCdbjJmM7xcyahkQLkfWf",
"12zt4L5nDMZEngMDX4GAY4aAdin5BfeCeM",
"1DFfeWrnXJY3A5QhqUwak4tzU7PBfZwLYu",
"1MAgQLX1ekN3QQQP6qcDrb7AWy6zhQu6XR",
"1HcvUfmbxJ9gUirpvHDqgKZTkpb4bkXDAj",
"1H4DUXQFHLUVgXUfyjGeK2GLPuKJpBnYRC",
"1JujyrrTKrVhL1W6SQEXvKrmRf1eK5nz94",
"1MH98sBKBzrf2RG43rg9WSHFZohryQowh3",
"1DfvPDwWiWWaSYwJJoLbmmMiRonBRoxCnQ",
"1ETjh8pWDaFUWuuHMw6XsypwBhiEVvrjmQ",
"1h76JAn9vHCGCyskwgMuvfVoZvw4Lpkgu",
"1MbjvajGaf747XDTRvnswESewFUsetA5Rt",
"1BYePMqCV6HzZvh61cXyvCga8sVHxZ5oFm",
"1MKzewxoESuQjZBzFCxjzWdk4WqMtbfrHb",
"16kchhKGsEr1Y8fzSdaPCQSbqJsXohze59",
"18c8y43qbdMbpfmYYfyoKsFEoLGWNbwDsB",
"19jJk2hEHX2DvSHgEfLJ7CH3acVipTz5oi",
"1LDEQ5hbWsaiEzThBSEqB4nRzPcQofnhRw",
"1JsyS55NbzMznZV5RnHMY1SPGtyDV5EddT",
"1KGtftfWRwpRYB7oieBeCHR1oC6BfxNPd9",
"1BiiywgL5JzyZGzUeNBRXhGeAfsX73FuzE",
"1NXTZTnMfX7SKKyJLM1oCc9Kt1cSpu2A1i",
"1NAZhDRnbsecFscucprju6avZMjhVAqMrX",
"1QAC7Qhm53CBTvRNWJTeRcYekMk4r3JGYg",
"1MP4D3DUvKTwsqhZkNBJDfKvNY74g3bKUZ",
"1Ntc5sCzBy9SRJB7CFwvbqdR2k8vZkZUR7",
"1MPjQJCCemRYQxBdQSpdadz1L1mupNCoCM",
"1BFvTAPPbfWosanAS3x7S5dPnwy11W1BN4",
"19WNMrUxx4ZN5YqakGrHpfsvME5Yprwhut",
"1PihJfAyna2soekE8NKuqNPtgTAkqAxjJB",
"1HJ4fkjM1ouxrwhpwzxeLG4CmC9aTapLjo",
"1Nm57mrxQ9GSt76zud3BHRLbiLYqXyXVRM",
"1GapdxH8G6VTqQmJVrqDebj3q8wVRiVfph",
"1NKd2vErtkXfLP9G9Y4D5nMtfHvjPc2w8j",
"1ASy4YrCdS4yLY5k3Fnw4TnFkUuSaJbxB2",
"145BNeFHmXYRq1A3F88FzZf2GC4CjofCga",
"1E1ndzjJu4vP7FsQ2HYHEj4JeTAoNRdDPs",
"17DfQhCX4HaVNhJCwNcUCWhwHLWV3bp78f",
"1LH1UGoWgUipsx2kN6uY4FTQtfDzGyTYeu",
"147jTXtNwYgGQGwcwRsY8F6NYpzUUUXbXE",
"1LDZaygngGzyV8gkBVni9ZpHTunHgwQRcw",
"1MPEUy1YwMHyMNLpvpKucZ9QLZZd68Qips",
"1M28ST7ULW9H39pj2pJK3QSoJkrBiHE7M5",
"1M9X3Z3VLptFcZJtMze6QPy94KfasyRN9s",
"184sXh8ZGgpsxM9CDr2Tog7bgKPkxPCfkg",
"1BYqcJPXiu6ZMBsLhtiu6Roq2ik3ha7V28",
"12ihcauQ3mvwHLhHN7XtBeCeLLTHM6HKBx",
"1HuuHQzRphWitkcbXzEujiYmCJuduM3dcn",
"1FCwBnHYGy88xpxPtssvUscjBWgu79YgRP",
"14AzfrMqVMtUWCfdNp5HT79gKNd8oTXoBY",
"1Byg5DqhB33NRZTrQ5Pp7Dw1KwVgwrK7uU",
"1AcmownunCLxBPjtiba1oXNzDToSDxigDn",
"1AdVP8p471tn3JKHPZngf3epifUpMbwie7",
"1MHqjnsJnhqkqt4qwNYPKzoPnvSHDhif71",
"19ETdL5no2cK5u7WJT8pc29X8hCswhyebr",
"15vY27DRoq8fwm3FsKRd6PjC3VQ66Gvxgq",
"1G1yDkFrgg3fDHB4Hi1hgHigKwumJ8D2eg",
"17pCaPf2RnkkfBBh84wq281NVUHD6oonBW",
"1JSdEAGK7b24NxFoPGWHTdzbzckFCFMAAh",
"1N5KJE85o3QrXXZZ5VKWVDxkPmKVy9zeN5",
"1PFbFsTL8j6ge7i8YGqMjWy9yya9pe3SRF",
"1Ps75Yyhuhrg4pSpRhX7GGdaHqe3bkVo6u",
"17fN3pjW3UV9o9MF7AQmaVTMQBcTcwRM55",
"17c3ugHT5kWEkyZhdFEE4z7jnD2xBvjwMp",
"1BtaHNyKZ9Au9hiM6wC9hTCndakVYvbnAn",
"1CCPTJQFdxMaNxjAN4jrVQgWqyAecHViFU",
"1JmNsZRVLMNiR5Sk7CnPSsUXdYL6F8zWYq",
"1RMgfujBnBH9ktJi5Cw1JyfPMQ9rpGWvS",
"16VZ8shtLAzQgthFichaJ3vzCtcLucEi8t",
"1LLD4mfiY5NpePu4KWRF9F41opP3hAWeAz",
"1WbTCQYXYsqFciuRp3wZZPwzNTUCQVZJL",
"15zdiGsF1Byj6AhhvveUWegQgxrz5GDKUm",
"1XLt4FthsRtKN6Gw77dhqKSSG496D1Qiz",
"1JpsqXc4jrAAwhdUorXq4Qw6qMa6rM3TKw",
"13RghqVH9Vq39YniQMkjrjYb7ndCyTW9PD",
"1Gp4qRsJ3Hp8Qs1QYdBZismcHPrd6MqMqY",
"1GXcRKCgyy9s2tfgzT3pfAhuTxqSf5RAWN",
"1A4om5XWhSQJtp3PeYFKQ4eeCatZRBeMBG",
"15qGAufmmQZX559SxtQyaVHYFvxSdZBjbb",
"1ArKS92ij7oLDf6epLhehjzPpWregTeDJw",
"167ik3mWsuGkUaYw2mADgs4yz7Lov13Cwh",
"1FFTEmRMcQAWYXNRwLXpR3Wkh48XRoARZj",
"1HQQ1Q2dHYoWMg77vsea7ja4NqQFjtw9pk",
"13HeJbBBXrNKSUmxADFXHD8LEX7yFN2tjE",
"1CALmj7BKRwXhjYsY3236B13VVR4SavMst",
"1AktJP145pZNTYFMHPZQjSrUwLiMXS7vmb",
"1F7qLxv7tCJA8zjTjJt8XnJip62LCQ1Zv8",
"13uEj4dzTucGTHou3cSinT8hM8Vjbv2ZNZ",
"14bXhKuRng2Sf81TF7FU3nUepRSv8iPP8w",
"1DYrRaqnSvNN1UpZFWV35CurL3EfyjyTCt",
"1L5Hk8373fyApz2pegtgq3N9PBuJ86WZ1m",
"12X8ivuQwHDFJfQ8Gzj3G146CtmwcUu1if",
"13cgDsKnPPe7Xrt93M3XHB2BFKr77mnTHQ",
"16JNYepizY8h3pqZ88uKerXR7ang48QieG",
"1eVGNudGWCSB7ExvHDeziBzwfCmTxSgpT",
"13D9GJZwN22vUtnSVUbc5JEQj5t7UXyFxm",
"122wMS2z4YRNcyb8QCzVbSdn8Nx7aZyTxi",
"1KXq52qfXKYdqpbN4vxzFXhoN5i7QNuKwu",
"1LHVQutfsF4rNeJxMNvv9WAHQQGG7dyMB4",
"17tfFwTW8KCrifUFhcGrG8FgLawi4wZqpf",
"12XXpqSdVEoXs5cdNnUtzGrd8nwenLprKd",
"192W2TnMvj6vXc1AWkYzxSzbQdVXxWbfqk",
"1K1AS8ecxyZiFhRgHDMRxQg14MuW1sCX9E",
"16RcYUdfBect25DF6SsFFkct2NywM9D58n",
"1Ag4dTosKCATeZjsWEuAWcMfsdAmxo78Kn",
"1J3wQdAnJHUEGMFKkVDSq2HJat3hFQnFUT",
"1PF29zGmCpxNy85ZyLgu1JsKv7GvW6mJFe",
"16nGH26fHFWkAWyVrcaBHMWS84V9b4TjKn",
"1DuWdZ8wSn4eUgiiy2741ZB2eDdzGXxrN6",
"13mL8ZCSmzcqZNTGcdT8fBwFeVNtTm1N7H",
"1KVcGg8uToY9qYp8NhVHoAmJTZrSndETsc",
"1Q93CgQPouzSXsHpXoGPtpdg9jrvv2VW5N",
"1ALRwEVmCBzwQAP5YYwGeB6YvE4CQn6rgE",
"1XXYT1xdr3JDeDRancnRRkGDPWVrAMJvH",
"1ETUsfahVDbz6ek2eXfbGWQSZmqakkJrNv",
"16qKYRcwo2ReFsVrZp9fCqj88yHvss7LwE",
"12AZzVqvJt7cBKtbHFCB9WgayDpfMhTU9A",
"1QJ4sN1KSt7dmCogjerMGjupfapX6hGPoG",
"1Eitp1F6nsEph7JeAhZPJZRcq2myFa8H7B",
"1GKDdY9JSR76U3iGf1k79rndXQE3kgJZmU",
"1J8ewk74YSDXhMjQuo4a4QvQgDyKQcRHia",
"1MLPpKdoWPFCvv7HREN9XuEGr85hAwR4jJ",
"1GWJLPHiCjGfrt8w7RQGkuHmSh3dFvMKwx",
"1KmgeRrN4E2k5oYXAddPevViyQ56fHLBmR",
"176fNY4G3NPrN8Wk2eZDMYYFFLZt3XWgnv",
"1MYefwij4s2zFuDqCF6vukbJ4eomJEskTz",
"1Menfxtv5pGf3kR98yrCa5FBWpo1iAagvN",
"19mZaFqzMNW3tDN9TLz7cWzqGx1zPMfDSF",
"1E5f7N38iDP439iCri38m3FCCtaEMBBcWL",
"16X7Lw69axbjhMTJ8n8p418GF5YDWh7GPC",
"1rmcnYdGxnqi4fd8SttHUFkbRMqSVnmbD",
"1EYJNVCjzkZGQdFaRMbY5c2nsRLU8m5BAZ",
"189PepfKzLh5TKAVPE5Jrnvoe1Fd4DPduR",
"1Phi9Z1NgrxQwqSx8F1qcp6vcj5382mWq2",
"1Czh5jPAANuZWmNxwPcKeXCpiGLCson2Kn",
"14gt7GCmEcu63SbCS721VrrmeQ4yQcTAS5",
"1MMajEsGBgcZi4aqYLa7kTthgv7JCYf7e8",
"1Pubqhz6X3U5AA4byms95UoP6fG7LsbsV9",
"1KyxPJdgueWC9xwxKTe8bdGn9nA2Y83CrP",
"1CiwbHFFzL1jMoGizqoJXhFYTv7GNDTmxU",
"1AJjDzBG9Roh6d7GS6K4TRJaLfHF9Vfmbd",
"1797hyjd2FeswfcKCDRaVvHZxvKmtgbxwu",
"1Dy2SZ9hNmEHMwQ1hYDy8GvFbGLUbV8a2Y",
"1K3C5QBhDWju42S7Qv2tqpuoVRP1jnAb2Y",
"1KJR9W5pkWaxak7kV2xGvvBGezVGPhNgmn",
"1Fyzrd1wgkc9gLD7vtQbQeNq5uabReRhLX",
"1PJ5evsFoDTx3LPKSn5PVJ2MrczeGLtEBo",
"1NX9ieC6oS5U1xxTbce1REJDHDPXbHmXJz",
"1NiyYfDLoKp7SkczGm2RD8KqrhFwLa5a6S",
"15Bjq3KTLSkYzj437sL8jGh4G2tctXLHyP",
"1KTHDAYsVYUA6XEUqBmCumZCj3uAdEGNnu",
"1MoRkXiqGYBfL5ZDprEGbyW5f7bMcQCfTj",
"1PRjoA16bbEeS4giHQCr9zj24k1nj2Ub68",
"1PWW5CqNS7xghpaf7RzDFTxa6WqGCVdFta",
"1EkckvQKZH9ftLX4fi8hKLHBjw2S9Xc3J8",
"1Fsyx7gdAqAd4qCTSqTNyg6oT8cRYKa7Do",
"1A5tG2kBqfmYaA6PAG8V71RV4WD8FibRu6",
"1KbM8Lc2ERQGxS8ZhyuGPbmtBwZpnHXsVs",
"1Ehd25XHJhjGJyDZ73zsE66LsUxVgoGYYN",
"1GRbGCWsmttdFF3SQrj5yyxwFTvPy6uFkN",
"16qxMqeCJgAQDLefNSVFUwFHBaohCKgknh",
"1LbsSLhpgCyTzCfSvvxqE7pK9WLxXTKLMR",
"1qEsVTcVHrT8tN5n5jBsBnJZVWgr3rzJV",
"1534fqaZ5AfHytqMM6HUJokwtrV5KoXA2C",
"189gnPekYoPZHRiGNF81Xj7i19nmCMtfef",
"19JJpdFoTZdJDhUmcQ4FErGJBaAanme3WK",
"18jD9735iVpY8SN7bKF2CCuf4HySGg185M",
"1PYyB8xQdaJGgmwuLzMX8NeowD1kg8Na4g",
"1MkzGKL8Cd8bAAEfUYAAczyxnGKoeh8JRN",
"121GakkCpufPv25TremBX5oUvmUHJSiQEM",
"19HMcdYhR8ijTmUqtVor6kT8RTTcGpF3GJ",
"1ELBYx34sj2LTrF9XwBXrJ8roH7isMQqhA",
"18jr1pr6eeG68kTzKKvj3F8e8VekcYFQti",
"1JviYC7qxZDoWMzs9mS9RZTTtAyvwq5zDv",
"1CAcuXWAeAe3TYQDhrCoV7wLfKQTP32Di6",
"12ERdRqffJSsqTWgPT3aEcnnmzrss6xssk",
"15gQMNhFyCWWu6NJ9YRnaezq58uwnu8o3U",
"1MJmibSUM2tDJ5q1FzBZgMBiJy6pxHKrH6",
"1CcpKjhrFE3q7hFocBg44DwnyxBrHfHjjQ",
"1Jtt6o924ySUFEmKfK2peDro38Q8UqA6fZ",
"1KsHYAkJnXdjJftUx6j9RpjJBd1FR6f7rB",
"194tUB5GMKYVWxSapJGBfBkgRftPBY5V2N",
"15DF8dWDYtE6xQJDRUhC35jAAuBdoTV2oD",
"18x8W54FFEEimKDM42LtofF9tqxrJiDmb3",
"1P8atzq8kch9QKxDxhUKPoxkQrbrjkANB7",
"15bGL5G4frPWWSoW1kYUALM6bWEkyxHY3d",
"1FmggvZcouv8KEqdKpKA4BNShFuK5R88DE",
"1QLMcvtPyzrWVPx2mxiVSPDWMk5G1vrcbU",
"1MQTUdD8ibALB87BhPAq8M5you2ND3DpvA",
"15koGbiSLBTivyKUcQ1i8XWvV8caVAqosJ",
"17vT6fowc96B5bog6gnF8PpE2ZakAq74NG",
"1DAU14cjWT4Feu9XutTHmm55xE1RLvA4iu",
"19XVjRLdR7mSwVK2GY3UoinDmEFYchBYxG",
"15mq4sL3fg3yjQ1iyscWTBugzR1Bvrm75K",
"146Ew7PEZkSy1d7JFmDZ3WdPyGx933DPU6",
"152JuKa6kmtzypLpRvqaK1M5HYJLf177ZG",
"18Yr6eGz3S6zBQjhfncUEgB8pGozPJZ4Zs",
"12oxcuL4qkuREH3YHhqV5ZZE32STQZK5rQ",
"1ftYAucWNbzTxQqP17bvNZgcRDTtGaCKs",
"1AJ8MbyPNE63d7d9XpbNhND4qXaEYQumtz",
"1K2Fms9Dk7dipnFxgdnFHkvbsrh9yHjDvo",
"1FHqeYJkaZZzbRndqCKZUNkyD4cyzdBAc2",
"1FBb8pnKpPuC3Mx5sUEcxvxKj8QKfJy8Yp",
"17NVah5hGtSPiXZBDvVzP3k3Z44EvenGLj",
"1DWWGcwj64x3kXJtBKn8MAygSAkeCD2eT5",
"1NgQSEwMBFVoE66k9SYq3E259w8T2avjvu",
"1Czx8t97peUNEJ5KKyhaAsTLwgqEHyp5Ap",
"1DNrwhdCc6CjoMDft8CVJ8P9Mhv5JW9vZ7",
"1NmBXKQEW9obdiZVvxFD8FByrRuy4vrWG",
"18dRakomJzy9tzbeW3f57yHS4Lk7PeVbRJ",
"17wBgT6DmK6z4bj96dYGzecAdqTERmzTqA",
"1LrR2bKeeDXuCJRUmhYD3bg6EXareGbJ26",
"1tynSW8kUhuE22yYVjp5caQ9MrbmDSP4c",
"1Jz8PMMs1xEUKuKVYHNWahKwcZgcixvR6p",
"1LAf92kVbr7s9zr8qTDRAcm5X314uC97C",
"151AcEfD1eKaqcHkwbZnnfQaqPC6Uwdoaa",
"16nNQauxCx3N4amp28haACnGQr3XUdgFwt",
"1FUt7LUou1eXkRzZhscLL1qxprDutr1gw4",
"1KosHmhMLjFxaq2j67T1ZeSVAQD17UrUyc",
"1PS7NPNMF8xRArhUrFd2Wdrt4z19iJnSCy",
"1LT9iwKWuuVBJRzBFbAKLFg1R1JmtKe7QK",
"1PtLtFTcCFU6ZihwGF6izve6FCsXw8LkKk",
"152u6N1EJW3cE1y4ayf9mmkJNyrny5FGye",
"15QFSptXn5VL3ymnGEwKWHe1HyMhd6uCjg",
"1ECdW2fHYX9cY7fmJNuqdGbo7pNvZdA6RA",
"1BPPCwcoXu8aBLxM4Aw4RttLmFgPZQASGf",
"13iiLmYvB46k1FBSwZ4UjRZEwGXj2rUjFF",
"1H7BjL2tpLkMPRYVZ9aqHcryookrVfzRQV",
"1HeUcQhJ1iV9MkQqikfJCBD9arrrHLtaB8",
"1MsJ7AT3Nc85fMXGYm8TDtiMn88LUsV3ah",
"1HMgT2FPxoM8yTFnxT9TPmJTqJiXVzUj4F",
"1GF2GetWCDiZZniNptsaCtM64ggj4SxBJA",
"1E4NWUr5ph7Ux8QLS6xYUJDdeV9GHvqZ8Y",
"14ALJMS62qrCSC3tNhPtiR2Lt7rzvPH1hb",
"1KJyFXjzFtP5U4m7gemeSy6ShcazVaT6Ku",
"1LBp7v6xheYYYWsXdYUG2nVAXKVvBHdFKM",
"1CgfzkYW9vLntWBn9d7JwJWtYYtzZdyeBi",
"1NKqZ3VjUBrD1s5C3tw65YXRwmGT4JxArs",
"1MmoGfZ11WanahKnWmrEvXUFfJcFE3fmba",
"1MjEZveEfF7mPrvW3rhBiC1cfei96Mcqh",
"155Yq7qS1Jx8wEWtrDHFrVyckxqAzUPaZ3",
"1M1ewak4aK2jqAyX644BGquj8ss8XxuKwa",
"1BrnnHtjosWuhb3JPgh3AKZUyUBtimW4Mj",
"1NTcjJun6y8ykivCETdSFpWkeFnSdqPCUN",
"1LEp6KouFhJCQRNvnncYTkKEHLaTLA7LgC",
"1AcaecgAkGeJNyRkC8CgiH9rVpFyDdQcfx",
"1Aj4kXoFKAXy6ck3Voy43LKjZEVZ8A5oWK",
"1NpqFVH4Wp4JdrhaTpk2AZP5YVU9FsLG4T",
"132A72SAb2TwjvUy7P8pcLbQnyYnw7uXQq",
"1LGceReoqViUdjTr97CL5LPhaBwbTQsK3s",
"122oENXJbGMxzKZzLMZnyeBSnYeiqEhroq",
"1uvNuSpVnp3wv43U3Z6JjYKnfiLKonPqf",
"148KXZvpNYunmmeKWQdke7SeKLPuLHBEYv",
"1GknYcYbVthmzPEvkg3V5YLWkq93Cezh1g",
"1MHZBs2cv15aidA5QWHJKQ4i179oHk7Uo3",
"1Jm4fSYdmfGecHnHsg53EP9o4EazxKNunr",
"1LqebMRbXzLBBd5ou8FJ1WEQxJNhwoqyLU",
"1HDVL4aR9pCi7sALCYaPg672YxLydYRZeG",
"1KGe5TPJ6Thw2Uuh3eMKWymBF22n7bdwQ8",
"1GgVNRhr2eDeuSRfiMNYqxEoWQhd7A3ycs",
"1Lm9D8bDCgJYbDtA4tXBsw6ceMYdCgJoQm",
"15e9v844YhZ4K38rMNWwU6C715u82AssXX",
"164xTFcqkAGHJFopDBfkZmGffo3uswjzUS",
"1m5BsMVr6gFaGsKxKaoMZcQiWmFoChsJe",
"1CQ9kAXBHtNz6hbC5FzyGcKi6k6r12js4y",
"1PC9hmqgMpooBD9sfzKgwd83GbAmoraRdA",
"1L5JncZYmjH6jpB2LeyruqFhmJ72CtEea5",
"1LG2yBix2GMrs11oPX7cuEcT9dJ7wj8s78",
"1HWqc9zWxXijppumALfU3QLRmkziEMbZKb",
"1cdu8GHVNGnJ8QyPLSebJop21977PkRCa",
"174PPyNpg8HbpkanezG3XpghyJ6TSg2BKj",
"1yxH9NnWdQ1mn6qfMUk3jzdHApGrRYsEa",
"15cfb7tRgBQt8Sb6FJdGGb8q8sVC81xcTx",
"1DzXw4F4PTZ5XSYGmb68MPA8i5DnaYNbkd",
"1EkRy1pHTQGXXcwtrwXEti59cZBagdGewC",
"1AnJAt1wWHZFMHYesKnfv3dmDptiU6xQeC",
"17tyTsox8zkZzSoU9NdfykEzSADRz3swF7",
"1DiT44pRzJVzTJLKhXY7Pje6w7KWMCTJcY",
"14H9ccXoWxajy8RdzRivhP1oJQDWm4WmTJ",
"1JVoHhfKorfb1Vx2ueHaTKrJM6k8t92kBN",
"1B2EQpg3beb25AMXwYMYYb3NLeAtHr9hXb",
"155eT586h8Ugfz1WJkKJyWeMNNJxyVMoLK",
"1AAekNUbXxudDEVT6P2ZhHwkdX2B6fj2cd",
"1ASmARRHtwtAfyuSGbHJGnPWmah6Ass4BU",
"1CbpvST57jDppLufgfMGvk9FCXUWWsikQ8",
"1PaT8KU2fc3gvT73vNGVcE3hq6cxYseisg",
"1GZ7qktVmhPGnmLciJwCc5UGdNwxenB3Yj",
"1PKEM74z4y2Vhq5orZpr18kuGfMDTQxE4f",
"19foEBndu8diM97fgpnijDphW7SAXcmfmy",
"13qjs4JAUwb2c1q1wGiBjkTFPbDKbmMBWy",
"13J7ao6X6oh91RwXzCdRXzAnyxpKsJEwFt",
"1NYJfcwKukn9DypspWgasMWr5VnoiLCqK7",
"1Dd9ixjibLw6T3jtCRrtLewpFB3dUTjs1w",
"13ovp3DpryVwGkbrW1V9oxSAaRXe6dEc8j",
"143wRE3tVf9uvPid1ka3ws9isHHbkcLRaQ",
"17h8vBsaT62XycJnBzn5yqKsfUe8FJnHaE",
"16Jt7wYD5aP7AtDDY28K4935Yd15P2yGx5",
"1DCfHedbmXxQKqiU94HnJUKaqDZuzJHedt",
"1B6bcQEqneCqVjTeCWemY1K2yrP9oqmrZC",
"16FHyk4YmAXjUSfnSEpPy23anUiBkuqTgT",
"1K5TLSKHetCNyg9mHNaAz5QCQ5PLAZkLxP",
"1CxiLuBFb9BSRjsi1sWBmqBjzpdUkum7D7",
"17ThbFi3piMHVr12nyCQvjhjQmGdK97RE2",
"1GyDnSgdSLWMT5GynnqQB5xcqjPcSfCLTR",
"1DxZCgfdfSH1A1n4ocoCQnpasoFCMmoYgP",
"14BeQgAp9NvCEUn4p5fFVD3rLYo4XHb2LY",
"15iiNzJkRksPqVcNN6rNrnTHUieBAFcpER",
"1KT3UAgJTUNoiRhkZV3iFa6PCfxQfdGao3",
"1GBKnqunHigRPfTFGmxMk8SfTLmAvwobQ9",
"19ibKUpEd1sKw8xXhF9tdHqQfHQXmqGbhi",
"1VgZKoD3oxX6oLocuubzbShiwc6Sx9CRL",
"1862yCS5PX9wBUafWFTorAuRHr9iDLJzF3",
"1LFzpkDoFYYnmawpGzj5zw24XcTsiAxH6A",
"18dU7S2n5FcA4UN4xmN293XAeGgq2S9qCf",
"1GbwGaoGgsGUzpWqjwGFqwvvoiAkGY2cW4",
"12NMcELXJZvZDqbbL3FnvTGpi7nWqowTzP",
"141rAPpskFureDCbuDUxhLcKDT3JNQzYAt",
"1GyqV6MpHMWwNqoFU49WxSTtp37XLqCK9m",
"1HNVp2gyZ6XEVTWRz8FDDu8GfnvqHNTUtW",
"18g1swFibECL1UNGJJVykQqGipR4csECFt",
"1Pw6mwTSmDktBjb6eNMmn3LPYaWGsVsxFK",
"1EgrN3or5uRjnnedgUprRwUnNueW85TWPD",
"1Crfduhk2pKiqbfLBuBt8xxMyD7YhJTKKr",
"1QKfWkZfhuUxRK2FZeXbC7ovfao6FJedck",
"1Jv7re4WwtBi1nA3zEerCGh6yqNGThB6P2",
"18DeNkbGY5zi3fXGe5JyqByHrQ54uBdhgs",
"197iKF6cLf4haMPpGKxo1rnoQXuNc7YSiL",
"1LfAH9ZpNKhvaxTSRnnY5vWZxR9dcmq9bV",
"1Hqs6mfhEohuT29ktkk3evoJMM9tpcnVuB",
"1Jd696Mz2fGQiMKqVKwPEAuzLnhC5LMaGL",
"1Lhb7pSuMc5Az8Gp8xHR27wzcbE4hMTx7t",
"1GoARwJ8Uj2ZXKagbAX7EMNbStyW9FqBUF",
"1Gs6Nr8h1P87ihce4aZ2GhDmeS7RLaFLek",
"17mDo5VgwgqtXxDaurnz5bPoSSLVdBRGsA",
"14avHdQ4JrSEzrrJApyCeaE62LsektmNve",
"1mHvm6r4LdM5nMzcQkNGeXSp3QDcmrCge",
"1P2ex32eZhySHMzfAYPgiGTDnRsRoUD29T",
"12YZzJ5xGjsCbUy59hTFuJBw9mzDDx4ozJ",
"132UbPiTm5yejghyJEcjV7uJhYjnnwRSEH",
"13fjZaLCbWJ4TeaGJhnrC2pHyqkJ6TJFSL",
"13PaPbbncYsLPsdkyDRsx9uvaU3wPWPaHA",
"12AHF98ZUxg7pD95szA4svF8EGnoEc4JTC",
"1LCLH1KvcGYe6dRMoA6CeAfsqSuMtviLiY",
"1EYDX7XLkxtY7zCpQMgJpinYFykchQ3FuQ",
"1J71VE34ejMfDpnNZWovg2fizLfHMkd5PV",
"1MdWWgR3GJWcvoXpeX4eHBnABBijDBwUwN",
"19Niubf71Uh92vkThm34HLhqcj4c7di3KS",
"1LXFaayJGrM9wqyyfqriJD2qsS7BUBWxKQ",
"1GvxwFknuRwgAMFvpnJFJjAMPrLnHctaoN",
"1MuwZHoTYoeGByfmHgMujTdzfKJxpH7LYr",
"1K15m2UKiF2o2eL47tEt5PkfwfsYTs5Yuw",
"15Xawp61M7eYuMVt87NCJiGs81SCDV3kT4",
"1GfbLzNZEmZhiXrYL924xuWi8Cumh4aEz4",
"14xaScgxfWczb5MaaGhK645WchFfVLfT4D",
"1CiBqPSftEdNeCW8W1XqiPobXpkYNZAdi7",
"12zudt62ziGydFVAsrPcZthfdFM1iBwTdN",
"1NcP7gAFGGgw8xJzjSpUJsM6Ni5TW7ekow",
"1HA196kTHgU1NzCa4LS2bSBtuoyPJt2Dg2",
"19uu8ejpYHWAwLSUJGU6a5AYA8DmHofNEN",
"12EV7T4sGdfxfufT1BfZphQH5fP74NcVHt",
"1XsjHcAdnFkfjg8M6RcH4QQSvHoNL9Lao",
"1NuUHMtpznQVPgdhY68HR5NVGGzaNDJXRs",
"1AMmX9vR1JKCGpFQ5NNwJ8K4CiWepPRpnR",
"1Eu2tYjzsvrsuxWBLmW4mnVPGFfYw4ytk6",
"13dA5WN4V9oCRF8njqLiYNQkidyEpm7ajb",
"1H9FSNKsawDH5PkADPzSZt3RiTtsHqFCpi",
"1EHCEKcNDt4kxUTdRdRFgXiRrbZx2rgzKM",
"1Bv4iwcsH1HjkJS91EV3wmMRQV8WCbeH9w",
"1HpUQSbHYPHCF8FhPatw32raDSjqyFKx28",
"19tm3UCswRuXSkuJuNERifC5phbX2buBZF",
"1P7PRox4advdFmXy5k92t8oP5ufb4DsMvG",
"1LZ19LasUiDBSc3dGz5ha35hrw4Qpb8kc2",
"1FtfsGFsX97zYzdG3j2QE6JSr5zkd2sojQ",
"1ZQideF2vZ9BPf84Fcx6QPyUpjuxJUFQx",
"1LqqV1yqhzWtsnUY6o44HHP7oH5naG7K26",
"1EhCmPen3PUMCnfP5KHeuH5hLuU4BQepgi",
"1JddX4KpRH8gFpBBo12UF9EgDDVDqdYiYv",
"1CJCpPd75hnpxS6QqJFsaPnHQmjM1bDYPY",
"1BqVDytvLdLkVRRrpa7LyS7rBT3LqyZ37F",
"18dwnVrVzKvr4uvxAMUu3Z28WmjcXyktmH",
"1K6Yk7cmVVL3nqkss7sWf9GhkbGfALTPBg",
"15JbPDJbEp3n1kAPaoMbPDcqrkgWicE5mG",
"1PPEmGH35bHcRJ7UXimyfhceuQhtgDtCRN",
"1DCoNwgGHLzLuXX64vkzAEKjeiTgoUJgy1",
"1C9Bb58ZasoCSRJauPqJCmKkuQK111axpN",
"1DWUKKUsz6L9S8SpHMRGxRjPtKTdRymJEi",
"12deg4ZjNmSHdiNGjBv7eNXASJC4rFC5F5",
"1FiYBB9HLz31AnY839mLq2DavPe8tZuHZe",
"1MvReN52GanuLEnH6LqcHG1qe5A55SmxGC",
"1KzBPAwoU1z1hGEwJgU3YqKRiUu55paW9h",
"12vLeMKRLttDt5waV7ZQkSkoQb7W3nMUxQ",
"1JJbQJT3YdKAQWh1yuQNttNQuwpL3QgYTq",
"1NxqNsnjwrvrhMvrFmJWPHCrthUpKEtTZB",
"1LcwQtRiAqzbwEDmZDYdYq2ZTSvEBoXctt",
"1FdfL4D11gbh2YNFf6pRHKKAxxpJKT2ks6",
"179VMmiu4RbyGFNFK2ZxN3QpMLpSz9A9Lv",
"112mwbscQT7dNMUveisxujuLkgvg52bVyS",
"169DcMk3HeGvTC9vJzx15yArvxpRxCnXNi",
"1KNpVjVNdiEjaQVUmqHXr6GADsSNYrw9b5",
"18qaobrshiUHU7hiCz28HMNJu4qhSuP54D",
"17cgeARZxnedKWiEWPxM8syFuUKffjbQuY",
"1PMKgVbNUgSYXMpBE92hVei4jDm5EUYHed",
"1MGgkYEzYjwQ12HMwVekCL2Vd1teYHEAT8",
"1MQm9GExvQzzN1u3PeYKPwTbfLV29Rm79c",
"19jqwFvKWoaqnvMb2DHZc3RHYgzFgw8jTE",
"1PKrYZeQ1SHbcGRoJWz1HL2QZYbAPo3TK8",
"184FQFjoUvhMasC5jMEfcTDVNrChAMCMoj",
"1Gtg4LZCMAY82TQEm7W7nmtGBdqnvcfVqb",
"1MZzucgXMDjsxDfET2NPTL17ZkAQHZDfmw",
"1Gx51Dxb5aco5v34SeEbNGnifUi8DYRLfY",
"1F5BFtTD87tKM7KuzPAfSVR58MPZ1MghYH",
"1EtjoC5DV1vgfEjHKVYUnTxK8irBz6vN8S",
"18UFhWNJPqFMFoXRZofdfWwE6ZGjjSBEXv",
"1Efk3CPYb9j7sE5BGY2oGEdu2HX4jKWQNm",
"1LFuCVffwXmF1wphxXNBaJeYsEt7bbYCuK",
"1LwERtgN49LSeYbES3MbQFo7M4tRMS6C4J",
"1Figmy5vaXN97kW53kE9zXdygnt6Et6Z5z",
"1ADa595YodkH4HyiVhK3a9wxYcbm6UdQrc",
"1C4iCBWADEM6M7vi35C1rGeKZnMtx9vnfv",
"16gWpvbLoS6byf3po6Hrf1SkyjFEQBrLFM",
"19GR52xAYCHvMbTqZY27tTjopjEX8r9xiX",
"1BJrkGWVSb3A8eVWnZFtsk8jUuusWTTKeo",
"1DvifkABkNhVxnQWJtcTgMbG4FJ4y8bsxs",
"16HLooGGWptiAERFNjWHc8Mdx3k18riD2v",
"1DkeReH4MtHjB68fY7fF93Vw8SveAqB3np",
"1GopfQ2US6XEEU8e6rTSUPExBYpHB2RFd6",
"12pMsr6inguG6whxe3Q3eQ4NZHfDceeE6S",
"1EdeSomt7FMgi6DPxEeMg8u5rkPGzA92iF",
"1KLer1iyQZeYFr9fwTSzCURMbAtxYuTnmN",
"1MFc9W7MNb4rR596UQgDHTK6UpJuQEXCW7",
"1PhjmSwNN2h3N7YjaX1h61rLKdu2YcALMG",
"1JyKeQEqagV7Q5iKM9TbSXqFCRo2phh2uX",
"1DYPe5xkrz9kB4xF39VZoHaYEhWWEKwZ5o",
"17EhVSjx3JAo3M2tHJyKx76u6CyhgL17P8",
"1JFpocS6uYKddFeSaxNYVTA963cjTtS2m6",
"1LJ39vUGizqDb7vvNrpCVkfBGDL6trZ1pL",
"141HNH4CEx8Aemg49K4e7HK7UJFzHGPjaS",
"1EZBv1tqJAhBUPsvveNYY4qY7yzA78HUt1",
"17BqVzpqgLynDdeemLno14PU77PcCKqc3f",
"1BjYJj7i8Kf1HEnFdgCzND4GjGKvS17ZAR",
"1FPi6YPPen7vRuh2w8qWru1uRXJzS1foUr",
"15jiiciNN8ygiFFRtr8zQiSYUyaZPh8FXy",
"1HGjdhLYAbTqQYJrJVSbUT3gsALuS9ZNy5",
"17zYj1SAVzM5Yb5d3EvhG5KHXQXZ81QjJQ",
"1LRf3raoRqKq2mWEZVS3KLSi4EdkBu6X27",
"1LEENz2rrrUrDB4AF4Dfcyw6TiZqN2yK5Y",
"18wQkTCPqft7CKiuy3TRyY5U5uGuFekJSQ",
"1Cedh9S14zAy2veERrrehDqfmY2z2DzHj6",
"1EwSDZUJhVyA1heo6zrcggYcvXGKgi9pYo",
"1M8qQicjDqmH8zvR3ga69rvy1ox26ws9se",
"1HqXnmdzQ5QQhZxTB8VWRJ7DdkQMSCco65",
"12pN8ncQWD8oMTZnzoqrMtYEUbYF7GqNK6",
"1JWptYKEZfkWMuoFrAky3sQKAsnfv8iUEx",
"15BBdKECgvp5BTDDpujgH7dToGXPNqnaA5",
"1NQk1t8aWZZHrCMjdgh4BqmUzSX3x6bDUx",
"1NZTHTfqt1itfpRsEAV4Pz9y3Q9u5vEVni",
"1EGxvVkyP6AbVmQduaxn2BooeFsHP54LWV",
"1GrE33YnLtCHEA45NH4bJXzgPULuyBgqhJ",
"17Bs7vx7C8q73YsJc6kazYmpvBNazv3nQB",
"143dxAsfgpVhnnM7FdwZmA3MaA3FY6AYdr",
"18551s9WzuRvizMVHLUsSfGsezh91Q7SHT",
"1H94XLSYPiiceJVqH29AuWkX6tZZULoMWE",
"1H3FEmLo5NFLBFKGzt38KAM4y1pxBXcnUu",
"13YioBaJN3HBEhEWY9HiCGSHBh6tFgt4iP",
"19is6oh3uKmyasGGA2SrwZzYS3asqL1vad",
"17qymFFQzyqHserRtCzsCbctMp7Ee4wzm2",
"19CtnURqJvKFeaRJN4anK9Li8trtDE2G72",
"17PWd9C22uyfsFKUnqfRp6mVKsaci26qKE",
"19U1oWWiwbkCjhMz1UbeniUx37jBpw1PnP",
"18XDju8wSDTbXr2y1hWHYrWkp4sbUkmgXn",
"1EC8iFCXcVGevm3TahA1s814RpLwmc3MsY",
"13ophKVxGJR9StB6zuuamHqegkA8yCEhC2",
"19rCmN98QPzwvMmpTXi5rDCWZgWHUQqGsJ",
"1MZrGPnmENcSc4yChuSxQMG4FM5CQpcYcp",
"12KqrtLAAqKrvgLaUBC1ZH3LvBeet9mHE3",
"1N5S86t5WxtB4w3eazQHWc5XjW578RQ8NW",
"1E8cW5QYHkcW12g86TbhoJZd78t5HsQc1j",
"1DtzErrqHBDTRFLcJP8uNEXnR8TPXvmLRx",
"14Q4eAkJ1n5BE4Xs958YG7EtFJyYN1PeMi",
"17QUoWGDPa6ActwyTk7MwVdVMNf7iT4dbc",
"18Sd475nKxXh2tXWJVpibC6Q1K81NKHAw7",
"1M9RPJrjyBbhrDvRrbJVwoNoXXVCXp1DcQ",
"1HMXSHBmsuhEDTvWLT8ZNNgdivvoSvT3ZD",
"1Jcnot73E4qyzcWeLT8AJZdbA4qPEQ59EK",
"1AEeBrdbrw4o46Hmd3HLeRdzeR9D5itXpv",
"19xKJJRnZx1V6wpVH1Q5XbaxxZ67ZiH24c",
"1GWjdT1wtTDK7Yi7EkrRSsrNtNT4SwXQCf",
"171zSZLEfA1A57EuC8nh4Jj6ZS71t85Z5C",
"18WLmVU3BZsuV2pv2xACmZEQXkSbKHUWq2",
"1H6ST8vUr6m17653spqyJGVGP14QzMpSdU",
"12rUaqPfEDq6tXhx4otvS5tjHV9hGYfUkF",
"1EnGxJx6LDpYA2JGtcSmZSAvkMyaX8ohnt",
"18G66ur5qzPdYG34k1ZMFQguXyWu325dkY",
"173mhRRcddspPgyALnjiaPGvdTkhdB6WB9",
"19vE2uCMtK7pvwemy8ivno4P9MnrfmRoRG",
"1QG5AX5nNqPz9sMpnKvUwsRvWYE3BRaBcU",
"17U5qhW4x6XZ2qkUrpYMahcHoSvuohnznQ",
"1DbmdC93VfuKkERpyWxDpkBWkYzwTaBYf5",
"12h9nXumbSVYPikQQibbiHJ4TamG3xictM",
"1DwfR8J5aTRJhgDFuzaUavpA1pAqwju3xr",
"1CPPzAJejhUiz5iBB1NNVJ9cgxqYhiHkAU",
"17EsK8Tb3Mea5jLes2eA3GXvxv8UZrRBhi",
"1Q859pEV8cy2tmmRXZE2gmLrvTmTyWXxwW",
"1GSYzHqx1Cm27MGE2KnPcMMeZ4WVMW9HB7",
"1DUNjN8KwsS5Gjz1gpFbK4DzQFCN1EJkCU",
"1GF8jXyauTRDuLeyFD1T1sRHprsGG459eB",
"1JVbD5yiNxf3GLaiv54YNMArZK4XmKAWRX",
"1BYFE87Rguzhf3MAkk3zuZmZbWj48jYZoL",
"134EejXw91PmrEiPPaeE8dtaPDMRauCm58",
"15Sp3qDSHPBUJgM7P8G6DJNS3evfregCTJ",
"13iRLLJVBEabyYupUipCekAaSprfYPfQrN",
"1PAHXFiSKf2nwqNhL4oJH182bQMyMEDNtY",
"19aLSCYoEUzofVr7ueoXRA6LaW4wM9vws9",
"1Am4op4ckUy9quUE6SRq7Ah7vpmMNBqUxP",
"1FpXEjcjs5XqrMCfwsGDxN3jYm5apmyfoB",
"1NYRYdbVXbbi8Gsd5xYeSzthyUcQZt4L54",
"15k8urb7hA3uXUQ1ezjJcfd8xwM2Nv2AcU",
"1CLqCAkn5ozwQnCAZCJ5vqq14tQnGaCjhd",
"192xZxMEVoE4QQ79fY1nAn756WDCnBxA6L",
"19sycrjKR8foYBVj6rcK1k4cSvLzyKHXTz",
"18b4cu5cDnyKPvCpXgEoQ7aaddprKsLHu7",
"1CUHRAeKGWjAdGPnRr7GmjADoHEMvx6ktm",
"1xDSeCK6hiCqZ5i26ZSK73mqGxm27D3D8",
"1FHDewPWd5fcF7C7rXCmwnemzUA6mYvsaf",
"1J8w58jGFeLf1ARh9V1vUk85Bxn64SptLz",
"1KvfDStxkLooAqPACGPCpBSjPQ1q6Jhvd6",
"1PzB7D7B3PxxMLLCaKN5zkjyP2WetJx5jy",
"1CNtwDNSveSjFNJ2qrmwPj3xCXD3aGjXN7",
"1xcUjHcWoWGQxg9mZZt6FMzbwe2BY2427",
"1GQ143XhdCfARtDe5GCunQetdV6yGKad3o",
"1MbSmH5LGLZLjYN46NNmH5mWwrFQA9gLtg",
"1HPSjqe5XwX2AAjPN8TxspDNi7j8xgkDdG",
"1HZhL2SnKuig6DtWqm3gHjcKNHQ93SiT3x",
"1NRbKj7J422jEstbmf6bbNQfEAcfHM88EM",
"1ACxeLhX1WytDF7ayVpiSANc2Ju3c275qf",
"1Ph61Dh5YzBvWuGNqA4pZbbqmoch9bHcAm",
"19xUa5tXShCjx4rWd63xQ8q7GWGh2WtQPw",
"15EyRKm8fKUzUNUuANqrWaGHnJa4T6PZD1",
"1KP3z72ta3gNhaTS93W2sDRLtMP2DSeox9",
"1NDwQR1QCeVW5upuw2rXmXtQshPLEweqAh",
"1w2ksZpdp7s5VvXJA1yAMPRtV5WpohMwS",
"1PJa1WmeJcdjNvTdh7iVCmqzaTgtZZwmED",
"1G4t1t8C2zg2PcC61uyY1NV3S3uxdjR55Y",
"1Fpe6ZVU51ML2FLk71HkEKqYw2QxiY1Fsz",
"1GSwwHiW3KHBsKh12Crypdw1zY4KNRPBi7",
"1AnFRXkzJaRtJvTP2mEzAdTmp3YvjVMNuf",
"18vhKp7yKWVrmnz7jYkx2W2adAKpKoxdFQ",
"1QF42Me4TWiw2ErtkZvbet7Thmnuz5yr7Z",
"1Q78uicpjAfYt5JYnnsVKAF8zBQBAN22tY",
"1D1ddy3fhkeaRzHHTo1rqNZJaPXGfesitz",
"1AvArrBh2f4vydE1cp655bJjSjdAHmS8L1",
"1LKPgF6sWx2PTUkyAFrddqqyCzxz3i5oSk",
"1AcaLE734KEqyAdaziRFv9Em3mC7Ur87nk",
"15keNKfMfvwjX2dR1jJutKKvPT9PGsDcYP",
"1EuKY61rwdM1N63LozbdJMcybYb9YmznR6",
"1QJK8BskDGT55gbKncAmtkHuqvKfj3WkZb",
"1FjR5ujL8aM6bFGuXppdCqbeL7UhTfiw1L",
"1KrPrBspqSCpJSQ98yP6DrAeLoaTZp2M8S",
"1BTqzbessczpo7JM6k7C5antArfoKG5Dhz",
"1P34HTM9TpomCnHCcUodm88iSiNcv8F5v8",
"171vAgJHqKvSwi5NGWLJxhom8NupZ53hBW",
"19WmtHtT7ERDjHYiwk2qdqTV81fQapckd1",
"1BeYa7RwwPiimDpsLzREWrn69SKtzaknc4",
"1MkqTYrtB2cTvMPDyryK7aeRywLEozzNmB",
"1JhrV3B5eCdXxRPVJmJyyXghbr148bpib8",
"1L4Z7qDH1diL8FYwRFNVe4swE3ER9sZona",
"13YdW2R5U5aZa2SAXVuV8pFbaMSAubrtRq",
"1KZf9Gw7SqFMwSvFb5qRMXKueV7akxN72R",
"1Cbp77YqH4GJqxTXsUQnJnofPU7BC1KJpX",
"16hZbxriwp7SpS1E3fKqbmbg2YVnVHm4WU",
"128uYSWoTN2eF6t86WZnXTbQAfTUmhHUw7",
"1HcyoJVBGgVa38oQz9Cm8Q8NMtHcSWiXUw",
"1B1AY4cBfA4ymQew2TBt7yACY3CsM2xVud",
"1A9eaCF7TmcaKdo4Dua5auHXmKbsk75fLH",
"1D62AikLxtUNHTxPAhZ3irzaomRVPy8wmn",
"1NFWeSXZgce9K59YbLBRFhWmxfHeFi8bwL",
"13zLuBszeYXZof3frp95k7tLH1biKhf9e4",
"1Mxh44SxSgAuBr1vLhkPPMKBqP881W45dC",
"15qvf3gxZfYJx4Mn4whoNCNV3CCLuKedeT",
"13ubRsz1MwNPW4myNWKCF83VShhSABpTws",
"113ya2hPtuPXkiivkC5xuFWy9QTbtXF63r",
"1LiNNFJxST3G1WUcU9TRk2XG9onwpJECwr",
"13Fk6SGqfRicDYgeQw9NZo2gurBRmK9BSR",
"1umaJ8cRC9tJ4xGn67UbRcT9KqCnCZA5Y",
"1PLFVUT1ntPjxZ6zJs6zgaiXpPLzPw8x4j",
"18RUmGpMvkY7md7oUkg7Ab3tuRpJQPfnLx",
"1DG2Lm2HzACu2igin37MRsFo91qYtw1baZ",
"1H8En8ZqeiDuyaK114e13P5bcktfJ4PUC3",
"1GSX4YDD5SmDvD86M7RotTbvurLGJXwQeh",
"1JpzN9dmFwzEgJrrkTkPZBkVERz9gdP7xj",
"1GK6hTV8LGY5H2AsB3UncFJD5CJT2NypsJ",
"1HKi1GR86HK2rANKfEZgjA4T2HaaVgdsyp",
"19ZLRytzCRrkNo4LcAW5nzK1eUudUEjcWR",
"1Cn5beXTx9tLsrBdQNpYBSrKF7Ypju54Ru",
"1FDTDUqhkWfx6FTzsZityFMVZvtnvjc2iN",
"13Q1GBsRLHnNcm5YLRcB63Yd9jokyC26Wa",
"1GU3ScEEGP9RMQKGf5M5o1eSE5uJff2sEM",
"1PjAgDPyhaj74HrZgoWUvVGZ27BaP96cxN",
"1A8XyqMRo86NhhmxLQ7pq1WvYeGapie3d3",
"164cniJvD8ji75kQhSy91aqge8251cy9o4",
"16ozmsxF2kN463n9NiQiTYPV9JrvH3nazQ",
"1PE1iEvc11Niuf362Ac6zwuNEL4ZmEn39W",
"14jzLkcia1Zq13qHBKzs5BmSA88rABzfSy",
"1DjVTumfdEWVQvcFrNP6Pqq5A1Hj5w7iL4",
"1HjMwN71YaLkNZvxMNFMTLUtELVQozZDKr",
"1PHTZf491WFsVNNrwZ2g1VmagW1d4WNYV8",
"1FQd5AmU3WK3TMr7MJ5YZ2pkvJXP8QaUTh",
"1HQ6hdMoDr1naXWfoYAFijeCE5TouTSuBq",
"1nYXabe6UbtJayEsriJvHuDPSD5e71NNy",
"13BpWuTUwMRo5YAt9MhDyF7DJeaLLe7yHJ",
"1PXxrUJR9k7xDYv1o8maofX1GnXqtYbSnC",
"16xzY2CmZgVkLGcwbD7pvhW3Z8Th3e6NLR",
"18dPzmMecoiY8x5UjGaC8oY2Gqa9t6rW21",
"1FoLjuExwiC6T1PekJDckbYevbGH96R8uG",
"1LnSQKxR5ebUU8hsZJV4Ja9oy4A9vqKtN8",
"1G2r2RGih8UTnrTkWijSD7xBypZzAgUvY9",
"1DLyAsY4ZaQJmVqvMBdQJ6iGFj9UhVG1UN",
"1EPFfsw33tBYmFsWuXJPyWLtkv4QEfXeL3",
"1D8f3DrgrfX1A7dDdhYToLT3FteU943D33",
"1DwByuP7CHDPXvPcYtKvuANjZKLitqz1nF",
"1H8zDif7y1ywoAZwVmTdowYqSWyaowfQZ5",
"1BeoqupyDgf8FV7oNyS4dA7kEFm2HVRuqB",
"1MLrJxtn1itJMD6ddAnEs1MHiXrCPRtZCr",
"1K9CZap8zZGxsvkRn8KFDSkYuW7WRDPnK4",
"1EnNSydecUCbDKrgXMGRSwCS9qkn5PqxHR",
"1NNms1q2ZtHP4cvcbsDPq5stFmF8rzbZNd",
"143hUCoubo736uTsBhadb8v9HkpZrhVx7i",
"1HyPnuJFw8yS1GPent2yyZU3iJVqeVrdjd",
"1JKySRgQC3Hsj23b37vMSTJMSY2bwBFcUp",
"1JGcH5jgHorDjBjUAptAyf4oAkTG5y1EPT",
"12AjE6FX1qCvH3aUmDHWhDoR1MWZeqm4Hj",
"1L8toCJQPjwnuCfwkHxFwq4b5R1CZMojvf",
"1BtzadoejxtVwXf4ZKm5qWkFu42T38Ctqo",
"1CmDgWortSGuXs5jyBLaLFbFUzfUS5WvWs",
"1BunXpGZsUMbaDdQ1PLCr7sNW2ej6EkhK4",
"1MG4J59qYmvaTsY3AV5xHTY7yJkMfkXe5s",
"16rH1dp8REN2nHwzMifpqixcBznAiMKQWr",
"18QQHJLp3S2eiNSCmMmDV8FCvMCdFWNTby",
"1NH5ey8ZrsPDE2ymTRJYMnqZv5MnBMwnAA",
"1GJaJXcf7GwavsTqtJy7gqmphbMy9cDp7a",
"1H7tCWNiDxbnzhyudMgssMWeAFAijM5ZBY",
"1KjNtcXa4hJ9JMsip447eEH9ELoYLzQDkY",
"1HBqH7V6ExZGXWhhDXf7DgjNsZJc7e3QX9",
"1FrUuLXPEUq4s6kzhDpLzeJJSwfPF6gX4A",
"1DAJMmWH4tNvEu6gS6ciGGC7toK97NL5UP",
"1LGRmVXcZyP2YiANaJxWFf5QFJ38BF9aNp",
"1NyaZMQcq4SCmNR4F9a5657hhqtrRXkbRN",
"1CzQNjQpaWy8HZeSJaydGxWhRJE8uuD5tu",
"17MpYKxsSDXaaRqTALwLVLjZpiwnp48dnp",
"19e8swWfGByi6rFFSReGaKSttoxZJk3aNK",
"1PPzJZqkQppkTGiwqbWqaXQrezoBwNbmHR",
"14SHqFspkgR5zWYtqnh7JkuKJwpFXTj3PY",
"1KBq1MZKftGqGnm58qD9YfhXya6uDinKWx",
"1KfkXmQsjRFN9vZjduXSskVwZod4vNDRWV",
"1Aiqa8CU8SPaT4e37keF5pVUWh4Zd3gqRy",
"15gXKzBCHoMjU7fPysPEHDrqCKA5r7hosz",
"16PgRhg6hg6X7RTPYtkkuCbUv5YYKuHidJ",
"1JoxTyktfTbrc2YhPCHH5BcCbMzonFzSfC",
"1ML3X25gCnSYTdGkCGNTNKTzmdP9akLYpX",
"1B63qEc3gusDiBMtnrW8LMTNFixdcTkoPH",
"12mgqjqNqXr4iSVeYXKBaszLvRvjbPwq1u",
"1NMktPJ28NjWSAZoxZV7RQi6pqSYs6hq1x",
"16yJ9YZLAm5ZxJZrwq3Uww2UtxDuHhxATt",
"14QGs2pPbT8ct9fE96pujiYFhYJTV8xeo3",
"17xwzbFqs4FQzYFPR6hZ8dk4WCKcPjbi8Y",
"1H711dVr37KFZEUD55fPpyDekBxVfzbAc6",
"158n7DAgDmrxE62urj93x63dnzQHtshyDE",
"1H41Lx6cLzDuXQzaei7dh7x45PHCcrnr6x",
"17g1QnT6Hvmftb2ZBnTJnPrDrbJinEjeS7",
"14sSyWrECa9T5B8GbjnMheeZD2gjQQwy4M",
"1JUoVFVcw75iYjpZqeV568sDigvtVM87EH",
"1AMZqp6Aj6myq3FmKN9MfC2EwDkYmus6Wm",
"1G81SuStMrdJBVBgrDDkd6iaZH7We19Azq",
"1Ei3suDK8pEzW5tBdPRb7rZG7Sj6pFvknZ",
"1BPXGWgoVgYdk66qborVEQnDfsMETfPJtX",
"1G2gFudBvjtUoVkxdQhTG7gfwmNh2UtJqx",
"1yWJufzUiDjCGghccdF5Djs7wRhkBcHNm",
"1CjJf3dKbh8DtTS4sRpjXphE7Km8AtHM9J",
"1D3166Z55x1bgQKvk7YTzrR2d3gSyC5m5z",
"1LjtYhWTp5A5Ns5BQZjHshsHBD55DmrG1s",
"18J7WsYdznGj4HtESx3vHjDFwa8JMrRedN",
"12VWBNVF1g7j9pmzpLCMrC1FeBBDehjFTW",
"1Mr9CMV6wWCCizff2mYWG7VWMGAbjuPQae",
"17ihH2YkxhzuYHSN2iKdZw2dCCZ5CLurAi",
"19HWfp1SQrpgN5sjZB1qdbVh7Un6Jjn1HN",
"14vrUrVjuYHxSgeqhofTYDSyFNd4vhfm1G",
"1GajNCDLqha5GMKz4kiyDAyeST1S9iWoum",
"14CZDgSfNAPCXaQuvKP7UpfoHVfAddyPRC",
"1AhFz2MdvYe95XUFcdU6xFcKzsv4LYPJSU",
"18XWBJbekEz2QVMnm1XWB6e9ALj4abdnTU",
"1N6cQPnoqWurb1p3bkB4C4qkpfbLTZekDR",
"1H1mjcXVGk7uNDLCzMVzmeHKcEspF9ZWjZ",
"1PEZ6p48MnAaFrWeNZuWUZcBnfPyTnz7rd",
"1n8aLUV4u6VSdVKZUhYHgSBo17CP5uD27",
"186MkuvWPSyVT5hLNvhcz4XtMFPisE8JT5",
"1Hv4ioH193kefp9GHFagiV9MWQhBtqKKT8",
"1PjFMrjjacSmJBz7vhp47ikUC2AXp5gGHQ",
"15GuebarQQiPg8f4kZCb6qFZs6N6BYEoR8",
"17MK5HyqdV99mTRqxRz92aB8g24Uk7dbnx",
"1GkYG3SjNwcLnUZtBoZuZgteCNfgCqVzrB",
"1LYKrzqM5kXvKxMEnKqLmfpWg4LR89Tfc4",
"1GA8GkAxSto1WqRwejjGeAwA8eq8eeMnqp",
"1KoLeZ4Xb9DrpTHZT4ydZCYHiJ3nMbjcFF",
"1E2YqaWLdiUoSUG8rf7mLYh1bKqPpTv4kQ",
"156F4BadtKHQZqoZ3RYaLyshe2hbut19mk",
"1JVtPVEhBtdHEXLNmd2WQSNxR2SfsvWyer",
"1Q5CorDkePFyuBWq3a7JZqgqLsmn9fTmcd",
"1AKiSG2BvZqAFUedjJjUR13YkgKrpBPryW",
"1NnDhyAprfcBBo4BeYrTDTuyvyYohFPwkq",
"1Fb4GFWToxHXEzQahFUaBUCjDUrXXBawb5",
"1KvjKNofN1vWXvn3PDs7XfLJ45qSb5KNEq",
"187uEsVnrmRCNDqB8dcKNTDh5hwG5gZyve",
"1PuQUGC3r8LtRumPAUAMS1b5XUM5XkX3ec",
"1Q9AosHC5ZpTthrKLtDvZwMdKTmA5Q6eAh",
"1NR1hRR7rHE4mRRZe3DQgr9vbhBo6op4Rq",
"14YXk1JS66A13MwoxiKgSwBDVMCGxcFGx7",
"17YUhuaHhy842TL9NdtpydCMbGjBF2YJhe",
"19EUDmqLYwaFzuy4D3M6cBB8Y3H7s3sukg",
"1J1wwP6kd7wzcwCHWnrqhxNxyUFGPP6ncG",
"14yijMqZjrECu7TihZq8EHqEHcWC53im6d",
"1DA7DpdjVDNNc9Na88Jf1uCpf7KWCM7axw",
"18fPLKjTgNBfNhiRDiatUfDcsuMxz2VNh7",
"1EzrbUq28EiWsNNgMfUAYbftoCeDCQZvw2",
"1BquMCiL7tQ911uXvMXD5dr5mqhEyTyTZR",
"1E24j1bgMeJQEjqSvwAXNJDqwNE5ykYNai",
"1QJ887tXBhogQ15ogZDFMQwGUMNuGDV7pG",
"1Gc92EdrxyVbNb5pKPPUry5LJhTkhax88q",
"1JskJParmxWpbNZx618GshoaR2QwswLyNd",
"1KthA8T39M2kUNCdGuztq4VzdfoNKNtsc8",
"1APw5234EtYMrasZnDyKN4v7RK5WkKKps4",
"15m7PuKGj4xRJHj178o768TQuWZnJa87oL",
"1LM8TzgqLPsgDvi28fFtKuPwwshiVh7Bk9",
"1DYf5WRwQ9SEbk8WBiw7bjrf6d59633DjD",
"1CKM9WhpKVAtvM4trufm3r2jsNNuzt3RUB",
"17KBeLUxLjMyd1TUE61gfWjegbnbYiGB8W",
"1DLB5ETwBYjQeWVwBf9FAWxRX1YXU8B8w6",
"1K2Ea7wjbqYB6fvfrPrhmag7BpNNwnX7zg",
"1GQjTPReq55XSD2TDb2UA2gQ61EUVH3aor",
"121DSX4SX2PwxCZrYRTHMYtsTnwCMVFhG3",
"1B8AAPVxN3f74ptFShLf3RmnAfm5czgreg",
"1L2d6GQmoRU4DZTtmqgDW1vH2Rmrv5k56x",
"1Eg7ffPsv4pUjetX4BbSi6tNnogheYdJTf",
"1FxBcpwyKzufXbL6sYt3wxCxyXEkWJDMRD",
"14uaN1GbiJTYGFxLHVerzSrWB8V3vKubBe",
"15869xANQAHABUPH2MhgxBTL5q5WoMFJaA",
"16dUoqGDTPNWLcja2DYyJRatWd1WxD1pSx",
"16qNkZpaygTqDEaN2Z9m3TBmae128KWAKB",
"1LuzXeFTESdBSJpr8JgguSqYhrcWHeaZj5",
"1GEiz29DmsPRWirQWCKK7VwA2juj76gh9B",
"16sasStPaMdiHWZBoU3fR3QyvjDTWdJgVo",
"19sAwGUf399Dc9gLxjEzyTUtE8eu3VuqJN",
"17LQka9AWUi3RKyZ9K1WeQNakGjVEhNG4z",
"1DdVnfHH48aPp3KfE23ajDhZSP9trkFES6",
"15vksV58CfxFGhhi1AbZM8VmLjHhkWQBpc",
"16mxmCrocfhg7qB5vxx47XZDupdNYD2Wg2",
"1G1tgq1Udh2ea97fSa23yFQGC3cb2yKkmi",
"1DHfcnytwJHz86i3c4ednHo2pwxTfYk213",
"1PS9tyoHCS8agbKTzJVsBfTqb6LwrGvb5K",
"14PJQxt6s85vxvMRkYFtqzQR568Kbz688u",
"1Hhwg4rGjt19fAvxHWTnzPxFw4vUzYjPnn",
"1DBw6HgNHLJbX5sJCgXs5jRWV6NTipLyop",
"13CafH7TqBtBhBEW6jD9Y82EQbppJCUdEU",
"1Jx9c4zY5tuFgw6nrhpQ5XQup8Bkzpweyk",
"1EbzTGAtYFbtvyGt48JHhY5EHvrQVRbTLi",
"1JNhreUYpYfSecUAdDqiYrVvoJsRci1xh4",
"12iYmNnXmnt5pqZ22TaCg2tpMR2ZFnT3Gz",
"1BafZ4pQKqDZ79AxDxdj3CoqvvmzRZcF4T",
"1N3y6iYZGcfzvgpDAyx3yNByFFejGM7P89",
"1Fei4YTFhUw9NmtG2WqVtjne7y4DggJh4g",
"1F6GzrLLPrwwXYVWLtsn6BpvFVfU9kzdGE",
"1GneTftmq1UNu2v2GPqS5MtRuba1wcjqdE",
"1GPXSjuLFGpS5QPAtm9wJmCCRrcCZgnmyQ",
"1Azz1jobxfFxTD5ph6Yjv3Bzf19aabYymW",
"1KtJWJ8eGJ8MNyYJzebtjYEBQat9wvmX7q",
"1LFMdnbsPnthvRVAqur5ysN9GXs249Mr55",
"1BTDHdcdNeLsHXAHMiVXe26RH9HRdsz6PF",
"1DnPo6FeAZTYJGMri73NYH7p8K8yzwLTzX",
"19vXah1kWrz2rqXpAcUfLkNRSaaDBKPy13",
"1Ga45aqUQA5QdmZ4t98UEWHFnueqfFpfh7",
"14JEZGkuCMY1DAH1MA8ALTgbHwgXv4gS1d",
"12VVHH4tF3w2N8r2WkjfA7k6qm3LCqbtLW",
"1ELpJujHmEQRE4AcNQF7MNEMMiPWxme6wa",
"1NsWDF87J3tofUXv3ibX96LgUnXW3nSYFL",
"18PSX4e2WJy6fG7qFNeu5ecwZpUeLLh6iB",
"1KVoCYaJKjBagnAhYDcXJSUhqq2VaPfjSC",
"1FXAFfQ4PRpkVrNcdgo6UE6FpnEk43D8nU",
"16cPX1VZf3q66vTKQmuZfjiWydjW2Sn6wf",
"1RgNLR8b3UgpmZs5HLKLDeKnK9gK39sbQ",
"1BcEr9eSrVBS3bs62CtNjBgdhNFuS5bw2a",
"17XL6kSAp3iDMackndyHBwVwqDWMCGHhFZ",
"15Gh6na2bZAnDbr1w7Y7XW6ePt44P6Q1Wi",
"1LYLnRDjMW9qCBC1sgsbhV43VDuyMSnNrW",
"1KsmPLfvdgXoCb1Q3DeySwLFSFqoftiuwe",
"184cVm13zzwA14sogGZtDxoya9NBp4s9ZG",
"1CEMRPAaim7KQTeFpo44UFuKk18EuGjsVr",
"1KvwGP9u1pMdJntci6NRnhtbBY85cLz7n6",
"1FVpRXrQpbj9q7L4TRgspNoBNp7UT3Xp84",
"12tPevYeRPkqMRrZD5awUmQG1TF2iabMcL",
"1KEtzLSYEbUSRyvRZLBFrehatF95LpH3tz",
"1KDeso7EU3qKwACbo7Xc9WjWmymqUAf56P",
"1KpZEDnbykK7A9d5gym51xSZ3xLnMWjPzQ",
"1P43QgXtfYShrw5eLBJTdkaoytexv9f8gh",
"1F7CqSpC2edWW9Gvw4vK9rgYuVNv7xrbHJ",
"1MraZcjab1k6fc8GcVEFACwcPbPJ1AY2W7",
"18tYkCbGJtmCj2u9cXiuABMBEFnK4kPEzi",
"1AkiqMQVSk1U9izk6JMrKASjdaK8HFdeYt",
"1CcQXTLE7Z4oDJ8eNecbUy2LphaAPdAAes",
"138N7wGfuzMTaLSyYRtjgcS6vWioGyMfms",
"1PasaVZ7ryiTLRYVCStU4ry6ryhc5CJosu",
"1FvqqPvsUrH8MDtzZFK8hxtf38KVmqTkKW",
"1JZYsduMeRNs5snDZp2kJ5cjNZQHxocapk",
"18FT8XwFwABTHktwgnMxoeYDc4WMF1NNyS",
"1KBYfi6n41LeVgsRYt8WDZKaMeF7VN5q2d",
"16onCXMR1VyxVCsa5GQFDX8aLCLzP5tcRt",
"1LV9qpE7qKM46wT7c17YYJskwueUEkGDTV",
"1VHPudB61zi8Mz8scJee4J7nHXaA8aq6G",
"1Dqsiki65rB1oB4eBc2qYy8LKfbo39pv3p",
"1MSkyqo3F65nQgb8BJMiJrWsrMrMQBSavM",
"1CaSzoqCgRHod72q54GpZAKRvdU3LeCAnk",
"1DvsxrQyrDVyD1redWMU9vg5JXczFrkN2p",
"1MAE7gEMKohs97qXDdj9xgSmZj39Qugwf",
"16aaguGZccarnFL3V4aACyEiHjtTqN7BYq",
"1PwJ5BdgHc2NQsVevGnNwMFxUH5ppri2Dh",
"1DoDdKCDbW7fjkrHotGBSj1njb4UDhZEsN",
"156GgLyoe1DqrPqh9Q2cVv4Yw9WE3PZt6q",
"1C21EafR97P6zuQjHLh3aPkHCvWLjAtEa6",
"1F4kGk75mdzVu69xm3aCqeA5v7TfokrZ4D",
"1BXuB2L4gycZkguMpyRypPvkjkyLf5mVs6",
"1KXinRjf4buGA74scivau6g8MoUCweNe9p",
"1BsQdtMvMo62rqTFLCd3TWtuBtubp2CVKB",
"1CRULNvc7NeBckV9bt6VK1oCeSPHMpixPG",
"1NvuurXhYeqra7rjs9ez5grE2A38c6eVx3",
"1NAGsurDfz4bLJLXTabFoBpHJCcomKaDfE",
"1PYTx7FNdWWMX9bhE268cBTLGW3br4u6CB",
"1HcrNzdXKKyUejtZDs6ZPxcfWwo9iFr1Ks",
"187bJuLq5cYcGgkn431sBBWGdE6j8qiQ56",
"1KYwDT234moaAufW7To7q5WBq4Hk7pUGg2",
"1KQsMqbP7CMnirWCU928Qt5hfbVu956p9Q",
"1DKHvsDJEuZrV8iSMAd1QLJvXSEp1zFmxD",
"1GdWZjvju4qMqVKjcr8y9Bcm6f62yUEkjD",
"13QtKhWUAC5tHBcD2sTd62mQ2fgM6AwuUd",
"1P9pTXop3vAijAh8TRYFJnppbeEAPYzi63",
"1NMriqZfEYTcQjkNAQdBgixPHZ96yoKF5d",
"1N9f4WpmPqdm8oRiDQB9PmEcVcsoET4oMd",
"1YwPzQ1robQL9toX12G5eYCziPitoVdYR",
"1GCJrNSBhhmPprjQRZVxjQb4LnYsqCG5xu",
"1J28VUa9TTjKGqEkKqJVSQc7WKUGxU4uQt",
"1MvRoy4apQa8JNedFpxVFpgYxCPmyTGHAe",
"1AMGrFUnE3nViv9X4P9viNHMxz3aXfJMN5",
"17TAc6P7rC6TTjjLsXeU14VirxWMYbGrbM",
"1ChpLfivqJrVKquSbCufDxp2nYijuZ7Pn6",
"1EbZwMZ5aSdqF2KdMy5h3vXzBxZ9SMmN9P",
"17zy7pkbbJLD9ikWVf2QACQbRSVC44xUzc",
"1DSnjpyEWb13RprE3Z3uFGxJYXKM3LJmTU",
"16DampsQSVFBvCVeEkF7aYzMsH5NKEMiez",
"14qZ6dVhTDq5PGvs5BhgijNLDcX6wzCGBf",
"19Nt8mbYiv8EZVnKy5cz8pQJsctZsH72Wh",
"1LU79JTFr493ivS5RrA7HASuARcGwFmEdu",
"1PzRVBaF4tTMV6Xi7bj9w6Z33uGdp5WxRU",
"13uWdLHmQa6SwTnPqXNfnDtD38KtYSaToi",
"1LRsfJJMrmRxMQA31kKyDX64xqycGDMkWY",
"16RroDyybzRjPQKitN6WHCuxtxDoJRs8tP",
"1MV8SnHjRKCC1EMCnLgeMiavu6UgQkE9qs",
"198vMiq7k5sqXbEfAtAYYU9cngytdQcogk",
"1EFaiQB2G9BgtDAWT1nr3RfRLKv9AMDqsX",
"1CrMnnBURcAC3DFqEPbKkTrWFnNM8W274n",
"1CSkDyvmxQ7r3wUgk9kzsAuQAr7ye929aW",
"1GLeBt3DAC15wpyTM67nYLsXHS5ZvpR5cy",
"16Dfz9SoVgb6bp9Apiy2CSwptVthjE7Buy",
"17aRdicaG4BPdJpsWR6U3g6wFYP2QW6Jc7",
"1K4YSdWxSCf4WoJXHRsBF9zk4hYHsMuppn",
"18rVs43JKMygTfNbUpov9xQrKrcmzfEmH6",
"19EPey9R8iDPN4oFKA7HtZsJWB5BiXGsvv",
"1BWEBASHMqTYaNTzjwmCwkAbAk2NSvWS1k",
"18qBaeCJ3jbDzvcVrqztMm3CwXAbQBmPVG",
"12Ac3hQvr6mvwp5g6pDDQR8spTLZMLzceZ",
"1G1TGMgKRobWCPxdL4gXDuPCThPjXmeyGr",
"1ByrhGC1i8owLhF2mLFnTdUoBN1yPufckU",
"14r2z63QneSLwMRkcUJeALFABhMfsRXPL1",
"12wU5UMwy5hwVjUwuzZ6jpuxmvpYLg2vJz",
"16wMZhn3rsxJ8nkHFdTP2AhNf1JumPczcD",
"1NnPeHk8JQJYtBzsVNQvtFV6Ri7ZaYGenR",
"1HjNzyjE2tFxQLuU7WQjohNPxSuUU8FGGb",
"1DMtdVSWRVoLPyX3Gt48EueXEHLeiio7Bj",
"14QTXLgEp3o728AdhPUB4oST5vdMBoPJK5",
"1CuVqxMSiLfT3UeWQfZddeWGysDNXjAmVB",
"17pXmTuMgqNPYCBwVJaqxCNE8noyvUDehS",
"1EiQsMwUo8Bvg5fNvyky2Y98jTREA85hFZ",
"14eA4VH7DqNpFxWgmS3Juyf9QuFScEb6hn",
"129pVDb1FJMKxmdWrevK1iyAXLzebDguWf",
"1BDMDRQoYxTq7YgZ5ytEdQdSyxtSZi3XLX",
"1E96wbqau3mKzJBahLspA12pGAJRrmhqAx",
"1USe3Q3WLwFzaGLzseyXZCsXWaqcHShrF",
"1GwDouMkudCfic4etyFhcENa7HFmbxNWyq",
"1LAAkKoDtUtTkNJfTfzob3LJ2eTZwWotqa",
"18DAUQw5myy7VqVc5oE6czWw2tSnBgVmQY",
"1L6eGdjrtAo37ktfFubEfdaUMAkWG2grFR",
"19MAEx3tuquu1rZW4dz42wxaKqvxwBB6Q9",
"1E1Q5bbQJoEcE6f2m97BkURNcJD3kPZFvM",
"1DBkfVMegzvkRydzKkjaqFW9btodUzqjUw",
"15185eYLrPHgH3vpsafwvYmfC1Eqe84LCX",
"1GSZiU95vj3JznEttkKbxtRTm9zdhT99bZ",
"1AtEVMaeqkJwaEjp35HFbR5gyT2X7bCEQ2",
"1xz4yMqnNxbHoEJczoJ9JD2roaWcW3Usc",
"1HsyUvEVekebBfvjswSnzufbheexV8gcsY",
"18F6iJMQhkE8HfzGHtLoCKqF1pGjn2mf2D",
"1AP1b8Cxm4osbRCsHMhmWwqkrf1ojRbufC",
"1LS6V3Z1rFwmHNYqjBgiske3K6WtuC8GHB",
"14nPY7NKUPDxSGcg3qhSJyk3BGW9zPBBgK",
"1LLgvrsqmHdHfxyZQRKDrah3RJqJfF9pEd",
"1AmD5Epnj439GTaWmR6rAvH1Nhs4GkqfUr",
"1C8YcQu12ichHYDeuuFy9KzMxQ3tvrAHCV",
"1DPpADPGzpvyaAq8HVVA7GcSwMBpaZw4KB",
"1MUDRYksW3HasDCbgrABWQW2GNctmtLU1U",
"1FtRh4SsgvGaMEwQCBGLiTqheq1gXppjLL",
"18Ao7QZsHTHDkxHYZwhLkGHm5PUeyhVdAc",
"1Jta2wy7B5hrinSkWyAck5FgwTXQ7EZBUD",
"15QgCjbiKyLXAfz6i1pVdg4iHwJ8gmzf1F",
"1B9jed5sZBSVCZeGh4REwpBAXqbnexbCUL",
"1MWb4Y661NRrPRSmBWCVy9HzDyipYpwG9e",
"12hCU92ydfCozFFVVuJ1NSz43M27hhoSCP",
"1CSWTnNrG8cRu4fAHffeWB4BTChpzSXNb9",
"1K7cmDSAhczsJi67LvNaC7DMovW6XME3u7",
"18LXAga1cj9V567JoPE8zYnBStQcD8zx2W",
"1KdYYjTdZoY694RM2qmNbAjxi1PERrfZSJ",
"1Nk6nyHAjfAiFFDY3NFQAv3sg28P6GhBMc",
"1LNzcG3Rf8ByNUsYMUyzvHUxtWB3bXj3GG",
"14zm1514K3XwuVYCfEE4wTDGXWCr8HjZVW",
"15XipWBD2Ftq7HqfGYuEqgyPEMKDeqfSM2",
"19KapYN5UnqDNsWsNwb3rQWowAbSHSngLh",
"15Ycu6M1Cz2fygE5G7AeVaZeuR8d2i9LBm",
"1Giw6NG4dA1dQRmmQg9NQQ7Du54GxYtVVZ",
"1BFjvrFnYn3pL6kJexoSh2YY9XCqBJSwx4",
"17zzJqVALwH1zJjMZFp7nJ5VMM4m9DdSnW",
"18gA4YhccUMry2Mp5dmzsV5LwShHRBp9Yh",
"1GP44gDERs4ZAkFJVEMHZaA31hXPmw21Jn",
"1JxakVFp4cb7ZpB7EjsKXNfNbo49BKAHji",
"17fSkkP3bqq1nCXWcWj277rrnbxW5nBJhr",
"17aFbRPDeXQA4AqNrJHGftQgxaGuzVqYG8",
"1NZ6AcXGXuXYPqjRS1B8p2aGrg3SqeJ3mp",
"17KueZ9fB9iSvAXcX5qqhhT4d1RHNGu7a8",
"19CNfct2w48SxYR9BfNEduT8dW6keaSUXh",
"1MV1z2AH3QZ2iMDAxJ9XGBtuN2p38vGpRf",
"14nKXHABwAhLyrEApzsyo9pAQu5XM29jZc",
"1FjFeVKo9MmnVVtwsDsXoZQqET4caC8oaT",
"1PZZjzCEZuWtQdb5eRedM4K3L4zZomYKru",
"1EAWBjReKGWAGD8whfQLEL1skes1rpg3Yb",
"1KiFFLxtdT7Cxp98XbDUS8u8X9KrV9fsPa",
"1Mne47we7JzuNf4NXMHTdVm1UMriUQYa2q",
"1HQZ3GV3scEfVKGn974haEncU5TLv77WNx",
"1GYBdvmx78W9VSChkzy7VQgv21ybjysBFW",
"1BZKM6E7km6qX8tFfY2d9q5frHUCA3Cgzq",
"1FxqhfEd8v23R3cmR2EwpYzMoDB1bdmkWg",
"1NeckLrnZZsNoSoXgf9mKHWpt9zLergzAf",
"1QLXe7noipTMgU2pT8qhfWqvNdoVCQfcB9",
"19ePxqpvEPrLYPTq3dcJJCau2vkAWm2CGa",
"1CVLU8sAmP2iBokA5cHjD9d7uhhDyUH1rw",
"1BcyUtrvcAywrr62XQnRsVzH8rGdTYAizz",
"19CLUyL37cXhTKYUvFYzC2WPVjRWk9DDJN",
"1HjrHJ92DUGke1W9hELPqJkGZzqUF6BK4P",
"1A7netm7wkQ84qaNvqhnVUDjskYNUQ9Vzy",
"14fWLGppPE8CArwW9SGCzfLuj4UtKuobvQ",
"13k21d99HR19U7pNKZrHFJn3KW5eN2QtYu",
"1JSdfHmoLQANfccXHSJ1Pz8A28NmAGnfkK",
"12ePf2gAa4AW5o2AwYjNjbNUm2c2Yw23ww",
"14QDGEaLgkBgAyyQaDzdwRkWVytm4TWGcP",
"1NnNmv9ca6jv4dsk65KbTADsQVt3BWN51",
"15YZLjYF3vrAFNvsgCuybUHv4HPwAZqGhv",
"1LpRu44qLfTgtUww3oVT6BMhpPVqPLZA2S",
"14urHBQfsQFZAYMJZhtQhATg18njLJkJiv",
"1Drr7vQF25udLC5s9b4uRxnZrLrT6jvUaP",
"1E9Z8eaXHDWDCXwjhhfsjdUYNJEUcvj3A2",
"1BdiszReukyxQEHMbSSup1bKkQM7LbnUpg",
"1PndkSje6RyPMjPQveyfyDQNNqMzj5Lhhe",
"14m5nzW9zczN4GDtVkiyjNipQVaotX49Ue",
"1Nf2MoMGjRFMxRzWNmRpY8JYWpMDsKXdNM",
"1DJABuA5YVvTeJ2EQt2ZT7CWkkjpQCyPGN",
"168VqcvBpAPUHVJvmt4RekqffojuevLd9e",
"14FDXZPkhbkSsyWYhnPRa6mNaJpxfY8cE8",
"12ad2GUMgHX8QBkMZgaeJpqhvB7Wt75GxZ",
"1JyAxndSfn6V3qr5FNbiT9NWM7yhdLo7bv",
"1KZyPKJ4YzKLViWxcnrp6mWnPLhPnpbbqQ",
"1N5cBpuDbJSJe4S6BBAzVamwQng9VfuCji",
"15CNinkCdpwKgMBy74HZGWJEnNXBmwgtH8",
"14f8r9vE4aEFrVgfdxoaX6QsBvLzeUNXdR",
"1PhHnr5PZ93RLpkK8JwqNvf8c975TJe2od",
"18TkahpSARrxH4Qn9y7LudbekiiHPAd4EX",
"14ZTbhDYFednzfKju8oK81p3wSW5hysJkF",
"19ibB3URxGTsP4cLCquLiTerP2BdpLJD7e",
"1GdFUZ8gPpzvGCcHYN9vM9b5ZZR9Dd2gXr",
"1JJ5cVGikk5K6GWDbFcK1uS9X9qXN8KjJL",
"18WvH1LsVNCxpit5MwovaWUNbPuP9gis54",
"1JGqdqMb2mjNogiHFwpGYXcgpsfzFHN2ma",
"13A5mq84j7vGMHBm61W6fCJYXaYTYDckx4",
"1FiVkJX2CrxEsYBdcNCgCYGniUbwyaRMQH",
"1HuNfp8v8qawEvJTxbsnosKMQrR8Kr6wD1",
"15hG1AzqBQ4ZhfbSspyTtYDtvMpRs16YtH",
"14VLJrx84nZ9K64VyCPbVELynAiBVEMXNk",
"15PQxsVEta1g34uG5TUS5qLRkaezukHvQS",
"1Wkhcsn1B9bHLTJmKnzMPbGSTAC9neQ5S",
"1JeaTkQk9tjjWgpBVmEFntcVeBC4ZFKNtJ",
"1MUtVAdmgRJaBMSJ3G3BsWL1eBaLnbCoVD",
"18CC3ApTmmLNUYeKdTH4tvno67mmAabEs6",
"1HPWcxgfPwZj7aa4EiArFm54ZBWg5wg3Vx",
"1KP6UfkT5kNHXrhwEtEc4kvAbDm3ykBxSK",
"1CbrDmkQLn79RPZbdFxY5mBW4mep6HXwSd",
"1DX1JYBgPpXvPaiiKYQgJPrmj24WpL2jqs",
"19ChgSCNQwykjvvbNF8M4nmh8N212xPvzp",
"19fr9qcoK2v1FxSzETUbbinuUgNsmQ9wQD",
"13bXqkTQptgADyZSXMydGM2PGQLMRBw92p",
"1F7aK67d4k9AKDX7DyuK1pFHPLhyLqjxe7",
"1N2CpTo4HJtnyMxftkDFXW1dosjo3Qckhm",
"1Cc7UdgwJTjmpWFSKkvCjaMXf1YEk2FSVL",
"1PMQTnPhcehMkpqCFMxnNmVXgYrkGAcBb9",
"17WxPJxXjPRzf9KZMTD2qUXbUvWmTFRJL1",
"1MAcV1HGNuxunrEpaxTVMHkqHgzBwcDYBC",
"19F2idveaphrETwgErSWyhsddjqRcRV3yv",
"1Q8375KzuqALFWEz3HWxA9RJZW5W2X9eJw",
"1Hn6yeH2N2i14hb6YqAN1SjCKnzma6VF3y",
"12T8ZwJ6Ys8Kv8Ao99Q2YdrwvChvKUjiYD",
"13BBPZkMwv3vy7kMUwGNPiTTR5vsgmq7jJ",
"1PCNpJGjmw1w1BAtxXvfkS8Tmv18WqLaA4",
"1NjbXKX1CnhqgBNiYAhyWFXVpMga88TsbE",
"1LVkV66tPkvvLon18WDHhCxXXZXNZfpq2V",
"1KVs31Pibti3BEiL8e9dVtnW1HsKtdZugf",
"1CD7M4z1HUyMdPJWV5Xczym7tW4q5vGvVD",
"175Yz7RSpRRmaEkb4H6aa3P68XZ7k9vix5",
"1KrCdQT7fRmre7Xt2wEPefXK7gWEucWK1Z",
"1NULEvGn2K62ZPuxWQDoL1S2RAxAQij4Ga",
"1E547fuZtdjBuZhJ3EXWLZLG2EEJ5ZS7YM",
"1Noc1oCrdB1bjBuvnNyJGBGuaE9Zvuxcpn",
"1JjrmpCvhnPCLygmZdRaLMG4JUAregmzEr",
"17D6JkQGi92qXu1NNysZfezGrvei2iqTs6",
"1BHiU2onBetMsqNMuEJbizXJhUuTEPo9C6",
"1J9BG1THbuThbWKHMQs69bnAb7VwNDuR6A",
"1LgDSreS3fWFKmHxbQ3tCcasQ69RPhYXSk",
"1CHDHZvPVtFB3STSNMpmfDxnRwwaRMdmkg",
"1PpMPkLRh7H3PWPWb2NioiuywhVV2HhnE8",
"1L1NLJn9T6kyJm7gb9vM6tLZRV9t1t2gas",
"174CXQseWweCfrrfjZ8JKo66tBXiVDwYyA",
"16NcADDEqugqgGp6Pay7H6rVkEWxZaioGY",
"1EWPRykRbLDBPdFoBovXWbwTFauSqTnopo",
"18WQFAU9N1vPeynbhWrh5z2kFfhrZgxNYJ",
"12B3KaUx3b2owJAynRwFozyNFsFcTsS1p4",
"1Da1xnZ88cEVDDkobhvRJtdcEJjriRHvt7",
"16sLAYCcCENWfCao7YTzktrSYvDrySVPbJ",
"18WmpNds87dPWa7U7xKt4VyBKDBx2srGXL",
"12NsPce9aDuEpARgPsxLZkfmhK7Cd3zUAb",
"1Jsb33wnyZnpi4VqoyTHGwuuHQ5hW4Nq36",
"1webRtzpVH9y1hyAQ91dfcAVeYYu1rTPb",
"1Ao4Fv2TMo3tVgBhuTfCqFr6ATgBUrwUqK",
"1JgXQLT6mis59gDF2Qm5vjfaeKHjTYqL12",
"129isgi2nW96nJkQvtiyYejkmUAUGFYQHJ",
"1AsdjUaUknYPpmaJ6najinfpjgpHzZ1zi8",
"12TadJez7xiGk7DGtyKsWMzsd98SjiLPZQ",
"1NyjKCYLnFB5mM1nLny4G9RwEEcwppr8Ci",
"163TWZgoyx7ZM6HzEo4KEJZVQc8BEcAqoq",
"17j1HS7r1CfZhghE5YhiDSCoK6GsKaGvS9",
"16oPZeBJ471dqE6Ff9znvkcYP7uNTrnqFS",
"1F5yVWaDMsFBxFJaBZNXmAc2dJo79WvVye",
"1AYTvSbahrpeKKbSikCGbx9rNRFubRUPwW",
"1DHoRB2NHWnZwBF8JWe5yBMXTGRXVtphKE",
"1K2PLg3WGbqKxsEpsaMfFcaqBArwPWj8aX",
"1EU5xoXj2ZzQhfexWbCrdYNECS8tccPEea",
"1BdPPQLNjQGQr2xUF1zAqygisZ3EVmCLhb",
"1Mdnw5qxbMg1DmD3cfHiY4UVz6mw5qfPtT",
"12uXHsdLY2ePfoA1hoWn5WfpdzfdVnV8Zd",
"15Jb9e5d4RyfqJa14Gzm6SU5wfJAw1TnXZ",
"17CHjWnTfhPwsdJyzWTC8fjh3FpKhuJppN",
"12QnW4ap7oYeXrUHNeca8NPB8p2KLAtnHY",
"1HDPPZ639pDvs4j3zrRU25JVjTk9M1PGoU",
"1J4PnAYJGYAK8egJVsg3MY2Esn5Q23dUKb",
"1zvZXKLtRLhy8hPetzURkWoHF8PBFp3Lh",
"1JqenvWdNWX3wQR9i4qsLMSGz5Ek9gPutC",
"1DpLJpMWi8YX1WESSxXLUANWg7335TcULn",
"123C9ef9cHimcgTTnqYQJ6xvGomSFeXqLQ",
"12JzRZUkwvmPfLAzWZXC2Jn4GZWid9uzJ9",
"1GzAYh6iUtTxQmVbYCPiv4b8RNSFJ9Dh6H",
"12W9JmmXW2topUjQfMcRnrjXbL6twutkcE",
"1JVXNUyp5KKhL5CdpQYjoGFQ8AMYKjoHjZ",
"1FBPGk1EuQ2R38tJJHwnYgsa3n6WCH9nr5",
"1L2WyZsGFzsy1odWTmHdd6PitTSwLc1ygD",
"19FM1RdBECc7Q5UacUMaRXMGgFRdu9ntHA",
"16wnK4bHV945UMJgGM7VmzRRXFq4TJNLVu",
"1KNpwdgWjqj6jXdeRXziXjgihrreY8gRC7",
"1DJuN8e7uFbrD82yegbuXGfrnaWBgYeu9z",
"1EuEkwsHursaTazTuapCnDXqH3n6whfKXm",
"1C2Lc2PcikkHanhHrYbGA18ipnUBhonh6y",
"1Hfj8U6SEEiSr5V45CnjmHXC9Z7eKiz3yY",
"1DWPsjhcX4ufbXj9iAa6veuYHGiLimvHSL",
"1N9C1e2Krevi1uWXF6iedbiwpjmvBR8dHL",
"1GNYHL165ko6pZLvSDBmTBwBYzY7VgHw2z",
"187Ja54MpybcdzHWXqVthfJQgK8dbF1ytH",
"13MroowrnHePbxTAALtc2fnyBngnCZa6vP",
"1JS2KSN28qPKbJ7xTXeMTA1wixWjkDnPbR",
"1BJXXAKv4AY8PMcnXULNp3MwWT2T739CRW",
"1FixDmVDZ5e1Yd5wBovMn6uLyAtT1QU55S",
"1PH7aPJDPjtfkuC5bAVyhJdL5ZWAe3H163",
"1CZUZXxKcGFKasgxaUoPmcwgZs3kaVVXHd",
"1Hn19DbTpQhCH9JDGKxALy6w8wuHmkj63D",
"1MBgvZ8Df7ENicJttkY77MfwD1HQJi7x5c",
"1KLaS6mMksx9LHeGQuDCGgQ9EpgQWgPTKG",
"1BZYSi74aNj51hyyVKT4M4bZYvrhwUMR6J",
"17NrWUJk538g5ifDGnpvyYQ9YfZ2F5gkx9",
"1ERizMjjm4jYPMFv8cTirScCk7dpT7YUpq",
"19BKTqGuaPLs4XYvgE5bPArkySE35HdrFr",
"1LxpWqEBUA6wU4DGZBS2LHXPUS1aFZm8b1",
"1HkqjVQTBSPNSveU6vfcTxp49XYzxQAysB",
"1BZ7xV6Dxxvp6vf1m6qVF8q78Yj1GQVa1T",
"16m3kWewiJLvTM9A99LhrWp6Wpjk4gEwaQ",
"1CXK2oG5PYGEnXTSxBWEKTxDNPUUb5bCxV",
"1MkPEjRjEtkw2p8ZEFx8Dg22dMLa99hhSz",
"1BXGXufH2tZkGZ1Lr6F7T4zXDbaPqrq1fF",
"18q5u4iZMau84Lt2iRHE6i3jmofo5NyEaa",
"17qaSLDKVed9FgD1ExFTxP6VJLqNWBdZGd",
"15CYgasKcTP4BTjSqYRuL6CHHZJwij8esJ",
"17KcQKpgJab2bqdjpPsk2GF7R4eAJfHV98",
"14gBHq3Ad1k5HVWpXpgincpZex4VVUqM5p",
"1FXp2J4Ctee5MJfWYcimzFK4fCDbe1PEqA",
"1AqrQSSWLRCuAYxhQaQ29aY1MDJDYc3jmX",
"1EdTyxCpUPVk82mVCb5kyhpStf71XYi1Vi",
"17WbVM8BKSDRsJR2bLtz6HwEKR27XzeCdF",
"16VpqhxF9CVPTEZ1hjfkdhi19oPwJHceKZ",
"154LTKHh7NyuYXyZXD2Mec1Vmg4ShMgChZ",
"1AMCvfNw6u2b3rFGc8daTaKmEUqKP157pQ",
"1KPqvXEwxbX4GsQLvr1hsgaaxawfA8u1oG",
"1KdUW61pJMaYQ7EPqYytZKWmzDgRjKeZ4Q",
"1PwFbgntUiWT1cRaKzUkjWCtFeiGz9wkVp",
"1KvaozH4CBRaqUch1NstRPvpLZVE2pwLU7",
"1HGrfbRiEWQRN8uP27whgKMDLPCQXstAvx",
"1KnS5qzkgiN1A4ZmWX33wTacphvMdPy4hi",
"1LQgesXt4Q4q83yFHEYVpSenDbicRstVxJ",
"18PH4m8Ju3Pw8wq8iHnj5hFrTRTwXMfe62",
"1PWiU5fVsVavUrsgJzZNno6VFUoC6T27No",
"16GgH8fJke2DweCwxNtLT9fs6v9dJuSXyc",
"19H9pEc49xZodqei18egUv7aN4ZSKL83Eb",
"17QSY447HLHMbDcnptBG7S7fEJF5khezQE",
"1PRe8trnn3LWdbDjzKByJ4aWRKCzEWjiTq",
"1CtbdwCCHMzYrp5RXSY5NEfeWyQBNnhjKR",
"16VCmdQ1EXAjUSGMyVLYAqidwjDoWTE84t",
"1ApzZykJxro61kcPwbRLTxQdf9PhXXcJME",
"15Z3EsJzQH65wsCnkrRU5FAfA6Nvm8dxnc",
"13VHKPG2x5qViBW7xqrPQHtqziTDjkcLZY",
"1BksT7kyQidufG4hw14CFp2ZsSwMTpbg84",
"15CWEfuZ9rdxaZgd937NZSTbKt2U8yygof",
"13LxLVK4gy73sV42LMS3GENU4nAme81YBr",
"19raXknZ53HNkw6fkCJhg2ZWXP7rB4ZrP6",
"1Ta2sRTRvs8mTVPSsZyL3jFaRMwWKQ9h2",
"1MMVxX4Ju56gtxgEVDYKk8J2pKhaWgndRN",
"1KpCzN1NKZaeFKVYP9vYLBZsofy1JoFYsb",
"1DKW5AyTa3ksnaZwFkqbxMNhbGVGwMtPdZ",
"17cbWp3teZ5WKXYTAidWCPF7YAjLEt147X",
"193JcgmzeYefneSEuLfaxRxuVYDAM7phhh",
"1EpiffyHZg45Zbq61NLdyRjSPeETMYrxTb",
"1LbZ1g5QmuAxBTKu1TJLiQBg9QerGsnFGB",
"1Ex51pYovFMyQ1fpYtTTPbyxoZCiY1P2AC",
"188thS9dEqS3iohdu9KxXDRhs1BUvRuEeE",
"1P41CT97y678tnNsBHZT8Pe9a5VP47vY1o",
"1Bjd7ZwHNPZU3vaQYp9EJFJifpfqDBeHcY",
"15XsgLJwZ49HfEmYThycDALR4ET7UxarUw",
"1Lcvbhp6M2ocgjBYxHJi4bs9VrVvea928L",
"1NLxZqEDv422HuNYSTCT8ieJxN51R1ndr8",
"1LJuFKPMDiNVPNZ2fezh6JyaTfrB98uf1C",
"183iZ3cyKLvKhB7pex8eF4Pyby1PejqXp2",
"1Kruo14Xsxp6ohBoBXmCmdd87M1csQ4yNy",
"1MVusrfTd47uZcJcvZHixzvi5EHSy1Hyr3",
"1Cc8VPVyTngUxJBtHTYSRH778DsQQLUfFz",
"16QqkoyhUseomjv5uvFnWw8do5kDFpNdZB",
"1DkDRtnWmUJsdp8udjwZDvp9u5qAEgqQwH",
"1FnXTHasFroj3zpBFRWjFHQyoMcmWjdPrE",
"13dHjm2ZP5aL2yUnKQgxuBsAwiT4FKsr5E",
"1AqGbbRBiRy42fyAjDh3kxEAUhaPNtVFUT",
"16wTkTKFxRdByCWsJSvHxdQYtHgSpaV9sJ",
"1pMifcZJKvFyY8fgnMiVCvF4jcKps4tj1",
"171sSQ6dg8bFsxWdDEcoh4E8mrBu6b6q8",
"1GBTCUd4vd3q9Vrfn9rn5e4Pup3kHnYQhz",
"12ZKRdzapC9GV9brqSardQWz1VfeTSHnfG",
"15k7C5F1Y7p9oXUiJ1PnVZ5QgK5LfFz5oR",
"1PAVA2V9L6DrYCgdsFLQHxXCobrCvh9gkk",
"1KFzrhRzgdCfLy6zsm2h64wuxmPfLftnT2",
"1DtCKLFyo3ER8zjCLikAEa8f4NSUepXtnd",
"1JciqCYXFkPTcm8rzxpH5KkCuZW4s2n1Mw",
"1KehA1x5kG7LdQFbMKXz5s1s1HPLfgpqYd",
"13WTH7FLr51AAKb7wsjaxpE9eMCAwsRFdL",
"1DCk26KAJ8LdeAssWgsrENe9DBPDW46zCp",
"1B9Ub33HMQJjgAKSUxaVbbrKcHyEghkmr6",
"14wcA2tnB6U4B5X15nBCSAVqwDDqqqioGX",
"1KeqYbb4KkGhZy7EXJ2vQ34Uq8cTDj1ATr",
"1PtBthMzQm5WMa4LaUKVL3fAQUYiRHS1Xy",
"1J8iCVy7ww6vWKmcnGbW9WxrtSktZLw9jV",
"1FHbbFYwXkmougRA7WR3zNuKCYbgskn58K",
"15WE4SFstqun2HZQy7isprBcJ4j6CBDHAt",
"1Q2ZXRya3uvSX9RmkHi83Xaho4Fda3CKDp",
"1EPLQ9WParYmnpCu8Ri6LLBYgh7EX755oE",
"1AViQvMe3pFiFg53cHWyyvMLFisMyZtBgb",
"12oz8BVk6JD9LPZWAeuebdPAbj2YrMyp87",
"1994uovSrfDdyQJdMLsKCFntSN48dk1fXy",
"136gnBUXseCr3DVV1uVhKHUYvCXn68Dopr",
"14F9xwSkt33PJyZstu5dgZrYzG1T9gowKc",
"13yGqDLAt31gNkbY9aeSJCH6w8SkPCic2p",
"1K9H6c9nZ5Tz9xUMvzXXJrH8hTHYXsJ9hD",
"13L5i3nbCNpxQYyUAi5rGYYkjtqa82xE1R",
"1NhoJ6R4PCNrXDNbkSrqdah2c2AbVF3Gms",
"1KgSztAVkkDBGQDk6KFhApA4TXTitXbmBT",
"1KNmTuQnvJuSwqyRmCkjeoFxzcc8EGaBQs",
"1D3XveXHxSeMSpKtdx8Mj5FosDgjPocZcB",
"1E3xrkecjp6nxyNQPKcyMWaSvoQaP2Do7S",
"1DxBgZwCovNdn6PYALCky1Dr2wTiRds9Y1",
"1P3CRCTZG7CkLRgNP8X3xxLterZbuDNEE9",
"1KwH61ngb5rbeFL4FDRLdzHABrR8JpqEAC",
"1P7PNV7ckPCQFZcTJdhqNBd6tu841uF1Cd",
"1M41gUj7gmyYcEShP1AwFECRYzG1aWTWQV",
"1MH9UB5vv5QzVoRBW7XGb8ttzh1A642SmL",
"1Az1C5MYzyWNnnE9aZ5tu2bgrpFiDKgBRT",
"1ErdZmdoaPy1CysWWrKJwvsdEbi9aMcBxV",
"15TvpmCHxvSWcVLWh8JYrNY2aa7D89CPhf",
"15VgkdMf83ovumBRyq91H8mBwZ5p54Sd19",
"1PZFALrMd4n93Vc3hSkLNCg5VZDQzU3oQJ",
"1GKCzJQyJ3wWp1WL4QLGXFpcEnys74j1gy",
"1FTFzrTXzfU3BQ4PRYCNbFf7Bd4fyYSnUy",
"1DF2rnnEvQXeoknLcnFfkHnBuHWYfqrEk5",
"133drGoPFsPzQX8AM8ShtrVY6UNwaZ5jQF",
"1NxBvzNTQUWo2Gd6N56nXoZnWJeAFfk9dQ",
"1DQ2axgFgtkjVBpCaB39QW5tiwZxbzjv54",
"1Dkbpe9QcETopeWmxa6KXjJuyMfThKX7Br",
"1Ez3HmZpTebo1zmFCF3kVA3LM64siN2erQ",
"1LB8AqT8in8ZXVUaLQGgeXv7nzRC11kMgU",
"1Cv4i4ji5ADvEQbJbrNkgnG1dERSikvQYi",
"1JgsZEUJ3TGiJeesrptbKmtfi2M5Zqfg6T",
"13vVZffx9Rrt6cAaDaCEUvFm4kZX8Bmvu8",
"1YxAaQ5TeVDdFB6LExkUbrCHUL5xM7Hyu",
"1HCXFmNzvEvukSVS8BidT65KCnnx9mYjaq",
"1CgTqeHkh4bXuQfA4FyX8UDgiHSptppbeN",
"15gQh916ac8gNVf2xpmQrRqVxqjyJendwG",
"1DJpdaX82FY5YyEsGtknumQWVZHsEkxrae",
"1FTZ5Z2B45RjZGtgYhHUNa35iuVMVt5fr5",
"15ZpZJ5vMhtWMchHXHqFZ5ZTd2a9p5Djka",
"16q31R58dVAmgmU1wVeZcQfDnGcRePafFV",
"1uQk4XqKvXubyUThMiD9aFcz558f1FJpY",
"1AuCvBqd4xzYjxS7Ppk9mNpDKHKsbriYsB",
"1HM8y2YHD82U9TLDiBUDtPDJFvfGnGKwKZ",
"1wecRK78P7oZYNFRhDaLUHc6zWRL6emTA",
"1FqrN9roRsmArze5N6KcxtpDFFQGXYef4C",
"1A43H2o8kHJqFuq43aiREtNin75P3sNNaF",
"16zurVermRVVDXkgWx3A1P6rWYHbimzqyM",
"1CVZH2ENQrs4G8iSy9V5DUJijbfCcjQThe",
"1MDovn4exM3Zskvwe3nyvfMrkYwSUTc7GJ",
"1Cajbmi23LVft3x4BicP6keoTZWcBiwgms",
"12YJCeZb1TDYjMEEkd25yv6piP1LrnwhNh",
"15aRj6mdjjYQAqptFPX5MVMZrWHYyKHHUj",
"1Lvk1pxjcXNXxVkrtAJnokXbuiit1Ht9Mw",
"13AKZAscfMDJDrwVM4z8M7uWL6bvHQCV76",
"1EWMXeRTJV4XFMgNEjy4c8nzpdGJZauFMt",
"1NHKbWFjJSupuSzFkTT7e3WKNfPBAdx2cS",
"1PBrnAn2jtVig53q174HkfbV1TCqkAfG8g",
"194c1Q1KXgksx6KdvYrBYNyGLp1GEfRwQw",
"1NetFAJ356MNFSAwHJNVkAJxrFRb94q1jM",
"1BJsUSRpm9eh2Avrsah7tfYqCiyx5GprZ8",
"19FHs1WwVsyPPWM3zrV9jVNKwuJwjVYeK9",
"1dERoW4HrJgHWFwt2ZtDVC1LdB6mFbe5n",
"1PLkmByJqM31Hg65YE9uQU1Pgp7pTq63zd",
"1Aoq4BmGqKRxHwhJsQpJFLWkjkMXZwpqhC",
"1J29MdGjFe28V4sdmdR6ERHKj999apw7Sq",
"1F7SZr9HeFzMrDYk94MR2tMqXVeWHMqa6F",
"1HpziNsMg7mep6FwhmUK1NZMXokA7CHWCf",
"129VYEtVpsnRP1B4JoLjFa1KQHkar2sBiP",
"1GfsfXqJHiB9JvfNVDVTThFwgJcije4dK6",
"15KVSBDkNsGPUJhWhbsiPdRaSiJyXxVQMH",
"1D5VDdhgUf6FWpTh6Warw5Yibzk1kLAoKq",
"1PawYVYPutZ8suoFmUVZMFD94cCASAjUp3",
"1Fjpevr5V7hpons2ukd23Zaa2yUaXAcqX",
"1CAZNuzP1qbsBeu5toyrcHwPqjsSRvRHwT",
"1JVcofwA4Xw5ij2nyatNUAhzErsqp6gWi8",
"1PXsCt7hvwpC4GbW54apFhVpJM3Zff4BNF",
"1L5WTGeNxKNZVX78zJsn5JrY7h8SBcecka",
"1LDTGGPkaTxo4VFGvdpXistcQG8fq2c1Jc",
"1AdGbvKc8iSGycdYJCUmQeHKf8cxWkKQwL",
"19dWLxU5ZfagbmbuMnHiofPxZTgaMYGv42",
"1NhpySQcJwu7BjZs7ekJvkJzShgs6RJ4jZ",
"1ZvmqqVjXdrWj1GRFKukWmGALRE1JtEpa",
"1MmKn8pV6mPf5TPRE2mvexF1tTWmxFJa8J",
"1P7R8966yXt3TZJLdSwdPE6GwpJGvgyxw7",
"1AkcQihHGA7fYx6WAJaX92FRREDL9KubGd",
"1PAfUYUem3bMATp1ugnHqoKkbuqLLRpbeh",
"13WbJnZh64gBR6z24bZnWkFwtBEShBUZ8Y",
"13q6nT8NduXF4TDFkJnWtAcGRuqFUg2K4j",
"16jwL7K72PpdvKLt5DKJydQNDTwfXQBLvz",
"12xxKDtNrJec9Fj2quEVhe7fWzGS8YTc2a",
"1JNP4DG4VVWRDu8B2UAK3RznwXqqZgGG23",
"1asoVAzBx9A9VxDyL6uvb5iAAXMC5pRL2",
"1McVe7vjyr9GD4yECdZ3UXJmK5X5Piv5Rp",
"1NmJhi5wQeaNz3Cs8qBeqro47sGwpVM1yC",
"19oNpTn7hK33rteCqjNVmeukXJKARq5PQ1",
"1BoF935no86R5mr1dtQZrLufW6i2pJ8izb",
"1NAQi2FUe2631kZ8TeXZCR9z7Pr2StWTit",
"13N5T6agNMsyQZUiTtePvWKSjUA7qTm68E",
"1JA5HBwk1piYyAKEUxg41tvdQJXTcma1v2",
"1NrpJV7qkE7i3rYN1BPNxQCjM5ftAFwrmJ",
"1Q6bvWbRT8DckQDghCANdew8md5DTZQWtd",
"1CavK88QWSfTediEXQHt5nxVxT7vSaAETY",
"1EZf17M9ZUTQSXiuV7DVCR963Rtdw1Cgn1",
"1CB1TMNpSHpU4HqF7nQbgBeLH8ALbgBKdo",
"13phadG54FFEFimRtKsDFYVYK3KhYQ4GHh",
"18sxkPptUT6bLs1eX6zxhP8HZgYPCxXHgq",
"1DMDUaTs3xqNfdYUjtwQM6zmNBD1cfygj3",
"19ZZ4sD3iFQABMABgLd5DQByMgzqp58EJn",
"1BbgXbLqCMk5bsfdhEVhZmh7vVfan7y4qu",
"145Y7r3SwLi18Ue7pBamcmcgeBRW86h84B",
"17CxvdjBa56Wahpij3kLfioYYgkdvUSA1G",
"1942DXT1qZRV4GbsRNP9bGaq2Jeyq78Ne7",
"1BpRkKbfGGcowSopGQt3uXYFp2RhXjYVXc",
"14aKzsYPQ7e22NtgdckBSKfVWUkES6v2Ud",
"1MCg8J7ee72fGy7ytJWHjRxmSUenPggaCf",
"19rJYGgCb8afLk51xBe34KMLTeo2cT1Ugn",
"12sMEswXAPZQBnr5EDBPUZLZLR5FrU14o8",
"1DByLSoYPJvhJFPpgBTrKbwFXprT7u3gJ2",
"1MvGovPRiLc4Wd3A1GGRLPQu6FmtjsEZpS",
"17iyPB13EkbYuLzi5HKaPcTmxpKe8SNH9S",
"1EfHkPsuWJTooXXkdVKnhACCotbHgCXDvW",
"1LJNEdJzBdPdHYLWrFCim1MAUBx9JHnvAE",
"1GaNmYQ5LBpjnqibFrcNwSLuicZ2pXKhj5",
"1Htj9o1cLjkkpT5Z4PT1wkUv36CRVu2mJb",
"1Brf84ZjuTxKXLAQEyFpQ3wD8EdJHozKbt",
"1zsxkfQ1JTriu8t5cfjgeYp4SDrEXPNbP",
"1FHuD1aEmVwZw9HKoTqiFhLwoBrbQ8HQfX",
"1G7qdqujXKtoLTst1FyKiBZhhLFrqvfiDr",
"15wCwy85r9PJr4D5hcAMVkF142KujwAqSy",
"16hEs9jSd1i9CDMm1cyzEPsaykt1kEdE8v",
"1DAFrqckS4uf7eWNpQfxLPrQChpkYmtmD2",
"1BZFZQAp5129Yfg8jTqbhcfMvftcpE2tjE",
"162BxrSTXYcjhsNbqDtDk3rxESqkpcu9St",
"1MTurU3ar5QR7ji1RRGFxtcJ22WvzrKniZ",
"195jdE8eyuNyPyb2fyxM9tukxfLko9YpBJ",
"1NiQbt4Nj2fUfcvtXZ1ePJGo9zuauGupjJ",
"1wdof4eBmJY19bkZ7rkkyiu7MHnrDmkph",
"1BeY1vx7gmpzN8Atk933mhi5STa9wdfsGe",
"18MWDWyTmCbmzpdQC2HU7PgczRmKxbCdCf",
"14uAH2nPTsA1MH8JUKUJ9j1NkdCvnsvRsW",
"1PqUBicn3JgBFjaeR8paMrNUpBtiPTqZe1",
"1H7UcfUWom9cpSMAe9qMuDdGRr8caMdPa7",
"17FfqqDUnVHdpvJyoYwBrCN7BG5qNz22wY",
"197Rna5duNNF15XiAqeTw3scaVDEf216DW",
"1F8tF6mcnMzZZw99T63eSMVSotyoEpCqUo",
"1GmhBE8erW3ridmachKdzdc5JacGgyLJhT",
"1KNNixRFrjmZjr9iBhFsgD22xNfw8JaW3v",
"13Wc7zK71WRRhEDdTyMt62zLhxzLUEncph",
"1A1oNDfUAWYv4oRHyoeSfwrhUpGdi7cWvP",
"1FJ9rjqjUxDoaPh45ygAqKbAAktHsdU5h",
"131EBshSbq5jAncEpaZAFMNRLdyJtiVfhw",
"1G6gmggyG5PjkHG58dgv6UPCZsxGdhDrS1",
"167fNa1XJNu73nfSCuvz1PHeR6NHNpfb87",
"1Eek4E9hiu4MtCNRY47MUPXmVuFMEhkDVP",
"121JiNhsmNB9xeiJh9KYQXmNZEcWZdiwcc",
"1MgrCbLZoCwPEAHRCAbkPKXkAcyM8xBfWk",
"1FzvXT5uW8dcSz4uvF7NdHoUiY9p5zq4Bz",
"15QDhBfAbxxFQ5x6hMQtGyN7jW91Rn8epn",
"1H9VpLkc8kXkwjU993NFzvNUiDSCxiKwNc",
"1KcxoSS3CR7cVp7gpbWVUqE8rbN9KJHoGu",
"1P5uxFxRKEEewQnG9eekPA9UVZs6NeRzyA",
"19fRfXmNdDv7xBEuvRxQr5AgarCNSWabud",
"1GCCqG2gd9dFmWWHcUc79By1DbJMFzXoMG",
"1HT56nALC2RT22H9GeVxWv6v4CHit3PBmw",
"1MvHNt1LeoUJ8oz5LBRxSdVwomv6dT5KNx",
"1AWhKYPmrktymKaX5v3TsmqHMcp2y897kD",
"1MKZtS3AjHtW7U4vYenTzzcmGuLLdFU3YL",
"1FW9VvB6SRM2NoUbStxpE2vT7qeysGtXRs",
"1KAZUjobuL9tkf7aFhCFpmPEmnnEEA9pAW",
"1AeP996yPnyX22qxpCZbCuxXuyUbqwYn6K",
"1JYyMmQzJ8PNEpFchenkgiCjivKUVmmEzf",
"18MJefSPneYNKBsDcj3NsydhpaQUQs7dm9",
"1BV9ryRMBsfTBtarX9LaWkppRizZZj1dja",
"13yvCNvFme8Yc4Tmquq3pkzG8DaZVKsrBz",
"1CJfs5hoUHootBvPXcW7mgHudjCRkExXAy",
"1J5RhDauBTz7Ji1BJcqQPYh65uPxBRmUDu",
"15fcyPScQDGx4nWwfhAwZbQYwBfDWKsZjz",
"18yoJDQRcKg1C9iYYNUMEy8wRcfgASUasL",
"1AwstfX7DWmxYNZexpKcWT76ptUxSkfNzn",
"1NeVEe4jBrFGxP7GjaaDw2mf8X4UARWjwC",
"1JCjFnf9C3MQdAfL5F568VXe8cMB92WkFv",
"144cRQKWyq2XDLbtmUesSUB72fi86mm75q",
"1Pwo1VG6KiT6ZLC9K2FWWZrzWfB61n3un3",
"1NCazKuyuUFyRyU5kCYFL1DsnwKiJA5Xe6",
"13fPPgsmAYaG1whVZC2Sqf4yMfDYgSg2eN",
"1HbUkRDq7v6eZDLZFtMaBfsJSMjkGaNBLV",
"1HzLmkpahKUPe5LGwEpgHZChv5xZUJfzS6",
"17LwJ4tVMoEWFxRCeHw72cjsGtpup9uXa6",
"1N3bxjvtcTMQuJ5tiHiai1aKCn4k9Mqibh",
"16D3Xt2RMUEjQhUowDbqX9CSHtcbFKi7R9",
"1PKNLC42Bys7TkoTU8tKESzerpBrMTNmts",
"1ADZXbrTLnRqXcUeJQi8GLLB6F8WbJuCLX",
"1Dct3pomHpTDPsz1bFuAintQNk8dk4C3T3",
"14av7XNHsprsM5wcW1XK8DNF7mReJtR2Cd",
"1P79ngJst3xLZuptHC7z4x6osPmCorzCki",
"1329YBkPvAaEW8b4482mPtV83wezRynZRb",
"1nBGTYMat6RoMH5W1uEGXVgK5EvkjYh4b",
"1FH42KnTpMgdRX3RNPKLERjgSrwfQPvWP",
"15FZqCFiz9HnFfFhPomPbUFdo4h2Wecvc5",
"1NNXqwtcRrFEist67gfwM6BdyFKCpKJgCm",
"1HrEARCxkAVPQsxBAQ8fLWM3YwQW4tc98x",
"1PMP11szJN4ngiP9HYkPKqLbA5ozqRE5og",
"1DzbcjHj4zagNgH3hsNfGrkXpZcQsW3qTp",
"1jLYLWvbbyFhsc1J76Wm8uJS6XJFxvWDr",
"1CDTr5bEHeuP5R7siG4CcKUcCYmj8jc6mG",
"1NP9GoR1oLJo4tyYxLZDStZ1Xn6orRbA26",
"1EKFaxGXgPbwPiZXyAn6PByTR4iMPNRTry",
"14EHVQc1DrUnA9hKC5EfN4THvemit52qyV",
"1QDqvuzP16U53NpcmUZ6ESRxMkTfUrgW6w",
"1PYUoeSaq23Zs5YE3WNafAnLKGSTXxPoXn",
"1QBev3fy3zAsXoQrWufe8HTXTLECr2Z62f",
"16ysKp1Xgb8XPirJ9BsL5wXUYFBARHJK9c",
"1LJRYZ1VQpXu5Kxbvs3JmXaHUZPfZ2FBX",
"1FihzodpiGgZmdNcLHCUANraWkxfgu83KF",
"17GrLCoErx2okfiPBFoKPwcVbUWexY7Fg5",
"19pDVPm8KKbEa6eaCRcnKUgBS4YmVCSonQ",
"12U1nfS7t3YP98esEurEPtsGSnwEiS7R9V",
"1DDQU7LicpddVwAjFs2QMuCsXb5TJco5v1",
"19j8EkbzP4gBbdAZsAQE1NTxzhNn8pZFUF",
"1FoQwVMYv8u9JHAqUFtdeRR7f1tz2gWGLC",
"12rCMcN7LJJDGYCYNqpWLyXrPZmrB8Fjvz",
"18Pjp4YCPo1M587MsFphzDSKh1tmQxMdNC",
"1HGN6SdwMhf8zRVxJypxezy81qRZWoXsWA",
"18bP2z3M8eE6NJVhB4nGg8ciRZQFsiLY5r",
"1H1NDkujKErrnEUJfkWZAB8pcx7jCkCJSh",
"1BmZPEPeJRsyHypVFGzNZergoRkCfK9seC",
"17qJvUkw45exHx65W6ow279sTdNwWvS8U",
"1J9AUvgx9atT965zQFb3vTSyWSGd3RyFNA",
"1Po856Ce3pPos1BcmrkNqksbcGu7S6wuB4",
"14xBiTXM2eo1heLnxDDqdLgvcuFvVnuuXi",
"115jDUkr5xV7fF6Ef17h3SseBMwFmEEMYK",
"185gyUVF12ueaTvu7ZHUXToqtyKGq1qkpJ",
"1JBwicnu17LzB84uxki6AgLs1MM63VWPQh",
"1EvHGVjtVTb6HkW4vwUWrd3SjxmyS4XtuM",
"13FB6RPpUvqFHBBz1CP4TZGHUhoUenRMev",
"12NywoZtcL8knRrqo8sDrBdmuDNNsCqsy8",
"17zRxzABwsFJ7tjucMufFLLm31oAD7wupJ",
"1EyX7dZH2uyU8Jp8qm36KWxX4mFEAtQypm",
"13oyeKNuAkECDhyqpUToi4XbXmMbEAEjP7",
"1BneGXuHH3R7By9DfsGh9aXXCK8H2xUYp1",
"12XjnKuP5UETukBziaR25jMSZVdn4192cA",
"12J2WLkxpTzkMykHV2HNgjZmagcSQKKh6x",
"14p8i1xoMyiu3cBxcTHjD5a7zFr8BkA5fF",
"1Fe74QUNPhoiEkcBYQhjMhNvRstxYNCWJr",
"1Ak5VLdDomZ4yfoa2832qPaJq5UBpQJAWj",
"1AYgi8bFfQhLJk1Nkdy11j4tme9mkyTgcT",
"1D8AJNsS2FUU7tbfCbYoh5MG1Pvwe6rnhr",
"1NSJfZms2wAfWfcS1YCyKDxf4wT4pab41R",
"13irM5fnmj14k5hpbUgsa3N9PDPMM2cZv6",
"1S6ALKEWxf4cA6xWSNuvfzSZuXWxRq6PF",
"1LffFLyGGDJemhtPyUk2QYqZuk6Xe99UZX",
"17RJDRdDhHAJ51x7nNwzgS5qi3vsyaELEb",
"16DJC68m6mDScpNFb5W6prZdW7cxi7CeFj",
"1MCUH135PT16KE11v2eUsypkP2U1zB4gv7",
"1LaNdYA5m5sXmCaj5WGZ3jXKju1fyz9muh",
"12Becxd3A6jqUPXNP5HNvjWyNnvNRndisu",
"13pYaN1Pm7yZiJEKnYEectUYdKyE45SyMf",
"12zKFucJQokCGxg43uPXZyfLHN66qEpS3k",
"12DqhjW4rZKc4oExGNKhmGBJ9JKn4p88xw",
"1JiSGNb9rSn45omhsUKT1SNABqnbDkXP4V",
"1ECFMUQiUB4rN1x7fycBtGuVdKQGCHWQKx",
"1746NYkwNuHipmBYEZznenRr9zERPUp6VD",
"1w8ZYMni62A4NqWqwTKcPvKhgHGS8hS3R",
"18MzUKUyXnZ1PYGUhWGh6sW1sKgjs3jz6n",
"1FzqsXTxtpqHRirtXbjcUJGn9LW6kUUtca",
"13JiZjWDtEG15Lww5uCTj4AWUuJq5naUDK",
"1E3nwTTkF2Ei91wtzdQRxQnuLSoXCjuiq5",
"15mrtb4nJAaUbu58T2F1S4b35YyVZWLn94",
"15oiTMJfWd33dfMoybYiqct44sPvmkMQJC",
"15ZFiLugLYMHQUgyHCQfMkbmMyhR2pYkES",
"1LQ1VCdfGALhTeYveyJqpqDbsfxJuA5S7z",
"1B62hgjg8DQ2HgoiyTvkxCPpNRStEM6djq",
"1LDzYfkYR1ZduQsVu6joa6frtWas4u3odX",
"1Q29ZbJC9Za6fUYgjHBhgpM7Daz8PpJF4n",
"1BuUfFSt6qVycz1Vmbs8oXokdsVHzVzuvf",
"1BiXgX3vmhGECLxiPpYomKogH1VxMMHkQ",
"17BVxUbHrcUPMxb2PsvkrTgX7VZ3HeuZuP",
"137uCNhtvQKXaxDqepknJrAFDsnmEuUD6o",
"13i1cFPJCuhwnCz4pyebv4Sic6b1Vom78K",
"1BGH6VuZ3Mop3fpfjEWnpsbWCAuoD4NVY2",
"12UswSnDgNRFoddor97zoM1Xgj96B3fXMr",
"1DmzDdnDr9jArs5dvganttWeXCstRBpkL4",
"18xTbY3aqrqQG2sjCQ6TknwmES3DibMW7E",
"113yCjaX3aH4ZzWAHapjdeUPmkyk7YfZpb",
"148LcF1n2sff5E9tSF6pq3xqijTeF96EAi",
"177fQK2VE4bMEKpyJnTkSHUgjuh4P6u7WZ",
"1QDdeXZ69cqUjCGxJY62xMtrw2TwUMfFXK",
"1C9WuU5XMKMWwtvwTF5n34y82KSsMx946Q",
"1LwZnvasXyqyt5gDXgG1pQntqLsWLEaUQr",
"1KHoQmoeWnvkRcf8RTT3s2JN7dCvdpBCzq",
"1A2Wk2d7CmxcQHRGyz8gVpQLaMHKBJYZkH",
"1MZJ79t2x5dgkuP2puMDwcetgNV8q8AMkZ",
"1NMSu3imMtELtPSy9rE8KCpWF5Yy4gDpa2",
"1HvRTH9SgtRpq9TCyPmXMPk31ZVA2YWHhe",
"12SFXz4QW7CDA8hmbGy3tDg2LQaMGqAYKL",
"1DKsLvEXi1rXUjSU4j3VqSu3xT24jMBb8Z",
"1GxK9vnfMS2cS7zHYztCwYGUUGJWKabJSj",
"1H7cxM88aoxKi69ec5myQo7Fg2TjaCyTeg",
"15JnwmrEToQ4NG6jkBr8QkGURSyzVyrD9M",
"1NNZjarN6WTaedujrjffw9jyoyrRgEBX3L",
"1BTvL62ksAbDrFHi82fPtLeEygftDa1Lvd",
"15voZ6yhd4Ph8JrNxLU4z3ZsKcwVonwkng",
"19wa6VhKFGFryxDb5rUFSrZUefcng6xR75",
"16aAXGR7GeZ7jv7SyhrVW1DwAdV8wiLrEP",
"1K9FfQV8ptzBnFNMvB7XLSQpPeiDnWDS4r",
"1rMt3M3vRMkmGKsorQK65RGcPkE1Wn3Ed",
"1CBM7AwNY4Y8SHAf5Hdkm28wiNaNiVpFPL",
"1Lh4zjRfJgLWHiZpLjrGuoetZYkBVu6M8t",
"1Es1ovvsGjhLMph88zEAwFhSGADDRK5GPy",
"1BxaDkiAikeDcykqPbroCvAB4zMX8NUeH8",
"17Z5L4FLAAHSsx9MUZeg3sDS22M395hqhg",
"17iEcXCJRuSbGn5QxhVzBWSBn6pQSA2Sv6",
"1D1g3jxzBVpS153MCWf418yM5dfrQ2Cphc",
"15BroXdoR3c5DNugQW8fkyyFitZchH9t5e",
"131uEFCvvwKMicgubcQKUNXfyEGY4XJkg9",
"1MxMKP4nzWmTFfQFbeTLbWo3nbUKiWc2Q1",
"1M9zpzeBrDZJYqyFg63TBmqPi1dajRDoUd",
"1CfcEkRT5EZmCm7Ha5zBskzomZ8AXcnTUq",
"1CFEtfxqCPtyi1YRe9DFmnVMnJbJLycvoD",
"1MyomcYfFJkSNEqUxNu3n4V1gbquhE61E2",
"15FCGhQ92ba8XiPTmyhC3wYdLk572Du37J",
"1FL1gcAvhHVSSS3y7xgqQVmK68sXiVSuvX",
"12cQHXAfL3iUwADbgEMoLGQS9UNGwXtNd5",
"19Tw5Ko3mEjtu92vSPQ9McdUvRQhBakWgX",
"1714bvY1mRhJB1XK8GoPNrHRv6TxFjARWm",
"16AT2sp297WwBURdQsz9oAhajYzGiMcH3D",
"1AiJrqUoDV6Q9mVXtmNdq2sV6DG9HhWm3F",
"16CcASvLK7mSNVBC6rRAVmhP5RwWrSdssj",
"1NvV3jCruYJWxtGB682gvUSuZ1eNeZeRzP",
"1LnssBFgCSXHcstkHaz4aEm2w5oXkijxBi",
"1PgoWteYuYEVWMryyUgkHHoejfwEKVzmaq",
"1DHpERvRX1H6cyw7xnh4LMrNNb8a1PoDRv",
"15GpYLYUGRbUU2Yn6VkH4FbMgFcVeFbG2w",
"14pMwTzXFcJHCJMweCD2mSoXPPAPRUddCA",
"1P39VJqjnegcTUnaCqg23iaHGtwxVh2Hk3",
"112pR1GVQdHWb91LjQrydtUrbh8cPgwk6y",
"1CFt3ThQePsnjB3ZJqUWWLWvGqqoLYpBP8",
"162yafacX7gPTMymvc4SDdibUoU37c6mSk",
"15X5DtfCUR4Rbh2dDFRzsTKApkFgiXYGSX",
"1BZetVWrAzHRoyY1wDcU6Rb2Nd97LmzT1U",
"1DZSYbsfUeqEny6Ne9UpAnLvAi71eyU5dD",
"143ATtAQ1CAMRUWxENKdEbgj4BHSyUB8kd",
"1GqB34Jg5DPKoZqnTuE2HxvSJh7MYXu7Xm",
"1CGugXL73joY4ndbfHuLso19y4Dks7Au66",
"1DqttDshzYT46Nr4nmtKt1oM3WYVb44LEp",
"15RsUJsDsteCrWNJSiQoHApBqgFHSa2gpd",
"1Em2x6KT5etprYJA3t3xjawCxEbmZ9MxuJ",
"1F2fvCBPn4PGQ44am8Ce9vK3fxmi2wWHKE",
"1JdErKTx4iHXojCo3kB9WAvRXKSTp9VjYC",
"1FAkbiGDPn67Zp8AdHRVYFKzPNqJjMvSrR",
"1AqC4DE8i98J2Nk9efa8TooDaFB7MJxpmn",
"1NyBxR11gHdbXRbnabc9qGJGQGiU8vSo9w",
"18e6hT51qAN9xDdL616B2zDUCUC4uD6X4X",
"1NL63xhEzWz6ff77wMDwtqpFKEsw2JbeaZ",
"1JEXm3F1qE7wvy7Up93Y7nrt9ZFYgMafWZ",
"1JbXmRVh5SPgujJW41M2H38awQvoJybdwR",
"1BB4EttFyUcsE7afAWVBEgjLb4TiXGHfjt",
"15tUWLqFDrRyZTWMXQUMLV2QT6ghz1Kvit",
"1NGUF9hRFhm1EJkkBtDXoUvx7G4d3vmbW8",
"1461rq1eHZWndbycVqZL4UmBKZa8tGKcJS",
"1ACHrzkG4G7Mpw3s1umn3SsmZmdtza8fAg",
"19a2XyZ6f7gFcchv6dUigCCsS2MiKtJf5u",
"1FXahVcqHwfVCBqf25QwDz8HC7CkJb6Dbx",
"1HCpa2YgM2qc9pVsVbg7wPdQMDJbg3EDDe",
"17qKULQMYE3Y7tLA3YJ1nCLR92MTL1F9bu",
"1HMEB4g8QYex9wLQUQbPEwvGRnBCehWQAi",
"1DrN79MGboMTwiaxkJbRvydvs1Ukf7JXRg",
"1Gkj9LRMySW1Fxh1iJKzkTX5XLdK3Vq3WS",
"1JAurjkJNo2kxS9xTujW3TS2XxYFw9g9qC",
"1Mcb8odsgaApinhwHm6mww2QV4bU3PTfXh",
"1FUMtNYJwEvbL88KacJeMidHcnKhJdfJNc",
"1CZKTC5H6ieaky9XyofAEdR5GvU8cAfJXw",
"1JNnCu1Lofo3ftRAbbXdpHn8yHviXGQRNP",
"15pCxXBNvKYFaZE13n7gWNRqDeAAhJkSrU",
"1H2NMY1gEenC4Fk5Ktfi8wenhc72yFbiKG",
"14uWPZTe2MGaybV9eGjEX9peYmrbPT9t2j",
"13K9CsBec2pYm5ezgjAs98ENGQHGUJHRms",
"1DvJhnbKUyoNfXyB6XdLwbyhA9qVzfCNsh",
"16AMRbxeCN88ohL8yYDzHeq5PZsx6czfyH",
"1PiFDvxyZmjRrEvCFqZtYZK87ZuLnNheqg",
"1Exw6oaCWCeEiYWjKtBGpEKXQpwmQ597k5",
"16kCzbn5t178CJcehpXssBdCqKNSUphJK2",
"1KmWoc4SFQN4m9V54WSVMmiTdQup555L8B",
"19VgDU64WFAMrpaykgrBvtwScU93PbFstv",
"1yrH8VAZCjRZgNrjgzJUur3rF8mLxnm3S",
"14kkvVHxyfhiG59VY7XFVUAvQPwD7spDhz",
"1BDSMMctUF4gKN1DunTz8YZt6BcHUD58JT",
"15Bgjnh2YwSwL7n11XX1vgm5W8EeAAib2g",
"1JRJiDnWkkm2qeLcKY3Kf7ViRBg7PNDRxQ",
"1A1PWt5uV45wNF2aCPH9shxQD1V5ytq3xo",
"1Q8E4kw69h4R7gN8ba3oPqTmLgTF9NUYHr",
"1E6PjQ9nEAbBXZTnHcBocpB1tij9VdnRPM",
"1PU2YKN6pKWkFuy88inKXcqxwfwsbzfvMv",
"1APSecRQDzYudhVLrsUrbaKnFd1io9sts4",
"1F6adnQQMxLkZeFRf9r24dtXyaAR25CMnq",
"18BpcVH44S32Ua4kSjFKYQuuQowJKcVQiQ",
"1NGxkkfnnb7KFWqQ7iNTmSZzD1TqriD8UH",
"1Jr1SrCqYkjJNu1HVrHHAqRUMQVA1Gh96s",
"18oXYSdPLj35zjfeoCRfaDMdnVt5UG6wC4",
"1M8ykMvpbzV6apFop9nwP13txnvdiuxuKu",
"1CFsg3XeVfr49xm3WpXhAXHnYoFEtdrmrB",
"1NYKcBMYgh7qA7KmRKzeGeH31zcLpwpyE4",
"1AFXyd4uzSeatjmBTLe4QEGM6SuAxQiQT3",
"1LFBgTpEveWnGdB9ky4QGRwT2e9ngeLbVs",
"1M1R1pVA7DZ4jkg4PfsFoDkjgk9zEtnNeb",
"1CPoQE4s7hFnM6LGASfKKaUsy5KB8gNdtn",
"1AFuSnKg2z5VtKZVnqTH5veHmJGAUpWHGo",
"1FBhfrgyqEfjfxovABTd1Ng8MzeUpEaNqn",
"15Xe2aV4bPMjduHHaQ5ZyPasoeMQCp5KaV",
"138tX15Fgdqcpy3ZFFVg8D1NiZzxma9RHE",
"1HPiGyxdfCanMfhEUg8Zn42UQKqpbEuFdF",
"1NhMJ9rdb44ZMaosLPFZqyzF3uEL75Z5zt",
"14FumbRtxM2LjHL4wsRAqQ1sMpq1yGbkvH",
"16xBSb6VqS1LBsg2fdVeMvgcSetufAmTcU",
"1Gt4wypb5hL4UtnK9tcLpKSm7oNF8EMXHV",
"1KRL2uLM1xvn4tcg3S7AKrZF5YYwEBaRu",
"16BygAz1uLSTTPUepuBUU3EnWQdqrBpAuK",
"16nKF7YfxehPDgojFEETbTFiVEVHwKuN25",
"1JsgocoTskcgqN5f1t9goDLLBNuroHkhzN",
"1EwcPbqT5jFxrM2py4TsjeYXopd2RJ7sDZ",
"1ERHByMngEDXE2VHfBVbvvDH6nww8i4ppx",
"14RcTZNwe4oE8ccBYLnC8neeZHjN8RSoDQ",
"14TJzUxz3ZSc3MDfzVRCQxe8nUS5yJahdD",
"1Ftgb8PUtJ4XQsdRd1MYAKaJ8fAmnqnEEP",
"1gn8ed29zKcpnsuBoMGcJSYokuhZN5bLQ",
"1ezALWps9GHKW8i4DNhzE6HAuztg7QR1w",
"1QCmVNe6kUMc6vccmhXuGzgFg7dRhQAKrg",
"1Gj57XGbiwXUznyRmScQXD45WSyd8uNSov",
"1JJkECkizVyryXK8ND94t8hhmSwajRty9B",
"1MG1PfY7eWtRntefozFHVbfCb5MmKnmakx",
"17ine4cqr2t5W5Fs3ogWsexhg21RoFhMWE",
"1JAvDaWfobzWxX12WM71YvRvN9YK4Z3YsU",
"1LuhU8qcYwYNpNHg2Uf1mEfnP3e2TK9auH",
"1JdEL35DDX7P8dQntWskGheDXFmGThmnHp",
"1ChwDGS4N842KdYS4X8ZSrg5VcfZPHVdSW",
"12CtnsCr4mRUTq5c5eoQpEH8dXNPaHKFmv",
"17RedbhRZWVwT1rTGJ58KCPKRebfr9Hs2r",
"16Ap426QA2LrTBqExUW2brTEh5Q7M67Dkf",
"1AF3oHXBcej78yHcomQTtSFAyJFvrn6e2o",
"123VbCjsMyxJFE3bEDzWZqMSWeJ6Ho8wt2",
"1LEjUba7dQnQGS8W4cYEJdAV6yL1opfJEe",
"15qDf2h1mp4y4gxwEhKGspeqVue8qeCedz",
"135bACAHJNXVHeDhTLhqtuNEQF5SqfM4N4",
"1Db2YYsSnFqjXb4rXV1VSXjyC1K7D6TmFT",
"1FBt81upTXsQnKgPYjs7HZy1E8TSQSZQoV",
"1CJTvv2KPwLHScrJdomcbgHgoik2ZJzuBA",
"1E9vsvy3zN96LgSxKRrCRfAdz1RE3H9B7j",
"1BDSPHRRLXHjGQvsLL639TD51wN9KcZck7",
"14XhN6CeJjAA9wtLazgmqjPzBZ91sKoPen",
"19YLaSb5ogYx8hfr2KCqKn9NXqGYGmVT6u",
"1MRph6HLD9GvUcATVkFqFXmssir99uLWs2",
"1MjvMPFkuuy1qE1tzj5ax3rFQ3Mwf9vAYc",
"19SwLh9cozJGUNcC54CwXrZngDsQpu1ZSn",
"1FMqpqkLXXzuJxaiVKTAFbU73S7A9iXX2r",
"1DKT1BXnXS1Q7jBRJxf8SYqd3XKU192nWu",
"1MYyNsNBSyjD5H5W6Tha4Kpo38CQV2VLtm",
"1DGBfePmstNuF8SDQPveAvPH2TeZnxu4Rw",
"13jaKe8EMe37gir2PV2MRn3kqtyQS283r2",
"1Q5SxofWQkKwZ4L2aT7GwERjyEThfqyXV4",
"15Gaz8RX56kS4KGo17eRfofQkzkpG8wLhC",
"15zRTHK1ZQcmWGwuboCpAQKpCn3tG298mS",
"1Fso7LuDvog68L1jZK7TcKkod9W7L7VuLm",
"1CKJs9LsESZ8Mq2iYxAqjCRZn7mfQMzu83",
"18SjfwobjmVkw3m4yzDBLygPGgZZJSDAu4",
"1BfmNB183wS97WEA39LGbPXCkwvaGrXRhY",
"1AkcDfbVcCs8Cmt47Fx4RyfTpmwZFyv7An",
"1DfcQ6Q6d8hnYNnJzmuvvCCgMtYEusCKS3",
"1Crgmsq71diE9z7v31Mvno9Hc4aMWZAmyN",
"1LehYtCm1b36sufbJVjrGmaRiqEKcRGJSZ",
"1JLeVzsLXyoQWHRxFcU4mKZ4UXedvMvqq6",
"19PemrBGTcoWnNhVV7gcBB463rXHU1rEX5",
"1H6QPPEQP2DuxpvfGNQS6KpupthktXwjHc",
"1LmgixE1QzpcTmHTK3SWHxdLtVUi3aFttH",
"1DxL6gcutig2Xq2YiMA1Y67RKEfKkBAL7w",
"1H7jqCUrXgqqATejpDWHXEEqzomfMAjhZG",
"12ffGuok5JChEM5LVUF63caNqpH9N9dBKX",
"1GFCJSgCgTVQeZisq1XkniTQrSHyTXWnwx",
"1DFA2pbcBPMf1iQWSu2pnrnTwuQediY7Kp",
"19HRsMUvu3J4C13YkShxkjH4yADPYPqGyq",
"18LXD1P5tkmDxePfcwpuPAjJQ3bc6UyWC1",
"1urv7LbQNQeYg4DE5t6Nj5EoRsyBKT8MT",
"1Q4ggSS5ASTs5ps7vUWRduUfPvPowc54dj",
"1GhkuyniskXdX37MxLNkY56oK89AfHwxCZ",
"12pzTMuhCcU4JsphCwmtppHWMSbH2p7sHz",
"16nTg5fsQWirLePj71c8PLGJ66tx2manc2",
"1DNJGb17G7whKXEtkak4cCD8p3ahAt9bNa",
"1BCGJp1vWwm44sdQudeadKUYBLokPsp12J",
"1CfLgz85nW6pBydxw1pEZqmpi4PpLtaSbD",
"14NCw2c9AN48Ak5rMQWDPCgfbN8thaYdwD",
"14aCHnCiZLzWtFvBQeu2K76kH12A2LJoJr",
"12UDcJtGGUdDEiyPqm9qQkHQheZvzJ8uvd",
"19fMhjm2fuT61Zf9wtiJ6FdgXDg3EX7iD1",
"15eaBhnBM8sxMWgjbPW7wHXKweE3uxpS8T",
"1JR8XCqXYzqiTwwQ4fdT8EFtyP4HfsDjj9",
"13hexwq1GkJWv4Wk9hZqYgWsKYu2zpKMdj",
"12BbqXR9ZZNvBpchTPSeK6m5Y15V3y72o7",
"1E2innjECfCUCnkD35Y6yfUxeL7jyLHr2F",
"1Bj2F1Vcr4oZa8Z4xc9aCVnWaWSLEb2hDK",
"19694qTkWVSdrrRLBMCFSzLitJQRTo7eC4",
"1AjKAoUJovnKWxmDJH1ASrWMP2vPmKVbPq",
"15PffN9fkRTPJPPhxLsfs8d8Vz7xPQ7ch8",
"1EHoD7YvkAAjCD8EZYeDiKA75C3EUtosLU",
"1NuJH1hkAYLKWMaPR5N8UB9qQvpBCqZa51",
"1AafdJqi89knbdNdqDxQnBvgECZ5Y1n36n",
"13wPcT55u6Pj77QfvVdxo7Fy8PUi51rmVV",
"14sEcaSizSsNi7RKBVnmw5sQybHLEJgrKh",
"1JLTguzRGyPMCBbbDCzxoUuCmNg8yzD7vq",
"1JrU5ao6gESGaaNenaikGqGgTY9XmSVYVv",
"1ije2mGMVeR5VfbhUk8PsUanohzTt13nW",
"1PxsYTx6N5jw6hZ1UsHsVTsdhtFLxqtn6s",
"161gDTP84xw3PHM9MohFyW9Cgo4g2bED2V",
"1JNjYVTpSSwV142k1BZYPva4dmXcTQtNaH",
"12tP6TyMTkQ2Ke9vSyvfRpkfhJDri2Bhri",
"1KVF5ySJXrh9Es5TYRha4uqpMLmyvHddkL",
"1BXbtbNbbb5AKP87mu8fgMiqW3F6utFAW4",
"1y2CkHZDJwi4aQ6Lv2eCUGfnWEL79Kt8j",
"15DWapcMzGQTNafNWRB4Vp6q8utoosy2WR",
"1CoyVZ6hVALp4rXexfFMeYJSaYaHP7F6j8",
"193krJeqq376U9xkYBBetZUJFGhfjb3oDJ",
"1P39ZNwjNhXngzNEPQoR6FVzRS6jBenS1E",
"1G2889UgRLw3pU4SZa6Rj8VSXVc2644TRH",
"1L6jWQwekbPqKmUAxxTjnLSAsL3Y866nzK",
"18cfaLZGuUhPtvuSyasGCckDLpD97RoGJT",
"1LkNBW4WAPqDX7FMfWYeJWeQzcjmnFAEyN",
"14jqhRrKnkd7x7eGwafDCFJ84b5GZEK1YP",
"1Jt3edUfhqtw9MePHk2vXhQP8GqPVWoBjw",
"1GjdGGUUXt7J2vWTtZT3JJMjBdpSyrK7M5",
"1NoM82YLcEWAnbUnWWg8E2fRtx2yUemNnk",
"1J7GrJADKM27fKaoFWt8hSD3aC5AGPY5VV",
"113YPqt3Xn7NnF5HNn5X44spvxk9njdihY",
"1EhK1kRifoBc6FGXrK793fU9TqaFnaHCgH",
"1Mz4aEZ5WxnrAEXeZDzoopmrU99b9emhg4",
"12127neSmgzEMv4E44CQjuM7UxLRdyJtJa",
"18hwKs5TWyF7MyYPmv2Egmg9REfqYb6zH2",
"1NWgcmFLAn4BALCDFByjPr1EzbRvByYeBr",
"1GFCbBcdcE7jUTB7cjsFiZVKnfAMsTW6u3",
"17jP3D6ZigkTY4THgSQfDgg2SHcRJzvZWK",
"1CiaaRD8mfTjSg8MoDizBC4KSATfpKJvYg",
"1PhfF9DwjAPEebMYBsvqE4Qnk6LqPbZWuc",
"1GvWcr565YhLBMAyoSSfzesxrmEykqfit",
"1C47v4Cn7tSCvKrmBTydR6KHaxSbw4sFMd",
"1CmyD2x6G848GHJHgaz42B9g5ys7DdDDgD",
"1PiwJNzuXrUiBxtDnUDQiMXXg7JJNyFoEd",
"1FFTyq3ziPFotFjbjap878YiJdszWz9b7p",
"1231uyznAA9vQDmvDSzp3sgLeBJ1qXhvmy",
"1GTLJ71YuYXRmHWNtB3iCAN7Y44zXBsNDz",
"149Y6jBQ5oh83n1bPwWgCijPtAvK6xikHw",
"1Ey3NwazG3DgVkpN6ZmoECFjBBakVGC482",
"171m77dzwL1PVb1YEVCKU5z4wcpsHG4JGT",
"1JHhWjpdKVa7kH5wb9HxUqW9gJFRVeDKow",
"1Hady59na88YSDiLzdXKxokDHKitkBHQyk",
"1N9gSx8b8qXPQf9Us8U7Nzg4QAMYW1kKBC",
"123aAWJXwsbGvfwXYWRTmDx3LeE4A3S3NK",
"1PoFUu3MSmv3RCrDA1N9bBPwBrmG2Bn7Se",
"15CytzUAdspeisvseL3WpTsyVvC4f3mJfh",
"1EnS4fMn2KDBDtXrVacv8QMxS8GANPmPCk",
"18LmA77NxJW2H3t7pXspz9o6FZgFU9Zn2b",
"12AFwBhWc8w2AWb1vxAqu7jepMCo1czuLi",
"1JFZWBbwguNYVU9sPQATZhVTyJJ72soNoJ",
"14Ss76QSnpoSbJsCujTdgfSzc8aD2Q2oAT",
"15tt64K5T9TZoNA5hLTt7WoaJndFQ3RosV",
"1Gh9C2bkwHwB2o7ycxy6xNn4orUrQb4oFn",
"1J8dxtysNfSjANR2MSfNBXWka92CgugXWb",
"1GTGoQ8TvX4FV2Vexo2z4vpgotkdiPMdhj",
"17uc6uQsgUHbB7L3zFReo47dpCae1BfZn3",
"1MrBs4KKZyktNr12G9kmm2R8TqgdPrcqgn",
"13jzScJJBsSTnJx5WnGe5FLFxs9TrC4C1H",
"1Ec6b6FButNvtoeoFXD8bERfuS67KPFsGQ",
"1Cbo73bNLaQgeEgYeYje2QXEwaYac8L1BK",
"15Mvr38bHvZeRfmXYXNfupPhLFecRkAJE8",
"191FveedB84H8rLpuZUM18sVUEvQ8axQMp",
"121DGebbCxLu3s37d8fL7T4cX3q9yNVJeq",
"1DkqGPUAS89mqiZmCsVguSqpE94Q32JYN",
"1BEgL6p4XyaUtwAxoUhbKbZJtviq2Q76rB",
"1BzWQL265P2E5P3XU5741mM4ctcF1BKG9a",
"19U6k3gkmUgBA1EfK1c2Un2At4bRFW8TYr",
"1LaXzYdmskASzqMXpgHTs81pzmREedLSok",
"1MWuEn7i8PRa91man2DZoQ51N5GAQsJdxh",
"1EPbhokktEGRPShR7MTw1UHw4rXnDCJr3T",
"1DGJy2dyyDvS9gjd9iP39VM1dCikzHt7Nj",
"1L7NBGbCfiFB8kUNobATJtDMWSebzNRFok",
"1PwbB5FteKBuMxdxXKgAERQkQXhZmwqiXc",
"15Dm7ZLR5PEhE2Haope5cw439ksdQaujJD",
"12aZjUFgmGuVRdeYXa1Yv4DxtRRy8ujkXB",
"15r4cR7tZsefK1ZmncX39TSS9tiiCaKnMk",
"1NDLreh1CG7QjgSWr18eZ1NvGxvVfqDzbJ",
"1EStupkHBqaZbQpjwWtgJt3qfrXWcdhN3g",
"1Pr32abhAsZFsxA6YJigNY8tvkkhLMxrHr",
"1A2U5wDR7ir9uDuHYwBZzYfznpzw7dtkgB",
"14Lt3nmwWLypotvq2ohR2XHqqD8WACbLpN",
"174UhUEPmyW4pGx2BjbWg2MNkeaoDMEPEb",
"1C1mG6o2DGa163VaMDNw5WpRDpLvVJJgiY",
"1EreyNvf2HkK7KqgWfDPHiA6VWs1FGMJ2o",
"1JDctguFoekgK9DXEkihTd3fyc1Jqq5DBV",
"1B7rETodfQ5que9FeAzRpqQSscnF5XTVHK",
"12R1QoASR3rfMPPbWLvKDMBkCgVafEJaVt",
"1FqaqYoBcfckmcqijzzK2HScSmgAwPPVKb",
"1JXZHPKjLXSeFvAudUk7AEyepmtRfLhyom",
"1NFGJYQCBhv6DLZ5DnQ7mVSviNdpFj39w8",
"1CVxcRbrkbcDzWF9VUVMadMzB9vKPMFXKY",
"13rreFAtZRRrkNXfGLj491Aovjcwsxp8Fw",
"1LUyUoqteVVMsPhXT7KnpDvQ9ZErfEepuw",
"12pwuqxxwEPJ6hPYnYgkYxirx9qD5vCwP5",
"15bKvf91oScMKXvez3Gygr2gcqLz2qpxbr",
"14XaXtpvd8gZTYiSA68knGU8VfVkRtJzLn",
"1Mhn16e4VeqJvUb6Dd6KJZaxk22AC9cVvw",
"1PzevjwQJmJmdABvjBzzCQu6WJRrJRQhm5",
"16ENphrrXa8Wu5AdhtjnzHCbLXeu6DVEbD",
"1DFeGaqnS7ZSaEVQ6GNEwLfArDh2BooMRr",
"158bZwoBJarCR3RLVeWgqsB1XTwcB2YPhp",
"1Pkxd1RcPWiyB55sczm83VTGXAzfujacmK",
"19Wq9XadFxe8LF868B2gE7kzoT4NXa4dnV",
"1DL3WiUj4zhNRDKcgNANLdcX67dVoz9ttA",
"1Q8DSdgMbVJtj39reCJfhAZvGBQw5GiFeb",
"12Q2Ne982T1qyzcuRb1hNvGypZewKch2kt",
"16SzQx28zcuH4nB2hhgFCjZMw7cp3V6c6h",
"13XqWVsjr9qLACWQ6fdvppjCLquk619gVU",
"17tE7kxXQZfEAYu4fPHLCeaSHaCz5NzGNh",
"1J31fcRopZidDiNHGEcXVUUgtzgvbhkCnn",
"16LHm8yHycQ9k4V4U71Hp1SRoHmz3WVnWs",
"1JdkMXbDAxHp7eUTF7zYuWN42GFN9YWLF5",
"1N7ohLQkdRSGTmrC5L5pgBQeHXBedh5SQC",
"1LSFSNfgQkxrcCzcy4XqYcuuRqrpmZk5r9",
"1Jvj6oPzRTGJAezyZ5DxLJ7DNFRJjeVVaH",
"1Nv92sJXrmTZyKmozw9gEiadi3y43q1QPu",
"1HD7MTj8ADywAvjjnf66t9oXeTCWZrqYkC",
"1GiUXdJUR7pSSGwpFhpJ7zSDGsQRRS7kJY",
"13P9FtVibQSsUs1PnM7RbEUAp34sUYwzrF",
"1PwJ1MkqEps149jRkAajNTUyA4jY8RGSbh",
"141XnjBGYfUwpMpMHa6NYc59kbonboB5Hm",
"1G3tX7bS3T1jo4bbTPPBeYEZN7bfiufjAg",
"15m7thLNpEB5M58ZUaNPiWdHHiS5iSH898",
"18hcWZ1YaK8RKC2WdZ98Nv8y1Xyzet5bWW",
"1gDg1T5sFLiEgmDHEdqMbZzEP29z9Rnf9",
"1C5qn9DRyTc6GG2jftgLx4vuSpBPxndenT",
"16T1tLDfMMLGtvZ6Z4ybWw7jG2xV2W7ggh",
"1N8Tj3cLuqJQiaaGc8dbtbMNjDTLQaCN9",
"1FJZrwvvS38AgFHBFVJWMRhWFe1jLbj1BY",
"12s8FbwHduhDeHYWSXsRLsEcoSA2d42b3L",
"1CmsneYNcsByrUULY9MsdZizov48ksJ55C",
"18NeExuhmW13Sn6hhCrW5DoCL7rQTixGAP",
"163xBwHbCpu3RwDVtiWMWzdzGhN6sjYHkF",
"1ChXUstP9FmKHbYn6aCCybUa6ejk8UqJmv",
"1NQKXqTJooG1j7puyLcjncMGLik5cYFxG7",
"1KEWPcq2MMZHfDK73QZkkKw5EvrrYGf5PX",
"1ap5SwsNAPE7DUX6eocVS7NUVrLZUTGQP",
"1Nfy81nryStogKMEsvqj1fnhUJQesDzd42",
"1RSKhuFHRV9Abda9ehDrUMctdeAmtqxoS",
"14aMwnbAKQS8noj7ka84D1ouqTdz4pgR3Y",
"18MThfU79inYjEhuoRFFf57cbjqhvjbdtE",
"1CLBNyP9SQV52atAvLHgxkFJUQRKhH1mFa",
"1NRX8awzv4azbGWG1aWNuPZ11rPMeyKSJE",
"1BpU2cCtxQ3qorPpFDQN9wjnuv31sw4r8N",
"1N7pLG4K6PtdomGGS42r4834BW3FxUNXkD",
"1KQ4vi3sv5PiM5UTqw6BmYAhHtAmMqNCMZ",
"15caRejHCtBRFWZfWkTJR5NqQPVFtnXAvh",
"14kL8kBbdoY5g1rnNyx7Rgtt1ovYXRnAiK",
"12QkH1C6zDzjVG3msmeRQMXkRibVuP7ZQE",
"1FPZv4rPTtPrt3b49awVVypkwPHeCiX5ha",
"1FTiiB2Cfryxg8s9CJutswMAJFyFt8o8yP",
"1PfFUPLTfFUKhkDnGGUaMpiNWcpMmUDVoe",
"1C9HUGufy8tbLQTKzV4jvooVWDLraaBCGH",
"1G3NhAowu8BAc265HHniL3X7X79kUXn1Xh",
"1BMCsfsvFbAajSUcAjxtRZEudLkoC41N2m",
"15Ct5A5qPqf6Zmpg3xXMdpwm7kThwvyha7",
"1PKgw8oCTWvpaRcketqD3HWAaHKBLQPUdA",
"1BPqAUrDmSqemdT6zdP3Cr3143zXBwprFE",
"1MVdvgokHBYEy2sQghVVYg6NqNn8tuiwuP",
"13iM6FmEQ5B7TvkKK9eXtpm7Ad2sh6CSPS",
"18Z8VX7R79kYgm8WwDjZDCC88Bf1pSz79H",
"1EDwXpRwaVR8oXsxigj7262VqbJAusD4Yt",
"1Dof6WFkCFjDVFssqRnyCTDKyycNM92anE",
"17gnJh4nmGfZxyagw59TT4y3kXvhDeRKTg",
"12YkicCKjevnppVVjuXN4zmXbEY3TJULFT",
"1Q2aqvafY6fT7xGvNhneCs67DQktFHoy9g",
"1N9GSSEXfKVrqH3zanQpEqsmWDmdZkJMMi",
"13VN512Qnp4QcyMMJ7SDL2Z8muM4mKkdVh",
"1Jrz62AqdvfXSMmaP7odHuuf6Spg3vU6aZ",
"1CVj4QPQJtANCobMPVtnxA9MoEYxqqPxUi",
"1Q43wMut7cFJMfVq9GSqvepuUxHcdkxb1X",
"16wEabsj355F4TaDp5gCdumsm7jpvvHo3Q",
"1LVUDdDgv1QmeZ9e28cw1KQGQ6ceW1nwnT",
"152NVeDQCUDdvXPmTAUDi7TaUdBzPQ6GSJ",
"1LeYRMWbCwaNgoEYB5SPKea6Mx4kUuCVsd",
"1KNKYDM6frgYw1Eb3pht8N8vtpimFAruZi",
"13TQ3M6FiWaSaAzJ7xgCvSFMUHrc34yrFc",
"1FPP1EqpLYBUak1L38GPNgBH2jeb9uu3q4",
"1NXw78UN8GrYmKXDxJ9uoYP3Gqxq54yig7",
"1Cywb1zvEbszecqusz7cqmivkUhiDQ8d4U",
"1HBeaqv9ez2FMoPzPScKo2fD59oWeARoXQ",
"1Gjr94X85xbFHWCdPgVG8Y6VLw6qWBMaaR",
"1GihscQcupnrpXgpm3AEWeS1V8ydqzwtsJ",
"1jKF9c8dHpRQQp3W1g8BSrJr2ThfxYzxL",
"15zTadYUwPP4tmQaWjTkv7i28WPrzKMNCY",
"1EZrtdNiePxr6ADFaegrZ6g2GfrVhzrWeS",
"1ND4MXvLQKoHHb2geuW2Pf39Z9RB3vVDBY",
"128S2e6tX3gA7qM7EPDdUB8btwcjLHQjd2",
"1NVZTwZVzsHg7j4wFuQ8UqhE2dyqkpCQHq",
"1J6kKXaUuQ5FA6Kgf1KoiU4Bwnmbb3Q1rw",
"19aRFGaCQRBqKG4cceFhiF5LRm4TgJYjTR",
"17UmM2JpzJKJokcDSFVmH2jWvoQaivqfNd",
"1BCYMxzXdJsez6XTKGQvccuVqHX1sJBm8u",
"1EUNMAyC528bTFENDnJWgtrtVGAfRjMMWK",
"17auFSuZuuXJbvgoEiZw3EQJ6jaUEiWjxS",
"1PmMTuNDFWE9VJknDHkR4dAGJD1psd1sjC",
"17USmZfvLHxUq72LtTUKFU7jsa35ueeJx9",
"14xnSUvN8woMY9QPDRzFJ1sxDBmSyvnfvT",
"1HZSB3FmrPEAR8UtS17vJMfYWWS6b9xzN5",
"16qfbrfwrguta2oHVR3W1CgVR7NpKSbgpP",
"1HSkyhjvYtN36MTvYdmSXYPZLJfrdQY5G5",
"1EWV5BMJVaj7uwkz3kwb2Y5sFY6NoH5N5j",
"13ieKvFPnsupVLtHZ77dL364WxnytaT99j",
"1MgCtNTnDn8H73ZNJCrB1Si8q1CXPD5omG",
"195JeoyzAYMu66ERvvbqn4ayivSXfwXFGh",
"1H4xC1ZVtMP93S5PurLnte72QGRNLY6VJM",
"1NTG9LuYEYU9quaH6FkdLwtx3aomB2S6iX",
"1GBroznxpdzPDr9bVRQoLALJ7jTx9vgn3x",
"1LkkJZkfN9bEWtP2up6roxhbk4ashPfgVW",
"13H1MDXbSkcq1k2sYm3k2HBogusz1gHFKv",
"1JvwKgcis5f2iJ7E1ep4XStWkdrESxMwn9",
"15FcZnueheTnfL48C4M4bZRz81Gky4crvq",
"171Dtg5Wx6tkoJpfHrTkpLmd3hHHAnAgiy",
"1G1mUuwyixB2QrYxG5mqZ3n6MGhLFudMA1",
"1Em7AhuVJt7o2i36jTWKjBSYDj1aRHgLrq",
"1DeqfzCoRDWAGf6pECk7XsXpTzojBqkLDJ",
"1M3JziLpJycxxbJLkh7AcQMVnmjLCo3bGq",
"1EsXRxtApU7kPZezBpJWU9hm6tucUHEdfS",
"146SRufZcNPFGdMwadJMiAKHXd7SEneMRA",
"1FyLbC4cBppFphpou1tDHVq76ntqG4vzdx",
"1JvCi9YKUvDBHnX6D3CdngdQPzpnAzJ3r1",
"18E74SJ6XwppvF1Ho18zfixbUR42YXgreL",
"1R7LewL6fA73nKjBkSNabH9pMMnDErro9",
"1KyXBdHJNCmrsqkdb8byGTszHvpmMNuXNc",
"1DXyno7KpLKKk8TxiCeD2pyQi5p6YcXVK7",
"1548u1FAZvAHk5KUSNANs1NyhSAf39vn1A",
"1kL63ewQGsAHnpwsNYbvh5gapsatHTPt7",
"1HDny7WCXrLizzW91vJoo2z3KjhH9syBpT",
"1JnLXLhnuCJRQEV9v3ZYSbfoReDA4utQyf",
"1EpZsgpw8NJVnYhBAdEsoC37UhWpc6xzPS",
"1EmN8QP8U32ZdZDPVDL7SR8fpvTYUfyyoV",
"1KcTCBHLNUHkgHEykS5RSKBHK7sjCfWx5j",
"17tiKZWrtiYmRxS6eE6yYrPedtd6qsCnYv",
"1EpZKVALiTRqPBexmaeBtuKnyJKyFxJVN",
"1Bv4WNojwgwruTfC4wp7aim58N8wzpekeW",
"19BVikLABBxaCaUdTyX4E3qVAZKqc5EXLh",
"16okZ43iezgksfzrSWqrwKgQ8o9wfaaVfH",
"1qmK8cTjFq89fKsmersoZhtEDbmqtqony",
"15ZaDxCt6ajzJBa4X7kdQAtBzweGtLYw6C",
"1HpP69uA4LUfGwxUmqt5LpomB7FFyxYukA",
"1FH4CZsefBY7NgZiWWW8sdSXTJkPM5i8s6",
"1G8VUHCeQNNGwfaDaEx7fSBz8GB86NuS8p",
"19D4uGsj6e4XfBLNzZTK6LFHUuKFBSgdDk",
"194PEp9DB3SnFu5kNmLymx9g1K34CbdgnD",
"15YeAEXdceAi7MWo1YdMLT6smzEV4UPJFw",
"18vdNiGr7qzp4ktrW68KkP15CwKuGEjFhx",
"14sY5emmzW4btJ4HXquKnrAmPP5NtnghF1",
"15ScD41TpQUuCQ6cCC9ebfsBELDopY9hge",
"19UzLV4BAA9BFyH92a6XstbddzDjnkXkvE",
"1Aenmr4jiovGkGWNig1s2JuYbAvaP4UExi",
"1D7Sz1Xw7syVM42eyzPEZjvpTu1jByJDap",
"12VWAmKnxssQKWjuFqGSx89FMicbWRj8S3",
"13uqwBgT8SLHKaGe93ernvUoKCRRDwvFUQ",
"17f8SXaycZXdG5X7ZTuQ9A2tbxEYtFSP2C",
"137THNji9SA4SybwbqxAFVS5RgCGynTTsL",
"1K23HFTaMt9w8XXBkHYxuH5Q9uLj6x9AXr",
"1ETK7XisDtFHAbKv7MWqATtYq8rHYvKUKu",
"1ERJL2fixF4b1jmfcRygNuFtMzjrGhSER8",
"1JykyAntAs6QuMpRAbbTtZStJiqmZRd66V",
"1KDf7QXNYjwap6TxhecKSEWgDm1GigbS8D",
"1FxdSiVXUuPFzyd875SmJoMAWruwHR6bbo",
"14WyDA1jFy1tsdmUEhRCPUFwGSrdmKZhGa",
"145La1bKELx5eowm9puPkpum7LaQ6GAjEB",
"16dd2DZyW4VvgXZPGwYw5PPkVsvob12Ycz",
"195cu8rLGJhvV2nTsfpHNJQhBfRcMtzNdZ",
"1JnFhNDvpWtj91tbZjTBNQdoxXaPFVH6ov",
"1DP9gHhjokTQxxVi43JqGVY8hpkNodFCgR",
"19bEq2xjWD3Gj9q8F9wwaDqBhEdrfDFWwh",
"1D9nXwqEaFao83xZE3Ac3BGRWW9PzpmjtD",
"116F8BDe3ehfZwoynCyAzGdQtTUcDhUmux",
"1AHqTSDXNLZbsUnadeb3kQgYALknxi7v3",
"1MVcuzsja6BjR6hBm3DMYEFU2iXXikbocz",
"14PcxBzgpwXufgEjpyybZLbidvD2ZbLrMT",
"1H67sXbjx9qsi1GYRBjNZfzdsqdBqv9NR2",
"1DQB2HaVdszDwD6Fuytfq9QhHtommgA9BX",
"1BqCrXoecYaCVqxTmbgmFXsKA9vUh365yi",
"1EKZgH4peKNANsV5KfWNCSSbQwfvMixp2a",
"1jzGNbBwc78LDW1k3PXWdhm1YJFg1rceA",
"1Ktdi6BoH2DbGC4sgfNeayyRTog2gz8Xxr",
"1KiNUxa8e2iuskNBczmHwwPAVRGUKMnHBZ",
"1EK1um7DQwJ3NCL1HbWSaALfyaXPktEzbN",
"16MAr66qG1NS7mPW88gxzXrNuHwWCUHriD",
"1ANJvziU1c5WpUZtomBvGch5aGMt153SUb",
"13VtwFhBw9rfxJwqoxDpwjsFqLVjb5VXGy",
"18JnVNfJ7AydF3nBxpowz5c4Ha9u1XMBBn",
"1DxnrVJ4DBMrFR8TYFmENc82ej66QKkbqJ",
"138ugSgN3XmP6VxKqWEpYszMyrXXTzyHm7",
"1AFurXfHo1p2fcuvYHjuH4JfdBp1FWA7HJ",
"1A1Y9wBtzEs2yB88EyyqfQG9JhmEhhcQRm",
"1AbB9Pzk861ufXeccUMuxZPRK8tLbDX2HR",
"1giPYw72isA4gdqsndcnbf74CPEWa6xuT",
"1Hs9SDv8Xuhv4HapzRvn1bsnFNg1EHY9XC",
"12MQx4qjSEqDA6eXrx1DA3fwX885iuEVgP",
"17APxVLYJB6v58sN6WNkxVyc4byNqw4M1m",
"1JLdYu9WAd4N4BW4LcQmmqfPMK7wsZwKce",
"1Jo8rg2jbDF2gQRnBPP2xwpeviKJ6d9Mok",
"1Mn97iWhfkTwSeZ9EvGwhjLHPE86RPCNsQ",
"1HocUJeCVX6Zh8mMqrr2vzUV6MV3TqSRqa",
"17TmHrwxVBrcy6msqB76Qhpxd89P32YHFG",
"1BW7BzQ6Gokh51NyNV4Aq7jRwkgbdQuXB1",
"1MHoRJVYqtBfh6weHvHM5LwuEiwMBKwdqP",
"14jPCazKExUZusZu2dTnJpCg8nnCb8eHeD",
"18R4MsieYAJ4X1sxwr1F47XxfzKapzxKMb",
"16rQXmjBgrHjgWJ7jfy8nZPNGYikqhhcSz",
"1TddpiHQy59NNXdA5kPVC5JGKuyGphuXu",
"1MM44X3J6JwRxZPimWyPPiSgC6sGEq3Q3E",
"1LfXZBdmsNLRNeqQV7ameFTBvw4j86Gnqy",
"15rK2DYCvXc8dg8CA7UbDXJRLhTAWBaCXm",
"1NhGVhex1ezq3nRMiLQ9H7PRdKMBgXLomm",
"12mU8Py6DG7sZCG8ktcmo3QF8GasHXLgxt",
"1GMN7cavVBPd7iKWV9vbnbvFrCrwLyiB6j",
"1Dfji5eepNsKx4pXGL1NdyKSW5pPDxdBgB",
"1Bj7RqhoDL79WvhVP436VFgQ46Lg4AmMDs",
"1EUM5SrEirptJ8ccMctYCsvJbtFSvJ8V7q",
"14qKLhHKjehSqnxncBfeX5uBERxJhrTmsQ",
"14Rkr7jdbSkgcw8otZ9GfX6RJBTDyDk6yw",
"1CDkfuxTN5ZZdkUMSL6snQfVX9X7asgoDb",
"1qVMZXuWZa54ZPzwwSq7jFEXgNrutK5cN",
"14vGJdqg5b4noUJ1QsGevz2wBdxECz7gPy",
"1MZY24mdFUf8b8Lo6ownYQpG2GXcBR34is",
"1K9ppGsYTv2GGvp2orEyTRyJK6jnpGtT7q",
"1Pa5Ucghsbxgoj2WxH9CqbdPBLnHqNwih8",
"14dj3wJhZjLogMeodNBerRTAGyR8MHTb8A",
"15hbMjMHpLUxdyetH3MNTo6Cr7xvS4j4KD",
"1MerVcPw33vsaY281WDusBs5Ao9Hmyzkao",
"1PMsjqqamf1xN9ceZz3gWrSnxCaUQP5za1",
"1NT9XGFCwGVCuweRDn9sDi2ZYJKEMqqdU2",
"1K9pupWPXTUWRM9NEL6ArmQddn4JdaP232",
"18hCHvJmjtmkhmAg2c1ZpNipENy6M8qj7d",
"17bZnNkVF5vTgdj6QSSpcEK6VEFyV5j9tQ",
"1QJQqiqSErc28qWDiCkFJ4LRTqz1Vd1kR7",
"1LRKNDzyC3d9FGaNYTgvMza9b6FxAq422B",
"14WhM62NZkgDCQAwYVMpNJinU8HFV5Jcvz",
"1KEf6p4EcddYcAc1Q993T5sqT5cUdkduWn",
"1LNtTJLmo72qS7uX5p4NgVVSrtqjKDYrbk",
"132EdCHGswiXDm8WZDmXc5uyE49VrZ1XMv",
"1ChgcLw7gt13DG9v5da5jn5eYVyKHWYcr2",
"1KYeqc61464Epi2UDat2WuecBZE3e9vxLW",
"18mujV7wG2XSjPCAoHb2LeU3fFaC4G2AZJ",
"1JW9aVF647nvZZaszRR7q7ahVTWv8qGwvd",
"1GjgKz1HhrMyRdeezzBwyfDBgx7qGXMGH3",
"12EEbijEnsLd2DBBpoP1XUrX7pFtouucsU",
"1EodFb2dqvVnowTo7DvA5n4bL137cyJX1p",
"1A9zJTbYQDYbEQQQeAPhHdCU3sXDYpVjan",
"1AnhZWNtuLGceYdLXaNEBKfg5TzSLRVkni",
"19FVRUPjPn3pZZuHqR2xveTjE7tLPCX3d1",
"1Gg1kWtCbheRGbN5sTergnr7RRVTS2Hb6w",
"15r7PUpNYsvB4sMdhA2otg5KUnpTo3wQYv",
"1L2keqUr9pQBbL6167KJrX3xETwx3bHgaf",
"14dTtK62GKw9cyQVfJAWvPjdvhdQ3szFdd",
"18BjMXQdD2mcAKvXuckt2oLMJtKhSjveyh",
"1KJZoRQYwQaHMot7YHGRD9CS6YSLvHfU6q",
"1HBeCr3dtRRGyw5hrzjmGcfYyKLTM3DeCc",
"1LWJddhWfzvN8gTFWxhoQvpH3yChEYThea",
"1GtDqEJcqSqdrQuir6UPLFfPg9Fpwbb2iR",
"18ZtQUJxazMFDzoQU8z5CSKhhU6xwY5xjF",
"17BUZxHmjecoEMYEtRAtxTenq6yHAp4eSb",
"19F4cobwmugx8CsiSJkXG6rfnYBPN2wpFA",
"1JsU1rBojpXcXwo6Aqh1Pco9BJ4Q42D7T3",
"17KyfZDjcq7hoRHe4YgP6pfcurCMTUSHuY",
"1DkPeWeV9yUUDioi1FaFLBo8R3dzo9qhEd",
"155XWfN1r9jMfGAzEYZ3xW9V9xZgfjiiui",
"1GcP6VJZx6kWYAVw4YKiNc8t6nqfo7Cwo",
"1B9RxbGH47hTdCPJmUg9h5Ridwh4dhdQof",
"1PLNVgrtaxPSBE6tmFWXuhoQeUWDkLoeAz",
"1P8PzrYfLezvYLyZEjY5yRiBpdEbrhBK9G",
"15FNWomD9XmrT2MGkrp2bXP7uza6NAPPrv",
"1MtU51xu74AzYpioh8HMqfFofYrtdf3ajL",
"19FjmHDtC89VK8hSxoA7x6yv6LC2L8BpXg",
"12yTfRhjhPENmiBknkv4aYx4f3za1PMHay",
"16MLkHaJJcAYkFqkeFCcTCgUSP7CqLwxMn",
"1BXcmC6ftZy1A2Ma7pstiYhQe2GGvrL8DG",
"1C9vf5nJdz4Yk5TuaNkDJ8z3eXMJ8JgbEH",
"1HSaQgjw2rRXHJq5TWM574sphas7usdQRR",
"1KY7feHcWLNEiLbvSXjFnMztPdq8cXy8JG",
"14VnbG8xxAfn5FiA8AMprD8i3GCexo17rR",
"1MQ3Dzh9DZfuAst8ebBjp9JRSuPP1zkzYt",
"1CYm2id5noWGcygScoBMwMGGMaAs7refF5",
"14EwiGXz7r5QrSDhb9mpmX3uaSNFCZzJmF",
"1AREfrmfRKNSUzDLEBBaXC9wsKhqf4xPb7",
"1Lg6fSHXJmvvDvcCzfcvmvXWzMveiXw44y",
"1KrFonu4nGbCdgEKVuih29jdaHe7TRAXRh",
"1JuGTWAxGG4aQujrhJx9jRMvzEENnRETCP",
"16yiEd9YB5wyTw6uabzLBpTZWL8QvxUSnV",
"1E3KWjhSWK55qFZawqYTSHoubE1Ks6miDF",
"18CY6GwCfDQJpftDmR6jZivd8KWnUBKhoQ",
"1CZNbv3ZZAPNescd4gQbGTudy1EUHEbNoA",
"1B3LzePXetuCqjc9wM9VeY6tdHNZQwUXv9",
"1A2BuL2K67eEeHWkNpo9WrfX5ppdVjUsqW",
"1JRiuYqQQeE8LrgAxBKUE6DmXqiw6PqGoa",
"1CcTB9yaeVNnRKTsCMPD21MH7nmfenKeFA",
"14dShp2EFpS4Y3dLWZquMGuPrHmR6CQiMY",
"1B4CRczK9swYKHUWACP2EP9TmohNbm4oPn",
"17gjYxK6xp5HLCam5WQ1NBDMsoo5YrCaSe",
"13jrorcZR24jzqXjwgsGRTBiCxszn826gC",
"17kBvhw5oJWpsCNwzsjzNZgDUpjGi47j1F",
"1bCsNuYiW6XGA8hFYvHXcjnUbKsXefTas",
"1Df4e6HpjeKnBPFmjWRiYoQCgqaQzYTd28",
"1LjUSLkXKVgFob4mJxyg4YNa6kCkKNDAQL",
"1C8tp5HukGYCNy94aALZFqXDYe88Qiwb37",
"19WoizGyniYCAci3QbnSxRdRwXYdqLCmsN",
"1Fr7vdWhTqxof6Bw7SdXMbTMvUPRMkkKqs",
"1N95rB7szcgj2twJYukriZ7txDZRu2tWRP",
"1GWzrPG7h55BpgrZ6nAaMcspXVKDuQFUuC",
"1J7UZvnegXkMot3DX7TNqcCVCGLWfU67x6",
"14PVb66BcBMabDWEunMh1gwNcN7eBMDdeA",
"1Bs19eWyAhqUnyqJW6Lb39JepczKGycYVn",
"14rxJB8yvrXNwDCkvVFK5z8jEFQSR9eQF2",
"1C9a2n2C1rJwnmnFThNuwN1ZkkwSL4Q8AZ",
"15vALYFBXYmzUiCJWL7TfCTg16r9jrPdmW",
"1H6h4hsirpgPkXKWZ8K7EKtm9jGgqjK4g6",
"12T2QNMPU3Dcobye8Eirr5wPf4XkfN5vmW",
"1JPrdkaKVq2G66JsofCUAA7ju3vtbRyM29",
"1AMt6Lb2Pe6VVwHzr1Mt4DU4jg5BG9w4Vj",
"12wJNGQhvgRMTo27sbTMLUuzNyRXrvSFpo",
"13MWisUcPFg7YZZ7dErMnD9SAdoeFmxpcY",
"1Bp3XmnBsFuq9fv468sNStu6X3n77ppLih",
"1B985Nst19GR6Qp7dP12zrHuKNjS6VPfTL",
"1P1KaP1K5M28e3u3uMkCwa3aNVmsSn7qLm",
"1Q78Ygb7V7wyi3ncN1FWXkJxs6LeNVKN8F",
"12ybKaxcTCS2xx6QQDk4huEvmZmAAKEf2T",
"1xvq5MWCWgtVJ334cecDMCcTAyHVsU6ks",
"1NhLMVUn1ubUGzZTS7DwA1bZnSRXCN9ZRL",
"1FmtWNUdUFYiPvPV74prAGdrz1kP8rTbPY",
"1MPZPash2vAxmqE7HR2AfqoDzCUkJ6LnGm",
"1Lkxm8QGziXNq6UUX8FmjADtTYvit3dYSG",
"1CHqPmmcJn3mtzjJs9TG5DhNY26H4uD3pP",
"1B4zbeakvD4snoiN9ACYWuzwqFwYVjwUyW",
"157fupBqP3k2ovJhvPZ9rmGCnuAikPS7fZ",
"1Gz8vAotT1Smesp3TGGfcPci8NEKBwVNnX",
"1Q7hRrwtKFoEJP2j6q3GzagRwBVSfYp6xm",
"1DP8w8LNWAVmHLoh4ws2wWTC4UnQqtwN17",
"16vpBmcKBsrie9CY28JatYcNspXTQmhyN6",
"1Egw2GigWeuiTcRn5JJCzjUXPWAkxNM4ok",
"14yKqMAXDUVhN7YdPCSHjpjr1ApGsX9XMJ",
"14hMHnqREG8yq9X9YGDzBCjnhi6AVgjbwd",
"1JykL1nVYTBqyTjdp6Yt3EA5kbwDKcurVY",
"19h2uwfyCftojnKd2Lo2et9icoiK4VJefr",
"1FzGdJsrDviyWNGEUFjzr57P8EiYriSPcP",
"1zGyPkiGzEGozLPK8omZUSbZ9ZELTRP8N",
"1rPJ19x5CvJoVDmX7BAUn9aSTW37ge8Bm",
"1AjjZ5Wu8j5YjRBeJRwLxVKuh776NfyAvc",
"1NtosFSt9a8bk6YNGuGCE4sxDriTt7G69A",
"1FUQj7XVHkm3kWhPUnooYaJvNTmBL76sU5",
"1Gz69uKQtfcCHVGj9wyV9m6vAX7thw96zn",
"17LijTZtDQFb7RBqWsaQ7NN4J4FHpHCLPr",
"1Km7yTy9GDV3qho2Nq57KQtnvfw7MmzCk4",
"17cEcuSaF7XgsR19inEwjSNN1Ehbpni35S",
"1K51tc9EoZdg3NjvLfQv3RNG7J8ZF15FZs",
"1DbVAtYQMSjm2mUR25GvnmfXqJnZhu72kq",
"1Faak4eumq3AhpnD9LrN2D4FYTtvzPkYN1",
"1ECnFwuXfosHJJCgoEz7B4kAQZwVi9g4Yg",
"16km7pQgoAUKXWyEHpzauypSLMzmAJ34gp",
"1EmqEPK5kxZZjawHTkdDCum7b4yUPSU6dC",
"19STuG2kHHyGtJyT2P7ewykwgfw4SD3ybx",
"1DqSF953HrWoSFDqZJ8kfXh2RdcwgFktGS",
"1H5eJ3QGruY541dvGtwpeZxERU9jnowuzX",
"1JyESVzFQ5XLGW1HxUikdjZ3N1jLGK9qSo",
"1AghJrkXYbgKg5LEesAevYehJ3fVkrxqkw",
"1JaHwE5vYb4QftG3jqCQASeNKnuazYYaZn",
"1Bnxp1ghCHnvU4Tw6Gfrapx7bDjMy5Sm6s",
"1DeFRmzTfKAjmmoCnAXTZ68znbr85ouEAi",
"13MnyBCWrb1ytcdT1PfNcHK1xwLFueWc4S",
"1N3U6QhvTxeLfEZgShQPJwhWQvnwkpMiE",
"1B4dc1dxMWx7v9ra1fSS9zCEcbMF6LLb88",
"17KbDHFAA7ovFQdzMhLT2wzwwQnSgpXnfK",
"16nv7xusxLL1ufL1nt5N7XoGDfrF2WjLGt",
"1PSoxeZA5ogKaerBWBs78gJD7qzSTisTJ3",
"1944YCYnDWF9VqctMQkq4efqJGzuxBTCRa",
"1Lq8tM1NDWNPJAU9AdXgHVcnRboH3LoBiD",
"1ApMCjd3Z2A9oLtsG84SHjYyUtiBqFPe6e",
"1Cqcf6EzqCymG19bSnhGqnAtk6V9jfdcvT",
"1ALLbyBzQtwZm5iNU7e4bghxdnZtECNJ9G",
"1M9NRJheogLsFMQm4vMqy435jsic715AG6",
"1MmoqubFmdVG9kDVi7KtfgEHXmyVBJFZH3",
"1meUSd5eZpSEPTRusBzm67DXmJgS9fj6x",
"1AX6pRqWU2vAwpkk6EgiyS5YZBxjG3EFsx",
"1EteaowcJouXabf17nniAtiRTQuWfpdqbx",
"1MyXkKiH3PkiNWLEUwctoNhCh6byf6Jqn4",
"1CoSydq4QhAdXhPZs1tX8gAmr7TQaCG5Tj",
"1MeDUvy9HyckRdBV9YPL2e4j698hKVViGM",
"1PwzXrXBzzRK6RBfoSYTBUdkugwchvBWhQ",
"1Fs4Uj6Nztfu2BwLZaMjTznu8HjVCk87cS",
"159yVHyNVXMNy5C2eRYUHhm55e1po14tJM",
"1CNcoi4LPnTfZA4kiCbJoGJSqsitAhP7NF",
"1UznG5Ab6WM13DVErWWUnVKnyHJcaLgEB",
"16W48bi5zDv7nKDLkCNCwHQFcBysmusjLy",
"1FvvJPnADwwBJYJg8hH7soefae2gCkF7pS",
"1MTS5cJzoTKw8uf9eAqJFTiFHDyi5GDB7C",
"1MnGQRc2GUCFyq34FZ4TKF1c6vKVt3DwjG",
"1Lc2jNY1VXMZ98NaNr9CoJFBMQoBVTtf5H",
"1LwMMuJax55hnvgyuvRgk5pwH1BgXDyb5j",
"19yTCGrGD5QhmcuuGznRRhHm33mh9hcRa2",
"1BWC4usJoc5wEnuePMRkegbHY7ayYskDjy",
"1BpLbNjeFJeFbYsT2QZL6acTb3QUrvU78z",
"1GhKPWqVeigJWkRBPZ4idYQmpeXkrqsUQF",
"1CQspuceaT4jJDhhz3qL9nqGq84SQ6jHFY",
"1FygXzQwWaPk64nxUCcG9Y4vdbdEuh4Fz2",
"1NhdE2eFcbXzuVw7SWp7qa1AgFgwRSGEkQ",
"1MgQ1m9EtWh8ZpCKwwHf6oQntEfQSc7QfG",
"1ERL4mtoZR29grJpJNmmbgmND85NTSR26M",
"1M36QHx6JuYzBZ4wBjiqMbRyqeMhsG3P2B",
"176MVda4sYTJXQrT5vg8t3W8EHnT6tbtdv",
"1MG9gNs49ZTt95Gww8ozDSoxN94H2Fzhop",
"1FNfgT5WrPob314V6Aa3vyjppF9GNQSJiP",
"1FpWtavRjQYAn6iD5Wqv5i93hJt36spZBR",
"1M4DtpcjShV8EuertoQ2JWcGpytxWqXCwo",
"19VQ2VqCJ8bbFEDsKbfrgxzMbAFyAKza1J",
"18oSvkkH3We8QUjVakWqTK3fm9paVsPpNu",
"1BmxiceXRficD2JuxQ3TVxuaSzbyekgt5q",
"1HQKJYPc5Bc1XQrx8xkjUMzTNLzS5jgsRL",
"185Bakk51nhDV35w6cZUWukf8krPLxssyM",
"1JRCUExpv4CFfQkyRqcf5GpkmkP2j9D3TM",
"1LCpEbu4mVA8VA5Ti1wE93LY91FKgh9bFP",
"1FpPJmzF6sr9jgc7x1QA9vCGrxPLxvit2W",
"18uDUFkhZTCHkihWsfKakMRXG7Sk8Z4WsE",
"1DQy67C3PExRua9UC4a5piA7zfyN9t2KtS",
"1FAybG3q4gUd1vM3YeoTDyuG9sVsZtEdPz",
"15vRhiBvfEWPzS1f2ytA54v86mGVxV7rpt",
"1LhyAaQ5kdYeTeUEKG1mNSMmxBMNhUicba",
"1QJvwLu53adFNdQgnSiwfXtsVLYm1sG4b3",
"1Q1r3Wz2RHLkvYfpeN2VMqbary833VGmtx",
"1CUoWsgGjMoT8VhdvLoyBdVcpxvG3md2P1",
"1KRpQ6XgbnAcbk8hqPiQAgxmRU92cCzECn",
"1GQWHtpxLZFQzgRW1KEGLwVJsMvs27fsEi",
"17adaS47Sup38QSm87guecq63Nvms3yHLS",
"137AZZEAPG4cs3SRcfHsMadLY27ozjbSj5",
"1Dh9CEERaV1qDxueNhGb63kmPbrvsL6FaK",
"1PFui2CGee4Dqmmj2swA6ExVoxm4Yt5hDU",
"1JxUzPWu3QWd9wamVVgsSzUPhkN2osnTDt",
"1LQhvZetH4EGTtBrT7p6L84sCYqxcsLvdq",
"1AXKEp2AoxNTQAU3sDpi36wg5FaZjMHSHQ",
"1915rLsqYbEhqopkwuqAFw9avg94W9AGt8",
"1BJeCfsP1PSrSDPrTiwcGWFLG4oY5B8Ujb",
"1BTMKvRjXjn52AgNGii8t2G99hWtrYfhy2",
"1G5RyRrypP6GuavmyuLbUdAUJ89RQRjyms",
"18hDeZyEir1NxaFQxx7Z6ZmzLsxzkDukJf",
"1LuHpXuxMMVRsvy5Nud8oTtGFrQaDAJ5uz",
"18CPkaEFC2TcLuQFdfmrVopdB1cBG1Ujt",
"12gv62rGa5B4NyPcJxQsXSsg5jiwcnpjhA",
"1QKSUtaK2Mjr7bm7iZFtVQXoBQfVqiXp7X",
"1Pzm69U29zZJqj2NF6xss35SVXu54XCfiy",
"1EvqiMgtjwb71gJdpo21uhqSQxgWLRKKYE",
"1MZ1mvEn7WFo3m2gyoMHhuST7B42k4dpeA",
"1JyR4Zw19z5nvcaS7edPwedv3xb7DTTKbH",
"1NAs8AZ5Bj75SQrvu5zhfyAW2XNB2xXmL5",
"1JFp6H5Sa8CUzCxqE14VMBH1BxCuXUab4B",
"18fakwWq1vBfw13XPwpskvuXmUWmZxiXmh",
"12xP67gSEvvR6czxXqm8VyL9y2nsFC3J8q",
"1GRSctofSMK1FxvqtDaQ39ynmW2XT1BZa7",
"1Bt5DBneHfcaM48rdwMx7AhMzSbgPEYk7G",
"1PBgt3yfcN5PYqxkk9vZNNrFjNj6m9LXuM",
"1LkaYvdr2roARysxSY3GLxww4KQzpdcUQh",
"1AkV7RusLhn1iqVa6sR5VuaUu9Jp7A523s",
"15eWTh7J9cL3UAV52bX7y61eGcPMkY6dG4",
"1D8s6sVzg82LDjJm48tH8TYDMMggTiFbdY",
"19y9EgXLJbYa7s1eXgWsqnjZTJd7JNbdFH",
"1M2WgXj5As4ksDXU8PZ6FYxZiHjFdbhLr1",
"1A6dYVnqHSu4EF23cnYHXmsr25AVYij4UQ",
"1GGnheeLGrAcPZAgBRFYPNSSHrAC2DYh7k",
"17viGiWoaqvBJoU51QyjQo5RM7qH3dnzBw",
"19v33tfGfcasbwbKrhhBBJ4ra3zfz9ZvFZ",
"1KwwtuhNF5gcTcbY6GN9cyuVP5P9mLGJQ1",
"1KyJTekP4YJC8zeRkRe1Hj9vBJjpVHCHWk",
"1PpjiXBQeTZdNXo34RMfo2LQGMtBAHxba6",
"1Q6DaNFsBtGYezG1gvvkuGfMnn3tdrPPzN",
"19xqfwUUBniM2Njo5rbkgwJmcZKiop2nzG",
"13qT5jqJpKcBUx4h4chPTpXWJomE4UBz3v",
"1CwmfDfkAY6SdPjLteUSFK4MnS92ABYDVm",
"12U4Gyqef2q7JYWqf6FQKK8sudA32Ru2gS",
"1LZAmzc2tonoq41UvBBSh1MePdTyESymQg",
"1BKERky4VDTyuYsDfV68ZqNmwGVGy4mW3J",
"1G5HvE984Ps6CnZE38YmoiUfWc75Fv62Ui",
"1B71daY2Wt4wgUQ37Pw1P3r1CLYnaHZUGE",
"1Mr1L1QgZJyu1PG9nU24ZE1EBPQPsWFrqh",
"1JohBhb3Hb2rwLtSbWYLrXBxCcNRZ3dLbg",
"1B7eUMT2gkicPoDC6UGqquQYkWPFg4ETmz",
"16hHxWXftMA6j7FegWgqr8D7fo6YsaYAJd",
"1AEkpT8AzD2BCeajvJZYt3hTQvydPhGBjE",
"1KGZ5Cj1GVozRDQvQjQZBfN12Mr8Pa7rUV",
"1EtThxcVuQ9f2LFV6PWZ8bMwATjSszmEBa",
"1F4MMfV2ZyCM8eTyamv9krWGqBzwhpyj7G",
"1By7opvV6sHkswbojEnLxDtJmpd7N9dkcx",
"1PAfKNFuN9CpXFeayp7A8AFveheYiCGa5f",
"15VwfHXrfuw8hPAnUSjgh6YSwbPvK15dGw",
"1M2gLZgmmcFPnRhUQaamLpfKbhMo4EiVu5",
"1EeqLUTcxiHXNp9cDbkqmXhUKrcL5jWfZv",
"17UnuDLx4e4ULpvJLYH1nb1qutWaZzprnC",
"1N4uPSQh9WedJPrNEm9cdkxTEDMVmonAbN",
"1DpiBcBQoNMf46iHmH43V8KzDaWE5zE9Bh",
"1GMgBYLdCiaHTy1iwLR1TSxgsUcng9VYFM",
"1BgpERfntN6GY1yAzaEbcJQ9C2v4MuvPME",
"1Piz2hdCpYD1bpyeaAXJJK5jrYjMkPtDhS",
"1Gz7esjWQGLrumhbFiAUicWKwfjFq2N9Vn",
"16hJmYHkfCefTzwXF6xqqffrjjV7ickeJ1",
"1Dg7MD6aDvdDFpKNSQK7pa6zWzedXdXKMG",
"13JLwm7eW7xcKAuGWnAKqYQ7sXNV4nucEc",
"16TYqLmEPgy6vJ9SC38P3myWD3zwMyHhfB",
"1GZDGCAFpzKErxg727vfPD4RPMJ3zJ9d3Z",
"12Hj4H1xLkfvkFcVstGN24pYvygcgcNuoh",
"15zkXgaN41q2cyDzisWZN6wwh5uHE8s5tu",
"1ABh76jT8vxbLnKBqceqL7FTevHTxvnNiC",
"13ejVWuwUFuCnLwpTmZmG9DJSXe6krh75j",
"1NhC1vN1NdmusBPkEWfe1ZAGdX53jPfw6s",
"1Bn1ewMtAFRYY7VQPBnErbS6HMkedeT43F",
"1dmsvGWCW526kDL9rqK4py9Suq6Jbvzh6",
"1PkBE2yCWnvrWvnPJpfoNPL4nMTo9g4Cih",
"15pUJNPqTM5qEZk3yC2U7dkF1pjgGXVeSB",
"1EAe9UEdFozCchasjhFfLyiv9qJYbvtKwJ",
"1P18ePksyq5j9ZtYCAznKKZETbBtYF8c4s",
"18Kco6ZXYTZJECu27DokLukThq6TwFski5",
"14GMyiDnCwJ7WnRG1w5zGus6Z9MSEb2QWj",
"1AkatYk2wCkneXtWZ4qEhyoq7z7i29qNfq",
"1Ekj7hQgZ9CXFwwCq7LD79RJN8JBpqWqm3",
"1FZMPEcn4trKRZ43dAch8WRyifCopKiAQs",
"1GnWPws1GiKs5GfZVQC1n1pQGcXTjVLRT1",
"1qqRFd3doEaDEXVSQC4Rz6tkjwFG93uNc",
"16qMBgfFFgeo12ZpnmQ83hj8HscaRfFVmg",
"1PDBotmkDnWV2owdc1Xr8DNVRUbZLFo65r",
"1CVAy51684LyuXFWPyHLoX6NtAJxh7tA65",
"1BFj8CD57LuTsy6CULqtbcZhWZxrep1cQE",
"14Ci5bQDbGFQ7wvUAbeCYKgsJXUKBeRmau",
"1DYLZHeVSi66CXcdNQhiPFvy48EKQ7nVKu",
"14UitGiDc5f6drwATWSn4iLp5oQDHFPMpV",
"19NWMy4b9xjXL5WeLKYGj2BReAreSw3raD",
"17vGEpxYvBGSwrBojKRayBtJ3wrufaXDRw",
"1JTqTg6uEhKPyAwen8c1dSEUA5XG6oWW8B",
"1C9QJtT9Qmdoka58tJEnHcF7eLxafvrPtB",
"12fuvNSV6ujYYKycyRuf4cSJELsM6cPsvR",
"14PtXpaQQEhgS2tGKQ4JXCLN7Cqun2yKti",
"16BobcZTbpEkPdwisMwsfpUXyoyQQ7rFXr",
"1FphE6SMW3vrktcPtAtHVUmN6HWwgLSdnd",
"1631LaopYAE2hq6eBwCoi2yqgdvfytKWNN",
"1BT78qYCYmqQK9dKF2k8E37uayMn11ajKC",
"1Pqou4HCfN5tDrCX8FSe1qW98LXq4gW49Y",
"17FRhJuSigov3CqXdRE5rtwqf97s4gDtM7",
"18yp45zTbWCnVuwNvfQNdb9meLwc6TydCZ",
"1KxMFbq1EhfgVVePauD5ePmJ6midfJ5FNk",
"1MAiePKfZCjgLam6o747astAfqKfZt9rcx",
"1F62RwxbY3WEkfjmuA29KbfvqbpD8E2Zmg",
"1Q1Pe2b2ANsecZSjU3L6F8UgDobNB1L4qq",
"1HaoQVdvx4ovCSEg4SjSxgYfQiVdNYQbZE",
"1Km8sXA1rwipxWgPTGHUVixNKF9ufwMmZj",
"14YqmqbR612zpfVCcoSrWoemGq5b4haQn6",
"1GiENZqLsbQMbf4iBTipGbXjkfCvhF5zks",
"1JsiQ1QkLXwWWarEJTKn4YtdKpvuseT25u",
"141iPMYt49W1UUXcbT45zSedEXdJWkhQw8",
"1LxSeQCkoXHABxUgh6JVWtQbXSwynZKzMa",
"1Dc7hJjXshFBAeHG3jxD3zPbBr1K6fhZLY",
"1Mwm5dNTJfbEFp82MsuB5jikjQSxrXcyVq",
"1JpdeTdVdahuE5gjjcnAtBknsSjH1h6E3L",
"1BeixpwYYzKBZLv2xQhqrYUTFRnXi3EQPt",
"1PbWvrSifcavMNu3pd8SLYJ3P1dSEYKPM7",
"1KjArUNDyuxH9Jjr4zm44hvdDCiE4BCg5d",
"1FiddR2JistLuyE7zEntixYxjEbPHZ2AMb",
"146TDT1qJRn4atCVBeUjPZcaUyzoPVHKSu",
"18XVf25NgKWJhcfo4divp1UsNYotEtBk8o",
"17Rf97avvfbxyaPiGLnvJJLvKVQK47recp",
"1Nsm59xDquh3V7qqVMQ6dAKBer31XDkZqm",
"1JrxqofEr6SEvng2JhM3zfMcxwGScCsQy6",
"1BerzdHB3B7zT9eFdG1aepXMiwFoCg19N7",
"137yKF5QnJnT5fRBmgs3GU3FFQq37cNj2r",
"1Nu9BGSnJUp3uA9d3D5GYvaBnCPbM5HC3j",
"19ZKc2bkh2CJGYvVhqtD4emLTMFpRRjukv",
"1APg5YAL4x7y3yUJc9uTkViGY1zpg783Vx",
"19kyb6ALuu4h45nUb1BkShGWzu3N17R5Fk",
"15kTu2y1TEpL9WfKhwHP5yLLZzd51WeqfW",
"1PkRZJwhJMPhE27jDDbqmub8JcxTJbegGP",
"1EbKthuiN7RRk9tUy8Vwm7L3g1fv7ye3CK",
"15FC5R2SGJq5vkTodMd8nbgY52VsNXr3AP",
"1MM1pgAGGb6TTcTrAeZM7MhmzKF3MEnXuL",
"123CsKbTso2asUcRXUHBUbVDmisM5XqrtR",
"19DzcUKyZN9rHFV8iJy3rBCWnE2wL9iRsV",
"13GYLCwRzSTf1DEC2Dv5NJguRoTFQQk3iM",
"152vmj1LmwZcQS847YZstVQ5NZvgrsnkdw",
"14zPUybMEooHR4cGjryf9XDvWkWa6uyyHC",
"16MBxbhrB4Bw2kHfTssFNrH1naWWEq9R7a",
"1BsesfCugxJ5EYbGD7m3xNwAnn7gmdMrgQ",
"1DuMYP6umohfmYdFYzbFNW9rxm6ehzKHsC",
"1Ng8o6pmBZbvH7jsr9szoo2A8huCXdC4Fv",
"1Bayd7QbuJWjxRV8EdMfhaof6Wo74xUCR4",
"154pHkFMjHXyTwUYRXMPS3d44msQxnimwL",
"1AWmLMDb5cJLD3WFr6tgqsUx4157jKL65p",
"178PaJGGF737oRjBVGJeFdopA8ZgX8CBxW",
"1HpK81rLVLCMwLh1zGb3fY9YdxgZZDPE7s",
"1BPaJg3kquFzJhnoTPiKuEbFPwPoFAJvfq",
"13gHnVGc5EYGRaDvjEqF47GWr5cQAk3CEU",
"1G6LcToc8K6Gq5Pm9gvaPyxukSka65p1mo",
"1C26nZamGCsgDti6dwqBuvTxYEik9v1L8",
"1BUp9Jh9Bz6JvooYCwWtWeq5jWbxztuQCQ",
"1DGXHLu8mavg4z3XTuGE8SLj6rYW1MXa7s",
"1ECesZKzAx9bcJ979SEfEAWTwvUorVUf1f",
"1Gjt1VcfxWB9DtW7LcC6QM6ZNCukVYV7nS",
"19z2oK43Z2716hzcfkNHP68JZ9Bg7mqgA6",
"1GAGJvHzPHYCJLdKNX55zwUHRiib6kQrwT",
"18UzJRnGX4mVSb46ppNjq33CXBmU8v9uVC",
"17xcWKbxbuYjUhTvyLkrDq6ExPsgyY1Xcs",
"1EHHudQEyd2FwxZM487fmDp86LtZ3Pfq9h",
"1AAnhr8AAsXy9qnFxYZXefMb4d1rvpm8JW",
"1DrQW8c6fvphYucyKP2mgswxSAbF56H8Rb",
"1DcNBPVhaFjh8mgnxGVH15QzcooirJ9RST",
"164igWzDmcEEXLeDiELQkkoqjXThTkghcy",
"1NDDhMD7B363ySZpjBzAdiNySQfSvsQPNE",
"1Z44V9QsDY7vERs1uh6ks8HmKm1LKoSSC",
"1BaC5rnFcHaQvu4XTJ4ehXGejnhkWJR2Gq",
"144FGFDqg58DLAAEhQZHbKTdFKoQTrxgxc",
"19pwAT6ch5di5zRc1KqVFZEY46egb4Wfnv",
"1B2R2CMKGgWLxW9VVJTrASL2Vx9WPc4NUc",
"1K5vGNVo36DJ8c2vA5JP1PLNdW8uhoUTjQ",
"1HpZSCgSHsBdkt42WsaETexqdfzdWYFgVo",
"1HYNDzhn1je9uxn4wprPSGoK45kiNctSRk",
"13v54wwWDXNMw3UBJqm55CN98PgAtkRdcQ",
"1HJwLtNP7v1fP6Gzn5pABAMLHLKEwRZV5Y",
"155WioBUHzhJnnbQ29LrMnT3L2oUFuD1EZ",
"1MD4ZpWWaDiSP6d9RfbyUjeM8nSSru5TsY",
"1QJBjqaqpzqzMr3uNW3cg47G5GeNVa4CTK",
"185FNtNN4c2Ym7SKGZz1oUpJ3D1dzVPnFp",
"1GhV2ZV1rpw1K9rb1C8KoxfxWo7bKpYSqN",
"1GtLXFwWrH48Ycghxa4S6rUVxgjSB5xyhW",
"14j9g83ZAXsj8ghaxBM8LjDKw28d9PnA9o",
"1GihRC79HuMGsi7ddTc2o8QZt9XiF2x4NK",
"1JzCr2AKCd5t2eXRsgJCUxGW6xRKeTmB68",
"14PNNCJX8eBcfwtt6yP6wzEvQCF27PCayQ",
"1Jywiiz8QDMWRzM2NxT2eRtfcnsaU725np",
"1Bf94rbmRjvhjDjr2t5FByQUMohJ4RZqhL",
"1GMRpMjRDyyBY6nmL3Zu4L6UfbwAu45jDg",
"1GRbDaa9EncKFi9Hs7V59AU8GdJjFT2z64",
"1NovgL8KYVGcrE6DfpZ5MjLWEGtjSD4Sh2",
"1KeHd7jNCNPc8vy5PTEKxhDpf2kdcNAHMp",
"1LR47tfrFqrWacvqeL4RtY7ymPhrzdVaPR",
"1PRK7Lhnkr2faAuHwaM2WGXWwhw9ZMtDBc",
"1FMdYC369mwjNf5GgRoNyJaNYt9CQCeG5b",
"1KAoePaCSFrpkx3nZ4N7QqmyaUkop4DwKt",
"1GBFKBnFKaEJ84bPgCNkhmbcJkkHT1fx95",
"1NQYupxvxeTWgNkd4GpAcCgXrMbcZYWAo7",
"1H2uPvWoKv4xToiBXbg1iHtEAWb5LqTAjd",
"1Q5jyj1DAcfnXda5HW89W85m6hbCbosYgd",
"1MUKKsxx8VSBhvnr4K32CxjEYLjKbbJ2jw",
"18iShNtLydcCQrT72paqV91fc7Q49tSRma",
"17iWYM2KjxTAqVs8xCUnrYM5LygEDqeTHe",
"1AseF1VTw96LdivrG8XNT6mP92Eo8FDD8h",
"1NZQiyL2kffAiVgDCXx5kYsNWyoC4WM76v",
"14R8pgftXXiLsSD7P1dRkodhwBBrRc7fYz",
"1FcZRgvWqTGU1kungeYbeTvq9SXFNh8K46",
"1C5xnR2hCaFkJsALWdKFKCqtQFSGd38h9P",
"1BcGyBHxCHNhPPCabo25CwPq1RvKVM1Li7",
"1CogSEaRtG2zTi9KcazuF8Jph2UpzaSjuJ",
"1KnZ7RPCAkKY9JzCzarUAvwDpH8hzkiaFQ",
"1EiksxfncdqAWPVbV9u2ui1Ag4uERV86aW",
"1Fmo7eM1QZ2EfMkjcd67BgGxfF1q5PvpsL",
"14GCSznnq3RKUWvKubwJaXyGHeHHPgeihL",
"1B8KTtv8YJtFnLGhfjXZc6nVWCFVHad7mA",
"18E47n9W1E68t76C3Dm7K6816dScj3dZo9",
"1BosGqME6RtS3TSqn2dREwNxtExLYPfHSw",
"1P6eJ1vP1jXioUCpiya2Z2HSaL6zWf3KDv",
"1JvZY8xkR6uCqGYYM4S9d95g7iKEXu7pPN",
"1LD5zqfUkeKUXBV1nb2mvLvRRSccaWNkVy",
"1AKykCUB5vbhH1PSnKKkxs5An9n1eApS6U",
"1D6vmfYX3GgGAaW7frzDtmLTtW7YF7affT",
"15UTgeJGm4u2Y1DQiqUchLwT5P1UDSDevW",
"1BEGQLANvuFvLeGU7AMtM78kwA7m9BVxTF",
"1HjLi34ZnoA1AV42xw7xpayFDYe58mpjFV",
"13hvjL2VL9jZim4f2ddPSZzDR6nPACpdjU",
"1BFD8hMym4ko1e7HhvT3pom9qTJQCNsdqg",
"1A5G6mur9guV2oDSK3CajvrAT3TiKazCKW",
"1FFDbtKY5iFpVLgyceNUQMLUDZAepNcvx",
"134gJQCsEe9KzCN83kTZ8MzgN8jo57aNVY",
"1FpryAaHJzELMAcPTuEUw3gMbhozUwtJhU",
"15TQjPbtb8LrcJqQztQ6HnUnp4Nx1UXoWu",
"1CUh5idgsHqmxEQa6E9EYV7yqzwbCygM1A",
"1Ptkuvw5WNJc53fx7zPcYwgVcSJGBvoq6R",
"15zvfp37XrkzCBrHpLLxpMYbPu65eENz2L",
"1DrPXdF1z3vWPyuzCyWP8UVVwAZjvYswwH",
"1APgqf55CWjrGfxUQZyQP9Rp1rjJAdM7vx",
"15F6AhK6VbGecrE6r6xMkPaFgpH25majHN",
"1GvLRgDZvsdSHP9JvKuP1ofYCNiG3w3XfP",
"1FR1cPP18CpEa8pPd7haHiJ7zsJRCnhqXm",
"1ACijKTFdRnSAG9cmG19yBvuZFcU2qBTAS",
"1ErQg6ew6DgV79joHGtjSKLtA69Vgmo5Y8",
"121hMUius23PUpkuaZyx7eh1kkHHzJz7tm",
"16EjXG1CQ5SSMSR4hC7QLFcVjcyQQCyXbm",
"13L2sTVWT5ihDwGQfeLWzBtECjeSy98JiG",
"13x27PQsUnGyYZoK94or6eiYMxnmUkoB6D",
"17err55eRNbbfGrxVAXTHE7ZrgXTRc8c4K",
"1PSw8mY7V9pEbZCkxaMsgzNmp8861La3J6",
"146RLP7t85FAvrNtnWy8T5MHCovtRHBMRV",
"1Ed5YWVp6Htuis4DpUnbKqYnj9dDWCT4xB",
"1J8m3BeuLdyf2PaLzmGG1ac2vbfkDcQuuf",
"12Zb2Zkj3m9ac3XuDmyzoQ1NWrSaZLuJNV",
"1Kh4ft2SADjoA92JMFD7VPRaBQFFkC7ZxK",
"1AoXKBG1htHz5NHivDwSvQkeznA7xsN5Dd",
"1BkHYuyCSfBiVq72iuBukL9kjSi6tcxZT7",
"1MPsWk9znQEcW7gKfr4chpy9KjkNa5QBYP",
"1FXWV7rDVN3kqmzvWm9CrRyT4LhKNb5Ywu",
"1JAyiM9kiLWr87tQCzJ6X4Q5XTGBPx1fVF",
"1Kz2uvrdau38CKNrwr9SdUBVVkD3Avkscb",
"14akXNjuiPefz6XwgTmWLbiFASyqCGDQCF",
"13A4jjTXSsuQgD9UqBFzgQUNApaLkjdKY7",
"15bqghPPjXj9RJgEq9cN2jFS2fg7GY1m6v",
"1HVM8D7g6qD7nGGK85Jxtvyab6VNKmnfFS",
"1GQs5KJEu2R3HWsVJcN1gGinekCfuXS3KV",
"1LFcVZiBTEY3EsbTnHn2Uo2NJo5mpKtBgh",
"1J1qVvxzh5c1MpnLyvd8VWiZxbaYAvKxRL",
"1y9greVbiSy5kHTnvL1sjCypXMWFh5ECT",
"1MY1n2dxaxCUqmm8ANzTec2Nm63ik91uc8",
"14oHQGa6dHYcS9ZpeBP7B7dHpesi6aiBby",
"13w1SXd58ovU5vLJ7RjH9gqSDAKi2kV2m3",
"175wbSd1LHSpdoWEae7BL3kKciTJ6XAYQC",
"1ATL5oRf9f8R3o6gwGu5fCncvLpt86Qy2e",
"1Kf4x2hTjrkwdZpvJRKjSckp56oCE7UjHs",
"1QyEnzudXC9ZarLe7qeDuofTJgGBCLo17",
"1BckbVM2gWP1EYtnd867dR8cCuqhECss92",
"15rwHowQ89Cv9zKtwfmvkYbBpHRVjNRogD",
"1LJ9DijobzAErfGMmHzoykc9p74xPX8Wdt",
"1BtXq6UvByuzsjRFp8LysFGD2QgDcNZFBR",
"1ysNEijP71wqwirEGJWEfibYxTnppnJGR",
"16qXkQv4PzoMCeh8TBHL8boG51BoiXWA25",
"18t5vgBAsfmVWeAdEBis83zhmBFpUjzonV",
"19th8ocJEzM3bcyEPJCYt2UkGkxJPheXLs",
"1DZa5wLjG9zPXB6UqqTv4pgg911CdNanHZ",
"1KkSp8kRp5krjkvADWuAtHx9YyASdM4Yui",
"18SaSaX2bm3J388tFq1RAc3xbXcWA7mHzk",
"1GydcxXHgmQJzEyKZSE2hbgJaVTJBrUiHa",
"1MWYYykHdukt2kNLzkCMQ9a2syVuF9zLx8",
"1Ew5k3KfohiwKDf7uNFr9MK7NCspufVbX5",
"1FpNBzxwheV2Hisu4A1ytbCxmzoD1oK6QT",
"13CRTp7CDcJpEK7Rb6UvUvTqSHDGfzts56",
"15Re4YAB5iqD5v35wBP9iPvDXufyNxkDAt",
"1JC257z3Yky6VwAen6ssH9fjFwHGAfuSZH",
"1BxdL5rH9FDp6QNMv8HRCCgo3ofmmds3k3",
"1G3VAb9ENjTyTmAEFY3HjtuZD2fv4vGLd2",
"14i8RwXaztuFrLgqb265P4Vn1nnC2gRCGA",
"1EMye4bAo5p9oATTcL6e81TywGjEn5XNpF",
"1HWnPc9XjefuT1fNVh4ZhjjWxTQD8rStYi",
"15D6PHqyxxctLjMqje5CaZdCtAs4zgd1a1",
"1BRp9LM81A2j4hwWfi1ZtLeQSeRT7dKGDW",
"1F8bS6hfDE6FjQboBh23NuyLVVnLJHBqnf",
"1NmAWx4eyvF5uyVGxLEBY5NYszJy8gwGSu",
"17sTu831Zyqg7GpHvRr7Dz4jLkYKq9SuUa",
"13rGW3SEWVo3kTX3C5jWh9kAXK3WT4XAvU",
"169bSSnUawwmf8rFvkhxUqC2b8v3DT1AuY",
"1L3rBnzb967hSkpeBKwT4BeWFfv35Hk36R",
"12ExjhEYbYhyhJWgVqzZTZTnPYv25qRswN",
"1EiJAYXa1vYnNQQ2ivQtFYqujEtuqSBmh1",
"14BFmYEAaYjsjBhXGPFYhYwiwd5RgwYTCs",
"1ACDdYSSYpU3ogJWMskMAf9YTtMo2XStwr",
"16gXhfrTEc1BvmdeMJh5qP4YUT7FsGe4Lp",
"15abdfDqFLVQTmwRDLVsN3hnJ2Hh1aQ4JS",
"1Cjz7hwoxw6ksg5wQ38HjTKfg4GmnfJ6eN",
"1GWm55A3rcgdv6AL9kF4cpjDDhF3zhfFNA",
"19xHprdFhHUDSYKPMvFmYV2Wv4R8xSYvRZ",
"1juPK2nhJZmqEFU7dDQ1Ytarc4z6MwuaV",
"15fxgJDUhDJVPM56Hj5zVmoUWGar65Ct7P",
"1BPuEGMFa1iFPPfWHGm51gvM2Be83GPpo7",
"17Q8nrgq2r2FoSDZ32KV3DCYqB5KNPXQpU",
"14zHbfjsfURcYWvSbiLSiB2KK6fJHxtySU",
"12sacMZAzP8NFNRsjhRakSQNunYLp6uQ9r",
"1DeEHfx5pZCsRvZumQKTZLjqUvSxnoA6tv",
"144JSDcH7jDVVhVbcbU8vcAda6JRRKoCxR",
"121CXmniSkVTLXNHWwaJMrbiJBKbWSv5bj",
"1rsAAVNctCUkCcABkNj8yHKGziRjq74M6",
"1YDanzzickKwiBtpRHAZ6jLWcAqW2s39b",
"1CJXo9GpPYtvqbN25RiNZ6GGGzLTUWk5ud",
"1MxQj1pXYVT3AuEqgLhwMcKYymS5sngAJg",
"115tLP9YBndMiQP2v1w8im9SEa9bvfN51p",
"1aWWPnGmMLQZHfZ3B7GW47Peki6DgHHcX",
"156kn6T3WmjtbBLpKnJS88vvhjvto6rwWQ",
"16Kgh4BSnuSpibNSmZ48WT9m2eNvr165hR",
"1HcPRBr8mtQZ13i5NffkBWo2o7SimDh6de",
"18HAYYKXdcLtNAJsGR8pg1UgesQ62xZHr4",
"1MHLuMXGKUdqo74ScdbsV6uCJA8vGp7G6n",
"198nDWXhv7vhwibeCQEtXhaKqbFoGRmuRU",
"1ByGgua48xX8iLKrpN2MvTiNbL2NVSuscV",
"19ygjZHtZcDN5nRPVyzLX15Kd1QWWncssV",
"137FGUXJ98BrfZAsM6EVa31bLY8wV4FnnZ",
"1KfsQYMVYh4bcCXMe7qacSzurCWS79Zrxn",
"1Av81ZvYf5s9BnqPMzbG2NtZrKzXVN8nPn",
"1Eupe5c2DWJGXXzELX6nPvWoMrmnXiPBFt",
"1P9PEzzdDN5QQ4MjLSHbV38egD6Fbmk3qa",
"1EcXxJp6uFvoJnUGPcdvrsTeGnRLLzcVNZ",
"1JTC6diRMrxVjPVcbCHT24QwqpefLRL9Wa",
"1NHMCt56MRZxQndHA8PneJsUGdCAvDHFgE",
"1N2AuNu1Aser5CKB72uRENwv6oTK5Xp8jL",
"1v663QKAz2L4PXUAW4rXVFC4XVGay8AvV",
"1GsfhwgBKqPxneqQ73tev95Qo87fcJnH92",
"1CrnZ3E1qCZdT8y3FV3BormYxat3ZShbdP",
"13jUjU3gzy4brj68rQ1BzcFXkoc7eJNhCE",
"18naeYkPkyRKsREpNN1vqdPcMUvwD8sMuj",
"1NpZncrSEepC3vDhtMikrDyjZB4N2wzG4z",
"1EQ5KkiX6j2MZiiRhgSwGJW5SHkJ7jaViR",
"1DMq9cmsFgu2es7bVx9fRh6B6yGKKuLoc9",
"1YTGZhz8DRMa4ctKoUc1mYPa2FDdip35z",
"1KArGJJGLMtQvd97eUTwaWqvqQwgjr2snu",
"1KHAX8ZeabH13yjGmHdw4GLJNJUbD34jZd",
"1Pu1zVveL4T2o8gURBokVrV7cY3zHEEcBF",
"1H1PEzWkBMfW4C43CPEyQcgFBkTXpZgTXe",
"1Nb3cJVoypctDCcuXmruui1yLgKgzmayBE",
"1FecxbKf9DCK1EnMVkejGZK2TscQ3mmzg2",
"16iLWNdEeSruRjbDRkH7iTKb1vFt3gP2o5",
"12vuLh4n9uS257zxSMzFxHPWVQoG4x4qRH",
"15anN3eShu3SXvvwnwf4m8qBUwfWLv6tki",
"1EovdgWPXuXH98tvKr1KftDhxdFTdR4k4s",
"1Bij75JQVJWELnNwP5UBXCj7Mu2q4EUbaZ",
"17B89K6eCoYT3woG2GU9UXLDMju3ZW7pBK",
"13DcqofFmhsEc3uuzkvn8U3S5yjpqq8fxC",
"1No1CKHGFVEaewfzK5iZE7z5eLzgKTFTgX",
"19e5w4ZbZxVcPS3XYZf2QtJAXn7p7rB9M6",
"13T4DHxW7SidZ8FmtNXTzaDfaFUvJ8eVAy",
"18KU1LcHTeLbJbftNrZaYKdZ28zSs4WU46",
"15iX4yub8sHEY9ec7ttPd9ftBt3mDFX12W",
"15F4FVvqJ9kKr5uLQZQAPKM9K2c6p4YVxx",
"1N5GorpWvDbfeC4jzxXaBf6FjWLmZYfEkV",
"1G11nMPbMZX2HxSKXy7yw9uTbuWqWBNzVi",
"1CdphSEwHMsznwftW8gPxHYYkuvyY8yZot",
"1NwNZRxC427H4asL3Ywe2CQUJnAEgHewoB",
"1CBDLNNuzu2i3YQj7pZh1ZjhZzvvXN7WLJ",
"164hGWDXkHMDpJdQwMvYeCZiJB2Lkj16Uj",
"16rhkSCoE3N8b4FwCJscExsF3pjt4UBFAD",
"17HFPUPTRZopZFuuR8sgc9xeAsR7E5ZnUn",
"1EwHV48ckxbKhPnV4gnjg547pBCwvAFY9Z",
"1GPdJvKUaEYVnNLLFDYPEzezMP2KG87N9P",
"1Ducr3dc3chvUzq9dxpvTRWc6Mh47HNPx9",
"13k9mvVJGRTWgWpP6vbWNCErBvAGjxPRzw",
"1B4R4XKbardRMpwavJNoemAwoCRdrAai2V",
"12bJiYWHwFjqRdLerFjDErSUMNx7mxg79T",
"1EeWNv4Yr7QBpvWBwh6rL1YDtKDeTMixTS",
"1DmbNjujrSkvUQQHTV6tng1eKopSKbegHR",
"1De2Gf5K2shxByXPjsskcUBP7kkey1QQgJ",
"1EANjaNy5BydBS8QYXh9CSVtpXrXgciGTA",
"1CbEwgxd2jUmhqvKrPyt9dmvpLbrj9AFtY",
"1HfL94v47LVBhJ7VtWJvziXcuX5W6YqKqy",
"1LYENtKhnV6CsHaxmJ9twg1sUuE3r8b2QE",
"19V7jbBrUbAHYL3DmYMWfGTze5fYViQu7V",
"1413wHMdsWEtZEZtiKq6b697vEKkiNRrZK",
"16LGGP26UNXnHmCVMjK5fBr3TW4aqjm3fa",
"1Difnn6bAhYHVheg5b5JAjcYSPMHZyxmwb",
"1Gaxeyc65T3LBQydRu93fSmuBp9uyth8GM",
"1NRZmBeur1AxGtfitcwW6aWCNgiqJJuHvH",
"1ESTejhtZKDTwmMWe6HrnhhPRRticDZxzx",
"1FrwzuL4MvrdAsy7Ac5JqxJLsLjhe4taoX",
"1CbQCbzYVGP8LmJjpqYooi2aKdYVxBqRF9",
"1JR1duAF8oAND6sqYjRebtWc7HKQXLC2cx",
"1DH9iaNtpYcEm4n7xScXV92WUntZQxMHX8",
"1Uzwu4iJzoLZLMkc1dsCbJF6xz3yz1mK5",
"1AkzhPBAtvKU9ZMoS4RkTDBTd96dBAfGhE",
"1HXmGMv1Zowv5EdVkVYoG5zUcmfE5WQPuH",
"17d7CDR8qiL5kgQyQ5ZJXNeHiHmPwjyH1r",
"16Wm4jSqz5V6nRZFM3XYb2NhAgxLpBPqAd",
"1MxXzPxF8LQGpcB36Q8QRrcMrw6h672PAF",
"14rR1wwfvRmGe5G87CpsuVoLkKrHSbzcwc",
"1MKKAJtqaFzVGeCvHyvAPJFxgtpkEwuS6Q",
"176yzbeujYFLJ4rJEKX87E7iJ3916ZYBkF",
"1MujcNc77KGR5sLrQpZeXWu1i6yinoMDjr",
"1JR6jtbME6c9z6Hfnnf3jMVFf6AqbmW3CQ",
"1ArSweFdjW3eGPxJ1CGkT2VMnHnvECYCpL",
"1DXzxdypW3ewEm2HzmPUWeeKwNLZ2zLP7Z",
"1AYqb7KaWHQe76koHL8ZsveCjhgEFDHboC",
"1FDCgobGnq5vUMWShUWf43C3RLDkW1dF1n",
"1JQq55Z2L3JvdFcCehNu1zgaJj1f7QqK1u",
"1MqHDYCsB5Na9LHGRMBwrwBmNMaH4WfHym",
"178kmevEGvCFjvtAoakqZG1682beCdXJJu",
"1Cyg75QEL6TE79SofJesNKJE5mhS1huvBH",
"1BCjGPkUncCCejXZfq4scZHhkMGy2j9jPj",
"14NLiYpCEYMPv62dF7i9JriqZQnyNWCDcP",
"1MMDhKgJ9WszSTS1uLaYcgbYxpWp6Tnw6i",
"1Pfnw2yajQsgWk68VpuzUHE2rFrdnEwqwa",
"1Fr8DYb9syra9zA3qWYsRTKKJXdwiBFxmg",
"1BnZybGxwQctKixEGqciuHTn1hrBj35VJk",
"1GvRU7EWDf3gh6N2WFRfhYrnop1m3FmXEF",
"1LFu3AVMvcUMJSMEyoErS1kqmANm3T2NN3",
"17fQ7D1p1euozyvJ4LLMLHvm3DfwW7dVFj",
"16h9dWb1qb7t581wDo2XTBUnLihwXtYAo9",
"1HYSd5kLNTtAo1W7hneNXrmttxnkfuMHyK",
"1B8gLJvtgJ59gH6DfioWJtzD56ki8dUXFP",
"19LjthtJuMRobipVnqi6MEui6riVZuDNNY",
"15V8hJKcL8p7W9HTqB5fA2uqucCwp7Tn1o",
"1B23r7SVERo2cB86PgWR2TJfwfELL9xiDy",
"1PVZMcqSgzGmYjTftpsPm2J2Vgf1zqnc2J",
"1A51fAD6X3NFPh5tnGzJp7VZhqonAkvZQj",
"1EFtdMuZfB8ps9XiwTxaR7TXptnxuxQbJR",
"1BX8qBGebK4KbGfRCgaxcUhu9wibUUnK2d",
"1NvxRavUzBteSDXhh5aRNcYpwhxQnAoQz9",
"1JgVGCYy4zojpsEqJDiRchsd9eAnyrbpwB",
"12HG9e2d7jw528MAGegE7cmHPA33mydmQX",
"1Fx51sB2ioChEoYS8T3DZxhybSygy9Z1Ht",
"1LW9SqcdFK58jC7bDytuSwDJVEtfDxEDpT",
"146LVxi4zSnReBtvFiGKJp8Ak7mRoKi9ff",
"1CUhrz2P7qQLHtGHLxVdLZETBsCRGqycbg",
"1FKJDwuPpvbCiYgicx556v9NjRWDbEwd1T",
"16rK3Bm8AZvfGGeoiJ827SPvXfWgRTZQiX",
"19h5yytH9BAwXuKXX3X5HqZegKcY1YL8Hw",
"1csPCXbqp6GZzUsXcGYWzcD7Ey9zPfe4E",
"1Eiupwk26oWUYdPsfM25zypDUwz3FXwMUD",
"15f6ZrA2s7CJHPptuceVTzy6ZcXT4rk8fR",
"1A4uRaXY9bMcVLrKYQmKVqyNUi7nghmR18",
"1ECS2kZ9UbaogpbjYNvGoAz2wt5HnmhbST",
"1YTWWEr571YKZ3NrJab8YwnPW5dg8qiq2",
"1GVasApsx3Tm6fua77SkbX7rzTUmzW8u2p",
"1MJGtxR8jENg522GtjQnn5uNEFJZ1anAiV",
"1EHn4xMGRA7hSUPb8QeqniqtLfvPw74jgr",
"1B5n5S4LM5whnbHyeptESwwqrWSr3Bh3z7",
"15xUamCN5oNQbQk67DE6YBUQREH9s5a52q",
"1P3c33QHMSrQUnvAC2Dea9KVzUFzL58NjZ",
"178cAhXh9PzfrxSSgXsYzzGkKRLcev1Hee",
"1M7zK7cme3ydGHY7GAbuPKbPT6ATwv2v9R",
"18pnSxDCaqh4d4gV16AhscrCvLGJDFWW5x",
"1AdocLFYhYk2cUAwcCvPtM1DkC2X9hkDzf",
"1QGeF2sse44UkkPvqmCqPnpiK6pjPCziWB",
"1K1Sher3Mx1SvCoxGnPYaXQViMafpsDYUf",
"16rpqrfYM2LLwCSRHo9FFbhqev8twNQT1P",
"1GCi4wnR1rKEPHqJ32YGyHhBzXe1XAr32R",
"15X2z7Dbc7A8nxY1Dms9KqY7jn5PMK97AV",
"15AtukXLxXfAXn8eYfvJEqLctMmVK86zVh",
"1HADtKcCV6J2mJBZuNBgB31hCC4ZWhWDRU",
"17jxXfE31UkC61KX8PsC8APfxNpzDwU17m",
"14eD6vmL59fhHbQEyq5WYZhExEGeX9pdc2",
"18F3qFCzpWA8G9v587oRgCw7s16SEcrzyU",
"1DQojsDjD4B7udT5KkwRLbXnqidzmqRyA9",
"19RCSuF7TC9wrW7Fdy9MutUxAQYSQ2iyr8",
"13DutFAeTJgfyzTC7bY8GR6w6wAU4SFFn1",
"15TeX4siAPciMr58Umg5BRa3nxzHdZkPeo",
"14pS4RiJKPhK7BoavoNdnfMP6rgbduiLss",
"13xUYfssPCTokXydts1ZdQxHSHW3F7eMLg",
"1EdNgacRDR5wKGNYu2tY49Lhx77mHX5s3G",
"1699Cxf6zCtjYp5NJW3XxniEkcGyV4mkh4",
"1QCth6PSQ19yqCYEzVdCtYBkxDJTCnqwig",
"1HyVoTRLCNJ9J8SMbhgQUHJ4mdMVo2b5rr",
"1Mu1BmkhiAECUeRW8RnqdMFK4dxf7mUTUE",
"18AUZyNtHRXyqecMUAVKLX1cGfmyhMj9p",
"1E4p9jZsKxMoJ8BqoMgSoUgp6UdJMR1zpW",
"1HeVpzZh5xnLUYvQK4otytaxYYrxN3Fjvs",
"1451h8AonhmosZQY9VofcGvXgZvYVSUJtS",
"1JigPBFwaWpuutLY8UvRHaJSNMgASLyFvF",
"1B7uyZnrqEo1wPaGpPo2jPuPfviPfbugbq",
"1BxoWyvGfkzCt65fnfkiJcQY6UGhBSNHZv",
"1E1jn6bvFVUBjwVrdjHyqzBJMiZfrGGFU3",
"1CfAxm8nc6fQUixgL7BopvyVFsCqY87Y6K",
"1Ha99PKcbEtpf2u38YjxURQAkYK5b5ghX6",
"1BqbUU3mfzPY5D5QrCfJ7akkc64unzAw4j",
"15cPgFgy55G1knAHoYUncrroG6gXuaUAiz",
"14V9Sx3f1vmG2v2BBun1mippyj73H5dcfq",
"1BPDCZzE3keZ8ZTtDFEZUySm3Jfs9bkMVX",
"18kP61vxFWZNqu2qdnCanFv9efqyLMwea",
"18Bpfxh3jZSeXuSPyfkdKdhNfqTFqihLY9",
"16vLfqFL7NHRZg7z2puJRvjnSS9WDsjN3F",
"14HLk7H8iDjB2euUU4m4D5cpbFcvMcdjM5",
"13xZSU3kBkDV4VGiAPyVmrRknJ6PZnsYku",
"1C4f7UXb6Aj7ZfoJLs8DY1DNVBJ8LuHjSi",
"1HJhToJZZce6rbhWBomFxRDMBoMbDjRAi2",
"1Q6XdtFpzSACjSScgDN3uJLDAVtEbajFzS",
"1ESC3ocXwtUHpYYZvjShW5gj8w8K8WXnxZ",
"1DkGExfap5JxF8uK4PyuHqWtPqCb9oPUCS",
"1dfmXv4nXtPseWXjgCKtCqt31XCeSLpCs",
"1FAzBmixx886iqZfpvbbX6EGj1oB9SD8LD",
"1F4Z4Xzsppn3K9yXCEoAWXuXR6imGeQ1xn",
"18MtHP9Y1CFVnKPHRuH3JLrgpy7NcjwkZ6",
"1Nce61YwScRxLwprgFYhe6xhrrPAkjdWgY",
"1Loy8BeBEcVzFZw7cwF5ksFwuf3ReFypLY",
"1BhKw9To4vrGA1WwnvkBzkKT8q9KChB9yF",
"1JD9YUQdr2D1YekNm8spw556efEqYfnY7D",
"1HGdjqWUVDoziAyyKGKcP9w7VHWDz1xERw",
"1PVQZsaptGMcJe25znqQcH3mJTv38rQNSz",
"1Q4TrUzSNnLDq9edNcis9PKoe2yS5XQ1is",
"14yeJnkeHhXDFPaT7uYmT2u1R55u5N2vxn",
"1MQbR1x1zvnBEujY3mWP5HxpQGPb3FoeaG",
"13Kc7oArWnrB5wrDpS729x3bLnU9Le5Bp4",
"1MgHusUk3PZFXkAnjHrpkVb7KkZ8JVHwoi",
"1PUtb2BfQvkYAQWi2qaXyTxZGCGFdMHVtC",
"1PGTsxEDfAMGkNHhZap6DxHS7g21DrhTWV",
"16tPZhJycaoQYV2mCZFSTe57wSqQUFp3BJ",
"19oEjFpWawP9yVAKihBK832KnQXbEj1Sut",
"123s4Z7uPhf3JH4GMPeTx8vNn5uNjVEw8b",
"1Dy9f1ugL7b1heBMoTPZ7stRu6qSdBw5b4",
"1LH9LD6mLT6DxUDuZPshwcEpgsfdKvsB2M",
"1HAdih6bciERPdjLKpKpiYewzY5eK8rGoA",
"1BhnqyBXaxWDgDCzfzNWWS8iWwfVMvcR5E",
"15MPswcGxT98HycaFce6QERaMxk3ESHgv5",
"1qSPp87E3UExRbHkR4TfspFyj8fotrZt8",
"1BkjCoeFqKF5oEea4QHYBgW1xFWYFAWPTJ",
"14xqHKFDPYNeUtFB4LaordsNv6CBby5kHb",
"1F7gApwpjk65bscKfbwA5XcUYC6F52spKb",
"16fvTU6ks5f3AjruWfqwnTY5cQhD8EZAfn",
"1FjqzfJRjF2Q9qTJMAQDZCK8KhzHMLVmgn",
"1DjBuvjKMnnfCEh6qpcy2hKn9E3mdYvhGz",
"1H1aaR8ruypBH1DjHpZ8M6uv9tXUF1RJtK",
"13aFmegKW7N3aXnGHkZoSKUMNkVPnajMY1",
"1NVv5ptd83HtT3UC2Hjz6Cbg2zH3fjHtf2",
"1BjwVU5npL8QNSsKFMeS6RxXwxRJvj2i2o",
"14sGRc5kSN2nRKeSqzcxERtQvfwWYQM2C1",
"1J1E4Ko6Qt2h3GvDv1EsZwGqEAJM3AFhBs",
"1Lseywg7hAypiTSCp8vkBM1DER72q87XnE",
"1HCcYnxxhU8TdvrG9tWjEAL5Kaow5QHXvE",
"1Jm7tReD2FJ26MdGpv5BWwU5884m6c2PAa",
"1CyMxhscKSY9iCH8n7vcToCWZxMnyFthjP",
"19HehQKtY39UgCaZmc1PVmueu7syqDgKSC",
"1CZvYGZYxCfb7fJTG34rKxrqJpXnyaEQWk",
"12FNkBaaJxHoJ4YhQFytagh4XyPqN9r8d1",
"14ouiWZrn7J48xcUzSkXaMw8hoZhFbzDiS",
"1M9KpTkDy2jvgVAvFaLdS4Ma1qVCwsTwe6",
"167KvfuyvDSyViLkiiY7REdW3opGyrLYwf",
"13gqHDhMpVM7xSWC96ZwLB7oCHso53GYfK",
"14RcCLEoBrMvdkgbbsCvdS68962ec7QHpd",
"1M7c98kWW9pKAMX1NY8XCKbQc15wKyNPYt",
"17AXToJUEURZybdYJcJXNVQhF1bLgcT1TV",
"13GfnyM4SRaKZn3y7kC68jHiDGoi8RnF4b",
"136Ax8uWZQhWQYxb2DEPdpKAQvvk3NbHbf",
"12mtcfNSsReYR7aoPuDgy1faSuosA4xmAj",
"1LBxLsMZeys6QFdDxiju4eCZ4osphhUyuM",
"1NwpfqzYvcuusMUeM5BBt5A82TYDogAup4",
"1LHvYKqpnekJ6qVo8NDXeyWFbHg9ZUdmQ4",
"15Pk1AnJdSQZazWM7QvABDfatBqYRkqWhY",
"1439mzcRj9Vj8FSDrMd92eVr8NHQG6JTui",
"1J6SKkw1hXeYuYs9HVBvMJwdZLW3F2utUZ",
"13UkwZwHfUoaAU9G8FmfSQes2hhFwoZDwB",
"17wa2vDTDufV5wkEuLm82S3yZ4bHWepG9p",
"1GrLvfXjafAP3hHei61tUVywtpEnpkikor",
"1K7CNHVhCuRKhkn9wNndRLNPrCKS2KyKAG",
"1123r9jNzBsLGjPSpDrczHWZPFrvmjXHm5",
"1JbWXL2DnvHR5maVUGYjBKB1FNKKvhk9FF",
"1DoifPUB8v9ZdFMG886xGueXGmYFox3t11",
"1MuQqExvJSxnq1raHWaXJwKBWEXhFMzXML",
"1L9tBVYHGRhvUZnbNSe9s9VHVfiTqentTS",
"127jTkfBsGzvcpP5Zyprfhtg151qt7q7cx",
"18LnpGGFp7BXfsNsKD9degyNY3jg2mnGm",
"1BBoBUdW7VCAKHJy2mrg9XhSAV68AP44JP",
"1GqRDrhex9nAYfF6gzRaNNso3SwU3HWK1o",
"1GX2QEQRL3nBMCDw4YnAwfUfLFEtBMHs96",
"1PZW4gL3rdSVgJ5hGkvmiWzvfxsMi4ePWJ",
"1GBLAVL8BuQD3MKTnNWYSEGQ85Wf7S7T5L",
"13SPhVc29sx4oXuN7z5MbN5MFYLCVFAnNx",
"1PtDEDKS5KEvJfSfMaFwCtk1SrWHi4Hdnr",
"1PxCuGdcnWhyCjreLrwTze7Eq9h58bHEAD",
"13xavyaMZ8pVT4hUfDzZmAacfhKUPZKN12",
"1PqY6MaUaE78Xz1na74Q23cH3Rk23F6m2r",
"15oXyzxYqWYB2uppVBmWLnFBjCQek7MnUE",
"1Aquwg4P1WmZeJXPv9XfdJdJ9v6JQRaMiu",
"1JjoGp8A5hNkt8MbP4529VhoKe3981tYfk",
"1Py81XvbSgr2iKubYoGvzU3FW5bfkVKqCK",
"1AJyW8bKFHoK27ibSW3eoBdNPaMEdz2hZL",
"1HUwLBsns5mbVDvmRakUbsTM4ytHHdbUEu",
"13RCrRLgsEJQf24RRHGA4hQ2KtrHoYhJs8",
"1D9BNC9nWz7tadkuixZH8NJb6TQ1VwG6TN",
"1GuJX39CgJncFGGCC1ez5DaCX42mybyDmt",
"12k8qByn1KKdE2zLg88TSpLZB2GSVdvBJj",
"1HDcPkSfEDVALwKRRDHnMYwVympvLPq9P1",
"18RxMQxf3wVdZwRiPECK5QDbjyLiHfZm4i",
"1MBeXsUJxMivtn9NsyLaByv2wfxg2eRtGS",
"17sMsLMG2wGiUZSJzxXPfvZqoeqo1mcfKE",
"1GzSSurH9Bpk5GirVNXxXzfALe8YMTbWLE",
"17YLiHKfKU7PByeUwvi8h3WGjA2zHbA939",
"1DbUiHXq4KDVx3dst12fWRNC9PcX7jpd9s",
"1GZCNTfuZfTxgrKVXgFmaQqwQBNfvDRB6C",
"17humcsm1rzx1yLvdyRrhAgAKAs4kuhVgX",
"1Di2wDmbM5vpdv7qnbAVQGAA8NDBijjYEh",
"1Nb5Cj3KFgDN7kCuZix8nChkGWzEfRQpg2",
"1Q2xCGoarv8jjruedN9qERGFw4o5VoocYY",
"1H6NwThQXRWhCAsafuxGptzkxxSKyNRW97",
"1H2R5HM1tBhd1C5Frg5RQLnmBecRGzPUDd",
"13xKCPRcHMmQ7CVwmz43HpYbH2UCDRn7AT",
"19botsuBXY1ZGR1DinLyqrPdJUmW4jR6i4",
"1LgJWXKBkunWr5YbZ9KnAJdiymazmGJWuL",
"13jisMnkvpFqiNfQdARBgkHJQWmh7deHWT",
"1EovdRt8MsJmAihE4pECw7MjjA45TjVRxX",
"19ExmvRC4bwAXt8SvD6yVNJnhUohSGiLpr",
"1Dq7djRHtUXySgkrWTMfrazREkSJRVgVMa",
"13eRWquF5wQR16LYiPCPeNwegYVL8f19Lz",
"19yqW1WmUkjSsAjFKyXakHZuR9Cw6ZFyMq",
"1HEAYfVvXhERGFgZXQmfCZhBPeqqvoJjBg",
"1GJryAB68uHjjnCJZxQThSaa49xDBCNNen",
"17jTA4NqmxEFydTSp3rseXaRveC6QqhgtB",
"1CHBSuCvMG76WHpTcPoiTLanzBNJXKrECG",
"1H2NFderZm8gQy4egSvf9DmERHCrzZNQDp",
"187VtgDAnfdt4nH96p8QNCBJiZC2LAhTLE",
"1NNuKgdkKiMW7L2FNheyQzHVQayNwgdbkA",
"1KGEbh9yKLafKX4AfA5ej7Nd28LpZgiJ8W",
"1MuREs7b68MgLwuzGTWpdhHekg9jHjsBr6",
"1AgBMLYH5JxBQWtDNiZaaMAcYuR3G92pkE",
"17zbmmB4dzRvWo83YqgCLuFZYhM4TAuzNg",
"1K9osQqrwxntfTgXEF32VAD8r2ehbkj5b",
"1MsWpeLxXFb8425oGMV4eFRFycLBuqJtJe",
"1DYqD2xQ9TCBBDu9qNx6ouSuJNpMCwets8",
"1SKP3Tu16wnCtnzNjpakp51svCwHD7ZKQ",
"1GxkN2UbWN1egbjje8FFackffGKAqA7Gcx",
"14jXSXqeXyvBzExmuEukKQNvQMr4VufPdW",
"1Br41jLzJUUgyMBDwcaaQaJ46NH9tmhUwz",
"13f9D1YnfPECQ9QEBBtiD63raXXHyVxsDr",
"1NMyVKtfSFpCQ2bPQAHrMzUxd4RZzPLjQd",
"189u7KkVvmiLTW4iwmwtSuvttapmZimCGa",
"14GtqXEP8DNN3KQMcqJLQRZYcr4xUCagJs",
"1AjHRgXz1UECL74dSeMAUYLjVMJ89UcBLs",
"1CxvSqavYbE76RZ5Je8DXcwyvST3Hmc6QD",
"1NvDy7iWFSu76f97hQK7AazyX5Tjw77eow",
"19TNiw2LNnKiVvWa2qDAT8vptA9k2mzmMM",
"1AUQ1zVr3DEgXfG7dcdx47HPPVZPQnwzpN",
"1D1N47ewscWrhwFUnJnUTN8WXEiscnc1hA",
"1ChivEsvEnFnhLkpFP6EDVDsK39Cr2ySrv",
"1CbPWg1an7PBcJQXEqYGsCiWJjdD4nrFfA",
"1PQ1rWu48TBZ8Sc5LBMBqbVDN8n2Zd2QMg",
"1BVeMzHazxHe3xFPQAcpfoQ2AeACBpzPmP",
"173stRK8rLhWqSnEe8LDoSX1jpqeXLaFSQ",
"1PYyad1tKxiH1sxVetuHuTX3rmnsJNFMEg",
"1FopRYDReWCEGKnAmES6qjfBCKnSz5KgVj",
"1Lfht79uJRwRZEPC2JasvY4LeJECegVjyd",
"153WzRba5k8Z4qQNJa3UzbpqphjQuLaNe8",
"1EcAngcaAS48jjbSXAXAY5TSv5UY48WLpe",
"1CnvqcVhyJgiwRLFgaxVSoeHvvdCJSYiWM",
"15fktWvVdhPkZuBhhcB7n5ZAuyicS2AFar",
"13YEp3uEQ3D4hPTM4ySdd5RzHxVRdj2pqY",
"133dFU5DV3Sj2p6FBE8kwTyNE8UgM74X2g",
"1A2wyYpBb8dUmW8jwfvKQdMm2KUANCp9AH",
"1LkkyGLRhEFQEkw8ZhxdsQUEQm34AX5BYn",
"1Kqmh3TKXfVudKdxULE3ht99sQHyb7YTyA",
"1K9Tz99FuxRu21mb99Ftukd7epfVa5YWth",
"1EwuKECkP1wehBNqDbVyiNTSezRwkha8m2",
"15n1mFMJG1WX4pMSXLqouV7hRrr4pzJkNN",
"1Nxmi1iLvF68KJypJAMmxWULuduCLba2cg",
"1NBnWSmhA5thqzabQA1R15JGbSG1XhP9mW",
"1NBZrpNg6BPxkVcPdyhn4CyRMgFzQ8f8UZ",
"12bAVGietubBGnQTS1R1FoeNUP79Ch4Kw4",
"1CWc6rUPMfsvrQYLXxtFEywDxbaSzU2fCs",
"1Dzd1FeuTqQ32CuExdasXoCezSe1ExoGeW",
"1F1Trzs28Swk6LikMsoos2NjyDwRAYkswQ",
"1DN2ffsrSU1PxAzTZdyMH2ETi4SyjKPvNj",
"1Mk8cc1oim6vM5Feeo9v941WAsE3kYaaB",
"12RsvHzzu2PTHSB2iZKghA1qX2BBMMeJuQ",
"1NUYaARFc5EWFAxsm7w3VHUgiU6eVn12mE",
"18zqC6HPsiJx4PCCcV2wG2RfRBhL4X2J1h",
"144B9XMyEJifvRcigewvHnQjcKEtUFkeMn",
"1FwtYCrTvc1u3k6ub5cmsG7FvcfzJE7jzz",
"1AEo1JsZrzo192ZK8SLt7jZ3g37cKxPteF",
"1CD1Ctoc8uPqa1dfUbU598NDraZjCTiJBB",
"1PDTto3kYB1xhUutT8yxyyevNY4qUtjDtE",
"1LpHQakG5CPMTehsNaNYoMhhN7SDonZtGC",
"1A879EKhTBAxxJfEq1dUESqnm8mL6MZyWz",
"183YGAYYbbEmL46yCMoNbo2tibFmMkV6sU",
"1BzqB3KrrcyfJfy1C9RsYuifzjFcSMLkqD",
"1LBgMpU6ccEWuL1mzVuy3kNDqnkJhp6ZnR",
"1CJNzGTseqqjo7c1dj7aXhb2FXeiWuo2K3",
"1HQpi9fn5TrVx1fif3NmUA8ayhyS9ZRGJh",
"1L1nEfeSmisLk1ThpAjCSHq9roRFockCG1",
"15xHTrSozZNuahmgHZUXX4aYR7kUe2voqF",
"1BQoz4JbMmPqa2WNC7GRrDG1tVDxM23tGt",
"15pgbi7LtWurVKiZQ4uQqWKXBAG27XFToQ",
"1Npx7NwY51xGwcyZHZkphBntJtNeigZdjS",
"16U1d56PEqsxYCKrtQrUV5rmMSAsLt9s1x",
"1K5HgFurAjNddUU6uhXgjs5Tf2hHF9ZQuP",
"12JhA8mxTnJ8bdw5NPAAsCmtBzdkfwEXDR",
"1NRuC56mpKN5iSUwDGaJyipbyHoMKutPE",
"15BfDpwqttxm3EBjU1F3jvTNPZtuxfK3F1",
"14Edoty4v2HUrVm6yi4ZqPgCKfyXbmuDT3",
"17LUPXzEzrUrHQzq6wrc3b5JQE56AnV9pK",
"15BAgkuJsh2wJ3my941DaEDTWUMNmWvQvJ",
"18sQCQXbcUJNrGfYfBKDcMLgKr1Jpbvqqh",
"1Kgv3gDpGgcFNnmKdxVTdQK339zGotDAzb",
"198xSpftUZwfK9g1F7FxHevyDG3ivhAVBA",
"18gMYFHYFcBZv8uCCQj8xPirM8eR4xGFh5",
"146Q1QtnEeprNFmCHG8ACaoETyetupxTUb",
"12fFjQjiA52gHL6dAYyjZNLYcPAV32Aa4a",
"1JCFmTdTMaKABWerjUUsSbrBJLyB3Arrw",
"1E6xG9LUK77xWKMYXWvaC51cpWTmEXhKvS",
"1C5GgDQKAKb1694WSFDvfHWFopMEvAWADB",
"16jVmeYU1KLPggSqgwkNGUK9UmM1uXnB1Z",
"1BEqmTRXetDchrzjDeXDNoFSMsLgfYefps",
"1E6GdG1tSxkWp2Mh8sg2McmYLXbEN1Dxmi",
"1G3SmogeSw79BbzFum1pryjZ1hoREGiQg3",
"1JhWP9tz2AacjppciYLDiN7MaCRdpH66Gx",
"16r9UuFyoJxTr63AENj9G3LoAFzPkVJ2Kw",
"1Ds6mFctkMNwcQgPFj2nbomscY5x83HwHQ",
"184sLF8FYM9rhWkBRStxrkWo5NtDPCaK5s",
"15wCybcPzpV4qPkobZLVbSLBVvkkXKUZg8",
"1GcwvJCNG8wouSJKUMypN9fY4nYMczKjZT",
"15fCb2mguKDqAuLkjwjsMNR9EoSiKpeEcj",
"1LsgqF23hHNYP5ZmSX4FoebnPsUtH4EmUC",
"12vzvwtC24G3LUuBhR6HdQGuRzABwLSp4a",
"1LZwv3bt1ELSdqTmFXAVSEj9s5nrNGFwdQ",
"15r9mPzFE22MVtBUmMHe1G1G4C4qLjoUos",
"1PTBzCgcKAz1XSpBBGzsbsvGpnwHazfEmT",
"12EqQXW5fXNByZaSgTCE1Pdcnum11raHZT",
"169JiEGNyoWmGdzeqLEoSMNc81cPnttoGq",
"1LG2tvd41GZEHNGwKNoGeDqWQPyvARnBW2",
"1KHzSQ98jmoEeTTiPFavB3uW9Ykxy4BJAv",
"1AAnhYUY4mni7T4PoJq3xC7qFSbLEPxbFX",
"1GtqSbwEF1kK4JR4W2eiPr3Mrowphw9G2m",
"1Fk3CShvqST81WBToqgVBGAb2RaQmenmvA",
"15k8uM1c9ybR1dYTtT4oqbBRTVQ4xLBs2p",
"14WSau1auTLbvqJCC9msxf1Z1VyeRVqCbe",
"12TFaCFAJwFQq3LyokNjDVCa3DyXeEpA99",
"1QCEB6dGz1gbvt5zkrm8JZb6tkLP1aoRuX",
"1LHXdbDjTEATPr2Vn7HMWEK5sf3V5TcEcZ",
"1GGFKvRQZm8NcZNoFzDyWdmwV8NSXJtWgK",
"1AoVgDcywLhzMVs5dver8X953xwZ3DoieM",
"1Kda9DmrF8u5QhxHLBbrQqzHVHtLvQmnXw",
"13h8ttxF5NShqbEraH2FBTxRSdqLpUrWhh",
"19QqsEkueSegeujsLc2bthNUr8hZx1wAW9",
"1HDNhdurYm8gNBU11mTZ1NMfD1kfiBZAfz",
"12QABXjpVAj2UzceVKahC7t6EHpFM2c4Jf",
"1A5sMqUc8Ukov3MXkD7WQ3Sf1GCjcfjwvE",
"1NM8LZzRyzQsKevuQ1oqYfFSaG1P4LHY8z",
"16JpoZZKYuJMtsnf6gZANqnnox7SKqSnQj",
"18AXeoY3j6DZoMJRbfUSBXjRPkpKtr86sV",
"17ymLckYxHEz1ZZjmaBtdr84bN51DPM5Z6",
"1SX88ZC6CZnMUDaZTBMjvvCV87UcL2Auq",
"1F6LGrpGKgPihAGJLZeL6q4XSYRmvdKE5s",
"1FUrpEW88Pup6CzXy4xZWhWjSW6tMz1fjP",
"18frmKDoki3NPn9bXXT9Z8uoJYA9cmjZVp",
"1DjGF6xkrjwBChxMNC9ZYLR9Saa3ZyYakZ",
"1FQg3X1ovv9HgFuhTipH6eipwAh9bLVWZd",
"1EckZEyydCz61zaVRUQCAnmadrMugvjz5C",
"1HtNsBsvjwkmm27FNyXQp7it9ni4QyUzJ7",
"1PtJpxb1bLvYBXDUESNDoA8Quufge9Wpjp",
"1BNYtdNihVrKwqgWVAWWdC1Yd4fPijjLEV",
"19LjzqvBhE1gavhqkVYSRPXGaaW3wUQA2K",
"1HezQMBMuEKN4bLZ9LAFqzBsSrjBBNo7QL",
"1K94DkHCkFfT4JC8gaBHyS3oTB8Z3XQh9u",
"1FPADBgxcknn5Z8G2r1dGozoirxckRJbWf",
"1KQ5fD4xRngR2JmeQvjtqx2UYqpjYTqsXC",
"1FCaaixGHKwSreELJQFKBRcBWvZGJJxKNj",
"17HGGk1B3SHS6mmLFgWxseK5QfvFksow6p",
"1CbZ6kXggMYUWkPkXww7WoDzTSLuWX5UWi",
"1DzKW794JwbH9aUtz2pEqvcQvTGLc9wsKu",
"18jqRbmBwcEw4pXaUFYSmCGbws1q7sfJNg",
"1855a9q282K4SnjjAun71YByYmUV5e9wiZ",
"1FQCo7k7RubCyNYM5ku9KgZhXVGvwGEHu1",
"1F3u2QBD4euYXdThpgRSmmHrygb7iaAofq",
"16jwyp7nkNwM9TPruTApUW6rCgWKtP7EaT",
"1FnH6Kvce5KGTGCP4V22oGQizUPUvT5Fyo",
"1CsrcxSUbsdq9HcLA2NTFRZpRf9wx1ZvtJ",
"1HwioNoRoiDgNiwyvBYQdoZ7sAox4pGTB8",
"1JjZzwrKZ1TCDTXRcTzuUmCAa5nggCcqRL",
"17Kf6zoKuPjk1dt8RS5mHVY2GQRNoS4RAP",
"1Gx7S5zsfnDqXMBFLFhHWkG2pdyAdhMBBU",
"1ANZvQ43goE122Gm31J9dCW2AE7x5GEeEP",
"1PM5LBWrAtVjKRzvbeo3m8AYi3RhW4p8m7",
"1CJNARduCCHeMMDFmzV8Qf11PRzQg4SR6e",
"1f2XLVDHYFWGFDuyPVxgppvHod1omdMXQ",
"1PfBNkcV9AFvSEuN7nhjSGVfa83dsVXhzG",
"1EaoaNVrweiNhh6ac9MDfc7FaBRvxNjkES",
"17UbEcjteTpanzzLW3vm5wwANzBrhojZz4",
"1DGr6F6CHWLvFKpVutohsF4vp4wxMyXqay",
"12aebunLNYqsDZL4SaJPgc4DjFE5PQmaR4",
"13ucSetN33pV5DSv8shHmdtTdZHR8DWKPG",
"1DtqimHbY8R7p8dGb6mgWT7QNXeFFnojyG",
"112JAZPk3m5ppE6ZWRS8CEuPqUMGg5qd5s",
"19DsPqd3YGjPKbr3Pnw8S3nzjT1fhxXUGQ",
"17W6NjHFr6u8f62oVNjRcztBCKFhEMXM6T",
"1CUSBvUK1wXnwYZAQnSzh8RCKntXGHWXkn",
"1MYMaCzLte1Cbb2Tuiem4FjpH9FoM7QQdN",
"1EX6T5QvafLD1HKFVuyq1rNTCite5SXxSP",
"18S9K1hTgQ9pEnZuDmceeBWMmxCvroZ6Yy",
"18XudXDrpyci3YRRqX3DBFk1drQRWYACWp",
"1KSioRC2WEjy58w9kip4ftqsGkc5q9UsJF",
"128WwMN1MRGM5PfYnzGGpft52qENph6wRv",
"172sKtnd2JhmsUQFSuwuPKd46P3EqhYbXQ",
"1HipgJxCkZdNjc9kZ1CdSntKaj6EpTeBW9",
"13mQXxJ8Rt7gbkY5CNyzqK1kPyWTLnPBXH",
"1HSEB9cTKdzykpQeSh9YKbJtmpw8TJogd9",
"1DyDuwmCCXfQ9hcmvycaHPjEeRzYhyrjif",
"14mfe41kCPCUEer8EJGtXHrwBLDssj8JFp",
"1NB8tpWp1okaXe13u8WWCynQZK8aFVCE5W",
"1FZJSbkZGEbhPLSoTvBLayst7ydEM3fDtZ",
"1GDYJ6Hnfe38V5s4BSLvgnT18Ujug2RZhZ",
"1QB41frPD67VMTMhqYZTigALBm5QNb6uD1",
"16RH99bEEn6B3YXte6t5Yqd7dwRkadi59H",
"1B8XU8ZqkgFeKd9BDpPSxHNbMZPPzySC56",
"1EX8pFAo3ciLK7DSSdbxupW1g5kvzQF4PF",
"12tZbcZUpqcBXx8YMskV5zx39EnRGeDKwQ",
"1Fkkvo2hevs2PW3DDsY1MtW3EMT6rNuxHb",
"1MEQW9fzuqymAWgjZGz5x3X35ARgErjSLq",
"19wTC8eowffcpmA2dxJsK3NroGCZWomjXp",
"1EhG8ZTNQT1orDH9neo7PmjM8vF2LvHDMH",
"14LnNsNjijJFp48wfrhkengBsKLfEpDHAH",
"13M13cMLeb8WUgErzMR1siw6imy3uLbupj",
"1HTTQvSgaUVpCt67fcEHggK138qBcRq4SG",
"1F4EERf58jBdaDFPBk6FNMMBxh7dNxqjeZ",
"15gB8BDjj71sf6XQxVM4J7DeskFf8XsEYb",
"1Fx7dbQkMcfuTsfCpfu2vs8XMtZGdeLXLL",
"17gqnnht2BytHTeDHfGnjgmuQrgFeyuj28",
"122rAp4AaqERXi1EwH7cNS9zC6bsNw2uyA",
"1NMbSg28sCD9KxpG7kaYRsGqci28S8Puj2",
"14hZxZGb6Pt1VXFxGafgbmc1JGDShYbSzP",
"1DyW5KwaiYqyA7Voodcqa8aP36cGGGXvbf",
"1MN9erQaRfrLDvbbrzq81pESNLkRr86WQC",
"1M6qaqJwaCJ3jix8pZZdfBy3VJffX8kZA5",
"1JxkGWQ1PwG2gZ7TmurdveEYYjbfgzqHjE",
"1NwDoqzYhbvZUeGcuhLnJEmsjZdJSUbTky",
"1GvHTbTFRpAno5dUdvyq1dbucLHzbwxosz",
"1MGSwWgYzs1anjDxTtt9AV6UUuYFxWwfsj",
"15Qh4K3Wv1KWn4VKqM3JqmxGP6kMyuFK19",
"1JLvRo8hGFNDqofatXxHQeUUxn5GcUuJc6",
"15GVp13m8R35BDCLSvW5ne2eh4cSVyHKCd",
"1F4Dhyb5YNDpPd7xx6U6gEQTwjyF51HLdm",
"1NCVnTUMbhjRaUkWb6gae3L8Bn5a4EjVKo",
"1DCaSznj4S4bCGhuWzSKcDApkuJj9Tcqbq",
"1LESzpB3Lue44X8tFpGN2X4R7j8gJxjkJG",
"1FeJcu4begSwi5mugPzR6C9dBn9V6GUHz9",
"1PNkgHdwSe813Rfn4eFqp6GxPwxJyAQWkx",
"14AWRbuyya5sMw7FrYzitdU4HQ7XPkmSZK",
"1ByrwStzXUbVtSoKeRYosrc9otWfUUgbSJ",
"1EAXCV81xNMbUdCM1g596wo6tWbsMXs3oQ",
"16VhLHii4E6rBBHu9LZ8bTbJLwYusDQSJU",
"14vitPBMS9G5GnYETFfV6PHRD51iThATcQ",
"1Bw5UrTH7bwTwXTwx7N4U8ojff5RAQQdG1",
"19TtgpY6f1pijf1Noq5C6HTJce8tgwovW2",
"13rCDFt2fka4kCFcoQRFAtFggx5PFcFGhq",
"1BGbmJfCqCaMm6BGRW5KnDWRiDxDn9eY4",
"15b6yJdnPepHMQ5zhXSPuLUjhKaGRqVDoW",
"1KwqphCP1hGuoUrHRjpAbzRaevq3a85Mw6",
"1EnRDhkpQxjpWEKBWbGDVwUjRnHWHanYe8",
"18jUVXN7jykZbF2MBjLYMj91mdp274Hz8L",
"1DUyFmr2sGetvxYKfjcFr6t7QhTYjCnNaM",
"1NiqVuttQk9k25LoyRW2CoCEWj6pDN5y8E",
"1KL1jvaUhbJRgj8V9u2PosXFrf7hMSpLVq",
"15N2CnYpZDVyU5eTFcyBy9Bowf4i92KQbS",
"1GcALAn13K8s7fNq4SEEgg9TYEop37ah6D",
"1N7cVbjS9bEdgMeW1rS9k6awMGFUDHXKbV",
"1LbbKhbsNkAzEncJ5GM5W7vVW6Tt7EpZs4",
"1E99tWYfWNDhVoeQYea3i3dkz13jnKahF3",
"15n29ffXV6P4bSfiaKPTRb7VLb286iHZdU",
"1EoQGomqppWm1r31Fo3VH96CMeNXtL8KSB",
"1Pu9z7iQUzSkdsPXVRh8kSLcbhfqy4Dzsm",
"13qKvvE6opnPzx9cWgvRUMsgsjuJgfqbYp",
"134weGLD3Un4Sf96F7MoGf9syauBLS8Bua",
"1B3Jtt934Z8EzFUFirQxNxFRTvneHeR3UB",
"15HMwAZJ1JCgvqGHjyXY9WQ1JTFSwKyC9n",
"125t94QfsZhTKeK33gGN6rMGEyDH69WxJj",
"19LdPgAUjMLXd8pWiTvZhCztB6P3dGFR7f",
"1KWzZzoAWgZdC5u3fUwUNparLXpPL86djD",
"1EBAGsvA32tPfS2956MoHvj53t9TgBgsWQ",
"12wUt1ckZjT8v93YYBsyHR6ufD5jYGof9U",
"1Eb3Bt3VvCT5rbpeCqPAsRW7ScEkWfqSRk",
"1MivzQej1toAwP5NR9KMYimVJ9UBSRgb1J",
"1Nqp2bk48nKvmxNzYdxuappqkRqYjkhfwg",
"1E3R4vPdg8mBoyQFWtYCFkZx9aMQbydhQY",
"1JLmGpwN3WSsif5JzXyJuBZgoSKTMKZj3Q",
"15PxDUs6YyPjXeyRxEaAw9rAenRnpqggDq",
"1FWaB6PNbJVA871mdefyXrzAYKUhHb8YBA",
"1KQnZJMQZ22fHUNzat2bkevdSR5BhVyang",
"14tiiYFNZSA2NDoJkpBkbiehybinKQyVoD",
"1QC7grZRyJvC8mJA9dPKbWD87jUDHRiA6x",
"1MaLKzdMhrsjsshiMtdWc4YT3Qvy6nLjcB",
"15WmBCig7rEiNWY3uigMZKwidxy2HN6aGa",
"1HJHQtY8EjCarhowdgGe4nmYBjCQBcFd6m",
"1nxMG2zjZn2pbtEeAYGqLq1f5YgSEPPP9",
"1Nm5rvC6STbLGT13CqZrhja77Q8HdRCKx5",
"1Ao9rTfBQaFyZCqTQFahwTdwnD17T7U7HM",
"152nNBrwuGGMRd3FS1qz5tk6BkcsA53UFp",
"17EYHVGeoB3XSGWnZkkYUc1Jsi2kFCPNBF",
"1CazniUmjHHtWWB45dzksfzitgHw86SCbm",
"1FkGQRaEWuytTwwWR7but5QSTMJeFUh7C2",
"1J5aPLPKyfhwy5VzKTH47HzFBjwo3s4GEv",
"1PpHq9me9FVGMXMwPkJAVkfXx9FkSp9m9a",
"1243776S8rv4e4VyGP7oe26etz4fGVgEXd",
"1MRKVD6wsQXY5ujuzH3uAwRnXexFCVPUny",
"1Bc9tZJpcjUfqnho315Wo3qACqwn7G5WCW",
"1Pq4N2iTnFwTvXABaDKAFDmZ2edJjQDhNd",
"1EparATM5hm4weT615rfdGS3DVC2Xzm4CV",
"1J9dt9z9HXVRPovTRZo8HsAC2TimavQ8sE",
"18mqGANvVRghqiKmJHdJ3sNMuoLataV4Vw",
"13emVY2Mk4VVdPM5Ct4Rak4HBZcbpXQmAK",
"141pfRbkPnUib677jEC9tGASdJDvvKev8d",
"1LMNdeHj5E2YTVaENDg8wvwzQoRyvFizRK",
"1273vVfYGiRdG5SynjYVcBGUsYN1CGrbw4",
"14YHXcg8RfYNNAkdMVQ5csYGBCAyFQNo25",
"1Q9w9ZosA1as5fd6gnQEAsfU8v5M5hN841",
"1BHmgvwvAaRaAimmwnEz7A4aEB6pxXMq5Z",
"15UrG6qGTpyTcNLiNW1BxcwCqD7H6mBuwD",
"1LEwApEfmSR14pW2WbqTzP7BNSuq44Jart",
"1BhLp6DrnTw444pX2sa2jjyProisPzkFry",
"18FrpbzmgWRGnUL4JaVUGKEwFLEB5Awcnv",
"1BCjsbpqv7fzGi56vv94Lcqp3B4Mk32s5b",
"16LCw3E3FBqxC38CtM7Bmj9pJvstrQaSuf",
"1Gu48DKBXEDYgA9GZVZ35NWEd9brrmbn8K",
"1EjGRdoqGc18XxYUJggo7pdXL8f2abNpY4",
"1LuktzvFdWeoV3eyJdTCrZ1owUXCxFY6LX",
"1PWvRF91nNJwrfusnTuQYB81ongwUrQDG2",
"1Ki7oNU7F54oqxQ4nGAChRyzsHn59X3Q6a",
"19y39LKNZd8k9iyaRAEvSMgx8qwkzsuFGs",
"1NMGvAQkBjYXtespqCCKY6V4ZNAaGfuKRk",
"1LwdtuZmsdrCKXYdkG8JdN7cypbiNzfVsA",
"1Q1WXSk8qt2RJ9wiuFoigybPiRi3FeBBuC",
"1CbUzrVPEkeu1PvHVBKfunQwm3uSY6C1Le",
"1C26QWoR6ASoVHNmbboye9buc88QoJDxCS",
"1Jfv6xR3aW7B7NrZLb98WxKuJKYiqnCFyL",
"135yZthwTsbjqZFMSz9JNMF9qLhmD2jHwK",
"1FezLscn8EdTXxTayZr8nitwS5DheE5Q5H",
"1LNi8XYv6G4SBAJtFBbku1a34jzdNKgpb1",
"14rCX4Wj7G8z7mgbHZ7VvCX94Tw2kJnJjV",
"13yTDE1EkjieJSmSmu9HMMKNCva4J42JCg",
"13X1dSihwpuhwkLucF7UTYsrGtjrAoaSem",
"12UbicwUtsD4AoKUYYv7NEfVvfjdr3xFGQ",
"1NeyyQvJuc11LTwGrnFT9L9TPqTeVFcX75",
"1KiUwpShPM7DsHPK4ZRngKkSRLARqbxnGa",
"1CSr8XBSGKgBEhzUQVCvkqtQAnfep5HnPK",
"1Df8914nmZrBWPH8QDSubPvZPs19RULT2p",
"1QXsHuBGAdCCBxBhs1JvVzkrXKhEyydy4",
"1Cn86mNTWbns7Sis6ij32DNZ6Ba4XEvoWc",
"14yxdjiyYVDXyFRDbHbYFmjqveVgo5n8DP",
"17YxvSxHxhis2V8RtTbRyQib48ZoEkQiRt",
"1P198UXvVgRNhHLAr2yVRAA3rgBioqM6pD",
"1N7u4LQjVrbnT9CdbhKccfe9B2yoidooCY",
"18yoDCjQ74oNfMynLBqkPAGX5RBvFkYGaq",
"1JFx4MA3LZv82oe7Mxg35G7eeVSRnve7J6",
"1HM2ysYpS2bdcNdnMoziWBCeNxYoSn4c2h",
"1Q7pptbCEQgkBZfmsDXqhbyQDHz9DEMPRe",
"1Ai2u15oXUAUhqBn97i36Q6LxqR92KnFLi",
"1QK5R4Hhw9iHnh3uFmJp41FZqSMSPF6mSJ",
"14gJMum2SbJqGdv7f1jwZskytnwMmKWx35",
"1ETRAsDcTRFfnFG5RcvhajkGxWT8Sacw5a",
"1A6CBKV9rFtTuWZbMAi7td7hqZv2vg27u9",
"14KJrE2HRoK64tXriWJYwmMBCgpATpG9fV",
"192ZeQvoMKd66h68ZCJsCZmnN5FCAC6QB7",
"18T7hbX4nNy1MoQAzoUCaQw4PPBv3P4pbt",
"1CxNtSAgrxnvwUkWwL4EHCAa7Kj6QVDmim",
"1CTCnCJ5PAL4sSkz9WwLDjArpVJJUE4e47",
"1Fe4PSnWCoATVtS21JdXGLknsb7q1B8XUw",
"1CXyKVcY6mnMYvr8Md1QU8CjTy3yW6wbPr",
"1Lim7SpBDPQmC4yzV21vpLUe2uG9LUqUTs",
"1LzBJPkvq1RWPkjLWARTvU8L5SvbjR6ULK",
"1LuSzZkGPKBJbe9gu7fjjMh2J3p2BKKV7x",
"13wc3Lw9zD3q3FAp5nd6yECYdzfdVcRbKf",
"1KshyY71BUkZ1tprvrUroZV7Bn48b4wzMt",
"1Lxjnxdre6WBhET8JyCDbW3ZxHUkrSEWFQ",
"13LUk2Pmk9VWntEpDKMaMJLXa7Lf7HiP44",
"1bvEQwwB8Dj7bTUXYhDogQMyPBHfeXnos",
"14vNsDdid92Hwy3UbMrhzvCAL3o3eDP5yf",
"17GainChu4UQR3HNhYyy755oNLvwSixjxT",
"16PaKyiHL5QM6NAB3iRTZ95hi5ckPkXMe3",
"1MYUEn7NKCym2Z9EX4BPmfzQb57uDhFGV2",
"1Ds1cw91fnEwS1S43D3ammkfRc8TQmfpn",
"16TTvZdhBVneghZt3vNWMs9dfPS29tBFFa",
"18zdiuLTNrMSqCdu5zP3ThGbMrPkr3yW3v",
"1N7Z739VkdRxyeo46Gt8dUVJv9JgmQQS4o",
"1CbNZP1xqmsmBSmQ1fJ4FR3j2Qz7VW7G8T",
"1H4rVrXXixva1zRjZhZtfZvjD4T4LwFNHj",
"1LYQxWoRCxb59G4UU5TVhfuB6139i6mzQu",
"1CH84tSTWkgKbB4vL7dEV5azu1uXhukkPG",
"1MYSucgeHLcFMCZYP52a8bro1YSXmTLtSE",
"1Jn4gLBZNKj4oQSb9TYuqgMJqmRRHr1WK3",
"1BVgfbYT5rETt9zMAyj5Hchsc5UKy5hX1N",
"154RVYnt7wWaZmhmzSPndNmETcMZpsfYPb",
"1MDQZHokWDgn3trbT1LVqTRo5o9PHdUUvd",
"1rRUFMYMp8LZDbaDo9ML1jWzDNoYZMtT3",
"1CmGjesXA3MUCBkdS4QavvTejzDe3RBZRB",
"1DxoypR6PGFBt7WQsaeyAghRKGUMBebTiZ",
"1FBpdMdrLASTF5RTLU2BBRaXQG9BiU539V",
"17rFNaVomLpYLZowPrgW6CwzuNvYFu3cp3",
"1KfDzXwMvqHpsh7RwpqD4rvvjZBp5AoRLj",
"1i4mwcBE3VRt5GjpSbV5kM4yeKPgGKZKq",
"19mrZiuYvaFF5auYkeWTFP2ezbyX1LZWs",
"1EUX2BLEiPjn9vF8ZjKTmGeRRZjdq2qakw",
"115XxpRrmaV4pBWpNA26u6WFmLvqj38Np9",
"1GYFV69Ri3qpdbTMHcw1LFDg4BLqexQv6o",
"15XfXgJ9wSwho5FS9M4ooAmFdC23vAGE75",
"1DgU5bMxZz8yQTfzzE62WJfHd3dhFcyU15",
"1EHri9JuFh2ikwVzhfWKdaMeon5dzFqPLc",
"1EDxVL8xH3y2dCyNisRf9N7AF5nsLeLXbU",
"1AdMafCn6joY36RfeetQT3RFZTkgMuS76m",
"19RZX6zwidJ3YdphSoUNz7J3m1u271zj2q",
"1HpgnAjAnChLBiTKzQTyR2uF5Jf1Mgcb8A",
"16ykqNW4v2JvCme7j2wKYmAMkPsSBRur5h",
"161zwACsZfg7yViGaC37LsrwZjhuD9UzGE",
"127XUEMteGN1akXKbMAPyMbQfxfDWCW67w",
"1D9avBbpfGsHp3aPqZwqr14BoeV3GDCzmv",
"1DfHRQYu6jVwX3XtyCBYyPWqPsU4nu2kZ8",
"16KnizDFBgnouss8FT6XTr4pAvE6Kb5WCp",
"16rxB4GQpeQoLnGWdPbB3uXVHB1856ehqk",
"1GmFNzezouGtJKh116B2pfbc1j2KKnJBHV",
"15muBErBXKJwydXig5eU5s9bjTNDD2g2Fc",
"19SCpnsUBmU6vzPhkAwBuhLshoA6uGsMjk",
"1Jbz5QZUsaXPmJfQ1dApJGpcHuZQcZQTfz",
"16hhP7cnETqKUcQUxHE9fLKwjpjt2oq8p4",
"19nGWFBfjU79mzFjwJk5D1kqNqapGEfa86",
"1DB5BEgRkbo3YUWN842oFGdetSXg6TbgJM",
"14Tp3WaWBYHzzqHWv1XpngrfGPbVTuUwpj",
"12vYodC1Zx2rqRhVRpNNAndtSK4sUeHtdV",
"1CVhRaDubeBbEh7dTXvbbamohKVP1rDCN7",
"1NfUaKFyX3mPcFpKgSmkYT7Mqa76KxqfpJ",
"1B4BtwigSQeaj5w58hbG7EsbGmvKCc7u6Q",
"1NVrbVwjWLVhPwzeMFCXiQqKH9TCvfRsp4",
"1PZigvYYhcrhpnPvg9pQWpNeCXDKeBXDHF",
"123xXfWM9HCVSVuQTF2CBmMoPdJxoZiLSe",
"15gZ6qibwrbyLUVfeF3e6rdn1HkaB5Dcty",
"17yb9zchA7xZmiWLv9R7vU5x9eHERPTqrE",
"1MkuzhcJBYMsEZkdTPn6nvjN84dDmtXr1S",
"1mjGhzMU8AJJnaKZaxeURbgMBz8K1F91t",
"1Jn5qGhiV6oVLceP2GhzLC5es71dZ3PAjg",
"1CFBn2FD3qpW2KXBbZpE4qoEVbeb2QJnoP",
"1EjYWpoJiz3THPdWf85utDxy9U3JhAgHL9",
"175orqf55y2SxsSfyiLSYRxyvvF6njStHA",
"1Q5LRxyxHjHPuwgt1DQstuy3Dk6Xxi5BpA",
"1NXeDayqYuMsScA1G8mzZhPktgtZQomK41",
"15VVFL9YeWgHJKbtoqX12j55wfwjY2zv2u",
"1EjJQLagRJ8izvAiBPwh52TSKBq2Q4zaoJ",
"18Q44HVVuZX2hPm1TW9FQbzkmsFUzfs66Z",
"1JypotvGXE4hKLjFT8pLvRA5AGxuWLFi2f",
"16UvKKnveKuHodBiWKdqkuvwcRBXUx3xAr",
"1NNqkpgk584Qzd5Ukf5YaRoiUJrriwGjVt",
"12a2pQHPjdMNkuiUi6VYv57XW1SSkbXET6",
"1HyFTVm6EUprtqP6V9YVR5Zfex1bt6gYv9",
"1BbRMoBEi6FiSCvQvuLKQ8XR3MXLXHZCdG",
"1GxqL2BHTMirJW1cPCUAzZnEJ8wHSwYiqn",
"15JdxJV3mSnj3svET5t5E1H9XCSSdbcgXf",
"18PURcbA2qqg36y1YBcwP21jk4pB36aVUW",
"19oKYYMUZpfa4cXqnoyS2MDgSdc9LLP5jV",
"12SbkZxwBhCHCSPDLQd6pnEqHBvDsF7F8L",
"16W3MJNqkVygB93gQySXd7jPmh9DUoghGL",
"18SS35e6VwXCJrkpVY2TiBfVh953SQjFTg",
"12pTXLQyPJw2st8mxHdJk2zihwawq275yZ",
"1FUJyEfysY7XwpN7vh1zLaPASsKQYPtHsH",
"1C1CsVNvdQdxasfBXTqK9kmtV678Zaq7Ud",
"1Fj2YpUuFwrh26QNyaVTTm6STJn5DyeiLc",
"1KehBFRXYJbwknmtnXTvn82ZMAH41LziE4",
"1CArmPTziBTyAv5tsSbY68tJiyn1YHe3KU",
"1Mkshr5AxRJbxCMHHbmDMMcF6cwiLp652j",
"16tTA3TTfVrQwcRHAGHHDuxUoTwYxhNeDC",
"168YG6YBWWZtW81Fs14n3Quj4wnj8c8vmd",
"1Fcc9LeHD9gPYkMtQVJZnfhoezGioi2kgZ",
"1EPjHtTL3XXzXu3QRuJ3aHFgYX9uXXJhx7",
"171e5N6vQPwepWaTXsiY9VB9kmq1Qdqbvq",
"1JqTucsvrupm6fqNz6PTqBrMfyYgzAVPiF",
"1NVGreJuzUcS2oXJCj8Yegdd53QEXinJjj",
"1LjjSuvECMHziy6a28JVenL1PQGDCp6yoA",
"1DM2Zc7xJ6qzrNfDPc4fwrpLFSruFvFCA5",
"1C2b55Jh1AAAp25LHLPYKMXVcNuRZApuRC",
"1GyVKN5F6EriwDvXTBAFh145eZHMfHz3hD",
"198LNNdFjq3Ab9UiyiYz2pQ5rwViHNEyDz",
"12J1cwQPPHgJM7vvfQn9bmuR7Hnph7n7gQ",
"1DroVM9hLK1i47NbMxrkiR3n19SRfNB9W3",
"1NSbchH3p79WeKviwCWr8fodE95iizie5P",
"1NTDdPJLAkuFYqhQXDewD3Sd6tKgB4KPB7",
"1E1fPdnAMqtpToar96bH88mmyzmGp2rtRz",
"16q3CZBcGphCQ5pgVVEbifbv6KdmYJpSXB",
"1N4JpdqwWHxVio68Bj7hLM1442wuMiaDMr",
"1BRiezLAAy4YfA1rFSEKyXnvzVUPURXTSm",
"1ERCVHQfALVWF1bQLvAdUBUvE3N6gWHUTa",
"1Mi9w9M9tTv2rXWRNCWUoqGppQYuLeCgG9",
"1GEmY1qTzZv47U5WzRtPTBRS7JeL3ewFe",
"15puHTbwo19CbYa1eszMCwZSGgJxi3o14D",
"1NC4cj2AmjNZtZFXeq8RxayH683nf7PiFD",
"17zfY29XN7pkyo9Ug4FNJ6XzNhs3WHVxLo",
"1HeNBRcHoFAsANArhgXrsUTxxTKF4yTs2w",
"1JmCNoYQCy46ufEJDvBjeN24iBFbyNSRqS",
"1P1JpfwpUZsArCWgx66tnM87TGgX8fDCcv",
"196z1GhNPj2vfqUoxeyScvCaQsogCkdJxq",
"1EkLU8AHbFWBFqN6DgtSNHzYw5LqaVriW2",
"158ShE3pFdeFQMHNAmfrRXxj3i55in2VNH",
"1QPK6wkqhPCxpvjV6jgCpR6ZzLjDj9tyY",
"1KGMRKTLV2nonXWJXFz8ZkSpmoCtG2WbXS",
"1Qusche6YkbcKdaV5wFnDARKqLR5Rv2LV",
"1F68A5zF8in9ubYqdS1fMN3K3Y89wztEZz",
"1599tQCzHtawFUwL5zQ9vxigAmGZLjNXbs",
"1GrJx8cn1NqqL31Ecdf94g79KSp74roXwk",
"1NDrNMSH2umbzRbCYvsYhcehEYu9uFYAcj",
"138h2uDEV9G4GkEJN2chepKxywowKxrNcT",
"1JfkpoFUj6uSLdnjRT6KZjgAWfHWzu5cPW",
"1DXhAiymh2H81vbbfesKXcTAavWyj1Nyg7",
"1LMkVpFzo9qxU7uknBnD2AePXx2g29Bvqa",
"1NjXasw6WkPsd5HxaSw8tRBSFUJgFgE4PZ",
"16rUM2P49Hm6SEV76aVcwVgTu6diQTMJYm",
"1MeCQwroxJJhXjtzPUKPWxHYzCapooR9Df",
"1BcGdDwNbJhUrKf1jupmx5FghenBu8c1fH",
"1LHLaCTZ55kBsFkgJjDmf46rVMTTXWN8MA",
"1JfexQsoAsX3RyhDESpKUwpVpTtKSync6M",
"1KKREZb1Z1A4AYceGCqrq254uixwRNVBNn",
"1LrSdJvMEc9jfVpzDezPGqdVzJkKchAghF",
"1Lb7VHvPmeLmrufZdZCZXQ37R5KfRMzRxV",
"1FzAg7GihwEMWQWGpYMMaoTJG7Z4WspC7c",
"1LLSytqBgG5qdVkrcWWG834fn3GyCjQJy7",
"13hJhbkShw2zhi7Ebn9QB6qLup22gRdroJ",
"1JqThmzwWwqPrvyz4mqjus6qaZMboX7nVY",
"1GChyKRiE33M6zNTxVwgEhjsvW3Ho5oQhZ",
"1CqtfPLHCVCjr1tno5FRWjs3h7rw79EinC",
"141e5L9EhYZMqHJdftGBEvPafvXKEoaHKD",
"16W3KiXV55EjZPKFj8VvvWTB1Y2ERPGtwJ",
"1NJnc5pPcYBp4ZB5ySeVnenN99a6Dj8WAU",
"14Z1ubBVq1EtKrrGJ2G2S4JWPRvShFVQVQ",
"1AFrCpzz1geRSHb5M8e44oE8aLYhb6p1rV",
"1BDwzzVKCH2FmvsH1Yuy1hrT9VgPLLisSv",
"1Dww9mHN9Dnc1uPm48HfF5dGi5vxsecYZU",
"1JFydPmgVQFHs12u9LkH6dgPeG63SjNebB",
"13QLohWcbyjpFgcTrShBbaASW4rD9aBNsv",
"17GZggJ3nTf3yYTas19CgNaLTMsTPHcJm5",
"15syxgne6gdLAs1BNPfD8bu8kbpQk32zEz",
"16j3otNwK8NEVEzfrAPnWX9NYD8u1CHZfG",
"1DWJiFaeja3cx3wRZkyrpGkic9x4uuguik",
"1HKjxrDRSkToiMk4Ywh6g4oLXZR5uT3c5b",
"1Cpt5Gr7rEv9LtTQHc6EvhVN8L8C2B85j9",
"1BqJGKnmeDH4QaLCbb14aXVHnEiaSHH3NY",
"1JhhR656fp92kwNrJa2DJXbLSNTq6VtwZR",
"16BATp9A5JeZCSfuz166Q7U3DdkHGNMiD4",
"1FKHXPiYbUKCC45taLkjHBqMPaxF4QjQxX",
"1Q4jBSgeq7KirwmjoQUYtES5yqiwzduXDB",
"1Di71szKLJz4SADSbxCi3kh6FxvRo2onDK"
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
