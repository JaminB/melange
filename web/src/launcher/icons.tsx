// Original inline icons, 20px, 1.75 stroke, currentColor, round caps — the same visual weight as the brand mark.
import type { ComponentChildren } from "preact";

interface IconProps { size?: number; class?: string; }

function Svg({ size = 20, class: cls, children }: IconProps & { children: ComponentChildren }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width={1.75}
         stroke-linecap="round" stroke-linejoin="round" class={cls} aria-hidden="true">
      {children}
    </svg>
  );
}

export const FolderIcon = (p: IconProps) => <Svg {...p}><path d="M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6Z" /></Svg>;
export const SearchIcon = (p: IconProps) => <Svg {...p}><circle cx="10.5" cy="10.5" r="6.5" /><path d="m20 20-4.3-4.3" /></Svg>;
export const ShieldCheckIcon = (p: IconProps) => <Svg {...p}><path d="M12 3 5 6v6c0 4.2 2.8 7.4 7 9 4.2-1.6 7-4.8 7-9V6l-7-3Z" /><path d="m9 12 2 2 4-4" /></Svg>;
export const ShieldAlertIcon = (p: IconProps) => <Svg {...p}><path d="M12 3 5 6v6c0 4.2 2.8 7.4 7 9 4.2-1.6 7-4.8 7-9V6l-7-3Z" /><path d="M12 8v4" /><path d="M12 15.5h.01" /></Svg>;
export const PlugIcon = (p: IconProps) => <Svg {...p}><path d="M9 3v5M15 3v5M7 8h10v3a5 5 0 0 1-5 5 5 5 0 0 1-5-5V8Z" /><path d="M12 16v5" /></Svg>;
export const SparkleIcon = (p: IconProps) => <Svg {...p}><path d="M12 3c.6 3.2 2.2 4.8 5.4 5.4-3.2.6-4.8 2.2-5.4 5.4-.6-3.2-2.2-4.8-5.4-5.4C9.8 7.8 11.4 6.2 12 3Z" /><path d="M18.5 15c.3 1.6 1.1 2.4 2.7 2.7-1.6.3-2.4 1.1-2.7 2.7-.3-1.6-1.1-2.4-2.7-2.7 1.6-.3 2.4-1.1 2.7-2.7Z" /></Svg>;
export const PuzzleIcon = (p: IconProps) => <Svg {...p}><path d="M9 4h4v2.2a1.8 1.8 0 1 0 0 3.6V12h3a1.8 1.8 0 1 1 0 3.6V18h-3v2.2a1.8 1.8 0 1 1-3.6 0V18H6v-3.2a1.8 1.8 0 1 0 0-3.6V8h3V4Z" /></Svg>;
export const StoreIcon = (p: IconProps) => <Svg {...p}><path d="M5 9 6.5 4h11L19 9" /><path d="M4 9h16v10a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V9Z" /><path d="M9 13a2 2 0 0 0 4 0" /></Svg>;
export const GearIcon = (p: IconProps) => <Svg {...p}><circle cx="12" cy="12" r="3" /><path d="M12 3.5v2M12 18.5v2M20.5 12h-2M5.5 12h-2M17.5 6.5l-1.4 1.4M7.9 16.1l-1.4 1.4M17.5 17.5l-1.4-1.4M7.9 7.9 6.5 6.5" /></Svg>;
export const LifeBuoyIcon = (p: IconProps) => <Svg {...p}><circle cx="12" cy="12" r="9" /><circle cx="12" cy="12" r="3.5" /><path d="m7.5 7.5 2.3 2.3M16.5 7.5l-2.3 2.3M7.5 16.5l2.3-2.3M16.5 16.5l-2.3-2.3" /></Svg>;
export const PlayIcon = (p: IconProps) => <Svg {...p}><path d="M7 4.5v15l13-7.5-13-7.5Z" /></Svg>;
export const CheckCircleIcon = (p: IconProps) => <Svg {...p}><circle cx="12" cy="12" r="9" /><path d="m8.5 12.5 2.3 2.3 4.7-5" /></Svg>;
// A plain, empty circle: not yet decided, as opposed to a (grey) checkmark for a step that will simply happen.
export const CircleDashedIcon = (p: IconProps) => <Svg {...p}><circle cx="12" cy="12" r="9" stroke-dasharray="3.2 3.6" /></Svg>;
export const AlertTriangleIcon = (p: IconProps) => <Svg {...p}><path d="M12 4 3 20h18L12 4Z" /><path d="M12 10.5v4" /><path d="M12 17.2h.01" /></Svg>;
export const XCircleIcon = (p: IconProps) => <Svg {...p}><circle cx="12" cy="12" r="9" /><path d="m9 9 6 6M15 9l-6 6" /></Svg>;
export const UndoIcon = (p: IconProps) => <Svg {...p}><path d="M7 7 3.5 10.5 7 14" /><path d="M3.5 10.5H14a6 6 0 1 1 0 12H9" /></Svg>;
export const ExternalLinkIcon = (p: IconProps) => <Svg {...p}><path d="M9 5H6a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-3" /><path d="M14 4h6v6" /><path d="M20 4 10.5 13.5" /></Svg>;
