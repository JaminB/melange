import type { LoadedCapture } from "./loadCapture";

export function Summary({ capture }: { capture: LoadedCapture }) {
  const m = capture.manifest;
  return (
    <div class="cap-summary">
      {capture.frameUrl ? <img class="cap-frame" src={capture.frameUrl} alt="the captured frame" /> : null}
      <dl class="facts">
        <dt>Scene</dt><dd>{m.scene ?? "—"}</dd>
        <dt>Frames</dt><dd>{m.frames?.join(", ") ?? "—"}</dd>
        <dt>Window</dt><dd>{m.window ? `${m.window.w}x${m.window.h}` : "—"}</dd>
        <dt>GPU</dt><dd>{m.gl ? `${m.gl.renderer} (${m.gl.vendor}), GL ${m.gl.version}` : "—"}</dd>
        <dt>Melange</dt><dd>{m.melangeVersion ?? "—"}</dd>
        <dt>Game build</dt><dd>{m.exeBuild ?? "—"}</dd>
        <dt>Calls</dt><dd>{m.counts.calls} ({m.counts.draws} draws)</dd>
        <dt>Textures</dt><dd>{m.counts.textures}</dd>
        <dt>Programs</dt><dd>{m.counts.programs} ({m.counts.programsWithAsm} with compiled text)</dd>
        <dt>Payloads</dt><dd>{m.counts.payloads}</dd>
        <dt>Readback hitch</dt><dd>{m.readbackMs !== undefined ? `${m.readbackMs.toFixed(2)} ms` : "—"}</dd>
        {m.droppedRecords ? <><dt>Dropped calls</dt><dd class="error">{m.droppedRecords}</dd></> : null}
        {m.droppedPayloads ? <><dt>Dropped payloads</dt><dd class="error">{m.droppedPayloads}</dd></> : null}
      </dl>
      {m.notes?.length ? (
        <>
          <h2>Notes</h2>
          <ul class="chips">{m.notes.map((n, i) => <li key={i}>{n}</li>)}</ul>
        </>
      ) : null}
    </div>
  );
}
