// AUTOEXTRACT_V1: embedded DiscrordBotRcon source files, auto-generated.
// Do not hand-edit; regenerate from DiscrordBotRcon/*.js if the bot source changes.
// NOTE: each file's content is split into small adjacent raw-string-literal chunks
// (which the compiler concatenates at compile time) because MSVC's single raw
// string literal token has a much lower practical limit than its documented
// 65,535-byte post-concatenation string limit (observed: error C2026 around ~19KB).
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace nc {

inline const char* kBotFile_index_js =
    R"ZVRNBOT(// DSBOT_V2 — Discord RCON бот Zevvoryn + веб-панель
require('dotenv').config();
const {
    Client, GatewayIntentBits, InteractionType,
    EmbedBuilder, AutocompleteInteraction,
} = require('discord.js');
const { RconBridge, stripColors, asCodeBlock } = require('./rcon');
const { QUICK_CMDS } = require('./commands');

const TOKEN      = process.env.DISCORD_TOKEN;
const ADMIN_ROLE = process.env.ADMIN_ROLE_ID || '';
const CHAN       = process.env.COMMAND_CHANNEL_ID || '';

// ── Веб-панель (WEBPANEL_V1) — работает независимо от Discord-бота ──
let webPanelStarted = false;
if (process.env.WEB_ENABLED !== 'false') {
    try {
        require('./webpanel').start();
        webPanelStarted = true;
    } catch (e) {
        console.warn('[WebPanel] Не удалось запустить:', e.message);
        console.warn('[WebPanel] Убедитесь, что express и ws установлены: npm install');
    }
}

if (!TOKEN) {
    console.log('[Bot] DISCORD_TOKEN не задан — Discord-бот пропущен, работает только веб-панель (если включена).');
    if (!webPanelStarted) {
        console.error('[Bot] DISCORD_TOKEN не задан и веб-панель выключена/не запущена — делать нечего, выхожу.');
        process.exit(1);
    }
}

// ── Вспомогательные функции ─────────────────────────────────
function makeRcon() {
    return new RconBridge({
        host:     process.env.RCON_HOST     || '127.0.0.1',
        port:     Number(process.env.RCON_PORT || 25575),
        password: process.env.RCON_PASSWORD || '',
        timeout:  Number(process.env.RCON_TIMEOUT_MS || 10000),
    });
}

// Отправить RCON-команду и получить ответ строкой
async function rcon(cmd) {
    const b = makeRcon();
    try {
        return stripColors(String(await b.send(cmd) || '(empty response)'));
    } finally {
        b.close().catch(() => {});
    }
}

// DSBOT_V2: длинный ответ бьём несколькими сообщениями
async function replyLong(interaction, text) {
    const MAX = 1900;
    const chunks = [];
    for (let i = 0; i < text.length; i += MAX) chunks.push(text.slice(i, i + MAX));
    if (chunks.length === 0) chunks.push('(empty response)');
    await interaction.editReply('```\n' + chunks[0] + '\n```');
    for (let i = 1; i < chunks.length; i++) {
        await interaction.followUp({ content: '```\n' + chunks[i] + '\n```', ephemeral: false });
    }
}

function isAdmin(interaction) {
    if (ADMIN_ROLE) return interaction.member?.roles?.cache?.has(ADMIN_ROLE);
    return interaction.member?.permissions?.has('Administrator');
}

// ── Discord клиент (только если задан токен) ─────────────────
if (TOKEN) {
const client = new Client({
    intents: [GatewayIntentBits.Guilds],
});

client.once('ready', () => {
    console.log(`[Bot] Ready: ${client.user.tag}`);
});

client.on('interactionCreate', async interaction => {
    // ── Autocomplete (/cmd) ──
    if (interaction.type === InteractionType.ApplicationCommandAutocomplete) {
        if (interaction.commandName === 'cmd') {
            const focused = interaction.options.getFoc)ZVRNBOT"
    R"ZVRNBOT(used().toLowerCase();
            const choices = QUICK_CMDS
                .filter(c => c.toLowerCase().includes(focused))
                .slice(0, 25)
                .map(c => ({ name: c, value: c }));
            return interaction.respond(choices);
        }
        return;
    }

    if (interaction.type !== InteractionType.ApplicationCommand) return;

    // Проверка канала
    if (CHAN && interaction.channelId !== CHAN) {
        return interaction.reply({
            content: `Команды только в <#${CHAN}>`,
            ephemeral: true,
        });
    }

    const { commandName } = interaction;
    const admin = isAdmin(interaction);

    // ── /status — не нужно defer ──
    if (commandName === 'status') {
        await interaction.deferReply();
        try {
            const t0 = Date.now();
            const resp = await rcon('list');
            const ms = Date.now() - t0;
            const embed = new EmbedBuilder()
                .setColor(0x3fb950)
                .setTitle('\u2705 Сервер онлайн')
                .setDescription('```' + resp.trim() + '```')
                .setFooter({ text: `RCON ping: ${ms}ms` })
                .setTimestamp();
            return interaction.editReply({ embeds: [embed] });
        } catch (err) {
            const embed = new EmbedBuilder()
                .setColor(0xf85149)
                .setTitle('\u274c Сервер недоступен')
                .setDescription(err.message);
            return interaction.editReply({ embeds: [embed] });
        }
    }

    await interaction.deferReply();

    try {
        // ── /list ──
        if (commandName === 'list') {
            const r = await rcon('list');
            return replyLong(interaction, r);
        }

        // ── /say ──
        if (commandName === 'say') {
            const text = interaction.options.getString('text');
            await rcon(`say ${text}`);
            return interaction.editReply(`\u2705 Отправлено: **${text}**`);
        }

        // ── /cmd — фулл поддержка команд ──
        if (commandName === 'cmd') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const cmd = interaction.options.getString('command');
            const r = await rcon(cmd);
            return replyLong(interaction, r);
        }

        // ── /tp ──
        if (commandName === 'tp') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.options.getString('player');
            const target = interaction.options.getString('target');
            const r = await rcon(`tp ${player} ${target}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /give ──
        if (commandName === 'give') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.op)ZVRNBOT"
    R"ZVRNBOT(tions.getString('player');
            const item   = interaction.options.getString('item');
            const amount = interaction.options.getInteger('amount') || 1;
            const r = await rcon(`give ${player} ${item} ${amount}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /kick ──
        if (commandName === 'kick') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.options.getString('player');
            const reason = interaction.options.getString('reason') || '';
            const r = await rcon(`kick ${player}${reason ? ' ' + reason : ''}`);
            return replyLong(interaction, r || `\u2705 ${player} выкинут`);
        }

        // ── /gamemode ──
        if (commandName === 'gamemode') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const mode   = interaction.options.getString('mode');
            const player = interaction.options.getString('player') || '@a';
            const r = await rcon(`gamemode ${mode} ${player}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /weather ──
        if (commandName === 'weather') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const type = interaction.options.getString('type');
            const r = await rcon(`weather ${type}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /time ──
        if (commandName === 'time') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const preset = interaction.options.getString('preset');
            const r = await rcon(`time ${preset}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /difficulty ──
        if (commandName === 'difficulty') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const level = interaction.options.getString('level');
            const r = await rcon(`difficulty ${level}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /stop ──
        if (commandName === 'stop') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            await rcon('stop').catch(() => {}); // сервер закроет соединение
            return interaction.editReply('\u2705 Сервер останавливается...');
        }

        // ── /whitelist ──
        if (commandName === 'whitelist') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const action = interaction.options.getString('action');
            const player = interaction.)ZVRNBOT"
    R"ZVRNBOT(options.getString('player') || '';
            const r = await rcon(`whitelist ${action}${player ? ' ' + player : ''}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /save ──
        if (commandName === 'save') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const r = await rcon('save-all');
            return replyLong(interaction, r || '\u2705 Мир сохранён');
        }

        return interaction.editReply('\u2753 Неизвестная команда.');

    } catch (err) {
        const msg = `\u274c RCON ошибка: ${err.message}`;
        try { await interaction.editReply(msg); } catch (_) {}
    }
});

client.login(TOKEN);
} // if (TOKEN)
)ZVRNBOT";

inline const char* kBotFile_webpanel_js =
    R"ZVRNBOT(// WEBPANEL_V3 - RCON web console
// Bind address comes from .env (WEB_HOST):
//   127.0.0.1 -> this machine only (open http://127.0.0.1:WEB_PORT)
//   0.0.0.0   -> every interface, reachable from LAN / public IP (password required)
//   any other IP -> bind that interface only
// UI language comes from .env (WEB_LANG=ru|en), users can switch it in the browser.
// Remote access without opening the port: ssh -L 3000:127.0.0.1:3000 user@host
// Зависимости: express, ws (npm install)

require('dotenv').config();
const http    = require('http');
const path    = require('path');
const fs      = require('fs');
const crypto  = require('crypto');
const express = require('express');
const { WebSocketServer } = require('ws');
const { RconBridge, stripColors } = require('./rcon');

// WEBPANEL_V3: bind address is configurable (see header)
const HOST       = String(process.env.WEB_HOST || '127.0.0.1').trim() || '127.0.0.1';
const BIND_ALL   = HOST === '0.0.0.0' || HOST === '::';
const LOCAL_ONLY = HOST === '127.0.0.1' || HOST === 'localhost' || HOST === '::1';
// WEBPANEL_V3: default UI language, set by the installer (WEB_LANG=ru|en)
const DEFAULT_LANG = String(process.env.WEB_LANG || 'en').trim().toLowerCase().slice(0, 2) === 'ru' ? 'ru' : 'en';
const PORT     = Number(process.env.WEB_PORT || 3000);
const PASSWORD = process.env.WEB_PASSWORD || '';
// AUTHFORM_V1: рут-пароль — второй независимый вход (полный доступ)
const ROOT_PASSWORD  = process.env.ROOT_PASSWORD || process.env.WEB_ROOT_PASSWORD || '';
const AUTH_ENABLED   = Boolean(PASSWORD || ROOT_PASSWORD);
const SESSION_TTL_MS = Number(process.env.WEB_SESSION_TTL_MIN || 720) * 60 * 1000;
// секрет новый на каждый запуск => рестарт сервера разлогинивает всех
const SESSION_SECRET = crypto.randomBytes(32).toString('hex');
const RCON_CFG = {
    host:     process.env.RCON_HOST     || '127.0.0.1',
    port:     Number(process.env.RCON_PORT || 25575),
    password: process.env.RCON_PASSWORD || '',
    timeout:  Number(process.env.RCON_TIMEOUT_MS || 10000),
};

// MAXPLAYERS_V1: лимит игроков берём из settings.properties (ответ list его не содержит)
function readMaxPlayers() {
    // MAXPLAYERS_V2: manual override wins, then the usual locations
    const envMax = Number(process.env.WEB_MAX_PLAYERS || 0);
    if (envMax > 0) return envMax;
    const candidates = [
        path.join(__dirname, '..', 'settings.properties'),
        path.join(process.cwd(), 'settings.properties'),
        path.join(__dirname, 'settings.properties'),
        path.join(__dirname, '..', '..', 'settings.properties'),
        path.join(process.cwd(), '..', 'settings.properties'),
    ];
    for (const file of candidates) {
        try {
            const m = fs.readFileSync(file, 'utf8').match(/^[ \t]*max-players[ \t]*=[ \t]*(\d+)/m);
            if (m) return Number(m[1]);
        } catch { /* файла нет — пробуем следующий */ }
    }
    return null;
}
const MAX_PLAYERS = readMaxPlayers();

// AUTHFORM_V1 — сессии на подписанной cookie -------------------------
function signSession(role, exp) {
    const payload = role + '.' + exp;
    const sig = crypto.createHmac('sha256', SESSION_SECRET).update(payload).digest('hex');
    return payload + '.' + sig;
}

function verifySession(token) {
    if (!token) return null;
    const parts = String(token).split('.');
    if (parts.length !== 3) return null;
    const [role, exp, sig] = parts;
    const expect = crypto.createHmac('sha256', SESSION_SECRET).update(role + '.' + exp).digest('hex');
    const a = Buffer.from(sig, 'utf8');
    const b = Buffer.from(expect, 'utf8');
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
    if (!Number(exp) || Number(exp) < Date.now()) return null;
    return { role, exp: Number(exp) };
}

function parseCookies(req) {
    const out = {};
    for (const part of String(req.headers.cookie || '').split(';')) {
        const i = part.indexOf('=');
        if (i > 0) out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
    }
    return out;
}

function sessionOf(req) {
    return verifySession(parseCookies(req).rcon_session);
}

// сравнение без утечки по времени
function pwEqual(given, real) {
    const a = Buffer.from(String(given), 'utf8');
    const b = Buffer.from(String(real), 'utf8');
    if (a.length !== b.length) return false;
    return crypto.timingSafeEqual(a, b);
}

// антибрутфорс: 5 промахов -> блокировка на 60 секунд
const loginFails = new Map();
function lockedFor(ip) {
    const e = loginFails.get(ip);
    if (!e || !e.until) return 0;
    if (Date.now() >= e.until) { loginFails.delete(ip); return 0; }
    return Math.ceil((e.until - Date.now()) / 1000);
}
function noteFail(ip) {
    const e = loginFails.get(ip) || { n: 0, until: 0 };
    e.n += 1;
    if (e.n >= 5) { e.until = Date.now() + 60000; e.n = 0; }
    loginFails.set(ip, e);
}

const LOGIN_TABS = [];
if (PASSWORD)      LOGIN_TABS.push('web');
if (ROOT_PASSWORD) LOGIN_TABS.push('root');

function loginPage() {
    return `<!DOCTYPE html>
<html lang="${DEFAULT_LANG}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RCON Panel</title>
<style>
* { box-sizing:border-box; margin:0; padding:0; font-family:'Segoe UI', system-ui, -apple-system, sans-serif; }
body { background:#0f1115; color:#e4e6eb; min-height:100vh; display:flex; align-items:center; justify-content:center; padding:20px; position:relative; }
.lang-switch { position:absolute; top:20px; right:20px; background:#161920; border:1px solid #2a2f3d; color:#fff; padding:8px 14px; border-radius:8px; cursor:pointer; font-weight:700; font-size:13px; letter-spacing:.5px; transition:background .2s, transform .1s; }
.lang-switch:hover { background:#2a2f3d; }
)ZVRNBOT"
    R"ZVRNBOT(.lang-switch:active { transform:scale(.96); }
.auth-card { background:#161920; border:1px solid #2a2f3d; border-radius:16px; padding:36px 32px; width:100%; max-width:430px; box-shadow:0 12px 40px rgba(0,0,0,.6); }
.auth-card.shake { animation:shake .35s; }
@keyframes shake { 0%,100%{transform:translateX(0)} 20%{transform:translateX(-8px)} 40%{transform:translateX(8px)} 60%{transform:translateX(-5px)} 80%{transform:translateX(5px)} }
.brand { display:flex; align-items:center; gap:10px; margin-bottom:22px; }
.brand-logo { width:12px; height:12px; background:#ffaa00; border-radius:3px; box-shadow:0 0 10px rgba(255,170,0,.5); }
.brand-title { font-size:18px; font-weight:700; letter-spacing:1.5px; color:#fff; text-transform:uppercase; }
.brand-badge { margin-left:auto; background:#1c212c; border:1px solid #2a2f3d; color:#8b92a0; font-size:11px; padding:3px 10px; border-radius:20px; text-transform:uppercase; letter-spacing:.5px; }
.auth-header h1 { font-size:22px; font-weight:600; color:#fff; margin-bottom:6px; }
.auth-header p { font-size:13px; color:#8b92a0; margin-bottom:22px; line-height:1.5; }
.tabs { display:flex; gap:8px; background:#1c212c; padding:6px; border-radius:12px; margin-bottom:20px; }
.tab { flex:1; padding:10px 8px; border-radius:8px; border:none; background:transparent; color:#9ea4b0; font-size:13px; font-weight:600; cursor:pointer; transition:background .2s, color .2s; }
.tab:hover { color:#e4e6eb; }
.tab.active { background:#ffaa00; color:#000; }
.form-group { margin-bottom:18px; }
.form-label { display:block; font-size:12px; font-weight:700; text-transform:uppercase; letter-spacing:.5px; color:#b0b8c6; margin-bottom:8px; }
.input-wrapper { position:relative; display:flex; align-items:center; }
.form-control { width:100%; background:#0f1115; border:1px solid #2a2f3d; border-radius:8px; padding:12px 74px 12px 14px; color:#fff; font-size:14px; outline:none; transition:border-color .2s, box-shadow .2s; }
.form-control:focus { border-color:#ffaa00; box-shadow:0 0 0 3px rgba(255,170,0,.15); }
.form-control::placeholder { color:#4a5162; }
.toggle-password { position:absolute; right:8px; background:none; border:none; color:#6c757d; cursor:pointer; font-size:12px; font-weight:600; padding:4px 6px; transition:color .2s; }
.toggle-password:hover { color:#ffaa00; }
.caps { display:none; font-size:12px; color:#ffaa00; margin-top:8px; }
.form-actions { display:flex; align-items:center; justify-content:space-between; margin-bottom:16px; font-size:13px; }
.remember-me { display:flex; align-items:center; gap:8px; color:#8b92a0; cursor:pointer; }
.remember-me input { accent-color:#ffaa00; cursor:pointer; }
.hint { border-left:4px solid #ffaa00; background:#1c212c; border-radius:10px; padding:12px 14px; font-size:12.5px; color:#9ea4b0; line-height:1.55; margin-bottom:18px; }
.hint b { color:#fff; }
.err { color:#ff6b6b; font-size:13px; min-height:18px; margin-bottom:10px; }
.btn-submit { width:100%; background:#ffaa00; color:#000; border:none; border-radius:8px; padding:13px; font-size:14px; font-weight:700; cursor:pointer; transition:background .2s, transform .1s; }
.btn-submit:hover { background:#ffbb22; }
.btn-submit:active { transform:scale(.99); }
.btn-submit:disabled { opacity:.6; cursor:default; }
.auth-footer { margin-top:22px; text-align:center; font-size:12px; color:#6c757d; line-height:1.6; }
.auth-footer .warn { color:#ffaa00; }
/* EULA_V1 on the login page */
#eula-back { position:fixed; inset:0; background:rgba(8,10,14,.8); display:none; align-items:center;
  justify-content:center; z-index:9999; padding:24px; }
#eula-back.show { display:flex; }
#eula-box { background:#161920; border:1px solid #2a2f3d; border-radius:14px; max-width:640px; width:100%;
  max-height:86vh; overflow:auto; padding:26px 28px; box-shadow:0 22px 60px rgba(0,0,0,.55); color:#e4e6eb; }
#eula-box h2 { margin:0 0 4px; font-size:17px; }
#eula-box .eula-sub { color:#8b92a0; font-size:12.5px; margin-bottom:16px; }
#eula-box ol { margin:0 0 16px 18px; padding:0; font-size:13px; line-height:1.65; }
#eula-box li { margin-bottom:8px; }
#eula-box .eula-fine { font-size:11.5px; color:#8b92a0; line-height:1.6; margin-bottom:16px; }
#eula-box label.eula-agree { display:flex; gap:9px; align-items:flex-start; font-size:13px; margin-bottom:16px; cursor:pointer; }
#eula-actions { display:flex; gap:10px; justify-content:flex-end; flex-wrap:wrap; }
#eula-actions button { border-radius:8px; padding:9px 16px; font-size:13px; cursor:pointer; border:1px solid #2a2f3d; background:transparent; color:#8b92a0; }
#eula-actions button.primary { background:#ffaa00; border-color:#ffaa00; color:#141414; font-weight:600; }
#eula-actions button.primary:disabled { opacity:.45; cursor:not-allowed; }
</style>
</head>
<body>
<button class="lang-switch" id="langBtn" onclick="toggleLang()" aria-label="Switch language">EN</button>
<main class="auth-card" id="card">
  <div class="brand">
    <div class="brand-logo"></div>
    <span class="brand-title">RCON Panel</span>
    <span class="brand-badge" id="badge">v2</span>
  </div>
  <div class="auth-header">
    <h1 id="title">Sign in</h1>
    <p id="sub">Enter your password to open the server control panel.</p>
  </div>
  <div class="tabs" id="tabs"></div>
  <form id="form" autocomplete="off">
    <div class="form-group">
      <label class="form-label" id="pwLabel" for="pw">Password</label>
      <div class="input-wrapper">
        <input id="pw" class="form-control" type="password" placeholder="........" autocomplete="current-password" autofocus>
        <button type="button" class="toggle-password" id="eye">Show</button>
      </div>
      <div class="caps" id="caps">Caps Lock</div>
    </div>
    <div class="form-actions">
      <label class="remember-me"><input type="checkbox" id="remember" checked><span id="rememberLabel">Keep me signed in</span></label>
    </div>
    <div class="hint" id="hint"></div>
    <div class="err" id="err"></div>
    <button class="btn-submit" id="go" type="submit">Sign in</button>
)ZVRNBOT"
    R"ZVRNBOT(  </form>
  <div class="auth-footer" id="foot"></div>
  <div class="auth-footer" style="margin-top:6px"><a href="#" id="eula-link" style="color:#8b92a0;text-decoration:underline">Terms of Service</a></div>
</main>
<div id="eula-back"><div id="eula-box">
  <h2 id="eula-title"></h2>
  <div class="eula-sub" id="eula-sub"></div>
  <ol id="eula-list"></ol>
  <div class="eula-fine" id="eula-fine"></div>
  <label class="eula-agree" id="eula-agree-wrap"><input type="checkbox" id="eula-agree"><span id="eula-agree-text"></span></label>
  <div id="eula-actions">
    <button type="button" id="eula-no"></button>
    <button type="button" class="primary" id="eula-yes" disabled></button>
  </div>
</div></div>
<script>
var TABS = ${JSON.stringify(LOGIN_TABS)};
var EXPOSED = ${BIND_ALL ? 'true' : 'false'};
var BIND_HOST = ${JSON.stringify(HOST)};
var TTL_MIN = ${Math.round(SESSION_TTL_MS / 60000)};
var T = {
  en: { title:'Sign in', sub:'Enter your password to open the server control panel.',
        label:'Password', show:'Show', hide:'Hide', go:'Sign in', wait:'Checking...', lang:'RU',
        remember:'Keep me signed in', caps:'Caps Lock is on',
        tabWeb:'Web password', tabRoot:'Root password',
        hintWeb:'<b>Web password</b> - the normal panel login. Set as WEB_PASSWORD in .env.',
        hintRoot:'<b>Root password</b> - a separate login with full access. Set as WEB_ROOT_PASSWORD in .env.',
        footLocal:'Panel is bound to ' + BIND_HOST + ' - this machine only.',
        footOpen:'Panel is bound to ' + BIND_HOST + ' - reachable from the network. Use a strong password.',
        footTtl:'Session lasts ' + TTL_MIN + ' min. A server restart signs everyone out.',
        empty:'Enter the password', bad:'Wrong password', net:'Server is not responding',
        locked:function (s) { return 'Too many attempts. Try again in ' + s + ' s'; } },
  ru: { title:'Авторизация', sub:'Введите пароль, чтобы открыть панель управления сервером.',
        label:'Пароль', show:'Показать', hide:'Скрыть', go:'Войти', wait:'Проверяем...', lang:'EN',
        remember:'Запомнить меня', caps:'Включён Caps Lock',
        tabWeb:'Пароль веба', tabRoot:'Рут-пароль',
        hintWeb:'<b>Пароль веба</b> - обычный вход в панель. Задаётся в .env как WEB_PASSWORD.',
        hintRoot:'<b>Рут-пароль</b> - отдельный вход с полным доступом. Задаётся в .env как WEB_ROOT_PASSWORD.',
        footLocal:'Панель слушает ' + BIND_HOST + ' - только эта машина.',
        footOpen:'Панель слушает ' + BIND_HOST + ' - доступна из сети. Используйте надёжный пароль.',
        footTtl:'Сессия живёт ' + TTL_MIN + ' мин. Рестарт сервера разлогинивает всех.',
        empty:'Введите пароль', bad:'Неверный пароль', net:'Сервер не отвечает',
        locked:function (s) { return 'Слишком много попыток. Повторите через ' + s + ' с'; } }
};
var lang = localStorage.getItem('rcon_lang') || ${JSON.stringify(DEFAULT_LANG)};
if (!T[lang]) lang = 'en';
var mode = TABS.length ? TABS[0] : 'web';
var tabsEl = document.getElementById('tabs');
var pw = document.getElementById('pw');
var errEl = document.getElementById('err');
var go = document.getElementById('go');
var card = document.getElementById('card');
var eye = document.getElementById('eye');

function render() {
  var t = T[lang];
  document.documentElement.lang = lang;
  document.getElementById('title').textContent = t.title;
  document.getElementById('sub').textContent = t.sub;
  document.getElementById('pwLabel').textContent = t.label;
  document.getElementById('rememberLabel').textContent = t.remember;
  document.getElementById('caps').textContent = t.caps;
  document.getElementById('langBtn').textContent = t.lang;
  document.getElementById('hint').innerHTML = mode === 'root' ? t.hintRoot : t.hintWeb;
  document.getElementById('foot').innerHTML =
    (EXPOSED ? '<span class="warn">' + t.footOpen + '</span>' : t.footLocal) + '<br>' + t.footTtl;
  eye.textContent = pw.type === 'password' ? t.show : t.hide;
  go.textContent = t.go;
  tabsEl.innerHTML = '';
  tabsEl.style.display = TABS.length > 1 ? 'flex' : 'none';
  TABS.forEach(function (id) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = 'tab' + (id === mode ? ' active' : '');
    b.textContent = id === 'root' ? t.tabRoot : t.tabWeb;
    b.onclick = function () { mode = id; errEl.textContent = ''; render(); pw.focus(); };
    tabsEl.appendChild(b);
  });
}
function toggleLang() { lang = lang === 'ru' ? 'en' : 'ru'; localStorage.setItem('rcon_lang', lang); render(); }
function fail(msg) {
  errEl.textContent = msg;
  card.classList.remove('shake'); void card.offsetWidth; card.classList.add('shake');
}
eye.onclick = function () { pw.type = pw.type === 'password' ? 'text' : 'password'; render(); pw.focus(); };
pw.addEventListener('keyup', function (e) {
  var on = e.getModifierState && e.getModifierState('CapsLock');
  document.getElementById('caps').style.display = on ? 'block' : 'none';
});
document.getElementById('form').onsubmit = function (e) {
  e.preventDefault();
  var t = T[lang];
  if (!pw.value) { fail(t.empty); return; }
  go.disabled = true;
  go.textContent = t.wait;
  errEl.textContent = '';
  fetch('/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mode: mode, password: pw.value, remember: document.getElementById('remember').checked })
  }).then(function (r) { return r.json().then(function (d) { return { ok: r.ok, d: d || {} }; }); })
    .then(function (res) {
      if (res.ok && res.d.ok) { location.href = '/'; return; }
)ZVRNBOT"
    R"ZVRNBOT(      var t2 = T[lang];
      fail(res.d.error === 'locked' ? t2.locked(res.d.seconds || 60) : t2.bad);
      go.disabled = false; go.textContent = t2.go;
      pw.value = ''; pw.focus();
    })
    .catch(function () {
      errEl.textContent = T[lang].net;
      go.disabled = false; go.textContent = T[lang].go;
    });
};
render();
pw.focus();
var eulaLink = document.getElementById('eula-link');
</script>
<script>
/* EULA_V1: the soul agreement. Default language is English; RU when the panel is in RU. */
(function () {
  var L = (localStorage.getItem('rcon_lang') || (window.PANEL_DEFAULT_LANG || 'en'));
  if (L !== 'ru') L = 'en';
  var TXT = {
    en: {
      title: 'END USER SOUL AGREEMENT (v1.0)',
      sub: 'Please read carefully. Or do not. Nobody ever does.',
      list: [
        'You (the "Operator") receive a non-exclusive, revocable licence to shout commands at a Minecraft server through this panel.',
        'In exchange, the Operator assigns to RCON Panel one (1) immortal soul, gently used, sold as-is, no refunds, no warranty, no exorcisms.',
        'If the Operator has no soul, a spare of comparable quality must be supplied within 30 days. Otherwise the Operator consents to being haunted by minor rendering glitches.',
        'The soul is stored in RAM and flushed to disk on restart. Please run save-all before rebooting the server.',
        'Creepers, lava, and your friend who typed /kill @a are expressly not covered by this agreement.',
        'Support hours for soul-related claims: never o\'clock, on the second Tuesday of next week.'
      ],
      fine: 'Legal effect of this document: exactly zero. It is a joke. This panel talks only to your own server over RCON, stores nothing in the cloud, and your soul stays exactly where it was.',
      agree: 'I have read the above and hereby sell my soul (and agree that this is a joke).',
      yes: 'Sign it in blood',
      no: 'Keep my soul',
      close: 'Close',
      kept: 'Fine, keep it. The panel works anyway.'
    },
    ru: {
      title: 'СОГЛАШЕНИЕ О ПРОДАЖЕ ДУШИ (v1.0)',
      sub: 'Внимательно прочитайте. Или нет. Всё равно никто не читает.',
      list: [
        'Вы («Оператор») получаете неисключительную отзываемую лицензию кричать команды на Minecraft-сервер через эту панель.',
        'Взамен Оператор передаёт RCON Panel одну (1) бессмертную душу, б/у, в состоянии «как есть», без возврата, гарантии и экзорцизма.',
        'Если души нет, в течение 30 дней предоставляется запасная сопоставимого качества. Иначе Оператор соглашается на преследование мелкими графическими багами.',
        'Душа хранится в оперативной памяти и сбрасывается на диск при рестарте. Перед перезагрузкой сервера выполните save-all.',
        'Криперы, лава и друг, написавший /kill @a, этим соглашением не покрываются.',
        'Поддержка по вопросам души: никогда:00, во второй вторник следующей недели.'
      ],
      fine: 'Юридическая сила документа: ровно нулевая. Это шутка. Панель общается только с вашим сервером по RCON, ничего не отправляет в облако, душа остаётся при вас.',
      agree: 'Я прочитал(а) всё выше и продаю свою душу (и понимаю, что это шутка).',
      yes: 'Подписать кровью',
      no: 'Оставить душу себе',
      close: 'Закрыть',
      kept: 'Ладно, оставляйте. Панель всё равно работает.'
    }
  };
  var t = TXT[L];
  var back = document.getElementById('eula-back');
  if (!back) return;
  document.getElementById('eula-title').textContent = t.title;
  document.getElementById('eula-sub').textContent = t.sub;
  var ol = document.getElementById('eula-list');
  ol.innerHTML = '';
  t.list.forEach(function (item) {
    var li = document.createElement('li');
    li.textContent = item;
    ol.appendChild(li);
  });
  document.getElementById('eula-fine').textContent = t.fine;
  document.getElementById('eula-agree-text').textContent = t.agree;
  var yes = document.getElementById('eula-yes');
  var no = document.getElementById('eula-no');
  var box = document.getElementById('eula-agree');
  var wrap = document.getElementById('eula-agree-wrap');
  yes.textContent = t.yes;
  no.textContent = t.no;
  box.addEventListener('change', function () { yes.disabled = !box.checked; });
  function close() { back.classList.remove('show'); }
  yes.addEventListener('click', function () {
    try { localStorage.setItem('rcon_eula_v1', String(Date.now())); } catch (e) {}
    close();
  });
  no.addEventListener('click', function () {
    no.textContent = t.kept;
    setTimeout(close, 900);
  });
  window.openEula = function (readOnly) {
    if (readOnly) {
      wrap.style.display = 'none';
      yes.style.display = 'none';
      no.textContent = t.close;
    }
    back.classList.add('show');
  };
  var signed = false;
  try { signed = !!localStorage.getItem('rcon_eula_v1'); } catch (e) {}
  if (!signed && window.EULA_AUTOSHOW) back.classList.add('show');
})();
</script>
<script>
if (eulaLink) eulaLink.addEventListener('click', function (e) { e.preventDefault(); window.openEula(true); });
if (eulaLink) eulaLink.textContent = (localStorage.getItem('rcon_lang') === 'ru')
  ? 'Пользовательское соглашение' : 'Terms of Service';
)ZVRNBOT"
    R"ZVRNBOT(</script>
</body>
</html>`;
}

// ── Dashboard HTML ─────────────────────────────
// ── HTML страница ─────────────────────────────────────────────
const PAGE = `<!DOCTYPE html>
<html lang="${DEFAULT_LANG}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RCON Panel</title>
<style>
:root {
  --bg-primary:#1a1a2e; --bg-secondary:#16213e; --bg-tertiary:#0f3460;
  --accent:#e94560; --accent-hover:#ff6b81;
  --text-primary:#e0e0e0; --text-secondary:#a0a0b0;
  --border-color:#2a2a4a; --sidebar-width:230px; --topbar-height:56px;
  --ok:#3fb950; --err:#f85149; --warn:#e3b341;
}
* { box-sizing:border-box; margin:0; padding:0; }
body {
  font-family: 'Segoe UI', 'Courier New', monospace;
  background: var(--bg-primary);
  color: var(--text-primary);
  height: 100vh;
  display: flex;
  overflow: hidden;
}
#sidebar {
  width: var(--sidebar-width);
  background: var(--bg-secondary);
  border-right: 1px solid var(--border-color);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
}
#sidebar .logo {
  padding: 16px; font-size: 15px; font-weight: bold; color: var(--accent);
  letter-spacing: 1px; border-bottom: 1px solid var(--border-color);
}
#sidebar nav { flex: 1; padding: 10px 0; }
.nav-item {
  display: flex; align-items: center; gap: 10px;
  padding: 10px 18px; color: var(--text-secondary); cursor: pointer;
  font-size: 13px; border-left: 3px solid transparent;
}
.nav-item:hover { background: rgba(255,255,255,0.04); color: var(--text-primary); }
.nav-item.active { background: rgba(233,69,96,0.12); color: var(--text-primary); border-left-color: var(--accent); }
#sidebar .footer { padding: 12px 18px; font-size: 11px; color: var(--text-secondary); border-top: 1px solid var(--border-color); }
#main { flex: 1; display: flex; flex-direction: column; min-width: 0; }
#topbar {
  height: var(--topbar-height);
  background: var(--bg-secondary);
  border-bottom: 1px solid var(--border-color);
  display: flex; align-items: center; padding: 0 18px; gap: 12px; flex-shrink: 0;
}
#topbar h1 { font-size: 15px; font-weight: 600; }
#status { font-size: 12px; padding: 2px 10px; border-radius: 10px; background: var(--bg-tertiary); }
#status.ok  { color: var(--ok); }
#status.err { color: var(--err); }
.tab-panel { display: none; flex: 1; min-height: 0; flex-direction: column; overflow: auto; padding: 18px; }
.tab-panel.active { display: flex; }
.cards-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px,1fr)); gap: 14px; margin-bottom: 18px; }
.card {
  background: var(--bg-secondary); border: 1px solid var(--border-color);
  border-radius: 8px; padding: 14px 16px;
}
.card .label { font-size: 11px; color: var(--text-secondary); text-transform: uppercase; letter-spacing: .5px; }
.card .value { font-size: 22px; font-weight: 600; margin-top: 6px; }
.card .value.ok  { color: var(--ok); }
.card .value.err { color: var(--err); }
/* TOAST_V1: уведомления в правом нижнем углу */
#toasts { position: fixed; right: 18px; bottom: 18px; display: flex; flex-direction: column; gap: 10px; z-index: 9999; align-items: flex-end; }
.toast {
  background: var(--bg-secondary); border: 1px solid var(--border-color);
  border-left: 4px solid var(--ok); border-radius: 8px;
  padding: 11px 15px; font-size: 13px; color: var(--text-primary);
  box-shadow: 0 8px 24px rgba(0,0,0,.45); max-width: 380px;
  animation: toast-in .22s ease-out;
}
.toast.err { border-left-color: var(--err); }
.toast.out { animation: toast-out .25s ease-in forwards; }
.toast .t-cmd { font-family: 'Courier New', monospace; color: #58a6ff; }
@keyframes toast-in  { from { opacity: 0; transform: translateY(12px); } to { opacity: 1; transform: none; } }
@keyframes toast-out { to { opacity: 0; transform: translateY(8px); } }
.btn-logout {
  background: transparent; border: 1px solid var(--err); color: var(--err);
  border-radius: 4px; padding: 4px 12px; font-size: 12px; cursor: pointer;
}
.btn-logout:hover { background: var(--err); color: #fff; }
.quick-actions { display: flex; flex-wrap: wrap; gap: 8px; }
#console-tab.tab-panel { padding: 0; }
#output {
  flex: 1;
  overflow-y: auto;
  padding: 12px 16px;
  font-size: 13px;
  line-height: 1.55;
  font-family: 'Courier New', monospace;
}
.line { white-space: pre-wrap; word-break: break-all; padding: 1px 0; }
.line.cmd  { color: #58a6ff; }
.line.resp { color: var(--text-primary); }
.line.ok   { color: var(--ok); }
.line.err  { color: var(--err); }
.line.info { color: var(--text-secondary); }
.bar {
  background: var(--bg-secondary);
  border-top: 1px solid var(--border-color);
  padding: 8px 10px;
  display: flex;
  gap: 6px;
  flex-shrink: 0;
  flex-wrap: wrap;
}
.quick {
  background: var(--bg-tertiary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  border-radius: 4px;
  padding: 3px 9px;
  font-size: 12px;
  cursor: pointer;
  font-family: monospace;
}
.quick:hover { background: var(--accent); }
.input-row {
  background: var(--bg-secondary);
  border-top: 1px solid var(--border-color);
  padding: 8px 10px;
  display: flex;
  gap: 6px;
  flex-shrink: 0;
}
#cmd-input {
  flex: 1;
  background: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  border-radius: 4px;
  padding: 6px 10px;
  font-family: monospace;
  font-size: 13px;
}
#cmd-input:focus { outline: none; border-color: var(--accent); }
#send-btn {
  background: var(--accent);
  border: none;
  color: #fff;
  padding: 6px 16px;
  border-radius: 4px;
  cursor: pointer;
  font-family: monospace;
  font-size: 13px;
}
#send-btn:hover { background: var(--accent-hover); }
#send-btn:disabled { opacity: 0.4; cursor:not-allowed; }
.settings-row { display:flex; align-items:center; justify-content:space-between; padding:10px 0; border-bottom:1px solid var(--border-color); font-size:13px; }
)ZVRNBOT"
    R"ZVRNBOT(.settings-row .k { color: var(--text-secondary); }
/* DASH_V4: nicer cards + info blocks */
.cards-grid { grid-template-columns: repeat(auto-fit, minmax(190px,1fr)); }
.card { position: relative; overflow: hidden; transition: transform .12s ease, border-color .12s ease; }
.card:hover { transform: translateY(-2px); border-color: #3a4152; }
.card.stat::before { content:''; position:absolute; left:0; top:0; bottom:0; width:3px; background: var(--accent); opacity:.75; }
.card.stat { padding-left: 18px; }
.card .label { display:flex; align-items:center; gap:7px; }
.card .ico { font-size: 13px; opacity:.85; }
.card .sub { margin-top:6px; font-size:11.5px; color: var(--text-secondary); }
.dash-cols { display:grid; grid-template-columns: 1.15fr 1fr; gap:14px; margin-bottom:18px; }
@media (max-width: 900px) { .dash-cols { grid-template-columns: 1fr; } }
.player-list { display:flex; flex-wrap:wrap; gap:8px; margin-top:10px; min-height:34px; align-items:flex-start; }
.player-chip { background: var(--bg-primary); border:1px solid var(--border-color); border-radius:999px;
  padding:5px 12px; font-size:12.5px; color: var(--text-primary); display:flex; align-items:center; gap:7px; }
.player-chip::before { content:''; width:7px; height:7px; border-radius:50%; background:#4caf50; }
.player-empty { color: var(--text-secondary); font-size:12.5px; margin-top:10px; }
.info-row { display:flex; justify-content:space-between; gap:12px; padding:8px 0; border-bottom:1px dashed var(--border-color); font-size:12.5px; }
.info-row:last-child { border-bottom:none; }
.info-row .k { color: var(--text-secondary); }
.info-row .v { color: var(--text-primary); font-family: monospace; text-align:right; word-break:break-all; }
/* EULA_V1 */
#eula-back { position:fixed; inset:0; background:rgba(8,10,14,.78); backdrop-filter:blur(3px);
  display:none; align-items:center; justify-content:center; z-index:9999; padding:24px; }
#eula-back.show { display:flex; }
#eula-box { background:var(--bg-secondary); border:1px solid var(--border-color); border-radius:14px;
  max-width:640px; width:100%; max-height:86vh; overflow:auto; padding:26px 28px; box-shadow:0 22px 60px rgba(0,0,0,.55); }
#eula-box h2 { margin:0 0 4px; font-size:17px; letter-spacing:.02em; }
#eula-box .eula-sub { color:var(--text-secondary); font-size:12.5px; margin-bottom:16px; }
#eula-box ol { margin:0 0 16px 18px; padding:0; font-size:13px; line-height:1.65; color:var(--text-primary); }
#eula-box li { margin-bottom:8px; }
#eula-box .eula-fine { font-size:11.5px; color:var(--text-secondary); line-height:1.6; margin-bottom:16px; }
#eula-box label.eula-agree { display:flex; gap:9px; align-items:flex-start; font-size:13px; margin-bottom:16px; cursor:pointer; }
#eula-actions { display:flex; gap:10px; justify-content:flex-end; flex-wrap:wrap; }
#eula-actions button { border-radius:8px; padding:9px 16px; font-size:13px; cursor:pointer; border:1px solid var(--border-color); background:transparent; color:var(--text-secondary); }
#eula-actions button.primary { background:var(--accent); border-color:var(--accent); color:#fff; }
#eula-actions button.primary:disabled { opacity:.45; cursor:not-allowed; }
</style>
</head>
<body>
<div id="sidebar">
  <div class="logo">⬡ RCON PANEL</div>
  <nav>
    <div class="nav-item active" data-tab="dashboard-tab">📊 Дашборд</div>
    <div class="nav-item" data-tab="console-tab">💻 Консоль</div>
    <div class="nav-item" data-tab="settings-tab">⚙️ Настройки</div>
  </nav>
  <div class="footer">WEBPANEL_V3 &bull; ${HOST}:${PORT}</div>
</div>
<div id="main">
  <div id="topbar">
    <h1 id="page-title">Дашборд</h1>
    <span style="flex:1"></span>
    <span id="status" class="err">disconnected</span>
  </div>

  <div class="tab-panel active" id="dashboard-tab">
    <div class="cards-grid">
      <div class="card stat"><div class="label"><span class="ico">🔌</span>RCON</div><div class="value err" id="card-rcon">Отключено</div><div class="sub" id="sub-rcon">${RCON_CFG.host}:${RCON_CFG.port}</div></div>
      <div class="card stat"><div class="label"><span class="ico">🎮</span>Игроки онлайн</div><div class="value" id="card-players">${MAX_PLAYERS !== null ? '0 / ' + MAX_PLAYERS : '0 / ?'}</div><div class="sub" id="sub-players">Свободных слотов: —</div></div>
      <div class="card stat"><div class="label"><span class="ico">🛡️</span>Whitelist</div><div class="value" id="card-whitelist">—</div><div class="sub" id="sub-whitelist">Записей: —</div></div>
      <div class="card stat"><div class="label"><span class="ico">📡</span>Пинг WS</div><div class="value" id="card-ping">—</div><div class="sub" id="sub-ping">Обновлено: —</div></div>
    </div>
    <div class="dash-cols">
      <div class="card">
        <div class="label"><span class="ico">👥</span>Игроки на сервере</div>
        <div class="player-list" id="player-list"></div>
        <div class="player-empty" id="player-empty">Сейчас никого нет онлайн</div>
      </div>
      <div class="card">
        <div class="label" style="margin-bottom:6px"><span class="ico">ℹ️</span>Информация</div>
        <div class="info-row"><span class="k">Адрес RCON</span><span class="v">${RCON_CFG.host}:${RCON_CFG.port}</span></div>
        <div class="info-row"><span class="k">Адрес панели</span><span class="v">${HOST}:${PORT}</span></div>
        <div class="info-row"><span class="k">Макс. игроков</span><span class="v" id="info-max">${MAX_PLAYERS !== null ? MAX_PLAYERS : '?'}</span></div>
        <div class="info-row"><span class="k">Аптайм панели</span><span class="v" id="info-uptime">—</span></div>
        <div class="info-row"><span class="k">Версия Node</span><span class="v" id="info-node">—</span></div>
        <div class="info-row"><span class="k">Доступ</span><span class="v">${BIND_ALL ? 'любой IP (0.0.0.0)' : (LOCAL_ONLY ? 'только эта машина' : HOST)}</span></div>
)ZVRNBOT"
    R"ZVRNBOT(      </div>
    </div>
    <div class="card">
      <div class="label" style="margin-bottom:10px">Быстрые действия</div>
      <div class="quick-actions">
        <button class="quick" data-cmd="list">list</button>
        <button class="quick" data-cmd="save-all">save-all</button>
        <button class="quick" data-cmd="whitelist list">wl list</button>
        <button class="quick" data-cmd="whitelist on">wl on</button>
        <button class="quick" data-cmd="whitelist off">wl off</button>
        <button class="quick" data-cmd="time set day">day</button>
        <button class="quick" data-cmd="weather clear">clear</button>
        <button class="quick" data-cmd="difficulty 2">normal</button>
        <button class="quick" data-cmd="difficulty 3">hard</button>
        <button class="quick" data-cmd="help">help</button>
      </div>
    </div>
  </div>

  <div class="tab-panel" id="console-tab">
    <div id="output"><div class="line info">Connecting to RCON...</div></div>
    <div class="bar">
      <button class="quick" data-cmd="list">list</button>
      <button class="quick" data-cmd="save-all">save-all</button>
      <button class="quick" data-cmd="whitelist list">wl list</button>
      <button class="quick" data-cmd="whitelist on">wl on</button>
      <button class="quick" data-cmd="whitelist off">wl off</button>
      <button class="quick" data-cmd="time set day">day</button>
      <button class="quick" data-cmd="weather clear">clear</button>
      <button class="quick" data-cmd="difficulty 2">normal</button>
      <button class="quick" data-cmd="difficulty 3">hard</button>
      <button class="quick" data-cmd="help">help</button>
    </div>
    <div class="input-row">
      <input id="cmd-input" type="text" placeholder="Введите команду (Enter)" autocomplete="off">
      <button id="send-btn" disabled>Send</button>
    </div>
  </div>

  <div class="tab-panel" id="settings-tab">
    <div class="card">
      <div class="settings-row"><span class="k">RCON адрес</span><span id="set-rcon-addr">${RCON_CFG.host}:${RCON_CFG.port}</span></div>
      <div class="settings-row"><span class="k">Защита паролем</span><span id="set-pass">${AUTH_ENABLED ? (PASSWORD && ROOT_PASSWORD ? 'Включена (пароль веба + рут)' : PASSWORD ? 'Включена (пароль веба)' : 'Включена (только рут-пароль)') : 'Отключена (WEB_PASSWORD и WEB_ROOT_PASSWORD не заданы)'}</span></div>
      <div class="settings-row"><span class="k">Макс. игроков</span><span>${MAX_PLAYERS !== null ? MAX_PLAYERS : 'не найдено в settings.properties'}</span></div>${AUTH_ENABLED ? '\n      <div class="settings-row"><span class="k">Сессия</span><span class="quick-actions"><button class="btn-logout" id="logout-btn">Выйти</button></span></div>' : ''}
      <div class="settings-row"><span class="k">Соглашение</span><span class="quick-actions"><button class="quick" id="eula-open">Открыть</button></span></div>
      <div class="settings-row"><span class="k">Доступ</span><span>${BIND_ALL ? 'любой IP (0.0.0.0)' : (LOCAL_ONLY ? 'только эта машина' : HOST)}</span></div>
      <div class="settings-row" style="border-bottom:none"><span class="k">Whitelist</span>
        <span class="quick-actions">
          <button class="quick" data-cmd="whitelist on">Включить</button>
          <button class="quick" data-cmd="whitelist off">Выключить</button>
          <button class="quick" data-cmd="whitelist list">Список</button>
        </span>
      </div>
    </div>
  </div>
</div>
<div id="toasts"></div>
<script>
const out    = document.getElementById('output');
const inp    = document.getElementById('cmd-input');
const btn    = document.getElementById('send-btn');
const stEl   = document.getElementById('status');
const hist   = [];
let histIdx  = -1;
let ws;
let MAX_HINT  = ${MAX_PLAYERS !== null ? MAX_PLAYERS : 'null'};  // MAXPLAYERS_V1
const pending = [];      // TOAST_V1: очередь команд для сопоставления с ответом
let pingTimer = null;    // PINGWS_V1
let pingSent  = 0;

function log(text, cls='resp') {
  const d = document.createElement('div');
  d.className = 'line ' + cls;
  d.textContent = text;
  out.appendChild(d);
  out.scrollTop = out.scrollHeight;
}

// TOAST_V1
const toastBox = document.getElementById('toasts');
function esc(t) {
  return String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
function toast(html, cls = 'ok', ttl = 2600) {
  const d = document.createElement('div');
  d.className = 'toast ' + cls;
  d.innerHTML = html;
  toastBox.appendChild(d);
  while (toastBox.children.length > 4) toastBox.removeChild(toastBox.firstChild);
  setTimeout(() => {
    d.classList.add('out');
    setTimeout(() => d.remove(), 260);
  }, ttl);
}

// PINGWS_V1: пинг через WebSocket — без HTTP и без RCON, чистый RTT
function sendPing() {
  if (!ws || ws.readyState !== 1) return;
  pingSent = performance.now();
  ws.send(JSON.stringify({ type: 'ping', t: pingSent }));
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto + '//' + location.host + '/ws');
  ws.onopen = () => {
    stEl.textContent = 'connected';
    stEl.className   = 'ok';
    btn.disabled = false;
    log('WebSocket connected', 'ok');
    setDashRcon(true);
    toast('\u0421\u0432\u044f\u0437\u044c \u0441 RCON \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0430', 'ok');
    sendPing();
    if (!pingTimer) pingTimer = setInterval(sendPing, 5000);
    pollStatus();   // QUIET_STATUS_V2: fill the cards at once, do not wait a minute
  };
  ws.onmessage = e => {
    const msg = JSON.parse(e.data);
    if (msg.type === 'pong') {                     // PINGWS_V1
      const rtt = performance.now() - (msg.t || pingSent);
)ZVRNBOT"
    R"ZVRNBOT(      cardPing.textContent = (rtt < 10 ? rtt.toFixed(1) : Math.round(rtt)) + ' ms';
      return;
    }
    if (msg.type === 'resp')  {
      log(msg.text || '(empty)', 'resp');
      handleDashResp(msg.text);
      const c = pending.shift();
      if (c) toast('\u041a\u043e\u043c\u0430\u043d\u0434\u0430 <span class="t-cmd">' + esc(c) + '</span> \u0432\u044b\u043f\u043e\u043b\u043d\u0435\u043d\u0430', 'ok');
    }
    if (msg.type === 'error') {
      log('ERR: ' + msg.text, 'err');
      const c = pending.shift();
      toast((c ? '<span class="t-cmd">' + esc(c) + '</span>: ' : '') + esc(msg.text), 'err', 4200);
    }
    if (msg.type === 'info')  log(msg.text, 'info');
  };
  ws.onerror = () => {};
  ws.onclose = () => {
    stEl.textContent = 'disconnected';
    stEl.className   = 'err';
    btn.disabled = true;
    log('Connection lost. Reconnecting in 3s...', 'err');
    setDashRcon(false);
    pending.length = 0;
    if (pingTimer) { clearInterval(pingTimer); pingTimer = null; }
    cardPing.textContent = '\u2014';
    toast('\u0421\u0432\u044f\u0437\u044c \u043f\u043e\u0442\u0435\u0440\u044f\u043d\u0430, \u043f\u0435\u0440\u0435\u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u0447\u0435\u0440\u0435\u0437 3 \u0441', 'err', 3000);
    setTimeout(connect, 3000);
  };
}

function send(cmdOverride) {
  const cmd = (cmdOverride !== undefined ? cmdOverride : inp.value).trim();
  if (!cmd || !ws || ws.readyState !== 1) return;
  log('> ' + cmd, 'cmd');
  if (cmdOverride === undefined) {
    hist.unshift(cmd);
    if (hist.length > 50) hist.pop();
    histIdx = -1;
    inp.value = '';
  }
  pending.push(cmd);            // TOAST_V1
  ws.send(JSON.stringify({ cmd }));
}

inp.addEventListener('keydown', e => {
  if (e.key === 'Enter') { send(); return; }
  if (e.key === 'ArrowUp')   { histIdx = Math.min(histIdx+1, hist.length-1); inp.value = hist[histIdx]||''; e.preventDefault(); }
  if (e.key === 'ArrowDown') { histIdx = Math.max(histIdx-1, -1);             inp.value = histIdx>=0 ? hist[histIdx] : ''; e.preventDefault(); }
});
btn.addEventListener('click', () => send());
const logoutBtn = document.getElementById('logout-btn');   // AUTHFORM_V1
if (logoutBtn) logoutBtn.addEventListener('click', () => { location.href = '/logout'; });
document.querySelectorAll('.quick').forEach(b =>
  b.addEventListener('click', () => { send(b.dataset.cmd); })
);

// WEBPANEL_V2: переключение вкладок
 const pageTitle = document.getElementById('page-title');
const navTitles = { 'dashboard-tab': 'Дашборд', 'console-tab': 'Консоль', 'settings-tab': 'Настройки' };
document.querySelectorAll('.nav-item').forEach(item => {
  item.addEventListener('click', () => {
    document.querySelectorAll('.nav-item').forEach(i => i.classList.remove('active'));
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    item.classList.add('active');
    const tab = item.dataset.tab;
    document.getElementById(tab).classList.add('active');
    pageTitle.textContent = navTitles[tab] || '';
    refreshOnTab(tab);   // QUIET_STATUS_V2: dashboard refresh on tab switch
    if (tab === 'console-tab') inp.focus();
  });
});

// WEBPANEL_V2: карточки дашборда
const cardRcon      = document.getElementById('card-rcon');
const cardPlayers   = document.getElementById('card-players');
const cardWhitelist = document.getElementById('card-whitelist');
const cardPing      = document.getElementById('card-ping');

function refreshOnTab(tabId) { if (tabId === 'dashboard-tab') pollStatus(); }
// DASH_V4: extra dashboard widgets
var subPlayers = document.getElementById('sub-players');
var subWl      = document.getElementById('sub-whitelist');
var subPing    = document.getElementById('sub-ping');
var infoUptime = document.getElementById('info-uptime');
var infoNode   = document.getElementById('info-node');
var infoMax    = document.getElementById('info-max');
var playerList = document.getElementById('player-list');
var playerEmpty= document.getElementById('player-empty');
function fmtUptime(sec) {
  sec = Math.max(0, Math.floor(sec || 0));
  var d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
  if (d) return d + 'd ' + h + 'h ' + m + 'm';
  if (h) return h + 'h ' + m + 'm';
  return m + 'm ' + (sec % 60) + 's';
}
function renderPlayers(names) {
  if (!playerList) return;
  playerList.innerHTML = '';
  var has = names && names.length;
  if (playerEmpty) playerEmpty.style.display = has ? 'none' : 'block';
  if (!has) return;
  names.forEach(function (n) {
    var chip = document.createElement('span');
    chip.className = 'player-chip';
    chip.textContent = n;
    playerList.appendChild(chip);
  });
}
function setDashRcon(ok) {
  cardRcon.textContent = ok ? 'Подключено' : 'Отключено';
  cardRcon.className = 'value ' + (ok ? 'ok' : 'err');
}

function handleDashResp(text) {
  if (!text) return;
  // ответ на "list": "There are N of a max of M players online: ..."
  const m = text.match(/(\\d+)\\s+of\\s+a\\s+max\\s+of\\s+(\\d+)/i) || text.match(/\u0418\u0433\u0440\u043e\u043a\u043e\u0432\\s*\\((\\d+)\\/(\\d+)\\)/i);  // PANELREGEX_V1
  if (m) { cardPlayers.textContent = m[1] + ' / ' + m[2]; MAX_HINT = m[2]; }
  // MAXPLAYERS_V1: the server answers "Players (0): none" — лимит берём из settings.properties
  const m2 = text.match(/(?:Players|\u0418\u0433\u0440\u043e\u043a\u043e\u0432)\\s*\\((\\d+)\\)/i);
  if (!m && m2) cardPlayers.textContent = m2[1] + ' / ' + (MAX_HINT !== null ? MAX_HINT : '?');
  if (/whitelist\s+(is\s+)?on|\u0432\u043a\u043b\u044e\u0447\u0451\u043d/i.test(text)) cardWhitelist.textContent = 'Включён';
  if (/whitelist\s+(is\s+)?off|\u0432\u044b\u043a\u043b\u044e\u0447\u0451\u043d/i.test(text)) cardWhitelist.textContent = 'Выключён';
}

// QUIET_STATUS_V2: status refresh only while the dashboard is actually visible.
var POLL_MS = 60000;
)ZVRNBOT"
    R"ZVRNBOT(function dashVisible() {
  var tab = document.getElementById('dashboard-tab');
  return !document.hidden && tab && tab.classList.contains('active');
}
function pollStatus() {
  if (!dashVisible()) return;
  if (ws && ws.readyState === 1) {
    fetch('/api/status').then(r => r.json()).then(d => {
      if (d.players) {
        cardPlayers.textContent = d.players;
        const parts = String(d.players).split('/');
        if (parts.length > 1) MAX_HINT = parts[1].trim();
      }
      if (d.whitelist !== undefined && d.whitelist !== null)
        cardWhitelist.textContent = d.whitelist ? 'Включён' : 'Выключён';
      // DASH_V4: extra info
      if (subWl && d.wlCount !== undefined && d.wlCount !== null) subWl.textContent = 'Записей: ' + d.wlCount;
      if (subPlayers && d.count !== undefined && d.count !== null && d.max)
        subPlayers.textContent = 'Свободных слотов: ' + Math.max(0, d.max - d.count);
      if (infoMax && d.max) infoMax.textContent = d.max;
      if (infoUptime && d.uptime !== undefined) infoUptime.textContent = fmtUptime(d.uptime);
      if (infoNode && d.node) infoNode.textContent = d.node;
      if (subPing) subPing.textContent = 'Обновлено: ' + new Date().toLocaleTimeString();
      renderPlayers(d.list || []);
    }).catch(() => {});
  }
}
setInterval(pollStatus, POLL_MS);
document.addEventListener('visibilitychange', function () { if (!document.hidden) pollStatus(); });

connect();
pollStatus();
var PANEL_DEFAULT_LANG = ${JSON.stringify(DEFAULT_LANG)};
</script>
<script>
// WEBPANEL_V3: ru/en switch for the dashboard (server default comes from WEB_LANG)
(function () {
  var DICT = {
    'Дашборд':'Dashboard','📊 Дашборд':'📊 Dashboard','💻 Консоль':'💻 Console','⚙️ Настройки':'⚙️ Settings',
    'Консоль':'Console','Настройки':'Settings','Быстрые действия':'Quick actions',
    'Игроки онлайн':'Players online','Пинг WS':'WS ping','Подключено':'Connected','Отключено':'Disconnected',
    'Включён':'On','Выключён':'Off','Включить':'Enable','Выключить':'Disable','Список':'List','Выйти':'Sign out','Сессия':'Session',
    'Доступ':'Access','Игроки на сервере':'Players on the server','Информация':'Information',
    'Сейчас никого нет онлайн':'Nobody is online right now','Адрес RCON':'RCON address',
    'Адрес панели':'Panel address','Аптайм панели':'Panel uptime','Версия Node':'Node version',
    'Соглашение':'Agreement','Открыть':'Open','Защита паролем':'Password protection','Макс. игроков':'Max players','RCON адрес':'RCON address',
    'не найдено в settings.properties':'not found in settings.properties',
    'только эта машина':'this machine only','любой IP (0.0.0.0)':'any IP (0.0.0.0)',
    'Включена (пароль веба + рут)':'Enabled (web + root password)',
    'Включена (пароль веба)':'Enabled (web password)',
    'Включена (только рут-пароль)':'Enabled (root password only)',
    'Отключена (WEB_PASSWORD и WEB_ROOT_PASSWORD не заданы)':'Disabled (WEB_PASSWORD and WEB_ROOT_PASSWORD are empty)'
  };
  var lang = localStorage.getItem('rcon_lang') || PANEL_DEFAULT_LANG;
  if (lang !== 'en' && lang !== 'ru') lang = 'en';
  function walk(node) {
    if (node.nodeType === 3) {
      var k = node.nodeValue.trim();
      if (k && DICT[k]) node.nodeValue = node.nodeValue.replace(k, DICT[k]);
      return;
    }
    if (node.nodeType !== 1 || node.tagName === 'SCRIPT' || node.tagName === 'STYLE') return;
    for (var i = 0; i < node.childNodes.length; i++) walk(node.childNodes[i]);
  }
  function apply() { if (lang === 'en') walk(document.body); }
  var btn = document.createElement('button');
  btn.id = 'lang-btn';
  btn.className = 'quick';
  btn.textContent = lang === 'en' ? 'RU' : 'EN';
  btn.title = 'Language';
  btn.onclick = function () {
    localStorage.setItem('rcon_lang', lang === 'en' ? 'ru' : 'en');
    location.reload();
  };
  var bar = document.getElementById('topbar');
  if (bar) bar.appendChild(btn);
  apply();
  new MutationObserver(function (muts) {
    if (lang !== 'en') return;
    muts.forEach(function (m) {
      if (m.type === 'characterData') walk(m.target);
      else m.addedNodes.forEach(function (n) { walk(n); });
    });
  }).observe(document.body, { childList: true, subtree: true, characterData: true });
})();
</script>
<div id="eula-back"><div id="eula-box">
  <h2 id="eula-title"></h2>
  <div class="eula-sub" id="eula-sub"></div>
  <ol id="eula-list"></ol>
  <div class="eula-fine" id="eula-fine"></div>
  <label class="eula-agree" id="eula-agree-wrap"><input type="checkbox" id="eula-agree"><span id="eula-agree-text"></span></label>
  <div id="eula-actions">
    <button type="button" id="eula-no"></button>
    <button type="button" class="primary" id="eula-yes" disabled></button>
  </div>
</div></div>
<script>window.EULA_AUTOSHOW = true;</script>
<script>
/* EULA_V1: the soul agreement. Default language is English; RU when the panel is in RU. */
(function () {
  var L = (localStorage.getItem('rcon_lang') || (window.PANEL_DEFAULT_LANG || 'en'));
  if (L !== 'ru') L = 'en';
  var TXT = {
    en: {
      title: 'END USER SOUL AGREEMENT (v1.0)',
      sub: 'Please read carefully. Or do not. Nobody ever does.',
      list: [
        'You (the "Operator") receive a non-exclusive, revocable licence to shout commands at a Minecraft server through this panel.',
        'In exchange, the Operator assigns to RCON Panel one (1) immortal soul, gently used, sold as-is, no refunds, no warranty, no exorcisms.',
        'If the Operator has no soul, a spare of comparable quality must be supplied within 30 days. Otherwise the Operator consents to being haunted by minor rendering glitches.',
)ZVRNBOT"
    R"ZVRNBOT(        'The soul is stored in RAM and flushed to disk on restart. Please run save-all before rebooting the server.',
        'Creepers, lava, and your friend who typed /kill @a are expressly not covered by this agreement.',
        'Support hours for soul-related claims: never o\'clock, on the second Tuesday of next week.'
      ],
      fine: 'Legal effect of this document: exactly zero. It is a joke. This panel talks only to your own server over RCON, stores nothing in the cloud, and your soul stays exactly where it was.',
      agree: 'I have read the above and hereby sell my soul (and agree that this is a joke).',
      yes: 'Sign it in blood',
      no: 'Keep my soul',
      close: 'Close',
      kept: 'Fine, keep it. The panel works anyway.'
    },
    ru: {
      title: 'СОГЛАШЕНИЕ О ПРОДАЖЕ ДУШИ (v1.0)',
      sub: 'Внимательно прочитайте. Или нет. Всё равно никто не читает.',
      list: [
        'Вы («Оператор») получаете неисключительную отзываемую лицензию кричать команды на Minecraft-сервер через эту панель.',
        'Взамен Оператор передаёт RCON Panel одну (1) бессмертную душу, б/у, в состоянии «как есть», без возврата, гарантии и экзорцизма.',
        'Если души нет, в течение 30 дней предоставляется запасная сопоставимого качества. Иначе Оператор соглашается на преследование мелкими графическими багами.',
        'Душа хранится в оперативной памяти и сбрасывается на диск при рестарте. Перед перезагрузкой сервера выполните save-all.',
        'Криперы, лава и друг, написавший /kill @a, этим соглашением не покрываются.',
        'Поддержка по вопросам души: никогда:00, во второй вторник следующей недели.'
      ],
      fine: 'Юридическая сила документа: ровно нулевая. Это шутка. Панель общается только с вашим сервером по RCON, ничего не отправляет в облако, душа остаётся при вас.',
      agree: 'Я прочитал(а) всё выше и продаю свою душу (и понимаю, что это шутка).',
      yes: 'Подписать кровью',
      no: 'Оставить душу себе',
      close: 'Закрыть',
      kept: 'Ладно, оставляйте. Панель всё равно работает.'
    }
  };
  var t = TXT[L];
  var back = document.getElementById('eula-back');
  if (!back) return;
  document.getElementById('eula-title').textContent = t.title;
  document.getElementById('eula-sub').textContent = t.sub;
  var ol = document.getElementById('eula-list');
  ol.innerHTML = '';
  t.list.forEach(function (item) {
    var li = document.createElement('li');
    li.textContent = item;
    ol.appendChild(li);
  });
  document.getElementById('eula-fine').textContent = t.fine;
  document.getElementById('eula-agree-text').textContent = t.agree;
  var yes = document.getElementById('eula-yes');
  var no = document.getElementById('eula-no');
  var box = document.getElementById('eula-agree');
  var wrap = document.getElementById('eula-agree-wrap');
  yes.textContent = t.yes;
  no.textContent = t.no;
  box.addEventListener('change', function () { yes.disabled = !box.checked; });
  function close() { back.classList.remove('show'); }
  yes.addEventListener('click', function () {
    try { localStorage.setItem('rcon_eula_v1', String(Date.now())); } catch (e) {}
    close();
  });
  no.addEventListener('click', function () {
    no.textContent = t.kept;
    setTimeout(close, 900);
  });
  window.openEula = function (readOnly) {
    if (readOnly) {
      wrap.style.display = 'none';
      yes.style.display = 'none';
      no.textContent = t.close;
    }
    back.classList.add('show');
  };
  var signed = false;
  try { signed = !!localStorage.getItem('rcon_eula_v1'); } catch (e) {}
  if (!signed && window.EULA_AUTOSHOW) back.classList.add('show');
})();
</script>
<script>
var eulaOpenBtn = document.getElementById('eula-open');
if (eulaOpenBtn) eulaOpenBtn.addEventListener('click', function () { window.openEula(true); });
</script>
</body>
</html>`;

// ── Express app ───────────────────────────────────────────────
const app = express();

// WEBPANEL_V3: when bound to 127.0.0.1 keep the strict localhost guard,
// when bound to 0.0.0.0 (or a LAN IP) allow remote clients - the login form protects the panel.
const LOCAL_IPS = ['127.0.0.1', '::1', '::ffff:127.0.0.1'];
function isLocalReq(ip) { return LOCAL_IPS.indexOf(String(ip).replace(/^::ffff:/, '::ffff:')) !== -1; }
app.use((req, res, next) => {
    const ip = req.socket.remoteAddress || '';
    if (LOCAL_ONLY && !isLocalReq(ip)) {
        res.status(403).send('Forbidden: localhost only (set WEB_HOST=0.0.0.0 in .env to allow remote access)');
        console.warn(`[WebPanel] Blocked request from ${ip}`);
        return;
    }
    next();
});

// AUTHFORM_V1: вместо Basic Auth — форма входа с вкладками "пароль веба" / "рут-пароль"
app.use(express.json({ limit: '16kb' }));

if (AUTH_ENABLED) {
    app.get('/login', (req, res) => {
        if (sessionOf(req)) return res.redirect('/');
        res.type('html').send(loginPage());
    });

    app.get('/logout', (req, res) => {
        res.setHeader('Set-Cookie', 'rcon_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0');
)ZVRNBOT"
    R"ZVRNBOT(        res.redirect('/login');
    });

    app.post('/login', (req, res) => {
        const ip = req.socket.remoteAddress || '';
        const wait = lockedFor(ip);
        if (wait) return res.status(429).json({ error: 'locked', seconds: wait });

        const mode  = String((req.body && req.body.mode) || '');
        const given = String((req.body && req.body.password) || '');
        const real  = mode === 'root' ? ROOT_PASSWORD : PASSWORD;

        if (!real || !given || !pwEqual(given, real)) {
            noteFail(ip);
            console.warn(`[WebPanel] Failed login (${mode || 'web'}) from ${ip}`);
            return res.status(401).json({ error: 'bad' });
        }

        loginFails.delete(ip);
        // AUTHFORM_V2: "keep me signed in" -> persistent cookie, otherwise a browser-session cookie
        const remember = Boolean(req.body && req.body.remember);
        const exp = Date.now() + SESSION_TTL_MS;
        const token = signSession(mode === 'root' ? 'root' : 'web', exp);
        const secure = req.socket.encrypted ? ' Secure;' : '';
        res.setHeader('Set-Cookie',
            `rcon_session=${encodeURIComponent(token)}; HttpOnly;${secure} SameSite=Strict; Path=/`
            + (remember ? `; Max-Age=${Math.floor(SESSION_TTL_MS / 1000)}` : ''));
        console.log(`[WebPanel] Login ok (${mode === 'root' ? 'root' : 'web'}) from ${ip}`);
        res.json({ ok: true });
    });

    // всё остальное требует сессию
    app.use((req, res, next) => {
        if (sessionOf(req)) return next();
        if (req.path.startsWith('/api/')) return res.status(401).json({ error: 'auth required' });
        res.redirect('/login');
    });

    console.log('[WebPanel] Password protection enabled'
        + (PASSWORD ? ' [web]' : '') + (ROOT_PASSWORD ? ' [root]' : ''));
}

app.get('/', (_req, res) => res.send(PAGE));

// WEBPANEL_V2: лёгкий JSON-статус для карточек дашборда (защищён IP-guard выше)
// QUIET_STATUS_V1: один постоянный RCON-мост + кэш вместо нового коннекта каждые 10 с
let statusBridge = null;
let statusCache = { at: 0, data: null };
let statusInFlight = null;
// QUIET_STATUS_V2: one 'list' per refresh at most, 'whitelist list' rarely.
// Tune from .env if you want: WEB_STATUS_TTL_SEC / WEB_WHITELIST_TTL_SEC.
const STATUS_TTL_MS = Math.max(10, Number(process.env.WEB_STATUS_TTL_SEC || 55)) * 1000;
const WL_TTL_MS     = Math.max(30, Number(process.env.WEB_WHITELIST_TTL_SEC || 600)) * 1000;
let wlCache = { at: 0, value: null, count: null };

async function fetchStatus() {
    const st = { players: null, whitelist: null, count: null, max: MAX_PLAYERS, list: [], wlCount: null };
    if (!statusBridge) statusBridge = new RconBridge(RCON_CFG);
    try {
        const listResp = stripColors(String(await statusBridge.send('list') || ''));
        let m = listResp.match(/(\d+)\s+of\s+a\s+max\s+of\s+(\d+)/i)
             || listResp.match(/\u0418\u0433\u0440\u043e\u043a\u043e\u0432\s*\((\d+)\/(\d+)\)/i);
        if (m) {
            st.players = `${m[1]} / ${m[2]}`;
            st.count = Number(m[1]);
            st.max = Number(m[2]);
        } else {
            // MAXPLAYERS_V1: ответ вида "Players (0): none" — лимит из settings.properties
            m = listResp.match(/(?:Players|\u0418\u0433\u0440\u043e\u043a\u043e\u0432)\s*\((\d+)\)/i);
            if (m) {
                st.players = `${m[1]} / ${MAX_PLAYERS !== null ? MAX_PLAYERS : '?'}`;
                st.count = Number(m[1]);
            }
        }
        // DASH_V4: player names for the dashboard list
        const namesPart = listResp.split(/:\s*/).slice(1).join(': ').trim();
        if (namesPart && !/^(none|\u043d\u0435\u0442)$/i.test(namesPart)) {
            st.list = namesPart.split(/\s*,\s*/).map(s => s.trim()).filter(Boolean).slice(0, 60);
        }
        if (st.count === null && st.list.length) st.count = st.list.length;
        // QUIET_STATUS_V2: the whitelist almost never changes on its own, so it is
        // polled once every WL_TTL_MS instead of on every status refresh.
        if (Date.now() - wlCache.at < WL_TTL_MS) {
            st.whitelist = wlCache.value;
            st.wlCount = wlCache.count !== undefined ? wlCache.count : null;
        } else {
            const wlResp = stripColors(String(await statusBridge.send('whitelist list') || ''));
            st.whitelist = /on\b/i.test(wlResp) || /\u0432\u043a\u043b\u044e\u0447/i.test(wlResp) ? true
                : /off\b/i.test(wlResp) || /\u0432\u044b\u043a\u043b\u044e\u0447/i.test(wlResp) ? false : null;
            const wlNum = wlResp.match(/entries\s*=\s*(\d+)/i) || wlResp.match(/(\d+)\s+whitelisted/i);
            st.wlCount = wlNum ? Number(wlNum[1]) : (/\bnone\b/i.test(wlResp) ? 0 : null);
            wlCache = { at: Date.now(), value: st.whitelist, count: st.wlCount };
        }
        return st;
    } catch (err) {
        // мост отвалился — закроем его, следующий опрос создаст новый
        const dead = statusBridge;
        statusBridge = null;
        if (dead) await dead.close().catch(() => {});
        throw err;
    }
}

// DASH_V4: runtime info that costs no RCON call at all
function runtimeInfo() {
    return { uptime: Math.floor(process.uptime()), node: process.version, host: HOST, port: PORT,
             rcon: `${RCON_CFG.host}:${RCON_CFG.port}`, maxPlayers: MAX_PLAYERS };
}

app.get('/api/status', async (_req, res) => {
    if (statusCache.data && Date.now() - statusCache.at < STATUS_TTL_MS) {
        return res.json(Object.assign({}, statusCache.data, runtimeInfo()));
    }
    if (!statusInFlight) {
        statusInFlight = fetchStatus()
            .then(data => { statusCache = { at: Date.now(), data }; return data; })
            .finally(() => { statusInFlight = null; });
    }
    try {
        res.json(Object.assign({}, await statusInFlight, runtimeInfo()));
)ZVRNBOT"
    R"ZVRNBOT(    } catch (err) {
        res.status(503).json(Object.assign({ error: err.message, players: null, whitelist: null }, runtimeInfo()));
    }
});

// ── HTTP + WebSocket сервер ───────────────────────────────────
const server = http.createServer(app);
const wss    = new WebSocketServer({ server, path: '/ws' });

wss.on('connection', (ws, req) => {
    // WEBPANEL_V3: same rule as HTTP - strict only while bound to localhost
    const ip = req.socket.remoteAddress || '';
    if (LOCAL_ONLY && !isLocalReq(ip)) {
        ws.close(4403, 'Forbidden');
        return;
    }

    // AUTHFORM_V1: WebSocket тоже требует сессию
    if (AUTH_ENABLED && !sessionOf(req)) {
        ws.close(4401, 'Unauthorized');
        return;
    }

    const send = (type, text) => {
        if (ws.readyState === ws.OPEN) ws.send(JSON.stringify({ type, text }));
    };

    send('info', `RCON → ${RCON_CFG.host}:${RCON_CFG.port}`);

    // Одно соединение RCON на клиента WebSocket
    const bridge = new RconBridge(RCON_CFG);

    ws.on('message', async raw => {
        let msg;
        try { msg = JSON.parse(raw); } catch { return; }
        if (msg && msg.type === 'ping') {   // PINGWS_V1: пинг не трогает RCON
            if (ws.readyState === ws.OPEN) ws.send(JSON.stringify({ type: 'pong', t: msg.t }));
            return;
        }
        const cmd = String((msg && msg.cmd) || '');
        // QUIET_STATUS_V2: a manual whitelist command must refresh the card at once
        if (/^whitelist\b/i.test(cmd.trim())) { wlCache = { at: 0, value: null }; statusCache = { at: 0, data: null }; }
        if (!cmd.trim()) return;
        try {
            const resp = await bridge.send(cmd.trim());
            const text = stripColors(String(resp || '(empty response)'));
            // WEBPANEL_V1: разбиваем длинный ответ на куски по 2000 символов
            const CHUNK = 2000;
            for (let i = 0; i < text.length; i += CHUNK) {
                send('resp', text.slice(i, i + CHUNK));
            }
        } catch (err) {
            await bridge.close();
            send('error', err.message);
        }
    });

    ws.on('close', () => bridge.close().catch(() => {}));
    ws.on('error', () => bridge.close().catch(() => {}));
});

function start() {
    server.listen(PORT, HOST, () => {
        const displayHost = BIND_ALL ? 'localhost' : HOST;
        console.log(`[WebPanel] http://${displayHost}:${PORT}`
            + (BIND_ALL ? '  (also reachable on your LAN / public IP, WEB_HOST=0.0.0.0)' : '  (WEB_HOST=' + HOST + ')'));
        if (AUTH_ENABLED)
            console.log('[WebPanel] Login form:'
                + (PASSWORD ? ' web password' : '')
                + (PASSWORD && ROOT_PASSWORD ? ' +' : '')
                + (ROOT_PASSWORD ? ' root password' : ''));
        else
            console.log('[WebPanel] No password set - add WEB_PASSWORD or WEB_ROOT_PASSWORD to .env');
        if (BIND_ALL && !AUTH_ENABLED)
            console.warn('[WebPanel] WARNING: the panel is open to the network without a password. Set WEB_PASSWORD in .env now.');
        if (MAX_PLAYERS === null)
            console.log('[WebPanel] settings.properties not found - the max players card will stay empty');
    });
}

module.exports = { start };

// Прямой запуск: node webpanel.js
if (require.main === module) start();
)ZVRNBOT";

inline const char* kBotFile_rcon_js =
    R"ZVRNBOT('use strict';
// DSBOT_V1: тонкая обёртка над rcon-client.
// Сервер Zevvoryn исполняет команды на тик-потоке и отвечает одним пакетом,
// поэтому держим одно соединение и шлём команды строго по очереди.

const { Rcon } = require('rcon-client');

class RconBridge {
	constructor({ host, port, password, timeout = 10000 }) {
		this.host = host;
		this.port = port;
		this.password = password;
		this.timeout = timeout;
		this.client = null;
		this.connecting = null;
		// Очередь: две команды одновременно по одному сокету — путаница ответов.
		this.queue = Promise.resolve();
	}

	async connect() {
		if (this.client) return this.client;
		if (this.connecting) return this.connecting;

		this.connecting = (async () => {
			const client = await Rcon.connect({
				host: this.host,
				port: this.port,
				password: this.password,
				timeout: this.timeout,
			});
			client.on('error', () => { this.client = null; });
			client.on('end', () => { this.client = null; });
			this.client = client;
			this.connecting = null;
			return client;
		})().catch((err) => {
			this.connecting = null;
			throw err;
		});

		return this.connecting;
	}

	// Одна попытка переподключения: сервер мог быть перезапущен.
	async send(command) {
		const run = async () => {
			try {
				const client = await this.connect();
				return await client.send(command);
			} catch (err) {
				this.client = null;
				const client = await this.connect();
				return await client.send(command);
			}
		};

		const task = this.queue.then(run, run);
		// Очередь не должна рваться из-за одной упавшей команды.
		this.queue = task.then(() => undefined, () => undefined);
		return task;
	}

	async close() {
		const client = this.client;
		this.client = null;
		if (client) {
			try { await client.end(); } catch (err) { /* уже закрыт */ }
		}
	}
}

// Убираем цветовые коды Minecraft, чтобы в Discord был чистый текст.
function stripColors(text) {
	return String(text === null || text === undefined ? '' : text).replace(/\u00a7[0-9a-fk-orA-FK-OR]/g, '');
}

// Discord не принимает больше 2000 символов в сообщении.
function asCodeBlock(text, limit = 1800) {
	const clean = stripColors(text).trimEnd();
	if (!clean) return '```\n(пустой ответ)\n```';
	const cut = clean.length > limit ? clean.slice(0, limit) + '\n... (обрезано)' : clean;
	return '```\n' + cut.split('```').join('`​``') + '\n```';
}

module.exports = { RconBridge, stripColors, asCodeBlock };
)ZVRNBOT";

inline const char* kBotFile_commands_js =
    R"ZVRNBOT(// DSBOT_V2 — полная поддержка команд + помощьники
const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');

// Быстрые команды: пользователь выбирает из списка — не нужно печатать
const QUICK_CMDS = [
    'list', 'save-all', 'help',
    'whitelist list', 'whitelist on', 'whitelist off',
    'weather clear', 'weather rain', 'weather thunder',
    'time set day', 'time set night', 'time set noon',
    'difficulty 0', 'difficulty 1', 'difficulty 2', 'difficulty 3',
    'gamemode survival @a', 'gamemode creative @a',
    'kill @e[type=!player]',
];

const commands = [
    // /list — список игроков
    new SlashCommandBuilder()
        .setName('list')
        .setDescription('Список игроков на сервере'),

    // /say — сообщение через сервер
    new SlashCommandBuilder()
        .setName('say')
        .setDescription('Отправить сообщение всем игрокам')
        .addStringOption(o => o.setName('text').setDescription('Текст').setRequired(true)),

    // /cmd — произвольная RCON-команда
    new SlashCommandBuilder()
        .setName('cmd')
        .setDescription('RCON: любая команда (только admins)')
        .addStringOption(o => o
            .setName('command')
            .setDescription('Любая команда сервера')
            .setRequired(true)
            .setAutocomplete(true)),

    // /tp — телепорт
    new SlashCommandBuilder()
        .setName('tp')
        .setDescription('Телепорт игрока (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Имя игрока (или @a/@p)').setRequired(true))
        .addStringOption(o => o.setName('target').setDescription('Цель: другой игрок или "x y z"').setRequired(true)),

    // /give — выдать предмет
    new SlashCommandBuilder()
        .setName('give')
        .setDescription('Выдать предмет (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Игрок').setRequired(true))
        .addStringOption(o => o.setName('item').setDescription('Предмет (minecraft:diamond)').setRequired(true))
        .addIntegerOption(o => o.setName('amount').setDescription('Количество (1-64)').setMinValue(1).setMaxValue(64)),

    // /kick — выгнать
    new SlashCommandBuilder()
        .setName('kick')
        .setDescription('Выгнать игрока (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Игрок').setRequired(true))
        .addStringOption(o => o.setName('reason').setDescription('Причина')),

    // /gamemode — режим
    new SlashCommandBuilder()
        .setName('gamemode')
        .setDescription('Изменить режим игррока (только admins)')
        .addStringOption(o => o
            .setName('mode')
            .setDescription('Режим')
            .setRequired(true)
            .addChoices(
                { name: 'survival', value: 'survival' },
                { name: 'creative', value: 'creative' },
                { name: 'adventure', value: 'adventure' },
                { name: 'spectator', value: 'spectator' },
           )ZVRNBOT"
    R"ZVRNBOT( ))
        .addStringOption(o => o.setName('player').setDescription('Игрок (по умолчанию @a)')),

    // /weather — погода
    new SlashCommandBuilder()
        .setName('weather')
        .setDescription('Изменить погоду (только admins)')
        .addStringOption(o => o
            .setName('type')
            .setDescription('Тип погоды')
            .setRequired(true)
            .addChoices(
                { name: 'clear', value: 'clear' },
                { name: 'rain', value: 'rain' },
                { name: 'thunder', value: 'thunder' },
            )),

    // /time — время
    new SlashCommandBuilder()
        .setName('time')
        .setDescription('Установить время (только admins)')
        .addStringOption(o => o
            .setName('preset')
            .setDescription('Время дня')
            .setRequired(true)
            .addChoices(
                { name: 'day (1000)',   value: 'set 1000'  },
                { name: 'noon (6000)',  value: 'set 6000'  },
                { name: 'night (13000)',value: 'set 13000' },
                { name: 'midnight (18000)', value: 'set 18000' },
            )),

    // /difficulty — сложность
    new SlashCommandBuilder()
        .setName('difficulty')
        .setDescription('Изменить сложность (только admins)')
        .addStringOption(o => o
            .setName('level')
            .setDescription('Сложность')
            .setRequired(true)
            .addChoices(
                { name: '0 — peaceful', value: '0' },
                { name: '1 — easy',     value: '1' },
                { name: '2 — normal',   value: '2' },
                { name: '3 — hard',     value: '3' },
            )),

    // /stop — остановка сервера
    new SlashCommandBuilder()
        .setName('stop')
        .setDescription('Остановить сервер (только admins)'),

    // /whitelist — белый список
    new SlashCommandBuilder()
        .setName('whitelist')
        .setDescription('Белый список (только admins)')
        .addStringOption(o => o
            .setName('action')
            .setDescription('Действие')
            .setRequired(true)
            .addChoices(
                { name: 'list',   value: 'list'   },
                { name: 'on',     value: 'on'     },
                { name: 'off',    value: 'off'    },
                { name: 'add',    value: 'add'    },
                { name: 'remove', value: 'remove' },
                { name: 'reload', value: 'reload' },
            ))
        .addStringOption(o => o.setName('player').setDescription('Ник (для add/remove)')),

    // /save — сохранение
    new SlashCommandBuilder()
        .setName('save')
        .setDescription('Сохранить мир (только admins)'),

    // /status — статус сервера (RCON ping)
    new SlashCommandBuilder()
        .setName('status')
        .setDescription('Статус сервера (RCON ping)'),

].map(c => c.toJSON());

const commandMap = new Map();
for (const c of commands) commandMap.set(c.name, c);
const commandData = commands)ZVRNBOT"
    R"ZVRNBOT(;
const { stripColors } = require('./rcon');
module.exports = { commands, commandData, commandMap, stripColors, QUICK_CMDS };
)ZVRNBOT";

inline const char* kBotFile_deploy_commands_js =
    R"ZVRNBOT('use strict';
// DSBOT_V1: регистрация slash-команд в одной гильдии (появляются мгновенно).
// Запуск: npm run deploy

require('dotenv').config();
const { REST, Routes } = require('discord.js');
const { commandData } = require('./commands');

const token = process.env.DISCORD_TOKEN;
const clientId = process.env.CLIENT_ID;
const guildId = process.env.GUILD_ID;

if (!token || !clientId || !guildId) {
	console.error('[deploy] Заполните DISCORD_TOKEN, CLIENT_ID и GUILD_ID в .env');
	process.exit(1);
}

(async () => {
	try {
		const rest = new REST({ version: '10' }).setToken(token);
		const body = commandData.map((c) => c.toJSON());
		await rest.put(Routes.applicationGuildCommands(clientId, guildId), { body });
		console.log('[deploy] Зарегистрировано команд: ' + body.length);
	} catch (err) {
		console.error('[deploy] Ошибка:', err);
		process.exit(1);
	}
})();
)ZVRNBOT";

inline const char* kBotFile_package_json =
    R"ZVRNBOT({
  "name": "discrordbotrcon",
  "version": "1.0.0",
  "description": "",
  "main": "index.js",
  "scripts": {
    "test": "echo \"Error: no test specified\" && exit 1"
  },
  "keywords": [],
  "author": "",
  "license": "ISC",
  "dependencies": {
    "discord.js": "^14.27.0",
    "dotenv": "^17.4.2",
    "rcon-client": "^4.2.5",
    "express": "^4.21.2",
    "ws": "^8.18.0"
  }
}
)ZVRNBOT";

inline const std::vector<std::pair<std::string, const char*>>& embeddedBotFiles() {
    static const std::vector<std::pair<std::string, const char*>> files = {
        {"index.js", kBotFile_index_js},
        {"webpanel.js", kBotFile_webpanel_js},
        {"rcon.js", kBotFile_rcon_js},
        {"commands.js", kBotFile_commands_js},
        {"deploy-commands.js", kBotFile_deploy_commands_js},
        {"package.json", kBotFile_package_json},
    };
    return files;
}

} // namespace nc
