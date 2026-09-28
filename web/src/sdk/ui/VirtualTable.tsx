// A virtualised table on @tanstack/virtual-core: only the visible rows are in the DOM.
import { Virtualizer, elementScroll, observeElementOffset, observeElementRect } from "@tanstack/virtual-core";
import type { ComponentChildren } from "preact";
import { useEffect, useLayoutEffect, useReducer, useRef, useState } from "preact/hooks";

export interface Column<T> {
  key: string;
  title: string;
  width?: string;                                // CSS grid track, e.g. "8rem" or "1fr"
  render: (row: T, index: number) => ComponentChildren;
}

export interface VirtualTableProps<T> {
  rows: readonly T[];
  columns: Column<T>[];
  rowHeight?: number;
  follow?: boolean;                              // keep the last row in view while at the bottom
  rowKey?: (row: T, index: number) => string | number;
  onRowClick?: (row: T, index: number) => void;
  selected?: number;
  empty?: ComponentChildren;
}

export function VirtualTable<T>(props: VirtualTableProps<T>) {
  const { rows, columns, rowHeight = 22, follow = false } = props;
  const scroller = useRef<HTMLDivElement>(null);
  const [, rerender] = useReducer((n: number) => n + 1, 0);
  const [atBottom, setAtBottom] = useState(true);
  const [v] = useState(
    () =>
      new Virtualizer<HTMLDivElement, Element>({
        count: 0,
        getScrollElement: () => scroller.current,
        estimateSize: () => rowHeight,
        overscan: 12,
        scrollToFn: elementScroll,
        observeElementRect,
        observeElementOffset,
        onChange: () => rerender(0),
      }),
  );
  v.setOptions({ ...v.options, count: rows.length, estimateSize: () => rowHeight });

  useLayoutEffect(() => v._didMount(), []);
  useLayoutEffect(() => v._willUpdate());
  useEffect(() => {
    if (follow && atBottom && rows.length) v.scrollToIndex(rows.length - 1, { align: "end" });
  }, [rows.length, follow, atBottom]);

  const grid = columns.map((c) => c.width ?? "1fr").join(" ");
  const items = v.getVirtualItems();
  const onScroll = () => {
    const el = scroller.current;
    if (el) setAtBottom(el.scrollTop + el.clientHeight >= el.scrollHeight - rowHeight);
  };
  return (
    <div class="vt">
      <div class="vt-head" style={{ gridTemplateColumns: grid }} role="row">
        {columns.map((c) => <div key={c.key} role="columnheader">{c.title}</div>)}
      </div>
      <div class="vt-body" ref={scroller} onScroll={onScroll} role="rowgroup">
        {rows.length === 0 && props.empty ? <div class="vt-empty">{props.empty}</div> : null}
        <div style={{ height: `${v.getTotalSize()}px`, position: "relative" }}>
          {items.map((it) => {
            const row = rows[it.index];
            return (
              <div
                key={props.rowKey ? props.rowKey(row, it.index) : it.key}
                class={`vt-row${props.selected === it.index ? " sel" : ""}`}
                role="row"
                style={{ gridTemplateColumns: grid, height: `${rowHeight}px`, transform: `translateY(${it.start}px)` }}
                onClick={props.onRowClick ? () => props.onRowClick!(row, it.index) : undefined}
              >
                {columns.map((c) => <div key={c.key} role="cell">{c.render(row, it.index)}</div>)}
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
