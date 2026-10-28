// Injected into QQ Music's web pages after web_dict.js (window.__qqenDict).
// Replaces Chinese UI text with English in text nodes and in title/placeholder
// attributes, and keeps doing so as the page re-renders. Text it cannot translate
// is collected for the add-on, which polls window.__qqenTakeMissing().
(function () {
  if (window.__qqen) return;
  window.__qqen = true;
  var dict = window.__qqenDict || {};
  var HAN = /[一-鿿]/;
  var RUN = /[　-鿿＀-￯]+/g;
  var EDGE = /^[^一-鿿]+|[^一-鿿]+$/g;
  var ALNUM = /[A-Za-z0-9]/;
  var SKIP = { SCRIPT: 1, STYLE: 1, TEXTAREA: 1, INPUT: 1, NOSCRIPT: 1, CODE: 1 };
  var seen = {}, missing = [];

  function note(t) {
    if (!seen[t] && t.length <= 200) { seen[t] = 1; missing.push(t); }
  }

  // printf-style entries ("共%d首歌曲") from window.__qqenTpl, as anchored regexes.
  var FIELD = /%[-0-9.]*(?:l|ll|I64)?([dius])/g;
  var templates = (window.__qqenTpl || []).map(function (p) {
    var parts = p[0].split(/(%[-0-9.]*(?:l|ll|I64)?[dius])/);
    var src = parts.map(function (x, i) {
      if (i % 2) return /[dui]$/.test(x) ? "(-?\\d+)" : "(.+?)";
      return x.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }).join("");
    return { re: new RegExp("^" + src + "$"), en: p[1] };
  });

  function fill(en, caps) {
    var k = 0;
    return en.replace(FIELD, function () {
      var c = caps[k++];
      return c === undefined ? "" : (dict[c] !== undefined ? dict[c] : c);
    });
  }

  // 万 = 10^4, 亿 = 10^8: "2494.0万" -> "24.9M".
  function numbers(t) {
    return t.replace(/(\d+(?:\.\d+)?)([万亿])/g, function (m, n, u) {
      var v = parseFloat(n) * (u === "万" ? 1e4 : 1e8);
      var d = v >= 1e9 ? 1e9 : v >= 1e6 ? 1e6 : 1e3;
      var x = v / d;
      return (x >= 100 || x === Math.round(x) ? x.toFixed(0) : x.toFixed(1)) + (d === 1e9 ? "B" : d === 1e6 ? "M" : "K");
    });
  }

  // Each run of Chinese translated on its own ("Hi wol 今日为你推荐"), if every run is known.
  function compose(t) {
    var ok = true;
    var out = t.replace(RUN, function (run, at) {
      var core = run.replace(EDGE, "");
      if (!core) return run;
      var en = dict[core];
      if (en === undefined) { ok = false; return run; }
      var i = run.indexOf(core);
      var pre = run.slice(0, i), post = run.slice(i + core.length);
      var before = at > 0 ? t.charAt(at - 1) : "", after = t.charAt(at + run.length);
      return pre + (!pre && ALNUM.test(before) ? " " : "") + en + (!post && ALNUM.test(after) ? " " : "") + post;
    });
    return ok ? out : null;
  }

  function translate(s) {
    if (!s || !HAN.test(s)) return null;
    var t = s.trim();
    if (t.length < 2) return null;
    var en = dict[t];
    for (var i = 0; en === undefined && i < templates.length; i++) {
      var m = templates[i].re.exec(t);
      if (m) en = fill(templates[i].en, m.slice(1));
    }
    if (en === undefined) en = compose(numbers(t));
    if (en === null || en === undefined) { note(t); return null; }
    return s.replace(t, en);
  }

  function editable(el) {
    for (; el; el = el.parentElement) {
      if (SKIP[el.tagName] || el.isContentEditable) return true;
    }
    return false;
  }

  function doText(node) {
    var en = translate(node.nodeValue);
    if (en !== null && !editable(node.parentElement)) node.nodeValue = en;
  }

  function doAttrs(el) {
    var names = ["title", "placeholder", "aria-label", "alt"];
    for (var i = 0; i < names.length; i++) {
      var v = el.getAttribute && el.getAttribute(names[i]);
      if (v) {
        var en = translate(v);
        if (en !== null) el.setAttribute(names[i], en);
      }
    }
  }

  function walk(root) {
    if (!root) return;
    if (root.nodeType === 3) { doText(root); return; }
    if (root.nodeType !== 1 && root.nodeType !== 9 && root.nodeType !== 11) return;
    if (root.nodeType === 1) {
      if (SKIP[root.tagName]) return;
      doAttrs(root);
    }
    var w = document.createTreeWalker(root, 5 /* SHOW_ELEMENT | SHOW_TEXT */, null, false);
    for (var n = w.nextNode(); n; n = w.nextNode()) {
      if (n.nodeType === 3) doText(n);
      else doAttrs(n);
    }
  }

  function start() {
    walk(document.body || document.documentElement);
    if (document.title) {
      var t = translate(document.title);
      if (t !== null) document.title = t;
    }
    new MutationObserver(function (list) {
      for (var i = 0; i < list.length; i++) {
        var m = list[i];
        if (m.type === "characterData") doText(m.target);
        else if (m.type === "attributes") doAttrs(m.target);
        else for (var j = 0; j < m.addedNodes.length; j++) walk(m.addedNodes[j]);
      }
    }).observe(document.documentElement, {
      childList: true, subtree: true, characterData: true,
      attributes: true, attributeFilter: ["title", "placeholder", "aria-label", "alt"]
    });
  }

  window.__qqenTakeMissing = function () {
    var r = missing.join("\n");
    missing = [];
    return r;
  };

  if (document.documentElement) start();
  else document.addEventListener("DOMContentLoaded", start);
})();
