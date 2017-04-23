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
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <Rdefines.h>
#include "corpus/src/error.h"
#include "corpus/src/filebuf.h"
#include "corpus/src/render.h"
#include "corpus/src/table.h"
#include "corpus/src/text.h"
#include "corpus/src/token.h"
#include "corpus/src/symtab.h"
#include "corpus/src/data.h"
#include "corpus/src/datatype.h"
#include "rcorpus.h"

#define DATASET_TAG install("corpus::dataset")


static SEXP subrows_dataset(SEXP sdata, SEXP si);
static SEXP subfield_dataset(SEXP sdata, SEXP sname);


static void free_dataset(SEXP sdataset)
{
        struct dataset *d = R_ExternalPtrAddr(sdataset);
	if (d) {
		free(d->rows);
		free(d);
	}
}


SEXP alloc_dataset(SEXP sfilebuf, SEXP sfield, SEXP srows)
{
	SEXP ans, sclass, shandle, snames;
	struct dataset *obj;
	int err;

	PROTECT(shandle = R_MakeExternalPtr(NULL, DATASET_TAG, R_NilValue));
	R_RegisterCFinalizerEx(shandle, free_dataset, TRUE);

	if (!(obj = malloc(sizeof(*obj)))) {
		error("failed allocating memory (%u bytes)",
			(unsigned)sizeof(*obj));
	}
	if ((err = schema_init(&obj->schema))) {
		free(obj);
		error("failed allocating memory");
	}

	obj->rows = NULL;
	obj->nrow = 0;
	obj->type_id = DATATYPE_NULL;
	obj->kind = DATATYPE_NULL;

	R_SetExternalPtrAddr(shandle, obj);

	PROTECT(ans = allocVector(VECSXP, 4));
	SET_VECTOR_ELT(ans, 0, shandle);
	SET_VECTOR_ELT(ans, 1, sfilebuf);
	SET_VECTOR_ELT(ans, 2, sfield);
	SET_VECTOR_ELT(ans, 3, srows);

	PROTECT(snames = allocVector(STRSXP, 4));
	SET_STRING_ELT(snames, 0, mkChar("handle"));
	SET_STRING_ELT(snames, 1, mkChar("filebuf"));
	SET_STRING_ELT(snames, 2, mkChar("field"));
	SET_STRING_ELT(snames, 3, mkChar("rows"));
	setAttrib(ans, R_NamesSymbol, snames);

	PROTECT(sclass = allocVector(STRSXP, 1));
	SET_STRING_ELT(sclass, 0, mkChar("dataset"));
	setAttrib(ans, R_ClassSymbol, sclass);

	UNPROTECT(4);
	return ans;
}


static void grow_datarows(struct data **rowsptr, R_xlen_t *nrow_maxptr)
{
	void *base1, *base = *rowsptr;
	size_t size1, size = (size_t)*nrow_maxptr;
	size_t width = sizeof(**rowsptr);

	if (size == 0) {
		size1 = 1;
	} else {
		size1 = 1.618 * size + 1; // (golden ratio)
	}

	if (size1 < size) { // overflow
		size1 = SIZE_MAX;
	}

	if (size1 > SIZE_MAX / width) {
		free(base);
		error("number of rows (%"PRIu64")"
			" exceeds maximum (%"PRIu64")",
			(uint64_t)size1, (uint64_t)SIZE_MAX / width);
	}
	if (size1 > R_XLEN_T_MAX) {
		free(base);
		error("number of rows (%"PRIu64") exceeds maximum (%"PRIu64")",
			(uint64_t)size1, (uint64_t)R_XLEN_T_MAX);
	}

	base1 = realloc(base, size1 * width);
	if (size1 > 0 && base1 == NULL) {
		free(base);
		error("failed allocating %"PRIu64" bytes",
			(uint64_t)size1 * width);
	}

	*rowsptr = base1;
	*nrow_maxptr = size1;
}


static void dataset_load(SEXP sdata)
{
	SEXP shandle, sfilebuf;
	struct dataset *obj;
	struct data *datarows;
	struct filebuf *buf;
	struct filebuf_iter it;
	const uint8_t *ptr;
	size_t size;
	R_xlen_t nrow, nrow_max;
	int err, type_id;

	shandle = getListElement(sdata, "handle");

	obj = R_ExternalPtrAddr(shandle);
	if (obj == NULL) {
		if (!(obj = malloc(sizeof(*obj)))) {
			error("failed allocating memory (%u bytes)",
			      (unsigned)sizeof(*obj));
		}
		if ((err = schema_init(&obj->schema))) {
			free(obj);
			error("failed allocating memory");
		}

		obj->rows = NULL;
		obj->nrow = 0;
		obj->type_id = DATATYPE_NULL;
		obj->kind = DATATYPE_NULL;

		R_SetExternalPtrAddr(shandle, obj);
		R_RegisterCFinalizerEx(shandle, free_dataset, TRUE);
	}

	if (obj->rows != NULL) {
		// already loaded
		return;
	}

	sfilebuf = getListElement(sdata, "filebuf");
	buf = as_filebuf(sfilebuf);

	type_id = DATATYPE_NULL;
	nrow = 0;
	nrow_max = 0;
	datarows = NULL;

	filebuf_iter_make(&it, buf);
	while (filebuf_iter_advance(&it)) {
		if (nrow == nrow_max) {
			grow_datarows(&datarows, &nrow_max);
		}

		ptr = it.current.ptr;
		size = it.current.size;

		if ((err = data_assign(&datarows[nrow], &obj->schema, ptr,
						size))) {
			free(datarows);
			free(obj);
			error("error parsing row %"PRIu64
			      " of JSON file", (uint64_t)(nrow + 1));
		}

		if ((err = schema_union(&obj->schema, type_id,
					datarows[nrow].type_id,
					&type_id))) {
			free(datarows);
			free(obj);
			error("memory allocation failure"
			      " after parsing row %"PRIu64
			      " of JSON file", (uint64_t)(nrow + 1));
		}
		nrow++;
	}

	// free excess memory
	datarows = realloc(datarows, nrow * sizeof(*datarows));

	// ensure datarows is non-NULL even if nrow == 0
	if (datarows == NULL) {
		datarows = malloc(sizeof(*datarows));
		if (!datarows) {
			error("failed allocating memory (%u bytes)",
				sizeof(*datarows));
		}
	}

	// set the fields
	obj->rows = datarows;
	obj->nrow = nrow;
	obj->type_id = type_id;

	if (type_id < 0) {
		obj->kind = DATATYPE_ANY;
	} else {
		obj->kind = obj->schema.types[type_id].kind;
	}


	// TODO: extract subfield, rows
}


int is_dataset(SEXP sdata)
{
	SEXP handle, filebuf;

	if (!isVectorList(sdata)) {
		return 0;
	}

	handle = getListElement(sdata, "handle");
	if (handle == R_NilValue) {
		return 0;
	}

	filebuf = getListElement(sdata, "filebuf");
	if (!is_filebuf(filebuf)) {
		return 0;
	}

	return ((TYPEOF(handle) == EXTPTRSXP)
		&& (R_ExternalPtrTag(handle) == DATASET_TAG));
}


struct dataset *as_dataset(SEXP sdata)
{
	SEXP shandle;
	struct dataset *obj;

	if (!is_dataset(sdata)) {
		error("invalid 'dataset' object");
	}

	dataset_load(sdata);

	shandle = getListElement(sdata, "handle");
	obj = R_ExternalPtrAddr(shandle);

	return obj;
}


SEXP dim_dataset(SEXP sdata)
{
	SEXP dims;
	const struct dataset *d = as_dataset(sdata);
	const struct datatype *t;
	const struct datatype_record *r;

	if (d->kind != DATATYPE_RECORD) {
		return R_NilValue;
	}

	t = &d->schema.types[d->type_id];
	r = &t->meta.record;

	if (d->nrow > INT_MAX) {
		PROTECT(dims = allocVector(REALSXP, 2));
		REAL(dims)[0] = (double)d->nrow;
		REAL(dims)[1] = (double)r->nfield;
	} else {
		PROTECT(dims = allocVector(INTSXP, 2));
		INTEGER(dims)[0] = d->nrow;
		INTEGER(dims)[1] = (int)r->nfield;
	}
	UNPROTECT(1);

	return dims;
}


SEXP length_dataset(SEXP sdata)
{
	const struct dataset *d = as_dataset(sdata);
	const struct datatype *t;
	const struct datatype_record *r;

	if (d->kind == DATATYPE_RECORD) {
		t = &d->schema.types[d->type_id];
		r = &t->meta.record;
		return ScalarInteger(r->nfield);
	}

	if (d->nrow > INT_MAX) {
		return ScalarReal((double)d->nrow);
	} else {
		return ScalarInteger((int)d->nrow);
	}
}


SEXP names_dataset(SEXP sdata)
{
	SEXP names, str;
	const struct dataset *d = as_dataset(sdata);
	const struct datatype *t;
	const struct datatype_record *r;
	const struct text *name;
	int i;

	if (d->kind != DATATYPE_RECORD) {
		return R_NilValue;
	}

	t = &d->schema.types[d->type_id];
	r = &t->meta.record;

	PROTECT(names = allocVector(STRSXP, r->nfield));
	for (i = 0; i < r->nfield; i++) {
		name = &d->schema.names.types[r->name_ids[i]].text;
		str = mkCharLenCE((char *)name->ptr, TEXT_SIZE(name), CE_UTF8);
		SET_STRING_ELT(names, i, str);
	}

	UNPROTECT(1);
	return names;
}


SEXP datatype_dataset(SEXP sdata)
{
	SEXP str, ans;
	const struct dataset *d = as_dataset(sdata);
	struct render r;

	if (render_init(&r, ESCAPE_NONE) != 0) {
		error("memory allocation failure");
	}
	render_set_tab(&r, "");
	render_set_newline(&r, " ");

	render_datatype(&r, &d->schema, d->type_id);
	if (r.error) {
		render_destroy(&r);
		error("memory allocation failure");
	}

	PROTECT(ans = allocVector(STRSXP, 1));
	str = mkCharLenCE(r.string, r.length, CE_UTF8);
	SET_STRING_ELT(ans, 0, str);

	render_destroy(&r);
	UNPROTECT(1);
	return ans;
}


SEXP datatypes_dataset(SEXP sdata)
{
	SEXP types, str, names;
	const struct dataset *d = as_dataset(sdata);
	const struct datatype *t;
	const struct datatype_record *rec;
	struct render r;
	int i;

	if (d->kind != DATATYPE_RECORD) {
		return R_NilValue;
	}

	PROTECT(names = names_dataset(sdata));

	t = &d->schema.types[d->type_id];
	rec = &t->meta.record;

	if (render_init(&r, ESCAPE_NONE) != 0) {
		error("memory allocation failure");
	}
	render_set_tab(&r, "");
	render_set_newline(&r, " ");

	PROTECT(types = allocVector(STRSXP, rec->nfield));
	for (i = 0; i < rec->nfield; i++) {
		render_datatype(&r, &d->schema, rec->type_ids[i]);
		if (r.error) {
			render_destroy(&r);
			error("memory allocation failure");
		}
		str = mkCharLenCE(r.string, r.length, CE_UTF8);
		SET_STRING_ELT(types, i, str);
		render_clear(&r);
	}
	setAttrib(types, R_NamesSymbol, names);

	render_destroy(&r);
	UNPROTECT(2);
	return types;
}


SEXP print_dataset(SEXP sdata)
{
	const struct dataset *d = as_dataset(sdata);
	struct render r;

	if (render_init(&r, ESCAPE_CONTROL) != 0) {
		error("memory allocation failure");
	}

	render_datatype(&r, &d->schema, d->type_id);
	if (r.error) {
		render_destroy(&r);
		error("memory allocation failure");
	}

	if (d->kind == DATATYPE_RECORD) {
		Rprintf("JSON dataset with %"PRIu64" rows"
			" of the following type:\n%s\n",
			(uint64_t)d->nrow, r.string);
	} else {
		Rprintf("JSON dataset with %"PRIu64" rows"
			" of type %s\n", (uint64_t)d->nrow, r.string);
	}

	render_destroy(&r);
	return sdata;
}


SEXP subscript_dataset(SEXP sdata, SEXP si)
{
	SEXP ans, sname;
	const struct dataset *d = as_dataset(sdata);
	const struct schema *s = &d->schema;
	const struct datatype *t;
	const struct datatype_record *r;
	const struct text *name;
	double i;
	int name_id;

	if (!(isReal(si) && LENGTH(si) == 1)) {
		error("invalid 'i' argument");
	}
	i = REAL(si)[0];

	if (d->kind != DATATYPE_RECORD) {
		ans = subrows_dataset(sdata, si);
	} else {
		t = &d->schema.types[d->type_id];
		r = &t->meta.record;

		if (!(1 <= i && i <= r->nfield)) {
			error("invalid subscript: %g", i);
		}
		name_id = r->name_ids[(int)(i - 1)];
		name = &s->names.types[name_id].text;

		PROTECT(sname = allocVector(STRSXP, 1));
		SET_STRING_ELT(sname, 0,
				mkCharLenCE((const char *)name->ptr,
					    (int)TEXT_SIZE(name), CE_UTF8));
		PROTECT(ans = subfield_dataset(sdata, sname));
		UNPROTECT(2);
	}

	return ans;
}


SEXP subrows_dataset(SEXP sdata, SEXP si)
{
	SEXP ans, shandle, sfilebuf, sfield, srows, srows2;
	const struct dataset *obj = as_dataset(sdata);
	struct dataset *obj2;
	struct data *rows;
	const struct data *src;
	const double *index;
	double *irows;
	R_xlen_t i, n, ind;
	int type_id;
	int err;

	if (si == R_NilValue) {
		return sdata;
	}

	index = REAL(si);
	n = XLENGTH(si);

	sfilebuf = getListElement(sdata, "filebuf");
	sfield = getListElement(sdata, "field");
	srows = getListElement(sdata, "rows");

	PROTECT(srows2 = allocVector(REALSXP, n));
	irows = REAL(srows2);

	PROTECT(ans = alloc_dataset(sfilebuf, sfield, srows2));
	shandle = getListElement(ans, "handle");
	obj2 = R_ExternalPtrAddr(shandle);

	rows = malloc(n * sizeof(*rows));
	obj2->rows = rows;

	if (n > 0 && !rows) {
		error("failed allocating %"PRIu64" bytes",
		      (uint64_t)n * sizeof(*rows));
	}

	type_id = DATATYPE_NULL;

	for (i = 0; i < n; i++) {
		if (!(1 <= index[i] && index[i] <= (double)obj->nrow)) {
			free(rows);
			error("invalid index: %g", index[i]);
		}

		ind = (R_xlen_t)(index[i] - 1);
		if (srows == R_NilValue) {
			irows[i] = index[i];
		} else {
			irows[i] = REAL(srows)[ind];
		}
		src = &obj->rows[ind];

		// TODO: what about null?
		if ((err = data_assign(&rows[i], &obj2->schema, src->ptr,
				       src->size))) {
			error("error parsing row %"PRIu64
			      " of JSON file", (uint64_t)(irows[i] + 1));
		}

		if ((err = schema_union(&obj2->schema, type_id,
					rows[i].type_id, &type_id))) {
			error("memory allocation failure"
			      " after parsing row %"PRIu64
			      " of JSON file", (uint64_t)(irows[i] + 1));
		}

	}

	// set the fields
	obj2->nrow = n;
	obj2->type_id = type_id;

	if (type_id < 0) {
		obj2->kind = DATATYPE_ANY;
	} else {
		obj2->kind = obj2->schema.types[type_id].kind;
	}

	UNPROTECT(2);
	return ans;
}


SEXP subfield_dataset(SEXP sdata, SEXP sname)
{
	SEXP ans, sfilebuf, sfield, sfield2, shandle, srows;
	const struct dataset *obj = as_dataset(sdata);
	struct text name;
	struct data *rows;
	struct data field;
	const char *name_ptr;
	size_t name_len;
	struct dataset *obj2;
	R_xlen_t i, n;
	int err, j, m, name_id, type_id;

	if (sname == R_NilValue) {
		return sdata;
	} else if (!(isString(sname) && LENGTH(sname) == 1)) {
                error("invalid 'name' argument");
        }
	name_ptr = translateCharUTF8(STRING_ELT(sname, 0));
	name_len = strlen(name_ptr);
	PROTECT(sname = mkCharLenCE(name_ptr, name_len, CE_UTF8));
	if ((err = text_assign(&name, (uint8_t *)name_ptr, name_len,
					TEXT_NOESCAPE))) {
		error("invalid UTF-8 in 'name' argument");
	}
	if (!symtab_has_type(&obj->schema.names, &name, &name_id)) {
		UNPROTECT(1);
		return R_NilValue;
	}

	sfilebuf = getListElement(sdata, "filebuf");
	sfield = getListElement(sdata, "field");
	srows = getListElement(sdata, "rows");

	if (sfield == R_NilValue) {
		m = 0;
	} else {
		m = LENGTH(sfield);
	}

	PROTECT(sfield2 = allocVector(STRSXP, m + 1));
	for (j = 0; j < m; j++) {
		SET_STRING_ELT(sfield2, j, STRING_ELT(sfield, j));
	}
	SET_STRING_ELT(sfield2, m, sname);

	PROTECT(ans = alloc_dataset(sfilebuf, sfield2, srows));
	shandle = getListElement(ans, "handle");
	obj2 = R_ExternalPtrAddr(shandle);

	n = obj->nrow;
	rows = malloc(n * sizeof(*rows));

	if (n > 0 && !rows) {
		error("failed allocating %"PRIu64" bytes",
		      (uint64_t)n * sizeof(*rows));
	}
	obj2->rows = rows;

	type_id = DATATYPE_NULL;
	for (i = 0; i < n; i++) {
		data_field(&obj->rows[i], &obj->schema, name_id, &field);
		data_assign(&rows[i], &obj2->schema, field.ptr,
			    field.size);
		if (schema_union(&obj2->schema, type_id, rows[i].type_id,
				 &type_id) != 0) {
			error("memory allocation failure");
		}
	}

	obj2->nrow = n;
	obj2->type_id = type_id;

	if (type_id < 0) {
		obj2->kind = DATATYPE_ANY;
	} else {
		obj2->kind = obj2->schema.types[type_id].kind;
	}

	UNPROTECT(3);
	return ans;
}


SEXP subset_dataset(SEXP sdata, SEXP si, SEXP sj)
{
	SEXP ans;
	const struct dataset *d = as_dataset(sdata);

	if (si == R_NilValue) {
		if (sj == R_NilValue) {
			return sdata;
		} else {
			if (d->kind != DATATYPE_RECORD) {
				error("incorrect number of dimensions");
			}
			return subscript_dataset(sdata, sj);
		}
	} else if (sj == R_NilValue) {
		return subrows_dataset(sdata, si);
	} else {
		if (d->kind != DATATYPE_RECORD) {
			error("incorrect number of dimensions");
		}

		PROTECT(sdata = subrows_dataset(sdata, si));
		ans = subscript_dataset(sdata, sj);
		UNPROTECT(1);
		return ans;
	}
}


SEXP as_double_dataset(SEXP sdata)
{
	SEXP ans;
	const struct dataset *d = as_dataset(sdata);
	double *val;
	R_xlen_t i, n = d->nrow;
	int err, overflow;

	PROTECT(ans = allocVector(REALSXP, n));
	val = REAL(ans);
	overflow = 0;

	for (i = 0; i < n; i++) {
		err = data_double(&d->rows[i], &val[i]);
		if (err == ERROR_INVAL) {
			val[i] = NA_REAL;
		} else if (err == ERROR_OVERFLOW) {
			overflow = 1;
		}
	}

	if (overflow) {
		warning("NAs introduced by coercion to double range");
	}

	UNPROTECT(1);
	return ans;
}


static SEXP as_integer_dataset_check(SEXP sdata, int *overflowptr)
{
	SEXP ans;
	const struct dataset *d = as_dataset(sdata);
	int *val;
	R_xlen_t i, n = d->nrow;
	int err, overflow;

	PROTECT(ans = allocVector(INTSXP, n));
	val = INTEGER(ans);
	overflow = 0;

	for (i = 0; i < n; i++) {
		err = data_int(&d->rows[i], &val[i]);
		if (err == ERROR_INVAL) {
			val[i] = NA_INTEGER;
		} else {
			if (err == ERROR_OVERFLOW) {
				overflow = 1;
			}
			assert(NA_INTEGER == INT_MIN);
			if (val[i] == NA_INTEGER) {
				val[i] = NA_INTEGER + 1;
			}
		}
	}

	if (overflowptr) {
		*overflowptr = overflow;
	}

	UNPROTECT(1);
	return ans;
}


SEXP as_integer_dataset(SEXP sdata)
{
	SEXP ans;
	int overflow;

	PROTECT(ans = as_integer_dataset_check(sdata, &overflow));
	if (overflow) {
		warning("NAs introduced by coercion to integer range");
	}

	UNPROTECT(1);
	return ans;
}


SEXP as_logical_dataset(SEXP sdata)
{
	SEXP ans;
	const struct dataset *d = as_dataset(sdata);
	R_xlen_t i, n = d->nrow;
	int *val;
	int b, err;

	PROTECT(ans = allocVector(LGLSXP, n));
	val = LOGICAL(ans);

	for (i = 0; i < n; i++) {
		err = data_bool(&d->rows[i], &b);
		if (err == ERROR_INVAL) {
			val[i] = NA_LOGICAL;
		} else {
			val[i] = b ? TRUE : FALSE;
		}
	}

	UNPROTECT(1);
	return ans;
}


SEXP as_text_dataset(SEXP sdata)
{
	SEXP ans, prot;
	const struct dataset *d = as_dataset(sdata);
	struct text *text;
	R_xlen_t i, n;

	prot = R_ExternalPtrProtected(sdata);
	PROTECT(ans = alloc_text(d->nrow, prot));
	text = as_text(ans, &n);

	for (i = 0; i < n; i++) {
		if (data_text(&d->rows[i], &text[i]) != 0) {
			text[i].ptr = NULL;
			text[i].attr = 0;
		}
	}

	UNPROTECT(1);
	return ans;
}


static SEXP alloc_dataset_array(const struct schema *schema, int type_id,
				const struct data *array, SEXP prot)
{
	// TODO implement
	(void)schema;
	(void)type_id;
	(void)array;
	(void)prot;
	return R_NilValue;
	/*
	SEXP ans;
	struct data *rows;
	struct data_items it;
	int err, i, n;

	if ((err = data_nitem(array, schema, &n))
			|| ((err = data_items(array, schema, &it)))) {
		n = 0;
		rows = NULL;
	} else {
		rows = malloc(n * sizeof(*rows));
		if (!rows) {
			error("failed allocating memory (%"PRIu64" bytes)",
			      (uint64_t)n * sizeof(*rows));
		}

		i = 0;
		while (data_items_advance(&it)) {
			rows[i] = it.current;
			i++;
		}
	}

	ans = alloc_dataset(schema, type_id, rows, n, prot);
	return ans;
	*/
}


static SEXP as_list_dataset_record(SEXP sdata)
{
	// TODO implement
	(void)sdata;
	return R_NilValue;
}
/*
	SEXP ans, ans_j, prot;
	const struct dataset *d = as_dataset(sdata);
	const struct schema *s = &d->schema;
	const struct datatype_record *r;
	struct data_fields it;
	R_xlen_t i, n = d->nrow;
	int err, j, nfield;
	struct data **rows;
	int *cols;

	if (d->kind != DATATYPE_RECORD) {
		return R_NilValue;
	}

	r = &s->types[d->type_id].meta.record;
	nfield = r->nfield;

	prot = R_ExternalPtrProtected(sdata);
	PROTECT(ans = allocVector(VECSXP, r->nfield));
	setAttrib(ans, R_NamesSymbol, names_dataset(sdata));
	rows = (struct data **)R_alloc(nfield, sizeof(*rows));
	cols = (int *)R_alloc(s->names.ntype, sizeof(*cols));

	for (j = 0; j < nfield; j++) {
		// use calloc so that all items are initialized to null
		rows[j] = calloc(n, sizeof(*rows[j]));
		if (!rows[j] && n) {
			error("failed allocating memory (%"PRIu64" bytes)",
			      (uint64_t)n * sizeof(*rows[j]));
		}
		cols[r->name_ids[j]] = j;

		ans_j = alloc_dataset(s, r->type_ids[j], rows[j], n, prot);
		SET_VECTOR_ELT(ans, j, ans_j);
	}

	for (i = 0; i < n; i++) {
		if ((err = data_fields(&d->rows[i], s, &it))) {
			continue;
		}

		while (data_fields_advance(&it)) {
			j = cols[it.name_id];
			rows[j][i] = it.current;
		}
	}

	for (j = 0; j < nfield; j++) {
		ans_j = VECTOR_ELT(ans, j);
		ans_j = simplify_dataset(ans_j);
		SET_VECTOR_ELT(ans, j, ans_j);
	}

	UNPROTECT(1);
	return ans;
}
*/


SEXP as_list_dataset(SEXP sdata)
{
	SEXP ans, prot, val;
	const struct dataset *d = as_dataset(sdata);
	const struct schema *s = &d->schema;
	R_xlen_t i, n = d->nrow;
	int type_id;

	if (d->kind == DATATYPE_RECORD) {
		return as_list_dataset_record(sdata);
	} else if (d->kind != DATATYPE_ARRAY) {
		return R_NilValue;
	}

	type_id = s->types[d->type_id].meta.array.type_id;

	prot = R_ExternalPtrProtected(sdata);
	PROTECT(ans = allocVector(VECSXP, n));

	for (i = 0; i < n; i++) {
		val = alloc_dataset_array(s, type_id, &d->rows[i], prot);
		SET_VECTOR_ELT(ans, i, val);
	}

	UNPROTECT(1);
	return ans;
}


SEXP simplify_dataset(SEXP sdata)
{
	SEXP ans;
	const struct dataset *d = as_dataset(sdata);
	int overflow;

	switch (d->kind) {
	case DATATYPE_NULL:
	case DATATYPE_BOOLEAN:
		ans = as_logical_dataset(sdata);
		break;

	case DATATYPE_INTEGER:
		ans = as_integer_dataset_check(sdata, &overflow);
		if (overflow) {
			ans = as_double_dataset(sdata);
		}
		break;

	case DATATYPE_REAL:
		ans = as_double_dataset(sdata);
		break;

	case DATATYPE_TEXT:
		ans = as_text_dataset(sdata);
		break;

	case DATATYPE_ARRAY:
		ans = as_list_dataset(sdata);
		break;

	default:
		ans = sdata;
		break;
	}

	return ans;
}
