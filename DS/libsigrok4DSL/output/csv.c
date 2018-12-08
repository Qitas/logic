/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2011 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "libsigrok.h"
#include "libsigrok-internal.h"
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "config.h" /* Needed for PACKAGE_STRING and others. */

#define LOG_PREFIX "output/csv"

struct context {
	unsigned int num_enabled_channels;
	uint64_t samplerate;
    uint64_t limit_samples;
	char separator;
	gboolean header_done;
	int *channel_index;
    float *channel_vdiv;
    double *channel_vpos;
    double *channel_mmin;
    double *channel_mmax;
    uint64_t mask;
    uint64_t pre_data;
    uint64_t index;
    int type;
};

/*
 * TODO:
 *  - Option to specify delimiter character and/or string.
 *  - Option to (not) print metadata as comments.
 *  - Option to specify the comment character(s), e.g. # or ; or C/C++-style.
 *  - Option to (not) print samplenumber / time as extra column.
 *  - Option to "compress" output (only print changed samples, VCD-like).
 *  - Option to print comma-separated bits, or whole bytes/words (for 8/16
 *    channel LAs) as ASCII/hex etc. etc.
 *  - Trigger support.
 */

static int init(struct sr_output *o, GHashTable *options)
{
	struct context *ctx;
	struct sr_channel *ch;
	GSList *l;
	int i;

	if (!o || !o->sdi)
		return SR_ERR_ARG;

	ctx = g_malloc0(sizeof(struct context));
	o->priv = ctx;
	ctx->separator = ',';
    ctx->mask = 0;
    ctx->index = 0;
    ctx->type = g_variant_get_int16(g_hash_table_lookup(options, "type"));

	/* Get the number of channels, and the unitsize. */
	for (l = o->sdi->channels; l; l = l->next) {
		ch = l->data;
        if (ch->type != ctx->type)
			continue;
		if (!ch->enabled)
			continue;
		ctx->num_enabled_channels++;
	}
	ctx->channel_index = g_malloc(sizeof(int) * ctx->num_enabled_channels);
    ctx->channel_vdiv = g_malloc(sizeof(float) * ctx->num_enabled_channels);
    ctx->channel_vpos = g_malloc(sizeof(double) * ctx->num_enabled_channels);
    ctx->channel_mmax = g_malloc(sizeof(double) * ctx->num_enabled_channels);
    ctx->channel_mmin = g_malloc(sizeof(double) * ctx->num_enabled_channels);

	/* Once more to map the enabled channels. */
	for (i = 0, l = o->sdi->channels; l; l = l->next) {
		ch = l->data;
        if (ch->type != ctx->type)
			continue;
		if (!ch->enabled)
			continue;
        ctx->channel_index[i] = ch->index;
        //ctx->mask |= (1 << ch->index);
        ctx->mask |= (1 << i);
        ctx->channel_vdiv[i] = ch->vdiv * ch->vfactor >= 500 ? ch->vdiv * ch->vfactor / 100.0f : ch->vdiv * ch->vfactor * 10.0f;
        ctx->channel_vpos[i] = ch->vdiv * ch->vfactor >= 500 ? ch->vpos / 1000 : ch->vpos;
        ctx->channel_mmax[i] = ch->map_max;
        ctx->channel_mmin[i] = ch->map_min;
        i++;
	}

	return SR_OK;
}

static GString *gen_header(const struct sr_output *o)
{
	struct context *ctx;
	struct sr_channel *ch;
	GString *header;
	GSList *l;
	time_t t;
	int num_channels, i;

	ctx = o->priv;
	header = g_string_sized_new(512);

	/* Some metadata */
	t = time(NULL);
	g_string_append_printf(header, "; CSV, generated by %s on %s",
			PACKAGE_STRING, ctime(&t));

	/* Columns / channels */
    if (ctx->type == SR_CHANNEL_LOGIC)
        num_channels = g_slist_length(o->sdi->channels);
    else
        num_channels = ctx->num_enabled_channels;
    g_string_append_printf(header, "; Channels (%d/%d)\n",
			ctx->num_enabled_channels, num_channels);

//	if (ctx->samplerate == 0) {
//		if (sr_config_get(o->sdi->driver, o->sdi, NULL, NULL, SR_CONF_SAMPLERATE,
//				&gvar) == SR_OK) {
//			ctx->samplerate = g_variant_get_uint64(gvar);
//			g_variant_unref(gvar);
//		}
//	}
    char *samplerate_s = sr_samplerate_string(ctx->samplerate);
    g_string_append_printf(header, "; Sample rate: %s\n", samplerate_s);
    g_free(samplerate_s);

//    if (sr_config_get(o->sdi->driver, o->sdi, NULL, NULL, SR_CONF_LIMIT_SAMPLES,
//            &gvar) == SR_OK) {
//        uint64_t depth = g_variant_get_uint64(gvar);
//        g_variant_unref(gvar);
//        char *depth_s = sr_samplecount_string(depth);
//        g_string_append_printf(header, "; Sample count: %s\n", depth_s);
//        g_free(depth_s);
//    }
    char *depth_s = sr_samplecount_string(ctx->limit_samples);
    g_string_append_printf(header, "; Sample count: %s\n", depth_s);
    g_free(depth_s);

    if (ctx->type == SR_CHANNEL_LOGIC)
        g_string_append_printf(header, "Time(s),");
    for (i = 0, l = o->sdi->channels; l; l = l->next, i++) {
        ch = l->data;
        if (ch->type != ctx->type)
            continue;
        if (!ch->enabled)
            continue;
        if (ctx->type == SR_CHANNEL_DSO) {
            char *unit_s = (ch->vdiv * ch->vfactor) >= 500 ? "V" : "mV";
            g_string_append_printf(header, " %s (Unit: %s),", ch->name, unit_s);
        } else if (ctx->type == SR_CHANNEL_ANALOG) {
            g_string_append_printf(header, " %s (Unit: %s),", ch->name, ch->map_unit);
        } else {
            g_string_append_printf(header, " %s,", ch->name);
        }
    }
    if (o->sdi->channels)
        /* Drop last separator. */
        g_string_truncate(header, header->len - 1);
    g_string_append_printf(header, "\n");

	return header;
}

static int receive(const struct sr_output *o, const struct sr_datafeed_packet *packet,
		GString **out)
{
	const struct sr_datafeed_meta *meta;
	const struct sr_datafeed_logic *logic;
    const struct sr_datafeed_dso *dso;
    const struct sr_datafeed_analog *analog;
	const struct sr_config *src;
	GSList *l;
	struct context *ctx;
	int idx;
	uint64_t i, j;
    unsigned char *p, c;

	*out = NULL;
	if (!o || !o->sdi)
		return SR_ERR_ARG;
	if (!(ctx = o->priv))
		return SR_ERR_ARG;

	switch (packet->type) {
	case SR_DF_META:
		meta = packet->payload;
		for (l = meta->config; l; l = l->next) {
			src = l->data;
            if (src->key == SR_CONF_SAMPLERATE)
                ctx->samplerate = g_variant_get_uint64(src->data);
            else if (src->key == SR_CONF_LIMIT_SAMPLES)
                ctx->limit_samples = g_variant_get_uint64(src->data);
		}
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		if (!ctx->header_done) {
			*out = gen_header(o);
			ctx->header_done = TRUE;
		} else {
			*out = g_string_sized_new(512);
		}

		for (i = 0; i <= logic->length - logic->unitsize; i += logic->unitsize) {
            ctx->index++;
            if (ctx->index > 1 && (*(uint64_t *)(logic->data + i) & ctx->mask) == ctx->pre_data)
                continue;
            g_string_append_printf(*out, "%0.10g", (ctx->index-1)*1.0/ctx->samplerate);
            for (j = 0; j < ctx->num_enabled_channels; j++) {
                //idx = ctx->channel_index[j];
                idx = j;
				p = logic->data + i + idx / 8;
				c = *p & (1 << (idx % 8));
                g_string_append_c(*out, ctx->separator);
                g_string_append_c(*out, c ? '1' : '0');
			}
			g_string_append_printf(*out, "\n");
            ctx->pre_data = (*(uint64_t *)(logic->data + i) & ctx->mask);
		}
		break;
     case SR_DF_DSO:
        dso = packet->payload;
        if (!ctx->header_done) {
            *out = gen_header(o);
            ctx->header_done = TRUE;
        } else {
            *out = g_string_sized_new(512);
        }

        for (i = 0; i < (uint64_t)dso->num_samples; i++) {
            for (j = 0; j < ctx->num_enabled_channels; j++) {
                idx = ctx->channel_index[j];
                p = dso->data + i * ctx->num_enabled_channels + idx * ((ctx->num_enabled_channels > 1) ? 1 : 0);
                g_string_append_printf(*out, "%0.2f", (128 - *p) * ctx->channel_vdiv[j] / 255 - ctx->channel_vpos[j]);
                g_string_append_c(*out, ctx->separator);
            }

            /* Drop last separator. */
            g_string_truncate(*out, (*out)->len - 1);
            g_string_append_printf(*out, "\n");
        }
        break;
    case SR_DF_ANALOG:
       analog = packet->payload;
       if (!ctx->header_done) {
           *out = gen_header(o);
           ctx->header_done = TRUE;
       } else {
           *out = g_string_sized_new(512);
       }

       for (i = 0; i < (uint64_t)analog->num_samples; i++) {
           for (j = 0; j < ctx->num_enabled_channels; j++) {
               idx = ctx->channel_index[j];
               p = analog->data + i * ctx->num_enabled_channels + idx * ((ctx->num_enabled_channels > 1) ? 1 : 0);
               g_string_append_printf(*out, "%0.2f",
                                      ctx->channel_mmin[j] + (255.0 - *p) / 255.0 * (ctx->channel_mmax[j] - ctx->channel_mmin[j]));
               g_string_append_c(*out, ctx->separator);
           }

           /* Drop last separator. */
           g_string_truncate(*out, (*out)->len - 1);
           g_string_append_printf(*out, "\n");
       }
       break;
	}

	return SR_OK;
}

static int cleanup(struct sr_output *o)
{
	struct context *ctx;

	if (!o || !o->sdi)
		return SR_ERR_ARG;

	if (o->priv) {
		ctx = o->priv;
		g_free(ctx->channel_index);
		g_free(o->priv);
		o->priv = NULL;
	}

	return SR_OK;
}

SR_PRIV struct sr_output_module output_csv = {
	.id = "csv",
	.name = "CSV",
	.desc = "Comma-separated values",
	.exts = (const char*[]){"csv", NULL},
	.options = NULL,
	.init = init,
	.receive = receive,
	.cleanup = cleanup,
};