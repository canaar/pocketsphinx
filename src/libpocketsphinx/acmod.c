/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2008 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */


/**
 * @file acmod.c Acoustic model structures for PocketSphinx.
 * @author David Huggins-Daines <dhuggins@cs.cmu.edu>
 */

/* System headers. */

/* SphinxBase headers. */
#include <prim_type.h>
#include <cmd_ln.h>
#include <strfuncs.h>
#include <string.h>

/* Local headers. */
#include "cmdln_macro.h"
#include "acmod.h"

/* Feature and front-end parameters that may be in feat.params */
static const arg_t feat_defn[] = {
    waveform_to_cepstral_command_line_macro(),
    input_cmdln_options(),
    CMDLN_EMPTY_OPTION
};

static int
acmod_init_am(acmod_t *acmod)
{
    char *mdeffn, *tmatfn, *featparams;

    /* Look for feat.params in acoustic model dir. */
    if ((featparams = cmd_ln_str_r(acmod->config, "-featparams"))) {
        if (cmd_ln_parse_file_r(acmod->config, feat_defn, featparams, FALSE) != NULL) {
	    E_INFO("Parsed model-specific feature parameters from %s\n", featparams);
        }
    }

    /* Read model definition. */
    if ((mdeffn = cmd_ln_str_r(acmod->config, "-mdef")) == NULL) {
        E_ERROR("Must specify -mdef or -hmm\n");
        return -1;
    }

    if ((acmod->mdef = bin_mdef_read(mdeffn)) == NULL) {
        E_ERROR("Failed to read model definition from %s\n", mdeffn);
        return -1;
    }

    /* Read transition matrices. */
    if ((tmatfn = cmd_ln_str_r(acmod->config, "-tmat")) == NULL) {
        E_ERROR("No tmat file specified\n");
        return -1;
    }
    acmod->tmat = tmat_init(tmatfn, acmod->lmath,
                            cmd_ln_float32_r(acmod->config, "-tmatfloor"),
                            TRUE);

    /* Read the acoustic models. */
    if ((cmd_ln_str_r(acmod->config, "-mean") == NULL)
        || (cmd_ln_str_r(acmod->config, "-var") == NULL)
        || (cmd_ln_str_r(acmod->config, "-tmat") == NULL)) {
        E_ERROR("No mean/var/tmat files specified\n");
        return -1;
    }

    E_INFO("Attempting to use SCGMM computation module\n");
    acmod->semi_mgau
        = s2_semi_mgau_init(acmod->config, acmod->lmath, acmod->mdef);
    if (acmod->semi_mgau) {
        char *kdtreefn = cmd_ln_str_r(acmod->config, "-kdtree");
        if (kdtreefn)
            s2_semi_mgau_load_kdtree(acmod->semi_mgau, kdtreefn,
                                     cmd_ln_int32_r(acmod->config, "-kdmaxdepth"),
                                     cmd_ln_int32_r(acmod->config, "-kdmaxbbi"));
    }
    else {
        E_INFO("Falling back to general multi-stream GMM computation\n");
        acmod->ms_mgau =
            ms_mgau_init(acmod->config, acmod->lmath);
    }

    return 0;
}

static int
acmod_init_feat(acmod_t *acmod)
{
    acmod->fcb = 
        feat_init(cmd_ln_str_r(acmod->config, "-feat"),
                  cmn_type_from_str(cmd_ln_str_r(acmod->config,"-cmn")),
                  cmd_ln_boolean_r(acmod->config, "-varnorm"),
                  agc_type_from_str(cmd_ln_str_r(acmod->config, "-agc")),
                  1, cmd_ln_int32_r(acmod->config, "-ceplen"));
    if (acmod->fcb == NULL)
        return -1;

    return 0;
}

int
acmod_fe_mismatch(acmod_t *acmod, fe_t *fe)
{
    /* Output vector dimension needs to be the same. */
    if (cmd_ln_int32_r(acmod->config, "-ceplen") != fe_get_output_size(fe))
        return TRUE;
    /* Feature parameters need to be the same. */
    /* ... */
    return FALSE;
}

int
acmod_feat_mismatch(acmod_t *acmod, feat_t *fcb)
{
    /* Feature type needs to be the same. */
    if (0 != strcmp(cmd_ln_str_r(acmod->config, "-feat"), feat_name(fcb)))
        return TRUE;
    /* Input vector dimension needs to be the same. */
    if (cmd_ln_int32_r(acmod->config, "-ceplen") != feat_cepsize(fcb))
        return TRUE;
    /* FIXME: Need to check LDA and stuff too. */
    return FALSE;
}

acmod_t *
acmod_init(cmd_ln_t *config, logmath_t *lmath, fe_t *fe, feat_t *fcb)
{
    acmod_t *acmod;

    acmod = ckd_calloc(1, sizeof(*acmod));
    acmod->config = config;
    acmod->lmath = lmath;

    /* Load acoustic model parameters. */
    if (acmod_init_am(acmod) < 0)
        goto error_out;

    if (fe) {
        if (acmod_fe_mismatch(acmod, fe))
            goto error_out;
        acmod->fe = fe;
    }
    else {
        /* Initialize a new front end. */
        acmod->retain_fe = TRUE;
        acmod->fe = fe_init_auto_r(config);
        if (acmod->fe == NULL)
            goto error_out;
    }
    if (fcb) {
        if (acmod_feat_mismatch(acmod, fcb))
            goto error_out;
        acmod->fcb = fcb;
    }
    else {
        /* Initialize a new fcb. */
        acmod->retain_fcb = TRUE;
        if (acmod_init_feat(acmod) < 0)
            goto error_out;
    }

    /* The MFCC buffer needs to be at least as large as the dynamic
     * feature window.  */
    acmod->n_mfc_alloc = acmod->fcb->window_size * 2 + 1;
    acmod->mfc_buf = (mfcc_t **)
        ckd_calloc_2d(acmod->n_mfc_alloc, acmod->fcb->cepsize,
                      sizeof(**acmod->mfc_buf));

    /* Allocate a single frame of features for the time being. */
    acmod->n_feat_alloc = 1;
    acmod->feat_buf = feat_array_alloc(acmod->fcb, 1);

    return acmod;

error_out:
    acmod_free(acmod);
    return NULL;
}

void
acmod_free(acmod_t *acmod)
{
    if (acmod->retain_fcb)
        feat_free(acmod->fcb);
    if (acmod->retain_fe)
        fe_close(acmod->fe);
    ckd_free_2d((void **)acmod->mfc_buf);
    feat_array_free(acmod->feat_buf);
    ckd_free(acmod);
}

int
acmod_process_raw(acmod_t *acmod,
		  int16 const **inout_raw,
		  size_t *inout_n_samps,
		  int full_utt)
{
    /* If this is a full utterance, process it all at once. */
    if (full_utt) {
        int32 nfr, ntail;

        /* Resize mfc_buf to fit. */
        if (fe_process_frames(acmod->fe, NULL, inout_n_samps, NULL, &nfr) < 0)
            return -1;
        if (acmod->n_mfc_alloc < nfr + 1) {
            ckd_free_2d(acmod->mfc_buf);
            acmod->mfc_buf = ckd_calloc_2d(nfr + 1, fe_get_output_size(acmod->fe),
                                           sizeof(**acmod->mfc_buf));
            acmod->n_mfc_alloc = nfr + 1;
        }
        acmod->n_mfc_frame = 0;
        fe_start_utt(acmod->fe);
        if (fe_process_frames(acmod->fe, inout_raw, inout_n_samps,
                              acmod->mfc_buf, &nfr) < 0)
            return -1;
        fe_end_utt(acmod->fe, acmod->mfc_buf[nfr], &ntail);
        nfr += ntail;
        acmod->n_mfc_frame = nfr;

        /* Resize feat_buf to fit. */
        if (acmod->n_feat_alloc < nfr) {
            feat_array_free(acmod->feat_buf);
            acmod->feat_buf = feat_array_alloc(acmod->fcb, nfr);
            acmod->n_feat_alloc = nfr;
            acmod->n_feat_frame = 0;
        }
        /* Make dynamic features. */
        nfr = feat_s2mfc2feat_block(acmod->fcb, acmod->mfc_buf, nfr,
                                    TRUE, TRUE, acmod->feat_buf);
        acmod->n_feat_frame = nfr;
        /* Mark all MFCCs as consumed. */
        acmod->n_mfc_frame = 0;
        return nfr;
    }
    else {
        /* Process as many as possible. */
        return 0;
    }
}

int
acmod_process_cep(acmod_t *acmod,
		  mfcc_t const ***inout_cep,
		  int *inout_n_frames,
		  int full_utt)
{
    return 0;
}

int
acmod_process_feat(acmod_t *acmod,
		   mfcc_t const ***feat)
{
    return 0;
}

int32 const *
acmod_score(acmod_t *acmod,
	    int *out_frame_idx,
	    int *out_best_score,
	    int *out_best_senid)
{
    int32 best_score = acmod->log_zero;
    int32 best_idx = -1;

    /* If senone scores haven't been generated for this frame,
     * compute them. */

    if (out_frame_idx) *out_frame_idx = acmod->output_frame;
    if (out_best_score) *out_best_score = best_score;
    if (out_best_senid) *out_best_senid = best_idx;
    return acmod->senone_scores;
}

void
acmod_clear_active(acmod_t *acmod)
{
}

void
acmod_activate_hmm(acmod_t *acmod, hmm_t *hmm)
{
}

int const *
acmod_active_list(acmod_t *acmod, int32 *out_n_active)
{
    /* If active list has not been generated for this frame,
     * generate it from the active bitvector. */

    if (out_n_active) *out_n_active = acmod->n_senone_active;
    return acmod->senone_active;
}