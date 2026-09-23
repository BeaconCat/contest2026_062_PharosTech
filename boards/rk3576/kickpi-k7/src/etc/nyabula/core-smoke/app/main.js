import * as core from "@nyabula/core";
import * as ui from "@nyabula/ui";
import * as ai from "@nyabula/ai";

export async function ny_on_start() {
  const info = core.info();
  core.log("BOARD_CORE_START", info.id, info.version, info.apiVersion);
  await ui.notify("BOARD_CORE_UI");
  core.log("BOARD_CORE_AI", await ai.invoke("BOARD_CORE_PROMPT"));
}

export function ny_on_event(event) {
  core.log("BOARD_CORE_EVENT", event);
}

export function ny_on_stop() {
  core.log("BOARD_CORE_STOP");
}
