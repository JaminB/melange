// entity.inspect: known fields, a read-only hex view, typed values at a chosen offset, and pointer following.
import { useEffect, useMemo, useState } from "preact/hooks";
import type { Client } from "../../sdk/client";
import { RpcError } from "../../sdk/client";
import { JsonTree } from "../../sdk/ui";
import { hex32, hexRows, parseAddress, readFloat, readLE, type Inspection } from "./model";

export type Target = { handle: number } | { addr: number };

const LENGTHS = [64, 256, 1024, 4096];

export function Inspector({ client, target, onTarget }: { client: Client; target?: Target; onTarget: (t: Target) => void }) {
  const [len, setLen] = useState(256);
  const [res, setRes] = useState<Inspection>();
  const [error, setError] = useState<string>();
  const [auto, setAuto] = useState(false);
  const [offset, setOffset] = useState(0);
  const [addrText, setAddrText] = useState("");
  const [history, setHistory] = useState<Target[]>([]);
  const [tick, setTick] = useState(0);

  useEffect(() => {
    if (!target) return;
    let dead = false;
    client.call<Inspection>("entity.inspect", { ...target, len }).then(
      (r) => { if (!dead) { setRes(r); setError(undefined); } },
      (e) => { if (!dead) { setError(e instanceof RpcError ? e.message : String(e)); if (!auto) setRes(undefined); } },
    );
    return () => { dead = true; };
  }, [target && ("handle" in target ? `h${target.handle}` : `a${target.addr}`), len, tick]);
  useEffect(() => {
    if (!auto || !target) return;
    const t = setInterval(() => setTick((n) => n + 1), 1000);
    return () => clearInterval(t);
  }, [auto, target]);
  useEffect(() => setOffset(0), [target]);

  const go = (t: Target) => {
    if (target) setHistory([...history.slice(-31), target]);
    onTarget(t);
  };
  const back = () => {
    const prev = history[history.length - 1];
    if (!prev) return;
    setHistory(history.slice(0, -1));
    onTarget(prev);
  };
  const submitAddr = (e: Event) => {
    e.preventDefault();
    const a = parseAddress(addrText);
    if (a === undefined) setError("Enter an address such as 0x0095b4a8.");
    else go({ addr: a });
  };

  const rows = useMemo(() => (res?.hex ? hexRows(res.hex, res.addr) : []), [res]);
  const hex = res?.hex ?? "";
  const u32 = readLE(hex, offset, 4);
  const values = res?.hex ? [
    ["u8", readLE(hex, offset, 1)], ["u16", readLE(hex, offset, 2)], ["u32", u32],
    ["i32", u32 === undefined ? undefined : u32 | 0], ["f32", readFloat(hex, offset)],
  ] as const : [];

  return (
    <aside class="gs-insp" aria-label="Inspector">
      <form class="gs-insp-bar" onSubmit={submitAddr}>
        <button type="button" class="btn" onClick={back} disabled={!history.length} title="Back">←</button>
        <input class="fb-text" value={addrText} placeholder="Address (0x…)" aria-label="Address"
               onInput={(e) => setAddrText((e.currentTarget as HTMLInputElement).value)} />
        <button class="btn" type="submit" data-action="inspect-addr">Go</button>
        <select class="gs-select" value={String(len)} aria-label="Bytes" onChange={(e) => setLen(Number((e.currentTarget as HTMLSelectElement).value))}>
          {LENGTHS.map((n) => <option key={n} value={n}>{n} bytes</option>)}
        </select>
        <label class="gs-check"><input type="checkbox" checked={auto} onChange={(e) => setAuto((e.currentTarget as HTMLInputElement).checked)} /> live</label>
      </form>
      {error ? <p class="error gs-pad" role="alert">{error}</p> : null}
      {!res && !error ? <p class="muted gs-pad">Pick a worm, an entity or a container variable, or enter an address.</p> : null}
      {res ? (
        <div class="gs-insp-body" data-gs="inspection">
          <h3 class="gs-insp-title">{res.type || "(no type information)"}{res.kind ? <span class={`gs-kind k-${res.kind}`}>{res.kind}</span> : null}</h3>
          <dl class="gs-facts">
            {res.handle !== undefined ? <><dt>Handle</dt><dd><code>{hex32(res.handle)}</code></dd></> : null}
            <dt>Address</dt><dd><code>{hex32(res.addr)}</code></dd>
            <dt>Vtable</dt><dd><code>{hex32(res.vtable)}</code></dd>
          </dl>
          {Object.keys(res.fields ?? {}).length ? <JsonTree value={res.fields} open={2} name="fields" /> : null}
          {res.hex === null ? <p class="muted">The raw memory view is off ([Oasis] RawInspect=0).</p> : (
            <>
              <div class="gs-values" data-gs="values">
                <span class="muted">+0x{offset.toString(16)}</span>
                {values.map(([k, v]) => <span key={k}><span class="muted">{k}</span> {v === undefined ? "??" : k === "f32" ? Number(v).toPrecision(7) : String(v)}</span>)}
                {u32 !== undefined && u32 >= 0x10000 ? <button class="btn small" data-action="follow" onClick={() => go({ addr: u32 })}>Follow {hex32(u32)}</button> : null}
              </div>
              <div class="gs-hex" role="table" aria-label="Memory">
                {rows.map((r) => (
                  <div key={r.offset} class="gs-hex-row" role="row">
                    <span class="gs-hex-addr">{hex32(r.addr)}</span>
                    <span class="gs-hex-bytes">
                      {r.cells.map((b, i) => (
                        <span key={i} class={`gs-byte${b === null ? " na" : ""}${r.offset + i === offset ? " sel" : ""}`}
                              onClick={() => setOffset(r.offset + i)}>{b === null ? "??" : b.toString(16).padStart(2, "0")}</span>
                      ))}
                    </span>
                    <span class="gs-hex-ascii">{r.ascii}</span>
                  </div>
                ))}
              </div>
            </>
          )}
        </div>
      ) : null}
    </aside>
  );
}
