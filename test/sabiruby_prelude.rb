# What mrbtest gives a test file, for SabiRuby (sabiruby -r this --resume IMAGE):
# the C writer had these as C functions and constants before its baseline
# (mruby-checkpoint-fuzz/src/fuzz_testenv.c), so an image does not carry them.
module Kernel
  def t_print(*args)
    args.each { |a| print a.to_s }
    nil
  end

  # File.fnmatch-like matching for assert_match: * ? [set] [^set] {a,b} \x
  def _str_match?(pattern, str)
    __ck_alts(pattern).any? { |pat| __ck_glob(pat, 0, str, 0) }
  end

  def __ck_alts(pat)
    i = pat.index('{')
    return [pat] unless i
    j = pat.index('}', i)
    return [pat] unless j
    pat[i + 1...j].split(',', -1).flat_map { |alt| __ck_alts(pat[0...i] + alt + pat[j + 1..]) }
  end

  def __ck_glob(p, pi, s, si)
    while pi < p.size
      c = p[pi]
      if c == '*'
        pi += 1 while pi < p.size && p[pi] == '*'
        return true if pi == p.size
        k = si
        while k <= s.size
          return true if __ck_glob(p, pi, s, k)
          k += 1
        end
        return false
      end
      return false if si >= s.size
      if c == '?'
        pi += 1; si += 1
      elsif c == '['
        j = pi + 1
        neg = (p[j] == '^' || p[j] == '!')
        j += 1 if neg
        ok = false
        while j < p.size && p[j] != ']'
          j += 1 if p[j] == '\\'
          a = p[j]
          if p[j + 1] == '-' && p[j + 2] && p[j + 2] != ']'
            j += 2
            j += 1 if p[j] == '\\'
            b = p[j]
            ok = true if a <= s[si] && s[si] <= b
          else
            ok = true if a == s[si]
          end
          j += 1
        end
        return false if ok == neg
        pi = j + 1; si += 1
      else
        if c == '\\'
          pi += 1
          c = p[pi]
        end
        return false unless c == s[si]
        pi += 1; si += 1
      end
    end
    si == s.size
  end
end

module Mrbtest
  FLOAT_TOLERANCE = 1e-10
end
GEMNAME = 'mruby-test' unless Object.const_defined?(:GEMNAME)
$mrbtest_verbose = true
