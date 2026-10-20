/*!
 * VaCuus stock Preact fixtures: preact 11.0.1, esbuild 0.28.2.
 * Rebuild: node Web/build-stock-fixtures.mjs
 * Sources: Web/apps/ep-fixture/src/stock-{dom,counter}.js
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-present Jason Miller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
(() => {
  // Web/node_modules/preact/dist/preact.mjs
  var n;
  var t;
  var i;
  var r;
  var u;
  var f;
  var o;
  var e;
  var l;
  var c;
  var a;
  var s;
  var h = {};
  var p = [];
  var v = /^m(i|n|o|s|text|space)$/;
  var y = Array.isArray;
  var d = p.slice;
  var w = Object.assign;
  function _(n2) {
    n2 && n2.parentNode && n2.remove();
  }
  function g(n2, t2, i2) {
    var r2, u2, f2, o2 = {}, e2 = arguments.length;
    for (f2 in t2) "key" == f2 ? r2 = t2[f2] : "ref" == f2 && "function" != typeof n2 ? u2 = t2[f2] : o2[f2] = t2[f2];
    return e2 > 2 && (o2.children = e2 > 3 ? d.call(arguments, 2) : i2), b(n2, o2, r2, u2, null);
  }
  function b(i2, r2, u2, f2, o2) {
    var e2 = { type: i2, props: r2, key: u2, ref: f2, __k: null, __: null, __b: 0, __e: null, __c: null, constructor: void 0, __v: o2 || ++t, __i: -1, __u: 0 };
    return !o2 && n.vnode && n.vnode(e2), e2;
  }
  function M(n2) {
    return n2.children;
  }
  function $(n2, t2) {
    this.props = n2, this.context = t2, this.__g = 0;
  }
  function x(n2, t2) {
    if (null == t2) return n2.__ ? x(n2.__, n2.__i + 1) : null;
    for (var i2; t2 < n2.__k.length; t2++) if ((i2 = n2.__k[t2]) && i2.__e) return i2.__e;
    return "function" != typeof n2.type || n2.props.__P ? null : x(n2);
  }
  function S(n2) {
    if ((n2 = n2.__) && n2.__c && !n2.props.__P) return n2.__e = null, n2.__k.some(function(t2) {
      return t2 && (n2.__e = t2.__e);
    }), S(n2);
  }
  function C(t2) {
    (8 & t2.__g || !(t2.__g |= 8) || !r.push(t2) || f++) && u == n.debounceRendering || ((u = n.debounceRendering) || queueMicrotask)(j);
  }
  function j() {
    var t2, i2, u2, e2, l2, c2, a2, s2, h2;
    try {
      for (i2 = 1; r.length; ) r.length > i2 && r.sort(o), t2 = r.shift(), i2 = r.length, 8 & t2.__g && (e2 = void 0, l2 = void 0, c2 = (l2 = (u2 = t2).__v).__e, a2 = [], s2 = [], (h2 = u2.__P) && ((e2 = w({ constructor: void 0 }, l2)).__v = l2.__v + 1, n.vnode && n.vnode(e2), N(h2, e2, l2, u2.__n, h2.namespaceURI, 32 & l2.__u ? [c2] : null, a2, c2 || x(l2), 32 & l2.__u, s2), e2.__v = l2.__v, e2.__.__k[e2.__i] = e2, z(a2, e2, s2), l2.__ = l2.__e = null, e2.__e != c2 && S(e2)));
    } finally {
      r.length = f = 0;
    }
  }
  function L(n2, t2, i2, r2, u2, f2, o2, e2, l2, c2, a2) {
    var s2, v2, y2, d2, w2, _2, g2 = r2.__k || p, k = t2.length;
    for (l2 = H(i2, t2, g2, l2, k), s2 = 0; s2 < k; s2++) null != (y2 = i2.__k[s2]) && (v2 = ~y2.__i && g2[y2.__i] || h, y2.__i = s2, _2 = N(n2, y2, v2, u2, f2, o2, e2, l2, c2, a2), d2 = y2.__e, (v2.ref != y2.ref || 8 & v2.__u) && (v2.ref && D(v2.ref, null, y2, v2), y2.ref && a2.push(y2.ref, y2.__c || d2, y2)), w2 = w2 || d2, 4 & y2.__u ? (l2 = I(y2, l2, n2, !v2.__v), v2.__e && (v2.__e = null)) : "function" == typeof y2.type && void 0 !== _2 ? l2 = _2 : d2 && (l2 = d2.nextSibling), y2.__u &= -7);
    return i2.__e = w2, l2;
  }
  function H(n2, t2, i2, r2, u2) {
    var f2, o2, e2, l2, c2, a2, s2, h2, p2, v2, d2 = i2.length, w2 = d2, _2 = 0, g2 = false, k = n2.__k = Array(u2);
    for (f2 = 0; f2 < u2; f2++) null != (o2 = t2[f2]) && "boolean" != typeof o2 && "function" != typeof o2 ? ("object" != typeof o2 || o2.constructor == String ? o2 = k[f2] = b(null, o2) : y(o2) ? o2 = k[f2] = b(M, { children: o2 }) : void 0 === o2.constructor && o2.__b ? o2 = k[f2] = b(o2.type, o2.props, o2.key, o2.ref, o2.__v) : k[f2] = o2, l2 = f2 + _2, o2.__ = n2, o2.__b = n2.__b + 1, e2 = null, ~(c2 = o2.__i = O(o2, i2, l2, w2)) && (w2--, (e2 = i2[c2]) && (e2.__u |= 2)), e2 && e2.__v ? (o2.__u |= 2, c2 == l2 - 1 ? _2-- : c2 == l2 + 1 ? _2++ : c2 != l2 && (c2 > l2 ? _2-- : _2++, g2 = true)) : (~c2 || (u2 > d2 ? _2-- : u2 < d2 && _2++), "function" != typeof o2.type && (o2.__u |= 4))) : k[f2] = null;
    if (g2) {
      for (a2 = [], s2 = [], f2 = 0; f2 < u2; f2++) if ((o2 = k[f2]) && 2 & o2.__u) {
        for (h2 = 0, p2 = a2.length; h2 < p2; ) a2[v2 = h2 + p2 >> 1] < o2.__i ? h2 = v2 + 1 : p2 = v2;
        a2[h2] = o2.__i, s2[f2] = h2 + 1;
      }
      for (_2 = a2.length; f2--; ) s2[f2] && (s2[f2] == _2 ? _2-- : k[f2].__u |= 4);
    }
    if (w2) for (f2 = 0; f2 < d2; f2++) !(e2 = i2[f2]) || 2 & e2.__u || (e2.__e == r2 && (r2 = x(e2)), E(e2, e2));
    return r2;
  }
  function I(n2, t2, i2, r2) {
    var u2, f2, o2;
    if ("function" == typeof n2.type) {
      if (n2.props.__P) return t2;
      if (u2 = n2.__k) for (f2 = 0; f2 < u2.length; f2++) u2[f2] && (u2[f2].__ = n2, t2 = I(u2[f2], t2, i2, false));
      return t2;
    }
    for (t2 && !t2.parentNode && (t2 = x(n2)) && !t2.parentNode && (t2 = null), o2 = t2; o2 && 8 == o2.nodeType; ) o2 = o2.nextSibling;
    for (n2.__e != o2 && (!r2 && i2.moveBefore && n2.__e.parentNode ? i2.moveBefore(n2.__e, t2) : i2.insertBefore(n2.__e, t2 || null)), t2 = n2.__e; (t2 = t2 && t2.nextSibling) && 8 == t2.nodeType; ) ;
    return t2;
  }
  function O(n2, t2, i2, r2) {
    var u2, f2, o2, e2 = n2.key, l2 = n2.type, c2 = t2[i2], a2 = c2 && !(2 & c2.__u);
    if (null === c2 && null == e2 || a2 && e2 == c2.key && l2 == c2.type) return i2;
    if (r2 > (a2 ? 1 : 0)) {
      for (u2 = i2 - 1, f2 = i2 + 1; u2 >= 0 || f2 < t2.length; ) if ((c2 = t2[o2 = u2 >= 0 ? u2-- : f2++]) && !(2 & c2.__u) && e2 == c2.key && l2 == c2.type) return o2;
    }
    return -1;
  }
  function P(n2, t2, i2) {
    null == i2 && (i2 = ""), "-" == t2[0] ? n2.setProperty(t2, i2) : n2[t2] = i2;
  }
  function T(n2, t2, i2, r2, u2) {
    var f2, o2;
    n: if ("style" == t2) if ("string" == typeof i2) n2.style.cssText = i2;
    else {
      if ("string" == typeof r2 && (n2.style.cssText = r2 = ""), r2) for (t2 in r2) i2 && t2 in i2 || P(n2.style, t2, "");
      if (i2) for (t2 in i2) r2 && i2[t2] == r2[t2] || P(n2.style, t2, i2[t2]);
    }
    else if ("o" == t2[0] && "n" == t2[1]) (n2.__e || (n2.__e = {}))[t2] = i2, i2 && r2 || (o2 = a[t2] || (a[t2] = q(t2)), (n2.__a || (n2.__a = {}))[t2] = c, f2 = t2 != (t2 = t2.replace(l, "$1")), (t2 = t2.slice(2))[0] < "a" && (t2 = t2.toLowerCase()), i2 ? n2.addEventListener(t2, o2, f2) : n2.removeEventListener(t2, o2, f2));
    else {
      if ("http://www.w3.org/2000/svg" == u2) t2 = t2.replace(/xlink(H|:h)/, "h").replace(/sName$/, "s");
      else if ("width" != t2 && "height" != t2 && "href" != t2 && "list" != t2 && "form" != t2 && "tabIndex" != t2 && "download" != t2 && "rowSpan" != t2 && "colSpan" != t2 && "role" != t2 && "popover" != t2 && t2 in n2) try {
        n2[t2] = null == i2 ? "" : i2;
        break n;
      } catch (n3) {
      }
      "function" == typeof i2 || (null == i2 || false === i2 && "-" != t2[4] ? n2.removeAttribute(t2) : n2.setAttribute(t2, "popover" == t2 && 1 == i2 ? "" : i2));
    }
  }
  function q(t2) {
    return function(i2) {
      if (this.__e) {
        var r2 = this.__e[t2];
        if (null == i2[e]) i2[e] = c++;
        else if (i2[e] < this.__a[t2]) return;
        return r2(n.event ? n.event(i2) : i2);
      }
    };
  }
  function N(t2, i2, r2, u2, f2, o2, e2, l2, c2, a2) {
    var s2, h2, v2, d2, g2, k, b2, m, S2, C2, j2, H2, I2, A, O2, P2, T2, q2, N2, z2, D2 = i2.type;
    if (void 0 !== i2.constructor) return null;
    if (128 & r2.__u && (c2 = 32 & r2.__u, s2 = r2.__c.__z)) {
      if (i2.__u |= c2, h2 = o2 = [], 8 == s2.nodeType) for (v2 = 1, d2 = s2.nextSibling; d2; d2 = d2.nextSibling) {
        if (8 == d2.nodeType) {
          if (d2.data.startsWith("$s")) v2++;
          else if (d2.data.startsWith("/$s") && !--v2) break;
        }
        o2.push(d2);
      }
      else o2.push(s2);
      l2 = o2[0];
    }
    (s2 = n.__b) && s2(i2);
    n: if ("function" == typeof D2) {
      g2 = e2.length;
      try {
        if (C2 = i2.props, j2 = (s2 = D2.prototype) && s2.render, H2 = (s2 = D2.contextType) && u2[s2.__c], I2 = s2 ? H2 ? H2.props.value : s2.__ : u2, r2.__c ? 2 & (k = i2.__c = r2.__c).__g && (k.__g |= 1) : (j2 ? i2.__c = k = new D2(C2, I2) : (i2.__c = k = new $(C2, I2), k.constructor = D2, k.render = F), H2 && H2.sub(k), k.state || (k.state = {}), k.__n = u2, k.__g |= 8, k.__h = [], k.__k = []), j2 && (k.__s || (k.__s = k.state), D2.getDerivedStateFromProps && (k.__s == k.state && (k.__s = w({}, k.__s)), w(k.__s, D2.getDerivedStateFromProps(C2, k.__s)))), b2 = k.props, m = k.state, k.__v = i2, r2.__c) {
          if (j2 && !D2.getDerivedStateFromProps && C2 !== b2 && k.componentWillReceiveProps && k.componentWillReceiveProps(C2, I2), i2.__v == r2.__v && !(8 & k.__g) || !(4 & k.__g) && k.shouldComponentUpdate && false === k.shouldComponentUpdate(C2, k.__s, I2)) {
            i2.__v != r2.__v && (k.props = C2, k.state = k.__s, k.__g &= -9), i2.__e = r2.__e, i2.__k = r2.__k, i2.__k.some(function(n2) {
              n2 && (n2.__ = i2);
            }), p.push.apply(k.__h, k.__k), k.__k = [], k.__h.length && e2.push(k), l2 = x(r2);
            break n;
          }
          k.componentWillUpdate && k.componentWillUpdate(C2, k.__s, I2), j2 && k.componentDidUpdate && k.__h.push(function() {
            k.componentDidUpdate(b2, m, S2);
          });
        } else j2 && !D2.getDerivedStateFromProps && k.componentWillMount && k.componentWillMount(), j2 && k.componentDidMount && k.__h.push(k.componentDidMount);
        if (k.context = I2, k.props = C2, k.__P = t2, k.__g &= -5, A = n.__r, O2 = 0, j2) k.state = k.__s, k.__g &= -9, A && A(i2), s2 = k.render(k.props, k.state, k.context), p.push.apply(k.__h, k.__k), k.__k = [];
        else do {
          k.__g &= -9, A && A(i2), s2 = k.render(k.props, k.state, k.context), k.state = k.__s;
        } while (8 & k.__g && ++O2 < 25);
        k.state = k.__s, k.getChildContext && (u2 = w({}, u2, k.getChildContext())), j2 && r2.__c && k.getSnapshotBeforeUpdate && (S2 = k.getSnapshotBeforeUpdate(b2, m)), P2 = s2 && s2.type === M && null == s2.key ? s2.props.children : s2, C2.__P && (s2 = l2, f2 = (t2 = C2.__P).namespaceURI, c2 = o2 = null, r2.props && r2.props.__P != t2 && (r2.__k.some(function(n2) {
          n2 && E(n2, n2);
        }), r2.__k = null), l2 = r2.__k ? x(r2, 0) : null), l2 = L(t2, y(P2) ? P2 : [P2], i2, r2, u2, f2, o2, e2, l2, c2, a2), C2.__P && (i2.__e = null, l2 = s2), i2.__u &= -161, 128 & r2.__u && (k.__z = null), h2 && h2.some(_), k.__h.length && e2.push(k), 1 & k.__g && (k.__g &= -4);
      } catch (t3) {
        if (e2.length = g2, i2.__v = null, c2 || o2) if (t3.then) {
          if (T2 = 0, i2.__u |= c2 ? 160 : 128, o2) {
            for (~(N2 = o2.indexOf(l2 || void 0)) || (N2 = o2.length); (z2 = o2[N2 - 1]) && 8 == z2.nodeType; ) N2--;
            for (; N2 < o2.length; N2++) if (z2 = o2[N2]) {
              if (o2[N2] = null, 8 == z2.nodeType) {
                if (z2.data.startsWith("$s")) T2++ || (q2 = z2);
                else if (T2 && z2.data.startsWith("/$s") && !--T2) {
                  l2 = z2;
                  break;
                }
              } else if (!T2) break;
            }
          }
          if (!q2) {
            for (; l2 && 8 == l2.nodeType && l2.nextSibling; ) l2 = l2.nextSibling;
            q2 = l2;
          }
          i2.__c.__z || (i2.__c.__z = q2), i2.__e = l2;
        } else o2 && o2.some(_);
        else i2.__e = r2.__e;
        i2.__k || (i2.__k = r2.__k || []), t3.then || V(i2), n.__e(t3, i2, r2);
      }
    } else l2 = i2.__e = B(r2.__e, i2, r2, u2, f2, o2, e2, c2, a2, t2);
    return (s2 = n.diffed) && s2(i2), 128 & i2.__u ? void 0 : l2;
  }
  function V(n2) {
    n2 && (n2.__c && (n2.__c.__g |= 4), n2.__k && n2.__k.some(V));
  }
  function z(t2, i2, r2) {
    for (var u2 = 0; u2 < r2.length; ) D(r2[u2++], r2[u2++], r2[u2++]);
    n.__c && n.__c(i2, t2), t2.some(function(i3) {
      try {
        t2 = i3.__h, i3.__h = [], t2.some(function(n2) {
          n2.call(i3);
        });
      } catch (t3) {
        n.__e(t3, i3.__v);
      }
    });
  }
  function B(t2, i2, r2, u2, f2, o2, e2, l2, c2, a2) {
    var s2, p2, w2, g2, k, b2, m, M2, $2, S2 = r2.props || h, C2 = i2.props, j2 = i2.type;
    if ("svg" == j2 ? f2 = "http://www.w3.org/2000/svg" : "math" == j2 ? f2 = "http://www.w3.org/1998/Math/MathML" : f2 || (f2 = "http://www.w3.org/1999/xhtml"), o2) {
      for (s2 = 0; s2 < o2.length; s2++) if ((k = o2[s2]) && (j2 ? k.localName == j2 : 3 == k.nodeType)) {
        t2 = k, o2[s2] = null;
        break;
      }
    }
    if (!t2) {
      if (M2 = a2.ownerDocument || document, !j2) return M2.createTextNode(C2);
      t2 = M2.createElementNS(f2, j2, C2.is && C2), l2 && (n.__m && n.__m(i2, o2), l2 = false), o2 = null;
    }
    if (j2) {
      if (a2 = "template" == j2 ? t2.content : t2, o2 = "textarea" == j2 && null != C2.defaultValue ? null : o2 && d.call(a2.childNodes), !l2 && o2) for (S2 = {}, s2 = 0; s2 < t2.attributes.length; s2++) S2[(k = t2.attributes[s2]).name] = k.value;
      for (s2 in S2) k = S2[s2], "dangerouslySetInnerHTML" == s2 ? w2 = k : "children" == s2 || s2 in C2 || "value" == s2 && "defaultValue" in C2 || "checked" == s2 && "defaultChecked" in C2 || T(t2, s2, null, k, f2);
      for (s2 in $2 = 1 & r2.__u, C2) k = C2[s2], "children" == s2 ? g2 = k : "dangerouslySetInnerHTML" == s2 ? p2 = k : "value" == s2 ? b2 = k : "checked" == s2 ? m = k : l2 && "function" != typeof k || !(S2[s2] !== k || $2 && null != k) || T(t2, s2, k, S2[s2], f2);
      p2 ? (l2 || w2 && (p2.__html == w2.__html || p2.__html == t2.innerHTML) || (t2.innerHTML = p2.__html), i2.__k = []) : (w2 && (t2.textContent = ""), ("foreignObject" == j2 || "http://www.w3.org/1998/Math/MathML" == f2 && v.test(j2)) && (f2 = "http://www.w3.org/1999/xhtml"), L(a2, y(g2) ? g2 : [g2], i2, r2, u2, f2, o2, e2, o2 ? o2[0] : r2.__k && x(r2, 0), l2, c2), o2 && o2.some(_)), l2 && "textarea" != j2 || (s2 = "value", "progress" == j2 && null == b2 ? t2.removeAttribute(s2) : null == b2 || b2 === t2[s2] && ("progress" != j2 || b2) || T(t2, s2, b2, S2[s2], f2), s2 = "checked", null != m && m != t2[s2] && T(t2, s2, m, S2[s2], f2));
    } else S2 === C2 || l2 && t2.data == C2 || (t2.data = C2);
    return t2;
  }
  function D(t2, i2, r2, u2) {
    try {
      "function" == typeof t2 ? i2 ? i2.__x = t2(i2) || 1 : u2 && (u2 = u2.__c || u2.__e) && (i2 = u2.__x) && (u2.__x = null, "function" == typeof i2 ? i2() : t2(null)) : t2.current = i2;
    } catch (t3) {
      n.__e(t3, r2);
    }
  }
  function E(t2, i2, r2) {
    var u2, f2;
    if (n.unmount && n.unmount(t2), !(u2 = t2.ref) || u2.current && u2.current != t2.__e || D(u2, null, i2, t2), u2 = t2.__c) {
      if (u2.componentWillUnmount) try {
        u2.componentWillUnmount();
      } catch (t3) {
        n.__e(t3, i2);
      }
      u2.__P = u2.__n = null;
    }
    if (u2 = t2.__k) for (f2 = 0; f2 < u2.length; f2++) u2[f2] && E(u2[f2], i2, "function" != typeof t2.type || r2 && !t2.props.__P);
    (u2 = t2.__e) && (r2 || _(u2), u2.__e && (u2.__e = null)), t2.__e = t2.__c = t2.__ = null;
  }
  function F(n2, t2, i2) {
    return this.constructor(n2, i2);
  }
  function G(t2, i2) {
    var r2, u2, f2, o2;
    n.__ && n.__(t2, i2), 9 == i2.nodeType && (i2 = i2.documentElement), u2 = (r2 = t2 && 32 & t2.__u) ? null : i2.__k, i2.__k = b(M, { children: [t2] }), f2 = [], o2 = [], N(i2, i2.__k, u2 || h, h, i2.namespaceURI, u2 ? null : i2.firstChild ? d.call(i2.childNodes) : null, f2, u2 ? u2.__e : i2.firstChild, r2, o2), z(f2, i2.__k, o2), i2.__k.props.children = null;
  }
  n = { __e: function(n2, t2, i2, r2) {
    for (var u2, o2, e2; t2 = t2.__; ) if ((u2 = t2.__c) && !(1 & u2.__g)) {
      u2.__g |= 4;
      try {
        if ((o2 = u2.constructor) && o2.getDerivedStateFromError && (u2.setState(o2.getDerivedStateFromError(n2)), e2 = 8 & u2.__g), u2.componentDidCatch && (u2.componentDidCatch(n2, r2 || {}), e2 = 8 & u2.__g), e2) return void (u2.__g |= 2);
      } catch (t3) {
        n2 = t3, e2 = 0;
      }
    }
    throw f = 0, n2;
  } }, t = 0, i = function(n2) {
    return null != n2 && void 0 === n2.constructor;
  }, $.prototype.setState = function(n2, t2) {
    var i2 = this.__s;
    i2 && i2 != this.state || (i2 = this.__s = w({}, this.state)), "function" == typeof n2 && (n2 = n2(w({}, i2), this.props)), n2 && (w(i2, n2), this.__v && (t2 && this.__k.push(t2), C(this)));
  }, $.prototype.forceUpdate = function(n2) {
    this.__v && (this.__g |= 4, n2 && this.__h.push(n2), C(this));
  }, $.prototype.render = M, r = [], f = 0, o = function(n2, t2) {
    return n2.__v.__b - t2.__v.__b;
  }, e = /* @__PURE__ */ Symbol(), l = /(PointerCapture)$|Capture$/i, c = 0, a = {}, s = 0;

  // Web/apps/ep-fixture/src/stock-dom.js
  var log = (message) => globalThis.EPLOG.push(message);
  globalThis.EPLOG = [];
  var result = globalThis.EPRUN = {};
  try {
    G(g("div", { id: "a" }, g("span", { id: "b" })), document.getElementById("mount"));
    result.mount = document.getElementById("mount").innerRML;
    G(g("div", { id: "f" }), document.getElementById("prefilled"));
    result.adopt = document.getElementById("prefilled").innerRML;
    const row = (key) => g("div", { key, id: "row" + key });
    const order = () => document.getElementById("list").children.map((child) => child.id).join(",");
    const mount = document.getElementById("mount2");
    G(g("div", { id: "list" }, [1, 2, 3, 4].map(row)), mount);
    result.mounted = order();
    G(g("div", { id: "list" }, [4, 3, 2, 1].map(row)), mount);
    result.reversed = order();
  } catch (error) {
    log("FIXTURE THREW: " + error);
    result.error = String(error);
  }
})();
