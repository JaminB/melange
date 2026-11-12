// The detail layer's renders (render_view with scenery: true: the bank's meshes drawn flat in their palette's colour, the
// ARABIAN props as the editor draws them; the game's own look is the in-game set). Poses are map units.
import fs from "node:fs";
import path from "node:path";

const LOOK = { antialias: true, tints: true, waterLook: true, scenery: true };

export async function renderViews(ctx, dir, hoodoos = []) {
  fs.mkdirSync(dir, { recursive: true });
  const { call, say } = ctx;
  const shot = async (name, args) => {
    const r = await call("render_view", { ...LOOK, ...args }, { soft: true });
    if (r.images[0]) fs.writeFileSync(path.join(dir, `${name}.png`), Buffer.from(r.images[0].data, "base64"));
    else say("render", name, "no image", r.text.slice(0, 200));
  };
  await call("set_level_camera", { view: "sw", elevation: 20, target: [0, 18, 0], fit: "island", margin: 1 }, { soft: true });
  await shot("overview-level", { view: "level" });
  await shot("overview-se", { view: "se" });
  await shot("overview-nw", { view: "nw" });
  await shot("overview-low-ne", { pos: [170, 62, -150], target: [0, 8, 0], fov: 55 });
  await shot("palace-se", { pos: [90, 70, 90], target: [0, 26, 0], fov: 50 });
  await shot("palace-nw", { pos: [-80, 45, -95], target: [0, 28, 0], fov: 50 });
  await shot("palace-courts", { pos: [34, 70, 46], target: [0, 24, 0], fov: 55 });
  await shot("palace-banners-N", { pos: [13, 33, -58], target: [13, 26, -26], fov: 55 });
  await shot("palace-banners-E", { pos: [60, 30, 13], target: [26, 24, 13], fov: 60 });
  await shot("palace-tower-NE", { pos: [58, 44, -58], target: [24.75, 38, -24.75], fov: 45 });
  await shot("palace-crown", { pos: [22, 52, 34], target: [0, 46, 0], fov: 50 });
  await shot("palace-obelisks", { pos: [62, 24, 38], target: [36, 16, 36], fov: 55 });
  for (const [name, x, z] of [["W", -86, 0], ["E", 86, 0], ["N", 0, -86], ["S", 0, 86]]) {
    const k = Math.hypot(x, z), ux = x / k, uz = z / k;
    await shot(`area-${name}`, { pos: [x + ux * 14, 12, z + uz * 14], target: [0, 26, 0], fov: 55 });
  }
  await shot("refuge-NW", { pos: [-95, 30, -20], target: [-60, 12, -60], fov: 50 });
  await shot("refuge-NW-top", { pos: [-40, 40, -40], target: [-60, 22, -60], fov: 50 });
  await shot("refuge-SE", { pos: [95, 26, 30], target: [60, 12, 60], fov: 50 });
  await shot("low-mesa-W", { pos: [-96, 14, -14], target: [-74, 7, -30], fov: 50 });
  await shot("low-mesa-NE", { pos: [74, 13, -36], target: [56, 8, -58], fov: 50 });
  await shot("low-mesa-SW", { pos: [-74, 13, 36], target: [-56, 8, 58], fov: 50 });
  for (const [i, [gx, gz]] of hoodoos.slice(0, 4).entries()) {
    const k = Math.hypot(gx, gz) || 1;
    await shot(`hoodoo-${i}`, { pos: [gx - (gz / k) * 14 + (gx / k) * 6, 9, gz + (gx / k) * 14 + (gz / k) * 6], target: [gx, 7, gz], fov: 55 });
  }
  await shot("dunes-field-W", { pos: [-104, 12, 48], target: [-60, 5, 0], fov: 55 });
  await shot("dunes-field-S", { pos: [104, 10, 86], target: [0, 5, 74], fov: 55 });
}
