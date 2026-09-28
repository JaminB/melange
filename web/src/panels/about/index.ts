import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "about", title: "About", order: 1000, needs: [], load: () => import("./About") });
