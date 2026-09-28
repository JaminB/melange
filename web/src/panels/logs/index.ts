import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "logs", title: "Logs", order: 10, needs: [], load: () => import("./Logs") });
