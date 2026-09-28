import "./entities.css";
import { registerPanel } from "../../sdk/panels";

registerPanel({ id: "entities", title: "Game state", order: 30, needs: ["game"], load: () => import("./GameState") });
