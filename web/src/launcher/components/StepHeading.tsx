import type { ComponentChildren } from "preact";
import { useEffect, useRef } from "preact/hooks";

// Focus moves to the step's <h1> whenever the step changes (spec §5.2 accessibility).
export function StepHeading({ children }: { children: ComponentChildren }) {
  const ref = useRef<HTMLHeadingElement>(null);
  useEffect(() => { ref.current?.focus(); }, []);
  return <h1 ref={ref} tabIndex={-1}>{children}</h1>;
}
