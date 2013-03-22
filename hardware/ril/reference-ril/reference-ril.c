/* //device/system/reference-ril/reference-ril.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
/* Copyright (C) 2011-2012 Freescale Semiconductor, Inc. */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <termios.h>
#include <sys/system_properties.h>

#include "ril.h"
#include "hardware/qemu_pipe.h"
#include <linux/if.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#include "runtime.h"
#include "fcp_parser.h"

#define MAX_AT_RESPONSE 0x1000

/* property to control the delay (in seconds) to initialize RIL */
#define RIL_DELAY_TIME "gsm.ril.delay"

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
#define PPP_TTY_PATH "ppp0"
#define PPP_OPERSTATE_PATH "/sys/class/net/ppp0/operstate"
#define SERVICE_PPPD_GPRS "pppd_gprs"
#define PROPERTY_PPPD_EXIT_CODE "net.gprs.ppp-exit"
// Max wait time to 2*10 secondes for ppp enable
#define POLL_PPP_SYSFS_SECONDS 2
#define POLL_PPP_SYSFS_RETRY   10

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2, /* SIM_READY means the radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status;

typedef enum {
    UICC_TYPE_UNKNOWN,
    UICC_TYPE_SIM,
    UICC_TYPE_USIM,
} UICC_Type;

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status);
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status);
static void onDataCallListChanged(void *param);
static void onNewSmsArrived(void *param);
static void onNewSmsReportArrived(void *param);

extern const char * requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static int s_port = -1;
static const char * s_device_path = NULL;
#ifdef HAVE_DATA_DEVICE
static const char * s_data_device_path = NULL;
#endif
static int          s_device_socket = 0;

// flag for pppd status. 1: started; 0: stoped 
static int pppd = 0;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;
static int smsStorageIndex = 0;
//"SM", "ME", "SR"
static char smsReadStorage[3];

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};
static struct timeval TIMEVAL_DELAYINIT = {0,0}; // will be set according to property value

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

static int sUnsolictedCREG_failed = 0;
static int sUnsolictedCGREG_failed = 0;

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);

static int clccStateToRILState(int state, RIL_CallState *p_state)

{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
        //+CLCC: 1,0,2,0,0,\"+18005551212\",145
        //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    LOGE("invalid CLCC line\n");
    return -1;
}


/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif

    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    if (current_modem_type != HUAWEI_MODEM)
        at_send_command("AT+CNMI=1,2,2,1,1", NULL);
    else
        at_send_command("AT+CNMI=2,1,2,2,0", NULL);

    at_send_command("AT+CPMS=\"ME\",\"ME\",\"ME\"",NULL);
    at_send_command("AT+CMGD=1,4",NULL);
}

static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
	    if (current_modem_type == AMAZON_MODEM)
		    err = at_send_command("AT+CFUN=4", &p_response);
	    else
		    err = at_send_command("AT+CFUN=0", &p_response);
	    if (err < 0 || p_response->success == 0)
		    goto error;
	    setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestOrSendDataCallList(RIL_Token *t);

static void onDataCallListChanged(void *param)
{
    requestOrSendDataCallList(NULL);
}

static void readNewSms(void *param, int response_index)
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;
    int index = *(int *)param;
    char *cmd;
    char *line;
    char oldReadStorage[3];
    char *storage;
    int setStorage = 0;
    //Fetch current read storage
    //Set the prefer read storage
    err = at_send_command_singleline("AT+CPMS?", "+CPMS:", &p_response);
    if (err != 0 || p_response->success == 0) {
        LOGE("Cannot read prefer SMS storage");
        return;
    }
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;
    err = at_tok_nextstr(&line, &storage);
    if (err < 0)
        goto error;
    memset(oldReadStorage, 0, 3);
    memcpy(oldReadStorage, storage, 2);
    LOGI("oldReadStorage %s, current storage %s",oldReadStorage,smsReadStorage);
    
    //Set the prefer read storage
    asprintf(&cmd, "AT+CPMS=\"%s\"", smsReadStorage);
    at_send_command(cmd,NULL);
    setStorage = 1;

    asprintf(&cmd, "AT+CMGR=%d", index);
    err = at_send_command_multiline(cmd, "+CMGR:", &p_response);
    if (err != 0 || p_response->success == 0) {
        LOGE("No sms store in ME");
        return;
    }
    
    //Read the sms pdu
    //<CR><LF>+CMGR:<stat>,[<reserved>],<length><CR><LF><pdu><CR><LF><CR><LF>OK<CR><LF>
    p_cur = p_response->p_intermediates;
    if(p_cur != NULL) {
        int state = 0;
        char *reserved = NULL;
        int lenght = 0;
        line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;
        err = at_tok_nextint(&line, &state);
        if (err < 0)
            goto error;

        p_cur = p_cur->p_next;
        if(p_cur != NULL) {
            char *sms_pdu = NULL;
            sms_pdu = p_cur->line;
            if(sms_pdu != NULL) {
                RIL_onUnsolicitedResponse (
                    response_index,
                    sms_pdu, strlen(sms_pdu));
            }
        }
    }

    //Delete the sms in storage
    asprintf(&cmd, "AT+CMGD=%d", index);
    at_send_command(cmd,NULL);

error:
    //Need recover after read 
    if(setStorage){
        asprintf(&cmd, "AT+CPMS=\"%s\"", oldReadStorage);
        at_send_command(cmd,NULL);
    }

    at_response_free(p_response);
}

static void onNewSmsReportArrived(void *param)
{
    readNewSms(param,RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT);
}

static void onNewSmsArrived(void *param)
{
    readNewSms(param,RIL_UNSOL_RESPONSE_NEW_SMS);
}

static void requestDataCallList(void *data, size_t datalen, RIL_Token t)
{
    requestOrSendDataCallList(&t);
}

void onDeactiveDataCallList()
{
    RIL_Data_Call_Response_v4 *responses =
        alloca(sizeof(RIL_Data_Call_Response_v4));
    LOGI("onDeactiveDataCallList");
    //For n pdp context, or just one pdp context?
    //Always cid =1 ?
    //CDMADataConnectionTracker::onDataStateChanged()
    //or GSMDataConnectionTracker::onPdpStateChanged(), will handle this result

    responses->cid = 1;
    //LINK_INACTIVE = 0
    //LINK_DOWN = 1
    //LINK_UP = 2;
    responses->active = 0;
    responses->type = "";
    responses->apn = "";
    responses->address = "";

    RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses,
                                  sizeof(RIL_Data_Call_Response_v4));
}

static void requestOrSendDataCallList(RIL_Token *t)
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;
    int n = 0;
    char *out;

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response_v6 *responses =
        alloca(n * sizeof(RIL_Data_Call_Response_v6));

    int i;
    for (i = 0; i < n; i++) {
        responses[i].status = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].ifname = "";
        responses[i].addresses = "";
        responses[i].dnses = "";
        responses[i].gateways = "";
    }

    RIL_Data_Call_Response_v6 *response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        for (i = 0; i < n; i++) {
            if (responses[i].cid == cid)
                break;
        }

        if (i >= n) {
            /* details for a context we didn't hear about in the last request */
            continue;
        }

        // Assume no error
        responses[i].status = 0;

        // type
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;
        responses[i].type = alloca(strlen(out) + 1);
        strcpy(responses[i].type, out);

        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].ifname = alloca(strlen(PPP_TTY_PATH) + 1);
        strcpy(responses[i].ifname, PPP_TTY_PATH);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].addresses = alloca(strlen(out) + 1);
        strcpy(responses[i].addresses, out);

        {
            char  propValue[PROP_VALUE_MAX];

            if (__system_property_get("ro.kernel.qemu", propValue) != 0) {
                /* We are in the emulator - the dns servers are listed
                 * by the following system properties, setup in
                 * /system/etc/init.goldfish.sh:
                 *  - net.eth0.dns1
                 *  - net.eth0.dns2
                 *  - net.eth0.dns3
                 *  - net.eth0.dns4
                 */
                const int   dnslist_sz = 128;
                char*       dnslist = alloca(dnslist_sz);
                const char* separator = "";
                int         nn;

                dnslist[0] = 0;
                for (nn = 1; nn <= 4; nn++) {
                    /* Probe net.eth0.dns<n> */
                    char  propName[PROP_NAME_MAX];
                    snprintf(propName, sizeof propName, "net.eth0.dns%d", nn);

                    /* Ignore if undefined */
                    if (__system_property_get(propName, propValue) == 0) {
                        continue;
                    }

                    /* Append the DNS IP address */
                    strlcat(dnslist, separator, dnslist_sz);
                    strlcat(dnslist, propValue, dnslist_sz);
                    separator = " ";
                }
                responses[i].dnses = dnslist;

                /* There is only on gateway in the emulator */
                responses[i].gateways = "10.0.2.2";
            }
            else {
                /* I don't know where we are, so use the public Google DNS
                 * servers by default and no gateway.
                 */
                //responses[i].dnses = "8.8.8.8 8.8.4.4";
                //responses[i].gateways = "";
                const char* separator = " ";
                const int   dnslist_sz = 128;
                char*       dnslist = alloca(dnslist_sz);
                char  propName[PROP_NAME_MAX];
                memset(dnslist, 0, 128);
                property_get("net.ppp0.dns1", propName, "8.8.8.8");
                strlcat(dnslist, propValue, dnslist_sz);
                strlcat(dnslist, separator, dnslist_sz);
                property_get("net.ppp0.dns2", propName, "8.8.4.4");
                strlcat(dnslist, propValue, dnslist_sz);

                responses[i].dnses = dnslist;
                responses[i].gateways = "";
            }
        }
    }

    at_response_free(p_response);

    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                              n * sizeof(RIL_Data_Call_Response_v6));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses,
                                  n * sizeof(RIL_Data_Call_Response_v6));

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);

    at_response_free(p_response);
}

static void requestQueryNetworkSelectionMode(
                void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void sendCallStateChanged(void *param)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

static void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i;
    int needRepoll = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                LOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDial(void *data, size_t datalen, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int response[2];
    char *line;

    if(sUnsolictedCREG_failed) {
        LOGW("Retry the AT+CREG event report setting");
        /*  Network registration events */
        err = at_send_command("AT+CREG=2", &p_response);
    
        /* some handsets -- in tethered mode -- don't support CREG=2 */
        if (err < 0 || p_response->success == 0) {
            at_response_free(p_response);
            err = at_send_command("AT+CREG=1", &p_response);
        }
    
        if (err < 0 || p_response->success == 0) {
            LOGE("Warning!No network registration events reported");
            sUnsolictedCREG_failed = 1;
        }
        else {
            sUnsolictedCREG_failed = 0;
        }
        at_response_free(p_response);
    }

    if(sUnsolictedCGREG_failed) {
        LOGW("Retry the AT+CGREG event report setting");
        /*  GPRS registration events */
        err = at_send_command("AT+CGREG=1", &p_response);
        if (err < 0 || p_response->success == 0) {
          LOGE("Warning!No GPRS registration events reported");
          sUnsolictedCGREG_failed = 1;
        }
        else {
          sUnsolictedCGREG_failed = 0;
       }

      at_response_free(p_response);
    }


    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[0]));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(response[1]));
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

    at_response_free(p_response);
    return;

error:
    LOGE("requestSignalStrength must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestRegistrationState(int request, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int response[4];
    char * responseStr[4];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 3;


    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        cmd = "AT+CGREG?";
        prefix = "+CGREG:";
    } else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
        break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            response[1] = -1;
            response[2] = -1;
            if (err < 0) goto error;
        break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
        break;
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[3]);
            if (err < 0) goto error;
            count = 4;
        break;
        default:
            goto error;
    }

    asprintf(&responseStr[0], "%d", response[0]);
    asprintf(&responseStr[1], "%x", response[1]);
    asprintf(&responseStr[2], "%x", response[2]);

    if (count > 3)
        asprintf(&responseStr[3], "%d", response[3]);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
    at_response_free(p_response);

    return;
error:
    LOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    //Workaround for 770w with below response
    //+COPS: 0,0,"FFFFFFFFFFFFFFFF",2
    //+COPS: 0,1,"FFFFFFFFFFFFFFFF",2
    //+COPS: 0,2,"46001",2
    //"China Unicom","UNICOM","46001"
    //"China Mobile Com","CMCC","46000"
    //"China Mobile Com","CMCC","46002"
    //"China Mobile Com","CMCC","46007"
    char *invalid_operator = "FFFFFFFFFFFFFFFF";
    if (response[0] && response[2] &&
        !strncmp(response[0], invalid_operator, strlen(invalid_operator))) {
        //Set the long operator name based on mcc/mnc
        if (!strncmp(response[2], "46001", 5))
            strcpy(response[0], "China Unicom");
        if (!strncmp(response[2], "46000", 5)||
           !strncmp(response[2], "46002", 5)||
           !strncmp(response[2], "46007", 5))
            strcpy(response[0], "China Mobile Com");
    }
    if (response[1] && response[2] &&
        !strncmp(response[1], invalid_operator, strlen(invalid_operator))) {
        //Set the long operator name based on mcc/mnc
        if (!strncmp(response[2], "46001", 5))
            strcpy(response[1], "UNICOM");
        if (!strncmp(response[2], "46000", 5)||
           !strncmp(response[2], "46002", 5)||
           !strncmp(response[2], "46007", 5))
            strcpy(response[1], "CMCC");
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    LOGE("requestOperator must not return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    memset(&response, 0, sizeof(response));

    /* FIXME fill in messageRef and ackPDU */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *cid;
    char *cmd;
    int err;

    cid = ((const char **)data)[0];
    LOGD("requesting deactivating data connection with CID '%s'", cid);

    /* Stop PPPd firstly */
    LOGD("Stopping PPPD");
    property_set("ctl.stop", SERVICE_PPPD_GPRS);

    /* Dactivate PDP Context */
    asprintf(&cmd, "AT+CGACT=0,%s", cid);
    err = at_send_command(cmd, NULL);

    if (err < 0 ) {
	LOGW("Dactivate PDP failure");
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    free(cmd);

    return;
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *apn;
    char *cmd;
    int err;
    ATResponse *p_response = NULL;
    char *response[3] = { "1", PPP_TTY_PATH, ""};

    apn = ((const char **)data)[2];

#ifdef USE_TI_COMMANDS
    // Config for multislot class 10 (probably default anyway eh?)
    err = at_send_command("AT%CPRIM=\"GMM\",\"CONFIG MULTISLOT_CLASS=<10>\"",
                        NULL);

    err = at_send_command("AT%DATA=2,\"UART\",1,,\"SER\",\"UART\",0", NULL);
#endif /* USE_TI_COMMANDS */

    if (current_modem_type == UNKNOWN_MODEM) {
    int fd, qmistatus;
    size_t cur = 0;
    size_t len;
    ssize_t written, rlen;
    char status[32] = {0};
    int retry = 10;
    const char *pdp_type;

    LOGD("requesting data connection to APN '%s'", apn);

    fd = open ("/dev/qmi", O_RDWR);
    if (fd >= 0) { /* the device doesn't exist on the emulator */

        LOGD("opened the qmi device\n");
        asprintf(&cmd, "up:%s", apn);
        len = strlen(cmd);

        while (cur < len) {
            do {
                written = write (fd, cmd + cur, len - cur);
            } while (written < 0 && errno == EINTR);

            if (written < 0) {
                LOGE("### ERROR writing to /dev/qmi");
                close(fd);
                goto error;
            }

            cur += written;
        }

        // wait for interface to come online

        do {
            sleep(1);
            do {
                rlen = read(fd, status, 31);
            } while (rlen < 0 && errno == EINTR);

            if (rlen < 0) {
                LOGE("### ERROR reading from /dev/qmi");
                close(fd);
                goto error;
            } else {
                status[rlen] = '\0';
                LOGD("### status: %s", status);
            }
        } while (strncmp(status, "STATE=up", 8) && strcmp(status, "online") && --retry);

        close(fd);

        if (retry == 0) {
            LOGE("### Failed to get data connection up\n");
            goto error;
        }

        qmistatus = system("netcfg rmnet0 dhcp");

        LOGD("netcfg rmnet0 dhcp: status %d\n", qmistatus);

        if (qmistatus < 0) goto error;

    } else {

        if (datalen > 6 * sizeof(char *)) {
            pdp_type = ((const char **)data)[6];
        } else {
            pdp_type = "IP";
        }

        asprintf(&cmd, "AT+CGDCONT=1,\"%s\",\"%s\",,0,0", pdp_type, apn);
        //FIXME check for error here
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Set required QoS params to default
        err = at_send_command("AT+CGQREQ=1", NULL);

        // Set minimum QoS params to default
        err = at_send_command("AT+CGQMIN=1", NULL);

        // packet-domain event reporting
        err = at_send_command("AT+CGEREP=1,0", NULL);

        // Hangup anything that's happening there now
        err = at_send_command("AT+CGACT=1,0", NULL);

        // Start data on PDP context 1
        err = at_send_command("ATD*99***1#", &p_response);

        if (err < 0 || p_response->success == 0) {
            goto error;
        }
    }
    } else {/* if current_modem_type == UNKNOWN_MODEM */

    int fd;
    char buffer[20];
    char exit_code[PROPERTY_VALUE_MAX];
    static char local_ip[PROPERTY_VALUE_MAX];
    int retry = POLL_PPP_SYSFS_RETRY;

    if(pppd) {
	// clean up any existing PPP connection before activating new PDP context
	LOGW("Stop existing PPPd before activating PDP");
	property_set("ctl.stop", SERVICE_PPPD_GPRS);
	pppd = 0;
    }

    LOGD("requesting data connection to APN '%s'", apn);

    asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\",,0,0", apn);
    err = at_send_command(cmd, NULL);
    free(cmd);

    // Set required QoS params to default
    err = at_send_command("AT+CGQREQ=1", NULL);

    // Set minimum QoS params to default
    err = at_send_command("AT+CGQMIN=1", NULL);

    // packet-domain event reporting
    err = at_send_command("AT+CGEREP=1,0", NULL);

    if (current_modem_type != AMAZON_MODEM) {
	    /* Hangup anything that's happening there now */
	    err = at_send_command("AT+CGACT=0,1", NULL);

	    /* Start data on PDP context 1 */
	    err = at_send_command("ATD*99***1#", &p_response);

	    if (err < 0 || p_response->success == 0)
		    goto error;
    } else {
	    if (err < 0)
		    goto error;
    }
    // Setup PPP connection after PDP Context activated successfully
    // The ppp service name is specified in /init.rc
    property_set(PROPERTY_PPPD_EXIT_CODE, "");
    property_set("net.ppp0.local-ip", "");
    err = property_set("ctl.start", SERVICE_PPPD_GPRS);
    pppd = 1;
    if (err < 0) {
	LOGW("Can not start PPPd");
	goto ppp_error;
    }
    LOGD("PPPd started");

    // Wait for PPP interface ready
    if (current_modem_type == AMAZON_MODEM)
	    sleep(2);
    else
	    sleep(1);
    do {
	// Check whether PPPD exit abnormally
	property_get(PROPERTY_PPPD_EXIT_CODE, exit_code, "");
	if(strcmp(exit_code, "") != 0) {
	    LOGW("PPPd exit with code %s", exit_code);
	    retry = 0;
	    break;
        }

        fd  = open(PPP_OPERSTATE_PATH, O_RDONLY);
        if (fd >= 0)  {
	    buffer[0] = 0;
            read(fd, buffer, sizeof(buffer));
	    close(fd);
	    if(!strncmp(buffer, "up", strlen("up")) || !strncmp(buffer, "unknown", strlen("unknown"))) {
		// Should already get local IP address from PPP link after IPCP negotation 
		// system property net.ppp0.local-ip is created by PPPD in "ip-up" script 
        local_ip[0] = 0;
		property_get("net.ppp0.local-ip", local_ip, "");
		if(!strcmp(local_ip, "")) {
		    LOGW("PPP link is up but no local IP is assigned. Will retry %d times after %d seconds", \
			  retry, POLL_PPP_SYSFS_SECONDS);
		} else {
		    LOGD("PPP link is up with local IP address %s", local_ip);
		    // other info like dns will be passed to framework via property (set in ip-up script)
            response[1] = "ppp0";
		    response[2] = local_ip;  
		    // now we think PPPd is ready
		    break;	
		}
	    } else {
		LOGW("PPP link status in %s is %s. Will retry %d times after %d seconds", \
		    PPP_OPERSTATE_PATH, buffer, retry, POLL_PPP_SYSFS_SECONDS);
	    }
	} else {
    	    LOGW("Can not detect PPP state in %s. Will retry %d times after %d seconds", \
		  PPP_OPERSTATE_PATH, retry-1, POLL_PPP_SYSFS_SECONDS);
	}
        sleep(POLL_PPP_SYSFS_SECONDS);
    } while (--retry);

    if(retry == 0)
	goto ppp_error;
    }
    requestOrSendDataCallList(&t);
    at_response_free(p_response);

    return;

ppp_error:
    /* PDP activated successfully, but PPP connection can't be setup. So deactivate PDP context */
    at_send_command("AT+CGACT=0,1", NULL);
    /* If PPPd is already launched, stop it */
    if(pppd) {
	property_set("ctl.stop", SERVICE_PPPD_GPRS);
	pppd = 0;
    }
    // fall through
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);

}

static void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    int ackSuccess;
    int err;

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
    } else {
        LOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

}

static int convertSimIoFcp(RIL_SIM_IO_Response *sr, char **cvt)
{
    int err = 0;
    size_t fcplen;
    struct ts_51011_921_resp resp;
    void *cvt_buf = NULL;

    if (!sr->simResponse || !cvt) {
        err = -1;
        goto error;
    }

    fcplen = strlen(sr->simResponse);
    if ((fcplen == 0) || (fcplen & 1)) {
        err = -1;
        goto error;
    }

    err = fcp_to_ts_51011(sr->simResponse, fcplen, &resp);
    if (err < 0){
        goto error;
    }

    cvt_buf = malloc(sizeof(resp) * 2 + 1);
    if (!cvt_buf) {
        err = -1;
        goto error;
    }

    err = binaryToString((unsigned char*)(&resp),
                   sizeof(resp), cvt_buf);
    if (err < 0){
        goto error;
    }

    /* cvt_buf ownership is moved to the caller */
    *cvt = cvt_buf;
    cvt_buf = NULL;

finally:
    return err;

error:
    free(cvt_buf);
    goto finally;
}

/**
* Fetch information about UICC card type (SIM/USIM)
*
* \return UICC_Type: type of UICC card.
*/
static UICC_Type getUICCType()
{
    ATResponse *atresponse = NULL;
    static UICC_Type UiccType = UICC_TYPE_UNKNOWN; /* FIXME: Static variable */
    int err;
    int swx;
    char *line = NULL;
    char *dir = NULL;

    if (currentState() == RADIO_STATE_OFF ||
        currentState() == RADIO_STATE_UNAVAILABLE) {
        UiccType = UICC_TYPE_UNKNOWN;
        goto exit;
    }

    /* No need to get type again, it is stored */
    if (UiccType != UICC_TYPE_UNKNOWN)
        goto exit;

    /*
     * For USIM, 'AT+CRSM=192,28480,0,0,15' will respond with TLV tag "62" which indicates
     * FCP template (refer to ETSI TS 101 220). Currently, USIM detection succeeds on
     * Innocomm Amazon1901 and Huawei EM770W.
     */
    err = at_send_command_singleline("AT+CRSM=192,28480,0,0,15", "+CRSM:", &atresponse);

    if (err != 0 || !atresponse->success)
        goto error;

    if (atresponse->p_intermediates != NULL) {
        line = atresponse->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &swx);
        if (err < 0)
            goto error;
        err = at_tok_nextint(&line, &swx);
        if (err < 0)
            goto error;

	if (at_tok_hasmore(&line)) {
	 	err = at_tok_nextstr(&line, &dir);
		if (err < 0) goto error;
	}

        if (strstr(dir, "62") == dir) {
            UiccType = UICC_TYPE_USIM;
            LOGI("Detected card type USIM - stored");
            goto finally;
        }
    }

    UiccType = UICC_TYPE_SIM;
    LOGI("Detected card type SIM - stored");
    goto finally;

error:
    UiccType = UICC_TYPE_UNKNOWN;
    LOGW("%s(): Failed to detect card type - Retry at next request", __func__);

finally:
    at_response_free(atresponse);

exit:
    return UiccType;
}



static void  requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO_v6 *p_args;
    char *line;
    int cvt_done = 0;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO_v6 *)data;

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto error;
    }

/*
* In case the command is GET_RESPONSE and cardtype is 3G SIM
* conversion to 2G FCP is required
*/
    if (p_args->command == 0xC0 && getUICCType() == UICC_TYPE_USIM) {
        if (convertSimIoFcp(&sr, &sr.simResponse) < 0) {
            //rilErrorCode = RIL_E_GENERIC_FAILURE;
            goto error;
        }
        cvt_done = 1; /* sr.simResponse needs to be freed */
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);

    if (cvt_done)
    free(sr.simResponse);

    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if ( datalen == sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else if ( datalen == 2*sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}


static void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    const char *ussdRequest;

    ussdRequest = (char *)(data);


    RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);

// @@@ TODO

}

static void  requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char *response = { "UNKNOWN"};
    char *line;

    err = at_send_command_numeric("AT+CGMR", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        line = p_response->p_intermediates->line;
        if(line != NULL) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, line, sizeof(line));
        }
        else{
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
    }
    at_response_free(p_response);
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Will always be called from the same thread, so returning here implies
 * that the radio is ready to process another command (whether or not
 * the previous command has completed).
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    int err;

    LOGD("onRequest: %s", requestToString(request));

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS)
     */
    if (sState == RADIO_STATE_OFF
        && !(request == RIL_REQUEST_RADIO_POWER
            || request == RIL_REQUEST_GET_SIM_STATUS)
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus_v6 *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            at_send_command("AT+CHLD=0", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // 3GPP 22.030 6.5.5
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=1", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // 3GPP 22.030 6.5.5
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            at_send_command("AT+CHLD=2", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFERENCE:
            // 3GPP 22.030 6.5.5
            // "Adds a held call to the conversation"
            at_send_command("AT+CHLD=3", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_UDUB:
            /* user determined user busy */
            /* sometimes used: ATH */
            at_send_command("ATH", NULL);

            /* success or failure is ignored by the upper layer here.
               it will call GET_CURRENT_CALLS and determine success that way */
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SEND_SMS:
            requestSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            requestDeactivateDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CIMI", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_GET_IMEI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            at_send_command("AT+COPS=0", NULL);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            LOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                LOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestEnterSimPin(data, datalen, t);
            break;

        case RIL_REQUEST_BASEBAND_VERSION:
            if (current_modem_type == HUAWEI_MODEM)
                requestBasebandVersion(data, datalen, t);
            else
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;

        default:
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t)
{
    //@@@todo

}

static const char * getVersion(void)
{
    return "android reference-ril 1.0";
}

static void
setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);


    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (sState == RADIO_STATE_SIM_READY) {
            onSIMReady();
        } else if (sState == RADIO_STATE_SIM_NOT_READY) {
            onRadioPowerOn();
        }
    }
}

/** Returns SIM_NOT_READY on error */
static SIM_Status
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v6 *p_card_status = malloc(sizeof(RIL_CardStatus_v6));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->ims_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        // Only support one app, gsm
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param)
{
    ATResponse *p_response;
    int ret;

    if (sState != RADIO_STATE_SIM_NOT_READY) {
        // no longer valid to poll
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            setRadioState(RADIO_STATE_SIM_READY);
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param)
{
    ATResponse *p_response = NULL;
    int err;

    setRadioState (RADIO_STATE_OFF);

    at_handshake();

    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_response_free(p_response);
        err = at_send_command("AT+CREG=1", &p_response);
    }

    if (err < 0 || p_response->success == 0) {
        LOGE("Warning!No network registration events reported");
        sUnsolictedCREG_failed = 1;
    }
    else {
        sUnsolictedCREG_failed = 0;
    }
    at_response_free(p_response);

    /*  GPRS registration events */
    err = at_send_command("AT+CGREG=1", &p_response);
    if (err < 0 || p_response->success == 0) {
        LOGE("Warning!No GPRS registration events reported");
        sUnsolictedCGREG_failed = 1;
    }
    else {
        sUnsolictedCGREG_failed = 0;
    }

    at_response_free(p_response);

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */


    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_SIM_NOT_READY);
    }
}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL;
    int err;

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = strdup(s);
        at_tok_start(&line);

        err = at_tok_nextstr(&line, &response);

        if (err != 0) {
            LOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response));
        }
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CMTI:")) {
        char *storage = NULL;
        //read the sms in ME
        line = strdup(s);
        at_tok_start(&line);

        //+CMTI: "ME",1
        err = at_tok_nextstr(&line, &storage);
        if (err < 0) return;

        err = at_tok_nextint(&line, &smsStorageIndex);
        if (err < 0) return;

        memset(smsReadStorage, 0, 3);
        //"SM","ME","SR"
        strncpy(smsReadStorage, storage, 2);

        RIL_requestTimedCallback (onNewSmsArrived, &smsStorageIndex, NULL);
        free(line);
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDSI:")) {
        char *storage = NULL;
        //read the sms in ME
        line = strdup(s);
        at_tok_start(&line);
        //+CDSI: "SM",8
        err = at_tok_nextstr(&line, &storage);
        if (err < 0) return;
        
        err = at_tok_nextint(&line, &smsStorageIndex);
        if (err < 0) return;

        memset(smsReadStorage, 0, 3);
        //"SM","ME","SR"
        strncpy(smsReadStorage, storage, 2);
        RIL_requestTimedCallback (onNewSmsReportArrived, &smsStorageIndex, NULL);
        free(line);
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    LOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    LOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

static void usage(char *s)
{
#ifdef RIL_SHLIB
#ifdef HAVE_DATA_DEVICE
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device -u /dev/data_device\n");
#else
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#endif
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}

static void *
mainLoop(void *param)
{
    int fd;
    int ret;
#ifdef HAVE_DATA_DEVICE
    struct stat dummy;
#endif
    struct termios new_termios, old_termios;
    char delay_init[PROPERTY_VALUE_MAX];
    int delay;

    AT_DUMP("== ", "entering mainLoop()", -1 );
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        fd = -1;
        while  (fd < 0) {
#ifdef HAVE_DATA_DEVICE
	    /* If the modem have two channel, e.g. UART for AT command and USB for data,
	       we check the data channel firstly (e.g. /dev/ttyUSB0) so that after AT handshake
	       we can make sure the data channel is ready.
	    */
	    if(stat(s_data_device_path, &dummy)) {
                LOGE ("opening data channel device - %s. retrying...", s_data_device_path);
                sleep(10);
		continue;
	    }
#endif
            if (s_port > 0) {
                fd = socket_loopback_client(s_port, SOCK_STREAM);
            } else if (s_device_socket) {
                if (!strcmp(s_device_path, "/dev/socket/qemud")) {
                    /* Before trying to connect to /dev/socket/qemud (which is
                     * now another "legacy" way of communicating with the
                     * emulator), we will try to connecto to gsm service via
                     * qemu pipe. */
                    fd = qemu_pipe_open("qemud:gsm");
                    if (fd < 0) {
                        /* Qemu-specific control socket */
                        fd = socket_local_client( "qemud",
                                                  ANDROID_SOCKET_NAMESPACE_RESERVED,
                                                  SOCK_STREAM );
                        if (fd >= 0 ) {
                            char  answer[2];

                            if ( write(fd, "gsm", 3) != 3 ||
                                 read(fd, answer, 2) != 2 ||
                                 memcmp(answer, "OK", 2) != 0)
                            {
                                close(fd);
                                fd = -1;
                            }
                       }
                    }
                }
                else
                    fd = socket_local_client( s_device_path,
                                            ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                            SOCK_STREAM );
            } else if (s_device_path != NULL) {
                fd = open (s_device_path, O_RDWR);
		if (fd < 0)
			LOGE("Error On open:%s:%s", s_device_path, strerror(errno));
                if ( fd >= 0 && !memcmp( s_device_path, "/dev/ttyS", 9 ) ) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    tcgetattr( fd, &ios );
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    tcsetattr( fd, TCSANOW, &ios );
                }
            }

            if (fd < 0) {
                LOGE ("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }
	if (current_modem_type != UNKNOWN_MODEM) {
		/* Set UART parameters (e.g. Buad rate) for connecting with HUAWEI modem */
		tcgetattr(fd, &old_termios);
		new_termios = old_termios;
		new_termios.c_lflag &= ~(ICANON | ECHO | ISIG);
		new_termios.c_cflag |= (CREAD | CLOCAL);
		new_termios.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
		new_termios.c_cflag &= ~(CBAUD | CSIZE) ;
		new_termios.c_cflag |= (B115200 | CS8);
		ret = tcsetattr(fd, TCSANOW, &new_termios);
		if (ret < 0) {
			LOGE ("Fail to set UART parameters. tcsetattr return %d \n", ret);
			return 0;
		}
	}
        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            LOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        //RIL framework can handle the modem not response on AT+COPS in time
        //So remove the delay here
        TIMEVAL_DELAYINIT.tv_sec = 0;

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_DELAYINIT);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        LOGI("Re-opening after close");
    }
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;

    s_rilenv = env;
    start_uevent_monitor();
#ifdef HAVE_DATA_DEVICE
    while ( -1 != (opt = getopt(argc, argv, "p:d:u:s:"))) {
#else
    while ( -1 != (opt = getopt(argc, argv, "p:d:s:"))) {
#endif
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                LOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                LOGI("Opening tty device %s\n", s_device_path);
            break;

#ifdef HAVE_DATA_DEVICE
	   /* e.g. /dev/ttyUSB0 for seperate data channel */
	    case 'u':
		s_data_device_path = optarg;
	    break;
#endif

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                LOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (s_device_path == NULL)
	    s_device_path = runtime_3g_port_device();
    if (s_port < 0 && s_device_path == NULL) {
        usage(argv[0]);
        return NULL;
    }

#ifdef HAVE_DATA_DEVICE
    if (s_data_device_path == NULL)
	    s_data_device_path = runtime_3g_port_data();
    if (s_data_device_path == NULL) {
        usage(argv[0]);
        return NULL;
    }
#endif

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

    return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;

    while ( -1 != (opt = getopt(argc, argv, "p:d:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                }
                LOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                LOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                LOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
        }
    }

    if (s_port < 0 && s_device_path == NULL) {
        usage(argv[0]);
    }

    RIL_register(&s_callbacks);

    mainLoop(NULL);

    return 0;
}

#endif /* RIL_SHLIB */
