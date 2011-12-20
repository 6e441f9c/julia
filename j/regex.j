load("pcre.j")

## object-oriented Regex interface ##

type Regex
    pattern::ByteString
    options::Int32
    regex::Array{Uint8}
    extra::Ptr{Void}

    function Regex(pat::String, opts::Integer, study::Bool)
        pat = cstring(pat); opts = int32(opts)
        if (opts & ~PCRE_OPTIONS_MASK) != 0
            error("invalid regex option(s)")
        end
        re = pcre_compile(pat, opts & PCRE_COMPILE_MASK)
        ex = study ? pcre_study(re) : C_NULL
        new(pat, opts, re, ex)
    end
end
Regex(p::String, s::Bool) = Regex(p, 0, s)
Regex(p::String, o::Integer)  = Regex(p, o, false)
Regex(p::String)          = Regex(p, 0, false)

# TODO: make sure thing are escaped in a way PCRE
# likes so that Julia all the Julia string quoting
# constructs are correctly handled.

begin
    local i = PCRE_CASELESS
    local m = PCRE_MULTILINE
    local s = PCRE_DOTALL
    local x = PCRE_EXTENDED

    macro r_str(p);     Regex(p);          end
    macro ri_str(p);    Regex(p, i);       end
    macro rm_str(p);    Regex(p, m);       end
    macro rs_str(p);    Regex(p, s);       end
    macro rx_str(p);    Regex(p, x);       end
    macro rim_str(p);   Regex(p, i|m);     end
    macro ris_str(p);   Regex(p, i|s);     end
    macro rix_str(p);   Regex(p, i|x);     end
    macro rms_str(p);   Regex(p, m|s);     end
    macro rmx_str(p);   Regex(p, m|x);     end
    macro rsx_str(p);   Regex(p, s|x);     end
    macro rims_str(p);  Regex(p, i|m|s);   end
    macro rimx_str(p);  Regex(p, i|m|x);   end
    macro risx_str(p);  Regex(p, i|s|x);   end
    macro rmsx_str(p);  Regex(p, m|s|x);   end
    macro rimsx_str(p); Regex(p, i|m|s|x); end

    function show(re::Regex)
        if (re.options & ~(i|m|s|x)) == 0
            print('r')
            if (re.options & i) != 0; print('i'); end
            if (re.options & m) != 0; print('m'); end
            if (re.options & s) != 0; print('s'); end
            if (re.options & x) != 0; print('x'); end
            print_quoted_literal(re.pattern)
        else
            print("Regex(")
            show(re.pattern)
            print(',')
            show(re.options)
            print(')')
        end
    end
end

type RegexMatch
    match::ByteString
    captures::Tuple
    offset::Int
    offsets::Vector{Int}
end

function show(m::RegexMatch)
    print("RegexMatch(")
    show(m.match)
    if !isempty(m.captures)
        print(", ")
        for i = 1:length(m.captures)
            print(i, "=")
            show(m.captures[i])
            if i < length(m.captures)
                print(", ")
            end
        end
    end
    print(")")
end

matches(r::Regex, s::String, o::Integer) = pcre_exec(r.regex, r.extra, cstring(s), 1, o, false)
matches(r::Regex, s::String) = matches(r, s, r.options & PCRE_EXECUTE_MASK)

function match(re::Regex, str::ByteString, offset::Integer, opts::Integer)
    m, n = pcre_exec(re.regex, re.extra, str, offset, opts, true)
    if isempty(m); return nothing; end
    mat = str[m[1]+1:m[2]]
    cap = ntuple(n, i->(m[2i+1] < 0 ? nothing : str[m[2i+1]+1:m[2i+2]]))
    off = map(i->int(m[2i+1]+1), [1:n])
    RegexMatch(mat, cap, int(m[1]+1), off)
end
match(r::Regex, s::String, o::Integer, p::Integer) = match(r, cstring(s), o, p)
match(r::Regex, s::String, o::Integer) = match(r, s, o, r.options & PCRE_EXECUTE_MASK)
match(r::Regex, s::String) = match(r, s, 1)

type RegexMatchIterator
    regex::Regex
    string::ByteString
    overlap::Bool
end

start(itr::RegexMatchIterator) = match(itr.regex, itr.string)
done(itr::RegexMatchIterator, m) = m == nothing
next(itr::RegexMatchIterator, m) =
    (m, match(itr.regex, itr.string, m.offset + (itr.overlap ? 1 : length(m.match))))

each_match(r::Regex, s::String) = RegexMatchIterator(r,s,false)
each_match_overlap(r::Regex, s::String) = RegexMatchIterator(r,s,true)

function split(s::String, regex::Regex, include_empty::Bool)
    s = cstring(s)
    i = j = start(s)
    strs = empty(typeof(s))
    while !done(s,i)
        m = match(regex,s,j)
        if m == nothing
            break
        end
        tok = s[i:m.offset-1]
        if include_empty || !isempty(tok)
            push(strs, tok)
        end
        i = m.offset+length(m.match)
        j = m.offset+max(1,length(m.match))
    end
    if include_empty || i < length(s)
        push(strs, s[i:end])
    end
    strs
end

split(s::String, x::String, incl::Bool) = split(s, Regex(strcat("\\Q",x)), incl)
