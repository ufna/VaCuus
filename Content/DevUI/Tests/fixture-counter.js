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
  function g(n2, t3, i3) {
    var r3, u3, f3, o3 = {}, e3 = arguments.length;
    for (f3 in t3) "key" == f3 ? r3 = t3[f3] : "ref" == f3 && "function" != typeof n2 ? u3 = t3[f3] : o3[f3] = t3[f3];
    return e3 > 2 && (o3.children = e3 > 3 ? d.call(arguments, 2) : i3), b(n2, o3, r3, u3, null);
  }
  function b(i3, r3, u3, f3, o3) {
    var e3 = { type: i3, props: r3, key: u3, ref: f3, __k: null, __: null, __b: 0, __e: null, __c: null, constructor: void 0, __v: o3 || ++t, __i: -1, __u: 0 };
    return !o3 && n.vnode && n.vnode(e3), e3;
  }
  function M(n2) {
    return n2.children;
  }
  function $(n2, t3) {
    this.props = n2, this.context = t3, this.__g = 0;
  }
  function x(n2, t3) {
    if (null == t3) return n2.__ ? x(n2.__, n2.__i + 1) : null;
    for (var i3; t3 < n2.__k.length; t3++) if ((i3 = n2.__k[t3]) && i3.__e) return i3.__e;
    return "function" != typeof n2.type || n2.props.__P ? null : x(n2);
  }
  function S(n2) {
    if ((n2 = n2.__) && n2.__c && !n2.props.__P) return n2.__e = null, n2.__k.some(function(t3) {
      return t3 && (n2.__e = t3.__e);
    }), S(n2);
  }
  function C(t3) {
    (8 & t3.__g || !(t3.__g |= 8) || !r.push(t3) || f++) && u == n.debounceRendering || ((u = n.debounceRendering) || queueMicrotask)(j);
  }
  function j() {
    var t3, i3, u3, e3, l3, c3, a3, s3, h3;
    try {
      for (i3 = 1; r.length; ) r.length > i3 && r.sort(o), t3 = r.shift(), i3 = r.length, 8 & t3.__g && (e3 = void 0, l3 = void 0, c3 = (l3 = (u3 = t3).__v).__e, a3 = [], s3 = [], (h3 = u3.__P) && ((e3 = w({ constructor: void 0 }, l3)).__v = l3.__v + 1, n.vnode && n.vnode(e3), N(h3, e3, l3, u3.__n, h3.namespaceURI, 32 & l3.__u ? [c3] : null, a3, c3 || x(l3), 32 & l3.__u, s3), e3.__v = l3.__v, e3.__.__k[e3.__i] = e3, z(a3, e3, s3), l3.__ = l3.__e = null, e3.__e != c3 && S(e3)));
    } finally {
      r.length = f = 0;
    }
  }
  function L(n2, t3, i3, r3, u3, f3, o3, e3, l3, c3, a3) {
    var s3, v3, y3, d3, w2, _3, g3 = r3.__k || p, k2 = t3.length;
    for (l3 = H(i3, t3, g3, l3, k2), s3 = 0; s3 < k2; s3++) null != (y3 = i3.__k[s3]) && (v3 = ~y3.__i && g3[y3.__i] || h, y3.__i = s3, _3 = N(n2, y3, v3, u3, f3, o3, e3, l3, c3, a3), d3 = y3.__e, (v3.ref != y3.ref || 8 & v3.__u) && (v3.ref && D(v3.ref, null, y3, v3), y3.ref && a3.push(y3.ref, y3.__c || d3, y3)), w2 = w2 || d3, 4 & y3.__u ? (l3 = I(y3, l3, n2, !v3.__v), v3.__e && (v3.__e = null)) : "function" == typeof y3.type && void 0 !== _3 ? l3 = _3 : d3 && (l3 = d3.nextSibling), y3.__u &= -7);
    return i3.__e = w2, l3;
  }
  function H(n2, t3, i3, r3, u3) {
    var f3, o3, e3, l3, c3, a3, s3, h3, p3, v3, d3 = i3.length, w2 = d3, _3 = 0, g3 = false, k2 = n2.__k = Array(u3);
    for (f3 = 0; f3 < u3; f3++) null != (o3 = t3[f3]) && "boolean" != typeof o3 && "function" != typeof o3 ? ("object" != typeof o3 || o3.constructor == String ? o3 = k2[f3] = b(null, o3) : y(o3) ? o3 = k2[f3] = b(M, { children: o3 }) : void 0 === o3.constructor && o3.__b ? o3 = k2[f3] = b(o3.type, o3.props, o3.key, o3.ref, o3.__v) : k2[f3] = o3, l3 = f3 + _3, o3.__ = n2, o3.__b = n2.__b + 1, e3 = null, ~(c3 = o3.__i = O(o3, i3, l3, w2)) && (w2--, (e3 = i3[c3]) && (e3.__u |= 2)), e3 && e3.__v ? (o3.__u |= 2, c3 == l3 - 1 ? _3-- : c3 == l3 + 1 ? _3++ : c3 != l3 && (c3 > l3 ? _3-- : _3++, g3 = true)) : (~c3 || (u3 > d3 ? _3-- : u3 < d3 && _3++), "function" != typeof o3.type && (o3.__u |= 4))) : k2[f3] = null;
    if (g3) {
      for (a3 = [], s3 = [], f3 = 0; f3 < u3; f3++) if ((o3 = k2[f3]) && 2 & o3.__u) {
        for (h3 = 0, p3 = a3.length; h3 < p3; ) a3[v3 = h3 + p3 >> 1] < o3.__i ? h3 = v3 + 1 : p3 = v3;
        a3[h3] = o3.__i, s3[f3] = h3 + 1;
      }
      for (_3 = a3.length; f3--; ) s3[f3] && (s3[f3] == _3 ? _3-- : k2[f3].__u |= 4);
    }
    if (w2) for (f3 = 0; f3 < d3; f3++) !(e3 = i3[f3]) || 2 & e3.__u || (e3.__e == r3 && (r3 = x(e3)), E(e3, e3));
    return r3;
  }
  function I(n2, t3, i3, r3) {
    var u3, f3, o3;
    if ("function" == typeof n2.type) {
      if (n2.props.__P) return t3;
      if (u3 = n2.__k) for (f3 = 0; f3 < u3.length; f3++) u3[f3] && (u3[f3].__ = n2, t3 = I(u3[f3], t3, i3, false));
      return t3;
    }
    for (t3 && !t3.parentNode && (t3 = x(n2)) && !t3.parentNode && (t3 = null), o3 = t3; o3 && 8 == o3.nodeType; ) o3 = o3.nextSibling;
    for (n2.__e != o3 && (!r3 && i3.moveBefore && n2.__e.parentNode ? i3.moveBefore(n2.__e, t3) : i3.insertBefore(n2.__e, t3 || null)), t3 = n2.__e; (t3 = t3 && t3.nextSibling) && 8 == t3.nodeType; ) ;
    return t3;
  }
  function O(n2, t3, i3, r3) {
    var u3, f3, o3, e3 = n2.key, l3 = n2.type, c3 = t3[i3], a3 = c3 && !(2 & c3.__u);
    if (null === c3 && null == e3 || a3 && e3 == c3.key && l3 == c3.type) return i3;
    if (r3 > (a3 ? 1 : 0)) {
      for (u3 = i3 - 1, f3 = i3 + 1; u3 >= 0 || f3 < t3.length; ) if ((c3 = t3[o3 = u3 >= 0 ? u3-- : f3++]) && !(2 & c3.__u) && e3 == c3.key && l3 == c3.type) return o3;
    }
    return -1;
  }
  function P(n2, t3, i3) {
    null == i3 && (i3 = ""), "-" == t3[0] ? n2.setProperty(t3, i3) : n2[t3] = i3;
  }
  function T(n2, t3, i3, r3, u3) {
    var f3, o3;
    n: if ("style" == t3) if ("string" == typeof i3) n2.style.cssText = i3;
    else {
      if ("string" == typeof r3 && (n2.style.cssText = r3 = ""), r3) for (t3 in r3) i3 && t3 in i3 || P(n2.style, t3, "");
      if (i3) for (t3 in i3) r3 && i3[t3] == r3[t3] || P(n2.style, t3, i3[t3]);
    }
    else if ("o" == t3[0] && "n" == t3[1]) (n2.__e || (n2.__e = {}))[t3] = i3, i3 && r3 || (o3 = a[t3] || (a[t3] = q(t3)), (n2.__a || (n2.__a = {}))[t3] = c, f3 = t3 != (t3 = t3.replace(l, "$1")), (t3 = t3.slice(2))[0] < "a" && (t3 = t3.toLowerCase()), i3 ? n2.addEventListener(t3, o3, f3) : n2.removeEventListener(t3, o3, f3));
    else {
      if ("http://www.w3.org/2000/svg" == u3) t3 = t3.replace(/xlink(H|:h)/, "h").replace(/sName$/, "s");
      else if ("width" != t3 && "height" != t3 && "href" != t3 && "list" != t3 && "form" != t3 && "tabIndex" != t3 && "download" != t3 && "rowSpan" != t3 && "colSpan" != t3 && "role" != t3 && "popover" != t3 && t3 in n2) try {
        n2[t3] = null == i3 ? "" : i3;
        break n;
      } catch (n3) {
      }
      "function" == typeof i3 || (null == i3 || false === i3 && "-" != t3[4] ? n2.removeAttribute(t3) : n2.setAttribute(t3, "popover" == t3 && 1 == i3 ? "" : i3));
    }
  }
  function q(t3) {
    return function(i3) {
      if (this.__e) {
        var r3 = this.__e[t3];
        if (null == i3[e]) i3[e] = c++;
        else if (i3[e] < this.__a[t3]) return;
        return r3(n.event ? n.event(i3) : i3);
      }
    };
  }
  function N(t3, i3, r3, u3, f3, o3, e3, l3, c3, a3) {
    var s3, h3, v3, d3, g3, k2, b2, m2, S2, C3, j2, H2, I2, A, O2, P2, T2, q2, N2, z3, D3 = i3.type;
    if (void 0 !== i3.constructor) return null;
    if (128 & r3.__u && (c3 = 32 & r3.__u, s3 = r3.__c.__z)) {
      if (i3.__u |= c3, h3 = o3 = [], 8 == s3.nodeType) for (v3 = 1, d3 = s3.nextSibling; d3; d3 = d3.nextSibling) {
        if (8 == d3.nodeType) {
          if (d3.data.startsWith("$s")) v3++;
          else if (d3.data.startsWith("/$s") && !--v3) break;
        }
        o3.push(d3);
      }
      else o3.push(s3);
      l3 = o3[0];
    }
    (s3 = n.__b) && s3(i3);
    n: if ("function" == typeof D3) {
      g3 = e3.length;
      try {
        if (C3 = i3.props, j2 = (s3 = D3.prototype) && s3.render, H2 = (s3 = D3.contextType) && u3[s3.__c], I2 = s3 ? H2 ? H2.props.value : s3.__ : u3, r3.__c ? 2 & (k2 = i3.__c = r3.__c).__g && (k2.__g |= 1) : (j2 ? i3.__c = k2 = new D3(C3, I2) : (i3.__c = k2 = new $(C3, I2), k2.constructor = D3, k2.render = F), H2 && H2.sub(k2), k2.state || (k2.state = {}), k2.__n = u3, k2.__g |= 8, k2.__h = [], k2.__k = []), j2 && (k2.__s || (k2.__s = k2.state), D3.getDerivedStateFromProps && (k2.__s == k2.state && (k2.__s = w({}, k2.__s)), w(k2.__s, D3.getDerivedStateFromProps(C3, k2.__s)))), b2 = k2.props, m2 = k2.state, k2.__v = i3, r3.__c) {
          if (j2 && !D3.getDerivedStateFromProps && C3 !== b2 && k2.componentWillReceiveProps && k2.componentWillReceiveProps(C3, I2), i3.__v == r3.__v && !(8 & k2.__g) || !(4 & k2.__g) && k2.shouldComponentUpdate && false === k2.shouldComponentUpdate(C3, k2.__s, I2)) {
            i3.__v != r3.__v && (k2.props = C3, k2.state = k2.__s, k2.__g &= -9), i3.__e = r3.__e, i3.__k = r3.__k, i3.__k.some(function(n2) {
              n2 && (n2.__ = i3);
            }), p.push.apply(k2.__h, k2.__k), k2.__k = [], k2.__h.length && e3.push(k2), l3 = x(r3);
            break n;
          }
          k2.componentWillUpdate && k2.componentWillUpdate(C3, k2.__s, I2), j2 && k2.componentDidUpdate && k2.__h.push(function() {
            k2.componentDidUpdate(b2, m2, S2);
          });
        } else j2 && !D3.getDerivedStateFromProps && k2.componentWillMount && k2.componentWillMount(), j2 && k2.componentDidMount && k2.__h.push(k2.componentDidMount);
        if (k2.context = I2, k2.props = C3, k2.__P = t3, k2.__g &= -5, A = n.__r, O2 = 0, j2) k2.state = k2.__s, k2.__g &= -9, A && A(i3), s3 = k2.render(k2.props, k2.state, k2.context), p.push.apply(k2.__h, k2.__k), k2.__k = [];
        else do {
          k2.__g &= -9, A && A(i3), s3 = k2.render(k2.props, k2.state, k2.context), k2.state = k2.__s;
        } while (8 & k2.__g && ++O2 < 25);
        k2.state = k2.__s, k2.getChildContext && (u3 = w({}, u3, k2.getChildContext())), j2 && r3.__c && k2.getSnapshotBeforeUpdate && (S2 = k2.getSnapshotBeforeUpdate(b2, m2)), P2 = s3 && s3.type === M && null == s3.key ? s3.props.children : s3, C3.__P && (s3 = l3, f3 = (t3 = C3.__P).namespaceURI, c3 = o3 = null, r3.props && r3.props.__P != t3 && (r3.__k.some(function(n2) {
          n2 && E(n2, n2);
        }), r3.__k = null), l3 = r3.__k ? x(r3, 0) : null), l3 = L(t3, y(P2) ? P2 : [P2], i3, r3, u3, f3, o3, e3, l3, c3, a3), C3.__P && (i3.__e = null, l3 = s3), i3.__u &= -161, 128 & r3.__u && (k2.__z = null), h3 && h3.some(_), k2.__h.length && e3.push(k2), 1 & k2.__g && (k2.__g &= -4);
      } catch (t4) {
        if (e3.length = g3, i3.__v = null, c3 || o3) if (t4.then) {
          if (T2 = 0, i3.__u |= c3 ? 160 : 128, o3) {
            for (~(N2 = o3.indexOf(l3 || void 0)) || (N2 = o3.length); (z3 = o3[N2 - 1]) && 8 == z3.nodeType; ) N2--;
            for (; N2 < o3.length; N2++) if (z3 = o3[N2]) {
              if (o3[N2] = null, 8 == z3.nodeType) {
                if (z3.data.startsWith("$s")) T2++ || (q2 = z3);
                else if (T2 && z3.data.startsWith("/$s") && !--T2) {
                  l3 = z3;
                  break;
                }
              } else if (!T2) break;
            }
          }
          if (!q2) {
            for (; l3 && 8 == l3.nodeType && l3.nextSibling; ) l3 = l3.nextSibling;
            q2 = l3;
          }
          i3.__c.__z || (i3.__c.__z = q2), i3.__e = l3;
        } else o3 && o3.some(_);
        else i3.__e = r3.__e;
        i3.__k || (i3.__k = r3.__k || []), t4.then || V(i3), n.__e(t4, i3, r3);
      }
    } else l3 = i3.__e = B(r3.__e, i3, r3, u3, f3, o3, e3, c3, a3, t3);
    return (s3 = n.diffed) && s3(i3), 128 & i3.__u ? void 0 : l3;
  }
  function V(n2) {
    n2 && (n2.__c && (n2.__c.__g |= 4), n2.__k && n2.__k.some(V));
  }
  function z(t3, i3, r3) {
    for (var u3 = 0; u3 < r3.length; ) D(r3[u3++], r3[u3++], r3[u3++]);
    n.__c && n.__c(i3, t3), t3.some(function(i4) {
      try {
        t3 = i4.__h, i4.__h = [], t3.some(function(n2) {
          n2.call(i4);
        });
      } catch (t4) {
        n.__e(t4, i4.__v);
      }
    });
  }
  function B(t3, i3, r3, u3, f3, o3, e3, l3, c3, a3) {
    var s3, p3, w2, g3, k2, b2, m2, M2, $2, S2 = r3.props || h, C3 = i3.props, j2 = i3.type;
    if ("svg" == j2 ? f3 = "http://www.w3.org/2000/svg" : "math" == j2 ? f3 = "http://www.w3.org/1998/Math/MathML" : f3 || (f3 = "http://www.w3.org/1999/xhtml"), o3) {
      for (s3 = 0; s3 < o3.length; s3++) if ((k2 = o3[s3]) && (j2 ? k2.localName == j2 : 3 == k2.nodeType)) {
        t3 = k2, o3[s3] = null;
        break;
      }
    }
    if (!t3) {
      if (M2 = a3.ownerDocument || document, !j2) return M2.createTextNode(C3);
      t3 = M2.createElementNS(f3, j2, C3.is && C3), l3 && (n.__m && n.__m(i3, o3), l3 = false), o3 = null;
    }
    if (j2) {
      if (a3 = "template" == j2 ? t3.content : t3, o3 = "textarea" == j2 && null != C3.defaultValue ? null : o3 && d.call(a3.childNodes), !l3 && o3) for (S2 = {}, s3 = 0; s3 < t3.attributes.length; s3++) S2[(k2 = t3.attributes[s3]).name] = k2.value;
      for (s3 in S2) k2 = S2[s3], "dangerouslySetInnerHTML" == s3 ? w2 = k2 : "children" == s3 || s3 in C3 || "value" == s3 && "defaultValue" in C3 || "checked" == s3 && "defaultChecked" in C3 || T(t3, s3, null, k2, f3);
      for (s3 in $2 = 1 & r3.__u, C3) k2 = C3[s3], "children" == s3 ? g3 = k2 : "dangerouslySetInnerHTML" == s3 ? p3 = k2 : "value" == s3 ? b2 = k2 : "checked" == s3 ? m2 = k2 : l3 && "function" != typeof k2 || !(S2[s3] !== k2 || $2 && null != k2) || T(t3, s3, k2, S2[s3], f3);
      p3 ? (l3 || w2 && (p3.__html == w2.__html || p3.__html == t3.innerHTML) || (t3.innerHTML = p3.__html), i3.__k = []) : (w2 && (t3.textContent = ""), ("foreignObject" == j2 || "http://www.w3.org/1998/Math/MathML" == f3 && v.test(j2)) && (f3 = "http://www.w3.org/1999/xhtml"), L(a3, y(g3) ? g3 : [g3], i3, r3, u3, f3, o3, e3, o3 ? o3[0] : r3.__k && x(r3, 0), l3, c3), o3 && o3.some(_)), l3 && "textarea" != j2 || (s3 = "value", "progress" == j2 && null == b2 ? t3.removeAttribute(s3) : null == b2 || b2 === t3[s3] && ("progress" != j2 || b2) || T(t3, s3, b2, S2[s3], f3), s3 = "checked", null != m2 && m2 != t3[s3] && T(t3, s3, m2, S2[s3], f3));
    } else S2 === C3 || l3 && t3.data == C3 || (t3.data = C3);
    return t3;
  }
  function D(t3, i3, r3, u3) {
    try {
      "function" == typeof t3 ? i3 ? i3.__x = t3(i3) || 1 : u3 && (u3 = u3.__c || u3.__e) && (i3 = u3.__x) && (u3.__x = null, "function" == typeof i3 ? i3() : t3(null)) : t3.current = i3;
    } catch (t4) {
      n.__e(t4, r3);
    }
  }
  function E(t3, i3, r3) {
    var u3, f3;
    if (n.unmount && n.unmount(t3), !(u3 = t3.ref) || u3.current && u3.current != t3.__e || D(u3, null, i3, t3), u3 = t3.__c) {
      if (u3.componentWillUnmount) try {
        u3.componentWillUnmount();
      } catch (t4) {
        n.__e(t4, i3);
      }
      u3.__P = u3.__n = null;
    }
    if (u3 = t3.__k) for (f3 = 0; f3 < u3.length; f3++) u3[f3] && E(u3[f3], i3, "function" != typeof t3.type || r3 && !t3.props.__P);
    (u3 = t3.__e) && (r3 || _(u3), u3.__e && (u3.__e = null)), t3.__e = t3.__c = t3.__ = null;
  }
  function F(n2, t3, i3) {
    return this.constructor(n2, i3);
  }
  function G(t3, i3) {
    var r3, u3, f3, o3;
    n.__ && n.__(t3, i3), 9 == i3.nodeType && (i3 = i3.documentElement), u3 = (r3 = t3 && 32 & t3.__u) ? null : i3.__k, i3.__k = b(M, { children: [t3] }), f3 = [], o3 = [], N(i3, i3.__k, u3 || h, h, i3.namespaceURI, u3 ? null : i3.firstChild ? d.call(i3.childNodes) : null, f3, u3 ? u3.__e : i3.firstChild, r3, o3), z(f3, i3.__k, o3), i3.__k.props.children = null;
  }
  n = { __e: function(n2, t3, i3, r3) {
    for (var u3, o3, e3; t3 = t3.__; ) if ((u3 = t3.__c) && !(1 & u3.__g)) {
      u3.__g |= 4;
      try {
        if ((o3 = u3.constructor) && o3.getDerivedStateFromError && (u3.setState(o3.getDerivedStateFromError(n2)), e3 = 8 & u3.__g), u3.componentDidCatch && (u3.componentDidCatch(n2, r3 || {}), e3 = 8 & u3.__g), e3) return void (u3.__g |= 2);
      } catch (t4) {
        n2 = t4, e3 = 0;
      }
    }
    throw f = 0, n2;
  } }, t = 0, i = function(n2) {
    return null != n2 && void 0 === n2.constructor;
  }, $.prototype.setState = function(n2, t3) {
    var i3 = this.__s;
    i3 && i3 != this.state || (i3 = this.__s = w({}, this.state)), "function" == typeof n2 && (n2 = n2(w({}, i3), this.props)), n2 && (w(i3, n2), this.__v && (t3 && this.__k.push(t3), C(this)));
  }, $.prototype.forceUpdate = function(n2) {
    this.__v && (this.__g |= 4, n2 && this.__h.push(n2), C(this));
  }, $.prototype.render = M, r = [], f = 0, o = function(n2, t3) {
    return n2.__v.__b - t3.__v.__b;
  }, e = /* @__PURE__ */ Symbol(), l = /(PointerCapture)$|Capture$/i, c = 0, a = {}, s = 0;

  // Web/node_modules/preact/hooks/dist/hooks.mjs
  var t2;
  var r2;
  var u2;
  var i2;
  var o2 = Object.is;
  var f2 = 0;
  var c2 = [];
  var e2 = [];
  var a2 = n;
  var v2 = a2.__b;
  var l2 = a2.__r;
  var m = a2.diffed;
  var s2 = a2.__c;
  var h2 = a2.unmount;
  var p2 = a2.__;
  function y2(n2, t3) {
    a2.__h && a2.__h(r2, n2, f2 || t3), f2 = 0;
    var u3 = r2.__H || (r2.__H = { __: [], __h: [] });
    return n2 >= u3.__.length && u3.__.push({}), u3.__[n2];
  }
  function d2(n2) {
    return f2 = 1, _2(G2, n2);
  }
  function _2(n2, u3, i3) {
    var f3 = y2(t2++, 2);
    if (f3.t = n2, !f3.__c && (f3.__ = [i3 ? i3(u3) : G2(void 0, u3), function(n3) {
      var t3 = f3.__N ? f3.__N[0] : f3.__[0], r3 = f3.t(t3, n3);
      o2(t3, r3) || (f3.__N = [r3, f3.__[1]], f3.__c.setState({}));
    }], f3.__c = r2, !r2.__f)) {
      r2.__f = true;
      var c3 = r2.shouldComponentUpdate;
      r2.shouldComponentUpdate = function(n3, t3, r3) {
        var u4 = this.__H;
        if (!u4) return true;
        var i4 = false, f4 = this.props != n3;
        if (u4.__.some(function(n4) {
          n4.__N && (i4 = true, o2(n4.__[0], n4.__N[0]) || (f4 = true));
        }), c3) {
          var e3 = c3.call(this, n3, t3, r3);
          return i4 ? e3 || f4 : e3;
        }
        return !i4 || f4;
      };
    }
    return f3.__;
  }
  function g2() {
    var n2;
    do {
      for (; n2 = e2.shift(); ) try {
        C2(n2);
      } catch (t4) {
        a2.__e(t4, { __: (n2 = n2.__P) && n2.__v });
      }
      for (; n2 = c2.shift(); ) {
        var t3 = n2.__H;
        if (n2.__P && t3) try {
          t3.__h.some(C2), t3.__h.some(D2), t3.__h = [];
        } catch (r3) {
          t3.__h = [], a2.__e(r3, n2.__v);
        }
      }
    } while (e2.length);
  }
  a2.__b = function(n2) {
    r2 = null, v2 && v2(n2);
  }, a2.__ = function(n2, t3) {
    n2 && t3.__k && t3.__k.__m && (n2.__m = t3.__k.__m), p2 && p2(n2, t3);
  }, a2.__r = function(n2) {
    l2 && l2(n2), t2 = 0;
    var i3 = (r2 = n2.__c).__H;
    i3 && (u2 == r2 ? r2.__h = [] : (i3.__h.some(C2), i3.__h.some(D2), t2 = 0), i3.__h = [], i3.__.some(function(n3) {
      n3.__N && (n3.__ = n3.__N), n3.u = n3.__N = void 0;
    })), u2 = r2;
  }, a2.diffed = function(n2) {
    m && m(n2);
    var t3 = n2.__c;
    t3 && t3.__H && (t3.__H.__h.length && B2(c2.push(t3)), t3.__H.__.some(function(n3) {
      n3.u && (n3.__H = n3.u);
    })), u2 = r2 = null;
  }, a2.__c = function(n2, t3) {
    t3.some(function(n3) {
      try {
        n3.__h.some(C2), n3.__h = n3.__h.filter(function(n4) {
          return !n4.__ || D2(n4);
        });
      } catch (r3) {
        t3.some(function(n4) {
          n4.__h && (n4.__h = []);
        }), t3 = [], a2.__e(r3, n3.__v);
      }
    }), s2 && s2(n2, t3);
  }, a2.unmount = function(n2) {
    h2 && h2(n2);
    var t3, r3, u3 = n2.__c;
    u3 && u3.__H && (u3.__H.__.some(function(u4) {
      try {
        if (u4.__P && u4.__c) {
          if (void 0 === r3) {
            for (r3 = n2.__; r3 && (!r3.__c || !r3.__c.__P); ) r3 = r3.__;
            r3 = r3 && r3.__c;
          }
          u4.__P = r3, B2(e2.push(u4));
        } else C2(u4);
      } catch (n3) {
        t3 = n3;
      }
    }), u3.__H = void 0, t3 && a2.__e(t3, u3.__v));
  };
  var k = "function" == typeof requestAnimationFrame;
  function z2(n2) {
    var t3, r3 = function() {
      clearTimeout(u3), k && cancelAnimationFrame(t3), setTimeout(n2);
    }, u3 = setTimeout(r3, 35);
    k && (t3 = requestAnimationFrame(r3));
  }
  function B2(n2) {
    1 != n2 && i2 == a2.requestAnimationFrame || ((i2 = a2.requestAnimationFrame) || z2)(g2);
  }
  function C2(n2) {
    var t3 = r2, u3 = n2.__c;
    "function" == typeof u3 && (n2.__c = void 0, u3()), r2 = t3;
  }
  function D2(n2) {
    var t3 = r2;
    n2.__c = n2.__(), r2 = t3;
  }
  function G2(n2, t3) {
    return "function" == typeof t3 ? t3(n2) : t3;
  }

  // Web/apps/ep-fixture/src/stock-counter.js
  globalThis.EPLOG = [];
  var log = (message) => globalThis.EPLOG.push(message);
  var documentPrototype = Object.getPrototypeOf(document);
  var createTextNode = documentPrototype.createTextNode;
  documentPrototype.createTextNode = function(text) {
    log("createTextNode(" + JSON.stringify(text) + ")");
    return createTextNode.call(this, text);
  };
  var elementPrototype = Object.getPrototypeOf(document.createElement("div"));
  var data = Object.getOwnPropertyDescriptor(elementPrototype, "data");
  Object.defineProperty(elementPrototype, "data", {
    configurable: true,
    get() {
      return data.get.call(this);
    },
    set(value) {
      log("data=" + JSON.stringify(value));
      data.set.call(this, value);
    }
  });
  function Counter() {
    const [count, setCount] = d2(0);
    globalThis.bump = () => setCount((value) => value + 1);
    return g("div", { id: "ctr" }, "count:", count);
  }
  try {
    G(g(Counter, null), document.getElementById("mount"));
  } catch (error) {
    log("FIXTURE THREW: " + error);
  }
})();
