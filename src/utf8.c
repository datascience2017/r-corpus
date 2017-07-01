/*
 * Copyright 2017 Patrick O. Perry.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "corpus/src/array.h"
#include "corpus/src/unicode.h"
#include "rcorpus.h"


int charsxp_width(SEXP charsxp)
{
	const uint8_t *ptr = (const uint8_t *)CHAR(charsxp);
	const uint8_t *end = ptr + XLENGTH(charsxp);
	uint32_t code;
	int width, cw;

	width = 0;
	while (ptr != end) {
		corpus_decode_utf8(&ptr, &code);
		cw = corpus_unicode_charwidth(code);

		switch (cw) {
		case CORPUS_CHARWIDTH_NARROW:
		case CORPUS_CHARWIDTH_AMBIGUOUS:
			width += 1;
			break;

		case CORPUS_CHARWIDTH_WIDE:
		case CORPUS_CHARWIDTH_EMOJI:
			width += 2;
			break;

		default:
			continue;
		}
	}

	return width;
}


static const char *encoding_name(cetype_t ce)
{
	switch (ce) {
	case CE_LATIN1:
		return "latin1";
	case CE_UTF8:
		return "UTF-8";
	case CE_SYMBOL:
		return "symbol";
	case CE_BYTES:
		return "bytes";
	case CE_ANY:
	case CE_NATIVE:
	default:
		return "unknown";
	}
}


static int is_valid(const uint8_t *str, size_t size, size_t *errptr)
{
	const uint8_t *end = str + size;
	const uint8_t *ptr = str;
	size_t err = (size_t)-1;
	int valid, nbyte;

	valid = 1;
	while (ptr != end) {
		nbyte = 1 + CORPUS_UTF8_TAIL_LEN(*ptr);
		if (corpus_scan_utf8(&ptr, end)) {
			err = (size_t)(ptr - str);
			valid = 0;
			goto out;
		}
	}

out:
	if (!valid && errptr) {
		*errptr = err;
	}

	return valid;
}


static int needs_encode_chars(const uint8_t *str, size_t size0, int utf8,
			      int *sizeptr)
{
	const uint8_t *end = str + size0;
	const uint8_t *ptr = str;
	const uint8_t *start;
	uint32_t code;
	int cw, err, needs, nbyte, size;

	size = 0;
	needs = 0;
	while (ptr != end) {
		nbyte = 1 + CORPUS_UTF8_TAIL_LEN(*ptr);
		start = ptr;
		if ((err = corpus_scan_utf8(&start, end))) {
			// encode invalid byte as \xHH (4 bytes)
			needs = 1;
			nbyte = 4;
			ptr++;
		} else {
			if (nbyte == 1) {
				code = *ptr++;

				switch (code) {
				case '\a':
				case '\b':
				case '\f':
				case '\n':
				case '\r':
				case '\t':
				case '\v':
					needs = 1;
					nbyte = 2;
					break;
				default:
					if (!isprint((int)code)) {
						needs = 1;
						nbyte = 4; // \xHH
					}
					break;
				}
			} else {
				corpus_decode_utf8(&ptr, &code);

				if (utf8) {
					cw = corpus_unicode_charwidth(code);
					needs = (cw == CORPUS_CHARWIDTH_OTHER);
				} else {
					needs = 1;
				}

				if (needs) {
					// \uXXXX or \UXXXXYYYY
					nbyte = (code <= 0xFFFF) ? 6 : 10;
				}
			}
		}

		if (size > INT_MAX - nbyte) {
			error("encoded character string size",
			      " exceeds maximum (2^31-1 bytes)");
		}
		size += nbyte;
	}
	if (sizeptr) {
		*sizeptr = size;
	}

	return needs;
}


static void encode_chars(uint8_t *dst, const uint8_t *str, size_t size,
			 int utf8)
{
	const uint8_t *end = str + size;
	const uint8_t *ptr = str;
	const uint8_t *start;
	uint32_t code;
	int cw, err, nbyte;

	while (ptr != end) {
		start = ptr;
		if ((err = corpus_scan_utf8(&start, end))) {
			sprintf((char *)dst, "\\x%02x", (unsigned)*ptr);
			dst += 4;
			ptr++;
			continue;
		}

		start = ptr;
		nbyte = 1 + CORPUS_UTF8_TAIL_LEN(*ptr);

		if (nbyte == 1) {
			code = *ptr++;
			switch (code) {
			case '\a':
				*dst++ = '\\';
				*dst++ = 'a';
				break;
			case '\b':
				*dst++ = '\\';
				*dst++ = 'b';
				break;
			case '\f':
				*dst++ = '\\';
				*dst++ = 'f';
				break;
			case '\n':
				*dst++ = '\\';
				*dst++ = 'n';
				break;
			case '\r':
				*dst++ = '\\';
				*dst++ = 'r';
				break;
			case '\t':
				*dst++ = '\\';
				*dst++ = 't';
				break;
			case '\v':
				*dst++ = '\\';
				*dst++ = 'v';
				break;
			default:
				if (code >= 0x80 || !isprint((int)code)) {
					sprintf((char *)dst, "\\x%02x",
						(unsigned)code);
					dst += 4;
				} else {
					*dst++ = (uint8_t)code;
				}
				break;
			}
			continue;
		}

		corpus_decode_utf8(&ptr, &code);

		if (utf8) {
			cw = corpus_unicode_charwidth(code);
			if (cw != CORPUS_CHARWIDTH_OTHER) {
				while (nbyte-- > 0) {
					*dst++ = *start++;
				}
				continue;
			}
		}

		if (code <= 0xFFFF) {
			sprintf((char *)dst, "\\u%04x", (unsigned)code);
			dst += 6;
		} else {
			sprintf((char *)dst, "\\U%08x", (unsigned)code);
			dst += 10;
		}
	}
}


static int needs_encode_bytes(const uint8_t *str, size_t size0, int *sizeptr)
{
	const uint8_t *end = str + size0;
	const uint8_t *ptr = str;
	int code;
	int needs, nbyte, size;

	size = 0;
	needs = 0;
	while (ptr != end) {
		code = *ptr++;
		switch (code) {
		case '\a':
		case '\b':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
		case '\v':
			needs = 1;
			nbyte = 2;
			break;
		default:
			if (code < 0x80 && isprint(code)) {
				nbyte = 1;
			} else {
				needs = 1;
				nbyte = 4; // \xXX
			}
			break;
		}

		if (size > INT_MAX - nbyte) {
			error("encoded character string size",
			      " exceeds maximum (2^31-1 bytes)");
		}
		size += nbyte;
	}
	if (sizeptr) {
		*sizeptr = size;
	}

	return needs;
}


static void encode_bytes(uint8_t *dst, const uint8_t *str, size_t size)
{
	const uint8_t *end = str + size;
	const uint8_t *ptr = str;
	uint8_t code;

	while (ptr != end) {
		code = *ptr++;
		switch (code) {
		case '\a':
			*dst++ = '\\';
			*dst++ = 'a';
			break;
		case '\b':
			*dst++ = '\\';
			*dst++ = 'b';
			break;
		case '\f':
			*dst++ = '\\';
			*dst++ = 'f';
			break;
		case '\n':
			*dst++ = '\\';
			*dst++ = 'n';
			break;
		case '\r':
			*dst++ = '\\';
			*dst++ = 'r';
			break;
		case '\t':
			*dst++ = '\\';
			*dst++ = 't';
			break;
		case '\v':
			*dst++ = '\\';
			*dst++ = 'v';
			break;
		default:
			if (code < 0x80 && isprint((int)code)) {
				*dst++ = code;
			} else {
				sprintf((char *)dst, "\\x%02x",
					(unsigned)code);
				dst += 4;
			}
			break;
		}
	}
}


static SEXP charsxp_encode(SEXP sx, int utf8, char **bufptr, int *nbufptr)
{
	const uint8_t *str, *str2;
	char *buf = *bufptr;
	int nbuf = *nbufptr;
	int conv, size, size2;
	cetype_t ce;

	if (sx == NA_STRING) {
		return NA_STRING;
	}

	str = (const uint8_t *)CHAR(sx);
	size = (size_t)XLENGTH(sx);
	conv = 0;

	ce = getCharCE(sx);
	switch (ce) {
	case CE_ANY:
	case CE_BYTES:
	case CE_UTF8:
#if (!defined(_WIN32) && !defined(_WIN64))
	case CE_NATIVE: // assume that 'native' is UTF-8 on non-Windows
#endif
		break;

	default:
		str2 = (const uint8_t *)translateCharUTF8(sx);
		ce = CE_UTF8;
		if (str2 != str) {
			str = str2;
			size = strlen((const char *)str);
			conv = 1;
		}
	}

	if (ce == CE_BYTES) {
		if (!needs_encode_bytes(str, size, &size2)) {
			return sx;
		}
	} else if (!needs_encode_chars(str, size, utf8, &size2)) {
		if (conv) {
			sx = mkCharLenCE((const char *)str, size, CE_UTF8);
		}
		return sx;
	}

	if (size2 > nbuf) {
		corpus_array_size_add(&nbuf, 1, 0, size2);
		buf = R_alloc(size2, 1);
		*bufptr = buf;
		*nbufptr = nbuf;
	}

	if (ce == CE_BYTES) {
		encode_bytes((uint8_t *)buf, str, size);
	} else {
		encode_chars((uint8_t *)buf, str, size, utf8);
	}

	return mkCharLenCE(buf, size2, CE_UTF8);
}


SEXP utf8_valid(SEXP sx)
{
	char buf[256];
	SEXP ans, sstr;
	cetype_t ce;
	const uint8_t *str;
	size_t size, err;
	R_xlen_t i, n;
	int nprot, raw;
	
	if (sx == R_NilValue) {
		return ScalarLogical(TRUE);
	}

	nprot = 0;
	PROTECT(sx = coerceVector(sx, STRSXP)); nprot++;
	n = XLENGTH(sx);
	for (i = 0; i < n; i++) {
		sstr = STRING_ELT(sx, i);
		if (sstr == NA_STRING) {
			continue;
		}

		ce = getCharCE(sstr);
		raw = (ce == CE_ANY || ce == CE_UTF8 || ce == CE_BYTES);

		if (raw) {
			str = (const uint8_t *)CHAR(sstr);
			size = (size_t)XLENGTH(sstr);
		} else {
			str = (const uint8_t *)translateCharUTF8(sstr);
			size = strlen((const char *)str);
		}

		if (!is_valid(str, size, &err)) {
			if (ce == CE_UTF8) {
				snprintf(buf, sizeof(buf),
					 "entry %"PRIu64
					 " is marked as \"UTF-8\""
					 " but string byte %"PRIu64
					 " (\"\\x%0x\")"
					 " is invalid in that encoding",
					 (uint64_t)i + 1,
					 (uint64_t)err + 1,
					 (unsigned)str[err]);
			} else {
				snprintf(buf, sizeof(buf),
					 "cannot convert entry %"PRIu64
					 " from \"%s\" encoding to \"UTF-8\";"
					 " %sstring byte %"PRIu64
					 " (\"\\x%0x\") is invalid",
					 (uint64_t)i + 1,
					 encoding_name(ce),
					 ce == CE_BYTES ? ""
					 	        : "after conversion, ",
					 (uint64_t)err + 1,
					 (unsigned)str[err]);
			}
			PROTECT(ans = ScalarString(mkChar(buf))); nprot++;
			goto out;
		}
	}

	ans = ScalarLogical(TRUE);
out:
	UNPROTECT(nprot);
	return ans;
}


SEXP utf8_width(SEXP sx)
{
	SEXP ans, elt;
	R_xlen_t i, n;
	int w;

	if (!isString(sx)) {
		error("argument 'x' is not a character vector");
	}
	n = XLENGTH(sx);

	PROTECT(ans = allocVector(INTSXP, n));
	setAttrib(ans, R_NamesSymbol, getAttrib(sx, R_NamesSymbol));

	for (i = 0; i < n; i++) {
		elt = STRING_ELT(sx, i);
		if (elt == NA_STRING) {
			w = NA_INTEGER;
		} else {
			w = charsxp_width(elt);
		}
		INTEGER(ans)[i] = w;
	}

	UNPROTECT(1);
	return ans;
}


SEXP utf8_encode(SEXP sx, SEXP sutf8)
{
	SEXP ans, elt, elt2;
	char *buf;
	R_xlen_t i, n;
	int duped, nbuf, utf8;

	if (!isString(sx)) {
		error("argument 'x' is not a character vector");
	}
	n = XLENGTH(sx);

	if (!isLogical(sutf8) || XLENGTH(sutf8) != 1) {
		error("argument 'utf8' is not a logical scalar");
	}
	utf8 = LOGICAL(sutf8)[0] == TRUE;

	ans = sx;
	duped = 0;
	buf = NULL;
	nbuf = 0;

	for (i = 0; i < n; i++) {
		elt = STRING_ELT(sx, i);
		elt2 = charsxp_encode(elt, utf8, &buf, &nbuf);
		if (!duped && elt != elt2) {
			PROTECT(ans = duplicate(ans));
			duped = 1;
		}
		SET_STRING_ELT(ans, i, elt2);
	}

	if (duped) {
		UNPROTECT(1);
	}

	return ans;
}
