import type { Client } from "../../sdk/client";
import type { WizardAction, WizardState } from "../state";

export interface WizardProps { client: Client; state: WizardState; dispatch: (a: WizardAction) => void; onFinish?: () => void; }
