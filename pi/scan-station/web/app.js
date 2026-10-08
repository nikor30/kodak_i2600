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
    const dest = el("select", {}, el("option", { value: "paperless", text: "Paperless-ngx" }),
      el("option", { value: "smb", text: "Network share" }), el("option", { value: "email", text: "E-mail" }));
    dest.value = p.destination;
    const mailTo = el("input", { type: "text", value: p.email_to, placeholder: "the recipient set under E-mail" });
    mailTo.addEventListener("change", () => { p.email_to = mailTo.value.trim(); });
    const tagsField = field("Paperless tags (comma separated)", tags);
    const mailToField = field("Recipient for this profile (optional)", mailTo);
    const showDest = () => { tagsField.hidden = p.destination !== "paperless"; mailToField.hidden = p.destination !== "email"; };
    dest.addEventListener("change", () => { p.destination = dest.value; showDest(); });
    showDest();

    root.append(el("div", { class: "profile" },
      el("div", { class: "head" }, title, remove),
      el("div", { class: "grid" },
        field("Name", name), field("Text on the display", label), field("Colour mode", mode), qualityField, thresholdField,
        field("Send to", dest), mailToField, field("Document title / file name", docTitle), tagsField),
      el("div", { class: "row", style: "margin-top:8px" },
        check("Scan both sides", p.duplex, (v) => { p.duplex = v; }),
        check("Leave out blank sides", p.drop_blank, (v) => { p.drop_blank = v; }))));
  });
}

function renderAll() {
  $("pl-url").value = settings.paperless.url;
  $("pl-token").placeholder = settings.paperless.token_set ? "stored – leave empty to keep it" : "not set yet";
  $("trigger").value = settings.trigger;
  $("standby").value = settings.standby_after;
  $("display-info").checked = settings.display_info;
  for (const k of ["share", "folder", "username", "domain"]) $(`smb-${k}`).value = settings.smb[k];
  $("smb-password").placeholder = settings.smb.password_set ? "stored – leave empty to keep it" : "not set";
  for (const k of ["host", "port", "security", "username", "sender", "to"]) $(`mail-${k}`).value = settings.email[k];
  $("mail-password").placeholder = settings.email.password_set ? "stored – leave empty to keep it" : "not set";
  renderFunctions();
  renderProfiles();
}

async function load() {
  try {
    settings = await api("/api/settings");
    for (const id of ["pl-token", "smb-password", "mail-password"]) $(id).value = "";
    renderAll();
    message($("save-msg"), "");
  } catch (e) {
    message($("save-msg"), e.message, false);
  }
}

$("pl-url").addEventListener("change", () => { settings.paperless.url = $("pl-url").value.trim(); });
$("trigger").addEventListener("change", () => { settings.trigger = $("trigger").value; });
$("standby").addEventListener("change", () => { settings.standby_after = Number($("standby").value) || 0; });
$("display-info").addEventListener("change", () => { settings.display_info = $("display-info").checked; });
$("add-profile").addEventListener("click", () => {
  let n = settings.profiles.length + 1;
  while (profileNames().includes(`profile${n}`)) n += 1;
  settings.profiles.push({ name: `profile${n}`, label: "", mode: "Color", duplex: true, drop_blank: true,
    jpeg_quality: 85, bw_threshold: 200, title: "Scan {created:%Y-%m-%d %H:%M}", tags: [],
    destination: "paperless", email_to: "" });
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

function smbForm() {
  return { share: $("smb-share").value.trim(), folder: $("smb-folder").value.trim(), username: $("smb-username").value.trim(),
    domain: $("smb-domain").value.trim(), password: $("smb-password").value };
}

function mailForm() {
  return { host: $("mail-host").value.trim(), port: Number($("mail-port").value) || 587, security: $("mail-security").value,
    username: $("mail-username").value.trim(), sender: $("mail-sender").value.trim(), to: $("mail-to").value.trim(),
    password: $("mail-password").value };
}

function testButton(button, result, path, form, busy) {
  $(button).addEventListener("click", async () => {
    const out = $(result);
    message(out, busy);
    $(button).disabled = true;
    try {
      const r = await api(path, form());
      message(out, r.message, r.ok);
    } catch (e) {
      message(out, e.message, false);
    }
    $(button).disabled = false;
  });
}
testButton("smb-test", "smb-result", "/api/test-smb", smbForm, "Testing…");
testButton("mail-test", "mail-result", "/api/test-email", mailForm, "Sending…");

/* ---- statistics ----------------------------------------------------------- */
const fmt = (n) => Number(n || 0).toLocaleString();

function tile(label, value, sub) {
  return el("div", { class: "tile" }, el("div", { class: "k", text: label }), el("div", { class: "v", text: value }),
    el("div", { class: "s", text: sub }));
}

function fillTable(id, rows) {
  const body = $(id).tBodies[0];
  body.replaceChildren();
  for (const cells of rows) body.append(el("tr", {}, ...cells.map((c) => el("td", { text: String(c) }))));
  $(id).hidden = !rows.length;
}

async function refreshStats() {
  let s;
  try { s = await api("/api/statistics"); } catch (e) { return; }
  $("stats").hidden = !s.available;
  if (!s.available) return;
  const t = s.totals;
  $("stats-since").textContent = `Counted since ${s.since}.` + (t.sheets ? ` ${fmt(t.sheets)} sheets fed, ${fmt(t.blank)} blank sides left out.` : "");
  const perJob = t.sheets && t.scan_seconds ? ` · ${(t.scan_seconds / t.sheets).toFixed(1)} s per sheet` : "";
  $("stats-tiles").replaceChildren(
    tile("Today", fmt(s.today.pages), `pages in ${fmt(s.today.jobs)} scan(s)`),
    tile("Last 7 days", fmt(s.week.pages), `pages in ${fmt(s.week.jobs)} scan(s)`),
    tile("Last 30 days", fmt(s.month.pages), `pages in ${fmt(s.month.jobs)} scan(s)`),
    tile("In total", fmt(t.pages), `pages in ${fmt(t.jobs)} scan(s)${perJob}`));
  const max = Math.max(1, ...s.days.map((d) => d.pages));
  const readout = $("bars-readout");
  const bars = s.days.map((d) => {
    const fill = el("i");
    fill.style.height = `${Math.max(d.pages ? 4 : 0, Math.round((d.pages / max) * 100))}%`;
    const bar = el("div", { class: "bar" + (d.pages ? "" : " empty"), tabIndex: 0 }, fill);
    const text = `${d.day}: ${fmt(d.pages)} page(s) in ${fmt(d.jobs)} scan(s)`;
    bar.title = text;
    bar.setAttribute("aria-label", text);
    const show = () => { readout.textContent = text; };
    bar.addEventListener("mouseenter", show);
    bar.addEventListener("focus", show);
    return bar;
  });
  $("bars").replaceChildren(...bars);
  $("bars-from").textContent = s.days[0].day;
  $("bars-to").textContent = "today";
  fillTable("by-profile", Object.entries(s.profiles).sort((a, b) => b[1].pages - a[1].pages).map(([n, v]) => [n, fmt(v.jobs), fmt(v.pages)]));
  const names = { paperless: "Paperless-ngx", smb: "Network share", email: "E-mail" };
  fillTable("by-destination", Object.entries(s.destinations).sort((a, b) => b[1] - a[1]).map(([n, v]) => [names[n] || n, fmt(v)]));
}
$("bars").addEventListener("mouseleave", () => { $("bars-readout").textContent = ""; });

$("save").addEventListener("click", async () => {
  const out = $("save-msg");
  const body = JSON.parse(JSON.stringify(settings));
  body.paperless = { url: $("pl-url").value.trim() };
  if ($("pl-token").value.trim()) body.paperless.token = $("pl-token").value.trim();
  body.smb = smbForm();
  body.email = mailForm();
  $("save").disabled = true;
  message(out, "Saving…");
  try {
    const r = await api("/api/settings", body);
    settings = r.settings;
    for (const id of ["pl-token", "smb-password", "mail-password"]) $(id).value = "";
    renderAll();
    message(out, r.notes.join(" "), true);
  } catch (e) {
    message(out, e.message, false);
  }
  $("save").disabled = false;
});

load();
refreshState();
refreshStats();
setInterval(refreshState, 3000);
setInterval(refreshStats, 30000);
