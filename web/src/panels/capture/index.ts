import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "capture", title: "Capture", order: 400, needs: [], load: () => import("./Capture") });
