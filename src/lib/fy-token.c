/*
 * fy-token.c - YAML token methods
 *
 * Copyright (c) 2019 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <alloca.h>

#include <libfyaml.h>

#include "fy-parse.h"

#include "fy-ctype.h"
#include "fy-utf8.h"

#include "fy-token.h"

struct fy_token *fy_token_alloc(struct fy_document_state *fyds)
{
	struct fy_token *fyt;
	unsigned int i;

	if (!fyds)
		return NULL;

	fyt = malloc(sizeof(*fyt));
	if (!fyt)
		return fyt;

	memset(fyt, 0, sizeof(*fyt));

	fyt->type = FYTT_NONE;
	fyt->analyze_flags = 0;
	fyt->text_len = 0;
	fyt->text = NULL;
	fyt->text0 = NULL;
	fyt->handle.fyi = NULL;
	for (i = 0; i < sizeof(fyt->comment)/sizeof(fyt->comment[0]); i++)
		fyt->comment[i].fyi = NULL;

	fyt->refs = 1;

	return fyt;
}

void fy_token_free(struct fy_token *fyt)
{
	if (!fyt)
		return;

	/* fy_notice(NULL, "%s: %p #%d", __func__, fyt, fyt->refs); */

	switch (fyt->type) {
	case FYTT_TAG:
		fy_token_unref(fyt->tag.fyt_td);
		break;
	default:
		break;
	}

	if (fyt->text0)
		free(fyt->text0);

	free(fyt);
}

struct fy_token *fy_token_ref(struct fy_token *fyt)
{
	/* take care of overflow */
	if (!fyt)
		return NULL;
	assert(fyt->refs + 1 > 0);
	fyt->refs++;

	/* fy_notice(NULL, "%s: %p #%d", __func__, fyt, fyt->refs); */

	return fyt;
}

void fy_token_unref(struct fy_token *fyt)
{
	if (!fyt)
		return;

	assert(fyt->refs > 0);

	/* fy_notice(NULL, "%s: %p #%d", __func__, fyt, fyt->refs); */

	if (fyt->refs == 1)
		fy_token_free(fyt);
	else
		fyt->refs--;
}

void fy_token_list_unref_all(struct fy_token_list *fytl)
{
	struct fy_token *fyt;

	while ((fyt = fy_token_list_pop(fytl)) != NULL)
		fy_token_unref(fyt);
}

void fy_parse_token_free(struct fy_parser *fyp, struct fy_token *fyt)
{
	if (!fyt)
		return;

	fy_token_free(fyt);
}

struct fy_token *fy_parse_token_alloc(struct fy_parser *fyp)
{
	if (!fyp || !fyp->current_document_state)
		return NULL;

	return fy_token_alloc(fyp->current_document_state);
}

struct fy_token *fy_document_token_alloc(struct fy_document *fyd)
{
	if (!fyd || !fyd->fyds)
		return NULL;

	return fy_token_alloc(fyd->fyds);
}

void fy_parse_token_recycle(struct fy_parser *fyp, struct fy_token *fyt)
{
	fy_token_unref(fyt);
}

struct fy_token *fy_parse_token_new(struct fy_parser *fyp, enum fy_token_type type)
{
	struct fy_token *fyt;

	fyt = fy_parse_token_alloc(fyp);
	if (!fyt)
		return fyt;
	fyt->type = type;

	/* fy_notice(NULL, "%s: %p #%d", __func__, fyt, fyt->refs); */

	return fyt;
}

static int fy_tag_token_format_internal(const struct fy_token *fyt, void *out, size_t *outszp)
{
	char *o = NULL, *oe = NULL;
	size_t outsz;
	const char *handle, *suffix;
	size_t handle_size, suffix_size;
	int len, code_length, rlen;
	uint8_t code[4];
	const char *t, *s, *e;

	if (!fyt || fyt->type != FYTT_TAG)
		return 0;

	if (out && *outszp <= 0)
		return 0;

	if (out) {
		outsz = *outszp;
		o = out;
		oe = out + outsz;
	}

	if (!fyt->tag.fyt_td)
		return -1;

	handle = fy_tag_directive_token_prefix(fyt->tag.fyt_td, &handle_size);
	if (!handle)
		return -1;

	suffix = fy_atom_data(&fyt->handle) + fyt->tag.skip + fyt->tag.handle_length;
	suffix_size = fyt->tag.suffix_length;

#define O_CPY(_src, _len) \
	do { \
		int _l = (_len); \
		if (o && _l) { \
			int _cl = _l; \
			if (_cl > (oe - o)) \
				_cl = oe - o; \
			memcpy(o, (_src), _cl); \
			o += _cl; \
		} \
		len += _l; \
	} while(0)

	len = 0;
	O_CPY(handle, handle_size);

	/* escape suffix as a URI */
	s = suffix;
	e = s + suffix_size;
	while (s < e) {
		/* find next escape */
		t = memchr(s, '%', e - s);
		rlen = (t ? t : e) - s;
		O_CPY(s, rlen);

		/* end of string */
		if (!t)
			break;
		s = t;

		code_length = sizeof(code);
		t = fy_uri_esc(s, e - s, code, &code_length);
		if (!t)
			break;

		/* output escaped utf8 */
		O_CPY(code, code_length);
		s = t;
	}

#undef O_CPY
	return len;

}

int fy_tag_token_format_text_length(const struct fy_token *fyt)
{
	return fy_tag_token_format_internal(fyt, NULL, NULL);
}

const char *fy_tag_token_format_text(const struct fy_token *fyt, char *buf, size_t maxsz)
{
	fy_tag_token_format_internal(fyt, buf, &maxsz);
	return buf;
}

static int fy_tag_directive_token_format_internal(const struct fy_token *fyt,
			void *out, size_t *outszp)
{
	char *o = NULL, *oe = NULL;
	size_t outsz;
	int len;
	const char *handle, *prefix;
	size_t handle_size, prefix_size;

	if (!fyt || fyt->type != FYTT_TAG_DIRECTIVE)
		return 0;

	if (out && *outszp <= 0)
		return 0;

	if (out) {
		outsz = *outszp;
		o = out;
		oe = out + outsz;
	}

#define O_CPY(_src, _len) \
	do { \
		int _l = (_len); \
		if (o && _l) { \
			int _cl = _l; \
			if (_cl > (oe - o)) \
				_cl = oe - o; \
			memcpy(o, (_src), _cl); \
			o += _cl; \
		} \
		len += _l; \
	} while(0)

	len = 0;

	handle = fy_atom_data(&fyt->handle);
	handle_size = fy_atom_size(&fyt->handle);

	prefix = handle + handle_size - fyt->tag_directive.uri_length;
	prefix_size = fyt->tag_directive.uri_length;
	handle_size = fyt->tag_directive.tag_length;

	if (handle_size)
		O_CPY(handle, handle_size);
	else
		O_CPY("!<", 2);
	O_CPY(prefix, prefix_size);
	if (!handle_size)
		O_CPY(">", 1);

#undef O_CPY
	return len;

}

int fy_tag_directive_token_format_text_length(const struct fy_token *fyt)
{
	return fy_tag_directive_token_format_internal(fyt, NULL, NULL);
}

const char *fy_tag_directive_token_format_text(const struct fy_token *fyt, char *buf, size_t maxsz)
{
	fy_tag_directive_token_format_internal(fyt, buf, &maxsz);
	return buf;
}

const char *fy_tag_directive_token_prefix(struct fy_token *fyt, size_t *lenp)
{
	const char *ptr;
	size_t len;

	if (!fyt || fyt->type != FYTT_TAG_DIRECTIVE) {
		*lenp = 0;
		return NULL;
	}
	ptr = fy_atom_data(&fyt->handle);
	len = fy_atom_size(&fyt->handle);
	ptr = ptr + len - fyt->tag_directive.uri_length;
	*lenp = fyt->tag_directive.uri_length;

	return ptr;
}

const char *fy_tag_directive_token_handle(struct fy_token *fyt, size_t *lenp)
{
	const char *ptr;

	if (!fyt || fyt->type != FYTT_TAG_DIRECTIVE) {
		*lenp = 0;
		return NULL;
	}
	ptr = fy_atom_data(&fyt->handle);
	*lenp = fyt->tag_directive.tag_length;
	return ptr;
}

struct fy_token *fy_token_vcreate(struct fy_parser *fyp, enum fy_token_type type, va_list ap)
{
	struct fy_token *fyt = NULL;
	struct fy_atom *handle;
	struct fy_token *fyt_td;

	if (!fyp)
		return NULL;

	fy_error_check(fyp, (unsigned int)type <= FYTT_SCALAR, err_out,
			"illegal token type");

	fyt = fy_parse_token_new(fyp, type);
	fy_error_check(fyp, fyt != NULL, err_out,
			"fy_parse_token_new() failed");

	handle = va_arg(ap, struct fy_atom *);
	fy_error_check(fyp, handle != NULL, err_out,
			"illegal handle argument");
	fyt->handle = *handle;

	switch (fyt->type) {
	case FYTT_TAG_DIRECTIVE:
		fyt->tag_directive.tag_length = va_arg(ap, unsigned int);
		fyt->tag_directive.uri_length = va_arg(ap, unsigned int);
		break;
	case FYTT_SCALAR:
		fyt->scalar.style = va_arg(ap, enum fy_scalar_style);
		fy_error_check(fyp, (unsigned int)fyt->scalar.style < FYSS_MAX, err_out,
					"illegal scalar style argument");
		break;
	case FYTT_TAG:
		fyt->tag.skip = va_arg(ap, unsigned int);
		fyt->tag.handle_length = va_arg(ap, unsigned int);
		fyt->tag.suffix_length = va_arg(ap, unsigned int);

		fyt_td = va_arg(ap, struct fy_token *);
		fy_error_check(fyp, fyt_td != NULL, err_out,
				"illegal tag fyt_td argument");
		fyt->tag.fyt_td = fy_token_ref(fyt_td);
		break;

	case FYTT_NONE:
		fy_error(fyp, "Illegal token type (NONE) for queueing");
		goto err_out;

	default:
		break;
	}

	assert(fyt->handle.fyi);

	return fyt;

err_out:
	fyp->stream_error = true;
	fy_token_unref(fyt);

	return NULL;
}

struct fy_token *fy_token_create(struct fy_parser *fyp, enum fy_token_type type, ...)
{
	struct fy_token *fyt;
	va_list ap;

	va_start(ap, type);
	fyt = fy_token_vcreate(fyp, type, ap);
	va_end(ap);

	return fyt;
}

static struct fy_token *
fy_token_vqueue_internal(struct fy_parser *fyp, struct fy_token_list *fytl,
			 enum fy_token_type type, va_list ap)
{
	struct fy_token *fyt;

	fyt = fy_token_vcreate(fyp, type, ap);
	if (!fyt)
		return NULL;
	fy_token_list_add_tail(fytl, fyt);

	/* special handling for zero indented scalars */
	if (fyt->type == FYTT_DOCUMENT_START) {
		fyp->document_first_content_token = true;
		fy_scan_debug(fyp, "document_first_content_token set to true");
	} else if (fyp->document_first_content_token &&
			fy_token_type_is_content(fyt->type)) {
		fyp->document_first_content_token = false;
		fy_scan_debug(fyp, "document_first_content_token set to false");
	}

	fy_debug_dump_token_list(fyp, fytl, fyt, "queued: ");
	return fyt;
}

struct fy_token *fy_token_queue_internal(struct fy_parser *fyp, struct fy_token_list *fytl,
					 enum fy_token_type type, ...)
{
	va_list ap;
	struct fy_token *fyt;

	va_start(ap, type);
	fyt = fy_token_vqueue_internal(fyp, fytl, type, ap);
	va_end(ap);

	return fyt;
}

struct fy_token *fy_token_vqueue(struct fy_parser *fyp, enum fy_token_type type, va_list ap)
{
	struct fy_token *fyt;

	fyt = fy_token_vqueue_internal(fyp, &fyp->queued_tokens, type, ap);
	if (fyt)
		fyp->token_activity_counter++;
	return fyt;
}

struct fy_token *fy_token_queue(struct fy_parser *fyp, enum fy_token_type type, ...)
{
	va_list ap;
	struct fy_token *fyt;

	va_start(ap, type);
	fyt = fy_token_vqueue(fyp, type, ap);
	va_end(ap);

	return fyt;
}

int fy_token_format_text_length(struct fy_token *fyt)
{
	int length;

	if (!fyt)
		return 0;

	switch (fyt->type) {

	case FYTT_TAG:
		return fy_tag_token_format_text_length(fyt);

	case FYTT_TAG_DIRECTIVE:
		return fy_tag_directive_token_format_text_length(fyt);

	default:
		break;
	}

	length = fy_atom_format_text_length(&fyt->handle);

	return length;
}

const char *fy_token_format_text(struct fy_token *fyt, char *buf, size_t maxsz)
{
	const char *str;

	if (maxsz == 0)
		return buf;

	if (!fyt) {
		if (maxsz > 0)
			buf[0] = '\0';
		return buf;
	}

	switch (fyt->type) {

	case FYTT_TAG:
		return fy_tag_token_format_text(fyt, buf, maxsz);

	case FYTT_TAG_DIRECTIVE:
		return fy_tag_directive_token_format_text(fyt, buf, maxsz);

	default:
		break;
	}

	str = fy_atom_format_text(&fyt->handle, buf, maxsz);

	return str;
}

int fy_token_format_utf8_length(struct fy_token *fyt)
{
	const char *str;
	size_t len;

	if (!fyt)
		return 0;

	switch (fyt->type) {

	case FYTT_TAG:
	case FYTT_TAG_DIRECTIVE:
		str = fy_token_get_text(fyt, &len);
		if (!str)
			return 0;
		return fy_utf8_count(str, len);

	default:
		break;
	}

	return fy_atom_format_utf8_length(&fyt->handle);
}


struct fy_atom *fy_token_atom(struct fy_token *fyt)
{
	return fyt ? &fyt->handle : NULL;
}

const struct fy_mark *fy_token_start_mark(struct fy_token *fyt)
{
	const struct fy_atom *atom;

	atom = fy_token_atom(fyt);
	if (atom)
		return &atom->start_mark;

	/* something we don't track */
	return NULL;
}

const struct fy_mark *fy_token_end_mark(struct fy_token *fyt)
{
	const struct fy_atom *atom;

	atom = fy_token_atom(fyt);
	if (atom)
		return &atom->end_mark;

	/* something we don't track */
	return NULL;
}

enum fy_atom_style fy_token_atom_style(struct fy_token *fyt)
{
	if (!fyt)
		return FYAS_PLAIN;

	if (fyt->type == FYTT_TAG)
		return FYAS_URI;

	return fyt->handle.style;
}

enum fy_scalar_style fy_token_scalar_style(struct fy_token *fyt)
{
	if (!fyt || fyt->type != FYTT_SCALAR)
		return FYSS_PLAIN;

	if (fyt->type == FYTT_SCALAR)
		return fyt->scalar.style;

	return FYSS_PLAIN;
}

int fy_token_text_analyze(struct fy_token *fyt)
{
	const char *s, *e, *nwslb;
	const char *value = NULL;
	enum fy_atom_style style;
	int c, w, ww, cn;
	size_t len;
	int flags;

	if (!fyt)
		return FYTTAF_CAN_BE_SIMPLE_KEY | FYTTAF_DIRECT_OUTPUT | FYTTAF_EMPTY;

	if (fyt->analyze_flags)
		return fyt->analyze_flags;

	/* only tokens that can generate text */
	if (fyt->type != FYTT_SCALAR &&
	    fyt->type != FYTT_TAG &&
	    fyt->type != FYTT_ANCHOR &&
	    fyt->type != FYTT_ALIAS)
		return fyt->analyze_flags = FYTTAF_NO_TEXT_TOKEN;

	flags = FYTTAF_TEXT_TOKEN;

	style = fy_token_atom_style(fyt);

	/* can this token be a simple key initial condition */
	if (!fy_atom_style_is_block(style) && style != FYAS_URI)
		flags |= FYTTAF_CAN_BE_SIMPLE_KEY;

	/* can this token be directly output initial condition */
	if (!fy_atom_style_is_block(style))
		flags |= FYTTAF_DIRECT_OUTPUT;

	/* get value */
	value = fy_token_get_text(fyt, &len);
	if (!value || len == 0)
		return fyt->analyze_flags = flags | FYTTAF_EMPTY;

	s = value;
	e = value + len;

	/* escape for URI, single and double quoted */
	if ((style == FYAS_URI && fy_utf8_memchr(s, '%', e - s)) ||
	    (style == FYAS_SINGLE_QUOTED && fy_utf8_memchr(s, '\'', e - s)) ||
	    (style == FYAS_DOUBLE_QUOTED && fy_utf8_memchr(s, '\\', e - s))) {
		flags &= ~FYTTAF_DIRECT_OUTPUT;
		flags |= FYTTAF_HAS_ESCAPE;
	}

	while (s < e && (c = fy_utf8_get(s, e - s, &w)) >= 0) {

		/* zero can't be output */
		if (c == 0) {
			flags &= ~FYTTAF_DIRECT_OUTPUT;
			flags |= FYTTAF_HAS_ESCAPE;

			s += w;
			continue;
		}

		if (fy_is_ws(c)) {

			cn = fy_utf8_get(s + w, e - (s + w), &ww);

			flags |= FYTTAF_HAS_WS;
			if (fy_is_ws(cn))
				flags |= FYTTAF_HAS_CONSECUTIVE_WS;

			s += w;
			continue;
		}

		if (fy_is_lb(c)) {

			cn = fy_utf8_get(s + w, e - (s + w), &ww);

			flags |= FYTTAF_HAS_LB;
			if (fy_is_lb(cn))
				flags |= FYTTAF_HAS_CONSECUTIVE_LB;
			/* any non double quoted style is a new line */
			if (style != FYAS_DOUBLE_QUOTED)
				flags &= ~FYTTAF_CAN_BE_SIMPLE_KEY;

			/* anything with linebreaks, can't be direct */
			flags &= ~FYTTAF_DIRECT_OUTPUT;

			s += w;
			continue;
		}

		/* find next white space or linebreak */
		nwslb = fy_find_ws_lb(s, e - s);
		if (!nwslb)
			nwslb = e;

		s = nwslb;
	}

	return fyt->analyze_flags = flags;
}

const char *fy_tag_token_get_directive_handle(struct fy_token *fyt, size_t *td_handle_sizep)
{
	if (!fyt || fyt->type != FYTT_TAG || !fyt->tag.fyt_td)
		return NULL;

	return fy_tag_directive_token_handle(fyt->tag.fyt_td, td_handle_sizep);
}

const char *fy_tag_token_get_directive_prefix(struct fy_token *fyt, size_t *td_prefix_sizep)
{
	if (!fyt || fyt->type != FYTT_TAG || !fyt->tag.fyt_td)
		return NULL;

	return fy_tag_directive_token_prefix(fyt->tag.fyt_td, td_prefix_sizep);
}

const char *fy_token_get_direct_output(struct fy_token *fyt, size_t *sizep)
{
	const struct fy_atom *fya;

	fya = fy_token_atom(fyt);
	if (!fya || !fya->direct_output ||
	    (fyt->type == FYTT_TAG || fyt->type == FYTT_TAG_DIRECTIVE) ) {
		*sizep = 0;
		return NULL;
	}
	*sizep = fy_atom_size(fya);
	return fy_atom_data(fya);
}

static void fy_token_prepare_text(struct fy_token *fyt)
{
	int ret;

	assert(fyt);

	/* get text length of this token */
	ret = fy_token_format_text_length(fyt);

	/* no text on this token? */
	if (ret == -1) {
		fyt->text_len = 0;
		fyt->text = fyt->text0 = strdup("");
		return;
	}

	fyt->text0 = malloc(ret + 1);
	if (!fyt->text0) {
		fyt->text_len = 0;
		fyt->text = fyt->text0 = strdup("");
		return;
	}

	fyt->text0[0] = '\0';

	fyt->text_len = ret;

	fy_token_format_text(fyt, fyt->text0, ret + 1);
	fyt->text0[ret] = '\0';

	fyt->text_len = ret;
	fyt->text = fyt->text0;
}

const char *fy_token_get_text(struct fy_token *fyt, size_t *lenp)
{
	/* return empty */
	if (!fyt) {
		*lenp = 0;
		return "";
	}

	/* already found something */
	if (fyt->text) {
		*lenp = fyt->text_len;
		return fyt->text;
	}

	/* try direct output first */
	fyt->text = fy_token_get_direct_output(fyt, &fyt->text_len);
	if (!fyt->text)
		fy_token_prepare_text(fyt);

	*lenp = fyt->text_len;
	return fyt->text;
}

const char *fy_token_get_text0(struct fy_token *fyt)
{
	/* return empty */
	if (!fyt)
		return "";

	/* created text is always zero terminated */
	if (!fyt->text0)
		fy_token_prepare_text(fyt);

	return fyt->text0;
}

size_t fy_token_get_text_length(struct fy_token *fyt)
{
	return fy_token_format_text_length(fyt);
}

unsigned int fy_analyze_scalar_content(const char *data, size_t size)
{
	const char *s, *e;
	int c, lastc, nextc, w, ww, col, break_run;
	unsigned int flags;
	bool first;

	flags = FYACF_EMPTY | FYACF_BLOCK_PLAIN | FYACF_FLOW_PLAIN |
		FYACF_PRINTABLE | FYACF_SINGLE_QUOTED | FYACF_DOUBLE_QUOTED |
		FYACF_SIZE0;

	s = data;
	e = data + size;

	col = 0;
	first = true;
	lastc = -1;
	break_run = 0;
	while (s < e && (c = fy_utf8_get(s, e - s, &w)) >= 0) {

		flags &= ~FYACF_SIZE0;

		lastc = c;

		if (first) {
			if (fy_is_ws(c))
				flags |= FYACF_STARTS_WITH_WS;
			else if (fy_is_lb(c))
				flags |= FYACF_STARTS_WITH_LB;
			first = false;
		}
		nextc = fy_utf8_get(s + w, e - (s + w), &ww);

		/* anything other than white space or linebreak */
		if (!fy_is_ws(c) && !fy_is_lb(c))
			flags &= ~FYACF_EMPTY;

		/* linebreak */
		if (fy_is_lb(c)) {
			flags |= FYACF_LB;
			if (fy_is_lb(nextc))
				flags |= FYACF_CONSECUTIVE_LB;
			break_run++;
		} else
			break_run = 0;

		/* white space */
		if (fy_is_ws(c))
			flags |= FYACF_WS;

		/* anything not printable */
		if (!fy_is_print(c)) {
			flags &= ~FYACF_PRINTABLE;
			flags &= ~(FYACF_BLOCK_PLAIN | FYACF_FLOW_PLAIN | FYACF_SINGLE_QUOTED);
		}

		/* check for document indicators (at column 0) */
		if (col == 0 && (e - s) >= 3 &&
			(!strncmp(s, "---", 3) || !strncmp(s, "...", 3))) {
			flags |= FYACF_DOC_IND;
			flags &= ~(FYACF_BLOCK_PLAIN | FYACF_FLOW_PLAIN);
		}

		/* comment indicator can't be present after a space or lb */
		if ((fy_is_blank(c) || fy_is_lb(c)) && nextc == '#')
			flags &= ~(FYACF_BLOCK_PLAIN | FYACF_FLOW_PLAIN);

		/* : followed by blank can't be any plain */
		if (c == ':' && fy_is_blankz(nextc))
			flags &= ~(FYACF_BLOCK_PLAIN | FYACF_FLOW_PLAIN);

		/* : followed by flow markers can't be a plain in flow context */
		if (fy_utf8_strchr(",[]{}", c) || (c == ':' && fy_utf8_strchr(",[]{}", nextc)))
			flags &= ~FYACF_FLOW_PLAIN;

		if (fy_is_lb(c))
			col = 0;
		else
			col++;

		s += w;
	}

	if (fy_is_ws(lastc))
		flags |= FYACF_ENDS_WITH_WS;
	else if (fy_is_lb(lastc))
		flags |= FYACF_ENDS_WITH_LB;

	if (break_run > 1)
		flags |= FYACF_TRAILING_LB;

	return flags;
}

char *fy_token_debug_text(struct fy_token *fyt)
{
	const char *typetxt;
	const char *text;
	char *buf;
	size_t length;
	int wlen;
	int rc __FY_DEBUG_UNUSED__;

	if (!fyt) {
		typetxt = "<NULL>";
		goto out;
	}

	switch (fyt->type) {
	case FYTT_NONE:
		typetxt = NULL;
		break;
	case FYTT_STREAM_START:
		typetxt = "STRM+";
		break;
	case FYTT_STREAM_END:
		typetxt = "STRM-";
		break;
	case FYTT_VERSION_DIRECTIVE:
		typetxt = "VRSD";
		break;
	case FYTT_TAG_DIRECTIVE:
		typetxt = "TAGD";
		break;
	case FYTT_DOCUMENT_START:
		typetxt = "DOC+";
		break;
	case FYTT_DOCUMENT_END:
		typetxt = "DOC-";
		break;
	case FYTT_BLOCK_SEQUENCE_START:
		typetxt = "BSEQ+";
		break;
	case FYTT_BLOCK_MAPPING_START:
		typetxt = "BMAP+";
		break;
	case FYTT_BLOCK_END:
		typetxt = "BEND";
		break;
	case FYTT_FLOW_SEQUENCE_START:
		typetxt = "FSEQ+";
		break;
	case FYTT_FLOW_SEQUENCE_END:
		typetxt = "FSEQ-";
		break;
	case FYTT_FLOW_MAPPING_START:
		typetxt = "FMAP+";
		break;
	case FYTT_FLOW_MAPPING_END:
		typetxt = "FMAP-";
		break;
	case FYTT_BLOCK_ENTRY:
		typetxt = "BENTR";
		break;
	case FYTT_FLOW_ENTRY:
		typetxt = "FENTR";
		break;
	case FYTT_KEY:
		typetxt = "KEY";
		break;
	case FYTT_SCALAR:
		typetxt = "SCLR";
		break;
	case FYTT_VALUE:
		typetxt = "VAL";
		break;
	case FYTT_ALIAS:
		typetxt = "ALIAS";
		break;
	case FYTT_ANCHOR:
		typetxt = "ANCHR";
		break;
	case FYTT_TAG:
		typetxt = "TAG";
		break;
	default:
		typetxt = NULL;
		break;
	}
	/* should never happen really */
	assert(typetxt);

out:
	text = fy_token_get_text(fyt, &length);

	wlen = length > 8 ? 8 : length;

	rc = asprintf(&buf, "%s:%.*s%s", typetxt, wlen, text, wlen < (int)length ? "..." : "");
	assert(rc != -1);

	return buf;
}

int fy_token_memcmp(struct fy_token *fyt, const void *ptr, size_t len)
{
	const char *value = NULL;
	size_t tlen = 0;

	/* handle NULL cases */
	if (!fyt && (!ptr || !len))
		return 0;

	if (!fyt && (ptr || len))
		return -1;

	if (fyt && (!ptr || !len))
		return 1;

	/* those two are special */
	if (fyt->type == FYTT_TAG || fyt->type == FYTT_TAG_DIRECTIVE) {
		value = fy_token_get_text(fyt, &tlen);
		if (!value)
			return -1;
		return tlen == len ? memcmp(value, ptr, tlen) : tlen < len ? -1 : 1;
	}

	return fy_atom_memcmp(fy_token_atom(fyt), ptr, len);
}

int fy_token_strcmp(struct fy_token *fyt, const char *str)
{
	size_t len;

	len = str ? strlen(str) : 0;

	return fy_token_memcmp(fyt, str, len);
}

int fy_token_cmp(struct fy_token *fyt1, struct fy_token *fyt2)
{
	const char *t1, *t2;
	size_t l1, l2, l;
	int ret;

	/* handles both NULL */
	if (fyt1 == fyt2)
		return 0;

	/* fyt1 is null, 2 wins */
	if (!fyt1 && fyt2)
		return -1;

	/* fyt2 is null, 1 wins */
	if (fyt1 && !fyt2)
		return 1;

	/* tokens with different types can't be equal */
	if (fyt1->type != fyt2->type)
		return fyt2->type > fyt1->type ? -1 : 1;

	/* special case, these can't use the atom comparisons */
	if (fyt1->type == FYTT_TAG || fyt1->type == FYTT_TAG_DIRECTIVE) {
		t1 = fy_token_get_text(fyt1, &l1);
		t2 = fy_token_get_text(fyt2, &l2);
		l = l1 > l2 ? l2 : l1;
		ret = memcmp(t1, t2, l);
		if (ret)
			return ret;
		return l1 == l2 ? 0 : l2 > l1 ? -1 : 1;
	}

	/* just pass it to the atom comparison methods */
	return fy_atom_cmp(fy_token_atom(fyt1), fy_token_atom(fyt2));
}

void fy_token_iter_start(struct fy_token *fyt, struct fy_token_iter *iter)
{
	if (!iter)
		return;

	iter->unget_c = -1;

	if (!fyt)
		return;

	iter->fyt = fyt;

	/* TAG or TAG_DIRECTIVE may only work by getting the text */
	if (fyt->type == FYTT_TAG || fyt->type == FYTT_TAG_DIRECTIVE)
		iter->ic.str = fy_token_get_text(fyt, &iter->ic.len);
	else /* try the direct output next  */
		iter->ic.str = fy_token_get_direct_output(fyt, &iter->ic.len);

	/* got it */
	if (iter->ic.str) {
		memset(&iter->atom_iter, 0, sizeof(iter->atom_iter));
		return;
	}

	assert(fyt->type != FYTT_TAG && fyt->type != FYTT_TAG_DIRECTIVE);

	/* fall back to the atom iterator */
	fy_atom_iter_start(fy_token_atom(fyt), &iter->atom_iter);
}

void fy_token_iter_finish(struct fy_token_iter *iter)
{
	if (!iter)
		return;

	if (!iter->ic.str)
		fy_atom_iter_finish(&iter->atom_iter);

	memset(iter, 0, sizeof(*iter));
}

struct fy_token_iter *
fy_token_iter_create(struct fy_token *fyt)
{
	struct fy_token_iter *iter;

	iter = malloc(sizeof(*iter));
	if (!iter)
		return NULL;
	memset(iter, 0, sizeof(*iter));
	if (fyt)
		fy_token_iter_start(fyt, iter);
	return iter;
}

void fy_token_iter_destroy(struct fy_token_iter *iter)
{
	if (!iter)
		return;

	fy_token_iter_finish(iter);
	free(iter);
}

const struct fy_iter_chunk *fy_token_iter_peek_chunk(struct fy_token_iter *iter)
{
	if (!iter)
		return NULL;

	/* direct mode? */
	if (iter->ic.str)
		return &iter->ic;

	/* fallback to the atom iterator */
	return fy_atom_iter_peek_chunk(&iter->atom_iter);
}

void fy_token_iter_advance(struct fy_token_iter *iter, size_t len)
{
	if (!iter)
		return;

	/* direct mode? */
	if (iter->ic.str) {
		if (len > iter->ic.len)
			len = iter->ic.len;
		iter->ic.str += len;
		iter->ic.len -= len;
		return;
	}

	/* fallback to the atom iterator */
	fy_atom_iter_advance(&iter->atom_iter, len);
}

const struct fy_iter_chunk *
fy_token_iter_chunk_next(struct fy_token_iter *iter, const struct fy_iter_chunk *curr, int *errp)
{
	if (!iter)
		return NULL;

	if (errp)
		*errp = 0;

	/* first time in */
	if (!curr) {
		if (iter->ic.str)
			return iter->ic.len ? &iter->ic : NULL;
		return fy_atom_iter_chunk_next(&iter->atom_iter, NULL, errp);
	}

	/* direct, all consumed */
	if (curr == &iter->ic) {
		iter->ic.str += iter->ic.len;
		iter->ic.len = 0;
		return NULL;
	}

	/* fallback */
	return fy_atom_iter_chunk_next(&iter->atom_iter, curr, errp);
}

ssize_t fy_token_iter_read(struct fy_token_iter *iter, void *buf, size_t count)
{
	if (!iter || !buf)
		return -1;

	/* direct mode */
	if (iter->ic.str) {
		if (count > iter->ic.len)
			count = iter->ic.len;
		memcpy(buf, iter->ic.str, count);
		iter->ic.str += count;
		iter->ic.len -= count;
		return count;
	}

	return fy_atom_iter_read(&iter->atom_iter, buf, count);
}

int fy_token_iter_getc(struct fy_token_iter *iter)
{
	int c;

	if (!iter)
		return -1;

	/* first try the pushed ungetc */
	if (iter->unget_c != -1) {
		c = iter->unget_c;
		iter->unget_c = -1;
		return c;
	}

	/* direct mode */
	if (iter->ic.str) {
		if (!iter->ic.len)
			return -1;
		c = *iter->ic.str++;
		iter->ic.len--;
		return c;
	}

	return fy_atom_iter_getc(&iter->atom_iter);
}

int fy_token_iter_ungetc(struct fy_token_iter *iter, int c)
{
	if (iter->unget_c != -1)
		return -1;
	if (c == -1) {
		iter->unget_c = -1;
		return 0;
	}
	iter->unget_c = c & 0xff;
	return c & 0xff;
}

int fy_token_iter_peekc(struct fy_token_iter *iter)
{
	int c;

	c = fy_token_iter_getc(iter);
	if (c == -1)
		return -1;

	return fy_token_iter_ungetc(iter, c);
}

int fy_token_iter_utf8_get(struct fy_token_iter *iter)
{
	int c, w, w1;

	/* first try the pushed ungetc */
	if (iter->unget_c != -1) {
		c = iter->unget_c;
		iter->unget_c = -1;
		return c;
	}

	/* direct */
	if (iter->ic.str) {

		/* not even 1 octet */
		if (!iter->ic.len)
			return -1;

		/* get width by the first octet */
		w = fy_utf8_width_by_first_octet((uint8_t)*iter->ic.str);
		if (!w || (unsigned int)w > iter->ic.len)
			return -1;

		/* get the next character */
		c = fy_utf8_get(iter->ic.str, w, &w1);

		iter->ic.str += w;
		iter->ic.len -= w;

		return c;
	}

	return fy_atom_iter_utf8_get(&iter->atom_iter);
}

int fy_token_iter_utf8_unget(struct fy_token_iter *iter, int c)
{
	if (iter->unget_c != -1)
		return -1;

	if (c == -1) {
		iter->unget_c = -1;
		return 0;
	}

	iter->unget_c = c;
	return c;
}

int fy_token_iter_utf8_peek(struct fy_token_iter *iter)
{
	int c;

	c = fy_token_iter_utf8_get(iter);
	if (c == -1)
		return -1;

	return fy_token_iter_utf8_unget(iter, c);
}
