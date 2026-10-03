import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "ini", title: "Settings", order: 50, needs: [], load: () => import("./Ini") });

export { Ini } from "./Ini";
