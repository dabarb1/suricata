/* Copyright (C) 2007-2012 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "suricata-common.h"
#include "tm-threads.h"
#include "conf.h"
#include "runmodes.h"
#include "runmode-pcap.h"
#include "log-httplog.h"
#include "output.h"
#include "cuda-packet-batcher.h"
#include "source-pfring.h"
#include "detect-engine-mpm.h"

#include "alert-fastlog.h"
#include "alert-prelude.h"
#include "alert-unified2-alert.h"
#include "alert-debuglog.h"

#include "util-debug.h"
#include "util-time.h"
#include "util-cpu.h"
#include "util-affinity.h"
#include "util-device.h"
#include "util-runmodes.h"
#include "util-atomic.h"
#include "util-misc.h"

static const char *default_mode = NULL;

const char *RunModeIdsGetDefaultMode(void)
{
    return default_mode;
}

int RunModeIdsPcapWorkers(DetectEngineCtx *de_ctx);

void RunModeIdsPcapRegister(void)
{
    RunModeRegisterNewRunMode(RUNMODE_PCAP_DEV, "single",
                              "Single threaded pcap live mode",
                              RunModeIdsPcapSingle);
    RunModeRegisterNewRunMode(RUNMODE_PCAP_DEV, "auto",
                              "Multi threaded pcap live mode",
                              RunModeIdsPcapAuto);
    default_mode = "autofp";
    RunModeRegisterNewRunMode(RUNMODE_PCAP_DEV, "autofp",
                              "Multi threaded pcap live mode.  Packets from "
                              "each flow are assigned to a single detect thread, "
                              "unlike \"pcap_live_auto\" where packets from "
                              "the same flow can be processed by any detect "
                              "thread",
                              RunModeIdsPcapAutoFp);
    RunModeRegisterNewRunMode(RUNMODE_PCAP_DEV, "workers",
                              "Workers pcap live mode, each thread does all"
                              " tasks from acquisition to logging",
                              RunModeIdsPcapWorkers);

    return;
}

void PcapDerefConfig(void *conf)
{
    PcapIfaceConfig *pfp = (PcapIfaceConfig *)conf;
    /* Pcap config is used only once but cost of this low. */
    if (SC_ATOMIC_SUB(pfp->ref, 1) == 0) {
        SCFree(pfp);
    }
}


void *ParsePcapConfig(const char *iface)
{
    char *threadsstr = NULL;
    ConfNode *if_root;
    ConfNode *pcap_node;
    PcapIfaceConfig *aconf = SCMalloc(sizeof(*aconf));
    char *tmpbpf;
    char *tmpctype;
    intmax_t value;

    if (unlikely(aconf == NULL)) {
        return NULL;
    }

    if (iface == NULL) {
        SCFree(aconf);
        return NULL;
    }

    memset(aconf, 0x00, sizeof(*aconf));
    strlcpy(aconf->iface, iface, sizeof(aconf->iface));

    aconf->buffer_size = 0;
    /* If set command line option has precedence over config */
    if ((ConfGetInt("pcap.buffer-size", &value)) == 1) {
        SCLogInfo("Pcap will use %d buffer size", (int)value);
        aconf->buffer_size = value;
    }

    aconf->checksum_mode = CHECKSUM_VALIDATION_AUTO;
    aconf->bpf_filter = NULL;
    if ((ConfGet("bpf-filter", &tmpbpf)) == 1) {
        aconf->bpf_filter = tmpbpf;
    }

    SC_ATOMIC_INIT(aconf->ref);
    (void) SC_ATOMIC_ADD(aconf->ref, 1);
    aconf->DerefFunc = PcapDerefConfig;
    aconf->threads = 1;

    /* Find initial node */
    pcap_node = ConfGetNode("pcap");
    if (pcap_node == NULL) {
        SCLogInfo("Unable to find pcap config using default value");
        return aconf;
    }

    if_root = ConfNodeLookupKeyValue(pcap_node, "interface", iface);
    if (if_root == NULL) {
        SCLogInfo("Unable to find pcap config for "
                  "interface %s, using default value",
                  iface);
        return aconf;
    }

    if (ConfGetChildValue(if_root, "threads", &threadsstr) != 1) {
        aconf->threads = 1;
    } else {
        if (threadsstr != NULL) {
            aconf->threads = (uint8_t)atoi(threadsstr);
        }
    }
    if (aconf->threads == 0) {
        aconf->threads = 1;
    }
    if (aconf->buffer_size == 0) {
        const char *s_limit = ConfNodeLookupChildValue(if_root, "buffer-size");
        if (s_limit != NULL) {
            uint64_t bsize = 0;

            if (ParseSizeStringU64(s_limit, &bsize) < 0) {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                    "Failed to parse pcap buffer size: %s",
                    s_limit);
            } else {
                /* the string 2gb returns 2147483648 which is 1 to high
                 * for a int. */
                if (bsize == (uint64_t)((uint64_t)INT_MAX + (uint64_t)1))
                    bsize = (uint64_t)INT_MAX;

                if (bsize > INT_MAX) {
                    SCLogError(SC_ERR_INVALID_ARGUMENT,
                            "Failed to set pcap buffer size: 2gb max. %"PRIu64" > %d", bsize, INT_MAX);
                } else {
                    aconf->buffer_size = (int)bsize;
                }
            }
        }
    }

    if (aconf->bpf_filter == NULL) {
        /* set bpf filter if we have one */
        if (ConfGetChildValue(if_root, "bpf-filter", &tmpbpf) != 1) {
            SCLogDebug("could not get bpf or none specified");
        } else {
            aconf->bpf_filter = tmpbpf;
        }
    } else {
        SCLogInfo("BPF filter set from command line or via old 'bpf-filter' option.");
    }

    if (ConfGetChildValue(if_root, "checksum-checks", &tmpctype) == 1) {
        if (strcmp(tmpctype, "auto") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_AUTO;
        } else if (strcmp(tmpctype, "yes") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_ENABLE;
        } else if (strcmp(tmpctype, "no") == 0) {
            aconf->checksum_mode = CHECKSUM_VALIDATION_DISABLE;
        } else {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid value for checksum-checks for %s", aconf->iface);
        }
    }

    return aconf;
}

int PcapConfigGeThreadsCount(void *conf)
{
    PcapIfaceConfig *pfp = (PcapIfaceConfig *)conf;
    return pfp->threads;
}

/**
 * \brief Single thread version of the Pcap live processing.
 */
int RunModeIdsPcapSingle(DetectEngineCtx *de_ctx)
{
    int ret;
    char *live_dev = NULL;

    SCEnter();

    RunModeInitialize();
    TimeModeSetLive();

    (void)ConfGet("pcap.single-pcap-dev", &live_dev);

    ret = RunModeSetLiveCaptureSingle(de_ctx,
                                    ParsePcapConfig,
                                    PcapConfigGeThreadsCount,
                                    "ReceivePcap",
                                    "DecodePcap", "PcapLive",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPcapSingle initialised");

    SCReturnInt(0);
}


/**
 * \brief RunModeIdsPcapAuto set up the following thread packet handlers:
 *        - Receive thread (from iface pcap)
 *        - Decode thread
 *        - Stream thread
 *        - Detect: If we have only 1 cpu, it will setup one Detect thread
 *                  If we have more than one, it will setup num_cpus - 1
 *                  starting from the second cpu available.
 *        - Respond/Reject thread
 *        - Outputs thread
 *        By default the threads will use the first cpu available
 *        except the Detection threads if we have more than one cpu.
 *
 * \param de_ctx Pointer to the Detection Engine.
 *
 * \retval 0 If all goes well. (If any problem is detected the engine will
 *           exit()).
 */
int RunModeIdsPcapAuto(DetectEngineCtx *de_ctx)
{
    /* tname = Detect + cpuid, this is 11bytes length as max */
    char *live_dev = NULL;
    int ret;

    SCEnter();

    RunModeInitialize();
    TimeModeSetLive();

    (void) ConfGet("pcap.single-pcap-dev", &live_dev);

    ret = RunModeSetLiveCaptureAuto(de_ctx,
                                    ParsePcapConfig,
                                    PcapConfigGeThreadsCount,
                                    "ReceivePcap",
                                    "DecodePcap", "RecvPcap",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPcapAuto initialised");

    SCReturnInt(0);
}

/**
 * \brief RunModIdsPcapAutoFp set up the following thread packet handlers:
 *        - Receive thread (from pcap device)
 *        - Decode thread
 *        - Stream thread
 *        - Detect: If we have only 1 cpu, it will setup one Detect thread
 *                  If we have more than one, it will setup num_cpus - 1
 *                  starting from the second cpu available.
 *        - Outputs thread
 *        By default the threads will use the first cpu available
 *        except the Detection threads if we have more than one cpu.
 *
 * \param de_ctx Pointer to the Detection Engine
 *
 * \retval 0 If all goes well. (If any problem is detected the engine will
 *           exit()).
 */
int RunModeIdsPcapAutoFp(DetectEngineCtx *de_ctx)
{
    int ret;
    char *live_dev = NULL;

    SCEnter();
    RunModeInitialize();
    TimeModeSetLive();

    (void) ConfGet("pcap.single-pcap-dev", &live_dev);

    ret = RunModeSetLiveCaptureAutoFp(de_ctx,
                              ParsePcapConfig,
                              PcapConfigGeThreadsCount,
                              "ReceivePcap",
                              "DecodePcap", "RxPcap",
                              live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Runmode start failed");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPcapAutoFp initialised");

    SCReturnInt(0);
}

/**
 * \brief Workers version of the PCAP LIVE processing.
 *
 * Start N threads with each thread doing all the work.
 *
 */
int RunModeIdsPcapWorkers(DetectEngineCtx *de_ctx)
{
    int ret;
    char *live_dev = NULL;
    SCEnter();

    RunModeInitialize();
    TimeModeSetLive();

    (void) ConfGet("pcap.single-pcap-dev", &live_dev);

    ret = RunModeSetLiveCaptureWorkers(de_ctx,
                                    ParsePcapConfig,
                                    PcapConfigGeThreadsCount,
                                    "ReceivePcap",
                                    "DecodePcap", "RxPcap",
                                    live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsPcapWorkers initialised");

    SCReturnInt(0);
}
