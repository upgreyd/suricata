/* Copyright (C) 2007-2010 Open Information Security Foundation
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

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Simple flowvar content match part of the detection engine.
 */

#include "suricata-common.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-content.h"
#include "threads.h"
#include "flow.h"
#include "flow-var.h"
#include "detect-flowvar.h"
#include "util-spm.h"
#include "util-var-name.h"
#include "util-debug.h"

#define PARSE_REGEX         "(.*),(.*)"
static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectFlowvarMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
static int DetectFlowvarSetup (DetectEngineCtx *, Signature *, char *);
static int DetectFlowvarPostMatch(ThreadVars *tv, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *sm);
static void DetectFlowvarDataFree(void *ptr);

void DetectFlowvarRegister (void) {
    sigmatch_table[DETECT_FLOWVAR].name = "flowvar";
    sigmatch_table[DETECT_FLOWVAR].Match = DetectFlowvarMatch;
    sigmatch_table[DETECT_FLOWVAR].Setup = DetectFlowvarSetup;
    sigmatch_table[DETECT_FLOWVAR].Free  = DetectFlowvarDataFree;
    sigmatch_table[DETECT_FLOWVAR].RegisterTests  = NULL;

    /* post-match for flowvar storage */
    sigmatch_table[DETECT_FLOWVAR_POSTMATCH].name = "__flowvar__postmatch__";
    sigmatch_table[DETECT_FLOWVAR_POSTMATCH].Match = DetectFlowvarPostMatch;
    sigmatch_table[DETECT_FLOWVAR_POSTMATCH].Setup = NULL;
    sigmatch_table[DETECT_FLOWVAR_POSTMATCH].Free  = DetectFlowvarDataFree;
    sigmatch_table[DETECT_FLOWVAR_POSTMATCH].RegisterTests  = NULL;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

    return;

error:
    return;
}

/**
 * \brief this function will SCFree memory associated with DetectFlowvarData
 *
 * \param cd pointer to DetectCotentData
 */
static void DetectFlowvarDataFree(void *ptr) {
    if (ptr == NULL)
        SCReturn;

    DetectFlowvarData *fd = (DetectFlowvarData *)ptr;

    if (fd->name)
        SCFree(fd->name);
    if (fd->content)
        SCFree(fd->content);

    SCFree(fd);
}

/*
 * returns 0: no match
 *         1: match
 *        -1: error
 */

int DetectFlowvarMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m)
{
    int ret = 0;
    DetectFlowvarData *fd = (DetectFlowvarData *)m->ctx;

    /* we need a lock */
    FLOWLOCK_RDLOCK(p->flow);

    FlowVar *fv = FlowVarGet(p->flow, fd->idx);
    if (fv != NULL) {
        uint8_t *ptr = SpmSearch(fv->data.fv_str.value,
                                 fv->data.fv_str.value_len,
                                 fd->content, fd->content_len);
        if (ptr != NULL)
            ret = 1;
    }
    FLOWLOCK_UNLOCK(p->flow);

    return ret;
}

static int DetectFlowvarSetup (DetectEngineCtx *de_ctx, Signature *s, char *rawstr)
{
    DetectFlowvarData *cd = NULL;
    SigMatch *sm = NULL;
    char *str = rawstr;
    char dubbed = 0;
    uint16_t len;
    char *varname = NULL, *varcontent = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, rawstr, strlen(rawstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret != 3) {
        SCLogError(SC_ERR_PCRE_MATCH, "\"%s\" is not a valid setting for flowvar.", rawstr);
        return -1;
    }

    const char *str_ptr;
    res = pcre_get_substring((char *)rawstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        return -1;
    }
    varname = (char *)str_ptr;

    if (ret > 2) {
        res = pcre_get_substring((char *)rawstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            return -1;
        }
        varcontent = (char *)str_ptr;
    }

    if (varcontent[0] == '\"' && varcontent[strlen(varcontent)-1] == '\"') {
        str = SCStrdup(varcontent+1);
        if (unlikely(str == NULL)) {
            return -1;
        }
        str[strlen(varcontent)-2] = '\0';
        dubbed = 1;
    }

    len = strlen(str);
    if (len == 0) {
        if (dubbed) SCFree(str);
        return -1;
    }

    cd = SCMalloc(sizeof(DetectFlowvarData));
    if (unlikely(cd == NULL))
        goto error;

    char converted = 0;

    {
        uint16_t i, x;
        uint8_t bin = 0, binstr[3] = "", binpos = 0;
        for (i = 0, x = 0; i < len; i++) {
            // printf("str[%02u]: %c\n", i, str[i]);
            if (str[i] == '|') {
                if (bin) {
                    bin = 0;
                } else {
                    bin = 1;
                }
            } else {
                if (bin) {
                    if (isdigit((unsigned char)str[i]) ||
                        str[i] == 'A' || str[i] == 'a' ||
                        str[i] == 'B' || str[i] == 'b' ||
                        str[i] == 'C' || str[i] == 'c' ||
                        str[i] == 'D' || str[i] == 'd' ||
                        str[i] == 'E' || str[i] == 'e' ||
                        str[i] == 'F' || str[i] == 'f') {
                        // printf("part of binary: %c\n", str[i]);

                        binstr[binpos] = (char)str[i];
                        binpos++;

                        if (binpos == 2) {
                            uint8_t c = strtol((char *)binstr, (char **) NULL, 16) & 0xFF;
                            binpos = 0;
                            str[x] = c;
                            x++;
                            converted = 1;
                        }
                    } else if (str[i] == ' ') {
                        // printf("space as part of binary string\n");
                    }
                } else {
                    str[x] = str[i];
                    x++;
                }
            }
        }
#ifdef DEBUG
        if (SCLogDebugEnabled()) {
            for (i = 0; i < x; i++) {
                if (isprint((unsigned char)str[i])) printf("%c", str[i]);
                else printf("\\x%02u", str[i]);
            }
            printf("\n");
        }
#endif

        if (converted)
            len = x;
    }

    cd->content = SCMalloc(len);
    if (cd->content == NULL) {
        if (dubbed) SCFree(str);
        SCFree(cd);
        return -1;
    }

    cd->name = SCStrdup(varname);
    cd->idx = VariableNameGetIdx(de_ctx, varname, DETECT_FLOWVAR);
    memcpy(cd->content, str, len);
    cd->content_len = len;
    cd->flags = 0;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_FLOWVAR;
    sm->ctx = (void *)cd;

    SigMatchAppendSMToList(s, sm, DETECT_SM_LIST_MATCH);

    if (dubbed) SCFree(str);
    return 0;

error:
    if (dubbed) SCFree(str);
    if (cd) SCFree(cd);
    if (sm) SCFree(sm);
    return -1;
}


/** \brief Store flowvar in det_ctx so we can exec it post-match */
int DetectFlowvarStoreMatch(DetectEngineThreadCtx *det_ctx, uint16_t idx, uint8_t *buffer, uint16_t len) {
    DetectFlowvarList *fs = det_ctx->flowvarlist;

    /* first check if we have had a previous match for this idx */
    for ( ; fs != NULL; fs = fs->next) {
        if (fs->idx == idx) {
            /* we're replacing the older store */
            SCFree(fs->buffer);
            fs->buffer = NULL;
            break;
        }
    }

    if (fs == NULL) {
        fs = SCMalloc(sizeof(*fs));
        if (unlikely(fs == NULL))
            return -1;

        fs->idx = idx;

        fs->next = det_ctx->flowvarlist;
        det_ctx->flowvarlist = fs;
    }

    fs->len = len;
    fs->buffer = buffer;
    return 0;
}

/** \brief Setup a post-match for flowvar storage
 *  We're piggyback riding the DetectFlowvarData struct
 */
int DetectFlowvarPostMatchSetup(Signature *s, uint16_t idx) {
    SigMatch *sm = NULL;
    DetectFlowvarData *fv = NULL;

    fv = SCMalloc(sizeof(DetectFlowvarData));
    if (unlikely(fv == NULL))
        goto error;
    memset(fv, 0x00, sizeof(*fv));

    /* we only need the idx */
    fv->idx = idx;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_FLOWVAR_POSTMATCH;
    sm->ctx = (void *)fv;

    SigMatchAppendSMToList(s, sm, DETECT_SM_LIST_POSTMATCH);
    return 0;
error:
    return -1;
}

/** \internal
 *  \brief post-match func to store flowvars in the flow
 *  \param sm sigmatch containing the idx to store
 *  \retval 1 or -1 in case of error
 */
static int DetectFlowvarPostMatch(ThreadVars *tv, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *sm) {
    DetectFlowvarList *fs, *prev;
    DetectFlowvarData *fd;

    if (det_ctx->flowvarlist == NULL || p->flow == NULL)
        return 1;

    fd = (DetectFlowvarData *)sm->ctx;

    prev = NULL;
    fs = det_ctx->flowvarlist;
    while (fs != NULL) {
        if (fd->idx == fs->idx) {
            FlowVarAddStr(p->flow, fs->idx, fs->buffer, fs->len);
            /* memory at fs->buffer is now the responsibility of
             * the flowvar code. */

            if (fs == det_ctx->flowvarlist) {
                det_ctx->flowvarlist = fs->next;
                SCFree(fs);
                fs = det_ctx->flowvarlist;
            } else {
                prev->next = fs->next;
                SCFree(fs);
                fs = prev->next;
            }
        } else {
            prev = fs;
            fs = fs->next;
        }
    }
    return 1;
}

/** \brief Clean flowvar candidate list in det_ctx */
void DetectFlowvarCleanupList(DetectEngineThreadCtx *det_ctx) {
    DetectFlowvarList *fs, *next;
    if (det_ctx->flowvarlist != NULL) {
        fs = det_ctx->flowvarlist;
        while (fs != NULL) {
            next = fs->next;
            SCFree(fs->buffer);
            SCFree(fs);
            fs = next;
        }

        det_ctx->flowvarlist = NULL;
    }
}

