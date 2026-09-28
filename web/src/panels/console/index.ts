import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "console", title: "Console", order: 30, needs: ["game"], load: () => import("./Console") });
