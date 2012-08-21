/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2012 MonetDB B.V.
 * All Rights Reserved.
 */


#include "monetdb_config.h"
#include <math.h>
#include "rel_trans.h"
#include "rel_select.h"
#include "rel_updates.h"
#include "rel_exp.h"
#include "rel_schema.h"
#include "sql_parser.h"
#include "sql_privileges.h"

#define qname_index(qname) qname_table(qname)
#define qname_func(qname) qname_table(qname)

static sql_table *
_bind_table(sql_table *t, sql_schema *ss, sql_schema *s, char *name)
{
	sql_table *tt = NULL;

	if (t && strcmp(t->base.name, name) == 0)
		tt = t;
	if (!tt && ss) 
		tt = find_sql_table(ss, name);
	if (!tt && s) 
		tt = find_sql_table(s, name);
	return tt;
}

static sql_rel *
rel_table(mvc *sql, int cat_type, char *sname, sql_table *t, int nr)
{
	sql_rel *rel = rel_create(sql->sa);
	list *exps = new_exp_list(sql->sa);

	append(exps, exp_atom_int(sql->sa, nr));
	append(exps, exp_atom_str(sql->sa, sname, sql_bind_localtype("str") ));
	if (t)
		append(exps, exp_atom_ptr(sql->sa, t));
	rel->l = rel_basetable(sql, t, t->base.name);
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = cat_type;
	rel->exps = exps;
	rel->card = CARD_MULTI;
	rel->nrcols = 0;
	return rel;
}

sql_rel *
rel_list(sql_allocator *sa, sql_rel *l, sql_rel *r) 
{
	sql_rel *rel = rel_create(sa);

	if (!l)
		return r;
	rel->l = l;
	rel->r = r;
	rel->op = op_ddl;
	rel->flag = DDL_LIST;
	return rel;
}

static sql_rel *
view_rename_columns( mvc *sql, char *name, sql_rel *sq, dlist *column_spec)
{
	dnode *n = column_spec->h;
	node *m = sq->exps->h;
	list *l = new_exp_list(sql->sa);

	for (; n && m; n = n->next, m = m->next) {
		char *cname = n->data.sval;
		sql_exp *e = m->data;
		sql_exp *n;
	       
		if (!exp_is_atom(e) && !e->name)
			exp_setname(sql->sa, e, NULL, cname);
		n = exp_is_atom(e)?e:exp_column(sql->sa, exp_relname(e), e->name, exp_subtype(e), sq->card, has_nil(e), is_intern(e), e->type == e_column?e->f:NULL);

		exp_setname(sql->sa, n, NULL, cname);
		list_append(l, n);
	}
	/* skip any intern columns */
	for (; m; m = m->next) {
		sql_exp *e = m->data;
		if (!is_intern(e))
			break;
	}
	if (n || m) 
		return sql_error(sql, 02, "M0M03!Column lists do not match");
	(void)name;
	sq = rel_project(sql->sa, sq, l);
	set_processed(sq);
	return sq;
}

static int
as_subquery( mvc *sql, sql_table *t, sql_rel *sq, dlist *column_spec, const char *msg )
{
        sql_rel *r = sq;

	if (!r)
		return 0;

        if (is_topn(r->op) || is_sample(r->op))
                r = sq->l;

	if (column_spec) {
		dnode *n = column_spec->h;
		node *m = r->exps->h;

		for (; n && m; n = n->next, m = m->next) {
			char *cname = n->data.sval;
			sql_exp *e = m->data;
			sql_subtype *tp = exp_subtype(e);

			if (mvc_bind_column(sql, t, cname)) {
				sql_error(sql, 01, "42S21!%s: duplicate column name %s", msg, cname);
				return -1;
			}
			mvc_create_column(sql, t, cname, tp);
		}
		if (n || m) {
			sql_error(sql, 01, "21S02!%s: number of columns does not match", msg);
			return -1;
		}
	} else {
		node *m;

		for (m = r->exps->h; m; m = m->next) {
			sql_exp *e = m->data;
			char *cname = exp_name(e);
			sql_subtype *tp = exp_subtype(e);

			if (!cname)
				cname = "v";
			if (mvc_bind_column(sql, t, cname)) {
				sql_error(sql, 01, "42S21!%s: duplicate column name %s", msg, cname);
				return -1;
			}
			mvc_create_column(sql, t, cname, tp);
		}
	}
	return 0;
}

sql_table *
mvc_create_table_as_subquery( mvc *sql, sql_rel *sq, sql_schema *s, char *tname, dlist *column_spec, int temp, int commit_action )
{
	int tt =(temp == SQL_REMOTE)?tt_remote:
		(temp == SQL_STREAM)?tt_stream:
	        (temp == SQL_MERGE_TABLE)?tt_merge_table:
	        (temp == SQL_REPLICA_TABLE)?tt_replica_table:tt_table;

	sql_table *t = mvc_create_table(sql, s, tname, tt, 0, SQL_DECLARED_TABLE, commit_action, -1);
	if (as_subquery( sql, t, sq, column_spec, "CREATE TABLE") != 0)

		return NULL;
	return t;
}

static char *
table_constraint_name(symbol *s, sql_table *t)
{
	/* create a descriptive name like table_col_pkey */
	char *suffix;		/* stores the type of this constraint */
	dnode *nms = NULL;
	char *buf;
	size_t buflen;
	size_t len;
	size_t slen;

	switch (s->token) {
		case SQL_UNIQUE:
			suffix = "_unique";
			nms = s->data.lval->h;	/* list of columns */
			break;
		case SQL_PRIMARY_KEY:
			suffix = "_pkey";
			nms = s->data.lval->h;	/* list of columns */
			break;
		case SQL_FOREIGN_KEY:
			suffix = "_fkey";
			nms = s->data.lval->h->next->data.lval->h;	/* list of colums */
			break;
		default:
			suffix = "_?";
			nms = NULL;
	}

	/* copy table name */
	len = strlen(t->base.name);
	buflen = BUFSIZ;
	slen = strlen(suffix);
	while (len + slen >= buflen)
		buflen += BUFSIZ;
	buf = malloc(buflen);
	strcpy(buf, t->base.name);

	/* add column name(s) */
	for (; nms; nms = nms->next) {
		slen = strlen(nms->data.sval);
		while (len + slen + 1 >= buflen) {
			buflen += BUFSIZ;
			buf = realloc(buf, buflen);
		}
		snprintf(buf + len, buflen - len, "_%s", nms->data.sval);
		len += slen + 1;
	}

	/* add suffix */
	slen = strlen(suffix);
	while (len + slen >= buflen) {
		buflen += BUFSIZ;
		buf = realloc(buf, buflen);
	}
	snprintf(buf + len, buflen - len, "%s", suffix);

	return buf;
}

static char *
column_constraint_name(symbol *s, sql_column *sc, sql_table *t)
{
	/* create a descriptive name like table_col_pkey */
	char *suffix;		/* stores the type of this constraint */
	static char buf[BUFSIZ];

	switch (s->token) {
		case SQL_UNIQUE:
			suffix = "unique";
			break;
		case SQL_PRIMARY_KEY:
			suffix = "pkey";
			break;
		case SQL_FOREIGN_KEY:
			suffix = "fkey";
			break;
		default:
			suffix = "?";
	}

	snprintf(buf, BUFSIZ, "%s_%s_%s", t->base.name, sc->base.name, suffix);

	return buf;
}

static int
column_constraint_type(mvc *sql, char *name, symbol *s, sql_schema *ss, sql_table *t, sql_column *cs)
{
	int res = SQL_ERR;

	switch (s->token) {
	case SQL_UNIQUE:
	case SQL_PRIMARY_KEY: {
		key_type kt = (s->token == SQL_UNIQUE) ? ukey : pkey;
		sql_key *k;

		if (kt == pkey && t->pkey) {
			(void) sql_error(sql, 02, "42000!CONSTRAINT PRIMARY KEY: a table can have only one PRIMARY KEY\n");
			return res;
		}
		if (name && mvc_bind_key(sql, ss, name)) {
			(void) sql_error(sql, 02, "42000!CONSTRAINT PRIMARY KEY: key %s already exists", name);
			return res;
		}
		k = (sql_key*)mvc_create_ukey(sql, t, name, kt);

		mvc_create_kc(sql, k, cs);
		mvc_create_ukey_done(sql, k);
		res = SQL_OK;
	} 	break;
	case SQL_FOREIGN_KEY: {
		dnode *n = s->data.lval->h;
		char *rtname = qname_table(n->data.lval);
		int ref_actions = n->next->next->next->data.i_val; 
		sql_table *rt;
		sql_fkey *fk;
		list *cols;
		sql_key *rk = NULL;

		assert(n->next->next->next->type == type_int);
/*
		if (isTempTable(t)) {
			(void) sql_error(sql, 02, "42000!CONSTRAINT: constraints on temporary tables are not supported\n");
			return res;
		}
*/
		rt = _bind_table(t, ss, cur_schema(sql), rtname);
		if (!rt) {
			(void) sql_error(sql, 02, "42S02!CONSTRAINT FOREIGN KEY: no such table '%s'\n", rtname);
			return res;
		}
		if (name && mvc_bind_key(sql, ss, name)) {
			(void) sql_error(sql, 02, "42000!CONSTRAINT FOREIGN KEY: key '%s' already exists", name);
			return res;
		}

		/* find unique referenced key */
		if (n->next->data.lval) {	
			char *rcname = n->next->data.lval->h->data.sval;

			cols = list_append(sa_list(sql->sa), rcname);
			rk = mvc_bind_ukey(rt, cols);
		} else if (rt->pkey) {
			/* no columns specified use rt.pkey */
			rk = &rt->pkey->k;
		}
		if (!rk) {
			(void) sql_error(sql, 02, "42000!CONSTRAINT FOREIGN KEY: could not find referenced PRIMARY KEY in table %s\n", rtname);
			return res;
		}
		fk = mvc_create_fkey(sql, t, name, fkey, rk, ref_actions & 255, (ref_actions>>8) & 255);
		mvc_create_fkc(sql, fk, cs);
		res = SQL_OK;
	} 	break;
	case SQL_NOT_NULL:
	case SQL_NULL: {
		int null = (s->token == SQL_NOT_NULL) ? 0 : 1;

		mvc_null(sql, cs, null);
		res = SQL_OK;
	} 	break;
	}
	if (res == SQL_ERR) {
		(void) sql_error(sql, 02, "M0M03!unknown constraint (" PTRFMT ")->token = %s\n", PTRFMTCAST s, token2string(s->token));
	}
	return res;
}

static int
column_option(
		mvc *sql,
		symbol *s,
		sql_schema *ss,
		sql_table *t,
		sql_column *cs)
{
	int res = SQL_ERR;

	assert(cs);
	switch (s->token) {
	case SQL_CONSTRAINT: {
		dlist *l = s->data.lval;
		char *opt_name = l->h->data.sval;
		symbol *sym = l->h->next->data.sym;

		if (!sym) /* For now we only parse CHECK Constraints */
			return SQL_OK;
		if (!opt_name)
			opt_name = column_constraint_name(sym, cs, t);
		res = column_constraint_type(sql, opt_name, sym, ss, t, cs);
	} 	break;
	case SQL_DEFAULT: {
		char *err = NULL, *r = symbol2string(sql, s->data.sym, &err);

		if (!r) {
			(void) sql_error(sql, 02, "42000!incorrect default value '%s'\n", err?err:"");
			if (err) _DELETE(err);
			return SQL_ERR;
		} else {
			mvc_default(sql, cs, r);
			_DELETE(r);
			res = SQL_OK;
		}
	} 	break;
	case SQL_ATOM: {
		AtomNode *an = (AtomNode *) s;

		if (!an || !an->a) {
			mvc_default(sql, cs, NULL);
		} else {
			atom *a = an->a;

			if (a->data.vtype == TYPE_str) {
				mvc_default(sql, cs, a->data.val.sval);
			} else {
				char *r = atom2string(sql->sa, a);

				mvc_default(sql, cs, r);
			}
		}
		res = SQL_OK;
	} 	break;
	case SQL_NOT_NULL:
	case SQL_NULL: {
		int null = (s->token == SQL_NOT_NULL) ? 0 : 1;

		mvc_null(sql, cs, null);
		res = SQL_OK;
	} 	break;
	}
	if (res == SQL_ERR) {
		(void) sql_error(sql, 02, "M0M03!unknown column option (" PTRFMT ")->token = %s\n", PTRFMTCAST s, token2string(s->token));
	}
	return res;
}

static int
column_options(mvc *sql, dlist *opt_list, sql_schema *ss, sql_table *t, sql_column *cs)
{
	assert(cs);

	if (opt_list) {
		dnode *n = NULL;

		for (n = opt_list->h; n; n = n->next) {
			int res = column_option(sql, n->data.sym, ss, t, cs);

			if (res == SQL_ERR)
				return SQL_ERR;
		}
	}
	return SQL_OK;
}

static int 
table_foreign_key(mvc *sql, char *name, symbol *s, sql_schema *ss, sql_table *t)
{
	dnode *n = s->data.lval->h;
	char *rtname = qname_table(n->data.lval);
	sql_table *ft = mvc_bind_table(sql, ss, rtname);

	if (!ft) {
		sql_error(sql, 02, "42S02!CONSTRAINT FOREIGN KEY: no such table '%s'\n", rtname);
		return SQL_ERR;
	} else {
		sql_key *rk = NULL;
		sql_fkey *fk;
		dnode *nms = n->next->data.lval->h;
		node *fnms;
		int ref_actions = n->next->next->next->next->data.i_val;

		assert(n->next->next->next->next->type == type_int);
		if (name && mvc_bind_key(sql, ss, name)) {
			sql_error(sql, 02, "42000!Create Key failed, key '%s' already exists", name);
			return SQL_ERR;
		}
		if (n->next->next->data.lval) {	/* find unique referenced key */
			dnode *rnms = n->next->next->data.lval->h;
			list *cols = sa_list(sql->sa);

			for (; rnms; rnms = rnms->next)
				list_append(cols, rnms->data.sval);

			/* find key in ft->keys */
			rk = mvc_bind_ukey(ft, cols);
		} else if (ft->pkey) {	
			/* no columns specified use ft.pkey */
			rk = &ft->pkey->k;
		}
		if (!rk) {
			sql_error(sql, 02, "42000!CONSTRAINT FOREIGN KEY: could not find referenced PRIMARY KEY in table '%s'\n", ft->base.name);
			return SQL_ERR;
		}
		fk = mvc_create_fkey(sql, t, name, fkey, rk, ref_actions & 255, (ref_actions>>8) & 255);

		for (fnms = rk->columns->h; nms && fnms; nms = nms->next, fnms = fnms->next) {
			char *nm = nms->data.sval;
			sql_column *c = mvc_bind_column(sql, t, nm);

			if (!c) {
				sql_error(sql, 02, "42S22!CONSTRAINT FOREIGN KEY: no such column '%s' in table '%s'\n", nm, t->base.name);
				return SQL_ERR;
			}
			mvc_create_fkc(sql, fk, c);
		}
		if (nms || fnms) {
			sql_error(sql, 02, "42000!CONSTRAINT FOREIGN KEY: not all columns are handled\n");
			return SQL_ERR;
		}
	}
	return SQL_OK;
}

static int 
table_constraint_type(mvc *sql, char *name, symbol *s, sql_schema *ss, sql_table *t)
{
	int res = SQL_OK;

	switch (s->token) {
	case SQL_UNIQUE:
	case SQL_PRIMARY_KEY: {
		key_type kt = (s->token == SQL_PRIMARY_KEY ? pkey : ukey);
		dnode *nms = s->data.lval->h;
		sql_key *k;

		if (kt == pkey && t->pkey) {
			sql_error(sql, 02, "42000!CONSTRAINT PRIMARY KEY: a table can have only one PRIMARY KEY\n");
			return SQL_ERR;
		}
		if (name && mvc_bind_key(sql, ss, name)) {
			sql_error(sql, 02, "42000!CONSTRAINT %s: key '%s' already exists",
					kt == pkey ? "PRIMARY KEY" : "UNIQUE", name);
			return SQL_ERR;
		}
			
 		k = (sql_key*)mvc_create_ukey(sql, t, name, kt);
		for (; nms; nms = nms->next) {
			char *nm = nms->data.sval;
			sql_column *c = mvc_bind_column(sql, t, nm);

			if (!c) {
				sql_error(sql, 02, "42S22!CONSTRAINT %s: no such column '%s' for table '%s'",
						kt == pkey ? "PRIMARY KEY" : "UNIQUE",
						nm, t->base.name);
				return SQL_ERR;
			} 
			(void) mvc_create_kc(sql, k, c);
		}
		mvc_create_ukey_done(sql, k);
	} 	break;
	case SQL_FOREIGN_KEY:
		res = table_foreign_key(sql, name, s, ss, t);
		break;
	}
	if (!res) {
		sql_error(sql, 02, "M0M03!table constraint type: wrong token (" PTRFMT ") = %s\n", PTRFMTCAST s, token2string(s->token));
		return SQL_ERR;
	}
	return res;
}

static int 
table_constraint(mvc *sql, symbol *s, sql_schema *ss, sql_table *t)
{
	int res = SQL_OK;

	if (s->token == SQL_CONSTRAINT) {
		dlist *l = s->data.lval;
		char *opt_name = l->h->data.sval;
		symbol *sym = l->h->next->data.sym;

		if (!opt_name)
			opt_name = table_constraint_name(sym, t);
		res = table_constraint_type(sql, opt_name, sym, ss, t);
		if (opt_name != l->h->data.sval)
			free(opt_name);
	}

	if (!res) {
		sql_error(sql, 02, "M0M03!table constraint: wrong token (" PTRFMT ") = %s\n", PTRFMTCAST s, token2string(s->token));
		return SQL_ERR;
	}
	return res;
}

/**
 * Get the string value of a dimension constraint 'dimcstr' out of the parse
 * tree 'lst'.  Also check if the data type of this dimension constraint
 * conforms the data type of the column 'ctype'.
 *
 * TODO: deal with TIMESTAMP dimensions differently, since the 'step' is an
 * interval.
 */
static int
get_dim_constraints(mvc *sql, sql_subtype *ctype, dlist *lst, char **dimcstr, int isStep)
{
	sql_exp *exp = NULL;
	atom *a = NULL;

	assert(lst->h->type == type_string || lst->h->type == type_symbol);

	if(lst->h->type == type_symbol && !lst->h->data.sym) { /* case: [*] */
		*dimcstr = GDKstrdup("");
		return SQL_OK;
	}

	if (isStep && (strcmp(ctype->type->base.name, "str") == 0 || strcmp(ctype->type->base.name, "date") == 0 || strcmp(ctype->type->base.name, "daytime") == 0 || strcmp(ctype->type->base.name, "timestamp") == 0 ))
	{
		/* TODO: for these type of dimensions, their step size should always be an int-typed value */
		sql_error(sql, 02, "CREATE ARRAY: dimension type \"%s\" unsupported yet", ctype->type->sqlname);
		return SQL_ERR;
	}

	if (lst->h->type == type_string) { /* handle negative (numerical) value */
		a = ((AtomNode *) lst->h->next->data.sym)->a;
		atom_neg(a);
	} else { /* handle non-negative value */
		a = ((AtomNode *) lst->h->data.sym)->a;
	}
	exp = exp_atom(sql->sa, a);
	if (!(exp = rel_check_type(sql, ctype, exp, type_equal)))
		return SQL_ERR;
	/* TODO: do we want to convert the atom? */
	*dimcstr = atom2string(sql->sa, a);
	return SQL_OK;
}


/* checks if 'tpe' is one of the SQL int types, i.e., TINYINT, SMALLINT, INT or BIGINT */
#define isAnIntType(tpe, len) (tpe[len-3] == 'i' && tpe[len-2] == 'n' && tpe[len-1] == 't')

static int
create_column(mvc *sql, symbol *s, sql_schema *ss, sql_table *t, int alter)
{
	dlist *l = s->data.lval;
	dlist *dim = NULL;
	char *cname = l->h->data.sval, *tname = NULL;
	sql_subtype *ctype = &l->h->next->data.typeval;
	dlist *opt_list = NULL;
	int res = SQL_OK;

(void)ss;
	if (alter && !isTableOrArray(t)) {
		sql_error(sql, 02, "42000!ALTER %s: cannot add column to VIEW '%s'\n", isTable(t)?"TABLE":(isArray(t)?"ARRAY":"TABLE/ARRAY"), t->base.name);
		return SQL_ERR;
	}
	if (l->h->next->next)
		opt_list = l->h->next->next->data.lval;

	if (cname && ctype) {
		sql_column *cs = NULL;

		cs = find_sql_column(t, cname);
		if (cs) {
			sql_error(sql, 02, "42S21!%s TABLE: a column named '%s' already exists\n", (alter)?"ALTER":"CREATE", cname);
			return SQL_ERR;
		}

		cs = mvc_create_column(sql, t, cname, ctype);
		if (l->h->next->next->next) {
			/* this is a dimension, see its structure in parser.y/column_def */
			assert(l->h->next->next->next->type == type_symbol && l->h->next->next->next->data.sym->token == SQL_DIMENSION);
			if (!isArray(t)){
				sql_error(sql, 02, "%s %s: dimension column '%s' used in non-ARRAY\n", (alter)?"ALTER":"CREATE", (t->type==tt_table)?"TABLE":"OTHER_TT", cname);
				return SQL_ERR;
			}

			t->ndims++;

			/* TODO: check if this is the correct place where the NULL
			 * dim_range list of the "DIMENSION" case is denoted */
			dim = l->h->next->next->next->data.sym->data.lval;
			if (dim && !dim->h->next) { /* "DIMENSION dim_range" case */
				dim = dim->h->data.lval; /* here starts the actual dimension constraints */

				cs->dim = ZNEW(sql_dimspec);
				switch (dim->cnt) {
					case 1: {/* [size], [-size], [seqname] */
						if(dim->h->type == type_string) { /* TODO: implementation: look up the constraints of the [seq] */
							sql_error(sql, 02, "%s ARRAY: dimension column '%s' uses sequence \"%s\" as constraint, not implemented yet\n", (alter)?"ALTER":"CREATE", cname, dim->h->data.sval);
							return SQL_ERR;
						}

						/* In cases [size] or [-size], the column's data type MUST be INT */
						tname = ctype->type->sqlname;
						if(!isAnIntType(tname, strlen(tname))) {
							sql_error(sql, 02, "%s ARRAY: syntax shortcut '[size]' only allowed for int typed dimensions, dimension column \"%s\" has type \"%s\"\n", (alter)?"ALTER":"CREATE", cname, tname);
							return SQL_ERR;
						}

						assert(dim->h->type == type_list);
						if (dim->h->data.lval->h->type == type_symbol){
							if (dim->h->data.lval->h->data.sym) { 		/* the case: [size] */
								tname = ((AtomNode*)dim->h->data.lval->h->data.sym)->a->tpe.type->sqlname;
								if(!isAnIntType(tname, strlen(tname))) {
									sql_error(sql, 02, "%s ARRAY: constraints of dimension column '%s' has invalid data type: expect int type, got \"%s\"\n", (alter)?"ALTER":"CREATE", cname, tname);
									return SQL_ERR;
								}

								cs->dim->start = GDKstrdup("0");
								cs->dim->step = GDKstrdup("1");
								cs->dim->stop = atom2string(sql->sa, ((AtomNode*)dim->h->data.lval->h->data.sym)->a);
							} else {									/* the case: [*] */
								cs->dim->start = GDKstrdup("");
								cs->dim->step = GDKstrdup("");
								cs->dim->stop = GDKstrdup("");
							}
						} else { 										/* the case: [-size] */
							assert(dim->h->data.lval->h->type == type_string && strcmp(dim->h->data.lval->h->data.sval, "sql_neg")==0);

							tname = ((AtomNode*)dim->h->data.lval->h->next->data.sym)->a->tpe.type->sqlname;
							if(!isAnIntType(tname, strlen(tname))) {
								sql_error(sql, 02, "%s ARRAY: constraints of dimension column '%s' has invalid data type: expect int type, got \"%s\"\n", (alter)?"ALTER":"CREATE", cname, tname);
								return SQL_ERR;
							}

							cs->dim->start = GDKstrdup("0");
							cs->dim->step = GDKstrdup("-1");
							atom_neg( ((AtomNode *) dim->h->data.lval->h->next->data.sym)->a );
							cs->dim->stop = atom2string(sql->sa, ((AtomNode*)dim->h->data.lval->h->next->data.sym)->a);
						}
					} break;
					case 2: /* [start:stop] */
						if((res = get_dim_constraints(sql, ctype, dim->h->data.lval, &cs->dim->start, 0)) != SQL_OK)
							return res;
						if((res = get_dim_constraints(sql, ctype, dim->h->next->data.lval, &cs->dim->stop, 0)) != SQL_OK)
							return res;
						tname = ctype->type->sqlname;
						/* For int-typed dimensions, we allow [start:stop] to be the shortcut of [start:1:stop] */
						cs->dim->step = isAnIntType(tname, strlen(tname)) ? GDKstrdup("1") : GDKstrdup("");
						break;
					case 3: /* [start:step:stop] */
						if((res = get_dim_constraints(sql, ctype, dim->h->data.lval, &cs->dim->start, 0)) != SQL_OK)
							return res;
						if((res = get_dim_constraints(sql, ctype, dim->h->next->data.lval, &cs->dim->step, 1)) != SQL_OK)
							return res;
						if((res = get_dim_constraints(sql, ctype, dim->h->next->next->data.lval, &cs->dim->stop, 0)) != SQL_OK)
							return res;
						break;
					default:
						sql_error(sql, 02, "%s ARRAY: dimension '%s' has wrong number of range constraints %d\n", (alter)?"ALTER":"CREATE", cname, dim->cnt);
						return SQL_ERR;
				}
			} else if (dim && dim->h->next) { /* TODO: the case "ARRAY dim_range_list" is not dealt with */
				sql_error(sql, 02, "%s ARRAY: dimension '%s' constraint with syntax 'ARRAY dim_range_list' not implemented yet\n", (alter)?"ALTER":"CREATE", cname);
				return SQL_ERR;
			} else { /* "DIMENSION" case: only allocate space for empty [start:step:stop] */
				cs->dim = ZNEW(sql_dimspec);
				cs->dim->start = GDKstrdup("");
				cs->dim->step = GDKstrdup("");
				cs->dim->stop = GDKstrdup("");
			}
			if (!(isFixedDim(cs->dim)))
				t->fixed = 0;
		}
		if (column_options(sql, opt_list, ss, t, cs) == SQL_ERR)
			return SQL_ERR;
	}

	if (res == SQL_ERR) 
		sql_error(sql, 02, "42000!CREATE: column type or name");
	return res;
}

static int 
table_element(mvc *sql, symbol *s, sql_schema *ss, sql_table *t, int alter)
{
	int res = SQL_OK;

	if (alter && (isView(t) || ((isMergeTable(t) || isReplicaTable(t)) && s->token != SQL_TABLE && s->token != SQL_DROP_TABLE) || (isTable(t) && (s->token == SQL_TABLE || s->token == SQL_DROP_TABLE)) || (isArray(t) && (s->token == SQL_ARRAY || s->token == SQL_DROP_ARRAY))
				)){
		char *msg = "";

		switch (s->token) {
		case SQL_TABLE: 	
			msg = "add table to"; 
			break;
		case SQL_COLUMN: 	
			msg = "add column to"; 
			break;
		case SQL_DIMENSION:
			msg = "add dimension to";
			break;
		case SQL_CONSTRAINT: 	
			msg = "add constraint to"; 
			break;
		case SQL_COLUMN_OPTIONS:
		case SQL_DEFAULT:
		case SQL_NOT_NULL:
		case SQL_NULL:
			msg = "set column options for"; 
			break;
		case SQL_DROP_DEFAULT:
			msg = "drop default column option from"; 
			break;
		case SQL_DROP_TABLE:
			msg = "drop table from"; 
			break;
		case SQL_DROP_ARRAY:
			msg = "drop array from"; 
			break;
		case SQL_DROP_COLUMN:
			msg = "drop column from"; 
			break;
		case SQL_DROP_CONSTRAINT:
			msg = "drop constraint from"; 
			break;
		}
		sql_error(sql, 02, "42000!ALTER %s: cannot %s %s '%s'\n",
				isTable(t)?"TABLE":(isArray(t)?"ARRAY":"TABLE/ARRAY"),
				msg, 
				isMergeTable(t)?"MERGE TABLE":
				isReplicaTable(t)?"REPLICA TABLE":"VIEW",
				t->base.name);
		return SQL_ERR;
	}

	switch (s->token) {
	case SQL_COLUMN:
		res = create_column(sql, s, ss, t, alter);
		break;
	case SQL_CONSTRAINT:
		res = table_constraint(sql, s, ss, t);
		break;
	case SQL_COLUMN_OPTIONS:
	{
		dnode *n = s->data.lval->h;
		char *cname = n->data.sval;
		sql_column *c = mvc_bind_column(sql, t, cname);
		dlist *olist = n->next->data.lval;

		if (!c) {
			sql_error(sql, 02, "42S22!ALTER TABLE: no such column '%s'\n", cname);
			return SQL_ERR;
		} else {
			return column_options(sql, olist, ss, t, c);
		}
	} 	break;
	case SQL_DEFAULT:
	{
		char *r, *err = NULL;
		dlist *l = s->data.lval;
		char *cname = l->h->data.sval;
		symbol *sym = l->h->next->data.sym;
		sql_column *c = mvc_bind_column(sql, t, cname);

		if (!c) {
			sql_error(sql, 02, "42S22!ALTER TABLE: no such column '%s'\n", cname);
			return SQL_ERR;
		}
		r = symbol2string(sql, sym, &err);
		if (!r) {
			(void) sql_error(sql, 02, "42000!incorrect default value '%s'\n", err?err:"");
			if (err) _DELETE(err);
			return SQL_ERR;
		}
		mvc_default(sql, c, r);
		_DELETE(r);
	}
	break;
	case SQL_NOT_NULL:
	case SQL_NULL:
	{
		dnode *n = s->data.lval->h;
		char *cname = n->data.sval;
		sql_column *c = mvc_bind_column(sql, t, cname);
		int null = (s->token == SQL_NOT_NULL) ? 0 : 1;

		if (!c) {
			sql_error(sql, 02, "42S22!ALTER TABLE: no such column '%s'\n", cname);
			return SQL_ERR;
		}
		mvc_null(sql, c, null);
	} 	break;
	case SQL_DROP_DEFAULT:
	{
		char *cname = s->data.sval;
		sql_column *c = mvc_bind_column(sql, t, cname);
		if (!c) {
			sql_error(sql, 02, "42S22!ALTER TABLE: no such column '%s'\n", cname);
			return SQL_ERR;
		}
		mvc_drop_default(sql,c);
	} 	break;
	case SQL_LIKE:
	{
		char *sname = qname_schema(s->data.lval);
		char *name = qname_table(s->data.lval);
		sql_schema *os = NULL;
		sql_table *ot = NULL;
		node *n;

		if (sname && !(os = mvc_bind_schema(sql, sname))) {
			sql_error(sql, 02, "3F000!CREATE TABLE: no such schema '%s'", sname);
			return SQL_ERR;
		}
		if (!os)
			os = ss;
	       	ot = mvc_bind_table(sql, os, name);
		if (!ot)
			return SQL_ERR;
		for (n = ot->columns.set->h; n; n = n->next) {
			sql_column *oc = n->data;

			(void)mvc_create_column(sql, t, oc->base.name, &oc->type);
		}
	} 	break;
	case SQL_DROP_COLUMN:
	{
		dlist *l = s->data.lval;
		char *cname = l->h->data.sval;
		int drop_action = l->h->next->data.i_val;
		sql_column *col = mvc_bind_column(sql, t, cname);

		assert(l->h->next->type == type_int);
		if (col == NULL) {
			sql_error(sql, 02, "42S22!ALTER TABLE: no such column '%s'\n", cname);
			return SQL_ERR;
		}
		if (cs_size(&t->columns) <= 1) {
			sql_error(sql, 02, "42000!ALTER TABLE: cannot drop column '%s': table needs at least one column\n", cname);
			return SQL_ERR;
		}
		if (t->system) {
			sql_error(sql, 02, "42000!ALTER TABLE: cannot drop column '%s': table is a system table\n", cname);
			return SQL_ERR;
		}
		if (isView(t)) {
			sql_error(sql, 02, "42000!ALTER TABLE: cannot drop column '%s': '%s' is a view\n", cname, t->base.name);
			return SQL_ERR;
		}
		if (!drop_action && mvc_check_dependency(sql, col->base.id, COLUMN_DEPENDENCY, NULL)) {
			sql_error(sql, 02, "2BM37!ALTER TABLE: cannot drop column '%s': there are database objects which depend on it\n", cname);
			return SQL_ERR;
		}
		if (!drop_action  && t->keys.set) {
			node *n, *m;

			for (n = t->keys.set->h; n; n = n->next) {
				sql_key *k = n->data;
				for (m = k->columns->h; m; m = m->next) {
					sql_kc *kc = m->data;
					if (strcmp(kc->c->base.name, cname) == 0) {
						sql_error(sql, 02, "2BM37!ALTER TABLE: cannot drop column '%s': there are constraints which depend on it\n", cname);
						return SQL_ERR;
					}
				}
			}
		}
		mvc_drop_column(sql, t, col, drop_action);
	} 	break;
	case SQL_DROP_CONSTRAINT:
		assert(0);
	}
	if (res == SQL_ERR) {
		sql_error(sql, 02, "M0M03!unknown table element (" PTRFMT ")->token = %s\n", PTRFMTCAST s, token2string(s->token));
		return SQL_ERR;
	}
	return res;
}

sql_rel *
rel_create_table(mvc *sql, sql_schema *ss, int temp, char *sname, char *name, symbol *table_elements_or_subquery, int commit_action, char *loc)
{
	sql_schema *s = NULL;

	int instantiate = (sql->emode == m_instantiate);
	int deps = (sql->emode == m_deps);
	int create = (!instantiate && !deps);
	int tt = 0;

	(void)create;

	switch (temp) {
	case SQL_PERSIST:
		tt = (table_elements_or_subquery->token == SQL_CREATE_ARRAY) ? tt_array : tt_table;
		break;
	case SQL_LOCAL_TEMP:
		tt = (table_elements_or_subquery->token == SQL_CREATE_ARRAY) ? tt_array : tt_table;
		break;
	case SQL_GLOBAL_TEMP:
		tt = (table_elements_or_subquery->token == SQL_CREATE_ARRAY) ? tt_array : tt_table;
		break;
	case SQL_DECLARED_TABLE:
		tt = tt_table;
		break;
	case SQL_DECLARED_ARRAY:
		tt = tt_array;
		break;
	case SQL_MERGE_TABLE:
		tt = tt_merge_table;
		break;
	case SQL_STREAM:
		tt = tt_stream;
		break;
	case SQL_REMOTE:
		tt = tt_remote;
		break;
	case SQL_REPLICA_TABLE:
		tt = tt_replica_table;
		break;
	default:
		return sql_error(sql, 02, "Invalid table type %d", temp);
	}

	if (sname && !(s = mvc_bind_schema(sql, sname)))
		return sql_error(sql, 02, "3F000!CREATE %s: no such schema '%s'", tt2string(tt), sname);

	if (temp != SQL_PERSIST && (tt == tt_table || tt == tt_array) &&
			commit_action == CA_COMMIT)
		commit_action = CA_DELETE;
	
	if (temp != SQL_DECLARED_TABLE && temp != SQL_DECLARED_ARRAY) {
		if (temp != SQL_PERSIST && (tt == tt_table || tt == tt_array)) {
			s = mvc_bind_schema(sql, "tmp");
		} else if (s == NULL) {
			s = ss;
		}
	}

	if (temp != SQL_DECLARED_TABLE  && temp != SQL_DECLARED_ARRAY && s)
		sname = s->base.name;

	if (mvc_bind_table(sql, s, name)) {
		char *cd = (temp == SQL_DECLARED_TABLE || temp == SQL_DECLARED_ARRAY)?"DECLARE":"CREATE";
		return sql_error(sql, 02, "42S01!%s %s: name '%s' already in use", cd, tt2string(tt), name);
	} else if (temp != SQL_DECLARED_TABLE && temp != SQL_DECLARED_ARRAY && (!schema_privs(sql->role_id, s) && !(isTempSchema(s) && temp == SQL_LOCAL_TEMP))){
		return sql_error(sql, 02, "42000!CREATE %s: insufficient privileges for user '%s' in schema '%s'", tt2string(tt), stack_get_string(sql, "current_user"), s->base.name);
	} else if (table_elements_or_subquery->token == SQL_CREATE_TABLE || table_elements_or_subquery->token == SQL_CREATE_ARRAY) {
		/* table or array element list, value of 'tt' separates ARRAY from TABLE */
		/* reuse SQL_DECLARED_TABLE for temp arrays as well. actual type is in 'tt' */
		sql_table *t = (tt == tt_remote)?
			mvc_create_remote(sql, s, name, SQL_DECLARED_TABLE, loc):
			mvc_create_table(sql, s, name, tt, 0, SQL_DECLARED_TABLE, commit_action, -1);
		dnode *n;
		dlist *columns = table_elements_or_subquery->data.lval;
		sql_exp *id_l = NULL, *id_r = NULL;
		sql_rel *res = NULL, *joinl = NULL, *joinr = NULL;
		list *rp = new_exp_list(sql->sa);
		node *col = NULL;
		int i = 0, j = 0, cnt = 0, *N = NULL, *M = NULL;
		lng cntall = 1;

		if (isArray(t))
			t->fixed = 1;
		for (n = columns->h; n; n = n->next) {
			symbol *sym = n->data.sym;
			int res = table_element(sql, sym, s, t, 0);
			if (res == SQL_ERR) 
				return NULL;
		}

		if (isArray(t) && t->ndims == 0)
			return sql_error(sql, 02, "CREATE ARRAY: an array must have at least one dimension");

		temp = (tt == tt_table || tt == tt_array)?temp:SQL_PERSIST;
		/* For tables and unbounded arrays we are done. */
		if (!isFixedArray(t)) {
			return isArray(t)? rel_table(sql, DDL_CREATE_ARRAY, sname, t, temp) : rel_table(sql, DDL_CREATE_TABLE, sname, t, temp);
		}
		/* For fixed arrays, we immediately create and fill in BATs for dimensions with dimension values, and for non-dim. attributes with default values */

		/* To compute N (the #times each value is repeated), multiply the size of dimensions defined after the current dimension.  For the last dimension, its N is 1.  To compute M (the #times each value group is repeated), multiply the size of dimensions defined before the current dimension.  For the first dimension, its M is 1. */
		N = SA_NEW_ARRAY(sql->sa, int, t->ndims);
		M = SA_NEW_ARRAY(sql->sa, int, t->ndims);
		if(!N || !M) {
			return sql_error(sql, 02, "CREATE ARRAY: failed to allocate space");
		}
		for(i = 0; i < t->ndims; i++) N[i] = M[i] = 1;

		for (col = t->columns.set->h, i = 0; col; col = col->next){
			sql_column *sc = (sql_column *) col->data;
			if (sc->dim){
				atom *a_sta = atom_general(sql->sa, &sc->type, sc->dim->start);
				atom *a_ste = atom_general(sql->sa, &sc->type, sc->dim->step);
				atom *a_sto = atom_general(sql->sa, &sc->type, sc->dim->stop);
				switch(a_sto->data.vtype){
				case TYPE_bte:
					cnt = ceil((*(bte *)VALget(&a_sto->data) * 1.0 - *(bte *)VALget(&a_sta->data)) / *(bte *)VALget(&a_ste->data)); break;
				case TYPE_sht:
					cnt = ceil((*(sht *)VALget(&a_sto->data) * 1.0 - *(sht *)VALget(&a_sta->data)) / *(sht *)VALget(&a_ste->data)); break;
				case TYPE_int:
					cnt = ceil((*(int *)VALget(&a_sto->data) * 1.0 - *(int *)VALget(&a_sta->data)) / *(int *)VALget(&a_ste->data)); break;
				case TYPE_flt:
					cnt = ceil((*(flt *)VALget(&a_sto->data) - *(flt *)VALget(&a_sta->data)) / *(flt *)VALget(&a_ste->data)); break;
				case TYPE_dbl:
					cnt = ceil((*(dbl *)VALget(&a_sto->data) - *(dbl *)VALget(&a_sta->data)) / *(dbl *)VALget(&a_ste->data)); break;
				case TYPE_lng:
					cnt = ceil((*(lng *)VALget(&a_sto->data) * 1.0 - *(lng *)VALget(&a_sta->data)) / *(lng *)VALget(&a_ste->data)); break;
				default: /* should not reach here */
					return sql_error(sql, 02, "CREATE ARRAY: unsupported data type \"%s\"", sc->type.type->sqlname);
				}
				for (j = 0; j < i; j++) N[j] = N[j] * cnt;
				for (j = t->ndims-1; j > i; j--) M[j] = M[j] * cnt;
				cntall *= cnt;
				i++;
			}
		}
		if (i != t->ndims) {
			return sql_error(sql, 02, "CREATE ARRAY: expected number of dimension columns (%d) does not match actual numbre of dimension columns (%d)", t->ndims, i);
		}

		/* create and fill all columns */
		for (col = t->columns.set->h, i = 0; col; col = col->next){
			sql_column *sc = (sql_column *) col->data;
			list *args = new_exp_list(sql->sa), *col_exps = new_exp_list(sql->sa), *rng_exps = new_exp_list(sql->sa), *drngs = sa_list(sql->sa);
			sql_exp *e = NULL, *func_exp = NULL, *estrt = NULL, *estep = NULL, *estop = NULL;
			sql_subtype *oid_tpe = sql_bind_localtype("oid");
			sql_subfunc *sf = NULL;

			if (sc->dim){
				/* TODO: can we avoid computing these 'atom_general' twice? */
				estrt = exp_atom(sql->sa, atom_general(sql->sa, &sc->type, sc->dim->start));
				estep = exp_atom(sql->sa, atom_general(sql->sa, &sc->type, sc->dim->step));
				estop = exp_atom(sql->sa, atom_general(sql->sa, &sc->type, sc->dim->stop));

				append(args, estrt);
				append(args, estep);
				append(args, estop);
				append(args, exp_atom_int(sql->sa, N[i]));
				append(args, exp_atom_int(sql->sa, M[i]));
				sf = sql_bind_func_(sql->sa, mvc_bind_schema(sql, "sys"), "array_series", exps_subtype(args), F_FUNC);
				if (!sf)
					return sql_error(sql, 02, "failed to bind to the SQL function \"array_series\"");
				func_exp = exp_op(sql->sa, args, sf);
				if (!id_l) {
					id_l = exp_column(sql->sa, sc->base.name, "id", oid_tpe, CARD_MULTI, 0, 0, NULL);
					append(col_exps, id_l);
				} else {
					id_r = exp_column(sql->sa, sc->base.name, "id", oid_tpe, CARD_MULTI, 0, 0, NULL);
					append(col_exps, id_r);
				}
				append(rng_exps, estrt);
				append(rng_exps, estep);
				append(rng_exps, estop);
				list_append(drngs, rng_exps);
				list_append(drngs, new_exp_list(sql->sa)); /* empty lists for slicing and */
				list_append(drngs, new_exp_list(sql->sa)); /* tiling ranges */
				append(col_exps, exp_column(sql->sa, sc->base.name, "dimval", &sc->type, CARD_MULTI, 0, 0, drngs));
				append(rp, exp_column(sql->sa, sc->base.name, "dimval", &sc->type, CARD_MULTI, 0, 0, drngs));
				i++;
			} else {
				if (sc->def) {
					char *q = sql_message("select %s;", sc->def);
					e = rel_parse_val(sql, q, sql->emode);
					_DELETE(q);
					if (!e || (e = rel_check_type(sql, &sc->type, e, type_equal)) == NULL)
						return NULL;
				} else {
					atom *a = atom_general(sql->sa, &sc->type, NULL);
					e = exp_atom(sql->sa, a);
				}
				append(args, exp_atom_lng(sql->sa, cntall));
				append(args, e);
				sf = sql_bind_func_(sql->sa, mvc_bind_schema(sql, "sys"), "array_filler", exps_subtype(args), F_FUNC);
				if (!sf)
					return sql_error(sql, 02, "failed to bind to the SQL function \"array_filler\"");
				func_exp = exp_op(sql->sa, args, sf);
				if (!id_l) {
					id_l = exp_column(sql->sa, sc->base.name, "id", oid_tpe, CARD_MULTI, 0, 0, NULL);
					append(col_exps, id_l);
				} else {
					id_r = exp_column(sql->sa, sc->base.name, "id", oid_tpe, CARD_MULTI, 0, 0, NULL);
					append(col_exps, id_r);

				}
				append(col_exps, exp_column(sql->sa, sc->base.name, "cellval", &sc->type, CARD_MULTI, (!sc->dim && !sc->def)?1:0, 0, NULL));
				append(rp, exp_column(sql->sa, sc->base.name, "cellval", &sc->type, CARD_MULTI, (!sc->dim && !sc->def)?1:0, 0, NULL));
			}
			if (!joinl) {
				joinl = rel_table_func(sql->sa, NULL, func_exp, col_exps);
			} else {
				joinr = rel_table_func(sql->sa, NULL, func_exp, col_exps);
				joinl = rel_crossproduct(sql->sa, joinl, joinr, op_join);
				rel_join_add_exp(sql->sa, joinl, exp_compare(sql->sa, id_l, id_r, cmp_equal));
			}

		}
		if (i != t->ndims) {
			return sql_error(sql, 02, "CREATE ARRAY: expected number of dimension columns (%d) does not match actual numbre of dimension columns (%d)", t->ndims, i);
		}
		res = rel_table(sql, DDL_CREATE_ARRAY, sname, t, temp);
		return rel_insert(sql, res, rel_project(sql->sa, joinl, rp));
	} else { /* [col name list] as subquery with or without data */
		/* TODO: handle create_array_as_subquery??? */
		sql_rel *sq = NULL, *res = NULL;
		dlist *as_sq = table_elements_or_subquery->data.lval;
		dlist *column_spec = as_sq->h->data.lval;
		symbol *subquery = as_sq->h->next->data.sym;
		int with_data = as_sq->h->next->next->data.i_val;
		sql_table *t = NULL; 

		assert(as_sq->h->next->next->type == type_int);
		sq = rel_selects(sql, subquery);
		if (!sq)
			return NULL;

		/* create table */
		if (create && (t = mvc_create_table_as_subquery( sql, sq, s, name, column_spec, temp, commit_action)) == NULL) { 
			rel_destroy(sq);
			return NULL;
		}

		/* insert query result into this table */
		temp = (tt == tt_table || tt == tt_array)?temp:SQL_PERSIST;
		res = (tt == tt_array)? rel_table(sql, DDL_CREATE_ARRAY, sname, t, temp) : rel_table(sql, DDL_CREATE_TABLE, sname, t, temp);
		if (with_data) {
			res = rel_insert(sql, res, sq);
		} else {
			rel_destroy(sq);
		}
		return res;
	}
	/*return NULL;*/ /* never reached as all branches of the above if() end with return ... */
}

static sql_rel *
rel_create_view(mvc *sql, sql_schema *ss, dlist *qname, dlist *column_spec, symbol *query, int check, int persistent)
{
	char *name = qname_table(qname);
	char *sname = qname_schema(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL;
	int instantiate = (sql->emode == m_instantiate || !persistent);
	int deps = (sql->emode == m_deps);
	int create = (!instantiate && !deps);

(void)ss;
	(void) check;		/* Stefan: unused!? */
	if (sname && !(s = mvc_bind_schema(sql, sname))) 
		return sql_error(sql, 02, "3F000!CREATE VIEW: no such schema '%s'", sname);
	if (s == NULL)
		s = cur_schema(sql);

	if (create && mvc_bind_table(sql, s, name) != NULL) {
		return sql_error(sql, 02, "42S01!CREATE VIEW: name '%s' already in use", name);
	} else if (create && (!schema_privs(sql->role_id, s) && !(isTempSchema(s) && persistent == SQL_LOCAL_TEMP))) {
		return sql_error(sql, 02, "42000!CREATE VIEW: access denied for %s to schema ;'%s'", stack_get_string(sql, "current_user"), s->base.name);
	} else if (query) {
		sql_rel *sq = NULL;
		char *q = QUERY(sql->scanner);

		if (query->token == SQL_SELECT) {
			SelectNode *sn = (SelectNode *) query;

			if (sn->limit)
				return sql_error(sql, 01, "0A000!42000!CREATE VIEW: LIMIT not supported");
			if (sn->orderby)
				return sql_error(sql, 01, "42000!CREATE VIEW: ORDER BY not supported");
		}

		sq = rel_selects(sql, query);
		if (!sq)
			return NULL;

		if (!create)
			rel_add_intern(sql, sq);

		if (create) {
			t = mvc_create_view(sql, s, name, SQL_DECLARED_TABLE, q, 0);
			if (as_subquery( sql, t, sq, column_spec, "CREATE VIEW") != 0) {
				rel_destroy(sq);
				return NULL;
			}
			return rel_table(sql, DDL_CREATE_VIEW, s->base.name, t, SQL_PERSIST);
		}
		t = mvc_bind_table(sql, s, name);
		if (!persistent && column_spec) 
			sq = view_rename_columns( sql, name, sq, column_spec);
		return sq;
	}
	return NULL;
}

static char *
dlist_get_schema_name(dlist *name_auth)
{
	assert(name_auth && name_auth->h);
	return name_auth->h->data.sval;
}

static char *
schema_auth(dlist *name_auth)
{
	assert(name_auth && name_auth->h && dlist_length(name_auth) == 2);
	return name_auth->h->next->data.sval;
}

static sql_rel *
rel_schema(sql_allocator *sa, int cat_type, char *sname, char *auth, int nr)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	append(exps, exp_atom_int(sa, nr));
	append(exps, exp_atom_clob(sa, sname));
	if (auth)
		append(exps, exp_atom_clob(sa, auth));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = cat_type;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
rel_schema2(sql_allocator *sa, int cat_type, char *sname, char *auth, int nr)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	append(exps, exp_atom_clob(sa, sname));
	append(exps, exp_atom_clob(sa, auth));
	append(exps, exp_atom_int(sa, nr));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = cat_type;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}


static sql_rel *
rel_create_schema(mvc *sql, dlist *auth_name, dlist *schema_elements)
{
	char *name = dlist_get_schema_name(auth_name);
	char *auth = schema_auth(auth_name);
	int auth_id = sql->role_id;

	if (auth && (auth_id = sql_find_auth(sql, auth)) < 0) {
		sql_error(sql, 02, "28000!CREATE SCHEMA: no such authorization '%s'", auth);
		return NULL;
	}
	if (sql->user_id != USER_MONETDB && sql->role_id != ROLE_SYSADMIN) {
		sql_error(sql, 02, "42000!CREATE SCHEMA: insufficient privileges for user '%s'", stack_get_string(sql, "current_user"));
		return NULL;
	}
	if (mvc_bind_schema(sql, name)) {
		sql_error(sql, 02, "3F000!CREATE SCHEMA: name '%s' already in use", name);
		return NULL;
	} else {
		sql_schema *os = sql->session->schema;
		dnode *n;
		sql_schema *ss = SA_ZNEW(sql->sa, sql_schema);
		sql_rel *ret;

		ret = rel_schema(sql->sa, DDL_CREATE_SCHEMA, 
			   dlist_get_schema_name(auth_name),
			   schema_auth(auth_name), 0);

		ss->base.name = name;
		ss->auth_id = auth_id;
		ss->owner = sql->user_id;

		sql->session->schema = ss;
		n = schema_elements->h;
		while (n) {
			sql_rel *res = rel_semantic(sql, n->data.sym);
			if (!res) {
				rel_destroy(ret);
				return NULL;
			}
			ret = rel_list(sql->sa, ret, res);
			n = n->next;
		}
		sql->session->schema = os;
		return ret;
	}
}

static str
get_schema_name( mvc *sql, char *sname, char *tname)
{
	if (!sname) {
		sql_schema *ss = cur_schema(sql);
		sql_table *t = mvc_bind_table(sql, ss, tname);
		if (!t)
			ss = tmp_schema(sql);
		sname = ss->base.name;
	}
	return sname;
}

static sql_rel *
rel_alter_table(mvc *sql, dlist *qname, symbol *te)
{
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);
	sql_schema *s = NULL;
	sql_table *t = NULL;

	if (sname && !(s=mvc_bind_schema(sql, sname))) {
		(void) sql_error(sql, 02, "3F000!ALTER TABLE: no such schema '%s'", sname);
		return NULL;
	}
	if (!s)
		s = cur_schema(sql);

	if ((t = mvc_bind_table(sql, s, tname)) == NULL) {
		return sql_error(sql, 02, "42S02!ALTER TABLE: no such table '%s'", tname);
	} else {
		node *n;
		sql_rel *res = NULL, *r;
		sql_table *nt = dup_sql_table(sql->sa, t);
		sql_exp ** updates, *e;

		if (nt && te && te->token == SQL_DROP_CONSTRAINT) {
			dlist *l = te->data.lval;
			char *kname = l->h->data.sval;
			int drop_action = l->h->next->data.i_val;
			
			sname = get_schema_name(sql, sname, tname);
			return rel_schema(sql->sa, DDL_DROP_CONSTRAINT, sname, kname, drop_action);
		}

		if (!nt || (te && table_element(sql, te, s, nt, 1) == SQL_ERR)) 
			return NULL;

		if (t->persistence != SQL_DECLARED_TABLE && s)
			sname = s->base.name;

		if (t->s && !nt->s)
			nt->s = t->s;

		if (!te) /* Set Read only */
			nt = mvc_readonly(sql, nt, 1);
		res = rel_table(sql, DDL_ALTER_TABLE, sname, nt, 0);
		if (!te) /* Set Read only */
			return res;
		/* table add table */
		if (te->token == SQL_TABLE) {
			char *ntname = te->data.lval->h->data.sval;
			sql_table *nnt = mvc_bind_table(sql, s, ntname);

			if (nnt)
				cs_add(&nt->tables, nnt, TR_NEW); 
		}
		/* table drop table */
		if (te->token == SQL_DROP_TABLE) {
			char *ntname = te->data.lval->h->data.sval;
			int drop_action = te->data.lval->h->next->data.i_val;
			node *n = cs_find_name(&nt->tables, ntname);

			if (n) {
				sql_table *ntt = n->data;

				ntt->drop_action = drop_action;
				cs_del(&nt->tables, n, ntt->base.flag); 
			}
		}

		/* new columns need update with default values */
		updates = table_update_array(sql, nt);
		e = exp_column(sql->sa, nt->base.name, "%TID%", sql_bind_localtype("oid"), CARD_MULTI, 0, 1, NULL);
		r = rel_project(sql->sa, res, append(new_exp_list(sql->sa),e));
		if (nt->columns.nelm) {
			list *cols = new_exp_list(sql->sa);
			for (n = nt->columns.nelm; n; n = n->next) {
				sql_column *c = n->data;

				if (c->def) {
					char *d = sql_message("select %s;", c->def);
					e = rel_parse_val(sql, d, sql->emode);
					_DELETE(d);
				} else {
					e = exp_atom(sql->sa, atom_general(sql->sa, &c->type, NULL));
				}
				if (!e || (e = rel_check_type(sql, &c->type, e, type_equal)) == NULL) {
					rel_destroy(r);
					return NULL;
				}
				if(c->dim) {
					list *rng_exps = new_exp_list(sql->sa), *drngs = sa_list(sql->sa);
					assert(rng_exps && drngs);

					append(rng_exps, exp_atom(sql->sa, atom_general(sql->sa, &c->type, c->dim->start)));
					append(rng_exps, exp_atom(sql->sa, atom_general(sql->sa, &c->type, c->dim->step)));
					append(rng_exps, exp_atom(sql->sa, atom_general(sql->sa, &c->type, c->dim->stop)));
					append(drngs, rng_exps);
					append(drngs, new_exp_list(sql->sa)); /* empty lists for slicing and */
					append(drngs, new_exp_list(sql->sa)); /* tiling ranges */
					list_append(cols, exp_column(sql->sa, nt->base.name, c->base.name, &c->type, CARD_MULTI, 0, 0, drngs));
				} else {
					list_append(cols, exp_column(sql->sa, nt->base.name, c->base.name, &c->type, CARD_MULTI, 0, 0, NULL));
				}

				assert(!updates[c->colnr]);
				exp_setname(sql->sa, e, c->t->base.name, c->base.name);
				updates[c->colnr] = e;
			}
			res = rel_update(sql, res, r, updates, cols); 
		} else { /* new indices or keys */
			res = rel_update(sql, res, r, updates, NULL); 
		}
		return res;
	}
}

static sql_rel *
rel_role(sql_allocator *sa, char *grantee, char *auth, int type)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	assert(type == DDL_GRANT_ROLES || type == DDL_REVOKE_ROLES);
	append(exps, exp_atom_clob(sa, grantee));
	append(exps, exp_atom_clob(sa, auth));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = type;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
rel_grant_roles(mvc *sql, sql_schema *schema, dlist *roles, dlist *grantees, int grant, int grantor)
{
	sql_rel *res = NULL;
	/* grant roles to the grantees */
	dnode *r, *g;

	(void) schema;
	(void) grant;
	(void) grantor;		/* Stefan: unused!? */

	for (r = roles->h; r; r = r->next) {
		char *role = r->data.sval;

		for (g = grantees->h; g; g = g->next) {
			char *grantee = g->data.sval;

			if ((res = rel_list(sql->sa, res, rel_role(sql->sa, grantee, role, DDL_GRANT_ROLES))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
		}
	}
	return res;
}

static sql_rel *
rel_revoke_roles(mvc *sql, sql_schema *schema, dlist *roles, dlist *grantees, int admin, int grantor)
{
	sql_rel *res = NULL;
	/* revoke roles from the grantees */
	dnode *r, *g;

	(void) schema;
	(void) admin;
	(void) grantor;		/* Stefan: unused!? */

	for (r = roles->h; r; r = r->next) {
		char *role = r->data.sval;

		for (g = grantees->h; g; g = g->next) {
			char *grantee = g->data.sval;

			if ((res = rel_list(sql->sa, res, rel_role(sql->sa, grantee, role, DDL_REVOKE_ROLES))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
		}
	}
	return res;
}

static sql_rel *
rel_priv(sql_allocator *sa, char *sname, char *name, char *grantee, int privs, char *cname, int grant, int grantor, int type)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	assert(type == DDL_GRANT || type == DDL_REVOKE);
	append(exps, exp_atom_clob(sa, sname));
	append(exps, exp_atom_clob(sa, name));
	append(exps, exp_atom_clob(sa, grantee));
	append(exps, exp_atom_int(sa, privs));
	append(exps, cname?(void*)exp_atom_clob(sa, cname):(void*)cname);
	append(exps, exp_atom_int(sa, grant));
	append(exps, exp_atom_int(sa, grantor));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = type;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
rel_grant_table(mvc *sql, sql_schema *cur, dlist *privs, dlist *qname, dlist *grantees, int grant, int grantor)
{
	sql_rel *res = NULL;
	dnode *gn;
	int all = PRIV_SELECT | PRIV_UPDATE | PRIV_INSERT | PRIV_DELETE;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);

	if (!sname)
		sname = cur->base.name;
	for (gn = grantees->h; gn; gn = gn->next) {
		dnode *opn;
		char *grantee = gn->data.sval;

		if (!grantee)
			grantee = "public";

		if (!privs) {
			if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, all, NULL, grant, grantor, DDL_GRANT))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
			continue;
		}
		for (opn = privs->h; opn; opn = opn->next) {
			symbol *op = opn->data.sym;
			int priv = PRIV_SELECT;
	
			switch (op->token) {
			case SQL_SELECT:
				priv = PRIV_SELECT;
				break;
			case SQL_UPDATE:
				priv = PRIV_UPDATE;
				break;
			case SQL_INSERT:
				priv = PRIV_INSERT;
				break;
			case SQL_DELETE:
				priv = PRIV_DELETE;
				break;
			case SQL_EXECUTE:
			default:
				return sql_error(sql, 02, "42000!Cannot GRANT EXECUTE on table name %s", tname);
			}

			if ((op->token == SQL_SELECT || op->token == SQL_UPDATE) && op->data.lval) {
				dnode *cn;

				for (cn = op->data.lval->h; cn; cn = cn->next) {
					char *cname = cn->data.sval;
					if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, priv, cname, grant, grantor, DDL_GRANT))) == NULL) {
						rel_destroy(res);
						return NULL;
					}
				}
			} else if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, priv, NULL, grant, grantor, DDL_GRANT))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
		}
	}
	return res;
}

static sql_rel *
rel_grant_func(mvc *sql, sql_schema *cur, dlist *privs, dlist *qname, dlist *grantees, int grant, int grantor)
{
	char *fname = qname_func(qname);

	/* todo */
	(void) sql;
	(void) cur;
	(void) privs;
	(void) grantees;
	(void) grant;
	(void) grantor;
	return sql_error(sql, 02, "42000!GRANT Table/Function name %s doesn't exist", fname);
}


static sql_rel *
rel_grant_privs(mvc *sql, sql_schema *cur, dlist *privs, dlist *grantees, int grant, int grantor)
{
	dlist *obj_privs = privs->h->data.lval;
	symbol *obj = privs->h->next->data.sym;
	int token = obj->token;

	if (token == SQL_NAME) {
		dlist *qname = obj->data.lval;
		char *sname = qname_schema(qname);
		char *tname = qname_table(qname);
		sql_schema *s = cur;

		if (sname)
			s = mvc_bind_schema(sql, sname);
		if (s && mvc_bind_table(sql, s, tname) != NULL)
			token = SQL_TABLE;
	}

	switch (token) {
	case SQL_TABLE:
		return rel_grant_table(sql, cur, obj_privs, obj->data.lval, grantees, grant, grantor);
	case SQL_NAME:
		return rel_grant_func(sql, cur, obj_privs, obj->data.lval, grantees, grant, grantor);
	default:
		return sql_error(sql, 02, "M0M03!Grant: unknown token %d", token);
	}
}

static sql_rel *
rel_revoke_table(mvc *sql, sql_schema *cur, dlist *privs, dlist *qname, dlist *grantees, int grant, int grantor)
{
	dnode *gn;
	sql_rel *res = NULL;
	int all = PRIV_SELECT | PRIV_UPDATE | PRIV_INSERT | PRIV_DELETE;
	char *sname = qname_schema(qname);
	char *tname = qname_table(qname);

	if (!sname)
		sname = cur->base.name;
	for (gn = grantees->h; gn; gn = gn->next) {
		dnode *opn;
		char *grantee = gn->data.sval;

		if (!grantee)
			grantee = "public";

		if (!privs) {
			if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, all, NULL, grant, grantor, DDL_REVOKE))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
			continue;
		}
		for (opn = privs->h; opn; opn = opn->next) {
			symbol *op = opn->data.sym;
			int priv = PRIV_SELECT;

			switch (op->token) {
			case SQL_SELECT:
				priv = PRIV_SELECT;
				break;
			case SQL_UPDATE:
				priv = PRIV_UPDATE;
				break;

			case SQL_INSERT:
				priv = PRIV_INSERT;
				break;
			case SQL_DELETE:
				priv = PRIV_DELETE;
				break;

			case SQL_EXECUTE:
			default:
				return sql_error(sql, 02, "42000!Cannot GRANT EXECUTE on table name %s", tname);
			}

			if ((op->token == SQL_SELECT || op->token == SQL_UPDATE) && op->data.lval) {
				dnode *cn;

				for (cn = op->data.lval->h; cn; cn = cn->next) {
					char *cname = cn->data.sval;
					if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, priv, cname, grant, grantor, DDL_REVOKE))) == NULL) {
						rel_destroy(res);
						return NULL;
					}
				}
			} else if ((res = rel_list(sql->sa, res, rel_priv(sql->sa, sname, tname, grantee, priv, NULL, grant, grantor, DDL_REVOKE))) == NULL) {
				rel_destroy(res);
				return NULL;
			}
		}
	}
	return res;
}

static sql_rel *
rel_revoke_func(mvc *sql, sql_schema *cur, dlist *privs, dlist *qname, dlist *grantees, int grant, int grantor)
{
	char *fname = qname_func(qname);

	/* todo */
	(void) sql;
	(void) cur;
	(void) privs;
	(void) fname;
	(void) grantees;
	(void) grant;
	(void) grantor;
	return NULL;
}

static sql_rel *
rel_revoke_privs(mvc *sql, sql_schema *cur, dlist *privs, dlist *grantees, int grant, int grantor)
{
	dlist *obj_privs = privs->h->data.lval;
	symbol *obj = privs->h->next->data.sym;
	int token = obj->token;

	if (token == SQL_NAME) {
		dlist *qname = obj->data.lval;
		char *sname = qname_schema(qname);
		char *tname = qname_table(qname);
		sql_schema *s = cur;

		if (sname)
			s = mvc_bind_schema(sql, sname);
		if (s && mvc_bind_table(sql, s, tname) != NULL)
			token = SQL_TABLE;
	}

	switch (token) {
	case SQL_TABLE:
		return rel_revoke_table(sql, cur, obj_privs, obj->data.lval, grantees, grant, grantor);
	case SQL_NAME:
		return rel_revoke_func(sql, cur, obj_privs, obj->data.lval, grantees, grant, grantor);
	default:
		return sql_error(sql, 02, "M0M03!Grant: unknown token %d", token);
	}
}

/* iname, itype, sname.tname (col1 .. coln) */
static sql_rel *
rel_create_index(mvc *sql, sql_schema *ss, char *iname, int itype, dlist *qname, dlist *column_list)
{
	sql_allocator *sa = sql->sa;
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);
	char *tname = qname_table(qname);
	char *sname = qname_schema(qname);
	dnode *n = column_list->h;

	if (!sname && ss)
		sname = ss->base.name;

	append(exps, exp_atom_clob(sa, iname));
	append(exps, exp_atom_int(sa, itype));
	append(exps, exp_atom_clob(sa, sname));
	append(exps, exp_atom_clob(sa, tname));

	for (; n; n = n->next) {
		char *cname = n->data.sval;

		append(exps, exp_atom_clob(sa, cname));
	}
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = DDL_CREATE_INDEX;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
rel_create_user(sql_allocator *sa, char *user, char *passwd, int enc, char *fullname, char *schema)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	append(exps, exp_atom_clob(sa, user));
	append(exps, exp_atom_clob(sa, passwd));
	append(exps, exp_atom_int(sa, enc));
	append(exps, exp_atom_clob(sa, schema));
	append(exps, exp_atom_clob(sa, fullname));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = DDL_CREATE_USER;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

static sql_rel *
rel_alter_user(sql_allocator *sa, char *user, char *passwd, int enc, char *schema, char *oldpasswd)
{
	sql_rel *rel = rel_create(sa);
	list *exps = new_exp_list(sa);

	append(exps, exp_atom_clob(sa, user));
	append(exps, exp_atom_clob(sa, passwd));
	append(exps, exp_atom_int(sa, enc));
	append(exps, exp_atom_clob(sa, schema));
	append(exps, exp_atom_clob(sa, oldpasswd));
	rel->l = NULL;
	rel->r = NULL;
	rel->op = op_ddl;
	rel->flag = DDL_ALTER_USER;
	rel->exps = exps;
	rel->card = 0;
	rel->nrcols = 0;
	return rel;
}

sql_rel *
rel_schemas(mvc *sql, symbol *s)
{
	sql_rel *ret = NULL;

	if (s->token != SQL_CREATE_TABLE && s->token != SQL_CREATE_ARRAY && s->token != SQL_CREATE_VIEW && STORE_READONLY(active_store_type))
		return sql_error(sql, 06, "25006!schema statements cannot be executed on a readonly database.");

	switch (s->token) {
	case SQL_CREATE_SCHEMA:
	{
		dlist *l = s->data.lval;

		ret = rel_create_schema(sql, l->h->data.lval,
				l->h->next->next->next->data.lval);
	} 	break;
	case SQL_DROP_SCHEMA:
	{
		dlist *l = s->data.lval;
		dlist *auth_name = l->h->data.lval;

		assert(l->h->next->type == type_int);
		ret = rel_schema(sql->sa, DDL_DROP_SCHEMA, 
			   dlist_get_schema_name(auth_name),
			   NULL,
			   l->h->next->data.i_val);	/* drop_action */
	} 	break;
	case SQL_CREATE_ARRAY:
	case SQL_CREATE_TABLE:
	{
		dlist *l = s->data.lval;
		dlist *qname = l->h->next->data.lval;
		char *sname = qname_schema(qname);
		char *name = qname_table(qname);
		int temp = l->h->data.i_val;

		assert(l->h->type == type_int);
		assert(l->h->next->next->next->type == type_int);
		ret = rel_create_table(sql, cur_schema(sql), temp, sname, name, l->h->next->next->data.sym, l->h->next->next->next->data.i_val, l->h->next->next->next->next->data.sval);
	} 	break;
	case SQL_CREATE_VIEW:
	{
		dlist *l = s->data.lval;

		assert(l->h->next->next->next->type == type_int);
		assert(l->h->next->next->next->next->type == type_int);
		ret = rel_create_view(sql, NULL, l->h->data.lval, l->h->next->data.lval, l->h->next->next->data.sym, l->h->next->next->next->data.i_val, l->h->next->next->next->next->data.i_val);
	} 	break;
	case SQL_DROP_TABLE:
	case SQL_DROP_ARRAY:
	{
		dlist *l = s->data.lval;
		char *sname = qname_schema(l->h->data.lval);
		char *tname = qname_table(l->h->data.lval);

		assert(l->h->next->type == type_int);
		sname = get_schema_name(sql, sname, tname);
		ret = rel_schema(sql->sa, (s->token == SQL_DROP_TABLE)?DDL_DROP_TABLE:DDL_DROP_ARRAY, sname, tname, l->h->next->data.i_val);
	} 	break;
	case SQL_DROP_VIEW:
	{
		dlist *l = s->data.lval;
		char *sname = qname_schema(l->h->data.lval);
		char *tname = qname_table(l->h->data.lval);

		assert(l->h->next->type == type_int);
		sname = get_schema_name(sql, sname, tname);
		ret = rel_schema(sql->sa, DDL_DROP_VIEW, sname, tname, l->h->next->data.i_val);
	} 	break;
	case SQL_ALTER_TABLE:
	{
		dlist *l = s->data.lval;

		ret = rel_alter_table(sql, 
			l->h->data.lval,	/* table name */
		  	l->h->next->data.sym);/* table element */
	} 	break;
	case SQL_GRANT_ROLES:
	{
		dlist *l = s->data.lval;

		assert(l->h->next->next->type == type_int);
		assert(l->h->next->next->next->type == type_int);
		ret = rel_grant_roles(sql, cur_schema(sql), l->h->data.lval,	/* authids */
				  l->h->next->data.lval,	/* grantees */
				  l->h->next->next->data.i_val,	/* admin? */
				  l->h->next->next->next->data.i_val ? sql->user_id : sql->role_id);
		/* grantor ? */
	} 	break;
	case SQL_REVOKE_ROLES:
	{
		dlist *l = s->data.lval;

		assert(l->h->next->next->type == type_int);
		assert(l->h->next->next->next->type == type_int);
		ret = rel_revoke_roles(sql, cur_schema(sql), l->h->data.lval,	/* authids */
				  l->h->next->data.lval,	/* grantees */
				  l->h->next->next->data.i_val,	/* admin? */
				  l->h->next->next->next->data.i_val ? sql->user_id : sql->role_id);
		/* grantor ? */
	} 	break;
	case SQL_GRANT:
	{
		dlist *l = s->data.lval;

		assert(l->h->next->next->type == type_int);
		assert(l->h->next->next->next->type == type_int);
		ret = rel_grant_privs(sql, cur_schema(sql), l->h->data.lval,	/* privileges */
				  l->h->next->data.lval,	/* grantees */
				  l->h->next->next->data.i_val,	/* grant ? */
				  l->h->next->next->next->data.i_val ? sql->user_id : sql->role_id);
		/* grantor ? */
	} 	break;
	case SQL_REVOKE:
	{
		dlist *l = s->data.lval;

		assert(l->h->next->next->type == type_int);
		assert(l->h->next->next->next->type == type_int);
		ret = rel_revoke_privs(sql, cur_schema(sql), l->h->data.lval,	/* privileges */
				   l->h->next->data.lval,	/* grantees */
				   l->h->next->next->data.i_val,	/* grant ? */
				   l->h->next->next->next->data.i_val ? sql->user_id : sql->role_id);
		/* grantor ? */
	} 	break;
	case SQL_CREATE_ROLE:
	{
		dlist *l = s->data.lval;
		char *rname = l->h->data.sval;
		ret = rel_schema2(sql->sa, DDL_CREATE_ROLE, rname, NULL,
				 l->h->next->data.i_val);
	} 	break;
	case SQL_DROP_ROLE:
	{
		char *rname = s->data.sval;
		ret = rel_schema2(sql->sa, DDL_DROP_ROLE, rname, NULL, 0);
	} 	break;
	case SQL_CREATE_INDEX: {
		dlist *l = s->data.lval;

		assert(l->h->next->type == type_int);
		ret = rel_create_index(sql, cur_schema(sql), l->h->data.sval, l->h->next->data.i_val, l->h->next->next->data.lval, l->h->next->next->next->data.lval);
	} 	break;
	case SQL_DROP_INDEX: {
		dlist *l = s->data.lval;
		char *sname = qname_schema(l);

		if (!sname)
			sname = cur_schema(sql)->base.name;
		ret = rel_schema2(sql->sa, DDL_DROP_INDEX, sname, qname_index(l), 0);
	} 	break;
	case SQL_CREATE_USER: {
		dlist *l = s->data.lval;

		ret = rel_create_user(sql->sa, l->h->data.sval,	/* user name */
				  l->h->next->data.sval,	/* password */
				  l->h->next->next->next->next->data.i_val == SQL_PW_ENCRYPTED, /* encrypted */
				  l->h->next->next->data.sval,	/* fullname */
				  l->h->next->next->next->data.sval);	/* dschema */
	} 	break;
	case SQL_DROP_USER:
		ret = rel_schema2(sql->sa, DDL_DROP_USER, s->data.sval, NULL, 0);
		break;
	case SQL_ALTER_USER: {
		dlist *l = s->data.lval;
		dnode *a = l->h->next->data.lval->h;

		ret = rel_alter_user(sql->sa, l->h->data.sval,	/* user */
				     a->data.sval,	/* passwd */
				     a->next->next->data.i_val == SQL_PW_ENCRYPTED, /* encrypted */
				     a->next->data.sval,	/* schema */
				     a->next->next->next->data.sval /* old passwd */
		    );
	} 	break;
	case SQL_RENAME_USER: {
		dlist *l = s->data.lval;

		ret = rel_schema2(sql->sa, DDL_RENAME_USER, l->h->data.sval, l->h->next->data.sval, 0);
	} 	break;
	case SQL_CREATE_TYPE: {
		dlist *l = s->data.lval;

		ret = rel_schema2(sql->sa, DDL_CREATE_TYPE, 
				l->h->data.sval, l->h->next->data.sval, 0);
	} 	break;
	case SQL_DROP_TYPE: {
		ret = rel_schema2(sql->sa, DDL_DROP_TYPE, s->data.sval, NULL, 0);
	} 	break;
	default:
		return sql_error(sql, 01, "M0M03!schema statement unknown symbol(" PTRFMT ")->token = %s", PTRFMTCAST s, token2string(s->token));
	}

	sql->last = NULL;
	sql->type = Q_SCHEMA;
	return ret;
}
