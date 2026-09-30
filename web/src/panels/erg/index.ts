import { registerPanel } from "../../sdk/panels";
import "./erg.css";

registerPanel({ id: "erg", title: "Erg", order: 45, needs: [], load: () => import("./Erg") });
