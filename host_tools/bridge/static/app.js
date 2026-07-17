/* SmartPhone Bridge UI */

const $ = (sel) => document.querySelector(sel);

let translated = null;
let pianoKeysMap = {};
let uploadInFlight = null;

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
  // Cap DOM size so the log panel stays responsive
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
}

function setPianoArmed(on) {
  const chip = $("#pianoChip");
  chip.textContent = on ? "armed" : "disarmed";
  chip.classList.toggle("on", on);
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
    // Keep the last known theme; default (night palette) applies if none set.
  }
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
  buildPiano();
  return s;
}

function buildPiano() {
  const root = $("#pianoKeys");
  if (root.dataset.built === "1") return;
  const order = ["a", "w", "s", "e", "d", "f", "t", "g", "y", "h", "u", "j", "k"];
  const blacks = new Set(["w", "e", "t", "y", "u"]);
  order.forEach((k) => {
    const freq = pianoKeysMap[k];
    if (!freq) return;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "key" + (blacks.has(k) ? " black" : "");
    btn.innerHTML = `<span>${k.toUpperCase()}</span><span>${freq}</span>`;
    const down = async () => {
      btn.classList.add("down");
      try {
        await api("/api/piano/note-on", { method: "POST", body: { freq } });
      } catch (e) {
        logLine(`[ui] ${e.message}`);
      }
    };
    const up = async () => {
      btn.classList.remove("down");
      try {
        await api("/api/piano/note-off", { method: "POST" });
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
    if (line.includes("[piano] ARMED") || line.includes("[PIANO] enter")) {
      setPianoArmed(true);
    }
    if (line.includes("[piano] DISARMED") || line.includes("[PIANO] exit")) {
      setPianoArmed(false);
    }
    if (line.includes("[hub] connected")) {
      refreshStatus().catch(() => {});
    }
    if (line.includes("[hub] disconnected")) {
      setConnected(false, "");
      setPianoArmed(false);
      setDayNight("unknown");
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

  $("#refreshPorts").onclick = () => refreshStatus().catch((e) => logLine(`[ui] ${e.message}`));

  $("#btnConnect").onclick = async () => {
    try {
      await withCancel(async () => {
        const port = $("#portInput").value.trim();
        if (!port) throw new Error("enter COM port");
        await api("/api/connect", { method: "POST", body: { port } });
        await refreshStatus();
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

  $("#pianoOn").onclick = () =>
    withCancel(() => api("/api/piano/on", { method: "POST" })).catch((e) =>
      logLine(`[ui] ${e.message}`)
    );
  $("#pianoOff").onclick = () =>
    withCancel(() => api("/api/piano/off", { method: "POST" })).catch((e) =>
      logLine(`[ui] ${e.message}`)
    );

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
      // Cancel any previous upload, then start this one (exclusive on server).
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
      await sendSetting("volume", $("#volRange").value);
    } catch (e) {
      logLine(`[ui] ${e.message}`);
    }
  };

  $("#btnLdrApply").onclick = async () => {
    try {
      const v = Math.max(0, Math.min(4095, parseInt($("#ldrValue").value, 10) || 0));
      $("#ldrValue").value = String(v);
      await sendSetting("ldr", v);
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
