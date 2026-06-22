// Minimal Varian syntax highlighter.
//
// IMPORTANT: this is a SINGLE-PASS tokenizer, not a sequence of
// string.replace() calls. The old version applied one regex per token class
// in order, inserting <span class="k">…</span> etc. as it went — and then a
// later pass (the string regex /"([^"]*)"/) matched the quoted class names it
// had just inserted ("k", "f", …), wrapping them again and breaking the
// markup. The single-letter class names then leaked into the page as stray
// visible characters (the "f" and "k" seen all over code blocks). Classifying
// every token exactly once, left to right, makes that impossible.

document.addEventListener('DOMContentLoaded', function () {
  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  var KW = /^(fn|let|struct|impl|actor|enum|trait|match|try|catch|throw|loop|break|continue|while|for|in|if|else|return|null|true|false|self|comptime|use|and|or|not)$/;
  var TY = /^(c_int|c_double|c_float|c_char|ptr|int|float|string|bool|any)$/;
  var FN_WORD = /^(print|assert_eq|assert_ne|assert_throws|string_to_int|string_to_float)$/;
  var FN_NS = /^(task|io|math|mock)\.\w+$/;

  // One regex, ordered alternation. Each match is exactly one token:
  //  1 comment  2 string  3 operator  4 decorator  5 number  6 word(.member)
  var TOKEN = /(\/\/[^\n]*)|("(?:[^"\\]|\\.)*")|(&lt;-|\?\.|\?\?)|(@\w+)|(\d+\.?\d*)|([A-Za-z_]\w*(?:\.\w+)?)/g;

  document.querySelectorAll('pre code.vn').forEach(function (block) {
    var src = esc(block.textContent);
    block.innerHTML = src.replace(TOKEN, function (m, com, str, op, dec, num, word) {
      if (com != null) return '<span class="c">' + com + '</span>';
      if (str != null) return '<span class="s">' + str + '</span>';
      if (op  != null) return '<span class="o">' + op + '</span>';
      if (dec != null) return '<span class="d">' + dec + '</span>';
      if (num != null) return '<span class="n">' + num + '</span>';
      if (word != null) {
        if (KW.test(word)) return '<span class="k">' + word + '</span>';
        if (TY.test(word)) return '<span class="t">' + word + '</span>';
        if (FN_WORD.test(word) || FN_NS.test(word)) return '<span class="f">' + word + '</span>';
        return word;
      }
      return m;
    });
  });
});
