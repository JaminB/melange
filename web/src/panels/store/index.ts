import "./store.css";
import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "store", title: "Store", order: 25, needs: ["game"], load: () => import("./Store") });

export { Store } from "./Store";
