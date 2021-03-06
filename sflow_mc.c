/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <sysexits.h>
#include <stddef.h>
#include <syslog.h>

#include "sflow_mc.h"

#include "sflow_api.h"

#define SFMC_DEFAULT_CONFIGFILE "/etc/hsflowd.auto"
#define SFMC_SEPARATORS " \t\r\n="
/* SFMC_MAX LINE LEN must be enough to hold the whole list of targets */
#define SFMC_MAX_LINELEN 1024
#define SFMC_MAX_COLLECTORS 10

typedef struct _SFMCCollector {
    struct sockaddr sa;
    SFLAddress addr;
    uint16_t port;
    uint16_t priority;
} SFMCCollector;

typedef struct _SFMCConfig {
    int error;
    uint32_t sampling_n;
    uint32_t polling_secs;
    SFLAddress agentIP;
    uint32_t num_collectors;
    SFMCCollector collectors[SFMC_MAX_COLLECTORS];
} SFMCConfig;

typedef struct _SFMC {
    /* sampling parameters */
    uint32_t sflow_random_seed;
    uint32_t sflow_random_threshold;
    /* the sFlow agent */
    SFLAgent *agent;
    /* need mutex when building sample */
    pthread_mutex_t *mutex;
    /* time */
    struct timeval start_time;
    rel_time_t tick;
    /* config */
    char *configFile;
    time_t configFile_modTime;
    SFMCConfig *config;
    uint32_t configTests;
    /* UDP send sockets */
    int socket4;
    int socket6;
} SFMC;

#define SFLOW_DURATION_UNKNOWN 0

/* file-scoped globals */
static SFMC sfmc;

#define SFMC_LOCK() pthread_mutex_lock(sfmc.mutex)
#define SFMC_UNLOCK() pthread_mutex_unlock(sfmc.mutex)

static void sflow_tick(rel_time_t current_time);
static void sfmc_init_config(SFMC *sm);

static void *sfmc_calloc(size_t bytes)
{
    void *mem = calloc(1, bytes);
    if(mem == NULL) {
        perror("sfmc_calloc");
        exit(EXIT_FAILURE);
    }
    return mem;
}

static void *sfmc_cb_alloc(void *magic, SFLAgent *agent, size_t bytes)
{
    return sfmc_calloc(bytes);
}

static int sfmc_cb_free(void *magic, SFLAgent *agent, void *obj)
{
    free(obj);
    return 0;
}

static void sfmc_cb_error(void *magic, SFLAgent *agent, char *msg)
{
    perror(msg);
}

static void sfmc_cb_counters(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
{
    SFMC *sm = (SFMC *)poller->magic;

    if(sm->config == NULL ||
       sm->config->polling_secs == 0) {
        /* not configured */
        return;
    }

    // sm->mutex should already be acquired here (when we generate the tick)

    SFLCounters_sample_element mcElem = { 0 };
    mcElem.tag = SFLCOUNTERS_MEMCACHE;

    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);
    struct slab_stats slab_stats;
    slab_stats_aggregate(&thread_stats, &slab_stats);

    STATS_LOCK();
    mcElem.counterBlock.memcache.cmd_set = slab_stats.set_cmds;
    mcElem.counterBlock.memcache.cmd_touch = thread_stats.touch_cmds;
    mcElem.counterBlock.memcache.cmd_flush = thread_stats.flush_cmds;
    mcElem.counterBlock.memcache.get_hits = slab_stats.get_hits;
    mcElem.counterBlock.memcache.get_misses = thread_stats.get_misses;
    mcElem.counterBlock.memcache.delete_hits = slab_stats.delete_hits;
    mcElem.counterBlock.memcache.delete_misses = thread_stats.delete_misses;
    mcElem.counterBlock.memcache.incr_hits = slab_stats.incr_hits;
    mcElem.counterBlock.memcache.incr_misses = thread_stats.incr_misses;
    mcElem.counterBlock.memcache.decr_hits = slab_stats.decr_hits;
    mcElem.counterBlock.memcache.decr_misses = thread_stats.decr_misses;
    mcElem.counterBlock.memcache.cas_hits = slab_stats.cas_hits;
    mcElem.counterBlock.memcache.cas_misses = thread_stats.cas_misses;
    mcElem.counterBlock.memcache.cas_badval = slab_stats.cas_badval;
    mcElem.counterBlock.memcache.auth_cmds = thread_stats.auth_cmds;
    mcElem.counterBlock.memcache.auth_errors = thread_stats.auth_errors;
    mcElem.counterBlock.memcache.threads = settings.num_threads;
    mcElem.counterBlock.memcache.conn_yields = thread_stats.conn_yields;
    mcElem.counterBlock.memcache.listen_disabled_num = stats.listen_disabled_num;
    mcElem.counterBlock.memcache.curr_connections = stats.curr_conns;
    mcElem.counterBlock.memcache.rejected_connections = stats.rejected_conns;
    mcElem.counterBlock.memcache.total_connections = stats.total_conns;
    mcElem.counterBlock.memcache.connection_structures = stats.conn_structs;
    mcElem.counterBlock.memcache.evictions = stats.evictions;
    mcElem.counterBlock.memcache.reclaimed = stats.reclaimed;
    mcElem.counterBlock.memcache.curr_items = stats.curr_items;
    mcElem.counterBlock.memcache.total_items = stats.total_items;
    mcElem.counterBlock.memcache.bytes_read = thread_stats.bytes_read;
    mcElem.counterBlock.memcache.bytes_written = thread_stats.bytes_written;
    mcElem.counterBlock.memcache.bytes = stats.curr_bytes;
    mcElem.counterBlock.memcache.limit_maxbytes = settings.maxbytes;
    STATS_UNLOCK();
    SFLADD_ELEMENT(cs, &mcElem);
    sfl_poller_writeCountersSample(poller, cs);
}

static SFLMemcache_prot sflow_map_protocol(enum protocol prot) {
    SFLMemcache_prot sflprot = SFMC_PROT_OTHER;
    switch(prot) {
    case ascii_prot: sflprot = SFMC_PROT_ASCII; break;
    case binary_prot: sflprot = SFMC_PROT_BINARY; break;
    case negotiating_prot:
    default: break;
    }
    return sflprot;
}

static SFLMemcache_operation_status sflow_map_status(int ret) {
    /* need to turm "EXISTS" into "SFMC_OP_DELETED" if the command was "DELETE" $$$ */
    SFLMemcache_operation_status sflret = SFMC_OP_UNKNOWN;
    switch(ret) {
    case STORED: sflret = SFMC_OP_STORED; break;
    case EXISTS: sflret = SFMC_OP_EXISTS; break;
    case NOT_FOUND: sflret = SFMC_OP_NOT_FOUND; break;
    case NOT_STORED: sflret = SFMC_OP_NOT_STORED; break;
    }
    return sflret;
}


static SFLMemcache_cmd sflow_map_ascii_op(int op) {
    SFLMemcache_cmd sflcmd = SFMC_CMD_OTHER;
    switch(op) {
    case NREAD_ADD: sflcmd=SFMC_CMD_ADD; break;
    case NREAD_REPLACE: sflcmd = SFMC_CMD_REPLACE; break;
    case NREAD_APPEND: sflcmd = SFMC_CMD_APPEND; break;
    case NREAD_PREPEND: sflcmd = SFMC_CMD_PREPEND; break;
    case NREAD_SET: sflcmd = SFMC_CMD_SET; break;
    case NREAD_CAS: sflcmd = SFMC_CMD_CAS; break;
        /*            SFMC_CMD_GET */
        /*             SFMC_CMD_GETS */
        /*             SFMC_CMD_INCR */
        /*             SFMC_CMD_DECR */
        /*             SFMC_CMD_DELETE */
        /*             SFMC_CMD_STATS */
        /*             SFMC_CMD_FLUSH */
        /*             SFMC_CMD_VERSION */
        /*             SFMC_CMD_QUIT */
    default:
        break;
    }
    return sflcmd;
}

static SFLMemcache_cmd sflow_map_binary_cmd(int cmd) {
    SFLMemcache_cmd sflcmd = SFMC_CMD_OTHER;
    switch(cmd) {
    case PROTOCOL_BINARY_CMD_GET: sflcmd = SFMC_CMD_GET; break;
    case PROTOCOL_BINARY_CMD_SET: sflcmd = SFMC_CMD_SET; break;
    case PROTOCOL_BINARY_CMD_ADD: sflcmd = SFMC_CMD_ADD; break;
    case PROTOCOL_BINARY_CMD_REPLACE: sflcmd = SFMC_CMD_REPLACE; break;
    case PROTOCOL_BINARY_CMD_DELETE: sflcmd = SFMC_CMD_DELETE; break;
    case PROTOCOL_BINARY_CMD_INCREMENT: sflcmd = SFMC_CMD_INCR; break;
    case PROTOCOL_BINARY_CMD_DECREMENT: sflcmd = SFMC_CMD_DECR; break;
    case PROTOCOL_BINARY_CMD_QUIT: sflcmd = SFMC_CMD_QUIT; break;
    case PROTOCOL_BINARY_CMD_FLUSH: sflcmd = SFMC_CMD_FLUSH; break;
    case PROTOCOL_BINARY_CMD_GETQ: sflcmd = SFMC_CMD_GET; break;
    case PROTOCOL_BINARY_CMD_NOOP: break;
    case PROTOCOL_BINARY_CMD_VERSION: sflcmd = SFMC_CMD_VERSION; break;
    case PROTOCOL_BINARY_CMD_GETK: sflcmd = SFMC_CMD_GET; break;
    case PROTOCOL_BINARY_CMD_GETKQ: sflcmd = SFMC_CMD_GET; break;
    case PROTOCOL_BINARY_CMD_APPEND: sflcmd = SFMC_CMD_APPEND; break;
    case PROTOCOL_BINARY_CMD_PREPEND: sflcmd = SFMC_CMD_PREPEND; break;
    case PROTOCOL_BINARY_CMD_STAT: sflcmd = SFMC_CMD_STATS; break;
    case PROTOCOL_BINARY_CMD_SETQ: sflcmd = SFMC_CMD_SET; break;
    case PROTOCOL_BINARY_CMD_ADDQ: sflcmd = SFMC_CMD_ADD; break;
    case PROTOCOL_BINARY_CMD_REPLACEQ: sflcmd = SFMC_CMD_REPLACE; break;
    case PROTOCOL_BINARY_CMD_DELETEQ: sflcmd = SFMC_CMD_DELETE; break;
    case PROTOCOL_BINARY_CMD_INCREMENTQ: sflcmd = SFMC_CMD_INCR; break;
    case PROTOCOL_BINARY_CMD_DECREMENTQ: sflcmd = SFMC_CMD_DECR; break;
    case PROTOCOL_BINARY_CMD_QUITQ: sflcmd = SFMC_CMD_QUIT; break;
    case PROTOCOL_BINARY_CMD_FLUSHQ: sflcmd = SFMC_CMD_FLUSH; break;
    case PROTOCOL_BINARY_CMD_APPENDQ: sflcmd = SFMC_CMD_APPEND; break;
    case PROTOCOL_BINARY_CMD_PREPENDQ: sflcmd = SFMC_CMD_PREPEND; break;
        //case PROTOCOL_BINARY_CMD_VERBOSITY: break;
        //case PROTOCOL_BINARY_CMD_TOUCH: break;
        //case PROTOCOL_BINARY_CMD_GAT: break;
        //case PROTOCOL_BINARY_CMD_GATQ: break;
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: break;
    case PROTOCOL_BINARY_CMD_SASL_AUTH: break;
    case PROTOCOL_BINARY_CMD_SASL_STEP: break;
    default:
        break;
    }
    return sflcmd;
}

/* This is the 32-bit PRNG recommended in G. Marsaglia, "Xorshift RNGs",
 * _Journal of Statistical Software_ 8:14 (July 2003).  According to the paper,
 * it has a period of 2**32 - 1 and passes almost all tests of randomness.  It
 * is currently also used for sFlow sampling in the Open vSwitch project
 * at http://www.openvswitch.org.
 */

void sflow_sample_test(struct conn *c) {

    if(unlikely(sfmc.tick != current_time)) {
        /* generate ticks here now - rather than from ISR */
        sfmc.tick = current_time;
        sflow_tick(current_time);
    }

    c->thread->sflow_sample_pool++;
    c->thread->sflow_random ^= c->thread->sflow_random << 13;
    c->thread->sflow_random ^= c->thread->sflow_random >> 17;
    c->thread->sflow_random ^= c->thread->sflow_random << 5;
    if(unlikely(c->thread->sflow_random <= sfmc.sflow_random_threshold)) {

        if(sfmc.config == NULL || sfmc.config->sampling_n == 0) {
            /* sampling wasn't actually configured yet */
            return;
        }

        /* Relax. We are out of the critical path now. */
        /* since we are sampling at the start of the transaction
           all we have to do here is record the wall-clock time.
           The rest is done at the end of the transaction.
           We could use clock_gettime(CLOCK_REALTIME) here to get
           nanosecond resolution but it is not always implemented
           as efficiently as gettimeofday and it's not clear that
           that we can really do better than microsecond accuracy
           anyway. */
        gettimeofday(&c->sflow_start_time, NULL);
        /* try and record the command, but c->cmd may not be set yet
           particularly for the ascii protocol,  so we have to be
           prepared to look at this again when we take the sample */
        c->sflow_op = (c->protocol == binary_prot) ?
            sflow_map_binary_cmd(c->cmd) :
            sflow_map_ascii_op(c->cmd);
    }
}

void sflow_sample(SFLMemcache_cmd command, struct conn *c, const void *key, size_t keylen, uint32_t nkeys, size_t value_bytes, int status)
{
    SFMC *sm = &sfmc;
    SFLSampler *sampler = NULL;

    // use a semaphore to protect access to the config that might otherwise change under our feet
    SFMC_LOCK();
    if(sm->config && sm->config->sampling_n && sm->agent && sm->agent->samplers) {
        sampler = sm->agent->samplers;
    }
    SFMC_UNLOCK();


    if(sampler == NULL) {
        // not configured yet
        return;
    }

    struct timeval timenow,elapsed;
    gettimeofday(&timenow, NULL);
    timersub(&timenow, &c->sflow_start_time, &elapsed);
    timerclear(&c->sflow_start_time);

    SFL_FLOW_SAMPLE_TYPE fs = { 0 };

    /* have to add up the pool from all the threads */
    fs.sample_pool = sflow_sample_pool_aggregate();

    /* indicate that I am the server by setting the
       destination interface to 0x3FFFFFFF=="internal"
       and leaving the source interface as 0=="unknown" */
    fs.output = 0x3FFFFFFF;

    SFLFlow_sample_element mcopElem = { 0 };
    mcopElem.tag = SFLFLOW_MEMCACHE;
    mcopElem.flowType.memcache.protocol = sflow_map_protocol(c->protocol);

    /* sometimes we pass the command in explicitly
       otherwise we allow it to pick up the
       op-code that we stashed at sample-test time.
       However c->cmd may not have been set to anything
       then,  so we can still have another look at it now.
       This extra checking was added because of the way
       that the binary protocol sets c->cmd to the request_op
       at the beginnging but then changes it later to something
       like NREAD_SET.  So the most reliable way to know the
       binary op was to stash it at sample-test time.
    */

    if(command == SFMC_CMD_OTHER) {
        if(c->sflow_op == SFMC_CMD_OTHER) {
            /* we didn't store the cmd at sample_test time
               so just use the inferred one here */
            command = (c->protocol == binary_prot) ?
                sflow_map_binary_cmd(c->cmd) :
                sflow_map_ascii_op(c->cmd);
        }
        else {
            /* it was stored at sample_test time - (probably
               this is the binary protcol) */
            command = c->sflow_op;
        }
    }

    mcopElem.flowType.memcache.command = command;

    mcopElem.flowType.memcache.key.str = (char *)key;
    mcopElem.flowType.memcache.key.len = (key ? keylen : 0);
    mcopElem.flowType.memcache.nkeys = (nkeys == 0) ? 1 : nkeys;
    mcopElem.flowType.memcache.value_bytes = value_bytes;
    mcopElem.flowType.memcache.duration_uS = (elapsed.tv_sec * 1000000) + elapsed.tv_usec;
    mcopElem.flowType.memcache.status = sflow_map_status(status);
    SFLADD_ELEMENT(&fs, &mcopElem);

    SFLFlow_sample_element socElem = { 0 };

    if(c->transport == tcp_transport ||
       c->transport == udp_transport) {
        /* add a socket structure */
        struct sockaddr_storage localsoc;
        socklen_t localsoclen = sizeof(localsoc);
        struct sockaddr_storage peersoc;
        socklen_t peersoclen = sizeof(peersoc);

        /* ask the fd for the local socket - may have wildcards, but
           at least we may learn the local port */
        if(getsockname(c->sfd, (struct sockaddr *)&localsoc, &localsoclen) == -1) {
            if(settings.verbose > 0) {
                perror("sflow_sample() : getsockname() failed");
                memset(&localsoc, 0, sizeof(localsoc));
                localsoclen = 0;
            }
        }

        /* for tcp the socket can tell us the peer info - provided it's still open */
        if(c->transport == tcp_transport) {
            if(getpeername(c->sfd, (struct sockaddr *)&peersoc, &peersoclen) == -1) {
                if(settings.verbose > 0) {
                    perror("sflow_sample() : getpeername() failed");
                }
                memset(&peersoc, 0, sizeof(peersoc));
                peersoclen = 0;
            }
        }
        else {
            /* for UDP the peer can be different for every packet, but
               this info is captured in the recvfrom() and given to us */
            memcpy(&peersoc, &c->request_addr, c->request_addr_size);
            peersoclen = c->request_addr_size;
        }

        /* two possibilities here... */
        struct sockaddr_in *soc4 = (struct sockaddr_in *)&peersoc;
        struct sockaddr_in6 *soc6 = (struct sockaddr_in6 *)&peersoc;

        if(peersoclen == sizeof(*soc4) && soc4->sin_family == AF_INET) {
            struct sockaddr_in *lsoc4 = (struct sockaddr_in *)&localsoc;
            socElem.tag = SFLFLOW_EX_SOCKET4;
            socElem.flowType.socket4.protocol = (c->transport == tcp_transport ? 6 : 17);
            socElem.flowType.socket4.local_ip.addr = lsoc4->sin_addr.s_addr;
            socElem.flowType.socket4.remote_ip.addr = soc4->sin_addr.s_addr;
            socElem.flowType.socket4.local_port = ntohs(lsoc4->sin_port);
            socElem.flowType.socket4.remote_port = ntohs(soc4->sin_port);
        }
        else if(peersoclen == sizeof(*soc6) && soc6->sin6_family == AF_INET6) {
            struct sockaddr_in6 *lsoc6 = (struct sockaddr_in6 *)&localsoc;
            socElem.tag = SFLFLOW_EX_SOCKET6;
            socElem.flowType.socket6.protocol = (c->transport == tcp_transport ? 6 : 17);
            memcpy(socElem.flowType.socket6.local_ip.addr, lsoc6->sin6_addr.s6_addr, 16);
            memcpy(socElem.flowType.socket6.remote_ip.addr, soc6->sin6_addr.s6_addr, 16);
            socElem.flowType.socket6.local_port = ntohs(lsoc6->sin6_port);
            socElem.flowType.socket6.remote_port = ntohs(soc6->sin6_port);
        }
        if(socElem.tag) {
            SFLADD_ELEMENT(&fs, &socElem);
        }
        else {
            if(settings.verbose > 0) {
                fprintf(stderr, "sflow_sample() : unexpected socket length (%d) or address family (v4=%d, v6=%d)\n",
                        peersoclen,
                        soc4->sin_family,
                        soc6->sin6_family);
            }
        }
    }
    SFMC_LOCK();
    sfl_sampler_writeFlowSample(sampler, &fs);
    SFMC_UNLOCK();
}


static void sfmc_cb_sendPkt(void *magic, SFLAgent *agent, SFLReceiver *receiver, u_char *pkt, uint32_t pktLen)
{
    SFMC *sm = (SFMC *)magic;
    size_t socklen = 0;
    int fd = 0;

    if(sm->config == NULL) {
        /* config is disabled */
        return;
    }

    for(int c = 0; c < sm->config->num_collectors; c++) {
        SFMCCollector *coll = &sm->config->collectors[c];
        switch(coll->addr.type) {
        case SFLADDRESSTYPE_UNDEFINED:
            /* skip over it if the forward lookup failed */
            break;
        case SFLADDRESSTYPE_IP_V4:
            {
                struct sockaddr_in *sa = (struct sockaddr_in *)&(coll->sa);
                socklen = sizeof(struct sockaddr_in);
                sa->sin_family = AF_INET;
                sa->sin_port = htons(coll->port);
                fd = sm->socket4;
            }
            break;
        case SFLADDRESSTYPE_IP_V6:
            {
                struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&(coll->sa);
                socklen = sizeof(struct sockaddr_in6);
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = htons(coll->port);
                fd = sm->socket6;
            }
            break;
        }

        if(socklen && fd > 0) {
            int result = sendto(fd,
                                pkt,
                                pktLen,
                                0,
                                (struct sockaddr *)&coll->sa,
                                socklen);
            if(result == -1 && errno != EINTR) {
                if(settings.verbose > 0) {
                    perror("sfmc_cb_sendPkt() : sendTo");
                }
            }
            if(result == 0) {
                if(settings.verbose > 0) {
                    perror("sfmc_cb_sendPkt() : sendTo returned 0");
                }
            }
        }
    }
}

static bool sfmc_lookupAddress(char *name, struct sockaddr *sa, SFLAddress *addr, int family)
{
    struct addrinfo *info = NULL;
    struct addrinfo hints = { 0 };
    hints.ai_socktype = SOCK_DGRAM; /* constrain this so we don't get lots of answers */
    hints.ai_family = family; /* PF_INET, PF_INET6 or 0 */
    int err = getaddrinfo(name, NULL, &hints, &info);
    if(err) {
        switch(err) {
        case EAI_NONAME: break;
        case EAI_NODATA: break;
        case EAI_AGAIN: break; /* loop and try again? */
        default: fprintf(stderr, "sFlow getaddrinfo() error: %s\n", gai_strerror(err)); break;
        }
        return false;
    }

    if(info == NULL) return false;

    if(info->ai_addr) {
        /* answer is now in info - a linked list of answers with sockaddr values. */
        /* extract the address we want from the first one. */
        switch(info->ai_family) {
        case PF_INET:
            {
                struct sockaddr_in *ipsoc = (struct sockaddr_in *)info->ai_addr;
                addr->type = SFLADDRESSTYPE_IP_V4;
                addr->address.ip_v4.addr = ipsoc->sin_addr.s_addr;
                if(sa) memcpy(sa, info->ai_addr, info->ai_addrlen);
            }
            break;
        case PF_INET6:
            {
                struct sockaddr_in6 *ip6soc = (struct sockaddr_in6 *)info->ai_addr;
                addr->type = SFLADDRESSTYPE_IP_V6;
                memcpy(&addr->address.ip_v6, &ip6soc->sin6_addr, 16);
                if(sa) memcpy(sa, info->ai_addr, info->ai_addrlen);
            }
            break;
        default:
            fprintf(stderr, "sFlow getaddrinfo: unexpected address family: %d\n", info->ai_family);
            return false;
            break;
        }
    }
    /* free the dynamically allocated data before returning */
    freeaddrinfo(info);
    return true;
}

static bool sfmc_syntaxOK(SFMCConfig *cfg, uint32_t line, uint32_t tokc, uint32_t tokcMin, uint32_t tokcMax, char *syntax) {
    if(tokc < tokcMin || tokc > tokcMax) {
        cfg->error = true;
        fprintf(stderr, "sFlow syntax error: expected %s on line %ud\n", syntax, line);
        return false;
    }
    return true;
}

static void sfmc_syntaxError(SFMCConfig *cfg, uint32_t line, char *msg) {
    cfg->error = true;
    fprintf(stderr, "sFlow syntax error:%s (on line %u)\n", msg, line);
}

static SFMCConfig *sfmc_readConfig(SFMC *sm)
{
    uint32_t rev_start = 0;
    uint32_t rev_end = 0;
    SFMCConfig *config = (SFMCConfig *)sfmc_calloc(sizeof(SFMCConfig));
    FILE *cfg = NULL;
    if((cfg = fopen(sm->configFile, "r")) == NULL) {
        if(settings.verbose) {
            fprintf(stderr, "cannot open config file %s : %s\n",
                    sm->configFile,
                    strerror(errno));
        }
        return NULL;
    }
    char line[SFMC_MAX_LINELEN+1];
    uint32_t lineNo = 0;
    char *tokv[5];
    uint32_t tokc;
    while(fgets(line, SFMC_MAX_LINELEN, cfg)) {
        lineNo++;
        char *p = line;
        /* comments start with '#' */
        p[strcspn(p, "#")] = '\0';
        /* 1 var and up to 3 value tokens, so detect up to 5 tokens overall */
        /* so we know if there was an extra one that should be flagged as a */
        /* syntax error. */
        tokc = 0;
        for(int i = 0; i < 5; i++) {
            size_t len;
            p += strspn(p, SFMC_SEPARATORS);
            if((len = strcspn(p, SFMC_SEPARATORS)) == 0) break;
            tokv[tokc++] = p;
            p += len;
            if(*p != '\0') *p++ = '\0';
        }

        if(tokc >=2) {
            if(settings.verbose > 1) {
                fprintf(stderr, "line=%s tokc=%u tokv=<%s> <%s> <%s>\n",
                        line,
                        tokc,
                        tokc > 0 ? tokv[0] : "",
                        tokc > 1 ? tokv[1] : "",
                        tokc > 2 ? tokv[2] : "");
            }
        }

        if(tokc) {
            if(strcasecmp(tokv[0], "rev_start") == 0
               && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "rev_start=<int>")) {
                rev_start = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "rev_end") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "rev_end=<int>")) {
                rev_end = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "sampling") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "sampling=<int>")) {
                config->sampling_n = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "sampling.memcache") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "sampling.memcache=<int>")) {
                config->sampling_n = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "polling") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "polling=<int>")) {
                config->polling_secs = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "polling.memcache") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "polling.memcache=<int>")) {
                config->polling_secs = strtol(tokv[1], NULL, 0);
            }
            else if(strcasecmp(tokv[0], "agentIP") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 2, "agentIP=<IP address>|<IPv6 address>")) {
                if(sfmc_lookupAddress(tokv[1],
                                      NULL,
                                      &config->agentIP,
                                      0) == false) {
                    sfmc_syntaxError(config, lineNo, "agent address lookup failed");
                }
            }
            else if(strcasecmp(tokv[0], "collector") == 0
                    && sfmc_syntaxOK(config, lineNo, tokc, 2, 4, "collector=<IP address>[ <port>[ <priority>]]")) {
                if(config->num_collectors < SFMC_MAX_COLLECTORS) {
                    uint32_t i = config->num_collectors++;
                    if(sfmc_lookupAddress(tokv[1],
                                          &config->collectors[i].sa,
                                          &config->collectors[i].addr,
                                          0) == false) {
                        sfmc_syntaxError(config, lineNo, "collector address lookup failed");
                    }
                    config->collectors[i].port = tokc >= 3 ? strtol(tokv[2], NULL, 0) : 6343;
                    config->collectors[i].priority = tokc >= 4 ? strtol(tokv[3], NULL, 0) : 0;
                }
                else {
                    sfmc_syntaxError(config, lineNo, "exceeded max collectors");
                }
            }
            else if(strcasecmp(tokv[0], "header") == 0) { /* ignore */ }
            else if(strcasecmp(tokv[0], "agent") == 0) { /* ignore */ }
            else if(strncasecmp(tokv[0], "sampling.", 9) == 0) { /* ignore */ }
            else if(strncasecmp(tokv[0], "polling.", 8) == 0) { /* ignore */ }
            else {
                // sfmc_syntaxError(config, lineNo, "unknown var=value setting");
            }
        }
    }
    fclose(cfg);

    /* sanity checks... */

    if(config->agentIP.type == SFLADDRESSTYPE_UNDEFINED) {
        sfmc_syntaxError(config, 0, "agentIP=<IP address>|<IPv6 address>");
    }

    if((rev_start == rev_end) && !config->error) {
        return config;
    }
    else {
        free(config);
        return NULL;
    }
}

/* return true if new_config should be applied */
static int sfmc_config_check(SFMC *sm, SFMCConfig **new_config) {
    struct stat statBuf;
    sm->configTests++;
    if(settings.verbose > 1) {
        fprintf(stderr, "checking for config file change <%s>\n", sm->configFile);
    }

    if(stat(sm->configFile, &statBuf) != 0) {
        /* config file missing => config should be cleared */
        return true;
    }

    if(statBuf.st_mtime != sm->configFile_modTime) {
        /* config file modified */
        if(settings.verbose) {
            fprintf(stderr, "sFlow config file changed\n");
        }
        if(((*new_config) = sfmc_readConfig(sm)) != NULL) {
            /* config OK - remember the mod_time and indicate that
               it should be applied by returning true */
            if(settings.verbose) {
                fprintf(stderr, "sFlow config OK\n");
            }
            sm->configFile_modTime = statBuf.st_mtime;
            return true;
        }
        else {
            /* bad config - ignore it (may be in transition) */
            if(settings.verbose) {
                fprintf(stderr, "sFlow config invalid - ignored (may be in transition)\n");
            }
        }
    }
    return false;
}

/* call this when you have the lock and a new config to adopt */
static void sfmc_init_config(SFMC *sm) {

    if(sm->config == NULL) return;

    /* create/re-create the agent */
    if(sm->agent) {
        sfl_agent_release(sm->agent);
        free(sm->agent);
    }
    sm->agent = (SFLAgent *)sfmc_calloc(sizeof(SFLAgent));

    /* open the sockets - one for v4 and another for v6 */
    if(sm->socket4 <= 0) {
        if((sm->socket4 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
            fprintf(stderr, "sFlow IPv4 send socket open failed : %s\n", strerror(errno));
    }
    if(sm->socket6 <= 0) {
        if((sm->socket6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) == -1)
            fprintf(stderr, "sFlow IPv6 send socket open failed : %s\n", strerror(errno));
    }

    /* initialize the agent with it's address, bootime, callbacks etc. */
    sfl_agent_init(sm->agent,
                   &sm->config->agentIP,
                   settings.port, /* subAgentId */
                   sm->tick,
                   sm->tick,
                   sm,
                   sfmc_cb_alloc,
                   sfmc_cb_free,
                   sfmc_cb_error,
                   sfmc_cb_sendPkt);

    /* add a receiver */
    SFLReceiver *receiver = sfl_agent_addReceiver(sm->agent);
    /* add a <logicalEntity> datasource to represent this application instance */
    SFLDataSource_instance dsi;
    /* ds_class = <logicalEntity>, ds_index = <service port>, ds_instance = 0 */
    /* set ds_index to the service port */
    SFL_DS_SET(dsi, SFL_DSCLASS_LOGICAL_ENTITY, settings.port, 0);
    /* add a poller for the counters */
    SFLPoller *poller = sfl_agent_addPoller(sm->agent, &dsi, sm, sfmc_cb_counters);
    sfl_poller_set_sFlowCpInterval(poller, sm->config->polling_secs);
    poller->myReceiver = receiver;
    /* add a sampler for the sampled operations */
    SFLSampler *sampler = sfl_agent_addSampler(sm->agent, &dsi);
    sfl_sampler_set_sFlowFsPacketSamplingRate(sampler, sm->config->sampling_n);
    sampler->myReceiver = receiver;

    if(sm->config->sampling_n) {
        /* seed the random number generator so that there is no
           synchronization even when a large cluster starts up all
           at the exact same instant */
        /* could also read 4 bytes from /dev/urandom to do this */
        int i;
        uint32_t hash = sm->start_time.tv_sec ^ sm->start_time.tv_usec;
        u_char *addr = sm->config->agentIP.address.ip_v6.addr;
        for(i = 0; i < 16; i += 2) {
            hash *= 3;
            hash += ((addr[i] << 8) | addr[i+1]);
        }
        sfmc.sflow_random_seed = hash;
        /* seed the threads */
        sflow_random_seed(sfmc.sflow_random_seed);
        sfmc.sflow_random_threshold = (uint32_t)-1 / sm->config->sampling_n;
    }
    else {
        sfmc.sflow_random_seed = 0;
        sfmc.sflow_random_threshold = 0;
    }
}

/* called every second or so - now from one of the work threads
   (used to be called from the timer ISR,  but that led to
   synchronization questions) */
static void sflow_tick(rel_time_t current_time) {
    SFMC *sm = &sfmc;
    SFMCConfig *new_config = NULL;
    int apply_new_config = false;

    if(sm->configTests == 0 || (sm->tick % 10 == 0)) {
        /* it make take time to read the config file, so
           do it here before we grab the lock */
        apply_new_config = sfmc_config_check(sm, &new_config);
    }
    SFMC_LOCK();
    if(apply_new_config) {
        /* now that we have the lock we can swap in the new config */
        if(sm->config != new_config) {
            SFMCConfig *old_config = sm->config;
            sm->config = new_config;
            if(old_config) free(old_config);
            if(new_config) sfmc_init_config(sm);
        }
    }
    if(sm->agent && sm->config) {
        sfl_agent_tick(sm->agent, sm->tick);
    }
    SFMC_UNLOCK();
}

/* can't wait for first tick to create the mutex because we already
 * need it at that point.  Hence this sflow_init call.  We could also
 * use this to pass in command-line parameters if there are any.
 */
void sflow_init(void) {
    SFMC *sm = &sfmc;
    if(sm->mutex == NULL) {
        sm->mutex = (pthread_mutex_t*)sfmc_calloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(sm->mutex, NULL);
    }
    gettimeofday(&sm->start_time, NULL);
    if(sm->configFile == NULL) {
        sm->configFile = SFMC_DEFAULT_CONFIGFILE;
    }
}
