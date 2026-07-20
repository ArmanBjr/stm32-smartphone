/* SmartPhone Bridge UI */

const $ = (sel) => document.querySelector(sel);

let translated = null;
let pianoKeysMap = {};
let uploadInFlight = null;
let pianoArmed = false;
let pianoInputMode = "keyboard"; /* "keyboard" | "screen" */
const pianoKeysDown = new Set();
let shotCapture = null;

const PIANO_KEY_ORDER = ["a", "w", "s", "e", "d", "f", "t", "g", "y", "h", "u", "j", "k"];
const PIANO_BLACK_KEYS = new Set(["w", "e", "t", "y", "u"]);

async function api(path, opts = {}) {
  const res = await fetch(path, {
    headers: opts.body && !(opts.body instanceof FormData)
      ? { "Content-Type": "application/json" }
      : undefined,
    ...opts,
    body:
      opts.body && !(opts.body instanceof FormData)
        ? JSON.stringify(opts.body)
        : opts.body,
  });
  const text = await res.text();
  let data;
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    data = { detail: text };
  }
  if (!res.ok) {
    const msg = data.detail || data.message || res.statusText;
    throw new Error(typeof msg === "string" ? msg : JSON.stringify(msg));
  }
  return data;
}

/** New UI action: stop any song upload immediately, then run fn. */
async function withCancel(fn) {
  try {
    await api("/api/cancel", { method: "POST" });
  } catch (_) {
    /* ignore */
  }
  return fn();
}

function logLine(line) {
  const el = $("#console");
  const maxChars = 120000;
  el.textContent += line + "\n";
  if (el.textContent.length > maxChars) {
    el.textContent = el.textContent.slice(-maxChars);
  }
  el.scrollTop = el.scrollHeight;
}

function setConnected(on, port) {
  const pill = $("#connPill");
  pill.dataset.state = on ? "on" : "off";
  pill.textContent = on ? `Connected · ${port}` : "Disconnected";
  $("#btnConnect").disabled = on;
  $("#btnDisconnect").disabled = !on;
  setChoice("connection", on ? "disconnect" : "connect");
}

/** Highlight the active button in a mutually-exclusive choice group. */
function setChoice(group, value) {
  if (group == null || value == null) return;
  const v = String(value);
  document.querySelectorAll(`[data-choice-group="${group}"]`).forEach((btn) => {
    const match = btn.getAttribute("data-choice-value") === v;
    btn.classList.toggle("selected", match);
    btn.setAttribute("aria-pressed", match ? "true" : "false");
  });
}

function clearChoice(group) {
  if (group == null) return;
  document.querySelectorAll(`[data-choice-group="${group}"]`).forEach((btn) => {
    btn.classList.remove("selected");
    btn.setAttribute("aria-pressed", "false");
  });
}

function setChoiceIfPreset(group, value) {
  const v = String(value);
  const preset = document.querySelector(
    `[data-choice-group="${group}"][data-choice-value="${v}"]`
  );
  if (preset) {
    setChoice(group, v);
  } else {
    clearChoice(group);
  }
}

function setPianoArmed(on) {
  pianoArmed = on;
  const chip = $("#pianoChip");
  chip.textContent = on ? "armed" : "disarmed";
  chip.classList.toggle("on", on);
  setChoice("piano", on ? "on" : "off");
  updatePianoKbStatus();
}

function updatePianoKbStatus() {
  const el = $("#pianoKbStatus");
  if (!el) return;
  if (pianoInputMode !== "keyboard") return;
  if (!pianoArmed) {
    el.textContent = "Keyboard idle — press Piano on first.";
  } else {
    el.textContent = "Keyboard active — play with A–K (W/E/T/Y/U = sharps).";
  }
}

function setPianoInputMode(mode) {
  pianoInputMode = mode;
  const kbPanel = $("#pianoKeyboardPanel");
  const screenPanel = $("#pianoScreenPanel");
  const tabKb = $("#pianoTabKeyboard");
  const tabScreen = $("#pianoTabScreen");
  const isKb = mode === "keyboard";
  kbPanel.classList.toggle("hidden", !isKb);
  screenPanel.classList.toggle("hidden", isKb);
  tabKb.classList.toggle("active", isKb);
  tabScreen.classList.toggle("active", !isKb);
  tabKb.setAttribute("aria-selected", isKb ? "true" : "false");
  tabScreen.setAttribute("aria-selected", !isKb ? "true" : "false");
  updatePianoKbStatus();
}

function isTypingTarget(el) {
  if (!el) return false;
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || el.isContentEditable;
}

async function pianoNoteOn(freq) {
  await api("/api/piano/note-on", { method: "POST", body: { freq } });
}

async function pianoNoteOff() {
  await api("/api/piano/note-off", { method: "POST" });
}

function highlightKbKey(key, down) {
  const el = document.querySelector(`.kb-key[data-key="${key}"]`);
  if (el) el.classList.toggle("down", down);
}

async function handlePianoKeyDown(key) {
  const freq = pianoKeysMap[key];
  if (!freq || pianoKeysDown.has(key)) return;
  pianoKeysDown.add(key);
  highlightKbKey(key, true);
  try {
    await pianoNoteOn(freq);
  } catch (e) {
    logLine(`[ui] ${e.message}`);
  }
}

async function handlePianoKeyUp(key) {
  if (!pianoKeysDown.has(key)) return;
  pianoKeysDown.delete(key);
  highlightKbKey(key, false);
  try {
    await pianoNoteOff();
  } catch (e) {
    logLine(`[ui] ${e.message}`);
  }
}

function releaseAllPianoKeys() {
  const held = [...pianoKeysDown];
  pianoKeysDown.clear();
  held.forEach((k) => highlightKbKey(k, false));
  if (held.length) {
    pianoNoteOff().catch(() => {});
  }
}

function onWindowKeyDown(ev) {
  if (pianoInputMode !== "keyboard" || !pianoArmed) return;
  if (isTypingTarget(ev.target)) return;
  if (ev.repeat) return;
  const key = ev.key.toLowerCase();
  if (!pianoKeysMap[key]) return;
  ev.preventDefault();
  handlePianoKeyDown(key);
}

function onWindowKeyUp(ev) {
  if (pianoInputMode !== "keyboard") return;
  const key = ev.key.toLowerCase();
  if (!pianoKeysMap[key]) return;
  if (isTypingTarget(ev.target)) return;
  ev.preventDefault();
  handlePianoKeyUp(key);
}

function onWindowBlur() {
  releaseAllPianoKeys();
}

/** mode: "day" | "night" | "unknown" — themes the whole page. */
function setDayNight(mode) {
  const pill = $("#modePill");
  pill.dataset.mode = mode;
  if (mode === "day") {
    pill.textContent = "☀ Day";
    document.body.dataset.mode = "day";
  } else if (mode === "night") {
    pill.textContent = "☾ Night";
    document.body.dataset.mode = "night";
  } else {
    pill.textContent = "Mode: —";
  }
}

function formatUptime(seconds) {
  if (seconds == null) return "—";
  const s = Number(seconds);
  const hours = Math.floor(s / 3600);
  const minutes = Math.floor((s % 3600) / 60);
  const secs = s % 60;
  return hours > 0
    ? `${hours}h ${minutes}m ${secs}s`
    : `${minutes}m ${secs}s`;
}

function setHealth(health = {}) {
  const value = (v, suffix = "") => (v == null ? "—" : `${v}${suffix}`);
  $("#healthUptime").textContent = formatUptime(health.uptime_s);
  $("#healthEventDrops").textContent = value(health.drops_evt);
  $("#healthTxDrops").textContent = value(health.drops_tx);
  $("#healthUiHwm").textContent = value(health.hwm_ui, " words");
  $("#healthAppHwm").textContent = value(health.hwm_app, " words");
  $("#healthStorageHwm").textContent = value(health.hwm_storage, " words");
  $("#healthUpdated").textContent = health.updated_at
    ? `Last report: ${health.updated_at}`
    : "Waiting for a health report.";
}

function applyHealthLine(line) {
  const stamp = () => {
    $("#healthUpdated").textContent =
      `Last report: ${new Date().toLocaleTimeString()}`;
  };
  let match = line.match(
    /\[HEALTH\]\s+uptime=(\d+)s\s+drops_evt=(\d+)\s+drops_tx=(\d+)/
  );
  if (match) {
    $("#healthUptime").textContent = formatUptime(Number(match[1]));
    $("#healthEventDrops").textContent = match[2];
    $("#healthTxDrops").textContent = match[3];
    stamp();
    return;
  }
  match = line.match(
    /\[HEALTH\]\s+hwm\s+ui=(\d+)\s+app=(\d+)\s+storage=(\d+)/
  );
  if (match) {
    $("#healthUiHwm").textContent = `${match[1]} words`;
    $("#healthAppHwm").textContent = `${match[2]} words`;
    $("#healthStorageHwm").textContent = `${match[3]} words`;
    stamp();
  }
}

function renderLcdPreview(rows) {
  const root = $("#lcdPreview");
  if (!root) return;
  root.innerHTML = rows
    .map((row) => `<div class="lcd-row">${escapeHtml(row)}</div>`)
    .join("");
}

function escapeHtml(s) {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

function handleShotLine(line) {
  if (line === "[SHOT]") {
    shotCapture = [];
    return;
  }
  if (line === "[/SHOT]") {
    if (shotCapture?.length) renderLcdPreview(shotCapture);
    shotCapture = null;
    return;
  }
  if (!shotCapture) return;
  const m = line.match(/^\|(.+)\|$/);
  if (m) shotCapture.push(m[1]);
}

async function requestHealth() {
  await api("/api/phone/health", { method: "POST" });
}

async function requestShot() {
  await api("/api/phone/shot", { method: "POST" });
}

async function refreshSmsHistory() {
  try {
    const data = await api("/api/sms/history");
    const body = $("#smsBody");
    const items = data.items || [];
    if (!items.length) {
      body.innerHTML = '<tr><td colspan="4" class="muted">No SMS yet.</td></tr>';
      return;
    }
    body.innerHTML = items
      .filter((it) => it.direction === "send" || it.direction === "result")
      .map((it) => {
        const to = (it.to || "—").replace(/</g, "&lt;");
        const text = (it.text || "").replace(/</g, "&lt;");
        const status = (it.status || "").replace(/</g, "&lt;");
        return `<tr><td>${it.time || ""}</td><td>${to}</td><td>${text}</td><td>${status}</td></tr>`;
      })
      .join("");
  } catch (e) {
    logLine(`[ui] sms history: ${e.message}`);
  }
}

async function refreshStatus() {
  const s = await api("/api/status");
  setConnected(s.connected, s.port);
  setPianoArmed(s.piano_armed);
  setDayNight(s.day_night || "unknown");
  setHealth(s.health || {});
  $("#maxNotes").textContent = String(s.max_song_notes || 80);
  pianoKeysMap = s.piano_keys || {};
  const dl = $("#portList");
  dl.innerHTML = "";
  (s.ports || []).forEach((p) => {
    const o = document.createElement("option");
    o.value = p;
    dl.appendChild(o);
  });
  if (!s.connected && s.ports?.length && !$("#portInput").value) {
    $("#portInput").value = s.ports[0];
  }
  buildPianoScreen();
  buildKbLayout();
  return s;
}

function buildKbLayout() {
  const root = $("#kbLayout");
  if (!root || root.dataset.built === "1") return;
  PIANO_KEY_ORDER.forEach((k) => {
    const freq = pianoKeysMap[k];
    if (!freq) return;
    const el = document.createElement("div");
    el.className = "kb-key" + (PIANO_BLACK_KEYS.has(k) ? " black" : "");
    el.dataset.key = k;
    el.innerHTML = `<span class="kb-letter">${k.toUpperCase()}</span><span class="kb-freq">${freq}</span>`;
    root.appendChild(el);
  });
  root.dataset.built = "1";
}

function buildPianoScreen() {
  const root = $("#pianoKeys");
  if (root.dataset.built === "1") return;
  PIANO_KEY_ORDER.forEach((k) => {
    const freq = pianoKeysMap[k];
    if (!freq) return;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "key" + (PIANO_BLACK_KEYS.has(k) ? " black" : "");
    btn.innerHTML = `<span>${k.toUpperCase()}</span><span>${freq}</span>`;
    const down = async () => {
      btn.classList.add("down");
      try {
        await pianoNoteOn(freq);
      } catch (e) {
        logLine(`[ui] ${e.message}`);
      }
    };
    const up = async () => {
      btn.classList.remove("down");
      try {
        await pianoNoteOff();
      } catch (e) {
        logLine(`[ui] ${e.message}`);
      }
    };
    btn.addEventListener("mousedown", (ev) => {
      ev.preventDefault();
      down();
    });
    btn.addEventListener("mouseup", up);
    btn.addEventListener("mouseleave", () => {
      if (btn.classList.contains("down")) up();
    });
    btn.addEventListener("touchstart", (ev) => {
      ev.preventDefault();
      down();
    });
    btn.addEventListener("touchend", (ev) => {
      ev.preventDefault();
      up();
    });
    root.appendChild(btn);
  });
  root.dataset.built = "1";
}

function connectEvents() {
  const es = new EventSource("/api/events");
  es.onmessage = (ev) => {
    const line = ev.data || "";
    logLine(line);
    applyHealthLine(line);
    handleShotLine(line);
    if (line.includes("[piano] ARMED") || line.includes("[PIANO] enter")) {
      setPianoArmed(true);
    }
    if (line.includes("[piano] DISARMED") || line.includes("[PIANO] exit")) {
      setPianoArmed(false);
      releaseAllPianoKeys();
    }
    if (line.includes("[hub] connected")) {
      refreshStatus().catch(() => {});
    }
    if (line.includes("[hub] disconnected")) {
      setConnected(false, "");
      setPianoArmed(false);
      releaseAllPianoKeys();
      setDayNight("unknown");
      setHealth({});
    }
    if (line.startsWith("[mode] ")) {
      setDayNight(line.slice(7).trim());
    }
    if (line.includes("SMS_SEND|") || line.includes("SMS_RESULT|") || line.includes("[HTTP] result=")) {
      refreshSmsHistory();
    }
  };
  es.onerror = () => {
    /* browser auto-reconnects */
  };
}

function showTranslateResult(data) {
  translated = data;
  const secs = (data.duration_ms / 1000).toFixed(1);
  let text = `${data.message}\nnotes=${data.note_count}  duration≈${secs}s`;
  if (data.truncated) text += `\n⚠ truncated from ${data.source_notes}`;
  if (data.uploaded != null) text += `\nuploaded ${data.uploaded} notes as "${data.name}"`;
  text += "\n\n" + data.notes.map(([f, m]) => `${f},${m}`).join("\n");
  $("#songPreview").textContent = text;
}

async function translateFile(uploadToo) {
  const file = $("#songFile").files?.[0];
  if (!file) throw new Error("choose a file first");
  const name = $("#songName").value.trim() || "Song";
  const fd = new FormData();
  fd.append("file", file);
  fd.append("name", name);
  const path = uploadToo
    ? "/api/song/translate-and-upload"
    : "/api/song/translate";
  const data = await api(path, { method: "POST", body: fd });
  showTranslateResult(data);
  return data;
}

document.addEventListener("DOMContentLoaded", async () => {
  connectEvents();
  window.addEventListener("keydown", onWindowKeyDown);
  window.addEventListener("keyup", onWindowKeyUp);
  window.addEventListener("blur", onWindowBlur);

  setPianoInputMode("keyboard");
  setChoice("piano", "off");
  setChoice("connection", "connect");

  $("#pianoTabKeyboard").onclick = () => setPianoInputMode("keyboard");
  $("#pianoTabScreen").onclick = () => setPianoInputMode("screen");

  $("#refreshPorts").onclick = () => refreshStatus().catch((e) => logLine(`[ui] ${e.message}`));

  $("#btnConnect").onclick = async () => {
    try {
      await withCancel(async () => {
        const port = $("#portInput").value.trim();
        if (!port) throw new Error("enter COM port");
        await api("/api/connect", { method: "POST", body: { port } });
        await refreshStatus();
        await requestHealth();
      });
    } catch (e) {
      logLine(`[ui] ${e.message}`);
    }
  };

  $("#btnDisconnect").onclick = async () => {
    try {
      await withCancel(async () => {
        await api("/api/disconnect", { method: "POST" });
        await refreshStatus();
      });
    } catch (e) {
      logLine(`[ui] ${e.message}`);
    }
  };

  document.querySelectorAll("[data-cmd]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const cmd = btn.getAttribute("data-cmd");
      try {
        if (cmd === "reset" && !confirm("Reset the board?")) return;
        await withCancel(() => api(`/api/phone/${cmd}`, { method: "POST" }));
      } catch (e) {
        logLine(`[ui] ${e.message}`);
      }
    });
  });

  $("#btnShot").onclick = () =>
    withCancel(() => requestShot()).catch((e) => logLine(`[ui] ${e.message}`));

  $("#pianoOn").onclick = () =>
    withCancel(() => api("/api/piano/on", { method: "POST" }))
      .then(() => setChoice("piano", "on"))
      .catch((e) => logLine(`[ui] ${e.message}`));
  $("#pianoOff").onclick = () =>
    withCancel(() => api("/api/piano/off", { method: "POST" }))
      .then(() => setChoice("piano", "off"))
      .catch((e) => logLine(`[ui] ${e.message}`));

  $("#btnTranslate").onclick = async () => {
    try {
      await withCancel(() => translateFile(false));
    } catch (e) {
      logLine(`[ui] ${e.message}`);
      $("#songPreview").textContent = "Error: " + e.message;
    }
  };

  $("#btnUploadSong").onclick = async () => {
    try {
      await api("/api/cancel", { method: "POST" });
      if ($("#songFile").files?.[0]) {
        await translateFile(true);
      } else if (translated?.notes) {
        const data = await api("/api/song/upload-notes", {
          method: "POST",
          body: {
            name: $("#songName").value || translated.name,
            notes: translated.notes,
          },
        });
        logLine(`[ui] uploaded ${data.uploaded} notes`);
      } else {
        throw new Error("translate a file first, or pick a file and upload");
      }
    } catch (e) {
      if (e.message === "cancelled") {
        logLine("[ui] previous/current upload cancelled");
      } else {
        logLine(`[ui] ${e.message}`);
      }
    }
  };

  $("#btnSmsRefresh").onclick = () => refreshSmsHistory();

  $("#btnHealthRefresh").onclick = () =>
    requestHealth().catch((e) => logLine(`[ui] health: ${e.message}`));

  async function sendSetting(name, value) {
    const line = `/setting-${name}-${value}`;
    await withCancel(() => api("/api/send", { method: "POST", body: { line } }));
  }

  document.querySelectorAll("[data-setting]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const name = btn.getAttribute("data-setting");
      const value = btn.getAttribute("data-value");
      try {
        if (name === "ldr") {
          $("#ldrValue").value = value;
        }
        if (name === "volume") {
          $("#volRange").value = value;
          $("#volChip").textContent = `${value}%`;
        }
        await sendSetting(name, value);
        setChoice(name, value);
      } catch (e) {
        logLine(`[ui] ${e.message}`);
      }
    });
  });

  $("#volRange").addEventListener("input", () => {
    $("#volChip").textContent = `${$("#volRange").value}%`;
  });

  $("#btnVolApply").onclick = async () => {
    try {
      const v = $("#volRange").value;
      await sendSetting("volume", v);
      setChoiceIfPreset("volume", v);
    } catch (e) {
      logLine(`[ui] ${e.message}`);
    }
  };

  $("#btnLdrApply").onclick = async () => {
    try {
      const v = Math.max(0, Math.min(4095, parseInt($("#ldrValue").value, 10) || 0));
      $("#ldrValue").value = String(v);
      await sendSetting("ldr", v);
      setChoiceIfPreset("ldr", String(v));
    } catch (e) {
      logLine(`[ui] ${e.message}`);
    }
  };

  $("#btnClearLog").onclick = () => {
    $("#console").textContent = "";
  };

  try {
    await refreshStatus();
    await refreshSmsHistory();
  } catch (e) {
    logLine(`[ui] ${e.message}`);
  }
});
