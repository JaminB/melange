import type { Issue } from "../model/checks";

export function Checks({ issues, onPick }: { issues: Issue[]; onPick(id: number): void }) {
  if (!issues.length) return <p class="muted pad-x" data-erg-checks="0">No problems found.</p>;
  return (
    <ul class="erg-issues" data-erg-checks={issues.length}>
      {issues.map((i, n) => (
        <li key={n} class={i.level}>
          {i.detail !== undefined ? <button class="link" onClick={() => onPick(i.detail!)}>{i.text}</button> : i.text}
        </li>
      ))}
    </ul>
  );
}
