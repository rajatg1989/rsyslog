/* omelasticsearch.c
 * This is the http://www.elasticsearch.org/ output module.
 *
 * NOTE: read comments in module-template.h for more specifics!
 *
 * Copyright 2011 Nathan Scott.
 * Copyright 2009-2012 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "module-template.h"
#include "errmsg.h"
#include "statsobj.h"
#include "cfsysline.h"
#include "unicode-helper.h"

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("omelasticsearch")

/* internal structures */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)
DEFobjCurrIf(statsobj)

statsobj_t *indexStats;
STATSCOUNTER_DEF(indexConFail, mutIndexConFail)
STATSCOUNTER_DEF(indexSubmit, mutIndexSubmit)
STATSCOUNTER_DEF(indexFailed, mutIndexFailed)
STATSCOUNTER_DEF(indexSuccess, mutIndexSuccess)

/* REST API for elasticsearch hits this URL:
 * http://<hostName>:<restPort>/<searchIndex>/<searchType>
 */
typedef struct curl_slist HEADER;
typedef struct _instanceData {
	uchar *server;
	int port;
	uchar *uid;
	uchar *pwd;
	uchar *searchIndex;
	uchar *searchType;
	uchar *tplName;
	uchar *timeout;
	sbool dynSrchIdx;
	sbool dynSrchType;
	sbool bulkmode;
	sbool asyncRepl;
	struct {
		es_str_t *data;
		uchar *currTpl1;
		uchar *currTpl2;
	} batch;
	CURL	*curlHandle;	/* libcurl session handle */
	HEADER	*postHeader;	/* json POST request info */
} instanceData;


/* tables for interfacing with the v6 config system */
/* action (instance) parameters */
static struct cnfparamdescr actpdescr[] = {
	{ "server", eCmdHdlrGetWord, 0 },
	{ "serverport", eCmdHdlrInt, 0 },
	{ "uid", eCmdHdlrGetWord, 0 },
	{ "pwd", eCmdHdlrGetWord, 0 },
	{ "searchindex", eCmdHdlrGetWord, 0 },
	{ "searchtype", eCmdHdlrGetWord, 0 },
	{ "dynsearchindex", eCmdHdlrBinary, 0 },
	{ "dynsearchtype", eCmdHdlrBinary, 0 },
	{ "bulkmode", eCmdHdlrBinary, 0 },
	{ "asyncrepl", eCmdHdlrBinary, 0 },
	{ "timeout", eCmdHdlrGetWord, 0 },
	{ "template", eCmdHdlrGetWord, 1 }
};
static struct cnfparamblk actpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(actpdescr)/sizeof(struct cnfparamdescr),
	  actpdescr
	};

BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance

BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature

BEGINfreeInstance
CODESTARTfreeInstance
	if (pData->postHeader) {
		curl_slist_free_all(pData->postHeader);
		pData->postHeader = NULL;
	}
	if (pData->curlHandle) {
		curl_easy_cleanup(pData->curlHandle);
		pData->curlHandle = NULL;
	}
	free(pData->server);
	free(pData->uid);
	free(pData->pwd);
	free(pData->searchIndex);
	free(pData->searchType);
	free(pData->tplName);
ENDfreeInstance

BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	dbgprintf("omelasticsearch\n");
	dbgprintf("\ttemplate='%s'\n", pData->tplName);
	dbgprintf("\tserver='%s'\n", pData->server);
	dbgprintf("\tserverport=%d\n", pData->port);
	dbgprintf("\tuid='%s'\n", pData->uid == NULL ? (uchar*)"(not configured)" : pData->uid);
	dbgprintf("\tpwd=(%s configured)\n", pData->pwd == NULL ? "not " : "");
	dbgprintf("\tsearch index='%s'\n", pData->searchIndex);
	dbgprintf("\tsearch index='%s'\n", pData->searchType);
	dbgprintf("\ttimeout='%s'\n", pData->timeout);
	dbgprintf("\tdynamic search index=%d\n", pData->dynSrchIdx);
	dbgprintf("\tdynamic search type=%d\n", pData->dynSrchType);
	dbgprintf("\tasync replication=%d\n", pData->asyncRepl);
	dbgprintf("\tbulkmode=%d\n", pData->bulkmode);
ENDdbgPrintInstInfo

BEGINtryResume
CODESTARTtryResume
ENDtryResume


/* get the current index and type for this message */
static inline void
getIndexAndType(instanceData *pData, uchar *tpl1, uchar *tpl2, uchar **srchIndex,
		uchar **srchType)
{
	if(pData->dynSrchIdx) {
		*srchIndex = tpl1;
		if(pData->dynSrchType)
			*srchType = tpl2;
		else 
			*srchType = pData->searchType;
	} else {
		*srchIndex = pData->searchIndex;
		if(pData->dynSrchType)
			*srchType = tpl1;
		else 
			*srchType = pData->searchType;
	}
}


static rsRetVal
setCurlURL(instanceData *pData, uchar *tpl1, uchar *tpl2)
{
	char authBuf[1024];
	char portBuf[64];
	char *restURL;
	uchar *searchIndex;
	uchar *searchType;
	es_str_t *url;
	int r;

	getIndexAndType(pData, tpl1, tpl2, &searchIndex, &searchType);
	url = es_newStr(128);
	snprintf(portBuf, sizeof(portBuf), "%d", pData->port);

	r = es_addBuf(&url, "http://", sizeof("http://")-1);
	if(r == 0) r = es_addBuf(&url, (char*)pData->server, strlen((char*)pData->server));
	if(r == 0) r = es_addChar(&url, ':');
	if(r == 0) r = es_addBuf(&url, portBuf, strlen(portBuf));
	if(r == 0) r = es_addChar(&url, '/');
	if(pData->bulkmode) {
		if(r == 0) r = es_addBuf(&url, "_bulk", sizeof("_bulk")-1);
	} else {
		if(r == 0) r = es_addBuf(&url, (char*)searchIndex, ustrlen(searchIndex));
		if(r == 0) r = es_addChar(&url, '/');
		if(r == 0) r = es_addBuf(&url, (char*)searchType, ustrlen(searchType));
	}
	if(r == 0) r = es_addChar(&url, '?');
	if(pData->asyncRepl) {
		if(r == 0) r = es_addBuf(&url, "replication=async&",
					sizeof("replication=async&")-1);
	}
	if(pData->timeout != NULL) {
		if(r == 0) r = es_addBuf(&url, "timeout=", sizeof("timeout=")-1);
		if(r == 0) r = es_addBuf(&url, (char*)pData->timeout, ustrlen(pData->timeout));
	}
	restURL = es_str2cstr(url, NULL);
	curl_easy_setopt(pData->curlHandle, CURLOPT_URL, restURL); 
	es_deleteStr(url);
	free(restURL);

	if(pData->uid != NULL) {
		snprintf(authBuf, sizeof(authBuf), "%s:%s", pData->uid,
			 (pData->pwd == NULL) ? "" : (char*)pData->pwd);
		//TODO: create better code, check errors!
		curl_easy_setopt(pData->curlHandle, CURLOPT_USERPWD, authBuf); 
		curl_easy_setopt(pData->curlHandle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
	}
	DBGPRINTF("omelasticsearch: using REST URL: '%s'\n", restURL);
	return RS_RET_OK;
}


/* this method does not directly submit but builds a batch instead. It
 * may submit, if we have dynamic index/type and the current type or
 * index changes.
 */
static rsRetVal
buildBatch(instanceData *pData, uchar *message, uchar *tpl1, uchar *tpl2)
{
	int length = strlen((char *)message);
	int r;
	uchar *searchIndex;
	uchar *searchType;
	DEFiRet;
#	define META_STRT "{\"index\":{\"_index\": \""
#	define META_TYPE "\",\"_type\":\""
#	define META_END  "\"}}\n"

#warning TODO: use dynamic index/type!
	getIndexAndType(pData, tpl1, tpl2, &searchIndex, &searchType);
	r = es_addBuf(&pData->batch.data, META_STRT, sizeof(META_STRT)-1);
	if(r == 0) r = es_addBuf(&pData->batch.data, (char*)searchIndex,
				 ustrlen(searchIndex));
	if(r == 0) r = es_addBuf(&pData->batch.data, META_TYPE, sizeof(META_TYPE)-1);
	if(r == 0) r = es_addBuf(&pData->batch.data, (char*)searchType,
				 ustrlen(searchType));
	if(r == 0) r = es_addBuf(&pData->batch.data, META_END, sizeof(META_END)-1);
	if(r == 0) r = es_addBuf(&pData->batch.data, (char*)message, length);
	if(r == 0) r = es_addBuf(&pData->batch.data, "\n", sizeof("\n")-1);
	if(r != 0) {
		DBGPRINTF("omelasticsearch: growing batch failed with code %d\n", r);
		ABORT_FINALIZE(RS_RET_ERR);
	}
	iRet = RS_RET_DEFER_COMMIT;

finalize_it:
	RETiRet;
}

static rsRetVal
curlPost(instanceData *instance, uchar *message, int msglen, uchar *tpl1, uchar *tpl2)
{
	CURLcode code;
	CURL *curl = instance->curlHandle;
	DEFiRet;

	if(instance->dynSrchIdx || instance->dynSrchType)
		CHKiRet(setCurlURL(instance, tpl1, tpl2));

	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (char *)message);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)message); 
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, msglen); 
	code = curl_easy_perform(curl);
	switch (code) {
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_CONNECT:
		case CURLE_WRITE_ERROR:
			STATSCOUNTER_INC(indexConFail, mutIndexConFail);
			return RS_RET_SUSPENDED;
		default:
			STATSCOUNTER_INC(indexSubmit, mutIndexSubmit);
			return RS_RET_OK;
	}
finalize_it:
	RETiRet;
}

BEGINbeginTransaction
CODESTARTbeginTransaction
dbgprintf("omelasticsearch: beginTransaction\n");
	if(!pData->bulkmode) {
		FINALIZE;
	}

	es_emptyStr(pData->batch.data);
finalize_it:
ENDbeginTransaction


BEGINdoAction
CODESTARTdoAction
	if(pData->bulkmode) {
		CHKiRet(buildBatch(pData, ppString[0], ppString[1], ppString[2]));
	} else {
		CHKiRet(curlPost(pData, ppString[0], strlen((char*)ppString[0]),
		                 ppString[1], ppString[2]));
	}
finalize_it:
dbgprintf("omelasticsearch: result doAction: %d\n", iRet);
ENDdoAction


BEGINendTransaction
	char *cstr;
CODESTARTendTransaction
	cstr = es_str2cstr(pData->batch.data, NULL);
	dbgprintf("elasticsearch: endTransaction, batch: '%s'\n", cstr);
	CHKiRet(curlPost(pData, (uchar*) cstr, strlen(cstr), NULL, NULL));
finalize_it:
	free(cstr);
ENDendTransaction

/* elasticsearch POST result string ... useful for debugging */
size_t
curlResult(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	unsigned int i;
	char *p = (char *)ptr;
	char *jsonData = (char *)userdata;
	static char ok[] = "{\"ok\":true,";

	ASSERT(size == 1);
DBGPRINTF("omelasticsearch request: %s\n", jsonData);
DBGPRINTF("omelasticsearch result: ");
for (i = 0; i < nmemb; i++)
	DBGPRINTF("%c", p[i]);
DBGPRINTF("\n");

	if (size == 1 &&
	    nmemb > sizeof(ok)-1 &&
	    strncmp(p, ok, sizeof(ok)-1) == 0) {
		STATSCOUNTER_INC(indexSuccess, mutIndexSuccess);
	} else {
		STATSCOUNTER_INC(indexFailed, mutIndexFailed);
		if (Debug) {
			DBGPRINTF("omelasticsearch request: %s\n", jsonData);
			DBGPRINTF("omelasticsearch result: ");
			for (i = 0; i < nmemb; i++)
				DBGPRINTF("%c", p[i]);
			DBGPRINTF("\n");
		}
	}
	return size * nmemb;
}


static rsRetVal
curlSetup(instanceData *pData)
{
	HEADER *header;
	CURL *handle;

	handle = curl_easy_init();
	if (handle == NULL) {
		return RS_RET_OBJ_CREATION_FAILED;
	}

	header = curl_slist_append(NULL, "Content-Type: text/json; charset=utf-8");
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header); 

	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curlResult);
	curl_easy_setopt(handle, CURLOPT_POST, 1); 

	pData->curlHandle = handle;
	pData->postHeader = header;

	if(pData->bulkmode || (pData->dynSrchIdx == 0 && pData->dynSrchType == 0)) {
		/* in this case, we know no tpls are involved in the request-->NULL OK! */
		setCurlURL(pData, NULL, NULL);
	}

	if(Debug) {
		if(pData->dynSrchIdx == 0 && pData->dynSrchType == 0)
			dbgprintf("omelasticsearch setup, using static REST URL\n");
		else
			dbgprintf("omelasticsearch setup, we have a dynamic REST URL\n");
	}
	return RS_RET_OK;
}

static inline void
setInstParamDefaults(instanceData *pData)
{
	pData->server = NULL;
	pData->port = 9200;
	pData->uid = NULL;
	pData->pwd = NULL;
	pData->searchIndex = NULL;
	pData->searchType = NULL;
	pData->timeout = NULL;
	pData->dynSrchIdx = 0;
	pData->dynSrchType = 0;
	pData->asyncRepl = 0;
	pData->bulkmode = 0;
	pData->tplName = NULL;
}

BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
	int iNumTpls;
CODESTARTnewActInst
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	CHKiRet(createInstance(&pData));
	setInstParamDefaults(pData);

	for(i = 0 ; i < actpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "server")) {
			pData->server = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "serverport")) {
			pData->port = (int) pvals[i].val.d.n, NULL;
		} else if(!strcmp(actpblk.descr[i].name, "uid")) {
			pData->uid = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "pwd")) {
			pData->pwd = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "searchindex")) {
			pData->searchIndex = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "searchtype")) {
			pData->searchType = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "dynsearchindex")) {
			pData->dynSrchIdx = pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "dynsearchtype")) {
			pData->dynSrchType = pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "bulkmode")) {
			pData->bulkmode = pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "timeout")) {
			pData->timeout = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "asyncrepl")) {
			pData->asyncRepl = pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "template")) {
			pData->tplName = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else {
			dbgprintf("omelasticsearch: program error, non-handled "
			  "param '%s'\n", actpblk.descr[i].name);
		}
	}
	
	if(pData->pwd != NULL && pData->uid == NULL) {
		errmsg.LogError(0, RS_RET_UID_MISSING,
			"omelasticsearch: password is provided, but no uid "
			"- action definition invalid");
		ABORT_FINALIZE(RS_RET_UID_MISSING);
	}
	if(pData->dynSrchIdx && pData->searchIndex == NULL) {
		errmsg.LogError(0, RS_RET_LEGA_ACT_NOT_SUPPORTED,
			"omelasticsearch: requested dynamic search index, but no "
			"name for index template given - action definition invalid");
		ABORT_FINALIZE(RS_RET_LEGA_ACT_NOT_SUPPORTED);
	}
	if(pData->dynSrchType && pData->searchType == NULL) {
		errmsg.LogError(0, RS_RET_LEGA_ACT_NOT_SUPPORTED,
			"omelasticsearch: requested dynamic search type, but no "
			"name for type template given - action definition invalid");
		ABORT_FINALIZE(RS_RET_LEGA_ACT_NOT_SUPPORTED);
	}

	if(pData->bulkmode) {
		pData->batch.currTpl1 = NULL;
		pData->batch.currTpl2 = NULL;
		if((pData->batch.data = es_newStr(1024)) == NULL) {
			DBGPRINTF("omelasticsearch: error creating batch string "
			          "turned off bulk mode\n");
			pData->bulkmode = 0; /* at least it works */
		}
	}

	iNumTpls = 1;
	if(pData->dynSrchIdx) ++iNumTpls;
	if(pData->dynSrchType) ++iNumTpls;
	DBGPRINTF("omelasticsearch: requesting %d templates\n", iNumTpls);
	CODE_STD_STRING_REQUESTparseSelectorAct(iNumTpls)

	CHKiRet(OMSRsetEntry(*ppOMSR, 0, (uchar*)strdup((pData->tplName == NULL) ?
					    " StdJSONFmt" : (char*)pData->tplName),
		OMSR_NO_RQD_TPL_OPTS));


	/* we need to request additional templates. If we have a dynamic search index,
	 * it will always be string 1. Type may be 1 or 2, depending on whether search
	 * index is dynamic as well. Rule needs to be followed throughout the module.
	 */
	if(pData->dynSrchIdx) {
		CHKiRet(OMSRsetEntry(*ppOMSR, 1, ustrdup(pData->searchIndex),
			OMSR_NO_RQD_TPL_OPTS));
		if(pData->dynSrchType) {
			CHKiRet(OMSRsetEntry(*ppOMSR, 2, ustrdup(pData->searchType),
				OMSR_NO_RQD_TPL_OPTS));
		}
	} else {
		if(pData->dynSrchType) {
			CHKiRet(OMSRsetEntry(*ppOMSR, 1, ustrdup(pData->searchType),
				OMSR_NO_RQD_TPL_OPTS));
		}
	}

	if(pData->server == NULL)
		pData->server = (uchar*) strdup("localhost");
	if(pData->searchIndex == NULL)
		pData->searchIndex = (uchar*) strdup("system");
	if(pData->searchType == NULL)
		pData->searchType = (uchar*) strdup("events");

	CHKiRet(curlSetup(pData));

CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst


BEGINparseSelectorAct
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	if(!strncmp((char*) p, ":omelasticsearch:", sizeof(":omelasticsearch:") - 1)) {
		errmsg.LogError(0, RS_RET_LEGA_ACT_NOT_SUPPORTED,
			"omelasticsearch supports only v6 config format, use: "
			"action(type=\"omelasticsearch\" server=...)");
	}
	ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
	curl_global_cleanup();
	statsobj.Destruct(&indexStats);
	objRelease(errmsg, CORE_COMPONENT);
        objRelease(statsobj, CORE_COMPONENT);
ENDmodExit

BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
CODEqueryEtryPt_TXIF_OMOD_QUERIES /* we support the transactional interface! */
ENDqueryEtryPt


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(statsobj, CORE_COMPONENT));

	if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
		errmsg.LogError(0, RS_RET_OBJ_CREATION_FAILED, "CURL fail. -elasticsearch indexing disabled");
		ABORT_FINALIZE(RS_RET_OBJ_CREATION_FAILED);
	}

	/* support statistics gathering */
	CHKiRet(statsobj.Construct(&indexStats));
	CHKiRet(statsobj.SetName(indexStats, (uchar *)"elasticsearch"));
	CHKiRet(statsobj.AddCounter(indexStats, (uchar *)"connfail",
		ctrType_IntCtr, &indexConFail));
	CHKiRet(statsobj.AddCounter(indexStats, (uchar *)"submits",
		ctrType_IntCtr, &indexSubmit));
	CHKiRet(statsobj.AddCounter(indexStats, (uchar *)"failed",
		ctrType_IntCtr, &indexFailed));
	CHKiRet(statsobj.AddCounter(indexStats, (uchar *)"success",
		ctrType_IntCtr, &indexSuccess));
	CHKiRet(statsobj.ConstructFinalize(indexStats));
ENDmodInit

/* vi:set ai:
 */