## floating point conversions ##

convert(::Type{Float32}, x::Int8)     = boxf32(sitofp32(unbox8(x)))
convert(::Type{Float32}, x::Int16)    = boxf32(sitofp32(unbox16(x)))
convert(::Type{Float32}, x::Int32)    = boxf32(sitofp32(unbox32(x)))
convert(::Type{Float32}, x::Int64)    = boxf32(sitofp32(unbox64(x)))
convert(::Type{Float32}, x::Uint8)    = boxf32(uitofp32(unbox8(x)))
convert(::Type{Float32}, x::Uint16)   = boxf32(uitofp32(unbox16(x)))
convert(::Type{Float32}, x::Uint32)   = boxf32(uitofp32(unbox32(x)))
convert(::Type{Float32}, x::Char)     = boxf32(uitofp32(unbox32(x)))
convert(::Type{Float32}, x::Uint64)   = boxf32(uitofp32(unbox64(x)))
convert(::Type{Float32}, x::Float64)  = boxf32(fptrunc32(unbox64(x)))
convert(::Type{Float32}, x::Float80)  = boxf32(fptrunc32(unbox80(x)))
convert(::Type{Float32}, x::Float128) = boxf32(fptrunc32(unbox128(x)))

convert(::Type{Float64}, x::Int8)     = boxf64(sitofp64(unbox8(x)))
convert(::Type{Float64}, x::Int16)    = boxf64(sitofp64(unbox16(x)))
convert(::Type{Float64}, x::Int32)    = boxf64(sitofp64(unbox32(x)))
convert(::Type{Float64}, x::Int64)    = boxf64(sitofp64(unbox64(x)))
convert(::Type{Float64}, x::Uint8)    = boxf64(uitofp64(unbox8(x)))
convert(::Type{Float64}, x::Uint16)   = boxf64(uitofp64(unbox16(x)))
convert(::Type{Float64}, x::Uint32)   = boxf64(uitofp64(unbox32(x)))
convert(::Type{Float64}, x::Char)     = boxf64(uitofp64(unbox32(x)))
convert(::Type{Float64}, x::Uint64)   = boxf64(uitofp64(unbox64(x)))
convert(::Type{Float64}, x::Float32)  = boxf64(fpext64(unbox32(x)))
convert(::Type{Float64}, x::Float80)  = boxf64(fptrunc64(unbox80(x)))
convert(::Type{Float64}, x::Float128) = boxf64(fptrunc64(unbox128(x)))

convert(::Type{Float80}, x::Int8)     = boxf80(sitofp80(unbox8(x)))
convert(::Type{Float80}, x::Int16)    = boxf80(sitofp80(unbox16(x)))
convert(::Type{Float80}, x::Int32)    = boxf80(sitofp80(unbox32(x)))
convert(::Type{Float80}, x::Int64)    = boxf80(sitofp80(unbox64(x)))
convert(::Type{Float80}, x::Uint8)    = boxf80(uitofp80(unbox8(x)))
convert(::Type{Float80}, x::Uint16)   = boxf80(uitofp80(unbox16(x)))
convert(::Type{Float80}, x::Uint32)   = boxf80(uitofp80(unbox32(x)))
convert(::Type{Float80}, x::Char)     = boxf80(uitofp80(unbox32(x)))
convert(::Type{Float80}, x::Uint64)   = boxf80(uitofp80(unbox64(x)))
convert(::Type{Float80}, x::Float32)  = boxf80(fpext80(unbox32(x)))
convert(::Type{Float80}, x::Float64)  = boxf80(fpext80(unbox64(x)))
convert(::Type{Float80}, x::Float128) = boxf80(fptrunc80(unbox128(x)))

convert(::Type{Float128}, x::Int8)    = boxf128(sitofp128(unbox8(x)))
convert(::Type{Float128}, x::Int16)   = boxf128(sitofp128(unbox16(x)))
convert(::Type{Float128}, x::Int32)   = boxf128(sitofp128(unbox32(x)))
convert(::Type{Float128}, x::Int64)   = boxf128(sitofp128(unbox64(x)))
convert(::Type{Float128}, x::Uint8)   = boxf128(uitofp128(unbox8(x)))
convert(::Type{Float128}, x::Uint16)  = boxf128(uitofp128(unbox16(x)))
convert(::Type{Float128}, x::Uint32)  = boxf128(uitofp128(unbox32(x)))
convert(::Type{Float128}, x::Char)    = boxf128(uitofp128(unbox32(x)))
convert(::Type{Float128}, x::Uint64)  = boxf128(uitofp128(unbox64(x)))
convert(::Type{Float128}, x::Float32) = boxf128(fpext128(unbox32(x)))
convert(::Type{Float128}, x::Float64) = boxf128(fpext128(unbox64(x)))
convert(::Type{Float128}, x::Float64) = boxf128(fpext128(unbox80(x)))

float32 (x::Scalar) = convert(Float32,  x)
float64 (x::Scalar) = convert(Float64,  x)
float80 (x::Scalar) = convert(Float80,  x)
float128(x::Scalar) = convert(Float128, x)

truncate(x::Float32) = convert(Int32, x)
truncate(x::Float)   = convert(Int64, x)

int(x::Float32) = int32(x)
int(x::Float)   = int64(x)

uint(x::Float32) = uint32(x)
uint(x::Float)   = uint64(x)

float(x::Float) = x

## floating point promotions ##

promote_rule(::Type{Float64}, ::Type{Float32} ) = Float64

promote_rule(::Type{Float32}, ::Type{Int8} ) = Float32
promote_rule(::Type{Float32}, ::Type{Int16}) = Float32
promote_rule(::Type{Float32}, ::Type{Int32}) = Float64
promote_rule(::Type{Float32}, ::Type{Int64}) = Float80

promote_rule(::Type{Float64}, ::Type{Int8} ) = Float64
promote_rule(::Type{Float64}, ::Type{Int16}) = Float64
promote_rule(::Type{Float64}, ::Type{Int32}) = Float64
promote_rule(::Type{Float64}, ::Type{Int64}) = Float80

promote_rule(::Type{Float32}, ::Type{Uint8} ) = Float32
promote_rule(::Type{Float32}, ::Type{Uint16}) = Float32
promote_rule(::Type{Float32}, ::Type{Uint32}) = Float64
promote_rule(::Type{Float32}, ::Type{Uint64}) = Float80

promote_rule(::Type{Float64}, ::Type{Uint8} ) = Float64
promote_rule(::Type{Float64}, ::Type{Uint16}) = Float64
promote_rule(::Type{Float64}, ::Type{Uint32}) = Float64
promote_rule(::Type{Float64}, ::Type{Uint64}) = Float80

promote_rule(::Type{Float32}, ::Type{Char}) = Float32
promote_rule(::Type{Float64}, ::Type{Char}) = Float64

## floating point arithmetic ##

(-)(x::Float32) = boxf32(neg_float(unbox32(x)))
(-)(x::Float64) = boxf64(neg_float(unbox64(x)))
(+)(x::Float32, y::Float32) = boxf32(add_float(unbox32(x), unbox32(y)))
(+)(x::Float64, y::Float64) = boxf64(add_float(unbox64(x), unbox64(y)))
(-)(x::Float32, y::Float32) = boxf32(sub_float(unbox32(x), unbox32(y)))
(-)(x::Float64, y::Float64) = boxf64(sub_float(unbox64(x), unbox64(y)))
(*)(x::Float32, y::Float32) = boxf32(mul_float(unbox32(x), unbox32(y)))
(*)(x::Float64, y::Float64) = boxf64(mul_float(unbox64(x), unbox64(y)))
(/)(x::Float32, y::Float32) = boxf32(div_float(unbox32(x), unbox32(y)))
(/)(x::Float64, y::Float64) = boxf64(div_float(unbox64(x), unbox64(y)))
rem(x::Float32, y::Float32) = boxf32(rem_float(unbox32(x), unbox32(y)))
rem(x::Float64, y::Float64) = boxf64(rem_float(unbox64(x), unbox64(y)))

# faster than generic for floats
mod{T<:Float}(x::T, y::T) = rem(y+rem(x,y),y)

## floating point comparisons ##

==(x::Float32, y::Float32) = eq_float(unbox32(x),unbox32(y))
==(x::Float64, y::Float64) = eq_float(unbox64(x),unbox64(y))
!=(x::Float32, y::Float32) = ne_float(unbox32(x),unbox32(y))
!=(x::Float64, y::Float64) = ne_float(unbox64(x),unbox64(y))
< (x::Float32, y::Float32) = lt_float(unbox32(x),unbox32(y))
< (x::Float64, y::Float64) = lt_float(unbox64(x),unbox64(y))
<=(x::Float32, y::Float32) = le_float(unbox32(x),unbox32(y))
<=(x::Float64, y::Float64) = le_float(unbox64(x),unbox64(y))
> (x::Float32, y::Float32) = gt_float(unbox32(x),unbox32(y))
> (x::Float64, y::Float64) = gt_float(unbox64(x),unbox64(y))
>=(x::Float32, y::Float32) = ge_float(unbox32(x),unbox32(y))
>=(x::Float64, y::Float64) = ge_float(unbox64(x),unbox64(y))

!=(x::Float, y::Float) = (!=)(promote(x,y)...)
<=(x::Float, y::Float) = (<=)(promote(x,y)...)
>=(x::Float, y::Float) = (>=)(promote(x,y)...)

## traits ##

eps(::Type{Float32}) = float32(1.1920928e-7)
eps(::Type{Float64}) = 2.2204460492503131e-16
typemin(::Type{Float32}) = float32(1.175494351e-38)
typemax(::Type{Float32}) = float32(3.402823466e+38)
typemin(::Type{Float64}) = 2.2250738585072014e-308
typemax(::Type{Float64}) = 1.7976931348623157e+308

sizeof(::Type{Float32}) = 4
sizeof(::Type{Float64}) = 8

## constants ##

Inf = 1/0
NaN = -(0/0)

pi() = 3.14159265358979323846
pi(x) = pi()
pi(::Union(Float64, Type{Float64})) = 3.14159265358979323846
pi(::Union(Float32, Type{Float32})) = float32(3.14159265358979323846)
