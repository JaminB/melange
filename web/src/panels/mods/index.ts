import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "mods", title: "Mods", order: 40, needs: [], load: () => import("./Mods") });

export { Mods } from "./Mods";
