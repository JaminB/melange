import assert from "node:assert/strict";
import { test } from "node:test";
import {
  containerAddr, filterEntities, filterVars, fmtClock, fmtVec, hex32, hexRows, kindCounts, parseAddress, physicsName,
  project, readFloat, readLE, speed, teamColour, valueText, weaponName, windDegrees, type Entity, type Var,
} from "../../src/panels/entities/model";

const ent = (handle: number, kind: Entity["kind"], type: string, label = "", pos: Entity["pos"] = null): Entity =>
  ({ handle, object: 0x1000 + handle, vtable: 0x85c194, kind, type, label, pos, vel: pos ? { x: 3, y: 4, z: 0 } : null });

test("hex dump rows, unreadable bytes and ascii", () => {
  const rows = hexRows("41424300??ff" + "00".repeat(14), 0x95b4a8, 16);
  assert.equal(rows.length, 2);
  assert.equal(rows[0].addr, 0x95b4a8);
  assert.equal(rows[1].offset, 16);
  assert.equal(rows[1].addr, 0x95b4b8);
  assert.deepEqual(rows[0].cells.slice(0, 6), [0x41, 0x42, 0x43, 0, null, 0xff]);
  assert.equal(rows[0].ascii.slice(0, 6), "ABC. .");
  assert.equal(rows[1].cells.length, 4);
  assert.deepEqual(hexRows("", 0), []);
  assert.equal(hexRows("00", 0xffffffff)[0].addr, 0xffffffff);
});

test("little-endian reads from a dump", () => {
  const hex = "78563412" + "0000803f" + "ff??";
  assert.equal(readLE(hex, 0, 4), 0x12345678);
  assert.equal(readLE(hex, 0, 2), 0x5678);
  assert.equal(readLE(hex, 1, 1), 0x56);
  assert.equal(readFloat(hex, 4), 1);
  assert.equal(readLE(hex, 8, 1), 0xff);
  assert.equal(readLE(hex, 8, 2), undefined);
  assert.equal(readLE(hex, 10, 4), undefined);
  assert.equal(readLE("ffffffff", 0, 4), 0xffffffff);
});

test("addresses parse and format", () => {
  assert.equal(parseAddress("0x95B4A8"), 0x95b4a8);
  assert.equal(parseAddress(" 95b4a8h "), 0x95b4a8);
  assert.equal(parseAddress("9811112"), 9811112);
  assert.equal(parseAddress("0x"), undefined);
  assert.equal(parseAddress("0"), undefined);
  assert.equal(parseAddress("0x1ffffffff"), undefined);
  assert.equal(parseAddress("banana"), undefined);
  assert.equal(hex32(0x95b4a8), "0x0095b4a8");
  assert.equal(hex32(-1), "0xffffffff");
});

test("names and formatting", () => {
  assert.equal(weaponName(1), "Bazooka");
  assert.equal(weaponName(35), "Ninja Rope");
  assert.equal(weaponName(52), "Mystery 3");
  assert.equal(weaponName(-1), "—");
  assert.equal(weaponName(33), "#33");
  assert.equal(physicsName(8), "DrownFloat");
  assert.equal(physicsName(42), "state 42");
  assert.equal(fmtClock(45000), "0:45");
  assert.equal(fmtClock(61001), "1:02");
  assert.equal(fmtClock(-5), "0:00");
  assert.equal(fmtVec({ x: 1, y: null, z: 2.25 }), "1.0, —, 2.3");
  assert.equal(fmtVec(null), "—");
  assert.equal(speed({ x: 3, y: 4, z: 0 }), 5);
  assert.equal(speed({ x: null, y: 2, z: null }), 2);
  assert.equal(teamColour(0), teamColour(8));
  assert.ok(teamColour(0) !== teamColour(1));
  assert.equal(windDegrees(Math.PI / 2), -90);
  assert.equal(windDegrees(null), 0);
});

test("entities filter by kind and text, and count by kind", () => {
  const list = [ent(0x1001, "Worm", "WXWormLogicEntity", "Paul"), ent(0x2002, "Projectile", "ParabolicPayloadLogicEntity", "Bazooka"),
    ent(0x3003, "Crate", "CrateLogicEntity"), ent(0x4004, "Other", "TimerLogicEntity")];
  assert.deepEqual(kindCounts(list), { Worm: 1, Projectile: 1, Crate: 1, Barrel: 0, Other: 1 });
  const noOther = new Set<Entity["kind"]>(["Worm", "Projectile", "Crate", "Barrel"]);
  assert.equal(filterEntities(list, noOther, "").length, 3);
  assert.deepEqual(filterEntities(list, noOther, "bazoo").map((e) => e.handle), [0x2002]);
  assert.deepEqual(filterEntities(list, noOther, "crate").map((e) => e.handle), [0x3003]);
  assert.deepEqual(filterEntities(list, noOther, "00003003").map((e) => e.handle), [0x3003]);
  assert.equal(filterEntities(list, new Set(), "").length, 0);
});

test("variables: value text, containers, filter and sort", () => {
  const vars: Var[] = [
    { name: "Wind.Speed", type: "Float", value: 0.0000425 },
    { name: "Land.Theme", type: "String", value: "ARABIAN" },
    { name: "Land.MinBounds", type: "Vector", value: [1, -2, null] },
    { name: "Worm.Data00", type: "Container", value: { addr: 0x1234, class: "WormDataContainer" } },
    { name: "Team.Colour", type: "Color", value: "#ff102030" },
    { name: "Some.Table", type: "StringTable", value: null },
  ];
  assert.equal(valueText(vars[1]), "\"ARABIAN\"");
  assert.equal(valueText(vars[2]), "1, -2, —");
  assert.equal(valueText(vars[3]), "WormDataContainer @ 0x00001234");
  assert.equal(valueText(vars[4]), "#ff102030");
  assert.equal(valueText(vars[5]), "—");
  assert.equal(containerAddr(vars[3]), 0x1234);
  assert.equal(containerAddr(vars[0]), undefined);
  assert.deepEqual(filterVars(vars, "land").map((v) => v.name), ["Land.MinBounds", "Land.Theme"]);
  assert.deepEqual(filterVars(vars, "arab").map((v) => v.name), ["Land.Theme"]);
  assert.deepEqual(filterVars(vars, "", new Set(["Container"])).map((v) => v.name), ["Worm.Data00"]);
  assert.equal(filterVars(vars, "").length, 6);
  assert.equal(filterVars(vars, "")[0].name, "Land.MinBounds");
});

test("map projection keeps the aspect ratio and orientation", () => {
  const pts = [{ p: { x: 0, y: 0, z: 0 } }, { p: { x: 100, y: 50, z: 200 } }, { p: { x: 5, y: null, z: 5 } }];
  const top = project(pts, (i) => i.p, "top", 400, 400, 0);
  assert.equal(top.points.length, 2, "points with a null coordinate are skipped");
  const [a, b] = top.points;
  assert.ok(b.y > a.y, "top view: +Z runs down the page");
  assert.ok(Math.abs((b.x - a.x) / (b.y - a.y) - 0.5) < 1e-9, "same scale on both axes");
  const side = project(pts, (i) => i.p, "side", 400, 400, 0, -30);
  assert.ok(side.points[1].y < side.points[0].y, "side view: +Y runs up the page");
  assert.ok(side.waterY !== undefined && side.waterY > side.points[0].y, "water below the lowest point");
  assert.equal(side.bounds.minB, -30);
  const one = project([{ p: { x: 7, y: 7, z: 7 } }], (i) => i.p, "top", 100, 100, 10);
  assert.ok(one.points[0].x >= 0 && one.points[0].x <= 100, "a single point stays inside");
  assert.equal(project([], (i: { p: null }) => i.p, "top", 10, 10).points.length, 0);
});
