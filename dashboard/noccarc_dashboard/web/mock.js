"use strict";
/* LAYOUT REVIEW ONLY. Sample models for index.html?mock=normal|fault|lost, in the shape
 * model.py will send. The server never serves this file. Values are from MCU1's log in a
 * Renode run (S1); the fault case is shaped like S3 (MCU3's cable pulled at 5 s) with MCU4
 * also late, so every kind of mark shows at once. */

(function () {
  const C = (t, l, title) => ({ t, l, title });
  const M = (t, l) => ({ t, l, m: true }); /* small mark */

  const VALUES = [
    ["0.000", "f32"], ["201.731", "f32"], ["-610", "i32"], ["700", "u32"], ["true", "bool"],
    ["0.000", "f32"], ["153.462", "f32"], ["-240", "i32"], ["5400", "u32"], ["true", "bool"],
    ["0.000", "f32"], ["105.193", "f32"], ["-850", "i32"], ["8100", "u32"], ["true", "bool"],
    ["0.000", "f32"], ["56.924", "f32"], ["480", "i32"], ["400", "u32"], ["true", "bool"],
  ];
  const OWN_AGES = ["4 ms", "34 ms", "84 ms", "384 ms", "884 ms"];

  function params(staleOwner) {
    return VALUES.map(([v, type], i) => {
      const id = i + 1;
      const owner = Math.floor(i / 5) + 1;
      const age = owner === 1 ? OWN_AGES[i % 5] : `${3 + (i % 5)} ms`;
      if (owner === staleOwner) {
        return [`P${id}`, `Param ${id}`, v, type, M(`MCU ${owner} offline`, "fault"),
                C("STALE", "caution"), "VALID", "1205 ms"];
      }
      return [`P${id}`, `Param ${id}`, v, type, `MCU ${owner}`, "FRESH", "VALID", age];
    });
  }

  function paramSegs(staleOwner) {
    const groups = [];
    for (let n = 1; n <= 4; n++) {
      const g = [];
      for (let k = 1; k <= 5; k++) {
        const id = (n - 1) * 5 + k;
        g.push(n === staleOwner ? C("", "caution", `P${id} STALE`) : C("", "", `P${id} synchronized`));
      }
      groups.push(g);
    }
    return groups;
  }

  const CONFIG = [
    ["Arbitration bit rate", "500 kbit/s"],
    ["Data bit rate", "2 Mbit/s"],
    ["Bit rate switch (BRS)", "on"],
    ["Parameter / heartbeat period", "10 ms / 100 ms"],
    ["Firmware, protocol", "1.0-dev, v1"],
  ];

  function uart(frames) {
    return [
      ["Source", "localhost:3456 (Renode)"],
      ["Frames received", String(frames)],
      ["Lost on the way", "0"],
      ["Replaced by MCU1 (latest wins)", "0"],
      ["Frame errors", "0"],
      ["MCU1 restarts seen", "0"],
    ];
  }

  const BOOT_EVENTS = [
    ["0.106", "02:41:00.731", "MCU1 OPERATIONAL"],
    ["0.100", "02:41:00.712", "MCU2 ONLINE"],
    ["0.100", "02:41:00.712", "MCU3 ONLINE"],
    ["0.100", "02:41:00.712", M("MCU1 RESYNCING", "info")],
    ["0.100", "02:41:00.712", "MCU4 ONLINE"],
    ["0.015", "02:41:00.688", "P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18 P19 P20 became FRESH"],
    ["0.010", "02:41:00.671", "P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18 P19 P20 became VALID"],
    ["0.010", "02:41:00.671", "P1 P2 P3 P4 P5 became FRESH"],
    ["0.010", "02:41:00.671", "P1 P2 P3 P4 P5 became VALID"],
  ];

  const NA = C("n/a", "dim");
  const MASTER = ["MCU 1 (master)", "This node", NA, NA, NA, NA, "Normal (OPERATIONAL)", "0"];
  const peer = (n, age) => [`MCU ${n}`, "ONLINE", "Receiving", age, "0", "None", "Normal (OPERATIONAL)", "0"];

  const normal = {
    pc_time: "2026-11-13 02:41:10",
    source: "MCU1 over UART, localhost:3456",
    link: "CONNECTED",
    stale: false,
    health_head: "100 % HEALTHY",
    uptime: "0 d 00:00:09.905",
    config: CONFIG,
    uart: uart(121),
    events: BOOT_EVENTS,
    snapshot: {
      note: "snapshot at MCU1 t = 9.905 s",
      health: {
        pct: 100, label: C("HEALTHY", ""),
        rule: "Health = the weaker of controllers online (4/4 = 100 %) and parameters synchronized (20/20 = 100 %), each rounded down.",
        online: "4/4", synced: "20/20",
        nodes: [C("1", "", "MCU1 master"), C("2", "", "MCU2 ONLINE"), C("3", "", "MCU3 ONLINE"), C("4", "", "MCU4 ONLINE")],
        params: paramSegs(0),
      },
      nodes: [MASTER, peer(2, "0.043 s"), peer(3, "0.041 s"), peer(4, "0.040 s")],
      params: params(0),
    },
  };

  const fault = {
    pc_time: "2026-11-13 02:41:14",
    source: "MCU1 over UART, localhost:3456",
    link: "CONNECTED",
    stale: false,
    health_head: C("50 % DEGRADED", "caution"),
    uptime: "0 d 00:00:06.205",
    config: CONFIG,
    uart: uart(74),
    events: [
      ["6.105", "02:41:13.894", M("MCU4 LATE", "caution")],
      ["5.355", "02:41:12.551", M("MCU3 OFFLINE", "fault")],
      ["5.355", "02:41:12.551", M("P11 P12 P13 P14 P15 owner went offline", "fault")],
      ["5.155", "02:41:11.902", M("MCU3 LATE", "caution")],
      ["5.035", "02:41:11.508", M("P11 P12 P13 P14 P15 became STALE", "caution")],
      ...BOOT_EVENTS,
    ],
    snapshot: {
      note: "snapshot at MCU1 t = 6.205 s",
      health: {
        pct: 50, label: C("DEGRADED", "caution"),
        rule: "Health = the weaker of controllers online (2/4 = 50 %) and parameters synchronized (15/20 = 75 %), each rounded down. Only ONLINE counts as online.",
        online: "2/4", synced: "15/20",
        nodes: [C("1", "", "MCU1 master"), C("2", "", "MCU2 ONLINE"), C("3", "fault", "MCU3 OFFLINE"), C("4", "caution", "MCU4 LATE")],
        params: paramSegs(3),
      },
      nodes: [
        MASTER,
        peer(2, "0.043 s"),
        ["MCU 3", C("OFFLINE", "fault"), M("Not received", "fault"), "1.205 s", M("11", "fault"),
         M("Heartbeat and data", "fault"), C("Waiting for heartbeat", "dim"), "0"],
        ["MCU 4", C("LATE", "caution"), M("Late", "caution"), "0.158 s", M("1", "caution"),
         M("Heartbeat late", "caution"), "Normal (OPERATIONAL)", "0"],
      ],
      params: params(3),
    },
  };

  const lost = Object.assign({}, fault, {
    pc_time: "2026-11-13 02:41:18",
    link: C("LOST: no valid frame for 4.2 s", "fault"),
    stale: true,
    health_head: C("NO DATA", "dim"),
  });

  window.MOCK = { normal, fault, lost };
})();
