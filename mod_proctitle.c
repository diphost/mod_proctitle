/*
** mod_proctitle.c - Apache proctitle module
**
** This module sets apache process titles to reflect the request currently
** processed, and some statistics so they will be visible. in top(1) or ps(1).
** Useful for debugging purposes. Very quickly and clearly.
**
**    DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
**    Version 2, December 2004
**
**    Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>
**
**    Everyone is permitted to copy and distribute verbatim or modified
**    copies of this license document, and changing it is allowed as long
**    as the name is changed.
**
**    DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
**    TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION
**
**    0. You just DO WHAT THE FUCK YOU WANT TO.
*/

/*
**  To play with this sample module first compile it into a
**  DSO file and install it into Apache's modules directory
**  by running:
**
**    $ apxs -c -i mod_proctitle.c
**
**  To activate it in Apache's httpd.conf file for instance
**    #   httpd.conf
**    LoadModule proctitle_module modules/mod_proctitle.so
**    <IFModule proctitle_module>
**        # Default: Off
**        ProctitleEnable On
**        # Default: ''
**        ProctitleIdent pool01
**    </IFmodule>
**
**    ProctitleIdent can to set from PROCTITLEIDENT enviroment
**    variable
*/

/* WTF?!!! for ap_show_mpm() definition on Apache < 2.4 */
#define CORE_PRIVATE

#if APR_HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <apr_strings.h>
#include "unistd.h"
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_connection.h"
#include "http_protocol.h"
#include "mpm_common.h"
#include "scoreboard.h"
#include "ap_mpm.h"
#include "ap_listen.h"


#define KBYTE 1024
#define KBYTE8 128
#define MBYTE 1048576L

/* Apache version < 2.4 compat */
#if HTTP_VERSION(AP_SERVER_MAJORVERSION_NUMBER, AP_SERVER_MINORVERSION_NUMBER) < 2004 
    /* r->useragent_ip is more accurate in this case in Apache 2.4 */
    #define useragent_ip connection->remote_ip
    /* new scoreboard function in Apache 2.4 */
    #define ap_get_scoreboard_worker_from_indexes ap_get_scoreboard_worker
#endif /* Apache version < 2.4 compat */

/* Module declaration */
module AP_MODULE_DECLARE_DATA proctitle_module;

static char* procident;               /* Identification string. Suggests that 
                                         it may be different for different profiles. */
unsigned int enabled;                 /* enable/disable */
static unsigned int true_mpm = 0;     /* We change the line after the query only for 
                                         "prefork" module. For perfomance...  */


/* Linux have not setproctitle() function 
dirty-dirty hack... Not works correctly with init.d on most systems*/
#ifdef __linux
extern char *ap_server_argv0;
char proctitle_buf[128];
static void setproctitle(const char *fmt,...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(proctitle_buf,sizeof(proctitle_buf),fmt, va);
    va_end(va);
    strncpy(ap_server_argv0,proctitle_buf,128)
}
#endif

/* SO_LISTENQLEN is only FreeBSD stuff*/
#ifdef SO_LISTENQLEN
/* Try to get the number of requests in the listen queue.
This estimation parameters, we do not want to handle the errors */
/* Shared Listen Queue Lenght values. Not threadsafe */
static int listenqlen = 0, listenincqlen = 0;

static void getlistenqlen(int *listenqlensum, int *listenincqlensum)
{
    socklen_t len;
    ap_listen_rec *lr;
    for (lr = ap_listeners; lr != NULL; lr = lr->next) {
        int sock;
        int listenqlen = 0, listenincqlen = 0;
        if (apr_os_sock_get(&sock, lr->sd) == APR_SUCCESS) {
            len  = sizeof(listenincqlen);
            if(getsockopt(sock, SOL_SOCKET, SO_LISTENINCQLEN, &listenincqlen, &len) == 0) {
                (*listenincqlensum) += listenincqlen;
            }
            len = sizeof(listenqlen);
            if (getsockopt(sock, SOL_SOCKET, SO_LISTENQLEN, &listenqlen, &len) == 0) {
                (*listenqlensum) += listenqlen;
            }
        }
    }
}
#endif /* SO_LISTENQLEN */

/* ServerLimit, ThreadLimit for Scoreboard */
static int server_limit, thread_limit;

#define STATUS_VECTOR_LEN 10
#define STATUS_FASE 6
#define STATUS_TIME 10
typedef struct s_status {
    unsigned long count;
    apr_off_t bcount;
    apr_time_t curtime;
} sstatus;
static sstatus vstatus[STATUS_VECTOR_LEN];
static int curstatus;
/* Cached status values */
static unsigned long qps, kbps;

static int proctitle_config(apr_pool_t *pconf, apr_pool_t *plog,
                          apr_pool_t *ptemp, server_rec *s)
{
    int i;
/* Apache >= 2.4 */
#if HTTP_VERSION(AP_SERVER_MAJORVERSION_NUMBER, AP_SERVER_MINORVERSION_NUMBER) >= 2004
    const char* mpm_name = ap_run_mpm_get_name();
#else
    const char* mpm_name = ap_show_mpm();
#endif  /* Apache >= 2.4 */
    true_mpm = strcmp(mpm_name, "prefork") ? 0 : 1;
    /* Get ident string from enviroment */
    if (procident == NULL) {
        procident = getenv("PROCTITLEIDENT");
    }
    if (enabled != 0 && procident != NULL) {
        setproctitle("[M] %s", procident);
    }
    /* Status init*/
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_DAEMONS, &server_limit);
    apr_time_t curtime = apr_time_now();
    for(i=0; i<STATUS_VECTOR_LEN;i++) {
        vstatus[i].count = vstatus[i].bcount = 0;
        vstatus[i].curtime = curtime;
    }
    curstatus = 0;
    qps = kbps = 0;
    return OK;
}

/* Apache >= 2.4 */
#if HTTP_VERSION(AP_SERVER_MAJORVERSION_NUMBER, AP_SERVER_MINORVERSION_NUMBER) >= 2004
static int proctitle_monitor(apr_pool_t *p, server_rec *s)
#else
static int proctitle_monitor(apr_pool_t *p)
#endif /* Apache >= 2.4 */
{
    int i, j;
    if (enabled != 0 && procident != NULL) {
        /* Status for main proctitle. Some code from mod_status.c */
        int prevstatus,prevstatus2;
        prevstatus=curstatus-1;
        if (prevstatus < 0)
            prevstatus += STATUS_VECTOR_LEN;
        prevstatus2=curstatus-STATUS_FASE;
        if (prevstatus2 < 0)
            prevstatus2 += STATUS_VECTOR_LEN;
        apr_time_t nowtime = apr_time_now();
        apr_interval_time_t delta_time = apr_time_sec(nowtime - vstatus[prevstatus].curtime);
        apr_interval_time_t delta_time2 = apr_time_sec(nowtime - vstatus[prevstatus2].curtime);
        if (ap_exists_scoreboard_image() && delta_time >= (apr_interval_time_t) STATUS_TIME) {
            unsigned long count,lres;
            apr_off_t bytes, bcount;
            count = bcount = 0;
            for (i = 0; i < server_limit; ++i) {
                process_score *ps_record = ap_get_scoreboard_process(i);
                for (j = 0; j < thread_limit; ++j) {
                    /* TODO: Apache 2.4.10 change to ap_copy_scoreboard_worker */
                    worker_score *ws_record = ap_get_scoreboard_worker_from_indexes(i, j);
                    if (ap_extended_status) { 
                        lres = ws_record->access_count;
                        bytes = ws_record->bytes_served;
                        count += lres;
                        bcount += bytes;
                    }
                }
            }
            qps = (count - vstatus[prevstatus2].count) / delta_time;
            kbps = (bcount - vstatus[prevstatus2].bcount) / KBYTE8 / delta_time;
            vstatus[curstatus].count = count;
            vstatus[curstatus].bcount = bcount;
            vstatus[curstatus].curtime = nowtime;
            if (++curstatus >= STATUS_VECTOR_LEN)
                curstatus = 0;
        }
/* SO_LISTENQLEN is only FreeBSD stuff*/
#ifdef SO_LISTENQLEN
        listenqlen = listenincqlen = 0;
        getlistenqlen(&listenqlen,&listenincqlen);
        setproctitle("[M   lq: %d|%d,  qps: %u,  rate: %u Kbps]   %s", listenqlen, listenincqlen, qps, kbps, procident);
#else
        setproctitle("[M   qps: %u,  rate: %u Kbps]   %s", qps, kbps, procident);
#endif /* SO_LISTENQLEN */
    }
    return OK;
}


static void proctitle_child_init(apr_pool_t *pchild, server_rec *s)
{
    if (enabled != 0 && procident != NULL) {
        setproctitle("[C] %s", procident);
    }
}

/* Set proctitle after request, only for prefork, not ProctitleIdent string */
static int proctitle_clear(request_rec *r)
{
    if (enabled != 0 && true_mpm) {
/* SO_LISTENQLEN is only FreeBSD stuff*/
#ifdef SO_LISTENQLEN
        /*TODO: Try to get new lenght values, if request time too long (more 1 second)
                Not yet found a way to accuratly get the time the request
                is complete without a separate call apr_time_now() function
         */
        setproctitle("[I lq: %d|%d] [%s] %s %s", listenqlen, listenincqlen,
           r->useragent_ip,
           r->hostname,
           r->the_request);
#else
        setproctitle("[I] [%s] %s %s",r->useragent_ip,
            r->hostname,
            r->the_request);
#endif /* SO_LISTENQLEN */
    }
    return DECLINED;
}

/* Set proctitle on request, only for prefork, not ProctitleIdent string */
static int proctitle_readreq(request_rec *r)
{
    if (enabled != 0 && true_mpm) {
/* SO_LISTENQLEN is only FreeBSD stuff*/
#ifdef SO_LISTENQLEN
        listenqlen = listenincqlen = 0;
        getlistenqlen(&listenqlen,&listenincqlen);
        setproctitle("[B lq: %d|%d] [%s] %s %s", listenqlen, listenincqlen,
           r->useragent_ip,
           r->hostname,
           r->the_request);
#else
        setproctitle("[B] [%s] %s %s",r->useragent_ip,
           r->hostname,
           r->the_request);
#endif /* SO_LISTENQLEN */
    }
    return DECLINED;
}

/* Set default */
static void *cfg_init(apr_pool_t *p, server_rec *s)
{
    enabled = 0;
    return NULL;
}

/* Set config enabled/disabled */
static const char *proctitle_flag(cmd_parms *cmd, void *mconfig, int flag)
{
    enabled = flag;
    return NULL;
}

/* Set config ident string */
static const char *proctitle_ident(cmd_parms *cmd, void *mconfig, const char *arg)
{
    /* Get ident string from config */
    if (arg != NULL && cmd != NULL) {
        procident = apr_pstrdup(cmd->pool, arg);
    }
    return NULL;
}


static void proctitle_register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(proctitle_config, NULL, NULL, APR_HOOK_LAST);
    ap_hook_child_init(proctitle_child_init, NULL, NULL, APR_HOOK_LAST);
    ap_hook_post_read_request(proctitle_readreq, NULL, NULL, APR_HOOK_LAST);
    ap_hook_log_transaction(proctitle_clear,NULL,NULL, APR_HOOK_LAST);
    ap_hook_monitor(proctitle_monitor,NULL,NULL, APR_HOOK_LAST);
}

static const command_rec proctitle_commands[] =
{
    AP_INIT_FLAG("ProctitleEnable", proctitle_flag, NULL, RSRC_CONF, \
    "Enable/disable setting process titles (off by default)"),
    AP_INIT_TAKE1("ProctitleIdent", proctitle_ident, NULL, RSRC_CONF, \
    "Set ident string ('' by default)"),
    {NULL, {NULL}, NULL, 0, 0, NULL},
};

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA proctitle_module = {
   STANDARD20_MODULE_STUFF,
   NULL,                     /* create per-dir    config structures */
   NULL,                     /* merge  per-dir    config structures */
   cfg_init,                 /* create per-server config structures */
   NULL,                     /* merge  per-server config structures */
   proctitle_commands,       /* table of config file commands       */
   proctitle_register_hooks  /* register hooks                      */
};
