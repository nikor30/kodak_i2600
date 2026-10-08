"use strict";
const $ = (id) => document.getElementById(id);
const FUNCTIONS = [1, 2, 3, 4, 5, 6, 7];
let settings = null;

function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "class") node.className = v; else if (k === "text") node.textContent = v; else node[k] = v;
  }
  node.append(...children);
  return node;
}

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: "POST", body: JSON.stringify(body),
    headers: { "Content-Type": "application/json", "X-Requested-With": "kodak-web" },
  };
  const r = await fetch(path, opts);
  let data = {};
  try { data = await r.json(); } catch (e) { /* no body */ }
  if (!r.ok) throw new Error(data.error || `The station answered with an error (${r.status})`);
  return data;
}

function message(node, text, ok) {
  node.textContent = text;
  node.className = "msg" + (ok === true ? " ok" : ok === false ? " bad" : "");
}

/* ---- station state ------------------------------------------------------ */
async function refreshState() {
  try {
    const s = await api("/api/state");
    const pill = $("state");
    if (s.service !== "running") {
      pill.textContent = "Station not running"; pill.className = "pill bad";
      $("state-detail").textContent = "The scan service is stopped or restarting.";
    } else {
      const names = { ready: "Ready", scanning: "Scanning", error: "Attention", starting: "Starting" };
      pill.textContent = names[s.state] || s.state || "Unknown";
      pill.className = "pill " + (s.state === "ready" ? "ok" : s.state === "error" ? "bad" : "warn");
      const parts = [];
      if (s.error) parts.push(s.error);
      if (s.label) parts.push(`Selected on the scanner: ${s.label}`);
      if (s.state === "scanning") parts.push(`Sheet ${s.pages || 0}`);
      parts.push(s.waiting ? `${s.waiting} scan(s) waiting for upload` : "Nothing waiting for upload");
      if (s.failed) parts.push(`${s.failed} upload(s) failed for good`);
      $("state-detail").textContent = parts.join(" · ");
    }
    const body = $("jobs").tBodies[0];
    body.replaceChildren();
    for (const j of s.jobs || []) {
      const where = { sent: "uploaded", outbox: "waiting", failed: "failed" }[j.where] || j.where;
      body.append(el("tr", {}, el("td", { text: j.title || j.job }), el("td", { text: String(j.pages) }), el("td", { text: where })));
    }
    $("jobs").hidden = !(s.jobs || []).length;
  } catch (e) {
    $("state").textContent = "No connection"; $("state").className = "pill bad";
  }
}

/* ---- settings form ------------------------------------------------------- */
function profileNames() { return settings.profiles.map((p) => p.name); }

function renderFunctions() {
  const body = $("functions").tBodies[0];
  body.replaceChildren();
  for (const n of FUNCTIONS) {
    const select = el("select", { id: `fn-${n}` }, el("option", { value: "", text: "– not used –" }));
    select.setAttribute("aria-label", `Profile for function ${n}`);
    for (const name of profileNames()) select.append(el("option", { value: name, text: name }));
    select.value = profileNames().includes(settings.functions[n]) ? settings.functions[n] : "";
    const text = el("td", { class: "small" });
    const show = () => {
      const p = settings.profiles.find((q) => q.name === select.value);
      text.textContent = p ? (p.label || p.name) : `${n}: not used`;
    };
    select.addEventListener("change", () => { settings.functions[n] = select.value; show(); });
    show();
    body.append(el("tr", {}, el("td", { class: "num", text: String(n) }), el("td", {}, select), text));
  }
}

function field(labelText, input) {
  const id = "f" + Math.random().toString(36).slice(2);
  input.id = id;
  return el("div", {}, el("label", { htmlFor: id, text: labelText }), input);
}

function check(labelText, checked, onChange) {
  const box = el("input", { type: "checkbox", checked });
  box.addEventListener("change", () => onChange(box.checked));
  return el("label", { class: "check" }, box, labelText);
}

function renderProfiles() {
  const root = $("profiles");
  root.replaceChildren();
  settings.profiles.forEach((p, i) => {
    const title = el("strong", { text: p.name || "New profile" });
    const remove = el("button", { type: "button", class: "danger", text: "Remove" });
    remove.addEventListener("click", () => {
      settings.profiles.splice(i, 1);
      for (const n of FUNCTIONS) if (settings.functions[n] === p.name) settings.functions[n] = "";
      renderAll();
    });
    remove.disabled = settings.profiles.length < 2;

    const name = el("input", { type: "text", value: p.name, maxLength: 32, spellcheck: false });
    name.addEventListener("change", () => {
      const old = p.name;
      p.name = name.value.trim();
      for (const n of FUNCTIONS) if (settings.functions[n] === old) settings.functions[n] = p.name;
      if (settings.default_profile === old) settings.default_profile = p.name;
      renderAll();
    });
    const label = el("input", { type: "text", value: p.label, maxLength: 60 });
    label.addEventListener("input", () => { p.label = label.value; renderFunctions(); });
    const mode = el("select", {}, el("option", { value: "Color", text: "Colour" }), el("option", { value: "Gray", text: "Gray" }),
      el("option", { value: "Lineart", text: "Black and white" }));
    mode.value = p.mode;
    const quality = el("input", { type: "number", min: 1, max: 100, value: p.jpeg_quality });
    quality.addEventListener("change", () => { p.jpeg_quality = Number(quality.value); });
    const threshold = el("input", { type: "number", min: 0, max: 255, value: p.bw_threshold });
    threshold.addEventListener("change", () => { p.bw_threshold = Number(threshold.value); });
    const qualityField = field("JPEG quality (1–100)", quality);
    const thresholdField = field("Black/white threshold (0–255, higher = darker)", threshold);
    const showMode = () => { qualityField.hidden = p.mode === "Lineart"; thresholdField.hidden = p.mode !== "Lineart"; };
    mode.addEventListener("change", () => { p.mode = mode.value; showMode(); });
    showMode();
    const docTitle = el("input", { type: "text", value: p.title, maxLength: 120 });
    docTitle.addEventListener("change", () => { p.title = docTitle.value; });
    const tags = el("input", { type: "text", value: p.tags.join(", "), placeholder: "e.g. inbox, scanner" });
    tags.addEventListener("change", () => { p.tags = tags.value.split(",").map((t) => t.trim()).filter(Boolean); });

    root.append(el("div", { class: "profile" },
      el("div", { class: "head" }, title, remove),
      el("div", { class: "grid" },
        field("Name", name), field("Text on the display", label), field("Colour mode", mode), qualityField, thresholdField,
        field("Document title in Paperless", docTitle), field("Paperless tags (comma separated)", tags)),
      el("div", { class: "row", style: "margin-top:8px" },
        check("Scan both sides", p.duplex, (v) => { p.duplex = v; }),
        check("Leave out blank sides", p.drop_blank, (v) => { p.drop_blank = v; }))));
  });
}

function renderAll() {
  $("pl-url").value = settings.paperless.url;
  $("pl-token").placeholder = settings.paperless.token_set ? "stored – leave empty to keep it" : "not set yet";
  $("trigger").value = settings.trigger;
  renderFunctions();
  renderProfiles();
}

async function load() {
  try {
    settings = await api("/api/settings");
    $("pl-token").value = "";
    renderAll();
    message($("save-msg"), "");
  } catch (e) {
    message($("save-msg"), e.message, false);
  }
}

$("pl-url").addEventListener("change", () => { settings.paperless.url = $("pl-url").value.trim(); });
$("trigger").addEventListener("change", () => { settings.trigger = $("trigger").value; });
$("add-profile").addEventListener("click", () => {
  let n = settings.profiles.length + 1;
  while (profileNames().includes(`profile${n}`)) n += 1;
  settings.profiles.push({ name: `profile${n}`, label: "", mode: "Color", duplex: true, drop_blank: true,
    jpeg_quality: 85, bw_threshold: 200, title: "Scan {created:%Y-%m-%d %H:%M}", tags: [] });
  renderAll();
});
$("reload").addEventListener("click", load);

$("pl-test").addEventListener("click", async () => {
  const out = $("pl-result");
  message(out, "Testing…");
  $("pl-test").disabled = true;
  try {
    const r = await api("/api/test-paperless", { url: $("pl-url").value, token: $("pl-token").value });
    message(out, r.message, r.ok);
  } catch (e) {
    message(out, e.message, false);
  }
  $("pl-test").disabled = false;
});

$("save").addEventListener("click", async () => {
  const out = $("save-msg");
  const body = JSON.parse(JSON.stringify(settings));
  body.paperless = { url: $("pl-url").value.trim() };
  if ($("pl-token").value.trim()) body.paperless.token = $("pl-token").value.trim();
  $("save").disabled = true;
  message(out, "Saving…");
  try {
    const r = await api("/api/settings", body);
    settings = r.settings;
    $("pl-token").value = "";
    renderAll();
    message(out, r.notes.join(" "), true);
  } catch (e) {
    message(out, e.message, false);
  }
  $("save").disabled = false;
});

load();
refreshState();
setInterval(refreshState, 3000);
