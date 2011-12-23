## generic operations on numbers ##

isreal(x::Number) = false
isreal(x::Real) = true

isinteger(x::Number) = false
isinteger(x::Integer) = true

real_valued(x::Real) = true
integer_valued(x::Integer) = true

size(x::Number) = ()
ndims(x::Number) = 0
numel(x::Number) = 1
length(x::Number) = 1
ref(x::Number) = x

signbit(x::Real) = int(x < 0)
sign(x::Real) = x < 0 ? -one(x) : x > 0 ? one(x) : x
abs(x::Real) = x < 0 ? -x : x
abs2(x::Real) = x*x

conj(x::Real) = x
transpose(x::Number) = x
ctranspose(x::Number) = conj(x)
inv(x::Number) = one(x)/x

# TODO: should we really treat numbers as iterable?
start(a::Real) = a
next(a::Real, i) = (a, a+1)
done(a::Real, i) = (i > a)
isempty(a::Number) = false

reinterpret{T<:Real,S<:Real}(::Type{T}, x::S) = box(T,unbox(S,x))
