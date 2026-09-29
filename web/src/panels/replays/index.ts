import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "replays", title: "Replays", order: 45, needs: [], load: () => import("./Replays") });
