import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "events", title: "Events", order: 20, needs: ["game"], load: () => import("./Events") });
