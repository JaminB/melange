// Loaded by a sandboxed mod panel (no cookie, no same-origin fetch to /ws). Everything goes through postMessage
// to the shell, which enforces the mod.<id>.* / state / log allowlist and holds the real connection.
(function () {
  "use strict";
  let seq = 0;
  const pending = new Map();
  const subs = new Map();

  window.addEventListener("message", function (ev) {
    if (ev.source !== window.parent) return;
    const m = ev.data;
    if (!m || typeof m !== "object") return;
    if (m.type === "res") {
      const p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      if (m.ok) p.resolve(m.result);
      else p.reject(new Error(m.error || "call failed"));
    } else if (m.type === "ev") {
      const cb = subs.get(m.subId);
      if (cb) cb(m.data, m.seq);
    }
  });

  function send(msg) {
    window.parent.postMessage(msg, "*");
  }

  window.OasisExt = {
    call: function (method, params) {
      return new Promise(function (resolve, reject) {
        const id = ++seq;
        pending.set(id, { resolve: resolve, reject: reject });
        send({ type: "call", id: id, m: method, p: params || {} });
      });
    },
    subscribe: function (channel, filter, cb) {
      const id = ++seq;
      subs.set(id, cb);
      send({ type: "subscribe", subId: id, ch: channel, filter: filter });
      return function () {
        subs.delete(id);
        send({ type: "unsubscribe", subId: id });
      };
    },
  };
})();
