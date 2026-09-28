import { JsonTree } from "../../sdk/ui";
import { stateDiff } from "./mcap";
import type { LoadedCapture } from "./loadCapture";

export function StateDiffView({ capture }: { capture: LoadedCapture }) {
  if (!capture.begin || !capture.end) return <p class="muted">No GL state was captured.</p>;
  const rows = stateDiff(capture.begin, capture.end);
  if (!rows.length) return <p class="muted">No GL state value changed between the first and last frame.</p>;
  return (
    <table class="cap-diff">
      <thead><tr><th>State</th><th>Begin</th><th>End</th></tr></thead>
      <tbody>
        {rows.map((r) => (
          <tr key={r.key}>
            <td><code>{r.key}</code></td>
            <td><JsonTree value={r.before} /></td>
            <td><JsonTree value={r.after} /></td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
