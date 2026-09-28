import type { LoadedCapture } from "./loadCapture";

export function Textures({ capture }: { capture: LoadedCapture }) {
  if (!capture.textures.length) return <p class="muted">No textures in this capture.</p>;
  return (
    <div class="cap-tex-grid">
      {capture.textures.map((t) => (
        <figure class="cap-tex" key={t.gl}>
          {t.url ? <img src={t.url} alt={t.name || `texture ${t.gl}`} loading="lazy" /> : <div class="cap-tex-missing">{t.skipped || "not read back"}</div>}
          <figcaption>
            <code>{t.gl}</code> {t.w}x{t.h}{t.depth ? " (depth)" : ""}
            {t.name ? <div class="muted small">{t.name}</div> : null}
          </figcaption>
        </figure>
      ))}
    </div>
  );
}
