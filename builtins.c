/*
  implementations of some built-in functions and utilities
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#ifdef BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"

#include <fcntl.h>
#include <unistd.h>

// --- system word size ---

int jl_word_size()
{
#ifdef BITS64
    return 64;
#else
    return 32;
#endif
}

// --- exceptions ---

extern char *julia_home;

void jl_error(const char *str)
{
    jl_value_t *msg = jl_pchar_to_string((char*)str, strlen(str));
    JL_GC_PUSH(&msg);
    jl_raise(jl_new_struct(jl_errorexception_type, msg));
}

void jl_errorf(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int nc = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    jl_value_t *msg = jl_pchar_to_string(buf, nc);
    JL_GC_PUSH(&msg);
    jl_raise(jl_new_struct(jl_errorexception_type, msg));
}

void jl_too_few_args(const char *fname, int min)
{
    // TODO: ArgumentError
    jl_errorf("%s: too few arguments (expected %d)", fname, min);
}

void jl_too_many_args(const char *fname, int max)
{
    jl_errorf("%s: too many arguments (expected %d)", fname, max);
}

void jl_type_error(const char *fname, jl_value_t *expected, jl_value_t *got)
{
    jl_value_t *ex = jl_new_struct(jl_typeerror_type, jl_symbol(fname),
                                   jl_an_empty_string, expected, got);
    jl_raise(ex);
}

void jl_type_error_rt(const char *fname, const char *context,
                      jl_value_t *ty, jl_value_t *got)
{
    jl_value_t *ctxt=NULL;
    JL_GC_PUSH(&ctxt, &got);
    ctxt = jl_pchar_to_string((char*)context, strlen(context));
    jl_value_t *ex = jl_new_struct(jl_typeerror_type, jl_symbol(fname),
                                   ctxt, ty, got);
    jl_raise(ex);
}

JL_CALLABLE(jl_f_throw)
{
    JL_NARGS(throw, 1, 1);
    jl_raise(args[0]);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_trycatch)
{
    assert(nargs == 2);
    assert(jl_is_function(args[0]));
    assert(jl_is_function(args[1]));
    jl_value_t *v;
    JL_TRY {
        v = jl_apply((jl_function_t*)args[0], NULL, 0);
    }
    JL_CATCH {
        v = jl_apply((jl_function_t*)args[1], &jl_exception_in_transit, 1);
    }
    return v;
}

// --- primitives ---

JL_CALLABLE(jl_f_is)
{
    JL_NARGS(is, 2, 2);
    if (args[0] == args[1])
        return jl_true;
    return jl_false;
}

JL_CALLABLE(jl_f_no_function)
{
    jl_error("function not defined");
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_typeof)
{
    JL_NARGS(typeof, 1, 1);
    return jl_full_type(args[0]);
}

JL_CALLABLE(jl_f_subtype)
{
    JL_NARGS(subtype, 2, 2);
    if (!jl_is_typector(args[0]) && !jl_is_typevar(args[0]))
        JL_TYPECHK(subtype, type, args[0]);
    if (!jl_is_typector(args[1]) && !jl_is_typevar(args[1]))
        JL_TYPECHK(subtype, type, args[1]);
    return (jl_subtype(args[0],args[1],0) ? jl_true : jl_false);
}

JL_CALLABLE(jl_f_isa)
{
    JL_NARGS(isa, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(isa, type, args[1]);
    return (jl_subtype(args[0],args[1],1) ? jl_true : jl_false);
}

JL_CALLABLE(jl_f_typeassert)
{
    JL_NARGS(typeassert, 2, 2);
    if (!jl_is_typector(args[1]))
        JL_TYPECHK(typeassert, type, args[1]);
    if (!jl_subtype(args[0],args[1],1))
        jl_type_error("typeassert", args[1], args[0]);
    return args[0];
}

JL_CALLABLE(jl_f_apply)
{
    JL_NARGSV(apply, 1);
    JL_TYPECHK(apply, function, args[0]);
    if (nargs == 2) {
        JL_TYPECHK(apply, tuple, args[1]);
        return jl_apply((jl_function_t*)args[0], &jl_tupleref(args[1],0),
                        ((jl_tuple_t*)args[1])->length);
    }
    size_t n=0, i, j;
    for(i=1; i < nargs; i++) {
        JL_TYPECHK(apply, tuple, args[i]);
        n += ((jl_tuple_t*)args[i])->length;
    }
    jl_value_t **newargs = alloca(n * sizeof(jl_value_t*));
    n = 0;
    for(i=1; i < nargs; i++) {
        jl_tuple_t *t = (jl_tuple_t*)args[i];
        for(j=0; j < t->length; j++)
            newargs[n++] = jl_tupleref(t, j);
    }
    return jl_apply((jl_function_t*)args[0], newargs, n);
}

// heuristic for whether a top-level input should be evaluated with
// the compiler or the interpreter.
static int eval_with_compiler_p(jl_array_t *body)
{
    size_t i;
    for(i=0; i < body->length; i++) {
        jl_value_t *stmt = jl_cellref(body,i);
        if (jl_is_expr(stmt) && (((jl_expr_t*)stmt)->head == goto_sym ||
                                 ((jl_expr_t*)stmt)->head == goto_ifnot_sym)) {
            return 1;
        }
    }
    return 0;
}

jl_value_t *jl_new_closure_internal(jl_lambda_info_t *li, jl_value_t *env);

jl_value_t *jl_toplevel_eval_thunk(jl_lambda_info_t *thk)
{
    //jl_show(thk);
    //ios_printf(ios_stdout, "\n");
    assert(jl_typeof(thk) == (jl_type_t*)jl_lambda_info_type);
    assert(jl_is_expr(thk->ast));
    if (eval_with_compiler_p(jl_lam_body((jl_expr_t*)thk->ast))) {
        jl_value_t *thunk=NULL;
        jl_function_t *gf=NULL;
        JL_GC_PUSH(&thunk, &gf);
        thunk = jl_new_closure_internal(thk, (jl_value_t*)jl_null);
        // use a generic function so type inference runs
        gf = jl_new_generic_function(lambda_sym);
        jl_add_method(gf, jl_null, (jl_function_t*)thunk);
        jl_value_t *result = jl_apply(gf, NULL, 0);
        JL_GC_POP();
        return result;
    }
    return jl_interpret_toplevel_thunk(thk);
}

int asprintf(char **strp, const char *fmt, ...);

// load toplevel expressions, from (file ...)
void jl_load_file_expr(char *fname, jl_value_t *ast)
{
    jl_array_t *b = ((jl_expr_t*)ast)->args;
    size_t i;
    volatile size_t lineno=0;
    JL_TRY {
        for(i=0; i < b->length; i++) {
            // process toplevel form
            jl_value_t *form = jl_cellref(b, i);
            if (jl_is_expr(form) && ((jl_expr_t*)form)->head == line_sym) {
                lineno = jl_unbox_int32(jl_exprarg(form, 0));
            }
            else {
                jl_lambda_info_t *lam = (jl_lambda_info_t*)form;
                (void)jl_interpret_toplevel_thunk(lam);
            }
        }
    }
    JL_CATCH {
        jl_value_t *fn=NULL, *ln=NULL;
        JL_GC_PUSH(&fn, &ln);
        fn = jl_pchar_to_string(fname, strlen(fname));
        ln = jl_box_int32(lineno);
        jl_raise(jl_new_struct(jl_loaderror_type, fn, ln,
                               jl_exception_in_transit));
    }
}

void jl_load(const char *fname)
{
    char *fpath = (char*)fname;
    int fid = open (fpath, O_RDONLY);
    // try adding just .j
    if (fid == -1) {
        asprintf(&fpath, "%s.j", fname);
        fid = open (fpath, O_RDONLY);
    }
    // try adding julia home and .j
    if (fid == -1 && julia_home && fname[0] != '/') {
        asprintf(&fpath, "%s/%s", julia_home, fname);
        fid = open (fpath, O_RDONLY);
        if (fid == -1) {
            asprintf(&fpath, "%s/%s.j", julia_home, fname);
            fid = open (fpath, O_RDONLY);
            if (fid == -1) {
                jl_errorf("could not open file %s", fpath);
            }
        }
    }
    close(fid);

    jl_value_t *ast = jl_parse_file(fpath);
    if (ast == (jl_value_t*)jl_null) 
	jl_errorf("could not open file %s", fpath);

    JL_GC_PUSH(&ast);
    jl_load_file_expr(fpath, ast);
    JL_GC_POP();
    if (fpath != fname) free(fpath);
}

JL_CALLABLE(jl_f_load)
{
    JL_NARGS(load, 1, 1);
    if (!jl_is_byte_string(args[0]))
        jl_error("load: expected Latin1String or UTF8String");
    char *fname = jl_string_data(args[0]);
    jl_load(fname);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_top_eval)
{
    JL_NARGS(eval, 1, 1);
    jl_value_t *e = args[0];
    if (!jl_is_expr(e))
        return jl_interpret_toplevel_expr(e);
    jl_expr_t *ex = (jl_expr_t*)e;
    if (ex->head == symbol_sym || ex->head == top_sym ||
        ex->head == quote_sym || ex->head == null_sym ||
        ex->head == unbound_sym) {
        // expression types simple enough not to need expansion
        return jl_interpret_toplevel_expr(e);
    }
    jl_lambda_info_t *exex = NULL;
    JL_GC_PUSH(&exex);
    exex = jl_expand(e);
    jl_value_t *result = jl_interpret_toplevel_thunk(exex);
    JL_GC_POP();
    return result;
}

JL_CALLABLE(jl_f_isbound)
{
    JL_NARGS(isbound, 1, 1);
    JL_TYPECHK(isbound, symbol, args[0]);
    return jl_boundp(jl_system_module, (jl_sym_t*)args[0]) ? jl_true : jl_false;
}

JL_CALLABLE(jl_f_tuple)
{
    size_t i;
    if (nargs == 0) return (jl_value_t*)jl_null;
    jl_tuple_t *t = jl_alloc_tuple_uninit(nargs);
    for(i=0; i < nargs; i++) {
        jl_tupleset(t, i, args[i]);
    }
    return (jl_value_t*)t;
}

JL_CALLABLE(jl_f_tupleref)
{
    JL_NARGS(tupleref, 2, 2);
    JL_TYPECHK(tupleref, tuple, args[0]);
    JL_TYPECHK(tupleref, int32, args[1]);
    jl_tuple_t *t = (jl_tuple_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= t->length)
        jl_error("tupleref: index out of range");
    return jl_tupleref(t, i);
}

JL_CALLABLE(jl_f_tuplelen)
{
    JL_NARGS(tuplelen, 1, 1);
    JL_TYPECHK(tuplelen, tuple, args[0]);
    return jl_box_int32(((jl_tuple_t*)args[0])->length);
}

static size_t field_offset(jl_struct_type_t *t, jl_sym_t *fld, int err)
{
    jl_tuple_t *fn = t->names;
    size_t i;
    for(i=0; i < fn->length; i++) {
        if (jl_tupleref(fn,i) == (jl_value_t*)fld) {
            if (t == jl_struct_kind || t == jl_bits_kind || t == jl_tag_kind)
                i += 3;
            return i;
        }
    }
    if (err)
        jl_errorf("type %s has no field %s", t->name->name->name, fld->name);
    return -1;
}

size_t jl_field_offset(jl_struct_type_t *t, jl_sym_t *fld)
{
    return field_offset(t, fld, 0);
}

JL_CALLABLE(jl_f_get_field)
{
    JL_NARGS(getfield, 2, 2);
    JL_TYPECHK(getfield, symbol, args[1]);
    jl_value_t *v = args[0];
    if (!jl_is_struct_type(jl_typeof(v)))
        // TODO: string is leaked
        jl_errorf("getfield: argument must be a struct, got %s",
                  jl_show_to_string(v));
    size_t i = field_offset((jl_struct_type_t*)jl_typeof(v),
                            (jl_sym_t*)args[1], 1);
    return ((jl_value_t**)v)[1+i];
}

JL_CALLABLE(jl_f_set_field)
{
    JL_NARGS(setfield, 3, 3);
    JL_TYPECHK(setfield, symbol, args[1]);
    jl_value_t *v = args[0];
    if (!jl_is_struct_type(jl_typeof(v)))
        // TODO: string is leaked
        jl_errorf("setfield: argument must be a struct, got %s",
                  jl_show_to_string(v));
    jl_struct_type_t *st = (jl_struct_type_t*)jl_typeof(v);
    size_t i = field_offset(st, (jl_sym_t*)args[1], 1);
    ((jl_value_t**)v)[1+i] = jl_convert((jl_type_t*)jl_tupleref(st->types,i),
                                        args[2]);
    return v;
}

JL_CALLABLE(jl_f_arraylen)
{
    JL_NARGS(arraylen, 1, 1);
    JL_TYPECHK(arraylen, array, args[0]);
    return jl_box_int32(((jl_array_t*)args[0])->length);
}

static jl_value_t *new_scalar(jl_bits_type_t *bt)
{
    size_t nb = jl_bitstype_nbits(bt)/8;
    jl_value_t *v = (jl_value_t*)allocb((NWORDS(LLT_ALIGN(nb,sizeof(void*)))+1)*
                                        sizeof(void*));
    v->type = (jl_type_t*)bt;
    return v;
}

jl_value_t *jl_arrayref(jl_array_t *a, size_t i)
{
    jl_type_t *el_type = (jl_type_t*)jl_tparam0(jl_typeof(a));
    jl_value_t *elt;
    if (jl_is_bits_type(el_type)) {
        if (el_type == (jl_type_t*)jl_bool_type) {
            if (((int8_t*)a->data)[i] != 0)
                return jl_true;
            return jl_false;
        }
        elt = new_scalar((jl_bits_type_t*)el_type);
        size_t nb = jl_bitstype_nbits(el_type)/8;
        switch (nb) {
        case 1:
            *(int8_t*)jl_bits_data(elt)  = ((int8_t*)a->data)[i];  break;
        case 2:
            *(int16_t*)jl_bits_data(elt) = ((int16_t*)a->data)[i]; break;
        case 4:
            *(int32_t*)jl_bits_data(elt) = ((int32_t*)a->data)[i]; break;
        case 8:
            *(int64_t*)jl_bits_data(elt) = ((int64_t*)a->data)[i]; break;
        default:
            memcpy(jl_bits_data(elt), &((char*)a->data)[i*nb], nb);
        }
    }
    else {
        elt = ((jl_value_t**)a->data)[i];
        if (elt == NULL)
            jl_errorf("array[%d]: uninitialized reference error", i+1);
    }
    return elt;
}

JL_CALLABLE(jl_f_arrayref)
{
    JL_NARGS(arrayref, 2, 2);
    JL_TYPECHK(arrayref, array, args[0]);
    JL_TYPECHK(arrayref, int32, args[1]);
    jl_array_t *a = (jl_array_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= a->length)
        jl_errorf("array[%d]: index out of range", i+1);
    return jl_arrayref(a, i);
}

void jl_arrayset(jl_array_t *a, size_t i, jl_value_t *rhs)
{
    jl_value_t *el_type = jl_tparam0(jl_typeof(a));
    if (el_type != (jl_value_t*)jl_any_type) {
        if (!jl_subtype(rhs, el_type, 1))
            jl_type_error("arrayset", el_type, rhs);
    }
    if (jl_is_bits_type(el_type)) {
        size_t nb = jl_bitstype_nbits(el_type)/8;
        switch (nb) {
        case 1:
            ((int8_t*)a->data)[i]  = *(int8_t*)jl_bits_data(rhs);  break;
        case 2:
            ((int16_t*)a->data)[i] = *(int16_t*)jl_bits_data(rhs); break;
        case 4:
            ((int32_t*)a->data)[i] = *(int32_t*)jl_bits_data(rhs); break;
        case 8:
            ((int64_t*)a->data)[i] = *(int64_t*)jl_bits_data(rhs); break;
        default:
            memcpy(&((char*)a->data)[i*nb], jl_bits_data(rhs), nb);
        }
    }
    else {
        ((jl_value_t**)a->data)[i] = rhs;
    }
}

JL_CALLABLE(jl_f_arrayset)
{
    JL_NARGS(arrayset, 3, 3);
    JL_TYPECHK(arrayset, array, args[0]);
    JL_TYPECHK(arrayset, int32, args[1]);
    jl_array_t *b = (jl_array_t*)args[0];
    size_t i = jl_unbox_int32(args[1])-1;
    if (i >= b->length)
        jl_errorf("array[%d]: index out of range", i+1);
    jl_arrayset(b, i, args[2]);
    return args[0];
}

// --- conversions ---

static jl_tuple_t *convert_tuple(jl_tuple_t *to, jl_tuple_t *x)
{
    if (to == jl_tuple_type)
        return x;
    size_t i, cl=x->length, pl=to->length;
    jl_tuple_t *out = jl_alloc_tuple(cl);
    JL_GC_PUSH(&out);
    jl_value_t *ce, *pe;
    int pseq=0;
    for(i=0; i < cl; i++) {
        ce = jl_tupleref(x,i);
        if (pseq) {
        }
        else if (i < pl) {
            pe = jl_tupleref(to,i);
            if (jl_is_seq_type(pe)) {
                pe = jl_tparam0(pe);
                pseq = 1;
            }
        }
        else {
            out = NULL;
            break;
        }
        jl_tupleset(out, i, jl_convert((jl_type_t*)pe, ce));
    }
    JL_GC_POP();
    return out;
}

jl_function_t *jl_convert_gf;

jl_value_t *jl_convert(jl_type_t *to, jl_value_t *x)
{
    jl_value_t *args[2];
    args[0] = (jl_value_t*)to; args[1] = x;
    return jl_apply(jl_convert_gf, args, 2);
}

JL_CALLABLE(jl_f_convert)
{
    JL_NARGS(convert, 2, 2);
    if (!jl_is_typector(args[0]))
        JL_TYPECHK(convert, type, args[0]);
    jl_type_t *to = (jl_type_t*)args[0];
    jl_value_t *x = args[1];
    jl_value_t *out;
    if (jl_is_tuple(x) && jl_is_tuple(to)) {
        out = (jl_value_t*)convert_tuple((jl_tuple_t*)to, (jl_tuple_t*)x);
        if (out == NULL)
            jl_error("convert: invalid tuple conversion");
        return out;
    }
    if (jl_subtype(x, (jl_value_t*)to, 1))
        return x;
    // TODO: string is leaked
    jl_errorf("cannot convert %s to %s",
              jl_show_to_string(x), jl_show_to_string((jl_value_t*)to));
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_convert_to_ptr)
{
    JL_NARGS(convert, 2, 2);
    assert(jl_is_cpointer_type(args[0]));
    jl_value_t *v = args[1];
    jl_value_t *elty = jl_tparam0(args[0]);
    void *p=NULL;
    if (v == (jl_value_t*)jl_null) {
        p = NULL;
    }
    else if (jl_is_cpointer(v)) {
        p = jl_unbox_pointer(v);
    }
    else if (jl_is_array(v) && jl_tparam0(jl_typeof(v)) == elty) {
        p = ((jl_array_t*)v)->data;
    }
    else {
        // TODO: string is leaked
        jl_errorf("cannot convert %s to %s",
                  jl_show_to_string(v), jl_show_to_string(args[0]));
    }
    return jl_box_pointer((jl_bits_type_t*)args[0], p);
}

// --- printing ---

jl_function_t *jl_print_gf;

JL_CALLABLE(jl_f_print_array_uint8)
{
    ios_t *s = jl_current_output_stream();
    jl_array_t *b = (jl_array_t*)args[0];
    ios_write(s, (char*)b->data, b->length);
    return (jl_value_t*)jl_null;
}

// --- showing ---

jl_function_t *jl_show_gf;

void jl_show(jl_value_t *v)
{
    jl_apply(jl_show_gf, &v, 1);
}

char *jl_show_to_string(jl_value_t *v)
{
    ios_t dest;
    ios_mem(&dest, 0);
    // use try/catch to reset the current output stream
    // if an error occurs during printing.
    JL_TRY {
        jl_set_current_output_stream(&dest);
        jl_show(v);
    }
    JL_CATCH {
        jl_raise(jl_exception_in_transit);
    }
    size_t n;
    return ios_takebuf(&dest, &n);
}

// comma_one prints a comma for 1 element, e.g. "(x,)"
static void show_tuple(jl_tuple_t *t, char opn, char cls, int comma_one)
{
    ios_t *s = jl_current_output_stream();
    ios_putc(opn, s);
    size_t i, n=t->length;
    for(i=0; i < n; i++) {
        jl_show(jl_tupleref(t, i));
        if ((i < n-1) || (n==1 && comma_one))
            ios_putc(',', s);
    }
    ios_putc(cls, s);
}

static void show_type(jl_value_t *t)
{
    ios_t *s = jl_current_output_stream();
    if (jl_is_func_type(t)) {
        jl_show((jl_value_t*)((jl_func_type_t*)t)->from);
        ios_write(s, "-->", 3);
        jl_show((jl_value_t*)((jl_func_type_t*)t)->to);
    }
    else if (jl_is_union_type(t)) {
        if (t == (jl_value_t*)jl_bottom_type) {
            ios_write(s, "None", 4);
        }
        else {
            ios_write(s, "Union", 5);
            show_tuple(((jl_uniontype_t*)t)->types, '(', ')', 0);
        }
    }
    else if (jl_is_seq_type(t)) {
        jl_show(jl_tparam0(t));
        ios_write(s, "...", 3);
    }
    else {
        assert(jl_is_some_tag_type(t));
        ios_puts(((jl_tag_type_t*)t)->name->name->name, s);
        jl_tuple_t *p = ((jl_tag_type_t*)t)->parameters;
        if (p->length > 0)
            show_tuple(p, '{', '}', 0);
    }
}

static void show_function(jl_value_t *v)
{
    ios_t *s = jl_current_output_stream();
    if (jl_is_gf(v)) {
        ios_puts("Methods for generic function ", s);
        ios_puts(jl_gf_name(v)->name, s);
        ios_putc('\n', s);
        jl_show_method_table((jl_function_t*)v);
    }
    else {
        ios_puts("#<closure>", s);
    }
}

static void show_int(void *data, int nbits)
{
    ios_t *s = jl_current_output_stream();
    switch (nbits) {
    case 8:
        ios_printf(s, "%hhd", *(int8_t*)data);
        break;
    case 16:
        ios_printf(s, "%hd", *(int16_t*)data);
        break;
    case 32:
        ios_printf(s, "%d", *(int32_t*)data);
        break;
    case 64:
        ios_printf(s, "%lld", *(int64_t*)data);
        break;
    default:
        jl_error("print: unsupported integer size");
    }
}

static void show_uint(void *data, int nbits)
{
    ios_t *s = jl_current_output_stream();
    switch (nbits) {
    case 8:
        ios_printf(s, "%hhu", *(int8_t*)data);
        break;
    case 16:
        ios_printf(s, "%hu", *(int16_t*)data);
        break;
    case 32:
        ios_printf(s, "%u", *(int32_t*)data);
        break;
    case 64:
        ios_printf(s, "%llu", *(int64_t*)data);
        break;
    default:
        jl_error("print: unsupported integer size");
    }
}

static void show_float64(double d, int single)
{
    ios_t *s = jl_current_output_stream();
    char buf[64];
    int ndec = single ? 8 : 16;
    if (!DFINITE(d)) {
        char *rep;
        if (isnan(d))
            rep = sign_bit(d) ? "-NaN" : "NaN";
        else
            rep = sign_bit(d) ? "-Inf" : "Inf";
        if (single)
            ios_printf(s, "float32(%s)", rep);
        else
            ios_puts(rep, s);
    }
    else if (d == 0) {
        if (1/d < 0)
            ios_puts("-0.0", s);
        else
            ios_puts("0.0", s);
    }
    else {
        snprint_real(buf, sizeof(buf), d, 0, ndec, 3, 10);
        int hasdec = (strpbrk(buf, ".eE") != NULL);
	    ios_puts(buf, s);
        if (!hasdec) ios_puts(".0", s);
    }
}

JL_CALLABLE(jl_f_show_bool)
{
    ios_t *s = jl_current_output_stream();
    if (jl_unbox_bool(args[0]) == 0)
        ios_puts("false", s);
    else
        ios_puts("true", s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_char)
{
    ios_t *s = jl_current_output_stream();
    u_int32_t wc = *(uint32_t*)jl_bits_data(args[0]);
    ios_putc('\'', s);
    ios_pututf8(s, wc);
    ios_putc('\'', s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_float32)
{
    show_float64((double)*(float*)jl_bits_data(args[0]), 1);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_float64)
{
    show_float64(*(double*)jl_bits_data(args[0]), 0);
    return (jl_value_t*)jl_null;
}

#define INT_SHOW_FUNC(sgn,nb)               \
JL_CALLABLE(jl_f_show_##sgn##nb)            \
{                                           \
    show_##sgn(jl_bits_data(args[0]), nb);  \
    return (jl_value_t*)jl_null;            \
}

INT_SHOW_FUNC(int,8)
INT_SHOW_FUNC(uint,8)
INT_SHOW_FUNC(int,16)
INT_SHOW_FUNC(uint,16)
INT_SHOW_FUNC(int,32)
INT_SHOW_FUNC(uint,32)
INT_SHOW_FUNC(int,64)
INT_SHOW_FUNC(uint,64)

JL_CALLABLE(jl_f_show_pointer)
{
    ios_t *s = jl_current_output_stream();
    void *ptr = *(void**)jl_bits_data(args[0]);
    if (jl_typeis(args[0],jl_pointer_void_type))
        ios_printf(s, "Ptr{Void}");
    else
        jl_show((jl_value_t*)jl_typeof(args[0]));
#ifdef BITS64
    ios_printf(s, " @0x%016x", (uptrint_t)ptr);
#else
    ios_printf(s, " @0x%08x", (uptrint_t)ptr);
#endif
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_symbol)
{
    ios_t *s = jl_current_output_stream();
    ios_puts(((jl_sym_t*)args[0])->name, s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_typevar)
{
    ios_t *s = jl_current_output_stream();
    jl_tvar_t *tv = (jl_tvar_t*)args[0];
    if (tv->lb != (jl_value_t*)jl_bottom_type) {
        jl_show((jl_value_t*)tv->lb);
        ios_puts("<:", s);
    }
    ios_puts(tv->name->name, s);
    if (tv->ub != (jl_value_t*)jl_any_type) {
        ios_puts("<:", s);
        jl_show((jl_value_t*)tv->ub);
    }
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_linfo)
{
    ios_t *s = jl_current_output_stream();
    ios_puts("AST(", s);
    jl_show(((jl_lambda_info_t*)args[0])->ast);
    ios_putc(')', s);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_show_any)
{
    JL_NARGS(print, 1, 1);
    // fallback for printing some other builtin types
    ios_t *s = jl_current_output_stream();
    jl_value_t *v = args[0];
    if (jl_is_tuple(v)) {
        show_tuple((jl_tuple_t*)v, '(', ')', 1);
    }
    else if (jl_is_type(v)) {
        show_type(v);
    }
    else if (jl_is_func(v)) {
        show_function(v);
    }
    else if (jl_typeis(v,jl_intrinsic_type)) {
        ios_printf(s, "#<intrinsic-function %d>", *(uint32_t*)jl_bits_data(v));
    }
    else if (v == (jl_value_t*)jl_function_type) {
        ios_printf(s, "Function");
    }
    else {
        jl_value_t *t = (jl_value_t*)jl_typeof(v);
        if (jl_is_bits_type(t)) {
            show_uint(jl_bits_data(v), jl_bitstype_nbits(t));
        }
        else {
            assert(jl_is_struct_type(t));
            jl_struct_type_t *st = (jl_struct_type_t*)t;
            ios_puts(st->name->name->name, s);
            ios_putc('(', s);
            size_t i;
            size_t n = st->names->length;
            for(i=0; i < n; i++) {
                jl_show(((jl_value_t**)v)[i+1]);
                if (i < n-1)
                    ios_putc(',', s);
            }
            ios_putc(')', s);
        }
    }
    return (jl_value_t*)jl_null;
}

// --- RTS primitives ---

JL_CALLABLE(jl_trampoline)
{
    jl_function_t *f = (jl_function_t*)jl_t0(env);
    assert(jl_is_func(f));
    assert(f->linfo != NULL);
    jl_compile(f);
    jl_generate_fptr(f);
    assert(f->fptr != NULL);
    return jl_apply(f, args, nargs);
}

jl_value_t *jl_new_closure_internal(jl_lambda_info_t *li, jl_value_t *env)
{
    assert(jl_is_lambda_info(li));
    assert(jl_is_tuple(env));
    jl_function_t *f=NULL;
    // note: env is pushed here to make codegen a little easier
    JL_GC_PUSH(&f, &env);
    if (li->fptr != NULL) {
        // function has been compiled
        f = jl_new_closure(li->fptr, env);
    }
    else {
        f = jl_new_closure(jl_trampoline, NULL);
        f->env = (jl_value_t*)jl_pair((jl_value_t*)f, env);
    }
    f->linfo = li;
    JL_GC_POP();
    return (jl_value_t*)f;
}

JL_CALLABLE(jl_f_instantiate_type)
{
    JL_NARGSV(instantiate_type, 1);
    if (!jl_is_some_tag_type(args[0]))
        JL_TYPECHK(instantiate_type, typector, args[0]);
    jl_tuple_t *tparams = (jl_tuple_t*)jl_f_tuple(NULL, &args[1], nargs-1);
    JL_GC_PUSH(&tparams);
    jl_value_t *v = jl_apply_type(args[0], tparams);
    JL_GC_POP();
    return v;
}

static int all_typevars(jl_tuple_t *p)
{
    size_t i;
    for(i=0; i < p->length; i++) {
        if (!jl_is_typevar(jl_tupleref(p,i)))
            return 0;
    }
    return 1;
}

static void check_supertype(jl_value_t *super, char *name)
{
    if (!(/*jl_is_struct_type(super) || */jl_is_tag_type(super)) ||
        super == (jl_value_t*)jl_sym_type ||
        super == (jl_value_t*)jl_undef_type ||
        jl_subtype(super,(jl_value_t*)jl_type_type,0) ||
        jl_subtype(super,(jl_value_t*)jl_array_type,0)) {
        jl_errorf("invalid subtyping in definition of %s", name);
    }
}

JL_CALLABLE(jl_f_new_struct_type)
{
    JL_NARGS(new_struct_type, 4, 4);
    JL_TYPECHK(new_struct_type, symbol, args[0]);
    JL_TYPECHK(new_struct_type, tuple, args[1]);
    JL_TYPECHK(new_struct_type, tuple, args[2]);
    if (args[3] != (jl_value_t*)jl_null)
        JL_TYPECHK(new_struct_type, function, args[3]);
    jl_sym_t *name = (jl_sym_t*)args[0];
    jl_tuple_t *params = (jl_tuple_t*)args[1];
    jl_tuple_t *fnames = (jl_tuple_t*)args[2];
    if (!all_typevars(params))
        jl_errorf("invalid type parameter list for %s", name->name);

    jl_struct_type_t *nst =
        jl_new_struct_type(name, jl_any_type, params, fnames, NULL);
    nst->ctor_factory = args[3];
    return (jl_value_t*)nst;
}

void jl_add_constructors(jl_struct_type_t *t);

JL_CALLABLE(jl_f_new_struct_fields)
{
    JL_NARGS(new_struct_fields, 3, 3);
    jl_value_t *super = args[1];
    JL_TYPECHK(new_struct_fields, tuple, args[2]);
    jl_value_t *t = args[0];
    jl_tuple_t *ftypes = (jl_tuple_t*)args[2];
    if (!jl_is_struct_type(t))
        jl_error("you can't do that.");
    jl_struct_type_t *st = (jl_struct_type_t*)t;
    if (st->types != NULL)
        jl_error("you can't do that.");
    jl_tuple_t *pft = NULL;
    jl_tuple_t *fnames = st->names;

    assert(jl_is_type(super));
    check_supertype(super, st->name->name->name);
    st->super = (jl_tag_type_t*)super;
    if (jl_is_struct_type(super)) {
        // UNUSED
        assert(0);
        st->names = jl_tuple_append(((jl_struct_type_t*)super)->names,
                                    fnames);
    }
    else {
        assert(jl_is_tag_type(super));
    }

    if (jl_is_struct_type(super))
        pft = ((jl_struct_type_t*)super)->types;
    else if (jl_is_tag_type(super))
        pft = jl_null;
    else
        assert(0);
    st->types = jl_tuple_append(pft, ftypes);
    jl_add_constructors(st);
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_new_type_constructor)
{
    JL_NARGS(new_type_constructor, 2, 2);
    JL_TYPECHK(new_type_constructor, tuple, args[0]);
    assert(jl_is_type(args[1]));
    jl_tuple_t *p = (jl_tuple_t*)args[0];
    if (!all_typevars(p)) {
        jl_errorf("invalid type parameter list in typealias");
    }
    return (jl_value_t*)jl_new_type_ctor(p, (jl_type_t*)args[1]);
}

JL_CALLABLE(jl_f_new_tag_type)
{
    JL_NARGS(new_tag_type, 2, 2);
    JL_TYPECHK(new_tag_type, symbol, args[0]);
    JL_TYPECHK(new_tag_type, tuple, args[1]);
    jl_tuple_t *p = (jl_tuple_t*)args[1];
    if (!all_typevars(p)) {
        jl_errorf("invalid type parameter list for %s",
                  ((jl_sym_t*)args[0])->name);
    }
    return (jl_value_t*)jl_new_tagtype((jl_value_t*)args[0], jl_any_type, p);
}

JL_CALLABLE(jl_f_new_tag_type_super)
{
    JL_NARGS(new_tag_type_super, 2, 2);
    JL_TYPECHK(new_tag_type_super, tag_type, args[1]);
    jl_value_t *super = args[1];
    check_supertype(super, ((jl_sym_t*)args[0])->name);
    ((jl_tag_type_t*)args[0])->super = (jl_tag_type_t*)super;
    return (jl_value_t*)jl_null;
}

JL_CALLABLE(jl_f_new_bits_type)
{
    JL_NARGS(new_bits_type, 3, 3);
    JL_TYPECHK(new_bits_type, symbol, args[0]);
    JL_TYPECHK(new_bits_type, tuple, args[1]);
    JL_TYPECHK(new_bits_type, int32, args[2]);
    jl_tuple_t *p = (jl_tuple_t*)args[1];
    if (!all_typevars(p)) {
        jl_errorf("invalid type parameter list for %s",
                  ((jl_sym_t*)args[0])->name);
    }
    int32_t nb = jl_unbox_int32(args[2]);
    if (nb != 8 && nb != 16 && nb != 32 && nb != 64 && nb != 80 && nb != 128)
        jl_errorf("invalid number of bits in type %s",
                  ((jl_sym_t*)args[0])->name);
    return (jl_value_t*)jl_new_bitstype((jl_value_t*)args[0], jl_any_type, p,
                                        nb);
}

JL_CALLABLE(jl_f_typevar)
{
    if (nargs < 1 || nargs > 3) {
        JL_NARGS(typevar, 1, 1);
    }
    JL_TYPECHK(typevar, symbol, args[0]);
    jl_value_t *lb = (jl_value_t*)jl_bottom_type;
    jl_value_t *ub = (jl_value_t*)jl_any_type;
    if (nargs > 1) {
        if (jl_is_typector(args[1])) {
            lb = (jl_value_t*)((jl_typector_t*)args[1])->body;
        }
        else {
            JL_TYPECHK(typevar, type, args[1]);
            lb = args[1];
        }
        if (nargs > 2) {
            if (jl_is_typector(args[2])) {
                ub = (jl_value_t*)((jl_typector_t*)args[2])->body;
            }
            else {
                JL_TYPECHK(typevar, type, args[2]);
                ub = args[2];
            }
        }
        else {
            // typevar(name, UB)
            ub = lb;
            lb = (jl_value_t*)jl_bottom_type;
        }
    }
    return (jl_value_t*)jl_new_typevar((jl_sym_t*)args[0], lb, ub);
}

JL_CALLABLE(jl_f_union)
{
    if (nargs == 0) return (jl_value_t*)jl_bottom_type;
    if (nargs == 1) return args[0];
    size_t i;
    jl_tuple_t *argt = jl_alloc_tuple_uninit(nargs);
    for(i=0; i < nargs; i++) {
        if (jl_is_typector(args[i])) {
            jl_tupleset(argt, i, (jl_value_t*)((jl_typector_t*)args[i])->body);
        }
        else if (!jl_is_type(args[i]) && !jl_is_typevar(args[i])) {
            jl_error("invalid union type");
        }
        else {
            jl_tupleset(argt, i, args[i]);
        }
    }
    JL_GC_PUSH(&argt);
    jl_value_t *u = jl_type_union(argt);
    JL_GC_POP();
    return u;
}

// --- generic function primitives ---

JL_CALLABLE(jl_f_new_generic_function)
{
    JL_NARGS(new_generic_function, 1, 1);
    JL_TYPECHK(new_generic_function, symbol, args[0]);
    return (jl_value_t*)jl_new_generic_function((jl_sym_t*)args[0]);
}

static void check_type_tuple(jl_tuple_t *t)
{
    size_t i;
    for(i=0; i < t->length; i++) {
        jl_value_t *elt = jl_tupleref(t,i);
        if (!jl_is_type(elt) && !jl_is_typector(elt) && !jl_is_typevar(elt)) {
            // TODO: string is leaked
            char *argstr = jl_show_to_string(elt);
            jl_errorf("invalid type %s in method definition", argstr);
        }
    }
}

JL_CALLABLE(jl_f_add_method)
{
    JL_NARGS(add_method, 3, 3);
    assert(jl_is_function(args[0]));
    if (!jl_is_gf(args[0]))
        jl_error("add_method: not a generic function");
    JL_TYPECHK(add_method, tuple, args[1]);
    check_type_tuple((jl_tuple_t*)args[1]);
    JL_TYPECHK(add_method, function, args[2]);
    jl_add_method((jl_function_t*)args[0], (jl_tuple_t*)args[1],
                  (jl_function_t*)args[2]);
    return args[0];
}

// --- generic function reflection ---

jl_function_t *jl_method_table_assoc(jl_methtable_t *mt,
                                     jl_value_t **args, size_t nargs, int t);

JL_CALLABLE(jl_f_methodexists)
{
    JL_NARGS(method_exists, 2, 2);
    JL_TYPECHK(method_exists, function, args[0]);
    if (!jl_is_gf(args[0]))
        jl_error("method_exists: not a generic function");
    JL_TYPECHK(method_exists, tuple, args[1]);
    check_type_tuple((jl_tuple_t*)args[1]);
    return jl_method_table_assoc(jl_gf_mtable(args[0]),
                                 &jl_tupleref(args[1],0),
                                 ((jl_tuple_t*)args[1])->length, 0) ?
        jl_true : jl_false;
}

JL_CALLABLE(jl_f_invoke)
{
    JL_NARGSV(invoke, 2);
    JL_TYPECHK(invoke, function, args[0]);
    if (!jl_is_gf(args[0]))
        jl_error("invoke: not a generic function");
    JL_TYPECHK(invoke, tuple, args[1]);
    check_type_tuple((jl_tuple_t*)args[1]);
    if (!jl_tuple_subtype(&args[2], nargs-2, &jl_tupleref(args[1],0),
                          ((jl_tuple_t*)args[1])->length, 1, 0))
        jl_error("invoke: argument type error");
    jl_function_t *mlfunc =
        jl_method_table_assoc(jl_gf_mtable(args[0]),
                              &jl_tupleref(args[1],0),
                              ((jl_tuple_t*)args[1])->length, 0);
    if (mlfunc == NULL)
        jl_no_method_error(jl_gf_name(args[0]), &args[2], nargs-2);
    return jl_apply(mlfunc, &args[2], nargs-2);
}

// --- c interface ---

JL_CALLABLE(jl_f_dlopen)
{
    JL_NARGS(dlopen, 1, 1);
    if (!jl_is_byte_string(args[0]))
        jl_error("dlopen: expected Latin1String or UTF8String");
    char *fname = jl_string_data(args[0]);
    return jl_box_pointer(jl_pointer_void_type,
                          jl_load_dynamic_library(fname));
}

JL_CALLABLE(jl_f_dlsym)
{
    JL_NARGS(dlsym, 2, 2);
    JL_TYPECHK(dlsym, pointer, args[0]);
    char *sym=NULL;
    if (jl_is_symbol(args[1]))
        sym = ((jl_sym_t*)args[1])->name;
    else if (jl_is_byte_string(args[1]))
        sym = jl_string_data(args[1]);
    else
        jl_error("dlsym: expected Latin1String or UTF8String");
    void *hnd = jl_unbox_pointer(args[0]);
    return jl_box_pointer(jl_pointer_void_type, jl_dlsym(hnd, sym));
}

// --- eq hash table ---

#include "table.c"

// --- hashing ---

jl_function_t *jl_hash_gf;

JL_CALLABLE(jl_f_hash_symbol)
{
#ifdef BITS64
    return jl_box_uint64(((jl_sym_t*)args[0])->hash);
#else
    return jl_box_uint32(((jl_sym_t*)args[0])->hash);
#endif
}

// --- init ---

static void add_builtin_method1(jl_function_t *gf, jl_type_t *t, jl_fptr_t f)
{
    jl_add_method(gf, jl_tuple(1, t), jl_new_closure(f, NULL));
}

static void add_builtin(const char *name, jl_value_t *v)
{
    jl_set_const(jl_system_module, jl_symbol(name), v);
}

static void add_builtin_func(const char *name, jl_fptr_t f)
{
    add_builtin(name, (jl_value_t*)jl_new_closure(f, NULL));
}

void jl_add_builtin_func(const char *name, jl_fptr_t f)
{
    return add_builtin_func(name, f);
}

void jl_add_builtin(const char *name, jl_value_t *v)
{
    return add_builtin(name, v);
}

void jl_init_primitives()
{
    add_builtin_func("is", jl_f_is);
    add_builtin_func("typeof", jl_f_typeof);
    add_builtin_func("subtype", jl_f_subtype);
    add_builtin_func("isa", jl_f_isa);
    add_builtin_func("typeassert", jl_f_typeassert);
    add_builtin_func("apply", jl_f_apply);
    add_builtin_func("throw", jl_f_throw);
    add_builtin_func("load", jl_f_load);
    add_builtin_func("tuple", jl_f_tuple);
    add_builtin_func("Union", jl_f_union);
    add_builtin_func("method_exists", jl_f_methodexists);
    add_builtin_func("invoke", jl_f_invoke);
    add_builtin_func("dlopen", jl_f_dlopen);
    add_builtin_func("dlsym", jl_f_dlsym);
    add_builtin_func("eval", jl_f_top_eval);
    add_builtin_func("isbound", jl_f_isbound);
    
    // functions for internal use
    add_builtin_func("tupleref", jl_f_tupleref);
    add_builtin_func("tuplelen", jl_f_tuplelen);
    add_builtin_func("getfield", jl_f_get_field);
    add_builtin_func("setfield", jl_f_set_field);
    add_builtin_func("arraylen", jl_f_arraylen);
    add_builtin_func("arrayref", jl_f_arrayref);
    add_builtin_func("arrayset", jl_f_arrayset);
    add_builtin_func("instantiate_type", jl_f_instantiate_type);
    add_builtin_func("typevar", jl_f_typevar);
    add_builtin_func("new_struct_type", jl_f_new_struct_type);
    add_builtin_func("new_struct_fields", jl_f_new_struct_fields);
    add_builtin_func("new_type_constructor", jl_f_new_type_constructor);
    add_builtin_func("new_tag_type", jl_f_new_tag_type);
    add_builtin_func("new_tag_type_super", jl_f_new_tag_type_super);
    add_builtin_func("new_bits_type", jl_f_new_bits_type);
    add_builtin_func("new_generic_function", jl_f_new_generic_function);
    add_builtin_func("add_method", jl_f_add_method);
    add_builtin_func("trycatch", jl_f_trycatch);

    // builtin types
    add_builtin("Any", (jl_value_t*)jl_any_type);
    add_builtin("None", (jl_value_t*)jl_bottom_type);
    add_builtin("Void", (jl_value_t*)jl_bottom_type);
    add_builtin("TypeVar", (jl_value_t*)jl_tvar_type);
    add_builtin("TypeName", (jl_value_t*)jl_typename_type);
    add_builtin("TypeConstructor", (jl_value_t*)jl_typector_type);
    add_builtin("Tuple", (jl_value_t*)jl_tuple_type);
    add_builtin("NTuple", (jl_value_t*)jl_ntuple_type);
    add_builtin("Type", (jl_value_t*)jl_type_type);
    add_builtin("Symbol", (jl_value_t*)jl_sym_type);
    add_builtin("...", (jl_value_t*)jl_seq_type);
    add_builtin("Function", (jl_value_t*)jl_functype_ctor);
    add_builtin("Tensor", (jl_value_t*)jl_tensor_type);
    add_builtin("Array", (jl_value_t*)jl_array_type);

    add_builtin("Expr", (jl_value_t*)jl_expr_type);
    add_builtin("Ptr", (jl_value_t*)jl_pointer_type);
    add_builtin("LambdaStaticData", (jl_value_t*)jl_lambda_info_type);
    add_builtin("Box", (jl_value_t*)jl_box_type);
    add_builtin("IntrinsicFunction", (jl_value_t*)jl_intrinsic_type);
    // todo: this should only be visible to compiler components
    add_builtin("Undef", (jl_value_t*)jl_undef_type);

    add_builtin("BitsKind", (jl_value_t*)jl_bits_kind);
    add_builtin("StructKind", (jl_value_t*)jl_struct_kind);
    add_builtin("FuncKind", (jl_value_t*)jl_func_kind);
    add_builtin("TagKind", (jl_value_t*)jl_tag_kind);
    add_builtin("UnionKind", (jl_value_t*)jl_union_kind);

    add_builtin("JuliaDLHandle", jl_box_pointer(jl_pointer_void_type,
                                                jl_load_dynamic_library(NULL)));
    add_builtin("C_NULL", jl_box_pointer(jl_pointer_void_type, NULL));
}

void jl_init_builtins()
{
    jl_print_gf = jl_new_generic_function(jl_symbol("print"));

    add_builtin_method1(jl_print_gf,
                        (jl_type_t*)jl_array_uint8_type,
                        jl_f_print_array_uint8);

    jl_show_gf = jl_new_generic_function(jl_symbol("show"));

    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_any_type,         jl_f_show_any);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_sym_type,         jl_f_show_symbol);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_tvar_type,        jl_f_show_typevar);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_lambda_info_type, jl_f_show_linfo);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_float32_type,     jl_f_show_float32);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_float64_type,     jl_f_show_float64);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_int8_type,        jl_f_show_int8);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_uint8_type,       jl_f_show_uint8);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_int16_type,       jl_f_show_int16);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_uint16_type,      jl_f_show_uint16);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_int32_type,       jl_f_show_int32);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_uint32_type,      jl_f_show_uint32);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_int64_type,       jl_f_show_int64);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_uint64_type,      jl_f_show_uint64);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_bool_type,        jl_f_show_bool);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_char_type,        jl_f_show_char);
    add_builtin_method1(jl_show_gf, (jl_type_t*)jl_pointer_type,     jl_f_show_pointer);

    jl_convert_gf = jl_new_generic_function(jl_symbol("convert"));
    jl_add_method(jl_convert_gf,
                  jl_tuple(2, jl_any_type, jl_any_type),
                  jl_new_closure(jl_f_convert, NULL));
    jl_add_method(jl_convert_gf,
                  jl_tuple(2, jl_wrap_Type((jl_value_t*)jl_pointer_type), jl_any_type),
                  jl_new_closure(jl_f_convert_to_ptr, NULL));

    jl_hash_gf = jl_new_generic_function(jl_symbol("hash"));

    add_builtin_method1(jl_hash_gf, (jl_type_t*)jl_sym_type, jl_f_hash_symbol);

    add_builtin("print",    (jl_value_t*)jl_print_gf);
    add_builtin("show",     (jl_value_t*)jl_show_gf);
    add_builtin("convert",  (jl_value_t*)jl_convert_gf);
    add_builtin("hash",     (jl_value_t*)jl_hash_gf);
}
