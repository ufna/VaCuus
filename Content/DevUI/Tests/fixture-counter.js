// VaCuus M5 Task 1 committed test fixture -- PROVENANCE:
//   preact 10.29.7 (npm), esbuild 0.28.1, node v26.5.0
//   build: npx esbuild src/fixture-counter.js --bundle --format=iife --outfile=fixture-counter.js
//   entry source: the un-bundled tail of this file (esbuild keeps it verbatim);
//   the preceding sections are stock preact/dist internals, unmodified.
// Rebuild by re-running the command above against the same preact version;
//   the in-engine tests pin this bundle's behavior, not its bytes.

(() => {
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __esm = (fn, res, err) => function __init() {
    if (err) throw err[0];
    try {
      return fn && (res = (0, fn[__getOwnPropNames(fn)[0]])(fn = 0)), res;
    } catch (e3) {
      throw err = [e3], e3;
    }
  };
  var __commonJS = (cb, mod) => function __require() {
    try {
      return mod || (0, cb[__getOwnPropNames(cb)[0]])((mod = { exports: {} }).exports, mod), mod.exports;
    } catch (e3) {
      throw mod = 0, e3;
    }
  };

  // node_modules/preact/dist/preact.module.js
  function m(n2, l3) {
    for (var u3 in l3) n2[u3] = l3[u3];
    return n2;
  }
  function b(n2) {
    n2 && n2.parentNode && n2.parentNode.removeChild(n2);
  }
  function k(l3, u3, t3) {
    var i3, r3, o3, e3 = {};
    for (o3 in u3) "key" == o3 ? i3 = u3[o3] : "ref" == o3 ? r3 = u3[o3] : e3[o3] = u3[o3];
    if (arguments.length > 2 && (e3.children = arguments.length > 3 ? n.call(arguments, 2) : t3), "function" == typeof l3 && null != l3.defaultProps) for (o3 in l3.defaultProps) void 0 === e3[o3] && (e3[o3] = l3.defaultProps[o3]);
    return x(l3, e3, i3, r3, null);
  }
  function x(n2, t3, i3, r3, o3) {
    var e3 = { type: n2, props: t3, key: i3, ref: r3, __k: null, __: null, __b: 0, __e: null, __c: null, constructor: void 0, __v: null == o3 ? ++u : o3, __i: -1, __u: 0 };
    return null == o3 && null != l.vnode && l.vnode(e3), e3;
  }
  function S(n2) {
    return n2.children;
  }
  function C(n2, l3) {
    this.props = n2, this.context = l3;
  }
  function $(n2, l3) {
    if (null == l3) return n2.__ ? $(n2.__, n2.__i + 1) : null;
    for (var u3; l3 < n2.__k.length; l3++) if (null != (u3 = n2.__k[l3]) && null != u3.__e) return u3.__e;
    return "function" == typeof n2.type ? $(n2) : null;
  }
  function I(n2) {
    if (n2.__P && n2.__d) {
      var u3 = n2.__v, t3 = u3.__e, i3 = [], r3 = [], o3 = m({}, u3);
      o3.__v = u3.__v + 1, l.vnode && l.vnode(o3), q(n2.__P, o3, u3, n2.__n, n2.__P.namespaceURI, 32 & u3.__u ? [t3] : null, i3, null == t3 ? $(u3) : t3, !!(32 & u3.__u), r3), o3.__v = u3.__v, o3.__.__k[o3.__i] = o3, D(i3, o3, r3), u3.__e = u3.__ = null, o3.__e != t3 && P(o3);
    }
  }
  function P(n2) {
    if (null != (n2 = n2.__) && null != n2.__c) return n2.__e = n2.__c.base = null, n2.__k.some(function(l3) {
      if (null != l3 && null != l3.__e) return n2.__e = n2.__c.base = l3.__e;
    }), P(n2);
  }
  function A(n2) {
    (!n2.__d && (n2.__d = true) && i.push(n2) && !H.__r++ || r != l.debounceRendering) && ((r = l.debounceRendering) || o)(H);
  }
  function H() {
    try {
      for (var n2, l3 = 1; i.length; ) i.length > l3 && i.sort(e), n2 = i.shift(), l3 = i.length, I(n2);
    } finally {
      i.length = H.__r = 0;
    }
  }
  function L(n2, l3, u3, t3, i3, r3, o3, e3, f3, c3, a3) {
    var s3, h2, p3, v3, y3, _2, g2, m3 = t3 && t3.__k || w, b2 = l3.length;
    for (f3 = T(u3, l3, m3, f3, b2), s3 = 0; s3 < b2; s3++) null != (p3 = u3.__k[s3]) && (h2 = -1 != p3.__i && m3[p3.__i] || d, p3.__i = s3, _2 = q(n2, p3, h2, i3, r3, o3, e3, f3, c3, a3), v3 = p3.__e, p3.ref && h2.ref != p3.ref && (h2.ref && J(h2.ref, null, p3), a3.push(p3.ref, p3.__c || v3, p3)), null == y3 && null != v3 && (y3 = v3), (g2 = !!(4 & p3.__u)) || h2.__k === p3.__k ? (f3 = j(p3, f3, n2, g2), g2 && h2.__e && (h2.__e = null)) : "function" == typeof p3.type && void 0 !== _2 ? f3 = _2 : v3 && (f3 = v3.nextSibling), p3.__u &= -7);
    return u3.__e = y3, f3;
  }
  function T(n2, l3, u3, t3, i3) {
    var r3, o3, e3, f3, c3, a3 = u3.length, s3 = a3, h2 = 0;
    for (n2.__k = new Array(i3), r3 = 0; r3 < i3; r3++) null != (o3 = l3[r3]) && "boolean" != typeof o3 && "function" != typeof o3 ? ("string" == typeof o3 || "number" == typeof o3 || "bigint" == typeof o3 || o3.constructor == String ? o3 = n2.__k[r3] = x(null, o3, null, null, null) : g(o3) ? o3 = n2.__k[r3] = x(S, { children: o3 }, null, null, null) : void 0 === o3.constructor && o3.__b > 0 ? o3 = n2.__k[r3] = x(o3.type, o3.props, o3.key, o3.ref ? o3.ref : null, o3.__v) : n2.__k[r3] = o3, f3 = r3 + h2, o3.__ = n2, o3.__b = n2.__b + 1, e3 = null, -1 != (c3 = o3.__i = O(o3, u3, f3, s3)) && (s3--, (e3 = u3[c3]) && (e3.__u |= 2)), null == e3 || null == e3.__v ? (-1 == c3 && (i3 > a3 ? h2-- : i3 < a3 && h2++), "function" != typeof o3.type && (o3.__u |= 4)) : c3 != f3 && (c3 == f3 - 1 ? h2-- : c3 == f3 + 1 ? h2++ : (c3 > f3 ? h2-- : h2++, o3.__u |= 4))) : n2.__k[r3] = null;
    if (s3) for (r3 = 0; r3 < a3; r3++) null != (e3 = u3[r3]) && 0 == (2 & e3.__u) && (e3.__e == t3 && (t3 = $(e3)), K(e3, e3));
    return t3;
  }
  function j(n2, l3, u3, t3) {
    var i3, r3;
    if ("function" == typeof n2.type) {
      for (i3 = n2.__k, r3 = 0; i3 && r3 < i3.length; r3++) i3[r3] && (i3[r3].__ = n2, l3 = j(i3[r3], l3, u3, t3));
      return l3;
    }
    n2.__e != l3 && (t3 && (l3 && n2.type && !l3.parentNode && (l3 = $(n2)), u3.insertBefore(n2.__e, l3 || null)), l3 = n2.__e);
    do {
      l3 = l3 && l3.nextSibling;
    } while (null != l3 && 8 == l3.nodeType);
    return l3;
  }
  function O(n2, l3, u3, t3) {
    var i3, r3, o3, e3 = n2.key, f3 = n2.type, c3 = l3[u3], a3 = null != c3 && 0 == (2 & c3.__u);
    if (null === c3 && null == e3 || a3 && e3 == c3.key && f3 == c3.type) return u3;
    if (t3 > (a3 ? 1 : 0)) {
      for (i3 = u3 - 1, r3 = u3 + 1; i3 >= 0 || r3 < l3.length; ) if (null != (c3 = l3[o3 = i3 >= 0 ? i3-- : r3++]) && 0 == (2 & c3.__u) && e3 == c3.key && f3 == c3.type) return o3;
    }
    return -1;
  }
  function z(n2, l3, u3) {
    "-" == l3[0] ? n2.setProperty(l3, null == u3 ? "" : u3) : n2[l3] = null == u3 ? "" : "number" != typeof u3 || _.test(l3) ? u3 : u3 + "px";
  }
  function N(n2, l3, u3, t3, i3) {
    var r3, o3;
    n: if ("style" == l3) if ("string" == typeof u3) n2.style.cssText = u3;
    else {
      if ("string" == typeof t3 && (n2.style.cssText = t3 = ""), t3) for (l3 in t3) u3 && l3 in u3 || z(n2.style, l3, "");
      if (u3) for (l3 in u3) t3 && u3[l3] == t3[l3] || z(n2.style, l3, u3[l3]);
    }
    else if ("o" == l3[0] && "n" == l3[1]) r3 = l3 != (l3 = l3.replace(s, "$1")), o3 = l3.toLowerCase(), l3 = o3 in n2 || "onFocusOut" == l3 || "onFocusIn" == l3 ? o3.slice(2) : l3.slice(2), n2.l || (n2.l = {}), n2.l[l3 + r3] = u3, u3 ? t3 ? u3[a] = t3[a] : (u3[a] = h, n2.addEventListener(l3, r3 ? v : p, r3)) : n2.removeEventListener(l3, r3 ? v : p, r3);
    else {
      if ("http://www.w3.org/2000/svg" == i3) l3 = l3.replace(/xlink(H|:h)/, "h").replace(/sName$/, "s");
      else if ("width" != l3 && "height" != l3 && "href" != l3 && "list" != l3 && "form" != l3 && "tabIndex" != l3 && "download" != l3 && "rowSpan" != l3 && "colSpan" != l3 && "role" != l3 && "popover" != l3 && l3 in n2) try {
        n2[l3] = null == u3 ? "" : u3;
        break n;
      } catch (n3) {
      }
      "function" == typeof u3 || (null == u3 || false === u3 && "-" != l3[4] ? n2.removeAttribute(l3) : n2.setAttribute(l3, "popover" == l3 && 1 == u3 ? "" : u3));
    }
  }
  function V(n2) {
    return function(u3) {
      if (this.l) {
        var t3 = this.l[u3.type + n2];
        if (null == u3[c]) u3[c] = h++;
        else if (u3[c] < t3[a]) return;
        return t3(l.event ? l.event(u3) : u3);
      }
    };
  }
  function q(n2, u3, t3, i3, r3, o3, e3, f3, c3, a3) {
    var s3, h2, p3, v3, y3, d3, _2, k3, x2, M, $2, I2, P2, A2, H2, T2, j3 = u3.type;
    if (void 0 !== u3.constructor) return null;
    128 & t3.__u && (c3 = !!(32 & t3.__u), o3 = [f3 = u3.__e = t3.__e]), (s3 = l.__b) && s3(u3);
    n: if ("function" == typeof j3) {
      h2 = e3.length;
      try {
        if (x2 = u3.props, M = j3.prototype && j3.prototype.render, $2 = (s3 = j3.contextType) && i3[s3.__c], I2 = s3 ? $2 ? $2.props.value : s3.__ : i3, t3.__c ? k3 = (p3 = u3.__c = t3.__c).__ = p3.__E : (M ? u3.__c = p3 = new j3(x2, I2) : (u3.__c = p3 = new C(x2, I2), p3.constructor = j3, p3.render = Q), $2 && $2.sub(p3), p3.state || (p3.state = {}), p3.__n = i3, v3 = p3.__d = true, p3.__h = [], p3._sb = []), M && null == p3.__s && (p3.__s = p3.state), M && null != j3.getDerivedStateFromProps && (p3.__s == p3.state && (p3.__s = m({}, p3.__s)), m(p3.__s, j3.getDerivedStateFromProps(x2, p3.__s))), y3 = p3.props, d3 = p3.state, p3.__v = u3, v3) M && null == j3.getDerivedStateFromProps && null != p3.componentWillMount && p3.componentWillMount(), M && null != p3.componentDidMount && p3.__h.push(p3.componentDidMount);
        else {
          if (M && null == j3.getDerivedStateFromProps && x2 !== y3 && null != p3.componentWillReceiveProps && p3.componentWillReceiveProps(x2, I2), u3.__v == t3.__v || !p3.__e && null != p3.shouldComponentUpdate && false === p3.shouldComponentUpdate(x2, p3.__s, I2)) {
            u3.__v != t3.__v && (p3.props = x2, p3.state = p3.__s, p3.__d = false), u3.__e = t3.__e, u3.__k = t3.__k, u3.__k.some(function(n3) {
              n3 && (n3.__ = u3);
            }), w.push.apply(p3.__h, p3._sb), p3._sb = [], p3.__h.length && e3.push(p3);
            break n;
          }
          null != p3.componentWillUpdate && p3.componentWillUpdate(x2, p3.__s, I2), M && null != p3.componentDidUpdate && p3.__h.push(function() {
            p3.componentDidUpdate(y3, d3, _2);
          });
        }
        if (p3.context = I2, p3.props = x2, p3.__P = n2, p3.__e = false, P2 = l.__r, A2 = 0, M) p3.state = p3.__s, p3.__d = false, P2 && P2(u3), s3 = p3.render(p3.props, p3.state, p3.context), w.push.apply(p3.__h, p3._sb), p3._sb = [];
        else do {
          p3.__d = false, P2 && P2(u3), s3 = p3.render(p3.props, p3.state, p3.context), p3.state = p3.__s;
        } while (p3.__d && ++A2 < 25);
        p3.state = p3.__s, null != p3.getChildContext && (i3 = m(m({}, i3), p3.getChildContext())), M && !v3 && null != p3.getSnapshotBeforeUpdate && (_2 = p3.getSnapshotBeforeUpdate(y3, d3)), H2 = null != s3 && s3.type === S && null == s3.key ? E(s3.props.children) : s3, f3 = L(n2, g(H2) ? H2 : [H2], u3, t3, i3, r3, o3, e3, f3, c3, a3), p3.base = u3.__e, u3.__u &= -161, p3.__h.length && e3.push(p3), k3 && (p3.__E = p3.__ = null);
      } catch (n3) {
        if (e3.length = h2, u3.__v = null, c3 || null != o3) {
          if (n3.then) {
            for (u3.__u |= c3 ? 160 : 128; f3 && 8 == f3.nodeType && f3.nextSibling; ) f3 = f3.nextSibling;
            null != o3 && (o3[o3.indexOf(f3)] = null), u3.__e = f3;
          } else if (null != o3) for (T2 = o3.length; T2--; ) b(o3[T2]);
        } else u3.__e = t3.__e;
        null == u3.__k && (u3.__k = t3.__k || []), n3.then || B(u3), l.__e(n3, u3, t3);
      }
    } else null == o3 && u3.__v == t3.__v ? (u3.__k = t3.__k, u3.__e = t3.__e) : f3 = u3.__e = G(t3.__e, u3, t3, i3, r3, o3, e3, c3, a3);
    return (s3 = l.diffed) && s3(u3), 128 & u3.__u ? void 0 : f3;
  }
  function B(n2) {
    n2 && (n2.__c && (n2.__c.__e = true), n2.__k && n2.__k.some(B));
  }
  function D(n2, u3, t3) {
    for (var i3 = 0; i3 < t3.length; i3++) J(t3[i3], t3[++i3], t3[++i3]);
    l.__c && l.__c(u3, n2), n2.some(function(u4) {
      try {
        n2 = u4.__h, u4.__h = [], n2.some(function(n3) {
          n3.call(u4);
        });
      } catch (n3) {
        l.__e(n3, u4.__v);
      }
    });
  }
  function E(n2) {
    return "object" != typeof n2 || null == n2 || n2.__b > 0 ? n2 : g(n2) ? n2.map(E) : void 0 !== n2.constructor ? null : m({}, n2);
  }
  function G(u3, t3, i3, r3, o3, e3, f3, c3, a3) {
    var s3, h2, p3, v3, y3, w3, _2, m3 = i3.props || d, k3 = t3.props, x2 = t3.type;
    if ("svg" == x2 ? o3 = "http://www.w3.org/2000/svg" : "math" == x2 ? o3 = "http://www.w3.org/1998/Math/MathML" : o3 || (o3 = "http://www.w3.org/1999/xhtml"), null != e3) {
      for (s3 = 0; s3 < e3.length; s3++) if ((y3 = e3[s3]) && "setAttribute" in y3 == !!x2 && (x2 ? y3.localName == x2 : 3 == y3.nodeType)) {
        u3 = y3, e3[s3] = null;
        break;
      }
    }
    if (null == u3) {
      if (null == x2) return document.createTextNode(k3);
      u3 = document.createElementNS(o3, x2, k3.is && k3), c3 && (l.__m && l.__m(t3, e3), c3 = false), e3 = null;
    }
    if (null == x2) m3 === k3 || c3 && u3.data == k3 || (u3.data = k3);
    else {
      if (e3 = "textarea" == x2 && null != k3.defaultValue ? null : e3 && n.call(u3.childNodes), !c3 && null != e3) for (m3 = {}, s3 = 0; s3 < u3.attributes.length; s3++) m3[(y3 = u3.attributes[s3]).name] = y3.value;
      for (s3 in m3) y3 = m3[s3], "dangerouslySetInnerHTML" == s3 ? p3 = y3 : "children" == s3 || s3 in k3 || "value" == s3 && "defaultValue" in k3 || "checked" == s3 && "defaultChecked" in k3 || N(u3, s3, null, y3, o3);
      for (s3 in k3) y3 = k3[s3], "children" == s3 ? v3 = y3 : "dangerouslySetInnerHTML" == s3 ? h2 = y3 : "value" == s3 ? w3 = y3 : "checked" == s3 ? _2 = y3 : c3 && "function" != typeof y3 || m3[s3] === y3 || N(u3, s3, y3, m3[s3], o3);
      if (h2) c3 || p3 && (h2.__html == p3.__html || h2.__html == u3.innerHTML) || (u3.innerHTML = h2.__html), t3.__k = [];
      else if (p3 && (u3.innerHTML = ""), L("template" == t3.type ? u3.content : u3, g(v3) ? v3 : [v3], t3, i3, r3, "foreignObject" == x2 ? "http://www.w3.org/1999/xhtml" : o3, e3, f3, e3 ? e3[0] : i3.__k && $(i3, 0), c3, a3), null != e3) for (s3 = e3.length; s3--; ) b(e3[s3]);
      c3 && "textarea" != x2 || (s3 = "value", "progress" == x2 && null == w3 ? u3.removeAttribute("value") : null != w3 && (w3 !== u3[s3] || "progress" == x2 && !w3 || "option" == x2 && w3 != m3[s3]) && N(u3, s3, w3, m3[s3], o3), s3 = "checked", null != _2 && _2 != u3[s3] && N(u3, s3, _2, m3[s3], o3));
    }
    return u3;
  }
  function J(n2, u3, t3) {
    try {
      if ("function" == typeof n2) {
        var i3 = "function" == typeof n2.__u;
        i3 && n2.__u(), i3 && null == u3 || (n2.__u = n2(u3));
      } else n2.current = u3;
    } catch (n3) {
      l.__e(n3, t3);
    }
  }
  function K(n2, u3, t3) {
    var i3, r3;
    if (l.unmount && l.unmount(n2), (i3 = n2.ref) && (i3.current && i3.current != n2.__e || J(i3, null, u3)), null != (i3 = n2.__c)) {
      if (i3.componentWillUnmount) try {
        i3.componentWillUnmount();
      } catch (n3) {
        l.__e(n3, u3);
      }
      i3.base = i3.__P = i3.__n = null;
    }
    if (i3 = n2.__k) for (r3 = 0; r3 < i3.length; r3++) i3[r3] && K(i3[r3], u3, t3 || "function" != typeof n2.type);
    t3 || b(n2.__e), n2.__c = n2.__ = n2.__e = void 0;
  }
  function Q(n2, l3, u3) {
    return this.constructor(n2, u3);
  }
  function R(u3, t3, i3) {
    var r3, o3, e3, f3;
    t3 == document && (t3 = document.documentElement), l.__ && l.__(u3, t3), o3 = (r3 = "function" == typeof i3) ? null : i3 && i3.__k || t3.__k, e3 = [], f3 = [], q(t3, u3 = (!r3 && i3 || t3).__k = k(S, null, [u3]), o3 || d, d, t3.namespaceURI, !r3 && i3 ? [i3] : o3 ? null : t3.firstChild ? n.call(t3.childNodes) : null, e3, !r3 && i3 ? i3 : o3 ? o3.__e : t3.firstChild, r3, f3), D(e3, u3, f3), u3.props.children = null;
  }
  var n, l, u, t, i, r, o, e, f, c, a, s, h, p, v, y, d, w, _, g;
  var init_preact_module = __esm({
    "node_modules/preact/dist/preact.module.js"() {
      d = {};
      w = [];
      _ = /acit|ex(?:s|g|n|p|$)|rph|grid|ows|mnc|ntw|ine[ch]|zoo|^ord|itera/i;
      g = Array.isArray;
      n = w.slice, l = { __e: function(n2, l3, u3, t3) {
        for (var i3, r3, o3; l3 = l3.__; ) if ((i3 = l3.__c) && !i3.__) try {
          if ((r3 = i3.constructor) && null != r3.getDerivedStateFromError && (i3.setState(r3.getDerivedStateFromError(n2)), o3 = i3.__d), null != i3.componentDidCatch && (i3.componentDidCatch(n2, t3 || {}), o3 = i3.__d), o3) return i3.__E = i3;
        } catch (l4) {
          n2 = l4;
        }
        throw n2;
      } }, u = 0, t = function(n2) {
        return null != n2 && void 0 === n2.constructor;
      }, C.prototype.setState = function(n2, l3) {
        var u3;
        u3 = null != this.__s && this.__s != this.state ? this.__s : this.__s = m({}, this.state), "function" == typeof n2 && (n2 = n2(m({}, u3), this.props)), n2 && m(u3, n2), null != n2 && this.__v && (l3 && this._sb.push(l3), A(this));
      }, C.prototype.forceUpdate = function(n2) {
        this.__v && (this.__e = true, n2 && this.__h.push(n2), A(this));
      }, C.prototype.render = S, i = [], o = "function" == typeof Promise ? Promise.prototype.then.bind(Promise.resolve()) : setTimeout, e = function(n2, l3) {
        return n2.__v.__b - l3.__v.__b;
      }, H.__r = 0, f = Math.random().toString(8), c = "__d" + f, a = "__a" + f, s = /(PointerCapture)$|Capture$/i, h = 0, p = V(false), v = V(true), y = 0;
    }
  });

  // node_modules/preact/hooks/dist/hooks.module.js
  function s2(n2, t3) {
    c2.__h && c2.__h(r2, n2, o2 || t3), o2 = 0;
    var u3 = r2.__H || (r2.__H = { __: [], __h: [] });
    return n2 >= u3.__.length && u3.__.push({}), u3.__[n2];
  }
  function d2(n2) {
    return o2 = 1, y2(D2, n2);
  }
  function y2(n2, u3, i3) {
    var o3 = s2(t2++, 2);
    if (o3.t = n2, !o3.__c && (o3.__ = [i3 ? i3(u3) : D2(void 0, u3), function(n3) {
      var t3 = o3.__N ? o3.__N[0] : o3.__[0], r3 = o3.t(t3, n3);
      t3 !== r3 && (o3.__N = [r3, o3.__[1]], o3.__c.setState({}));
    }], o3.__c = r2, !r2.__f)) {
      var f3 = function(n3, t3, r3) {
        if (!o3.__c.__H) return true;
        var u4 = false, i4 = o3.__c.props !== n3;
        if (o3.__c.__H.__.some(function(n4) {
          if (n4.__N) {
            u4 = true;
            var t4 = n4.__[0];
            n4.__ = n4.__N, n4.__N = void 0, t4 !== n4.__[0] && (i4 = true);
          }
        }), c3) {
          var f4 = c3.call(this, n3, t3, r3);
          return u4 ? f4 || i4 : f4;
        }
        return !u4 || i4;
      };
      r2.__f = true;
      var c3 = r2.shouldComponentUpdate, e3 = r2.componentWillUpdate;
      r2.componentWillUpdate = function(n3, t3, r3) {
        if (this.__e) {
          var u4 = c3;
          c3 = void 0, f3(n3, t3, r3), c3 = u4;
        }
        e3 && e3.call(this, n3, t3, r3);
      }, r2.shouldComponentUpdate = f3;
    }
    return o3.__N || o3.__;
  }
  function j2() {
    for (var n2; n2 = f2.shift(); ) {
      var t3 = n2.__H;
      if (n2.__P && t3) try {
        t3.__h.some(z2), t3.__h.some(B2), t3.__h = [];
      } catch (r3) {
        t3.__h = [], c2.__e(r3, n2.__v);
      }
    }
  }
  function w2(n2) {
    var t3, r3 = function() {
      clearTimeout(u3), k2 && cancelAnimationFrame(t3), setTimeout(n2);
    }, u3 = setTimeout(r3, 35);
    k2 && (t3 = requestAnimationFrame(r3));
  }
  function z2(n2) {
    var t3 = r2, u3 = n2.__c;
    "function" == typeof u3 && (n2.__c = void 0, u3()), r2 = t3;
  }
  function B2(n2) {
    var t3 = r2;
    n2.__c = n2.__(), r2 = t3;
  }
  function D2(n2, t3) {
    return "function" == typeof t3 ? t3(n2) : t3;
  }
  var t2, r2, u2, i2, o2, f2, c2, e2, a2, v2, l2, m2, p2, k2;
  var init_hooks_module = __esm({
    "node_modules/preact/hooks/dist/hooks.module.js"() {
      init_preact_module();
      o2 = 0;
      f2 = [];
      c2 = l;
      e2 = c2.__b;
      a2 = c2.__r;
      v2 = c2.diffed;
      l2 = c2.__c;
      m2 = c2.unmount;
      p2 = c2.__;
      c2.__b = function(n2) {
        r2 = null, e2 && e2(n2);
      }, c2.__ = function(n2, t3) {
        n2 && t3.__k && t3.__k.__m && (n2.__m = t3.__k.__m), p2 && p2(n2, t3);
      }, c2.__r = function(n2) {
        a2 && a2(n2), t2 = 0;
        var i3 = (r2 = n2.__c).__H;
        i3 && (u2 === r2 ? (i3.__h = [], r2.__h = [], i3.__.some(function(n3) {
          n3.__N && (n3.__ = n3.__N), n3.u = n3.__N = void 0;
        })) : (i3.__h.some(z2), i3.__h.some(B2), i3.__h = [], t2 = 0)), u2 = r2;
      }, c2.diffed = function(n2) {
        v2 && v2(n2);
        var t3 = n2.__c;
        t3 && t3.__H && (t3.__H.__h.length && (1 !== f2.push(t3) && i2 === c2.requestAnimationFrame || ((i2 = c2.requestAnimationFrame) || w2)(j2)), t3.__H.__.some(function(n3) {
          n3.u && (n3.__H = n3.u, n3.u = void 0);
        })), u2 = r2 = null;
      }, c2.__c = function(n2, t3) {
        t3.some(function(n3) {
          try {
            n3.__h.some(z2), n3.__h = n3.__h.filter(function(n4) {
              return !n4.__ || B2(n4);
            });
          } catch (r3) {
            t3.some(function(n4) {
              n4.__h && (n4.__h = []);
            }), t3 = [], c2.__e(r3, n3.__v);
          }
        }), l2 && l2(n2, t3);
      }, c2.unmount = function(n2) {
        m2 && m2(n2);
        var t3, r3 = n2.__c;
        r3 && r3.__H && (r3.__H.__.some(function(n3) {
          try {
            z2(n3);
          } catch (n4) {
            t3 = n4;
          }
        }), r3.__H = void 0, t3 && c2.__e(t3, r3.__v));
      };
      k2 = "function" == typeof requestAnimationFrame;
    }
  });

  // src/fixture-counter.js
  var require_fixture_counter = __commonJS({
    "src/fixture-counter.js"() {
      init_preact_module();
      init_hooks_module();
      var LOG = globalThis.EPLOG = [];
      var log = (m3) => LOG.push(m3);
      var docProto = Object.getPrototypeOf(document);
      var origCTN = docProto.createTextNode;
      docProto.createTextNode = function(t3) {
        log("createTextNode(" + JSON.stringify(t3) + ")");
        return origCTN.call(this, t3);
      };
      var elProto = Object.getPrototypeOf(document.createElement("div"));
      var dataDesc = Object.getOwnPropertyDescriptor(elProto, "data");
      Object.defineProperty(elProto, "data", {
        configurable: true,
        get() {
          return dataDesc.get.call(this);
        },
        set(v3) {
          log("data=" + JSON.stringify(v3));
          dataDesc.set.call(this, v3);
        }
      });
      function Counter() {
        const [n2, setN] = d2(0);
        globalThis.bump = () => setN((v3) => v3 + 1);
        return k("div", { id: "ctr" }, "count:", n2);
      }
      try {
        R(k(Counter, null), document.getElementById("mount"));
      } catch (e3) {
        log("FIXTURE THREW: " + e3);
      }
    }
  });
  require_fixture_counter();
})();
