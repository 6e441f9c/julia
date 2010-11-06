namespace JL_I {
    enum intrinsic {
        // wrap and unwrap
        boxui8=0, boxsi8, boxui16, boxsi16, boxui32, boxsi32, boxui64, boxsi64,
        boxf32, boxf64, boxf80, boxf128, box,
        unbox8, unbox16, unbox32, unbox64, unbox80, unbox128, unbox,
        // arithmetic
        neg_int, add_int, sub_int, mul_int,
        sdiv_int, udiv_int, srem_int, urem_int,
        neg_float, add_float, sub_float, mul_float, div_float, rem_float,
        // comparison
        eq_int, ne_int,
        slt_int, ult_int,
        sle_int, ule_int,
        sgt_int, ugt_int,
        sge_int, uge_int,
        eq_float, ne_float,
        lt_float, le_float,
        gt_float, ge_float,
        // bitwise operators
        and_int, or_int, xor_int, not_int, shl_int, lshr_int, ashr_int,
        bswap_int,
        // conversion
        sext16, zext16, sext32, zext32, sext64, zext64,
        trunc8, trunc16, trunc32,
        fptoui8, fptosi8, fptoui16, fptosi16, fptoui32, fptosi32,
        fptoui64, fptosi64,
        uitofp32, sitofp32, uitofp64, sitofp64,
        uitofp80, sitofp80, uitofp128, sitofp128,
        fptrunc32, fptrunc64, fptrunc80,
        fpext64, fpext80, fpext128,
        // functions
        sqrt_float, powi_float, sin_float, cos_float, pow_float,
        // c interface
        ccall,
    };
};

using namespace JL_I;

static Function *box_int8_func;
static Function *box_uint8_func;
static Function *box_int16_func;
static Function *box_uint16_func;
static Function *box_int32_func;
static Function *box_uint32_func;
static Function *box_int64_func;
static Function *box_uint64_func;
static Function *box_float32_func;
static Function *box_float64_func;
static Function *box_float80_func;
static Function *box_float128_func;
static Function *box_pointer_func;
static Function *box8_func;
static Function *box16_func;
static Function *box32_func;
static Function *box64_func;
static Function *box80_func;
static Function *box128_func;

/*
  low-level intrinsics design:
  functions like add_int expect unboxed values of matching bit-length.
  every operation that can return an unboxed value does so.
  this maximizes opportunities for composing functions without
    unnecessary boxing.
  this means that box and unbox functions might do nothing except change
    the type tag of a value.
  boxing is delayed until absolutely necessary, and handled at the point
    where the box is needed.
*/

// scheme for tagging llvm values with julia types

static std::map<int, jl_value_t*> typeIdToType;
static std::map<jl_value_t*, int> typeToTypeId;
static int cur_type_id = 1;

static int jl_type_to_typeid(jl_value_t *t)
{
    std::map<jl_value_t*, int>::iterator it = typeToTypeId.find(t);
    if (it == typeToTypeId.end()) {
        int mine = cur_type_id++;
        if (mine > 65025)
            jl_error("unexpected error: too many bits types");
        typeToTypeId[t] = mine;
        typeIdToType[mine] = t;
        return mine;
    }
    return (*it).second;
}

static jl_value_t *jl_typeid_to_type(int i)
{
    std::map<int, jl_value_t*>::iterator it = typeIdToType.find(i);
    if (it == typeIdToType.end()) {
        jl_error("unexpected error: invalid type id");
    }
    return (*it).second;
}

static bool has_julia_type(Value *v)
{
    return ((dynamic_cast<Instruction*>(v) != NULL) &&
            ((Instruction*)v)->hasMetadata());
}

static jl_value_t *julia_type_of_without_metadata(Value *v)
{
    if (dynamic_cast<AllocaInst*>(v) != NULL &&
        v->getType() != jl_pvalue_llvmt) {
        // an alloca always has llvm type pointer
        return llvm_type_to_julia(v->getType()->getContainedType(0));
    }
    return llvm_type_to_julia(v->getType());
}

static jl_value_t *julia_type_of(Value *v)
{
    if (dynamic_cast<Instruction*>(v) == NULL ||
        !((Instruction*)v)->hasMetadata()) {
        return julia_type_of_without_metadata(v);
    }
    MDNode *mdn = ((Instruction*)v)->getMetadata((unsigned int)0);
    MDString *md = (MDString*)mdn->getOperand(0);
    const char *vts = md->getString().data();
    int id = (vts[0]-1) + (vts[1]-1)*255;
    return jl_typeid_to_type(id);
}

// see if a julia type maps directly to an llvm type
static bool is_julia_type_representable(jl_value_t *jt)
{
    return (jt == (jl_value_t*)jl_bool_type ||
            jt == (jl_value_t*)jl_int8_type ||
            jt == (jl_value_t*)jl_int16_type ||
            jt == (jl_value_t*)jl_int32_type ||
            jt == (jl_value_t*)jl_int64_type ||
            jt == (jl_value_t*)jl_float32_type ||
            jt == (jl_value_t*)jl_float64_type ||
            jt == (jl_value_t*)jl_float80_type ||
            jt == (jl_value_t*)jl_float128_type ||
            (jl_is_cpointer_type(jt) &&
             is_julia_type_representable(jl_tparam0(jt))));
}

static Value *NoOpCast(Value *v)
{
    v = CastInst::Create(Instruction::BitCast, v, v->getType());
    builder.Insert((Instruction*)v);
    return v;
}

static Value *mark_julia_type(Value *v, jl_value_t *jt)
{
    if (has_julia_type(v) && julia_type_of(v) == jt)
        return v;
    if (julia_type_of_without_metadata(v) == jt)
        return NoOpCast(v);
    if (dynamic_cast<Instruction*>(v) == NULL)
        v = NoOpCast(v);
    assert(dynamic_cast<Instruction*>(v));
    char name[3];
    int id = jl_type_to_typeid(jt);
    // store id as base-255 to avoid NUL
    name[0] = (id%255)+1;
    name[1] = (id/255)+1;
    name[2] = '\0';
    MDString *md = MDString::get(jl_LLVMContext, name);
    Value *const vals[1] = {md};
    MDNode *mdn = MDNode::get(jl_LLVMContext, vals, 1);
    ((Instruction*)v)->setMetadata((unsigned int)0, mdn);
    return v;
}

static Value *mark_julia_type(Value *v, jl_bits_type_t *jt)
{
    return mark_julia_type(v, (jl_value_t*)jt);
}

// propagate julia type from value a to b. returns b.
static Value *tpropagate(Value *a, Value *b)
{
    if (has_julia_type(a))
        return mark_julia_type(b, julia_type_of(a));
    return b;
}

static const Type *julia_type_to_llvm(jl_value_t *jt)
{
    if (jt == (jl_value_t*)jl_bool_type) return T_int1;
    if (jt == (jl_value_t*)jl_float32_type) return T_float32;
    if (jt == (jl_value_t*)jl_float64_type) return T_float64;
    if (jt == (jl_value_t*)jl_float80_type) return T_float80;
    if (jt == (jl_value_t*)jl_float128_type) return T_float128;
    //if (jt == (jl_value_t*)jl_null) return T_void;
    if (jl_is_bits_type(jt) && jl_is_cpointer_type(jt)) {
        const Type *lt = julia_type_to_llvm(jl_tparam0(jt));
        if (lt == T_void)
            lt = T_int8;
        return PointerType::get(lt, 0);
    }
    if (jl_is_bits_type(jt)) {
        int nb = jl_bitstype_nbits(jt);
        if (nb == 8)  return T_int8;
        if (nb == 16) return T_int16;
        if (nb == 32) return T_int32;
        if (nb == 64) return T_int64;
    }
    if (jt == (jl_value_t*)jl_any_type)
        return jl_pvalue_llvmt;
    if (jt == (jl_value_t*)jl_bottom_type) return T_void;
    jl_errorf("cannot convert type %s to a native type",
              jl_show_to_string(jt));
    return NULL;
}

// NOTE: llvm cannot express all julia types (for example unsigned),
// so this is an approximation. it's only correct if the associated LLVM
// value is not tagged with our value name hack.
// boxed(v) below gets the correct type.
static jl_value_t *llvm_type_to_julia(const Type *t)
{
    if (t == T_int1)  return (jl_value_t*)jl_bool_type;
    if (t == T_int8)  return (jl_value_t*)jl_int8_type;
    if (t == T_int16) return (jl_value_t*)jl_int16_type;
    if (t == T_int32) return (jl_value_t*)jl_int32_type;
    if (t == T_int64) return (jl_value_t*)jl_int64_type;
    if (t == T_float32) return (jl_value_t*)jl_float32_type;
    if (t == T_float64) return (jl_value_t*)jl_float64_type;
    if (t == T_float80) return (jl_value_t*)jl_float80_type;
    if (t == T_float128) return (jl_value_t*)jl_float128_type;
    if (t == T_void) return (jl_value_t*)jl_bottom_type;
    if (t == jl_pvalue_llvmt)
        return (jl_value_t*)jl_any_type;
    if (t->isPointerTy()) {
        jl_value_t *elty = llvm_type_to_julia(t->getContainedType(0));
        return (jl_value_t*)jl_apply_type((jl_value_t*)jl_pointer_type,
                                          jl_tuple(1, elty));
    }
    jl_errorf("cannot convert type %s to a julia type",
              t->getDescription().c_str());
    return NULL;
}

// this is used to wrap values for generic contexts, where a
// dynamically-typed value is required (e.g. argument to unknown function).
// if it's already a pointer it's left alone.
static Value *boxed(Value *v)
{
    const Type *t = v->getType();
    if (t == jl_pvalue_llvmt)
        return v;
    if (t == T_int1) return julia_bool(v);
    jl_value_t *jt = julia_type_of(v);
    jl_bits_type_t *jb = (jl_bits_type_t*)jt;
    if (jb == jl_int8_type)  return builder.CreateCall(box_int8_func, v);
    if (jb == jl_int16_type) return builder.CreateCall(box_int16_func, v);
    if (jb == jl_int32_type) return builder.CreateCall(box_int32_func, v);
    if (jb == jl_int64_type) return builder.CreateCall(box_int64_func, v);
    if (jb == jl_float32_type) return builder.CreateCall(box_float32_func, v);
    if (jb == jl_float64_type) return builder.CreateCall(box_float64_func, v);
    if (jb == jl_float80_type) return builder.CreateCall(box_float80_func, v);
    if (jb == jl_float128_type) return builder.CreateCall(box_float128_func, v);
    if (jb == jl_uint8_type)  return builder.CreateCall(box_uint8_func, v);
    if (jb == jl_uint16_type) return builder.CreateCall(box_uint16_func, v);
    if (jb == jl_uint32_type) return builder.CreateCall(box_uint32_func, v);
    if (jb == jl_uint64_type) return builder.CreateCall(box_uint64_func, v);
    if (jl_is_cpointer_type(jt)) {
        if (!v->getType()->isPointerTy())
            v = builder.CreateIntToPtr(v, T_pint8);
        else
            v = builder.CreateBitCast(v, T_pint8);
        return builder.CreateCall2(box_pointer_func,
                                   literal_pointer_val(jt), v);
    }
    if (jl_is_bits_type(jt)) {
        int nb = jl_bitstype_nbits(jt);
        if (nb == 8)
            return builder.CreateCall2(box8_func,  literal_pointer_val(jt), v);
        if (nb == 16)
            return builder.CreateCall2(box16_func, literal_pointer_val(jt), v);
        if (nb == 32)
            return builder.CreateCall2(box32_func, literal_pointer_val(jt), v);
        if (nb == 64)
            return builder.CreateCall2(box64_func, literal_pointer_val(jt), v);
        if (nb == 80)
            return builder.CreateCall2(box80_func, literal_pointer_val(jt), v);
        if (nb == 128)
            return builder.CreateCall2(box128_func, literal_pointer_val(jt), v);
    }
    assert("Don't know how to box this type" && false);
    return NULL;
}

static Value *bitstype_pointer(Value *x)
{
    return builder.CreateGEP(builder.CreateBitCast(x, jl_ppvalue_llvmt),
                             ConstantInt::get(T_int32, 1));
}

static Function *value_to_pointer_func;

// TODO: per-thread
static char *temp_arg_area;
static const uint32_t arg_area_sz = 4196;
static uint32_t arg_area_loc;
#define N_TEMP_ARG_BLOCKS 1024
static void *temp_arg_blocks[N_TEMP_ARG_BLOCKS];
static uint32_t arg_block_n = 0;
static Function *save_arg_area_loc_func;
static Function *restore_arg_area_loc_func;

static uint64_t save_arg_area_loc()
{
    return (((uint64_t)arg_block_n)<<32) | ((uint64_t)arg_area_loc);
}
static void restore_arg_area_loc(uint64_t l)
{
    arg_area_loc = l&0xffffffff;
    uint32_t ab = l>>32;
    while (arg_block_n > ab) {
        arg_block_n--;
        free(temp_arg_blocks[arg_block_n]);
    }
}

static void *alloc_temp_arg_space(uint32_t sz)
{
    void *p;
    if (arg_area_loc+sz > arg_area_sz) {
#ifdef BOEHM_GC
        p = allocb(sz);
#elif defined(JL_GC_MARKSWEEP)
        if (arg_block_n >= N_TEMP_ARG_BLOCKS)
            jl_error("ccall: out of temporary argument space");
        p = malloc(sz);
        temp_arg_blocks[arg_block_n++] = p;
#else
        p = allocb(sz);
#endif
    }
    else {
        p = &temp_arg_area[arg_area_loc];
        arg_area_loc += sz;
    }
    return p;
}

static void *alloc_temp_arg_copy(void *obj, uint32_t sz)
{
    void *p = alloc_temp_arg_space(sz);
    memcpy(p, obj, sz);
    return p;
}

// this is a run-time function
extern "C" void *jl_value_to_pointer(jl_value_t *jt, jl_value_t *v, int argn)
{
    // this is a custom version of convert_to_ptr that is able to use
    // the temporary argument space.
    if (v == (jl_value_t*)jl_null)
        return NULL;
    if (jl_is_cpointer(v))
        return jl_unbox_pointer(v);
    if ((jl_value_t*)jl_typeof(v) == jt) {
        assert(jl_is_bits_type(jt));
        size_t osz = jl_bitstype_nbits(jt)/8;
        return alloc_temp_arg_copy(jl_bits_data(v), osz);
    }
    if ((jl_value_t*)jl_uint8_type == jt && jl_is_byte_string(v)) {
        return jl_string_data(v);
    }
    if (jl_is_array(v)) {
        if (jl_tparam0(jl_typeof(v)) == jt || jt==(jl_value_t*)jl_bottom_type)
            return ((jl_array_t*)v)->data;
        if (jl_is_cpointer_type(jt)) {
            jl_array_t *ar = (jl_array_t*)v;
            void **temp=(void**)alloc_temp_arg_space(ar->length*sizeof(void*));
            size_t i;
            for(i=0; i < ar->length; i++) {
                temp[i] = jl_value_to_pointer(jl_tparam0(jt),
                                              jl_arrayref(ar, i), argn);
            }
            return temp;
        }
    }
    jl_errorf("ccall: expected Ptr{%s} as argument %d",
              // TODO: string is leaked
              jl_show_to_string(jt), argn);
    return (jl_value_t*)jl_null;
}

static Value *julia_to_native(const Type *ty, jl_value_t *jt, Value *jv,
                              int argn, jl_codectx_t *ctx)
{
    const Type *vt = jv->getType();
    if (ty == jl_pvalue_llvmt) {
        return boxed(jv);
    }
    else if (ty == vt) {
        return jv;
    }
    else if (vt != jl_pvalue_llvmt) {
        if ((vt->isIntegerTy() && ty->isIntegerTy()) ||
            (vt->isFloatingPointTy() && ty->isFloatingPointTy()) ||
            (vt->isPointerTy() && ty->isPointerTy())) {
            if (vt->getPrimitiveSizeInBits() ==
                ty->getPrimitiveSizeInBits()) {
                return builder.CreateBitCast(jv, ty);
            }
        }
        if (ty->isPointerTy() && ty->getContainedType(0)==vt) {
            // we have an unboxed variable x, and need to pass &x
            // todo: pass address of stack-allocated variable
            jv = boxed(jv);
            Value *p = bitstype_pointer(jv);
            return builder.CreateBitCast(p, ty);
        }
        else {
            // error. box for error handling.
            jv = boxed(jv);
        }
        /*
        else {
            assert(false && "Unsupported native type.");
        }
        */
    }
    else if (jl_is_cpointer_type(jt)) {
        Value *p = builder.CreateCall3(value_to_pointer_func,
                                       literal_pointer_val(jl_tparam0(jt)), jv,
                                       ConstantInt::get(T_int32, argn));
        assert(ty->isPointerTy());
        return builder.CreateBitCast(p, ty);
    }
    assert(jl_is_bits_type(jt));
    std::stringstream msg;
    msg << "ccall argument ";
    msg << argn;
    emit_typecheck(jv, jt, msg.str(), ctx);
    Value *p = bitstype_pointer(jv);
    return builder.CreateLoad(builder.CreateBitCast(p,
                                                    PointerType::get(ty,0)),
                              false);
}

// ccall(pointer, rettype, (argtypes...), args...)
static Value *emit_ccall(jl_value_t **args, size_t nargs, jl_codectx_t *ctx)
{
    JL_NARGSV(ccall, 3);
    jl_value_t *ptr=NULL, *rt=NULL, *at=NULL;
    JL_GC_PUSH(&ptr, &rt, &at);
    ptr = jl_interpret_toplevel_expr(args[1]);
    rt  = jl_interpret_toplevel_expr_with(args[2],
                                          &jl_tupleref(ctx->sp,0),
                                          ctx->sp->length/2);
    at  = jl_interpret_toplevel_expr_with(args[3],
                                          &jl_tupleref(ctx->sp,0),
                                          ctx->sp->length/2);
    JL_TYPECHK(ccall, pointer, ptr);
    JL_TYPECHK(ccall, type, rt);
    JL_TYPECHK(ccall, tuple, at);
    JL_TYPECHK(ccall, type, at);
    jl_tuple_t *tt = (jl_tuple_t*)at;
    if (tt->length != nargs-3)
        jl_error("ccall: wrong number of arguments to C function");
    void *fptr = *(void**)jl_bits_data(ptr);
    std::vector<const Type *> fargt(0);
    const Type *lrt = julia_type_to_llvm(rt);
    size_t i;
    bool haspointers = false;
    for(i=0; i < tt->length; i++) {
        const Type *t = julia_type_to_llvm(jl_tupleref(tt,i));
        haspointers = haspointers || (t->isPointerTy() && t!=jl_pvalue_llvmt);
        fargt.push_back(t);
    }
    // make LLVM function object for the target
    Function *llvmf =
        Function::Create(FunctionType::get(lrt, fargt, false),
                         Function::ExternalLinkage,
                         "ccall_", jl_Module);
    jl_ExecutionEngine->addGlobalMapping(llvmf, fptr);

    // save temp argument area stack pointer
    Value *saveloc=NULL;
    if (haspointers) {
        saveloc = builder.CreateCall(save_arg_area_loc_func);
    }

    // emit arguments
    std::vector<Value*> argvals(0);
    int last_depth = ctx->argDepth;
    for(i=4; i < nargs+1; i++) {
        Value *arg = emit_expr(args[i], ctx, true);
#ifdef JL_GC_MARKSWEEP
        // make sure args are rooted
        if (fargt[i-4]->isPointerTy() &&
            (fargt[i-4] == jl_pvalue_llvmt ||
             !jl_is_bits_type(expr_type(args[i])))) {
            Value *gcroot = builder.CreateGEP(ctx->argTemp,
                                              ConstantInt::get(T_int32,
                                                               ctx->argDepth));
            builder.CreateStore(boxed(arg), gcroot);
            ctx->argDepth++;
        }
#endif
        argvals.push_back(julia_to_native(fargt[i-4], jl_tupleref(tt,i-4),
                                          arg, i-3, ctx));
    }
    // the actual call
    Value *result = builder.CreateCall(llvmf, argvals.begin(), argvals.end());

    // restore temp argument area stack pointer
    if (haspointers) {
        assert(saveloc != NULL);
        builder.CreateCall(restore_arg_area_loc_func, saveloc);
    }
    ctx->argDepth = last_depth;

    JL_GC_POP();
    if (lrt == T_void)
        return literal_pointer_val((jl_value_t*)jl_null);
    return mark_julia_type(result, rt);
}

// convert int type to same-size float type
static const Type *FT(const Type *t)
{
    if (t->isFloatingPointTy())
        return t;
    if (t == T_int32) return T_float32;
    assert(t == T_int64);
    return T_float64;
}

// reinterpret-cast to float
static Value *FP(Value *v)
{
    if (v->getType()->isFloatingPointTy())
        return v;
    return builder.CreateBitCast(v, FT(v->getType()));
}

static Value *uint_cnvt(const Type *to, Value *x)
{
    const Type *t = x->getType();
    if (t == to) return x;
    if (to->getPrimitiveSizeInBits() < x->getType()->getPrimitiveSizeInBits())
        return builder.CreateTrunc(x, to);
    return builder.CreateZExt(x, to);
}

static Value *emit_unbox(const Type *to, const Type *pto, Value *x)
{
    //if (x->getType()->isIntegerTy() || x->getType()->isFloatingPointTy()) {
    if (x->getType() != jl_pvalue_llvmt) {
        // bools are stored internally as int8 (for now), so we need to make
        // unbox8(x::Bool) work.
        if (x->getType() == T_int1 && to == T_int8)
            return builder.CreateZExt(x, T_int8);
        if (x->getType()->isPointerTy() && !to->isPointerTy())
            return builder.CreatePtrToInt(x, to);
        return x;
    }
    //assert(x->getType() == jl_pvalue_llvmt);
    Value *p = bitstype_pointer(x);
    if (to == T_int1) {
        // bools stored as int8, so an extra Trunc is needed to get an int1
        return builder.CreateTrunc(builder.
                                   CreateLoad(builder.
                                              CreateBitCast(p, T_pint8), false),
                                   T_int1);
    }
    return builder.CreateLoad(builder.CreateBitCast(p, pto), false);
}

static Value *emit_unboxed(jl_value_t *e, jl_codectx_t *ctx)
{
    // TODO: unclear why some types are here but others aren't?
    if (jl_is_int32(e)) {
        return ConstantInt::get(T_int32, jl_unbox_int32(e));
    }
    else if (jl_is_int64(e)) {
        return ConstantInt::get(T_int64, jl_unbox_int64(e));
    }
    else if (jl_is_uint64(e)) {
        return mark_julia_type(ConstantInt::get(T_int64,
                                                (int64_t)jl_unbox_uint64(e)),
                               jl_uint64_type);
    }
    else if (jl_is_float64(e)) {
        return ConstantFP::get(T_float64, jl_unbox_float64(e));
    }
    else if (e == jl_true) {
        return ConstantInt::get(T_int1, 1);
    }
    else if (e == jl_false) {
        return ConstantInt::get(T_int1, 0);
    }
    return emit_expr(e, ctx, true);
}

static Value *generic_box(jl_value_t *targ, jl_value_t *x, jl_codectx_t *ctx)
{
    jl_value_t *bt =
        jl_interpret_toplevel_expr_with(targ,
                                        &jl_tupleref(ctx->sp,0),
                                        ctx->sp->length/2);
    if (!jl_is_bits_type(bt))
        jl_error("box: expected bits type as first argument");
    unsigned int nb = jl_bitstype_nbits(bt);
    Value *vx = emit_unboxed(x, ctx);
    if (vx->getType()->getPrimitiveSizeInBits() != nb)
        jl_errorf("box: expected argument with %d bits", nb);
    return mark_julia_type(vx, bt);
}

static Value *generic_unbox(jl_value_t *targ, jl_value_t *x, jl_codectx_t *ctx)
{
    jl_value_t *bt =
        jl_interpret_toplevel_expr_with(targ,
                                        &jl_tupleref(ctx->sp,0),
                                        ctx->sp->length/2);
    if (!jl_is_bits_type(bt))
        jl_error("unbox: expected bits type as first argument");
    unsigned int nb = jl_bitstype_nbits(bt);
    const Type *to = IntegerType::get(jl_LLVMContext, nb);
    return emit_unbox(to, PointerType::get(to, 0), emit_unboxed(x, ctx));
}

#define HANDLE(intr,n)                                                  \
    case intr: if (nargs!=n) jl_error(#intr": wrong number of arguments");

static Value *emit_intrinsic(intrinsic f, jl_value_t **args, size_t nargs,
                             jl_codectx_t *ctx)
{
    if (f == ccall) return emit_ccall(args, nargs, ctx);
    if (f == box) {
        if (nargs!=2)
            jl_error("box: wrong number of arguments");
        return generic_box(args[1], args[2], ctx);
    }
    if (f == unbox) {
        if (nargs!=2)
            jl_error("unbox: wrong number of arguments");
        return generic_unbox(args[1], args[2], ctx);
    }
    if (nargs < 1) jl_error("invalid intrinsic call");
    Value *x = emit_unboxed(args[1], ctx);
    const Type *t = x->getType();
    const Type *fxt;
    const Type *fxts[2];
    Value *fx, *fy;
    switch (f) {
    HANDLE(boxui8,1)
        if (t != T_int8) x = builder.CreateBitCast(x, T_int8);
        return mark_julia_type(x, jl_uint8_type);
    HANDLE(boxsi8,1)
        if (t != T_int8) x = builder.CreateBitCast(x, T_int8);
        return mark_julia_type(x, jl_int8_type);
    HANDLE(boxui16,1)
        if (t != T_int16) x = builder.CreateBitCast(x, T_int16);
        return mark_julia_type(x, jl_uint16_type);
    HANDLE(boxsi16,1)
        if (t != T_int16) x = builder.CreateBitCast(x, T_int16);
        return mark_julia_type(x, jl_int16_type);
    HANDLE(boxui32,1)
        if (t != T_int32) x = builder.CreateBitCast(x, T_int32);
        return mark_julia_type(x, jl_uint32_type);
    HANDLE(boxsi32,1)
        if (t != T_int32) x = builder.CreateBitCast(x, T_int32);
        return mark_julia_type(x, jl_int32_type);
    HANDLE(boxui64,1)
        if (t != T_int64) x = builder.CreateBitCast(x, T_int64);
        return mark_julia_type(x, jl_uint64_type);
    HANDLE(boxsi64,1)
        if (t != T_int64) x = builder.CreateBitCast(x, T_int64);
        return mark_julia_type(x, jl_int64_type);
    HANDLE(boxf32,1)
        if (t != T_float32) x = builder.CreateBitCast(x, T_float32);
        return mark_julia_type(x, jl_float32_type);
    HANDLE(boxf64,1)
        if (t != T_float64) x = builder.CreateBitCast(x, T_float64);
        return mark_julia_type(x, jl_float64_type);
    HANDLE(boxf80,1)
        if (t != T_float80) x = builder.CreateBitCast(x, T_float80);
        return mark_julia_type(x, jl_float80_type);
    HANDLE(boxf128,1)
        if (t != T_float128) x = builder.CreateBitCast(x, T_float128);
        return mark_julia_type(x, jl_float128_type);

    HANDLE(unbox8,1)
        return emit_unbox(T_int8, T_pint8, x);
    HANDLE(unbox16,1)
        return emit_unbox(T_int16, T_pint16, x);
    HANDLE(unbox32,1)
        return emit_unbox(T_int32, T_pint32, x);
    HANDLE(unbox64,1)
        return emit_unbox(T_int64, T_pint64, x);
    HANDLE(unbox80,1)
        return emit_unbox(T_float80, T_pfloat80, x);
    HANDLE(unbox128,1)
        return emit_unbox(T_float128, T_pfloat128, x);

    HANDLE(neg_int,1)
        return builder.CreateSub(ConstantInt::get(t, 0), x);
    HANDLE(add_int,2)
        return builder.CreateAdd(x, emit_expr(args[2],ctx,true));
    HANDLE(sub_int,2)
        return builder.CreateSub(x, emit_expr(args[2],ctx,true));
    HANDLE(mul_int,2)
        return builder.CreateMul(x, emit_expr(args[2],ctx,true));
    HANDLE(sdiv_int,2)
        return builder.CreateSDiv(x, emit_expr(args[2],ctx,true));
    HANDLE(udiv_int,2)
        return builder.CreateUDiv(x, emit_expr(args[2],ctx,true));
    HANDLE(srem_int,2)
        return builder.CreateSRem(x, emit_expr(args[2],ctx,true));
    HANDLE(urem_int,2)
        return builder.CreateURem(x, emit_expr(args[2],ctx,true));

    HANDLE(neg_float,1)
        return builder.CreateFMul(ConstantFP::get(FT(t), -1.0), FP(x));
    HANDLE(add_float,2)
        return builder.CreateFAdd(FP(x), FP(emit_expr(args[2],ctx,true)));
    HANDLE(sub_float,2)
        return builder.CreateFSub(FP(x), FP(emit_expr(args[2],ctx,true)));
    HANDLE(mul_float,2)
        return builder.CreateFMul(FP(x), FP(emit_expr(args[2],ctx,true)));
    HANDLE(div_float,2)
        return builder.CreateFDiv(FP(x), FP(emit_expr(args[2],ctx,true)));
    HANDLE(rem_float,2)
        return builder.CreateFRem(FP(x), FP(emit_expr(args[2],ctx,true)));

    HANDLE(eq_int,2)
        return builder.CreateICmpEQ(x, emit_expr(args[2],ctx,true));
    HANDLE(ne_int,2)
        return builder.CreateICmpNE(x, emit_expr(args[2],ctx,true));
    HANDLE(slt_int,2)
        return builder.CreateICmpSLT(x, emit_expr(args[2],ctx,true));
    HANDLE(ult_int,2)
        return builder.CreateICmpULT(x, emit_expr(args[2],ctx,true));
    HANDLE(sle_int,2)
        return builder.CreateICmpSLE(x, emit_expr(args[2],ctx,true));
    HANDLE(ule_int,2)
        return builder.CreateICmpULE(x, emit_expr(args[2],ctx,true));
    HANDLE(sgt_int,2)
        return builder.CreateICmpSGT(x, emit_expr(args[2],ctx,true));
    HANDLE(ugt_int,2)
        return builder.CreateICmpUGT(x, emit_expr(args[2],ctx,true));
    HANDLE(sge_int,2)
        return builder.CreateICmpSGE(x, emit_expr(args[2],ctx,true));
    HANDLE(uge_int,2)
        return builder.CreateICmpUGE(x, emit_expr(args[2],ctx,true));

    HANDLE(eq_float,2)
        return builder.CreateFCmpOEQ(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));
    HANDLE(ne_float,2)
        return builder.CreateFCmpUNE(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));
    HANDLE(lt_float,2)
        return builder.CreateFCmpOLT(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));
    HANDLE(le_float,2)
        return builder.CreateFCmpOLE(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));
    HANDLE(gt_float,2)
        return builder.CreateFCmpOGT(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));
    HANDLE(ge_float,2)
        return builder.CreateFCmpOGE(FP(x),
                                     FP(emit_expr(args[2],ctx,true)));

    HANDLE(and_int,2)
        return builder.CreateAnd(x, emit_expr(args[2],ctx,true));
    HANDLE(or_int,2)
        return builder.CreateOr(x, emit_expr(args[2],ctx,true));
    HANDLE(xor_int,2)
        return builder.CreateXor(x, emit_expr(args[2],ctx,true));
    HANDLE(not_int,1)
        return builder.CreateXor(x, ConstantInt::get(t, -1));
    HANDLE(shl_int,2)
        return builder.CreateShl(x, uint_cnvt(t,emit_expr(args[2],ctx,true)));
    HANDLE(lshr_int,2)
        return builder.CreateLShr(x, uint_cnvt(t,emit_expr(args[2],ctx,true)));
    HANDLE(ashr_int,2)
        return builder.CreateAShr(x, uint_cnvt(t,emit_expr(args[2],ctx,true)));
    HANDLE(bswap_int,1)
        fxt = x->getType();
        return builder.CreateCall(Intrinsic::getDeclaration(jl_Module,
                                                            Intrinsic::bswap,
                                                            &fxt, 1),
                                  x);

    HANDLE(sext16,1)
        return builder.CreateSExt(x, T_int16);
    HANDLE(zext16,1)
        return builder.CreateZExt(x, T_int16);
    HANDLE(sext32,1)
        return builder.CreateSExt(x, T_int32);
    HANDLE(zext32,1)
        return builder.CreateZExt(x, T_int32);
    HANDLE(sext64,1)
        return builder.CreateSExt(x, T_int64);
    HANDLE(zext64,1)
        return builder.CreateZExt(x, T_int64);
    HANDLE(trunc8,1)
        return builder.CreateTrunc(x, T_int8);
    HANDLE(trunc16,1)
        return builder.CreateTrunc(x, T_int16);
    HANDLE(trunc32,1)
        return builder.CreateTrunc(x, T_int32);
    HANDLE(fptoui8,1)
        return builder.CreateFPToUI(FP(x), T_int8);
    HANDLE(fptosi8,1)
        return builder.CreateFPToSI(FP(x), T_int8);
    HANDLE(fptoui16,1)
        return builder.CreateFPToUI(FP(x), T_int16);
    HANDLE(fptosi16,1)
        return builder.CreateFPToSI(FP(x), T_int16);
    HANDLE(fptoui32,1)
        return builder.CreateFPToUI(FP(x), T_int32);
    HANDLE(fptosi32,1)
        return builder.CreateFPToSI(FP(x), T_int32);
    HANDLE(fptoui64,1)
        return builder.CreateFPToUI(FP(x), T_int64);
    HANDLE(fptosi64,1)
        return builder.CreateFPToSI(FP(x), T_int64);
    HANDLE(uitofp32,1)
        return builder.CreateUIToFP(x, T_float32);
    HANDLE(sitofp32,1)
        return builder.CreateSIToFP(x, T_float32);
    HANDLE(uitofp64,1)
        return builder.CreateUIToFP(x, T_float64);
    HANDLE(sitofp64,1)
        return builder.CreateSIToFP(x, T_float64);
    HANDLE(uitofp80,1)
        return builder.CreateUIToFP(x, T_float80);
    HANDLE(sitofp80,1)
        return builder.CreateSIToFP(x, T_float80);
    HANDLE(uitofp128,1)
        return builder.CreateUIToFP(x, T_float128);
    HANDLE(sitofp128,1)
        return builder.CreateSIToFP(x, T_float128);
    HANDLE(fptrunc32,1)
        return builder.CreateFPTrunc(FP(x), T_float32);
    HANDLE(fptrunc64,1)
        return builder.CreateFPTrunc(FP(x), T_float64);
    HANDLE(fptrunc80,1)
        return builder.CreateFPTrunc(FP(x), T_float80);
    HANDLE(fpext64,1)
        return builder.CreateFPExt(FP(x), T_float64);
    HANDLE(fpext80,1)
        return builder.CreateFPExt(FP(x), T_float80);
    HANDLE(fpext128,1)
        return builder.CreateFPExt(FP(x), T_float128);

    HANDLE(sqrt_float,1)
        fx = FP(x);
        fxt = fx->getType();
        return builder.CreateCall(Intrinsic::getDeclaration(jl_Module,
                                                            Intrinsic::sqrt,
                                                            &fxt, 1),
                                  fx);
    HANDLE(sin_float,1)
        fx = FP(x);
        fxt = fx->getType();
        return builder.CreateCall(Intrinsic::getDeclaration(jl_Module,
                                                            Intrinsic::sin,
                                                            &fxt, 1),
                                  fx);
    HANDLE(cos_float,1)
        fx = FP(x);
        fxt = fx->getType();
        return builder.CreateCall(Intrinsic::getDeclaration(jl_Module,
                                                            Intrinsic::cos,
                                                            &fxt, 1),
                                  fx);
    HANDLE(pow_float,2)
        fx = FP(x);
        fy = FP(emit_expr(args[2],ctx,true));
        fxts[0] = fx->getType(); fxts[1] = fy->getType();
        if (fxts[0] != fxts[1] ||
            !fxts[0]->isFloatingPointTy() || !fxts[1]->isFloatingPointTy())
            jl_error("invalid arguments to pow_float");
        return builder.CreateCall2(Intrinsic::getDeclaration(jl_Module,
                                                             Intrinsic::pow,
                                                             fxts, 2),
                                   fx, fy);
    HANDLE(powi_float,2)
        fx = FP(x);
        fy = emit_expr(args[2],ctx,true);
        fxts[0] = fx->getType(); fxts[1] = fy->getType();
        if (!fxts[0]->isFloatingPointTy() || fxts[1] != T_int32)
            jl_error("invalid arguments to powi_float");
        return builder.CreateCall2(Intrinsic::getDeclaration(jl_Module,
                                                             Intrinsic::powi,
                                                             fxts, 1),
                                   fx, fy);
    default:
        assert(false);
    }
    assert(false);
    return NULL;
}

#undef HANDLE

static Function *boxfunc_llvm(FunctionType *ft, const std::string &cname,
                              void *addr)
{
    Function *f =
        Function::Create(ft, Function::ExternalLinkage, cname, jl_Module);
    jl_ExecutionEngine->addGlobalMapping(f, addr);
    return f;
}

static FunctionType *ft1arg(const Type *ret, const Type *arg)
{
    std::vector<const Type*> args1(0);
    args1.push_back(arg);
    return FunctionType::get(ret, args1, false);
}

static FunctionType *ft2arg(const Type *ret, const Type *arg1,
                            const Type *arg2)
{
    std::vector<const Type*> args2(0);
    args2.push_back(arg1);
    args2.push_back(arg2);
    return FunctionType::get(ret, args2, false);
}

static void add_intrinsic(const std::string &name, intrinsic f)
{
    jl_value_t *i = jl_box32(jl_intrinsic_type, (int32_t)f);
    jl_set_const(jl_system_module, jl_symbol((char*)name.c_str()), i);
}

#define ADD_I(name) add_intrinsic(#name, name)
#define BOX_F(ct)                                                       \
    box_##ct##_func = boxfunc_llvm(ft1arg(jl_pvalue_llvmt, T_##ct),     \
                                   "jl_box_"#ct, (void*)&jl_box_##ct);

extern "C" void jl_init_intrinsic_functions()
{
    ADD_I(boxui8); ADD_I(boxsi8); ADD_I(boxui16); ADD_I(boxsi16);
    ADD_I(boxui32); ADD_I(boxsi32); ADD_I(boxui64); ADD_I(boxsi64);
    ADD_I(boxf32); ADD_I(boxf64); ADD_I(boxf80); ADD_I(boxf128);
    ADD_I(box); ADD_I(unbox);
    ADD_I(unbox8); ADD_I(unbox16); ADD_I(unbox32); ADD_I(unbox64);
    ADD_I(unbox80); ADD_I(unbox128);
    ADD_I(neg_int); ADD_I(add_int); ADD_I(sub_int); ADD_I(mul_int);
    ADD_I(sdiv_int); ADD_I(udiv_int); ADD_I(srem_int); ADD_I(urem_int);
    ADD_I(neg_float); ADD_I(add_float); ADD_I(sub_float); ADD_I(mul_float);
    ADD_I(div_float); ADD_I(rem_float);
    ADD_I(eq_int); ADD_I(ne_int);
    ADD_I(slt_int); ADD_I(ult_int);
    ADD_I(sle_int); ADD_I(ule_int);
    ADD_I(sgt_int); ADD_I(ugt_int);
    ADD_I(sge_int); ADD_I(uge_int);
    ADD_I(eq_float); ADD_I(ne_float);
    ADD_I(lt_float); ADD_I(le_float);
    ADD_I(gt_float); ADD_I(ge_float);
    ADD_I(and_int); ADD_I(or_int); ADD_I(xor_int); ADD_I(not_int);
    ADD_I(shl_int); ADD_I(lshr_int); ADD_I(ashr_int); ADD_I(bswap_int);
    ADD_I(sext16); ADD_I(zext16); ADD_I(sext32); ADD_I(zext32);
    ADD_I(sext64); ADD_I(zext64);
    ADD_I(trunc8); ADD_I(trunc16); ADD_I(trunc32);
    ADD_I(fptoui8); ADD_I(fptosi8); ADD_I(fptoui16); ADD_I(fptosi16);
    ADD_I(fptoui32); ADD_I(fptosi32); ADD_I(fptoui64); ADD_I(fptosi64);
    ADD_I(uitofp32); ADD_I(sitofp32); ADD_I(uitofp64); ADD_I(sitofp64);
    ADD_I(uitofp80); ADD_I(sitofp80); ADD_I(uitofp128); ADD_I(sitofp128);
    ADD_I(fptrunc32); ADD_I(fpext64); ADD_I(fptrunc80); ADD_I(fpext128);
    ADD_I(sqrt_float); ADD_I(powi_float); ADD_I(pow_float);
    ADD_I(sin_float); ADD_I(cos_float);
    ADD_I(ccall);
    
    BOX_F(int8);  BOX_F(uint8);
    BOX_F(int16); BOX_F(uint16);
    BOX_F(int32); BOX_F(uint32);
    BOX_F(int64); BOX_F(uint64);
    BOX_F(float32); BOX_F(float64);
    BOX_F(float80); BOX_F(float128);

    box8_func   = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_int8),
                               "jl_box8", (void*)*jl_box8);
    box16_func  = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_int16),
                               "jl_box16", (void*)*jl_box16);
    box32_func  = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_int32),
                               "jl_box32", (void*)*jl_box32);
    box64_func  = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_int64),
                               "jl_box64", (void*)*jl_box64);
    box80_func  = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_float80),
                               "jl_box80", (void*)*jl_box80);
    box128_func = boxfunc_llvm(ft2arg(jl_pvalue_llvmt, jl_pvalue_llvmt, T_float128),
                               "jl_box128", (void*)*jl_box128);

    std::vector<const Type*> boxpointerargs(0);
    boxpointerargs.push_back(jl_pvalue_llvmt);
    boxpointerargs.push_back(T_pint8);
    box_pointer_func =
        Function::Create(FunctionType::get(jl_pvalue_llvmt,
                                           boxpointerargs, false),
                         Function::ExternalLinkage, "jl_box_pointer",
                         jl_Module);
    jl_ExecutionEngine->addGlobalMapping(box_pointer_func,
                                         (void*)&jl_box_pointer);

    std::vector<const Type*> toptrargs(0);
    toptrargs.push_back(jl_pvalue_llvmt);
    toptrargs.push_back(jl_pvalue_llvmt);
    toptrargs.push_back(T_int32);
    value_to_pointer_func =
        Function::Create(FunctionType::get(T_pint8, toptrargs, false),
                         Function::ExternalLinkage, "jl_value_to_pointer",
                         jl_Module);
    jl_ExecutionEngine->addGlobalMapping(value_to_pointer_func,
                                         (void*)&jl_value_to_pointer);

#ifdef BOEHM_GC
    temp_arg_area = (char*)allocb(arg_area_sz);
#else
    temp_arg_area = (char*)alloc_permanent(arg_area_sz);
#endif
    arg_area_loc = 0;

    std::vector<const Type*> noargs(0);
    save_arg_area_loc_func =
        Function::Create(FunctionType::get(T_uint64, noargs, false),
                         Function::ExternalLinkage, "save_arg_area_loc",
                         jl_Module);
    jl_ExecutionEngine->addGlobalMapping(save_arg_area_loc_func,
                                         (void*)&save_arg_area_loc);

    restore_arg_area_loc_func =
        Function::Create(ft1arg(T_void, T_uint64),
                         Function::ExternalLinkage, "restore_arg_area_loc",
                         jl_Module);
    jl_ExecutionEngine->addGlobalMapping(restore_arg_area_loc_func,
                                         (void*)&restore_arg_area_loc);
}
